#include "AIModelGenerationClient.hpp"

#include "AISidecarClient.hpp"

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <regex>
#include <utility>

namespace Slic3r::GUI {
namespace {

constexpr size_t MAX_PREVIEW_SIZE = 10 * 1024 * 1024;
constexpr size_t MAX_ARTIFACT_SIZE = 768 * 1024 * 1024;
constexpr size_t MAX_RECOMMENDATION_TEXT_SIZE = 2048;
constexpr size_t MAX_REFINEMENT_ISSUES = 6;
constexpr size_t MAX_REFINEMENT_SUMMARY_SIZE = 1024;
constexpr size_t MAX_REFINEMENT_PROMPT_SIZE = 1200;
constexpr size_t MAX_REFINEMENT_FIELD_SIZE = 512;
constexpr size_t MAX_PROVIDER_TASK_ID_SIZE = 256;

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

bool valid_recommendation_text(const std::string& value)
{
    return !value.empty() && value.size() <= MAX_RECOMMENDATION_TEXT_SIZE;
}

bool valid_style_id(const std::string& value)
{
    static const std::vector<std::string> allowed {
        "sculpture", "realistic", "cartoon", "low_poly", "relief", "diorama"
    };
    return std::find(allowed.begin(), allowed.end(), value) != allowed.end();
}

bool valid_hex_color(const std::string& value)
{
    static const std::regex pattern(R"(^#[0-9A-Fa-f]{6}$)");
    return std::regex_match(value, pattern);
}

bool valid_provider_task_id(const std::string& value)
{
    return !value.empty() && value.size() <= MAX_PROVIDER_TASK_ID_SIZE &&
           std::all_of(value.begin(), value.end(), [](unsigned char character) {
               return std::isalnum(character) || character == '-' || character == '_';
           });
}

bool valid_refinement_text(const std::string& value, size_t maximum_size)
{
    return !value.empty() && value.size() <= maximum_size;
}

bool valid_refinement_code(const std::string& value)
{
    static const std::vector<std::string> allowed {
        "degenerate_faces", "boundary_edges", "non_manifold_edges", "inconsistent_winding_edges",
        "flat_or_empty_axis", "repairable_boundary_edges", "repairable_non_manifold_edges",
        "repairable_inconsistent_winding_edges", "floating_disconnected_components",
        "tiny_detached_components", "visual_detached_artifacts", "thin_structural_components",
        "thin_local_wall_regions", "extreme_aspect_ratio", "weak_bed_contact",
        "visual_base_relationship", "high_downward_surface_ratio", "localized_overhang_regions",
        "dense_micro_triangles", "visual_subject_incomplete", "visual_semantic_incoherence",
        "visual_silhouette_unclear", "colors_outside_target_palette",
        "too_few_meaningful_target_palette_colors", "tiny_printable_color_regions",
        "visual_color_regions_unclear", "visual_identity_mismatch",
        "visual_material_color_mixing"
    };
    return std::find(allowed.begin(), allowed.end(), value) != allowed.end();
}

bool valid_refinement_category(const std::string& value)
{
    static const std::vector<std::string> allowed {
        "topology", "attachments", "thickness", "base", "overhang", "detail", "identity", "semantics", "color"
    };
    return std::find(allowed.begin(), allowed.end(), value) != allowed.end();
}

} // namespace

AIModelGenerationClient::AIModelGenerationClient(std::string endpoint)
    : m_endpoint(normalize_endpoint(std::move(endpoint)))
{
}

AIModelGenerationClient::~AIModelGenerationClient()
{
    cancel_current();
    for (const std::shared_ptr<Http>& request : m_background_requests) {
        if (request)
            request->cancel();
    }
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
                                               const std::vector<std::string>& palette,
                                               const PaletteRoles& palette_roles,
                                               bool palette_recommendation_confirmed,
                                               const std::string& style,
                                               const std::string& custom_style,
                                               const ImagePrintSettings& print_settings,
                                               StatusFn on_complete, ErrorFn on_error)
{
    post_json("/v1/orcaslicer/model-jobs/text",
              json::object({ { "request_id", request_id }, { "prompt", prompt }, { "palette", palette },
                             { "palette_roles", palette_roles }, { "style", style },
                             { "palette_recommendation_confirmed", palette_recommendation_confirmed },
                             { "custom_style", custom_style },
                             { "print", serialize_print_settings(print_settings) } }),
              std::move(on_complete), std::move(on_error));
}

void AIModelGenerationClient::preprocess_image(const std::string& request_id, const std::string& instruction,
                                                const boost::filesystem::path& image_path,
                                                 const std::vector<std::string>& palette,
                                                 const PaletteRoles& palette_roles,
                                                 bool palette_recommendation_confirmed,
                                                 const std::string& style,
                                                 const std::string& custom_style,
                                                 const ImagePrintSettings& print_settings,
                                                 StatusFn on_complete, ErrorFn on_error)
{
    cancel_current();
    if (!is_loopback_endpoint(m_endpoint)) {
        if (on_error)
            on_error("Image preprocessing requires a loopback AI sidecar endpoint.");
        return;
    }

    auto http = Http::post(url("/v1/orcaslicer/model-jobs/image"));
    AISidecarClient::configure_native_request(http);
    http.timeout_connect(5)
        .timeout_max(130)
        .size_limit(1024 * 1024)
        .form_add("request_id", request_id)
        .form_add("instruction", instruction)
        .form_add("palette", json(palette).dump())
        .form_add("palette_roles", json(palette_roles).dump())
        .form_add("palette_recommendation_confirmed", palette_recommendation_confirmed ? "true" : "false")
        .form_add("style", style)
        .form_add("custom_style", custom_style)
        .form_add("print", serialize_print_settings(print_settings).dump())
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

void AIModelGenerationClient::recommend_text_palette(const std::string& request_id, const std::string& prompt,
                                                       const std::string& style, const std::string& custom_style,
                                                       const ImagePrintSettings& print_settings,
                                                       StatusFn on_complete, ErrorFn on_error)
{
    post_json("/v1/orcaslicer/model-jobs/recommend-text-palette",
              json::object({ { "request_id", request_id }, { "prompt", prompt }, { "style", style },
                             { "custom_style", custom_style },
                             { "print", serialize_print_settings(print_settings) } }),
              std::move(on_complete), std::move(on_error));
}

void AIModelGenerationClient::recommend_image_palette(const std::string& request_id, const std::string& instruction,
                                                        const boost::filesystem::path& image_path,
                                                        const std::string& style, const std::string& custom_style,
                                                        const ImagePrintSettings& print_settings,
                                                        StatusFn on_complete, ErrorFn on_error)
{
    cancel_current();
    if (!is_loopback_endpoint(m_endpoint)) {
        if (on_error)
            on_error("Palette recommendation requires a loopback AI sidecar endpoint.");
        return;
    }
    auto http = Http::post(url("/v1/orcaslicer/model-jobs/recommend-image-palette"));
    AISidecarClient::configure_native_request(http);
    http.timeout_connect(5)
        .timeout_max(130)
        .size_limit(1024 * 1024)
        .form_add("request_id", request_id)
        .form_add("instruction", instruction)
        .form_add("style", style)
        .form_add("custom_style", custom_style)
        .form_add("print", serialize_print_settings(print_settings).dump())
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

void AIModelGenerationClient::recommend_image_style(const std::string& prompt,
                                                      const boost::filesystem::path& image_path,
                                                      StyleRecommendationFn on_complete,
                                                      ErrorFn on_error)
{
    cancel_current();
    if (!is_loopback_endpoint(m_endpoint)) {
        if (on_error)
            on_error("Style recommendation requires a loopback AI sidecar endpoint.");
        return;
    }
    auto http = Http::post(url("/v1/orcaslicer/model-style-recommendation"));
    http.header("X-OrcaSlicer-Client", "native")
        .timeout_connect(5)
        .timeout_max(20)
        .size_limit(64 * 1024)
        .form_add("instruction", prompt)
        .form_add_file("image", image_path, image_path.filename().string());
    http.on_complete([on_complete = std::move(on_complete), on_error](std::string body, unsigned) mutable {
        auto response = json::parse(body, nullptr, false);
        if (response.is_discarded() || !response.contains("recommendation") ||
            !response["recommendation"].is_object()) {
            if (on_error)
                on_error("AI sidecar returned an invalid style recommendation.");
            return;
        }
        const auto& value = response["recommendation"];
        StyleRecommendation recommendation;
        recommendation.primary = value.value("primary", std::string());
        recommendation.reason = value.value("reason", std::string());
        recommendation.confidence = value.value("confidence", std::string());
        if (value.contains("alternatives") && value["alternatives"].is_array()) {
            for (const auto& alternative : value["alternatives"])
                if (alternative.is_string()) recommendation.alternatives.emplace_back(alternative.get<std::string>());
        }
        const bool valid = valid_style_id(recommendation.primary) &&
            recommendation.alternatives.size() == 2 &&
            valid_style_id(recommendation.alternatives[0]) &&
            valid_style_id(recommendation.alternatives[1]) &&
            recommendation.primary != recommendation.alternatives[0] &&
            recommendation.primary != recommendation.alternatives[1] &&
            recommendation.alternatives[0] != recommendation.alternatives[1] &&
            valid_recommendation_text(recommendation.reason);
        if (!valid) {
            if (on_error)
                on_error("AI sidecar returned an invalid style recommendation.");
            return;
        }
        if (on_complete)
            on_complete(std::move(recommendation));
    });
    http.on_error([on_error = std::move(on_error)](std::string body, std::string error, unsigned status) {
        if (on_error)
            on_error(error_message(body, error, status));
    });
    m_active_request = http.perform();
}

void AIModelGenerationClient::confirm_palette(const std::string& job_id, const std::vector<std::string>& palette,
                                                const PaletteRoles& palette_roles,
                                                StatusFn on_complete, ErrorFn on_error)
{
    post_json("/v1/orcaslicer/model-jobs/" + job_id + "/confirm-palette",
              json::object({ { "palette", palette }, { "palette_roles", palette_roles } }),
              std::move(on_complete), std::move(on_error));
}

void AIModelGenerationClient::generate(const std::string& job_id, const std::string& prepared_prompt,
                                       const std::vector<std::string>& palette, const std::string& generation_profile,
                                       StatusFn on_complete, ErrorFn on_error)
{
    post_json("/v1/orcaslicer/model-jobs/" + job_id + "/generate",
              json::object({ { "prepared_prompt", prepared_prompt }, { "palette", palette },
                             { "generation_profile", generation_profile } }),
              std::move(on_complete), std::move(on_error));
}

void AIModelGenerationClient::retexture(const std::string& reference_job_id,
                                        const std::string& geometry_job_id,
                                        StatusFn on_complete, ErrorFn on_error)
{
    post_json("/v1/orcaslicer/model-jobs/" + reference_job_id + "/retexture",
              json::object({ { "geometry_job_id", geometry_job_id } }),
              std::move(on_complete), std::move(on_error));
}

void AIModelGenerationClient::get_status(const std::string& job_id, StatusFn on_complete, ErrorFn on_error)
{
    // Status polling must not cancel preview/artifact downloads. Large portrait
    // references often span more than one polling interval; cancelling them
    // here made the UI silently keep an older fallback image.
    cancel_active_request();
    if (!is_loopback_endpoint(m_endpoint)) {
        if (on_error)
            on_error("Model generation requires a loopback AI sidecar endpoint.");
        return;
    }

    auto http = Http::get(url("/v1/orcaslicer/model-jobs/" + job_id));
    AISidecarClient::configure_native_request(http);
    http.timeout_connect(5).timeout_max(15).size_limit(1024 * 1024);
    http.on_complete([this, on_complete = std::move(on_complete), on_error](std::string body, unsigned) mutable {
        parse_status_response(std::move(body), std::move(on_complete), on_error);
    });
    http.on_error([on_error = std::move(on_error)](std::string body, std::string error, unsigned status) {
        if (on_error)
            on_error(error_message(body, error, status));
    });
    m_active_request = http.perform();
}

void AIModelGenerationClient::get_latest(LatestFn on_complete, ErrorFn on_error)
{
    cancel_active_request();
    if (!is_loopback_endpoint(m_endpoint)) {
        if (on_error)
            on_error("Model generation requires a loopback AI sidecar endpoint.");
        return;
    }
    auto http = Http::get(url("/v1/orcaslicer/model-jobs/latest"));
    AISidecarClient::configure_native_request(http);
    http.timeout_connect(5).timeout_max(15).size_limit(1024 * 1024);
    http.on_complete([on_complete = std::move(on_complete), on_error](std::string body, unsigned) mutable {
        auto parsed = json::parse(body, nullptr, false);
        if (parsed.is_discarded() || !parsed.contains("job")) {
            if (on_error) on_error("AI sidecar returned an invalid model job response.");
            return;
        }
        if (parsed["job"].is_null()) {
            if (on_complete) on_complete(std::nullopt);
            return;
        }
        auto status = parse_job(parsed["job"]);
        if (!status) {
            if (on_error) on_error("AI sidecar returned an incomplete model job response.");
            return;
        }
        if (on_complete) on_complete(std::move(status));
    });
    http.on_error([on_error = std::move(on_error)](std::string body, std::string error, unsigned status) {
        if (on_error) on_error(error_message(body, error, status));
    });
    m_active_request = http.perform();
}

void AIModelGenerationClient::recheck(const std::string& job_id, StatusFn on_complete, ErrorFn on_error)
{
    post_json("/v1/orcaslicer/model-jobs/" + job_id + "/recheck", json::object(),
              std::move(on_complete), std::move(on_error));
}

void AIModelGenerationClient::visual_review(const std::string& job_id, StatusFn on_complete, ErrorFn on_error)
{
    post_json("/v1/orcaslicer/model-jobs/" + job_id + "/visual-review", json::object(),
              std::move(on_complete), std::move(on_error), 420);
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
    AISidecarClient::configure_native_request(http);
    http.timeout_connect(5).timeout_max(15).size_limit(1024 * 1024);
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

void AIModelGenerationClient::download_image_output(const std::string& job_id, const std::string& output,
                                                      const boost::filesystem::path& path,
                                                      PathFn on_complete, ErrorFn on_error)
{
    static const std::vector<std::string> allowed {
        "raw-preview", "strict-preview", "preview", "model-reference", "heatmap"
    };
    if (std::find(allowed.begin(), allowed.end(), output) == allowed.end()) {
        if (on_error)
            on_error("The requested printable image output is not supported.");
        return;
    }
    download("/v1/orcaslicer/model-jobs/" + job_id + "/" + output, path, MAX_PREVIEW_SIZE,
             std::move(on_complete), std::move(on_error));
}

void AIModelGenerationClient::download_input(const std::string& job_id, const boost::filesystem::path& path,
                                               PathFn on_complete, ErrorFn on_error)
{
    download("/v1/orcaslicer/model-jobs/" + job_id + "/input", path, MAX_PREVIEW_SIZE,
             std::move(on_complete), std::move(on_error));
}

void AIModelGenerationClient::download_artifact(const std::string& job_id, const std::string& format,
                                                 const boost::filesystem::path& path,
                                                 PathFn on_complete, ErrorFn on_error)
{
    if (format != "obj" && format != "3mf" && format != "stl") {
        if (on_error)
            on_error("The generated artifact format is not supported.");
        return;
    }
    download("/v1/orcaslicer/model-jobs/" + job_id + "/artifact", path, MAX_ARTIFACT_SIZE,
             std::move(on_complete), std::move(on_error));
}

void AIModelGenerationClient::record_journey_event(const std::string& event, const std::string& job_id)
{
    if (!is_loopback_endpoint(m_endpoint))
        return;
    json body = json::object({ { "event", event } });
    if (!job_id.empty())
        body["job_id"] = job_id;
    auto http = Http::post(url("/v1/orcaslicer/journey-events"));
    AISidecarClient::configure_native_request(http);
    http.header("Content-Type", "application/json")
        .timeout_connect(2)
        .timeout_max(5)
        .size_limit(64 * 1024)
        .set_post_body(body.dump());
    http.on_complete([](std::string, unsigned) {});
    http.on_error([](std::string, std::string, unsigned) {});
    m_background_requests.emplace_back(http.perform());
}

void AIModelGenerationClient::post_json(const std::string& path, const json& body,
                                        StatusFn on_complete, ErrorFn on_error, long timeout_seconds)
{
    cancel_current();
    if (!is_loopback_endpoint(m_endpoint)) {
        if (on_error)
            on_error("Model generation requires a loopback AI sidecar endpoint.");
        return;
    }

    auto http = Http::post(url(path));
    AISidecarClient::configure_native_request(http);
    http.header("Content-Type", "application/json")
        .timeout_connect(5)
        .timeout_max(timeout_seconds)
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

    auto status = parse_job(parsed["job"]);
    if (!status) {
        if (on_error)
            on_error("AI sidecar returned an incomplete model job response.");
        return;
    }
    if (on_complete)
        on_complete(std::move(*status));
}

std::optional<AIModelGenerationClient::JobStatus> AIModelGenerationClient::parse_job(const json& job)
{
    if (!job.is_object())
        return std::nullopt;
    JobStatus status;
    status.id = job.value("id", std::string());
    status.source = job.value("source", std::string());
    status.state = job.value("state", std::string());
    status.phase = job.value("phase", std::string());
    status.message = job.value("message", std::string());
    status.prepared_prompt = job.value("prepared_prompt", std::string());
    status.user_prompt = job.value("user_prompt", std::string());
    status.progress = std::clamp(job.value("progress", 0), 0, 100);
    status.face_limit = job.value("face_limit", 2000000);
    status.generation_profile = job.value("generation_profile", std::string("quality"));
    status.style = job.value("style", std::string());
    status.custom_style = job.value("custom_style", std::string());
    status.updated_at = job.value("updated_at", 0.0);
    if (job.contains("palette") && job["palette"].is_array()) {
        for (const auto& color : job["palette"])
            if (color.is_string()) status.palette.emplace_back(color.get<std::string>());
    }
    if (job.contains("palette_roles") && job["palette_roles"].is_object()) {
        for (const auto& [role, color] : job["palette_roles"].items())
            if (color.is_string()) status.palette_roles.emplace(role, color.get<std::string>());
    }
    status.palette_recommendation.confirmed = job.value("palette_recommendation_confirmed", false);
    if (job.contains("palette_recommendation") && job["palette_recommendation"].is_object()) {
        const auto& recommendation = job["palette_recommendation"];
        const std::string summary = recommendation.value("summary", std::string());
        std::vector<PaletteRecommendationColor> colors;
        std::vector<std::string> roles;
        const std::vector<std::string> allowed_roles { "primary", "structure", "light", "accent" };
        if (valid_recommendation_text(summary) && recommendation.contains("colors") && recommendation["colors"].is_array()) {
            for (const auto& value : recommendation["colors"]) {
                if (!value.is_object())
                    continue;
                PaletteRecommendationColor color;
                color.hex = value.value("hex", std::string());
                color.name = value.value("name", std::string());
                color.role = value.value("role", std::string());
                color.usage = value.value("usage", std::string());
                color.reason = value.value("reason", std::string());
                if (!valid_hex_color(color.hex) || !valid_recommendation_text(color.name) ||
                    !valid_recommendation_text(color.usage) || !valid_recommendation_text(color.reason) ||
                    std::find(allowed_roles.begin(), allowed_roles.end(), color.role) == allowed_roles.end() ||
                    std::find(roles.begin(), roles.end(), color.role) != roles.end())
                    continue;
                roles.emplace_back(color.role);
                colors.emplace_back(std::move(color));
            }
        }
        if (colors.size() == allowed_roles.size()) {
            status.palette_recommendation.available = true;
            status.palette_recommendation.summary = summary;
            status.palette_recommendation.colors = std::move(colors);
        }
    }
    if (job.contains("print") && job["print"].is_object()) {
        const auto& print = job["print"];
        status.print_settings.width_mm = print.value("width_mm", 160.0);
        status.print_settings.nozzle_mm = print.value("nozzle_mm", 0.4);
        status.print_settings.line_width_mm = print.value("line_width_mm", 0.4);
        status.print_settings.minimum_feature_mm = print.value("minimum_feature_mm", 0.8);
        status.print_settings.color_distance = print.value("color_distance", std::string("ciede2000"));
        status.print_settings.print_mode = print.value("print_mode", std::string("solid_regions"));
        status.print_settings.shadow_color = print.value("shadow_color", std::string("blue"));
    }
    if (job.contains("input") && job["input"].is_object())
        status.input_ready = job["input"].value("ready", false);
    if (job.contains("preview") && job["preview"].is_object())
        status.preview_ready = job["preview"].value("ready", false);
    if (job.contains("image_outputs") && job["image_outputs"].is_object()) {
        const auto& outputs = job["image_outputs"];
        const auto ready = [&outputs](const char* name) {
            return outputs.contains(name) && outputs[name].is_object() && outputs[name].value("ready", false);
        };
        status.raw_preview_ready = ready("raw_preview");
        status.strict_preview_ready = ready("strict_preview");
        status.model_reference_ready = ready("model_reference");
        status.heatmap_ready = ready("heatmap");
        status.metadata_ready = ready("metadata");
    }
    if (job.contains("image_metrics") && job["image_metrics"].is_object()) {
        const auto& metrics = job["image_metrics"];
        status.image_score = metrics.value("score", 0.0);
        status.mean_color_error = metrics.value("mean_color_error", 0.0);
        status.small_region_ratio = metrics.value("small_region_ratio_after", 0.0);
        status.changed_pixel_ratio = metrics.value("changed_pixel_ratio", 0.0);
        status.boundary_complexity = metrics.value("boundary_complexity", 0.0);
        status.minimum_feature_px = metrics.value("minimum_feature_px", 0);
        status.meaningful_palette_count = metrics.value("meaningful_palette_count", 0);
        status.meaningful_subject_color_count = metrics.value("meaningful_subject_color_count", 0);
        status.printable_subject_area_ratio = metrics.value("printable_subject_area_ratio", 0.0);
        status.largest_subject_component_ratio = metrics.value("largest_subject_component_ratio", 0.0);
        status.largest_detached_subject_diagonal_ratio = metrics.value(
            "largest_detached_subject_diagonal_ratio", 0.0);
        status.palette_quality_ok = metrics.value("palette_quality_ok", true);
        status.material_fragmentation_ok = metrics.value("material_fragmentation_ok", true);
        // The generic preview check runs before the portrait-specific geometry
        // reference is built.  Prefer the later paid-task preflight result when
        // it is available; otherwise the UI can keep enabling "Generate 3D"
        // even though the exact image that would be sent to Tripo was rejected.
        const nlohmann::json* input_quality = nullptr;
        if (metrics.contains("generation_input_quality") && metrics["generation_input_quality"].is_object())
            input_quality = &metrics["generation_input_quality"];
        else if (metrics.contains("model_input_quality") && metrics["model_input_quality"].is_object())
            input_quality = &metrics["model_input_quality"];
        if (input_quality != nullptr) {
            status.model_input_score = input_quality->value("score", 0.0);
            status.model_input_eligible = input_quality->value("model_input_eligible", true);
            const auto read_codes = [input_quality](const char* name, std::vector<std::string>& output) {
                if (!input_quality->contains(name) || !(*input_quality)[name].is_array())
                    return;
                for (const auto& value : (*input_quality)[name])
                    if (value.is_string()) output.push_back(value.get<std::string>());
            };
            read_codes("blockers", status.model_input_blockers);
            read_codes("warnings", status.model_input_warnings);
        }
    }
    if (job.contains("provider_failure") && job["provider_failure"].is_object()) {
        const auto& failure = job["provider_failure"];
        status.provider_error_code = failure.value("code", std::string());
        status.provider_error_category = failure.value("category", std::string());
        status.provider_error_retryable = failure.value("retryable", false);
        status.provider_error_ambiguous = failure.value("ambiguous", false);
    }
    if (job.contains("provider_tasks") && job["provider_tasks"].is_object()) {
        const auto& tasks = job["provider_tasks"];
        const std::string provider = tasks.value("provider", std::string());
        const std::string generation_task_id = tasks.value("generation_task_id", std::string());
        const std::string conversion_task_id = tasks.value("conversion_task_id", std::string());
        if (provider == "tripo" && valid_provider_task_id(generation_task_id)) {
            status.provider_name = provider;
            status.provider_task_id = generation_task_id;
            if (valid_provider_task_id(conversion_task_id))
                status.provider_conversion_task_id = conversion_task_id;
        }
    }
    if (job.contains("artifact") && job["artifact"].is_object()) {
        status.artifact_ready = job["artifact"].value("ready", false);
        status.artifact_format = job["artifact"].value("format", std::string());
        status.artifact_color_encoding = job["artifact"].value("color_encoding", std::string());
        status.artifact_size = job["artifact"].value("size_bytes", size_t(0));
    }
    if (job.contains("model_quality") && job["model_quality"].is_object()) {
        const auto& quality = job["model_quality"];
        status.model_quality.status = quality.value("status", std::string());
        status.model_quality.available = !status.model_quality.status.empty();
        const auto read_codes = [&quality](const char* name, std::vector<std::string>& output) {
            if (!quality.contains(name) || !quality[name].is_array())
                return;
            for (const auto& code : quality[name])
                if (code.is_string()) output.emplace_back(code.get<std::string>());
        };
        read_codes("errors", status.model_quality.errors);
        read_codes("warnings", status.model_quality.warnings);
        if (quality.contains("thresholds") && quality["thresholds"].is_object()) {
            const auto& thresholds = quality["thresholds"];
            if (thresholds.contains("min_local_wall_thickness_mm") &&
                thresholds["min_local_wall_thickness_mm"].is_number()) {
                const double threshold = thresholds["min_local_wall_thickness_mm"].get<double>();
                if (std::isfinite(threshold) && threshold > 0.0) {
                    status.model_quality.local_wall_thickness_threshold_available = true;
                    status.model_quality.minimum_local_wall_thickness_mm = threshold;
                }
            }
        }
        if (quality.contains("metrics") && quality["metrics"].is_object()) {
            const auto& metrics = quality["metrics"];
            status.model_quality.vertex_count = metrics.value("vertex_count", size_t(0));
            status.model_quality.face_count = metrics.value("face_count", size_t(0));
            status.model_quality.component_count = metrics.value("component_count", size_t(0));
            status.model_quality.tiny_component_count = metrics.value("tiny_component_count", size_t(0));
            status.model_quality.largest_component_face_ratio = metrics.value("largest_component_face_ratio", 0.0);
            status.model_quality.contact_span_ratio = metrics.value("contact_span_ratio", 0.0);
            status.model_quality.bed_contact_area_available = metrics.contains("bed_contact_area_ratio");
            status.model_quality.bed_contact_area_ratio = metrics.value("bed_contact_area_ratio", 0.0);
            status.model_quality.downward_surface_ratio = metrics.value("downward_surface_ratio", 0.0);
            status.model_quality.elevated_downward_surface_ratio_available =
                metrics.contains("elevated_downward_surface_ratio");
            status.model_quality.elevated_downward_surface_ratio =
                metrics.value("elevated_downward_surface_ratio", 0.0);
            status.model_quality.overhang_region_metrics_available =
                metrics.contains("significant_overhang_region_count");
            status.model_quality.significant_overhang_region_count =
                metrics.value("significant_overhang_region_count", size_t(0));
            status.model_quality.component_thickness_available =
                metrics.value("component_thickness_available", false);
            status.model_quality.thin_component_count = metrics.value("thin_component_count", size_t(0));
            if (metrics.contains("minimum_component_thickness_mm") &&
                metrics["minimum_component_thickness_mm"].is_number())
                status.model_quality.minimum_component_thickness_mm =
                    metrics["minimum_component_thickness_mm"].get<double>();
            status.model_quality.local_thickness_available =
                metrics.value("local_thickness_available", false);
            status.model_quality.local_thickness_sample_count =
                metrics.value("local_thickness_sample_count", size_t(0));
            status.model_quality.thin_local_surface_sample_count =
                metrics.value("thin_local_surface_sample_count", size_t(0));
            if (metrics.contains("minimum_sampled_local_thickness_mm") &&
                metrics["minimum_sampled_local_thickness_mm"].is_number())
                status.model_quality.minimum_sampled_local_thickness_mm =
                    metrics["minimum_sampled_local_thickness_mm"].get<double>();
            status.model_quality.thin_local_region_count =
                metrics.value("thin_local_region_count", size_t(0));
            status.model_quality.reported_thin_local_region_count =
                metrics.value("reported_thin_local_region_count", size_t(0));
            status.model_quality.target_palette_metrics_available =
                metrics.value("target_palette_metrics_available", false);
            status.model_quality.target_palette_color_count =
                metrics.value("target_palette_color_count", size_t(0));
            status.model_quality.used_target_palette_color_count =
                metrics.value("used_target_palette_color_count", size_t(0));
            status.model_quality.meaningful_target_palette_color_count =
                metrics.value("meaningful_target_palette_color_count", size_t(0));
            status.model_quality.required_meaningful_target_palette_color_count =
                metrics.value("required_meaningful_target_palette_color_count", size_t(0));
            if (metrics.contains("target_palette_surface_coverage_ratio") &&
                metrics["target_palette_surface_coverage_ratio"].is_number()) {
                const double coverage = metrics["target_palette_surface_coverage_ratio"].get<double>();
                if (std::isfinite(coverage))
                    status.model_quality.target_palette_surface_coverage_ratio = std::clamp(coverage, 0.0, 1.0);
            }
            status.model_quality.target_palette_diversity_ok =
                metrics.value("target_palette_diversity_ok", false);
            status.model_quality.repairable_topology = metrics.value("repairable_topology", false);
        }
        if (quality.contains("evidence") && quality["evidence"].is_object()) {
            constexpr size_t max_evidence_faces = 256;
            const auto& evidence = quality["evidence"];
            if (evidence.contains("thin_local_face_indices") &&
                evidence["thin_local_face_indices"].is_array()) {
                for (const auto& face_index : evidence["thin_local_face_indices"]) {
                    if (!face_index.is_number_unsigned() ||
                        status.model_quality.thin_local_face_indices.size() >= max_evidence_faces)
                        continue;
                    status.model_quality.thin_local_face_indices.emplace_back(face_index.get<size_t>());
                }
            }
            if (evidence.contains("thin_local_regions") && evidence["thin_local_regions"].is_array()) {
                constexpr size_t max_regions = 16;
                size_t total_region_faces = 0;
                for (const auto& item : evidence["thin_local_regions"]) {
                    if (!item.is_object() || status.model_quality.thin_local_regions.size() >= max_regions)
                        continue;
                    AIModelGenerationClient::ModelQuality::ThinLocalRegion region;
                    if (item.contains("sample_count") && item["sample_count"].is_number_unsigned())
                        region.sample_count = item["sample_count"].get<size_t>();
                    if (item.contains("sampled_area_mm2") && item["sampled_area_mm2"].is_number())
                        region.sampled_area_mm2 = item["sampled_area_mm2"].get<double>();
                    if (item.contains("minimum_thickness_mm") && item["minimum_thickness_mm"].is_number())
                        region.minimum_thickness_mm = item["minimum_thickness_mm"].get<double>();
                    if (item.contains("representative_face_index") &&
                        item["representative_face_index"].is_number_unsigned())
                        region.representative_face_index = item["representative_face_index"].get<size_t>();
                    if (item.contains("face_indices") && item["face_indices"].is_array()) {
                        for (const auto& face_index : item["face_indices"]) {
                            if (!face_index.is_number_unsigned() || total_region_faces >= max_evidence_faces)
                                continue;
                            region.face_indices.emplace_back(face_index.get<size_t>());
                            ++total_region_faces;
                        }
                    }
                    status.model_quality.thin_local_regions.emplace_back(std::move(region));
                }
            }
            if (evidence.contains("target_palette_surface_usage") &&
                evidence["target_palette_surface_usage"].is_array()) {
                for (const auto& item : evidence["target_palette_surface_usage"]) {
                    if (!item.is_object() || status.model_quality.target_palette_surface_usage.size() >= 4)
                        continue;
                    AIModelGenerationClient::ModelQuality::TargetPaletteUsage usage;
                    usage.color = item.value("color", std::string());
                    if (!valid_hex_color(usage.color) || !item.contains("surface_ratio") ||
                        !item["surface_ratio"].is_number())
                        continue;
                    const double ratio = item["surface_ratio"].get<double>();
                    if (!std::isfinite(ratio))
                        continue;
                    usage.surface_ratio = std::clamp(ratio, 0.0, 1.0);
                    usage.meaningful = item.value("meaningful", false);
                    status.model_quality.target_palette_surface_usage.emplace_back(std::move(usage));
                }
            }
        }
    }
    if (job.contains("visual_quality") && job["visual_quality"].is_object()) {
        const auto& quality = job["visual_quality"];
        status.visual_quality.status = quality.value("status", std::string());
        status.visual_quality.available = !status.visual_quality.status.empty();
        status.visual_quality.import_recommended = quality.value("import_recommended", true);
        status.visual_quality.score = std::clamp(quality.value("score", 0), 0, 100);
        status.visual_quality.confidence = std::clamp(quality.value("confidence", 0.0), 0.0, 1.0);
        status.visual_quality.summary = quality.value("summary", std::string());
        const auto read_codes = [&quality](const char* name, std::vector<std::string>& output) {
            if (!quality.contains(name) || !quality[name].is_array())
                return;
            for (const auto& code : quality[name])
                if (code.is_string()) output.emplace_back(code.get<std::string>());
        };
        read_codes("errors", status.visual_quality.errors);
        read_codes("warnings", status.visual_quality.warnings);
        read_codes("blocking_warnings", status.visual_quality.blocking_warnings);
        if (quality.contains("checks") && quality["checks"].is_object()) {
            for (const auto& [name, check] : quality["checks"].items())
                if (check.is_object() && check.value("status", std::string()) == "review")
                    status.visual_quality.check_reasons.emplace(name, check.value("reason", std::string()));
        }
    }
    if (job.contains("refinement") && job["refinement"].is_object()) {
        const auto& refinement = job["refinement"];
        if (refinement.value("schema", 0) == 1 && refinement.value("available", false) &&
            refinement.contains("summary") && refinement["summary"].is_string() &&
            refinement.contains("prompt_suffix") && refinement["prompt_suffix"].is_string() &&
            refinement.contains("issues") && refinement["issues"].is_array()) {
            const std::string summary = refinement["summary"].get<std::string>();
            const std::string prompt_suffix = refinement["prompt_suffix"].get<std::string>();
            if (valid_refinement_text(summary, MAX_REFINEMENT_SUMMARY_SIZE) &&
                valid_refinement_text(prompt_suffix, MAX_REFINEMENT_PROMPT_SIZE)) {
                for (const auto& value : refinement["issues"]) {
                    if (!value.is_object() || status.refinement.issues.size() >= MAX_REFINEMENT_ISSUES ||
                        !value.contains("code") || !value["code"].is_string() ||
                        !value.contains("category") || !value["category"].is_string() ||
                        !value.contains("title") || !value["title"].is_string() ||
                        !value.contains("instruction") || !value["instruction"].is_string())
                        continue;
                    ModelRefinementAdvice::Issue issue;
                    issue.code = value["code"].get<std::string>();
                    issue.category = value["category"].get<std::string>();
                    issue.title = value["title"].get<std::string>();
                    issue.instruction = value["instruction"].get<std::string>();
                    if (!valid_refinement_code(issue.code) || !valid_refinement_category(issue.category) ||
                        !valid_refinement_text(issue.title, MAX_REFINEMENT_FIELD_SIZE) ||
                        !valid_refinement_text(issue.instruction, MAX_REFINEMENT_FIELD_SIZE))
                        continue;
                    status.refinement.issues.emplace_back(std::move(issue));
                }
                if (!status.refinement.issues.empty()) {
                    status.refinement.available = true;
                    status.refinement.summary = summary;
                    status.refinement.prompt_suffix = prompt_suffix;
                }
            }
        }
    }
    if (status.id.empty() || status.state.empty()) {
        return std::nullopt;
    }
    return status;
}

AIModelGenerationClient::json AIModelGenerationClient::serialize_print_settings(const ImagePrintSettings& settings)
{
    return json::object({
        { "width_mm", settings.width_mm },
        { "nozzle_mm", settings.nozzle_mm },
        { "line_width_mm", settings.line_width_mm },
        { "minimum_feature_mm", settings.minimum_feature_mm },
        { "color_distance", settings.color_distance },
        { "print_mode", settings.print_mode },
        { "shadow_color", settings.shadow_color },
    });
}

void AIModelGenerationClient::download(const std::string& path, const boost::filesystem::path& destination,
                                       size_t size_limit, PathFn on_complete, ErrorFn on_error)
{
    if (!is_loopback_endpoint(m_endpoint)) {
        if (on_error)
            on_error("Generated files may only be downloaded from the loopback AI sidecar.");
        return;
    }

    const boost::filesystem::path partial = destination.string() + ".part";
    auto http = Http::get(url(path));
    AISidecarClient::configure_native_request(http);
    http.timeout_connect(5).timeout_max(180).size_limit(size_limit);
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
    m_download_requests.emplace_back(http.perform());
}

void AIModelGenerationClient::cancel_current()
{
    cancel_active_request();
    for (const std::shared_ptr<Http>& request : m_download_requests) {
        if (request)
            request->cancel();
    }
    m_download_requests.clear();
}

void AIModelGenerationClient::cancel_active_request()
{
    if (m_active_request) {
        m_active_request->cancel();
        m_active_request.reset();
    }
}

} // namespace Slic3r::GUI
