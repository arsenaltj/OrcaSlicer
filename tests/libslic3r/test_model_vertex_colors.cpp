#include <catch2/catch_all.hpp>

#include "libslic3r/TextureToColor/CgalUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ObjColorUtils.hpp"
#include "libslic3r/TexturePainting.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TextureToColor/ColorUtils.hpp"
#include <cstring>
#include "test_utils.hpp"

#include <array>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <map>
#include <limits>
#include <set>
#include <atomic>
#include <thread>
#include <random>
#include <functional>
#include <stdexcept>
#include <cstdint>
#include <tbb/task_arena.h>
#include <CGAL/AABB_face_graph_triangle_primitive.h>
#include <CGAL/AABB_traits_3.h>
#include <CGAL/AABB_tree.h>

using namespace Slic3r;

TEST_CASE("Fixed import palettes keep edited target colors and source regions", "[ModelVertexColors][FixedPalette]")
{
    const bool include_vertex_colors = GENERATE(false, true);
    const bool share_target_color = GENERATE(false, true);
    TexturedMesh mesh;
    for (int part = 0; part < 2; ++part) {
        const int first = (int)mesh.vertices.size();
        for (const auto& v : std::array<std::array<float, 3>, 4>{{{0,0,0}, {10,0,0}, {0,10,0}, {0,0,10}}}) {
            mesh.vertices.push_back({v[0] + part * 20.f, v[1], v[2]});
            if (include_vertex_colors)
                mesh.precomputed_vertex_colors.push_back(part == 0 ? std::array<float,4>{1,0,0,1} : std::array<float,4>{0,0,1,1});
        }
        for (const auto& f : std::array<std::array<int, 3>, 4>{{{0,2,1}, {0,1,3}, {0,3,2}, {1,2,3}}}) {
            mesh.indices.push_back({first + f[0], first + f[1], first + f[2]});
            mesh.precomputed_face_colors.push_back(part == 0 ? std::array<size_t,3>{255,0,0} : std::array<size_t,3>{0,0,255});
        }
    }
    TexturePaintingSettings settings;
    settings.smooth_weight = 0;
    settings.fixed_mapping_palette = {{255,0,0}, {0,0,255}, {0,255,0}};
    settings.fixed_palette = {{0,0,255}, {255,0,0}, {255,255,0}};
    if (share_target_color)
        settings.fixed_palette[1] = settings.fixed_palette[0];
    PaintedMesh painted;
    REQUIRE(face_colors_to_painting(mesh, painted, settings));
    REQUIRE(painted.face_colors.size() == mesh.indices.size());
    for (size_t i = 0; i < painted.face_colors.size(); ++i)
        CHECK(painted.face_colors[i] == settings.fixed_palette[i < 4 ? 0 : 1]);
    CHECK(painted.cluster_colors.size() == (share_target_color ? 1 : 2));
    CHECK(painted.vertices == mesh.vertices);
    CHECK(painted.indices == mesh.indices);

    if (share_target_color) {
        settings.fixed_palette[1] = {247,226,218};
        REQUIRE(face_colors_to_painting(mesh, painted, settings));
        REQUIRE(painted.face_colors.size() == mesh.indices.size());
        for (size_t i = 0; i < painted.face_colors.size(); ++i)
            CHECK(painted.face_colors[i] == settings.fixed_palette[i < 4 ? 0 : 1]);
        CHECK(painted.cluster_colors.size() == 2);
        CHECK(painted.vertices == mesh.vertices);
        CHECK(painted.indices == mesh.indices);
    }
}

TEST_CASE("Fixed import palettes reject invalid colors and mismatched source centers", "[ModelVertexColors][FixedPalette]")
{
    TexturedMesh mesh;
    mesh.vertices = {{0,0,0}, {10,0,0}, {0,10,0}, {0,0,10}};
    mesh.indices = {{0,2,1}, {0,1,3}, {0,3,2}, {1,2,3}};
    mesh.precomputed_face_colors.assign(4, {255,0,0});
    TexturePaintingSettings settings;
    settings.smooth_weight = 0;
    settings.fixed_palette = {{0,0,255}};
    settings.fixed_mapping_palette = {{255,0,0}, {0,255,0}};
    PaintedMesh painted;
    CHECK_FALSE(face_colors_to_painting(mesh, painted, settings));
    settings.fixed_mapping_palette.clear();
    settings.fixed_palette = {{256,0,0}};
    CHECK_FALSE(face_colors_to_painting(mesh, painted, settings));
}

TEST_CASE("Explicit face colors survive old trial mapping centers without changing other regions", "[ModelVertexColors][FaceColorOverride]")
{
    const bool vertex_route = GENERATE(false, true);
    using Color = std::array<size_t, 3>;
    const Color red {255, 0, 0}, green {0, 255, 0}, blue {0, 0, 255};
    TexturedMesh mesh;
    mesh.vertices = {{0,0,0}, {10,0,0}, {0,10,0}, {0,0,10}};
    mesh.indices = {{0,2,1}, {0,1,3}, {0,3,2}, {1,2,3}};
    mesh.precomputed_face_colors.assign(4, blue);
    if (vertex_route)
        mesh.precomputed_vertex_colors.assign(4, {0,0,1,1});
    TexturePaintingSettings settings;
    settings.fixed_mapping_palette = {blue, red};
    settings.fixed_palette = {red, green};
    settings.smooth_weight = 1.;
    settings.face_color_overrides = {{0, blue}};
    PaintedMesh painted;
    REQUIRE(face_colors_to_painting(mesh, painted, settings));
    REQUIRE(painted.face_colors.size() == mesh.indices.size());
    CHECK(painted.face_colors[0] == blue);
    for (size_t face = 1; face < mesh.indices.size(); ++face)
        CHECK(painted.face_colors[face] == red);
    CHECK(painted.indices == mesh.indices);
    CHECK(painted.vertices == mesh.vertices);
    CHECK(std::set<Color>(painted.cluster_colors.begin(), painted.cluster_colors.end()) == std::set<Color>{red, blue});
    // Targets never join the source mapping centers or capture other regions.
    CHECK(settings.fixed_mapping_palette == std::vector<Color>{blue, red});
}

TEST_CASE("Textured GLB edits keep original face identities during color matching", "[ModelVertexColors][FaceColorOverride][TexturedOverride]")
{
    const auto fixture = GENERATE("textured.glb", "multi-material.glb");
    const auto model = Model::read_from_file(std::string(TEST_DATA_DIR) + "/model_artifact/" + fixture);
    REQUIRE(model.texture_mesh);
    const auto& source = *model.texture_mesh;
    REQUIRE_FALSE(source.textures.empty());
    REQUIRE_FALSE(source.indices.empty());
    REQUIRE(source.precomputed_face_colors.empty());
    const auto vertices = source.vertices;
    const auto indices = source.indices;
    TexturePaintingSettings settings;
    settings.fixed_palette = {{255, 0, 0}};
    settings.smooth_weight = 1.;
    settings.oversampling_iters = 2;
    settings.face_color_overrides = {{0, {0, 0, 255}}};
    settings.mesh_repair_decision = TexturePaintingSettings::MeshRepairDecision::RepairAndImport;
    bool repair_called = false;
    settings.mesh_repair_callback = [&](const indexed_triangle_set&, indexed_triangle_set&,
        std::function<void(const char*, unsigned)>, std::function<bool()>, std::string*) {
        repair_called = true;
        return false;
    };
    PaintedMesh painted;
    REQUIRE(texture_to_painting(source, painted, settings));
    CHECK_FALSE(repair_called);
    CHECK(painted.vertices == vertices);
    CHECK(painted.indices == indices);
    REQUIRE(painted.face_colors.size() == indices.size());
    CHECK(painted.face_colors[0] == std::array<size_t, 3>{0, 0, 255});
    for (size_t face = 1; face < indices.size(); ++face)
        CHECK(painted.face_colors[face] == std::array<size_t, 3>{255, 0, 0});
    CHECK(source.precomputed_face_colors.empty());
}

TEST_CASE("Invalid or cancelled texture edits preserve the previous painted result", "[ModelVertexColors][FaceColorOverride][TexturedOverride]")
{
    const auto model = Model::read_from_file(std::string(TEST_DATA_DIR) + "/model_artifact/textured.glb");
    REQUIRE(model.texture_mesh);
    const auto& source = *model.texture_mesh;
    const int failure = GENERATE(0, 1, 2);
    TexturePaintingSettings settings;
    settings.fixed_palette = {{255, 0, 0}};
    settings.face_color_overrides = {{failure == 0 ? source.indices.size() : 0,
        {0, 0, failure == 1 ? size_t{256} : size_t{255}}}};
    PaintedMesh previous;
    previous.face_colors = {{1, 2, 3}};
    CHECK_FALSE(texture_to_painting(source, previous, settings, nullptr, [failure] { return failure == 2; }));
    CHECK(previous.face_colors == std::vector<std::array<size_t, 3>>{{1, 2, 3}});
}

TEST_CASE("Explicit low-poly face colors retain conforming subdivision and untouched region colors", "[ModelVertexColors][FaceColorOverride]")
{
    using Color = std::array<size_t, 3>;
    TexturedMesh mesh;
    mesh.vertices = {{0,0,0}, {10,0,0}, {0,10,0}, {0,0,10}};
    mesh.indices = {{0,2,1}, {0,1,3}, {0,3,2}, {1,2,3}};
    mesh.precomputed_face_colors.assign(4, {128,128,128});
    mesh.precomputed_vertex_colors = {{1,0,0,1}, {0,1,0,1}, {0,0,1,1}, {1,1,1,1}};
    TexturePaintingSettings settings;
    settings.fixed_palette = {{255,0,0}, {0,255,0}, {0,0,255}, {255,255,255}};
    PaintedMesh baseline, edited;
    REQUIRE(face_colors_to_painting(mesh, baseline, settings));
    const Color target {102,140,182};
    settings.face_color_overrides = {{0, target}};
    REQUIRE(face_colors_to_painting(mesh, edited, settings));
    REQUIRE(edited.indices == baseline.indices);
    REQUIRE(edited.vertices == baseline.vertices);
    REQUIRE(edited.face_colors.size() == baseline.face_colors.size());
    size_t selected_children = 0;
    for (size_t face = 0; face < edited.indices.size(); ++face) {
        const auto& indices = edited.indices[face];
        const bool on_selected_plane = edited.vertices[indices[0]][2] == 0.f &&
            edited.vertices[indices[1]][2] == 0.f && edited.vertices[indices[2]][2] == 0.f;
        if (on_selected_plane) {
            ++selected_children;
            CHECK(edited.face_colors[face] == target);
        } else {
            CHECK(edited.face_colors[face] == baseline.face_colors[face]);
        }
    }
    CHECK(selected_children > 1);
    indexed_triangle_set output;
    for (const auto& vertex : edited.vertices) output.vertices.emplace_back(vertex[0], vertex[1], vertex[2]);
    for (const auto& face : edited.indices) output.indices.emplace_back(face[0], face[1], face[2]);
    CHECK(its_num_open_edges(output) == 0);
}

