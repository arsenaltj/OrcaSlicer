#include "slic3r/AI/SmartSlicing/Application/SmartSlicingEngine.hpp"

#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/catch_test_macros.hpp>

#include <map>

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

TEST_CASE("SmartSlice derives layer choices from actual slopes and nozzle limits", "[SmartSlice][SmartSliceModelGeometry]")
{
    ModelMetrics flat;
    flat.bounding_box = {0., 0., 0., 35., 35., 30.};
    flat.volume_mm3 = 1200.;
    flat.surface_area_mm2 = 1000.;
    flat.closed_mesh = true;
    ModelMetrics curved = flat;
    curved.slope_detail_area_ratio = 0.42;
    const SmartSlicingEngine engine;
    const auto planar = engine.AnalyzeAndPrecompute(flat, {}, {});
    const auto detailed = engine.AnalyzeAndPrecompute(curved, {}, {});
    REQUIRE(planar.completed);
    REQUIRE(detailed.completed);
    CHECK(planar.recommendations[0].layer_height_mm > detailed.recommendations[0].layer_height_mm);
    CHECK(planar.recommendations[2].layer_height_mm > detailed.recommendations[2].layer_height_mm);
    CHECK_FALSE(planar.recommendations[2].adaptive_layer_height);
    CHECK(detailed.recommendations[2].adaptive_layer_height);

    MachinePreset preset;
    preset.minimum_layer_height_mm = 0.18;
    preset.maximum_layer_height_mm = 0.21;
    const auto restricted = engine.AnalyzeAndPrecompute(curved, preset, {});
    REQUIRE(restricted.completed);
    for (const auto& plan : restricted.recommendations) {
        CHECK(plan.layer_height_mm >= 0.18);
        CHECK(plan.layer_height_mm <= 0.21);
        CHECK_FALSE(plan.adaptive_layer_height);
    }
    preset.minimum_layer_height_mm = 0.23;
    const auto blocked = engine.AnalyzeAndPrecompute(curved, preset, {});
    CHECK_FALSE(blocked.completed);
    CHECK(blocked.diagnostic_code == "smart_slice_layer_height_no_common_range");
}

TEST_CASE("SmartSlice identifies horizontal undersides away from the bed", "[SmartSlice][SmartSliceModelGeometry]")
{
    TriangleMeshSnapshot cube;
    cube.vertices = {{0.,0.,0.},{10.,0.,0.},{10.,10.,0.},{0.,10.,0.},
                     {0.,0.,10.},{10.,0.,10.},{10.,10.,10.},{0.,10.,10.}};
    cube.triangles = {{{0,2,1}},{{0,3,2}},{{4,5,6}},{{4,6,7}},
                      {{0,1,5}},{{0,5,4}},{{1,2,6}},{{1,6,5}},
                      {{2,3,7}},{{2,7,6}},{{3,0,4}},{{3,4,7}}};
    const SmartSlicingEngine engine;
    const auto contact = engine.AnalyzeAndPrecompute(cube, {}, {});
    auto thin = cube;
    for (auto& v : thin.vertices) v.z *= 0.05;
    const auto thin_span = engine.AnalyzeAndPrecompute(thin, {}, {});
    CHECK(thin_span.metrics.thin_wall_feature_ratio == 1.);
    auto dense = cube;
    for (int round = 0; round < 5; ++round) {
        std::map<std::pair<uint32_t, uint32_t>, uint32_t> midpoints;
        const auto midpoint = [&](uint32_t a, uint32_t b) {
            if (a > b) std::swap(a, b);
            const auto [it, created] = midpoints.emplace(std::make_pair(a, b), uint32_t(dense.vertices.size()));
            if (created) {
                const auto p = dense.vertices[a], q = dense.vertices[b];
                dense.vertices.push_back({(p.x + q.x) * 0.5, (p.y + q.y) * 0.5, (p.z + q.z) * 0.5});
            }
            return it->second;
        };
        auto old = std::move(dense.triangles);
        dense.triangles.clear();
        for (const auto& face : old) {
            const auto [a,b,c] = face.vertex_indices;
            const auto ab = midpoint(a,b), bc = midpoint(b,c), ca = midpoint(c,a);
            dense.triangles.insert(dense.triangles.end(), {{{a,ab,ca}},{{b,bc,ab}},{{c,ca,bc}},{{ab,bc,ca}}});
        }
    }
    const auto detailed = engine.AnalyzeAndPrecompute(dense, {}, {});
    CHECK(detailed.metrics.closed_mesh);
    CHECK(detailed.metrics.thin_wall_feature_ratio == contact.metrics.thin_wall_feature_ratio);
    for (auto& v : cube.vertices) v.z += 4.;
    const auto floating = engine.AnalyzeAndPrecompute(cube, {}, {});
    REQUIRE(contact.completed);
    REQUIRE(floating.completed);
    CHECK(contact.metrics.severe_overhang_area_ratio_60_deg == 0.);
    CHECK(floating.metrics.severe_overhang_area_ratio_60_deg > 0.15);
    CHECK_FALSE(contact.recommendations[0].enable_support);
    CHECK(floating.recommendations[0].support_type == "normal(auto)");
    for (auto& v : cube.vertices) v.x = -v.x;
    const auto mirrored = engine.AnalyzeAndPrecompute(cube, {}, {});
    CHECK(mirrored.metrics.severe_overhang_area_ratio_60_deg == floating.metrics.severe_overhang_area_ratio_60_deg);
}

