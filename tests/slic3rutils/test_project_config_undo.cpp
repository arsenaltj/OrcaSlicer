#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "slic3r/Utils/UndoRedo.hpp"
#include "slic3r/GUI/ProjectConfigRestore.hpp"
#include "local_print_recipe_fixture.hpp"

using namespace Slic3r;
namespace ConfigUndo=UndoRedo::ProjectConfigUndo;

TEST_CASE("restoring material state validates installed presets before committing both slot tables", "[ProjectConfigUndo]")
{
    Test::RecipeApplicationFixture fixture(GENERATE(2u,3u));
    auto& live=fixture.bundle;
    for(size_t i=0;i<live.filament_presets.size();++i) {
        const auto name="Undo test PLA "+std::to_string(i);
        live.filaments.load_preset("",name,live.filaments.default_preset().config,false);
        live.filament_presets[i]=name;
    }
    GUI::LocalPrintRecipeApplication::Prepared staged;std::string error;
    REQUIRE(GUI::LocalPrintRecipeApplication::prepare(fixture.mesh,fixture.mesh.its,fixture.result,fixture.snapshot,live,staged,error));
    live.ams_multi_color_filment={{"#123456"},{"#FEDCBA"},{}};
    const auto original_cache=live.ams_multi_color_filment;
    const auto original_config=live.project_config;const auto original_names=live.filament_presets;
    ConfigUndo::Prepared prepared {staged.bundle->project_config,staged.bundle->filament_presets,true};
    const auto expected_config=prepared.config;const auto expected_names=prepared.filament_presets;
    auto cache=GUI::ProjectConfigRestore::prepare_cache(expected_names.size(),live);
    REQUIRE(GUI::ProjectConfigRestore::validate(prepared.config,prepared.filament_presets,live,error));
    GUI::ProjectConfigRestore::commit(prepared,cache,live);
    CHECK(live.project_config==expected_config);CHECK(live.filament_presets==expected_names);
    CHECK(live.ams_multi_color_filment.size()==expected_names.size());
    CHECK(live.ams_multi_color_filment[0]==original_cache[0]);
    CHECK(cache==original_cache);
    CHECK(prepared.config==original_config);CHECK(prepared.filament_presets==original_names);CHECK_FALSE(prepared.changed);
    GUI::ProjectConfigRestore::commit(prepared,cache,live);
    CHECK(live.project_config==expected_config);
    prepared.changed=true;
    REQUIRE(GUI::ProjectConfigRestore::validate(prepared.config,prepared.filament_presets,live,error));
    GUI::ProjectConfigRestore::commit(prepared,cache,live);
    CHECK(live.project_config==original_config);CHECK(live.filament_presets==original_names);
}

TEST_CASE("invalid restored material definitions leave the live project untouched", "[ProjectConfigUndo]")
{
    Test::RecipeApplicationFixture fixture(2);
    auto& live=fixture.bundle;
    live.filaments.load_preset("","Undo test PLA",live.filaments.default_preset().config,false);
    live.filament_presets.assign(3,"Undo test PLA");
    const auto before=live.project_config;const auto names=live.filament_presets;
    for(int fault=0;fault<7;++fault) {
        DYNAMIC_SECTION("Rejected material restore "<<fault) {
            auto config=before;auto selections=names;
            if(fault==0)selections[0]="Missing preset";
            if(fault==1)config.erase("filament_colour");
            if(fault==2)config.option<ConfigOptionBools>("filament_is_mixed")->values.pop_back();
            if(fault==3)config.set_key_value("filament_nozzle_map",new ConfigOptionStrings({"wrong type"}));
            if(fault==4)selections.clear();
            if(fault==5) {
                config.option<ConfigOptionBools>("filament_is_mixed")->values[2]=true;
                config.option<ConfigOptionStrings>("filament_mixed_components")->values[2]="1,999";
            }
            if(fault==6)config.option<ConfigOptionFloats>("flush_volumes_matrix")->values.pop_back();
            std::string error;
            CHECK_FALSE(GUI::ProjectConfigRestore::validate(config,selections,live,error));
            CHECK_FALSE(error.empty());CHECK(live.project_config==before);CHECK(live.filament_presets==names);
        }
    }
}

