#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "slic3r/GUI/AI/Model/ModelFinishing.hpp"
#include "slic3r/GUI/AI/Model/GlbGeometryEditing.hpp"
#include "slic3r/GUI/AI/Model/VertexColorRegionEditor.hpp"
#include "slic3r/GUI/TextureImportModel.hpp"
#include "slic3r/GUI/AI/Orca/OrcaWorkspaceAdapter.hpp"
#include "libslic3r/Format/AssimpImport.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TexturePainting.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <numeric>
#include <set>

using namespace Slic3r;
using namespace Slic3r::AI;
using Catch::Matchers::WithinAbs;
using namespace Slic3r::GUI;
namespace {
struct Fixture {
    boost::filesystem::path directory = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("orca-glb-%%%%-%%%%-%%%%");
    Fixture() { boost::filesystem::create_directory(directory); }
    ~Fixture() { boost::system::error_code ec; boost::filesystem::remove_all(directory, ec); }
};
const boost::filesystem::path samples = boost::filesystem::path(std::string(TEST_DATA_DIR)) / "model_artifact";

std::array<int, 3> canonical_face(std::array<int, 3> face)
{
    const auto minimum = std::min_element(face.begin(), face.end());
    std::rotate(face.begin(), minimum, face.end());
    return face;
}

void require_closed_painted_mesh(const PaintedMesh& painted)
{
    // Count indexed edges, so coincident but unshared vertices and T-junctions
    // cannot be hidden by a later mesh repair or position-based edge lookup.
    std::map<std::array<int, 2>, std::pair<int, int>> edges;
    REQUIRE(painted.face_colors.size() == painted.indices.size());
    for (const auto& face : painted.indices) {
        for (int corner = 0; corner < 3; ++corner) {
            const int a = face[corner], b = face[(corner + 1) % 3];
            REQUIRE(a >= 0);
            REQUIRE(size_t(a) < painted.vertices.size());
            REQUIRE(a != b);
            auto& edge = edges[{std::min(a, b), std::max(a, b)}];
            ++edge.first;
            edge.second += a < b ? 1 : -1;
        }
    }
    for (const auto& edge : edges) {
        REQUIRE(edge.second.first == 2);
        REQUIRE(edge.second.second == 0);
    }
}
}

TEST_CASE("Local history import preserves GLB bytes and refuses to overwrite an existing asset", "[ModelArtifact][LocalModelImport]") {
    Fixture f;std::string error;const auto source=samples/"textured.glb",destination=f.directory/"archived.glb";
    const auto hash=model_artifact_sha256(source);
    REQUIRE(archive_local_model(source,destination,error));CHECK(model_artifact_sha256(destination)==hash);
    CHECK_FALSE(archive_local_model(source,destination,error));CHECK(model_artifact_sha256(destination)==hash);
    const auto malformed=f.directory/"broken.glb";{boost::filesystem::ofstream out(malformed);out<<"broken";}
    CHECK_FALSE(archive_local_model(malformed,f.directory/"rejected.glb",error));
    CHECK_FALSE(boost::filesystem::exists(f.directory/"rejected.glb"));CHECK(model_artifact_sha256(source)==hash);
}

