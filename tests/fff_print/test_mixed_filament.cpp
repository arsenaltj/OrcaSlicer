#include <catch2/catch_all.hpp>

#include "libslic3r/GCode/ToolOrdering.hpp"
#include "libslic3r/MultiNozzleUtils.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Slicing.hpp"
#include "slic3r/GUI/AI/Orca/OrcaPrintColorLayers.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>

#include "test_helpers.hpp"
#include "local_print_recipe_fixture.hpp"
#include "libslic3r/GCodeReader.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

// Two physical filaments plus one mixed slot (config index 2, 1-based id 3) blending them 60/40.
// The mixed arrays are parallel to filament_colour and must be sized to the filament count.
// Note ConfigOptionBools deserializes on ',' while ConfigOptionStrings uses ';'.
DynamicPrintConfig mixed_config(bool sublayer_on, const char *ratios = "0.6,0.4")
{
    DynamicPrintConfig config = multifilament_config(3);
    config.set_deserialize_strict({
        {"filament_is_mixed",               "0,0,1"},
        {"filament_mixed_components",       ";;1,2"},
        {"filament_mixed_sublayer_ratios",  std::string(";;") + ratios},
        {"filament_mixed_gradient",         "0,0,0"},
        {"filament_mixed_gradient_range",   ";;"},
        {"filament_mixed_gradient_curve",   ";;"},
        {"filament_mixed_gradient_per_part","0,0,0"},
        {"enable_mixed_color_sublayer",     sublayer_on ? "1" : "0"},
        // Assign every region role to the mixed slot so it actually participates in slicing.
        {"outer_wall_filament_id",          "3"},
        {"inner_wall_filament_id",          "3"},
        {"sparse_infill_filament_id",       "3"},
        {"internal_solid_filament_id",      "3"},
        {"top_surface_filament_id",         "3"},
        {"bottom_surface_filament_id",      "3"},
    });
    return config;
}

DynamicPrintConfig recipe_schedule_config(size_t components)
{
    auto config = multifilament_config(4);
    config.set_deserialize_strict({
        {"filament_is_mixed", "0,0,0,1"},
        {"filament_mixed_components", components == 2 ? ";;;1,3" : ";;;1,2,3"},
        {"filament_mixed_sublayer_ratios", components == 2 ? ";;;0.6,0.4" : ";;;0.2,0.3,0.5"},
        {"filament_mixed_gradient", "0,0,0,0"},
        {"filament_mixed_gradient_range", ";;;"},
        {"filament_mixed_gradient_curve", ";;;"},
        {"filament_mixed_gradient_per_part", "0,0,0,0"},
        {"enable_mixed_color_sublayer", "1"},
        {"layer_height", "0.2"}, {"initial_layer_print_height", "0.2"},
        {"print_sequence", "by layer"}, {"enable_support", "0"},
        {"enable_prime_tower", "0"}, {"skirt_loops", "0"},
        {"layer_change_gcode", "G92 E0\n"},
        {"outer_wall_filament_id", "4"}, {"inner_wall_filament_id", "4"},
        {"sparse_infill_filament_id", "4"}, {"internal_solid_filament_id", "4"},
        {"top_surface_filament_id", "4"}, {"bottom_surface_filament_id", "4"},
    });
    return config;
}

// Total sub-layer groups and per-layer mixed-filament resolutions across the whole tool ordering.
void count_mixed(ToolOrdering &to, size_t &groups, size_t &resolutions)
{
    groups = resolutions = 0;
    for (const LayerTools &lt : to.layer_tools()) {
        groups      += lt.mixed_sub_layer_groups.size();
        resolutions += lt.mixed_filament_resolution.size();
    }
}

} // namespace

