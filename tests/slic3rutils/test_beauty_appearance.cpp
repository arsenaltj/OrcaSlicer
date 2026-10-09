#include "slic3r/GUI/AI/Model/BeautyAppearance.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "slic3r/GUI/AI/Model/BeautyPuzzle.hpp"
#include "slic3r/GUI/AI/Model/BeautyLeafEdits.hpp"
#include "slic3r/GUI/AI/Model/SurfacePartition.hpp"
#include "slic3r/GUI/AI/Model/BeautyCellRemap.hpp"
#include "slic3r/GUI/AI/ModelGeneration/PortraitShapeDetails.hpp"
#include "slic3r/GUI/AI/Model/ModelFinishing.hpp"
#include "slic3r/GUI/AI/Model/SurfaceSelectionState.hpp"
#include "slic3r/GUI/AI/Model/GlbGeometryEditing.hpp"
#include "libslic3r/Format/AssimpImport.hpp"
#include "libslic3r/TexturePainting.hpp"
#include <catch2/catch_test_macros.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <chrono>
#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <chrono>
#include <cstdlib>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

using namespace Slic3r;
using namespace Slic3r::AI;
namespace {
using Json = nlohmann::json;
struct Fixture {
    boost::filesystem::path directory = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("orca-appearance-%%%%-%%%%-%%%%");
    Fixture() { boost::filesystem::create_directory(directory); }
    ~Fixture() { boost::system::error_code ignored; boost::filesystem::remove_all(directory,ignored); }
};
void append32(std::vector<unsigned char>& bytes, uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<unsigned char>(value >> shift));
}
void append_float(std::vector<unsigned char>& bytes, float value) {
    uint32_t bits; std::memcpy(&bits,&value,4); append32(bytes,bits);
}
uint32_t u32(const unsigned char* p) { return uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24; }
struct Glb {
    Json doc;
    std::vector<unsigned char> bytes, binary;
};
Glb read_glb(const boost::filesystem::path& path) {
    boost::filesystem::ifstream input(path,std::ios::binary);
    Glb glb;
    glb.bytes.assign(std::istreambuf_iterator<char>(input),{});
    REQUIRE(glb.bytes.size() >= 28);
    const size_t json_size = u32(glb.bytes.data()+12);
    glb.doc = Json::parse(glb.bytes.begin()+20,glb.bytes.begin()+20+json_size);
    const size_t size = glb.doc["buffers"][0]["byteLength"].get<size_t>();
    glb.binary.assign(glb.bytes.begin()+28+json_size,glb.bytes.begin()+28+json_size+size);
    return glb;
}
void write_glb(const boost::filesystem::path& path, Json doc, std::vector<unsigned char> binary) {
    doc["buffers"] = Json::array({{{"byteLength",binary.size()}}});
    std::string description = doc.dump();
    while (description.size()%4) description += ' ';
    while (binary.size()%4) binary.push_back(0);
    std::vector<unsigned char> bytes;
    append32(bytes,0x46546c67); append32(bytes,2); append32(bytes,uint32_t(28+description.size()+binary.size()));
    append32(bytes,uint32_t(description.size())); append32(bytes,0x4e4f534a);
    bytes.insert(bytes.end(),description.begin(),description.end());
    append32(bytes,uint32_t(binary.size())); append32(bytes,0x004e4942);
    bytes.insert(bytes.end(),binary.begin(),binary.end());
    boost::filesystem::ofstream output(path,std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
    REQUIRE(bool(output));
}
boost::filesystem::path make_fixture(const Fixture& fixture, bool overlapping = false, bool noise = false, bool multi_material = false, size_t extra_faces = 0) {
    std::vector<unsigned char> binary;
    const std::array<std::array<float,3>,8> positions{{{0,0,0},{1,0,0},{1,1,0},{0,1,0},{2,0,0},{3,0,0},{3,1,0},{2,1,0}}};
    for (const auto& p : positions) for (float c : p) append_float(binary,c);
    const size_t uv_offset = binary.size();
    std::array<std::array<float,2>,8> uvs{{{.05f,.1f},{.45f,.1f},{.45f,.9f},{.05f,.9f},
                                        {.55f,.1f},{.95f,.1f},{.95f,.9f},{.55f,.9f}}};
    if (overlapping) for (size_t i=0;i<4;++i) uvs[i+4] = uvs[i];
    for (const auto& uv : uvs) for (float c : uv) append_float(binary,c);
    const size_t index_offset = binary.size();
    for (uint32_t index : {0u,1u,2u,0u,2u,3u,4u,5u,6u,4u,6u,7u}) append32(binary,index);
    for (size_t i=0;i<extra_faces;++i) for (uint32_t index : {4u,5u,6u}) append32(binary,index);
    const size_t image_offset = binary.size();
    cv::Mat pixels(32,64,CV_8UC4);
    for (int y=0;y<pixels.rows;++y) for (int x=0;x<pixels.cols;++x)
        pixels.at<cv::Vec4b>(y,x) = cv::Vec4b(30+x/4,55+y/2,90+x+y,100+x);
    if (noise) pixels.at<cv::Vec4b>(16,16) = cv::Vec4b(240,240,240,116);
    std::vector<unsigned char> png;
    REQUIRE(cv::imencode(".png",pixels,png));
    binary.insert(binary.end(),png.begin(),png.end());
    Json doc = {
        {"asset",{{"version","2.0"}}}, {"scene",0}, {"scenes",Json::array({{{"nodes",Json::array({0})}}})},
        {"nodes",Json::array({{{"mesh",0},{"translation",{.01,.02,.03}}}})},
        {"bufferViews",Json::array({{{"buffer",0},{"byteOffset",0},{"byteLength",uv_offset}},
            {{"buffer",0},{"byteOffset",uv_offset},{"byteLength",index_offset-uv_offset}},
            {{"buffer",0},{"byteOffset",index_offset},{"byteLength",image_offset-index_offset}},
            {{"buffer",0},{"byteOffset",image_offset},{"byteLength",png.size()}}})},
        {"accessors",Json::array({{{"bufferView",0},{"componentType",5126},{"count",8},{"type","VEC3"},{"min",{0,0,0}},{"max",{3,1,0}}},
            {{"bufferView",1},{"componentType",5126},{"count",8},{"type","VEC2"}},
            {{"bufferView",2},{"componentType",5125},{"count",12+3*extra_faces},{"type","SCALAR"}}})},
        {"images",Json::array({{{"bufferView",3},{"mimeType","image/png"}}})},
        {"textures",Json::array({{{"source",0},{"sampler",0}}})},
        {"samplers",Json::array({{{"wrapS",33071},{"wrapT",33071}}})},
        {"materials",Json::array({{{"pbrMetallicRoughness",{{"baseColorTexture",{{"index",0}}},{"baseColorFactor",{.8,.9,1,1}},{"roughnessFactor",.7}}},
            {"normalTexture",{{"index",0}}},{"alphaMode","BLEND"},{"doubleSided",true}}})},
        {"meshes",Json::array({{{"primitives",Json::array({{{"attributes",{{"POSITION",0},{"TEXCOORD_0",1}}},{"indices",2},{"material",0}}})}}})}
    };
    if (multi_material) {
        doc["accessors"][2]["count"] = 6;
        Json second_accessor = doc["accessors"][2]; second_accessor["byteOffset"] = 24;
        doc["accessors"].push_back(second_accessor);
        Json primitive = doc["meshes"][0]["primitives"][0]; primitive["indices"] = 3; primitive["material"] = 1;
        doc["meshes"][0]["primitives"].push_back(primitive);
        Json material = doc["materials"][0]; material["pbrMetallicRoughness"]["baseColorFactor"] = {.9,.8,.7,1};
        doc["materials"].push_back(material);
    }
    const auto path = fixture.directory/"source.glb";
    write_glb(path,doc,binary);
    return path;
}
cv::Mat color_image(const Glb& glb, size_t material = 0) {
    const size_t texture = glb.doc["materials"][material]["pbrMetallicRoughness"]["baseColorTexture"]["index"].get<size_t>();
    const size_t image = glb.doc["textures"][texture]["source"].get<size_t>();
    const auto& view = glb.doc["bufferViews"][glb.doc["images"][image]["bufferView"].get<size_t>()];
    const size_t offset = view.value("byteOffset",size_t(0)), length = view["byteLength"].get<size_t>();
    std::vector<unsigned char> encoded(glb.binary.begin()+offset,glb.binary.begin()+offset+length);
    return cv::imdecode(encoded,cv::IMREAD_UNCHANGED);
}
BeautyAppearanceOptions selected_left() {
    BeautyAppearanceOptions options; options.face_weights = {1,1,0,0}; options.brightness = .12; return options;
}
void require_preserved_geometry(const Glb& original, const Glb& edited) {
    for (const char* field : {"meshes","nodes","scenes","scene","accessors","samplers"}) REQUIRE(edited.doc[field] == original.doc[field]);
    REQUIRE(edited.binary.size() >= original.binary.size());
    REQUIRE(std::equal(original.binary.begin(),original.binary.end(),edited.binary.begin()));
}
boost::filesystem::path make_neutral_fixture(const Fixture& fixture, bool overlapping = false, size_t extra_faces = 0) {
    const auto path = make_fixture(fixture, overlapping, false, false, extra_faces);
    auto original = read_glb(path);
    for (auto& material : original.doc["materials"])
        material["pbrMetallicRoughness"]["baseColorFactor"] = {1, 1, 1, 1};
    write_glb(path, original.doc, original.binary);
    return path;
}

BeautyAppearanceOptions puzzle_colors() {
    BeautyAppearanceOptions options;
    options.face_weights = {1, 1, 1, 1};
    options.face_target_colors = {{.8f, .2f, .1f}, {.8f, .2f, .1f}, {.1f, .2f, .8f}, {.1f, .2f, .8f}};
    return options;
}

ModelFinishingOptions puzzle_options(const boost::filesystem::path& base) {
    TriangleMesh mesh;
    ObjInfo info;
    std::string error;
    REQUIRE(load_model_artifact(base, mesh, info, error));
    ModelFinishingOptions options;
    options.smooth_surface = false;
    options.repair_mesh = false;
    options.beauty_puzzle = true;
    options.beauty_surface = BeautySurface::build(mesh.its, info.vertex_colors);
    const auto puzzle = BeautyPuzzle::create(*options.beauty_surface, 2);
    options.beauty_document = {{"puzzle", puzzle.encode()}, {"puzzle_base_file", base.filename().string()},
                               {"puzzle_base_sha256", model_artifact_sha256(base)}};
    return options;
}
boost::filesystem::path make_corner_fixture(const Fixture& fixture) {
    indexed_triangle_set mesh;
    mesh.vertices = {{0,0,0},{10,0,0},{0,10,0},{20,0,0},{30,0,0},{20,10,0}};
    mesh.indices = {{0,1,2},{3,4,5}};
    std::vector<RGBA> colors(6, {.6f,.4f,.2f,1.f});
    const auto path = fixture.directory / "corners.glb";
    std::string error;
    REQUIRE(write_model_artifact(path, mesh, colors, error));
    auto glb = read_glb(path);
    glb.doc["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"] = {.3,.4,.5,.6};
    glb.doc["materials"][0]["alphaMode"] = "BLEND";
    write_glb(path, glb.doc, glb.binary);
    return path;
}
}

// Opt-in real-asset measurement; never runs in the regular regression suite.
TEST_CASE("Puzzle save measurements preserve the verified original asset", "[.][PuzzleSaveProbe]") {
    const char* input = std::getenv("ORCA_SAVE_SOURCE");
    const char* output = std::getenv("ORCA_SAVE_OUTPUT");
    const char* mode = std::getenv("ORCA_SAVE_MODE");
    REQUIRE(input);
    REQUIRE(output);
    REQUIRE(mode);
    REQUIRE_FALSE(boost::filesystem::exists(output));
    Fixture fixture;
    const auto base = fixture.directory / "base.glb";
    const auto source = fixture.directory / "current.glb";
    boost::filesystem::copy_file(input, base);
    const auto hash = model_artifact_sha256(base);
    if (std::string(mode) == "material-edited") {
        auto glb = read_glb(base);
        REQUIRE_FALSE(glb.doc["materials"].empty());
        for (auto& material : glb.doc["materials"])
            material["pbrMetallicRoughness"]["baseColorFactor"] = {.8, .9, .7, 1.0};
        write_glb(source, glb.doc, glb.binary);
        REQUIRE(model_artifact_sha256(source) != hash);
    } else {
        REQUIRE(std::string(mode) == "identical");
        boost::filesystem::copy_file(base, source);
    }
    const auto source_hash = model_artifact_sha256(source);
    const auto options = puzzle_options(base);
    Json samples = Json::array();
#ifdef _WIN32
    const auto caller_cpu_ms = [] {
        FILETIME created{}, exited{}, kernel{}, user{};
        REQUIRE(GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user));
        ULARGE_INTEGER k{}, u{};
        k.LowPart=kernel.dwLowDateTime; k.HighPart=kernel.dwHighDateTime;
        u.LowPart=user.dwLowDateTime; u.HighPart=user.dwHighDateTime;
        return double(k.QuadPart + u.QuadPart) / 10000.;
    };
#endif
    for (int iteration = 0; iteration < 5; ++iteration) {
        const auto destination = fixture.directory / ("saved-" + std::to_string(iteration) + ".glb");
#ifdef _WIN32
        const double cpu_started = caller_cpu_ms();
#endif
        const auto start = std::chrono::steady_clock::now();
        const auto result = finish_model_artifact(source, destination, options);
        const double elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        Json cpu_ms = nullptr;
#ifdef _WIN32
        cpu_ms = caller_cpu_ms() - cpu_started; // Caller only; not total process CPU.
        REQUIRE(cpu_ms.get<double>() >= 0.);
#endif
        INFO(result.error);
        REQUIRE(result.success);
        REQUIRE(result.output_sha256 == hash);
        REQUIRE(model_artifact_sha256(destination) == hash);
        REQUIRE(result.faces_before == result.faces_after);
        REQUIRE(model_artifact_sha256(source) == source_hash);
        REQUIRE(model_artifact_sha256(base) == hash);
        uint64_t peak = 0;
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS counters{};
        REQUIRE(K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)));
        peak = counters.PeakWorkingSetSize;
#endif
        samples.push_back({{"save_ms", elapsed}, {"caller_thread_cpu_ms", cpu_ms}, {"process_lifetime_peak_bytes", peak},
                           {"output_sha256", result.output_sha256}, {"faces", result.faces_after}});
        boost::filesystem::remove(destination);
    }
    REQUIRE(model_artifact_sha256(input) == hash);
    boost::filesystem::ofstream stream(output);
    stream << Json{{"input_sha256", hash}, {"mode", mode}, {"samples", samples}}.dump(2);
    stream.close();
    REQUIRE(bool(stream));
}

