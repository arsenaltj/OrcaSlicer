#include <catch2/catch_all.hpp>

#include "slic3r/AI/SmartSlicing/Application/LocalModelFeatureAnalyzer.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>

using namespace Slic3r::AI::SmartSlicing;
using Catch::Matchers::WithinAbs;

namespace {

void append_box(TriangleMeshSnapshot& mesh, const MeshPoint3d& minimum, const MeshPoint3d& maximum)
{
    const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
    mesh.vertices.insert(mesh.vertices.end(), {
        {minimum.x, minimum.y, minimum.z},
        {maximum.x, minimum.y, minimum.z},
        {maximum.x, maximum.y, minimum.z},
        {minimum.x, maximum.y, minimum.z},
        {minimum.x, minimum.y, maximum.z},
        {maximum.x, minimum.y, maximum.z},
        {maximum.x, maximum.y, maximum.z},
        {minimum.x, maximum.y, maximum.z},
    });
    const auto triangle = [base](uint32_t a, uint32_t b, uint32_t c) {
        return MeshTriangle{{base + a, base + b, base + c}};
    };
    mesh.triangles.insert(mesh.triangles.end(), {
        triangle(0, 2, 1), triangle(0, 3, 2),
        triangle(4, 5, 6), triangle(4, 6, 7),
        triangle(0, 1, 5), triangle(0, 5, 4),
        triangle(1, 2, 6), triangle(1, 6, 5),
        triangle(2, 3, 7), triangle(2, 7, 6),
        triangle(3, 0, 4), triangle(3, 4, 7),
    });
}

TriangleMeshSnapshot cube(double size = 10.0)
{
    TriangleMeshSnapshot mesh;
    mesh.object_id = 11;
    mesh.volume_id = 17;
    mesh.geometry_fingerprint = "synthetic-cube-v1";
    append_box(mesh, {0.0, 0.0, 0.0}, {size, size, size});
    return mesh;
}

void append_rotated_cube(TriangleMeshSnapshot& mesh, double center_x, double angle_degrees)
{
    const size_t vertex_start = mesh.vertices.size();
    append_box(mesh, {center_x - 1.0, -1.0, -1.0}, {center_x + 1.0, 1.0, 1.0});
    const double radians = angle_degrees * 3.14159265358979323846 / 180.0;
    const double cosine = std::cos(radians);
    const double sine = std::sin(radians);
    double minimum_z = std::numeric_limits<double>::max();
    for (size_t index = vertex_start; index < mesh.vertices.size(); ++index) {
        MeshPoint3d& vertex = mesh.vertices[index];
        const double y = vertex.y;
        const double z = vertex.z;
        vertex.y = y * cosine - z * sine;
        vertex.z = y * sine + z * cosine;
        minimum_z = std::min(minimum_z, vertex.z);
    }
    for (size_t index = vertex_start; index < mesh.vertices.size(); ++index)
        mesh.vertices[index].z -= minimum_z;
}

ModelFeatureSnapshot completed_snapshot(ModelFeatureAnalysisResult result)
{
    REQUIRE(result.status == ModelFeatureAnalysisStatus::Completed);
    REQUIRE(result.snapshot);
    return std::move(*result.snapshot);
}

} // namespace