TEST_CASE("confirmed recipe painting emits the promised ordered physical sublayers", "[MixedFilament][RecipeApplicationPipeline]")
{
    const size_t components=GENERATE(2u,3u);
    RecipeApplicationFixture fixture(components,1.);
    auto settings=multifilament_config(4,{
        {"layer_height","0.2"},{"initial_layer_print_height","0.2"},
        {"enable_mixed_color_sublayer","1"},{"print_sequence","by layer"},
        {"enable_support","0"},{"enable_prime_tower","0"},{"skirt_loops","0"},
        {"brim_type","no_brim"},{"layer_change_gcode",""},
        {"use_relative_e_distances","0"},{"enable_arc_fitting","0"},
        {"outer_wall_filament_id","0"},{"inner_wall_filament_id","0"},
        {"sparse_infill_filament_id","0"},{"internal_solid_filament_id","0"},
        {"top_surface_filament_id","0"},{"bottom_surface_filament_id","0"}});
    Print print;Model model;init_print({fixture.mesh},print,model,settings);
    auto* volume=model.objects.front()->volumes.front();
    GUI::LocalPrintRecipeApplication::Prepared prepared;std::string error;
    REQUIRE(GUI::LocalPrintRecipeApplication::prepare(volume->mesh(),fixture.mesh.its,fixture.result,
        fixture.snapshot,fixture.bundle,prepared,error));
    REQUIRE(prepared.target_slots==std::vector<size_t>{3,3});
    REQUIRE(GUI::LocalPrintRecipeApplication::apply_painting(*volume,std::move(prepared.painting),error));
    // Size physical material vectors through the existing test helper, then
    // apply the actual staged native project. Region-role overrides stay zero:
    // the recipe must reach slicing through the model's native painting.
    auto config=settings;
    config.apply(prepared.bundle->project_config);
    config.set_key_value("filament_settings_id",new ConfigOptionStrings(prepared.bundle->filament_presets));
    print.apply(model,config);
    const auto validation=print.validate();INFO(validation.string);REQUIRE(validation.string.empty());
    print.process();
    REQUIRE(print.objects().size()==1);
    const auto* object=print.objects().front();
    REQUIRE(object->layers().size()==5);
    const auto& recipe=*fixture.result.targets.front().recipe;
    struct Run {unsigned tool;double z,height;};
    std::vector<Run> expected;
    for(const auto* layer:object->layers()) {
        CHECK_THAT(layer->height,Catch::Matchers::WithinAbs(.2,1e-8));
        const auto& tools=print.tool_ordering().tools_for_layer(layer->print_z);
        const auto* group=tools.mixed_group_by_slot(3);
        if(layer->id()==0) {
            REQUIRE(group==nullptr);
            const auto tool=tools.resolve_mixed(3);
            REQUIRE(std::any_of(recipe.components.begin(),recipe.components.end(),
                [tool](const auto& component){return component.slot==tool;}));
            expected.push_back({tool,layer->print_z,layer->height});
        } else {
            REQUIRE(group);REQUIRE(group->components_0based.size()==components);
            const auto plan=group->object_layers.find(object);REQUIRE(plan!=group->object_layers.end());
            REQUIRE(plan->second.sub_heights.size()==components);
            double z=layer->bottom_z();
            for(size_t i=0;i<components;++i) {
                const auto& component=recipe.components[i];
                CHECK(group->components_0based[i]==component.slot);
                const double height=layer->height*component.ratio;
                CHECK_THAT(plan->second.sub_heights[i],Catch::Matchers::WithinAbs(height,1e-8));
                z+=height;expected.push_back({unsigned(component.slot),z,height});
            }
            CHECK_THAT(z,Catch::Matchers::WithinAbs(layer->print_z,1e-8));
        }
    }
    const auto gc=Slic3r::Test::gcode(print);REQUIRE_FALSE(gc.empty());
    const char* output=std::getenv("ORCA_RECIPE_PIPELINE_OUTPUT");
    std::filesystem::path prefix;
    if(output && *output) {
        const std::filesystem::path directory(output);std::filesystem::create_directories(directory);
        prefix=directory/("recipe-"+std::to_string(components));
        REQUIRE_FALSE(std::filesystem::exists(prefix.string()+".gcode"));
        std::ofstream stream(prefix.string()+".gcode");REQUIRE(stream.good());stream<<gc;
    }
    std::vector<Run> actual;
    unsigned tool=std::numeric_limits<unsigned>::max();double height=0.;std::string role;
    size_t outer_moves=0;
    GCodeReader reader;reader.apply_config(config);
    reader.parse_buffer(gc,[&](GCodeReader& state,const GCodeReader::GCodeLine& line){
        const std::string command(line.cmd()),comment(line.comment());
        if(command.size()>1 && command[0]=='T' && std::isdigit(static_cast<unsigned char>(command[1])))
            tool=unsigned(std::stoul(command.substr(1)));
        if(comment.rfind("TYPE:",0)==0) role=comment.substr(5);
        if(comment.rfind("HEIGHT:",0)==0) height=std::stod(comment.substr(7));
        if(role!="Outer wall" || !line.extruding(state) || line.dist_XY(state)<=0) return;
        const double z=line.new_Z(state);
        REQUIRE(tool<3);REQUIRE(height>0);++outer_moves;
        if(actual.empty() || actual.back().tool!=tool || std::abs(actual.back().z-z)>1e-5 ||
           std::abs(actual.back().height-height)>1e-5) actual.push_back({tool,z,height});
    });
    REQUIRE(outer_moves>0);REQUIRE(actual.size()==expected.size());
    for(size_t i=0;i<expected.size();++i) {
        INFO("ordered outer-wall run "<<i);
        CHECK(actual[i].tool==expected[i].tool);
        CHECK_THAT(actual[i].z,Catch::Matchers::WithinAbs(expected[i].z,1e-4));
        CHECK_THAT(actual[i].height,Catch::Matchers::WithinAbs(expected[i].height,1e-5));
    }
    if(!prefix.empty()) {
        nlohmann::json record={{"synthetic_capabilities",true},{"components",components},
            {"outer_wall_moves",outer_moves},{"expected",nlohmann::json::array()},{"actual",nlohmann::json::array()}};
        for(const auto& run:expected) record["expected"].push_back({{"tool",run.tool},{"z",run.z},{"height",run.height}});
        for(const auto& run:actual) record["actual"].push_back({{"tool",run.tool},{"z",run.z},{"height",run.height}});
        std::ofstream stream(prefix.string()+".json");REQUIRE(stream.good());stream<<record.dump(2);
    }
}

