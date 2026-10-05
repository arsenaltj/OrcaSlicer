#include <catch2/catch_test_macros.hpp>
#include "slic3r/GUI/AI/ModelGeneration/ModelLibraryExport.hpp"
#include "libslic3r/Format/AssimpImport.hpp"
#include "libslic3r/TexturePainting.hpp"
#include <iterator>

using namespace Slic3r;
using namespace Slic3r::GUI;
namespace {
struct ExportFiles {
    boost::filesystem::path root = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("orca-export-%%%%-%%%%");
    ExportFiles() { boost::filesystem::create_directory(root); }
    ~ExportFiles() { boost::system::error_code ec; boost::filesystem::remove_all(root, ec); }
};
std::string read(const boost::filesystem::path& path) {
    boost::filesystem::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
void write(const boost::filesystem::path& path, const std::string& text) {
    boost::filesystem::ofstream out(path, std::ios::binary); out << text;
}
const boost::filesystem::path samples = boost::filesystem::path(std::string(TEST_DATA_DIR)) / "model_artifact";
}

TEST_CASE("Exported model copies retain bytes and task IDs without replacing previous copies", "[ModelLibraryExport]") {
    ExportFiles f; ModelLibraryExportRequest request;
    request.model = samples / "textured.glb"; request.id = "task-123";
    request.task_id = "provider-456"; request.conversion_task_id = "convert-789";
    const auto before = AI::model_artifact_sha256(request.model);
    boost::filesystem::path first, second; std::string error;
    REQUIRE(export_model_library_copy(request, f.root, first, error));
    REQUIRE(export_model_library_copy(request, f.root, second, error));
    CHECK(first != second);
    CHECK(AI::model_artifact_sha256(first / "model.glb") == before);
    CHECK(AI::model_artifact_sha256(second / "model.glb") == before);
    CHECK(AI::model_artifact_sha256(request.model) == before);
    CHECK(read(first / "README.txt").find("provider-456") != std::string::npos);
    CHECK(read(first / "README.txt").find("convert-789") != std::string::npos);
    TriangleMesh model; ObjInfo colors;
    REQUIRE(AI::load_model_artifact(second / "model.glb", model, colors, error));
    CHECK_FALSE(model.its.indices.empty());
}

TEST_CASE("Design copies preserve both images and need no model or live service", "[ModelLibraryExport]") {
    ExportFiles f; ModelLibraryExportRequest request;
    request.id = "../outside"; request.design = f.root / "design.png"; request.reference = f.root / "reference.jpg";
    write(request.design, "original image bytes"); write(request.reference, "reference bytes");
    boost::filesystem::path result; std::string error;
    REQUIRE(export_model_library_copy(request, f.root, result, error));
    CHECK(result.parent_path() == f.root);
    CHECK(read(result / "design.png") == read(request.design));
    CHECK(read(result / "reference.jpg") == read(request.reference));
    CHECK_FALSE(boost::filesystem::exists(result / "model.glb"));
}

TEST_CASE("Failed exports leave existing files intact and remove their incomplete copy", "[ModelLibraryExport]") {
    ExportFiles f; ModelLibraryExportRequest request;
    request.id = "broken"; request.model = f.root / "broken.glb";
    write(request.model, "broken");
    boost::filesystem::create_directory(f.root / "Orca-broken");
    write(f.root / "Orca-broken" / "keep.txt", "keep me");
    boost::filesystem::path result; std::string error;
    CHECK_FALSE(export_model_library_copy(request, f.root, result, error));
    CHECK(result.empty()); CHECK_FALSE(error.empty());
    CHECK_FALSE(boost::filesystem::exists(f.root / "Orca-broken-2"));
    CHECK(read(f.root / "Orca-broken" / "keep.txt") == "keep me");
    request.model = samples / "textured.glb"; request.design = f.root / "missing.png";
    CHECK_FALSE(export_model_library_copy(request, f.root, result, error));
    CHECK_FALSE(boost::filesystem::exists(f.root / "Orca-broken-2"));
    CHECK_FALSE(export_model_library_copy(request, f.root / "missing-parent", result, error));
}

TEST_CASE("Exported OBJ embeds its texture dependency and loads after source dependencies are removed", "[ModelLibraryExport]") {
    ExportFiles f; TexturedMesh source; std::string error;
    REQUIRE(load_assimp_textured_model((samples / "textured.glb").string(), source, &error));
    REQUIRE_FALSE(source.textures.empty());
    const auto& bytes = source.textures.front().data;
    write(f.root / "texture.png", std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    write(f.root / "model.mtl", "newmtl surface\nKd 1 1 1\nmap_Kd texture.png\n");
    write(f.root / "model.obj", "mtllib model.mtl\nv 0 0 0\nv 20 0 0\nv 0 20 0\nvt 0 0\nvt 1 0\nvt 0 1\nusemtl surface\nf 1/1 2/2 3/3\n");
    ModelLibraryExportRequest request; request.id = "textured"; request.model = f.root / "model.obj";
    boost::filesystem::path result;
    REQUIRE(export_model_library_copy(request, f.root, result, error));
    boost::filesystem::remove(f.root / "texture.png"); boost::filesystem::remove(f.root / "model.mtl");
    TexturedMesh exported;
    REQUIRE(load_assimp_textured_model((result / "model.glb").string(), exported, &error));
    REQUIRE(exported.textures.size() == 1);
    CHECK(exported.textures.front().data == bytes);
    CHECK(exported.indices.size() == 1);
}
