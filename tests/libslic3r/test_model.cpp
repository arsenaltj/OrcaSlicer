#include "libslic3r/libslic3r.h"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/ObjectID.hpp"
#include <catch2/catch_all.hpp>
#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "libslic3r/MeshRepair.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/AABBMesh.hpp"
#include "libslic3r/Format/AssimpImport.hpp"
#include "libslic3r/TexturePainting.hpp"

#include <algorithm>
#include <cmath>

using namespace Slic3r;

TEST_CASE("Unpainted volume skips facet extraction and invalidates painted extruder cache", "[Model][ExtruderCount]")
{
    Model model;
    ModelVolume* volume = model.add_object()->add_volume(make_cube(20, 20, 20), ModelVolumeType::MODEL_PART, false);
    REQUIRE(volume->mmu_segmentation_facets.empty());
    CHECK(volume->get_extruders() == std::vector<int>{1});
    volume->update_extruder_count(1);
    CHECK(volume->get_extruders() == std::vector<int>{1});

    TriangleSelector selector(volume->mesh());
    selector.set_facet(0, EnforcerBlockerType::Extruder2);
    REQUIRE(volume->mmu_segmentation_facets.set(selector));
    CHECK(volume->get_extruders() == std::vector<int>({2, 1}));
    CHECK(volume->get_extruders() == std::vector<int>({2, 1}));

    volume->mmu_segmentation_facets.reset();
    REQUIRE(volume->mmu_segmentation_facets.empty());
    CHECK(volume->get_extruders() == std::vector<int>{1});
}

TEST_CASE("AABB mesh copies retain vertex and face topology", "[Model][AABBMesh]")
{
    const TriangleMesh mesh = make_cube(20, 20, 20);
    AABBMesh original(mesh, true);
    AABBMesh copied(original);
    AABBMesh assigned(mesh);
    assigned = original;

    const VertexFaceIndex expected_vertices(mesh.its);
    const auto expected_neighbors = its_face_neighbors(mesh.its);
    for (const AABBMesh* candidate : {&original, &copied, &assigned}) {
        CHECK(candidate->indices().size() == mesh.its.indices.size());
        for (size_t vertex = 0; vertex < mesh.its.vertices.size(); ++vertex)
            CHECK(candidate->vertex_face_index().count(vertex) == expected_vertices.count(vertex));
        const auto& neighbors = candidate->face_neighbor_index();
        REQUIRE(neighbors.size() == expected_neighbors.size());
        for (size_t face = 0; face < neighbors.size(); ++face)
            for (int edge = 0; edge < 3; ++edge)
                CHECK(neighbors[face][edge] == expected_neighbors[face][edge]);
    }
}

TEST_CASE("Native textured imports retain closed geometry through color application", "[Model][ModelImport]")
{
    const std::string fixture = GENERATE("textured.glb", "outward-textured.glb", "transformed.glb", "nested-negative-nodes.glb");
    const std::string path = std::string(TEST_DATA_DIR) + "/model_artifact/" + fixture;
    CAPTURE(fixture);

    TexturedMesh original;
    REQUIRE(load_assimp_textured_model(path, original));
    // glTF is Y-up; the ordinary importer supplies Z-up geometry to Orca.
    // Its existing unit-conversion choice remains separate from orientation.
    for (auto& vertex : original.vertices)
        vertex = {vertex[0], -vertex[2], vertex[1]};
    indexed_triangle_set original_its;
    for (const auto& vertex : original.vertices)
        original_its.vertices.emplace_back(vertex[0], vertex[1], vertex[2]);
    for (const auto& face : original.indices)
        original_its.indices.emplace_back(face[0], face[1], face[2]);
    REQUIRE(its_num_open_edges(original_its) == 0);
    const float expected_volume = std::abs(its_volume(original_its));
    REQUIRE(expected_volume > 0.f);

    Model model = Model::read_from_file(path);
    REQUIRE(model.objects.size() == 1);
    REQUIRE(model.objects.front()->volumes.size() == 1);
    const ModelVolume* volume = model.objects.front()->volumes.front();
    CHECK_THAT(volume->mesh().stats().volume, Catch::Matchers::WithinRel(expected_volume, 1e-5f));
    CHECK(its_num_open_edges(volume->mesh().its) == 0);
    REQUIRE(model.texture_mesh);
    const TexturedMesh& textured = *model.texture_mesh;

    // Winding repair must preserve each face's material and vertex/UV pairing.
    REQUIRE(textured.vertices.size() == original.vertices.size());
    REQUIRE(textured.uvs.size() == original.uvs.size());
    REQUIRE(textured.indices.size() == original.indices.size());
    CHECK(textured.material_ids == original.material_ids);
    CHECK(textured.material_texture_map == original.material_texture_map);
    REQUIRE(textured.textures.size() == original.textures.size());
    for (size_t i = 0; i < original.vertices.size(); ++i) {
        for (size_t axis = 0; axis < 3; ++axis)
            CHECK_THAT(textured.vertices[i][axis], Catch::Matchers::WithinAbs(original.vertices[i][axis], 1e-7));
        for (size_t axis = 0; axis < 2; ++axis)
            CHECK_THAT(textured.uvs[i][axis], Catch::Matchers::WithinAbs(original.uvs[i][axis], 1e-7));
    }
    for (size_t i = 0; i < original.indices.size(); ++i) {
        auto actual_face = textured.indices[i];
        auto original_face = original.indices[i];
        std::sort(actual_face.begin(), actual_face.end());
        std::sort(original_face.begin(), original_face.end());
        CHECK(actual_face == original_face);
    }

    const int unit_conversion = GENERATE(0, 1, 2);
    CAPTURE(unit_conversion);
    float unit_scale = 1.f;
    if (unit_conversion == 1) {
        model.convert_from_imperial_units(true);
        unit_scale = 25.4f;
    } else if (unit_conversion == 2) {
        model.convert_from_meters(true);
        unit_scale = 1000.f;
    }
    const Vec3d expected_size = volume->mesh().bounding_box().size();
    const Vec3d expected_offset = volume->get_offset();

    // The color dialog rebuilds the volume from texture_mesh. Validate that handoff,
    // as well as the initial geometry checked by the main window's cleanup step.
    PaintedMesh painted;
    REQUIRE(texture_to_painting(textured, painted));
    std::vector<FilamentMatch> matches;
    for (size_t i = 0; i < painted.cluster_colors.size(); ++i) {
        FilamentMatch match;
        match.cluster_index = int(i);
        match.filament_index = int(i);
        matches.push_back(match);
    }
    REQUIRE(apply_painted_mesh_to_volume(painted, matches, *model.objects.front()->volumes.front()));
    CHECK(volume->mesh().stats().volume > 0.f);
    // Automatic texture subdivision creates thousands of triangles. Accumulate
    // their geometric volume in double precision instead of comparing the
    // single-precision cached sum with the original four-face mesh.
    const auto& applied = volume->mesh().its;
    const Vec3d origin = applied.vertices.front().cast<double>();
    double applied_volume = 0.;
    for (const auto& face : applied.indices) {
        const Vec3d a = applied.vertices[face[0]].cast<double>() - origin;
        const Vec3d b = applied.vertices[face[1]].cast<double>() - origin;
        const Vec3d c = applied.vertices[face[2]].cast<double>() - origin;
        applied_volume += a.dot(b.cross(c)) / 6.;
    }
    CHECK_THAT(applied_volume,
               Catch::Matchers::WithinRel(double(expected_volume) * unit_scale * unit_scale * unit_scale, 1e-5));
    for (size_t axis = 0; axis < 3; ++axis) {
        CHECK_THAT(volume->mesh().bounding_box().size()[axis], Catch::Matchers::WithinRel(expected_size[axis], 1e-5));
        CHECK_THAT(volume->get_offset()[axis], Catch::Matchers::WithinAbs(expected_offset[axis], 1e-4));
    }
    CHECK(its_num_open_edges(volume->mesh().its) == 0);
    CHECK(model.removed_objects_with_zero_volume() == 0);
    CHECK(model.objects.size() == 1);
}

