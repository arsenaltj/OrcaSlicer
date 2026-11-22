#include "slic3r/AI/SmartSlicing/Application/SmartSlicingEngine.hpp"

#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
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

namespace {
TriangleMeshSnapshot recommendation_cube()
{
    TriangleMeshSnapshot cube;
    cube.vertices = {{0.,0.,0.},{10.,0.,0.},{10.,10.,0.},{0.,10.,0.},
                     {0.,0.,10.},{10.,0.,10.},{10.,10.,10.},{0.,10.,10.}};
    cube.triangles = {{{0,2,1}},{{0,3,2}},{{4,5,6}},{{4,6,7}},
                      {{0,1,5}},{{0,5,4}},{{1,2,6}},{{1,6,5}},
                      {{2,3,7}},{{2,7,6}},{{3,0,4}},{{3,4,7}}};
    return cube;
}
}

TEST_CASE("horizontal undersides above the bed require support in all three plans",
          "[SmartSlice][SmartSlicingEngine][RecommendationParity]")
{
    auto cube = recommendation_cube();
    const SmartSlicingEngine engine;
    const auto contact = engine.AnalyzeAndPrecompute(cube, {}, {});
    REQUIRE(contact.completed);
    CHECK_THAT(contact.metrics.severe_overhang_area_ratio_60_deg, Catch::Matchers::WithinAbs(0., 1e-12));
    for (const auto& plan : contact.recommendations) CHECK_FALSE(plan.enable_support);
    for (auto& v : cube.vertices) v.z += 4.;
    const auto floating = engine.AnalyzeAndPrecompute(cube, {}, {});
    REQUIRE(floating.completed);
    CHECK_THAT(floating.metrics.severe_overhang_area_ratio_60_deg, Catch::Matchers::WithinAbs(1./6., 1e-12));
    for (const auto& plan : floating.recommendations) CHECK(plan.enable_support);
    // A reflected transform reverses winding but must preserve physical risk.
    for (auto& v : cube.vertices) v.x = -v.x;
    const auto mirrored = engine.AnalyzeAndPrecompute(cube, {}, {});
    REQUIRE(mirrored.completed);
    CHECK_THAT(mirrored.metrics.severe_overhang_area_ratio_60_deg,
               Catch::Matchers::WithinAbs(floating.metrics.severe_overhang_area_ratio_60_deg, 1e-12));
}

TEST_CASE("triangle subdivision does not turn a solid cube into a thin wall",
          "[SmartSlice][SmartSlicingEngine][RecommendationParity]")
{
    auto dense = recommendation_cube();
    const SmartSlicingEngine engine;
    const auto contact = engine.AnalyzeAndPrecompute(dense, {}, {});
    for (int round = 0; round < 5; ++round) {
        std::map<std::pair<uint32_t, uint32_t>, uint32_t> midpoints;
        const auto midpoint = [&](uint32_t a, uint32_t b) {
            if (a > b) std::swap(a, b);
            const auto [it, created] = midpoints.emplace(std::make_pair(a, b), uint32_t(dense.vertices.size()));
            if (created) {
                const auto p = dense.vertices[a], q = dense.vertices[b];
                dense.vertices.push_back({(p.x+q.x)*.5, (p.y+q.y)*.5, (p.z+q.z)*.5});
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
    REQUIRE(detailed.completed);
    CHECK(detailed.metrics.closed_mesh);
    CHECK_THAT(detailed.metrics.thin_wall_feature_ratio,
               Catch::Matchers::WithinAbs(contact.metrics.thin_wall_feature_ratio, 1e-12));
    auto thin = recommendation_cube();
    for (auto& v : thin.vertices) v.z *= .05;
    const auto plate = engine.AnalyzeAndPrecompute(thin, {}, {});
    REQUIRE(plate.completed);
    CHECK_THAT(plate.metrics.thin_wall_feature_ratio, Catch::Matchers::WithinAbs(1., 1e-12));
}

TEST_CASE("recommendations reject empty layer ranges and preserve legal boundaries",
          "[SmartSlice][SmartSlicingEngine][RecommendationParity]")
{
    const SmartSlicingEngine engine;
    MachinePreset machine;
    machine.minimum_layer_height_mm = .18;
    machine.maximum_layer_height_mm = .21;
    const auto legal = engine.AnalyzeAndPrecompute(recommendation_cube(), machine, {});
    REQUIRE(legal.completed);
    for (const auto& plan : legal.recommendations) {
        CHECK(plan.layer_height_mm >= .18);
        CHECK(plan.layer_height_mm <= .21);
    }
    machine.minimum_layer_height_mm = .23;
    const auto empty = engine.AnalyzeAndPrecompute(recommendation_cube(), machine, {});
    CHECK_FALSE(empty.completed);
    CHECK(empty.diagnostic_code == "smart_slice_layer_height_no_common_range");
    machine.minimum_layer_height_mm = .21;
    const auto boundary = engine.AnalyzeAndPrecompute(recommendation_cube(), machine, {});
    REQUIRE(boundary.completed);
    for (const auto& plan : boundary.recommendations)
        CHECK_THAT(plan.layer_height_mm, Catch::Matchers::WithinAbs(.21, 1e-12));
    machine.minimum_layer_height_mm = .31;
    machine.maximum_layer_height_mm = .35;
    const auto nozzle_limited = engine.AnalyzeAndPrecompute(recommendation_cube(), machine, {});
    CHECK_FALSE(nozzle_limited.completed);
    CHECK(nozzle_limited.diagnostic_code == "smart_slice_layer_height_no_common_range");
}

TEST_CASE("separately captured reflected bodies preserve aggregate physical risk",
          "[SmartSlice][SmartSlicingEngine][RecommendationParity]")
{
    const SmartSlicingEngine engine;
    for (double first_z : {0.,4.}) for (double second_z : {0.,4.}) {
        auto a=recommendation_cube(), b=recommendation_cube();
        for(auto& v:a.vertices) v.z+=first_z;
        for(auto& v:b.vertices) {v.x=30.-v.x;v.z+=second_z;}
        const auto single_a=engine.AnalyzeAndPrecompute(a,{},{});
        const auto single_b=engine.AnalyzeAndPrecompute(b,{},{});
        const auto together=engine.AnalyzeAndPrecompute(std::vector<TriangleMeshSnapshot>{a,b},{},{});
        REQUIRE(together.completed);
        CHECK(together.metrics.closed_mesh);
        CHECK_THAT(together.metrics.volume_mm3,Catch::Matchers::WithinAbs(2000.,1e-9));
        CHECK_THAT(together.metrics.severe_overhang_area_ratio_60_deg,Catch::Matchers::WithinAbs(
            .5*(single_a.metrics.severe_overhang_area_ratio_60_deg+single_b.metrics.severe_overhang_area_ratio_60_deg),1e-12));
        for(const auto& plan:together.recommendations) CHECK(plan.enable_support==(first_z>0. || second_z>0.));
    }
}
