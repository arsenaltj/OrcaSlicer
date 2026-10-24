#include "AITextClient.hpp"

#include "slic3r/GUI/AISidecarClient.hpp"
#include "slic3r/Utils/Http.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <utility>

namespace Slic3r::GUI {
namespace {

constexpr std::size_t MAX_TEXT_BYTES = 2000;
constexpr std::size_t MAX_WARNING_BYTES = 500;
constexpr std::size_t MAX_WARNINGS = 8;
constexpr const char* CAPABILITY_PATH = "/v1/orcaslicer/text-capabilities/image_prompt_polish";

bool valid_diagnostic_id(const std::string& value)
{
    return !value.empty() && value.size() <= 64;
}

bool valid_status(const std::string& value)
{
    return value == "success" || value == "needs_input" || value == "needs_clarification";
}

} // namespace

void AITextClient::RequestHandle::cancel()
{
    if (m_request) {
        m_request->cancel();
        m_request.reset();
    }
}

AITextClient::AITextClient(std::string endpoint) : m_endpoint(std::move(endpoint))
{
    while (!m_endpoint.empty() && m_endpoint.back() == '/')
        m_endpoint.pop_back();
}

std::string AITextClient::url(const char* path) const
{
    return m_endpoint + path;
}

bool AITextClient::parse_availability(const std::string& body, Availability& result)
{
    const auto parsed = nlohmann::json::parse(body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object() || parsed.size() != 4 ||
        parsed.value("capability", std::string()) != "image_prompt_polish" ||
        !parsed.contains("available") || !parsed["available"].is_boolean() ||
        !parsed.contains("reason") || !parsed["reason"].is_string() ||
        !parsed.contains("diagnostic_id") || !parsed["diagnostic_id"].is_string())
        return false;
    result.available = parsed["available"].get<bool>();
    result.reason = parsed["reason"].get<std::string>();
    result.diagnostic_id = parsed["diagnostic_id"].get<std::string>();
    return result.reason.size() <= 100 && valid_diagnostic_id(result.diagnostic_id);
}

bool AITextClient::parse_polish_result(const std::string& body, PolishResult& result)
{
    const auto parsed = nlohmann::json::parse(body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object() || parsed.size() != 4 ||
        !parsed.contains("status") || !parsed["status"].is_string() ||
        !parsed.contains("optimized_prompt") || !parsed["optimized_prompt"].is_string() ||
        !parsed.contains("warnings") || !parsed["warnings"].is_array() ||
        !parsed.contains("diagnostic_id") || !parsed["diagnostic_id"].is_string())
        return false;
    result.status = parsed["status"].get<std::string>();
    result.optimized_prompt = parsed["optimized_prompt"].get<std::string>();
    result.diagnostic_id = parsed["diagnostic_id"].get<std::string>();
    if (!valid_status(result.status) || result.optimized_prompt.size() > MAX_TEXT_BYTES ||
        !valid_diagnostic_id(result.diagnostic_id) || parsed["warnings"].size() > MAX_WARNINGS ||
        (result.status == "success") != !result.optimized_prompt.empty())
        return false;
    result.warnings.clear();
    for (const auto& warning : parsed["warnings"]) {
        if (!warning.is_string())
            return false;
        auto value = warning.get<std::string>();
        if (value.empty() || value.size() > MAX_WARNING_BYTES)
            return false;
        result.warnings.emplace_back(std::move(value));
    }
    return true;
}

AITextClient::Error AITextClient::parse_error(
    const std::string& body, const std::string& transport_error, unsigned status)
{
    Error result;
    const auto parsed = nlohmann::json::parse(body, nullptr, false);
    if (!parsed.is_discarded() && parsed.is_object()) {
        if (parsed.contains("error") && parsed["error"].is_object()) {
            result.code = parsed["error"].value("code", std::string());
            result.message = parsed["error"].value("message", std::string());
        }
        result.diagnostic_id = parsed.value("diagnostic_id", std::string());
    }
    if (result.message.empty())
        result.message = !transport_error.empty() ? transport_error :
            "AI text request failed with HTTP " + std::to_string(status) + ".";
    if (result.code.empty())
        result.code = status == 0 ? "transport_error" : "http_error";
    return result;
}

std::shared_ptr<AITextClient::RequestHandle> AITextClient::get_image_prompt_polish_availability(
    AvailabilityFn on_complete, ErrorFn on_error) const
{
    if (!AISidecarClient::is_loopback_endpoint(m_endpoint)) {
        if (on_error)
            on_error({"invalid_endpoint", "AI text capabilities require a loopback Sidecar endpoint.", {}});
        return {};
    }
    auto http = Http::get(url(CAPABILITY_PATH));
    AISidecarClient::configure_native_request(http);
    http.timeout_connect(5).timeout_max(60).size_limit(64 * 1024);
    http.on_complete([on_complete = std::move(on_complete), on_error](std::string body, unsigned) mutable {
        Availability result;
        if (!parse_availability(body, result)) {
            if (on_error)
                on_error({"invalid_response", "AI Sidecar returned an invalid text capability response.", {}});
            return;
        }
        if (on_complete)
            on_complete(std::move(result));
    });
    http.on_error([on_error = std::move(on_error)](std::string body, std::string error, unsigned status) {
        if (on_error)
            on_error(parse_error(body, error, status));
    });
    return std::shared_ptr<RequestHandle>(new RequestHandle(http.perform()));
}

std::shared_ptr<AITextClient::RequestHandle> AITextClient::polish_image_prompt(
    const PolishRequest& request, ResultFn on_complete, ErrorFn on_error) const
{
    if (!AISidecarClient::is_loopback_endpoint(m_endpoint)) {
        if (on_error)
            on_error({"invalid_endpoint", "AI text capabilities require a loopback Sidecar endpoint.", {}});
        return {};
    }
    if (request.has_reference_image != !request.reference_image_path.empty()) {
        if (on_error)
            on_error({"invalid_reference_image", "AI text request reference image state is inconsistent.", {}});
        return {};
    }
    const nlohmann::json payload {
        {"has_reference_image", request.has_reference_image},
        {"user_description", request.user_description},
        {"intended_use", "generate_2d_design_image"},
        {"constraints", {"主体完整呈现", "背景使用纯色", "尽量减少主体被遮挡"}},
    };
    auto http = Http::post(url(CAPABILITY_PATH));
    AISidecarClient::configure_native_request(http);
    http.timeout_connect(15).timeout_max(190).size_limit(64 * 1024);
    if (request.has_reference_image) {
        http.form_add("request", payload.dump())
            .form_add_file("image", request.reference_image_path, "reference-image");
    } else {
        http.header("Content-Type", "application/json").set_post_body(payload.dump());
    }
    http.on_complete([on_complete = std::move(on_complete), on_error](std::string body, unsigned) mutable {
        PolishResult result;
        if (!parse_polish_result(body, result)) {
            if (on_error)
                on_error({"invalid_response", "AI Sidecar returned an invalid polish response.", {}});
            return;
        }
        if (on_complete)
            on_complete(std::move(result));
    });
    http.on_error([on_error = std::move(on_error)](std::string body, std::string error, unsigned status) {
        if (on_error)
            on_error(parse_error(body, error, status));
    });
    return std::shared_ptr<RequestHandle>(new RequestHandle(http.perform()));
}

} // namespace Slic3r::GUI
