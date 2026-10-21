#include "orca_slicing/recommendation_support.hpp"
#include "orca_slicing/engine.hpp"
#include "libslic3r/PrintBase.hpp"
#include <miniz.h>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <limits>
#include <fstream>
#include <array>
#include <algorithm>
#include <cstdint>
#include "test_utils.hpp"

TEST_CASE("Recommendation planner emits baseline and three native quality plans", "[OrcaSlicingSupport]")
{
    orca::slicing::LoadedProject project{
        Slic3r::Model{},
        Slic3r::DynamicPrintConfig::full_print_config(),
        "fixture.3mf",
        "",
        nlohmann::json::object()};

    const auto candidates = orca::slicing::recommendation_candidates(project);
    REQUIRE(candidates.size() == 4);
    CHECK(candidates[0].candidateId == "baseline");
    CHECK(candidates[1].candidateId == "balanced");
    CHECK(candidates[2].candidateId == "speed");
    CHECK(candidates[3].candidateId == "quality");
    CHECK(std::abs(candidates[2].config.option<Slic3r::ConfigOptionFloat>("layer_height")->value - 0.28) < 1e-9);
    CHECK(std::abs(candidates[3].config.option<Slic3r::ConfigOptionFloat>("layer_height")->value - 0.12) < 1e-9);
}

TEST_CASE("Recommendation selection consumes only validated native trial metrics", "[OrcaSlicingSupport]")
{
    nlohmann::json trials = nlohmann::json::array({
        {{"candidateId", "baseline"}, {"quality", "standard"}, {"validated", true},
         {"metrics", {{"printTimeSeconds", 100.0}, {"filamentGrams", 20.0}}}},
        {{"candidateId", "speed"}, {"quality", "fast"}, {"validated", true},
         {"metrics", {{"printTimeSeconds", 70.0}, {"filamentGrams", 24.0}}}},
        {{"candidateId", "quality"}, {"quality", "fine"}, {"validated", true},
         {"metrics", {{"printTimeSeconds", 130.0}, {"filamentGrams", 18.0}}}},
        {{"candidateId", "proxy"}, {"quality", "fast"}, {"validated", false},
         {"metrics", {{"printTimeSeconds", 1.0}, {"filamentGrams", 1.0}}}}});

    for (auto& trial:trials) {
        trial["gcodePath"]="trial.gcode"; trial["gcodeSha256"]=std::string(64,'a');
        trial["previewPath"]="trial.orcapv"; trial["previewSha256"]=std::string(64,'b');
    }
    CHECK(orca::slicing::select_recommendation(trials, "speed")["selectedCandidateId"] == "speed");
    CHECK(orca::slicing::select_recommendation(trials, "quality")["selectedCandidateId"] == "quality");
    CHECK(orca::slicing::select_recommendation(trials, "balanced")["selectedCandidateId"] == "speed");
}


namespace {
void check_prepared_geometry(const orca::slicing::LoadedProject& actual,const orca::slicing::LoadedProject& expected)
{
    REQUIRE(actual.model.objects.size()==expected.model.objects.size());
    CHECK(actual.inputSha256==expected.inputSha256);
    CHECK(orca::slicing::config_json(actual.config)==orca::slicing::config_json(expected.config));
    for(const char* key:{"projectId","revision","projectCommands"})
        CHECK(actual.metadata.value(key,nlohmann::json())==expected.metadata.value(key,nlohmann::json()));
    for(size_t oi=0;oi<actual.model.objects.size();++oi) {
        const auto* a=actual.model.objects[oi];const auto* e=expected.model.objects[oi];
        CHECK(a->name==e->name);
        REQUIRE(a->instances.size()==e->instances.size());
        REQUIRE(a->volumes.size()==e->volumes.size());
        for(size_t ii=0;ii<a->instances.size();++ii) {
            // Euler angles have nonunique representations at gimbal lock. The
            // complete affine matrix and physical bounds define the transform.
            const auto am=a->instances[ii]->get_matrix();const auto em=e->instances[ii]->get_matrix();
            for(int row=0;row<4;++row) for(int col=0;col<4;++col)
                CHECK_THAT(am.matrix()(row,col),Catch::Matchers::WithinAbs(em.matrix()(row,col),1e-9));
            const auto ab=a->instance_bounding_box(ii), eb=e->instance_bounding_box(ii);
            for(int axis=0;axis<3;++axis) {
                CHECK_THAT(ab.min[axis],Catch::Matchers::WithinAbs(eb.min[axis],1e-9));
                CHECK_THAT(ab.max[axis],Catch::Matchers::WithinAbs(eb.max[axis],1e-9));
            }
        }
    }
}
orca::slicing::LoadedProject preparation_project()
{
    orca::slicing::LoadedProject project{Slic3r::Model{},Slic3r::DynamicPrintConfig::full_print_config(),"fixture.stl","",nlohmann::json::object()};
    auto* object=project.model.add_object(); object->name="box";
    object->add_volume(Slic3r::make_cube(10,20,30),Slic3r::ModelVolumeType::MODEL_PART,false);
    object->add_instance();
    project.config.set_key_value("printable_area",new Slic3r::ConfigOptionPoints({{0,0},{100,0},{100,100},{0,100}}));
    return project;
}
}
TEST_CASE("Project commands replay geometry without changing the source mesh", "[OrcaSlicingSupport]")
{
    auto project=preparation_project();
    const auto original=project.model.objects[0]->volumes[0]->mesh().its.vertices;
    const nlohmann::json commands=nlohmann::json::array({
        {{"type","transform"},{"objectIndex",0},{"scale",{2,2,2}}},
        {{"type","duplicate"},{"objectIndex",0}},
        {{"type","arrange"},{"gapMm",2.0}}});
    orca::slicing::apply_project_commands(project,commands);
    REQUIRE(project.model.objects[0]->instances.size()==2);
    CHECK(project.model.objects[0]->volumes[0]->mesh().its.vertices==original);
    CHECK_THAT(project.model.objects[0]->instance_bounding_box(0).size().x(),Catch::Matchers::WithinAbs(20,1e-6));
    auto replay=preparation_project();
    orca::slicing::apply_project_commands(replay,commands);
    CHECK(orca::slicing::describe_project(project)["objects"]==orca::slicing::describe_project(replay)["objects"]);
}
TEST_CASE("Cardinal orientation places a real model face on the bed", "[OrcaSlicingSupport]")
{
    auto project=preparation_project();
    orca::slicing::apply_project_commands(project,nlohmann::json::array({{{"type","orient"}}}));
    auto bounds=project.model.objects[0]->instance_bounding_box(0);
    CHECK_THAT(bounds.min.z(),Catch::Matchers::WithinAbs(0,1e-6));
    CHECK_THAT(bounds.size().z(),Catch::Matchers::WithinAbs(10,1e-6));
}
TEST_CASE("World dimensions apply along the axes of a cardinal rotation", "[OrcaSlicingSupport]")
{
    auto project=preparation_project();
    orca::slicing::apply_project_commands(project,nlohmann::json::array({
        {{"type","transform"},{"rotation",{0,0,1.5707963267948966}},{"sizeMm",{40,30,60}}}}));
    auto size=project.model.objects[0]->instance_bounding_box(0).size();
    CHECK_THAT(size.x(),Catch::Matchers::WithinAbs(40,1e-5));
    CHECK_THAT(size.y(),Catch::Matchers::WithinAbs(30,1e-5));
    CHECK_THAT(size.z(),Catch::Matchers::WithinAbs(60,1e-5));
}
TEST_CASE("Arrangement reports overflow without partially moving instances", "[OrcaSlicingSupport]")
{
    auto project=preparation_project();
    const auto before=project.model.objects[0]->instances[0]->get_offset();
    CHECK_THROWS_WITH(orca::slicing::apply_project_commands(project,nlohmann::json::array({{{"type","arrange"},{"gapMm",60.0}}})),"ARRANGE_BED_OVERFLOW");
    CHECK(project.model.objects[0]->instances[0]->get_offset()==before);
}
TEST_CASE("Invalid transform components are rejected", "[OrcaSlicingSupport]")
{
    auto project=preparation_project();
    CHECK_THROWS(orca::slicing::apply_project_commands(project,nlohmann::json::array({{{"type","transform"},{"scale",{-1,1,1}}}})));
    CHECK_THROWS(orca::slicing::apply_project_commands(project,nlohmann::json::array({{{"type","transform"},{"translation",{1,2}}}})));
}
TEST_CASE("Explicit material assignment is retained in the durable command snapshot", "[OrcaSlicingSupport]")
{
    auto project=preparation_project();
    orca::slicing::apply_project_commands(project,nlohmann::json::array({{{"type","assignMaterial"},{"volumeIndex",0},{"filamentIndex",0}}}));
    const auto description=orca::slicing::describe_project(project);
    CHECK(description["materialAssignments"][0]["filamentIndex"]==0);
    CHECK(description["objects"][0]["parts"][0]["material"]==1);
    CHECK(orca::slicing::slicing_model(project).objects[0]->volumes[0]->extruder_id()==1);
}
TEST_CASE("Native quality presets use nozzle size and reject smart fallback", "[OrcaSlicingSupport]")
{
    auto config=Slic3r::DynamicPrintConfig::full_print_config();
    config.set_key_value("nozzle_diameter",new Slic3r::ConfigOptionFloats({0.6}));
    config.set("enable_prime_tower",true);
    auto fine=orca::slicing::quality_config(config,"fine");
    CHECK_THAT(fine.opt_float("layer_height"),Catch::Matchers::WithinAbs(0.18,1e-9));
    CHECK(fine.opt_bool("enable_prime_tower"));
    CHECK_THROWS(orca::slicing::quality_config(config,"smart"));
}
TEST_CASE("Serialized effective config replays with an identical hash", "[OrcaSlicingSupport]")
{
    auto config=orca::slicing::quality_config(Slic3r::DynamicPrintConfig::full_print_config(),"fine");
    auto replay=Slic3r::DynamicPrintConfig::full_print_config();
    orca::slicing::apply_config_overrides(replay,orca::slicing::config_json(config));
    CHECK(orca::slicing::json_sha256(orca::slicing::config_json(config))==orca::slicing::json_sha256(orca::slicing::config_json(replay)));
}
TEST_CASE("Recommendation selection rejects missing artifacts and empty physical metrics", "[OrcaSlicingSupport]")
{
    nlohmann::json trial={{"candidateId","broken"},{"validated",true},{"metrics",{{"printTimeSeconds",10},{"filamentGrams",2}}}};
    CHECK_THROWS(orca::slicing::select_recommendation(nlohmann::json::array({trial})));
    trial["gcodePath"]="a"; trial["gcodeSha256"]="b"; trial["previewPath"]="c"; trial["previewSha256"]="d";
    trial["metrics"]["filamentGrams"]=0;
    CHECK_THROWS(orca::slicing::select_recommendation(nlohmann::json::array({trial})));
}

