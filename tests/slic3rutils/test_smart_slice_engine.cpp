#include "slic3r/AI/SmartSlicing/Application/SmartSlicingEngine.hpp"

#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace Slic3r::AI::SmartSlicing;

TEST_CASE("smart slice rules produce three differentiated safe plans", "[SmartSlice][SmartSlicingEngine]")
{
    ModelMetrics metrics;
    metrics.bounding_box = {0.0, 0.0, 0.0, 40.0, 35.0, 60.0};
    metrics.volume_mm3 = 42'000.0;
    metrics.surface_area_mm2 = 8'000.0;
    metrics.high_overhang_area_ratio_45_deg = 0.20;
    metrics.severe_overhang_area_ratio_60_deg = 0.08;
    metrics.top_micro_slope_surface_ratio = 0.15;
    metrics.thin_wall_feature_ratio = 0.05;
    metrics.closed_mesh = true;

    MachinePreset machine;
    machine.nozzle_diameter_mm = 0.4;
    machine.minimum_layer_height_mm = 0.05;
    machine.maximum_layer_height_mm = 0.30;
    FilamentPreset filament;

    const SmartAnalysisResult result = SmartSlicingEngine().AnalyzeAndPrecompute(metrics, machine, filament);
    REQUIRE(result.completed);
    const auto& balanced = result.recommendations[0];
    const auto& speed = result.recommendations[1];
    const auto& quality = result.recommendations[2];
    CHECK(balanced.mode == SliceMode::BALANCED);
    CHECK(speed.mode == SliceMode::SPEED);
    CHECK(quality.mode == SliceMode::QUALITY);
    CHECK(speed.layer_height_mm > balanced.layer_height_mm);
    CHECK(quality.layer_height_mm < balanced.layer_height_mm);
    CHECK(speed.wall_loops < quality.wall_loops);
    CHECK(speed.estimated_time_seconds < quality.estimated_time_seconds);
    CHECK(quality.surface_quality_score > speed.surface_quality_score);
    CHECK(balanced.enable_support);
    CHECK(speed.enable_support);
    CHECK(quality.enable_support);
    for (const auto& plan : result.recommendations) {
        CHECK(plan.layer_height_mm >= machine.minimum_layer_height_mm);
        CHECK(plan.layer_height_mm <= machine.nozzle_diameter_mm * 0.75);
        CHECK(plan.line_width_mm >= machine.nozzle_diameter_mm * 0.90);
        CHECK(plan.line_width_mm <= machine.nozzle_diameter_mm * 1.25);
    }
}

TEST_CASE("smart slice snapshot analysis rejects invalid geometry and estimates a closed tetrahedron",
          "[SmartSlice][SmartSlicingEngine]")
{
    TriangleMeshSnapshot tetra;
    tetra.vertices = {{0.0, 0.0, 0.0}, {10.0, 0.0, 0.0}, {0.0, 10.0, 0.0}, {0.0, 0.0, 10.0}};
    tetra.triangles = {{{0, 2, 1}}, {{0, 1, 3}}, {{1, 2, 3}}, {{2, 0, 3}}};
    const SmartAnalysisResult result = SmartSlicingEngine().AnalyzeAndPrecompute(
        tetra, MachinePreset{}, FilamentPreset{});
    REQUIRE(result.completed);
    CHECK(result.metrics.volume_mm3 > 100.0);
    CHECK(result.metrics.surface_area_mm2 > 0.0);
    CHECK(result.metrics.closed_mesh);

    tetra.triangles[0].vertex_indices = {0, 1, 99};
    const SmartAnalysisResult invalid = SmartSlicingEngine().AnalyzeAndPrecompute(
        tetra, MachinePreset{}, FilamentPreset{});
    CHECK_FALSE(invalid.completed);
    CHECK_FALSE(invalid.metrics.finite_geometry);
}

TEST_CASE("smart slice estimates preserve native baseline scale and mode ordering",
          "[SmartSlice][SmartSlicingEngine]")
{
    ModelMetrics metrics;
    metrics.bounding_box = {0.0, 0.0, 0.0, 120.0, 80.0, 80.0};
    metrics.volume_mm3 = 240'000.0;
    metrics.surface_area_mm2 = 42'000.0;
    metrics.high_overhang_area_ratio_45_deg = 0.06;
    metrics.severe_overhang_area_ratio_60_deg = 0.02;
    metrics.closed_mesh = true;

    MachinePreset machine;
    machine.native_baseline_time_seconds = 3.0 * 60.0 * 60.0;
    machine.native_baseline_material_grams = 36.0;
    machine.baseline_layer_height_mm = 0.20;
    machine.baseline_wall_loops = 3;
    machine.baseline_infill_density_percent = 15.0;
    FilamentPreset filament;

    const SmartAnalysisResult result = SmartSlicingEngine().AnalyzeAndPrecompute(metrics, machine, filament);
    REQUIRE(result.completed);
    REQUIRE(result.calibrated_to_native_baseline);
    CHECK(result.native_baseline_time_seconds == machine.native_baseline_time_seconds);
    CHECK(result.native_baseline_material_grams == machine.native_baseline_material_grams);
    CHECK(result.recommendations[1].estimated_time_seconds < result.recommendations[0].estimated_time_seconds);
    CHECK(result.recommendations[0].estimated_time_seconds < result.recommendations[2].estimated_time_seconds);
    CHECK(result.recommendations[1].estimated_time_seconds < 4.0 * 60.0 * 60.0);
    CHECK(result.recommendations[2].estimated_time_seconds < 8.0 * 60.0 * 60.0);
    CHECK(result.recommendations[0].estimated_material_grams > 0.0);
    CHECK(result.recommendations[2].estimated_material_grams >= result.recommendations[1].estimated_material_grams);
}