TEST_CASE("Local appearance changes selected texture pixels while preserving geometry and texture variation", "[BeautyWorkbench][BeautyAppearance]") {
    Fixture fixture;
    const auto source = make_fixture(fixture), destination = fixture.directory/"edited.glb";
    const auto hash = model_artifact_sha256(source);
    auto options = selected_left(); options.hue_degrees = 25; options.saturation = .9;
    const auto result = edit_glb_appearance(source,destination,options);
    INFO(result.error);
    REQUIRE(result.success);
    REQUIRE(result.changed_pixels > 100);
    REQUIRE(result.source_sha256 == hash);
    REQUIRE(model_artifact_sha256(source) == hash);
    REQUIRE(result.output_sha256 == model_artifact_sha256(destination));
    const auto original = read_glb(source), edited = read_glb(destination);
    require_preserved_geometry(original,edited);
    REQUIRE(edited.doc["materials"][0]["normalTexture"] == original.doc["materials"][0]["normalTexture"]);
    REQUIRE(edited.doc["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"] == original.doc["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"]);
    const auto before = color_image(original), after = color_image(edited);
    REQUIRE(after.channels() == 4);
    CHECK(after.at<cv::Vec4b>(16,16) != before.at<cv::Vec4b>(16,16));
    CHECK(after.at<cv::Vec4b>(10,10) != after.at<cv::Vec4b>(20,20));
    for (int y=0;y<32;++y) for (int x=0;x<64;++x) {
        CHECK(after.at<cv::Vec4b>(y,x)[3] == before.at<cv::Vec4b>(y,x)[3]);
        if (x >= 33) REQUIRE(after.at<cv::Vec4b>(y,x) == before.at<cv::Vec4b>(y,x));
    }
    TriangleMesh mesh; ObjInfo colors; std::string error;
    REQUIRE(load_model_artifact(destination,mesh,colors,error));
    REQUIRE(mesh.its.indices.size() == 4);
}
TEST_CASE("A selected UV leaf changes only its interior while its siblings retain source pixels", "[BeautyAppearance][BeautyLeafEditing]") {
    Fixture f; const auto source=make_neutral_fixture(f),output=f.directory/"leaf.glb";
    TriangleMesh mesh; ObjInfo colors; std::string error;
    REQUIRE(load_model_artifact(source,mesh,colors,error));
    BeautyAppearanceOptions options; options.face_weights.assign(4,0); options.face_target_colors.resize(4);
    options.canonical_geometry_id=SurfaceSelectionPersistence::geometry_fingerprint(mesh.its);
    for (uint8_t c=0;c<4;++c) options.leaves.push_back({{0,1,c},c==0?1.f:0.f,{1.f,0.f,0.f}});
    const auto original=read_glb(source);
    const auto result=edit_glb_appearance(source,output,options);
    INFO(result.error);
    REQUIRE(result.success); REQUIRE(result.changed_pixels>0);
    const auto edited=read_glb(output); require_preserved_geometry(original,edited);
    const auto a=color_image(original),b=color_image(edited);
    size_t changed=0;
    for (int y=0;y<a.rows;++y) for (int x=0;x<a.cols;++x) if (a.at<cv::Vec4b>(y,x)!=b.at<cv::Vec4b>(y,x)) {
        ++changed;
        REQUIRE(x<18);
        REQUIRE(y<17);
        REQUIRE(b.at<cv::Vec4b>(y,x)[3]==a.at<cv::Vec4b>(y,x)[3]);
    }
    REQUIRE(changed==result.changed_pixels);
    auto bad=options; bad.leaves.pop_back();
    REQUIRE_FALSE(edit_glb_appearance(source,f.directory/"incomplete.glb",bad).success);
    bad=options; bad.face_weights[0]=1;
    REQUIRE_FALSE(edit_glb_appearance(source,f.directory/"root.glb",bad).success);
    bad=options; bad.canonical_geometry_id=std::string(64,'0');
    REQUIRE_FALSE(edit_glb_appearance(source,f.directory/"drift.glb",bad).success);
}
TEST_CASE("UV leaves cannot recolor a shared unselected surface", "[BeautyAppearance][BeautyLeafEditing]") {
    Fixture f; const auto source=make_neutral_fixture(f,true),output=f.directory/"shared.glb";
    TriangleMesh mesh; ObjInfo colors; std::string error;
    REQUIRE(load_model_artifact(source,mesh,colors,error));
    BeautyAppearanceOptions options; options.face_weights.assign(4,0); options.face_target_colors.resize(4);
    options.canonical_geometry_id=SurfaceSelectionPersistence::geometry_fingerprint(mesh.its);
    for (uint8_t c=0;c<4;++c) options.leaves.push_back({{0,1,c},c==0?1.f:0.f,{1.f,0.f,0.f}});
    REQUIRE_FALSE(edit_glb_appearance(source,output,options).success);
    REQUIRE_FALSE(boost::filesystem::exists(output));
}

TEST_CASE("Clipped polygon colors bake and reopen while siblings retain source texture", "[BeautyAppearance][BeautyCellDomain]") {
    Fixture f;const auto source=make_neutral_fixture(f,false,96),output=f.directory/"cells.glb";
    TriangleMesh mesh;ObjInfo info;std::string error;
    REQUIRE(load_model_artifact(source,mesh,info,error));
    REQUIRE(mesh.its.indices.size()==100);
    BeautyAppearanceOptions options;options.face_weights.assign(100,0);
    options.canonical_geometry_id=SurfaceSelectionPersistence::geometry_fingerprint(mesh.its);
    const Vec3d a(1,0,0),b(0,1,0),c(0,0,1),d(.65,.35,0),e(.65,0,.35);
    options.cells={{0,std::string(64,'a'),{{a,d,e}},1,{1,0,0}},
                   {0,std::string(64,'b'),{{d,b,c},{d,c,e}},0,{}}};
    const auto original=read_glb(source);const auto hash=model_artifact_sha256(source);
    const auto result=edit_glb_appearance(source,output,options);
    INFO(result.error);
    REQUIRE(result.success);
    REQUIRE(result.changed_pixels>0);
    REQUIRE(model_artifact_sha256(source)==hash);
    const auto edited=read_glb(output);require_preserved_geometry(original,edited);
    const auto before=color_image(original),after=color_image(edited);
    size_t changed=0;
    for(int y=0;y<before.rows;++y) for(int x=0;x<before.cols;++x)
        if(before.at<cv::Vec4b>(y,x)!=after.at<cv::Vec4b>(y,x)) {
            ++changed;
            REQUIRE(x<14);
            REQUIRE(y<13);
            REQUIRE(after.at<cv::Vec4b>(y,x)[3]==before.at<cv::Vec4b>(y,x)[3]);
        }
    REQUIRE(changed==result.changed_pixels);
    TriangleMesh reopened;ObjInfo reopened_info;
    REQUIRE(load_model_artifact(output,reopened,reopened_info,error));
    REQUIRE(SurfaceSelectionPersistence::geometry_fingerprint(reopened.its)==options.canonical_geometry_id);
    auto bad=options;bad.cells.pop_back();
    REQUIRE_FALSE(edit_glb_appearance(source,f.directory/"missing-cell.glb",bad).success);
    bad=options;bad.cells.back().triangles.push_back({a,d,e});
    REQUIRE_FALSE(edit_glb_appearance(source,f.directory/"overlap-cell.glb",bad).success);
    bad=options;bad.canonical_geometry_id=std::string(64,'0');
    REQUIRE_FALSE(edit_glb_appearance(source,f.directory/"drift-cell.glb",bad).success);
    REQUIRE_FALSE(edit_glb_appearance(source,f.directory/"canceled-cell.glb",options,[]{return true;}).success);
    REQUIRE_FALSE(boost::filesystem::exists(f.directory/"canceled-cell.glb"));
}

TEST_CASE("Clipped polygon baking rejects a shared unselected UV surface", "[BeautyAppearance][BeautyCellDomain]") {
    Fixture f;const auto source=make_neutral_fixture(f,true,96),output=f.directory/"cells.glb";
    TriangleMesh mesh;ObjInfo info;std::string error;
    REQUIRE(load_model_artifact(source,mesh,info,error));
    BeautyAppearanceOptions options;options.face_weights.assign(100,0);
    options.canonical_geometry_id=SurfaceSelectionPersistence::geometry_fingerprint(mesh.its);
    const Vec3d a(1,0,0),b(0,1,0),c(0,0,1),d(.65,.35,0),e(.65,0,.35);
    options.cells={{0,std::string(64,'a'),{{a,d,e}},1,{1,0,0}},
                   {0,std::string(64,'b'),{{d,b,c},{d,c,e}},0,{}}};
    REQUIRE_FALSE(edit_glb_appearance(source,output,options).success);
    REQUIRE_FALSE(boost::filesystem::exists(output));
}
TEST_CASE("Adaptive four level UV partitions save and reopen without expanding the selected child", "[BeautyAppearance][BeautyLeafEditing]") {
    Fixture f; const auto source=make_neutral_fixture(f),output=f.directory/"adaptive.glb";
    TriangleMesh mesh; ObjInfo colors; std::string error;
    REQUIRE(load_model_artifact(source,mesh,colors,error));
    const auto geometry=SurfaceSelectionPersistence::geometry_fingerprint(mesh.its),hash=model_artifact_sha256(source);
    BeautyAppearanceOptions options; options.face_weights.assign(4,0); options.face_target_colors.resize(4);
    appearance_subface_colors(options,{{0,{1,1},{1,0,0},1},{0,{4,0},{0,0,1},1}},geometry);
    REQUIRE(options.leaves.size()==13);
    const auto result=edit_glb_appearance(source,output,options);
    INFO(result.error);
    REQUIRE(result.success); REQUIRE(result.changed_pixels>0);
    REQUIRE(result.source_sha256==hash);
    REQUIRE(model_artifact_sha256(source)==hash);
    TriangleMesh saved; ObjInfo saved_colors;
    REQUIRE(load_model_artifact(output,saved,saved_colors,error));
    REQUIRE(SurfaceSelectionPersistence::geometry_fingerprint(saved.its)==geometry);
    REQUIRE(saved.its.indices==mesh.its.indices);
    const auto before=color_image(read_glb(source)),after=color_image(read_glb(output));
    for (int y=0;y<before.rows;++y) for (int x=0;x<before.cols;++x) {
        REQUIRE(after.at<cv::Vec4b>(y,x)[3]==before.at<cv::Vec4b>(y,x)[3]);
        if (x>=33 || y>=19) REQUIRE(after.at<cv::Vec4b>(y,x)==before.at<cv::Vec4b>(y,x));
    }
}

TEST_CASE("Shared UVs cannot recolor an unselected surface", "[BeautyWorkbench][BeautyAppearance]") {
    Fixture fixture;
    const auto source = make_fixture(fixture,true), destination = fixture.directory/"edited.glb";
    const auto hash = model_artifact_sha256(source);
    auto options = selected_left();
    const auto result = edit_glb_appearance(source,destination,options);
    REQUIRE_FALSE(result.success);
    REQUIRE(result.error.find("shared UV") != std::string::npos);
    REQUIRE_FALSE(boost::filesystem::exists(destination));
    REQUIRE(model_artifact_sha256(source) == hash);
    options.face_weights = {1,1,1,1};
    const auto all = edit_glb_appearance(source,destination,options);
    INFO(all.error);
    REQUIRE(all.success);
    REQUIRE(all.changed_pixels > 0);
}

TEST_CASE("Appearance filters reduce isolated texture specks without changing the unselected area", "[BeautyWorkbench][BeautyAppearance]") {
    Fixture fixture;
    const auto source = make_fixture(fixture,false,true), destination = fixture.directory/"filtered.glb";
    auto options = selected_left(); options.brightness = 0; options.denoise = 1; options.soften = .3;
    const auto result = edit_glb_appearance(source,destination,options);
    INFO(result.error);
    REQUIRE(result.success);
    const auto before = color_image(read_glb(source)), after = color_image(read_glb(destination));
    CHECK(after.at<cv::Vec4b>(16,16)[0] < before.at<cv::Vec4b>(16,16)[0]-30);
    CHECK(after.at<cv::Vec4b>(16,16)[3] == before.at<cv::Vec4b>(16,16)[3]);
    for (int y=0;y<32;++y) for (int x=33;x<64;++x) REQUIRE(after.at<cv::Vec4b>(y,x) == before.at<cv::Vec4b>(y,x));
}

TEST_CASE("Soft selection weights proportionally blend an appearance edit", "[BeautyWorkbench][BeautyAppearance]") {
    Fixture fixture;
    const auto source = make_fixture(fixture);
    auto options = selected_left();
    const auto full_path = fixture.directory/"full.glb", half_path = fixture.directory/"half.glb";
    REQUIRE(edit_glb_appearance(source,full_path,options).success);
    options.face_weights = {.5f,.5f,0,0};
    REQUIRE(edit_glb_appearance(source,half_path,options).success);
    const auto original = color_image(read_glb(source)).at<cv::Vec4b>(16,16);
    const auto full = color_image(read_glb(full_path)).at<cv::Vec4b>(16,16);
    const auto half = color_image(read_glb(half_path)).at<cv::Vec4b>(16,16);
    REQUIRE(full[2] > original[2]);
    REQUIRE(half[2] > original[2]);
    REQUIRE(half[2] < full[2]);
    CHECK(std::abs(int(half[2])-int(std::lround((int(full[2])+int(original[2]))/2.0))) <= 1);
}

TEST_CASE("Local appearance preserves multiple materials sharing an embedded color image", "[BeautyWorkbench][BeautyAppearance]") {
    Fixture fixture;
    const auto source = make_fixture(fixture,false,false,true), destination = fixture.directory/"edited.glb";
    const auto result = edit_glb_appearance(source,destination,selected_left());
    INFO(result.error);
    REQUIRE(result.success);
    const auto original = read_glb(source), edited = read_glb(destination);
    require_preserved_geometry(original,edited);
    REQUIRE(edited.doc["materials"].size() == 2);
    for (size_t i=0;i<2;++i) REQUIRE(edited.doc["materials"][i]["pbrMetallicRoughness"]["baseColorFactor"] == original.doc["materials"][i]["pbrMetallicRoughness"]["baseColorFactor"]);
    const auto before = color_image(original,1), after = color_image(edited,1);
    for (int y=0;y<32;++y) for (int x=33;x<64;++x) REQUIRE(after.at<cv::Vec4b>(y,x) == before.at<cv::Vec4b>(y,x));
}

TEST_CASE("Invalid appearance selections and parameters leave no edited asset", "[BeautyWorkbench][BeautyAppearance]") {
    Fixture fixture;
    const auto source = make_fixture(fixture), destination = fixture.directory/"invalid.glb";
    const auto hash = model_artifact_sha256(source);
    for (int invalid=0;invalid<7;++invalid) {
        DYNAMIC_SECTION("invalid selection or parameter " << invalid) {
            auto options = selected_left();
            if (invalid==0) options.face_weights.clear();
            if (invalid==1) options.face_weights = {1,1};
            if (invalid==2) options.face_weights = {0,0,0,0};
            if (invalid==3) options.face_weights[0] = -1;
            if (invalid==4) options.face_weights[0] = std::numeric_limits<float>::quiet_NaN();
            if (invalid==5) options.brightness = .8;
            if (invalid==6) options.saturation = std::numeric_limits<double>::infinity();
            const auto result = edit_glb_appearance(source,destination,options);
            REQUIRE_FALSE(result.success);
            REQUIRE_FALSE(result.error.empty());
            REQUIRE_FALSE(boost::filesystem::exists(destination));
            REQUIRE(model_artifact_sha256(source) == hash);
        }
    }
}