TEST_CASE("Single material conversion requires an explicit command and preserves source color annotations", "[OrcaSlicingSupport]")
{
    auto project=preparation_project();
    project.config.set("enable_prime_tower",true);
    const auto vertices=project.model.objects[0]->volumes[0]->mesh().its.vertices;
    project.model.objects[0]->volumes[0]->mmu_segmentation_facets.set_triangle_from_string(0,"8");
    REQUIRE_FALSE(project.model.objects[0]->volumes[0]->mmu_segmentation_facets.empty());
    orca::slicing::apply_project_commands(project,nlohmann::json::array({{{"type","singleMaterial"},{"filamentIndex",0}}}));
    CHECK_FALSE(project.config.opt_bool("enable_prime_tower"));
    REQUIRE(project.metadata["materialAssignments"].size()==1);
    CHECK(project.metadata["outputMode"]=="singleMaterial");
    CHECK(project.model.objects[0]->volumes[0]->mesh().its.vertices==vertices);
    CHECK_FALSE(project.model.objects[0]->volumes[0]->mmu_segmentation_facets.empty());
    const auto copy=orca::slicing::slicing_model(project);
    CHECK(copy.objects[0]->volumes[0]->mmu_segmentation_facets.empty());
    CHECK(copy.objects[0]->volumes[0]->extruder_id()==1);
    CHECK_NOTHROW(orca::slicing::validate_output_capabilities(project));
}

TEST_CASE("Painted split facets retain their real geometry and palette in the scene", "[OrcaSlicingSupport]")
{
    auto project=preparation_project();
    project.metadata["sourceColors"]=nlohmann::json::array({"#ff0000","#00ff00"});
    auto* volume=project.model.objects[0]->volumes[0];
    const auto originalCount=volume->mesh().its.indices.size();
    // Root splits all three sides (3), four children carry material 2 (8).
    // Native 3MF serializes its nibbles in reverse traversal order.
    volume->mmu_segmentation_facets.set_triangle_from_string(0,"88883");
    ScopedTemporaryFile scene(".scene");
    const auto info=orca::slicing::write_scene(project,scene.string());
    CHECK(info["triangleCount"].get<size_t>()==originalCount+3);
    CHECK(volume->mesh().its.indices.size()==originalCount);
    std::ifstream input(scene.string(),std::ios::binary);
    input.seekg(32+12*info["vertexCount"].get<size_t>()+12*info["triangleCount"].get<size_t>());
    size_t green=0;
    for (size_t i=0;i<info["triangleCount"].get<size_t>();++i) {
        unsigned char bytes[2]; input.read(reinterpret_cast<char*>(bytes),2);
        if ((unsigned(bytes[0]) | unsigned(bytes[1])<<8)==1) ++green;
    }
    CHECK(green==4);
}

TEST_CASE("Placement validation rejects whole models outside the bed exclusion or height", "[OrcaSlicingSupport]")
{
    auto project=preparation_project();
    orca::slicing::apply_project_commands(project,nlohmann::json::array({{{"type","arrange"}}}));
    CHECK_NOTHROW(orca::slicing::validate_project_bounds(project));
    project.model.objects[0]->instances[0]->set_offset(Slic3r::Vec3d(1000,1000,0));
    project.model.objects[0]->invalidate_bounding_box();
    CHECK_THROWS_WITH(orca::slicing::validate_project_bounds(project),"MODEL_OUTSIDE_PRINTABLE_AREA");
    project=preparation_project();
    orca::slicing::apply_project_commands(project,nlohmann::json::array({{{"type","arrange"}}}));
    project.config.set("printable_height",5.0);
    CHECK_THROWS_WITH(orca::slicing::validate_project_bounds(project),"MODEL_EXCEEDS_PRINTABLE_HEIGHT");
    project.config.set("printable_height",100.0);
    project.config.set_key_value("bed_exclude_area",new Slic3r::ConfigOptionPoints({{0,0},{50,0},{50,50},{0,50}}));
    CHECK_THROWS_WITH(orca::slicing::validate_project_bounds(project),"MODEL_INTERSECTS_EXCLUDED_AREA");
}
TEST_CASE("Basic OBJ geometry imports through the native parser", "[OrcaSlicingSupport]")
{
    ScopedTemporaryFile file(".obj");
    {
        std::ofstream obj(file.string());
        obj<<"v 0 0 0\nv 10 0 0\nv 0 10 0\nv 0 0 10\nf 1 3 2\nf 1 2 4\nf 2 3 4\nf 3 1 4\n";
    }
    const auto project=orca::slicing::load_project(file.string());
    REQUIRE(project.model.objects.size()==1);
    CHECK(project.model.objects[0]->volumes[0]->mesh().its.indices.size()==4);
    CHECK(project.metadata["basePreset"]["id"]=="generic/fff-0.4-200");
}