TEST_CASE("Face overrides preserve an open source surface without invoking repair or smoothing", "[ModelVertexColors][FaceColorOverride]")
{
    const bool fixed_palette = GENERATE(false, true);
    TexturedMesh mesh;
    mesh.vertices = {{0,0,0}, {10,0,0}, {0,10,0}};
    mesh.indices = {{0,1,2}};
    mesh.precomputed_face_colors = {{255,0,0}};
    TexturePaintingSettings settings;
    settings.target_colors_num = 1;
    settings.smooth_weight = 1.;
    if (fixed_palette) settings.fixed_palette = {{255,0,0}};
    settings.mesh_repair_decision = TexturePaintingSettings::MeshRepairDecision::RepairAndImport;
    bool repair_called = false;
    settings.mesh_repair_callback = [&](const indexed_triangle_set&, indexed_triangle_set&,
        std::function<void(const char*, unsigned)>, std::function<bool()>, std::string*) {
        repair_called = true;
        return false;
    };
    settings.face_color_overrides = {{0, {102,140,182}}};
    PaintedMesh painted;
    REQUIRE(face_colors_to_painting(mesh, painted, settings));
    CHECK_FALSE(repair_called);
    CHECK(painted.vertices == mesh.vertices);
    CHECK(painted.indices == mesh.indices);
    CHECK(painted.face_colors == std::vector<std::array<size_t,3>>{{102,140,182}});
}

TEST_CASE("Invalid explicit face colors fail before replacing a previous painted result", "[ModelVertexColors][FaceColorOverride]")
{
    const bool vertex_route = GENERATE(false, true);
    const bool invalid_index = GENERATE(false, true);
    TexturedMesh mesh;
    mesh.vertices = {{0,0,0}, {10,0,0}, {0,10,0}};
    mesh.indices = {{0,1,2}};
    mesh.precomputed_face_colors = {{255,0,0}};
    if (vertex_route) mesh.precomputed_vertex_colors.assign(3, {1,0,0,1});
    TexturePaintingSettings settings;
    settings.fixed_palette = {{255,0,0}};
    settings.face_color_overrides = invalid_index
        ? std::vector<std::pair<size_t,std::array<size_t,3>>>{{1,{0,0,255}}}
        : std::vector<std::pair<size_t,std::array<size_t,3>>>{{0,{0,0,256}}};
    PaintedMesh previous;
    previous.face_colors = {{1,2,3}};
    CHECK_FALSE(face_colors_to_painting(mesh, previous, settings));
    CHECK(previous.face_colors == std::vector<std::array<size_t,3>>{{1,2,3}});
}

TEST_CASE("Strict texture repair failure stops before clustering and never uses the internal fallback", "[ModelVertexColors][StrictMeshRepair]")
{
    const int callback_result = GENERATE(0, 1, 2); // failure, open result, verified closed result
    TexturedMesh mesh;
    mesh.vertices = {{0,0,0}, {10,0,0}, {0,10,0}, {0,0,10}};
    mesh.indices = {{0,2,1}, {0,1,3}, {0,3,2}, {1,2,3}};
    mesh.precomputed_face_colors = {{255,0,0}, {0,255,0}, {0,0,255}, {255,255,255}};
    TexturePaintingSettings settings;
    settings.target_colors_num = 4;
    settings.strict_mesh_repair = true;
    settings.mesh_repair_decision = TexturePaintingSettings::MeshRepairDecision::RepairAndImport;
    int repair_calls = 0;
    settings.mesh_repair_callback = [&](const indexed_triangle_set& source, indexed_triangle_set& repaired,
        std::function<void(const char*, unsigned)>, std::function<bool()>, std::string* error) {
        ++repair_calls;
        if (callback_result == 0) {
            if (error) *error = "injected strict repair failure";
            return false;
        }
        repaired = source;
        if (callback_result == 1) repaired.indices.pop_back();
        return true;
    };
    std::vector<std::string> events;
    PaintedMesh painted;
    const bool success = face_colors_to_painting(mesh, painted, settings,
        [&](int, const char* message) { events.emplace_back(message ? message : ""); });
    CHECK(success == (callback_result == 2));
    CHECK(repair_calls == 1);
    const bool clustered = std::find(events.begin(), events.end(), "Color clustering") != events.end();
    const bool smoothed = std::find(events.begin(), events.end(), "Smoothing colors") != events.end();
    CHECK(clustered == success);
    CHECK(smoothed == success);
    if (!success) CHECK(painted.indices.empty());
}

TEST_CASE("Explicit face target colors resolve against current physical slot order", "[ModelVertexColors][FaceColorOverride]")
{
    const std::array<size_t, 3> blue {102,140,182};
    std::vector<RGBA> physical {{1,0,0,1}, {0,0,0,1}, {1,1,1,1}, {0,1,0,1},
        {102.f/255.f,140.f/255.f,182.f/255.f,1}, {0.5f,0.5f,0.5f,1}};
    const auto original = match_clusters_to_filaments({blue}, physical, {});
    REQUIRE(original.size() == 1);
    CHECK(original[0].filament_index == 4);
    std::swap(physical[1], physical[4]);
    const auto reordered = match_clusters_to_filaments({blue}, physical, {});
    REQUIRE(reordered.size() == 1);
    CHECK(reordered[0].filament_index == 1);
    CHECK(physical.size() == 6);
}

TEST_CASE("Vertex color boundary splits preserve closed surfaces and color regions", "[ModelVertexColors][ColorBoundary]")
{
    const int color_count = GENERATE(1, 2, 3, 4);
    const int face_rotation = GENERATE(0, 1, 2);
    CAPTURE(color_count, face_rotation);
    using Color = std::array<size_t, 3>;
    const std::array<Color, 4> source_colors = {{{255,0,0}, {0,255,0}, {0,0,255}, {255,255,255}}};
    const std::array<Color, 4> target_colors = {{{20,30,40}, {50,100,150}, {160,170,180}, {200,210,220}}};
    TexturedMesh textured;
    textured.vertices = {{0,0,0}, {60,0,0}, {0,0,100}, {0,40,0}};
    textured.indices = {{0,2,1}, {0,1,3}, {1,2,3}, {2,0,3}};
    for (auto& face : textured.indices) {
        const auto original = face;
        for (int corner = 0; corner < 3; ++corner)
            face[corner] = original[(corner + face_rotation) % 3];
    }
    for (size_t i = 0; i < textured.vertices.size(); ++i) {
        const auto& color = source_colors[i % color_count];
        textured.precomputed_vertex_colors.push_back({color[0] / 255.f, color[1] / 255.f, color[2] / 255.f, 1.f});
    }
    textured.precomputed_face_colors.assign(textured.indices.size(), {128,128,128});
    TexturePaintingSettings settings;
    settings.smooth_weight = 0;
    settings.fixed_mapping_palette.assign(source_colors.begin(), source_colors.begin() + color_count);
    settings.fixed_palette.assign(target_colors.begin(), target_colors.begin() + color_count);

    indexed_triangle_set original;
    for (const auto& v : textured.vertices)
        original.vertices.emplace_back(v[0], v[1], v[2]);
    for (const auto& f : textured.indices)
        original.indices.emplace_back(f[0], f[1], f[2]);
    REQUIRE(its_num_open_edges(original) == 0);
    PaintedMesh painted;
    REQUIRE(face_colors_to_painting(textured, painted, settings));
    REQUIRE(painted.face_colors.size() == painted.indices.size());
    indexed_triangle_set result;
    for (const auto& v : painted.vertices)
        result.vertices.emplace_back(v[0], v[1], v[2]);
    for (const auto& f : painted.indices) {
        for (int index : f) {
            REQUIRE(index >= 0);
            REQUIRE(size_t(index) < result.vertices.size());
        }
        result.indices.emplace_back(f[0], f[1], f[2]);
    }
    CHECK(its_num_open_edges(result) == 0);
    CHECK_THAT(its_volume(result), Catch::Matchers::WithinRel(its_volume(original), 1e-6f));
    std::array<bool, 4> retained_corners {};
    std::map<Color, double> base_color_areas;
    for (size_t fi = 0; fi < result.indices.size(); ++fi) {
        const auto& face = result.indices[fi];
        const Vec3f& a = result.vertices[face[0]];
        const Vec3f& b = result.vertices[face[1]];
        const Vec3f& c = result.vertices[face[2]];
        const double area = (b - a).cross(c - a).norm() / 2.;
        CHECK(area > 0.);
        for (size_t vi = 0; vi < original.vertices.size(); ++vi) {
            if (a.isApprox(original.vertices[vi]) || b.isApprox(original.vertices[vi]) || c.isApprox(original.vertices[vi])) {
                retained_corners[vi] = true;
                CHECK(painted.face_colors[fi] == target_colors[vi % color_count]);
            }
        }
        if (std::abs(a.y()) < 1e-6f && std::abs(b.y()) < 1e-6f && std::abs(c.y()) < 1e-6f)
            base_color_areas[painted.face_colors[fi]] += area;
    }
    for (bool retained : retained_corners)
        CHECK(retained);
    if (color_count == 4) {
        // The y=0 face has its widest angle at the origin. The centre cut
        // leaves that corner a quarter of the area and gives each other color
        // three eighths, regardless of the face's cyclic index order.
        const double base_area = 60. * 100. / 2.;
        CHECK_THAT(base_color_areas[target_colors[0]], Catch::Matchers::WithinRel(base_area / 4., 1e-6));
        CHECK_THAT(base_color_areas[target_colors[1]], Catch::Matchers::WithinRel(base_area * 3. / 8., 1e-6));
        CHECK_THAT(base_color_areas[target_colors[2]], Catch::Matchers::WithinRel(base_area * 3. / 8., 1e-6));
    }

    Model model;
    ModelVolume* volume = model.add_object()->add_volume(TriangleMesh(original), ModelVolumeType::MODEL_PART, false);
    std::vector<FilamentMatch> matches;
    for (size_t i = 0; i < painted.cluster_colors.size(); ++i) {
        FilamentMatch match;
        match.cluster_index = int(i);
        match.filament_index = int(i % 3);
        matches.push_back(match);
    }
    REQUIRE(apply_painted_mesh_to_volume(painted, matches, *volume));
    CHECK(its_num_open_edges(volume->mesh().its) == 0);
    CHECK_THAT(its_volume(volume->mesh().its), Catch::Matchers::WithinRel(its_volume(original), 1e-6f));
    for (const auto& match : matches) {
        const auto state = static_cast<EnforcerBlockerType>(static_cast<int>(EnforcerBlockerType::Extruder1) + match.filament_index);
        CHECK(volume->mmu_segmentation_facets.has_facets(*volume, state));
    }
}

