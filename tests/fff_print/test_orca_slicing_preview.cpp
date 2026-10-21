#include "orca_slicing/project_support.hpp"
#include "orca_slicing/engine.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "test_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <fstream>
#include <cstdint>
#include <cstring>

namespace {
uint64_t integer(std::ifstream& input,size_t bytes)
{
    uint64_t value=0;
    for(size_t i=0;i<bytes;++i) value|=uint64_t(static_cast<unsigned char>(input.get()))<<(8*i);
    return value;
}
float real(std::ifstream& input)
{
    const uint32_t bits=static_cast<uint32_t>(integer(input,4));
    float value;std::memcpy(&value,&bits,4);return value;
}
}
TEST_CASE("Native preview stores only real print layers with exact binary offsets", "[OrcaSlicingPreview]")
{
    Slic3r::GCodeProcessorResult result;
    result.moves.emplace_back();
    auto append=[&](unsigned layer,float z,float x) {
        Slic3r::GCodeProcessorResult::MoveVertex m;
        m.type=Slic3r::EMoveType::Extrude;m.layer_id=layer;m.print_z=z;
        m.position=Slic3r::Vec3f(x,0,z);m.width=.4f;m.delta_extruder=.5f;
        m.extrusion_role=Slic3r::erPerimeter;m.extruder_id=2;
        result.moves.push_back(m);
    };
    append(3,.2f,10);append(8,.4f,20);
    ScopedTemporaryFile file(".orcapv");
    const auto info=orca::slicing::write_preview(result,file.string());
    REQUIRE(info["layerCount"]==2);
    REQUIRE(info["segmentCount"]==2);
    CHECK(info["previewSize"]==32+2*16+2*32);
    CHECK(info["previewSha256"]==orca::slicing::file_sha256(file.string()));
    std::ifstream input(file.string(),std::ios::binary);
    char magic[8];input.read(magic,8);
    CHECK(std::string(magic,8)=="ORCAPV01");
    CHECK(integer(input,4)==2);CHECK(integer(input,4)==2);
    CHECK(integer(input,8)==32);CHECK(integer(input,8)==64);
    CHECK_THAT(real(input),Catch::Matchers::WithinAbs(.2,1e-6));
    CHECK(integer(input,4)==0);CHECK(integer(input,4)==1);CHECK(integer(input,4)==0);
    CHECK_THAT(real(input),Catch::Matchers::WithinAbs(.4,1e-6));
    CHECK(integer(input,4)==1);CHECK(integer(input,4)==1);CHECK(integer(input,4)==0);
    input.seekg(64+28);CHECK(integer(input,2)==2);CHECK(integer(input,1)==Slic3r::erPerimeter);CHECK(integer(input,1)==1);
}
TEST_CASE("Native preview rejects files with no print extrusion", "[OrcaSlicingPreview]")
{
    Slic3r::GCodeProcessorResult result;
    result.moves.emplace_back();
    ScopedTemporaryFile file(".orcapv");
    CHECK_THROWS_WITH(orca::slicing::write_preview(result,file.string()),"PREVIEW_NO_PRINT_LAYERS");
}
TEST_CASE("Native preview follows processor extrusion modes resets and circular moves", "[OrcaSlicingPreview]")
{
    const bool previous=Slic3r::GCodeProcessor::s_IsBBLPrinter;
    struct Restore {bool value;~Restore(){Slic3r::GCodeProcessor::s_IsBBLPrinter=value;}} restore{previous};
    Slic3r::GCodeProcessor::s_IsBBLPrinter=true;
    Slic3r::FullPrintConfig config;
    config.gcode_flavor.value=Slic3r::gcfMarlinFirmware;
    config.filament_map.values={1};
    ScopedTemporaryFile source(".gcode"), preview(".orcapv");
    {
        std::ofstream file(source.string());
        file<<"G90\nM82\nG92 E0\nG1 X10 Y10 Z0.2 F1200\n"
            "; CHANGE_LAYER\n; Z_HEIGHT: 0.2\n; LAYER_HEIGHT: 0.2\n; FEATURE: Outer wall\n"
            "G1 X20 Y10 E1 F1200\nG92 E0\nM83\nG1 X20 Y20 E1\n"
            "G2 X10 Y20 I-5 J0 E1\n; CHANGE_LAYER\n; Z_HEIGHT: 0.4\n"
            "G1 Z0.4\nG1 X10 Y10 E1\n";
    }
    Slic3r::GCodeProcessor processor;
    processor.apply_config(config);processor.initialize(source.string());
    processor.initialize_result_moves();
    std::ifstream input(source.string(),std::ios::binary);
    processor.process_buffer(std::string(std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()));
    processor.finalize(false);
    const auto info=orca::slicing::write_preview(processor.get_result(),preview.string());
    CHECK(info["layerCount"]==2);
    // The circular command must retain its processor-discretized intermediate
    // vertices rather than collapse to a straight chord or vanish under M83.
    CHECK(info["segmentCount"].get<size_t>()>5);
    CHECK_THAT(info["layerHeights"][0].get<float>(),Catch::Matchers::WithinAbs(.2,1e-5));
    CHECK_THAT(info["layerHeights"][1].get<float>(),Catch::Matchers::WithinAbs(.4,1e-5));
}