TEST_CASE("Offline catalog and applied material use the same inherited bundle", "[OrcaSlicingSupport][MobilePresets]")
{
    const auto catalog=orca::slicing::offline_presets();
    REQUIRE(catalog["devices"].size()==2);
    REQUIRE(catalog["materials"].size()==3);
    CHECK(catalog["devices"][1]["requiresConfirmation"]==true);
    CHECK(catalog["devices"][1]["resetScopes"]==nlohmann::json::array({"device","material","process"}));
    for(size_t n=1;n<catalog["materials"].size();++n) {
        DYNAMIC_SECTION(catalog["materials"][n]["id"].get<std::string>()) {
            auto project=preparation_project();
            const auto& entry=catalog["materials"][n];
            orca::slicing::apply_preset(project,"material",entry["id"].get<std::string>());
            for(const char* key:{"nozzle_temperature","nozzle_temperature_initial_layer","filament_density","filament_flow_ratio"}) {
                auto expected=Slic3r::DynamicPrintConfig::full_print_config();
                orca::slicing::apply_config_overrides(expected,nlohmann::json{{key,entry["config"][key]}});
                CHECK(project.config.option(key)->serialize()==expected.option(key)->serialize());
            }
            CHECK(project.metadata["materialPresetSourceRefs"]==entry["sourceRefs"]);
            REQUIRE(entry["sourceRefs"].size()==3);
            CHECK(entry["sourceRefs"][0]["path"].get<std::string>().find("fdm_filament_common.json")!=std::string::npos);
        }
    }
}
TEST_CASE("Generic machine preset resets actual machine material and process config", "[OrcaSlicingSupport][MobilePresets]")
{
    auto project=preparation_project();
    project.config.set("layer_height",0.31); project.config.set("wall_loops",5);
    project.config.set_key_value("nozzle_diameter",new Slic3r::ConfigOptionFloats({0.8}));
    const auto original=project.model.objects[0]->volumes[0]->mesh().its.vertices;
    orca::slicing::apply_preset(project,"device","generic/fff-0.4-200");
    const auto volume=orca::slicing::describe_project(project)["printVolume"];
    CHECK(volume["polygon"]==nlohmann::json::array({{0,0},{200,0},{200,200},{0,200}}));
    CHECK_THAT(volume["height"].get<double>(),Catch::Matchers::WithinAbs(200,1e-9));
    CHECK_THAT(project.config.option<Slic3r::ConfigOptionFloats>("nozzle_diameter")->values[0],Catch::Matchers::WithinAbs(0.4,1e-9));
    const auto defaults=Slic3r::DynamicPrintConfig::full_print_config();
    CHECK(project.config.opt_int("wall_loops")==defaults.opt_int("wall_loops"));
    CHECK_THAT(project.config.opt_float("layer_height"),Catch::Matchers::WithinAbs(defaults.opt_float("layer_height"),1e-9));
    CHECK(project.model.objects[0]->volumes[0]->mesh().its.vertices==original);
}
TEST_CASE("Preset changes reject stale logical material assignments without implicit mono conversion", "[OrcaSlicingSupport][MobilePresets]")
{
    auto project=preparation_project();
    project.metadata["sourceColors"]={"#ff0000","#00ff00"};
    project.config.set_key_value("filament_colour",new Slic3r::ConfigOptionStrings({"#ff0000","#00ff00"}));
    project.config.set_key_value("filament_density",new Slic3r::ConfigOptionFloats({1.2,1.2}));
    project.config.set_key_value("filament_map",new Slic3r::ConfigOptionInts({1,1}));
    orca::slicing::apply_project_commands(project,nlohmann::json::array({{{"type","assignMaterial"},{"filamentIndex",1}}}));
    CHECK_THROWS(orca::slicing::apply_preset(project,"material","generic/petg"));
    CHECK_FALSE(project.metadata.contains("outputMode"));
    project=preparation_project(); project.metadata["sourceColors"]={"#ff0000","#00ff00"};
    orca::slicing::apply_project_commands(project,nlohmann::json::array({{{"type","singleMaterial"},{"filamentIndex",0}}}));
    CHECK_NOTHROW(orca::slicing::apply_preset(project,"material","generic/petg"));
    CHECK(orca::slicing::describe_project(project)["model"]["colors"].size()==2);
    CHECK(project.config.option<Slic3r::ConfigOptionStrings>("filament_colour")->values.size()==1);
}
TEST_CASE("Each temporary orientation previews the exact confirmed command without modifying source", "[OrcaSlicingSupport][MobileOrientation]")
{
    auto source=preparation_project(); source.metadata["projectId"]="source";source.metadata["revision"]=7;
    const auto before=orca::slicing::describe_project(source);
    for(int n=0;n<6;++n) {
        DYNAMIC_SECTION("candidate "<<n) {
            auto temporary=source;
            auto preview=orca::slicing::orientation_preview(temporary,nlohmann::json{{"candidateIndex",n}});
            REQUIRE(preview["candidates"].size()==6);
            CHECK(preview["sourceRevision"]==7);
            CHECK(preview["projectId"]=="source");
            auto confirmed=source;
            orca::slicing::apply_project_commands(confirmed,nlohmann::json::array({preview["command"]}));
            const auto bounds=confirmed.model.objects[0]->instance_bounding_box(0);
            CHECK_THAT(bounds.min.z(),Catch::Matchers::WithinAbs(0,1e-6));
            CHECK_THAT(bounds.size().z(),Catch::Matchers::WithinAbs(preview["candidates"][n]["height"].get<double>(),1e-6));
            check_prepared_geometry(temporary,confirmed);
            CHECK(orca::slicing::describe_project(source)==before);
        }
    }
}
TEST_CASE("Picked world normals rotate the selected face down and preserve instance scale", "[OrcaSlicingSupport][MobileOrientation]")
{
    auto source=preparation_project();
    // This math fixture reaches 120 mm after nonuniform scaling. Give its
    // requested orientations a real bed that can contain them.
    source.config.set_key_value("printable_area",new Slic3r::ConfigOptionPoints({{0,0},{500,0},{500,500},{0,500}}));
    source.model.objects[0]->instances[0]->set_rotation(Slic3r::Vec3d(0.3,0.2,0.4));
    source.model.objects[0]->instances[0]->set_scaling_factor(Slic3r::Vec3d(2,3,4));
    source.model.objects[0]->invalidate_bounding_box();
    const Slic3r::Matrix3d rotation=source.model.objects[0]->instances[0]->get_transformation().get_rotation_matrix().linear();
    const Slic3r::Vec3d worldNormal=rotation*Slic3r::Vec3d::UnitX();
    auto temporary=source;
    const auto preview=orca::slicing::orientation_preview(temporary,{{"normal",{worldNormal.x(),worldNormal.y(),worldNormal.z()}}});
    CHECK(preview["selectedIndex"]==-1);
    auto confirmed=source;
    orca::slicing::apply_project_commands(confirmed,nlohmann::json::array({preview["command"]}));
    const auto* instance=confirmed.model.objects[0]->instances[0];
    const Slic3r::Vec3d finalNormal=instance->get_transformation().get_rotation_matrix().linear()*Slic3r::Vec3d::UnitX();
    CHECK_THAT(finalNormal.x(),Catch::Matchers::WithinAbs(0,1e-6));
    CHECK_THAT(finalNormal.y(),Catch::Matchers::WithinAbs(0,1e-6));
    CHECK_THAT(finalNormal.z(),Catch::Matchers::WithinAbs(-1,1e-6));
    CHECK_THAT(instance->get_scaling_factor().y(),Catch::Matchers::WithinAbs(3,1e-9));
    CHECK_THAT(confirmed.model.objects[0]->instance_bounding_box(0).min.z(),Catch::Matchers::WithinAbs(0,1e-6));
    check_prepared_geometry(temporary,confirmed);
    CHECK_THROWS(orca::slicing::orientation_preview(temporary,{{"normal",{0,0,0}}}));
}
TEST_CASE("Scene ranges cover exported painted triangles and distinguish instances and parts", "[OrcaSlicingSupport][MobileSceneRanges]")
{
    auto project=preparation_project();
    auto* object=project.model.objects[0];
    object->volumes[0]->mmu_segmentation_facets.set_triangle_from_string(0,"88883");
    object->add_volume(Slic3r::make_cube(5,5,5),Slic3r::ModelVolumeType::MODEL_PART,false);
    object->add_instance(*object->instances[0])->set_offset(Slic3r::Vec3d(40,0,0));
    project.metadata["sourceColors"]={"#ff0000","#00ff00"};
    ScopedTemporaryFile scene(".scene");
    const auto info=orca::slicing::write_scene(project,scene.string());
    REQUIRE(info["sceneRanges"].size()==4);
    size_t next=0;
    for(const auto& range:info["sceneRanges"]) {
        CHECK(range["firstTriangle"].get<size_t>()==next);
        next+=range["triangleCount"].get<size_t>();
        CHECK(range["objectIndex"]==0);
        CHECK(range.contains("bounds"));
    }
    CHECK(next==info["triangleCount"].get<size_t>());
    CHECK(info["sceneRanges"][0]["triangleCount"]==15);
    CHECK(info["sceneRanges"][1]["volumeIndex"]==1);
    CHECK(info["sceneRanges"][2]["instanceIndex"]==1);
    std::ifstream input(scene.string(),std::ios::binary); char magic[8];input.read(magic,8);
    CHECK(std::string(magic,8)=="ORCASC01");
}
TEST_CASE("Filament cost uses actual per slot mass and distinguishes unknown price and currency", "[OrcaSlicingSupport][MobileCost]")
{
    auto config=Slic3r::DynamicPrintConfig::full_print_config();
    config.set_key_value("filament_density",new Slic3r::ConfigOptionFloats({1.25,1.5}));
    config.set_key_value("filament_cost",new Slic3r::ConfigOptionFloats({20,40}));
    const nlohmann::json usage={{"0",1000.0},{"1",2000.0}};
    auto cost=orca::slicing::filament_cost(config,usage,"CNY");
    CHECK_THAT(cost["filamentCost"].get<double>(),Catch::Matchers::WithinAbs(0.145,1e-9));
    CHECK(cost["costCurrency"]=="CNY"); CHECK(cost["costStatus"]=="estimated");
    cost=orca::slicing::filament_cost(config,usage);
    CHECK(cost["costStatus"]=="missing_currency"); CHECK_FALSE(cost.contains("costCurrency"));
    config.set_key_value("filament_cost",new Slic3r::ConfigOptionFloats({20,0}));
    cost=orca::slicing::filament_cost(config,usage,"CNY");
    CHECK(cost["costStatus"]=="missing_price"); CHECK_FALSE(cost.contains("filamentCost"));
    cost=orca::slicing::filament_cost(config,usage);
    CHECK(cost["costStatus"]=="missing_price_and_currency");
}
TEST_CASE("Preset catalog reads do not touch invalid snapshots or require an input model", "[OrcaSlicingSupport][MobilePresets]")
{
    orca::slicing::Engine engine("");
    const auto catalog=engine.execute({{"operation","presets"},{"snapshotPath","does-not-exist"}},{});
    const auto staticCatalog=orca::slicing::offline_presets();
    for(const char* field:{"devices","materials","sourceRefs","version"}) CHECK(catalog[field]==staticCatalog[field]);
    CHECK(catalog.contains("parameterMetadata"));
}