namespace {

Model make_single_triangle_model()
{
    indexed_triangle_set its;
    its.vertices = {{0.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}, {0.0f, 10.0f, 0.0f}};
    its.indices = {{0, 1, 2}};

    Model model;
    ModelObject* object = model.add_object();
    object->add_volume(TriangleMesh(std::move(its)), ModelVolumeType::MODEL_PART, false);
    return model;
}

void write_colored_tetrahedra(const std::string& path, int color_count)
{
    std::ofstream out(path);
    out << std::setprecision(9);
    const std::array<std::array<int, 3>, 6> colors = {{{217, 107, 67}, {43, 36, 34}, {242, 215, 181},
                                                     {47, 107, 95}, {50, 103, 168}, {155, 63, 119}}};
    const std::array<std::array<int, 3>, 4> vertices = {{{0, 0, 0}, {10, 0, 0}, {0, 10, 0}, {0, 0, 10}}};
    for (int c = 0; c < color_count; ++c) {
        for (const auto& v : vertices)
            out << "v " << v[0] + 12 * c << ' ' << v[1] << ' ' << v[2] << ' '
                << colors[c][0] / 255.f << ' ' << colors[c][1] / 255.f << ' ' << colors[c][2] / 255.f << '\n';
        const int start = 4 * c + 1;
        for (const auto& f : std::array<std::array<int, 3>, 4>{{{0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 2, 3}}})
            out << "f " << start + f[0] << ' ' << start + f[1] << ' ' << start + f[2] << '\n';
    }
    REQUIRE(out.good());
}

Model read_with_colors(const std::string& path, ObjImportColorFn callback)
{
    return Model::read_from_file(path, nullptr, nullptr, LoadStrategy::LoadModel, nullptr, nullptr,
                                 nullptr, nullptr, nullptr, nullptr, nullptr, 0, std::move(callback));
}

} // namespace

TEST_CASE("OBJ vertex colors retain non-base filament facets", "[Model][OBJ][MMU]")
{
    Model model = make_single_triangle_model();
    ModelVolume* volume = model.objects.front()->volumes.front();

    REQUIRE(Model::obj_import_vertex_color_deal({1, 1, 1}, 1, &model));
    CHECK(volume->mmu_segmentation_facets.empty());

    REQUIRE(Model::obj_import_vertex_color_deal({1, 1, 2}, 1, &model));
    CHECK_FALSE(volume->mmu_segmentation_facets.empty());
    CHECK_FALSE(volume->mmu_segmentation_facets.get_triangle_as_string(0).empty());
}

TEST_CASE("Explicit OBJ color callbacks preserve one through six material regions", "[ModelVertexColors][OBJ]")
{
    const int color_count = GENERATE(1, 2, 3, 4, 5, 6);
    ScopedTemporaryFile file(".obj");
    write_colored_tetrahedra(file.string(), color_count);
    int calls = 0;
    bool applied = false;
    Model model = read_with_colors(file.string(), [&](ObjDialogInOut& in_out) {
        ++calls;
        REQUIRE(in_out.deal_vertex_color);
        REQUIRE(in_out.model != nullptr);
        REQUIRE(in_out.input_colors.size() == size_t(4 * color_count));
        CHECK(std::set<RGBA>(in_out.input_colors.begin(), in_out.input_colors.end()).size() == size_t(color_count));
        for (int i = 0; i < 4 * color_count; ++i)
            in_out.filament_ids.push_back(static_cast<unsigned char>(i / 4 + 1));
        in_out.first_extruder_id = 1;
        applied = Model::obj_import_vertex_color_deal(in_out.filament_ids, in_out.first_extruder_id, in_out.model);
    });
    REQUIRE(calls == 1);
    REQUIRE(applied);
    REQUIRE_FALSE(model.texture_mesh);
    REQUIRE(model.objects.size() == 1);
    const auto* volume = model.objects.front()->volumes.front();
    CHECK(volume->config.extruder() == 1);
    CHECK(volume->mesh().facets_count() == size_t(4 * color_count));
    for (int c = 1; c < color_count; ++c)
        CHECK(volume->mmu_segmentation_facets.has_facets(*volume, EnforcerBlockerType(c + 1)));
}

TEST_CASE("Default OBJ loading retains the native texture import path", "[ModelVertexColors][OBJ]")
{
    const int color_count = GENERATE(1, 2, 3, 4, 5, 6);
    ScopedTemporaryFile file(".obj");
    write_colored_tetrahedra(file.string(), color_count);
    Model model = read_with_colors(file.string(), nullptr);
    REQUIRE(model.texture_mesh);
    CHECK(model.texture_mesh->precomputed_vertex_colors.size() == size_t(4 * color_count));
    CHECK(model.texture_mesh->precomputed_face_colors.size() == size_t(4 * color_count));
    CHECK(model.objects.front()->volumes.front()->mmu_segmentation_facets.empty());
}

TEST_CASE("Explicit geometry-only OBJ loading does not invoke texture matching", "[ModelVertexColors][OBJ]")
{
    ScopedTemporaryFile file(".obj");
    write_colored_tetrahedra(file.string(), 6);
    int calls = 0;
    Model model = read_with_colors(file.string(), [&](ObjDialogInOut&) { ++calls; });
    REQUIRE(calls == 1);
    REQUIRE_FALSE(model.texture_mesh);
    REQUIRE(model.objects.size() == 1);
    CHECK(model.objects.front()->volumes.front()->mmu_segmentation_facets.empty());
}

TEST_CASE("Cancelling an explicit OBJ color callback leaves no imported objects", "[ModelVertexColors][OBJ]")
{
    ScopedTemporaryFile file(".obj");
    write_colored_tetrahedra(file.string(), 6);
    int calls = 0;
    Model model = read_with_colors(file.string(), [&](ObjDialogInOut& in_out) {
        ++calls;
        in_out.cancelled = true;
    });
    REQUIRE(calls == 1);
    CHECK(model.objects.empty());
    CHECK_FALSE(model.texture_mesh);
}

TEST_CASE("Explicit OBJ callbacks apply material face colors", "[ModelVertexColors][OBJ]")
{
    ScopedTemporaryDir dir;
    const auto obj = dir.path() / "face-colors.obj";
    {
        std::ofstream mtl((dir.path() / "face-colors.mtl").string());
        mtl << "newmtl red\nKa 0 0 0\nKd 1 0 0\nnewmtl blue\nKa 0 0 0\nKd 0 0 1\n";
        std::ofstream out(obj.string());
        out << "mtllib face-colors.mtl\nv 0 0 0\nv 10 0 0\nv 0 10 0\nv 0 0 10\n"
               "usemtl red\nf 1 3 2\nf 1 2 4\nusemtl blue\nf 1 4 3\nf 2 3 4\n";
    }
    int calls = 0;
    bool applied = false;
    Model model = read_with_colors(obj.string(), [&](ObjDialogInOut& in_out) {
        ++calls;
        REQUIRE_FALSE(in_out.deal_vertex_color);
        REQUIRE(in_out.input_colors.size() == 4);
        in_out.filament_ids = {1, 1, 6, 6};
        in_out.first_extruder_id = 1;
        applied = Model::obj_import_face_color_deal(in_out.filament_ids, in_out.first_extruder_id, in_out.model);
    });
    REQUIRE(calls == 1);
    REQUIRE(applied);
    REQUIRE_FALSE(model.texture_mesh);
    const auto* volume = model.objects.front()->volumes.front();
    CHECK(volume->mmu_segmentation_facets.has_facets(*volume, EnforcerBlockerType::Extruder6));
}

TEST_CASE("Preserved OBJ palettes keep nearby RGB colors and stable labels", "[ModelVertexColors][OBJ]")
{
    const int color_count = GENERATE(1, 2, 3, 4, 5, 6);
    const char requested = GENERATE(char(-1), char(6));
    std::vector<RGBA> input;
    for (int i = 0; i < 12; ++i)
        input.push_back({(90 + i % color_count) / 255.f, 100 / 255.f, 120 / 255.f, 1.f});
    std::vector<RGBA> palette;
    std::vector<int> labels;
    char count = requested;
    obj_color_deal_algo(input, palette, labels, count, 32, true);
    REQUIRE(palette.size() == size_t(color_count));
    REQUIRE(labels.size() == input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        REQUIRE(labels[i] == int(i % color_count));
        for (size_t c = 0; c < 4; ++c)
            CHECK_THAT(palette[labels[i]][c], Catch::Matchers::WithinAbs(input[i][c], 1e-7));
    }
}

TEST_CASE("An explicit smaller OBJ color count still permits quantization", "[ModelVertexColors][OBJ]")
{
    const bool preserve = GENERATE(false, true);
    std::vector<RGBA> input = {{1.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 1.f, 1.f}};
    std::vector<RGBA> palette;
    std::vector<int> labels;
    char count = 1;
    obj_color_deal_algo(input, palette, labels, count, 32, preserve);
    REQUIRE(palette.size() == 1);
    CHECK(labels == std::vector<int>{0, 0});
}


namespace {
Slic3r::tex2color::TriMesh smoothing_torus()
{
    Slic3r::tex2color::TriMesh mesh;
    constexpr int major_steps = 16, minor_steps = 384;
    constexpr double tau = 6.283185307179586476925286766559;
    for (int major = 0; major < major_steps; ++major) {
        const double u = tau * major / major_steps;
        for (int minor = 0; minor < minor_steps; ++minor) {
            const double v = tau * minor / minor_steps;
            const double radius = 10. + 2. * std::cos(v);
            mesh.vertices.emplace_back(float(radius * std::cos(u)), float(radius * std::sin(u)), float(2. * std::sin(v)));
        }
    }
    const auto vertex = [](int major, int minor) { return (major % major_steps) * minor_steps + minor % minor_steps; };
    for (int major = 0; major < major_steps; ++major)
        for (int minor = 0; minor < minor_steps; ++minor) {
            const auto a = vertex(major, minor), b = vertex(major + 1, minor);
            const auto c = vertex(major + 1, minor + 1), d = vertex(major, minor + 1);
            mesh.indices.emplace_back(a, b, c);
            mesh.indices.emplace_back(a, c, d);
        }
    return mesh;
}

void require_identical_smoothing_mesh(const Slic3r::tex2color::TriMesh& actual,
                                     const Slic3r::tex2color::TriMesh& expected)
{
    REQUIRE(actual.vertices.size() == expected.vertices.size());
    REQUIRE(actual.indices == expected.indices);
    for (std::size_t vertex = 0; vertex < expected.vertices.size(); ++vertex)
        REQUIRE(std::memcmp(actual.vertices[vertex].data(), expected.vertices[vertex].data(), 3 * sizeof(float)) == 0);
}
}