TEST_CASE("Canceling an appearance edit leaves the source and destination untouched", "[BeautyWorkbench][BeautyAppearance]") {
    Fixture fixture;
    const auto source = make_fixture(fixture), destination = fixture.directory/"canceled.glb";
    const auto hash = model_artifact_sha256(source);
    for (int limit : {0,12}) {
        DYNAMIC_SECTION("cancel after " << limit << " checkpoints") {
            int count=0;
            const auto result = edit_glb_appearance(source,destination,selected_left(),[&]{return count++ >= limit;});
            REQUIRE(result.canceled);
            REQUIRE_FALSE(result.success);
            REQUIRE_FALSE(boost::filesystem::exists(destination));
            REQUIRE(model_artifact_sha256(source) == hash);
            REQUIRE(std::distance(boost::filesystem::directory_iterator(fixture.directory),boost::filesystem::directory_iterator()) == 1);
        }
    }
}

TEST_CASE("An appearance edit refuses a stale source or an existing destination", "[BeautyWorkbench][BeautyAppearance]") {
    Fixture fixture;
    const auto source = make_fixture(fixture), destination = fixture.directory/"existing.glb";
    boost::filesystem::copy_file(source,destination);
    const auto hash = model_artifact_sha256(destination);
    REQUIRE_FALSE(edit_glb_appearance(source,destination,selected_left()).success);
    REQUIRE(model_artifact_sha256(destination) == hash);
    const auto new_path = fixture.directory/"stale.glb";
    int count=0; bool changed=false;
    const auto result = edit_glb_appearance(source,new_path,selected_left(),[&]{
        if (++count==12) { boost::filesystem::ofstream output(source,std::ios::binary|std::ios::app); output.put(' '); changed=true; }
        return false;
    });
    REQUIRE(changed);
    REQUIRE_FALSE(result.success);
    REQUIRE(result.error.find("changed") != std::string::npos);
    REQUIRE_FALSE(boost::filesystem::exists(new_path));
}

TEST_CASE("Appearance edits retain native JPEG and transformed model geometry", "[BeautyWorkbench][BeautyAppearance]") {
    Fixture fixture;
    const auto samples = boost::filesystem::path(std::string(TEST_DATA_DIR))/"model_artifact";
    for (const std::string name : {"jpeg-textured","transformed","multi-material"}) {
        DYNAMIC_SECTION(name) {
            const auto source = samples/(name+".glb"), destination = fixture.directory/(name+".glb");
            TriangleMesh mesh; ObjInfo colors; std::string error;
            REQUIRE(load_model_artifact(source,mesh,colors,error));
            // The PNG fixtures contain fully bright red/green/blue/white:
            // HSV value is already 1, so positive brightness correctly clips
            // to the original pixels. Darkening exercises a real pixel edit.
            BeautyAppearanceOptions options; options.face_weights.assign(mesh.its.indices.size(),1); options.brightness=-.1;
            const auto hash = model_artifact_sha256(source);
            const auto result = edit_glb_appearance(source,destination,options);
            INFO(result.error);
            REQUIRE(result.success);
            REQUIRE(result.changed_pixels > 0);
            require_preserved_geometry(read_glb(source),read_glb(destination));
            REQUIRE(model_artifact_sha256(source) == hash);
            TexturedMesh original_surface, edited_surface;
            REQUIRE(load_assimp_textured_model(source.string(),original_surface,&error));
            REQUIRE(load_assimp_textured_model(destination.string(),edited_surface,&error));
            std::vector<unsigned char> before, after;
            int before_width=0,before_height=0,after_width=0,after_height=0;
            REQUIRE(decode_texture_to_pixels(original_surface.textures.front(),before,before_width,before_height));
            REQUIRE(decode_texture_to_pixels(edited_surface.textures.front(),after,after_width,after_height));
            REQUIRE(before_width == after_width);
            REQUIRE(before_height == after_height);
            size_t darkened=0;
            for (size_t pixel=0;pixel<before.size();pixel+=3)
                if (std::max({after[pixel],after[pixel+1],after[pixel+2]}) <
                    std::max({before[pixel],before[pixel+1],before[pixel+2]})) ++darkened;
            REQUIRE(darkened > 0);
            TriangleMesh edited; ObjInfo edited_colors;
            REQUIRE(load_model_artifact(destination,edited,edited_colors,error));
            REQUIRE(edited.its.vertices == mesh.its.vertices);
            REQUIRE(edited.its.indices == mesh.its.indices);
        }
    }
}

TEST_CASE("Unsupported vertex-only appearance editing returns an explicit recoverable error", "[BeautyWorkbench][BeautyAppearance]") {
    Fixture fixture;
    const auto source = boost::filesystem::path(std::string(TEST_DATA_DIR))/"model_artifact"/"vertex-material-color.glb";
    TriangleMesh mesh; ObjInfo colors; std::string error;
    REQUIRE(load_model_artifact(source,mesh,colors,error));
    BeautyAppearanceOptions options; options.face_weights.assign(mesh.its.indices.size(),1); options.brightness=.1;
    const auto destination = fixture.directory/"unsupported.glb";
    const auto result = edit_glb_appearance(source,destination,options);
    REQUIRE_FALSE(result.success);
    REQUIRE(result.error.find("untextured") != std::string::npos);
    REQUIRE_FALSE(boost::filesystem::exists(destination));
}

TEST_CASE("Changed vertex-colored puzzle bases preserve the edit and allow saving after restoring the source", "[BeautyWorkbench][BeautyAppearance][BeautyPuzzle]") {
    Fixture fixture;
    const auto source = fixture.directory / "vertex-base.glb";
    boost::filesystem::copy_file(boost::filesystem::path(std::string(TEST_DATA_DIR)) /
        "model_artifact" / "vertex-material-color.glb", source);
    const auto hash = model_artifact_sha256(source);
    auto options = puzzle_options(source);
    auto puzzle = BeautyPuzzle::decode(options.beauty_document["puzzle"], options.beauty_surface->geometry_id,
                                      options.beauty_surface->face_patch.size());
    REQUIRE_FALSE(puzzle.face_piece.empty());
    const auto piece = puzzle.face_piece.front();
    puzzle.paint(piece, {.2f, .7f, .4f, 1.f});
    options.beauty_document["puzzle"] = puzzle.encode();
    const auto record = options.beauty_document;
    const auto destination = fixture.directory / "saved.glb";
    // Vertex-color painting is supported. The recoverable failure is a changed
    // verified base, not the presence of COLOR_0 itself.
    boost::filesystem::ifstream original_file(source, std::ios::binary);
    const std::vector<unsigned char> original_bytes((std::istreambuf_iterator<char>(original_file)), {});
    original_file.close();
    auto changed = read_glb(source);
    changed.doc["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"] = {.25, 1., 1., 1.};
    write_glb(source, changed.doc, changed.binary);
    const auto changed_hash = model_artifact_sha256(source);
    REQUIRE(changed_hash != hash);
    const auto rejected = finish_model_artifact(source, destination, options);
    REQUIRE_FALSE(rejected.success);
    REQUIRE_FALSE(rejected.canceled);
    REQUIRE(rejected.error.find("original puzzle texture changed") != std::string::npos);
    CHECK_FALSE(rejected.changed_edit_record);
    CHECK(rejected.output_sha256.empty());
    CHECK_FALSE(boost::filesystem::exists(destination));
    CHECK(model_artifact_sha256(source) == changed_hash);
    CHECK(options.beauty_document == record);
    CHECK(std::distance(boost::filesystem::directory_iterator(fixture.directory), boost::filesystem::directory_iterator()) == 1);

    { boost::filesystem::ofstream restore(source, std::ios::binary | std::ios::trunc);
      restore.write(reinterpret_cast<const char*>(original_bytes.data()), original_bytes.size()); }
    REQUIRE(model_artifact_sha256(source) == hash);
    puzzle.clear_color(piece);
    options.beauty_document["puzzle"] = puzzle.encode();
    const auto restored = finish_model_artifact(source, destination, options);
    INFO(restored.error);
    REQUIRE(restored.success);
    CHECK(restored.changed_edit_record);
    CHECK(model_artifact_sha256(destination) == hash);
    CHECK(model_artifact_sha256(source) == hash);
}

TEST_CASE("Puzzle appearance assigns absolute face colors while retaining alpha geometry and unpainted pixels", "[BeautyWorkbench][BeautyAppearance][BeautyPuzzle]") {
    Fixture fixture;
    const auto source = make_neutral_fixture(fixture), destination = fixture.directory / "puzzle.glb";
    auto options = puzzle_colors();
    const auto result = edit_glb_appearance(source, destination, options);
    INFO(result.error);
    REQUIRE(result.success);
    REQUIRE(result.changed_pixels > 100);
    const auto original = read_glb(source), edited = read_glb(destination);
    require_preserved_geometry(original, edited);
    const auto before = color_image(original), after = color_image(edited);
    CHECK(after.at<cv::Vec4b>(16, 16) == cv::Vec4b(26, 51, 204, before.at<cv::Vec4b>(16, 16)[3]));
    CHECK(after.at<cv::Vec4b>(16, 48) == cv::Vec4b(204, 51, 26, before.at<cv::Vec4b>(16, 48)[3]));
    for (int y = 0; y < before.rows; ++y) for (int x = 0; x < before.cols; ++x) {
        CHECK(after.at<cv::Vec4b>(y, x)[3] == before.at<cv::Vec4b>(y, x)[3]);
        if (x == 31 || x == 32) CHECK(after.at<cv::Vec4b>(y, x) == before.at<cv::Vec4b>(y, x));
    }
    options.face_weights = {1, 1, 0, 0};
    const auto local_path = fixture.directory / "one-piece.glb";
    REQUIRE(edit_glb_appearance(source, local_path, options).success);
    const auto local = color_image(read_glb(local_path));
    for (int y = 0; y < before.rows; ++y) for (int x = 33; x < before.cols; ++x)
        REQUIRE(local.at<cv::Vec4b>(y, x) == before.at<cv::Vec4b>(y, x));
}

TEST_CASE("Conflicting puzzle colors on shared UVs fail without producing a partial asset", "[BeautyWorkbench][BeautyAppearance][BeautyPuzzle]") {
    Fixture fixture;
    const auto source = make_neutral_fixture(fixture, true), destination = fixture.directory / "conflict.glb";
    auto options = puzzle_colors();
    const auto hash = model_artifact_sha256(source);
    const auto result = edit_glb_appearance(source, destination, options);
    CHECK_FALSE(result.success);
    CHECK(result.error.find("shared UV") != std::string::npos);
    CHECK_FALSE(boost::filesystem::exists(destination));
    CHECK(model_artifact_sha256(source) == hash);
    options.face_target_colors.assign(4, {.8f, .2f, .1f});
    const auto compatible = edit_glb_appearance(source, destination, options);
    INFO(compatible.error);
    REQUIRE(compatible.success);
    REQUIRE(compatible.changed_pixels > 0);
}

TEST_CASE("Absolute puzzle colors bake material multipliers without modifying the source", "[BeautyWorkbench][BeautyAppearance][BeautyPuzzle]") {
    Fixture fixture;
    const auto source = make_fixture(fixture), destination = fixture.directory / "material-puzzle.glb";
    const auto hash = model_artifact_sha256(source);
    auto options = puzzle_colors();
    const auto result = edit_glb_appearance(source, destination, options);
    INFO(result.error);
    REQUIRE(result.success);
    CHECK(result.changed_pixels > 0);
    CHECK(model_artifact_sha256(source) == hash);
    const auto saved = read_glb(destination);
    const auto primitive = saved.doc["meshes"][0]["primitives"][0];
    const auto material = primitive["material"].get<size_t>();
    CHECK(saved.doc["materials"][material]["pbrMetallicRoughness"]["baseColorFactor"] == Json::array({1, 1, 1, 1}));
}

TEST_CASE("Invalid puzzle target colors fail without producing a partial asset", "[BeautyWorkbench][BeautyAppearance][BeautyPuzzle]") {
    Fixture fixture;
    const auto source = make_neutral_fixture(fixture), destination = fixture.directory / "invalid-puzzle.glb";
    auto options = puzzle_colors();
    for (int invalid = 0; invalid < 3; ++invalid) {
        DYNAMIC_SECTION("invalid absolute target " << invalid) {
            options = puzzle_colors();
            if (invalid == 0) options.face_target_colors.pop_back();
            if (invalid == 1) options.face_target_colors[0][1] = std::numeric_limits<float>::quiet_NaN();
            if (invalid == 2) options.brightness = .1;
            CHECK_FALSE(edit_glb_appearance(source, destination, options).success);
            CHECK_FALSE(boost::filesystem::exists(destination));
        }
    }
}

TEST_CASE("Puzzle saves restore released regions from the verified original texture", "[BeautyWorkbench][BeautyAppearance][BeautyPuzzle]") {
    Fixture fixture;
    const auto source = make_neutral_fixture(fixture);
    auto options = puzzle_options(source);
    auto puzzle = BeautyPuzzle::decode(options.beauty_document["puzzle"], options.beauty_surface->geometry_id,
                                      options.beauty_surface->face_patch.size());
    const uint32_t left = puzzle.face_piece[0], right = puzzle.face_piece[2];
    REQUIRE(left != right);
    puzzle.paint(left, {.8f, .2f, .1f, 1.f});
    options.beauty_document["puzzle"] = puzzle.encode();
    const auto first_path = fixture.directory / "first.glb";
    const auto first = finish_model_artifact(source, first_path, options);
    INFO(first.error);
    REQUIRE(first.success);
    REQUIRE(first.changed_edit_record);
    REQUIRE(first.changed());
    const auto before = color_image(read_glb(source));
    CHECK(color_image(read_glb(first_path)).at<cv::Vec4b>(16, 16) != before.at<cv::Vec4b>(16, 16));

    puzzle.clear_color(left);
    puzzle.paint(right, {.1f, .2f, .8f, 1.f});
    options.beauty_document["puzzle"] = puzzle.encode();
    const auto second_path = fixture.directory / "second.glb";
    const auto second = finish_model_artifact(first_path, second_path, options);
    INFO(second.error);
    REQUIRE(second.success);
    const auto second_pixels = color_image(read_glb(second_path));
    CHECK(second_pixels.at<cv::Vec4b>(16, 16) == before.at<cv::Vec4b>(16, 16));
    CHECK(second_pixels.at<cv::Vec4b>(16, 48) == cv::Vec4b(204, 51, 26, before.at<cv::Vec4b>(16, 48)[3]));
    puzzle.clear_color(right);
    options.beauty_document["puzzle"] = puzzle.encode();
    const auto restored_path = fixture.directory / "restored.glb";
    const auto restored = finish_model_artifact(second_path, restored_path, options);
    INFO(restored.error);
    REQUIRE(restored.success);
    CHECK(restored.changed_edit_record);
    CHECK(restored.changed());
    CHECK(restored.changed_texture_pixels == 0);
    CHECK(model_artifact_sha256(restored_path) == model_artifact_sha256(source));
    CHECK_FALSE(finish_model_artifact(source, restored_path, options).success);
}