TEST_CASE("Restoring source profiles restores config without replacing geometry and remains externally undoable", "[OrcaSlicingSupport][MobilePresets]")
{
    ScopedTemporaryFile file(".obj");
    {std::ofstream obj(file.string());obj<<"v 0 0 0\nv 10 0 0\nv 0 10 0\nv 0 0 10\nf 1 3 2\nf 1 2 4\nf 2 3 4\nf 3 1 4\n";}
    const auto source=orca::slicing::load_project(file.string());
    auto project=source;
    orca::slicing::apply_preset(project,"material","generic/petg");
    project.config.set("wall_loops",5);
    const auto before=orca::slicing::config_json(project.config);
    orca::slicing::apply_preset(project,"material","source/current");
    CHECK(project.config.opt_int("wall_loops")==5);
    CHECK(project.config.option("filament_density")->serialize()==source.config.option("filament_density")->serialize());
    CHECK(project.metadata["materialPresetSourceRefs"][0]["sha256"]==source.inputSha256);
    orca::slicing::apply_preset(project,"device","source/current");
    CHECK(orca::slicing::config_json(project.config)==orca::slicing::config_json(source.config));
    orca::slicing::apply_config_overrides(project.config,before);
    CHECK(orca::slicing::config_json(project.config)==before);
    CHECK(orca::slicing::describe_project(project)["objects"]==orca::slicing::describe_project(source)["objects"]);
}
TEST_CASE("Preset catalog bypasses a worker lock already held by an import", "[OrcaSlicingSupport][MobilePresets]")
{
    orca::slicing::Engine engine(""); nlohmann::json observed;
    CHECK_THROWS(engine.execute({{"operation","import"},{"inputPath","missing-native-import.obj"}},[&](const nlohmann::json&) {
        observed=engine.execute({{"operation","presets"}},{});
    }));
    const auto staticCatalog=orca::slicing::offline_presets();
    for(const char* field:{"devices","materials","sourceRefs","version"}) CHECK(observed[field]==staticCatalog[field]);
    CHECK(observed.contains("parameterMetadata"));
}

TEST_CASE("Native warnings expose only genuine model instance volume or config locations", "[OrcaSlicingSupport][MobileWarnings]")
{
    const auto project=preparation_project(); Slic3r::StringObjectException warning;
    warning.string="Risk without native location";
    auto located=orca::slicing::describe_warning(project,warning);
    CHECK(located["kind"]=="global"); CHECK_FALSE(located.contains("objectIndex")); CHECK_FALSE(located.contains("layer"));
    warning.object=project.model.objects[0]->instances[0];
    located=orca::slicing::describe_warning(project,warning);
    CHECK(located["objectIndex"]==0); CHECK(located["instanceIndex"]==0);
    warning.object=project.model.objects[0]->volumes[0];
    located=orca::slicing::describe_warning(project,warning);
    CHECK(located["volumeIndex"]==0); CHECK_FALSE(located.contains("instanceIndex"));
    warning.object=nullptr; warning.opt_key="layer_height";
    located=orca::slicing::describe_warning(project,warning);
    CHECK(located["kind"]=="config"); CHECK(located["configKey"]=="layer_height"); CHECK_FALSE(located.contains("layer"));
    const auto unrelated=preparation_project(); warning.object=unrelated.model.objects[0];warning.opt_key.clear();
    located=orca::slicing::describe_warning(project,warning);
    CHECK(located["kind"]=="global"); CHECK_FALSE(located.contains("objectIndex"));
}

