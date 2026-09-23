#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "slic3r/GUI/AI/ModelGeneration/LocalPrintColorRecipes.hpp"
#include "slic3r/GUI/AI/Orca/OrcaPrintColorProcess.hpp"
#include "slic3r/GUI/AI/Model/LocalPrintColorState.hpp"
#include <set>

using namespace Slic3r;
namespace Recipes = GUI::LocalPrintColorRecipes;

TEST_CASE("workspace recipe input uses native profile values without inventing missing surface constraints", "[LocalPrintColorRecipes]")
{
    AI::PrintablePaletteSnapshot snapshot;
    snapshot.material_fingerprint = "materials"; snapshot.process_fingerprint = "process";
    snapshot.material_metadata_complete = true;
    snapshot.physical_channels = {{2, "#000000", "PLA", true}, {5, "#FFFFFF", "PLA", true}};
    DynamicPrintConfig printer, print, black, white;
    printer.set_key_value("nozzle_diameter", new ConfigOptionFloats{.4});
    printer.set_key_value("min_layer_height", new ConfigOptionFloats{.08});
    printer.set_key_value("max_layer_height", new ConfigOptionFloats{.3});
    print.set_key_value("layer_height", new ConfigOptionFloat(.2));
    print.set_key_value("line_width", new ConfigOptionFloatOrPercent(110, true));
    print.set_key_value("enable_mixed_color_sublayer", new ConfigOptionBool(true));
    for (auto* config : {&black, &white}) {
        config->set_key_value("nozzle_temperature", new ConfigOptionInts{215});
        config->set_key_value("nozzle_temperature_range_low", new ConfigOptionInts{190});
        config->set_key_value("nozzle_temperature_range_high", new ConfigOptionInts{230});
    }
    GUI::capture_print_color_process(snapshot, printer, print, {&black, &white}, {"black-profile", "white-profile"});
    auto input = Recipes::from_workspace(snapshot);
    REQUIRE(input.materials.size() == 2);
    CHECK(input.materials[0].channel.slot == 2);
    CHECK(input.materials[1].channel.slot == 5);
    CHECK(input.materials[1].identity == "white-profile");
    CHECK_THAT(input.materials[0].line_width_mm, Catch::Matchers::WithinAbs(.44, 1e-12));
    CHECK_THAT(input.materials[1].min_layer_mm, Catch::Matchers::WithinAbs(.08, 1e-12));
    CHECK_THAT(input.materials[1].temperature_c, Catch::Matchers::WithinAbs(215, 1e-12));
    REQUIRE(input.process.layer_heights_mm.size() == 1);
    CHECK_THAT(input.process.layer_heights_mm.front(), Catch::Matchers::WithinAbs(.2, 1e-12));
    CHECK(input.process.sublayers_enabled);
    CHECK(input.process.first_layer_unsplit);
    CHECK(input.process.surface_condition.empty());
    CHECK_THAT(input.process.z_resolution_mm, Catch::Matchers::WithinAbs(0, 1e-12));
    CHECK_THAT(input.process.region_width_mm, Catch::Matchers::WithinAbs(0, 1e-12));
    CHECK_FALSE(Recipes::enumerate(input).ok());
    // Only this fixture supplies synthetic missing evidence, never the adapter.
    input.process.surface_condition = "test-wall"; input.process.measurement_condition = "test-D65";
    input.process.z_resolution_mm = .01; input.process.region_width_mm = 2; input.process.region_height_mm = 1;
    const auto catalog = Recipes::enumerate(input);
    REQUIRE(catalog.ok());
    REQUIRE_FALSE(catalog.candidates.empty());
    for (const auto& candidate : catalog.candidates)
        for (double height : candidate.sublayer_heights_mm.front()) CHECK(height >= .08 - 1e-12);
    // Captured data is immutable; subsequent profile edits only affect recapture.
    print.set_key_value("layer_height", new ConfigOptionFloat(.12));
    CHECK_THAT(input.process.layer_heights_mm.front(), Catch::Matchers::WithinAbs(.2, 1e-12));
    snapshot.process_fingerprint = "process-updated";
    CHECK(Recipes::from_workspace(snapshot).materials.empty());
    GUI::capture_print_color_process(snapshot, printer, print, {&black, &white}, {"black-profile", "white-profile"});
    auto thinner = Recipes::from_workspace(snapshot);
    thinner.process.surface_condition = input.process.surface_condition;
    thinner.process.measurement_condition = input.process.measurement_condition;
    thinner.process.z_resolution_mm = .01; thinner.process.region_width_mm = 2; thinner.process.region_height_mm = 1;
    REQUIRE(Recipes::enumerate(thinner).ok());
    CHECK(Recipes::enumerate(thinner).candidates.empty());
    snapshot.physical_channels[1].display_color = "#FF0000";
    CHECK(Recipes::from_workspace(snapshot).materials.empty());
    // Multi-nozzle assignment and auto-width stay unknown until resolved.
    printer.set_key_value("nozzle_diameter", new ConfigOptionFloats{.4, .6});
    print.set_key_value("line_width", new ConfigOptionFloatOrPercent(0, false));
    GUI::capture_print_color_process(snapshot, printer, print, {&black, nullptr}, {"black-profile", ""});
    CHECK_THAT(snapshot.sublayer_materials[0].min_layer_mm, Catch::Matchers::WithinAbs(0, 1e-12));
    CHECK_THAT(snapshot.sublayer_materials[0].line_width_mm, Catch::Matchers::WithinAbs(0, 1e-12));
    CHECK_THAT(snapshot.sublayer_materials[1].temperature_c, Catch::Matchers::WithinAbs(0, 1e-12));
    CHECK(snapshot.sublayer_materials[1].identity.empty());
}

