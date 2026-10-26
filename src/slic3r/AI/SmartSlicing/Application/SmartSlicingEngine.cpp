#include "SmartSlicingEngine.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace Slic3r::AI::SmartSlicing {
namespace {

constexpr double MIN_SAFE_NOZZLE_MM = 0.05;
constexpr double MIN_SAFE_LAYER_MM = 0.04;
constexpr double MAX_SAFE_LAYER_MM = 0.80;
constexpr double MIN_SAFE_SPEED_MM_S = 5.0;
constexpr double MIN_MATERIAL_DENSITY = 0.1;
constexpr double MAX_MATERIAL_DENSITY = 4.0;

double clamp_finite(double value, double minimum, double maximum, double fallback)
{
    if (!std::isfinite(value))
        return fallback;
    return std::clamp(value, minimum, maximum);
}

double positive_or(double value, double fallback)
{
    return std::isfinite(value) && value > 0.0 ? value : fallback;
}

double safe_extent(const SmartSliceBoundingBox& box)
{
    const double extent = box.width_mm() * box.depth_mm() * box.height_mm();
    return std::isfinite(extent) && extent > 0.0 ? extent : 0.0;
}

std::string quality_rating(double score)
{
    if (score >= 90.0) return "excellent";
    if (score >= 75.0) return "good";
    if (score >= 55.0) return "fair";
    return "risk";
}

void add_warning(SliceRecommendation& recommendation, const char* warning)
{
    recommendation.warnings.emplace_back(warning);
}

ModelMetrics metrics_from_snapshot(const TriangleMeshSnapshot& mesh)
{
    ModelMetrics metrics;
    metrics.triangle_count = mesh.triangles.size();
    if (mesh.vertices.empty() || mesh.triangles.empty()) {
        metrics.finite_geometry = false;
        return metrics;
    }

    metrics.bounding_box.min_x_mm = metrics.bounding_box.min_y_mm =
        metrics.bounding_box.min_z_mm = std::numeric_limits<double>::max();
    metrics.bounding_box.max_x_mm = metrics.bounding_box.max_y_mm =
        metrics.bounding_box.max_z_mm = std::numeric_limits<double>::lowest();
    for (const MeshPoint3d& vertex : mesh.vertices) {
        if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) || !std::isfinite(vertex.z)) {
            metrics.finite_geometry = false;
            return metrics;
        }
        metrics.bounding_box.min_x_mm = std::min(metrics.bounding_box.min_x_mm, vertex.x);
        metrics.bounding_box.min_y_mm = std::min(metrics.bounding_box.min_y_mm, vertex.y);
        metrics.bounding_box.min_z_mm = std::min(metrics.bounding_box.min_z_mm, vertex.z);
        metrics.bounding_box.max_x_mm = std::max(metrics.bounding_box.max_x_mm, vertex.x);
        metrics.bounding_box.max_y_mm = std::max(metrics.bounding_box.max_y_mm, vertex.y);
        metrics.bounding_box.max_z_mm = std::max(metrics.bounding_box.max_z_mm, vertex.z);
    }

    constexpr double pi = 3.14159265358979323846;
    constexpr double top_micro_slope_degrees = 15.0;
    constexpr double thin_wall_feature_mm = 0.8;
    double signed_volume = 0.0;
    double overhang_45_area = 0.0;
    double overhang_60_area = 0.0;
    double top_micro_area = 0.0;
    double thin_wall_area = 0.0;
    std::map<std::pair<uint32_t, uint32_t>, size_t> edge_counts;

    const auto subtract = [](const MeshPoint3d& lhs, const MeshPoint3d& rhs) {
        return MeshPoint3d{lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
    };
    const auto cross = [](const MeshPoint3d& lhs, const MeshPoint3d& rhs) {
        return MeshPoint3d{lhs.y * rhs.z - lhs.z * rhs.y,
                           lhs.z * rhs.x - lhs.x * rhs.z,
                           lhs.x * rhs.y - lhs.y * rhs.x};
    };
    const auto dot = [](const MeshPoint3d& lhs, const MeshPoint3d& rhs) {
        return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
    };
    const auto norm = [&dot](const MeshPoint3d& value) {
        return std::sqrt(dot(value, value));
    };
    const auto edge_length = [&subtract, &norm](const MeshPoint3d& lhs, const MeshPoint3d& rhs) {
        return norm(subtract(lhs, rhs));
    };

    for (const MeshTriangle& triangle : mesh.triangles) {
        if (std::any_of(triangle.vertex_indices.begin(), triangle.vertex_indices.end(),
                        [&](uint32_t index) { return index >= mesh.vertices.size(); })) {
            metrics.finite_geometry = false;
            return metrics;
        }
        const MeshPoint3d& a = mesh.vertices[triangle.vertex_indices[0]];
        const MeshPoint3d& b = mesh.vertices[triangle.vertex_indices[1]];
        const MeshPoint3d& c = mesh.vertices[triangle.vertex_indices[2]];
        const MeshPoint3d area_vector = cross(subtract(b, a), subtract(c, a));
        const double area = 0.5 * norm(area_vector);
        if (!std::isfinite(area) || area <= 1e-12)
            continue;
        metrics.surface_area_mm2 += area;
        signed_volume += dot(a, cross(b, c)) / 6.0;
        const MeshPoint3d normal{area_vector.x / (2.0 * area),
                                 area_vector.y / (2.0 * area),
                                 area_vector.z / (2.0 * area)};
        if (normal.z < 0.0) {
            const double angle = std::acos(std::clamp(-normal.z, 0.0, 1.0)) * 180.0 / pi;
            if (angle >= 45.0) overhang_45_area += area;
            if (angle >= 60.0) overhang_60_area += area;
        } else if (normal.z >= std::cos(top_micro_slope_degrees * pi / 180.0)) {
            top_micro_area += area;
        }
        if (std::min({edge_length(a, b), edge_length(b, c), edge_length(c, a)}) <= thin_wall_feature_mm)
            thin_wall_area += area;

        const std::array<std::pair<uint32_t, uint32_t>, 3> edges{{
            {std::min(triangle.vertex_indices[0], triangle.vertex_indices[1]),
             std::max(triangle.vertex_indices[0], triangle.vertex_indices[1])},
            {std::min(triangle.vertex_indices[1], triangle.vertex_indices[2]),
             std::max(triangle.vertex_indices[1], triangle.vertex_indices[2])},
            {std::min(triangle.vertex_indices[2], triangle.vertex_indices[0]),
             std::max(triangle.vertex_indices[2], triangle.vertex_indices[0])}}};
        for (const auto& edge : edges) ++edge_counts[edge];
    }
    const double area = metrics.surface_area_mm2;
    metrics.volume_mm3 = std::abs(signed_volume);
    metrics.high_overhang_area_ratio_45_deg = area > 0.0 ? overhang_45_area / area : 0.0;
    metrics.severe_overhang_area_ratio_60_deg = area > 0.0 ? overhang_60_area / area : 0.0;
    metrics.top_micro_slope_surface_ratio = area > 0.0 ? top_micro_area / area : 0.0;
    metrics.thin_wall_feature_ratio = area > 0.0 ? thin_wall_area / area : 0.0;
    metrics.closed_mesh = std::all_of(edge_counts.begin(), edge_counts.end(),
                                      [](const auto& edge) { return edge.second == 2; });
    return metrics;
}

} // namespace