TEST_CASE("Source preset catalog reads the captured original profile and verifies frozen identity", "[OrcaSlicingSupport][MobilePresets]")
{
    const auto project=preparation_project();
    const auto config=orca::slicing::config_json(project.config);
    const std::string inputSha(64,'a');
    const nlohmann::json source={{"schemaVersion",2},{"inputPath","original.3mf"},{"inputSha256",inputSha},
        {"config",config},{"configSha256",orca::slicing::json_sha256(config)}};
    ScopedTemporaryFile sidecar(".source-profile.json");
    {std::ofstream output(sidecar.string());output<<source.dump();}
    const nlohmann::json request={{"operation","presets"},{"sourceProfilePath",sidecar.string()},
        {"sourceProfileSha256",orca::slicing::file_sha256(sidecar.string())},{"inputSha256",inputSha}};
    orca::slicing::Engine engine(""); const auto catalog=engine.execute(request,{});
    CHECK(catalog["devices"][0]["available"]==true);
    CHECK(catalog["devices"][0]["config"]==config);
    CHECK(catalog["materials"][0]["config"]["filament_density"]==config["filament_density"]);
    CHECK_FALSE(catalog["materials"][0]["config"].contains("printable_area"));
    CHECK(catalog["devices"][0]["sourceRefs"][0]["sha256"]==inputSha);
    auto wrong=request;wrong["inputSha256"]=std::string(64,'b');
    CHECK_THROWS_WITH(engine.execute(wrong,{}),"SOURCE_PROFILE_IDENTITY_MISMATCH");
    {std::ofstream output(sidecar.string(),std::ios::app);output<<" ";}
    CHECK_THROWS_WITH(engine.execute(request,{}),"SOURCE_PROFILE_IDENTITY_MISMATCH");
}
TEST_CASE("Print volume descriptor preserves distinct native exclusion rectangles", "[OrcaSlicingSupport][MobileSceneRanges]")
{
    auto project=preparation_project();
    project.config.set_key_value("bed_exclude_area",new Slic3r::ConfigOptionPoints({{0,0},{10,0},{10,10},{0,10},{80,80},{90,80},{90,90},{80,90}}));
    const auto volume=orca::slicing::print_volume(project.config);
    REQUIRE(volume["exclusions"].size()==2);
    CHECK(volume["exclusions"][0]==nlohmann::json::array({{0,0},{10,0},{10,10},{0,10}}));
    CHECK(volume["exclusions"][1]==nlohmann::json::array({{80,80},{90,80},{90,90},{80,90}}));
}
TEST_CASE("Final explicit reassignment supersedes historical slots while preserving source paint", "[OrcaSlicingSupport][MobilePresets]")
{
    auto project=preparation_project();
    project.metadata["sourceColors"]={"#ff0000","#00ff00"};
    project.model.objects[0]->volumes[0]->mmu_segmentation_facets.set_triangle_from_string(0,"8");
    CHECK_THROWS(orca::slicing::validate_material_assignments(project));
    // Slot 1 belonged to the old profile; the persisted history later remaps it
    // explicitly to slot 0. Only the final state may reach Print or the catalog.
    const nlohmann::json history=nlohmann::json::array({{{"type","assignMaterial"},{"filamentIndex",1}},{{"type","assignMaterial"},{"filamentIndex",0}}});
    CHECK_NOTHROW(orca::slicing::apply_project_commands(project,history));
    CHECK_NOTHROW(orca::slicing::apply_preset(project,"material","generic/pla"));
    REQUIRE(project.metadata["materialAssignments"].size()==1);
    CHECK(project.metadata["materialAssignments"][0]["filamentIndex"]==0);
    CHECK_FALSE(project.metadata.contains("outputMode"));
    CHECK_FALSE(project.model.objects[0]->volumes[0]->mmu_segmentation_facets.empty());
    CHECK(project.metadata["projectCommands"]==history);
    CHECK(orca::slicing::slicing_model(project).objects[0]->volumes[0]->mmu_segmentation_facets.empty());
}

TEST_CASE("Reflected scene face normals orient the picked outer face onto the bed", "[OrcaSlicingSupport][MobileOrientation][MobileSceneRanges]")
{
    struct Scene {std::vector<Slic3r::Vec3d> vertices;std::vector<std::array<uint32_t,3>> triangles;std::vector<uint16_t> palette;};
    auto read_scene=[](const orca::slicing::LoadedProject& project) {
        ScopedTemporaryFile file(".scene");const auto info=orca::slicing::write_scene(project,file.string());
        std::ifstream input(file.string(),std::ios::binary);input.seekg(32);Scene scene;
        for(size_t n=0;n<info["vertexCount"].get<size_t>();++n) {
            float xyz[3];input.read(reinterpret_cast<char*>(xyz),sizeof(xyz));scene.vertices.emplace_back(xyz[0],xyz[1],xyz[2]);
        }
        for(size_t n=0;n<info["triangleCount"].get<size_t>();++n) {
            std::array<uint32_t,3> triangle;input.read(reinterpret_cast<char*>(triangle.data()),sizeof(triangle));scene.triangles.push_back(triangle);
        }
        for(size_t n=0;n<scene.triangles.size();++n) {
            uint16_t color;input.read(reinterpret_cast<char*>(&color),sizeof(color));scene.palette.push_back(color);
        }
        REQUIRE(input.good());return scene;
    };
    auto normal=[](const Scene& scene,size_t index)->Slic3r::Vec3d {
        const auto& t=scene.triangles[index];
        return (scene.vertices[t[1]]-scene.vertices[t[0]]).cross(scene.vertices[t[2]]-scene.vertices[t[0]]).normalized();
    };
    auto original=preparation_project();original.metadata["projectId"]="mirrored";original.metadata["revision"]=3;
    original.metadata["sourceColors"]={"#ff0000","#00ff00"};
    original.model.objects[0]->volumes[0]->mmu_segmentation_facets.set_triangle_from_string(0,"8");
    const auto baseline=read_scene(original);
    for(int mirrors=0;mirrors<4;++mirrors) {
        DYNAMIC_SECTION("instance/volume mirror mask "<<mirrors) {
            auto source=original;auto* object=source.model.objects[0];auto* volume=object->volumes[0];
            if(mirrors&1) object->instances[0]->set_mirror(Slic3r::Vec3d(-1,1,1));
            if(mirrors&2) volume->set_mirror(Slic3r::Vec3d(1,-1,1));
            object->invalidate_bounding_box();
            const auto sourceVertices=volume->mesh().its.vertices;const auto sourceIndices=volume->mesh().its.indices;
            const auto sourcePaint=volume->mmu_segmentation_facets.get_triangle_as_string(0);
            const auto before=orca::slicing::describe_project(source);
            const auto scene=read_scene(source);CHECK(scene.palette==baseline.palette);
            const double top=object->instance_bounding_box(0).max.z();
            size_t picked=scene.triangles.size();
            for(size_t n=0;n<scene.triangles.size();++n) {
                const auto& face=scene.triangles[n];
                if(std::abs(scene.vertices[face[0]].z()-top)<1e-6 && std::abs(scene.vertices[face[1]].z()-top)<1e-6 && std::abs(scene.vertices[face[2]].z()-top)<1e-6) {picked=n;break;}
            }
            REQUIRE(picked<scene.triangles.size());
            const auto pickedNormal=normal(scene,picked);CHECK_THAT(pickedNormal.z(),Catch::Matchers::WithinAbs(1,1e-6));
            auto temporary=source;
            const auto preview=orca::slicing::orientation_preview(temporary,{{"normal",{pickedNormal.x(),pickedNormal.y(),pickedNormal.z()}}});
            auto confirmed=source;
            orca::slicing::apply_project_commands(confirmed,nlohmann::json::array({preview["command"]}));
            check_prepared_geometry(temporary,confirmed);
            const auto finalScene=read_scene(confirmed);CHECK(finalScene.palette==scene.palette);
            CHECK_THAT(normal(finalScene,picked).z(),Catch::Matchers::WithinAbs(-1,1e-6));
            for(const auto vertex:finalScene.triangles[picked])
                CHECK_THAT(finalScene.vertices[vertex].z(),Catch::Matchers::WithinAbs(0,1e-5));
            CHECK_THAT(confirmed.model.objects[0]->instance_bounding_box(0).min.z(),Catch::Matchers::WithinAbs(0,1e-6));
            CHECK(source.model.objects[0]->volumes[0]->mesh().its.vertices==sourceVertices);
            CHECK(source.model.objects[0]->volumes[0]->mesh().its.indices==sourceIndices);
            CHECK(source.model.objects[0]->volumes[0]->mmu_segmentation_facets.get_triangle_as_string(0)==sourcePaint);
            CHECK(orca::slicing::describe_project(source)==before);
        }
    }
}