static Recipes::Input recipe_input(size_t count = 3)
{
    Recipes::Input input;
    input.process.material_fingerprint = "six-material-test-fixture";
    input.process.process_fingerprint = "sublayer-test-fixture";
    input.process.surface_condition = "vertical-wall-coupon";
    input.process.measurement_condition = "test-D65-2deg";
    input.process.sublayers_enabled = true;
    input.process.layer_heights_mm = {.20};
    input.process.z_resolution_mm = .01;
    input.process.region_width_mm = 2;
    input.process.region_height_mm = 1;
    const std::vector<std::string> colors = {"#000000", "#FFFFFF", "#FF0000", "#FFFF00", "#00FF00", "#0000FF"};
    for (size_t i = 0; i < count; ++i) {
        Recipes::Material m;
        m.channel = {2*i, colors[i], "PLA", true}; // Physical IDs need not be contiguous.
        m.identity = "fixture-profile-batch-" + std::to_string(i);
        m.min_layer_mm = .04; m.max_layer_mm = .30; m.line_width_mm = .4;
        m.temperature_c = 210; m.min_temperature_c = 190; m.max_temperature_c = 230;
        input.materials.push_back(m);
    }
    return input;
}

TEST_CASE("sublayer recipes conserve thickness and reference only ordered physical materials", "[LocalPrintColorRecipes]")
{
    auto input = recipe_input(6);
    const auto catalog = Recipes::enumerate(input);
    REQUIRE(catalog.ok());
    REQUIRE_FALSE(catalog.candidates.empty());
    std::set<std::string> ids;
    bool two = false, three = false;
    for (const auto& c : catalog.candidates) {
        CHECK(AI::is_valid_mixed_color_recipe(c.recipe));
        CHECK_FALSE(c.recipe.existing_virtual_slot.has_value());
        CHECK(ids.insert(c.id).second);
        CHECK(c.evidence == AI::ColorEvidence::Estimated);
        two |= c.recipe.components.size() == 2; three |= c.recipe.components.size() == 3;
        REQUIRE(c.sublayer_heights_mm.size() == 1);
        const auto& row = c.sublayer_heights_mm.front();
        REQUIRE(row.size() == c.recipe.components.size());
        CHECK_THAT(std::accumulate(row.begin(), row.end(), 0.), Catch::Matchers::WithinAbs(.20, 1e-12));
        for (size_t i = 0; i < row.size(); ++i) {
            CHECK(row[i] >= .04 - 1e-12);
            CHECK_THAT(row[i]/.01, Catch::Matchers::WithinAbs(std::round(row[i]/.01), 1e-9));
            CHECK(c.recipe.components[i].slot % 2 == 0);
            CHECK(c.recipe.components[i].slot <= 10);
            if (i) CHECK(c.recipe.components[i-1].slot < c.recipe.components[i].slot);
        }
    }
    CHECK(two); CHECK(three);
    // 15 pairs * 13 ratios + 20 triples * 45 positive grid compositions.
    CHECK(catalog.candidates.size() == 1095);
}

