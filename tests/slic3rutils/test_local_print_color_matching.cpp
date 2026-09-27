#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "slic3r/GUI/AI/ModelGeneration/LocalPrintColorMatching.hpp"
#include "slic3r/GUI/AI/Model/LocalPrintColorState.hpp"
#include "slic3r/GUI/AI/Model/BeautyPrintColorHandoff.hpp"
#include "slic3r/GUI/AI/ModelGeneration/LocalPrintColorBoundaryRefinement.hpp"
#include "slic3r/GUI/AI/ModelGeneration/LocalPrintColorQuality.hpp"

using namespace Slic3r;
namespace Matching = GUI::LocalPrintColorMatching;

static Matching::Input matching_input(size_t count = 6)
{
    Matching::Input input;
    input.identity.source_sha256 = std::string(64, 'a');
    input.identity.geometry_id = "source-geometry";
    input.identity.material_fingerprint = "materials-1";
    input.identity.process_fingerprint = "process-1";
    input.identity.requested_color_count = count;
    const std::vector<std::string> colors {"#000000", "#FFFFFF", "#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    for (size_t i = 0; i < colors.size(); ++i) {
        input.identity.physical_channels.push_back({i, colors[i], "PLA", true});
        input.faces.push_back({Matching::hex_rgb(colors[i]), 1});
    }
    input.identity.face_count = input.faces.size();
    return input;
}

static AI::ModelMatchedColors saved_beauty(const Matching::Input& input)
{
    AI::ModelMatchedColors saved;
    saved.source_sha256=input.identity.source_sha256;
    saved.geometry_id=input.identity.geometry_id;
    saved.palette=input.identity.physical_channels;
    for(size_t f=0;f<input.faces.size();++f)saved.face_slots.push_back(f%saved.palette.size());
    return saved;
}

TEST_CASE("Beauty handoff preserves physical assignments through matching and persistence", "[LocalPrintColorMatching][BeautyHandoff]")
{
    const auto channels=GENERATE(1u,2u,3u,4u,5u,6u);
    auto input=matching_input();input.identity.geometry_id=std::string(64,'b');
    input.identity.physical_channels.resize(channels);
    // Duplicate RGB must not erase material identity; face samples can differ
    // from those outputs because of texture sampling or explicit user choice.
    for(auto& channel:input.identity.physical_channels)channel.display_color="#FFFFFF";
    auto saved=saved_beauty(input);REQUIRE(saved.valid());
    GUI::BeautyPrintColorHandoff::seed(saved,input.identity);
    const auto matched=Matching::compute(input);REQUIRE(matched.ok());
    for(size_t f=0;f<saved.face_slots.size();++f)
        CHECK(matched.result.targets.at(matched.result.face_targets.at(f)).physical_slot==saved.face_slots[f]);
    auto accepted=matched.result;accepted.confirmed=true;
    const auto json=GUI::LocalPrintColorState::encode(accepted);
    AI::LocalPrintColorResult restored;std::string error;
    REQUIRE(GUI::LocalPrintColorState::decode(json,accepted.source_sha256,accepted.geometry_id,
        accepted.material_fingerprint,accepted.process_fingerprint,restored,error));
    CHECK(restored.face_targets==accepted.face_targets);
    CHECK(restored.regions.size()==channels);
}

TEST_CASE("Stale beauty handoff leaves an existing draft unchanged", "[LocalPrintColorMatching][BeautyHandoff]")
{
    auto input=matching_input();input.identity.geometry_id=std::string(64,'b');
    auto saved=saved_beauty(input);
    SECTION("different source") {saved.source_sha256=std::string(64,'c');}
    SECTION("different geometry") {saved.geometry_id=std::string(64,'c');}
    SECTION("different face count") {saved.face_slots.pop_back();}
    SECTION("different material") {input.identity.physical_channels[0].material_type="PETG";}
    SECTION("different color") {input.identity.physical_channels[0].display_color="#808080";}
    SECTION("incompatible material") {input.identity.physical_channels[0].compatible=false;}
    SECTION("existing edits") {input.identity.user_overrides={{0,{1.f,0.f,0.f}}};}
    REQUIRE_THROWS(GUI::BeautyPrintColorHandoff::seed(saved,input.identity));
    CHECK(input.identity.regions.empty());
}

TEST_CASE("Explicit recolor releases only selected inherited material locks", "[LocalPrintColorMatching][BeautyHandoff]")
{
    auto input=matching_input();input.identity.geometry_id=std::string(64,'b');
    const auto saved=saved_beauty(input);
    GUI::BeautyPrintColorHandoff::seed(saved,input.identity);
    const auto before=input.identity;
    GUI::BeautyPrintColorHandoff::unlock_faces(input.identity.regions,{0});
    input.identity.user_overrides={{0,{1.f,1.f,1.f}}};
    const auto matched=Matching::compute(input);REQUIRE(matched.ok());
    CHECK(matched.result.targets.at(matched.result.face_targets[0]).physical_slot==1);
    for(size_t f=1;f<input.faces.size();++f)
        CHECK(matched.result.targets.at(matched.result.face_targets[f]).physical_slot==saved.face_slots[f]);
    // Undo restores the original handoff without rerunning recognition.
    input.identity=before;
    const auto undone=Matching::compute(input);REQUIRE(undone.ok());
    CHECK(undone.result.targets.at(undone.result.face_targets[0]).physical_slot==0);
}

TEST_CASE("Beauty assignments survive the production boundary refinement pipeline", "[LocalPrintColorMatching][BeautyHandoff]")
{
    const auto mesh=its_make_cube(10,10,10);
    auto input=matching_input();
    input.identity.geometry_id=AI::SurfaceSelectionPersistence::geometry_fingerprint(mesh);
    input.faces.assign(mesh.indices.size(),{{1.f,1.f,1.f},1});
    input.identity.face_count=input.faces.size();
    for(auto& channel:input.identity.physical_channels)channel.display_color="#FFFFFF";
    const auto saved=saved_beauty(input);
    GUI::BeautyPrintColorHandoff::seed(saved,input.identity);
    const auto matched=GUI::LocalPrintColorBoundaryRefinement::compute_guarded(input,mesh);
    REQUIRE(matched.ok());
    for(size_t f=0;f<saved.face_slots.size();++f)
        CHECK(matched.result.targets.at(matched.result.face_targets[f]).physical_slot==saved.face_slots[f]);
}