namespace {
void write_asymmetric_obj(const std::string& path,const Slic3r::Vec3d& origin=Slic3r::Vec3d::Zero())
{
    const auto mesh=Slic3r::make_cube(8,12,16);std::ofstream file(path);
    for(const auto& vertex:mesh.its.vertices) {
        const Slic3r::Vec3d p=vertex.cast<double>()+origin;file<<"v "<<p.x()<<" "<<p.y()<<" "<<p.z()<<"\n";
    }
    for(const auto& face:mesh.its.indices) file<<"f "<<face[0]+1<<" "<<face[1]+1<<" "<<face[2]+1<<"\n";
}
}
TEST_CASE("Bed edge raw OBJ orientations keep center or use only the necessary translation", "[OrcaSlicingSupport][MobileOrientation]")
{
    ScopedTemporaryFile file(".obj");write_asymmetric_obj(file.string());
    const auto source=orca::slicing::load_project(file.string());
    const auto before=orca::slicing::describe_project(source);
    const auto original=source.model.objects[0]->instance_bounding_box(0);
    CHECK_THAT(original.center().x(),Catch::Matchers::WithinAbs(4,1e-9));
    CHECK_THAT(original.center().y(),Catch::Matchers::WithinAbs(6,1e-9));
    for(int n=0;n<6;++n) {
        DYNAMIC_SECTION("raw candidate "<<n) {
            auto previewProject=source;
            const auto preview=orca::slicing::orientation_preview(previewProject,{{"candidateIndex",n}});
            for(const auto& candidate:preview["candidates"]) CHECK(candidate["valid"]==true);
            const auto confirmed=orca::slicing::load_project(file.string(),orca::slicing::config_json(source.config),
                source.metadata.at("transforms"),{},previewProject.metadata.at("projectCommands"));
            CHECK_NOTHROW(orca::slicing::validate_project_bounds(previewProject));
            CHECK_NOTHROW(orca::slicing::validate_project_bounds(confirmed));
            check_prepared_geometry(previewProject,confirmed);
            const auto bounds=confirmed.model.objects[0]->instance_bounding_box(0);
            for(int axis=0;axis<2;++axis) {
                const double half=bounds.size()[axis]*0.5;
                CHECK_THAT(bounds.center()[axis],Catch::Matchers::WithinAbs(std::clamp(original.center()[axis],half,200-half),1e-9));
            }
            if(n==1) { // The actual r13 command: original Y [0,12] becomes [0,16], never [-16,0].
                CHECK_THAT(bounds.min.y(),Catch::Matchers::WithinAbs(0,1e-9));
                CHECK_THAT(bounds.max.y(),Catch::Matchers::WithinAbs(16,1e-9));
            }
            if(n==4) { // Original center X=4 cannot hold a 16 mm width: move only +4 mm.
                CHECK_THAT(bounds.size().x(),Catch::Matchers::WithinAbs(16,1e-9));
                CHECK_THAT(bounds.center().x(),Catch::Matchers::WithinAbs(8,1e-9));
                CHECK_THAT(bounds.min.x(),Catch::Matchers::WithinAbs(0,1e-9));
            }
            CHECK(orca::slicing::describe_project(source)==before);
        }
    }
}
TEST_CASE("Far source geometry rotated scaled instances preserve XY center in preview and replay", "[OrcaSlicingSupport][MobileOrientation]")
{
    ScopedTemporaryFile file(".obj");write_asymmetric_obj(file.string(),Slic3r::Vec3d(120,-70,25));
    const nlohmann::json initial=nlohmann::json::array({{{"objectIndex",0},{"instanceIndex",0},
        {"rotation",{0.17,0.29,0.23}},{"scale",{1.5,0.75,2.0}},{"translation",{70,70,25}}}});
    auto source=orca::slicing::load_project(file.string(),nlohmann::json::object(),initial);
    auto* object=source.model.objects[0];auto* instance=object->instances[0];
    const auto initialBounds=object->instance_bounding_box(0);
    const Slic3r::Vec3d offset=instance->get_offset()+Slic3r::Vec3d(70-initialBounds.center().x(),100-initialBounds.center().y(),-initialBounds.min.z());
    auto transforms=initial;transforms[0]["translation"]={offset.x(),offset.y(),offset.z()};
    source=orca::slicing::load_project(file.string(),nlohmann::json::object(),transforms);
    const auto before=orca::slicing::describe_project(source);
    const Slic3r::Matrix3d rotation=source.model.objects[0]->instances[0]->get_transformation().get_rotation_matrix().linear();
    const Slic3r::Vec3d normal=rotation*Slic3r::Vec3d::UnitX();
    for(int mode=0;mode<7;++mode) {
        DYNAMIC_SECTION("translated source mode "<<mode) {
            auto temporary=source;
            const nlohmann::json request=mode<6?nlohmann::json{{"candidateIndex",mode}}:
                nlohmann::json{{"normal",{normal.x(),normal.y(),normal.z()}}};
            const auto preview=orca::slicing::orientation_preview(temporary,request);
            const auto confirmed=orca::slicing::load_project(file.string(),orca::slicing::config_json(source.config),transforms,{},temporary.metadata.at("projectCommands"));
            CHECK_NOTHROW(orca::slicing::validate_project_bounds(confirmed));check_prepared_geometry(temporary,confirmed);
            const auto bounds=confirmed.model.objects[0]->instance_bounding_box(0);
            CHECK_THAT(bounds.center().x(),Catch::Matchers::WithinAbs(70,1e-8));
            CHECK_THAT(bounds.center().y(),Catch::Matchers::WithinAbs(100,1e-8));
            CHECK_THAT(bounds.min.z(),Catch::Matchers::WithinAbs(0,1e-8));
            CHECK(orca::slicing::describe_project(source)==before);
        }
    }
}
TEST_CASE("Invalid orientation candidates retain the strict printable and exclusion gates", "[OrcaSlicingSupport][MobileOrientation]")
{
    ScopedTemporaryFile file(".obj");write_asymmetric_obj(file.string());
    auto source=orca::slicing::load_project(file.string());
    SECTION("rotated width exceeds a real rectangular bed") {
        source.config.set_key_value("printable_area",new Slic3r::ConfigOptionPoints({{0,0},{10,0},{10,200},{0,200}}));
        auto temporary=source;const auto result=orca::slicing::orientation_preview(temporary,{{"candidateIndex",0}});
        CHECK(result["candidates"][4]["valid"]==false);
        CHECK(result["candidates"][4]["error"]=="ORIENTATION_DOES_NOT_FIT_PRINTABLE_AREA");
        temporary=source;
        CHECK_THROWS_WITH(orca::slicing::orientation_preview(temporary,{{"candidateIndex",4}}),"ORIENTATION_DOES_NOT_FIT_PRINTABLE_AREA");
    }
    SECTION("nonrectangular bed rejects a footprint beyond its diagonal") {
        source.config.set_key_value("printable_area",new Slic3r::ConfigOptionPoints({{0,0},{40,0},{0,40}}));
        source.model.objects[0]->instances[0]->set_offset(Slic3r::Vec3d(15,5,0));source.model.objects[0]->invalidate_bounding_box();
        CHECK_NOTHROW(orca::slicing::validate_project_bounds(source));
        auto temporary=source;const auto result=orca::slicing::orientation_preview(temporary,{{"candidateIndex",0}});
        CHECK(result["candidates"][1]["valid"]==false);temporary=source;
        CHECK_THROWS_WITH(orca::slicing::orientation_preview(temporary,{{"candidateIndex",1}}),"MODEL_OUTSIDE_PRINTABLE_AREA");
    }
    SECTION("a rotated footprint cannot silently enter an excluded region") {
        source.config.set_key_value("bed_exclude_area",new Slic3r::ConfigOptionPoints({{10,0},{30,0},{30,30},{10,30}}));
        CHECK_NOTHROW(orca::slicing::validate_project_bounds(source));
        auto temporary=source;const auto result=orca::slicing::orientation_preview(temporary,{{"candidateIndex",0}});
        CHECK(result["candidates"][4]["valid"]==false);temporary=source;
        CHECK_THROWS_WITH(orca::slicing::orientation_preview(temporary,{{"candidateIndex",4}}),"MODEL_INTERSECTS_EXCLUDED_AREA");
    }
}