TEST_CASE("SmartSlice ranks native time, geometry risk and total material together", "[SmartSlice][SmartSliceMeasuredChoice]")
{
    std::vector<MeasuredSliceOption> options{{100., 10., 0.065, 1., true},
        {76., 9., 0.08, 0.9}, {116., 9.2, 0.045, 1.1}, {59., 8., 0.15, 0.6}};
    CHECK(select_measured_slice_option(SliceMode::SPEED, options) == 1);
    CHECK(select_measured_slice_option(SliceMode::QUALITY, options) == 2);
    CHECK(select_measured_slice_option(SliceMode::BALANCED, options) == 2);
    const auto baseline = options.front();
    options = {baseline, baseline};
    options[1].baseline = false;
    CHECK(select_measured_slice_option(SliceMode::SPEED, options) == 0);
    options = {{0., 0., 0.064, 0., true}, {116., 9.2, 0.036, 1.1}, {76., 9.0, 0.071, 0.9}};
    CHECK(select_measured_slice_option(SliceMode::SPEED, options) == 2);
    CHECK(select_measured_slice_option(SliceMode::QUALITY, options) == 1);
    options = {{}, {}};
    CHECK_FALSE(select_measured_slice_option(SliceMode::QUALITY, options));
}

TEST_CASE("local rules compute only the selected goal with the legacy rules unchanged", "[SmartSlice][SmartSlicingEngine]")
{
    ModelMetrics metrics;
    metrics.bounding_box = {0., 0., 0., 40., 35., 60.};
    metrics.volume_mm3 = 42'000.;
    metrics.surface_area_mm2 = 8'000.;
    metrics.slope_detail_area_ratio = 0.21;
    metrics.high_overhang_area_ratio_45_deg = 0.17;
    metrics.closed_mesh = true;
    const MachinePreset machine;
    const FilamentPreset filament;
    const SmartSlicingEngine engine;
    const auto all = engine.AnalyzeAndPrecompute(metrics, machine, filament);
    REQUIRE(all.completed);
    const std::array modes{SliceMode::BALANCED, SliceMode::SPEED, SliceMode::QUALITY};
    for (auto mode : modes) {
        CAPTURE(slice_mode_id(mode));
        const auto single = engine.AnalyzeGoal(metrics, machine, filament, mode);
        REQUIRE(single.completed);
        const auto* plan = single.find(mode);
        REQUIRE(plan);
        REQUIRE(all.find(mode));
        CHECK(plan->layer_height_mm == all.find(mode)->layer_height_mm);
        CHECK(plan->estimated_time_seconds == all.find(mode)->estimated_time_seconds);
        CHECK(plan->enable_support == all.find(mode)->enable_support);
        CHECK(plan->adaptive_layer_height == all.find(mode)->adaptive_layer_height);
        for (auto other : modes) if (other != mode) CHECK(single.find(other) == nullptr);
    }
}