TEST_CASE("Logical target projection preserves distinct targets sharing one real filament", "[RecipeLayerSnapshot]")
{
    const double scale=GENERATE(1.,2.);
    auto config=multifilament_config(1);
    config.set_deserialize_strict({{"layer_height","0.2"},{"initial_layer_print_height","0.2"},
        {"enable_prime_tower","0"},{"elefant_foot_compensation","0"},{"layer_change_gcode","G92 E0\n"},
        {"outer_wall_line_width","0.4"},{"initial_layer_line_width","0.4"}});
    auto mesh=make_cube(5,5,1);const size_t first=mesh.its.indices.size();
    auto second=make_cube(5,5,1);second.translate(Vec3f(10,0,0));mesh.merge(second);
    Print original;Model model;init_print({mesh},original,model,config);
    model.objects.front()->instances.front()->set_scaling_factor(Vec3d(1,1,scale));
    model.objects.front()->instances.front()->set_rotation(Vec3d(0,0,0.3));
    auto* volume=model.objects.front()->volumes.front();
    std::vector<size_t> labels(mesh.its.indices.size(),1);std::fill(labels.begin(),labels.begin()+first,0);
    std::atomic<bool> cancelled{false};
    const auto identity=GUI::OrcaPrintColorLayers::identity(*model.objects.front(),config);
    auto logical=GUI::OrcaPrintColorTargets::partition(*volume,labels,{size_t(0),size_t(0)},cancelled);
    const auto record=GUI::OrcaPrintColorLayers::inspect(model,config,cancelled,Vec3d::Zero(),0,&logical);
    const auto& projection=record.at("objects").at(0).at("logical_projection");
    REQUIRE(projection.at("target_count")==2);
    REQUIRE(projection.at("source_face_counts")==std::vector<size_t>{first,mesh.its.indices.size()-first});
    REQUIRE(projection.at("region_layer_count")==size_t(10*scale));
    REQUIRE_FALSE(projection.at("recipe_feasibility_verified").get<bool>());
    REQUIRE(projection.at("layers").size()==size_t(5*scale));
    for(const auto& layer:projection.at("layers")) {
        REQUIRE(layer.at("targets").size()==2);
        CHECK_THAT(layer.at("projected_overlap_mm2").get<double>(),Catch::Matchers::WithinAbs(0.,1e-6));
        CHECK_THAT(layer.at("model_without_projected_target_mm2").get<double>(),Catch::Matchers::WithinAbs(0.,0.001));
        for(size_t target=0;target<2;++target) {
            const auto& row=layer.at("targets").at(target);
            REQUIRE(row.at("target_index")==target);
            REQUIRE(row.at("native_regions").size()==1);
            const auto& region=row.at("native_regions").at(0);
            CHECK_THAT(region.at("area_mm2").get<double>(),Catch::Matchers::WithinAbs(25.,0.002));
            CHECK(region.at("native_external_wall_filament_id_1based")==1);
            CHECK(region.at("direct_slot_matches_native_wall").get<bool>());
        }
    }
    CHECK(config.option<ConfigOptionStrings>("filament_colour")->values.size()==1);
    CHECK(original.objects().front()->layers().empty());
    CHECK(GUI::OrcaPrintColorLayers::identity(*model.objects.front(),config)==identity);
    cancelled=true;
    CHECK_THROWS(GUI::OrcaPrintColorTargets::partition(*volume,labels,{size_t(0),size_t(0)},cancelled));
    cancelled=false;labels.front()=2;
    CHECK_THROWS(GUI::OrcaPrintColorTargets::partition(*volume,labels,{size_t(0),size_t(0)},cancelled));
}

TEST_CASE("Region evidence retains a zero-area contact without declaring printable material", "[RecipeLayerSnapshot]")
{
    ExPolygon contact(Polygon({Point(0,0),Point(scale_(1.),0.),Point(scale_(2.),0.)}));
    std::atomic<bool> cancel{false};size_t budget=10;
    const auto record=GUI::OrcaPrintColorRegions::geometry({contact},0.4,cancel,budget);
    REQUIRE(record.at("polygons").size()==1);
    CHECK(record.at("zero_area_islands")==1);
    CHECK(record.at("polygons").at(0).at("zero_area").get<bool>());
    CHECK_FALSE(record.at("polygons").at(0).at("inset_core_exists").get<bool>());
    CHECK_THAT(record.at("area_mm2").get<double>(),Catch::Matchers::WithinAbs(0.,1e-15));
    CHECK(budget==7);
}

TEST_CASE("Isolated color layer inspection retains real occupancy and leaves inputs unchanged", "[RecipeLayerSnapshot]")
{
    const bool gap=GENERATE(false,true);
    auto config=recipe_schedule_config(2);
    auto mesh=make_cube(10,10,gap ? 0.4 : 1.4);
    if(gap){auto upper=make_cube(10,10,0.4);upper.translate(Vec3f(0,0,1.f));mesh.merge(upper);}
    Print source_print;Model model;
    init_print({mesh},source_print,model,config);
    const auto before=GUI::OrcaPrintColorLayers::identity(*model.objects.front(),config);
    std::atomic<bool> cancelled{false};
    const auto result=GUI::OrcaPrintColorLayers::inspect(model,config,cancelled);
    REQUIRE(result.at("layer_count").get<size_t>()==7);
    REQUIRE(result.at("occupied_layer_count").get<size_t>()==(gap ? 4 : 7));
    REQUIRE_FALSE(result.at("final_tool_assignment_verified").get<bool>());
    REQUIRE_FALSE(result.at("region_feature_size_verified").get<bool>());
    REQUIRE(GUI::OrcaPrintColorLayers::identity(*model.objects.front(),config)==before);
    REQUIRE(source_print.objects().front()->layers().empty());
    cancelled=true;
    REQUIRE_THROWS(GUI::OrcaPrintColorLayers::inspect(model,config,cancelled));
}