TEST_CASE("Puzzle saves verify both identical input files even when reusing decoded geometry", "[BeautyAppearance][BeautyPuzzle]") {
    for (int mutation : {0, 1, 2}) {
        DYNAMIC_SECTION("input changed during save " << mutation) {
            Fixture fixture;
            const auto base = make_neutral_fixture(fixture);
            const auto source = fixture.directory / "copy.glb";
            const auto destination = fixture.directory / "saved.glb";
            boost::filesystem::copy_file(base, source);
            auto options = puzzle_options(base);
            options.beauty_surface.reset(); // Exercise building from the reused decoded colors.
            const auto hash = model_artifact_sha256(base);
            int checkpoints = 0;
            const auto result = finish_model_artifact(source, destination, options, [&] {
                if (++checkpoints == 2 && mutation != 0) {
                    boost::filesystem::ofstream output(mutation == 1 ? source : base,
                                                      std::ios::binary | std::ios::app);
                    output.put(' ');
                }
                return false;
            });
            INFO(result.error);
            if (mutation == 0) {
                REQUIRE(result.success);
                CHECK(model_artifact_sha256(destination) == hash);
                CHECK(model_artifact_sha256(source) == hash);
                CHECK(model_artifact_sha256(base) == hash);
            } else {
                CHECK_FALSE(result.success);
                CHECK_FALSE(result.changed_edit_record);
                CHECK(result.output_sha256.empty());
                CHECK_FALSE(boost::filesystem::exists(destination));
                CHECK(std::distance(boost::filesystem::directory_iterator(fixture.directory),
                                    boost::filesystem::directory_iterator()) == 2);
            }
        }
    }
}

TEST_CASE("Puzzle saves reject independently decoded geometry that differs from the original", "[BeautyAppearance][BeautyPuzzle]") {
    Fixture fixture;
    const auto base = make_neutral_fixture(fixture), source = fixture.directory / "different.glb";
    const auto destination = fixture.directory / "saved.glb";
    const auto options = puzzle_options(base);
    const auto base_hash = model_artifact_sha256(base);
    auto glb = read_glb(base);
    const size_t accessor_id = glb.doc["meshes"][0]["primitives"][0]["attributes"]["POSITION"].get<size_t>();
    const auto& accessor = glb.doc["accessors"][accessor_id];
    const auto& view = glb.doc["bufferViews"][accessor["bufferView"].get<size_t>()];
    const size_t offset = view.value("byteOffset", size_t(0)) + accessor.value("byteOffset", size_t(0));
    REQUIRE(offset + sizeof(float) <= glb.binary.size());
    std::vector<unsigned char> changed; append_float(changed, .125f);
    std::copy(changed.begin(), changed.end(), glb.binary.begin() + offset);
    write_glb(source, glb.doc, glb.binary);
    const auto source_hash = model_artifact_sha256(source);
    REQUIRE(source_hash != base_hash);
    const auto result = finish_model_artifact(source, destination, options);
    CHECK_FALSE(result.success);
    CHECK_FALSE(result.changed_edit_record);
    CHECK(result.output_sha256.empty());
    CHECK(result.error.find("different geometry") != std::string::npos);
    CHECK_FALSE(boost::filesystem::exists(destination));
    CHECK(model_artifact_sha256(base) == base_hash);
    CHECK(model_artifact_sha256(source) == source_hash);
    CHECK(std::distance(boost::filesystem::directory_iterator(fixture.directory),
                        boost::filesystem::directory_iterator()) == 2);
}

TEST_CASE("Puzzle saves reject invalid base references and clean canceled partition-only outputs", "[BeautyWorkbench][BeautyAppearance][BeautyPuzzle]") {
    Fixture fixture;
    const auto source = make_neutral_fixture(fixture), destination = fixture.directory / "invalid-base.glb";
    const auto options = puzzle_options(source);
    const auto hash = model_artifact_sha256(source);
    for (int invalid = 0; invalid < 4; ++invalid) {
        DYNAMIC_SECTION("invalid puzzle base " << invalid) {
            auto bad = options;
            if (invalid == 0) bad.beauty_document["puzzle_base_file"] = "../source.glb";
            if (invalid == 1) bad.beauty_document["puzzle_base_sha256"] = std::string(64, '0');
            if (invalid == 2) bad.beauty_document["puzzle"]["geometry_id"] = "different";
            if (invalid == 3) bad.beauty_document["puzzle_base_file"] = "missing.glb";
            CHECK_FALSE(finish_model_artifact(source, destination, bad).success);
            CHECK_FALSE(boost::filesystem::exists(destination));
            CHECK(model_artifact_sha256(source) == hash);
        }
    }
    for (int cancel_at : {1, 6}) {
        DYNAMIC_SECTION("cancel puzzle save at " << cancel_at) {
            int calls = 0;
            const auto canceled = finish_model_artifact(source, destination, options, [&] { return ++calls >= cancel_at; });
            CHECK(canceled.canceled);
            CHECK_FALSE(canceled.success);
            CHECK_FALSE(boost::filesystem::exists(destination));
            CHECK(model_artifact_sha256(source) == hash);
            CHECK(std::distance(boost::filesystem::directory_iterator(fixture.directory), boost::filesystem::directory_iterator()) == 1);
        }
    }
}

TEST_CASE("Subpixel unselected UV islands retain texels across winding and wrap modes", "[BeautyWorkbench][BeautyAppearance]") {
    for (float center : {.25f, 0.f, 1.f}) for (float radius : {.0001f, 0.f})
        for (bool reversed : {false, true}) for (int wrap : {10497, 33071, 33648}) {
            DYNAMIC_SECTION("center " << center << " radius " << radius << " reversed " << reversed << " wrap " << wrap) {
                Fixture fixture;
                const auto source = make_fixture(fixture), destination = fixture.directory / "edited.glb";
                auto original = read_glb(source);
                // The selected quad covers the image. The unselected quad has
                // a tiny (or collapsed) UV island that must still protect its
                // bilinear texels, including the texture's horizontal edges.
                const float left = std::clamp(center - radius, 0.f, 1.f);
                const float right = std::clamp(center + radius, 0.f, 1.f);
                std::array<std::array<float, 2>, 8> uvs{{{0,0},{1,0},{1,1},{0,1},
                    {left,.5f-radius},{right,.5f-radius},{right,.5f+radius},{left,.5f+radius}}};
                if (reversed) std::reverse(uvs.begin()+4, uvs.end());
                std::vector<unsigned char> encoded;
                for (const auto& uv : uvs) for (float value : uv) append_float(encoded, value);
                const auto& view = original.doc["bufferViews"][original.doc["accessors"][1]["bufferView"].get<size_t>()];
                const size_t offset = view.value("byteOffset", size_t(0));
                REQUIRE(offset + encoded.size() <= original.binary.size());
                std::copy(encoded.begin(), encoded.end(), original.binary.begin()+offset);
                original.doc["samplers"][0]["wrapS"] = wrap;
                original.doc["samplers"][0]["wrapT"] = wrap;
                write_glb(source, original.doc, original.binary);
                original = read_glb(source);
                const auto source_hash = model_artifact_sha256(source);
                const auto result = edit_glb_appearance(source, destination, selected_left());
                INFO(result.error);
                REQUIRE(result.success);
                REQUIRE(result.changed_pixels > 100);
                REQUIRE(model_artifact_sha256(source) == source_hash);
                const auto edited = read_glb(destination);
                require_preserved_geometry(original, edited);
                const auto before = color_image(original), after = color_image(edited);
                const int protected_x = std::min(63, int(center * 64));
                CHECK(after.at<cv::Vec4b>(16,protected_x) == before.at<cv::Vec4b>(16,protected_x));
                CHECK(after.at<cv::Vec4b>(16,40) != before.at<cv::Vec4b>(16,40));
                if (center == .25f)
                    CHECK(after.at<cv::Vec4b>(15,15) == before.at<cv::Vec4b>(15,15));
                for (int y=0; y<32; ++y) for (int x=0; x<64; ++x)
                    REQUIRE(after.at<cv::Vec4b>(y,x)[3] == before.at<cv::Vec4b>(y,x)[3]);
            }
        }

}

TEST_CASE("Corner puzzle colors retain geometry alpha and unpainted appearance with nonneutral material", "[BeautyAppearance][BeautyPuzzle][BeautyVertexColors]") {
    Fixture fixture;
    const auto source = make_corner_fixture(fixture), destination = fixture.directory / "painted.glb";
    TriangleMesh before; ObjInfo before_colors; std::string error;
    REQUIRE(load_model_artifact(source, before, before_colors, error));
    const auto original = read_glb(source);
    BeautyAppearanceOptions options; options.face_weights = {1,0}; options.face_target_colors = {{.9f,.2f,.1f},{0,0,0}};
    const auto result = edit_glb_appearance(source, destination, options);
    INFO(result.error);
    REQUIRE(result.success);
    CHECK(result.changed_vertices == 3);
    TriangleMesh after; ObjInfo after_colors;
    REQUIRE(load_model_artifact(destination, after, after_colors, error));
    CHECK(after.its.vertices == before.its.vertices);
    CHECK(after.its.indices == before.its.indices);
    for (int v : after.its.indices[0]) {
        for (size_t c = 0; c < 3; ++c) CHECK(std::abs(after_colors.vertex_colors[v][c] - options.face_target_colors[0][c]) < 1e-5);
        CHECK(std::abs(after_colors.vertex_colors[v][3] - before_colors.vertex_colors[v][3]) < 1e-6);
    }
    for (int v : after.its.indices[1]) for (size_t c = 0; c < 4; ++c)
        CHECK(std::abs(after_colors.vertex_colors[v][c] - before_colors.vertex_colors[v][c]) < 1e-5);
    const auto saved = read_glb(destination);
    CHECK(std::equal(original.binary.begin(), original.binary.end(), saved.binary.begin()));
    for (const char* field : {"nodes", "scenes", "scene"}) CHECK(saved.doc[field] == original.doc[field]);
    CHECK(saved.doc["meshes"][0]["primitives"][0]["indices"] == original.doc["meshes"][0]["primitives"][0]["indices"]);
    CHECK(saved.doc["meshes"][0]["primitives"][0]["attributes"]["POSITION"] == original.doc["meshes"][0]["primitives"][0]["attributes"]["POSITION"]);
    CHECK(saved.doc["materials"][0]["alphaMode"] == "BLEND");
    CHECK(saved.doc["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"][3] == original.doc["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"][3]);
    const size_t color_accessor = saved.doc["meshes"][0]["primitives"][0]["attributes"]["COLOR_0"].get<size_t>();
    const size_t color_view = saved.doc["accessors"][color_accessor]["bufferView"].get<size_t>();
    const size_t color_offset = saved.doc["bufferViews"][color_view]["byteOffset"].get<size_t>();
    for (size_t vertex = 0; vertex < 6; ++vertex) {
        float alpha = 0; std::memcpy(&alpha, saved.binary.data() + color_offset + vertex * 16 + 12, 4);
        CHECK(alpha == 1.f);
    }
    CHECK(model_artifact_sha256(source) == result.source_sha256);
    CHECK_FALSE(edit_glb_appearance(source, destination, options).success);
    CHECK(model_artifact_sha256(destination) == result.output_sha256);
}

TEST_CASE("Corner puzzle saves restore the verified base colors when a region is released", "[BeautyAppearance][BeautyPuzzle][BeautyVertexColors]") {
    Fixture fixture;
    const auto source = make_corner_fixture(fixture);
    auto options = puzzle_options(source);
    auto puzzle = BeautyPuzzle::decode(options.beauty_document["puzzle"], options.beauty_surface->geometry_id, options.beauty_surface->face_patch.size());
    puzzle.paint(puzzle.face_piece[0], {.9f,.2f,.1f,1});
    options.beauty_document["puzzle"] = puzzle.encode();
    const auto painted = fixture.directory / "painted.glb";
    const auto saved = finish_model_artifact(source, painted, options);
    INFO(saved.error); REQUIRE(saved.success); CHECK(saved.recolored_faces > 0);
    puzzle.clear_color(puzzle.face_piece[0]); options.beauty_document["puzzle"] = puzzle.encode();
    const auto restored = fixture.directory / "restored.glb";
    const auto restored_result = finish_model_artifact(painted, restored, options);
    INFO(restored_result.error); REQUIRE(restored_result.success);
    CHECK(model_artifact_sha256(restored) == model_artifact_sha256(source));
}

TEST_CASE("Corner puzzle cancellation and source changes leave no published version", "[BeautyAppearance][BeautyPuzzle][BeautyVertexColors]") {
    for (int stop : {1,10,-2,-1}) {
        Fixture fixture; const auto source = make_corner_fixture(fixture), destination = fixture.directory / "canceled.glb";
        BeautyAppearanceOptions options; options.face_weights = {1,0}; options.face_target_colors = {{.9f,.2f,.1f},{0,0,0}};
        const auto hash = model_artifact_sha256(source); int calls = 0;
        const auto result = edit_glb_appearance(source, destination, options, [&] {
            if (stop == -1) return boost::filesystem::exists(destination);
            if (stop == -2) return std::any_of(boost::filesystem::directory_iterator(fixture.directory), boost::filesystem::directory_iterator(),
                [](const auto& entry) { return boost::filesystem::is_directory(entry.path()); });
            return ++calls >= stop;
        });
        INFO(stop); REQUIRE(result.canceled); CHECK_FALSE(result.success); CHECK_FALSE(boost::filesystem::exists(destination));
        CHECK(model_artifact_sha256(source) == hash);
        CHECK(std::distance(boost::filesystem::directory_iterator(fixture.directory), boost::filesystem::directory_iterator()) == 1);
    }
    Fixture fixture; const auto source = make_corner_fixture(fixture), destination = fixture.directory / "stale.glb";
    BeautyAppearanceOptions options; options.face_weights = {1,0}; options.face_target_colors = {{.9f,.2f,.1f},{0,0,0}};
    bool changed = false;
    const auto result = edit_glb_appearance(source, destination, options, [&] {
        if (!changed && std::any_of(boost::filesystem::directory_iterator(fixture.directory), boost::filesystem::directory_iterator(),
            [](const auto& entry) { return boost::filesystem::is_directory(entry.path()); })) {
            auto replacement = read_glb(source); replacement.doc["asset"]["generator"] = "changed";
            write_glb(source, replacement.doc, replacement.binary); changed = true;
        }
        return false;
    });
    CHECK_FALSE(result.success); CHECK_FALSE(boost::filesystem::exists(destination));
    CHECK(changed);
}

TEST_CASE("Corner puzzle rejects malformed colors without partial output", "[BeautyAppearance][BeautyPuzzle][BeautyVertexColors]") {
    for (int bad : {1,2}) {
        Fixture fixture; const auto source = make_corner_fixture(fixture), destination = fixture.directory / "invalid.glb";
        auto doc = read_glb(source);
        if (bad == 1) doc.doc["accessors"][1]["count"] = 6000001;
        if (bad == 2) doc.doc["accessors"][1]["sparse"] = Json::object();
        write_glb(source, doc.doc, doc.binary);
        const auto hash = model_artifact_sha256(source);
        BeautyAppearanceOptions options; options.face_weights = {1,0}; options.face_target_colors = {{.9f,.2f,.1f},{0,0,0}};
        CHECK_FALSE(edit_glb_appearance(source, destination, options).success);
        CHECK_FALSE(boost::filesystem::exists(destination)); CHECK(model_artifact_sha256(source) == hash);
    }
}