TEST_CASE("closed cube features preserve exact global geometry", "[AI][SmartSlicing][ModelFeatures]")
{
    const TriangleMeshSnapshot mesh = cube();
    const ModelFeatureAnalysisResult result = LocalModelFeatureAnalyzer().analyze(mesh, {});
    const ModelFeatureSnapshot features = completed_snapshot(result);

    CHECK(features.schema_version == MODEL_FEATURE_SNAPSHOT_VERSION);
    CHECK(features.analysis_version == LocalModelFeatureAnalyzer::ANALYSIS_VERSION);
    CHECK(features.policy_version == "model-feature-policy/v1");
    REQUIRE(features.bounding_box.known());
    CHECK_THAT(features.bounding_box.value->minimum.x, WithinAbs(0.0, 1e-12));
    CHECK_THAT(features.bounding_box.value->maximum.z, WithinAbs(10.0, 1e-12));
    REQUIRE(features.volume_mm3.known());
    CHECK_THAT(*features.volume_mm3.value, WithinAbs(1000.0, 1e-9));
    REQUIRE(features.surface_area_mm2.known());
    CHECK_THAT(*features.surface_area_mm2.value, WithinAbs(600.0, 1e-9));
    REQUIRE(features.center_of_mass.known());
    CHECK_THAT(features.center_of_mass.value->x, WithinAbs(5.0, 1e-9));
    CHECK_THAT(features.center_of_mass.value->y, WithinAbs(5.0, 1e-9));
    CHECK_THAT(features.center_of_mass.value->z, WithinAbs(5.0, 1e-9));
    REQUIRE(features.closedness.known());
    CHECK(*features.closedness.value == MeshClosedness::Closed);
    REQUIRE(features.winding_consistent.known());
    CHECK(*features.winding_consistent.value);
    REQUIRE(features.degenerate_faces.known());
    CHECK(features.degenerate_faces.value->face_count == 0);
    REQUIRE(features.bed_contact_candidate.known());
    CHECK_THAT(features.bed_contact_candidate.value->projected_area_mm2, WithinAbs(100.0, 1e-9));
    CHECK(features.bed_contact_candidate.value->facet_count == 2);
    REQUIRE(features.height_to_footprint_ratio.known());
    CHECK_THAT(*features.height_to_footprint_ratio.value, WithinAbs(1.0, 1e-12));
}

TEST_CASE("open and degenerate meshes keep unreliable solid properties unknown",
          "[AI][SmartSlicing][ModelFeatures]")
{
    LocalModelFeatureAnalyzer analyzer;

    SECTION("open mesh") {
        TriangleMeshSnapshot mesh = cube();
        mesh.triangles.erase(mesh.triangles.begin() + 2, mesh.triangles.begin() + 4);
        const ModelFeatureSnapshot features = completed_snapshot(analyzer.analyze(mesh, {}));

        REQUIRE(features.closedness.known());
        CHECK(*features.closedness.value == MeshClosedness::Open);
        CHECK(features.volume_mm3.availability == FeatureAvailability::Unknown);
        CHECK_FALSE(features.volume_mm3.value);
        CHECK(features.center_of_mass.availability == FeatureAvailability::Unknown);
        CHECK(features.bed_contact_candidate.availability == FeatureAvailability::Unknown);
        CHECK(features.overhangs.availability == FeatureAvailability::Unknown);
        CHECK(features.bridge_candidates.availability == FeatureAvailability::Unknown);
    }

    SECTION("zero area face") {
        TriangleMeshSnapshot mesh = cube();
        mesh.triangles.push_back({{0, 0, 1}});
        const ModelFeatureSnapshot features = completed_snapshot(analyzer.analyze(mesh, {}));

        REQUIRE(features.closedness.known());
        CHECK(*features.closedness.value == MeshClosedness::Closed);
        REQUIRE(features.degenerate_faces.known());
        CHECK(features.degenerate_faces.value->face_count == 1);
        REQUIRE(features.volume_mm3.known());
        CHECK_THAT(*features.volume_mm3.value, WithinAbs(1000.0, 1e-9));
    }

    const ModelFeatureSnapshot cube_features = completed_snapshot(analyzer.analyze(cube(), {}));
    CHECK(cube_features.small_hole_candidates.availability == FeatureAvailability::Unavailable);
    CHECK(cube_features.small_text_candidates.availability == FeatureAvailability::Unavailable);
    CHECK(cube_features.curvature_distribution.availability == FeatureAvailability::Unavailable);
    CHECK(cube_features.orientation_sensitive_surfaces.availability == FeatureAvailability::Unknown);
    CHECK(cube_features.visibility_approximation.availability == FeatureAvailability::Unavailable);
    CHECK(cube_features.bed_stability.availability == FeatureAvailability::Unknown);
    CHECK(cube_features.support_contact_risk.availability == FeatureAvailability::Unknown);
    CHECK(cube_features.seam_visibility_risk.availability == FeatureAvailability::Unknown);
    CHECK(cube_features.material_partition_count.availability == FeatureAvailability::Unavailable);
    CHECK(cube_features.color_partition_count.availability == FeatureAvailability::Unavailable);
    CHECK(cube_features.layer_tool_sequence_complexity.availability == FeatureAvailability::Unavailable);
    CHECK(cube_features.tool_change_density_per_layer.availability == FeatureAvailability::Unavailable);
}