TEST_CASE("Adaptive textured imports survive geometric color smoothing on a small closed mesh", "[Model][ModelImport][Regression]")
{
    const Model model = Model::read_from_file(std::string(TEST_DATA_DIR) + "/model_artifact/textured.glb");
    REQUIRE(model.texture_mesh);
    REQUIRE_FALSE(model.texture_mesh->textures.empty());

    TexturePaintingSettings settings;
    settings.target_colors_num = 0;
    settings.smooth_weight = 0.5;
    settings.mesh_repair_decision = TexturePaintingSettings::MeshRepairDecision::ImportWithoutRepair;

    PaintedMesh painted;
    REQUIRE(texture_to_painting(*model.texture_mesh, painted, settings));
    REQUIRE_FALSE(painted.indices.empty());
    REQUIRE(painted.indices.size() == painted.face_colors.size());

    indexed_triangle_set result;
    result.vertices.reserve(painted.vertices.size());
    result.indices.reserve(painted.indices.size());
    for (const auto& vertex : painted.vertices)
        result.vertices.emplace_back(vertex[0], vertex[1], vertex[2]);
    for (const auto& face : painted.indices)
        result.indices.emplace_back(face[0], face[1], face[2]);

    CHECK(its_num_open_edges(result) == 0);
    CHECK(result.indices.size() >= 4);
}

TEST_CASE("Painted mesh unit conversion preserves nearby corners at a nonzero source offset", "[Model][ModelImport][ModelVertexColors]")
{
    const int unit_conversion = GENERATE(0, 1, 2);
    CAPTURE(unit_conversion);
    float nearby_z = 0.05f;
    for (int i = 0; i < 3; ++i) nearby_z = std::nextafter(nearby_z, 1.f);
    const float adjacent = std::nextafter(nearby_z, 1.f);
    indexed_triangle_set source;
    source.vertices = {Vec3f(0.f, 0.f, nearby_z), Vec3f(0.f, 0.f, adjacent),
                       Vec3f(0.01f, 0.f, 0.f), Vec3f(0.f, 0.01f, 0.1f)};
    source.indices = {Vec3i32(0, 2, 1), Vec3i32(0, 1, 3), Vec3i32(0, 3, 2), Vec3i32(1, 2, 3)};
    Model model;
    auto* volume = model.add_object("nearby corners", "nearby.obj", TriangleMesh(source))->volumes.front();
    const Vec3d source_offset = volume->source.mesh_offset;
    REQUIRE(source_offset.z() > 0.);
    double unit_scale = 1.;
    if (unit_conversion == 1) {
        model.convert_from_imperial_units(false);
        unit_scale = 25.4;
    } else if (unit_conversion == 2) {
        model.convert_from_meters(false);
        unit_scale = 1000.;
    }
    const Vec3d expected_offset = volume->get_offset();
    PaintedMesh painted;
    for (const auto& vertex : source.vertices) painted.vertices.push_back({vertex.x(), vertex.y(), vertex.z()});
    for (const auto& face : source.indices) painted.indices.push_back({face[0], face[1], face[2]});
    painted.cluster_colors = {{255, 0, 0}, {0, 0, 255}};
    for (size_t face = 0; face < source.indices.size(); ++face)
        painted.face_colors.push_back(painted.cluster_colors[face % 2]);
    std::vector<FilamentMatch> matches(2);
    for (int i = 0; i < 2; ++i) { matches[i].cluster_index = i; matches[i].filament_index = i; }
    REQUIRE(apply_painted_mesh_to_volume(painted, matches, *volume));
    const auto& result = volume->mesh().its;
    REQUIRE(result.vertices.size() == source.vertices.size());
    REQUIRE(result.indices.size() == source.indices.size());
    for (size_t face = 0; face < source.indices.size(); ++face) CHECK(result.indices[face] == source.indices[face]);
    const double expected_gap = (double(adjacent) - double(nearby_z)) * unit_scale;
    const double actual_gap = double(result.vertices[1].z()) - double(result.vertices[0].z());
    CHECK_THAT(actual_gap, Catch::Matchers::WithinRel(expected_gap, 1e-4));
    CHECK_THAT((volume->source.mesh_offset - source_offset).norm(), Catch::Matchers::WithinAbs(0., 1e-12));
    CHECK_THAT((volume->get_offset() - expected_offset).norm(), Catch::Matchers::WithinAbs(0., 1e-6));
    CHECK(volume->mmu_segmentation_facets.has_facets(*volume, EnforcerBlockerType::Extruder1));
    CHECK(volume->mmu_segmentation_facets.has_facets(*volume, EnforcerBlockerType::Extruder2));
}