TEST_CASE("Rejected color topologies preserve the final ordered smoothing result", "[ModelVertexColors][RegionSmoothing]")
{
    namespace colors = Slic3r::tex2color::color_utils;
    const auto input = smoothing_torus();
    std::vector<std::size_t> labels(input.indices.size());
    for (std::size_t face = 0; face < labels.size(); ++face) labels[face] = face < labels.size() / 2 ? 0 : 1;
    auto reference = input;
    auto reference_labels = labels;
    REQUIRE(colors::smooth_region(reference, reference_labels));

    std::map<std::string, std::size_t> stages;
    colors::SmoothParameters parameters;
    parameters.diagnostic_callback = [&](const char* stage, std::uint64_t, std::size_t) { ++stages[stage]; };
    colors::RegionSmoothingWorkspace workspace(input, parameters);
    auto rejected = labels;
    for (auto& label : rejected) label = 0;
    REQUIRE(workspace.apply_topology(rejected, parameters));
    REQUIRE(workspace.apply_topology(labels, parameters));
    CHECK(stages["prepare"] == 1);
    CHECK(stages["topology"] == 2);
    CHECK(stages["projection"] == 0);
    auto result = input;
    REQUIRE(workspace.finish(result, labels, parameters));
    CHECK(stages["projection"] == 1);
    CHECK(stages["export"] == 1);
    CHECK(labels == reference_labels);
    require_identical_smoothing_mesh(result, reference);
    CHECK_FALSE(workspace.finish(result, labels, parameters));
    require_identical_smoothing_mesh(result, reference);
}

TEST_CASE("Canceled geometric smoothing never installs a partially modified mesh", "[ModelVertexColors][RegionSmoothing]")
{
    namespace colors = Slic3r::tex2color::color_utils;
    const auto input = smoothing_torus();
    std::vector<std::size_t> labels(input.indices.size());
    for (std::size_t face = 0; face < labels.size(); ++face) labels[face] = face < labels.size() / 2 ? 0 : 1;
    bool canceled = false;
    colors::SmoothParameters parameters;
    parameters.cancel_callback = [&] { return canceled; };
    parameters.diagnostic_callback = [&](const char* stage, std::uint64_t, std::size_t) {
        if (std::string(stage) == "projection-index") canceled = true;
    };
    colors::RegionSmoothingWorkspace workspace(input, parameters);
    REQUIRE(workspace.apply_topology(labels, parameters));
    auto result = input;
    CHECK_FALSE(workspace.finish(result, labels, parameters));
    require_identical_smoothing_mesh(result, input);
    canceled = false;
    CHECK_FALSE(workspace.finish(result, labels, parameters));
    require_identical_smoothing_mesh(result, input);
}

TEST_CASE("Smoothing diagnostics cannot fail a valid model", "[ModelVertexColors][RegionSmoothing]")
{
    namespace colors = Slic3r::tex2color::color_utils;
    auto input = smoothing_torus();
    std::vector<std::size_t> labels(input.indices.size(), 0);
    colors::SmoothParameters parameters;
    parameters.diagnostic_callback = [](const char*, std::uint64_t, std::size_t) {
        throw std::runtime_error("diagnostic receiver unavailable");
    };
    REQUIRE(colors::smooth_region(input, labels, parameters));
    CHECK(input.indices.size() == labels.size());
}

TEST_CASE("Parallel boundary paths preserve serial coordinates at shared endpoints", "[ModelVertexColors][RegionSmoothing]")
{
    namespace colors = Slic3r::tex2color::color_utils;
    const auto input = smoothing_torus();
    const auto pattern = GENERATE(0, 1, 2);
    std::vector<std::size_t> labels(input.indices.size());
    for (std::size_t face = 0; face < labels.size(); ++face) {
        if (pattern == 0) labels[face] = ((face / 2 / 384) + (face / 2 % 384) / 12) % 3;
        else if (pattern == 1) labels[face] = (face / 2 % 384) / 7 % 4;
        else labels[face] = face / 19 % 5;
    }
    colors::SmoothParameters parameters;
    parameters.smooth_weight = .2; // Keep the branched label boundaries intact.
    const auto caller = std::this_thread::get_id();
    std::atomic<bool> callback_on_worker {false};
    parameters.cancel_callback = [&] {
        if (std::this_thread::get_id() != caller) callback_on_worker.store(true);
        return false;
    };
    auto serial_mesh = input, parallel_mesh = input;
    auto serial_labels = labels, parallel_labels = labels;
    tbb::task_arena serial(1), parallel(4);
    REQUIRE(serial.execute([&] { return colors::smooth_region(serial_mesh, serial_labels, parameters); }));
    REQUIRE(parallel.execute([&] { return colors::smooth_region(parallel_mesh, parallel_labels, parameters); }));
    CHECK_FALSE(callback_on_worker.load());
    CHECK(serial_labels == parallel_labels);
    require_identical_smoothing_mesh(parallel_mesh, serial_mesh);
}

TEST_CASE("Canceling a prepared projection batch leaves the source mesh unchanged", "[ModelVertexColors][RegionSmoothing]")
{
    namespace colors = Slic3r::tex2color::color_utils;
    auto input = smoothing_torus();
    const auto original = input;
    std::vector<std::size_t> labels(input.indices.size());
    for (std::size_t face = 0; face < labels.size(); ++face) labels[face] = face / 19 % 5;
    bool canceled = false;
    colors::SmoothParameters parameters;
    parameters.smooth_weight = .2;
    parameters.cancel_callback = [&] { return canceled; };
    parameters.diagnostic_callback = [&](const char* stage, std::uint64_t, std::size_t) {
        if (std::string(stage) == "projection-plan") canceled = true;
    };
    CHECK_FALSE(colors::smooth_region(input, labels, parameters));
    require_identical_smoothing_mesh(input, original);
}

TEST_CASE("Cancellation before smoothing invalidates the request workspace", "[ModelVertexColors][RegionSmoothing]")
{
    namespace colors = Slic3r::tex2color::color_utils;
    const auto input = smoothing_torus();
    std::vector<std::size_t> labels(input.indices.size(), 0);
    bool canceled = false;
    colors::SmoothParameters parameters;
    parameters.cancel_callback = [&] { return canceled; };
    colors::RegionSmoothingWorkspace workspace(input, parameters);
    canceled = true;
    const bool cancel_topology = GENERATE(false, true);
    auto result = input;
    if (cancel_topology) CHECK_FALSE(workspace.apply_topology(labels, parameters));
    else CHECK_FALSE(workspace.finish(result, labels, parameters));
    canceled = false;
    CHECK_FALSE(workspace.finish(result, labels, parameters));
    require_identical_smoothing_mesh(result, input);
}

TEST_CASE("Color clustering retries keep strict repair single and reject insufficient regions", "[ModelVertexColors][RegionSmoothing]")
{
    TexturedMesh source;
    source.vertices = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    source.indices = {{0,2,4},{2,1,4},{1,3,4},{3,0,4},{2,0,5},{1,2,5},{3,1,5},{0,3,5}};
    source.precomputed_face_colors = {{230,70,60},{45,45,45},{235,220,190},{50,100,210},
        {210,70,150},{70,180,90},{230,70,60},{45,45,45}};
    const auto immutable = source;
    TexturePaintingSettings settings;
    settings.target_colors_num = 0;
    settings.min_cluster_k = 6;
    settings.max_cluster_k = 32;
    settings.max_color_distance = 10.;
    settings.max_tail_color_distance = 18.;
    settings.strict_mesh_repair = true;
    settings.mesh_repair_decision = TexturePaintingSettings::MeshRepairDecision::RepairAndImport;
    int repairs = 0, topologies = 0;
    settings.mesh_repair_callback = [&](const indexed_triangle_set& input, indexed_triangle_set& output,
        std::function<void(const char*, unsigned)>, std::function<bool()>, std::string*) {
        ++repairs;
        output = input;
        return true;
    };
    PaintedMesh output;
    const auto progress = [&](int, const char* stage) {
        if (std::string(stage) == "Smoothing colors") ++topologies;
    };
    CHECK_FALSE(face_colors_to_painting(source, output, settings, progress));
    CHECK(repairs == 1);
    CHECK(topologies > 1);
    CHECK(source.vertices == immutable.vertices);
    CHECK(source.indices == immutable.indices);
    CHECK(source.precomputed_face_colors == immutable.precomputed_face_colors);
}

namespace {
using Slic3r::tex2color::TriMesh;
using Slic3r::tex2color::TriFace;
bool legacy_halfedge_compatible(const TriMesh& mesh) {
    std::vector<std::unordered_set<std::size_t>> vtx_to_adj_faces(mesh.vertices.size());
    std::size_t edge_id = 0;
    std::vector<std::unordered_set<std::size_t>> edge_to_faces;
    std::vector<std::unordered_set<std::size_t>> vtx_to_prev_vtxs(mesh.vertices.size());
    std::vector<std::unordered_set<std::size_t>> vtx_to_next_vtxs(mesh.vertices.size());
    std::vector<std::unordered_map<std::size_t, std::size_t>> vtx_vtx_to_edge(mesh.vertices.size());

    for (std::size_t fid = 0; fid < mesh.indices.size(); ++fid) {
        const TriFace& face = mesh.indices[fid];
        if (face[0] == face[1] || face[1] == face[2] || face[2] == face[0]) {
            return false;
        }
        for (std::size_t i = 0; i < 3; ++i) {
            if (static_cast<std::size_t>(face[i]) >= mesh.vertices.size()) {
                return false;
            }
            vtx_to_adj_faces[face[i]].insert(fid);

            std::size_t prev_vtx = face[(i + 2) % 3];
            std::size_t next_vtx = face[(i + 1) % 3];

            if (vtx_to_prev_vtxs[face[i]].count(prev_vtx)) {
                return false;
            }
            vtx_to_prev_vtxs[face[i]].insert(prev_vtx);

            if (vtx_to_next_vtxs[face[i]].count(next_vtx)) {
                return false;
            }
            vtx_to_next_vtxs[face[i]].insert(next_vtx);
        }

        for (std::size_t i = 0; i < 3; ++i) {
            std::size_t va = face[i];
            std::size_t vb = face[(i + 1) % 3];
            if (!vtx_vtx_to_edge[va].count(vb)) {
                vtx_vtx_to_edge[va][vb] = edge_id;
                vtx_vtx_to_edge[vb][va] = edge_id;
                ++edge_id;
                edge_to_faces.emplace_back(std::unordered_set<std::size_t>());
            }
            edge_to_faces[vtx_vtx_to_edge[va][vb]].insert(fid);
        }
    }

    for (std::size_t vid = 0; vid < mesh.vertices.size(); ++vid) {
        if (vtx_to_adj_faces[vid].empty()) {
            continue;
        }
        std::unordered_set<std::size_t> visited_faces;
        std::queue<std::size_t> face_queue;
        face_queue.push(*(vtx_to_adj_faces[vid].begin()));
        visited_faces.insert(*(vtx_to_adj_faces[vid].begin()));
        while (!face_queue.empty()) {
            std::size_t fid = face_queue.front();
            face_queue.pop();
            const TriFace& face = mesh.indices[fid];
            for (std::size_t i = 0; i < 3; ++i) {
                if (static_cast<std::size_t>(face[i]) != vid) {
                    continue;
                }
                std::size_t v_next = face[(i + 1) % 3];
                std::size_t v_prev = face[(i + 2) % 3];
                for (std::size_t nbr : {v_next, v_prev}) {
                    std::size_t eid = vtx_vtx_to_edge[vid][nbr];
                    for (std::size_t adj_fid : edge_to_faces[eid]) {
                        if (!visited_faces.count(adj_fid) && vtx_to_adj_faces[vid].count(adj_fid)) {
                            visited_faces.insert(adj_fid);
                            face_queue.push(adj_fid);
                        }
                    }
                }
                break;
            }
        }

        for (std::size_t fid : vtx_to_adj_faces[vid]) {
            if (!visited_faces.count(fid)) {
                return false;
            }
        }
    }

    return true;
}

}

