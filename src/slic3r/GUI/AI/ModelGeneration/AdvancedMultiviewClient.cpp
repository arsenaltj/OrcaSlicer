#include "AdvancedMultiviewClient.hpp"
#include "slic3r/GUI/AISidecarClient.hpp"
#include "slic3r/GUI/AIModelGenerationHttpError.hpp"
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <openssl/evp.h>
#include <algorithm>
#include <array>
#include <cctype>

namespace Slic3r::GUI {
namespace {
const std::string prefix = "/v1/orcaslicer/advanced-multiview";

std::string sha256(const std::string& body)
{
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest {};
    unsigned length = 0;
    if (EVP_Digest(body.data(), body.size(), digest.data(), &length, EVP_sha256(), nullptr) != 1) return {};
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (unsigned i = 0; i < length; ++i) { result += hex[digest[i] >> 4]; result += hex[digest[i] & 15]; }
    return result;
}

AdvancedCreation::DesignDraft parse_draft(const nlohmann::json& value)
{
    return {value.at("prompt").get<std::string>(), value.at("style").get<std::string>(),
            value.at("custom_style").get<std::string>(), value.at("design_provider").get<std::string>(),
            value.at("design_model").get<std::string>()};
}
}

AdvancedMultiviewClient::AdvancedMultiviewClient(std::string endpoint) : m_endpoint(std::move(endpoint))
{ while (!m_endpoint.empty() && m_endpoint.back() == '/') m_endpoint.pop_back(); }
AdvancedMultiviewClient::~AdvancedMultiviewClient() { cancel(); }

bool AdvancedMultiviewClient::valid_id(const std::string& value)
{
    if (value.size() != 36) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (value[i] != '-') return false; }
        else if (!std::isdigit(static_cast<unsigned char>(value[i])) && (value[i] < 'a' || value[i] > 'f')) return false;
    }
    return true;
}

bool AdvancedMultiviewClient::available(Error error) const
{
    if (AISidecarClient::is_loopback_endpoint(m_endpoint)) return true;
    if (error) error("Advanced creation requires the authenticated local AI service.");
    return false;
}

void AdvancedMultiviewClient::perform(Http http, Complete complete, Error error)
{
    auto done = std::make_shared<std::atomic<bool>>(false);
    AISidecarClient::configure_native_request(http);
    http.timeout_connect(5).timeout_max(120).size_limit(1024 * 1024);
    http.on_complete([complete = std::move(complete), error, done](std::string body, unsigned) {
        *done = true;
        auto value = Json::parse(body, nullptr, false);
        if (!value.is_object()) { if (error) error("The advanced service response is incomplete."); return; }
        if (complete) complete(std::move(value));
    });
    http.on_error([error, done](std::string body, std::string detail, unsigned status) {
        *done = true;
        if (error) error(model_generation_http_error(body, detail, status));
    });
    track(std::move(http), done);
}

void AdvancedMultiviewClient::track(Http http, std::shared_ptr<std::atomic<bool>> done)
{
    m_requests.erase(std::remove_if(m_requests.begin(), m_requests.end(),
        [](const PendingRequest& request) { return request.done->load(); }), m_requests.end());
    m_requests.push_back({http.perform(), std::move(done)});
}