TEST_CASE("minimum sublayer thickness excludes percentage recipes the process cannot print", "[LocalPrintColorRecipes]")
{
    auto input = recipe_input();
    for (auto& m : input.materials) m.min_layer_mm = .08;
    input.minimum_component_percent = 5;
    auto catalog = Recipes::enumerate(input);
    REQUIRE(catalog.ok());
    // Only 40/60 through 60/40 in steps of five fit a .20 layer.
    REQUIRE(catalog.candidates.size() == 15);
    for (const auto& c : catalog.candidates) {
        CHECK(c.recipe.components.size() == 2);
        for (const auto& component : c.recipe.components) CHECK(component.ratio >= .4 - 1e-12);
    }
    CHECK(catalog.rejected_by_process > 0);
    input.process.layer_heights_mm.push_back(.12);
    catalog = Recipes::enumerate(input);
    REQUIRE(catalog.ok());
    CHECK(catalog.candidates.empty()); // Both materials cannot fit in the thinner adaptive layer.
}

TEST_CASE("recipe legality checks region dimensions temperature compatibility and Z resolution", "[LocalPrintColorRecipes]")
{
    auto input = recipe_input(2);
    const auto failure = GENERATE("width", "height", "temperature", "compatibility", "type", "resolution", "max-height");
    if (std::string(failure) == "width") input.process.region_width_mm = .39;
    if (std::string(failure) == "height") input.process.region_height_mm = .19;
    if (std::string(failure) == "temperature") input.materials[1].temperature_c = 240;
    if (std::string(failure) == "compatibility") input.materials[1].channel.compatible = false;
    if (std::string(failure) == "type") input.materials[1].channel.material_type = "PETG";
    if (std::string(failure) == "resolution") input.process.z_resolution_mm = .07;
    if (std::string(failure) == "max-height") for (auto& m : input.materials) m.max_layer_mm = .06;
    const auto catalog = Recipes::enumerate(input);
    REQUIRE(catalog.ok());
    CHECK(catalog.candidates.empty());
    CHECK(catalog.rejected_by_process > 0);
}

TEST_CASE("recipe catalogs reject missing process data and unsupported execution modes", "[LocalPrintColorRecipes]")
{
    auto input = recipe_input();
    const auto bad = GENERATE("identity", "mode", "first", "nan", "empty", "duplicate", "grid", "infinite");
    if (std::string(bad) == "identity") input.materials[0].identity.clear();
    if (std::string(bad) == "mode") input.process.sublayers_enabled = false;
    if (std::string(bad) == "first") input.process.first_layer_unsplit = false;
    if (std::string(bad) == "nan") input.materials[0].min_layer_mm = std::numeric_limits<double>::quiet_NaN();
    if (std::string(bad) == "empty") input.process.layer_heights_mm.clear();
    if (std::string(bad) == "duplicate") input.materials[1].channel.slot = input.materials[0].channel.slot;
    if (std::string(bad) == "grid") input.ratio_step_percent = 3;
    if (std::string(bad) == "infinite") input.process.region_height_mm = std::numeric_limits<double>::infinity();
    const auto catalog = Recipes::enumerate(input);
    CHECK_FALSE(catalog.ok());
    CHECK(catalog.candidates.empty());
    CHECK_FALSE(catalog.error.empty());
}