TEST_CASE("Repeated GLB import reuses only a source and OBJ verified in this process", "[ModelArtifact][GlbImportCache]") {
    Fixture f;
    const auto source = f.directory / "source.glb";
    const auto same_bytes = f.directory / "same-bytes.glb";
    boost::filesystem::copy_file(samples / "textured.glb", source);
    boost::filesystem::copy_file(source, same_bytes);
    const auto stale_copy = f.directory / "ai-import" /
        ("orcaslicer-ai-glb-" + model_artifact_sha256(source) + ".obj");
    boost::filesystem::create_directories(stale_copy.parent_path());
    { boost::filesystem::ofstream stale(stale_copy);
      stale << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"; }
    const auto stale_hash = model_artifact_sha256(stale_copy);
    VerifiedGlbImportCopy verified;
    boost::filesystem::path output;
    std::string error;
    bool reused = true;

    REQUIRE(prepare_glb_obj_import(source, f.directory, verified, output, error, reused));
    REQUIRE_FALSE(reused);
    REQUIRE(is_model_artifact(output));
    CHECK(output != stale_copy);
    CHECK(model_artifact_sha256(stale_copy) == stale_hash);
    const auto original_copy = output;
    const auto original_hash = model_artifact_sha256(output);
    REQUIRE(prepare_glb_obj_import(same_bytes, f.directory, verified, output, error, reused));
    REQUIRE(reused);
    CHECK(output == original_copy);

    // A changed but still valid OBJ must not become the import copy merely
    // because its filename still contains the source GLB's hash.
    { boost::filesystem::ofstream changed(original_copy, std::ios::app); changed << "\n# altered cache\n"; }
    REQUIRE(prepare_glb_obj_import(source, f.directory, verified, output, error, reused));
    REQUIRE_FALSE(reused);
    REQUIRE(output != original_copy);
    CHECK(model_artifact_sha256(original_copy) != original_hash);
    TriangleMesh restored;
    ObjInfo colors;
    REQUIRE(load_model_artifact(output, restored, colors, error));
    CHECK_FALSE(restored.its.indices.empty());

    boost::filesystem::remove(output);
    REQUIRE(prepare_glb_obj_import(source, f.directory, verified, output, error, reused));
    REQUIRE_FALSE(reused);
    REQUIRE(is_model_artifact(output));

    // Changing source bytes invalidates the content identity even if its
    // pathname and the previously prepared OBJ remain unchanged.
    { boost::filesystem::ofstream changed(source, std::ios::binary | std::ios::trunc); changed << "broken"; }
    REQUIRE_FALSE(prepare_glb_obj_import(source, f.directory, verified, output, error, reused));
    CHECK_FALSE(reused);
    CHECK(output.empty());
    CHECK_FALSE(error.empty());
}

TEST_CASE("Local OBJ import embeds texture bytes and retains printable size and UV orientation", "[ModelArtifact][LocalModelImport]") {
    Fixture f;std::string error;TexturedMesh reference;
    REQUIRE(load_assimp_textured_model((samples/"textured.glb").string(),reference,&error));
    REQUIRE_FALSE(reference.textures.empty());
    {boost::filesystem::ofstream out(f.directory/"color.png",std::ios::binary);const auto& data=reference.textures.front().data;out.write(reinterpret_cast<const char*>(data.data()),data.size());}
    {boost::filesystem::ofstream out(f.directory/"model.mtl");out<<"newmtl surface\nKd 1 1 1\nmap_Kd color.png\n";}
    {boost::filesystem::ofstream out(f.directory/"model.obj");out<<"mtllib model.mtl\nv 0 0 0\nv 20 0 0\nv 0 20 0\nvt 0.2 0.2\nvt 0.8 0.2\nvt 0.2 0.8\nusemtl surface\nf 1/1 2/2 3/3\n";}
    const auto destination=f.directory/"archived.glb";
    REQUIRE(archive_local_model(f.directory/"model.obj",destination,error));
    TexturedMesh obj,glb;REQUIRE(load_assimp_textured_model((f.directory/"model.obj").string(),obj,&error));
    REQUIRE(load_assimp_textured_model(destination.string(),glb,&error));
    REQUIRE(glb.textures.size()==1);CHECK(glb.textures.front().data==reference.textures.front().data);
    REQUIRE(glb.uvs.size()==obj.uvs.size());
    // The native OBJ loader flips bottom-origin OBJ UVs into the same
    // top-origin sampling convention as GLB. Raw Assimp OBJ UVs do not.
    for(size_t i=0;i<obj.uvs.size();++i) {
        CHECK_THAT(glb.uvs[i][0],WithinAbs(obj.uvs[i][0],1e-6));
        CHECK_THAT(glb.uvs[i][1],WithinAbs(1.f-obj.uvs[i][1],1e-6));
    }
    TriangleMesh native;ObjInfo obj_info;ObjParser::MtlData materials;TexturedMesh native_texture;
    REQUIRE(load_obj((f.directory/"model.obj").string().c_str(),&native,obj_info,error,&materials));
    REQUIRE(obj_to_textured_mesh(obj_info,native.its,materials,f.directory.string(),native_texture));
    std::vector<std::array<size_t,3>> native_colors,converted_colors;
    REQUIRE(sample_original_face_colors(native_texture,native_colors));REQUIRE(sample_original_face_colors(glb,converted_colors));
    CHECK(native_colors==converted_colors);
    TriangleMesh mesh;ObjInfo colors;REQUIRE(load_model_artifact(destination,mesh,colors,error));
    CHECK(mesh.its.indices.size()==1);CHECK_THAT(mesh.bounding_box().size().x(),WithinAbs(20.,.001));
    boost::filesystem::remove(f.directory/"color.png");boost::filesystem::remove(f.directory/"model.mtl");
    REQUIRE(load_model_artifact(destination,mesh,colors,error));
}

TEST_CASE("GLB textures and transformed scenes match the local analysis colors and print coordinates", "[ModelArtifact]") {
    for (const std::string name : {"textured", "transformed", "baseline", "uv-rotation", "uv-repeat",
                                  "uv-mirrored-repeat", "uv-clamp", "multi-material", "vertex-material-color",
                                  "ushort-vertex-colors", "nested-negative-nodes", "nested-negative-nodes-prepared"}) {
        DYNAMIC_SECTION(name) {
            TriangleMesh glb, expected; ObjInfo colors, expected_colors; std::string error;
            REQUIRE(load_model_artifact(samples / (name + ".glb"), glb, colors, error));
            REQUIRE(load_model_artifact(samples / (name + ".obj"), expected, expected_colors, error));
            REQUIRE(glb.its.vertices.size() == expected.its.vertices.size());
            REQUIRE(glb.its.indices.size() == expected.its.indices.size());
            // Assimp may reorder vertices while generating normals. Compare
            // the surface and its colors by position, not incidental indices.
            std::vector<bool> used(expected.its.vertices.size(), false);
            std::vector<int> remap(glb.its.vertices.size(), -1);
            for (size_t i = 0; i < glb.its.vertices.size(); ++i) {
                INFO("vertex " << i);
                size_t match = expected.its.vertices.size();
                for (size_t j = 0; j < expected.its.vertices.size(); ++j) {
                    bool same_color = true;
                    for (int c = 0; c < 3; ++c)
                        same_color = same_color && std::abs(colors.vertex_colors[i][c] - expected_colors.vertex_colors[j][c]) < .00001f;
                    if (!used[j] && same_color && (glb.its.vertices[i] - expected.its.vertices[j]).norm() < .001f) {
                        match = j; break;
                    }
                }
                REQUIRE(match < expected.its.vertices.size());
                used[match] = true;
                remap[i] = int(match);
                for (int c = 0; c < 3; ++c) {
                    REQUIRE_THAT(glb.its.vertices[i][c], WithinAbs(expected.its.vertices[match][c], .001));
                    REQUIRE_THAT(colors.vertex_colors[i][c], WithinAbs(expected_colors.vertex_colors[match][c], .00001));
                }
            }
            std::multiset<std::array<int, 3>> actual_faces, expected_faces;
            for (const auto& f : glb.its.indices)
                actual_faces.insert(canonical_face({remap[f[0]], remap[f[1]], remap[f[2]]}));
            for (const auto& f : expected.its.indices)
                expected_faces.insert(canonical_face({f[0], f[1], f[2]}));
            REQUIRE(actual_faces == expected_faces);
            if (name == "textured" || name == "multi-material") {
                Model native = Model::read_from_file((samples / (name + ".glb")).string());
                REQUIRE(native.texture_mesh);
                REQUIRE_FALSE(native.texture_mesh->textures.empty());
                CHECK(native.texture_mesh->precomputed_vertex_colors.empty());
                CHECK(native.texture_mesh->precomputed_face_colors.empty());
            }
        }
    }
}

TEST_CASE("Workbench GLB handoff retains texture pixels and print dimensions", "[WorkbenchTextureImport]") {
    for (const std::string name : {"textured", "transformed", "multi-material", "jpeg-textured"}) {
        DYNAMIC_SECTION(name) {
            const auto path = samples / (name + ".glb");
            const auto hash = model_artifact_sha256(path);
            ModelImportRequest request;
            request.artifact.local_path = path;
            const auto options = model_import_color_options(request);
            REQUIRE(options.source_units_in_meters);
            Model imported = Model::read_from_file(path.string());
            REQUIRE(apply_texture_import_units(imported, &options));
            REQUIRE(imported.texture_mesh);
            REQUIRE(imported.objects.size() == 1);
            const auto* volume = imported.objects.front()->volumes.front();
            TriangleMesh preview; ObjInfo colors; std::string error;
            REQUIRE(load_model_artifact(path, preview, colors, error));
            REQUIRE(volume->mesh().bounding_box().size().isApprox(preview.bounding_box().size(), 1e-5f));
            REQUIRE(model_artifact_sha256(path) == hash);
            ModelMatchedColors matched;
            matched.source_sha256=hash;
            matched.geometry_id=SurfaceSelectionPersistence::geometry_fingerprint(preview.its);
            matched.palette={{0,"#F7E2DA","PLA",true},{1,"#282629","PLA",true}};
            for(size_t face=0;face<preview.its.indices.size();++face)matched.face_slots.push_back(face%2);
            request.matched_colors=matched;
            auto saved_options=model_import_color_options(request);
            saved_options.matched_source=std::make_shared<indexed_triangle_set>(preview.its);
            imported.objects.front()->center_around_origin(false);
            REQUIRE_NOTHROW(apply_matched_texture_colors(imported,saved_options,saved_options.matched_filaments));
            TriangleSelector expected(volume->mesh());
            for(size_t face=0;face<matched.face_slots.size();++face)
                expected.set_facet(int(face),EnforcerBlockerType(int(EnforcerBlockerType::Extruder1)+int(matched.face_slots[face])));
            REQUIRE(volume->mmu_segmentation_facets.get_data()==expected.serialize());
        }
    }
    TextureImportOptions ordinary;
    Model millimetres;
    auto* object = millimetres.add_object("tiny model", "", make_cube(0.1,0.2,0.3));
    const auto bounds = object->volumes.front()->mesh().bounding_box().size();
    CHECK_FALSE(apply_texture_import_units(millimetres, &ordinary));
    CHECK(object->volumes.front()->mesh().bounding_box().size().isApprox(bounds));
}

TEST_CASE("An explicit local portrait retains face correspondence through native import", "[.][PortraitImportProbe]") {
    const auto path=std::getenv("ORCA_PORTRAIT_SOURCE");
    if(!path || !*path)SKIP("Explicit local portrait required.");
    TriangleMesh preview;ObjInfo colors;std::string error;
    REQUIRE(load_model_artifact(path,preview,colors,error));
    ModelImportRequest request;request.artifact.local_path=path;
    auto options=model_import_color_options(request);
    Model imported=Model::read_from_file(path);
    REQUIRE(apply_texture_import_units(imported,&options));
    REQUIRE(imported.objects.size()==1);
    auto normalized=preview.its;its_merge_vertices(normalized);its_compactify_vertices(normalized);
    const auto& target=imported.objects.front()->volumes.front()->mesh().its;
    WARN("source faces="<<preview.its.indices.size()<<" vertices="<<preview.its.vertices.size()
        <<" normalized vertices="<<normalized.vertices.size()<<" native faces="<<target.indices.size()<<" vertices="<<target.vertices.size());
    if(preview.its.indices.size()==target.indices.size()) {
        double max_delta=0;
        for(size_t f=0;f<target.indices.size();++f)for(int c=0;c<3;++c)
            max_delta=std::max(max_delta,double((preview.its.vertices[preview.its.indices[f][c]]-target.vertices[target.indices[f][c]]).cwiseAbs().maxCoeff()));
        WARN("ordered corner max delta before centering="<<max_delta);
    }
    ModelMatchedColors matched;matched.source_sha256=model_artifact_sha256(path);
    matched.geometry_id=SurfaceSelectionPersistence::geometry_fingerprint(preview.its);
    matched.palette={{0,"#FFFFFF","PLA",true},{1,"#000000","PLA",true}};
    for(size_t f=0;f<preview.its.indices.size();++f)matched.face_slots.push_back(f%2);
    request.matched_colors=matched;options=model_import_color_options(request);
    options.matched_source=std::make_shared<indexed_triangle_set>(preview.its);
    imported.objects.front()->center_around_origin(false);
    REQUIRE_NOTHROW(apply_matched_texture_colors(imported,options,options.matched_filaments));
}


TEST_CASE("Embedded JPEG textures retain their dimensions and projected colors", "[ModelArtifact][JPEG]") {
    const auto source = samples / "jpeg-textured.glb";
    TriangleMesh mesh; ObjInfo colors; std::string error;
    const bool loaded = load_model_artifact(source, mesh, colors, error);
    INFO(error);
    REQUIRE(loaded);
    REQUIRE(mesh.its.vertices.size() == 4);
    REQUIRE(mesh.its.indices.size() == 4);
    REQUIRE(colors.vertex_colors.size() == mesh.its.vertices.size());

    // Four quadrants in a 32 by 16 baseline JPEG, sampled at their centers.
    // Allow two code values for JPEG rounding, but catch swapped RGB/UV axes.
    const std::array<Vec3f, 4> positions { Vec3f(0, 0, 0), Vec3f(60, 0, 0), Vec3f(0, 0, 100), Vec3f(0, 40, 0) };
    const std::array<RGBA, 4> expected { RGBA{1, 0, 0, 1}, RGBA{0, 1, 0, 1}, RGBA{0, 0, 1, 1}, RGBA{1, 1, 1, 1} };
    for (size_t sample = 0; sample < positions.size(); ++sample) {
        const auto found = std::find_if(mesh.its.vertices.begin(), mesh.its.vertices.end(),
            [&](const Vec3f& v) { return (v - positions[sample]).norm() < .001f; });
        REQUIRE(found != mesh.its.vertices.end());
        const size_t index = size_t(std::distance(mesh.its.vertices.begin(), found));
        for (size_t channel = 0; channel < 3; ++channel)
            REQUIRE_THAT(colors.vertex_colors[index][channel], WithinAbs(expected[sample][channel], 2.0 / 255.0));
    }

    // Exercise the same image adapter headlessly, without wx image handlers.
    TexturedMesh textured;
    REQUIRE(load_assimp_textured_model(source.string(), textured, &error));
    REQUIRE(textured.textures.size() == 1);
    CHECK(textured.precomputed_vertex_colors.empty());
    CHECK(textured.precomputed_face_colors.empty());
    std::vector<unsigned char> pixels;
    int width = 0, height = 0;
    REQUIRE(decode_texture_to_pixels(textured.textures.front(), pixels, width, height));
    REQUIRE(width == 32);
    REQUIRE(height == 16);
    REQUIRE(pixels.size() == size_t(width) * height * 3);
}

TEST_CASE("Malformed or truncated embedded JPEG textures fail without publishing a model", "[ModelArtifact][JPEG]") {
    for (const std::string name : {"jpeg-malformed", "jpeg-truncated"}) {
        DYNAMIC_SECTION(name) {
            const auto source = samples / (name + ".glb");
            const auto hash = model_artifact_sha256(source);
            TriangleMesh mesh; ObjInfo colors; std::string error;
            bool loaded = true;
            REQUIRE_NOTHROW(loaded = load_model_artifact(source, mesh, colors, error));
            INFO(error);
            REQUIRE_FALSE(loaded);
            REQUIRE(error.find("texture") != std::string::npos);
            REQUIRE(mesh.empty());
            REQUIRE(colors.vertex_colors.empty());
            REQUIRE(model_artifact_sha256(source) == hash);
        }
    }
}

// Hidden because the input is an explicitly selected local asset, not a suite fixture.
// This probe only reads through the production loader; it does not edit or generate models.
TEST_CASE("An explicitly supplied local GLB loads without changing its source", "[ModelArtifact][.LocalArtifactProbe]") {
    const char* fixture = std::getenv("ORCASLICER_MODEL_ARTIFACT_FIXTURE");
    if (!fixture || !*fixture) SKIP("Set ORCASLICER_MODEL_ARTIFACT_FIXTURE to an existing local GLB.");
    const boost::filesystem::path source(fixture);
    REQUIRE(model_artifact_format(source) == "glb");
    const auto hash = model_artifact_sha256(source);
    REQUIRE_FALSE(hash.empty());
    TriangleMesh mesh; ObjInfo colors; std::string error;
    const auto started = std::chrono::steady_clock::now();
    const bool loaded = load_model_artifact(source, mesh, colors, error);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    INFO("Artifact: " << source.string() << "; SHA256: " << hash << "; load seconds: " << seconds << "; error: " << error);
    REQUIRE(loaded);
    REQUIRE_FALSE(mesh.empty());
    REQUIRE(colors.vertex_colors.size() == mesh.its.vertices.size());
    const bool finite_vertices = std::all_of(mesh.its.vertices.begin(), mesh.its.vertices.end(),
        [](const Vec3f& v) { return v.allFinite(); });
    const bool valid_colors = std::all_of(colors.vertex_colors.begin(), colors.vertex_colors.end(), [](const RGBA& color) {
        return std::all_of(color.begin(), color.end(), [](float value) { return std::isfinite(value) && value >= 0.f && value <= 1.f; });
    });
    INFO("Vertices: " << mesh.its.vertices.size() << "; triangles: " << mesh.its.indices.size());
    REQUIRE(finite_vertices);
    REQUIRE(valid_colors);
    REQUIRE(model_artifact_sha256(source) == hash);
}

TEST_CASE("Local GLB versions preserve geometry colors and the source file", "[ModelArtifact]") {
    Fixture f;
    const auto source = samples / "textured.glb";
    const auto hash = model_artifact_sha256(source);
    TriangleMesh mesh, loaded; ObjInfo colors, read_colors; std::string error;
    REQUIRE(load_model_artifact(source, mesh, colors, error));
    const auto output = f.directory / "edited.glb";
    colors.vertex_colors[0] = { .15f, .43f, .71f, 1.f };
    REQUIRE(write_model_artifact(output, mesh.its, colors.vertex_colors, error));
    REQUIRE(load_model_artifact(output, loaded, read_colors, error));
    REQUIRE(loaded.its.vertices.size() == mesh.its.vertices.size());
    for (size_t i = 0; i < mesh.its.vertices.size(); ++i) {
        size_t match = loaded.its.vertices.size();
        for (size_t j = 0; j < loaded.its.vertices.size(); ++j)
            if ((loaded.its.vertices[j] - mesh.its.vertices[i]).norm() < .001f) { match = j; break; }
        REQUIRE(match < loaded.its.vertices.size());
        for (int c = 0; c < 3; ++c) {
            REQUIRE_THAT(loaded.its.vertices[match][c], WithinAbs(mesh.its.vertices[i][c], .001));
            REQUIRE_THAT(read_colors.vertex_colors[match][c], WithinAbs(colors.vertex_colors[i][c], .00001));
        }
    }
    // File > Import and drag-and-drop use the default loader, without requesting
    // the optional raw Assimp colors used by the local artifact preview.
    Model native = Model::read_from_file(output.string());
    REQUIRE(native.texture_mesh);
    const auto& textured = *native.texture_mesh;
    REQUIRE(textured.textures.empty());
    REQUIRE(textured.precomputed_vertex_colors.size() == textured.vertices.size());
    REQUIRE(textured.precomputed_face_colors.size() == textured.indices.size());
    for (size_t i = 0; i < textured.vertices.size(); ++i) {
        const auto& p = textured.vertices[i];
        const Vec3f position = Vec3f(p[0], p[1], p[2]) * 1000.f;
        const auto found = std::find_if(mesh.its.vertices.begin(), mesh.its.vertices.end(),
            [&](const Vec3f& vertex) { return (vertex - position).norm() < .001f; });
        REQUIRE(found != mesh.its.vertices.end());
        const size_t original = size_t(std::distance(mesh.its.vertices.begin(), found));
        for (size_t channel = 0; channel < 3; ++channel)
            CHECK_THAT(textured.precomputed_vertex_colors[i][channel], WithinAbs(colors.vertex_colors[original][channel], .00001));
    }
    REQUIRE(model_artifact_sha256(source) == hash);
    REQUIRE_FALSE(write_model_artifact(output, mesh.its, colors.vertex_colors, error));
}

TEST_CASE("OBJ writer keeps repeated and signed-zero colors byte stable", "[ModelArtifact]") {
    Fixture f;
    indexed_triangle_set mesh;
    mesh.vertices = {Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(0, 1, 0), Vec3f(0, 0, 1)};
    mesh.indices = {Vec3i32(0, 1, 2), Vec3i32(0, 2, 3)};
    const std::vector<RGBA> colors = {{.25f, .5f, .75f, 1.f}, {.25f, .5f, .75f, 1.f},
                                      {-0.f, 0.f, 1.f, 1.f}, {0.f, 0.f, 1.f, 1.f}};
    const auto path = f.directory / "colors.obj";
    std::string error;
    REQUIRE(write_model_artifact(path, mesh, colors, error));
    boost::filesystem::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    const std::string actual((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    CHECK(actual == "# Orca AI model: Z-up millimetres, sRGB vertex colors\n"
                    "v 0 0 0 0.25 0.5 0.75\n"
                    "v 1 0 0 0.25 0.5 0.75\n"
                    "v 0 1 0 -0 0 1\n"
                    "v 0 0 1 0 0 1\n"
                    "f 1 2 3\n"
                    "f 1 3 4\n");
}

TEST_CASE("Ordinary GLB imports apply material factors to linear vertex colors", "[ModelArtifact]") {
    for (const std::string name : {"vertex-material-color", "ushort-vertex-colors"}) {
        DYNAMIC_SECTION(name) {
            Model native = Model::read_from_file((samples / (name + ".glb")).string());
            TriangleMesh expected; ObjInfo colors; std::string error;
            REQUIRE(load_model_artifact(samples / (name + ".obj"), expected, colors, error));
            REQUIRE(native.texture_mesh);
            const auto& textured = *native.texture_mesh;
            REQUIRE(textured.precomputed_vertex_colors.size() == textured.vertices.size());
            REQUIRE(textured.precomputed_face_colors.size() == textured.indices.size());
            REQUIRE_FALSE(colors.vertex_colors.empty());
            // These fixtures have a uniform COLOR_0 multiplied by a nonwhite factor.
            for (const auto& color : textured.precomputed_vertex_colors)
                for (size_t channel = 0; channel < 3; ++channel)
                    CHECK_THAT(color[channel], WithinAbs(colors.vertex_colors.front()[channel], .00001));
            for (const auto& color : textured.precomputed_face_colors)
                for (size_t channel = 0; channel < 3; ++channel)
                    CHECK_THAT(double(color[channel]), WithinAbs(colors.vertex_colors.front()[channel] * 255.f, 1.));
        }
    }
}

TEST_CASE("Locally recolored OBJ and GLB artifacts stay closed through native painting", "[ModelArtifact]") {
    for (const std::string extension : {"obj", "glb"}) {
        for (int split_edges : {0, 1, 2, 3}) {
            DYNAMIC_SECTION(extension << " with " << split_edges << " split edges on the recolored face") {
                Fixture fixture;
                indexed_triangle_set mesh;
                mesh.vertices = {Vec3f(0, 0, 0), Vec3f(20, 0, 0), Vec3f(0, 20, 0), Vec3f(0, 0, 20)};
                mesh.indices = {Vec3i32(0, 2, 1), Vec3i32(0, 1, 3), Vec3i32(1, 2, 3), Vec3i32(2, 0, 3)};
                const RGBA red{1, 0, 0, 1}, green{0, 1, 0, 1}, white{1, 1, 1, 1}, blue{0, 0, 1, 1};
                std::vector<RGBA> colors{red, red, red, green};
                if (split_edges >= 2) colors[1] = green;
                if (split_edges == 3) colors[2] = white;
                if (split_edges == 1) {
                    // A preexisting seam also makes a two-color neighbor split
                    // its equal-color edge to conform to the opposite side.
                    for (int corner = 0; corner < 3; ++corner) {
                        const Vec3f position = mesh.vertices[mesh.indices[1][corner]];
                        mesh.indices[1][corner] = int(mesh.vertices.size());
                        mesh.vertices.push_back(position);
                        colors.push_back(corner == 0 ? red : green);
                    }
                }
                std::string error;
                const auto source = fixture.directory / ("source." + extension);
                const auto output = fixture.directory / ("recolored." + extension);
                REQUIRE(write_model_artifact(source, mesh, colors, error));
                const auto source_hash = model_artifact_sha256(source);
                TriangleMesh loaded; ObjInfo loaded_colors;
                REQUIRE(load_model_artifact(source, loaded, loaded_colors, error));
                VertexColorRegionEditor editor;
                REQUIRE(editor.initialize(loaded.its, loaded_colors.vertex_colors, error));
                REQUIRE(editor.select_faces({0}) == 1);
                REQUIRE(editor.apply_color_to_obj_copy(blue, source, output, error));

                Model native = Model::read_from_file(output.string());
                REQUIRE(native.objects.size() == 1);
                REQUIRE(native.objects.front()->volumes.size() == 1);
                REQUIRE(native.texture_mesh);
                const auto& textured = *native.texture_mesh;
                REQUIRE(textured.precomputed_vertex_colors.size() == textured.vertices.size());
                REQUIRE(textured.precomputed_face_colors.size() == textured.indices.size());
                const auto& selected = textured.indices.front();
                const auto point = [&](int index) {
                    const auto& p = textured.vertices[index];
                    return Vec3f(p[0], p[1], p[2]);
                };
                const Vec3f origin = point(selected[0]);
                const Vec3f cross = (point(selected[1]) - origin).cross(point(selected[2]) - origin);
                const Vec3f normal = cross.normalized();
                TexturePaintingSettings settings;
                settings.fixed_palette = {{255, 0, 0}, {0, 255, 0}, {255, 255, 255}, {0, 0, 255}};
                settings.smooth_weight = 0;
                std::array<size_t, 3> selected_color{0, 0, 255};
                SECTION("saved face color") {}
                SECTION("explicit face color override") {
                    selected_color = {255, 255, 0};
                    settings.face_color_overrides.push_back({0, selected_color});
                }
                PaintedMesh painted;
                REQUIRE(face_colors_to_painting(textured, painted, settings));
                require_closed_painted_mesh(painted);
                size_t selected_children = 0;
                double selected_area = 0;
                for (size_t i = 0; i < painted.indices.size(); ++i) {
                    const auto& face = painted.indices[i];
                    const auto vertex = [&](int corner) {
                        const auto& p = painted.vertices[face[corner]];
                        return Vec3f(p[0], p[1], p[2]);
                    };
                    const Vec3f center = (vertex(0) + vertex(1) + vertex(2)) / 3.f;
                    if (std::abs((center - origin).dot(normal)) < 1e-6f) {
                        ++selected_children;
                        CHECK(painted.face_colors[i] == selected_color);
                        selected_area += (vertex(1) - vertex(0)).cross(vertex(2) - vertex(0)).norm();
                    } else {
                        CHECK(painted.face_colors[i] != selected_color);
                    }
                }
                CHECK(selected_children == size_t(split_edges + 1));
                CHECK_THAT(selected_area, WithinAbs(cross.norm(), cross.norm() * .00001));
                std::vector<FilamentMatch> matches(painted.cluster_colors.size());
                for (size_t i = 0; i < matches.size(); ++i) {
                    matches[i].cluster_index = int(i);
                    matches[i].filament_index = int(i);
                }
                auto& volume = *native.objects.front()->volumes.front();
                const double original_volume = its_volume(volume.mesh().its);
                REQUIRE(apply_painted_mesh_to_volume(painted, matches, volume));
                CHECK(volume.mesh().stats().manifold());
                CHECK_THAT(its_volume(volume.mesh().its), WithinAbs(original_volume, std::abs(original_volume) * .00001));
                CHECK_FALSE(volume.mmu_segmentation_facets.empty());
                CHECK(model_artifact_sha256(source) == source_hash);
            }
        }
    }
}

TEST_CASE("GLB finishing creates a separate version and supports cancellation", "[ModelArtifact]") {
    Fixture f;
    const auto source = samples / "textured.glb";
    const auto hash = model_artifact_sha256(source);
    ModelFinishingOptions options {true, false, .35};
    const auto output = f.directory / "finished.glb";
    auto result = finish_model_artifact(source, output, options, [] { return true; });
    REQUIRE(result.canceled);
    REQUIRE_FALSE(boost::filesystem::exists(output));
    result = finish_model_artifact(source, output, options);
    INFO(result.error);
    REQUIRE(result.success);
    REQUIRE(result.source_sha256 == hash);
    REQUIRE(result.output_sha256 == model_artifact_sha256(output));
    REQUIRE(model_artifact_sha256(source) == hash);
    REQUIRE_FALSE(boost::filesystem::exists(output.string() + ".source.obj"));
}

namespace {
using GlbJson = nlohmann::json;
struct GlbFixtureData { GlbJson doc; std::vector<unsigned char> binary; };
uint32_t glb_u32(const unsigned char* data) {
    return uint32_t(data[0]) | uint32_t(data[1]) << 8 | uint32_t(data[2]) << 16 | uint32_t(data[3]) << 24;
}
void glb_append_u32(std::vector<unsigned char>& bytes, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<unsigned char>(value >> shift));
}
GlbFixtureData read_glb_fixture(const boost::filesystem::path& path) {
    boost::filesystem::ifstream input(path, std::ios::binary);
    const std::vector<unsigned char> bytes {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    REQUIRE(bytes.size() >= 28);
    const size_t json_size = glb_u32(bytes.data() + 12);
    REQUIRE(json_size + 28 <= bytes.size());
    GlbFixtureData result;
    result.doc = GlbJson::parse(bytes.data() + 20, bytes.data() + 20 + json_size);
    const size_t binary_size = result.doc.at("buffers")[0].at("byteLength").get<size_t>();
    REQUIRE(json_size + binary_size + 28 <= bytes.size());
    result.binary.assign(bytes.begin() + 28 + json_size, bytes.begin() + 28 + json_size + binary_size);
    return result;
}
void save_glb_fixture(const boost::filesystem::path& path, GlbFixtureData fixture) {
    fixture.doc["buffers"][0]["byteLength"] = fixture.binary.size();
    std::string json = fixture.doc.dump();
    while (json.size() % 4) json += ' ';
    while (fixture.binary.size() % 4) fixture.binary.push_back(0);
    std::vector<unsigned char> bytes;
    glb_append_u32(bytes, 0x46546c67); glb_append_u32(bytes, 2); glb_append_u32(bytes, uint32_t(json.size() + fixture.binary.size() + 28));
    glb_append_u32(bytes, uint32_t(json.size())); glb_append_u32(bytes, 0x4e4f534a);
    bytes.insert(bytes.end(), json.begin(), json.end());
    glb_append_u32(bytes, uint32_t(fixture.binary.size())); glb_append_u32(bytes, 0x004e4942);
    bytes.insert(bytes.end(), fixture.binary.begin(), fixture.binary.end());
    boost::filesystem::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    output.close();
    REQUIRE(bool(output));
}
size_t add_float_accessor(GlbFixtureData& fixture, const std::vector<float>& values, size_t width) {
    while (fixture.binary.size() % 4) fixture.binary.push_back(0);
    const size_t start = fixture.binary.size(), view = fixture.doc["bufferViews"].size(), accessor = fixture.doc["accessors"].size();
    for (float value : values) { uint32_t bits; std::memcpy(&bits, &value, 4); glb_append_u32(fixture.binary, bits); }
    fixture.doc["bufferViews"].push_back({{"buffer", 0}, {"byteOffset", start}, {"byteLength", values.size() * 4}});
    fixture.doc["accessors"].push_back({{"bufferView", view}, {"componentType", 5126}, {"count", values.size() / width}, {"type", width == 2 ? "VEC2" : width == 4 ? "VEC4" : "VEC3"}});
    return accessor;
}
std::vector<Vec3f> glb_vectors(const GlbFixtureData& fixture, const char* semantic) {
    const size_t index = fixture.doc["meshes"][0]["primitives"][0]["attributes"][semantic].get<size_t>();
    const auto& accessor = fixture.doc["accessors"][index];
    const auto& view = fixture.doc["bufferViews"][accessor.at("bufferView").get<size_t>()];
    const size_t offset = view.value("byteOffset", size_t(0)) + accessor.value("byteOffset", size_t(0));
    const size_t stride = view.value("byteStride", size_t(12));
    std::vector<Vec3f> values(accessor.at("count").get<size_t>());
    for (size_t i = 0; i < values.size(); ++i) for (int c = 0; c < 3; ++c) {
        const uint32_t bits = glb_u32(fixture.binary.data() + offset + i * stride + c * 4);
        std::memcpy(&values[i][c], &bits, 4);
    }
    return values;
}
void require_glb_appearance_retained(const GlbFixtureData& original, const GlbFixtureData& edited) {
    REQUIRE(edited.binary.size() >= original.binary.size());
    REQUIRE(std::equal(original.binary.begin(), original.binary.end(), edited.binary.begin()));
    for (const char* field : {"materials", "images", "textures", "samplers", "nodes", "scenes", "scene", "extensionsUsed", "extensionsRequired"}) {
        INFO(field);
        REQUIRE(edited.doc.value(field, GlbJson()) == original.doc.value(field, GlbJson()));
    }
    const auto& before = original.doc["meshes"][0]["primitives"][0];
    const auto& after = edited.doc["meshes"][0]["primitives"][0];
    for (const char* attribute : {"TEXCOORD_0", "COLOR_0"})
        REQUIRE(after.at("attributes").value(attribute, GlbJson()) == before.at("attributes").value(attribute, GlbJson()));
    REQUIRE(after.value("indices", GlbJson()) == before.value("indices", GlbJson()));
    REQUIRE(after.value("material", GlbJson()) == before.value("material", GlbJson()));
}
GlbFixtureData noisy_glb_grid() {
    auto fixture = read_glb_fixture(samples / "baseline.glb");
    std::vector<float> positions, normals, uvs;
    for (int y = 0; y < 13; ++y) for (int x = 0; x < 13; ++x) {
        positions.insert(positions.end(), {float(x) * .001f, .00009f * float(std::sin(x * 1.7) * std::cos(y * 1.3)), float(-y) * .001f});
        normals.insert(normals.end(), {0, 1, 0});
        uvs.insert(uvs.end(), {float(x) / 12, float(y) / 12});
    }
    // Unreferenced source vertices must survive Assimp's used-vertex compaction.
    positions.insert(positions.end(), {.1f, .2f, .3f}); normals.insert(normals.end(), {0, 1, 0}); uvs.insert(uvs.end(), {.5f, .5f});
    auto& primitive = fixture.doc["meshes"][0]["primitives"][0];
    primitive["attributes"] = GlbJson::object();
    primitive["attributes"]["POSITION"] = add_float_accessor(fixture, positions, 3);
    primitive["attributes"]["NORMAL"] = add_float_accessor(fixture, normals, 3);
    primitive["attributes"]["TEXCOORD_0"] = add_float_accessor(fixture, uvs, 2);
    while (fixture.binary.size() % 4) fixture.binary.push_back(0);
    const size_t offset = fixture.binary.size(), view = fixture.doc["bufferViews"].size(), indices = fixture.doc["accessors"].size();
    for (int y = 0; y < 12; ++y) for (int x = 0; x < 12; ++x) {
        const uint32_t vertex = uint32_t(y * 13 + x);
        // The first encountered vertex is deliberately not accessor element 0.
        for (uint32_t index : {vertex + 1, vertex + 13, vertex, vertex + 1, vertex + 14, vertex + 13}) glb_append_u32(fixture.binary, index);
    }
    fixture.doc["bufferViews"].push_back({{"buffer", 0}, {"byteOffset", offset}, {"byteLength", fixture.binary.size() - offset}});
    fixture.doc["accessors"].push_back({{"bufferView", view}, {"componentType", 5125}, {"count", 12 * 12 * 6}, {"type", "SCALAR"}});
    fixture.doc["meshes"][0]["primitives"][0]["indices"] = indices;
    return fixture;
}
}

TEST_CASE("Untextured GLB repeats and changes vertex colors without stale material sampling", "[ModelArtifact]") {
    Fixture f;
    auto fixture = read_glb_fixture(samples / "baseline.glb");
    fixture.doc["materials"][0]["pbrMetallicRoughness"].erase("baseColorTexture");
    const std::array<std::array<float, 3>, 4> input = {{{.2f, .4f, .6f}, {.2f, .4f, .6f},
                                                        {.8f, .1f, .3f}, {.8f, .1f, .3f}}};
    std::vector<float> values;
    for (const auto& color : input) values.insert(values.end(), color.begin(), color.end());
    auto& primitive = fixture.doc["meshes"][0]["primitives"][0];
    primitive["attributes"]["COLOR_0"] = add_float_accessor(fixture, values, 3);
    const auto positions = glb_vectors(fixture, "POSITION");
    const auto source = f.directory / "repeated-colors.glb";
    save_glb_fixture(source, fixture);
    TriangleMesh mesh; ObjInfo colors; std::string error;
    REQUIRE(load_model_artifact(source, mesh, colors, error));
    REQUIRE(mesh.its.vertices.size() == input.size());
    REQUIRE(colors.vertex_colors.size() == input.size());
    TexturedMesh native;
    REQUIRE(load_assimp_textured_model(source.string(), native, &error));
    REQUIRE(native.precomputed_vertex_colors.size() == native.vertices.size());
    REQUIRE(native.precomputed_face_colors.size() == native.indices.size());
    for (size_t i = 0; i < input.size(); ++i) {
        const Vec3f position(positions[i].x() * 1000.f, -positions[i].z() * 1000.f,
                             positions[i].y() * 1000.f);
        const auto found = std::find_if(mesh.its.vertices.begin(), mesh.its.vertices.end(),
            [&](const Vec3f& v) { return (v - position).norm() < .001f; });
        REQUIRE(found != mesh.its.vertices.end());
        const auto& output = colors.vertex_colors[std::distance(mesh.its.vertices.begin(), found)];
        const auto native_found = std::find_if(native.vertices.begin(), native.vertices.end(), [&](const auto& v) {
            return (Vec3f(v[0] * 1000.f, -v[2] * 1000.f, v[1] * 1000.f) - position).norm() < .001f;
        });
        REQUIRE(native_found != native.vertices.end());
        const auto& native_color = native.precomputed_vertex_colors[std::distance(native.vertices.begin(), native_found)];
        for (size_t c = 0; c < 3; ++c) {
            const float linear = .5f * input[i][c];
            const float expected = linear <= .0031308f ? linear * 12.92f
                : 1.055f * std::pow(linear, 1.f / 2.4f) - .055f;
            CHECK_THAT(output[c], WithinAbs(expected, 1e-6));
            CHECK_THAT(native_color[c], WithinAbs(expected, 1e-6));
        }
    }

    const auto accessor_id = primitive["attributes"]["COLOR_0"].get<size_t>();
    const auto& accessor = fixture.doc["accessors"][accessor_id];
    const auto& view = fixture.doc["bufferViews"][accessor["bufferView"].get<size_t>()];
    const size_t offset = view.value("byteOffset", size_t(0)) + 3 * 12;
    const uint32_t nan_bits = 0x7fc00000u;
    std::memcpy(fixture.binary.data() + offset, &nan_bits, sizeof(nan_bits));
    const auto invalid = f.directory / "late-invalid-color.glb";
    save_glb_fixture(invalid, fixture);
    REQUIRE_FALSE(load_model_artifact(invalid, mesh, colors, error));
    REQUIRE_FALSE(load_assimp_textured_model(invalid.string(), native, &error));
    CHECK(native.vertices.empty());
    CHECK(native.precomputed_vertex_colors.empty());
}

TEST_CASE("Native GLB colors preserve alpha differences and signed zero", "[ModelArtifact]") {
    Fixture f;
    auto fixture = read_glb_fixture(samples / "baseline.glb");
    auto& material = fixture.doc["materials"][0]["pbrMetallicRoughness"];
    material.erase("baseColorTexture");
    material["baseColorFactor"] = {.5f, .5f, .5f, .5f};
    const std::array<RGBA, 4> input = {{{0.f, .25f, .75f, .2f}, {-0.f, .25f, .75f, .2f},
                                       {.8f, .1f, .3f, .4f}, {.8f, .1f, .3f, .9f}}};
    std::vector<float> values;
    for (const auto& color : input) values.insert(values.end(), color.begin(), color.end());
    auto& primitive = fixture.doc["meshes"][0]["primitives"][0];
    const size_t accessor_id = add_float_accessor(fixture, values, 4);
    primitive["attributes"]["COLOR_0"] = accessor_id;
    const auto positions = glb_vectors(fixture, "POSITION");
    const auto source = f.directory / "alpha-colors.glb";
    save_glb_fixture(source, fixture);
    std::string error;
    for (bool request_raw : {false, true})
        for (auto policy : {AssimpRawColorPolicy::Always, AssimpRawColorPolicy::FallbackOnly}) {
        DYNAMIC_SECTION("raw colors requested " << request_raw << " policy " << int(policy)) {
            TexturedMesh native; std::vector<RGBA> raw;
            REQUIRE(load_assimp_textured_model(source.string(), native, &error, request_raw ? &raw : nullptr, policy));
            REQUIRE(native.precomputed_vertex_colors.size() == native.vertices.size());
            if (request_raw && policy == AssimpRawColorPolicy::Always)
                REQUIRE(raw.size() == native.vertices.size());
            else
                REQUIRE(raw.empty());
            TriangleMesh edited; ObjInfo edited_colors;
            REQUIRE(load_model_artifact(source, edited, edited_colors, error));
            REQUIRE(edited_colors.vertex_colors.size() == native.vertices.size());
            for (size_t i = 0; i < input.size(); ++i) {
                const auto found = std::find_if(native.vertices.begin(), native.vertices.end(), [&](const auto& v) {
                    return (Vec3f(v[0], v[1], v[2]) - positions[i]).norm() < 1e-6f;
                });
                REQUIRE(found != native.vertices.end());
                const size_t vertex = std::distance(native.vertices.begin(), found);
                const auto& color = native.precomputed_vertex_colors[vertex];
                if (!raw.empty()) for (size_t channel = 0; channel < 4; ++channel) {
                    CHECK_THAT(raw[vertex][channel], WithinAbs(input[i][channel], 1e-6));
                    CHECK(std::signbit(raw[vertex][channel]) == std::signbit(input[i][channel]));
                }
                CHECK_THAT(color[3], WithinAbs(.5f * input[i][3], 1e-6));
                if (i < 2) CHECK(std::signbit(color[0]) == std::signbit(input[i][0]));
                const auto& edited_color = edited_colors.vertex_colors[std::distance(native.vertices.begin(), found)];
                CHECK_THAT(edited_color[3], WithinAbs(1.f, 1e-6));
                for (size_t c = 0; c < 3; ++c)
                    CHECK_THAT(edited_color[c], WithinAbs(color[c], 1e-6));
                if (i < 2) CHECK(std::signbit(edited_color[0]) == std::signbit(input[i][0]));
            }
        }
    }
    const auto& accessor = fixture.doc["accessors"][accessor_id];
    const auto& view = fixture.doc["bufferViews"][accessor["bufferView"].get<size_t>()];
    const size_t offset = view.value("byteOffset", size_t(0)) + 3 * 16 + 3 * sizeof(float);
    const uint32_t nan_bits = 0x7fc00000u;
    std::memcpy(fixture.binary.data() + offset, &nan_bits, sizeof(nan_bits));
    const auto invalid = f.directory / "late-invalid-alpha.glb";
    save_glb_fixture(invalid, fixture);
    for (auto policy : {AssimpRawColorPolicy::Always, AssimpRawColorPolicy::FallbackOnly}) {
        TexturedMesh rejected; std::vector<RGBA> raw {{9.f, 9.f, 9.f, 9.f}};
        REQUIRE_FALSE(load_assimp_textured_model(invalid.string(), rejected, &error, &raw, policy));
        CHECK(rejected.precomputed_vertex_colors.empty());
        CHECK(rejected.vertices.empty());
    }
}

TEST_CASE("Fallback raw color requests retain texture and unconverted scene colors", "[ModelArtifact]") {
    for (const std::string scene : {"textured", "no-colors", "mixed"}) {
        DYNAMIC_SECTION(scene) {
            Fixture f;
            auto fixture = read_glb_fixture(samples / "baseline.glb");
            auto& primitive = fixture.doc["meshes"][0]["primitives"][0];
            if (scene == "no-colors") {
                primitive["attributes"].erase("COLOR_0");
                fixture.doc["materials"][0]["pbrMetallicRoughness"].erase("baseColorTexture");
            } else {
                primitive["attributes"]["COLOR_0"] = add_float_accessor(fixture,
                    {0.f, .25f, .75f, .2f, -0.f, .25f, .75f, .2f,
                     .8f, .1f, .3f, .4f, .8f, .1f, .3f, .9f}, 4);
                if (scene == "mixed") {
                    auto untextured = fixture.doc["materials"][0];
                    untextured["pbrMetallicRoughness"].erase("baseColorTexture");
                    fixture.doc["materials"].push_back(untextured);
                    auto other = primitive;
                    other["material"] = 1;
                    fixture.doc["meshes"][0]["primitives"].push_back(other);
                }
            }
            const auto source = f.directory / (scene + ".glb");
            save_glb_fixture(source, fixture);
            TexturedMesh reference, candidate;
            std::vector<RGBA> always {{9.f, 9.f, 9.f, 9.f}}, fallback = always;
            std::string error;
            REQUIRE(load_assimp_textured_model(source.string(), reference, &error, &always));
            REQUIRE(load_assimp_textured_model(source.string(), candidate, &error, &fallback,
                AssimpRawColorPolicy::FallbackOnly));
            REQUIRE(reference.precomputed_vertex_colors.empty());
            REQUIRE(candidate.precomputed_vertex_colors.empty());
            REQUIRE(always.size() == reference.vertices.size());
            REQUIRE(fallback.size() == always.size());
            CHECK(std::memcmp(always.data(), fallback.data(), always.size() * sizeof(RGBA)) == 0);
            CHECK(candidate.vertices == reference.vertices);
            CHECK(candidate.indices == reference.indices);
            CHECK(candidate.uvs == reference.uvs);
            CHECK(candidate.material_ids == reference.material_ids);
            CHECK(candidate.material_texture_map == reference.material_texture_map);
            TriangleMesh edited; ObjInfo colors;
            REQUIRE(load_model_artifact(source, edited, colors, error));
            REQUIRE(colors.vertex_colors.size() == edited.its.vertices.size());
        }
    }
}

TEST_CASE("Untextured GLB primitives retain their own colors when unused source vertices are discarded", "[ModelArtifact]") {
    Fixture f;
    auto fixture = read_glb_fixture(samples / "baseline.glb");
    auto& first_material = fixture.doc["materials"][0]["pbrMetallicRoughness"];
    first_material.erase("baseColorTexture");
    first_material["baseColorFactor"] = {.5f, .5f, .5f, .5f};
    auto second_material = fixture.doc["materials"][0];
    second_material["pbrMetallicRoughness"]["baseColorFactor"] = {.25f, .25f, .25f, .25f};
    fixture.doc["materials"].push_back(second_material);
    auto positions = glb_vectors(fixture, "POSITION");
    positions.emplace_back(3.f, 4.f, 5.f);
    std::vector<float> coordinates, colors, normals;
    for (const auto& position : positions) {
        for (int c = 0; c < 3; ++c) coordinates.push_back(position[c]);
        colors.insert(colors.end(), {.8f, .1f, .3f, .4f});
        normals.insert(normals.end(), {0.f, 0.f, 1.f});
    }
    auto& primitives = fixture.doc["meshes"][0]["primitives"];
    auto primitive = primitives[0];
    primitive["attributes"].erase("TEXCOORD_0");
    primitive["attributes"]["POSITION"] = add_float_accessor(fixture, coordinates, 3);
    primitive["attributes"]["NORMAL"] = add_float_accessor(fixture, normals, 3);
    primitive["attributes"]["COLOR_0"] = add_float_accessor(fixture, colors, 4);
    primitive["material"] = 0;
    primitives[0] = primitive;
    primitive["material"] = 1;
    primitives.push_back(primitive);
    const auto source = f.directory / "unused-multimaterial.glb";
    save_glb_fixture(source, fixture);
    TexturedMesh native; TriangleMesh edited; ObjInfo edited_colors; std::string error;
    REQUIRE(load_assimp_textured_model(source.string(), native, &error));
    REQUIRE(load_model_artifact(source, edited, edited_colors, error));
    REQUIRE(edited.its.vertices.size() == native.vertices.size());
    REQUIRE(edited_colors.vertex_colors.size() == native.vertices.size());
    std::vector<bool> referenced(native.vertices.size(), false);
    for (const auto& face : native.indices) for (int vertex : face) referenced[vertex] = true;
    // Assimp discards the unused source vertex even with explicit normals.
    // Check that removing it does not blend the two materials' colors.
    CHECK(std::none_of(native.vertices.begin(), native.vertices.end(), [](const auto& v) {
        return v[0] == 3.f && v[1] == 4.f && v[2] == 5.f;
    }));
    REQUIRE(std::set<int>(native.material_ids.begin(), native.material_ids.end()).size() == 2);
    REQUIRE(native.precomputed_vertex_colors.size() == native.vertices.size());
    for (size_t i = 0; i < native.vertices.size(); ++i) {
        for (size_t c = 0; c < 3; ++c) {
            const float expected = referenced[i] ? native.precomputed_vertex_colors[i][c] : 1.f;
            CHECK_THAT(edited_colors.vertex_colors[i][c], WithinAbs(expected, 1e-6));
        }
        CHECK_THAT(edited_colors.vertex_colors[i][3], WithinAbs(1.f, 1e-6));
    }
}

TEST_CASE("GLB editing rejects a missing declared texture even with vertex colors", "[ModelArtifact]") {
    Fixture f;
    auto fixture = read_glb_fixture(samples / "baseline.glb");
    const std::vector<float> colors(16, .5f);
    fixture.doc["meshes"][0]["primitives"][0]["attributes"]["COLOR_0"] = add_float_accessor(fixture, colors, 4);
    fixture.doc["textures"][0]["source"] = 99;
    const auto source = f.directory / "missing-color-texture.glb";
    save_glb_fixture(source, fixture);
    TriangleMesh mesh; ObjInfo info; std::string error;
    REQUIRE_FALSE(load_model_artifact(source, mesh, info, error));
    REQUIRE_FALSE(error.empty());
}

TEST_CASE("GLB geometry edits retain embedded appearance through accessor remapping and node transforms", "[ModelArtifact][GlbGeometry]") {
    for (const std::string name : {"baseline", "nested-negative-nodes", "nested-negative-nodes-prepared"}) {
        DYNAMIC_SECTION(name) {
            Fixture f;
            const auto path = samples / (name + ".glb");
            const auto original = read_glb_fixture(path);
            const auto hash = model_artifact_sha256(path);
            TriangleMesh mesh, loaded; ObjInfo colors, after_colors; std::string error;
            REQUIRE(load_model_artifact(path, mesh, colors, error));
            const auto source = read_glb_geometry_source(path, mesh.its);
            auto edited = mesh.its;
            edited.vertices[0].x() += .025f;
            const auto output = f.directory / "retained.glb";
            REQUIRE_NOTHROW(write_glb_geometry_edit(*source, output, edited, {}));
            require_glb_appearance_retained(original, read_glb_fixture(output));
            REQUIRE(load_model_artifact(output, loaded, after_colors, error));
            REQUIRE(loaded.its.indices == edited.indices);
            REQUIRE(loaded.its.vertices.size() == edited.vertices.size());
            for (size_t i = 0; i < edited.vertices.size(); ++i) {
                REQUIRE_THAT((loaded.its.vertices[i] - edited.vertices[i]).norm(), WithinAbs(0, .0002));
                REQUIRE(after_colors.vertex_colors[i] == colors.vertex_colors[i]);
            }
            REQUIRE(model_artifact_sha256(path) == hash);
        }
    }
}

TEST_CASE("GLB local smoothing retains texture bytes and outside normals while moving selected geometry", "[ModelArtifact][GlbGeometry][WorkbenchTextureImport]") {
    Fixture f;
    const auto input = f.directory / "source.glb", output = f.directory / "smoothed.glb";
    const auto original = noisy_glb_grid();
    save_glb_fixture(input, original);
    const auto hash = model_artifact_sha256(input);
    ModelFinishingOptions options {true, false, .8};
    for (size_t y = 3; y < 9; ++y) for (size_t x = 3; x < 9; ++x) {
        options.selected_faces.push_back((y * 12 + x) * 2);
        options.selected_faces.push_back((y * 12 + x) * 2 + 1);
    }
    const auto result = finish_model_artifact(input, output, options);
    INFO(result.error);
    REQUIRE(result.success);
    REQUIRE(result.moved_vertices > 0);
    REQUIRE(result.faces_before == result.faces_after);
    ModelImportRequest request;
    request.artifact.local_path = output;
    const auto import_options = model_import_color_options(request);
    Model imported = Model::read_from_file(output.string());
    REQUIRE(apply_texture_import_units(imported, &import_options));
    REQUIRE(imported.texture_mesh);
    const auto edited = read_glb_fixture(output);
    require_glb_appearance_retained(original, edited);
    const auto before = glb_vectors(original, "POSITION"), after = glb_vectors(edited, "POSITION");
    const auto normals_before = glb_vectors(original, "NORMAL"), normals_after = glb_vectors(edited, "NORMAL");
    REQUIRE(after.size() == before.size());
    size_t moved = 0;
    for (size_t i = 0; i < before.size(); ++i) {
        if ((after[i] - before[i]).norm() > 1e-10f) ++moved;
        if (i == 169 || i % 13 <= 3 || i % 13 >= 9 || i / 13 <= 3 || i / 13 >= 9) {
            REQUIRE(after[i] == before[i]);
            REQUIRE(normals_after[i] == normals_before[i]);
        }
    }
    REQUIRE(moved > 0);
    REQUIRE(model_artifact_sha256(input) == hash);
    REQUIRE_FALSE(boost::filesystem::exists(output.string() + ".source.obj"));
    REQUIRE_FALSE(boost::filesystem::exists(output.string() + ".edited.obj"));
}

TEST_CASE("GLB smoothing preserves optional specular materials and retains required-extension checks", "[ModelArtifact][GlbGeometry]") {
    for (bool required : {false, true}) {
        DYNAMIC_SECTION("required=" << required) {
            Fixture f;
            auto original = noisy_glb_grid();
            original.doc["extensionsUsed"] = {"KHR_materials_specular"};
            original.doc["extensionsRequired"] = required ? GlbJson {"KHR_materials_specular"} : GlbJson::array();
            original.doc["materials"][0]["extensions"] = {{"KHR_materials_specular", {
                {"specularFactor", 0}, {"specularColorFactor", {.2, .4, .6}},
                {"specularTexture", {{"index", 0}}}, {"specularColorTexture", {{"index", 0}}}
            }}};
            const auto input = f.directory / "specular.glb", output = f.directory / "smoothed.glb";
            save_glb_fixture(input, original);
            const auto hash = model_artifact_sha256(input);
            const auto result = finish_model_artifact(input, output, {true, false, .8});
            INFO(result.error);
            if (required) {
                // The color importer does not implement specular shading; it
                // must still reject assets that require that rendering model.
                REQUIRE_FALSE(result.success);
                REQUIRE(result.error == "Unsupported GLB extension: KHR_materials_specular");
                REQUIRE_FALSE(boost::filesystem::exists(output));
            } else {
                REQUIRE(result.success);
                REQUIRE(result.moved_vertices > 0);
                REQUIRE(result.faces_before == result.faces_after);
                require_glb_appearance_retained(original, read_glb_fixture(output));
            }
            REQUIRE(model_artifact_sha256(input) == hash);
        }
    }
}

TEST_CASE("GLB preservation rejects unsupported structures and unverified editor mappings before writing", "[ModelArtifact][GlbGeometry]") {
    const auto original = read_glb_fixture(samples / "baseline.glb");
    TriangleMesh mesh; ObjInfo colors; std::string error;
    REQUIRE(load_model_artifact(samples / "baseline.glb", mesh, colors, error));
    for (const std::string variant : {"multiple-primitives", "instances", "sparse", "quantized", "tangents", "compressed", "singular", "shared", "bad-offset", "changed-vertices", "changed-topology"}) {
        DYNAMIC_SECTION(variant) {
            Fixture f;
            auto fixture = original;
            auto editor = mesh.its;
            auto& primitive = fixture.doc["meshes"][0]["primitives"][0];
            const size_t position = primitive["attributes"]["POSITION"].get<size_t>();
            if (variant == "multiple-primitives") fixture.doc["meshes"][0]["primitives"].push_back(primitive);
            if (variant == "instances") { fixture.doc["nodes"].push_back({{"mesh", 0}, {"translation", {1, 0, 0}}}); fixture.doc["scenes"][0]["nodes"].push_back(1); }
            if (variant == "sparse") fixture.doc["accessors"][position]["sparse"] = {{"count", 1}};
            if (variant == "quantized") fixture.doc["accessors"][position]["componentType"] = 5123;
            if (variant == "tangents") primitive["attributes"]["TANGENT"] = position;
            if (variant == "compressed") primitive["extensions"] = {{"KHR_draco_mesh_compression", {{"bufferView", 0}}}};
            if (variant == "singular") fixture.doc["nodes"][0]["scale"] = {0, 1, 1};
            if (variant == "shared") primitive["attributes"]["NORMAL"] = position;
            if (variant == "bad-offset") fixture.doc["accessors"][position]["byteOffset"] = 1000000;
            if (variant == "changed-vertices") editor.vertices[0].x() += 1;
            if (variant == "changed-topology") editor.indices[0][0] = editor.indices[0][1];
            const auto path = f.directory / "source.glb";
            save_glb_fixture(path, fixture);
            const auto hash = model_artifact_sha256(path);
            REQUIRE_THROWS_AS(read_glb_geometry_source(path, editor), std::exception);
            REQUIRE(model_artifact_sha256(path) == hash);
        }
    }
    Fixture f;
    const auto result = finish_model_artifact(samples / "multi-material.glb", f.directory / "unsupported.glb", {true, false, .5});
    REQUIRE_FALSE(result.success);
    REQUIRE_FALSE(boost::filesystem::exists(f.directory / "unsupported.glb"));
    REQUIRE_FALSE(boost::filesystem::exists(f.directory / "unsupported.glb.source.obj"));
}

TEST_CASE("Canceling or invalidating a GLB edit preserves sources and earlier versions", "[ModelArtifact][GlbGeometry]") {
    Fixture f;
    const auto input = f.directory / "source.glb", output = f.directory / "new.glb";
    const auto original = read_glb_fixture(samples / "baseline.glb");
    save_glb_fixture(input, original);
    TriangleMesh mesh; ObjInfo colors; std::string error;
    REQUIRE(load_model_artifact(input, mesh, colors, error));
    const auto source = read_glb_geometry_source(input, mesh.its);
    const auto hash = model_artifact_sha256(input);
    auto edited = mesh.its;
    edited.vertices[0].x() += .02f;
    struct Cancel {};
    REQUIRE_THROWS_AS(write_glb_geometry_edit(*source, output, edited, {}, [&] {
        if (boost::filesystem::exists(output.string() + ".partial")) throw Cancel {};
    }), Cancel);
    REQUIRE_FALSE(boost::filesystem::exists(output));
    REQUIRE_FALSE(boost::filesystem::exists(output.string() + ".partial"));
    REQUIRE(model_artifact_sha256(input) == hash);
    auto altered = original; altered.doc["asset"]["generator"] = "A newer source version";
    save_glb_fixture(input, altered);
    REQUIRE_THROWS_AS(write_glb_geometry_edit(*source, output, edited, {}), std::exception);
    REQUIRE_FALSE(boost::filesystem::exists(output));
    REQUIRE_FALSE(boost::filesystem::exists(output.string() + ".partial"));
    const auto current = read_glb_geometry_source(input, mesh.its);
    REQUIRE_NOTHROW(write_glb_geometry_edit(*current, output, edited, {}));
    const auto output_hash = model_artifact_sha256(output);
    REQUIRE_THROWS_AS(write_glb_geometry_edit(*current, output, edited, {}), std::exception);
    REQUIRE(model_artifact_sha256(output) == output_hash);
    auto topology = edited; topology.indices.pop_back();
    REQUIRE_THROWS_AS(write_glb_geometry_edit(*current, f.directory / "topology.glb", topology, {}), std::exception);
    REQUIRE_THROWS_AS(write_glb_geometry_edit(*current, f.directory / "outside.glb", edited, {0}), std::exception);
}

TEST_CASE("An existing local textured GLB retains its appearance payload through actual smoothing", "[ModelArtifact][GlbGeometry][.LocalGlbFinishingProbe]") {
    const char* fixture = std::getenv("ORCASLICER_MODEL_ARTIFACT_FIXTURE");
    if (!fixture || !*fixture) SKIP("Set ORCASLICER_MODEL_ARTIFACT_FIXTURE to an existing local static GLB.");
    Fixture f;
    const boost::filesystem::path original_path(fixture);
    const auto hash = model_artifact_sha256(original_path);
    REQUIRE_FALSE(hash.empty());
    const auto source = f.directory / "source.glb", output = f.directory / "smoothed.glb";
    boost::filesystem::copy_file(original_path, source);
    const auto before = read_glb_fixture(source);
    REQUIRE_FALSE(before.doc.value("images", GlbJson::array()).empty());
    const auto started = std::chrono::steady_clock::now();
    const auto result = finish_model_artifact(source, output, {true, false, .65});
    INFO("Source " << original_path.string() << "; SHA256 " << hash << "; error " << result.error);
    REQUIRE(result.success);
    REQUIRE(result.moved_vertices > 0);
    REQUIRE(result.faces_before == result.faces_after);
    const auto after = read_glb_fixture(output);
    require_glb_appearance_retained(before, after);
    const auto before_positions = glb_vectors(before, "POSITION"), after_positions = glb_vectors(after, "POSITION");
    REQUIRE(before_positions.size() == after_positions.size());
    size_t moved = 0;
    for (size_t i = 0; i < before_positions.size(); ++i) if ((before_positions[i] - after_positions[i]).norm() > 1e-10f) ++moved;
    REQUIRE(moved > 0);
    REQUIRE(model_artifact_sha256(original_path) == hash);
    REQUIRE(model_artifact_sha256(source) == hash);
    TriangleMesh reloaded; ObjInfo reloaded_colors; std::string error;
    REQUIRE(load_model_artifact(output, reloaded, reloaded_colors, error));
    REQUIRE(reloaded.its.indices.size() == result.faces_after);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    SUCCEED("GLB source SHA256 " << hash << "; output SHA256 " << result.output_sha256 << "; source vertices moved " << moved
        << "; faces " << result.faces_after << "; original appearance bytes retained " << before.binary.size() << "; elapsed seconds " << seconds);
}

TEST_CASE("A GLB source changed after its bytes are cached cannot become an editing snapshot", "[ModelArtifact][GlbGeometry]") {
    Fixture f;
    const auto path = f.directory / "source.glb";
    const auto original = read_glb_fixture(samples / "baseline.glb");
    save_glb_fixture(path, original);
    TriangleMesh mesh; ObjInfo colors; std::string error;
    REQUIRE(load_model_artifact(path, mesh, colors, error));
    auto newer = original; newer.doc["asset"]["generator"] = "Changed after source bytes were read";
    size_t checkpoints = 0;
    bool changed = false;
    REQUIRE_THROWS_AS(read_glb_geometry_source(path, mesh.its, [&] {
        // The small fixture fits in one read. The next checkpoint is inside
        // accessor decoding, after all original bytes have been cached.
        if (++checkpoints == 3) { save_glb_fixture(path, newer); changed = true; }
    }), std::exception);
    REQUIRE(changed);
    REQUIRE(read_glb_fixture(path).doc == newer.doc);
}

TEST_CASE("Publishing a GLB never overwrites a version created at the final checkpoint", "[ModelArtifact][GlbGeometry]") {
    Fixture f;
    const auto path = samples / "baseline.glb", output = f.directory / "new.glb";
    TriangleMesh mesh; ObjInfo colors; std::string error;
    REQUIRE(load_model_artifact(path, mesh, colors, error));
    const auto source = read_glb_geometry_source(path, mesh.its);
    auto edited = mesh.its; edited.vertices[0].x() += .025f;
    size_t completed_checkpoints = 0;
    bool created = false;
    std::string earlier_hash;
    REQUIRE_THROWS_AS(write_glb_geometry_edit(*source, output, edited, {}, [&] {
        const boost::filesystem::path partial(output.string() + ".partial");
        if (!boost::filesystem::exists(partial) || boost::filesystem::file_size(partial) == 0) return;
        // For this one-chunk fixture these are the completed-write, source-
        // recheck, and final publication checkpoints. Create after the last
        // destination existence check to exercise no-overwrite publication.
        if (++completed_checkpoints == 3) {
            boost::filesystem::copy_file(path, output);
            earlier_hash = model_artifact_sha256(output);
            created = true;
        }
    }), std::exception);
    REQUIRE(created);
    REQUIRE(model_artifact_sha256(output) == earlier_hash);
    REQUIRE_FALSE(boost::filesystem::exists(output.string() + ".partial"));
}

TEST_CASE("GLB regional recoloring preserves unselected colors and the source editor", "[ModelArtifact]") {
    Fixture f;
    const auto source = samples / "textured.glb";
    const auto hash = model_artifact_sha256(source);
    TriangleMesh mesh, edited; ObjInfo colors, edited_colors; std::string error;
    REQUIRE(load_model_artifact(source, mesh, colors, error));
    VertexColorRegionEditor editor;
    REQUIRE(editor.initialize(mesh.its, colors.vertex_colors, error));
    REQUIRE(editor.select_faces({0}) == 1);
    const RGBA target { .12f, .34f, .56f, 1.f };
    const auto output = f.directory / "recolored.glb";
    REQUIRE(editor.apply_color_to_obj_copy(target, source, output, error));
    REQUIRE(load_model_artifact(output, edited, edited_colors, error));
    REQUIRE(editor.vertex_colors() == colors.vertex_colors);
    REQUIRE(edited.its.indices.size() == mesh.its.indices.size());
    REQUIRE(edited_colors.vertex_colors.size() == edited.its.vertices.size());
    // Boundary vertices may split to keep the unselected side's color. Check
    // each face corner rather than assuming exported vertex indices are stable.
    for (size_t face = 0; face < mesh.its.indices.size(); ++face) {
        for (int corner = 0; corner < 3; ++corner) {
            const int original = mesh.its.indices[face][corner];
            const int derived = edited.its.indices[face][corner];
            for (int c = 0; c < 3; ++c) {
                REQUIRE_THAT(edited.its.vertices[derived][c], WithinAbs(mesh.its.vertices[original][c], .001));
                REQUIRE_THAT(edited_colors.vertex_colors[derived][c],
                             WithinAbs(face == 0 ? target[c] : colors.vertex_colors[original][c], .00001));
            }
        }
    }
    REQUIRE(model_artifact_sha256(source) == hash);
}

TEST_CASE("Repeated GLB materials keep independent texture transforms across faces and loads", "[ModelArtifact]") {
    Fixture f;
    auto plain = read_glb_fixture(samples / "textured.glb");
    const auto rotated = read_glb_fixture(samples / "uv-rotation.glb");
    plain.doc["materials"].push_back(rotated.doc["materials"][0]);
    auto primitive = plain.doc["meshes"][0]["primitives"][0];
    plain.doc["meshes"][0]["primitives"] = GlbJson::array();
    for (int material : {0, 1, 0, 1}) {
        primitive["material"] = material;
        plain.doc["meshes"][0]["primitives"].push_back(primitive);
    }
    plain.doc["extensionsUsed"] = {"KHR_texture_transform"};
    const auto path = f.directory / "repeated.glb";
    save_glb_fixture(path, plain);
    // Compare face positions AND unquantized RGBA; palette agreement alone
    // could conceal a changed texture sample. Face/vertex ordering is incidental.
    const auto painted_faces = [](const boost::filesystem::path& input) {
        TriangleMesh mesh; ObjInfo colors; std::string error;
        const bool loaded = load_model_artifact(input, mesh, colors, error);
        INFO(error); REQUIRE(loaded);
        std::multiset<std::array<float, 21>> faces;
        for (const auto& face : mesh.its.indices) {
            std::array<std::array<float, 7>, 3> corners;
            for (int k = 0; k < 3; ++k) {
                for (int c = 0; c < 3; ++c) corners[k][c] = mesh.its.vertices[face[k]][c];
                for (int c = 0; c < 4; ++c) corners[k][c+3] = colors.vertex_colors[face[k]][c];
            }
            std::sort(corners.begin(), corners.end());
            std::array<float, 21> record;
            for (int k = 0; k < 3; ++k) std::copy(corners[k].begin(), corners[k].end(), record.begin()+k*7);
            faces.insert(record);
        }
        return faces;
    };
    const auto original = painted_faces(samples / "textured.glb");
    const auto rotation = painted_faces(samples / "uv-rotation.glb");
    REQUIRE(original != rotation);
    auto expected = original;
    expected.insert(original.begin(), original.end());
    for (int i = 0; i < 2; ++i) expected.insert(rotation.begin(), rotation.end());
    CHECK(painted_faces(path) == expected);
    CHECK(painted_faces(samples / "textured.glb") == original);
}

TEST_CASE("A later GLB material still rejects an invalid sampler and can be repaired", "[ModelArtifact]") {
    Fixture f;
    auto fixture = read_glb_fixture(samples / "multi-material.glb");
    fixture.doc["samplers"].push_back({{"wrapS", 123}, {"wrapT", 33071}});
    fixture.doc["textures"].push_back({{"source", 0}, {"sampler", 1}});
    fixture.doc["materials"][1]["pbrMetallicRoughness"]["baseColorTexture"]["index"] = 1;
    const auto path = f.directory / "sampler.glb";
    save_glb_fixture(path, fixture);
    TriangleMesh mesh; ObjInfo colors; std::string error;
    CHECK_FALSE(load_model_artifact(path, mesh, colors, error));
    CHECK(error.find("wrapping mode") != std::string::npos);
    fixture.doc["samplers"][1]["wrapS"] = 33071;
    save_glb_fixture(path, fixture);
    REQUIRE(load_model_artifact(path, mesh, colors, error));
    CHECK(error.empty());
}

TEST_CASE("Canceling a partially read OBJ releases its file and discards parsed data", "[ModelArtifact]") {
    Fixture f;
    const auto path = f.directory / "partial.obj";
    {
        boost::filesystem::ofstream output(path);
        output << "v 0 0 0 1 0 0\nv 1 0 0 0 1 0\nv 0 1 0 0 0 1\nv 0 0 1 1 1 1\n";
        for (int i = 0; i < 6000; ++i)
            output << "f 1 3 2\nf 1 2 4\nf 2 3 4\nf 3 1 4\n";
        REQUIRE(output.good());
    }
    const auto hash = model_artifact_sha256(path);
    ObjParser::ObjData parsed;
    REQUIRE_FALSE(ObjParser::objparse(path.string().c_str(), parsed, [&] { return !parsed.coordinates.empty(); }));
    CHECK(parsed.coordinates.empty());
    CHECK(parsed.vertices.empty());
    CHECK_FALSE(parsed.has_vertex_color);
    // Windows refuses this rename if the parser still holds its FILE handle.
    const auto renamed = f.directory / "retry.obj";
    boost::filesystem::rename(path, renamed);
    CHECK(model_artifact_sha256(renamed) == hash);
    REQUIRE(ObjParser::objparse(renamed.string().c_str(), parsed));
    CHECK(parsed.coordinates.size() == 4 * OBJ_VERTEX_LENGTH);
    CHECK(parsed.has_vertex_color);
    CHECK(parsed.vertices.size() == 6000 * 4 * ONE_FACE_SIZE);
    REQUIRE_FALSE(ObjParser::objparse((f.directory / "missing.obj").string().c_str(), parsed, [] { return true; }));
    CHECK(parsed.coordinates.empty());
    CHECK(parsed.vertices.empty());
}

TEST_CASE("Canceled OBJ loading clears old outputs and retries with identical geometry and colors", "[ModelArtifact]") {
    Fixture f;
    const auto path = f.directory / "colored.obj";
    {
        boost::filesystem::ofstream output(path);
        output << "v 0 0 0 1 0 0\nv 1 0 0 0 1 0\nv 0 1 0 0 0 1\nv 0 0 1 1 1 1\n";
        for (int i = 0; i < 6000; ++i)
            output << "f 1 3 2\nf 1 2 4\nf 2 3 4\nf 3 1 4\n";
        REQUIRE(output.good());
    }
    const auto hash = model_artifact_sha256(path);
    TriangleMesh reference;
    ObjInfo reference_colors;
    std::string error;
    REQUIRE(load_obj(path.string().c_str(), &reference, reference_colors, error));
    REQUIRE(reference.its.indices.size() == 24000);
    REQUIRE(reference_colors.vertex_colors.size() == 4);
    for (int checkpoint : {1, 5, 12}) {
        INFO("Cancellation poll " << checkpoint);
        TriangleMesh mesh = reference;
        ObjInfo colors = reference_colors;
        ObjParser::MtlData materials;
        materials.mtl_orders = {"stale"};
        int visits = 0;
        // A one-time request must remain effective after the parser returns.
        REQUIRE_FALSE(load_obj(path.string().c_str(), &mesh, colors, error, &materials,
            [&] { return ++visits == checkpoint; }));
        CHECK(error == "Model loading canceled.");
        CHECK(mesh.empty());
        CHECK(colors.vertex_colors.empty());
        CHECK(colors.face_colors.empty());
        CHECK(colors.uvs.empty());
        CHECK(materials.mtl_orders.empty());
        REQUIRE(load_model_artifact(path, mesh, colors, error, [] { return false; }));
        CHECK(error.empty());
        CHECK(mesh.its.vertices == reference.its.vertices);
        CHECK(mesh.its.indices == reference.its.indices);
        CHECK(colors.vertex_colors == reference_colors.vertex_colors);
    }
    CHECK(model_artifact_sha256(path) == hash);
}

TEST_CASE("Canceling mixed OBJ faces stops material conversion before all faces finish", "[ModelArtifact]") {
    Fixture f;
    const auto path = f.directory / "mixed.obj";
    {
        boost::filesystem::ofstream material(f.directory / "paint.mtl");
        material << "newmtl paint\nKa 0 0 0\nKd 0.2 0.4 0.6\nTr 1\n";
        REQUIRE(material.good());
        boost::filesystem::ofstream output(path);
        output << "mtllib paint.mtl\nusemtl paint\nv 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\nf 1 2 3 4\n";
        for (int i = 0; i < 6000; ++i)
            output << "f 1 3 2\nf 1 2 4\nf 2 3 4\nf 3 1 4\n";
        REQUIRE(output.good());
    }
    TriangleMesh reference;
    ObjInfo reference_colors;
    std::string error;
    REQUIRE(load_obj(path.string().c_str(), &reference, reference_colors, error));
    REQUIRE(reference_colors.face_colors.size() == 24002);
    TriangleMesh mesh;
    ObjInfo colors;
    size_t converted_before_cancel = 0;
    REQUIRE_FALSE(load_obj(path.string().c_str(), &mesh, colors, error, nullptr, [&] {
        if (colors.face_colors.size() < 3000) return false;
        converted_before_cancel = colors.face_colors.size();
        return true;
    }));
    CHECK(converted_before_cancel >= 3000);
    CHECK(converted_before_cancel < reference_colors.face_colors.size());
    CHECK(error == "Model loading canceled.");
    CHECK(mesh.empty());
    CHECK(colors.face_colors.empty());
    REQUIRE(load_model_artifact(path, mesh, colors, error));
    CHECK(mesh.its.vertices == reference.its.vertices);
    CHECK(mesh.its.indices == reference.its.indices);
    CHECK(colors.face_colors == reference_colors.face_colors);
}

TEST_CASE("Canceled GLB loading discards outputs and preserves the normal retry", "[ModelArtifact]") {
    const auto path = samples / "baseline.glb";
    const auto hash = model_artifact_sha256(path);
    TriangleMesh reference;
    ObjInfo reference_colors;
    std::string error;
    REQUIRE(load_model_artifact(path, reference, reference_colors, error));
    for (int checkpoint : {1, 3, 5}) {
        TriangleMesh mesh = reference;
        ObjInfo colors = reference_colors;
        int visits = 0;
        REQUIRE_FALSE(load_model_artifact(path, mesh, colors, error, [&] { return ++visits == checkpoint; }));
        CHECK(error == "Model loading canceled.");
        CHECK(mesh.empty());
        CHECK(colors.vertex_colors.empty());
        REQUIRE(load_model_artifact(path, mesh, colors, error, [] { return false; }));
        CHECK(error.empty());
        CHECK(mesh.its.vertices == reference.its.vertices);
        CHECK(mesh.its.indices == reference.its.indices);
        CHECK(colors.vertex_colors == reference_colors.vertex_colors);
    }
    CHECK(model_artifact_sha256(path) == hash);
}

// Opt-in stage probe; this is the adapter's GLB-to-OBJ preparation only,
// not Plater loading, modal interaction, seam repair or event-loop latency.
TEST_CASE("A local OBJ reports parser mesh and single-color model loading separately", "[.NativeObjLoadProbe]") {
    const char* input = std::getenv("ORCASLICER_OBJ_PROBE_FIXTURE");
    const char* report = std::getenv("ORCASLICER_OBJ_PROBE_REPORT");
    REQUIRE(input != nullptr);
    REQUIRE(report != nullptr);
    REQUIRE_FALSE(boost::filesystem::exists(report));
    const auto original_hash = model_artifact_sha256(input);
    REQUIRE_FALSE(original_hash.empty());
    TriangleMesh reference; ObjInfo reference_colors; std::string error;
    REQUIRE(load_obj(input, &reference, reference_colors, error));
    nlohmann::json runs = nlohmann::json::array();
    auto elapsed = [](const auto& started) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    };
    // File cache is prewarmed. Rotate order to avoid always timing the parser first.
    for (int run = 0; run < 6; ++run) {
        for (int step = 0; step < 3; ++step) {
            const int stage = (run + step) % 3;
            double ms = 0;
            if (stage == 0) {
                ObjParser::ObjData parsed;
                const auto started = std::chrono::steady_clock::now();
                const bool ok = ObjParser::objparse(input, parsed);
                ms = elapsed(started);
                REQUIRE(ok);
                REQUIRE(parsed.coordinates.size() / OBJ_VERTEX_LENGTH == reference.its.vertices.size());
                for (size_t i = 0; i < reference.its.vertices.size(); ++i)
                    for (size_t axis = 0; axis < 3; ++axis)
                        if (parsed.coordinates[i * OBJ_VERTEX_LENGTH + axis] != reference.its.vertices[i][axis])
                            FAIL("Parser changed a vertex coordinate");
            } else {
                TriangleMesh mesh; ObjInfo colors; Model model;
                const auto started = std::chrono::steady_clock::now();
                if (stage == 1) {
                    const bool ok = load_obj(input, &mesh, colors, error);
                    ms = elapsed(started);
                    REQUIRE(ok);
                } else {
                    // Same no-op color callback as the single-color GUI importer.
                    model = Model::read_from_file(input, nullptr, nullptr, LoadStrategy::LoadModel,
                        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0,
                        [](ObjDialogInOut&) {});
                    ms = elapsed(started);
                    REQUIRE(model.objects.size() == 1);
                    REQUIRE(model.objects.front()->volumes.size() == 1);
                }
                const auto& its = stage == 1 ? mesh.its : model.objects.front()->volumes.front()->mesh().its;
                REQUIRE(its.vertices.size() == reference.its.vertices.size());
                // ModelVolume centers local coordinates and retains their offset in its transform.
                double max_error = 0;
                for (size_t i = 0; i < its.vertices.size(); ++i) {
                    const Vec3d point = stage == 1 ? its.vertices[i].cast<double>().eval()
                        : (model.objects.front()->volumes.front()->get_matrix() * its.vertices[i].cast<double>()).eval();
                    max_error = std::max(max_error, (point - reference.its.vertices[i].cast<double>()).cwiseAbs().maxCoeff());
                }
                CHECK_THAT(max_error, WithinAbs(0.0, 1e-5));
                REQUIRE(its.indices == reference.its.indices);
                if (stage == 1) REQUIRE(colors.vertex_colors == reference_colors.vertex_colors);
            }
            runs.push_back({{"run", run}, {"stage", stage}, {"milliseconds", ms}});
        }
    }
    REQUIRE(model_artifact_sha256(input) == original_hash);
    boost::filesystem::ofstream output(report, std::ios::binary);
    output << nlohmann::json({{"source_sha256", original_hash}, {"runs", runs}}).dump(2);
    REQUIRE(output.good());
}

TEST_CASE("Local GLB import preparation reports hash decode and OBJ write separately", "[.ModelImportPreparationProbe]") {
    const char* input = std::getenv("ORCASLICER_MODEL_ARTIFACT_FIXTURE");
    const char* report = std::getenv("ORCASLICER_IMPORT_PROBE_REPORT");
    if (!input || !*input || !report || !*report)
        SKIP("Set fixture and a new ORCASLICER_IMPORT_PROBE_REPORT path.");
    REQUIRE_FALSE(boost::filesystem::exists(report));
    Fixture f;
    const boost::filesystem::path source(input);
    REQUIRE(model_artifact_format(source) == "glb");
    const auto original_hash = model_artifact_sha256(source);
    REQUIRE_FALSE(original_hash.empty());
    nlohmann::json runs = nlohmann::json::array();
    std::string first_output_hash;
    auto elapsed = [](const auto& begin) {
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - begin).count();
    };
    // Input hashing prewarms the file cache. First run is not disk-cold.
    for (int run = 0; run < 6; ++run) {
        auto started = std::chrono::steady_clock::now();
        const auto hash = model_artifact_sha256(source);
        const double hash_ms = elapsed(started);
        REQUIRE(hash == original_hash);
        TriangleMesh mesh; ObjInfo colors; std::string error;
        started = std::chrono::steady_clock::now();
        const bool loaded = load_model_artifact(source, mesh, colors, error);
        const double decode_ms = elapsed(started);
        INFO(error);
        REQUIRE(loaded);
        const auto output = f.directory / (std::to_string(run) + ".obj");
        started = std::chrono::steady_clock::now();
        const bool written = write_model_artifact(output, mesh.its, colors.vertex_colors, error);
        const double write_ms = elapsed(started);
        REQUIRE(written);
        started = std::chrono::steady_clock::now();
        const bool cached = is_model_artifact(output);
        const double cache_check_ms = elapsed(started);
        REQUIRE(cached);
        started = std::chrono::steady_clock::now();
        const auto output_hash = model_artifact_sha256(output);
        const double obj_hash_ms = elapsed(started);
        REQUIRE_FALSE(output_hash.empty());
        if (run == 0) first_output_hash = output_hash;
        REQUIRE(output_hash == first_output_hash);
        // Verify the conversion once, outside the measured stages.
        if (run == 0) {
            TriangleMesh restored; ObjInfo restored_colors;
            REQUIRE(load_model_artifact(output, restored, restored_colors, error));
            REQUIRE(restored.its.indices == mesh.its.indices);
            REQUIRE(restored.its.vertices == mesh.its.vertices);
            REQUIRE(restored_colors.vertex_colors.size() == colors.vertex_colors.size());
            for (size_t i = 0; i < colors.vertex_colors.size(); ++i)
                for (size_t c = 0; c < 3; ++c)
                    if (std::abs(restored_colors.vertex_colors[i][c] - colors.vertex_colors[i][c]) > 1e-6f)
                        FAIL("OBJ round trip changed a vertex color");
        }
        runs.push_back({{"run", run}, {"hash_ms", hash_ms}, {"decode_ms", decode_ms},
                        {"write_ms", write_ms}, {"cache_check_ms", cache_check_ms},
                        {"obj_hash_ms", obj_hash_ms},
                        {"obj_bytes", boost::filesystem::file_size(output)},
                        {"vertices", mesh.its.vertices.size()}, {"faces", mesh.its.indices.size()},
                        {"output_sha256", output_hash}});
    }
    REQUIRE(model_artifact_sha256(source) == original_hash);
    boost::filesystem::ofstream output(report, std::ios::binary);
    output << nlohmann::json({{"source_sha256", original_hash}, {"runs", runs}}).dump(2);
    output.close();
    REQUIRE(output.good());
}

TEST_CASE("Verified GLB import reports first preparation and reuse separately", "[.GlbImportPreparationProbe]") {
    const char* input = std::getenv("ORCASLICER_MODEL_ARTIFACT_FIXTURE");
    const char* report = std::getenv("ORCASLICER_IMPORT_CACHE_PROBE_REPORT");
    if (!input || !*input || !report || !*report)
        SKIP("Set fixture and a new ORCASLICER_IMPORT_CACHE_PROBE_REPORT path.");
    REQUIRE_FALSE(boost::filesystem::exists(report));
    Fixture f;
    const boost::filesystem::path source(input);
    REQUIRE(model_artifact_format(source) == "glb");
    const auto source_hash = model_artifact_sha256(source);
    REQUIRE_FALSE(source_hash.empty());
    nlohmann::json runs = nlohmann::json::array();
    std::string first_obj_hash;
    for (int run = 0; run < 5; ++run) {
        VerifiedGlbImportCopy verified;
        boost::filesystem::path output;
        std::string error;
        bool reused = true;
        const auto cache_root = f.directory / std::to_string(run);
        auto started = std::chrono::steady_clock::now();
        REQUIRE(prepare_glb_obj_import(source, cache_root, verified, output, error, reused));
        const double first_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        REQUIRE_FALSE(reused);
        REQUIRE(verified.source_sha256 == source_hash);
        REQUIRE(verified.obj_path == output);
        const auto on_disk_hash = model_artifact_sha256(output);
        REQUIRE_FALSE(on_disk_hash.empty());
        REQUIRE(verified.obj_sha256 == on_disk_hash);
        if (run == 0) first_obj_hash = on_disk_hash;
        REQUIRE(on_disk_hash == first_obj_hash);
        const auto original_output = output;
        started = std::chrono::steady_clock::now();
        REQUIRE(prepare_glb_obj_import(source, cache_root, verified, output, error, reused));
        const double reuse_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        REQUIRE(reused);
        REQUIRE(output == original_output);
        runs.push_back({{"run", run}, {"first_ms", first_ms}, {"reuse_ms", reuse_ms},
                        {"obj_bytes", boost::filesystem::file_size(output)},
                        {"obj_sha256", on_disk_hash}});
        REQUIRE(boost::filesystem::remove(output));
    }
    REQUIRE(model_artifact_sha256(source) == source_hash);
    boost::filesystem::ofstream result(report, std::ios::binary);
    result << nlohmann::json({{"source_sha256", source_hash}, {"runs", runs}}).dump(2);
    result.close();
    REQUIRE(result.good());
}

TEST_CASE("OBJ writing retains nine digit classic locale text across numeric extremes and batches", "[ModelArtifact]") {
    Fixture f;
    indexed_triangle_set mesh;
    std::vector<RGBA> colors;
    uint32_t random = 1729;
    const float edges[] = {0.f, -0.f, 1.f, -1.f, 0.0001f, 0.00001f,
        999999999.f, 1e9f, std::numeric_limits<float>::max(),
        std::numeric_limits<float>::min(), std::numeric_limits<float>::denorm_min()};
    for (size_t i = 0; i < 8193; ++i) {
        Vec3f point;
        for (int c = 0; c < 3; ++c) {
            random = random * 1664525u + 1013904223u;
            float value; std::memcpy(&value, &random, sizeof(value));
            point[c] = i < std::size(edges) ? edges[i] : (std::isfinite(value) ? value : 0.f);
        }
        mesh.vertices.push_back(point);
        colors.push_back({float(i % 251) / 250.f, .123456789f, .99999994f, 1.f});
        if (i % 3 == 2) mesh.indices.emplace_back(int(i - 2), int(i - 1), int(i));
    }
    // The pre-optimization serializer is an independent compatibility oracle.
    std::ostringstream expected;
    expected.imbue(std::locale::classic());
    expected << "# Orca AI model: Z-up millimetres, sRGB vertex colors\n" << std::setprecision(9);
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        const auto& v = mesh.vertices[i]; const auto& c = colors[i];
        expected << "v " << v.x() << ' ' << v.y() << ' ' << v.z() << ' ' << c[0] << ' ' << c[1] << ' ' << c[2] << '\n';
    }
    for (const auto& face : mesh.indices)
        expected << "f " << face[0] + 1 << ' ' << face[1] + 1 << ' ' << face[2] + 1 << '\n';
    std::string error;
    const auto path = f.directory / "numbers.obj";
    REQUIRE(write_model_artifact(path, mesh, colors, error));
    boost::filesystem::ifstream file(path, std::ios::binary);
    const std::string actual((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    REQUIRE(actual == expected.str());
    const auto hash = model_artifact_sha256(path);
    REQUIRE_FALSE(write_model_artifact(path, mesh, colors, error));
    REQUIRE(model_artifact_sha256(path) == hash);
    REQUIRE_FALSE(boost::filesystem::exists(path.string() + ".partial"));
}

TEST_CASE("Artifact file hashes preserve binary bytes across stream boundaries and missing files", "[ModelArtifact]")
{
    Fixture fixture;
    const auto file=fixture.directory/boost::filesystem::path(L"\u6a21\u578b-\u6307\u7eb9.bin");
    const std::vector<std::pair<size_t,std::string>> expected {
        {0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {1, "6e340b9cffb37a989ca544e6bb780a2c78901d3fb33738768511a30617afa01d"},
        {55, "5d05a4435b96f43e53e5a582bbb0b8d840a71c023e7468d6604d1c9ae6511c4c"},
        {56, "0851bc0b733318bd14db8098dec9a591c02ee1e72295d3ba54560e56342430fb"},
        {64, "949f78c7321c5fa8a90f3d236c471950df72d869abc1d36e985cfce9a3ac98b9"},
        {65535, "7295617328ad381348e633b2e223536068c1ea4ea2c1e3b63fb73acef69a50b4"},
        {65536, "0b1920fd5be0f3e4af437b7fa14d7053c9b5a43b94942bdf206c6d3cee2e5a03"},
        {65537, "bf3f784409bf5d96eca02467657fcf888a9c2643991b31ec2169a96fae3cf4d4"},
        {131073, "dec259f4fd5678fed4e56bb51b0acedcf2c0573a900fa2086a5f2d779f0bdb46"}
    };
    std::string data(131073,'\0');
    for(size_t i=0;i<data.size();++i)data[i]=char((i*131u+(i>>8))&255u);
    for(const auto& sample:expected) {
        DYNAMIC_SECTION(sample.first) {
            {boost::filesystem::ofstream output(file,std::ios::binary);output.write(data.data(),sample.first);output.close();REQUIRE(output.good());}
            CHECK(model_artifact_sha256(file)==sample.second);
            CHECK(boost::filesystem::file_size(file)==sample.first);
            boost::filesystem::ifstream input(file,std::ios::binary);
            const std::string contents((std::istreambuf_iterator<char>(input)),{});
            CHECK(contents==data.substr(0,sample.first));
        }
    }
    CHECK(model_artifact_sha256(fixture.directory/"missing.bin").empty());
}

TEST_CASE("A local artifact reports full file hashing without modifying its source", "[.ArtifactFileHashProbe]")
{
    const auto input=std::getenv("ORCA_HASH_SOURCE"),report=std::getenv("ORCA_HASH_REPORT");
    if(!input || !*input || !report || !*report)SKIP("Set a local source and fresh hash report path.");
    REQUIRE_FALSE(boost::filesystem::exists(report));
    const boost::filesystem::path source(input);
    const auto expected=model_artifact_sha256(source);
    REQUIRE_FALSE(expected.empty());
    const auto bytes=boost::filesystem::file_size(source);
    nlohmann::json samples=nlohmann::json::array();
    for(int round=0;round<6;++round) {
        const auto start=std::chrono::steady_clock::now();
        const auto actual=model_artifact_sha256(source);
        const auto finish=std::chrono::steady_clock::now();
        REQUIRE(actual==expected);
        samples.push_back({{"round",round},{"elapsed_ms",std::chrono::duration<double,std::milli>(finish-start).count()},{"sha256",actual}});
    }
    REQUIRE(boost::filesystem::file_size(source)==bytes);
    REQUIRE(model_artifact_sha256(source)==expected);
    boost::filesystem::ofstream output(report,std::ios::binary);
    output<<nlohmann::json{{"source_sha256",expected},{"bytes",bytes},{"samples",samples},
        {"scope","Full-file hash CPU/IO wall time; prewarmed by identity check, no GUI/speedup/working-set claim"}}.dump(2);
    output.close();REQUIRE(output.good());
}