bool AdvancedMultiviewClient::cached_asset_matches(const boost::filesystem::path& path, const std::string& expected)
{
    boost::system::error_code ec;
    if (!boost::filesystem::is_regular_file(path, ec) || boost::filesystem::file_size(path, ec) > 20 * 1024 * 1024) return false;
    boost::filesystem::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    const std::string body((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    return sha256(body) == expected;
}

void AdvancedMultiviewClient::request(const std::string& method, const std::string& route,
                                     const Json& payload, Complete complete, Error error)
{
    if (!available(error)) return;
    if (route.empty() || route.front() != '/' || route.find("..") != std::string::npos ||
        route.find_first_of("\r\n#") != std::string::npos) {
        if (error) error("The advanced route is invalid."); return;
    }
    if (method != "GET" && method != "POST" && method != "PUT") {
        if (error) error("The advanced method is invalid."); return;
    }
    auto http = method == "GET" ? Http::get(m_endpoint + prefix + route) :
                // put() streams a file through READDATA; JSON uses the custom PUT variant.
                method == "PUT" ? Http::put2(m_endpoint + prefix + route) : Http::post(m_endpoint + prefix + route);
    if (method != "GET") http.header("Content-Type", "application/json").set_post_body(payload.dump());
    perform(std::move(http), std::move(complete), std::move(error));
}

AdvancedMultiviewClient::Json AdvancedMultiviewClient::design_json(const AdvancedCreation::DesignDraft& draft)
{
    return {{"prompt", draft.prompt}, {"style", draft.style}, {"custom_style", draft.custom_style},
            {"design_provider", draft.provider}, {"design_model", draft.model}};
}

void AdvancedMultiviewClient::create_group(const std::string& request_id, const AdvancedCreation::InputDraft& draft,
                                          Complete complete, Error error)
{
    if (!available(error)) return;
    if (!valid_id(request_id)) { if (error) error("The creation request identity is invalid."); return; }
    const bool images = std::any_of(draft.uploads.begin(), draft.uploads.end(), [](const auto& path) { return !path.empty(); });
    auto payload = design_json(draft.design);
    payload["request_id"] = request_id;
    payload["source"] = images ? "image" : "text";
    if (!images) { request("POST", "/groups", payload, std::move(complete), std::move(error)); return; }
    if (draft.uploads[0].empty()) { if (error) error("missing_front: A front reference is required."); return; }
    auto http = Http::post(m_endpoint + prefix + "/groups");
    for (auto field = payload.begin(); field != payload.end(); ++field) http.form_add(field.key(), field.value().get<std::string>());
    for (auto view : AdvancedCreation::AllViews) {
        const auto& path = draft.uploads[AdvancedCreation::index(view)];
        if (!path.empty()) http.form_add_file(std::string(AdvancedCreation::view_name(view)) + "_image", boost::filesystem::path(path));
    }
    perform(std::move(http), std::move(complete), std::move(error));
}

void AdvancedMultiviewClient::update_group(const std::string& group_id, std::uint64_t expected_revision,
                                          const AdvancedCreation::InputDraft& draft,
                                          Complete complete, Error error)
{
    if (!available(error)) return;
    if (!valid_id(group_id)) { if (error) error("The group identity is invalid."); return; }
    auto payload = design_json(draft.design);
    payload["expected_group_revision"] = std::to_string(expected_revision);
    auto http = Http::post(m_endpoint + prefix + "/groups/" + group_id + "/inputs");
    for (auto field = payload.begin(); field != payload.end(); ++field)
        http.form_add(field.key(), field.value().get<std::string>());
    for (auto view : AdvancedCreation::AllViews) {
        const auto& path = draft.uploads[AdvancedCreation::index(view)];
        if (!path.empty())
            http.form_add_file(std::string(AdvancedCreation::view_name(view)) + "_image", boost::filesystem::path(path));
    }
    perform(std::move(http), std::move(complete), std::move(error));
}

boost::filesystem::path AdvancedMultiviewClient::asset_path(const boost::filesystem::path& root,
                                                           const std::string& group, const std::string& asset)
{ return valid_id(group) && valid_id(asset) ? root / group / (asset + ".image") : boost::filesystem::path(); }

void AdvancedMultiviewClient::download_asset(const std::string& group, const std::string& asset,
                                            const std::string& expected, const boost::filesystem::path& destination,
                                            std::function<void()> complete, Error error)
{
    if (!available(error)) return;
    if (!valid_id(group) || !valid_id(asset) || expected.size() != 64 || destination.empty()) {
        if (error) error("The advanced asset identity is invalid."); return;
    }
    auto http = Http::get(m_endpoint + prefix + "/groups/" + group + "/assets/" + asset);
    auto done = std::make_shared<std::atomic<bool>>(false);
    AISidecarClient::configure_native_request(http);
    http.timeout_connect(5).timeout_max(120).size_limit(20 * 1024 * 1024);
    http.on_complete([expected, destination, complete = std::move(complete), error, done](std::string body, unsigned) {
        *done = true;
        if (body.empty() || sha256(body) != expected) { if (error) error("The downloaded image fingerprint does not match."); return; }
        const auto partial = boost::filesystem::path(destination.string() + ".part");
        boost::system::error_code ec;
        boost::filesystem::create_directories(destination.parent_path(), ec);
        boost::filesystem::ofstream stream(partial, std::ios::binary | std::ios::trunc);
        stream.write(body.data(), static_cast<std::streamsize>(body.size())); stream.close();
        if (!stream) { if (error) error("The advanced image cache could not be written."); return; }
        boost::filesystem::remove(destination, ec);
        boost::filesystem::rename(partial, destination, ec);
        if (ec) { if (error) error("The advanced image cache could not be finalized."); return; }
        if (complete) complete();
    });
    http.on_error([error, done](std::string body, std::string detail, unsigned status) {
        *done = true;
        if (error) error(model_generation_http_error(body, detail, status));
    });
    track(std::move(http), done);
}

std::optional<AdvancedCreation::ModelJobProjection> AdvancedMultiviewClient::parse_model_job(
    const Json& job, const std::string& group, const std::string& expected_job)
{
    try {
        if (!valid_id(group) || !valid_id(expected_job) || job.at("id") != expected_job ||
            job.at("creation_kind") != "advanced_multiview_v1" || job.at("advanced_group_id") != group ||
            job.at("workflow_mode") != "advanced") return {};
        AdvancedCreation::ModelJobProjection result;
        result.id = expected_job;
        result.state = job.at("state").get<std::string>();
        if (result.state.empty()) return {};
        result.phase = job.value("phase", std::string());
        result.updated_at = job.value("updated_at", 0.0);
        result.message = job.value("message", std::string());
        if (job.contains("provider_tasks") && job["provider_tasks"].is_object())
            result.resumable = !job["provider_tasks"].value("generation_task_id", std::string()).empty();
        if (job.contains("provider_failure") && job["provider_failure"].is_object()) {
            result.error_code = job["provider_failure"].value("code", std::string());
            result.error_ambiguous = job["provider_failure"].value("ambiguous", false);
        }
        return result;
    } catch (const Json::exception&) { return {}; }
}

std::optional<AdvancedCreation::GroupSnapshot> AdvancedMultiviewClient::parse_group(const Json& value,
                                                                                 const boost::filesystem::path& root)
{
    using namespace AdvancedCreation;
    try {
        if (value.at("schema") != "advanced-multiview-group-v1" || value.at("workflow_kind") != "advanced_multiview_v1") return {};
        GroupSnapshot result;
        result.id = value.at("id").get<std::string>();
        result.display_name = value.value("display_name", std::string());
        result.input_epoch = value.at("input_epoch").get<std::string>();
        if (!valid_id(result.id) || !valid_id(result.input_epoch) || !value.at("revision").is_number_unsigned()) return {};
        result.revision = value.at("revision").get<std::uint64_t>();
        if (value.contains("pending_views")) {
            if (!value["pending_views"].is_array()) return {};
            for (const auto& name : value["pending_views"]) {
                if (!name.is_string()) return {};
                const auto view = parse_view(name.get<std::string>());
                if (!view) return {};
                result.pending_views[index(*view)] = true;
            }
        }
        result.initial_input = parse_draft(value.at("initial_input"));
        result.saved_draft = parse_draft(value.at("saved_draft"));
        if (!value.at("latest_submitted_draft").is_null()) result.latest_submitted_draft = parse_draft(value.at("latest_submitted_draft"));
        if (!value.at("active_operation_id").is_null()) {
            result.active_operation_id = value.at("active_operation_id").get<std::string>();
            if (!valid_id(result.active_operation_id)) return {};
        }
        result.stop_intent = value.at("stop_intent").get<bool>();
        result.execution_state = value.value("execution_state", std::string());
        result.published = value.at("published").get<bool>();
        result.model_busy = value.value("model_busy", false);
        if (value.contains("model_jobs") && value["model_jobs"].is_array() && !value["model_jobs"].empty()) {
            const Json* projected = &value["model_jobs"].back();
            for (const auto& candidate : value["model_jobs"]) {
                const auto state = candidate.value("state", std::string());
                if (state == "queued" || state == "running" || state == "stopping") { projected = &candidate; break; }
            }
            result.model_job_id = projected->value("id", std::string());
            if (!valid_id(result.model_job_id)) return {};
            const auto& model = *projected;
            result.model_state = model.value("state", std::string());
            result.model_phase = model.value("phase", std::string());
            result.model_updated_at = model.value("updated_at", 0.0);
            result.model_message = model.value("message", std::string());
            if (model.contains("provider_failure") && model["provider_failure"].is_object()) {
                result.model_error_code = model["provider_failure"].value("code", std::string());
                result.model_error_ambiguous = model["provider_failure"].value("ambiguous", false);
            }
            if (model.contains("provider_tasks") && model["provider_tasks"].is_object())
                result.model_resumable = !model["provider_tasks"].value("generation_task_id", std::string()).empty();
        }
        for (auto view : AllViews) {
            const auto name = view_name(view); const auto slot = index(view);
            const auto& upload = value.at("uploads").at(name);
            if (!upload.is_null()) {
                const auto asset = upload.at("asset_id").get<std::string>();
                if (!valid_id(asset)) return {};
                result.uploads[slot] = asset_path(root, result.id, asset).string();
            }
            const auto& source = value.at("views").at(name);
            auto& target = result.views[slot];
            if (!source.at("current_version_id").is_null()) {
                target.version_id = source.at("current_version_id").get<std::string>();
                target.canonical_version_id = target.version_id;
                if (!valid_id(target.version_id)) return {};
            }
            if (source.contains("current") && source["current"].is_object()) {
                const auto& current = source["current"];
                target.sha256 = current.at("sha256").get<std::string>();
                target.image_path = asset_path(root, result.id, target.version_id).string();
                target.usable = target.sha256.size() == 64;
            }
            if (!source.at("latest_attempt").is_null()) {
                const auto& attempt = source.at("latest_attempt");
                target.operation_id = attempt.at("operation_id").get<std::string>();
                target.attempt_state = attempt.at("state").get<std::string>();
                if (attempt.contains("error") && attempt["error"].is_object()) {
                    const auto& error = attempt["error"];
                    target.error_code = error.value("code", std::string());
                    target.error_message = error.value("provider_message", error.value("message", std::string()));
                    target.provider_request_id = error.value("request_id", std::string());
                    target.error_http_status = error.value("http_status", 0);
                }
            }
        }
        return result;
    } catch (const Json::exception&) { return {}; }
}

void AdvancedMultiviewClient::cancel()
{
    for (auto& request : m_requests) if (request.http) request.http->cancel();
    m_requests.clear();
}

} // namespace Slic3r::GUI