TEST_CASE("Shared vertex puzzle colors preserve neighboring faces and ordered geometry", "[BeautyAppearance][BeautyPuzzle][BeautyVertexColors]") {
    for (const std::string name : {"vertex-material-color", "ushort-vertex-colors"}) {
        Fixture fixture;
        const auto source = fixture.directory/"source.glb";
        boost::filesystem::copy_file(boost::filesystem::path(std::string(TEST_DATA_DIR))/"model_artifact"/(name+".glb"),source);
        if (name == "vertex-material-color") {
            auto doc = read_glb(source);
            while (doc.binary.size() % 4) doc.binary.push_back(0);
            const size_t offset = doc.binary.size(), view = doc.doc["bufferViews"].size(), accessor = doc.doc["accessors"].size();
            for (size_t i = 0; i < 4; ++i) for (const float value : {0.f,1.f,0.f}) append_float(doc.binary,value);
            doc.doc["bufferViews"].push_back({{"buffer",0},{"byteOffset",offset},{"byteLength",48}});
            doc.doc["accessors"].push_back({{"bufferView",view},{"componentType",5126},{"count",4},{"type","VEC3"}});
            doc.doc["meshes"][0]["primitives"][0]["attributes"]["NORMAL"] = accessor;
            write_glb(source,doc.doc,doc.binary);
        }
        const auto destination = fixture.directory/"painted.glb";
        TriangleMesh before; ObjInfo before_colors; std::string error;
        REQUIRE(load_model_artifact(source,before,before_colors,error));
        const auto original = read_glb(source); const auto hash = model_artifact_sha256(source);
        BeautyAppearanceOptions options;
        options.face_weights.assign(before.its.indices.size(),0); options.face_weights[0] = 1;
        options.face_target_colors.resize(before.its.indices.size(),{.9f,.2f,.1f});
        const auto result = edit_glb_appearance(source,destination,options);
        INFO(name); INFO(result.error); REQUIRE(result.success);
        CHECK(result.changed_vertices == 3);
        TriangleMesh after; ObjInfo after_colors;
        REQUIRE(load_model_artifact(destination,after,after_colors,error));
        CHECK(SurfaceSelectionPersistence::geometry_fingerprint(after.its) == SurfaceSelectionPersistence::geometry_fingerprint(before.its));
        for (size_t f = 0; f < before.its.indices.size(); ++f) for (size_t c = 0; c < 3; ++c) {
            const int from = before.its.indices[f][c], to = after.its.indices[f][c];
            CHECK(after.its.vertices[to] == before.its.vertices[from]);
            for (size_t ch = 0; ch < 4; ++ch) {
                const float expected = f == 0 && ch < 3 ? options.face_target_colors[0][ch] : before_colors.vertex_colors[from][ch];
                CHECK(std::abs(after_colors.vertex_colors[to][ch] - expected) < 1e-5);
            }
        }
        const auto edited = read_glb(destination);
        CHECK(std::equal(original.binary.begin(),original.binary.end(),edited.binary.begin()));
        for (const char* field : {"nodes","scenes","scene"}) CHECK(edited.doc[field] == original.doc[field]);
        auto attribute_corner = [](const Glb& glb,const char* semantic,size_t corner) {
            const auto& primitive = glb.doc["meshes"][0]["primitives"][0];
            const auto& index = glb.doc["accessors"][primitive["indices"].get<size_t>()];
            const auto& index_view = glb.doc["bufferViews"][index["bufferView"].get<size_t>()];
            const size_t width = index["componentType"] == 5123 ? 2 : 4;
            const auto* bytes = glb.binary.data()+index_view.value("byteOffset",size_t(0))+index.value("byteOffset",size_t(0))+corner*width;
            const size_t vertex = width == 2 ? size_t(bytes[0]) | size_t(bytes[1]) << 8 : u32(bytes);
            const auto& accessor = glb.doc["accessors"][primitive["attributes"][semantic].get<size_t>()];
            const auto& view = glb.doc["bufferViews"][accessor["bufferView"].get<size_t>()];
            const size_t element = accessor["type"] == "VEC2" ? 8 : 12;
            const size_t offset = view.value("byteOffset",size_t(0))+accessor.value("byteOffset",size_t(0))+vertex*view.value("byteStride",element);
            return std::vector<unsigned char>(glb.binary.begin()+offset,glb.binary.begin()+offset+element);
        };
        for (size_t c = 0; c < before.its.indices.size()*3; ++c) {
            CHECK(attribute_corner(original,"TEXCOORD_0",c) == attribute_corner(edited,"TEXCOORD_0",c));
            if (name == "vertex-material-color") CHECK(attribute_corner(original,"NORMAL",c) == attribute_corner(edited,"NORMAL",c));
        }
        CHECK(model_artifact_sha256(source) == hash);
        const auto restored = remap_model_vertex_colors(before.its,before_colors.vertex_colors,after.its);
        for (size_t f = 0; f < before.its.indices.size(); ++f) for (size_t c = 0; c < 3; ++c)
            CHECK(restored[after.its.indices[f][c]] == before_colors.vertex_colors[before.its.indices[f][c]]);
    }
}

TEST_CASE("Shared vertex puzzle versions reopen repaint and release colors from their original", "[BeautyAppearance][BeautyPuzzle][BeautyVertexColors]") {
    Fixture fixture;
    const auto source = fixture.directory/"source.glb";
    boost::filesystem::copy_file(boost::filesystem::path(std::string(TEST_DATA_DIR))/"model_artifact"/"vertex-material-color.glb",source);
    const auto hash = model_artifact_sha256(source);
    auto options = puzzle_options(source);
    auto puzzle = BeautyPuzzle::decode(options.beauty_document["puzzle"],options.beauty_surface->geometry_id,options.beauty_surface->face_patch.size());
    puzzle.paint(puzzle.face_piece[0],{.9f,.2f,.1f,1}); options.beauty_document["puzzle"] = puzzle.encode();
    const auto painted = fixture.directory/"painted.glb";
    const auto saved = finish_model_artifact(source,painted,options);
    INFO(saved.error); REQUIRE(saved.success);
    options.beauty_surface.reset();
    puzzle.paint(puzzle.face_piece[0],{.1f,.2f,.9f,1}); options.beauty_document["puzzle"] = puzzle.encode();
    const auto repainted = fixture.directory/"repainted.glb";
    const auto second = finish_model_artifact(painted,repainted,options);
    INFO(second.error); REQUIRE(second.success);
    puzzle.clear_color(puzzle.face_piece[0]); options.beauty_document["puzzle"] = puzzle.encode();
    const auto released = fixture.directory/"released.glb";
    const auto third = finish_model_artifact(repainted,released,options);
    INFO(third.error); REQUIRE(third.success);
    CHECK(model_artifact_sha256(released) == hash);
    CHECK(model_artifact_sha256(source) == hash);
}

TEST_CASE("Absolute texture colors bake material RGB while preserving unpainted appearance alpha and normal images", "[BeautyAppearance][BeautyPuzzle]") {
    for (bool zero_channel : {false, true}) for (float weight : {1.f, .5f}) {
        Fixture fixture; const auto source = make_fixture(fixture);
        auto original = read_glb(source);
        const std::array<double, 4> factor {zero_channel ? 0. : .25, .6, .5, .7};
        original.doc["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"] = factor;
        write_glb(source, original.doc, original.binary);
        const auto hash = model_artifact_sha256(source);
        const auto before_pixels = color_image(original);
        BeautyAppearanceOptions options; options.face_weights = {weight, weight, 0, 0};
        options.face_target_colors.assign(4, {.9f, .2f, .1f});
        const auto destination = fixture.directory / "baked.glb";
        const auto result = edit_glb_appearance(source, destination, options);
        INFO(zero_channel); INFO(weight); INFO(result.error); REQUIRE(result.success);
        const auto saved = read_glb(destination);
        const size_t mi = saved.doc["meshes"][0]["primitives"][0]["material"].get<size_t>();
        REQUIRE(mi != 0);
        CHECK(saved.doc["materials"][0] == original.doc["materials"][0]);
        CHECK(saved.doc["materials"][mi]["normalTexture"] == original.doc["materials"][0]["normalTexture"]);
        CHECK(saved.doc["materials"][mi]["alphaMode"] == original.doc["materials"][0]["alphaMode"]);
        const auto normalized = saved.doc["materials"][mi]["pbrMetallicRoughness"]["baseColorFactor"].get<std::array<double, 4>>();
        CHECK(normalized[0] == 1); CHECK(normalized[1] == 1); CHECK(normalized[2] == 1); CHECK(normalized[3] == factor[3]);
        const auto after_pixels = color_image(saved, mi);
        auto linear = [](double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); };
        auto srgb = [](double v) { return v <= .0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - .055; };
        for (int y = 0; y < before_pixels.rows; ++y) for (int x = 0; x < before_pixels.cols; ++x) {
            CHECK(after_pixels.at<cv::Vec4b>(y, x)[3] == before_pixels.at<cv::Vec4b>(y, x)[3]);
            if (x >= 32) for (size_t ch = 0; ch < 3; ++ch) {
                const double expected = srgb(linear(before_pixels.at<cv::Vec4b>(y, x)[2-ch] / 255.) * factor[ch]) * 255;
                CHECK(std::abs(after_pixels.at<cv::Vec4b>(y, x)[2-ch] - expected) <= .501);
            }
        }
        for (size_t ch = 0; ch < 3; ++ch) {
            const double old = linear(before_pixels.at<cv::Vec4b>(16, 10)[2-ch] / 255.) * factor[ch];
            const double expected = srgb(old * (1-weight) + linear(options.face_target_colors[0][ch]) * weight) * 255;
            CHECK(std::abs(after_pixels.at<cv::Vec4b>(16, 10)[2-ch] - expected) <= 1.01);
        }
        CHECK(saved.doc["meshes"][0]["primitives"][0]["attributes"] == original.doc["meshes"][0]["primitives"][0]["attributes"]);
        CHECK(saved.doc["meshes"][0]["primitives"][0]["indices"] == original.doc["meshes"][0]["primitives"][0]["indices"]);
        for (const char* field : {"nodes", "scenes", "scene", "samplers"}) CHECK(saved.doc[field] == original.doc[field]);
        CHECK(std::equal(original.binary.begin(), original.binary.end(), saved.binary.begin()));
        CHECK(model_artifact_sha256(source) == hash);
    }
}

TEST_CASE("Materials sharing an image retain independent RGB factors during absolute texture painting", "[BeautyAppearance][BeautyPuzzle]") {
    for (bool paint_second : {false, true}) for (bool neutral_second : {false, true}) {
        Fixture fixture; const auto source = make_fixture(fixture, false, false, true);
        auto original = read_glb(source);
        if (neutral_second) original.doc["materials"][1]["pbrMetallicRoughness"]["baseColorFactor"] = {1, 1, 1, .7};
        write_glb(source, original.doc, original.binary);
        BeautyAppearanceOptions options; options.face_weights = {1, 1, paint_second ? 1.f : 0.f, paint_second ? 1.f : 0.f};
        options.face_target_colors = {{.9f, .2f, .1f}, {.9f, .2f, .1f}, {.1f, .2f, .9f}, {.1f, .2f, .9f}};
        const auto destination = fixture.directory / "independent.glb";
        const auto result = edit_glb_appearance(source, destination, options);
        INFO(paint_second); INFO(neutral_second); INFO(result.error); REQUIRE(result.success);
        const auto saved = read_glb(destination);
        const size_t first = saved.doc["meshes"][0]["primitives"][0]["material"].get<size_t>();
        const size_t second = saved.doc["meshes"][0]["primitives"][1]["material"].get<size_t>();
        CHECK(saved.doc["materials"][0] == original.doc["materials"][0]);
        const auto first_pixels = color_image(saved, first);
        CHECK(int(first_pixels.at<cv::Vec4b>(16,10)[2]) == std::lround(255. * options.face_target_colors[0][0]));
        CHECK(int(first_pixels.at<cv::Vec4b>(16,10)[0]) == std::lround(255. * options.face_target_colors[0][2]));
        const auto second_pixels = color_image(saved, second);
        if (paint_second) {
            CHECK(int(second_pixels.at<cv::Vec4b>(16,50)[0]) == std::lround(255. * options.face_target_colors[2][2]));
            CHECK(int(second_pixels.at<cv::Vec4b>(16,50)[2]) == std::lround(255. * options.face_target_colors[2][0]));
        } else {
            CHECK(saved.doc["meshes"][0]["primitives"][1] == original.doc["meshes"][0]["primitives"][1]);
            CHECK(saved.doc["materials"][1] == original.doc["materials"][1]);
            CHECK(cv::countNonZero(color_image(original,1).reshape(1) != second_pixels.reshape(1)) == 0);
        }
        TriangleMesh before, after; ObjInfo before_colors, after_colors; std::string error;
        REQUIRE(load_model_artifact(source, before, before_colors, error));
        REQUIRE(load_model_artifact(destination, after, after_colors, error));
        CHECK(after.its.vertices == before.its.vertices); CHECK(after.its.indices == before.its.indices);
        for (const char* field : {"nodes", "scenes", "scene"}) CHECK(saved.doc[field] == original.doc[field]);
        for (size_t mi = 0; mi < 2; ++mi) {
            const size_t actual = saved.doc["meshes"][0]["primitives"][mi]["material"].get<size_t>();
            CHECK(saved.doc["materials"][actual]["normalTexture"] == original.doc["materials"][mi]["normalTexture"]);
        }
        CHECK(std::equal(original.binary.begin(),original.binary.end(),saved.binary.begin()));
        CHECK(model_artifact_sha256(source) == result.source_sha256);
    }
}

TEST_CASE("Nonneutral texture puzzle versions repaint and release their original material and image bytes", "[BeautyAppearance][BeautyPuzzle]") {
    Fixture fixture; const auto source = make_fixture(fixture);
    const auto hash = model_artifact_sha256(source);
    auto options = puzzle_options(source);
    auto puzzle = BeautyPuzzle::decode(options.beauty_document["puzzle"], options.beauty_surface->geometry_id, options.beauty_surface->face_patch.size());
    puzzle.paint(puzzle.face_piece[0], {.9f, .2f, .1f, 1}); options.beauty_document["puzzle"] = puzzle.encode();
    const auto painted = fixture.directory / "painted.glb";
    const auto first = finish_model_artifact(source, painted, options);
    INFO(first.error); REQUIRE(first.success);
    options.beauty_surface.reset();
    puzzle.paint(puzzle.face_piece[0], {.1f, .2f, .9f, 1}); options.beauty_document["puzzle"] = puzzle.encode();
    const auto repainted = fixture.directory / "repainted.glb";
    const auto second = finish_model_artifact(painted, repainted, options);
    INFO(second.error); REQUIRE(second.success);
    puzzle.clear_color(puzzle.face_piece[0]); options.beauty_document["puzzle"] = puzzle.encode();
    const auto released = fixture.directory / "released.glb";
    const auto third = finish_model_artifact(repainted, released, options);
    INFO(third.error); REQUIRE(third.success);
    CHECK(model_artifact_sha256(released) == hash); CHECK(model_artifact_sha256(source) == hash);
}