namespace {
std::string basic_3mf_model(const std::string& extra={},bool painted=false)
{
    return std::string("<model xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\" xmlns:p=\"http://schemas.microsoft.com/3dmanufacturing/production/2015/06\" unit=\"millimeter\" requiredextensions=\"p\">")+
        "<metadata name=\"OrcaSlicer\">2.5.0</metadata><resources><object id=\"1\" type=\"model\"><mesh><vertices>"
        "<vertex x=\"0\" y=\"0\" z=\"0\"/><vertex x=\"8\" y=\"0\" z=\"0\"/><vertex x=\"0\" y=\"12\" z=\"0\"/><vertex x=\"0\" y=\"0\" z=\"16\"/>"
        "</vertices><triangles><triangle v1=\"0\" v2=\"2\" v3=\"1\""+(painted?" paint_color=\"8\" paint_supports=\"4\"":"")+"/>"
        "<triangle v1=\"0\" v2=\"1\" v3=\"3\"/><triangle v1=\"1\" v2=\"2\" v3=\"3\"/><triangle v1=\"2\" v2=\"0\" v3=\"3\"/>"
        "</triangles></mesh></object>"+extra+"</resources><build><item objectid=\"1\"/></build></model>";
}
std::string one_plate(const std::string& extra={})
{
    return "<plate><metadata key=\"plater_id\" value=\"1\"/>"+extra+
        "<model_instance><metadata key=\"object_id\" value=\"1\"/><metadata key=\"instance_id\" value=\"0\"/></model_instance></plate>";
}
void write_3mf_fixture(const std::string& path,const std::string& model,const std::string& settings={},const nlohmann::json& config=nlohmann::json::object(),
                       const std::map<std::string,std::string>& extras={})
{
    mz_zip_archive zip{};REQUIRE(mz_zip_writer_init_file(&zip,path.c_str(),0));
    struct End{mz_zip_archive& zip;~End(){mz_zip_writer_end(&zip);}} end{zip};
    auto add=[&](const std::string& name,const std::string& data){REQUIRE(mz_zip_writer_add_mem(&zip,name.c_str(),data.data(),data.size(),MZ_DEFAULT_COMPRESSION));};
    add("3D/3dmodel.model",model);
    if(!settings.empty()) add("Metadata/model_settings.config","<config>"+settings+"</config>");
    if(!config.empty()) add("Metadata/project_settings.config",config.dump());
    for(const auto& extra:extras) add(extra.first,extra.second);
    REQUIRE(mz_zip_writer_finalize_archive(&zip));
}
bool has_3mf_feature(const nlohmann::json& inspection,const std::string& id,const std::string& status)
{
    for(const auto& feature:inspection.at("features")) if(feature.at("id")==id && feature.at("status")==status) return true;
    return false;
}
}
TEST_CASE("Basic 3MF five-color paint and native support annotations remain explicit-consent output", "[OrcaSlicingSupport][ThreeMfPreflight]")
{
    ScopedTemporaryFile file(".3mf");
    const nlohmann::json config={{"filament_colour",{"#110000","#002200","#000033","#444400","#555555"}},
        {"filament_density",{"1.24","1.24","1.24","1.24","1.24"}},{"filament_diameter",{"1.75","1.75","1.75","1.75","1.75"}},
        {"filament_map",{1,1,1,1,1}},{"nozzle_diameter",{"0.4"}},{"enable_prime_tower","0"},{"silent_mode","0"}};
    write_3mf_fixture(file.string(),basic_3mf_model({},true),one_plate(),config,{{"Metadata/filament_sequence.json",R"({"plate_1":{"sequence":[],"nozzle_sequence":[],"optimal_assignment":[]}})"}});
    const auto original=orca::slicing::file_sha256(file.string());
    const auto inspection=orca::slicing::inspect_three_mf(file.string());
    CHECK(inspection["blockedFeatures"].empty());CHECK(inspection["currentPlate"]=="1");
    CHECK(has_3mf_feature(inspection,"paint_color","requires_single_material"));
    CHECK(has_3mf_feature(inspection,"paint_supports","supported_native"));
    CHECK(has_3mf_feature(inspection,"retired_disabled_silent_mode","preserved_metadata"));
    CHECK(has_3mf_feature(inspection,"empty_filament_sequence","preserved_metadata"));
    auto project=orca::slicing::load_project(file.string());
    CHECK(project.metadata["blockingFeatures"].empty());
    CHECK(project.metadata["sourceInspectionVersion"]==1);
    CHECK(orca::slicing::describe_project(project)["model"]["colors"].size()==5);
    const auto vertices=project.model.objects[0]->volumes[0]->mesh().its.vertices;
    orca::slicing::apply_project_commands(project,nlohmann::json::array({{{"type","singleMaterial"},{"filamentIndex",0}}}));
    CHECK_NOTHROW(orca::slicing::validate_output_capabilities(project));
    CHECK(project.model.objects[0]->volumes[0]->mesh().its.vertices==vertices);
    CHECK_FALSE(project.model.objects[0]->volumes[0]->mmu_segmentation_facets.empty());
    CHECK(orca::slicing::file_sha256(file.string())==original);
}
TEST_CASE("3MF semantic blocks cannot be bypassed by any output material mode", "[OrcaSlicingSupport][ThreeMfPreflight]")
{
    const std::map<std::string,std::string> extras{{"Metadata/vendor_geometry.json","{\"lattice\":true}"}};
    ScopedTemporaryFile file(".3mf");write_3mf_fixture(file.string(),basic_3mf_model(),one_plate(),{},extras);
    const auto source=orca::slicing::load_project(file.string());
    REQUIRE_FALSE(source.metadata.at("blockingFeatures").empty());
    for(const std::string mode:{"native","singleMaterial"}) {
        DYNAMIC_SECTION(mode) {
            auto project=source;
            if(mode=="singleMaterial") orca::slicing::apply_project_commands(project,nlohmann::json::array({{{"type","singleMaterial"},{"filamentIndex",0}}}));
            CHECK_THROWS(orca::slicing::validate_output_capabilities(project));
            CHECK(project.metadata["blockingFeatures"][0]["blocksAllOutputModes"]==true);
        }
    }
}
TEST_CASE("Multi-plate and unowned 3MF build instances are enumerated and never flattened", "[OrcaSlicingSupport][ThreeMfPreflight]")
{
    ScopedTemporaryFile file(".3mf");
    const std::string second="<plate><metadata key=\"plater_id\" value=\"2\"/><model_instance><metadata key=\"object_id\" value=\"1\"/><metadata key=\"instance_id\" value=\"0\"/></model_instance></plate>";
    write_3mf_fixture(file.string(),basic_3mf_model(),one_plate()+second);
    auto scan=orca::slicing::inspect_three_mf(file.string());CHECK(scan["plateCount"]==2);CHECK(scan["currentPlate"].is_null());
    CHECK(has_3mf_feature(scan,"multiple_plates","blocked"));
    write_3mf_fixture(file.string(),basic_3mf_model(),"<plate><metadata key=\"plater_id\" value=\"1\"/></plate>");
    scan=orca::slicing::inspect_three_mf(file.string());CHECK(has_3mf_feature(scan,"unassigned_build_items","blocked"));
}
TEST_CASE("Unknown extensions geometry part types and scripts have specific 3MF diagnostics", "[OrcaSlicingSupport][ThreeMfPreflight]")
{
    ScopedTemporaryFile file(".3mf");
    write_3mf_fixture(file.string(),basic_3mf_model("<vendor:implicit xmlns:vendor=\"urn:vendor:geometry\"/>"),one_plate());
    auto scan=orca::slicing::inspect_three_mf(file.string());CHECK(has_3mf_feature(scan,"model_namespace:urn:vendor:geometry","blocked"));
    write_3mf_fixture(file.string(),basic_3mf_model(),"<object id=\"1\"><part id=\"1\" subtype=\"vendor_lattice\"/></object>"+one_plate());
    scan=orca::slicing::inspect_three_mf(file.string());CHECK(has_3mf_feature(scan,"part_type:vendor_lattice","blocked"));
    write_3mf_fixture(file.string(),basic_3mf_model(),one_plate(),{},{{"Metadata/filament_sequence.json",R"({"plate_1":{"sequence":[1,2]}})"}});
    scan=orca::slicing::inspect_three_mf(file.string());CHECK(has_3mf_feature(scan,"custom_filament_sequence","blocked"));
    write_3mf_fixture(file.string(),basic_3mf_model(),one_plate(),{{"post_process",{"python fix.py"}},{"slicing_pipeline_plugin",{"vendor.python"}}});
    scan=orca::slicing::inspect_three_mf(file.string());CHECK(has_3mf_feature(scan,"external_processing:post_process","blocked"));
    CHECK(has_3mf_feature(scan,"external_processing:slicing_pipeline_plugin","blocked"));
    write_3mf_fixture(file.string(),basic_3mf_model(),one_plate(),{{"sparse_infill_pattern","future_vendor_pattern"}});
    const auto substituted=orca::slicing::load_project(file.string());
    CHECK_FALSE(substituted.metadata["blockingFeatures"].empty());
    CHECK_THROWS(orca::slicing::validate_output_capabilities(substituted));
    write_3mf_fixture(file.string(),basic_3mf_model(),one_plate(),{{"vendor_unsupported_flow_policy","special"}});
    scan=orca::slicing::inspect_three_mf(file.string());CHECK(has_3mf_feature(scan,"unknown_config:vendor_unsupported_flow_policy","blocked"));
}
TEST_CASE("Native single-plate settings are applied before overrides without rejecting normal parts", "[OrcaSlicingSupport][ThreeMfPreflight]")
{
    ScopedTemporaryFile file(".3mf");
    const std::string parts="<object id=\"1\"><part id=\"1\" subtype=\"normal_part\"/><part id=\"2\" subtype=\"modifier_part\"/></object>";
    write_3mf_fixture(file.string(),basic_3mf_model(),parts+one_plate("<metadata key=\"print_sequence\" value=\"by object\"/>"),{{"print_sequence","by layer"}});
    const auto scan=orca::slicing::inspect_three_mf(file.string());CHECK(scan["blockedFeatures"].empty());
    CHECK(has_3mf_feature(scan,"parts_modifiers","supported_native"));
    // Use a real single mesh for the load/plate configuration assertion; the
    // two-part scan above protects recognition without inventing missing geometry.
    write_3mf_fixture(file.string(),basic_3mf_model(),one_plate("<metadata key=\"print_sequence\" value=\"by object\"/>"),{{"print_sequence","by layer"}});
    const auto project=orca::slicing::load_project(file.string());
    CHECK(orca::slicing::config_json(project.config)["print_sequence"]=="by object");
    const auto override=orca::slicing::load_project(file.string(),{{"print_sequence","by layer"}});
    CHECK(orca::slicing::config_json(override.config)["print_sequence"]=="by layer");
}
TEST_CASE("External processing is rejected even after explicit single-material conversion", "[OrcaSlicingSupport][ThreeMfPreflight]")
{
    for(const char* key:{"post_process","plugins","slicing_pipeline_plugin"}) {
        DYNAMIC_SECTION(key) {
            auto project=preparation_project();
            project.config.set_key_value(key,new Slic3r::ConfigOptionStrings({"unavailable.py"}));
            orca::slicing::apply_project_commands(project,nlohmann::json::array({{{"type","singleMaterial"},{"filamentIndex",0}}}));
            CHECK_THROWS(orca::slicing::validate_output_capabilities(project));
        }
    }
}

