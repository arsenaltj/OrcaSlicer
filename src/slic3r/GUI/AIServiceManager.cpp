#include "AIServiceManager.hpp"

#include "AISidecarClient.hpp"
#include "GUI_App.hpp"
#include "slic3r/Utils/Http.hpp"

#include <nlohmann/json.hpp>

#include <utility>

namespace Slic3r::GUI {
namespace {

std::string health_url(const std::string& endpoint)
{
    if (!endpoint.empty() && endpoint.back() == '/')
        return endpoint.substr(0, endpoint.size() - 1) + "/health";
    return endpoint + "/health";
}

AIServiceAvailability parse_health_response(const std::string& body)
{
    AIServiceAvailability result;
    const auto parsed = nlohmann::json::parse(body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        result.error = "AI sidecar returned an invalid health response.";
        return result;
    }

    if (!parsed.value("ok", false) || !parsed.contains("protocol_version") || !parsed["protocol_version"].is_number_integer() ||
        parsed["protocol_version"].get<int>() != 1 || !parsed.contains("sidecar_version") || !parsed["sidecar_version"].is_string() ||
        !parsed.contains("capabilities") || !parsed["capabilities"].is_object()) {
        result.error = "AI sidecar returned an incompatible health response.";
        return result;
    }

    const auto& capabilities = parsed["capabilities"];
    const auto config_proposal = capabilities.find("config_proposal");
    const auto model_generation = capabilities.find("model_generation");
    const auto expected_sources = nlohmann::json::array({ "text", "image" });
    const auto expected_artifact_formats = nlohmann::json::array({ "3mf", "stl" });
    if (config_proposal == capabilities.end() || !config_proposal->is_object() || !config_proposal->contains("available") ||
        !(*config_proposal)["available"].is_boolean() || model_generation == capabilities.end() || !model_generation->is_object() ||
        !model_generation->contains("available") || !(*model_generation)["available"].is_boolean() ||
        !model_generation->contains("sources") || (*model_generation)["sources"] != expected_sources ||
        !model_generation->contains("artifact_formats") || (*model_generation)["artifact_formats"] != expected_artifact_formats) {
        result.error = "AI sidecar returned incomplete capability data.";
        return result;
    }

    result.compatible = true;
    result.sidecar_version = parsed["sidecar_version"].get<std::string>();
    result.config_proposal_available = (*config_proposal)["available"].get<bool>();
    result.model_generation_available = (*model_generation)["available"].get<bool>();
    return result;
}

} // namespace

AIServiceManager::AIServiceManager(std::string endpoint)
    : m_endpoint(std::move(endpoint))
    , m_lifetime(std::make_shared<int>(0))
{
}

AIServiceManager::~AIServiceManager()
{
    shutdown();
}

void AIServiceManager::discover_async(CompleteFn on_complete)
{
    shutdown();
    m_lifetime = std::make_shared<int>(0);
    const std::weak_ptr<int> lifetime = m_lifetime;

    if (!AISidecarClient::is_loopback_endpoint(m_endpoint)) {
        AIServiceAvailability result;
        result.error = "AI sidecar discovery requires a loopback endpoint.";
        wxGetApp().CallAfter([lifetime, on_complete, result = std::move(result)]() mutable {
            if (!lifetime.expired() && on_complete)
                on_complete(std::move(result));
        });
        return;
    }

    auto http = Http::get(health_url(m_endpoint));
    http.timeout_connect(2).timeout_max(5).size_limit(16 * 1024);

    http.on_complete([lifetime, on_complete](std::string body, unsigned) mutable {
        auto result = parse_health_response(body);
        wxGetApp().CallAfter([lifetime, on_complete, result = std::move(result)]() mutable {
            if (lifetime.expired())
                return;
            if (on_complete)
                on_complete(std::move(result));
        });
    });
    http.on_error([lifetime, on_complete](std::string, std::string, unsigned) mutable {
        AIServiceAvailability result;
        result.error = "AI sidecar is not reachable.";
        wxGetApp().CallAfter([lifetime, on_complete, result = std::move(result)]() mutable {
            if (lifetime.expired())
                return;
            if (on_complete)
                on_complete(std::move(result));
        });
    });
    m_active_request = http.perform();
}

void AIServiceManager::shutdown()
{
    m_lifetime.reset();
    if (m_active_request) {
        m_active_request->cancel();
        m_active_request.reset();
    }
}

} // namespace Slic3r::GUI