TEST_CASE("Color layer input identity follows object overrides and transformations", "[RecipeLayerSnapshot]")
{
    auto config=recipe_schedule_config(2);Print print;Model model;
    init_print({make_cube(10,10,1.4)},print,model,config);
    auto* object=model.objects.front();
    auto previous=GUI::OrcaPrintColorLayers::identity(*object,config);
    auto changed=[&]{auto current=GUI::OrcaPrintColorLayers::identity(*object,config);REQUIRE(current!=previous);previous=current;};
    object->config.set_key_value("layer_height",new ConfigOptionFloat(0.3));changed();
    object->instances.front()->set_scaling_factor(Vec3d(1.,1.,1.5));changed();
    object->layer_config_ranges[{0.6,1.4}].set_key_value("layer_height",new ConfigOptionFloat(0.1));changed();
    object->layer_height_profile.set({0.,0.2,1.4,0.2});changed();
    config.set_key_value("initial_layer_print_height",new ConfigOptionFloat(0.25));changed();
}

TEST_CASE("Region clearance preserves holes and rejects wide boxes with thin material", "[RecipeLayerSnapshot]")
{
    auto rectangle=[](double x,double y,double w,double h) {
        return Polygon(Points{Point(scale_(x),scale_(y)),Point(scale_(x+w),scale_(y)),
            Point(scale_(x+w),scale_(y+h)),Point(scale_(x),scale_(y+h))});
    };
    ExPolygon frame(rectangle(0,0,10,10));
    auto hole=rectangle(.1,.1,9.8,9.8);hole.reverse();frame.holes.push_back(hole);
    ExPolygons polygons{frame,ExPolygon(rectangle(20,0,20,.1)),ExPolygon(rectangle(50,0,2,2))};
    std::atomic<bool> cancelled{false};size_t budget=100;
    const auto result=GUI::OrcaPrintColorRegions::geometry(polygons,.4,cancelled,budget);
    REQUIRE(result.at("polygons").size()==3);
    REQUIRE(result.at("polygons")[0].at("holes").size()==1);
    REQUIRE(result.at("islands_without_inset_core").get<int>()==2);
    REQUIRE_THAT(result.at("area_mm2").get<double>(),Catch::Matchers::WithinAbs(9.96,1e-6));
    REQUIRE(result.at("polygons")[2].at("inset_core_exists").get<bool>());
    REQUIRE(budget==84);
    budget=3;
    REQUIRE_THROWS(GUI::OrcaPrintColorRegions::geometry(polygons,.4,cancelled,budget));
    budget=100;cancelled=true;
    REQUIRE_THROWS(GUI::OrcaPrintColorRegions::geometry(polygons,.4,cancelled,budget));
}

TEST_CASE("Layer region evidence separates native material areas and role filaments", "[RecipeLayerSnapshot]")
{
    auto config=multifilament_config(2,{{"layer_height","0.2"},{"initial_layer_print_height","0.2"},
        {"outer_wall_line_width","0.4"},{"initial_layer_line_width","0.4"},
        {"elefant_foot_compensation","0"},{"enable_prime_tower","0"},{"layer_change_gcode","G92 E0\n"}});
    Print source;Model model;init_print({make_cube(5,10,1)},source,model,config);
    auto* object=model.objects.front();object->volumes.front()->config.set_key_value("extruder",new ConfigOptionInt(1));
    auto right=make_cube(5,10,1);right.translate(Vec3f(5,0,0));
    object->add_volume(right)->config.set_key_value("extruder",new ConfigOptionInt(2));
    std::atomic<bool> cancelled{false};
    const auto result=GUI::OrcaPrintColorLayers::inspect(model,config,cancelled);
    REQUIRE_FALSE(result.at("logical_target_mapping_verified").get<bool>());
    REQUIRE(result.at("region_layer_count").get<size_t>()==10);
    for(const auto& layer:result.at("objects")[0].at("layers")) {
        REQUIRE(layer.at("regions").size()==2);
        std::set<unsigned> filaments;double area=0;
        for(const auto& region:layer.at("regions")) {
            filaments.insert(region.at("role_filament_ids_1based").at("external_wall").get<unsigned>());
            area+=region.at("area_mm2").get<double>();
            REQUIRE_THAT(region.at("external_wall_width_mm").get<double>(),Catch::Matchers::WithinAbs(.4,1e-6));
        }
        REQUIRE(filaments==std::set<unsigned>{1,2});
        REQUIRE_THAT(area,Catch::Matchers::WithinAbs(100.,1e-4));
    }
    REQUIRE(source.get_object(0)->layers().empty());
}

TEST_CASE("Color layer snapshots preserve referenced model material overrides", "[RecipeLayerSnapshot]")
{
    auto config=recipe_schedule_config(2);Print print;Model model;
    init_print({make_cube(10,10,1.4)},print,model,config);
    auto* object=model.objects.front();
    object->volumes.front()->set_material_id("surface");
    auto* material=object->volumes.front()->material();
    REQUIRE(material!=nullptr);
    material->config.set_key_value("wall_loops",new ConfigOptionInt(3));
    const auto before=GUI::OrcaPrintColorLayers::identity(*object,config);
    const auto snapshot=GUI::OrcaPrintColorLayers::snapshot_model(*object);
    REQUIRE(snapshot->objects.front()->volumes.front()->material()!=nullptr);
    REQUIRE(snapshot->objects.front()->volumes.front()->material()!=material);
    REQUIRE(snapshot->objects.front()->volumes.front()->material()->config.opt_int("wall_loops")==3);
    std::atomic<bool> cancelled{false};
    REQUIRE(GUI::OrcaPrintColorLayers::inspect(*snapshot,config,cancelled).at("layer_count").get<size_t>()==7);
    material->config.set_key_value("wall_loops",new ConfigOptionInt(4));
    REQUIRE(GUI::OrcaPrintColorLayers::identity(*object,config)!=before);
    REQUIRE(snapshot->objects.front()->volumes.front()->material()->config.opt_int("wall_loops")==3);
}

