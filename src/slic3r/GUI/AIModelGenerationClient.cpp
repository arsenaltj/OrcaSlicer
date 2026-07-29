#include "AIModelGenerationClient.hpp"

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>

#include <algorithm>
#include <cctype>
#include <regex>
#include <utility>

namespace Slic3r::GUI {
namespace {

constexpr size_t MAX_PREVIEW_SIZE = 10 * 1024 * 1024;
constexpr size_t MAX_ARTIFACT_SIZE = 250 * 1024 * 1024;

std::string normalize_endpoint(std::string endpoint)
{
    while (!endpoint.empty() && endpoint.back() == '/')
        endpoint.pop_back();
    return endpoint;
}

std::string error_message(const std::string& body, const std::string& error, unsigned status)
{
    if (!error.empty()) {
        if (error.find("connect") != std::string::npos || error.find("Connection") != std::string::npos)
            return "AI sidecar is not reachable.";
        if (error.find("timed out") != std::string::npos || error.find("Timeout") != std::string::npos)
            return "AI sidecar request timed out.";
        return "AI sidecar request failed.";
    }
    auto parsed = nlohmann::json::parse(body, nullptr, false);
    if (!parsed.is_discarded()) {
        if (parsed.contains("error") && parsed["error"].is_object())
            return parsed["error"].value("message", "Model generation request failed.");
        if (parsed.contains("error") && parsed["error"].is_string())
            return parsed["error"].get<std::string>();
    }
    return "Model generation request failed with HTTP " + std::to_string(status) + ".";
}

} // namespace

AIModelGenerationClient::AIModelGenerationClient(std::string endpoint)
    : m_endpoint(normalize_endpoint(std::move(endpoint)))
{
}

AIModelGenerationClient::~AIModelGenerationClient()
{
    cancel_current();
}

bool AIModelGenerationClient::is_loopback_endpoint(const std::string& endpoint)
{
    static const std::regex pattern(R"(^https?://(\[[^\]]+\]|[^/:?#]+)(?::[0-9]+)?(?:[/?#]|$))", std::regex::icase);
    std::smatch match;
    if (!std::regex_search(endpoint, match, pattern))
        return false;
    std::string host = match[1].str();
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return host == "localhost" || host == "127.0.0.1" || host == "[::1]";
}

std::string AIModelGenerationClient::url(const std::string& path) const
{
    return m_endpoint + path;
}

void AIModelGenerationClient::preprocess_text(const std::string& request_id, const std::string& prompt,
                                               StatusFn on_complete, ErrorFn on_error)
{
    post_json("/v1/orcaslicer/model-jobs/text",
              json::object({ { "request_id", request_id }, { "prompt", prompt } }),
              std::move(on_complete), std::move(on_error));
}

void AIModelGenerationClient::preprocess_image(const std::string& request_id, const std::string& instruction,
                                                const boost::filesystem::path& image_path,
                                                StatusFn on_complete, ErrorFn on_error)
{
    cancel_current();
    if (!is_loopback_endpoint(m_endpoint)) {
        if (on_error)
            on_error("Image preprocessing requires a loopback AI sidecar endpoint.");
        return;
    }

    auto http = Http::post(url("/v1/orcaslicer/model-jobs/image"));
    http.header("X-OrcaSlicer-Client", "native")
        .timeout_connect(5)
        .timeout_max(130)
        .size_limit(1024 * 1024)
        .form_add("request_id", request_id)
        .form_add("instruction", instruction)
        .form_add_file("image", image_path, image_path.filename().string());
    http.on_complete([this, on_complete = std::move(on_complete), on_error](std::string body, unsigned) mutable {
        parse_status_response(std::move(body), std::move(on_complete), on_error);
    });
    http.on_error([on_error = std::move(on_error)](std::string body, std::string error, unsigned status) {
        if (on_error)
            on_error(error_message(body, error, status));
    });
    m_active_request = http.perform();
}

void AIModelGenerationClient::generate(const std::string& job_id, const std::string& prepared_prompt,
                                       StatusFn on_complete, ErrorFn on_error)
{
    post_json("/v1/orcaslicer/model-jobs/" + job_id + "/generate",
              json::object({ { "prepared_prompt", prepared_prompt } }),
              std::move(on_complete), std::move(on_error));
}

void AIModelGenerationClient::get_status(const std::string& job_id, StatusFn on_complete, ErrorFn on_error)
{
    cancel_current();
    if (!is_loopback_endpoint(m_endpoint)) {
        if (on_error)
            on_error("Model generation requires a loopback AI sidecar endpoint.");
        return;
    }

    auto http = Http::get(url("/v1/orcaslicer/model-jobs/" + job_id));
    http.header("X-OrcaSlicer-Client", "native").timeout_connect(5).timeout_max(15).size_limit(1024 * 1024);
    http.on_complete([this, on_complete = std::move(on_complete), on_error](std::string body, unsigned) mutable {
        parse_status_response(std::move(body), std::move(on_complete), on_error);
    });
    http.on_error([on_error = std::move(on_error)](std::string body, std::string error, unsigned status) {
        if (on_error)
            on_error(error_message(body, error, status));
    });
    m_active_request = http.perform();
}

void AIModelGenerationClient::stop(const std::string& job_id, StatusFn on_complete, ErrorFn on_error)
{
    post_json("/v1/orcaslicer/model-jobs/" + job_id + "/stop", json::object(),
              std::move(on_complete), std::move(on_error));
}

void AIModelGenerationClient::remove(const std::string& job_id, CompleteFn on_complete, ErrorFn on_error)
{
    cancel_current();
    if (!is_loopback_endpoint(m_endpoint)) {
        if (on_error)
            on_error("Model generation requires a loopback AI sidecar endpoint.");
        return;
    }

    auto http = Http::del(url("/v1/orcaslicer/model-jobs/" + job_id));
    http.header("X-OrcaSlicer-Client", "native").timeout_connect(5).timeout_max(15).size_limit(1024 * 1024);
    http.on_complete([on_complete = std::move(on_complete)](std::string, unsigned) {
        if (on_complete)
            on_complete();
    });
    http.on_error([on_error = std::move(on_error)](std::string body, std::string error, unsigned status) {
        if (on_error)
            on_error(error_message(body, error, status));
    });
    m_active_request = http.perform();
}

void AIModelGenerationClient::download_preview(const std::string& job_id, const boost::filesystem::path& path,
                                                PathFn on_complete, ErrorFn on_error)
{
    download("/v1/orcaslicer/model-jobs/" + job_id + "/preview", path, MAX_PREVIEW_SIZE,
             std::move(on_complete), std::move(on_error));
}

void AIModelGenerationClient::download_artifact(const std::string& job_id, const std::string& format,
                                                 const boost::filesystem::path& path,
                                                 PathFn on_complete, ErrorFn on_error)
{
    if (format != "3mf" && format != "stl") {
        if (on_error)
            on_error("The generated artifact format is not supported.");
        return;
    }
    download("/v1/orcaslicer/model-jobs/" + job_id + "/artifact", path, MAX_ARTIFACT_SIZE,
             std::move(on_complete), std::move(on_error));
}

void AIModelGenerationClient::post_json(const std::string& path, const json& body,
                                        StatusFn on_complete, ErrorFn on_error)
{
    cancel_current();
    if (!is_loopback_endpoint(m_endpoint)) {
        if (on_error)
            on_error("Model generation requires a loopback AI sidecar endpoint.");
        return;
    }

    auto http = Http::post(url(path));
    http.header("Content-Type", "application/json")
        .header("X-OrcaSlicer-Client", "native")
        .timeout_connect(5)
        .timeout_max(130)
        .size_limit(1024 * 1024)
        .set_post_body(body.dump());
    http.on_complete([this, on_complete = std::move(on_complete), on_error](std::string response, unsigned) mutable {
        parse_status_response(std::move(response), std::move(on_complete), on_error);
    });
    http.on_error([on_error = std::move(on_error)](std::string response, std::string error, unsigned status) {
        if (on_error)
            on_error(error_message(response, error, status));
    });
    m_active_request = http.perform();
}

void AIModelGenerationClient::parse_status_response(std::string body, StatusFn on_complete, ErrorFn on_error)
{
    auto parsed = json::parse(body, nullptr, false);
    if (parsed.is_discarded() || !parsed.contains("job") || !parsed["job"].is_object()) {
        if (on_error)
            on_error("AI sidecar returned an invalid model job response.");
        return;
    }

    const json& job = parsed["job"];
    JobStatus status;
    status.id = job.value("id", std::string());
    status.source = job.value("source", std::string());
    status.state = job.value("state", std::string());
    status.phase = job.value("phase", std::string());
    status.message = job.value("message", std::string());
    status.prepared_prompt = job.value("prepared_prompt", std::string());
    status.progress = std::clamp(job.value("progress", 0), 0, 100);
    if (job.contains("preview") && job["preview"].is_object())
        status.preview_ready = job["preview"].value("ready", false);
    if (job.contains("artifact") && job["artifact"].is_object()) {
        status.artifact_ready = job["artifact"].value("ready", false);
        status.artifact_format = job["artifact"].value("format", std::string());
        status.artifact_size = job["artifact"].value("size_bytes", size_t(0));
    }
    if (status.id.empty() || status.state.empty()) {
        if (on_error)
            on_error("AI sidecar returned an incomplete model job response.");
        return;
    }
    if (on_complete)
        on_complete(std::move(status));
}

void AIModelGenerationClient::download(const std::string& path, const boost::filesystem::path& destination,
                                       size_t size_limit, PathFn on_complete, ErrorFn on_error)
{
    cancel_current();
    if (!is_loopback_endpoint(m_endpoint)) {
        if (on_error)
            on_error("Generated files may only be downloaded from the loopback AI sidecar.");
        return;
    }

    const boost::filesystem::path partial = destination.string() + ".part";
    auto http = Http::get(url(path));
    http.header("X-OrcaSlicer-Client", "native").timeout_connect(5).timeout_max(180).size_limit(size_limit);
    http.on_complete([partial, destination, on_complete = std::move(on_complete), on_error](std::string body, unsigned) {
        boost::system::error_code ec;
        boost::filesystem::remove(partial, ec);
        boost::filesystem::ofstream stream(partial, std::ios::binary | std::ios::trunc);
        stream.write(body.data(), static_cast<std::streamsize>(body.size()));
        stream.close();
        if (!stream || body.empty()) {
            boost::filesystem::remove(partial, ec);
            if (on_error)
                on_error("Could not save the generated file.");
            return;
        }
        boost::filesystem::remove(destination, ec);
        boost::filesystem::rename(partial, destination, ec);
        if (ec) {
            boost::filesystem::remove(partial, ec);
            if (on_error)
                on_error("Could not finalize the generated file.");
            return;
        }
        if (on_complete)
            on_complete(destination);
    });
    http.on_error([partial, on_error = std::move(on_error)](std::string body, std::string error, unsigned status) {
        boost::system::error_code ec;
        boost::filesystem::remove(partial, ec);
        if (on_error)
            on_error(error_message(body, error, status));
    });
    m_active_request = http.perform();
}

void AIModelGenerationClient::cancel_current()
{
    if (m_active_request) {
        m_active_request->cancel();
        m_active_request.reset();
    }
}

} // namespace Slic3r::GUI