TEST_CASE("Multiple untextured primitives paint independently and retain untouched material colors", "[BeautyAppearance][BeautyVertexColors]") {
    for (bool shared_material : {false, true}) for (bool paint_second : {false, true}) {
        Fixture fixture; const auto source = make_fixture(fixture, false, false, true);
        auto original = read_glb(source);
        for (auto& material : original.doc["materials"]) material["pbrMetallicRoughness"].erase("baseColorTexture");
        if (shared_material) original.doc["meshes"][0]["primitives"][1]["material"] = 0;
        write_glb(source, original.doc, original.binary);
        const auto hash = model_artifact_sha256(source);
        TriangleMesh before; ObjInfo before_colors; std::string error;
        REQUIRE(load_model_artifact(source, before, before_colors, error));
        BeautyAppearanceOptions options; options.face_weights = {1, 0, paint_second ? .5f : 0.f, 0};
        options.face_target_colors.assign(4, {.9f, .2f, .1f});
        const auto destination = fixture.directory / "painted.glb";
        const auto result = edit_glb_appearance(source, destination, options);
        INFO(shared_material); INFO(result.error); REQUIRE(result.success);
        CHECK(result.changed_vertices == (paint_second ? 6 : 3)); CHECK(result.changed_pixels == 0);
        TriangleMesh after; ObjInfo after_colors;
        REQUIRE(load_model_artifact(destination, after, after_colors, error));
        REQUIRE(after.its.indices.size() == before.its.indices.size());
        for (size_t f = 0; f < 4; ++f) for (size_t c = 0; c < 3; ++c) {
            const int from = before.its.indices[f][c], to = after.its.indices[f][c];
            CHECK(after.its.vertices[to] == before.its.vertices[from]);
            for (size_t ch = 0; ch < 4; ++ch) {
                float expected = before_colors.vertex_colors[from][ch];
                if (options.face_weights[f] > 0 && ch < 3) {
                    auto linear = [](float v) { return v <= .04045f ? v / 12.92f : std::pow((v + .055f) / 1.055f, 2.4f); };
                    const float value = linear(expected) + options.face_weights[f] * (linear(options.face_target_colors[f][ch]) - linear(expected));
                    expected = value <= .0031308f ? 12.92f * value : 1.055f * std::pow(value, 1.f / 2.4f) - .055f;
                }
                CHECK(std::abs(after_colors.vertex_colors[to][ch] - expected) < 1e-5);
            }
        }
        const auto saved = read_glb(destination);
        if (!paint_second) CHECK(saved.doc["meshes"][0]["primitives"][1] == original.doc["meshes"][0]["primitives"][1]);
        for (size_t mi = 0; mi < 2; ++mi) CHECK(saved.doc["materials"][mi] == original.doc["materials"][mi]);
        for (const char* field : {"nodes", "scenes", "scene"}) CHECK(saved.doc[field] == original.doc[field]);
        CHECK(std::equal(original.binary.begin(), original.binary.end(), saved.binary.begin()));
        CHECK(model_artifact_sha256(source) == hash);
    }
}

TEST_CASE("Mixed texture and vertex primitives save selected colors through both appearance paths", "[BeautyAppearance][BeautyVertexColors]") {
    for (int selection : {0, 1, 2}) {
        Fixture fixture; const auto source = make_fixture(fixture, false, false, true);
        auto original = read_glb(source);
        original.doc["materials"][0]["pbrMetallicRoughness"].erase("baseColorTexture");
        original.doc["materials"][1]["pbrMetallicRoughness"]["baseColorFactor"] = {1, 1, 1, .7};
        write_glb(source, original.doc, original.binary);
        TriangleMesh before; ObjInfo before_colors; std::string error;
        REQUIRE(load_model_artifact(source, before, before_colors, error));
        BeautyAppearanceOptions options; options.face_weights = {selection != 1 ? 1.f : 0.f, 0, selection != 0 ? 1.f : 0.f, 0};
        options.face_target_colors = {{.9f, .2f, .1f}, {0, 0, 0}, {.1f, .2f, .9f}, {0, 0, 0}};
        const auto destination = fixture.directory / "mixed.glb";
        const auto result = edit_glb_appearance(source, destination, options);
        INFO(selection); INFO(result.error); REQUIRE(result.success);
        CHECK(result.changed_vertices == (selection != 1 ? 3 : 0));
        CHECK((result.changed_pixels > 0) == (selection != 0));
        TriangleMesh after; ObjInfo after_colors;
        REQUIRE(load_model_artifact(destination, after, after_colors, error));
        REQUIRE(after.its.indices.size() == before.its.indices.size());
        for (size_t f = 0; f < 4; ++f) for (size_t c = 0; c < 3; ++c)
            CHECK(after.its.vertices[after.its.indices[f][c]] == before.its.vertices[before.its.indices[f][c]]);
        for (size_t f = 0; f < 2; ++f) for (size_t c = 0; c < 3; ++c) for (size_t ch = 0; ch < 4; ++ch) {
            const float expected = f == 0 && selection != 1 && ch < 3 ? options.face_target_colors[0][ch] : before_colors.vertex_colors[before.its.indices[f][c]][ch];
            CHECK(std::abs(after_colors.vertex_colors[after.its.indices[f][c]][ch] - expected) < 1e-5);
        }
        const auto saved = read_glb(destination);
        const auto before_pixels = color_image(original, 1), after_pixels = color_image(saved, 1);
        for (int y = 0; y < before_pixels.rows; ++y) for (int x = 0; x < before_pixels.cols; ++x) {
            CHECK(before_pixels.at<cv::Vec4b>(y, x)[3] == after_pixels.at<cv::Vec4b>(y, x)[3]);
            if (x < 30 || selection == 0) CHECK(before_pixels.at<cv::Vec4b>(y, x) == after_pixels.at<cv::Vec4b>(y, x));
        }
        CHECK(saved.doc["materials"][1]["normalTexture"] == original.doc["materials"][1]["normalTexture"]);
        CHECK(std::equal(original.binary.begin(), original.binary.end(), saved.binary.begin()));
        CHECK(model_artifact_sha256(source) == result.source_sha256);
    }
}

TEST_CASE("Multiple primitive puzzle versions repaint and release to their verified original", "[BeautyAppearance][BeautyVertexColors][BeautyPuzzle]") {
    Fixture fixture; const auto source = make_fixture(fixture, false, false, true);
    auto doc = read_glb(source);
    for (auto& material : doc.doc["materials"]) material["pbrMetallicRoughness"].erase("baseColorTexture");
    write_glb(source, doc.doc, doc.binary);
    const auto hash = model_artifact_sha256(source);
    auto options = puzzle_options(source);
    auto puzzle = BeautyPuzzle::decode(options.beauty_document["puzzle"], options.beauty_surface->geometry_id, options.beauty_surface->face_patch.size());
    puzzle.paint(puzzle.face_piece[0], {.9f, .2f, .1f, 1}); options.beauty_document["puzzle"] = puzzle.encode();
    const auto painted = fixture.directory / "painted.glb";
    const auto first = finish_model_artifact(source, painted, options);
    INFO(first.error); REQUIRE(first.success);
    options.beauty_surface.reset();
    puzzle.paint(puzzle.face_piece[0], {.1f, .2f, .9f, 1}); options.beauty_document["puzzle"] = puzzle.encode();
    const auto repainted = fixture.directory / "repainted.glb";
    const auto second = finish_model_artifact(painted, repainted, options);
    INFO(second.error); REQUIRE(second.success);
    puzzle.clear_color(puzzle.face_piece[0]); options.beauty_document["puzzle"] = puzzle.encode();
    const auto released = fixture.directory / "released.glb";
    const auto third = finish_model_artifact(repainted, released, options);
    INFO(third.error); REQUIRE(third.success);
    CHECK(model_artifact_sha256(released) == hash); CHECK(model_artifact_sha256(source) == hash);
}


TEST_CASE("Constant texture vertex RGB bakes privately while preserving native alpha and untouched faces", "[BeautyAppearance][BeautyVertexColors]") {
    for (int component : {5126, 5121, 5123}) for (float weight : {1.f, .5f}) {
        Fixture fixture; const auto source = make_fixture(fixture); auto original = read_glb(source);
        while (original.binary.size() % 4) original.binary.push_back(0);
        const size_t offset = original.binary.size(), view = original.doc["bufferViews"].size(), accessor = original.doc["accessors"].size();
        const std::array<float, 3> vertex {.2f, .4f, 0.f};
        for (size_t v = 0; v < 8; ++v) for (float ch : {vertex[0], vertex[1], vertex[2], v % 2 ? .8f : .4f}) {
            if (component == 5126) append_float(original.binary, ch);
            else if (component == 5121) original.binary.push_back(static_cast<unsigned char>(std::lround(ch * 255)));
            else { const auto value = static_cast<unsigned>(std::lround(ch * 65535)); original.binary.push_back(value & 255); original.binary.push_back(value >> 8); }
        }
        original.doc["bufferViews"].push_back({{"buffer",0},{"byteOffset",offset},{"byteLength",original.binary.size()-offset}});
        Json color {{"bufferView",view},{"componentType",component},{"count",8},{"type","VEC4"}};
        if (component != 5126) color["normalized"] = true;
        original.doc["accessors"].push_back(color); original.doc["meshes"][0]["primitives"][0]["attributes"]["COLOR_0"] = accessor;
        original.doc["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"] = {.8,.9,1.,.7};
        write_glb(source, original.doc, original.binary); const auto hash = model_artifact_sha256(source);
        BeautyAppearanceOptions options; options.face_weights = {weight,weight,0,0}; options.face_target_colors.assign(4,{.9f,.2f,.1f});
        const auto destination = fixture.directory / "vertex-baked.glb"; const auto result = edit_glb_appearance(source,destination,options);
        INFO(component); INFO(weight); INFO(result.error); REQUIRE(result.success);
        const auto saved = read_glb(destination); const auto& primitive = saved.doc["meshes"][0]["primitives"][0];
        const size_t mi = primitive["material"].get<size_t>(); REQUIRE(mi != 0);
        const auto before = color_image(original), after = color_image(saved,mi);
        auto linear = [](double v) { return v <= .04045 ? v/12.92 : std::pow((v+.055)/1.055,2.4); };
        auto srgb = [](double v) { return v <= .0031308 ? v*12.92 : 1.055*std::pow(v,1/2.4)-.055; };
        for (int y = 0; y < before.rows; ++y) for (int x = 0; x < before.cols; ++x) {
            CHECK(before.at<cv::Vec4b>(y,x)[3] == after.at<cv::Vec4b>(y,x)[3]);
            if (x >= 32) for (size_t ch = 0; ch < 3; ++ch) {
                const double factor = original.doc["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"][ch].get<double>();
                CHECK(std::abs(after.at<cv::Vec4b>(y,x)[2-ch] - 255*srgb(linear(before.at<cv::Vec4b>(y,x)[2-ch]/255.)*factor*vertex[ch])) <= .501);
            }
        }
        for (size_t ch = 0; ch < 3; ++ch) {
            const double factor = original.doc["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"][ch].get<double>();
            const double old = linear(before.at<cv::Vec4b>(16,10)[2-ch]/255.)*factor*vertex[ch];
            CHECK(std::abs(after.at<cv::Vec4b>(16,10)[2-ch] - 255*srgb(old*(1-weight)+linear(options.face_target_colors[0][ch])*weight)) <= 1.01);
        }
        const auto& new_color = saved.doc["accessors"][primitive["attributes"]["COLOR_0"].get<size_t>()];
        const size_t color_offset = saved.doc["bufferViews"][new_color["bufferView"].get<size_t>()]["byteOffset"].get<size_t>();
        for (size_t v=0;v<8;++v) for (size_t ch=0;ch<4;++ch) {
            float value; std::memcpy(&value,saved.binary.data()+color_offset+v*16+ch*4,4);
            CHECK(std::abs(value-(ch<3 ? 1.f : v%2 ? .8f : .4f)) < 1e-6);
        }
        CHECK(primitive["indices"] == original.doc["meshes"][0]["primitives"][0]["indices"]);
        for (const char* attr : {"POSITION","NORMAL","TEXCOORD_0"}) if (primitive["attributes"].contains(attr))
            CHECK(primitive["attributes"][attr] == original.doc["meshes"][0]["primitives"][0]["attributes"][attr]);
        CHECK(saved.doc["materials"][0] == original.doc["materials"][0]);
        CHECK(saved.doc["materials"][mi]["normalTexture"] == original.doc["materials"][0]["normalTexture"]);
        CHECK(saved.doc["materials"][mi]["pbrMetallicRoughness"]["baseColorFactor"][3] == .7);
        CHECK(std::equal(original.binary.begin(),original.binary.end(),saved.binary.begin())); CHECK(model_artifact_sha256(source) == hash);
    }
}

TEST_CASE("Textured primitives sharing a material retain separate constant vertex RGB factors", "[BeautyAppearance][BeautyVertexColors]") {
    Fixture fixture; const auto source=make_fixture(fixture,false,false,true); auto original=read_glb(source);
    original.doc["meshes"][0]["primitives"][1]["material"]=0;
    while(original.binary.size()%4) original.binary.push_back(0);
    const size_t offset=original.binary.size(), view=original.doc["bufferViews"].size(), accessor=original.doc["accessors"].size();
    for(size_t v=0;v<8;++v) for(float ch : {v<4 ? .2f : .8f,.4f,.6f,1.f}) append_float(original.binary,ch);
    original.doc["bufferViews"].push_back({{"buffer",0},{"byteOffset",offset},{"byteLength",128}});
    original.doc["accessors"].push_back({{"bufferView",view},{"componentType",5126},{"count",8},{"type","VEC4"}});
    for(auto& primitive:original.doc["meshes"][0]["primitives"]) primitive["attributes"]["COLOR_0"]=accessor;
    write_glb(source,original.doc,original.binary);
    BeautyAppearanceOptions options; options.face_weights={1,1,0,0}; options.face_target_colors.assign(4,{.9f,.2f,.1f});
    const auto destination=fixture.directory/"separate.glb"; const auto result=edit_glb_appearance(source,destination,options);
    INFO(result.error); REQUIRE(result.success); const auto saved=read_glb(destination);
    CHECK(saved.doc["meshes"][0]["primitives"][1] == original.doc["meshes"][0]["primitives"][1]);
    CHECK(saved.doc["materials"][0] == original.doc["materials"][0]); CHECK(color_image(saved,0).at<cv::Vec4b>(16,50) == color_image(original,0).at<cv::Vec4b>(16,50));
    TexturedMesh before,after; std::string error; std::vector<std::array<float,4>> before_colors,after_colors;
    REQUIRE(load_assimp_textured_model(source.string(),before,&error,&before_colors));
    REQUIRE(load_assimp_textured_model(destination.string(),after,&error,&after_colors));
    REQUIRE(after.indices.size()==before.indices.size());
    for(size_t f=2;f<4;++f) for(size_t c=0;c<3;++c) CHECK(after_colors[after.indices[f][c]] == before_colors[before.indices[f][c]]);
    auto finish=puzzle_options(source); auto puzzle=BeautyPuzzle::decode(finish.beauty_document["puzzle"],finish.beauty_surface->geometry_id,finish.beauty_surface->face_patch.size());
    puzzle.paint(puzzle.face_piece[0],{.9f,.2f,.1f,1}); finish.beauty_document["puzzle"]=puzzle.encode();
    const auto painted=fixture.directory/"painted.glb"; auto first=finish_model_artifact(source,painted,finish); INFO(first.error); REQUIRE(first.success);
    finish.beauty_surface.reset(); puzzle.paint(puzzle.face_piece[0],{.1f,.2f,.9f,1}); finish.beauty_document["puzzle"]=puzzle.encode();
    const auto repainted=fixture.directory/"repainted.glb"; auto second=finish_model_artifact(painted,repainted,finish); INFO(second.error); REQUIRE(second.success);
    puzzle.clear_color(puzzle.face_piece[0]); finish.beauty_document["puzzle"]=puzzle.encode();
    const auto released=fixture.directory/"released.glb"; auto third=finish_model_artifact(repainted,released,finish); INFO(third.error); REQUIRE(third.success);
    CHECK(model_artifact_sha256(released)==model_artifact_sha256(source));
}