TEST_CASE("Native GLB imports use print axes independently of the meter conversion choice", "[Model][ModelImport][GltfAxes]")
{
    const std::string path = std::string(TEST_DATA_DIR) + "/model_artifact/baseline.glb";
    Model model = Model::read_from_file(path);
    REQUIRE(model.objects.size() == 1);
    REQUIRE(model.objects.front()->volumes.size() == 1);
    REQUIRE(model.texture_mesh);
    // This fixture spans 0.06 m in X, 0.10 m in glTF Y (up), and 0.04 m in Z.
    const Vec3d expected_size(0.06, 0.04, 0.10);
    for (const float unit_scale : {1.f, 1000.f}) {
        if (unit_scale == 1000.f)
            model.convert_from_meters(true);
        CAPTURE(unit_scale);
        const auto& mesh = model.objects.front()->volumes.front()->mesh();
        const Vec3d actual_size = mesh.bounding_box().size();
        BoundingBoxf3 textured_bounds;
        for (const auto& vertex : model.texture_mesh->vertices)
            textured_bounds.merge(Vec3d(vertex[0], vertex[1], vertex[2]));
        for (size_t axis = 0; axis < 3; ++axis) {
            CHECK_THAT(actual_size[axis], Catch::Matchers::WithinAbs(expected_size[axis] * unit_scale, 1e-5));
            // The color handoff applies the volume's unit-conversion flag later.
            CHECK_THAT(textured_bounds.size()[axis] * unit_scale, Catch::Matchers::WithinAbs(actual_size[axis], 1e-5));
        }
        CHECK(mesh.stats().volume > 0.f);
        CHECK_THAT(textured_bounds.max.z(), Catch::Matchers::WithinAbs(0.10, 1e-5));
        CHECK_THAT(textured_bounds.min.z(), Catch::Matchers::WithinAbs(0., 1e-5));
    }
}

// convex_hull_2d does not clip geometry below the bed, so these cases avoid
// sinking transforms.
TEST_CASE("A part's 2D convex hull is its footprint projected onto the bed", "[Model]")
{
    Model model;
    ModelObject* object = model.add_object();
    // Keep the cube's raw coordinates ([0,20] on every axis): the default
    // add_volume re-centers the geometry, which would move the footprint.
    object->add_volume(make_cube(20, 20, 20), ModelVolumeType::MODEL_PART, false);

    SECTION("identity transform yields the 20 mm square") {
        const Polygon hull   = object->convex_hull_2d(Geometry::Transformation{}.get_matrix());
        const BoundingBox bb = hull.bounding_box();
        CHECK(hull.size() == 4);
        CHECK(bb.min.x() == scaled(0.));
        CHECK(bb.min.y() == scaled(0.));
        CHECK(bb.max.x() == scaled(20.));
        CHECK(bb.max.y() == scaled(20.));
    }

    SECTION("scaling and offset move and grow the footprint") {
        Geometry::Transformation t;
        t.set_scaling_factor({2, 2, 2}); // cube now spans [0,40]
        t.set_offset({10, 5, 0});        // then shift +10 in X, +5 in Y

        const Polygon hull   = object->convex_hull_2d(t.get_matrix());
        const BoundingBox bb = hull.bounding_box();
        CHECK(hull.size() == 4);
        CHECK(bb.min.x() == scaled(10.));
        CHECK(bb.min.y() == scaled(5.));
        CHECK(bb.max.x() == scaled(50.));
        CHECK(bb.max.y() == scaled(45.));
    }
}

namespace {
void append_painted_root(TriangleSelector::TriangleSplittingData& data, int root,
                         std::initializer_list<int> nibbles)
{
    data.triangles_to_split.emplace_back(root, int(data.bitstream.size()));
    for (int nibble : nibbles)
        for (int bit = 0; bit < 4; ++bit)
            data.bitstream.push_back((nibble >> bit) & 1);
}
}

TEST_CASE("Painted extruder queries retain split leaves with missing or stale summaries", "[Model][ExtruderCount]")
{
    Model model;
    auto* volume = model.add_object()->add_volume(make_cube(20, 20, 20), ModelVolumeType::MODEL_PART, false);
    TriangleSelector::TriangleSplittingData data;
    // A four-child root contains a nested two-child split and extended slots.
    append_painted_root(data, 0, {3, 4, 1, 12, 14, 12, 15, 0, 12, 15, 14, 0});
    append_painted_root(data, 1, {8});
    append_painted_root(data, 2, {12, 0});
    const int summary = GENERATE(0, 1, 2, 3);
    CAPTURE(summary);
    if (summary == 0) data.used_states.clear();
    if (summary == 1) std::fill(data.used_states.begin(), data.used_states.end(), false);
    if (summary == 2) std::fill(data.used_states.begin(), data.used_states.end(), true);
    if (summary == 3) data.used_states[4] = true;
    const auto saved = data;
    volume->mmu_segmentation_facets.set_data(std::move(data));
    const auto stamp = volume->mmu_segmentation_facets.timestamp();
    CHECK(volume->get_extruders() == std::vector<int>({1, 2, 3, 17, 18, 32, 1}));
    CHECK(volume->get_extruders() == std::vector<int>({1, 2, 3, 17, 18, 32, 1}));
    CHECK(volume->mmu_segmentation_facets.get_data() == saved);
    CHECK(volume->mmu_segmentation_facets.timestamp() == stamp);
}