TEST_CASE("overhang bands are deterministic for fixed face inclinations", "[AI][SmartSlicing][ModelFeatures]")
{
    TriangleMeshSnapshot mesh;
    mesh.object_id = 21;
    mesh.volume_id = 22;
    mesh.geometry_fingerprint = "rotated-cubes-v1";
    append_rotated_cube(mesh, 0.0, 15.0);
    append_rotated_cube(mesh, 5.0, 45.0);
    append_rotated_cube(mesh, 10.0, 70.0);

    const ModelFeatureSnapshot features = completed_snapshot(LocalModelFeatureAnalyzer().analyze(mesh, {}));
    REQUIRE(features.overhangs.known());
    const OverhangSummary& overhangs = *features.overhangs.value;
    CHECK(overhangs.bands[0].severity == OverhangSeverity::Severe);
    CHECK(overhangs.bands[1].severity == OverhangSeverity::Moderate);
    CHECK(overhangs.bands[2].severity == OverhangSeverity::Mild);
    CHECK(overhangs.bands[0].facet_count > 0);
    CHECK(overhangs.bands[1].facet_count > 0);
    CHECK(overhangs.bands[2].facet_count > 0);
    CHECK(overhangs.bands[0].surface_area_mm2 > 0.0);
    CHECK(overhangs.bands[1].surface_area_mm2 > 0.0);
    CHECK(overhangs.bands[2].surface_area_mm2 > 0.0);
}

TEST_CASE("bridge and thin wall outputs remain conservative candidates", "[AI][SmartSlicing][ModelFeatures]")
{
    LocalModelFeatureAnalyzer analyzer;

    SECTION("raised slab underside") {
        TriangleMeshSnapshot mesh;
        mesh.object_id = 31;
        mesh.volume_id = 32;
        mesh.geometry_fingerprint = "bridge-candidate-v1";
        append_box(mesh, {0.0, 0.0, 0.0}, {2.0, 3.0, 5.0});
        append_box(mesh, {8.0, 0.0, 0.0}, {10.0, 3.0, 5.0});
        append_box(mesh, {0.0, 0.0, 5.0}, {10.0, 3.0, 6.0});
        const ModelFeatureSnapshot features = completed_snapshot(analyzer.analyze(mesh, {}));

        REQUIRE(features.bridge_candidates.known());
        REQUIRE_FALSE(features.bridge_candidates.value->empty());
        const BridgeCandidate& candidate = features.bridge_candidates.value->front();
        CHECK(candidate.span_mm >= 3.0);
        CHECK_THAT(candidate.direction_xy.z, WithinAbs(0.0, 1e-12));
        CHECK(candidate.confidence.lower == 0.0);
        CHECK(candidate.confidence.upper < 0.5);
        CHECK(candidate.centroid.z > 0.0);
    }

    SECTION("globally narrow box") {
        TriangleMeshSnapshot mesh;
        mesh.object_id = 41;
        mesh.volume_id = 42;
        mesh.geometry_fingerprint = "thin-candidate-v1";
        append_box(mesh, {0.0, 0.0, 0.0}, {10.0, 0.4, 5.0});
        const ModelFeatureSnapshot features = completed_snapshot(analyzer.analyze(mesh, {}));

        REQUIRE(features.thin_wall_candidates.known());
        REQUIRE(features.thin_wall_candidates.value->size() == 1);
        const ThinWallCandidate& candidate = features.thin_wall_candidates.value->front();
        CHECK(candidate.narrow_axis == PrincipalAxis::Y);
        CHECK_THAT(candidate.scale_upper_bound_mm, WithinAbs(0.4, 1e-12));
        CHECK(candidate.confidence.upper < 0.5);
    }

    const ModelFeatureSnapshot ordinary = completed_snapshot(analyzer.analyze(cube(), {}));
    CHECK(ordinary.thin_wall_candidates.availability == FeatureAvailability::Unknown);
    CHECK(ordinary.bridge_candidates.availability == FeatureAvailability::Unknown);
}