TEST_CASE("P1 parameter catalog exports actual acceleration vector constraints without invented limits", "[OrcaSlicingSupport][ParameterMetadata]")
{
    const auto catalog=orca::slicing::project_presets(nlohmann::json::object());
    const auto& metadata=catalog.at("parameterMetadata");const auto& parameters=metadata.at("parameters");
    CHECK(metadata.at("version")=="p1-common-v1");CHECK(parameters.size()<=16);CHECK(metadata.dump().size()<16384);
    CHECK(metadata.at("sourceRef").at("path")=="src/libslic3r/PrintConfig.cpp");
    for(const char* key:{"inner_wall_acceleration","machine_max_acceleration_extruding"}) {
        DYNAMIC_SECTION(key) {
            const auto* definition=Slic3r::print_config_def.get(key);REQUIRE(definition);
            const auto& item=parameters.at(key);
            CHECK(item.at("type")=="coFloats");CHECK(item.at("typeId")==int(definition->type));
            CHECK(item.at("vector")==true);CHECK(item.at("nullable")==definition->nullable);
            CHECK(item.at("sidetext")==definition->sidetext);CHECK(item.at("unit")==definition->sidetext);
            CHECK_THAT(item.at("min").get<double>(),Catch::Matchers::WithinAbs(definition->min,1e-9));
            if(definition->max==std::numeric_limits<float>::max()) CHECK_FALSE(item.contains("max"));
            else CHECK_THAT(item.at("max").get<double>(),Catch::Matchers::WithinAbs(definition->max,1e-9));
            const auto* defaults=dynamic_cast<const Slic3r::ConfigOptionVector<double>*>(definition->default_value.get());REQUIRE(defaults);
            REQUIRE(item.at("default").size()==defaults->values.size());
            for(size_t i=0;i<defaults->values.size();++i) CHECK_THAT(item.at("default")[i].get<double>(),Catch::Matchers::WithinAbs(defaults->values[i],1e-9));
            CHECK(item.at("defaultSerialized")==definition->default_value->serialize());
            if(definition->nullable) CHECK(item.at("nilToken")=="nil");else CHECK_FALSE(item.contains("nilToken"));
        }
    }
}
TEST_CASE("Parameter enum identifiers and defaults use the native schema", "[OrcaSlicingSupport][ParameterMetadata]")
{
    const auto metadata=orca::slicing::project_presets(nlohmann::json::object()).at("parameterMetadata");
    const auto& item=metadata.at("parameters").at("support_type");
    const auto* definition=Slic3r::print_config_def.get("support_type");REQUIRE(definition);
    CHECK(item.at("type")=="coEnum");CHECK(item.at("enumValues")==definition->enum_values);
    CHECK(item.at("enumLabels")==definition->enum_labels);
    CHECK(std::find(definition->enum_values.begin(),definition->enum_values.end(),item.at("default").get<std::string>())!=definition->enum_values.end());
    CHECK(item.at("default")==item.at("defaultSerialized"));
}