TEST_CASE("Painted extruder caches refresh after replacement copy assignment and reset", "[Model][ExtruderCount]")
{
    Model model;
    auto* volume = model.add_object()->add_volume(make_cube(20, 20, 20), ModelVolumeType::MODEL_PART, false);
    TriangleSelector selector(volume->mesh());
    selector.set_facet(0, EnforcerBlockerType::Extruder32);
    REQUIRE(volume->mmu_segmentation_facets.set(selector));
    CHECK(volume->get_extruders() == std::vector<int>({32, 1}));
    Model copied(model);
    auto* other = copied.objects.front()->volumes.front();
    CHECK(other->get_extruders() == std::vector<int>({32, 1}));
    TriangleSelector replacement(other->mesh());
    replacement.set_facet(1, EnforcerBlockerType::Extruder18);
    REQUIRE(other->mmu_segmentation_facets.set(replacement));
    CHECK(other->get_extruders() == std::vector<int>({18, 1}));
    CHECK(volume->get_extruders() == std::vector<int>({32, 1}));
    volume->mmu_segmentation_facets.assign(other->mmu_segmentation_facets);
    CHECK(volume->get_extruders() == std::vector<int>({18, 1}));
    volume->config.set_key_value("extruder", new ConfigOptionInt(5));
    CHECK(volume->get_extruders() == std::vector<int>({18, 5}));
    volume->mmu_segmentation_facets.reset();
    CHECK(volume->get_extruders() == std::vector<int>({5}));
}

TEST_CASE("Historical noncanonical painted roots keep deserialization behavior", "[Model][ExtruderCount]")
{
    Model model;
    auto* volume = model.add_object()->add_volume(make_cube(20, 20, 20), ModelVolumeType::MODEL_PART, false);
    TriangleSelector::TriangleSplittingData data;
    const int history = GENERATE(0, 1, 2);
    CAPTURE(history);
    if (history == 0) {
        append_painted_root(data, 0, {8});
        append_painted_root(data, 0, {12, 15, 14}); // Last assignment wins.
    } else if (history == 1) {
        append_painted_root(data, 0, {8});
        append_painted_root(data, int(volume->mesh().its.indices.size()), {12, 15, 14});
    } else {
        append_painted_root(data, 2, {8});
        append_painted_root(data, 0, {12, 15, 14});
    }
    volume->mmu_segmentation_facets.set_data(std::move(data));
    const auto expected = history == 0 ? std::vector<int>{32, 1} :
                          history == 1 ? std::vector<int>{1} : std::vector<int>{2, 32, 1};
    CHECK(volume->get_extruders() == expected);
    CHECK(volume->get_extruders() == expected);
}

TEST_CASE("Painted extruder lists refresh after filament limiting and deletion", "[Model][ExtruderCount]")
{
    Model model;
    auto* volume = model.add_object()->add_volume(make_cube(20, 20, 20), ModelVolumeType::MODEL_PART, false);
    TriangleSelector selector(volume->mesh());
    selector.set_facet(0, EnforcerBlockerType::Extruder1);
    selector.set_facet(1, EnforcerBlockerType::Extruder2);
    selector.set_facet(2, EnforcerBlockerType::Extruder18);
    selector.set_facet(3, EnforcerBlockerType::Extruder32);
    REQUIRE(volume->mmu_segmentation_facets.set(selector));
    CHECK(volume->get_extruders() == std::vector<int>({1, 2, 18, 32, 1}));
    const bool remove = GENERATE(false, true);
    CAPTURE(remove);
    if (remove) {
        volume->update_extruder_count_when_delete_filament(31, 1, 2);
        CHECK(volume->get_extruders() == std::vector<int>({1, 2, 17, 31, 1}));
    } else {
        volume->update_extruder_count(18);
        CHECK(volume->get_extruders() == std::vector<int>({1, 2, 18, 1}));
    }
}

TEST_CASE("An object's raw mesh keeps the triangles of each part on its own vertices", "[Model]")
{
    Model model;
    ModelObject *object = model.add_object();
    object->add_volume(make_cube(10, 10, 10), ModelVolumeType::MODEL_PART, false);
    TriangleMesh second = make_cube(10, 10, 10);
    second.translate(30, 0, 0);
    object->add_volume(std::move(second), ModelVolumeType::MODEL_PART, false);

    // Two separate cubes stay two closed components, one around each cube.
    const std::vector<indexed_triangle_set> parts = its_split(object->raw_indexed_triangle_set());
    REQUIRE(parts.size() == 2);
    std::vector<double> min_x;
    for (const indexed_triangle_set &part : parts) {
        CHECK(part.indices.size() == 12);
        min_x.push_back(bounding_box(part).min.x());
    }
    std::sort(min_x.begin(), min_x.end());
    CHECK_THAT(min_x.front(), Catch::Matchers::WithinAbs(0., 1e-4));
    CHECK_THAT(min_x.back(), Catch::Matchers::WithinAbs(30., 1e-4));
}

TEST_CASE("An instance added from another's scale, rotation and mirror matches it", "[Model]")
{
    // Add instance and Fill bed copy an instance this way.
    const int case_idx = GENERATE(0, 1);
    Transform3d trafo = Transform3d::Identity();
    if (case_idx == 0)
        trafo.linear() = Eigen::AngleAxisd(0.5 * PI, Vec3d::UnitZ()).toRotationMatrix() * Vec3d(-1., 1., 1.).asDiagonal();
    else
        trafo.linear() << 4.4408921e-16,   0.819152044,    0.573576436,
                          1.0,            -4.4408921e-16, -1.11022302e-16,
                         -5.55111512e-17, -0.573576436,    0.819152044;
    CAPTURE(case_idx);

    Model model;
    ModelObject *object = model.add_object();
    object->add_volume(make_cube(10, 20, 30));
    ModelInstance *original = object->add_instance();
    original->set_transformation(Geometry::Transformation(trafo));
    const ModelInstance *copy = object->add_instance(original->get_offset(), original->get_scaling_factor(),
                                                     original->get_rotation(), original->get_mirror());
    CHECK(copy->get_matrix().linear().isApprox(original->get_matrix().linear(), 1e-9));
}

namespace {

// A flat triangle first, then a cube for each offset, all in one mesh.
TriangleMesh flat_triangle_then_cubes(const std::vector<double> &cube_offsets)
{
    indexed_triangle_set its;
    its.vertices = {{-20.f, 0.f, 0.f}, {-10.f, 0.f, 0.f}, {-15.f, 5.f, 0.f}};
    its.indices  = {{0, 1, 2}};
    for (double offset : cube_offsets) {
        TriangleMesh cube = make_cube(10, 10, 10);
        cube.translate(float(offset), 0.f, 0.f);
        its_merge(its, cube.its);
    }
    return TriangleMesh(its);
}

} // namespace

