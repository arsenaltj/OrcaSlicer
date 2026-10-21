#include "orca_slicing/recommendation_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <limits>
#include <fstream>
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
    CHECK(project.metadata["basePreset"]["presetId"]=="p1-offline-fff-0.4-pla");
}