TEST_CASE("Gradient texture painting preserves unselected shared corners and alpha", "[BeautyAppearance][BeautyVertexColors]") {
    for (int component : {5126,5121,5123}) for (float weight : {1.f,.5f}) {
        Fixture fixture; const auto source = make_fixture(fixture);
        auto original = read_glb(source);
        while (original.binary.size()%4) original.binary.push_back(0);
        const size_t offset=original.binary.size(), view=original.doc["bufferViews"].size(), accessor=original.doc["accessors"].size();
        const std::array<std::array<float,4>,8> values {{{.2f,.3f,.4f,.4f},{.8f,.3f,.4f,.8f},
            {.8f,.7f,.4f,.4f},{.2f,.7f,.4f,.8f},{.4f,.2f,.7f,.4f},{.7f,.2f,.7f,.8f},
            {.7f,.8f,.7f,.4f},{.4f,.8f,.7f,.8f}}};
        for (const auto& rgba : values) for (float ch : rgba) {
            if (component==5126) append_float(original.binary,ch);
            else if (component==5121) original.binary.push_back(static_cast<unsigned char>(std::lround(ch*255)));
            else { const unsigned value=unsigned(std::lround(ch*65535)); original.binary.push_back(value&255); original.binary.push_back(value>>8); }
        }
        original.doc["bufferViews"].push_back({{"buffer",0},{"byteOffset",offset},{"byteLength",original.binary.size()-offset}});
        Json color {{"bufferView",view},{"componentType",component},{"count",8},{"type","VEC4"}};
        if (component!=5126) color["normalized"]=true;
        original.doc["accessors"].push_back(color);
        original.doc["meshes"][0]["primitives"][0]["attributes"]["COLOR_0"]=accessor;
        original.doc["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"]={.8,.9,1.,.7};
        write_glb(source,original.doc,original.binary);
        const auto hash=model_artifact_sha256(source);
        BeautyAppearanceOptions options; options.face_weights={weight,0,0,0}; options.face_target_colors.assign(4,{.9f,.2f,.1f});
        const auto destination=fixture.directory/"gradient.glb";
        const auto result=edit_glb_appearance(source,destination,options);
        INFO(component); INFO(weight); INFO(result.error); REQUIRE(result.success);
        const auto saved=read_glb(destination); const auto& runs=saved.doc["meshes"][0]["primitives"];
        REQUIRE(runs.size()==2);
        CHECK(runs[1]["material"]==0); CHECK(runs[1]["attributes"]==original.doc["meshes"][0]["primitives"][0]["attributes"]);
        for (const char* field : {"nodes","scenes","scene","samplers"}) CHECK(saved.doc[field]==original.doc[field]);
        CHECK(saved.doc["materials"][0]==original.doc["materials"][0]); CHECK(saved.doc["images"][0]==original.doc["images"][0]);
        CHECK(std::equal(original.binary.begin(),original.binary.end(),saved.binary.begin()));
        TexturedMesh before,after; std::vector<std::array<float,4>> before_colors,after_colors; std::string error;
        REQUIRE(load_assimp_textured_model(source.string(),before,&error,&before_colors));
        REQUIRE(load_assimp_textured_model(destination.string(),after,&error,&after_colors));
        REQUIRE(before.indices.size()==after.indices.size());
        for (size_t face=0;face<4;++face) for (size_t corner=0;corner<3;++corner) {
            const auto bi=before.indices[face][corner], ai=after.indices[face][corner];
            CHECK(after.vertices[ai]==before.vertices[bi]); CHECK(after.uvs[ai]==before.uvs[bi]);
            CHECK(after_colors[ai][3]==before_colors[bi][3]);
            if (face>0) CHECK(after_colors[ai]==before_colors[bi]);
            else for (size_t ch=0;ch<3;++ch) CHECK(after_colors[ai][ch]==1);
        }
        const size_t mi=runs[0]["material"].get<size_t>();
        CHECK(saved.doc["materials"][mi]["normalTexture"]==original.doc["materials"][0]["normalTexture"]);
        CHECK(saved.doc["materials"][mi]["pbrMetallicRoughness"]["baseColorFactor"][3]==.7);
        const auto base=color_image(original), baked=color_image(saved,mi), untouched=color_image(saved,0);
        CHECK(cv::norm(base,untouched,cv::NORM_INF)==0);
        auto linear=[](double v){return v<=.04045?v/12.92:std::pow((v+.055)/1.055,2.4);};
        auto srgb=[](double v){return v<=.0031308?v*12.92:1.055*std::pow(v,1/2.4)-.055;};
        std::array<std::array<double,2>,3> uv;
        for (size_t c=0;c<3;++c) { const auto& p=before.uvs[before.indices[0][c]]; uv[c]={p[0]*base.cols,p[1]*base.rows}; }
        const auto& a=uv[0]; const auto& b=uv[1]; const auto& c=uv[2];
        const double area=(b[1]-c[1])*(a[0]-c[0])+(c[0]-b[0])*(a[1]-c[1]);
        size_t samples=0;
        for (int y=0;y<base.rows;++y) for (int x=0;x<base.cols;++x) {
            CHECK(base.at<cv::Vec4b>(y,x)[3]==baked.at<cv::Vec4b>(y,x)[3]);
            std::array<double,3> bary {((b[1]-c[1])*(x+.5-c[0])+(c[0]-b[0])*(y+.5-c[1]))/area,
                ((c[1]-a[1])*(x+.5-c[0])+(a[0]-c[0])*(y+.5-c[1]))/area,0};
            bary[2]=1-bary[0]-bary[1];
            if (*std::min_element(bary.begin(),bary.end())<.05) continue;
            ++samples;
            for (size_t ch=0;ch<3;++ch) {
                double factor=0;
                for (size_t corner=0;corner<3;++corner) factor+=bary[corner]*before_colors[before.indices[0][corner]][ch];
                const double old=linear(base.at<cv::Vec4b>(y,x)[2-ch]/255.)*factor*original.doc["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"][ch].get<double>();
                const double expected=255*srgb(old*(1-weight)+linear(options.face_target_colors[0][ch])*weight);
                CHECK(std::abs(baked.at<cv::Vec4b>(y,x)[2-ch]-expected)<=1.01);
            }
        }
        CHECK(samples>100); CHECK(model_artifact_sha256(source)==hash);
    }
}

TEST_CASE("Gradient puzzle repaint and release recover the original model exactly", "[BeautyAppearance][BeautyVertexColors]") {
    Fixture fixture; const auto source=make_fixture(fixture); auto original=read_glb(source);
    while(original.binary.size()%4) original.binary.push_back(0);
    const size_t offset=original.binary.size(),view=original.doc["bufferViews"].size(),accessor=original.doc["accessors"].size();
    for(size_t v=0;v<8;++v) for(float ch : {v%2 ? .8f:.2f,.4f,.6f,1.f}) append_float(original.binary,ch);
    original.doc["bufferViews"].push_back({{"buffer",0},{"byteOffset",offset},{"byteLength",128}});
    original.doc["accessors"].push_back({{"bufferView",view},{"componentType",5126},{"count",8},{"type","VEC4"}});
    original.doc["meshes"][0]["primitives"][0]["attributes"]["COLOR_0"]=accessor; write_glb(source,original.doc,original.binary);
    auto options=puzzle_options(source);
    auto puzzle=BeautyPuzzle::decode(options.beauty_document["puzzle"],options.beauty_surface->geometry_id,options.beauty_surface->face_patch.size());
    puzzle.paint(puzzle.face_piece[0],{.9f,.2f,.1f,1}); options.beauty_document["puzzle"]=puzzle.encode();
    const auto painted=fixture.directory/"painted.glb"; auto result=finish_model_artifact(source,painted,options); INFO(result.error); REQUIRE(result.success);
    options.beauty_surface.reset(); puzzle.paint(puzzle.face_piece[0],{.1f,.2f,.9f,1}); options.beauty_document["puzzle"]=puzzle.encode();
    const auto repainted=fixture.directory/"repainted.glb"; result=finish_model_artifact(painted,repainted,options); INFO(result.error); REQUIRE(result.success);
    puzzle.clear_color(puzzle.face_piece[0]); options.beauty_document["puzzle"]=puzzle.encode();
    const auto released=fixture.directory/"released.glb"; result=finish_model_artifact(repainted,released,options); INFO(result.error); REQUIRE(result.success);
    CHECK(model_artifact_sha256(released)==model_artifact_sha256(source));
}

TEST_CASE("Conflicting overlapping vertex gradients never publish partial appearance", "[BeautyAppearance][BeautyVertexColors]") {
    Fixture fixture; const auto source=make_fixture(fixture,true); auto original=read_glb(source);
    while(original.binary.size()%4) original.binary.push_back(0);
    const size_t offset=original.binary.size(),view=original.doc["bufferViews"].size(),accessor=original.doc["accessors"].size();
    for(size_t v=0;v<8;++v) for(float ch : {v<4 ? (v%2 ? .8f:.2f):.4f,.4f,.6f,1.f}) append_float(original.binary,ch);
    original.doc["bufferViews"].push_back({{"buffer",0},{"byteOffset",offset},{"byteLength",128}});
    original.doc["accessors"].push_back({{"bufferView",view},{"componentType",5126},{"count",8},{"type","VEC4"}});
    original.doc["meshes"][0]["primitives"][0]["attributes"]["COLOR_0"]=accessor; write_glb(source,original.doc,original.binary);
    const auto hash=model_artifact_sha256(source);
    BeautyAppearanceOptions options; options.face_weights={.5f,.5f,.5f,.5f}; options.face_target_colors.assign(4,{.9f,.2f,.1f});
    const auto destination=fixture.directory/"failed.glb"; const auto result=edit_glb_appearance(source,destination,options);
    CHECK_FALSE(result.success); CHECK(result.error.find("重叠UV")!=std::string::npos);
    CHECK_FALSE(boost::filesystem::exists(destination)); CHECK(result.changed_pixels==0); CHECK(result.changed_vertices==0);
    CHECK(model_artifact_sha256(source)==hash);
}

TEST_CASE("Mixed appearance failure and cancellation never publish a half painted version", "[BeautyAppearance][BeautyVertexColors]") {
    Fixture fixture; const auto source = make_fixture(fixture, false, false, true);
    auto doc = read_glb(source); doc.doc["materials"][0]["pbrMetallicRoughness"].erase("baseColorTexture");
    while (doc.binary.size() % 4) doc.binary.push_back(0);
    const size_t color_view = doc.doc["bufferViews"].size(), color_accessor = doc.doc["accessors"].size(), offset = doc.binary.size();
    // Degenerate gradient UVs cannot represent the native appearance. Never
    // publish only the already editable untextured part as a half-painted file.
    for (size_t i = 0; i < 8; ++i) for (float channel : {i == 6 ? .25f : .5f, .5f, .5f, 1.f}) append_float(doc.binary, channel);
    doc.doc["bufferViews"].push_back({{"buffer",0},{"byteOffset",offset},{"byteLength",128}});
    doc.doc["accessors"].push_back({{"bufferView",color_view},{"componentType",5126},{"count",8},{"type","VEC4"}});
    doc.doc["meshes"][0]["primitives"][1]["attributes"]["COLOR_0"] = color_accessor;
    const auto original_binary = doc.binary;
    const size_t uv_offset = doc.doc["bufferViews"][1]["byteOffset"].get<size_t>();
    for (size_t v : {size_t(5),size_t(6)}) std::memcpy(doc.binary.data()+uv_offset+v*8,doc.binary.data()+uv_offset+4*8,8);
    write_glb(source, doc.doc, doc.binary);
    const auto destination = fixture.directory / "failed.glb";
    BeautyAppearanceOptions options; options.face_weights = {1, 0, 1, 0}; options.face_target_colors.assign(4, {.9f, .2f, .1f});
    const auto failure = edit_glb_appearance(source, destination, options);
    CHECK_FALSE(failure.success); CHECK_FALSE(failure.error.empty()); CHECK_FALSE(boost::filesystem::exists(destination));
    CHECK(failure.error.find("渐变") != std::string::npos);
    CHECK(failure.changed_vertices == 0); CHECK(failure.changed_pixels == 0);
    doc.binary = original_binary;
    doc.doc["materials"][1]["pbrMetallicRoughness"]["baseColorFactor"] = {1, 1, 1, 1};
    doc.doc["meshes"][0]["primitives"][1]["attributes"].erase("COLOR_0");
    write_glb(source, doc.doc, doc.binary); const auto neutral_hash = model_artifact_sha256(source);
    const auto canceled = edit_glb_appearance(source, destination, options, [&] { return boost::filesystem::exists(destination); });
    CHECK(canceled.canceled); CHECK_FALSE(canceled.success); CHECK_FALSE(boost::filesystem::exists(destination));
    CHECK(model_artifact_sha256(source) == neutral_hash);
    CHECK(std::distance(boost::filesystem::directory_iterator(fixture.directory), boost::filesystem::directory_iterator()) == 1);
}

TEST_CASE("An unpaintable texture selection cannot silently save only its vertex colored part", "[BeautyAppearance][BeautyVertexColors]") {
    for (bool extra_editable_image : {false, true}) {
    Fixture fixture; const auto source = make_fixture(fixture, false, false, true);
    auto doc = read_glb(source);
    doc.doc["materials"][0]["pbrMetallicRoughness"].erase("baseColorTexture");
    doc.doc["materials"][1]["pbrMetallicRoughness"]["baseColorFactor"] = {1, 1, 1, 1};
    if (extra_editable_image) {
        auto extra_image = doc.doc["images"][0]; doc.doc["images"].push_back(extra_image);
        auto extra_texture = doc.doc["textures"][0]; extra_texture["source"] = 1; doc.doc["textures"].push_back(extra_texture);
        auto extra_material = doc.doc["materials"][1]; extra_material["pbrMetallicRoughness"]["baseColorTexture"]["index"] = 1;
        doc.doc["materials"].push_back(extra_material);
        auto extra_primitive = doc.doc["meshes"][0]["primitives"][0]; extra_primitive["material"] = 2;
        doc.doc["meshes"][0]["primitives"].push_back(extra_primitive);
    }
    // Textured faces coincide exactly: their texels belong to both the selected
    // and untouched face, so texture painting must not be silently skipped.
    const auto& indices = doc.doc["accessors"][doc.doc["meshes"][0]["primitives"][1]["indices"].get<size_t>()];
    const auto& view = doc.doc["bufferViews"][indices["bufferView"].get<size_t>()];
    const size_t offset = view.value("byteOffset", size_t(0)) + indices.value("byteOffset", size_t(0));
    std::copy_n(doc.binary.data() + offset, 12, doc.binary.data() + offset + 12);
    write_glb(source, doc.doc, doc.binary);
    const auto hash = model_artifact_sha256(source);
    const auto destination = fixture.directory / "partial.glb";
    BeautyAppearanceOptions options; options.face_weights = {1, 0, 1, 0}; options.face_target_colors.assign(4, {.9f, .2f, .1f});
    if (extra_editable_image) { options.face_weights.insert(options.face_weights.end(), {1, 0}); options.face_target_colors.resize(6, {.9f, .2f, .1f}); }
    const auto result = edit_glb_appearance(source, destination, options);
    INFO(extra_editable_image);
    CHECK_FALSE(result.success); CHECK_FALSE(result.error.empty()); CHECK_FALSE(boost::filesystem::exists(destination));
    CHECK(result.error.find("selected texture") != std::string::npos);
    CHECK(result.changed_vertices == 0); CHECK(result.changed_pixels == 0); CHECK(model_artifact_sha256(source) == hash);
    }
}

