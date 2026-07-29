#include "AISidecarClient.hpp"

#include "slic3r/Utils/Http.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <regex>
#include <utility>

namespace Slic3r::GUI {

namespace {

bool is_loopback_endpoint(const std::string& endpoint)
{
    static const std::regex pattern(R"(^https?://(\[[^\]]+\]|[^/:?#]+)(?::[0-9]+)?(?:[/?#]|$))", std::regex::icase);
    std::smatch match;
    if (!std::regex_search(endpoint, match, pattern))
        return false;
    std::string host = match[1].str();
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return host == "localhost" || host == "127.0.0.1" || host == "[::1]";
}

std::string proposal_url(const std::string& endpoint)
{
    if (!endpoint.empty() && endpoint.back() == '/')
        return endpoint.substr(0, endpoint.size() - 1) + "/v1/orcaslicer/config-proposal";
    return endpoint + "/v1/orcaslicer/config-proposal";
}

} // namespace

AISidecarClient::AISidecarClient(std::string endpoint)
    : m_endpoint(std::move(endpoint))
{
}

AISidecarClient::~AISidecarClient()
{
    cancel_current();
}

std::string AISidecarClient::default_endpoint()
{
    if (const char* endpoint = std::getenv("ORCASLICER_AI_SIDECAR_URL"); endpoint != nullptr && endpoint[0] != '\0')
        return endpoint;
    return "http://127.0.0.1:18764";
}

void AISidecarClient::propose_config_changes(const json& request, CompleteFn on_complete, ErrorFn on_error)
{
    cancel_current();

    auto http = Http::post(proposal_url(m_endpoint));
    http.header("Content-Type", "application/json")
        .timeout_connect(5)
        .timeout_max(120)
        .size_limit(5 * 1024 * 1024)
        .set_post_body(request.dump());

    if (!is_loopback_endpoint(m_endpoint))
        http.tls_verify(true);

    http.on_complete([on_complete = std::move(on_complete), on_error](std::string body, unsigned) mutable {
        auto parsed = json::parse(body, nullptr, false);
        if (parsed.is_discarded()) {
            if (on_error)
                on_error("AI sidecar returned invalid JSON.");
            return;
        }

        Response response;
        response.raw = parsed;
        response.request_id = parsed.value("request_id", std::string());
        response.assistant_text = parsed.value("assistant_text", std::string());
        if (parsed.contains("proposal"))
            response.proposal = parsed["proposal"];

        if (on_complete)
            on_complete(std::move(response));
    });

    http.on_error([on_error = std::move(on_error)](std::string body, std::string error, unsigned status) {
        if (!error.empty()) {
            if (on_error)
                on_error(error);
            return;
        }

        if (on_error)
            on_error("AI sidecar request failed with HTTP " + std::to_string(status) + (body.empty() ? std::string() : ": " + body));
    });

    m_active_request = http.perform();
}

void AISidecarClient::cancel_current()
{
    if (m_active_request) {
        m_active_request->cancel();
        m_active_request.reset();
    }
}

} // namespace Slic3r::GUI
