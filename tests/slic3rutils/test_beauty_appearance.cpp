#include "slic3r/GUI/AI/Model/BeautyAppearance.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "slic3r/GUI/AI/Model/BeautyPuzzle.hpp"
#include "slic3r/GUI/AI/Model/ModelFinishing.hpp"
#include "libslic3r/Format/AssimpImport.hpp"
#include "libslic3r/TexturePainting.hpp"
#include <catch2/catch_test_macros.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>

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
boost::filesystem::path make_fixture(const Fixture& fixture, bool overlapping = false, bool noise = false, bool multi_material = false) {
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
            {{"buffer",0},{"byteOffset",index_offset},{"byteLength",48}},
            {{"buffer",0},{"byteOffset",image_offset},{"byteLength",png.size()}}})},
        {"accessors",Json::array({{{"bufferView",0},{"componentType",5126},{"count",8},{"type","VEC3"},{"min",{0,0,0}},{"max",{3,1,0}}},
            {{"bufferView",1},{"componentType",5126},{"count",8},{"type","VEC2"}},
            {{"bufferView",2},{"componentType",5125},{"count",12},{"type","SCALAR"}}})},
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
boost::filesystem::path make_neutral_fixture(const Fixture& fixture, bool overlapping = false) {
    const auto path = make_fixture(fixture, overlapping);
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

TEST_CASE("Invalid puzzle target colors and material multipliers fail explicitly", "[BeautyWorkbench][BeautyAppearance][BeautyPuzzle]") {
    Fixture fixture;
    const auto nonneutral = make_fixture(fixture), destination = fixture.directory / "invalid-puzzle.glb";
    auto options = puzzle_colors();
    const auto factor_result = edit_glb_appearance(nonneutral, destination, options);
    CHECK_FALSE(factor_result.success);
    CHECK(factor_result.error.find("multipliers") != std::string::npos);
    CHECK_FALSE(boost::filesystem::exists(destination));
    const auto source = make_neutral_fixture(fixture);
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