TEST_CASE("Native mixed beauty assignments are not silently flattened by physical handoff", "[LocalPrintColorMatching][BeautyHandoff]")
{
    auto input=matching_input();input.identity.geometry_id=std::string(64,'b');
    auto saved=saved_beauty(input);
    saved.mixed_recipes.push_back({"#808080",{{0,.5},{1,.5}},6,std::string(64,'a'),true});
    saved.face_slots[0]=6;REQUIRE(saved.valid());
    REQUIRE_THROWS_WITH(GUI::BeautyPrintColorHandoff::seed(saved,input.identity),
        "This beauty version uses native mixed filaments. Import it directly from the beauty workbench.");
    CHECK(input.identity.regions.empty());
}

TEST_CASE("unsaved beauty draft completes the cold color matching path and reloads its selection", "[LocalPrintColorMatching][ColdPath]")
{
    // This deliberately starts with an empty LocalPrintColorResult: a newly
    // generated beauty model has no saved color-version record yet. The panel
    // must still be able to analyze, select a color, apply it, and reopen the
    // resulting identity without borrowing a stale saved result.
    auto input = matching_input(6);
    input.identity.face_count = 6;
    input.identity.regions = {
        {"auto-semantic:person-a:skin", "person-a", "skin", .94, false, {0, 1, 2}, true, {}},
        {"auto-semantic:person-a:hair", "person-a", "hair", .91, false, {3, 4, 5}, true, {}}
    };
    const auto cold = Matching::compute(input);
    REQUIRE(cold.ok());
    REQUIRE_FALSE(cold.result.confirmed);
    REQUIRE(cold.result.face_targets.size() == input.faces.size());

    // The user changes one selected face before confirming the color match.
    auto selected = input;
    selected.identity = cold.result;
    selected.identity.confirmed = false;
    selected.identity.user_overrides = {{1, Matching::hex_rgb("#FF8080")}};
    const auto applied = Matching::compute(selected);
    REQUIRE(applied.ok());
    CHECK(applied.result.face_targets.size() == selected.faces.size());
    CHECK(applied.result.user_overrides.size() == 1);

    auto confirmed = applied.result;
    confirmed.confirmed = true;
    std::string validation_error;
    REQUIRE(confirmed.valid(validation_error));
    const auto stored = GUI::LocalPrintColorState::encode(confirmed);
    AI::LocalPrintColorResult reopened;
    REQUIRE(GUI::LocalPrintColorState::decode(stored, confirmed.source_sha256, confirmed.geometry_id,
        confirmed.material_fingerprint, confirmed.process_fingerprint, reopened, validation_error));
    CHECK(GUI::LocalPrintColorState::encode(reopened) == stored);

    // Restoring the original selected color must be another complete compute,
    // with the confirmed state left untouched until the replacement succeeds.
    auto restored = selected;
    restored.identity = reopened;
    restored.identity.confirmed = false;
    restored.identity.user_overrides.clear();
    const auto restored_result = Matching::compute(restored);
    REQUIRE(restored_result.ok());
    CHECK(restored_result.result.user_overrides.empty());
    CHECK(restored_result.result.face_targets.size() == restored.faces.size());
}

TEST_CASE("layered requests compare every target with physical approximations without losing the original groups", "[LocalPrintColorMatching]")
{
    const size_t count = GENERATE(size_t(8), size_t(12), size_t(16));
    auto input = matching_input(count);
    input.faces.clear();
    for (size_t i = 0; i < count; ++i) {
        AI::PrintRgb color {float((i*53+19)%256)/255, float((i*97+31)%256)/255, float((i*29+71)%256)/255};
        input.faces.push_back({color, double(i+1)});
        input.identity.user_overrides.push_back({i, color});
    }
    input.identity.face_count = count;
    const auto computed = Matching::compute(input);
    REQUIRE(computed.ok());
    REQUIRE(computed.result.targets.size() == count);
    CHECK(computed.result.requested_color_count == count);
    CHECK(computed.result.physical_channels.size() == 6);
    CHECK(computed.result.algorithm_version == "region-layered-v2");
    for (size_t f = 0; f < count; ++f) {
        const auto& target = computed.result.targets[computed.result.face_targets[f]];
        REQUIRE(target.executable);
        REQUIRE(target.physical_slot);
        CHECK_FALSE(target.recipe);
        double nearest = std::numeric_limits<double>::infinity();
        for (const auto& channel : input.identity.physical_channels)
            nearest = std::min(nearest, Matching::delta_e(input.faces[f].color, Matching::hex_rgb(channel.display_color)));
        CHECK_THAT(target.delta_e00, Catch::Matchers::WithinAbs(nearest, 1e-8));
        CHECK(target.within_tolerance == (nearest <= input.tolerance));
    }
    auto saved = computed.result; saved.confirmed = true;
    const auto json = GUI::LocalPrintColorState::encode(saved);
    AI::LocalPrintColorResult decoded; std::string reason;
    REQUIRE(GUI::LocalPrintColorState::decode(json, saved.source_sha256, saved.geometry_id,
        saved.material_fingerprint, saved.process_fingerprint, decoded, reason, saved.algorithm_version));
    CHECK(decoded.face_targets == saved.face_targets);
    CHECK(decoded.targets.size() == count);
    CHECK(decoded.confirmed);
}