// Compare pre-slice layer proposals with real geometry-bearing layers before
// deciding which native data can support a local color recipe. Capturing a
// proposal is not proof that its layer contains the target colored surface.
TEST_CASE("Recipe layer context exposes proposal and actual geometry separately", "[MixedFilament][RecipeLayerContextEvidence][.]")
{
    const char* output = std::getenv("ORCASLICER_RECIPE_LAYER_EVIDENCE_DIR");
    if (!output || !*output) SKIP("Set ORCASLICER_RECIPE_LAYER_EVIDENCE_DIR to an empty local directory.");
    const std::filesystem::path root(output);
    REQUIRE(std::filesystem::is_directory(root));
    for (const std::string scenario : {"uniform", "object_override", "height_range", "variable_profile", "precise_top", "scaled", "vertical_gap"}) {
        INFO(scenario);
        const auto path = root / (scenario + ".tsv");
        REQUIRE_FALSE(std::filesystem::exists(path));
        auto config = recipe_schedule_config(2);
        config.set_deserialize_strict({{"precise_z_height", scenario == "precise_top" ? "1" : "0"}});
        auto mesh = make_cube(10, 10, scenario == "precise_top" ? 1.37 : 1.4);
        if (scenario == "vertical_gap") {
            mesh = make_cube(10,10,0.4);
            auto upper = make_cube(10,10,0.4);
            upper.translate(Vec3f(0,0,1.f));
            mesh.merge(upper);
        }
        Print print; Model model;
        init_print({mesh}, print, model, config);
        auto* source = model.objects.front();
        if (scenario == "object_override") source->config.set_key_value("layer_height", new ConfigOptionFloat(0.3));
        if (scenario == "height_range") source->layer_config_ranges[{0.6,1.4}].set_key_value("layer_height", new ConfigOptionFloat(0.1));
        if (scenario == "variable_profile") source->layer_height_profile.set({0.,0.2,0.2,0.2,0.6,0.1,1.,0.3,1.4,0.2});
        if (scenario == "scaled") source->instances.front()->set_scaling_factor(Vec3d(1.,1.,1.5));
        source->invalidate_bounding_box();
        print.apply(model, config);
        REQUIRE(print.objects().size() == 1);
        auto* object = print.get_object(0);
        const auto live_params = object->slicing_parameters();
        REQUIRE(live_params.valid);
        std::vector<coordf_t> live_profile;
        PrintObject::update_layer_height_profile(*object->model_object(),live_params,live_profile);
        REQUIRE_FALSE(live_profile.empty());
        const auto proposal = generate_object_layers(live_params,live_profile,object->config().precise_z_height.value);
        const auto helper_params = PrintObject::slicing_parameters(config,*source,0.f,Vec3d::Ones());
        std::vector<coordf_t> helper_profile;
        PrintObject::update_layer_height_profile(*source,helper_params,helper_profile);
        const auto helper = generate_object_layers(helper_params,helper_profile,object->config().precise_z_height.value);
        object->slice();
        REQUIRE_FALSE(object->layers().empty());
        std::ofstream table(path);
        REQUIRE(table.good());
        table << std::setprecision(17) << "source\tlayer\tbottom_z\ttop_z\theight\thas_geometry\n";
        auto write_proposal = [&](const char* name,const auto& heights,double zmin) {
            REQUIRE(heights.size() % 2 == 0);
            for (size_t i=0;i<heights.size();i+=2)
                table << name << '\t' << i/2 << '\t' << heights[i]+zmin << '\t' << heights[i+1]+zmin << '\t' << heights[i+1]-heights[i] << "\t-1\n";
        };
        write_proposal("applied_print",proposal,live_params.object_print_z_min);
        write_proposal("static_helper",helper,helper_params.object_print_z_min);
        for (const auto* layer : object->layers()) {
            REQUIRE(layer->height > 0.);
            table << "actual_slice\t" << layer->id() << '\t' << layer->bottom_z() << '\t' << layer->print_z << '\t' << layer->height << '\t' << !layer->empty() << '\n';
        }
        table.close();
        REQUIRE(table.good());
    }
}