TEST_CASE("Splitting a part drops pieces enclosing no volume and keeps the part itself", "[Model]")
{
    Model model;
    ModelObject *object = model.add_object();
    ModelVolume *volume = object->add_volume(flat_triangle_then_cubes({0., 30.}), ModelVolumeType::MODEL_PART, false);
    REQUIRE(volume->is_splittable());

    CHECK(volume->split(1, false) == 2);
    REQUIRE(object->volumes.size() == 2);
    CHECK(std::find(object->volumes.begin(), object->volumes.end(), volume) != object->volumes.end());
    for (const ModelVolume *part : object->volumes) {
        CHECK(part->mesh().facets_count() == 12);
        CHECK_FALSE(part->is_splittable());
    }
}

TEST_CASE("Splitting a part with a single solid piece keeps just that piece", "[Model]")
{
    Model model;
    ModelObject *object = model.add_object();
    ModelVolume *volume = object->add_volume(flat_triangle_then_cubes({0.}), ModelVolumeType::MODEL_PART, false);

    CHECK(volume->split(1, false) == 1);
    REQUIRE(object->volumes.size() == 1);
    CHECK(object->volumes.front() == volume);
    CHECK(volume->mesh().facets_count() == 12);
}

TEST_CASE("Splitting a part made only of pieces enclosing no volume leaves it whole", "[Model]")
{
    Model model;
    ModelObject *object = model.add_object();
    indexed_triangle_set its;
    its.vertices = {{0.f, 0.f, 0.f}, {10.f, 0.f, 0.f}, {5.f, 5.f, 0.f}, {20.f, 0.f, 0.f}, {30.f, 0.f, 0.f}, {25.f, 5.f, 0.f}};
    its.indices  = {{0, 1, 2}, {3, 4, 5}};
    ModelVolume *volume = object->add_volume(TriangleMesh(its), ModelVolumeType::MODEL_PART, false);

    CHECK(volume->split(1, false) == 1);
    REQUIRE(object->volumes.size() == 1);
    CHECK(object->volumes.front() == volume);
    CHECK(volume->mesh().facets_count() == 2);
}

TEST_CASE("Replacing a part's mesh updates whether it can be split", "[Model]")
{
    Model model;
    ModelObject *object = model.add_object();
    TriangleMesh two_cubes = make_cube(10, 10, 10);
    TriangleMesh second    = make_cube(10, 10, 10);
    second.translate(30.f, 0.f, 0.f);
    two_cubes.merge(second);
    ModelVolume *volume = object->add_volume(std::move(two_cubes), ModelVolumeType::MODEL_PART, false);
    REQUIRE(volume->is_splittable());

    volume->set_mesh(make_cube(10, 10, 10));
    CHECK_FALSE(volume->is_splittable());
}

TEST_CASE("Repairing a part replaces its mesh only when the repair changes it", "[Model]")
{
    Model model;
    ModelObject *object = model.add_object();
    TriangleMesh flipped = make_cube(10, 10, 10);
    std::swap(flipped.its.indices.front()[1], flipped.its.indices.front()[2]);
    ModelVolume *broken = object->add_volume(std::move(flipped), ModelVolumeType::MODEL_PART, false);
    TriangleMesh cube = make_cube(10, 10, 10);
    cube.translate(30.f, 0.f, 0.f);
    ModelVolume *valid = object->add_volume(std::move(cube), ModelVolumeType::MODEL_PART, false);
    const ObjectID broken_id = broken->id();
    const ObjectID valid_id  = valid->id();

    MeshRepairStats stats;
    REQUIRE(broken->repair(false, &stats));
    CHECK(stats.modified);
    CHECK(broken->id() != broken_id);
    CHECK_THAT(its_volume(broken->mesh().its), Catch::Matchers::WithinRel(1000., 1e-6));
    CHECK_FALSE(broken->get_convex_hull().empty());

    REQUIRE(valid->repair(false, &stats));
    CHECK_FALSE(stats.modified);
    CHECK(valid->id() == valid_id);
}

// A cube whose raw mesh spans [0, size] on every axis, with one instance at plate_offset on the plate.
static ModelObject *add_cube(Model &model, double size, const Vec3d &plate_offset)
{
    ModelObject *object = model.add_object();
    object->add_volume(make_cube(size, size, size), ModelVolumeType::MODEL_PART, false);
    object->add_instance()->set_offset(plate_offset);
    return object;
}

TEST_CASE("An object placed in the assembly view goes 10 mm to the right of the others", "[Model]")
{
    Model model;
    ModelObject *placed = add_cube(model, 20., Vec3d::Zero());
    placed->instances.front()->set_assemble_transformation(placed->instances.front()->get_transformation());
    const Vec3d plate_offset(128., 128., 5.);
    ModelObject *added = add_cube(model, 10., plate_offset);

    model.set_assembly_pos(added);

    const BoundingBoxf3 placed_box = placed->bounding_box_in_assembly_view();
    const BoundingBoxf3 added_box  = added->bounding_box_in_assembly_view();
    CHECK(added->instances.front()->is_assemble_initialized());
    CHECK_THAT(added_box.min.x(), Catch::Matchers::WithinAbs(placed_box.max.x() + 10., 1e-6));
    CHECK_THAT(added_box.center().y(), Catch::Matchers::WithinAbs(placed_box.center().y(), 1e-6));
    // The height comes from the plate, and the plate itself is left alone.
    CHECK_THAT(added_box.min.z(), Catch::Matchers::WithinAbs(plate_offset.z(), 1e-6));
    CHECK_THAT((added->instances.front()->get_offset() - plate_offset).norm(), Catch::Matchers::WithinAbs(0., 1e-6));
}

TEST_CASE("An object alone in the assembly view is centred on the origin and keeps its instance layout", "[Model]")
{
    Model model;
    ModelObject *object = add_cube(model, 10., {100., 100., 0.});
    object->add_instance()->set_offset({130., 100., 0.});

    model.set_assembly_pos(object);

    const BoundingBoxf3 box = object->bounding_box_in_assembly_view();
    CHECK_THAT(box.center().x(), Catch::Matchers::WithinAbs(0., 1e-6));
    CHECK_THAT(box.center().y(), Catch::Matchers::WithinAbs(0., 1e-6));
    const Vec3d spacing = object->instances[1]->get_assemble_offset() - object->instances[0]->get_assemble_offset();
    CHECK_THAT((spacing - Vec3d(30., 0., 0.)).norm(), Catch::Matchers::WithinAbs(0., 1e-6));
}

