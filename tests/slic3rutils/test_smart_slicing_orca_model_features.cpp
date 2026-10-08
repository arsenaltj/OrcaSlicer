#include <catch2/catch_all.hpp>

#include "libslic3r/TriangleMesh.hpp"
#include "slic3r/GUI/AI/Orca/OrcaModelFeatureAnalyzer.hpp"

#include <limits>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::AI::SmartSlicing;
using namespace Slic3r::GUI;
using Catch::Matchers::WithinAbs;

namespace {

OrcaModelFeatureCaptureSource source(const indexed_triangle_set& mesh, uint64_t object_id,
                                     uint64_t volume_id, uint64_t instance_id,
                                     const Transform3d& transform = Transform3d::Identity())
{
    return {object_id, volume_id, instance_id,
            "geometry-" + std::to_string(object_id) + "-" + std::to_string(volume_id),
            &mesh, transform};
}

ModelFeatureAnalysisLimits generous_limits()
{
    ModelFeatureAnalysisLimits limits;
    limits.maximum_working_memory_bytes = 64ull * 1024ull * 1024ull;
    return limits;
}

} // namespace

TEST_CASE("Orca capture deep copies transformed model parts per instance",
          "[AI][SmartSlicing][OrcaModelFeatures]")
{
    indexed_triangle_set cube = its_make_cube(2.0, 3.0, 4.0);
    const auto original_vertices = cube.vertices;
    const auto original_indices = cube.indices;

    Transform3d first = Transform3d::Identity();
    first.translate(Vec3d(10.0, 20.0, 30.0));
    first.scale(Vec3d(2.0, 1.0, 1.0));
    const auto original_first_transform = first.matrix();
    Transform3d second = Transform3d::Identity();
    second.translate(Vec3d(-5.0, 0.0, 1.0));

    const std::vector<OrcaModelFeatureCaptureSource> sources{
        source(cube, 11, 21, 31, first),
        source(cube, 11, 21, 32, second),
        source(cube, 12, 22, 33),
    };
    const auto captured = OrcaModelFeatureAnalyzer::capture_sources(
        sources, OrcaModelFeatureTarget{11, 21, std::nullopt}, generous_limits());

    REQUIRE(captured.completed());
    REQUIRE(captured.inputs.size() == 2);
    CHECK(captured.inputs[0].instance_id == 31);
    CHECK(captured.inputs[1].instance_id == 32);
    CHECK(captured.inputs[0].mesh.object_id == 11);
    CHECK(captured.inputs[0].mesh.volume_id == 21);
    CHECK(captured.inputs[0].mesh.geometry_fingerprint == "geometry-11-21");
    CHECK(captured.accounted_memory_bytes ==
          triangle_mesh_snapshot_bytes(captured.inputs[0].mesh) +
              triangle_mesh_snapshot_bytes(captured.inputs[1].mesh));
    REQUIRE(captured.inputs[0].mesh.vertices.size() == cube.vertices.size());
    CHECK_THAT(captured.inputs[0].mesh.vertices.front().x, WithinAbs(14.0, 1e-12));
    CHECK_THAT(captured.inputs[0].mesh.vertices.front().y, WithinAbs(23.0, 1e-12));
    CHECK_THAT(captured.inputs[0].mesh.vertices.front().z, WithinAbs(30.0, 1e-12));

    OrcaModelFeatureAnalyzer analyzer;
    const auto analyzed = analyzer.analyze_captured(captured.inputs, generous_limits());
    REQUIRE(analyzed.completed());
    REQUIRE(analyzed.outputs.size() == 2);
    CHECK(analyzed.outputs[0].instance_id == 31);
    REQUIRE(analyzed.outputs[0].features.bounding_box.known());
    const auto& bounds = *analyzed.outputs[0].features.bounding_box.value;
    CHECK_THAT(bounds.minimum.x, WithinAbs(10.0, 1e-12));
    CHECK_THAT(bounds.maximum.x, WithinAbs(14.0, 1e-12));
    CHECK_THAT(bounds.minimum.y, WithinAbs(20.0, 1e-12));
    CHECK_THAT(bounds.maximum.y, WithinAbs(23.0, 1e-12));
    CHECK_THAT(bounds.minimum.z, WithinAbs(30.0, 1e-12));
    CHECK_THAT(bounds.maximum.z, WithinAbs(34.0, 1e-12));
    REQUIRE(analyzed.outputs[0].features.volume_mm3.known());
    CHECK_THAT(*analyzed.outputs[0].features.volume_mm3.value, WithinAbs(48.0, 1e-8));

    REQUIRE(cube.vertices.size() == original_vertices.size());
    for (size_t index = 0; index < cube.vertices.size(); ++index)
        CHECK(cube.vertices[index].isApprox(original_vertices[index]));
    REQUIRE(cube.indices.size() == original_indices.size());
    for (size_t index = 0; index < cube.indices.size(); ++index)
        CHECK(cube.indices[index] == original_indices[index]);
    CHECK(first.matrix().isApprox(original_first_transform));
}

