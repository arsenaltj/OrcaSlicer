#include "SlicingRecommendationClient.hpp"

#include "slic3r/GUI/AISidecarClient.hpp"
#include "slic3r/Utils/Http.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

namespace Slic3r::GUI {
namespace {

using Json = nlohmann::json;
using AI::SmartSlicing::SliceMode;
using AI::SmartSlicing::SliceRecommendation;

constexpr const char* CAPABILITY = "slicing_parameter_recommendation";
constexpr const char* CAPABILITY_PATH = "/v1/orcaslicer/text-capabilities/slicing_parameter_recommendation";
constexpr size_t MAX_TEXT_BYTES = 500;
constexpr size_t MAX_WARNINGS = 8;

const std::array<const char*, 3> GOALS {"balanced", "speed", "quality"};
const std::array<const char*, 7> PARAMETERS {
    "layer_height", "wall_loops", "sparse_infill_density", "sparse_infill_pattern",
    "outer_wall_speed", "enable_support", "support_type",
};
const std::vector<std::string> INFILL_PATTERNS {
    "adaptivecubic", "cubic", "grid", "gyroid", "honeycomb", "line", "rectilinear",
};
const std::vector<std::string> SUPPORT_TYPES {
    "none", "normal(auto)", "normal(manual)", "tree(auto)", "tree(manual)",
};

bool exact_object(const Json& value, std::initializer_list<const char*> fields)
{
    if (!value.is_object() || value.size() != fields.size())
        return false;
    return std::all_of(fields.begin(), fields.end(), [&](const char* field) { return value.contains(field); });
}

bool bounded_text(const Json& value, std::string& output, size_t maximum = MAX_TEXT_BYTES)
{
    if (!value.is_string()) return false;
    output = value.get<std::string>();
    return !output.empty() && output.size() <= maximum;
}

bool finite_number(const Json& value, double minimum, double maximum, double& output)
{
    if (!value.is_number() || value.is_boolean()) return false;
    output = value.get<double>();
    return std::isfinite(output) && output >= minimum && output <= maximum;
}

bool valid_diagnostic_id(const std::string& value)
{
    return !value.empty() && value.size() <= 64;
}

size_t mode_index(SliceMode mode)
{
    switch (mode) {
    case SliceMode::BALANCED: return 0;
    case SliceMode::SPEED: return 1;
    case SliceMode::QUALITY: return 2;
    }
    return 0;
}

SliceMode mode_for_goal(const std::string& goal)
{
    if (goal == "speed") return SliceMode::SPEED;
    if (goal == "quality") return SliceMode::QUALITY;
    return SliceMode::BALANCED;
}

std::string canonical_infill_pattern(const std::string& value)
{
    return value == "lines" ? "line" : value;
}

Json baseline_json(const SliceRecommendation& recommendation)
{
    return {
        {"goal", AI::SmartSlicing::slice_mode_id(recommendation.mode)},
        {"layer_height", recommendation.layer_height_mm},
        {"wall_loops", recommendation.wall_loops},
        {"sparse_infill_density", recommendation.sparse_infill_density_percent},
        {"sparse_infill_pattern", canonical_infill_pattern(recommendation.sparse_infill_pattern)},
        {"outer_wall_speed", recommendation.outer_wall_speed_mm_s},
        {"enable_support", recommendation.enable_support},
        {"support_type", recommendation.enable_support ? recommendation.support_type : "none"},
        {"support_threshold_angle", recommendation.support_threshold_angle_deg},
        {"line_width", recommendation.line_width_mm},
        {"wall_generator", recommendation.arachne_wall_generator ? "arachne" : "classic"},
        {"adaptive_layer_height", recommendation.adaptive_layer_height},
    };
}

bool apply_change(const Json& change, const SlicingRecommendationRequest& request,
                  SliceRecommendation& recommendation, std::set<std::string>& changed,
                  std::string* diagnostic)
{
    if (!exact_object(change, {"parameter", "value", "rationale"}) ||
        !change["parameter"].is_string()) {
        if (diagnostic) *diagnostic = "cloud_change_fields_invalid";
        return false;
    }
    const std::string parameter = change["parameter"].get<std::string>();
    if (std::find(PARAMETERS.begin(), PARAMETERS.end(), parameter) == PARAMETERS.end() ||
        !changed.insert(parameter).second) {
        if (diagnostic) *diagnostic = "cloud_change_parameter_invalid";
        return false;
    }
    std::string rationale;
    if (!bounded_text(change["rationale"], rationale)) {
        if (diagnostic) *diagnostic = "cloud_change_rationale_invalid";
        return false;
    }
    const Json& value = change["value"];
    double number = 0.0;
    if (parameter == "layer_height") {
        const double minimum = std::max(0.04, request.machine.minimum_layer_height_mm);
        const double maximum = std::min({0.8, request.machine.maximum_layer_height_mm,
                                         request.machine.nozzle_diameter_mm * 0.75});
        if (!finite_number(value, minimum, maximum, number) ||
            std::abs(number - recommendation.layer_height_mm) > 0.08 + 1e-9) {
            if (diagnostic) *diagnostic = "cloud_layer_height_out_of_range";
            return false;
        }
        recommendation.layer_height_mm = number;
    } else if (parameter == "wall_loops") {
        if (!value.is_number_integer() || value.is_boolean()) {
            if (diagnostic) *diagnostic = "cloud_wall_loops_invalid";
            return false;
        }
        const int walls = value.get<int>();
        if (walls < 1 || walls > 8 || std::abs(walls - recommendation.wall_loops) > 2) {
            if (diagnostic) *diagnostic = "cloud_wall_loops_out_of_range";
            return false;
        }
        recommendation.wall_loops = walls;
    } else if (parameter == "sparse_infill_density") {
        if (!finite_number(value, 0.0, 60.0, number) ||
            std::abs(number - recommendation.sparse_infill_density_percent) > 15.0 + 1e-9) {
            if (diagnostic) *diagnostic = "cloud_infill_out_of_range";
            return false;
        }
        recommendation.sparse_infill_density_percent = number;
    } else if (parameter == "outer_wall_speed") {
        const double maximum = std::min(300.0, request.machine.maximum_outer_wall_speed_mm_s);
        if (!finite_number(value, 5.0, maximum, number) ||
            std::abs(number - recommendation.outer_wall_speed_mm_s) > 40.0 + 1e-9) {
            if (diagnostic) *diagnostic = "cloud_outer_wall_speed_out_of_range";
            return false;
        }
        recommendation.outer_wall_speed_mm_s = number;
    } else if (parameter == "enable_support") {
        if (!value.is_boolean()) {
            if (diagnostic) *diagnostic = "cloud_support_switch_invalid";
            return false;
        }
        recommendation.enable_support = value.get<bool>();
    } else {
        if (!value.is_string()) {
            if (diagnostic) *diagnostic = "cloud_enum_invalid";
            return false;
        }
        const std::string selected = parameter == "sparse_infill_pattern" ?
            canonical_infill_pattern(value.get<std::string>()) : value.get<std::string>();
        const auto& allowed = parameter == "sparse_infill_pattern" ? INFILL_PATTERNS : SUPPORT_TYPES;
        if (std::find(allowed.begin(), allowed.end(), selected) == allowed.end()) {
            if (diagnostic) *diagnostic = "cloud_enum_unavailable";
            return false;
        }
        if (parameter == "sparse_infill_pattern") recommendation.sparse_infill_pattern = selected;
        else recommendation.support_type = selected;
    }
    return true;
}

} // namespace

void SlicingRecommendationClient::RequestHandle::cancel()
{
    if (m_request) {
        m_request->cancel();
        m_request.reset();
    }
}

SlicingRecommendationClient::SlicingRecommendationClient(std::string endpoint) : m_endpoint(std::move(endpoint))
{
    while (!m_endpoint.empty() && m_endpoint.back() == '/') m_endpoint.pop_back();
}

std::string SlicingRecommendationClient::url() const { return m_endpoint + CAPABILITY_PATH; }

Json SlicingRecommendationClient::build_request_json(const SlicingRecommendationRequest& request)
{
    const auto& metrics = request.metrics;
    const auto& machine = request.machine;
    const auto& filament = request.filament;
    Json baselines = Json::array();
    for (const auto& recommendation : request.local_baselines.recommendations)
        baselines.push_back(baseline_json(recommendation));
    const double minimum_layer = std::max(0.04, machine.minimum_layer_height_mm);
    const double maximum_layer = std::min({0.8, machine.maximum_layer_height_mm,
                                           machine.nozzle_diameter_mm * 0.75});
    const double maximum_speed = std::min(300.0, machine.maximum_outer_wall_speed_mm_s);
    return {
        {"request_id", request.request_id},
        {"model", {
            {"object_count", request.object_count},
            {"width_mm", metrics.bounding_box.width_mm()},
            {"depth_mm", metrics.bounding_box.depth_mm()},
            {"height_mm", metrics.bounding_box.height_mm()},
            {"volume_mm3", metrics.volume_mm3},
            {"surface_area_mm2", metrics.surface_area_mm2},
            {"triangle_count", metrics.triangle_count},
            {"closed_mesh", metrics.closed_mesh},
            {"high_overhang_ratio", metrics.high_overhang_area_ratio_45_deg},
            {"severe_overhang_ratio", metrics.severe_overhang_area_ratio_60_deg},
            {"top_micro_slope_ratio", metrics.top_micro_slope_surface_ratio},
            {"thin_wall_ratio", metrics.thin_wall_feature_ratio},
        }},
        {"machine", {
            {"nozzle_diameter_mm", machine.nozzle_diameter_mm},
            {"minimum_layer_height_mm", minimum_layer},
            {"maximum_layer_height_mm", maximum_layer},
            {"maximum_outer_wall_speed_mm_s", machine.maximum_outer_wall_speed_mm_s},
            {"maximum_volumetric_speed_mm3_s", machine.maximum_volumetric_speed_mm3_s},
            {"default_acceleration_mm_s2", machine.default_acceleration_mm_s2},
            {"outer_wall_acceleration_mm_s2", machine.outer_wall_acceleration_mm_s2},
        }},
        {"material", {
            {"types", filament.material_types},
            {"density_g_cm3", filament.density_g_cm3},
            {"maximum_volumetric_speed_mm3_s", filament.maximum_volumetric_speed_mm3_s},
        }},
        {"baselines", std::move(baselines)},
        {"policy", {
            {"allowed_changes", PARAMETERS},
            {"ranges", {
                {"layer_height", {{"minimum", minimum_layer}, {"maximum", maximum_layer}, {"maximum_delta", 0.08}}},
                {"wall_loops", {{"minimum", 1}, {"maximum", 8}, {"maximum_delta", 2}}},
                {"sparse_infill_density", {{"minimum", 0}, {"maximum", 60}, {"maximum_delta", 15}}},
                {"sparse_infill_pattern", {{"values", INFILL_PATTERNS}}},
                {"outer_wall_speed", {{"minimum", 5}, {"maximum", maximum_speed}, {"maximum_delta", 40}}},
                {"enable_support", {{"values", {false, true}}}},
                {"support_type", {{"values", SUPPORT_TYPES}}},
            }},
        }},
    };
}

bool SlicingRecommendationClient::parse_availability(const std::string& body, Availability& result)
{
    const Json parsed = Json::parse(body, nullptr, false);
    if (parsed.is_discarded() || !exact_object(parsed, {"capability", "available", "reason", "diagnostic_id"}) ||
        parsed.value("capability", std::string()) != CAPABILITY || !parsed["available"].is_boolean() ||
        !parsed["reason"].is_string() || !parsed["diagnostic_id"].is_string())
        return false;
    result.available = parsed["available"].get<bool>();
    result.reason = parsed["reason"].get<std::string>();
    result.diagnostic_id = parsed["diagnostic_id"].get<std::string>();
    return result.reason.size() <= 100 && valid_diagnostic_id(result.diagnostic_id);
}

bool SlicingRecommendationClient::parse_result(
    const std::string& body, const SlicingRecommendationRequest& request,
    SlicingRecommendationResult& result, std::string* diagnostic, Error* error)
{
    if (error) *error = {};
    const Json parsed = Json::parse(body, nullptr, false);
    if (parsed.is_discarded() || !exact_object(parsed, {"request_id", "status", "recommendations", "warnings", "diagnostic_id"}) ||
        !parsed["request_id"].is_string() || parsed["request_id"].get<std::string>() != request.request_id ||
        parsed.value("status", std::string()) != "success" || !parsed["recommendations"].is_array() ||
        parsed["recommendations"].size() != GOALS.size() || !parsed["warnings"].is_array() ||
        parsed["warnings"].size() > MAX_WARNINGS || !parsed["diagnostic_id"].is_string()) {
        if (diagnostic) *diagnostic = "cloud_response_envelope_invalid";
        return false;
    }
    result = {};
    result.request_id = request.request_id;
    result.analysis = request.local_baselines;
    const auto diagnostic_id = parsed["diagnostic_id"].get<std::string>();
    if (error && valid_diagnostic_id(diagnostic_id)) error->diagnostic_id = diagnostic_id;
    std::set<std::string> seen_goals;
    for (const Json& row : parsed["recommendations"]) {
        if (!exact_object(row, {"goal", "changes", "summary"}) || !row["goal"].is_string() ||
            !row["changes"].is_array() || row["changes"].size() > PARAMETERS.size()) {
            if (diagnostic) *diagnostic = "cloud_recommendation_fields_invalid";
            return false;
        }
        const std::string goal = row["goal"].get<std::string>();
        if (std::find(GOALS.begin(), GOALS.end(), goal) == GOALS.end() || !seen_goals.insert(goal).second) {
            if (diagnostic) *diagnostic = "cloud_recommendation_goal_invalid";
            return false;
        }
        const SliceMode mode = mode_for_goal(goal);
        if (error) error->goal = goal;
        SliceRecommendation* recommendation = result.analysis.find(mode);
        std::string summary;
        if (recommendation == nullptr || recommendation->mode != mode || !bounded_text(row["summary"], summary)) {
            if (diagnostic) *diagnostic = "cloud_recommendation_baseline_invalid";
            return false;
        }
        recommendation->sparse_infill_pattern = canonical_infill_pattern(recommendation->sparse_infill_pattern);
        std::set<std::string> changed;
        for (const Json& change : row["changes"]) {
            if (error) {
                error->parameter.clear();
                error->value.clear();
            }
            if (error && change.is_object()) {
                const auto parameter = change.find("parameter");
                if (parameter != change.end() && parameter->is_string() &&
                    std::find(PARAMETERS.begin(), PARAMETERS.end(), parameter->get<std::string>()) != PARAMETERS.end()) {
                    error->parameter = parameter->get<std::string>();
                    const auto value = change.find("value");
                    if (value != change.end() && (value->is_number() || value->is_boolean() ||
                        (value->is_string() && value->get_ref<const std::string&>().size() <= 64)))
                        error->value = value->dump();
                }
            }
            if (!apply_change(change, request, *recommendation, changed, diagnostic)) return false;
        }
        if ((recommendation->enable_support && recommendation->support_type == "none") ||
            (!recommendation->enable_support && changed.count("support_type") && recommendation->support_type != "none")) {
            if (error) {
                error->parameter = "support_type";
                error->value = Json(recommendation->support_type).dump();
            }
            if (diagnostic) *diagnostic = "cloud_support_configuration_invalid";
            return false;
        }
        result.summaries[mode_index(mode)] = std::move(summary);
        if (error) {
            error->goal.clear();
            error->parameter.clear();
            error->value.clear();
        }
    }
    if (seen_goals.size() != GOALS.size()) {
        if (diagnostic) *diagnostic = "cloud_recommendation_goal_missing";
        return false;
    }
    for (const Json& warning : parsed["warnings"]) {
        std::string value;
        if (!bounded_text(warning, value)) {
            if (diagnostic) *diagnostic = "cloud_warning_invalid";
            return false;
        }
        result.warnings.push_back(std::move(value));
    }
    result.diagnostic_id = diagnostic_id;
    if (!valid_diagnostic_id(result.diagnostic_id)) {
        if (diagnostic) *diagnostic = "cloud_diagnostic_id_invalid";
        return false;
    }
    result.analysis.completed = true;
    result.analysis.diagnostic_code.clear();
    if (diagnostic) diagnostic->clear();
    if (error) *error = {};
    return true;
}

SlicingRecommendationClient::Error SlicingRecommendationClient::parse_error(
    const std::string& body, const std::string& transport_error, unsigned status)
{
    Error result;
    const Json parsed = Json::parse(body, nullptr, false);
    if (!parsed.is_discarded() && parsed.is_object()) {
        if (parsed.contains("error") && parsed["error"].is_object()) {
            result.code = parsed["error"].value("code", std::string());
            result.message = parsed["error"].value("message", std::string());
        }
        result.diagnostic_id = parsed.value("diagnostic_id", std::string());
    }
    if (result.message.empty()) result.message = !transport_error.empty() ? transport_error :
        "Cloud slicing recommendation failed with HTTP " + std::to_string(status) + ".";
    if (result.code.empty()) result.code = status == 0 ? "transport_error" : "http_error";
    return result;
}

std::shared_ptr<SlicingRecommendationClient::RequestHandle> SlicingRecommendationClient::get_availability(
    AvailabilityFn on_complete, ErrorFn on_error) const
{
    if (!AISidecarClient::is_loopback_endpoint(m_endpoint)) {
        if (on_error) on_error({"invalid_endpoint", "Cloud slicing requires a loopback Sidecar endpoint.", {}});
        return {};
    }
    auto http = Http::get(url());
    AISidecarClient::configure_native_request(http);
    http.timeout_connect(5).timeout_max(60).size_limit(64 * 1024);
    http.on_complete([on_complete = std::move(on_complete), on_error](std::string body, unsigned) mutable {
        Availability result;
        if (!parse_availability(body, result)) {
            if (on_error) on_error({"invalid_response", "AI Sidecar returned an invalid capability response.", {}});
        } else if (on_complete) on_complete(std::move(result));
    });
    http.on_error([on_error = std::move(on_error)](std::string body, std::string error, unsigned status) {
        if (on_error) on_error(parse_error(body, error, status));
    });
    return std::shared_ptr<RequestHandle>(new RequestHandle(http.perform()));
}

std::shared_ptr<SlicingRecommendationClient::RequestHandle> SlicingRecommendationClient::recommend(
    const SlicingRecommendationRequest& request, ResultFn on_complete, ErrorFn on_error) const
{
    if (!AISidecarClient::is_loopback_endpoint(m_endpoint)) {
        if (on_error) on_error({"invalid_endpoint", "Cloud slicing requires a loopback Sidecar endpoint.", {}});
        return {};
    }
    auto http = Http::post(url());
    AISidecarClient::configure_native_request(http);
    http.header("Content-Type", "application/json").timeout_connect(15).timeout_max(190)
        .size_limit(256 * 1024).set_post_body(build_request_json(request).dump());
    http.on_complete([request, on_complete = std::move(on_complete), on_error](std::string body, unsigned) mutable {
        SlicingRecommendationResult result;
        std::string diagnostic;
        Error error;
        if (!parse_result(body, request, result, &diagnostic, &error)) {
            error.code = diagnostic.empty() ? "invalid_response" : diagnostic;
            error.message = "AI Sidecar returned an invalid cloud slicing response.";
            if (on_error) on_error(std::move(error));
        } else if (on_complete) on_complete(std::move(result));
    });
    http.on_error([on_error = std::move(on_error)](std::string body, std::string error, unsigned status) {
        if (on_error) on_error(parse_error(body, error, status));
    });
    return std::shared_ptr<RequestHandle>(new RequestHandle(http.perform()));
}

} // namespace Slic3r::GUI