TEST_CASE("calibration binds actual material process order and observation conditions", "[LocalPrintColorRecipes]")
{
    auto input = recipe_input();
    const auto initial = Recipes::enumerate(input);
    REQUIRE(initial.ok()); REQUIRE_FALSE(initial.candidates.empty());
    const auto key = initial.candidates.front().id;
    input.calibration.push_back({key, "synthetic-test-coupon", std::string(64, 'b'), AI::ColorEvidence::Measured, {.2f,.3f,.4f}, .7});
    const auto measured = Recipes::enumerate(input);
    REQUIRE(measured.ok());
    CHECK(measured.candidates.front().evidence == AI::ColorEvidence::Measured);
    CHECK(measured.candidates.front().color == input.calibration.front().color);
    CHECK(measured.candidates.front().evidence_sha256 == std::string(64,'b'));
    const auto change = GENERATE("reorder", "batch", "height", "surface", "measurement", "temperature", "width", "color", "slot");
    if (std::string(change) == "reorder") std::reverse(input.materials.begin(), input.materials.end());
    if (std::string(change) == "batch") input.materials[0].identity += "-other";
    if (std::string(change) == "height") input.process.layer_heights_mm = {.30};
    if (std::string(change) == "surface") input.process.surface_condition = "top-surface";
    if (std::string(change) == "measurement") input.process.measurement_condition = "other-illuminant";
    if (std::string(change) == "temperature") input.materials[0].temperature_c = 215;
    if (std::string(change) == "width") input.process.region_width_mm = 3;
    if (std::string(change) == "color") input.materials[0].channel.display_color = "#010203";
    if (std::string(change) == "slot") input.materials[0].channel.slot = 5;
    const auto updated = Recipes::enumerate(input);
    REQUIRE(updated.ok()); REQUIRE_FALSE(updated.candidates.empty());
    if (std::string(change) == "reorder") {
        CHECK(updated.candidates.front().id == key);
        CHECK(updated.candidates.front().evidence == AI::ColorEvidence::Measured);
    } else {
        for (const auto& candidate : updated.candidates) CHECK(candidate.evidence == AI::ColorEvidence::Estimated);
    }
}

TEST_CASE("interpolation stays distinct and contradictory calibration is rejected", "[LocalPrintColorRecipes]")
{
    auto input = recipe_input(2);
    const auto initial = Recipes::enumerate(input);
    REQUIRE(initial.ok()); REQUIRE_FALSE(initial.candidates.empty());
    input.calibration.push_back({initial.candidates.front().id, "synthetic-interpolation", std::string(64,'a'),
        AI::ColorEvidence::Interpolated, {.5f,.5f,.5f}, 2.});
    const auto catalog = Recipes::enumerate(input);
    REQUIRE(catalog.ok());
    CHECK(catalog.candidates.front().evidence == AI::ColorEvidence::Interpolated);
    auto ranking = Recipes::rank(catalog, {{{.5f,.5f,.5f}, 1}}, 1.);
    const auto score = std::find_if(ranking.begin(), ranking.end(), [](const auto& x) { return x.candidate == 0; });
    REQUIRE(score != ranking.end());
    CHECK_FALSE(score->within_tolerance); // Zero nominal error cannot hide declared uncertainty.
    input.calibration.push_back(input.calibration.front());
    CHECK_FALSE(Recipes::enumerate(input).ok());
}

TEST_CASE("all extended target groups keep a ranked nearest legal recipe even outside tolerance", "[LocalPrintColorRecipes]")
{
    const size_t n = GENERATE(size_t(8), size_t(12), size_t(16));
    const auto catalog = Recipes::enumerate(recipe_input(6));
    REQUIRE(catalog.ok()); REQUIRE_FALSE(catalog.candidates.empty());
    for (size_t t = 0; t < n; ++t) {
        const AI::PrintRgb dominant = {float(t+1)/float(n+1), .2f, .7f};
        const AI::PrintRgb detail = {1,1,1};
        const auto ranking = Recipes::rank(catalog, {{dominant, 99}, {detail, 1}}, 0.);
        REQUIRE(ranking.size() == catalog.candidates.size());
        REQUIRE_FALSE(ranking.front().within_tolerance);
        const auto& color = catalog.candidates[ranking.front().candidate].color;
        const double expected = .99*Recipes::delta_e(dominant,color) + .01*Recipes::delta_e(detail,color);
        CHECK_THAT(ranking.front().mean_delta_e00, Catch::Matchers::WithinAbs(expected, 1e-10));
        const double worst = std::max(Recipes::delta_e(dominant,color), Recipes::delta_e(detail,color));
        CHECK_THAT(ranking.front().worst_delta_e00, Catch::Matchers::WithinAbs(worst, 1e-10));
        for (const auto& c : catalog.candidates)
            CHECK(expected <= .99*Recipes::delta_e(dominant,c.color) + .01*Recipes::delta_e(detail,c.color) + 1e-10);
    }
}

TEST_CASE("cancelled recipe enumeration publishes no partial candidate catalog", "[LocalPrintColorRecipes]")
{
    auto input = recipe_input(6);
    int checks = 0;
    input.cancelled = [&] { return ++checks > 17; };
    const auto catalog = Recipes::enumerate(input);
    CHECK(catalog.cancelled);
    CHECK(catalog.candidates.empty());
    CHECK_FALSE(catalog.ok());
}