TEST_CASE("repeated analysis is stable and leaves the source mesh unchanged",
          "[AI][SmartSlicing][ModelFeatures]")
{
    TriangleMeshSnapshot mesh = cube();
    const TriangleMeshSnapshot original = mesh;
    LocalModelFeatureAnalyzer analyzer;
    const ModelFeatureSnapshot first = completed_snapshot(analyzer.analyze(mesh, {}));
    const ModelFeatureSnapshot second = completed_snapshot(analyzer.analyze(mesh, {}));

    REQUIRE(first.volume_mm3.known());
    REQUIRE(second.volume_mm3.known());
    CHECK(*first.volume_mm3.value == *second.volume_mm3.value);
    REQUIRE(first.overhangs.known());
    REQUIRE(second.overhangs.known());
    for (size_t index = 0; index < first.overhangs.value->bands.size(); ++index) {
        CHECK(first.overhangs.value->bands[index].facet_count ==
              second.overhangs.value->bands[index].facet_count);
        CHECK(first.overhangs.value->bands[index].surface_area_mm2 ==
              second.overhangs.value->bands[index].surface_area_mm2);
    }
    CHECK(mesh.vertices.size() == original.vertices.size());
    CHECK(mesh.triangles.size() == original.triangles.size());
    CHECK(mesh.geometry_fingerprint == original.geometry_fingerprint);
    CHECK(mesh.vertices.front().x == original.vertices.front().x);
    CHECK(mesh.vertices.back().z == original.vertices.back().z);
    CHECK(mesh.triangles.front().vertex_indices == original.triangles.front().vertex_indices);
    CHECK(mesh.triangles.back().vertex_indices == original.triangles.back().vertex_indices);
}

TEST_CASE("cancellation and memory limits return no partial feature snapshot",
          "[AI][SmartSlicing][ModelFeatures]")
{
    const TriangleMeshSnapshot mesh = cube();
    LocalModelFeatureAnalyzer analyzer;

    SECTION("already canceled") {
        ModelFeatureAnalysisLimits limits;
        limits.cancellation_requested = [] { return true; };
        const ModelFeatureAnalysisResult result = analyzer.analyze(mesh, limits);
        CHECK(result.status == ModelFeatureAnalysisStatus::Canceled);
        CHECK_FALSE(result.snapshot);
    }

    SECTION("canceled during traversal") {
        size_t cancellation_polls = 0;
        ModelFeatureAnalysisLimits limits;
        limits.cancellation_requested = [&] {
            ++cancellation_polls;
            return cancellation_polls > mesh.vertices.size() + 4;
        };
        const ModelFeatureAnalysisResult result = analyzer.analyze(mesh, limits);
        CHECK(result.status == ModelFeatureAnalysisStatus::Canceled);
        CHECK_FALSE(result.snapshot);
        CHECK(cancellation_polls > mesh.vertices.size());
    }

    SECTION("input exceeds budget before analysis") {
        ModelFeatureAnalysisLimits limits;
        limits.maximum_working_memory_bytes = triangle_mesh_snapshot_bytes(mesh) - 1;
        const ModelFeatureAnalysisResult result = analyzer.analyze(mesh, limits);
        CHECK(result.status == ModelFeatureAnalysisStatus::ResourceLimitExceeded);
        CHECK_FALSE(result.snapshot);
    }

    SECTION("budget is exhausted while working storage grows") {
        const ModelFeatureAnalysisResult reference = analyzer.analyze(mesh, {});
        REQUIRE(reference.completed());
        REQUIRE(reference.peak_accounted_memory_bytes > triangle_mesh_snapshot_bytes(mesh));
        ModelFeatureAnalysisLimits limits;
        limits.maximum_working_memory_bytes = reference.peak_accounted_memory_bytes - 1;
        const ModelFeatureAnalysisResult result = analyzer.analyze(mesh, limits);
        CHECK(result.status == ModelFeatureAnalysisStatus::ResourceLimitExceeded);
        CHECK_FALSE(result.snapshot);
        CHECK(result.peak_accounted_memory_bytes <= limits.maximum_working_memory_bytes);
    }
}