static std::vector<UndoRedo::Snapshot> history()
{
    UndoRedo::SnapshotData action {};action.snapshot_type=UndoRedo::SnapshotType::Action;action.printer_technology=ptFFF;
    UndoRedo::SnapshotData selection=action;selection.snapshot_type=UndoRedo::SnapshotType::Selection;
    return {{"First material change",10,1,action},{"Selection",20,1,selection},
            {"Second material change",30,1,action},{"@@@ Topmost @@@",40,0,action}};
}

TEST_CASE("native recipe project changes undo and redo without replacing unrelated settings", "[ProjectConfigUndo]")
{
    Test::RecipeApplicationFixture fixture(GENERATE(2u,3u));
    GUI::LocalPrintRecipeApplication::Prepared staged;std::string error;
    REQUIRE(GUI::LocalPrintRecipeApplication::prepare(fixture.mesh,fixture.mesh.its,fixture.result,fixture.snapshot,fixture.bundle,staged,error));
    auto change=ConfigUndo::Change::capture(fixture.bundle.project_config,fixture.bundle.filament_presets,
        staged.bundle->project_config,staged.bundle->filament_presets);
    REQUIRE(change);CHECK(change->memsize()>sizeof(ConfigUndo::Change));
    auto steps=history();steps.erase(steps.begin()+1,steps.begin()+3);
    REQUIRE(UndoRedo::record_project_config_change(steps,40,change));
    auto live=staged.bundle->project_config;
    live.set_key_value("wipe_tower_x",new ConfigOptionFloats({99.125}));
    const auto live_before=live;
    ConfigUndo::Prepared undone;
    REQUIRE(ConfigUndo::prepare_jump(steps,40,10,live,staged.bundle->filament_presets,undone,error));
    REQUIRE(undone.changed);CHECK(live==live_before);
    auto expected=fixture.bundle.project_config;expected.set_key_value("wipe_tower_x",new ConfigOptionFloats({99.125}));
    CHECK(undone.config==expected);CHECK(undone.filament_presets==fixture.bundle.filament_presets);
    ConfigUndo::Prepared redone;
    REQUIRE(ConfigUndo::prepare_jump(steps,10,40,undone.config,undone.filament_presets,redone,error));
    CHECK(redone.config==live);CHECK(redone.filament_presets==staged.bundle->filament_presets);
    CHECK(fixture.bundle.filament_presets.size()==3);
}

TEST_CASE("jumping across multiple config actions respects action direction and skips selection snapshots", "[ProjectConfigUndo]")
{
    auto steps=history();
    DynamicPrintConfig a;a.set_key_value("filament_colour",new ConfigOptionStrings({"#000000"}));
    auto b=a;b.set_key_value("filament_colour",new ConfigOptionStrings({"#FFFFFF"}));
    auto c=b;c.set_key_value("filament_colour",new ConfigOptionStrings({"#FF0000"}));
    const std::vector<std::string> names={"PLA"};
    steps[0].project_config_change=ConfigUndo::Change::capture(a,names,b,names);
    REQUIRE(UndoRedo::record_project_config_change(steps,40,ConfigUndo::Change::capture(b,names,c,names)));
    ConfigUndo::Prepared result;std::string error;
    REQUIRE(ConfigUndo::prepare_jump(steps,40,10,c,names,result,error));CHECK(result.config==a);
    REQUIRE(ConfigUndo::prepare_jump(steps,10,40,a,names,result,error));CHECK(result.config==c);
    REQUIRE(ConfigUndo::prepare_jump(steps,40,20,c,names,result,error));CHECK(result.config==b);
    REQUIRE(ConfigUndo::prepare_jump(steps,20,30,b,names,result,error));CHECK_FALSE(result.changed);
    REQUIRE(ConfigUndo::prepare_jump(steps,30,30,b,names,result,error));CHECK_FALSE(result.changed);
}