TEST_CASE("Ignored objects do not push an object aside in the assembly view", "[Model]")
{
    Model model;
    ModelObject *ignored = add_cube(model, 20., Vec3d::Zero());
    ignored->instances.front()->set_assemble_transformation(ignored->instances.front()->get_transformation());
    ModelObject *object = add_cube(model, 10., {128., 128., 0.});

    model.set_assembly_pos(object, {ignored});

    const BoundingBoxf3 box = object->bounding_box_in_assembly_view();
    CHECK_THAT(box.center().x(), Catch::Matchers::WithinAbs(0., 1e-6));
    CHECK_THAT(box.center().y(), Catch::Matchers::WithinAbs(0., 1e-6));
}
namespace {

// A box of the given size whose lowest corner is at `corner` in the file's coordinates. The mesh keeps them, unless
// `as_reader` centers it and keeps the same place in the volume's offset, as the file readers do.
ModelVolume *add_box(ModelObject *object, const Vec3d &size, const Vec3d &corner, bool as_reader = false)
{
    TriangleMesh box = make_cube(size.x(), size.y(), size.z());
    box.translate(corner.cast<float>());
    return object->add_volume(std::move(box), ModelVolumeType::MODEL_PART, as_reader);
}

// The vertices of every volume (or only of the model parts) of every instance of every object, in world coordinates,
// in a stable order.
std::vector<Vec3d> world_vertices(const Model &model, bool parts_only = false)
{
    std::vector<Vec3d> points;
    for (const ModelObject *object : model.objects)
        for (const ModelInstance *instance : object->instances)
            for (const ModelVolume *volume : object->volumes)
                if (!parts_only || volume->is_model_part())
                    for (const Vec3f &p : volume->mesh().its.vertices)
                        points.push_back(instance->get_matrix() * volume->get_matrix() * p.cast<double>());
    return points;
}

// The box around the given points.
BoundingBoxf3 points_box(const std::vector<Vec3d> &points)
{
    BoundingBoxf3 bb;
    for (const Vec3d &p : points)
        bb.merge(p);
    return bb;
}

// The larger distance between the matching corners of two boxes.
double box_distance(const BoundingBoxf3 &a, const BoundingBoxf3 &b) { return std::max((a.min - b.min).norm(), (a.max - b.max).norm()); }

// Largest distance between two lists of points of equal length.
double max_distance(const std::vector<Vec3d> &a, const std::vector<Vec3d> &b)
{
    REQUIRE(a.size() == b.size());
    double distance = 0.;
    for (size_t i = 0; i < a.size(); ++i)
        distance = std::max(distance, (a[i] - b[i]).norm());
    return distance;
}

// The points turned by `angle` around the vertical axis through `center`.
std::vector<Vec3d> turned(const std::vector<Vec3d> &points, double angle, const Vec2d &center)
{
    const Vec3d       center_3d(center.x(), center.y(), 0.);
    const Transform3d around = Geometry::translation_transform(center_3d) * Geometry::rotation_transform(angle * Vec3d::UnitZ()) *
                               Geometry::translation_transform(-center_3d);
    std::vector<Vec3d> out;
    for (const Vec3d &p : points)
        out.push_back(around * p);
    return out;
}

// The XY center of the box around the given points.
Vec2d layout_center(const std::vector<Vec3d> &points)
{
    const BoundingBoxf3 bb = points_box(points);
    return {bb.center().x(), bb.center().y()};
}

// The linear parts (rotation, scale, mirror) of the volumes and of the instances of an object.
std::vector<Matrix3d> linear_parts(const ModelObject &object)
{
    std::vector<Matrix3d> parts;
    for (const ModelVolume *volume : object.volumes)
        parts.push_back(volume->get_matrix().linear());
    for (const ModelInstance *instance : object.instances)
        parts.push_back(instance->get_matrix().linear());
    return parts;
}

} // namespace

TEST_CASE("Centering a placed object keeps it where it stands", "[Model]")
{
    const bool as_reader = GENERATE(false, true);
    CAPTURE(as_reader);

    Model model;
    ModelObject *object = model.add_object();
    // A part far from its object's origin, placed by its instance, as an AMF constellation leaves it, with a
    // modifier reaching beyond it.
    add_box(object, {20., 10., 5.}, {100., 50., 0.}, as_reader);
    TriangleMesh modifier = make_cube(40., 10., 10.);
    modifier.translate(90.f, 50.f, 0.f);
    object->add_volume(std::move(modifier), ModelVolumeType::PARAMETER_MODIFIER, false);
    object->add_instance()->set_offset({5., 7., 0.});
    const std::vector<Vec3d> before = world_vertices(model);

    model.rotate_and_center_objects(0.);

    CHECK_THAT(max_distance(world_vertices(model), before), Catch::Matchers::WithinAbs(0., 1e-4));
    CHECK_THAT(object->full_raw_mesh_bounding_box().center().norm(), Catch::Matchers::WithinAbs(0., 1e-4));
    // The instance compensates the centering, so there is nothing for merging or export to undo.
    CHECK_THAT(object->origin_translation.norm(), Catch::Matchers::WithinAbs(0., 1e-9));
}

TEST_CASE("Turning placed objects of different heights keeps them on the bed", "[Model]")
{
    const double angle  = GENERATE(0., 0.25 * PI);
    const double raised = GENERATE(0., 1.);
    CAPTURE(angle, raised);

    Model model;
    ModelObject *low  = model.add_object();
    ModelObject *flat = model.add_object();
    add_box(low, {20., 10., 5.}, {0., 0., 0.});
    add_box(flat, {30., 30., 2.}, {0., 0., raised});
    low->add_instance()->set_offset({110., 110., 0.});
    flat->add_instance()->set_offset({150., 125., 0.});
    // Fill the bounding box caches first, so a stale cache would show.
    REQUIRE_THAT(low->min_z(), Catch::Matchers::WithinAbs(0., 1e-6));
    REQUIRE_THAT(flat->min_z(), Catch::Matchers::WithinAbs(raised, 1e-6));

    model.rotate_and_center_objects(angle);

    CHECK_THAT(low->min_z(), Catch::Matchers::WithinAbs(0., 1e-4));
    CHECK_THAT(flat->min_z(), Catch::Matchers::WithinAbs(raised, 1e-4));
    CHECK(model.looks_like_multipart_object() == (raised != 0.));
}