TEST_CASE("oversized recipe searches report their limit without a partial nearest candidate", "[LocalPrintColorRecipes]")
{
    auto input = recipe_input(6);
    const auto excessive = GENERATE("grid", "layers");
    if (std::string(excessive) == "grid") {
        input.minimum_component_percent = 1;
        input.ratio_step_percent = 1;
    } else {
        input.process.layer_heights_mm.resize(4096, .20);
    }
    const auto catalog = Recipes::enumerate(input);
    CHECK_FALSE(catalog.ok());
    CHECK(catalog.candidates.empty());
    CHECK(catalog.error.find("bounded catalog") != std::string::npos);
}

static AI::LocalPrintColorResult recipe_record(size_t components,AI::ColorEvidence evidence)
{
    auto input=recipe_input();input.process.layer_heights_mm={.20,.30};
    auto catalog=Recipes::enumerate(input);
    const auto first=std::find_if(catalog.candidates.begin(),catalog.candidates.end(),[&](const auto& c){return c.recipe.components.size()==components;});
    if(first==catalog.candidates.end()) throw std::runtime_error("Fixture requires a generated recipe.");
    const auto key=first->id;
    if(evidence!=AI::ColorEvidence::Estimated) {
        input.calibration.push_back({key,"synthetic-coupon",std::string(64,'b'),evidence,{.2f,.3f,.4f},.7});
        catalog=Recipes::enumerate(input);
    }
    const auto& c=*std::find_if(catalog.candidates.begin(),catalog.candidates.end(),[&](const auto& v){return v.id==key;});
    AI::LocalPrintColorResult result;result.algorithm_version="recipe-proof-fixture-v1";
    result.source_sha256=std::string(64,'a');result.geometry_id="synthetic-geometry";
    result.material_fingerprint=input.process.material_fingerprint;result.process_fingerprint=input.process.process_fingerprint;
    result.requested_color_count=8;result.mode=AI::PrintColorMode::Layered;result.face_count=1;result.face_targets={0};
    result.physical_channels=catalog.physical_channels;result.color_tolerance=1;
    AI::PrintColorTarget target;target.source=c.color;target.output=c.color;target.area=1;target.recipe=c.recipe;target.recipe_proof=c.proof;
    target.candidate_id=c.id;target.evidence=c.evidence;target.executable=true;target.within_tolerance=true;
    result.targets.push_back(std::move(target));return result;
}

TEST_CASE("saved recipe candidates retain ordered constraints and color provenance", "[LocalPrintColorRecipes]")
{
    namespace State=GUI::LocalPrintColorState;
    const auto count=GENERATE(size_t(2),size_t(3));
    const auto evidence=GENERATE(AI::ColorEvidence::Estimated,AI::ColorEvidence::Measured,AI::ColorEvidence::Interpolated);
    const auto result=recipe_record(count,evidence);const auto encoded=State::encode(result);
    AI::LocalPrintColorResult restored;std::string error;
    REQUIRE(State::decode(nlohmann::json::parse(encoded.dump()),result.source_sha256,result.geometry_id,result.material_fingerprint,result.process_fingerprint,restored,error,result.algorithm_version));
    REQUIRE(restored.targets.front().recipe_proof);
    const auto& proof=*restored.targets.front().recipe_proof;
    CHECK(proof.materials.size()==3);
    CHECK(proof.sublayer_heights_mm.size()==2);
    CHECK(proof.sublayer_heights_mm.front().size()==count);
    CHECK(restored.targets.front().evidence==evidence);
    CHECK_THAT(proof.uncertainty_delta_e,Catch::Matchers::WithinAbs(evidence==AI::ColorEvidence::Estimated ? 0 : .7,1e-12));
    CHECK(State::encode(restored)==encoded);
    CHECK(encoded["targets"][0]["recipe_proof"]["scope"]=="supplied-constraints-only");
}