TEST_CASE("Primitive appearance mapping rejects reordered triangles materials and additional instances", "[BeautyAppearance][GlbGeometryEditing]") {
    Fixture fixture; const auto source = make_fixture(fixture, false, false, true);
    auto doc = read_glb(source);
    TexturedMesh imported; std::string error; REQUIRE(load_assimp_textured_model(source.string(), imported, &error));
    indexed_triangle_set geometry;
    for (const auto& v : imported.vertices) geometry.vertices.emplace_back(v[0] * 1000.f, -v[2] * 1000.f, v[1] * 1000.f);
    for (const auto& f : imported.indices) geometry.indices.emplace_back(f[0], f[1], f[2]);
    const std::vector<size_t> expected_counts {2, 2};
    CHECK(verify_glb_appearance_layout(doc.doc, doc.binary, geometry, imported.material_ids) == expected_counts);
    auto reordered = geometry; std::swap(reordered.indices[0], reordered.indices[1]);
    CHECK_THROWS(verify_glb_appearance_layout(doc.doc, doc.binary, reordered, imported.material_ids));
    auto wrong_materials = imported.material_ids; wrong_materials[0] = 1;
    CHECK_THROWS(verify_glb_appearance_layout(doc.doc, doc.binary, geometry, wrong_materials));
    auto moved = geometry; moved.vertices[0][0] += 10;
    CHECK_THROWS(verify_glb_appearance_layout(doc.doc, doc.binary, moved, imported.material_ids));
    CHECK_THROWS(read_glb_geometry_source(source, geometry)); // Geometry-writing scope remains unchanged.
    doc.doc["nodes"].push_back({{"mesh", 0}}); doc.doc["scenes"][0]["nodes"].push_back(1);
    CHECK_THROWS(verify_glb_appearance_layout(doc.doc, doc.binary, geometry, imported.material_ids));
}

TEST_CASE("Shared vertex painting cancels and rejects a changed source without a partial asset", "[BeautyAppearance][BeautyPuzzle][BeautyVertexColors]") {
    for (const int stop : {1,12,-2,-1}) {
        Fixture fixture;
        const auto source = fixture.directory/"source.glb", destination = fixture.directory/"canceled.glb";
        boost::filesystem::copy_file(boost::filesystem::path(std::string(TEST_DATA_DIR))/"model_artifact"/"ushort-vertex-colors.glb",source);
        BeautyAppearanceOptions options; options.face_weights = {1,0,0,0}; options.face_target_colors.assign(4,{.9f,.2f,.1f});
        const auto hash = model_artifact_sha256(source); int calls = 0;
        const auto result = edit_glb_appearance(source,destination,options,[&] {
            if (stop == -1) return boost::filesystem::exists(destination);
            if (stop == -2) return std::any_of(boost::filesystem::directory_iterator(fixture.directory), boost::filesystem::directory_iterator(),
                [](const auto& entry) { return boost::filesystem::is_directory(entry.path()); });
            return ++calls >= stop;
        });
        INFO(stop); INFO(result.error); REQUIRE(result.canceled); CHECK_FALSE(result.success); CHECK_FALSE(boost::filesystem::exists(destination));
        CHECK(model_artifact_sha256(source) == hash);
        CHECK(std::distance(boost::filesystem::directory_iterator(fixture.directory),boost::filesystem::directory_iterator()) == 1);
    }
    Fixture fixture;
    const auto source = fixture.directory/"source.glb", destination = fixture.directory/"changed.glb";
    boost::filesystem::copy_file(boost::filesystem::path(std::string(TEST_DATA_DIR))/"model_artifact"/"vertex-material-color.glb",source);
    BeautyAppearanceOptions options; options.face_weights = {1,0,0,0}; options.face_target_colors.assign(4,{.9f,.2f,.1f});
    bool changed = false;
    const auto result = edit_glb_appearance(source,destination,options,[&] {
        if (!changed && boost::filesystem::exists(destination)) {
            auto doc = read_glb(source); doc.doc["asset"]["generator"] = "changed"; write_glb(source,doc.doc,doc.binary); changed = true;
        }
        return false;
    });
    CHECK_FALSE(result.success); CHECK_FALSE(boost::filesystem::exists(destination)); CHECK(changed);
}

TEST_CASE("Exact preview retains interior texture paint and alpha without resampling it into corner colors", "[ModelArtifact][BeautyAppearance]") {
    Fixture f; const auto source=make_fixture(f); const auto destination=f.directory/"preview.glb";
    auto glb=read_glb(source); glb.doc["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"]={.25,.6,.5,.7};
    write_glb(source,glb.doc,glb.binary); const auto source_hash=model_artifact_sha256(source);
    BeautyAppearanceOptions options; options.face_weights={1,0,0,0}; options.face_target_colors.assign(4,{.1f,.3f,.9f});
    REQUIRE(edit_glb_appearance(source,destination,options).success);
    TriangleMesh mesh; ObjInfo colors; std::string error; ModelArtifactTextureSurface exact;
    REQUIRE(load_model_artifact(destination,mesh,colors,error,{},&exact));
    REQUIRE(exact.faces.size()==mesh.its.indices.size()); REQUIRE(exact.faces.size()==4);
    const int image=exact.faces[0].image; REQUIRE(image>=0); REQUIRE(size_t(image)<exact.images.size());
    const auto& pixels=exact.images[image]; REQUIRE(pixels.width==64); REQUIRE(pixels.height==32);
    const auto saved=read_glb(destination); const size_t material=saved.doc["meshes"][0]["primitives"][0]["material"].get<size_t>();
    const auto expected=color_image(saved,material); size_t painted=0;
    for(int y=0;y<pixels.height;++y)for(int x=0;x<pixels.width;++x) {
        const auto pixel=expected.at<cv::Vec4b>(y,x); const auto* actual=pixels.rgba.data()+(size_t(y)*pixels.width+x)*4;
        CHECK(actual[0]==pixel[2]); CHECK(actual[1]==pixel[1]); CHECK(actual[2]==pixel[0]); CHECK(actual[3]==pixel[3]);
        if(actual[2]>actual[0]+40)++painted;
    }
    CHECK(painted>100); // Interior edits survive even when the protected shared corners keep their old color.
    TexturedMesh loaded; std::vector<std::array<float,4>> vertex;
    REQUIRE(load_assimp_textured_model(destination.string(),loaded,&error,&vertex));
    for(size_t face=0;face<exact.faces.size();++face)for(size_t corner=0;corner<3;++corner) {
        const auto vi=loaded.indices[face][corner]; const auto mi=loaded.material_ids[face];
        CHECK(exact.faces[face].corners[corner].uv==loaded.uvs[vi]);
        for(size_t ch=0;ch<4;++ch)CHECK(std::abs(exact.faces[face].corners[corner].multiplier[ch]-vertex[vi][ch]*loaded.material_colors[mi][ch])<1e-6);
    }
    CHECK(model_artifact_sha256(source)==source_hash);
}

TEST_CASE("Exact preview retains mixed primitive textures transforms and distinct linear material multipliers", "[ModelArtifact][BeautyAppearance]") {
    Fixture f; const auto source=make_fixture(f,false,false,true); auto glb=read_glb(source);
    glb.doc["materials"][1]["pbrMetallicRoughness"]["baseColorTexture"]["extensions"]["KHR_texture_transform"]={{"scale",{.5,.75}},{"offset",{.1,.2}}};
    write_glb(source,glb.doc,glb.binary);
    TriangleMesh ordinary,exact_mesh; ObjInfo colors,exact_colors; std::string error; ModelArtifactTextureSurface exact;
    REQUIRE(load_model_artifact(source,ordinary,colors,error)); REQUIRE(load_model_artifact(source,exact_mesh,exact_colors,error,{},&exact));
    CHECK(ordinary.its.indices==exact_mesh.its.indices); CHECK(ordinary.its.vertices==exact_mesh.its.vertices);
    REQUIRE(exact.faces.size()==4); CHECK(exact.faces[0].corners[0].multiplier[0]!=exact.faces[2].corners[0].multiplier[0]);
    TexturedMesh loaded; REQUIRE(load_assimp_textured_model(source.string(),loaded,&error));
    for(size_t face=0;face<4;++face)for(size_t corner=0;corner<3;++corner) {
        const auto uv=loaded.uvs[loaded.indices[face][corner]]; const bool second=face>=2;
        CHECK(std::abs(exact.faces[face].corners[corner].uv[0]-(second?uv[0]*.5f+.1f:uv[0]))<1e-6);
        CHECK(std::abs(exact.faces[face].corners[corner].uv[1]-(second?uv[1]*.75f+.2f:uv[1]))<1e-6);
    }
}

// Opt-in regression uses the actual failed drafts without shipping private
// source models or rewriting their content-addressed cache.
TEST_CASE("Saved portrait drafts rebuild and bake all palette variants without recognition", "[.PortraitSaveReplay]") {
    const char* input=boost::nowide::getenv("ORCA_PORTRAIT_SAVE_REPLAY");
    REQUIRE(input);
    const boost::filesystem::path base(input);
    const char* output=boost::nowide::getenv("ORCA_PORTRAIT_SAVE_OUTPUT");
    REQUIRE(output);
    const boost::filesystem::path destination(output), cache=base/"gui-data/cache";
    REQUIRE_FALSE(boost::filesystem::exists(destination));
    boost::filesystem::create_directories(destination);
    const auto read=[](const boost::filesystem::path& path) {
        boost::filesystem::ifstream stream(path,std::ios::binary);
        if(!stream) throw std::runtime_error("Missing replay input: "+path.string());
        return Json::parse(stream);
    };
    const auto read_ref=[&](const Json& ref) {
        const auto path=cache/ref.at("path").get<std::string>();
        REQUIRE(model_artifact_sha256(path)==ref.at("sha256"));
        return read(path);
    };
    const auto source=base/"gui-data/generated_models/downloads/orcaslicer-ai-8f6f1850-c076-4264-b26b-ced09f0397ff.glb";
    const auto source_hash=model_artifact_sha256(source);
    GUI::PortraitShapeCache::preserve_source(destination/"cache",source,source_hash);
    Json report=Json::array();
    for(const auto* name:{"three","four","five","six"}) {
        const auto started=std::chrono::steady_clock::now();
        const auto draft=read_ref(read(base/(std::string(name)+"-draft-reference.json")));
        REQUIRE(draft.at("source_sha256")==source_hash);
        const auto doc=read_ref(draft.at("shapes"));
        auto shapes=GUI::PortraitShapeCache::load(draft.at("shapes"),cache,draft.at("geometry"),draft.at("face_count"),doc.at("runtime_fingerprint"),true);
        const auto old=*shapes->surface_partition;
        auto rebuilt=SurfacePartition::rebuild_tessellation(old);
        REQUIRE(SurfacePartition::rebuild_tessellation(rebuilt)==rebuilt);
        REQUIRE(rebuilt.at("faces").size()==old.at("faces").size());
        auto next=std::make_shared<GUI::PortraitShapeDetails>(*shapes);
        next->surface_partition=std::make_shared<const Json>(std::move(rebuilt));
        auto locks=shapes->contour_locks->document;
        const auto hash=beauty_leaf_digest(next->surface_partition->dump());
        locks["partition_ref"]={{"schema","orca.surface-partition-reference/v1"},{"path","surface-partitions/"+hash+".json"},{"sha256",hash}};
        next->contour_locks=std::make_shared<const BeautySurfaceShapeLock>(BeautySurfaceShapeLock::decode(locks,*next->surface_partition,BeautySurfaceShapeLock::identity(*next->surface_partition),hash));
        const auto edits=remap_cell_edits(draft.at("leaf_edits"),old,*next->surface_partition,shapes->boundary_fingerprint(),next->boundary_fingerprint());
        if(draft.at("leaf_edits").is_null()) REQUIRE(edits.is_null());
        else for(const auto* key:{"colors","selected","protected","foreground","domain"}) REQUIRE(edits.at(key)==draft.at("leaf_edits").at(key));
        const auto saved_ref=GUI::PortraitShapeCache::save(*next,destination/"cache");
        const auto reopened=GUI::PortraitShapeCache::load(saved_ref,destination/"cache",draft.at("geometry"),draft.at("face_count"),doc.at("runtime_fingerprint"),true);
        REQUIRE(reopened->cell_colors==shapes->cell_colors);
        REQUIRE(reopened->contour_locks->document.at("locks")==shapes->contour_locks->document.at("locks"));
        BeautyAppearanceOptions options;
        options.face_weights.assign(draft.at("face_count"),0.f);
        options.face_target_colors.resize(options.face_weights.size());
        for(const auto& row:draft.at("semantic").at("faces")) {
            const auto face=row[0].get<size_t>();options.face_weights.at(face)=1.f;
            options.face_target_colors.at(face)=row[1].get<std::array<float,3>>();
        }
        SemanticColoring::SubfaceColors subfaces;
        for(const auto& row:draft.at("semantic").at("subfaces"))
            subfaces.push_back({row[0].get<size_t>(),{row[1].get<uint8_t>(),row[2].get<uint8_t>()},row[3].get<std::array<float,3>>(),1.f});
        appearance_subface_colors(options,subfaces,draft.at("geometry"));
        appearance_cell_colors(options,*next->surface_partition,next->cell_colors,draft.at("geometry"));
        const auto path=destination/(std::string(name)+".glb");
        const auto result=edit_glb_appearance(source,path,options);
        INFO(name); INFO(result.error); REQUIRE(result.success);
        TriangleMesh mesh;ObjInfo info;std::string error;
        REQUIRE(load_model_artifact(path,mesh,info,error));
        REQUIRE(mesh.its.indices.size()==draft.at("face_count"));
        REQUIRE(SurfaceSelectionPersistence::geometry_fingerprint(mesh.its)==draft.at("geometry"));
        REQUIRE(model_artifact_sha256(source)==source_hash);
        report.push_back({{"palette",name},{"output_sha256",model_artifact_sha256(path)},
            {"shape_reference",saved_ref},{"changed_texture_pixels",result.changed_pixels},
            {"seconds",std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count()},
            {"recognition_started",false},{"reopened",true}});
        boost::filesystem::ofstream(destination/"results.json")<<report.dump(2);
    }
}