TEST_CASE("Projection hints preserve original equidistant surface selection", "[ModelVertexColors][RegionSmoothing]")
{
    namespace geometry = Slic3r::tex2color::cgalutils;
    using Point = geometry::Kernel::Point_3;
    using Primitive = CGAL::AABB_face_graph_triangle_primitive<geometry::CGALMesh>;
    using Tree = CGAL::AABB_tree<CGAL::AABB_traits_3<geometry::Kernel, Primitive>>;
    geometry::CGALMesh mesh;
    for (const double x : {-1., 1.}) {
        const auto a = mesh.add_vertex(Point(x,-2.,-2.));
        const auto b = mesh.add_vertex(Point(x, 2.,-2.));
        const auto c = mesh.add_vertex(Point(x, 2., 2.));
        const auto d = mesh.add_vertex(Point(x,-2., 2.));
        REQUIRE(mesh.add_face(a,b,c) != geometry::CGALMesh::null_face());
        REQUIRE(mesh.add_face(a,c,d) != geometry::CGALMesh::null_face());
    }
    Tree tree(mesh.faces().begin(), mesh.faces().end(), mesh);
    tree.do_not_accelerate_distance_queries();
    tree.build();
    // Mid-plane ties, unique minima, near-zero and on-surface projections.
    for (const double x : {0., -0.25, 0.25, -1., 1., 1. + 1e-15})
        for (const double y : {0., -0.5, 0.75})
            for (const double z : {0., -0.75, 0.5}) {
                const Point query(x,y,z);
                const auto expected = tree.closest_point(query);
                for (const auto& hint : {Point(-1.,y,z), Point(1.,y,z), Point(-1.,-2.,-2.), Point(1.,2.,2.)}) {
                    const auto actual = geometry::closest_point_with_strict_hint(tree, query, hint);
                    const std::array<double,3> expected_values {expected.x(),expected.y(),expected.z()};
                    const std::array<double,3> actual_values {actual.x(),actual.y(),actual.z()};
                    CHECK(std::memcmp(expected_values.data(), actual_values.data(), sizeof(expected_values)) == 0);
                }
            }
}

TEST_CASE("Clustering seeds preserve serial point order across worker limits", "[ModelVertexColors][ClusteringSeeds]")
{
    namespace colors = Slic3r::tex2color::color_utils;
    // Frozen pre-optimization FPS oracle. The ordered choices, including the
    // first equal-distance point, are part of the appearance contract.
    const auto serial_seeds = [](std::vector<colors::Color> input, std::size_t k) {
        if (input.empty() || k == 0) return std::vector<colors::Color>{};
        std::sort(input.begin(), input.end());
        input.erase(std::unique(input.begin(), input.end()), input.end());
        if (input.size() < k) return input;
        std::vector<colors::ColorDouble> points;
        colors::ColorDouble centroid {0.,0.,0.};
        for (const auto& color : input) {
            points.push_back({double(color[0]),double(color[1]),double(color[2])});
            for (std::size_t channel = 0; channel < 3; ++channel)
                centroid[channel] += points.back()[channel];
        }
        for (auto& channel : centroid) channel /= double(points.size());
        double nearest = std::numeric_limits<double>::max();
        std::size_t first = 0;
        for (std::size_t i = 0; i < points.size(); ++i) {
            const double distance = colors::calc_rgb_color_difference_by_squared_rgb_double(points[i], centroid);
            if (distance < nearest) { nearest = distance; first = i; }
        }
        std::vector<colors::Color> result {input[first]};
        std::vector<double> distances(points.size(), std::numeric_limits<double>::max());
        for (std::size_t center = 1; center < k; ++center) {
            const auto& selected = result.back();
            const colors::ColorDouble last {double(selected[0]),double(selected[1]),double(selected[2])};
            double farthest = -1.;
            std::size_t index = 0;
            for (std::size_t i = 0; i < points.size(); ++i) {
                const double distance = colors::calc_rgb_color_difference_by_squared_rgb_double(points[i], last);
                if (distance < distances[i]) distances[i] = distance;
                if (distances[i] > farthest) { farthest = distances[i]; index = i; }
            }
            result.push_back(input[index]);
        }
        return result;
    };
    std::mt19937 random(715019);
    std::vector<std::vector<colors::Color>> inputs {{}, {{17,17,17}}, {{0,0,0},{0,0,0},{255,255,255}}};
    // Symmetric lattice exercises ties and the parallel-grain boundary.
    for (const unsigned side : {4u, 8u}) {
        std::vector<colors::Color> lattice;
        for (unsigned r = 0; r < side; ++r)
            for (unsigned g = 0; g < side; ++g)
                for (unsigned b = 0; b < side; ++b)
                    lattice.push_back({r*255/(side-1),g*255/(side-1),b*255/(side-1)});
        inputs.push_back(std::move(lattice));
    }
    for (const unsigned count : {255u, 256u, 257u, 4096u}) {
        std::vector<colors::Color> points;
        for (unsigned i = 0; i < count; ++i)
            points.push_back({random()%256,random()%256,random()%256});
        const std::vector<colors::Color> duplicates(points.begin(), points.begin() + std::min<std::size_t>(16, points.size()));
        points.insert(points.end(), duplicates.begin(), duplicates.end());
        std::shuffle(points.begin(), points.end(), random);
        inputs.push_back(std::move(points));
    }
    for (const auto& input : inputs)
        for (const unsigned k : {0u,1u,2u,6u,16u,32u}) {
            colors::ClusterParameters parameters;
            parameters.color_difference_method = colors::ColorDifferenceMethod::RGB;
            parameters.cluster_k = k;
            parameters.max_iter = 0; // Exercise FPS without later center updates.
            const auto expected = serial_seeds(input, k);
            for (const int threads : {1,2,4}) {
                CAPTURE(input.size(), k, threads);
                tbb::task_arena arena(threads);
                const auto actual = arena.execute([&] { return colors::cluster_k_means(input, parameters); });
                CHECK(actual == expected);
            }
        }
}