TEST_CASE("changed recipe proof fields cannot replace the last saved result", "[LocalPrintColorRecipes]")
{
    namespace State=GUI::LocalPrintColorState;
    const auto field=GENERATE("material","slot","ratio","order","thickness","layer","z","temperature","dimensions","observation","id","source","evidence","uncertainty","checksum","echo","scope");
    auto saved=recipe_record(3,AI::ColorEvidence::Measured);auto encoded=State::encode(saved);const auto before=encoded;
    auto& target=encoded["targets"][0];auto& proof=target["recipe_proof"];auto& context=proof["context"];
    const std::string f=field;
    if(f=="material") context["materials"][0]["identity"]="another-batch";
    if(f=="slot") context["materials"][0]["slot"]=-1;
    if(f=="ratio") target["recipe"]["components"][0]["ratio"]=.11;
    if(f=="order") context["order"]="reverse";
    if(f=="thickness") proof["sublayer_heights_mm"][0][0]=.03;
    if(f=="layer") context["layer_heights_mm"][0]=.4;
    if(f=="z") context["z_resolution_mm"]=.003;
    if(f=="temperature") context["materials"][0]["temperature_c"]=500;
    if(f=="dimensions") context["region_width_mm"]=.1;
    if(f=="observation") context["surface_condition"]="different-face";
    if(f=="id") target["candidate_id"]=std::string(64,'c');
    if(f=="source") proof["evidence_sha256"]=std::string(64,'c');
    if(f=="evidence") target["evidence"]="estimated";
    if(f=="uncertainty") proof["uncertainty_delta_e"]=0;
    if(f=="checksum") proof["checksum"]=std::string(64,'c');
    if(f=="echo") proof["output"]={.1,.2,.3};
    if(f=="scope") proof["scope"]="native-slicing-verified";
    std::string error;
    CHECK_FALSE(State::decode(encoded,saved.source_sha256,saved.geometry_id,saved.material_fingerprint,saved.process_fingerprint,saved,error,saved.algorithm_version));
    CHECK_FALSE(error.empty());CHECK(State::encode(saved)==before);
}

TEST_CASE("recomputed checksums do not authorize illegal recipe process constraints", "[LocalPrintColorRecipes]")
{
    namespace Proof=GUI::LocalPrintRecipeProofState;
    auto result=recipe_record(3,AI::ColorEvidence::Measured);auto& target=result.targets.front();auto& proof=*target.recipe_proof;
    const std::string field=GENERATE("width","z","height","temperature","order","uncertainty","source");
    if(field=="width") proof.process.region_width_mm=.1;
    if(field=="z") proof.process.z_resolution_mm=.003;
    if(field=="height") proof.sublayer_heights_mm[0][0]=.03;
    if(field=="temperature") proof.materials[0].temperature_c=500;
    if(field=="order") std::reverse(target.recipe->components.begin(),target.recipe->components.end());
    if(field=="uncertainty") proof.uncertainty_delta_e=-1;
    if(field=="source") proof.evidence_sha256="missing";
    auto identity=Proof::context(proof.materials,proof.process);identity["components"]=nlohmann::json::array();
    for(const auto& c:target.recipe->components) identity["components"].push_back({c.slot,int(std::lround(c.ratio*100))});
    target.candidate_id=Proof::digest(identity);proof.checksum=Proof::digest(Proof::payload(target));
    std::string error;CHECK_FALSE(Proof::valid(target,result,error));CHECK_FALSE(error.empty());
}

TEST_CASE("legacy recipe previews remain unconfirmed until their missing evidence is recomputed", "[LocalPrintColorRecipes]")
{
    namespace State=GUI::LocalPrintColorState;
    const auto source=recipe_record(2,AI::ColorEvidence::Estimated);auto encoded=State::encode(source);
    encoded["targets"][0].erase("recipe_proof");AI::LocalPrintColorResult restored;std::string error;
    REQUIRE(State::decode(encoded,source.source_sha256,source.geometry_id,source.material_fingerprint,source.process_fingerprint,restored,error,source.algorithm_version));
    CHECK_FALSE(restored.targets[0].executable);CHECK_FALSE(restored.targets[0].within_tolerance);
    CHECK_FALSE(restored.targets[0].recipe_proof);CHECK_FALSE(restored.targets[0].unresolved_reason.empty());
    const auto previous=State::encode(restored);encoded["confirmed"]=true;
    CHECK_FALSE(State::decode(encoded,source.source_sha256,source.geometry_id,source.material_fingerprint,source.process_fingerprint,restored,error,source.algorithm_version));
    CHECK(State::encode(restored)==previous);
}