TEST_CASE("smart slice separates machine speed, wall cap, flow and identity metadata",
          "[SmartSlice][SmartSlicingEngine][FixedFFF270]")
{
    ModelMetrics metrics;
    metrics.bounding_box = {0.0, 0.0, 0.0, 40.0, 40.0, 40.0};
    metrics.volume_mm3 = 50'000.0;
    metrics.surface_area_mm2 = 8'000.0;
    metrics.closed_mesh = true;

    MachinePreset machine;
    machine.nozzle_diameter_mm = 0.4;
    machine.machine_max_speed_mm_s = 1000.0;
    machine.outer_wall_speed_cap_mm_s = 500.0;
    machine.maximum_volumetric_speed_mm3_s = 100.0;
    machine.default_acceleration_mm_s2 = 20'000.0;
    machine.outer_wall_acceleration_mm_s2 = 25'000.0;
    machine.travel_acceleration_mm_s2 = 20'000.0;
    machine.profile_hash = "profile-fixed-fff-270";
    machine.capability_hash = "capability-mock";
    machine.effective_config_hash = "config-v1";
    FilamentPreset filament;
    filament.id = "PLA";
    filament.maximum_volumetric_speed_mm3_s = 12.0;

    const SmartAnalysisResult result = SmartSlicingEngine().AnalyzeAndPrecompute(metrics, machine, filament);
    REQUIRE(result.completed);
    CHECK(result.profile_hash == machine.profile_hash);
    CHECK(result.capability_hash == machine.capability_hash);
    CHECK(result.effective_config_hash == machine.effective_config_hash);
    CHECK(result.recommendations[1].outer_wall_speed_mm_s <= 500.0);
    CHECK(result.recommendations[1].outer_wall_speed_mm_s < 1000.0);
}

TEST_CASE("smart slice aggregates all plate meshes while preserving the single-mesh result",
          "[SmartSlice][SmartSlicingEngine]")
{
    TriangleMeshSnapshot first;
    first.vertices = {{0.0, 0.0, 0.0}, {10.0, 0.0, 0.0}, {0.0, 10.0, 0.0}, {0.0, 0.0, 10.0}};
    first.triangles = {{{0, 2, 1}}, {{0, 1, 3}}, {{1, 2, 3}}, {{2, 0, 3}}};
    TriangleMeshSnapshot second = first;
    for (auto& vertex : second.vertices) vertex.x += 30.0;

    const SmartSlicingEngine engine;
    const auto original = engine.AnalyzeAndPrecompute(first, MachinePreset{}, FilamentPreset{});
    const auto single = engine.AnalyzeAndPrecompute(
        std::vector<TriangleMeshSnapshot>{first}, MachinePreset{}, FilamentPreset{});
    REQUIRE(original.completed);
    REQUIRE(single.completed);
    CHECK(single.metrics.volume_mm3 == original.metrics.volume_mm3);
    CHECK(single.metrics.surface_area_mm2 == original.metrics.surface_area_mm2);
    CHECK(single.recommendations[0].layer_height_mm == original.recommendations[0].layer_height_mm);

    const auto combined = engine.AnalyzeAndPrecompute(
        std::vector<TriangleMeshSnapshot>{first, second}, MachinePreset{}, FilamentPreset{});
    REQUIRE(combined.completed);
    CHECK(combined.metrics.triangle_count == first.triangles.size() + second.triangles.size());
    CHECK_THAT(combined.metrics.volume_mm3,
               Catch::Matchers::WithinRel(original.metrics.volume_mm3 * 2.0, 1e-12));
    CHECK(combined.metrics.surface_area_mm2 == original.metrics.surface_area_mm2 * 2.0);
    CHECK(combined.metrics.bounding_box.min_x_mm == 0.0);
    CHECK(combined.metrics.bounding_box.max_x_mm == 40.0);
    CHECK(combined.metrics.closed_mesh);
}