namespace {
namespace frozen_clustering {
namespace colors = Slic3r::tex2color::color_utils;
using Point = colors::ColorDouble;
using Color = colors::Color;
using Parameters = colors::ClusterParameters;
using Distance = double (*)(const Point&, const Point&);

// Frozen from the clustering-before immutable source snapshot (2026-11-14).
// This deliberately does not call production FPS, kmeans or adaptive helpers.
// Only the unchanged scalar distance formula is shared. Ordered accumulation,
// mixed old/new centers in empty-cluster recovery, and cancellation checkpoints
// are retained so the oracle can reject a plausible but different algorithm.
Point rgb_to_lab(const Color& color)
{
    const auto gamma = [](double value) {
        value /= 255.0;
        return value > 0.04045 ? std::pow((value + 0.055) / 1.055, 2.4) : value / 12.92;
    };
    const double r = gamma(double(color[0])), g = gamma(double(color[1])), b = gamma(double(color[2]));
    const Point xyz {r * 0.4124564 + g * 0.3575761 + b * 0.1804375,
                     r * 0.2126729 + g * 0.7151522 + b * 0.0721750,
                     r * 0.0193339 + g * 0.1191920 + b * 0.9503041};
    const auto f = [](double value) {
        const double delta = 6.0 / 29.0;
        return value > delta * delta * delta ? std::cbrt(value) : value / (3.0 * delta * delta) + 4.0 / 29.0;
    };
    const double fx = f(xyz[0] / 0.95047), fy = f(xyz[1] / 1.0), fz = f(xyz[2] / 1.08883);
    return {116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
}

Color lab_to_rgb(const Point& lab)
{
    const double delta = 6.0 / 29.0, delta2x3 = 3.0 * delta * delta;
    const double fy = (lab[0] + 16.0) / 116.0, fx = lab[1] / 500.0 + fy, fz = fy - lab[2] / 200.0;
    const double x = 0.95047 * (fx > delta ? fx * fx * fx : delta2x3 * (fx - 4.0 / 29.0));
    const double y = 1.0 * (fy > delta ? fy * fy * fy : delta2x3 * (fy - 4.0 / 29.0));
    const double z = 1.08883 * (fz > delta ? fz * fz * fz : delta2x3 * (fz - 4.0 / 29.0));
    const double r = 3.2404542 * x - 1.5371385 * y - 0.4985314 * z;
    const double g = -0.9692660 * x + 1.8760108 * y + 0.0415560 * z;
    const double b = 0.0556434 * x - 0.2040259 * y + 1.0572252 * z;
    const auto inverse_gamma = [](double value) {
        value = std::max(value, 0.0);
        return value <= 0.0031308 ? 12.92 * value : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
    };
    const auto channel = [&](double value) {
        return std::size_t(std::clamp(std::round(inverse_gamma(value) * 255.0), 0.0, 255.0));
    };
    return {channel(r), channel(g), channel(b)};
}

std::vector<Color> unique(std::vector<Color> input)
{
    std::sort(input.begin(), input.end());
    input.erase(std::unique(input.begin(), input.end()), input.end());
    return input;
}

std::vector<Point> working(const std::vector<Color>& input, bool lab)
{
    std::vector<Point> result;
    result.reserve(input.size());
    for (const auto& color : input)
        result.push_back(lab ? rgb_to_lab(color) : Point {double(color[0]), double(color[1]), double(color[2])});
    return result;
}

std::vector<Point> seeds(const std::vector<Point>& points, std::size_t k, Distance distance)
{
    Point centroid {0., 0., 0.};
    for (const auto& point : points) {
        centroid[0] += point[0]; centroid[1] += point[1]; centroid[2] += point[2];
    }
    const auto n = double(points.size());
    centroid[0] /= n; centroid[1] /= n; centroid[2] /= n;
    double best = std::numeric_limits<double>::max();
    std::size_t first = 0;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const double d = distance(points[i], centroid);
        if (d < best) { best = d; first = i; }
    }
    std::vector<Point> centers(k);
    centers[0] = points[first];
    std::vector<double> nearest(points.size(), std::numeric_limits<double>::max());
    for (std::size_t center = 1; center < k; ++center) {
        double farthest = -1.;
        std::size_t selected = 0;
        for (std::size_t i = 0; i < points.size(); ++i) {
            const double d = distance(points[i], centers[center - 1]);
            if (d < nearest[i]) nearest[i] = d;
            if (nearest[i] > farthest) { farthest = nearest[i]; selected = i; }
        }
        centers[center] = points[selected];
    }
    return centers;
}

struct Trial {
    std::size_t k = 0, assignments = 0, updates = 0;
    bool converged = false, canceled = false;
    double percentile = 0., tail = 0.;
};

std::vector<Point> fit(const std::vector<Point>& points, std::size_t k, const Parameters& parameters,
                       Distance distance, Trial& trace)
{
    trace.k = k;
    auto centers = seeds(points, k, distance);
    std::vector<std::size_t> assignments(points.size());
    for (std::size_t iteration = 0; iteration < parameters.max_iter; ++iteration) {
        if (parameters.cancel_callback && parameters.cancel_callback()) {
            trace.canceled = true;
            return centers;
        }
        bool changed = false;
        ++trace.assignments;
        for (std::size_t i = 0; i < points.size(); ++i) {
            double nearest = std::numeric_limits<double>::max();
            std::size_t selected = 0;
            for (std::size_t center = 0; center < k; ++center) {
                const double d = distance(points[i], centers[center]);
                if (d < nearest) { nearest = d; selected = center; }
            }
            if (assignments[i] != selected) { changed = true; assignments[i] = selected; }
        }
        if (!changed) { trace.converged = true; break; }
        ++trace.updates;
        std::vector<Point> sums(k, {0., 0., 0.});
        std::vector<std::size_t> counts(k, 0);
        for (std::size_t i = 0; i < points.size(); ++i) {
            const auto cluster = assignments[i];
            ++counts[cluster];
            sums[cluster][0] += points[i][0];
            sums[cluster][1] += points[i][1];
            sums[cluster][2] += points[i][2];
        }
        for (std::size_t cluster = 0; cluster < k; ++cluster) {
            if (counts[cluster] == 0) {
                double farthest = -1.;
                std::size_t selected = 0;
                for (std::size_t point = 0; point < points.size(); ++point) {
                    double nearest = std::numeric_limits<double>::max();
                    for (std::size_t other = 0; other < k; ++other) {
                        if (other == cluster || counts[other] == 0) continue;
                        const double d = distance(points[point], centers[other]);
                        if (d < nearest) nearest = d;
                    }
                    if (nearest > farthest) { farthest = nearest; selected = point; }
                }
                centers[cluster] = points[selected];
            } else {
                centers[cluster][0] = sums[cluster][0] / counts[cluster];
                centers[cluster][1] = sums[cluster][1] / counts[cluster];
                centers[cluster][2] = sums[cluster][2] / counts[cluster];
            }
        }
    }
    return centers;
}

std::vector<Color> output(const std::vector<Point>& centers, bool lab)
{
    std::vector<Color> result;
    for (const auto& center : centers)
        result.push_back(lab ? lab_to_rgb(center) : Color {std::size_t(std::round(center[0])),
            std::size_t(std::round(center[1])), std::size_t(std::round(center[2]))});
    return result;
}

std::vector<Color> adaptive(const std::vector<Color>& input, const Parameters& parameters,
                            std::vector<Trial>* observed = nullptr)
{
    if (input.empty()) return {};
    const auto deduplicated = unique(input);
    if (deduplicated.size() <= 1) return deduplicated;
    const bool lab = parameters.color_difference_method != colors::ColorDifferenceMethod::RGB;
    const Distance distance = lab ? colors::calc_lab_color_difference_by_ciede2000 :
                                   colors::calc_rgb_color_difference_by_squared_rgb_double;
    const auto points = working(deduplicated, lab);
    const std::size_t maximum = std::max<std::size_t>(1, std::min(parameters.max_cluster_k, points.size()));
    std::size_t lo = std::min(std::max<std::size_t>(1, parameters.min_cluster_k), maximum), hi = maximum, best_k = 0;
    std::vector<Point> best;
    while (lo <= hi) {
        if (parameters.cancel_callback && parameters.cancel_callback()) return {};
        const auto mid = lo + (hi - lo) / 2;
        Trial trial;
        auto centers = fit(points, mid, parameters, distance, trial);
        if (observed) observed->push_back(trial);
        if (parameters.cancel_callback && parameters.cancel_callback()) return {};
        std::vector<double> distances(points.size());
        for (std::size_t point = 0; point < points.size(); ++point) {
            double nearest = std::numeric_limits<double>::max();
            for (const auto& center : centers) {
                const double d = distance(points[point], center);
                if (d < nearest) nearest = d;
            }
            distances[point] = nearest;
        }
        trial.tail = *std::max_element(distances.begin(), distances.end());
        const auto rank = std::min<std::size_t>(std::size_t(distances.size() * 0.99), distances.size() - 1);
        std::nth_element(distances.begin(), distances.begin() + rank, distances.end());
        trial.percentile = distances[rank];
        if (observed) observed->back() = trial;
        const bool tail_satisfied = parameters.max_tail_color_distance <= 0.0 || trial.tail <= parameters.max_tail_color_distance;
        if (trial.percentile <= parameters.max_color_distance && tail_satisfied) {
            best_k = mid;
            best = std::move(centers);
            hi = mid - 1;
        } else lo = mid + 1;
    }
    if (best_k == 0) {
        Trial fallback;
        best_k = std::min(maximum, points.size());
        best = fit(points, best_k, parameters, distance, fallback);
        if (observed) observed->push_back(fallback);
        // The original deliberately has no extra cancellation check here.
    }
    return output(best, lab);
}

void compare(const std::vector<Color>& input, const Parameters& parameters)
{
    const auto expected = adaptive(input, parameters);
    for (const int threads : {1, 4}) {
        CAPTURE(input.size(), parameters.max_iter, parameters.min_cluster_k, parameters.max_cluster_k, threads);
        tbb::task_arena arena(threads);
        const auto actual = arena.execute([&] { return colors::cluster_adaptive(input, parameters); });
        CHECK(actual == expected); // Ordered integer RGB output is the contract.
    }
}
} // namespace frozen_clustering
} // namespace

TEST_CASE("Adaptive clustering preserves its scalar convergence and iteration boundaries", "[ModelVertexColors][ClusteringRadiusReuse]")
{
    namespace oracle = frozen_clustering;
    namespace colors = Slic3r::tex2color::color_utils;
    const auto method = GENERATE(colors::ColorDifferenceMethod::RGB, colors::ColorDifferenceMethod::Lab);
    oracle::Parameters parameters;
    parameters.color_difference_method = method;
    parameters.max_color_distance = 25.;
    parameters.max_tail_color_distance = 0.;
    const std::vector<oracle::Color> separated {{0,0,0},{1,1,1},{2,2,2},{200,200,200},{201,201,201},{202,202,202}};
    std::vector<std::vector<oracle::Color>> inputs {{}, {{17,17,17}}, {{0,0,0},{0,0,0},{255,255,255}}, separated};
    std::vector<oracle::Color> lattice;
    for (unsigned r = 0; r < 4; ++r)
        for (unsigned g = 0; g < 4; ++g)
            for (unsigned b = 0; b < 4; ++b) lattice.push_back({r * 85, g * 85, b * 85});
    lattice.insert(lattice.end(), separated.begin(), separated.end());
    std::reverse(lattice.begin(), lattice.end());
    inputs.push_back(std::move(lattice));
    for (const auto iterations : {0u, 1u, 2u, 50u})
        for (const auto range : {std::array<std::size_t,2>{1,1}, {2,2}, {1,4}}) {
            parameters.max_iter = iterations;
            parameters.min_cluster_k = range[0]; parameters.max_cluster_k = range[1];
            for (const auto& input : inputs) oracle::compare(input, parameters);
        }

    parameters.min_cluster_k = parameters.max_cluster_k = 2;
    parameters.max_color_distance = std::numeric_limits<double>::max();
    parameters.max_iter = 0;
    std::vector<oracle::Trial> unassigned;
    oracle::adaptive(separated, parameters, &unassigned);
    REQUIRE(unassigned.size() == 1);
    CHECK(unassigned.front().assignments == 0);
    CHECK(unassigned.front().updates == 0);
    CHECK_FALSE(unassigned.front().converged);
    parameters.max_iter = 1;
    std::vector<oracle::Trial> limited;
    oracle::adaptive(separated, parameters, &limited);
    REQUIRE(limited.size() == 1);
    CHECK(limited.front().updates == 1);
    CHECK_FALSE(limited.front().converged); // Distances precede the final center update.
    parameters.max_iter = 50;
    std::vector<oracle::Trial> converged;
    oracle::adaptive(separated, parameters, &converged);
    REQUIRE(converged.size() == 1);
    CHECK(converged.front().converged);
    CHECK(converged.front().assignments == converged.front().updates + 1);

    parameters.min_cluster_k = parameters.max_cluster_k = 1;
    parameters.max_iter = 1;
    const std::vector<oracle::Color> skewed {{0,0,0},{10,10,10},{250,250,250}};
    std::vector<oracle::Trial> first;
    oracle::adaptive(skewed, parameters, &first);
    REQUIRE(first.size() == 1);
    CHECK(first.front().converged);
    CHECK(first.front().updates == 0); // All assignments start at zero: keep the FPS center.
    oracle::compare(skewed, parameters);
}

TEST_CASE("Adaptive clustering keeps percentile and tail threshold decisions at exact boundaries", "[ModelVertexColors][ClusteringRadiusReuse]")
{
    namespace oracle = frozen_clustering;
    namespace colors = Slic3r::tex2color::color_utils;
    const auto method = GENERATE(colors::ColorDifferenceMethod::RGB, colors::ColorDifferenceMethod::Lab);
    oracle::Parameters parameters;
    parameters.color_difference_method = method;
    parameters.min_cluster_k = 1; parameters.max_cluster_k = 3;
    parameters.max_color_distance = std::numeric_limits<double>::max();
    parameters.max_iter = 50;
    std::vector<oracle::Color> input;
    for (std::size_t gray = 0; gray < 100; ++gray) input.push_back({gray,gray,gray});
    input.push_back({255,255,255}); // 101 unique points puts P99 at rank 99, not rank 100.
    std::vector<oracle::Trial> trials;
    oracle::adaptive(input, parameters, &trials);
    REQUIRE_FALSE(trials.empty());
    REQUIRE(trials.front().k == 2);
    REQUIRE(trials.front().percentile > 0.);
    REQUIRE(trials.front().tail >= trials.front().percentile);
    const double infinity = std::numeric_limits<double>::infinity();
    const double radius = trials.front().percentile, tail = trials.front().tail;
    for (const double threshold : {std::nextafter(radius, -infinity), radius, std::nextafter(radius, infinity)}) {
        parameters.max_color_distance = threshold;
        for (const double limit : {0., std::nextafter(tail, -infinity), tail, std::nextafter(tail, infinity)}) {
            parameters.max_tail_color_distance = limit;
            CAPTURE(int(method), threshold, limit);
            oracle::compare(input, parameters);
        }
    }
    parameters.max_color_distance = 0.;
    parameters.max_tail_color_distance = 0.;
    std::vector<oracle::Trial> fallback;
    oracle::adaptive(input, parameters, &fallback);
    REQUIRE(fallback.size() >= 2);
    CHECK(fallback.back().k == parameters.max_cluster_k);
    CHECK(fallback[fallback.size()-2].k == parameters.max_cluster_k);
    oracle::compare(input, parameters); // Preserve the original repeated max-K fallback.
}