TEST_CASE("Orca capture filters targets and fails atomically on cancellation or request memory",
          "[AI][SmartSlicing][OrcaModelFeatures]")
{
    const indexed_triangle_set cube = its_make_cube(2.0, 3.0, 4.0);
    const std::vector<OrcaModelFeatureCaptureSource> sources{
        source(cube, 11, 21, 31), source(cube, 12, 22, 32)};

    auto selected = OrcaModelFeatureAnalyzer::capture_sources(
        sources, OrcaModelFeatureTarget{12, 22, 32}, generous_limits());
    REQUIRE(selected.completed());
    REQUIRE(selected.inputs.size() == 1);
    CHECK(selected.inputs.front().instance_id == 32);

    const auto missing = OrcaModelFeatureAnalyzer::capture_sources(
        sources, OrcaModelFeatureTarget{99, std::nullopt, std::nullopt}, generous_limits());
    CHECK(missing.status == OrcaModelFeatureCaptureStatus::NoMatchingGeometry);
    CHECK(missing.inputs.empty());

    auto limited = generous_limits();
    limited.maximum_working_memory_bytes =
        triangle_mesh_snapshot_bytes(selected.inputs.front().mesh);
    const auto over_budget = OrcaModelFeatureAnalyzer::capture_sources(
        sources, {}, limited);
    CHECK(over_budget.status == OrcaModelFeatureCaptureStatus::ResourceLimitExceeded);
    CHECK(over_budget.inputs.empty());

    size_t cancellation_checks = 0;
    auto canceled = generous_limits();
    canceled.cancellation_requested = [&] { return ++cancellation_checks > 4; };
    const auto canceled_result = OrcaModelFeatureAnalyzer::capture_sources(
        sources, {}, canceled);
    CHECK(canceled_result.status == OrcaModelFeatureCaptureStatus::Canceled);
    CHECK(canceled_result.inputs.empty());
}

TEST_CASE("Orca batch analysis enforces one request budget without partial results",
          "[AI][SmartSlicing][OrcaModelFeatures]")
{
    const indexed_triangle_set cube = its_make_cube(10.0, 10.0, 10.0);
    const auto captured = OrcaModelFeatureAnalyzer::capture_sources(
        {source(cube, 11, 21, 31), source(cube, 11, 21, 32)}, {}, generous_limits());
    REQUIRE(captured.completed());

    OrcaModelFeatureAnalyzer analyzer;
    const auto one = analyzer.analyze_captured({captured.inputs.front()}, generous_limits());
    REQUIRE(one.completed());
    REQUIRE(one.peak_accounted_memory_bytes > 0);

    auto limited = generous_limits();
    limited.maximum_working_memory_bytes = one.peak_accounted_memory_bytes;
    const auto over_budget = analyzer.analyze_captured(captured.inputs, limited);
    CHECK(over_budget.status == ModelFeatureAnalysisStatus::ResourceLimitExceeded);
    CHECK(over_budget.outputs.empty());

    auto canceled = generous_limits();
    canceled.cancellation_requested = [] { return true; };
    const auto canceled_result = analyzer.analyze_captured(captured.inputs, canceled);
    CHECK(canceled_result.status == ModelFeatureAnalysisStatus::Canceled);
    CHECK(canceled_result.outputs.empty());
}
