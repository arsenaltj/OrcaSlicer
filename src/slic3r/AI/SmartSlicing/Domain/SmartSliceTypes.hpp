#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

// Shared contract for the lightweight recommendation cards.  The suffix in
// every numeric field name is intentional: this object crosses the GUI/worker
// boundary and must not depend on native Orca configuration types.
struct SmartSliceBoundingBox
{
    double min_x_mm{0.0};
    double min_y_mm{0.0};
    double min_z_mm{0.0};
    double max_x_mm{0.0};
    double max_y_mm{0.0};
    double max_z_mm{0.0};

    double width_mm() const { return max_x_mm - min_x_mm; }
    double depth_mm() const { return max_y_mm - min_y_mm; }
    double height_mm() const { return max_z_mm - min_z_mm; }
};

struct ModelMetrics
{
    SmartSliceBoundingBox bounding_box;
    double volume_mm3{0.0};
    double surface_area_mm2{0.0};
    double high_overhang_area_ratio_45_deg{0.0};
    double severe_overhang_area_ratio_60_deg{0.0};
    double top_micro_slope_surface_ratio{0.0};
    double thin_wall_feature_ratio{0.0};
    size_t triangle_count{0};
    bool closed_mesh{false};
    bool finite_geometry{true};

    bool usable() const
    {
        return finite_geometry && surface_area_mm2 > 0.0 &&
               bounding_box.width_mm() >= 0.0 && bounding_box.depth_mm() >= 0.0 &&
               bounding_box.height_mm() >= 0.0;
    }
};

// Immutable snapshots populated by the Orca adapter.  They intentionally do
// not expose Preset or DynamicPrintConfig, so the worker remains UI agnostic.
struct MachinePreset
{
    std::string id;
    double nozzle_diameter_mm{0.4};
    double minimum_layer_height_mm{0.05};
    double maximum_layer_height_mm{0.28};
    // Machine-wide speed ceiling and the independent outer-wall policy cap.
    // Keep maximum_outer_wall_speed_mm_s below as a source-compatible fallback.
    double machine_max_speed_mm_s{0.0};
    double outer_wall_speed_cap_mm_s{0.0};
    double maximum_outer_wall_speed_mm_s{80.0};
    double maximum_volumetric_speed_mm3_s{12.0};

    // Role speeds are captured from the active Orca process configuration.
    // The lightweight estimator must not use the outer-wall speed as a proxy
    // for every path role: infill, top surfaces, supports and travel have
    // different native limits and dominate different model shapes.
    double inner_wall_speed_mm_s{100.0};
    double sparse_infill_speed_mm_s{120.0};
    double internal_solid_infill_speed_mm_s{100.0};
    double top_surface_speed_mm_s{60.0};
    double support_speed_mm_s{80.0};
    double travel_speed_mm_s{180.0};
    double default_acceleration_mm_s2{1000.0};
    double outer_wall_acceleration_mm_s2{1000.0};
    double travel_acceleration_mm_s2{0.0};

    // The current Orca process is used as a calibration reference whenever a
    // valid native G-code result is already available for this exact plate.
    // A zero time means that the estimator is running without a native
    // baseline and will report a heuristic result.
    double baseline_layer_height_mm{0.20};
    int baseline_wall_loops{3};
    double baseline_infill_density_percent{15.0};
    bool baseline_support_enabled{false};
    double native_baseline_time_seconds{0.0};
    double native_baseline_material_grams{0.0};
    std::string profile_hash;
    std::string capability_hash;
    std::string effective_config_hash;
};

struct FilamentPreset
{
    std::string id;
    std::vector<std::string> material_types {"UNKNOWN"};
    double density_g_cm3{1.24};
    double extrusion_multiplier{1.0};
    double maximum_volumetric_speed_mm3_s{12.0};
};

enum class SliceMode { BALANCED, SPEED, QUALITY };

const char* slice_mode_id(SliceMode mode);

struct SliceRecommendation
{
    SliceMode mode{SliceMode::BALANCED};

    // Native Orca process concepts, represented with stable DTO types.
    double layer_height_mm{0.2};
    int wall_loops{3};
    double sparse_infill_density_percent{15.0};
    std::string sparse_infill_pattern{"gyroid"};
    double outer_wall_speed_mm_s{50.0};
    bool enable_support{false};
    std::string support_type{"tree(auto)"};
    double support_threshold_angle_deg{50.0};
    bool adaptive_layer_height{false};
    bool arachne_wall_generator{true};
    double line_width_mm{0.42};

    // These values are fast estimates. Native trial or official slice
    // statistics supersede them before they are shown as final time metrics.
    double estimated_time_seconds{0.0};
    double estimated_material_grams{0.0};
    double surface_quality_score{0.0};
    std::string surface_quality_rating;
    double overhang_risk_score{0.0};
    std::vector<std::string> warnings;
};

struct SmartAnalysisResult
{
    static constexpr uint32_t CONTRACT_VERSION = 1;

    uint32_t contract_version{CONTRACT_VERSION};
    bool completed{false};
    std::string diagnostic_code;
    ModelMetrics metrics;
    std::array<SliceRecommendation, 3> recommendations{};
    bool calibrated_to_native_baseline{false};
    double native_baseline_time_seconds{0.0};
    double native_baseline_material_grams{0.0};

    // Optional identity metadata. Empty values preserve the desktop call shape.
    std::string profile_hash;
    std::string capability_hash;
    std::string effective_config_hash;
    std::string algorithm_version{"smart-slicing-core/1"};
    std::string source_commit;

    const SliceRecommendation* find(SliceMode mode) const;
    SliceRecommendation* find(SliceMode mode);
};

} // namespace Slic3r::AI::SmartSlicing