TEST_CASE("Adaptive clustering retains every original cancellation checkpoint and fallback", "[ModelVertexColors][ClusteringRadiusReuse]")
{
    namespace oracle = frozen_clustering;
    namespace colors = Slic3r::tex2color::color_utils;
    const auto method = GENERATE(colors::ColorDifferenceMethod::RGB, colors::ColorDifferenceMethod::Lab);
    const auto iterations = GENERATE(0u, 1u, 50u);
    const auto maximum_radius = GENERATE(0., 1000000.);
    const std::vector<oracle::Color> input {{0,0,0},{10,10,10},{30,30,30},{190,190,190},{210,210,210},{255,255,255}};
    oracle::Parameters parameters;
    parameters.color_difference_method = method;
    parameters.min_cluster_k = 1; parameters.max_cluster_k = 3;
    parameters.max_iter = iterations;
    parameters.max_color_distance = maximum_radius;
    std::size_t total_checks = 0;
    parameters.cancel_callback = [&] { ++total_checks; return false; };
    oracle::adaptive(input, parameters);
    REQUIRE(total_checks > 0);
    for (std::size_t cancel_at = 1; cancel_at <= total_checks + 1; ++cancel_at) {
        std::size_t expected_checks = 0;
        parameters.cancel_callback = [&] { return ++expected_checks >= cancel_at; };
        const auto expected = oracle::adaptive(input, parameters);
        for (const int threads : {1, 4}) {
            std::size_t actual_checks = 0;
            parameters.cancel_callback = [&] { return ++actual_checks >= cancel_at; };
            tbb::task_arena arena(threads);
            const auto actual = arena.execute([&] { return colors::cluster_adaptive(input, parameters); });
            CAPTURE(int(method), iterations, maximum_radius, cancel_at, threads);
            CHECK(actual == expected);
            CHECK(actual_checks == expected_checks);
        }
    }
    // Both early-return cases precede even the first cancellation callback.
    for (const std::vector<oracle::Color> early : {std::vector<oracle::Color>{}, {{17,17,17}}}) {
        std::size_t checks = 0;
        parameters.cancel_callback = [&] { ++checks; return true; };
        CHECK(colors::cluster_adaptive(early, parameters) == early);
        CHECK(checks == 0);
    }
}

TEST_CASE("Clustering reuse diagnostics exclude final updates without a subsequent assignment", "[ModelVertexColors][ClusteringReuseEvidence]")
{
    using namespace Slic3r::tex2color::color_utils;
    const auto method = GENERATE(ColorDifferenceMethod::RGB, ColorDifferenceMethod::Lab);
    const std::size_t iterations = GENERATE(0, 1, 2, 3);
    const std::vector<Color> colors = {{0,0,0}, {128,64,32}, {255,255,255}};
    ClusterParameters parameters;
    parameters.cluster_k = colors.size();
    parameters.max_iter = iterations;
    parameters.color_difference_method = method;
    std::map<std::string, std::size_t> counts;
    parameters.diagnostic_callback = [&](const char* stage, std::uint64_t, std::size_t count) {
        counts[stage] = count;
    };
    const auto actual = cluster_k_means(colors, parameters);
    parameters.diagnostic_callback = {};
    CHECK(actual == cluster_k_means(colors, parameters));
    CHECK(counts["kmeans.working_colors"] == colors.size());
    CHECK(counts["kmeans.assignment_reusable_columns"] == (iterations >= 2 ? colors.size() : 0));
    CHECK(counts["kmeans.assignment_column_comparisons"] == (iterations >= 2 ? colors.size() : 0));
    CHECK(counts["kmeans.assignment"] == std::min<std::size_t>(iterations, 2));
}

TEST_CASE("Clustering diagnostics cannot alter ordered results or propagate failures", "[ModelVertexColors][ClusteringRadiusReuse]")
{
    namespace oracle = frozen_clustering;
    namespace colors = Slic3r::tex2color::color_utils;
    const std::vector<oracle::Color> input {{0,0,0},{1,2,3},{127,32,240},{255,255,255}};
    oracle::Parameters parameters;
    parameters.color_difference_method = GENERATE(colors::ColorDifferenceMethod::RGB, colors::ColorDifferenceMethod::Lab);
    parameters.min_cluster_k = 1; parameters.max_cluster_k = 3;
    parameters.max_iter = 50;
    std::atomic<std::size_t> calls {0};
    parameters.diagnostic_callback = [&](const char*, std::uint64_t, std::size_t) {
        ++calls;
        throw std::runtime_error("diagnostic probe");
    };
    oracle::compare(input, parameters);
    CHECK(calls.load() > 0);
}

TEST_CASE("Parallel clustering combines changed ranges without losing another range's update", "[ModelVertexColors][ClusteringRangeChanges]")
{
    namespace oracle = frozen_clustering;
    namespace colors = Slic3r::tex2color::color_utils;
    const auto method = GENERATE(colors::ColorDifferenceMethod::RGB, colors::ColorDifferenceMethod::Lab);
    oracle::Parameters parameters;
    parameters.color_difference_method = method;
    parameters.max_color_distance = std::numeric_limits<double>::max();
    std::vector<oracle::Color> input;
    for (unsigned r = 0; r < 16; ++r)
        for (unsigned g = 0; g < 16; ++g)
            for (unsigned b = 0; b < 16; ++b) input.push_back({r * 17, g * 17, b * 17});
    for (const auto k : {1u, 2u, 6u})
        for (const auto iterations : {1u, 2u, 50u}) {
            parameters.min_cluster_k = parameters.max_cluster_k = k;
            parameters.max_iter = iterations;
            std::vector<oracle::Trial> trials;
            const auto expected = oracle::adaptive(input, parameters, &trials);
            REQUIRE(trials.size() == 1);
            if (k == 1) {
                CHECK(trials.front().converged);
                CHECK(trials.front().updates == 0);
            } else CHECK(trials.front().updates > 0);
            for (const auto threads : {1, 2, 4}) {
                CAPTURE(int(method), k, iterations, threads);
                tbb::task_arena arena(threads);
                const auto actual = arena.execute([&] { return colors::cluster_adaptive(input, parameters); });
                CHECK(actual == expected);
            }
        }
}

TEST_CASE("Unordered smoothing weights retain the original no-smoothing behavior", "[ModelVertexColors][RegionSmoothing]")
{
    namespace colors = Slic3r::tex2color::color_utils;
    Slic3r::tex2color::TriMesh input;
    input.vertices = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    input.indices = {{0,2,4},{2,1,4},{1,3,4},{3,0,4},{2,0,5},{1,2,5},{3,1,5},{0,3,5}};
    const std::vector<std::size_t> original_labels {0,1,1,1,1,1,1,1};
    auto labels = original_labels;
    auto result = input;
    std::map<std::string, std::size_t> stages;
    colors::SmoothParameters parameters;
    parameters.smooth_weight = std::numeric_limits<double>::quiet_NaN();
    REQUIRE(std::isnan(parameters.smooth_weight));
    parameters.diagnostic_callback = [&](const char* stage, std::uint64_t, std::size_t) { ++stages[stage]; };
    const bool explicit_workspace = GENERATE(false, true);
    if (explicit_workspace) {
        colors::RegionSmoothingWorkspace workspace(input, parameters);
        REQUIRE(workspace.apply_topology(labels, parameters));
        REQUIRE(workspace.finish(result, labels, parameters));
    } else {
        REQUIRE(colors::smooth_region(result, labels, parameters));
    }
    CHECK(labels == original_labels);
    require_identical_smoothing_mesh(result, input);
    CHECK(stages["prepare"] == 1);
    CHECK(stages["topology"] == 0);
    CHECK(stages["boundary-extraction"] == 0);
    CHECK(stages["projection-index"] == 0);
    CHECK(stages["projection"] == 0);
    CHECK(stages["export"] == 1);
}

TEST_CASE("Immutable palette queries retain scalar CIEDE2000 labels and ties", "[ModelVertexColors][ColorDistancePalette]")
{
    namespace colors = Slic3r::tex2color::color_utils;
    std::mt19937 random(817201);
    for (const auto count : {1u, 2u, 6u, 16u, 32u}) {
        std::vector<colors::RGB> centers;
        for (unsigned i = 0; i < count; ++i)
            centers.push_back({random() % 256, random() % 256, random() % 256});
        centers.push_back(centers.front()); // Exact ties must select the first center.
        const colors::ColorDistancePalette palette(centers);
        for (unsigned query = 0; query < 512; ++query) {
            const colors::RGB color = query < centers.size() ? centers[query] :
                colors::RGB{random() % 256, random() % 256, random() % 256};
            std::size_t expected = 0, actual = 0;
            REQUIRE(colors::calc_nearest_color_id(centers, color, expected));
            REQUIRE(palette.nearest(color, actual));
            CHECK(actual == expected);
        }
    }
    std::size_t unchanged = 19;
    const colors::ColorDistancePalette empty({});
    CHECK_FALSE(empty.nearest({0,0,0}, unchanged));
    CHECK(unchanged == 19);
}

TEST_CASE("Halfedge validation retains the original fan decisions for ordered soups", "[ModelVertexColors][HalfedgeCompatibility]")
{
    namespace geometry = Slic3r::tex2color;
    geometry::TriMesh mesh;
    mesh.vertices.resize(8, Vec3f(0.f, 0.f, 0.f)); // Coordinate degeneracy was never part of this predicate.
    const std::array<Vec3i32, 4> tetra {{ {0,2,1}, {0,1,3}, {0,3,2}, {1,2,3} }};
    for (unsigned subset = 0; subset < 16; ++subset)
        for (unsigned flips = 0; flips < 16; ++flips) {
            mesh.indices.clear();
            for (unsigned face = 0; face < 4; ++face)
                if (subset & (1u << face)) {
                    auto triangle = tetra[face];
                    if (flips & (1u << face)) std::swap(triangle[0], triangle[1]);
                    mesh.indices.push_back(triangle);
                }
            CHECK(geometry::cgalutils::is_mesh_halfedge_compatible(mesh) == legacy_halfedge_compatible(mesh));
        }
    mesh.indices = {{0,1,2},{0,2,3},{0,4,5},{0,5,6}};
    CHECK_FALSE(geometry::cgalutils::is_mesh_halfedge_compatible(mesh));
    std::mt19937 random(615917);
    for (int iteration = 0; iteration < 2000; ++iteration) {
        mesh.indices.clear();
        const auto faces = random() % 16;
        for (unsigned face = 0; face < faces; ++face) {
            int a = int(random() % 8), b = int(random() % 8), c = int(random() % 8);
            if (iteration % 31 == 0 && face == 0) a = -1;
            if (iteration % 37 == 0 && face == 0) c = 8;
            mesh.indices.emplace_back(a,b,c);
        }
        CHECK(geometry::cgalutils::is_mesh_halfedge_compatible(mesh) == legacy_halfedge_compatible(mesh));
    }
}