TEST_CASE("layered target selection uses legal recipes across the full target set and rejects stale catalogs", "[LocalPrintColorMatching]")
{
    namespace Recipes = GUI::LocalPrintColorRecipes;
    const size_t count = GENERATE(size_t(8), size_t(12), size_t(16));
    auto input = matching_input(count);
    Recipes::Input recipe;
    recipe.process.material_fingerprint = input.identity.material_fingerprint;
    recipe.process.process_fingerprint = input.identity.process_fingerprint;
    recipe.process.sublayers_enabled = true; recipe.process.layer_heights_mm = {.2};
    recipe.process.z_resolution_mm = .01; recipe.process.region_width_mm = 2; recipe.process.region_height_mm = 1;
    recipe.process.surface_condition = "synthetic-wall"; recipe.process.measurement_condition = "synthetic-D65";
    for (const auto& channel : input.identity.physical_channels) {
        Recipes::Material material; material.channel = channel; material.identity = "fixture-" + std::to_string(channel.slot);
        material.min_layer_mm = .04; material.max_layer_mm = .3; material.line_width_mm = .4;
        material.temperature_c = 210; material.min_temperature_c = 190; material.max_temperature_c = 230;
        recipe.materials.push_back(material);
    }
    auto catalog = std::make_shared<Recipes::Catalog>(Recipes::enumerate(recipe));
    REQUIRE(catalog->ok());
    input.recipe_catalog = catalog; input.faces.clear();
    std::set<uint32_t> seen;
    for (const auto& candidate : catalog->candidates) {
        bool distinct = true;
        for (const auto& channel : input.identity.physical_channels)
            distinct &= Matching::delta_e(candidate.color, Matching::hex_rgb(channel.display_color)) > 1;
        if (!distinct || !seen.insert(Matching::packed_rgb(candidate.color)).second) continue;
        input.identity.user_overrides.push_back({input.faces.size(), candidate.color});
        input.faces.push_back({candidate.color, 1});
        if (input.faces.size() == count) break;
    }
    REQUIRE(input.faces.size() == count);
    input.identity.face_count = count;
    const auto computed = Matching::compute(input);
    REQUIRE(computed.ok());
    REQUIRE(computed.result.targets.size() == count);
    for (const auto& target : computed.result.targets) {
        REQUIRE(target.recipe);
        REQUIRE(target.recipe_proof);
        CHECK(target.executable);
        CHECK(target.within_tolerance);
        CHECK_FALSE(target.physical_slot);
        CHECK_THAT(target.delta_e00, Catch::Matchers::WithinAbs(0, 1e-8));
        CHECK(std::any_of(catalog->candidates.begin(), catalog->candidates.end(),
            [&](const auto& candidate) { return candidate.id == target.candidate_id; }));
    }
    const auto serialized=GUI::LocalPrintColorState::encode(computed.result);
    AI::LocalPrintColorResult restored;std::string restore_error;
    REQUIRE(GUI::LocalPrintColorState::decode(serialized,computed.result.source_sha256,computed.result.geometry_id,
        computed.result.material_fingerprint,computed.result.process_fingerprint,restored,restore_error,computed.result.algorithm_version));
    CHECK(GUI::LocalPrintColorState::encode(restored)==serialized);
    catalog->process_fingerprint = "different-process";
    const auto stale = Matching::compute(input);
    REQUIRE(stale.ok());
    for (const auto& target : stale.result.targets) { CHECK_FALSE(target.recipe); CHECK(target.physical_slot.has_value()); }
    int polls = 0; input.cancelled = [&] { return ++polls > 5; };
    CHECK(Matching::compute(input).cancelled);
}

TEST_CASE("layered joint assignment respects physical locks and hard region contrast", "[LocalPrintColorMatching]")
{
    auto input = matching_input(8);
    input.faces.resize(2); input.identity.face_count = 2;
    input.identity.physical_channels.resize(2);
    input.identity.regions = {{"a", "person", "hair", 1, true, {0}}, {"b", "person", "skin", 1, true, {1}}};
    input.identity.regions[0].locked_physical_slot = 1;
    input.contrasts = {{"a", "b", 1, 30, true}};
    auto computed = Matching::compute(input);
    REQUIRE(computed.ok());
    const auto& a = computed.result.targets[computed.result.face_targets[0]];
    const auto& b = computed.result.targets[computed.result.face_targets[1]];
    CHECK(a.physical_slot == std::optional<size_t>(1));
    REQUIRE(b.executable);
    CHECK(Matching::delta_e(a.output, b.output) >= 30);
    input.identity.physical_channels[0].display_color = "#FFFFFF";
    computed = Matching::compute(input);
    REQUIRE(computed.ok());
    for (const auto& target : computed.result.targets) CHECK_FALSE(target.executable);
}

TEST_CASE("layered facial matching preserves supported lip skin separation and honors user overrides", "[LocalPrintColorMatching]")
{
    auto input = matching_input(8);
    input.faces = {{{.80f,.40f,.30f}, .2}, {{.75f,.10f,.16f}, .01}, {{0,0,0}, .79}};
    input.identity.face_count = 3; input.identity.physical_channels.resize(3);
    input.identity.regions = {{"auto-semantic:p:face", "p", "face", 1, false, {0}},
        {"auto-semantic:p:llip", "p", "llip", 1, false, {1}}};
    const auto computed = Matching::compute(input);
    REQUIRE(computed.ok());
    const auto& face = computed.result.targets[computed.result.face_targets[0]];
    const auto& lip = computed.result.targets[computed.result.face_targets[1]];
    REQUIRE(face.executable); REQUIRE(lip.executable);
    CHECK(Matching::delta_e(face.output, lip.output) > 10);
    CHECK(computed.result.contrasts.empty()); // Derived rules do not become user-authored constraints.
    input.identity.regions[0].locked_physical_slot = 2;
    const auto locked = Matching::compute(input);
    REQUIRE(locked.ok());
    CHECK(locked.result.targets[locked.result.face_targets[0]].physical_slot == std::optional<size_t>(2));
    input.identity.regions[0].locked_physical_slot.reset();
    input.identity.regions[1].subject_id = "other-person";
    const auto separate = Matching::compute(input);
    REQUIRE(separate.ok());
    CHECK(separate.result.targets[separate.result.face_targets[0]].physical_slot == std::optional<size_t>(2));
}