TEST_CASE("conflicting history jumps leave live and prepared configurations unchanged", "[ProjectConfigUndo]")
{
    auto steps=history();
    DynamicPrintConfig a;a.set_key_value("filament_colour",new ConfigOptionStrings({"#000000"}));
    auto b=a;b.set_key_value("filament_colour",new ConfigOptionStrings({"#FFFFFF"}));
    auto c=b;c.set_key_value("filament_colour",new ConfigOptionStrings({"#FF0000"}));
    const std::vector<std::string> names={"PLA"};auto current_names=names;
    steps[0].project_config_change=ConfigUndo::Change::capture(a,names,b,names);
    steps[2].project_config_change=ConfigUndo::Change::capture(b,names,c,{"Other PLA"});
    auto live=c;current_names={"Other PLA"};size_t from=40,to=10;
    const int problem=GENERATE(0,1,2,3,4,5);
    if(problem==0) live.set_key_value("filament_colour",new ConfigOptionStrings({"#ABCDEF"}));
    if(problem==1) current_names={"Manually changed"};
    if(problem==2) to=15;
    if(problem==3) steps[1].timestamp=10;
    if(problem==4) live.set_key_value("filament_colour",new ConfigOptionInt(2));
    // The first reversed delta succeeds in scratch state; the next conflicts.
    if(problem==5) {auto different=b;different.set_key_value("filament_colour",new ConfigOptionStrings({"#123456"}));
        steps[0].project_config_change=ConfigUndo::Change::capture(a,names,different,names);}
    const auto old_live=live;
    ConfigUndo::Prepared destination;destination.changed=true;destination.filament_presets={"untouched"};
    destination.config.set_key_value("wipe_tower_x",new ConfigOptionFloats({71.25}));const auto old_destination=destination.config;
    std::string error;
    CHECK_FALSE(ConfigUndo::prepare_jump(steps,from,to,live,current_names,destination,error));CHECK_FALSE(error.empty());
    CHECK(live==old_live);CHECK(destination.config==old_destination);
    CHECK(destination.filament_presets==std::vector<std::string>{"untouched"});CHECK(destination.changed);
}

TEST_CASE("config changes retain typed values and support option creation and deletion", "[ProjectConfigUndo]")
{
    DynamicPrintConfig a;a.set_key_value("wipe_tower_x",new ConfigOptionFloats({1.234567890123}));
    auto b=a;b.erase("wipe_tower_x");b.set_key_value("filament_colour",new ConfigOptionStrings({"#00FF00"}));
    const std::vector<std::string> names;
    auto change=ConfigUndo::Change::capture(a,names,b,names);REQUIRE(change);
    CHECK_FALSE(ConfigUndo::Change::capture(a,names,a,names));
    auto steps=history();steps.erase(steps.begin()+1,steps.begin()+3);
    REQUIRE(UndoRedo::record_project_config_change(steps,40,change));
    ConfigUndo::Prepared restored;std::string error;
    REQUIRE(ConfigUndo::prepare_jump(steps,40,10,b,names,restored,error));
    REQUIRE(restored.config.option<ConfigOptionFloats>("wipe_tower_x"));
    CHECK(restored.config.option<ConfigOptionFloats>("wipe_tower_x")->values==a.option<ConfigOptionFloats>("wipe_tower_x")->values);
    CHECK_FALSE(restored.config.has("filament_colour"));
}

TEST_CASE("native history binds config payloads only to fresh ordinary actions", "[ProjectConfigUndo]")
{
    auto steps=history();DynamicPrintConfig a,b;b.set_key_value("filament_colour",new ConfigOptionStrings({"#000000"}));
    auto change=ConfigUndo::Change::capture(a,{},b,{});REQUIRE(change);
    const int state=GENERATE(0,1,2,3,4,5);
    if(state==0) steps[2].snapshot_data.snapshot_type=UndoRedo::SnapshotType::GizmoAction;
    if(state==1) steps[2].snapshot_data.snapshot_type=UndoRedo::SnapshotType::Selection;
    if(state==2) steps.back().model_id=1;
    if(state==3) steps[2].project_config_change=change;
    if(state==4) steps[2].name="Selection!";
    if(state==5) steps.back().name="ordinary action";
    CHECK_FALSE(UndoRedo::record_project_config_change(steps,40,change));
    steps=history();CHECK_FALSE(UndoRedo::record_project_config_change(steps,30,change));
    REQUIRE(UndoRedo::record_project_config_change(steps,40,change));
    std::weak_ptr<const ConfigUndo::Change> lifetime=change;change.reset();
    CHECK_FALSE(lifetime.expired());
    steps.erase(steps.begin()+2,steps.end());CHECK(lifetime.expired());
    UndoRedo::Stack empty;CHECK_FALSE(empty.record_project_config_change({}));
}