TEST_CASE("Turning placed objects turns their whole layout around its center", "[Model]")
{
    Model model;
    ModelObject *bracket = model.add_object();
    ModelObject *plate   = model.add_object();
    add_box(bracket, {20., 10., 5.}, {0., 0., 0.});
    // A part at an offset inside its object turns with it.
    add_box(bracket, {4., 4., 8.}, {25., 0., 0.});
    add_box(plate, {30., 30., 2.}, {40., 40., 0.});
    bracket->add_instance()->set_offset({110., 110., 0.});
    bracket->add_instance()->set_offset({110., 140., 0.});
    // A turned and scaled copy still takes the rotation in its volumes.
    ModelInstance *plate_instance = plate->add_instance();
    plate_instance->set_offset({150., 125., 0.});
    plate_instance->set_rotation({0., 0., 0.3});
    plate_instance->set_scaling_factor({1.5, 1.5, 1.5});
    // A modifier reaching far to one side turns with its object but does not move the layout's center.
    TriangleMesh modifier = make_cube(60., 5., 5.);
    modifier.translate(200.f, 0.f, 0.f);
    bracket->add_volume(std::move(modifier), ModelVolumeType::PARAMETER_MODIFIER, false);
    // An object with nothing in it takes no part in the layout.
    model.add_object()->add_instance();
    // Fill the bounding box caches first, so a stale cache would show.
    for (const ModelObject *object : model.objects) {
        object->bounding_box_exact();
        object->raw_mesh_bounding_box();
    }

    const double                angle        = 0.5 * PI;
    const std::vector<Vec3d>    before       = world_vertices(model);
    const Vec2d                 center       = layout_center(world_vertices(model, true));
    const std::vector<Matrix3d> plate_before = linear_parts(*plate);
    // The modifier must reach outside the parts, or counting it would not move the center.
    REQUIRE((layout_center(before) - center).norm() > 1.);

    model.rotate_and_center_objects(angle);

    CHECK_THAT(max_distance(world_vertices(model), turned(before, angle, center)), Catch::Matchers::WithinAbs(0., 1e-3));
    // Instances that commute with the turn keep their own rotation and scale; the volumes take the turn.
    const Matrix3d              turn        = Geometry::rotation_transform(angle * Vec3d::UnitZ()).linear();
    const std::vector<Matrix3d> plate_after = linear_parts(*plate);
    CHECK(plate_after[0].isApprox(turn * plate_before[0], 1e-9));
    CHECK(plate_after[1].isApprox(plate_before[1], 1e-9));
    // The caches follow the change.
    for (const ModelObject *object : model.objects) {
        if (object->volumes.empty())
            continue;
        BoundingBoxf3 raw;
        for (const ModelVolume *volume : object->volumes)
            if (volume->is_model_part())
                raw.merge(volume->mesh().transformed_bounding_box(volume->get_matrix()));
        CHECK_THAT(box_distance(object->raw_mesh_bounding_box(), raw), Catch::Matchers::WithinAbs(0., 1e-4));
        BoundingBoxf3 exact;
        for (size_t i = 0; i < object->instances.size(); ++i)
            exact.merge(object->instance_bounding_box(i));
        CHECK_THAT(box_distance(object->bounding_box_exact(), exact), Catch::Matchers::WithinAbs(0., 1e-4));
    }
}

TEST_CASE("Turning keeps mirrored, tilted and skewed instances in the turned layout", "[Model]")
{
    const int case_idx = GENERATE(0, 1, 2, 3, 4);
    CAPTURE(case_idx);
    Transform3d linear = Transform3d::Identity();
    switch (case_idx) {
    case 0: linear.linear() = Vec3d(-1., 1., 1.).asDiagonal(); break;                            // mirror X
    case 1: linear.linear() = Vec3d(-1., -1., 1.).asDiagonal(); break;                           // both horizontal mirrors
    case 2: linear.linear() = Eigen::AngleAxisd(0.5, Vec3d::UnitX()).toRotationMatrix(); break; // tilt about X
    case 3: linear.linear() << 1., 0.3, 0., 0., 1., 0., 0., 0., 1.; break;                      // skew in XY
    default: linear.linear() = Vec3d(2., 1., 1.).asDiagonal(); break;                            // uneven XY scale
    }

    Model model;
    ModelObject *object = model.add_object();
    add_box(object, {20., 10., 5.}, {100., 0., 0.});
    add_box(object, {4., 4., 8.}, {125., 0., 0.});
    // One plain copy and one transformed copy of the same object: both must stay right.
    object->add_instance()->set_offset({60., 60., 0.});
    ModelInstance *transformed = object->add_instance();
    transformed->set_transformation(Geometry::Transformation(Geometry::translation_transform({160., 90., 0.}) * linear));

    const double                angle         = -0.25 * PI;
    const std::vector<Vec3d>    before        = world_vertices(model);
    const Vec2d                 center        = layout_center(before);
    const std::vector<Matrix3d> linear_before = linear_parts(*object);

    model.rotate_and_center_objects(angle);

    CHECK_THAT(max_distance(world_vertices(model), turned(before, angle, center)), Catch::Matchers::WithinAbs(0., 1e-3));
    // Both horizontal mirrors commute with the turn, so the volumes take it; otherwise every instance of the object
    // takes it and the shared volumes keep theirs.
    const Matrix3d              turn         = Geometry::rotation_transform(angle * Vec3d::UnitZ()).linear();
    const std::vector<Matrix3d> linear_after = linear_parts(*object);
    const size_t                volumes      = object->volumes.size();
    for (size_t i = 0; i < linear_after.size(); ++i) {
        CAPTURE(i);
        const bool turned_here = (i < volumes) == (case_idx == 1);
        CHECK(linear_after[i].isApprox(turned_here ? Matrix3d(turn * linear_before[i]) : linear_before[i], 1e-9));
    }
}