// Opt-in native evidence for connecting local color recipes to actual sliced
// object layers. This is synthetic offline input, not a calibrated print.
// Keep the raw ToolOrdering and G-code outputs even if a separate audit finds
// a mismatch; successful capture must not be confused with schedule validity.
TEST_CASE("Mixed recipe schedules expose actual object layer heights for audit", "[MixedFilament][MixedRecipeScheduleEvidence][.]")
{
    const char* output = std::getenv("ORCASLICER_MIXED_RECIPE_EVIDENCE_DIR");
    if (!output || !*output) SKIP("Set ORCASLICER_MIXED_RECIPE_EVIDENCE_DIR to an empty local evidence directory.");
    const std::filesystem::path root(output);
    REQUIRE(std::filesystem::is_directory(root));
    for (const size_t components : {size_t(2), size_t(3)}) {
        for (const bool different_heights : {false, true}) {
            const auto prefix = root / (std::to_string(components) + (different_heights ? "-different" : "-uniform"));
            REQUIRE_FALSE(std::filesystem::exists(prefix.string() + ".tsv"));
            REQUIRE_FALSE(std::filesystem::exists(prefix.string() + ".gcode"));
            auto config = recipe_schedule_config(components);
            std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides {
                {{"layer_height", "0.2"}}, {{"layer_height", different_heights ? "0.3" : "0.2"}}
            };
            Print print; Model model;
            init_print(std::vector<TriangleMesh>{make_cube(10,10,1.4),make_cube(10,10,1.4)}, print, model, config, &overrides);
            REQUIRE(print.objects().size() == 2);
            const auto validation = print.validate();
            INFO(validation.string);
            REQUIRE(validation.string.empty());
            print.process();
            std::ofstream table(prefix.string() + ".tsv");
            REQUIRE(table.good());
            table << std::setprecision(17) << "object\tlayer\tz\tactual_height\tgroup_height\tcomponent\tsubheight\tresolved\n";
            size_t object_index = 0, rows = 0;
            for (const auto* object : print.objects()) {
                for (const auto* layer : object->layers()) {
                    const auto& tools = print.tool_ordering().tools_for_layer(layer->print_z);
                    REQUIRE_THAT(tools.print_z, Catch::Matchers::WithinAbs(layer->print_z, 1e-6));
                    const auto* group = tools.mixed_group_by_slot(3);
                    if (group) {
                        REQUIRE(group->components_0based.size() == components);
                        const auto plan = group->object_layers.find(object);
                        REQUIRE(plan != group->object_layers.end());
                        REQUIRE(plan->second.sub_heights.size() == components);
                        for (size_t i=0;i<components;++i) {
                            table << object_index << '\t' << layer->id() << '\t' << layer->print_z << '\t' << layer->height << '\t'
                                << plan->second.layer_height << '\t' << group->components_0based[i] << '\t' << plan->second.sub_heights[i] << "\t-1\n";
                            ++rows;
                        }
                    } else {
                        table << object_index << '\t' << layer->id() << '\t' << layer->print_z << '\t' << layer->height
                            << "\t0\t-1\t0\t" << tools.resolve_mixed(3) << '\n';
                        ++rows;
                    }
                }
                ++object_index;
            }
            REQUIRE(rows > 0);
            table.close();
            const auto gc = Slic3r::Test::gcode(print);
            REQUIRE_FALSE(gc.empty());
            std::ofstream gcode_file(prefix.string() + ".gcode");
            REQUIRE(gcode_file.good());
            gcode_file << gc;
        }
    }
}

TEST_CASE("Shared mixed slots keep each object's sliced layer thickness", "[MixedFilament][Regression]")
{
    const size_t components = GENERATE(size_t(2), size_t(3));
    const bool different_heights = GENERATE(false, true);
    const bool gradient = GENERATE(false, true);
    if (gradient && components == 3) return; // Native gradients have two components.
    auto config = recipe_schedule_config(components);
    if (gradient) config.set_deserialize_strict({
        {"filament_mixed_gradient", "0,0,0,1"},
        {"filament_mixed_gradient_range", ";;;0.8,0.2"}
    });
    std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides {
        {{"layer_height", "0.2"}}, {{"layer_height", different_heights ? "0.3" : "0.2"}}
    };
    Print print; Model model;
    init_print(std::vector<TriangleMesh>{make_cube(10,10,1.4),make_cube(10,10,1.4)}, print, model, config, &overrides);
    const auto validation = print.validate();
    INFO(validation.string);
    REQUIRE(validation.string.empty());
    print.process();
    REQUIRE(print.objects().size() == 2);
    size_t split_layers = 0;
    for (const auto* object : print.objects()) {
        for (const auto* layer : object->layers()) {
            const auto& tools = print.tool_ordering().tools_for_layer(layer->print_z);
            const auto* group = tools.mixed_group_by_slot(3);
            if (layer->id() == 0) {
                REQUIRE(group == nullptr);
                REQUIRE(tools.resolve_mixed(3) < 3);
                continue;
            }
            REQUIRE(group != nullptr);
            const auto plan = group->object_layers.find(object);
            REQUIRE(plan != group->object_layers.end());
            REQUIRE_THAT(plan->second.layer_height, Catch::Matchers::WithinAbs(layer->height, 1e-8));
            REQUIRE_THAT(plan->second.bottom_z(), Catch::Matchers::WithinAbs(layer->bottom_z(), 1e-8));
            REQUIRE(plan->second.sub_heights.size() == components);
            double sum = 0;
            for (double sub_height : plan->second.sub_heights) {
                REQUIRE(sub_height > 0.);
                sum += sub_height;
            }
            REQUIRE_THAT(sum, Catch::Matchers::WithinAbs(layer->height, 1e-8));
            ++split_layers;
        }
    }
    REQUIRE(split_layers > 0);
}

TEST_CASE("enable_mixed_color_sublayer reaches the Print config", "[MixedFilament]")
{
    Print print;
    Model model;
    init_print({cube(20)}, print, model, mixed_config(true));

    // The option lives in PrintConfig; if it did not survive Print::apply the slicer would
    // silently fall back to the whole-layer path.
    CHECK(print.config().enable_mixed_color_sublayer.value == true);
    REQUIRE(print.config().filament_is_mixed.values.size() == 3);
    CHECK(print.config().filament_is_mixed.values[2] == true);
    REQUIRE(print.config().filament_mixed_components.values.size() == 3);
    CHECK(print.config().filament_mixed_components.values[2] == "1,2");
}