SmartAnalysisResult SmartSlicingEngine::AnalyzeAndPrecompute(
    const TriangleMeshSnapshot& mesh, const MachinePreset& machine, const FilamentPreset& filament) const
{
    const ModelMetrics metrics = metrics_from_snapshot(mesh);
    return AnalyzeAndPrecompute(metrics, machine, filament);
}

SmartAnalysisResult SmartSlicingEngine::AnalyzeAndPrecompute(
    const ModelMetrics& metrics, const MachinePreset& machine, const FilamentPreset& filament) const
{
    SmartAnalysisResult result;
    result.metrics = metrics;
    if (!metrics.usable()) {
        result.diagnostic_code = metrics.finite_geometry ?
            "smart_slice_geometry_unavailable" : "smart_slice_geometry_non_finite";
        return result;
    }

    const double nozzle = clamp_finite(machine.nozzle_diameter_mm, MIN_SAFE_NOZZLE_MM, 2.0, 0.4);
    const double minimum_layer = clamp_finite(
        machine.minimum_layer_height_mm, MIN_SAFE_LAYER_MM, MAX_SAFE_LAYER_MM, nozzle * 0.10);
    const double maximum_layer = std::max(
        minimum_layer, clamp_finite(machine.maximum_layer_height_mm, minimum_layer,
                                    MAX_SAFE_LAYER_MM, nozzle * 0.70));
    // Zero-valued additive fields preserve the existing desktop policy. The
    // machine motion limit is never a substitute for a process wall speed.
    const double legacy_wall = clamp_finite(machine.maximum_outer_wall_speed_mm_s,
                                             MIN_SAFE_SPEED_MM_S, 500.0, 80.0);
    const double machine_speed = positive_or(machine.machine_max_speed_mm_s, legacy_wall);
    const double wall_speed_cap = positive_or(machine.outer_wall_speed_cap_mm_s, legacy_wall);
    const double maximum_speed = std::max(MIN_SAFE_SPEED_MM_S,
        std::min({machine_speed, wall_speed_cap, 500.0}));
    const double machine_flow = clamp_finite(machine.maximum_volumetric_speed_mm3_s,
                                             0.1, 200.0, 12.0);
    const double filament_flow = clamp_finite(filament.maximum_volumetric_speed_mm3_s,
                                              0.1, 200.0, machine_flow);
    const double effective_flow_limit = std::min(machine_flow, filament_flow);
    const double density = clamp_finite(filament.density_g_cm3,
                                        MIN_MATERIAL_DENSITY, MAX_MATERIAL_DENSITY, 1.24);
    const double extrusion_multiplier = clamp_finite(filament.extrusion_multiplier,
                                                     0.5, 2.0, 1.0);
    const double volume = positive_or(metrics.volume_mm3, safe_extent(metrics.bounding_box) * 0.25);
    const double surface = positive_or(metrics.surface_area_mm2, volume > 0.0 ?
                                       std::pow(volume, 2.0 / 3.0) * 6.0 : 0.0);
    const double overhang_risk = std::clamp(
        0.70 * metrics.high_overhang_area_ratio_45_deg +
        1.10 * metrics.severe_overhang_area_ratio_60_deg, 0.0, 1.0);
    const double micro_slope = std::clamp(metrics.top_micro_slope_surface_ratio, 0.0, 1.0);
    const double thin_wall = std::clamp(metrics.thin_wall_feature_ratio, 0.0, 1.0);

    result.calibrated_to_native_baseline = machine.native_baseline_time_seconds > 0.0;
    result.native_baseline_time_seconds = machine.native_baseline_time_seconds;
    result.native_baseline_material_grams = machine.native_baseline_material_grams;
    result.profile_hash = machine.profile_hash;
    result.capability_hash = machine.capability_hash;
    result.effective_config_hash = machine.effective_config_hash;
#if defined(ORCA_SMART_SLICING_SOURCE_COMMIT)
    result.source_commit = ORCA_SMART_SLICING_SOURCE_COMMIT;
#endif

    struct ModeDefaults {
        SliceMode mode;
        double layer_ratio;
        double fixed_layer;
        int walls;
        double density;
        const char* pattern;
        double speed_ratio;
        double support_threshold;
        bool support;
        bool adaptive;
        bool arachne;
    };
    const std::array<ModeDefaults, 3> defaults{{
        {SliceMode::BALANCED, 0.50, 0.20, 3, 15.0, "gyroid", 0.90, 52.0,
         overhang_risk > 0.04, false, true},
        {SliceMode::SPEED, 0.65, 0.24, 2, 10.0, "grid", 1.05,
         60.0, metrics.severe_overhang_area_ratio_60_deg > 0.03, false, true},
        {SliceMode::QUALITY, 0.30, 0.12, 3, 15.0, "gyroid", 0.72, 45.0,
         overhang_risk > 0.015, micro_slope > 0.10, true},
    }};

    struct RawEstimate
    {
        double seconds{0.0};
        double material_grams{0.0};
    };

    // Fast path-length proxy.  The model volume is split into wall, infill and
    // support volumes, then each role is evaluated with its own configured
    // speed and volumetric-flow cap.  This keeps the estimate cheap while
    // following the same physical limits as Orca's native G-code generator.
    const auto estimate_raw = [&](double layer_height, int wall_loops,
                                  double infill_density, double outer_speed,
                                  bool support, bool adaptive) -> RawEstimate {
        const double effective_layer_height = std::clamp(
            adaptive ? layer_height * 1.20 : layer_height,
            minimum_layer, std::min(maximum_layer, nozzle * 0.75));
        const double wall_factor = std::clamp(
            0.14 + surface * nozzle * static_cast<double>(wall_loops) /
                std::max(volume, 1e-6) * 0.16,
            0.12, 0.55);
        const double fill_factor = std::clamp(
            0.28 + (infill_density / 100.0) * 0.72,
            0.28, 1.0);
        const double support_factor = support ? std::clamp(
            overhang_risk * 0.55 + metrics.severe_overhang_area_ratio_60_deg * 0.20,
            0.0, 0.25) : 0.0;
        const double wall_volume = volume * wall_factor;
        const double infill_volume = volume * (1.0 - wall_factor) * fill_factor;
        const double support_volume = volume * support_factor;
        const double outer_wall_volume = wall_volume * std::clamp(
            0.32 + 0.04 * static_cast<double>(wall_loops), 0.32, 0.52);
        const double inner_wall_volume = std::max(0.0, wall_volume - outer_wall_volume);
        const double outer_line_width = nozzle * 1.05;
        const double inner_line_width = nozzle * 1.05;
        const double infill_line_width = nozzle * 1.05;
        const double support_line_width = nozzle;
        const auto flow = [&](double width, double speed) {
            return std::max(0.1, std::min(effective_flow_limit,
                width * effective_layer_height * std::max(MIN_SAFE_SPEED_MM_S, speed)));
        };
        const double outer_flow = flow(outer_line_width, outer_speed);
        const double inner_flow = flow(inner_line_width, machine.inner_wall_speed_mm_s);
        const double infill_flow = flow(infill_line_width, machine.sparse_infill_speed_mm_s);
        const double support_flow = flow(support_line_width, machine.support_speed_mm_s);
        const double extrusion_time = outer_wall_volume / outer_flow +
            inner_wall_volume / inner_flow + infill_volume / infill_flow +
            support_volume / support_flow;
        const double layer_count = std::max(1.0,
            metrics.bounding_box.height_mm() / effective_layer_height);
        const double travel_distance_per_layer = std::max(
            12.0, (metrics.bounding_box.width_mm() + metrics.bounding_box.depth_mm()) * 0.85);
        const double travel_time = layer_count * travel_distance_per_layer /
            std::max(MIN_SAFE_SPEED_MM_S, machine.travel_speed_mm_s);
        const double effective_acceleration = machine.travel_acceleration_mm_s2 > 0.0
            ? std::max(1.0, (machine.default_acceleration_mm_s2 +
                            machine.outer_wall_acceleration_mm_s2 +
                            machine.travel_acceleration_mm_s2) / 3.0)
            : machine.default_acceleration_mm_s2;
        const double acceleration_factor = std::clamp(
            1.12 - effective_acceleration * 0.00008,
            0.92, 1.12);
        const double layer_overhead = layer_count *
            (0.18 + (support ? 0.04 : 0.0) + (adaptive ? 0.02 : 0.0));
        const double seconds = std::max(1.0,
            (extrusion_time + travel_time) * acceleration_factor + layer_overhead);
        const double material_volume = std::max(0.0,
            (wall_volume + infill_volume + support_volume) * extrusion_multiplier);
        return {seconds, material_volume * density / 1000.0};
    };

    const RawEstimate baseline_raw = estimate_raw(
        clamp_finite(machine.baseline_layer_height_mm, minimum_layer, maximum_layer, 0.20),
        std::clamp(machine.baseline_wall_loops, 1, 6),
        clamp_finite(machine.baseline_infill_density_percent, 0.0, 100.0, 15.0),
        maximum_speed, machine.baseline_support_enabled, false);
    const double time_calibration = machine.native_baseline_time_seconds > 0.0 && baseline_raw.seconds > 0.0 ?
        std::clamp(machine.native_baseline_time_seconds / baseline_raw.seconds, 0.45, 2.50) : 1.0;
    const double material_calibration = machine.native_baseline_material_grams > 0.0 &&
        baseline_raw.material_grams > 0.0 ?
        std::clamp(machine.native_baseline_material_grams / baseline_raw.material_grams, 0.50, 2.00) : 1.0;

    for (size_t index = 0; index < defaults.size(); ++index) {
        const ModeDefaults& policy = defaults[index];
        SliceRecommendation& recommendation = result.recommendations[index];
        recommendation.mode = policy.mode;
        recommendation.layer_height_mm = clamp_finite(
            std::max(policy.fixed_layer, nozzle * policy.layer_ratio), minimum_layer,
            std::min(maximum_layer, nozzle * 0.75), nozzle * 0.5);
        recommendation.wall_loops = std::clamp(policy.walls, 1, 6);
        recommendation.sparse_infill_density_percent = std::clamp(policy.density, 0.0, 100.0);
        recommendation.sparse_infill_pattern = policy.pattern;
        recommendation.outer_wall_speed_mm_s = clamp_finite(
            maximum_speed * policy.speed_ratio, MIN_SAFE_SPEED_MM_S, maximum_speed,
            std::max(MIN_SAFE_SPEED_MM_S, maximum_speed * 0.5));
        recommendation.enable_support = policy.support;
        recommendation.support_type = policy.support ? "tree(auto)" : "none";
        recommendation.support_threshold_angle_deg = clamp_finite(
            policy.support_threshold, 25.0, 75.0, 50.0);
        recommendation.adaptive_layer_height = policy.adaptive;
        recommendation.arachne_wall_generator = policy.arachne;
        recommendation.line_width_mm = clamp_finite(
            nozzle * (policy.mode == SliceMode::QUALITY ? 1.0 : 1.05),
            nozzle * 0.90, nozzle * 1.25, nozzle);

        const RawEstimate raw = estimate_raw(
            recommendation.layer_height_mm,
            recommendation.wall_loops,
            recommendation.sparse_infill_density_percent,
            recommendation.outer_wall_speed_mm_s,
            recommendation.enable_support,
            recommendation.adaptive_layer_height);
        recommendation.estimated_material_grams = std::max(
            0.0, raw.material_grams * material_calibration);
        recommendation.estimated_time_seconds = std::max(
            1.0, raw.seconds * time_calibration);

        const double quality_penalty =
            overhang_risk * (recommendation.enable_support ? 8.0 : 32.0) +
            (recommendation.layer_height_mm / std::max(nozzle, 0.05)) * 16.0 +
            (recommendation.outer_wall_speed_mm_s / std::max(maximum_speed, 1.0)) * 12.0 +
            thin_wall * 12.0;
        recommendation.surface_quality_score = std::clamp(100.0 - quality_penalty, 0.0, 100.0);
        recommendation.surface_quality_rating = quality_rating(recommendation.surface_quality_score);
        recommendation.overhang_risk_score = overhang_risk;

        if (metrics.high_overhang_area_ratio_45_deg > 0.10)
            add_warning(recommendation, "high_overhang_area");
        if (metrics.severe_overhang_area_ratio_60_deg > 0.05 && !recommendation.enable_support)
            add_warning(recommendation, "severe_overhang_without_support");
        if (thin_wall > 0.10)
            add_warning(recommendation, "thin_wall_features");
        if (!metrics.closed_mesh)
            add_warning(recommendation, "open_or_non_manifold_mesh");
        if (recommendation.adaptive_layer_height)
            add_warning(recommendation, "adaptive_layer_height_requires_native_capability");
    }

    result.completed = true;
    result.diagnostic_code = "smart_slice_analysis_completed";
    return result;
}

} // namespace Slic3r::AI::SmartSlicing