TEST_CASE("Geometry-only STL imports a visible default profile accepted by real Print validation", "[OrcaSlicingPreview]")
{
    ScopedSlic3rTemporaryDir directory;
    ScopedTemporaryFile file(".stl");
    auto mesh=Slic3r::make_cube(10,10,10);
    REQUIRE(Slic3r::store_stl(file.string().c_str(),&mesh,true));
    auto project=orca::slicing::load_project(file.string(),nlohmann::json::object(),nlohmann::json::array(),directory.string());
    CHECK(project.metadata["basePreset"]["version"]==1);
    CHECK(project.config.opt_string("before_layer_change_gcode").find("G92 E0")!=std::string::npos);
    Slic3r::Print print;
    print.set_status_silent();print.apply(project.model,project.config);
    CHECK(print.validate().string.empty());
    auto restored=orca::slicing::load_project(file.string(),orca::slicing::config_json(project.config),nlohmann::json::array(),directory.string());
    Slic3r::Print restoredPrint;restoredPrint.set_status_silent();
    restoredPrint.apply(restored.model,restored.config);
    CHECK(restoredPrint.validate().string.empty());
    CHECK_NOTHROW(restoredPrint.process());
    const auto* sourceVariants=dynamic_cast<const Slic3r::ConfigOptionInts*>(project.config.option("nozzle_volume_type"));
    const auto* restoredVariants=dynamic_cast<const Slic3r::ConfigOptionInts*>(restored.config.option("nozzle_volume_type"));
    REQUIRE(sourceVariants); REQUIRE(restoredVariants);
    CHECK(restoredVariants->values==sourceVariants->values);
    auto changedConfig=orca::slicing::config_json(project.config);
    changedConfig["wall_loops"]=4;
    auto changed=orca::slicing::load_project(file.string(),changedConfig,nlohmann::json::array(),directory.string());
    Slic3r::Print changedPrint;changedPrint.set_status_silent();changedPrint.apply(changed.model,changed.config);
    CHECK_NOTHROW(changedPrint.process());
    CHECK_THROWS_AS(orca::slicing::load_project(file.string(),{{"nozzle_volume_type",nlohmann::json::array()}},nlohmann::json::array(),directory.string()),std::invalid_argument);
    const auto* definition=project.config.def()->get("nozzle_volume_type");
    REQUIRE(definition); REQUIRE(definition->enum_keys_map);
    for(const auto& entry:*definition->enum_keys_map) if(!sourceVariants->values.empty() && entry.second!=sourceVariants->values.front()) {
        auto variantConfig=orca::slicing::config_json(project.config);variantConfig["nozzle_volume_type"]=entry.first;
        auto variant=orca::slicing::load_project(file.string(),variantConfig,nlohmann::json::array(),directory.string());
        const auto* values=dynamic_cast<const Slic3r::ConfigOptionInts*>(variant.config.option("nozzle_volume_type"));
        REQUIRE(values); REQUIRE(!values->values.empty()); CHECK(values->values.front()==entry.second); break;
    }
    // An explicit user override remains authoritative, even if invalid.
    auto user=orca::slicing::load_project(file.string(),{{"before_layer_change_gcode",""}},nlohmann::json::array(),directory.string());
    CHECK(user.config.opt_string("before_layer_change_gcode").empty());
}
TEST_CASE("Engine result identity reports actual normalized vase configuration", "[OrcaSlicingPreview]")
{
    ScopedSlic3rTemporaryDir directory;
    ScopedTemporaryFile file(".stl");
    auto mesh=Slic3r::make_cube(10,10,10);
    REQUIRE(Slic3r::store_stl(file.string().c_str(),&mesh,true));
    orca::slicing::Engine engine(directory.string());
    const nlohmann::json request={{"operation","slice"},{"projectId","vase-test"},{"projectRevision",1},
        {"jobId","vase-job"},{"attemptId","vase-attempt"},{"inputPath",file.string()},
        {"executeEffectiveConfig",true},{"quality","standard"},
        {"config",{{"spiral_mode",true},{"wall_loops",3},{"top_shell_layers",4},{"sparse_infill_density","15%"}}}};
    const auto result=engine.execute(request,{});
    CHECK(result["requestedConfig"]["wall_loops"]=="3");
    CHECK(result["effectiveConfig"]["wall_loops"]=="1");
    CHECK(result["effectiveConfig"]["top_shell_layers"]=="0");
    CHECK(result["configSha256"]==orca::slicing::json_sha256(result["effectiveConfig"]));
    CHECK(result["requestedConfigSha256"]!=result["configSha256"]);
    CHECK(result["gcodeSha256"]==orca::slicing::file_sha256(result["gcodePath"]));
}