TEST_CASE("Mixed filament splits layers into sub-layers when the option is on", "[MixedFilament]")
{
    Print print;
    Model model;
    init_print({cube(20)}, print, model, mixed_config(true));
    print.process();

    ToolOrdering &to = const_cast<ToolOrdering &>(print.tool_ordering());
    REQUIRE(!to.layer_tools().empty());

    size_t groups = 0, resolutions = 0;
    count_mixed(to, groups, resolutions);

    INFO("layers=" << to.layer_tools().size() << " groups=" << groups);
    CHECK(groups > 0);
}

TEST_CASE("Mixed filament alternates whole layers when the option is off", "[MixedFilament]")
{
    Print print;
    Model model;
    init_print({cube(20)}, print, model, mixed_config(false));
    print.process();

    ToolOrdering &to = const_cast<ToolOrdering &>(print.tool_ordering());
    REQUIRE(!to.layer_tools().empty());

    size_t groups = 0, resolutions = 0;
    count_mixed(to, groups, resolutions);

    // With splitting off the slot is realized by the deficit round-robin scheduler instead:
    // no sub-layer groups, but a per-layer resolution to one physical component.
    INFO("layers=" << to.layer_tools().size() << " resolutions=" << resolutions);
    CHECK(groups == 0);
    CHECK(resolutions > 0);
}

TEST_CASE("Sub-layer splitting emits the scaled sub-heights into G-code", "[MixedFilament]")
{
    // layer_height 0.2 split 60/40 gives sub-layers of 0.12 and 0.08. The emitter reports the
    // sub-height (not the nominal layer height) in the HEIGHT tag and scales flow to match.
    DynamicPrintConfig config = mixed_config(true);
    config.set_deserialize_strict({{"layer_height", "0.2"}, {"initial_layer_print_height", "0.2"}});

    Print print;
    Model model;
    init_print({cube(20)}, print, model, config);
    print.process();
    const std::string gc = Slic3r::Test::gcode(print);

    REQUIRE(!gc.empty());
    INFO("gcode bytes=" << gc.size());
    CHECK(gc.find(";HEIGHT:0.12") != std::string::npos);
    CHECK(gc.find(";HEIGHT:0.08") != std::string::npos);
}

TEST_CASE("Whole-layer mixing emits only the nominal layer height", "[MixedFilament]")
{
    DynamicPrintConfig config = mixed_config(false);
    config.set_deserialize_strict({{"layer_height", "0.2"}, {"initial_layer_print_height", "0.2"}});

    Print print;
    Model model;
    init_print({cube(20)}, print, model, config);
    print.process();
    const std::string gc = Slic3r::Test::gcode(print);

    REQUIRE(!gc.empty());
    // No sub-layer split, so the 60/40 sub-heights must never appear.
    CHECK(gc.find(";HEIGHT:0.12") == std::string::npos);
    CHECK(gc.find(";HEIGHT:0.08") == std::string::npos);
}

TEST_CASE("By-object prints without mixed filaments keep their used-filament set", "[MixedFilament]")
{
    // With no mixed slot the by-object bookkeeping stays plain: object 2 prints with filament 2,
    // so both filaments are used and no mixed filament is reported.
    DynamicPrintConfig config = multifilament_config(2, {{"print_sequence", "by object"}});
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{ {}, { {"extruder", "2"} } };

    Print print;
    Model model;
    init_print(std::vector<TriangleMesh>{cube(20), cube(20)}, print, model, config, &overrides);
    REQUIRE(print.objects().size() == 2);
    print.process();

    CHECK(print.get_slice_used_filaments(false) == std::vector<unsigned int>{0, 1});
    CHECK(print.get_slice_used_filaments(true) == std::vector<unsigned int>{0, 1});
    CHECK(print.get_slice_used_mixed_filaments().empty());
}

TEST_CASE("By-layer prints record a mixed slot's components and the slot itself", "[MixedFilament]")
{
    // Control for the by-object case below: the by-layer path publishes the physical
    // components (0-based 0 and 1) as used filaments and the mixed slot (config index 2) as
    // a used mixed filament. By-object prints must report exactly the same.
    Print print;
    Model model;
    init_print({cube(20)}, print, model, mixed_config(false));
    print.process();

    CHECK(print.get_slice_used_filaments(false) == std::vector<unsigned int>{0, 1});
    CHECK(print.get_slice_used_mixed_filaments() == std::vector<unsigned int>{2});
}

TEST_CASE("By-object prints expand a mixed slot to its components in the slice bookkeeping", "[MixedFilament]")
{
    // Sequential prints build their filament lists from unsorted per-object orderings, which
    // still carry the virtual slot (config index 2). The slice-used sets and the published
    // grouping result must see the physical components 0 and 1 instead, and the slot itself
    // must still be reported as a used mixed filament — exactly what the by-layer path yields.
    DynamicPrintConfig config = mixed_config(false);
    config.set_deserialize_strict({{"print_sequence", "by object"}});

    Print print;
    Model model;
    init_print({cube(20), cube(20)}, print, model, config);
    REQUIRE(print.objects().size() == 2);
    print.process();

    const std::vector<unsigned int> components{0, 1};
    CHECK(print.get_slice_used_filaments(false) == components);
    CHECK(print.get_slice_used_filaments(true) == components);
    CHECK(print.get_slice_used_mixed_filaments() == std::vector<unsigned int>{2});

    auto group_result = print.get_layered_nozzle_group_result();
    REQUIRE(group_result != nullptr);
    CHECK(group_result->get_used_filaments() == components);
}