TEST_CASE("a self-consistent stored checksum cannot change the locally predicted recipe color", "[LocalPrintColorRecipes]")
{
    namespace Proof=GUI::LocalPrintRecipeProofState;
    auto result=recipe_record(2,AI::ColorEvidence::Estimated);auto& target=result.targets.front();
    target.output[0]=target.output[0]<.5f ? 1.f : 0.f;
    target.recipe_proof->checksum=Proof::digest(Proof::payload(target));
    std::string error;CHECK_FALSE(Proof::valid(target,result,error));
    CHECK(error.find("forward model")!=std::string::npos);
}

TEST_CASE("recipe display colors remain bound to the persisted prediction", "[LocalPrintColorRecipes]")
{
    namespace Proof=GUI::LocalPrintRecipeProofState;
    auto result=recipe_record(2,AI::ColorEvidence::Measured);auto& target=result.targets.front();
    target.recipe->target_color="#000000";
    target.recipe_proof->checksum=Proof::digest(Proof::payload(target));
    std::string error;CHECK_FALSE(Proof::valid(target,result,error));
    CHECK(error.find("display color")!=std::string::npos);
}

TEST_CASE("restoring a recipe confirmation requires matching freshly computed evidence", "[LocalPrintColorRecipes]")
{
    namespace State=GUI::LocalPrintColorState;namespace Proof=GUI::LocalPrintRecipeProofState;
    auto saved=recipe_record(2,AI::ColorEvidence::Measured);saved.confirmed=true;
    auto current=saved;auto destination=saved;std::string error;
    REQUIRE(State::restore_confirmed(saved,current,true,destination,error));
    const auto original=State::encode(destination);
    const std::string change=GENERATE("missing","calibration","uncertainty");
    auto& target=current.targets.front();
    if(change=="missing") target.recipe_proof.reset();
    else {
        if(change=="calibration") target.recipe_proof->evidence_sha256=std::string(64,'d');
        if(change=="uncertainty") target.recipe_proof->uncertainty_delta_e=.8;
        target.recipe_proof->checksum=Proof::digest(Proof::payload(target));
    }
    CHECK_FALSE(State::restore_confirmed(saved,current,true,destination,error));
    CHECK_FALSE(error.empty());CHECK(State::encode(destination)==original);
}