TEST_CASE("layered facial separation does not exaggerate contrast against multicolor mouth and eyes", "[LocalPrintColorMatching]")
{
    auto input = matching_input(8);
    input.identity.physical_channels = {{0,"#000000","PLA",true}, {1,"#FFFFFF","PLA",true},
        {2,"#E53935","PLA",true}, {3,"#26A69A","PLA",true}};
    input.faces.clear();
    const std::vector<std::string> source {"#E2A681", "#B33226", "#433028", "#D9D2CC", "#717173", "#346869", "#B47C5A", "#802620"};
    const std::vector<double> area {.048,.177,.113,.056,.295,.256,.019,.036};
    for (size_t i = 0; i < source.size(); ++i) input.faces.push_back({Matching::hex_rgb(source[i]), area[i]});
    input.identity.face_count = input.faces.size();
    input.identity.regions = {{"auto-semantic:p:face","p","face",.85,false,{0}},
        {"auto-semantic:p:llip","p","llip",.85,false,{1}},
        {"auto-semantic:p:hair","p","hair",.85,false,{2}},
        {"auto-semantic:p:imouth","p","imouth",.85,false,{3}},
        {"auto-semantic:p:le","p","le",.85,false,{2,3}}};
    const auto computed = Matching::compute(input);
    REQUIRE(computed.ok());
    auto slot = [&](const auto& result, size_t face) { return result.targets[result.face_targets[face]].physical_slot; };
    CHECK(slot(computed.result, 0) == std::optional<size_t>(1));
    CHECK(slot(computed.result, 1) == std::optional<size_t>(2));
    CHECK(slot(computed.result, 2) == std::optional<size_t>(0));
    CHECK(slot(computed.result, 3) == std::optional<size_t>(1));
    CHECK(computed.result.requested_color_count == 8);
    CHECK(computed.result.targets.size() == 8);
    // Subdividing the same skin and lip evidence must not multiply its effect.
    input.identity.regions.push_back({"auto-semantic:p:nose","p","nose",.85,false,{0}});
    input.identity.regions.push_back({"auto-semantic:p:neck","p","neck",.85,false,{0}});
    input.identity.regions.push_back({"auto-semantic:p:ulip","p","ulip",.85,false,{1}});
    const auto duplicate = Matching::compute(input);
    REQUIRE(duplicate.ok());
    for (size_t f = 0; f < input.faces.size(); ++f) CHECK(slot(duplicate.result, f) == slot(computed.result, f));
}

TEST_CASE("direct assignment minimizes surface error instead of palette centroid error", "[LocalPrintColorMatching]")
{
    auto input = matching_input(1);
    input.faces = {{{0, 0, 0}, 15}, {{1, 1, 1}, 85}};
    input.identity.face_count = 2;
    input.identity.physical_channels = {{0, "#FFFFFF", "PLA", true}, {1, "#CECECE", "PLA", true}};
    const auto computed = Matching::compute(input);
    REQUIRE(computed.ok());
    const auto quality = GUI::LocalPrintColorQuality::evaluate(input.faces, computed.result);
    REQUIRE(quality.error.empty());
    double optimum = std::numeric_limits<double>::infinity();
    for (const auto& slot : input.identity.physical_channels) {
        const auto color = Matching::hex_rgb(slot.display_color);
        double error = 0;
        for (const auto& face : input.faces) error += face.area * Matching::delta_e(face.color, color) / 100;
        optimum = std::min(optimum, error);
    }
    REQUIRE(computed.result.targets.size() == 1);
    CHECK(computed.result.targets.front().physical_slot == std::optional<size_t>(0));
    CHECK_THAT(quality.mean_delta_e, Catch::Matchers::WithinAbs(optimum, 1e-8));
    CHECK_THAT(computed.result.targets.front().delta_e00, Catch::Matchers::WithinAbs(optimum, 1e-8));
}

TEST_CASE("local printing mode follows the requested budget rather than source RGB count", "[LocalPrintColorMatching]")
{
    const auto requested = GENERATE(size_t(1), size_t(4), size_t(6), size_t(7), size_t(8), size_t(12), size_t(16));
    auto input = matching_input(requested);
    auto computed = Matching::compute(input);
    REQUIRE(computed.ok());
    CHECK(computed.result.requested_color_count == requested);
    CHECK(computed.result.mode == AI::print_color_mode(requested));
    CHECK(computed.result.targets.size() <= requested);
    CHECK(computed.result.face_targets.size() == input.faces.size());
    CHECK(computed.result.physical_channels.size() == 6);
}

TEST_CASE("direct assignment retains each available source color with a distinct physical slot", "[LocalPrintColorMatching]")
{
    const auto input = matching_input();
    const auto computed = Matching::compute(input);
    REQUIRE(computed.ok());
    CHECK(computed.result.unresolved_count() == 0);
    std::set<size_t> slots;
    for (size_t f = 0; f < input.faces.size(); ++f) {
        const auto& target = computed.result.targets[computed.result.face_targets[f]];
        REQUIRE(target.physical_slot);
        slots.insert(*target.physical_slot);
        CHECK_THAT(Matching::delta_e(input.faces[f].color, target.output), Catch::Matchers::WithinAbs(0, 0.02));
        CHECK(target.evidence == AI::ColorEvidence::Estimated);
    }
    CHECK(slots.size() == input.faces.size());
}

TEST_CASE("a tiny protected lip region survives beside a dominant skin region", "[LocalPrintColorMatching]")
{
    auto input = matching_input(2);
    input.faces = {{{.8f, .6f, .5f}, 9999}, {{.75f, .12f, .2f}, 1}};
    input.identity.face_count = 2;
    input.identity.regions = {{"skin", "person-1", "skin", 1, true, {0}}, {"lips", "person-1", "lips", 1, true, {1}}};
    const auto computed = Matching::compute(input);
    REQUIRE(computed.ok());
    CHECK(computed.result.face_targets[0] != computed.result.face_targets[1]);
    CHECK(computed.result.targets.size() == 2);
    CHECK(computed.result.regions[1].subject_id == "person-1");
}