TEST_CASE("By-object G-code lists a mixed slot's components in the filament header", "[MixedFilament]")
{
    DynamicPrintConfig config = mixed_config(false);
    config.set_deserialize_strict({{"print_sequence", "by object"}});

    Print print;
    Model model;
    init_print({cube(20), cube(20)}, print, model, config);
    const std::string gc = Slic3r::Test::gcode(print);

    REQUIRE(!gc.empty());
    // The header names the filaments that must be loaded (components 1 and 2, 1-based),
    // never the virtual slot 3.
    CHECK(gc.find("; filament: 1,2\n") != std::string::npos);
    CHECK(gc.find("; filament: 3") == std::string::npos);
}

TEST_CASE("Print::validate rejects a mixed filament as the wipe tower filament", "[MixedFilament]")
{
    // The validate backstop refuses a mixed (virtual) slot as the wipe tower filament; the GUI hides
    // the slot from that option. Two cubes on physical filaments 1 and 2 make the tower real, and the
    // region roles mixed_config() points at the slot are reset so only the tower uses it.
    DynamicPrintConfig config = mixed_config(false);
    config.set_deserialize_strict({
        {"enable_prime_tower",         "1"},
        {"wipe_tower_x",               "50"}, // inside the 200x200 test bed
        {"wipe_tower_y",               "50"}, // (the default y, 220, is not)
        {"layer_change_gcode",         "G92 E0\n"}, // validate() relative-E reset, as in test_print.cpp's build_cubes
        {"outer_wall_filament_id",     "0"},
        {"inner_wall_filament_id",     "0"},
        {"sparse_infill_filament_id",  "0"},
        {"internal_solid_filament_id", "0"},
        {"top_surface_filament_id",    "0"},
        {"bottom_surface_filament_id", "0"},
    });
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{ { {"extruder", "1"} }, { {"extruder", "2"} } };

    SECTION("a physical wipe tower filament validates") {
        config.set_deserialize_strict({{"wipe_tower_filament", "2"}});
        Print print;
        Model model;
        init_print(std::vector<TriangleMesh>{cube(20), cube(20)}, print, model, config, &overrides);
        REQUIRE(print.has_wipe_tower());
        const StringObjectException err = print.validate();
        INFO(err.string);
        CHECK(err.string.empty());
    }

    SECTION("the mixed slot is refused") {
        config.set_deserialize_strict({{"wipe_tower_filament", "3"}});
        Print print;
        Model model;
        init_print(std::vector<TriangleMesh>{cube(20), cube(20)}, print, model, config, &overrides);
        REQUIRE(print.has_wipe_tower());
        const StringObjectException err = print.validate();
        CHECK_FALSE(err.string.empty());
        CHECK(err.opt_key == "wipe_tower_filament");
    }
}

TEST_CASE("Print::validate warns when a gradient mixed filament is used without sublayer mixing", "[MixedFilament]")
{
    // A gradient mixed filament only renders its gradient with the process option enabled; without
    // it ToolOrdering prints one whole component per layer and the gradient is dropped silently,
    // so validate() warns whenever the slot actually takes part in the print. The layer-change
    // reset avoids an unrelated relative-extrusion warning, as in the wipe tower test above.
    DynamicPrintConfig config = mixed_config(false);
    config.set_deserialize_strict({
        {"filament_mixed_gradient", "0,0,1"},
        {"layer_change_gcode",       "G92 E0\n"},
    });

    auto count_opt = [](Print &print, const char *opt_key) {
        std::vector<StringObjectException> warnings;
        print.validate(&warnings);
        return std::count_if(warnings.begin(), warnings.end(),
                             [&](const StringObjectException &w) { return w.opt_key == opt_key; });
    };

    SECTION("gradient slot used, sublayer mixing off") {
        Print print;
        Model model;
        init_print({cube(20)}, print, model, config);
        std::vector<StringObjectException> warnings;
        const StringObjectException err = print.validate(&warnings);
        CHECK(err.string.empty());
        const auto it = std::find_if(warnings.begin(), warnings.end(), [](const StringObjectException &w) {
            return w.opt_key == "enable_mixed_color_sublayer";
        });
        REQUIRE(it != warnings.end());
        CHECK(it->is_warning);
        CHECK(std::count_if(warnings.begin(), warnings.end(), [](const StringObjectException &w) {
                  return w.opt_key == "enable_mixed_color_sublayer";
              }) == 1);
    }

    SECTION("sublayer mixing on") {
        config.set_deserialize_strict({{"enable_mixed_color_sublayer", "1"}});
        Print print;
        Model model;
        init_print({cube(20)}, print, model, config);
        CHECK(count_opt(print, "enable_mixed_color_sublayer") == 0);
    }

    SECTION("gradient flag off") {
        config.set_deserialize_strict({{"filament_mixed_gradient", "0,0,0"}});
        Print print;
        Model model;
        init_print({cube(20)}, print, model, config);
        CHECK(count_opt(print, "enable_mixed_color_sublayer") == 0);
    }

    SECTION("mixed slot not used") {
        config.set_deserialize_strict({
            {"outer_wall_filament_id",     "0"},
            {"inner_wall_filament_id",     "0"},
            {"sparse_infill_filament_id",  "0"},
            {"internal_solid_filament_id", "0"},
            {"top_surface_filament_id",    "0"},
            {"bottom_surface_filament_id", "0"},
        });
        Print print;
        Model model;
        const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{{{ "extruder", "1" }}};
        init_print(std::vector<TriangleMesh>{cube(20)}, print, model, config, &overrides);
        CHECK(count_opt(print, "enable_mixed_color_sublayer") == 0);
    }
}