TEST_CASE("Merging turned objects into one keeps every part where it stands", "[Model]")
{
    Model model;
    ModelObject *bracket = model.add_object();
    ModelObject *plate   = model.add_object();
    add_box(bracket, {20., 10., 5.}, {100., 50., 0.});
    add_box(plate, {30., 30., 2.}, {-40., 10., 3.});
    bracket->add_instance()->set_offset({10., 20., 0.});
    plate->add_instance()->set_offset({60., 20., 0.});

    model.rotate_and_center_objects(0.25 * PI);
    const std::vector<Vec3d> standing = world_vertices(model);
    model.convert_multipart_object(1);

    // The merged object has no instance yet: its parts are in world coordinates.
    REQUIRE(model.objects.size() == 1);
    std::vector<Vec3d> merged;
    for (const ModelVolume *volume : model.objects.front()->volumes)
        for (const Vec3f &p : volume->mesh().its.vertices)
            merged.push_back(volume->get_matrix() * p.cast<double>());
    CHECK_THAT(max_distance(merged, standing), Catch::Matchers::WithinAbs(0., 1e-3));
}

TEST_CASE("Unplaced objects turn around the file origin and keep their height", "[Model]")
{
    Model model;
    ModelObject *bracket = model.add_object();
    ModelObject *plate   = model.add_object();
    add_box(bracket, {20., 10., 5.}, {100., 50., 0.});
    add_box(plate, {30., 30., 2.}, {-40., 10., 3.});
    // Unplaced objects lie in the file's coordinates; origin_translation records how far they were centered.
    auto file_points = [&model]() {
        std::vector<Vec3d> points;
        for (const ModelObject *object : model.objects)
            for (const ModelVolume *volume : object->volumes)
                for (const Vec3f &p : volume->mesh().its.vertices)
                    points.push_back(volume->get_matrix() * p.cast<double>() - object->origin_translation);
        return points;
    };
    const double             angle  = 0.5 * PI;
    const std::vector<Vec3d> before = file_points();

    model.rotate_and_center_objects(angle);

    CHECK_THAT(max_distance(file_points(), turned(before, angle, Vec2d::Zero())), Catch::Matchers::WithinAbs(0., 1e-3));
    for (const ModelObject *object : model.objects) {
        const BoundingBoxf3 bb = object->full_raw_mesh_bounding_box();
        CHECK_THAT(bb.center().x(), Catch::Matchers::WithinAbs(0., 1e-4));
        CHECK_THAT(bb.center().y(), Catch::Matchers::WithinAbs(0., 1e-4));
        CHECK_THAT(object->origin_translation.z(), Catch::Matchers::WithinAbs(0., 1e-9));
    }
    CHECK_THAT(bracket->full_raw_mesh_bounding_box().min.z(), Catch::Matchers::WithinAbs(0., 1e-4));
    CHECK_THAT(plate->full_raw_mesh_bounding_box().min.z(), Catch::Matchers::WithinAbs(3., 1e-4));
}

TEST_CASE("Unplaced objects scaled to millimetres still merge on a common base", "[Model]")
{
    Model model;
    ModelObject *tall = model.add_object();
    ModelObject *flat = model.add_object();
    add_box(tall, {2., 1., 0.5}, {0., 0., 0.});
    add_box(flat, {3., 3., 0.2}, {5., 0., 0.});

    model.rotate_and_center_objects(0.);
    // The rest of the import as it runs for several files loaded as one object.
    model.convert_from_imperial_units(false);
    for (ModelObject *object : model.objects)
        object->center_around_origin(false);
    model.convert_multipart_object(1);

    REQUIRE(model.objects.size() == 1);
    REQUIRE(model.objects.front()->volumes.size() == 2);
    std::vector<double> bases;
    for (const ModelVolume *volume : model.objects.front()->volumes)
        bases.push_back(volume->mesh().transformed_bounding_box(volume->get_matrix()).min.z());
    CHECK_THAT(bases.front(), Catch::Matchers::WithinAbs(bases.back(), 1e-3));
}

TEST_CASE("Converting a part's units keeps its material", "[Model]")
{
    Model model;
    ModelObject *object = model.add_object();
    ModelVolume *volume = object->add_volume(make_cube(10, 10, 10), ModelVolumeType::MODEL_PART, false);
    volume->set_material_id("mat_a");

    ModelObjectPtrs converted;
    object->convert_units(converted, ConversionType::CONV_FROM_INCH, {});

    REQUIRE(converted.size() == 1);
    REQUIRE(converted.front()->volumes.size() == 1);
    CHECK(converted.front()->volumes.front()->material_id() == "mat_a");

    // convert_units() hands its objects to the caller unowned; give them to a model so they are freed.
    Model owner;
    owner.objects = std::move(converted);
}

TEST_CASE("Converting a part's units keeps an id whose material is missing", "[Model]")
{
    Model model;
    ModelObject *object = model.add_object();
    ModelVolume *volume = object->add_volume(make_cube(10, 10, 10), ModelVolumeType::MODEL_PART, false);
    volume->set_material_id("mat_a");
    model.delete_material("mat_a");

    ModelObjectPtrs converted;
    object->convert_units(converted, ConversionType::CONV_FROM_INCH, {});

    REQUIRE(converted.size() == 1);
    REQUIRE(converted.front()->volumes.size() == 1);
    CHECK(converted.front()->volumes.front()->material_id() == "mat_a");

    // convert_units() hands its objects to the caller unowned; give them to a model so they are freed.
    Model owner;
    owner.objects = std::move(converted);
}

TEST_CASE("A no-op volume repair preserves its mesh identity and painting", "[Model][MeshRepair][Regression]")
{
    const bool keep_painting = GENERATE(false, true);
    Model model;
    ModelVolume *volume = model.add_object()->add_volume(
        make_cube(10, 10, 10), ModelVolumeType::MODEL_PART, false);
    TriangleSelector selector(volume->mesh());
    selector.set_facet(0, EnforcerBlockerType::Extruder2);
    REQUIRE(volume->mmu_segmentation_facets.set(selector));
    const ObjectID id = volume->id();
    const auto mesh = volume->mesh_ptr();
    const auto paint = volume->mmu_segmentation_facets.get_data();
    MeshRepairStats stats;
    std::string error;
    REQUIRE(volume->repair(keep_painting, &stats, &error));
    CHECK_FALSE(stats.modified);
    CHECK(error.empty());
    CHECK(volume->id() == id);
    CHECK(volume->mesh_ptr() == mesh);
    CHECK(volume->mmu_segmentation_facets.get_data() == paint);
}