TEST_CASE("a protected multicolor region preserves a tiny contrasting detail", "[LocalPrintColorMatching]")
{
    auto input = matching_input(3);
    input.faces = {{{1, 0, 0}, 100}, {{1, 1, 1}, .01}, {{0, 0, 0}, 100}, {{0, 0, 1}, 100}};
    input.identity.face_count = input.faces.size();
    input.identity.regions = {{"shirt-emblem", "person-1", "shirt and emblem", 1, true, {0, 1}}};
    const auto computed = Matching::compute(input);
    REQUIRE(computed.ok());
    CHECK(computed.result.targets.size() <= 3);
    CHECK(computed.result.face_targets[0] != computed.result.face_targets[1]);
    const auto& detail = computed.result.targets[computed.result.face_targets[1]];
    REQUIRE(detail.executable);
    CHECK_THAT(Matching::delta_e(input.faces[1].color, detail.output), Catch::Matchers::WithinAbs(0, .02));
}

TEST_CASE("too many protected colors report a budget conflict without increasing the request", "[LocalPrintColorMatching]")
{
    auto input = matching_input(1);
    input.identity.user_overrides = {{0, {0, 0, 0}}, {1, {1, 1, 1}}};
    const auto computed = Matching::compute(input);
    CHECK_FALSE(computed.ok());
    CHECK(computed.result.requested_color_count == 1);
    CHECK(computed.error.find("Protected colors") != std::string::npos);
}

TEST_CASE("four physical channels do not become six channels or silently collapse six target groups", "[LocalPrintColorMatching]")
{
    auto input = matching_input();
    input.identity.physical_channels.resize(4);
    const auto computed = Matching::compute(input);
    REQUIRE(computed.ok());
    CHECK(computed.result.physical_channels.size() == 4);
    CHECK(computed.result.requested_color_count == 6);
    CHECK(computed.result.targets.size() == 6);
    CHECK(std::count_if(computed.result.targets.begin(), computed.result.targets.end(),
        [](const auto& t) { return t.executable; }) == 4);
    CHECK(computed.result.unresolved_count() >= 2);
    for (const auto& t : computed.result.targets) CHECK_FALSE(t.recipe);
}

TEST_CASE("near identical materials cannot satisfy a hard region contrast requirement", "[LocalPrintColorMatching]")
{
    auto input = matching_input(2);
    input.faces.resize(2); input.identity.face_count = 2;
    input.identity.physical_channels = {{0, "#FFFFFF", "PLA", true}, {1, "#FEFEFE", "PLA", true}};
    input.identity.regions = {{"dark", "person-1", "hair", 1, true, {0}}, {"light", "person-1", "skin", 1, true, {1}}};
    input.contrasts = {{"dark", "light", 1, 10, true}};
    const auto computed = Matching::compute(input);
    REQUIRE(computed.ok());
    CHECK(computed.result.unresolved_count() == 2);
    for (const auto& t : computed.result.targets) CHECK_FALSE(t.executable);
}

TEST_CASE("user material locks override automatic assignment and reject unavailable slots", "[LocalPrintColorMatching]")
{
    auto input = matching_input();
    input.identity.regions = {{"selected", "", "user", 1, true, {0}}};
    input.identity.regions[0].locked_physical_slot = 1;
    const auto computed = Matching::compute(input);
    REQUIRE(computed.ok());
    CHECK(computed.result.targets[computed.result.face_targets[0]].physical_slot == 1);
    input.identity.physical_channels[1].compatible = false;
    CHECK_FALSE(Matching::compute(input).ok());
}

TEST_CASE("cancellation preserves the source input without returning a confirmed result", "[LocalPrintColorMatching]")
{
    auto input = matching_input();
    input.cancelled = [] { return true; };
    const auto computed = Matching::compute(input);
    CHECK(computed.cancelled);
    CHECK_FALSE(computed.ok());
    CHECK_FALSE(computed.result.confirmed);
    CHECK(input.identity.targets.empty());
}