#include "libslic3r/TextureToColor/ColorDistancePrepared.hpp"
#include <cfenv>
#include <cfloat>
#include <exception>
#include <thread>

#if defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2) || defined(__SSE2__)
#include <xmmintrin.h>
#define ORCA_TEST_HAS_SSE2_FP_CONTROLS 1
#else
#define ORCA_TEST_HAS_SSE2_FP_CONTROLS 0
#endif

namespace {
namespace prepared_fp_test {
namespace prepared = Slic3r::tex2color::color_utils::detail;

std::uint64_t bits(double value)
{
    std::uint64_t result = 0;
    static_assert(sizeof(result) == sizeof(value), "Expected a 64-bit double representation");
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

struct SavedEnvironment {
    std::fenv_t original {};
    bool valid = false;
#if ORCA_TEST_HAS_SSE2_FP_CONTROLS
    unsigned mxcsr = _mm_getcsr();
#endif
    SavedEnvironment() : valid(std::fegetenv(&original) == 0) {}
    ~SavedEnvironment()
    {
        if (valid) std::fesetenv(&original);
#if ORCA_TEST_HAS_SSE2_FP_CONTROLS
        _mm_setcsr(mxcsr); // Restore controls AND sticky state exactly.
#endif
    }
    SavedEnvironment(const SavedEnvironment&) = delete;
    SavedEnvironment& operator=(const SavedEnvironment&) = delete;
};

bool set_default_environment()
{
    std::fenv_t ignored;
    // Hold/clear exceptions before any intentional test-mode changes.
    if (std::feholdexcept(&ignored) != 0) return false;
    if (std::fesetround(FE_TONEAREST) != 0) return false;
#if ORCA_TEST_HAS_SSE2_FP_CONTROLS
    _mm_setcsr(0x1F80u);
#endif
    return true;
}

bool supported_compilation()
{
#if defined(__FAST_MATH__) || defined(_M_FP_FAST) || \
    (defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__ > 0)
    return false;
#elif ORCA_TEST_HAS_SSE2_FP_CONTROLS && FLT_EVAL_METHOD == 0
    return true;
#else
    return false;
#endif
}

struct Refusal {
    bool guard = true, accepted = true;
    std::uint64_t sentinel = 0;
};

Refusal observe_refusal()
{
    // On a deliberately unmasked exception environment only early-return guard
    // code may run. These inputs and sentinel loads require no FP arithmetic.
    double value = -17.125;
    Refusal result;
    result.guard = prepared::has_default_lab_chroma_environment();
    result.accepted = prepared::prepare_lab_chroma({50.,3.,4.}, value);
    result.sentinel = bits(value);
    return result;
}
} // namespace prepared_fp_test
} // namespace

TEST_CASE("Prepared chroma qualification rejects unknown and excess-precision environments", "[ModelVertexColors][PreparedLabChroma][PreparedLabEnvironment]")
{
    namespace fp = prepared_fp_test;
    namespace prepared = Slic3r::tex2color::color_utils::detail;
    bool setup = false, qualified = false, accepted = false;
    std::uint64_t result = 0;
    {
        fp::SavedEnvironment saved;
        if (saved.valid) setup = fp::set_default_environment();
        if (setup) {
            qualified = prepared::has_default_lab_chroma_environment();
            double chroma = -17.125;
            accepted = prepared::prepare_lab_chroma({50.,3.,4.}, chroma);
            result = fp::bits(chroma);
        }
    }
    REQUIRE(setup);
    CHECK(qualified == fp::supported_compilation());
    CHECK(accepted == fp::supported_compilation());
    CHECK(result == fp::bits(fp::supported_compilation() ? 5. : -17.125));
}

TEST_CASE("Prepared chroma refuses nondefault rounding without changing the output", "[ModelVertexColors][PreparedLabChroma][PreparedLabEnvironment]")
{
    namespace fp = prepared_fp_test;
    for (const int mode : {FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
        bool setup = false, round_unchanged = false;
        fp::Refusal observed;
        {
            fp::SavedEnvironment saved;
            if (saved.valid) setup = fp::set_default_environment();
            if (setup) setup = std::fesetround(mode) == 0;
            if (setup) {
                observed = fp::observe_refusal();
                round_unchanged = std::fegetround() == mode;
            }
        }
        CAPTURE(mode);
        REQUIRE(setup);
        CHECK_FALSE(observed.guard);
        CHECK_FALSE(observed.accepted);
        CHECK(observed.sentinel == fp::bits(-17.125));
        CHECK(round_unchanged);
    }
}

TEST_CASE("Prepared chroma refuses altered SSE controls and accepts default sticky flags", "[ModelVertexColors][PreparedLabChroma][PreparedLabEnvironment]")
{
    namespace fp = prepared_fp_test;
    namespace prepared = Slic3r::tex2color::color_utils::detail;
#if ORCA_TEST_HAS_SSE2_FP_CONTROLS
    // The nondefault mask case clears all sticky bits before unmasking invalid.
    // No scalar/kernel arithmetic is called while an exception is unmasked.
    for (const unsigned controls : {0x1F80u | 0x8000u, 0x1F80u | 0x0040u, 0x1F80u & ~0x0080u}) {
        bool setup = false;
        unsigned after = 0;
        fp::Refusal observed;
        {
            fp::SavedEnvironment saved;
            if (saved.valid) setup = fp::set_default_environment();
            if (setup) {
                _mm_setcsr(controls); // low-six status bits deliberately zero
                observed = fp::observe_refusal();
                after = _mm_getcsr();
            }
        }
        CAPTURE(controls);
        REQUIRE(setup);
        CHECK_FALSE(observed.guard);
        CHECK_FALSE(observed.accepted);
        CHECK(observed.sentinel == fp::bits(-17.125));
        CHECK((after & 0xFFC0u) == controls);
    }
    for (const unsigned sticky : {0x01u, 0x04u, 0x20u, 0x3Fu}) {
        bool setup = false, qualified = false, accepted = false;
        unsigned after = 0;
        std::uint64_t result = 0;
        {
            fp::SavedEnvironment saved;
            if (saved.valid) setup = fp::set_default_environment();
            if (setup) {
                _mm_setcsr(0x1F80u | sticky);
                qualified = prepared::has_default_lab_chroma_environment();
                double chroma = -17.125;
                accepted = prepared::prepare_lab_chroma({50.,3.,4.}, chroma);
                result = fp::bits(chroma);
                after = _mm_getcsr();
            }
        }
        CAPTURE(sticky);
        REQUIRE(setup);
        CHECK(qualified == fp::supported_compilation());
        CHECK(accepted == fp::supported_compilation());
        CHECK(result == fp::bits(fp::supported_compilation() ? 5. : -17.125));
        CHECK((after & 0xFFC0u) == 0x1F80u);
        CHECK((after & sticky) == sticky);
    }
#else
    const auto observed = fp::observe_refusal();
    CHECK_FALSE(observed.guard);
    CHECK_FALSE(observed.accepted);
    CHECK(observed.sentinel == fp::bits(-17.125));
#endif
}

TEST_CASE("Public clustering keeps scalar results and cancellation on an unqualified worker", "[ModelVertexColors][PreparedLabChroma][PreparedLabEnvironment]")
{
    namespace fp = prepared_fp_test;
    namespace oracle = frozen_clustering;
    namespace colors = Slic3r::tex2color::color_utils;
    const auto iterations = GENERATE(1u, 50u);
    const auto mode = GENERATE(0u, 1u, 2u);
    const std::vector<oracle::Color> input {{0,0,0},{1,1,1},{2,2,2},{127,32,240},
        {200,200,200},{201,201,201},{202,202,202}};
    const auto caller_round = std::fegetround();
#if ORCA_TEST_HAS_SSE2_FP_CONTROLS
    const auto caller_mxcsr = _mm_getcsr();
#endif
    bool setup = false, qualified = true, guard_stayed_unqualified = false;
    std::vector<oracle::Color> expected, actual;
    std::size_t expected_checks = 0, actual_checks = 0;
    std::exception_ptr failure;
    std::thread worker([&] {
        try {
            fp::SavedEnvironment saved;
            if (saved.valid) setup = fp::set_default_environment();
            if (!setup) return;
            if (mode == 0) setup = std::fesetround(FE_DOWNWARD) == 0;
#if ORCA_TEST_HAS_SSE2_FP_CONTROLS
            else if (mode == 1) _mm_setcsr(0x1F80u | 0x8000u);
            else _mm_setcsr(0x1F80u | 0x0040u);
#else
            else setup = std::fesetround(FE_UPWARD) == 0;
#endif
            if (!setup) return;
            qualified = Slic3r::tex2color::color_utils::detail::has_default_lab_chroma_environment();
            oracle::Parameters parameters;
            parameters.color_difference_method = colors::ColorDifferenceMethod::Lab;
            parameters.min_cluster_k = parameters.max_cluster_k = 2;
            parameters.max_iter = iterations;
            parameters.max_color_distance = 1000000.;
            parameters.cancel_callback = [&] { ++expected_checks; return false; };
            expected = oracle::adaptive(input, parameters);
            parameters.cancel_callback = [&] { ++actual_checks; return false; };
            // One thread keeps the entire actual public algorithm in this
            // intentionally unqualified environment. It must select scalar.
            tbb::task_arena arena(1);
            actual = arena.execute([&] { return colors::cluster_adaptive(input, parameters); });
            guard_stayed_unqualified = !Slic3r::tex2color::color_utils::detail::has_default_lab_chroma_environment();
        } catch (...) { failure = std::current_exception(); }
    });
    worker.join();
    if (failure) std::rethrow_exception(failure);
    REQUIRE(setup);
    CHECK_FALSE(qualified);
    CHECK(guard_stayed_unqualified);
    CHECK(actual == expected);
    CHECK(actual_checks == expected_checks);
    CHECK(std::fegetround() == caller_round);
#if ORCA_TEST_HAS_SSE2_FP_CONTROLS
    CHECK(_mm_getcsr() == caller_mxcsr);
#endif
}

#undef ORCA_TEST_HAS_SSE2_FP_CONTROLS