TEST_CASE("Mapped nozzle limits follow logical slots and automatic routes satisfy every nozzle", "[LocalPrintColorRecipes]")
{
    AI::PrintablePaletteSnapshot snapshot;
    snapshot.material_fingerprint = "materials"; snapshot.process_fingerprint = "routing";
    snapshot.material_metadata_complete = true;
    snapshot.physical_channels = {{2, "#000000", "PLA", true}, {5, "#FFFFFF", "PLA", true}};
    DynamicPrintConfig printer, print, filament;
    printer.set_key_value("nozzle_diameter", new ConfigOptionFloats{.4,.6});
    printer.set_key_value("min_layer_height", new ConfigOptionFloats{.04,.09});
    printer.set_key_value("max_layer_height", new ConfigOptionFloats{.30,.35});
    print.set_key_value("line_width", new ConfigOptionFloatOrPercent(110,true));
    print.set_key_value("layer_height", new ConfigOptionFloat(.2));
    print.set_key_value("enable_mixed_color_sublayer", new ConfigOptionBool(true));
    filament.set_key_value("nozzle_temperature", new ConfigOptionInts{210});
    filament.set_key_value("nozzle_temperature_range_low", new ConfigOptionInts{190});
    filament.set_key_value("nozzle_temperature_range_high", new ConfigOptionInts{230});
    GUI::PrintColorNozzleRouting routing;
    routing.mode = GENERATE(fmmManual, fmmNozzleManual, fmmAutoForFlush, fmmAutoForMatch);
    routing.filament_maps = {0,0,1,0,0,2};
    GUI::capture_print_color_process(snapshot,printer,print,{&filament,&filament},{"a","b"},&routing);
    const bool automatic = routing.mode == fmmAutoForFlush || routing.mode == fmmAutoForMatch;
    CHECK_THAT(snapshot.sublayer_materials[0].min_layer_mm, Catch::Matchers::WithinAbs(automatic ? .09 : .04,1e-12));
    CHECK_THAT(snapshot.sublayer_materials[0].max_layer_mm, Catch::Matchers::WithinAbs(.30,1e-12));
    CHECK_THAT(snapshot.sublayer_materials[0].line_width_mm, Catch::Matchers::WithinAbs(automatic ? .66 : .44,1e-12));
    CHECK_THAT(snapshot.sublayer_materials[1].min_layer_mm, Catch::Matchers::WithinAbs(.09,1e-12));
    CHECK_THAT(snapshot.sublayer_materials[1].max_layer_mm, Catch::Matchers::WithinAbs(automatic ? .30 : .35,1e-12));
    auto input=Recipes::from_workspace(snapshot);
    // Only the fixture supplies these missing geometry/observation facts.
    input.process.surface_condition="fixture-wall";input.process.measurement_condition="fixture-D65";
    input.process.region_width_mm=2;input.process.region_height_mm=1;input.process.z_resolution_mm=.01;
    auto catalog=Recipes::enumerate(input);
    REQUIRE(catalog.ok());REQUIRE_FALSE(catalog.candidates.empty());
    for (const auto& candidate:catalog.candidates) {
        REQUIRE(candidate.sublayer_heights_mm.size()==1);
        const auto& h=candidate.sublayer_heights_mm[0];REQUIRE(h.size()==2);
        CHECK(h[0] >= (automatic ? .09 : .04)-1e-9);CHECK(h[1] >= .09-1e-9);
        CHECK_THAT(h[0]+h[1],Catch::Matchers::WithinAbs(.2,1e-12));
    }
    const double prior_min=snapshot.sublayer_materials[0].min_layer_mm;
    routing.mode=fmmManual;routing.filament_maps[2]=2;routing.filament_maps[5]=1;
    CHECK_THAT(snapshot.sublayer_materials[0].min_layer_mm,Catch::Matchers::WithinAbs(prior_min,1e-12));
    GUI::capture_print_color_process(snapshot,printer,print,{&filament,&filament},{"a","b"},&routing);
    CHECK_THAT(snapshot.sublayer_materials[0].min_layer_mm,Catch::Matchers::WithinAbs(.09,1e-12));
    CHECK_THAT(snapshot.sublayer_materials[1].min_layer_mm,Catch::Matchers::WithinAbs(.04,1e-12));
}

TEST_CASE("Unknown or inconsistent nozzle routes do not borrow a different tool's limits", "[LocalPrintColorRecipes]")
{
    AI::PrintablePaletteSnapshot snapshot;snapshot.physical_channels={{3,"#000000","PLA",true}};
    DynamicPrintConfig printer,print;
    printer.set_key_value("nozzle_diameter",new ConfigOptionFloats{.4,.6});
    printer.set_key_value("min_layer_height",new ConfigOptionFloats{.08,.09});
    printer.set_key_value("max_layer_height",new ConfigOptionFloats{.28,.35});
    print.set_key_value("line_width",new ConfigOptionFloatOrPercent(.42,false));
    GUI::PrintColorNozzleRouting route;route.mode=fmmManual;
    route.filament_maps={1,1,1,GENERATE(0,-1,3)};
    GUI::capture_print_color_process(snapshot,printer,print,{nullptr},{""},&route);
    CHECK_THAT(snapshot.sublayer_materials[0].min_layer_mm,Catch::Matchers::WithinAbs(0,1e-12));
    route.mode=fmmAutoForFlush;
    printer.set_key_value("min_layer_height",new ConfigOptionFloats{.08});
    GUI::capture_print_color_process(snapshot,printer,print,{nullptr},{""},&route);
    CHECK_THAT(snapshot.sublayer_materials[0].min_layer_mm,Catch::Matchers::WithinAbs(0,1e-12));
    printer.set_key_value("min_layer_height",new ConfigOptionFloats{.08,.30});
    GUI::capture_print_color_process(snapshot,printer,print,{nullptr},{""},&route);
    CHECK_THAT(snapshot.sublayer_materials[0].min_layer_mm,Catch::Matchers::WithinAbs(0,1e-12));
    printer.set_key_value("min_layer_height",new ConfigOptionFloats{.08,.09});
    route.mode=fmmDefault;
    GUI::capture_print_color_process(snapshot,printer,print,{nullptr},{""},&route);
    CHECK_THAT(snapshot.sublayer_materials[0].min_layer_mm,Catch::Matchers::WithinAbs(0,1e-12));
}