TEST_CASE("local color results reject stale partitions invalid recipes and unknown evidence claims", "[LocalPrintColorMatching][AIContracts]")
{
    auto result = Matching::compute(matching_input()).result;
    std::string error;
    REQUIRE(result.valid(error));
    SECTION("missing face") { result.face_targets.pop_back(); }
    SECTION("absent target") { result.face_targets[0] = 100; }
    SECTION("missing identity") { result.source_sha256.clear(); }
    SECTION("unavailable slot") { result.targets[0].physical_slot = 99; }
    SECTION("unknown cannot prove tolerance") { result.targets[0].evidence = AI::ColorEvidence::Unknown; }
    SECTION("invalid evidence enum") { result.targets[0].evidence = static_cast<AI::ColorEvidence>(50); }
    SECTION("direct output differs from material") { result.targets[0].output = {.12f, .34f, .56f}; }
    SECTION("direct mode cannot contain recipe") {
        result.targets[0].physical_slot.reset();
        result.targets[0].recipe = AI::MixedColorRecipe {"#808080", {{0, .5}, {1, .5}}, {}};
        result.targets[0].candidate_id = "candidate";
    }
    CHECK_FALSE(result.valid(error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("equal colors with different material locks retain independent targets", "[LocalPrintColorMatching]")
{
    auto input = matching_input(2);
    input.faces = {{{0, 0, 0}, 1}, {{0, 0, 0}, 1}};
    input.identity.face_count = 2;
    input.identity.regions = {{"a", "", "user", 1, true, {0}}, {"b", "", "user", 1, true, {1}}};
    input.identity.regions[0].locked_physical_slot = 0;
    input.identity.regions[1].locked_physical_slot = 1;
    auto computed = Matching::compute(input);
    REQUIRE(computed.ok());
    REQUIRE(computed.result.targets.size() == 2);
    CHECK(computed.result.targets[computed.result.face_targets[0]].physical_slot == 0);
    CHECK(computed.result.targets[computed.result.face_targets[1]].physical_slot == 1);
    auto& changed = computed.result.targets[computed.result.face_targets[1]];
    changed.physical_slot = 0; changed.output = {0, 0, 0};
    computed.result.confirmed = true;
    std::string error;
    CHECK_FALSE(computed.result.valid(error));
}

TEST_CASE("one region locked to one material can contain multiple source colors", "[LocalPrintColorMatching]")
{
    auto input = matching_input(1);
    input.faces.resize(2); input.identity.face_count = 2;
    input.identity.regions = {{"a", "", "user", 1, true, {0, 1}}};
    input.identity.regions[0].locked_physical_slot = 0;
    const auto computed = Matching::compute(input);
    REQUIRE(computed.ok());
    REQUIRE(computed.result.targets.size() == 1);
    CHECK(computed.result.targets.front().physical_slot == 0);
}

TEST_CASE("invalid region overlap duplicates and degenerate faces are explained", "[LocalPrintColorMatching]")
{
    auto input = matching_input();
    SECTION("duplicate face") { input.identity.regions = {{"a", "", "user", 1, true, {0, 0}}}; }
    SECTION("conflicting locks") {
        input.identity.regions = {{"a", "", "user", 1, true, {0}}, {"b", "", "user", 1, true, {0}}};
        input.identity.regions[0].locked_physical_slot = 0;
        input.identity.regions[1].locked_physical_slot = 1;
    }
    SECTION("zero area override") { input.faces[0].area = 0; input.identity.user_overrides = {{0, {1, 1, 1}}}; }
    const auto computed = Matching::compute(input);
    CHECK_FALSE(computed.ok());
    CHECK_FALSE(computed.error.empty());
}

TEST_CASE("original color metadata survives local recoloring", "[LocalPrintColorMatching]")
{
    auto input = matching_input();
    input.identity.source_color_count = 123456;
    input.identity.user_overrides = {{0, {1, 1, 1}}};
    const auto computed = Matching::compute(input);
    REQUIRE(computed.ok());
    CHECK(computed.result.source_color_count == 123456);
    CHECK(computed.result.sampled_source_color_count == 6);
}

TEST_CASE("local printing can quantize sixteen colors without changing legacy six color palettes", "[LocalPrintColorMatching][ModelPreviewPalette]")
{
    GUI::PreviewPalette::Histogram histogram;
    for (uint32_t i = 0; i < 16; ++i) histogram.add((i * 17) * 0x010101, 1);
    CHECK(histogram.palette(16).size() == 6);
    CHECK(histogram.print_palette(16).size() == 16);
    CHECK(histogram.print_palette(33).empty());
}

TEST_CASE("color version records reopen with the exact partition and region locks", "[LocalPrintColorMatching]")
{
    auto input = matching_input();
    input.identity.parent_version = "original-version";
    input.identity.user_overrides = {{0, {0, 0, 0}}};
    input.identity.regions = {{"hair", "person-1", "hair", .95, true, {0}}};
    input.identity.regions[0].locked_physical_slot = 0;
    input.identity.regions.push_back({"skin", "person-1", "skin", .9, true, {1}});
    input.contrasts = {{"hair", "skin", 1.5, 10, true}};
    input.tolerance = 4;
    const auto original = Matching::compute(input).result;
    const auto stored = GUI::LocalPrintColorState::encode(original).dump();
    AI::LocalPrintColorResult restored;
    std::string error;
    REQUIRE(GUI::LocalPrintColorState::decode(nlohmann::json::parse(stored), original.source_sha256,
        original.geometry_id, original.material_fingerprint, original.process_fingerprint, restored, error));
    CHECK(GUI::LocalPrintColorState::encode(restored).dump() == stored);
}

TEST_CASE("stale and malformed color records leave the confirmed destination unchanged", "[LocalPrintColorMatching]")
{
    auto destination = Matching::compute(matching_input()).result;
    destination.confirmed = true;
    const auto before = GUI::LocalPrintColorState::encode(destination);
    auto incoming = before;
    SECTION("geometry changed") { incoming["geometry_id"] = "changed"; }
    SECTION("materials changed") { incoming["material_fingerprint"] = "changed"; }
    SECTION("process changed") { incoming["process_fingerprint"] = "changed"; }
    SECTION("original changed") { incoming["source_sha256"] = std::string(64, 'b'); }
    SECTION("negative index") { incoming["face_targets"][0] = -1; }
    SECTION("fractional index") { incoming["face_targets"][0] = .5; }
    SECTION("unknown mode") { incoming["mode"] = "six-color"; }
    SECTION("stale algorithm") { incoming["algorithm_version"] = "old-v0"; }
    SECTION("unknown evidence") { incoming["targets"][0]["evidence"] = "AI approved"; }
    SECTION("incomplete partition") { incoming["face_targets"] = nlohmann::json::array(); }
    SECTION("extra source RGB channel") { incoming["targets"][0]["source"] = {0, 0, 0, 99}; }
    SECTION("extra output RGB channel") { incoming["targets"][0]["output"] = {0, 0, 0, 99}; }
    SECTION("extra override RGB channel") { incoming["user_overrides"] = {{0, {0, 0, 0, 99}}}; }
    std::string error;
    CHECK_FALSE(GUI::LocalPrintColorState::decode(incoming, destination.source_sha256,
        destination.geometry_id, destination.material_fingerprint, destination.process_fingerprint, destination, error));
    CHECK_FALSE(error.empty());
    CHECK(GUI::LocalPrintColorState::encode(destination) == before);
}

TEST_CASE("colors inside a material-locked region do not spend the free palette budget", "[LocalPrintColorMatching]")
{
    auto input = matching_input(2);
    input.faces = {{{0, 0, 0}, 10}, {{1, 1, 1}, 100}, {{0, 0, 0}, 1}}; input.identity.face_count = 3;
    input.identity.regions = {{"selected", "", "user", 1, true, {0, 1}}};
    input.identity.regions[0].locked_physical_slot = 2;
    const auto computed = Matching::compute(input);
    REQUIRE(computed.ok());
    CHECK(computed.result.targets[computed.result.face_targets[0]].physical_slot == 2);
    CHECK(computed.result.targets[computed.result.face_targets[1]].physical_slot == 2);
    CHECK(computed.result.targets[computed.result.face_targets[2]].physical_slot == 0);
}

TEST_CASE("different physical locks remain distinct even when installed colors are identical", "[LocalPrintColorMatching]")
{
    auto input = matching_input(2);
    input.faces = {{{0, 0, 0}, 1}, {{0, 0, 0}, 1}}; input.identity.face_count = 2;
    input.identity.physical_channels[1].display_color = "#000000";
    input.identity.regions = {{"a", "", "user", 1, true, {0}}, {"b", "", "user", 1, true, {1}}};
    input.identity.regions[0].locked_physical_slot = 0;
    input.identity.regions[1].locked_physical_slot = 1;
    const auto computed = Matching::compute(input);
    REQUIRE(computed.ok());
    CHECK(computed.result.targets[computed.result.face_targets[0]].physical_slot == 0);
    CHECK(computed.result.targets[computed.result.face_targets[1]].physical_slot == 1);
}

TEST_CASE("local material locks do not recolor unrelated faces with the same source color", "[LocalPrintColorMatching]")
{
    auto input = matching_input(2);
    input.faces = {{{0, 0, 0}, 1}, {{0, 0, 0}, 1}}; input.identity.face_count = 2;
    input.identity.regions = {{"selected", "", "user", 1, true, {0}}};
    input.identity.regions[0].locked_physical_slot = 2;
    const auto computed = Matching::compute(input);
    REQUIRE(computed.ok());
    CHECK(computed.result.targets[computed.result.face_targets[0]].physical_slot == 2);
    CHECK(computed.result.targets[computed.result.face_targets[1]].physical_slot == 0);
    CHECK(computed.result.targets[computed.result.face_targets[0]].delta_e00 > 10);
    input.identity.requested_color_count = 1;
    input.identity.regions[0].locked_physical_slot = 0;
    const auto same_color = Matching::compute(input);
    REQUIRE(same_color.ok());
    CHECK(same_color.result.targets.size() == 1);
}

TEST_CASE("automatic eye preferences retain white and pupil modes without reserving every shade", "[LocalPrintColorMatching]")
{
    auto input = matching_input(3);
    input.faces = {{{.8f, .6f, .5f}, 10000}, {{1, 1, 1}, .9}, {{0, 0, 0}, .1}};
    input.identity.face_count = input.faces.size();
    input.identity.physical_channels = {{0, "#CC9980", "PLA", true}, {1, "#FFFFFF", "PLA", true}, {2, "#000000", "PLA", true}};
    input.identity.regions = {{"eye", "person", "eye", 1, false, {1, 2}, true}};
    const auto computed = Matching::compute(input);
    REQUIRE(computed.ok());
    REQUIRE(computed.result.targets.size() == 3);
    CHECK(computed.result.face_targets[1] != computed.result.face_targets[2]);
    for (size_t f : {size_t(1), size_t(2)}) {
        const auto& target = computed.result.targets[computed.result.face_targets[f]];
        REQUIRE(target.executable);
        CHECK_THAT(Matching::delta_e(input.faces[f].color, target.output), Catch::Matchers::WithinAbs(0, .02));
    }
    CHECK_FALSE(computed.result.notices.empty());
}

TEST_CASE("automatic textured regions approximate within n while explicit protection remains strict", "[LocalPrintColorMatching]")
{
    auto input = matching_input(2);
    input.identity.regions = {{"eye-mouth", "person", "detail", 1, false, {0, 1, 2, 3, 4, 5}, true}};
    auto computed = Matching::compute(input);
    REQUIRE(computed.ok());
    CHECK(computed.result.targets.size() <= 2);
    CHECK(std::any_of(computed.result.notices.begin(), computed.result.notices.end(), [](const auto& notice) {
        return notice.find("requested color tolerance was not preserved") != std::string::npos;
    }));
    for (const auto& target : computed.result.targets) CHECK_FALSE(target.recipe);
    input.identity.regions[0].user_protected = true;
    computed = Matching::compute(input);
    CHECK_FALSE(computed.ok());
    CHECK(computed.error.find("Protected colors") != std::string::npos);
}

TEST_CASE("duplicating automatic region evidence does not amplify its color preference", "[LocalPrintColorMatching]")
{
    auto input = matching_input(3);
    input.faces[0].area = 1000;
    input.identity.regions = {{"eye", "person", "eye", 1, false, {1, 2, 3}, true}};
    const auto once = Matching::compute(input);
    REQUIRE(once.ok());
    for (size_t i = 0; i < 100; ++i) {
        auto region = input.identity.regions.front();
        region.id = "overlap-" + std::to_string(i);
        input.identity.regions.push_back(std::move(region));
    }
    const auto duplicated = Matching::compute(input);
    REQUIRE(duplicated.ok());
    CHECK(once.result.face_targets == duplicated.result.face_targets);
    REQUIRE(once.result.targets.size() == duplicated.result.targets.size());
    for (size_t t = 0; t < once.result.targets.size(); ++t)
        CHECK(once.result.targets[t].physical_slot == duplicated.result.targets[t].physical_slot);
}

TEST_CASE("inactive automatic preferences leave statistical matching unchanged", "[LocalPrintColorMatching]")
{
    auto input = matching_input(3);
    const auto baseline = Matching::compute(input);
    REQUIRE(baseline.ok());
    input.identity.regions = {{"uncertain", "person", "eye", .49, false, {0, 1}, true},
                             {"measurement-only", "person", "mouth", 1, false, {2, 3}, false}};
    const auto with_regions = Matching::compute(input);
    REQUIRE(with_regions.ok());
    CHECK(baseline.result.face_targets == with_regions.result.face_targets);
    REQUIRE(baseline.result.targets.size() == with_regions.result.targets.size());
    for (size_t t = 0; t < baseline.result.targets.size(); ++t)
        CHECK(baseline.result.targets[t].physical_slot == with_regions.result.targets[t].physical_slot);
}

TEST_CASE("automatic importance does not replace physical surface error with weighted quality", "[LocalPrintColorMatching]")
{
    auto input = matching_input(1);
    input.faces = {{{0, 0, 0}, 99}, {{1, 1, 1}, 1}};
    input.identity.face_count = 2;
    input.identity.regions = {{"eye", "person", "eye", 1, false, {1}, true}};
    input.identity.physical_channels = {{0, "#000000", "PLA", true}};
    const auto computed = Matching::compute(input);
    REQUIRE(computed.ok());
    const auto quality = GUI::LocalPrintColorQuality::evaluate(input.faces, computed.result);
    REQUIRE(quality.error.empty());
    const double expected = Matching::delta_e(input.faces[1].color, input.faces[0].color) / 100;
    CHECK_THAT(quality.mean_delta_e, Catch::Matchers::WithinAbs(expected, 1e-8));
    CHECK_THAT(computed.result.targets.front().delta_e00, Catch::Matchers::WithinAbs(expected, 1e-8));
    CHECK(quality.regions.front().worst_delta_e > 90);
    CHECK(std::any_of(computed.result.notices.begin(), computed.result.notices.end(), [](const auto& notice) {
        return notice.find("worst DeltaE00=") != std::string::npos;
    }));
}

TEST_CASE("automatic preferences cannot override an overlapping user material lock", "[LocalPrintColorMatching]")
{
    auto input = matching_input(2);
    input.faces = {{{0, 0, 0}, 1}, {{1, 1, 1}, 100}};
    input.identity.face_count = 2;
    input.identity.regions = {{"eye", "person", "eye", 1, false, {0}, true},
                             {"user-lock", "", "selection", 1, true, {0}}};
    input.identity.regions[1].locked_physical_slot = 2;
    const auto computed = Matching::compute(input);
    REQUIRE(computed.ok());
    CHECK(computed.result.targets[computed.result.face_targets[0]].physical_slot == 2);
    input.identity.physical_channels[2].compatible = false;
    CHECK_FALSE(Matching::compute(input).ok());
}

TEST_CASE("automatic color preferences depend on area rather than face subdivision", "[LocalPrintColorMatching]")
{
    auto input = matching_input(3);
    input.faces[0].area = 1000;
    input.identity.regions = {{"eye", "person", "eye", 1, false, {1, 2}, true}};
    const auto original = Matching::compute(input);
    REQUIRE(original.ok());
    const auto subdivided_color = input.faces[1].color;
    input.faces[1].area = .25;
    for (size_t i = 0; i < 3; ++i) {
        input.identity.regions[0].faces.push_back(input.faces.size());
        input.faces.push_back({subdivided_color, .25});
    }
    input.identity.face_count = input.faces.size();
    const auto subdivided = Matching::compute(input);
    REQUIRE(subdivided.ok());
    for (size_t f = 0; f < original.result.face_targets.size(); ++f) {
        const auto& a = original.result.targets[original.result.face_targets[f]];
        const auto& b = subdivided.result.targets[subdivided.result.face_targets[f]];
        CHECK(a.physical_slot == b.physical_slot);
    }
}

TEST_CASE("ordinary matching replaces an automatic candidate dominated on actual regional quality", "[LocalPrintColorMatching]")
{
    auto input = matching_input(2);
    const std::vector<std::string> colors {"#366FF1", "#676D01", "#393B1D", "#5AA042", "#E068EA", "#B40238", "#4E890A", "#F33A3F"};
    const std::vector<double> areas {9, 21, 53, 17, 18, 6, 72, 39};
    input.faces.clear();
    for (size_t i = 0; i < colors.size(); ++i) input.faces.push_back({Matching::hex_rgb(colors[i]), areas[i]});
    input.identity.face_count = input.faces.size();
    input.important_area_floor = .5;
    input.identity.regions = {{"detail", "person", "eye", 1, false, {0, 1, 2, 3, 4, 5, 6, 7}, false}};
    const auto baseline = Matching::compute(input);
    REQUIRE(baseline.ok());
    input.identity.regions[0].protect_color = true;
    const auto selected = Matching::compute(input);
    REQUIRE(selected.ok());
    CHECK(selected.result.face_targets == baseline.result.face_targets);
    CHECK(selected.result.regions[0].protect_color);
    CHECK_FALSE(selected.result.regions[0].user_protected);
    const auto q = GUI::LocalPrintColorQuality::evaluate(input.faces, selected.result);
    REQUIRE(q.error.empty());
    // Without the dominance check this fixed sample has mean 33.1624533779;
    // the ordinary partition has lower actual error with the same worst error.
    CHECK_THAT(q.mean_delta_e, Catch::Matchers::WithinAbs(33.03342789371995, 1e-8));
    CHECK_THAT(q.worst_delta_e, Catch::Matchers::WithinAbs(52.6019479766133, 1e-8));
    CHECK(std::any_of(selected.result.notices.begin(), selected.result.notices.end(), [](const auto& notice) {
        return notice.find("retained ordinary matching") != std::string::npos;
    }));
    CHECK(std::any_of(selected.result.notices.begin(), selected.result.notices.end(), [](const auto& notice) {
        return notice.find("requested color tolerance was not preserved") != std::string::npos;
    }));
}

TEST_CASE("ordinary matching cannot discard a real improvement in an automatic region", "[LocalPrintColorMatching]")
{
    auto input = matching_input(1);
    input.faces = {{{0, 0, 0}, 99}, {{1, 1, 1}, 1}};
    input.identity.face_count = 2;
    input.identity.physical_channels.resize(2);
    input.important_area_floor = 1;
    input.identity.regions = {{"eye", "person", "eye", 1, false, {1}, false}};
    const auto baseline = Matching::compute(input);
    REQUIRE(baseline.ok());
    CHECK(baseline.result.targets.front().physical_slot == 0);
    input.identity.regions[0].protect_color = true;
    const auto selected = Matching::compute(input);
    REQUIRE(selected.ok());
    CHECK(selected.result.targets.front().physical_slot == 1);
    CHECK(std::none_of(selected.result.notices.begin(), selected.result.notices.end(), [](const auto& notice) {
        return notice.find("retained ordinary matching") != std::string::npos;
    }));
}
