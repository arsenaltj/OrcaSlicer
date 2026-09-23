#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "slic3r/GUI/AI/Orca/LocalPrintColorApplication.hpp"
#include "slic3r/GUI/AI/Orca/LocalPrintColorCommit.hpp"
#include "slic3r/GUI/AI/Orca/LocalPrintModelImport.hpp"
#include "slic3r/GUI/AI/Orca/LocalPrintRecipeApplication.hpp"
#include "slic3r/GUI/AI/ModelGeneration/LocalPrintColorRecipes.hpp"
#include <catch2/generators/catch_generators.hpp>
#include "slic3r/GUI/AI/ModelGeneration/LocalPrintColorMatching.hpp"
#include "slic3r/GUI/AI/ModelGeneration/LocalPrintColorQuality.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Semver.hpp"
#include "test_utils.hpp"
#include "local_print_recipe_fixture.hpp"
#include "slic3r/GUI/AI/Orca/LocalPrintRecipeTransition.hpp"
#include "slic3r/GUI/AI/Orca/LocalPrintColorRestore.hpp"
#include "slic3r/GUI/AI/Model/LocalPrintColorState.hpp"

using namespace Slic3r;
namespace Application = GUI::LocalPrintColorApplication;

TEST_CASE("new color models and recipe configuration are adopted together without changing existing objects", "[LocalPrintModelImport]")
{
    Test::RecipeApplicationFixture fixture(GENERATE(2u, 3u));
    Model source; auto* incoming = source.add_object();
    incoming->input_file = "confirmed.glb";
    auto* part = incoming->add_volume(TriangleMesh(fixture.mesh), ModelVolumeType::MODEL_PART, false);
    part->source.input_file = incoming->input_file;
    GUI::LocalPrintRecipeApplication::Prepared recipe; std::string error;
    REQUIRE(GUI::LocalPrintRecipeApplication::prepare(part->mesh(), fixture.mesh.its, fixture.result,
        fixture.snapshot, fixture.bundle, recipe, error));
    const auto paint = recipe.painting.data;
    REQUIRE(GUI::LocalPrintRecipeApplication::apply_painting(*part, std::move(recipe.painting), error));
    std::unique_ptr<Model> prepared;
    REQUIRE(GUI::LocalPrintModelImport::prepare(*incoming, {75, 85}, {250, 250}, prepared, error));
    REQUIRE(incoming->instances.empty());
    CHECK(part->mesh().its.vertices == fixture.mesh.its.vertices);
    Model live; auto* previous = live.add_object();
    previous->name = "Existing object";
    previous->add_volume(TriangleMesh(its_make_cube(5, 6, 7)));
    auto* instance = previous->add_instance(); instance->set_offset({13, 17, 19});
    const auto id = previous->id(); const auto matrix = instance->get_matrix();
    const bool assembled = instance->is_assemble_initialized();
    const auto before = GUI::LocalPrintRecipeApplication::identity(fixture.bundle);
    UndoRedo::ProjectConfigUndo::Prepared config {recipe.bundle->project_config, recipe.bundle->filament_presets, true};
    auto cache = GUI::ProjectConfigRestore::prepare_cache(config.filament_presets.size(), fixture.bundle);
    size_t index = 999; int recorded = 0;
    REQUIRE(GUI::LocalPrintModelImport::adopt(live, *prepared->objects.front(), config, cache, fixture.bundle, [&] {
        ++recorded;
        CHECK(live.objects.size() == 2);
        CHECK(GUI::LocalPrintRecipeApplication::identity(fixture.bundle) == before);
        return true;
    }, index, error));
    REQUIRE(index == 1); CHECK(recorded == 1);
    CHECK(live.objects[0] == previous); CHECK(previous->id() == id);
    CHECK(instance->get_matrix().matrix() == matrix.matrix()); CHECK(instance->is_assemble_initialized() == assembled);
    CHECK(live.objects[index]->get_model() == &live);
    CHECK(live.objects[index]->volumes[0]->get_object() == live.objects[index]);
    CHECK(live.objects[index]->volumes[0]->mmu_segmentation_facets.get_data() == paint);
    CHECK(live.objects[index]->input_file == "confirmed.glb");
    CHECK_THAT(live.objects[index]->instances[0]->get_offset().x(), Catch::Matchers::WithinAbs(75., 1e-12));
    CHECK_THAT(live.objects[index]->instances[0]->get_offset().y(), Catch::Matchers::WithinAbs(85., 1e-12));
    CHECK_THAT(live.objects[index]->min_z(), Catch::Matchers::WithinAbs(0., 1e-9));
    CHECK(fixture.bundle.project_config == recipe.bundle->project_config);
    CHECK(fixture.bundle.filament_presets == recipe.bundle->filament_presets);
}

TEST_CASE("failed color import history registration removes only the new object and leaves materials unchanged", "[LocalPrintModelImport]")
{
    const bool throws = GENERATE(false, true);
    Test::RecipeApplicationFixture fixture(2);
    Model source; auto* incoming = source.add_object(); incoming->input_file = "confirmed.glb";
    auto* part = incoming->add_volume(TriangleMesh(fixture.mesh), ModelVolumeType::MODEL_PART, false);
    part->source.input_file = incoming->input_file;
    GUI::LocalPrintRecipeApplication::Prepared recipe; std::string error;
    REQUIRE(GUI::LocalPrintRecipeApplication::prepare(part->mesh(), fixture.mesh.its, fixture.result,
        fixture.snapshot, fixture.bundle, recipe, error));
    REQUIRE(GUI::LocalPrintRecipeApplication::apply_painting(*part, std::move(recipe.painting), error));
    std::unique_ptr<Model> prepared;
    REQUIRE(GUI::LocalPrintModelImport::prepare(*incoming, {25, 35}, {250, 250}, prepared, error));
    Model live; auto* previous = live.add_object(); previous->name = "Retain this";
    const auto before = GUI::LocalPrintRecipeApplication::identity(fixture.bundle);
    UndoRedo::ProjectConfigUndo::Prepared config {recipe.bundle->project_config, recipe.bundle->filament_presets, true};
    auto cache = GUI::ProjectConfigRestore::prepare_cache(config.filament_presets.size(), fixture.bundle);
    size_t index = 999;
    auto attempt = [&] { return GUI::LocalPrintModelImport::adopt(live, *prepared->objects.front(), config, cache, fixture.bundle,
        [&] { if (throws) throw std::runtime_error("Injected registration failure"); return false; }, index, error); };
    if (throws) CHECK_THROWS_AS(attempt(), std::runtime_error); else CHECK_FALSE(attempt());
    REQUIRE(live.objects.size() == 1); CHECK(live.objects.front() == previous);
    CHECK(previous->name == "Retain this"); CHECK(index == 999); CHECK(config.changed);
    CHECK(GUI::LocalPrintRecipeApplication::identity(fixture.bundle) == before);
}

TEST_CASE("invalid color import placement is rejected before preparing a live model", "[LocalPrintModelImport]")
{
    Model source; auto* incoming = source.add_object(); incoming->input_file = "original.glb";
    auto* volume = incoming->add_volume(TriangleMesh(its_make_cube(10, 10, 10))); volume->source.input_file = incoming->input_file;
    auto bed = Vec2d(250, 250); auto placement = Vec2d(50, 50);
    const auto fault = GENERATE(0, 1, 2, 3);
    if (fault == 0) bed.x() = 0;
    if (fault == 1) incoming->add_instance();
    if (fault == 2) incoming->input_file.clear();
    if (fault == 3) bed = Vec2d(.1, .1);
    std::unique_ptr<Model> prepared; std::string error;
    CHECK_FALSE(GUI::LocalPrintModelImport::prepare(*incoming, placement, bed, prepared, error));
    CHECK_FALSE(prepared); CHECK_FALSE(error.empty());
    REQUIRE(source.objects.size() == 1); CHECK(source.objects.front() == incoming);
}

TEST_CASE("prepared color commit moves native recipe slots and painting together without changing geometry", "[LocalPrintColorCommit]")
{
    Test::RecipeApplicationFixture fixture(GENERATE(2u, 3u));
    Model model; auto* object = model.add_object();
    auto* volume = object->add_volume(TriangleMesh(fixture.mesh), ModelVolumeType::MODEL_PART, false);
    auto* instance = object->add_instance(); instance->set_offset(Vec3d(11, 17, 23));
    volume->source.input_file = "original.glb";
    volume->config.set("layer_height", 0.24); object->config.set("wall_loops", 7);
    const auto vertices = volume->mesh().its.vertices;
    const auto config_id = volume->config.id(), object_config_id = object->config.id();
    const auto volume_id = volume->id();
    const auto live_config = fixture.bundle.project_config;
    GUI::LocalPrintRecipeApplication::Prepared recipe; std::string error;
    REQUIRE(GUI::LocalPrintRecipeApplication::prepare(volume->mesh(), fixture.mesh.its, fixture.result,
        fixture.snapshot, fixture.bundle, recipe, error));
    const auto expected_paint = recipe.painting.data;
    const auto expected_config = recipe.bundle->project_config;
    GUI::LocalPrintColorCommit::Prepared prepared;
    REQUIRE(GUI::LocalPrintColorCommit::prepare(*volume, std::move(recipe.painting), "version.glb", prepared, error));
    REQUIRE(GUI::LocalPrintColorCommit::validate(*volume, prepared, error));
    CHECK(volume->source.input_file == "original.glb");
    CHECK(volume->mmu_segmentation_facets.empty());
    CHECK(fixture.bundle.project_config == live_config);
    UndoRedo::ProjectConfigUndo::Prepared config {recipe.bundle->project_config, recipe.bundle->filament_presets, true};
    auto cache = GUI::ProjectConfigRestore::prepare_cache(config.filament_presets.size(), fixture.bundle);
    GUI::LocalPrintColorCommit::commit(*volume, prepared, config, cache, fixture.bundle);
    CHECK(prepared.consumed); CHECK_FALSE(config.changed);
    CHECK(fixture.bundle.project_config == expected_config);
    CHECK(volume->mmu_segmentation_facets.get_data() == expected_paint);
    CHECK(volume->source.input_file == "version.glb");
    CHECK(volume->config.extruder() == object->config.extruder());
    CHECK_THAT(volume->config.opt_float("layer_height"), Catch::Matchers::WithinAbs(0.24, 1e-12));
    CHECK(object->config.opt_int("wall_loops") == 7);
    CHECK(volume->config.id() == config_id); CHECK(object->config.id() == object_config_id);
    CHECK(volume->id() == volume_id); CHECK(volume->mesh().its.vertices == vertices);
    CHECK(instance->get_offset() == Vec3d(11, 17, 23));
    // Reusing the consumed state cannot toggle the configuration back.
    config.changed = true;
    GUI::LocalPrintColorCommit::commit(*volume, prepared, config, cache, fixture.bundle);
    CHECK(fixture.bundle.project_config == expected_config);
    CHECK(volume->mmu_segmentation_facets.get_data() == expected_paint);
    CHECK_FALSE(GUI::LocalPrintColorCommit::validate(*volume, prepared, error));
}

TEST_CASE("stale prepared color changes reject before modifying model state", "[LocalPrintColorCommit]")
{
    Test::RecipeApplicationFixture fixture(2);
    Model model; auto* object = model.add_object();
    auto* volume = object->add_volume(TriangleMesh(fixture.mesh), ModelVolumeType::MODEL_PART, false);
    volume->source.input_file = "original.glb";
    GUI::LocalPrintRecipeApplication::Prepared recipe; std::string error;
    REQUIRE(GUI::LocalPrintRecipeApplication::prepare(volume->mesh(), fixture.mesh.its, fixture.result,
        fixture.snapshot, fixture.bundle, recipe, error));
    GUI::LocalPrintColorCommit::Prepared prepared;
    REQUIRE(GUI::LocalPrintColorCommit::prepare(*volume, std::move(recipe.painting), "version.glb", prepared, error));
    const int fault = GENERATE(0, 1, 2, 3, 4, 5);
    if (fault == 0) volume->source.input_file = "manual.glb";
    if (fault == 1) volume->config.set("extruder", 2);
    if (fault == 2) object->config.set("extruder", 2);
    if (fault == 3) volume->mmu_segmentation_facets.reset();
    if (fault == 4) object->add_volume(TriangleMesh(fixture.mesh), ModelVolumeType::MODEL_PART, false);
    if (fault == 5) prepared.painting.geometry_id = "changed";
    const auto source = volume->source.input_file;
    const auto config = volume->config.get(), object_config = object->config.get();
    const auto paint = volume->mmu_segmentation_facets.get_data();
    REQUIRE_FALSE(GUI::LocalPrintColorCommit::validate(*volume, prepared, error));
    CHECK_FALSE(error.empty()); CHECK_FALSE(prepared.consumed);
    CHECK(volume->source.input_file == source); CHECK(volume->config.get() == config);
    CHECK(object->config.get() == object_config); CHECK(volume->mmu_segmentation_facets.get_data() == paint);
}

TEST_CASE("multi volume color commit retains object routing and other parts", "[LocalPrintColorCommit]")
{
    Test::RecipeApplicationFixture fixture(2);
    Model model; auto* object = model.add_object();
    auto* volume = object->add_volume(TriangleMesh(fixture.mesh), ModelVolumeType::MODEL_PART, false);
    auto* other = object->add_volume(TriangleMesh(fixture.mesh), ModelVolumeType::MODEL_PART, false);
    object->config.set("extruder", 2); other->source.input_file = "other.glb";
    const auto config = object->config.get();
    GUI::LocalPrintRecipeApplication::Prepared recipe; std::string error;
    REQUIRE(GUI::LocalPrintRecipeApplication::prepare(volume->mesh(), fixture.mesh.its, fixture.result,
        fixture.snapshot, fixture.bundle, recipe, error));
    GUI::LocalPrintColorCommit::Prepared prepared;
    REQUIRE(GUI::LocalPrintColorCommit::prepare(*volume, std::move(recipe.painting), "version.glb", prepared, error));
    REQUIRE(GUI::LocalPrintColorCommit::validate(*volume, prepared, error));
    GUI::LocalPrintColorCommit::commit(*volume, prepared);
    CHECK(object->config.get() == config); CHECK(other->mmu_segmentation_facets.empty());
    CHECK(other->source.input_file == "other.glb"); CHECK(other->config.empty());
}

static AI::LocalPrintColorResult exact_cube_result(const TriangleMesh& mesh)
{
    AI::LocalPrintColorResult result;
    result.algorithm_version = "region-direct-v1";
    result.source_sha256 = std::string(64, 'a');
    result.geometry_id = AI::SurfaceSelectionPersistence::geometry_fingerprint(mesh.its);
    result.material_fingerprint = "materials"; result.process_fingerprint = "process";
    result.requested_color_count = 2; result.face_count = mesh.its.indices.size();
    // Identical RGB values intentionally refer to distinct actual project slots.
    result.physical_channels = {{0, "#000000", "PLA", true}, {7, "#000000", "PLA", true}};
    for (size_t slot : {size_t(0), size_t(7)}) {
        AI::PrintColorTarget target;
        target.source = target.output = {0, 0, 0}; target.area = 1;
        target.physical_slot = slot; target.executable = target.within_tolerance = true;
        target.evidence = AI::ColorEvidence::Estimated;
        result.targets.push_back(target);
    }
    for (size_t f = 0; f < result.face_count; ++f) result.face_targets.push_back(f % 2);
    result.confirmed = true;
    return result;
}

TEST_CASE("layered requests can apply shared physical approximations without inventing tool slots", "[LocalPrintColorApplication]")
{
    TriangleMesh mesh(its_make_cube(10, 10, 10));
    auto result = exact_cube_result(mesh);
    result.requested_color_count = 8; result.mode = AI::PrintColorMode::Layered;
    result.algorithm_version = "region-layered-v1";
    const auto two = result.targets;
    result.targets.clear();
    for (size_t t = 0; t < 8; ++t) result.targets.push_back(two[t%2]);
    for (size_t f = 0; f < result.face_count; ++f) result.face_targets[f] = f%8;
    Application::PreparedPainting painting; std::string error;
    REQUIRE(Application::prepare(mesh, result, "materials", "process", painting, error));
    TriangleSelector selector(mesh); selector.deserialize(painting.data, true);
    const auto data = selector.serialize();
    CHECK_FALSE(data.triangles_to_split.empty());
    CHECK(std::count(data.used_states.begin(), data.used_states.end(), true) == 2);
    CHECK(data.used_states[size_t(EnforcerBlockerType::Extruder1)]);
    CHECK(data.used_states[size_t(EnforcerBlockerType::Extruder8)]);
    CHECK(result.targets.size() == 8);
    CHECK(result.physical_channels.size() == 2);
}

TEST_CASE("exact color painting preserves same RGB slot identity and unrelated annotations", "[LocalPrintColorApplication]")
{
    Model model;
    auto* object = model.add_object();
    auto* volume = object->add_volume(TriangleMesh(its_make_cube(10, 10, 10)), ModelVolumeType::MODEL_PART, false);
    const auto before = volume->mesh().its;
    const auto object_id = object->id(), volume_id = volume->id();
    TriangleSelector annotations(volume->mesh());
    annotations.set_facet(0, EnforcerBlockerType::ENFORCER);
    volume->supported_facets.set(annotations); volume->seam_facets.set(annotations); volume->fuzzy_skin_facets.set(annotations);
    const auto supports = volume->supported_facets.get_triangle_as_string(0);
    const auto seams = volume->seam_facets.get_triangle_as_string(0);
    const auto fuzzy = volume->fuzzy_skin_facets.get_triangle_as_string(0);
    auto result = exact_cube_result(volume->mesh());
    Application::PreparedPainting painting;
    std::string error;
    REQUIRE(Application::prepare(volume->mesh(), result, "materials", "process", painting, error));
    REQUIRE(Application::apply(*volume, std::move(painting), error));
    CHECK(volume->mmu_segmentation_facets.get_facets(*volume, EnforcerBlockerType::Extruder1).indices.size() == 6);
    CHECK(volume->mmu_segmentation_facets.get_facets(*volume, EnforcerBlockerType::Extruder8).indices.size() == 6);
    CHECK(volume->mesh().its.vertices == before.vertices);
    CHECK(volume->mesh().its.indices == before.indices);
    CHECK(volume->supported_facets.get_triangle_as_string(0) == supports);
    CHECK(volume->seam_facets.get_triangle_as_string(0) == seams);
    CHECK(volume->fuzzy_skin_facets.get_triangle_as_string(0) == fuzzy);
    CHECK(object->id() == object_id); CHECK(volume->id() == volume_id);
}

TEST_CASE("invalid or stale exact painting never changes the destination", "[LocalPrintColorApplication]")
{
    const TriangleMesh mesh(its_make_cube(10, 10, 10));
    auto result = exact_cube_result(mesh);
    SECTION("not confirmed") { result.confirmed = false; }
    SECTION("changed process") { result.process_fingerprint = "changed"; }
    SECTION("changed materials") { result.material_fingerprint = "changed"; }
    SECTION("changed mesh") { result.geometry_id = std::string(64, 'b'); }
    SECTION("unsupported physical index") { result.physical_channels[1].slot = 99; result.targets[1].physical_slot = 99; }
    Application::PreparedPainting destination;
    destination.geometry_id = "previous";
    std::string error;
    CHECK_FALSE(Application::prepare(mesh, result, "materials", "process", destination, error));
    CHECK(destination.geometry_id == "previous");
    CHECK_FALSE(error.empty());
}

TEST_CASE("quality evaluation exposes source detail lost behind an exact palette centroid", "[LocalPrintColorApplication]")
{
    const TriangleMesh mesh(its_make_cube(10, 10, 10));
    auto result = exact_cube_result(mesh);
    std::vector<GUI::LocalPrintColorMatching::FaceSample> samples;
    for (size_t f = 0; f < result.face_count; ++f) samples.push_back({{1, 1, 1}, 1});
    auto quality = GUI::LocalPrintColorQuality::evaluate(samples, result);
    REQUIRE(quality.error.empty());
    CHECK(quality.mean_delta_e > 90);
    CHECK(quality.p95_delta_e > 90);
    CHECK(quality.has_covered_samples);
    CHECK_THAT(quality.unresolved_area_fraction, Catch::Matchers::WithinAbs(0, 1e-12));
    CHECK_THAT(quality.over_tolerance_area_fraction, Catch::Matchers::WithinAbs(1, 1e-12));
}

TEST_CASE("unassigned surfaces do not claim available zero error metrics", "[LocalPrintColorApplication]")
{
    const TriangleMesh mesh(its_make_cube(10, 10, 10));
    auto result = exact_cube_result(mesh);
    result.confirmed = false;
    for (auto& target : result.targets) {
        target.physical_slot.reset(); target.executable = false; target.within_tolerance = false;
        target.evidence = AI::ColorEvidence::Unknown; target.unresolved_reason = "No compatible material.";
    }
    std::vector<GUI::LocalPrintColorMatching::FaceSample> samples(result.face_count, {{1, 1, 1}, 1});
    const auto quality = GUI::LocalPrintColorQuality::evaluate(samples, result);
    REQUIRE(quality.error.empty());
    CHECK_FALSE(quality.has_covered_samples);
    CHECK_THAT(quality.unresolved_area_fraction, Catch::Matchers::WithinAbs(1, 1e-12));
    CHECK_THAT(quality.over_tolerance_area_fraction, Catch::Matchers::WithinAbs(0, 1e-12));
}

TEST_CASE("source face correspondence rejects edits beyond native recentering", "[LocalPrintColorApplication]")
{
    const TriangleMesh source(its_make_cube(10, 20, 30));
    auto target = source.its;
    for (auto& vertex : target.vertices) vertex -= Vec3f(5, 10, 15);
    REQUIRE(Application::same_surface_partition(source.its, target));
    SECTION("a vertex moves") { target.vertices[0].x() += 0.01f; }
    SECTION("face order changes") { std::swap(target.indices[0], target.indices[1]); }
    SECTION("winding changes") { std::swap(target.indices[0][0], target.indices[0][1]); }
    SECTION("scale changes") { for (auto& vertex : target.vertices) vertex *= 1.01f; }
    SECTION("an extra translation changes the binding") { for (auto& vertex : target.vertices) vertex.x() += 1.f; }
    CHECK_FALSE(Application::same_surface_partition(source.its, target));
    auto result = exact_cube_result(source);
    Application::PreparedPainting painting; painting.geometry_id = "unchanged";
    std::string error;
    CHECK_FALSE(Application::prepare_from_source(TriangleMesh(target), source.its, result, "materials", "process", painting, error));
    CHECK(painting.geometry_id == "unchanged");
}

TEST_CASE("saved local color groups bind to the recentered mesh after a real 3MF round trip", "[LocalPrintColorApplication]")
{
    Model model;
    auto* object = model.add_object(); object->name = "color round trip";
    TriangleMesh source(its_make_cube(10, 20, 30)); source.translate(Vec3f(0.1f, 0.2f, 0.3f));
    auto* volume = object->add_volume(TriangleMesh(source), ModelVolumeType::MODEL_PART, false);
    volume->source.input_file = "orca-color-round-trip.glb";
    object->add_instance()->set_offset(Vec3d(110, 75, 0));
    auto result = exact_cube_result(source);
    result.physical_channels[1].slot = 1; result.targets[1].physical_slot = 1;
    Application::PreparedPainting painting; std::string error;
    REQUIRE(Application::prepare_from_source(volume->mesh(), source.its, result, "materials", "process", painting, error));
    REQUIRE(Application::apply(*volume, std::move(painting), error));
    ScopedTemporaryDir backup("local-color-source"); model.set_backup_path(backup.string());
    ScopedTemporaryFile file(".3mf"); const std::string path = file.string();
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("filament_colour", new ConfigOptionStrings({"#000000", "#000000"}));
    PlateData plate; plate.plate_index = 0;
    StoreParams params; params.path = path.c_str(); params.model = &model; params.config = &config;
    params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence; params.plate_data_list.push_back(&plate);
    REQUIRE(store_bbs_3mf(params));
    Model restored; ScopedTemporaryDir restored_backup("local-color-restored"); restored.set_backup_path(restored_backup.string());
    DynamicPrintConfig restored_config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Enable};
    PlateDataPtrs plates; std::vector<Preset*> presets; bool is_bbl = false, is_orca = false; Semver version;
    const bool loaded = load_bbs_3mf(path.c_str(), &restored_config, &substitutions, &restored, &plates,
        &presets, &is_bbl, &is_orca, &version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig);
    release_PlateData_list(plates); for (auto* preset : presets) delete preset;
    REQUIRE(loaded); REQUIRE(restored.objects.size() == 1); REQUIRE(restored.objects.front()->volumes.size() == 1);
    auto* reopened = restored.objects.front()->volumes.front();
    CHECK(AI::SurfaceSelectionPersistence::geometry_fingerprint(reopened->mesh().its) != result.geometry_id);
    REQUIRE(Application::same_surface_partition(source.its, reopened->mesh().its));
    CHECK(Application::matches_saved_painting(reopened->mesh(), source.its, reopened->mmu_segmentation_facets.get_data(), result));
    REQUIRE(Application::prepare_from_source(reopened->mesh(), source.its, result, "materials", "process", painting, error));
    const auto native_geometry = AI::SurfaceSelectionPersistence::geometry_fingerprint(reopened->mesh().its);
    const auto transform = reopened->get_matrix();
    REQUIRE(Application::apply(*reopened, std::move(painting), error));
    CHECK(AI::SurfaceSelectionPersistence::geometry_fingerprint(reopened->mesh().its) == native_geometry);
    CHECK_THAT((reopened->get_matrix().matrix() - transform.matrix()).norm(), Catch::Matchers::WithinAbs(0, 1e-12));
    CHECK(reopened->mmu_segmentation_facets.get_facets(*reopened, EnforcerBlockerType::Extruder1).indices.size() == 6);
    CHECK(reopened->mmu_segmentation_facets.get_facets(*reopened, EnforcerBlockerType::Extruder2).indices.size() == 6);
}

TEST_CASE("native float recentering preserves a small model far from the origin", "[LocalPrintColorApplication]")
{
    auto source_geometry = its_make_cube(20, 20, 20);
    for (auto& vertex : source_geometry.vertices) vertex.x() = vertex.x() < 10.f ? 10000.001f : 10020.f;
    const TriangleMesh source(source_geometry);
    Model model;
    auto* object = model.add_object();
    auto* centered = object->add_volume(TriangleMesh(source));
    REQUIRE(Application::same_surface_partition(source.its, centered->mesh().its));
    auto deformed = centered->mesh().its;
    deformed.vertices.front().y() += 0.01f;
    CHECK_FALSE(Application::same_surface_partition(source.its, deformed));
}

TEST_CASE("manual native painting changes prevent direct restoration of an older confirmation", "[LocalPrintColorApplication][LocalPrintColorRestore]")
{
    const TriangleMesh mesh(its_make_cube(10,10,10));
    const auto saved=exact_cube_result(mesh);
    Application::PreparedPainting painting; std::string error;
    REQUIRE(Application::prepare(mesh,saved,"materials","process",painting,error));
    REQUIRE(Application::matches_saved_painting(mesh,mesh.its,painting.data,saved));
    // Serialized used-state bookkeeping is a cache, not a different painting.
    auto bookkeeping=painting.data; bookkeeping.reset_used_states();
    CHECK(Application::matches_saved_painting(mesh,mesh.its,bookkeeping,saved));
    TriangleSelector edited(mesh); edited.deserialize(painting.data);
    edited.set_facet(0,EnforcerBlockerType::Extruder8);
    CHECK_FALSE(Application::matches_saved_painting(mesh,mesh.its,edited.serialize(),saved));
    CHECK(Application::matches_saved_painting(mesh,mesh.its,painting.data,saved));
}

using Test::RecipeApplicationFixture;

static void capture_native_recipe_fixture(RecipeApplicationFixture& fixture,size_t components,AI::ColorEvidence evidence)
{
    auto& bundle=fixture.bundle;
    auto& printer=bundle.printers.get_edited_preset().config;
    printer.set_key_value("nozzle_diameter",new ConfigOptionFloats({.4}));
    printer.set_key_value("min_layer_height",new ConfigOptionFloats({.04}));
    printer.set_key_value("max_layer_height",new ConfigOptionFloats({.3}));
    auto& print=bundle.prints.get_edited_preset().config;
    print.set_key_value("layer_height",new ConfigOptionFloat(.2));
    print.set_key_value("line_width",new ConfigOptionFloatOrPercent(.4,false));
    print.set_key_value("enable_mixed_color_sublayer",new ConfigOptionBool(true));
    for(size_t i=0;i<bundle.filament_presets.size();++i) {
        DynamicPrintConfig config=bundle.filaments.get_edited_preset().config;
        config.set_key_value("filament_type",new ConfigOptionStrings({"PLA"}));
        config.set_key_value("nozzle_temperature",new ConfigOptionInts({210}));
        config.set_key_value("nozzle_temperature_range_low",new ConfigOptionInts({190}));
        config.set_key_value("nozzle_temperature_range_high",new ConfigOptionInts({230}));
        const auto name="Synthetic transition material "+std::to_string(i);
        bundle.filaments.load_preset("",name,config,false);
        bundle.filament_presets[i]=name;
    }
    const auto supplied=fixture.snapshot.sublayer_process;
    fixture.snapshot=GUI::OrcaPrintPaletteSnapshot::capture(bundle);
    // Only missing fields use explicit synthetic evidence; all native values
    // and fingerprints above come from the real capture implementation.
    GUI::LocalPrintRecipeTransition::retain_surface_constraints(fixture.snapshot.sublayer_process,supplied);
    auto input=GUI::LocalPrintColorRecipes::from_workspace(fixture.snapshot);
    auto catalog=GUI::LocalPrintColorRecipes::enumerate(input);REQUIRE(catalog.ok());
    const auto found=std::find_if(catalog.candidates.begin(),catalog.candidates.end(),[&](const auto& c){return c.recipe.components.size()==components;});
    REQUIRE(found!=catalog.candidates.end());
    auto candidate=*found;
    if(evidence!=AI::ColorEvidence::Estimated) {
        input.calibration.push_back({candidate.id,"synthetic-observation",std::string(64,'d'),evidence,{.23f,.37f,.41f},1.25});
        catalog=GUI::LocalPrintColorRecipes::enumerate(input);REQUIRE(catalog.ok());
        candidate=*std::find_if(catalog.candidates.begin(),catalog.candidates.end(),[&](const auto& c){return c.id==candidate.id;});
    }
    auto& result=fixture.result;
    result.material_fingerprint=fixture.snapshot.material_fingerprint;result.process_fingerprint=fixture.snapshot.process_fingerprint;
    result.physical_channels=fixture.snapshot.physical_channels;
    for(auto& target:result.targets) {
        target.source=target.output=candidate.color;target.recipe=candidate.recipe;target.recipe_proof=candidate.proof;
        target.candidate_id=candidate.id;target.evidence=candidate.evidence;
    }
}

TEST_CASE("native recipe slot staging rebuilds condition-bound proofs while retaining exact confirmed intent", "[LocalPrintRecipeTransition]")
{
    const auto components=GENERATE(2u,3u);
    const auto evidence=GENERATE(AI::ColorEvidence::Estimated,AI::ColorEvidence::Measured,AI::ColorEvidence::Interpolated);
    RecipeApplicationFixture fixture(components);capture_native_recipe_fixture(fixture,components,evidence);
    namespace State=GUI::LocalPrintColorState;
    namespace Transition=GUI::LocalPrintRecipeTransition;
    const auto original=State::encode(fixture.result);
    const auto live_identity=GUI::LocalPrintRecipeApplication::identity(fixture.bundle);
    Transition::Prepared staged;std::string error;
    REQUIRE(Transition::prepare(fixture.mesh,fixture.mesh.its,fixture.result,fixture.snapshot,fixture.bundle,{},staged,error));
    CHECK(State::encode(fixture.result)==original);
    CHECK(GUI::LocalPrintRecipeApplication::identity(fixture.bundle)==live_identity);
    CHECK(staged.native.added_slots==1);
    CHECK(staged.native.target_slots==std::vector<size_t>{3,3});
    CHECK(staged.result.material_fingerprint!=fixture.result.material_fingerprint);
    CHECK(staged.result.process_fingerprint!=fixture.result.process_fingerprint);
    CHECK(staged.result.face_targets==fixture.result.face_targets);
    CHECK(staged.result.confirmed);
    auto expected=original;
    expected["material_fingerprint"]=staged.result.material_fingerprint;
    expected["process_fingerprint"]=staged.result.process_fingerprint;
    const auto encoded=State::encode(staged.result);
    for(size_t i=0;i<staged.result.targets.size();++i) {
        const auto& target=staged.result.targets[i];const auto& old=fixture.result.targets[i];
        CHECK(target.candidate_id!=old.candidate_id);
        CHECK(target.recipe_proof->checksum!=old.recipe_proof->checksum);
        CHECK(target.output==old.output);CHECK(target.evidence==old.evidence);
        REQUIRE(GUI::LocalPrintRecipeProofState::valid(target,staged.result,error));
        expected["targets"][i]["candidate_id"]=encoded["targets"][i]["candidate_id"];
        expected["targets"][i]["recipe_proof"]=encoded["targets"][i]["recipe_proof"];
    }
    CHECK(encoded==expected);
    AI::LocalPrintColorResult decoded;
    REQUIRE(State::decode(encoded,staged.result.source_sha256,staged.result.geometry_id,staged.result.material_fingerprint,
        staged.result.process_fingerprint,decoded,error,staged.result.algorithm_version));
    Transition::Prepared reused;
    REQUIRE(Transition::prepare(fixture.mesh,fixture.mesh.its,decoded,staged.snapshot,*staged.native.bundle,{},reused,error));
    CHECK(reused.native.added_slots==0);
    CHECK(State::encode(reused.result)==encoded);
}

TEST_CASE("recipe transitions reject stale native state and unknown evidence without changing either destination", "[LocalPrintRecipeTransition]")
{
    RecipeApplicationFixture fixture(2);capture_native_recipe_fixture(fixture,2,AI::ColorEvidence::Estimated);
    const auto change=GENERATE(0,1,2,3,4,5,6,7);
    if(change==0) fixture.bundle.prints.get_edited_preset().config.set_key_value("layer_height",new ConfigOptionFloat(.25));
    if(change==1) fixture.bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values[0]="#112233";
    if(change==2) fixture.snapshot.sublayer_materials[0].min_layer_mm=.02;
    if(change==3) fixture.snapshot.sublayer_process.z_resolution_mm=0;
    if(change==4) fixture.result.targets[1].recipe_proof->checksum=std::string(64,'0');
    if(change==5) fixture.mesh.its.vertices[0].x()+=.1f;
    if(change==6) fixture.snapshot.sublayer_process.measurement_condition.clear();
    if(change==7) fixture.bundle.printers.get_edited_preset().config.set_key_value("nozzle_diameter",new ConfigOptionFloats({.6}));
    const auto original=GUI::LocalPrintRecipeApplication::identity(fixture.bundle);
    GUI::LocalPrintRecipeTransition::Prepared destination;destination.result.parent_version="untouched";
    destination.native.input_identity="untouched";std::string error;
    CHECK_FALSE(GUI::LocalPrintRecipeTransition::prepare(fixture.mesh,fixture.mesh.its,fixture.result,fixture.snapshot,fixture.bundle,{},destination,error));
    CHECK_FALSE(error.empty());CHECK_FALSE(destination.native.bundle);
    CHECK(destination.result.parent_version=="untouched");CHECK(destination.native.input_identity=="untouched");
    CHECK(GUI::LocalPrintRecipeApplication::identity(fixture.bundle)==original);
}

TEST_CASE("staged recipe confirmations remain bound to native slots after saving and loading project data", "[LocalPrintRecipeTransition][LocalPrintNativeRestore]")
{
    const auto components=GENERATE(2u,3u);
    RecipeApplicationFixture fixture(components);capture_native_recipe_fixture(fixture,components,AI::ColorEvidence::Estimated);
    // A direct physical target coexists with two logical targets sharing a recipe.
    AI::PrintColorTarget direct;direct.source=direct.output={0,0,0};direct.area=1;
    direct.physical_slot=0;direct.evidence=AI::ColorEvidence::Estimated;direct.executable=direct.within_tolerance=true;
    fixture.result.targets.push_back(direct);
    for(size_t i=0;i<fixture.result.face_count;++i) fixture.result.face_targets[i]=i%3;
    fixture.result.regions={{"protected","synthetic","detail",1,true,{0,3,6,9}}};
    fixture.result.regions[0].protect_color=true;
    fixture.result.user_overrides={{2,{0,0,0}}};
    namespace Transition=GUI::LocalPrintRecipeTransition;
    namespace State=GUI::LocalPrintColorState;
    Model model;auto* object=model.add_object();
    auto* volume=object->add_volume(TriangleMesh(fixture.mesh),ModelVolumeType::MODEL_PART,false);
    object->add_instance()->set_offset(Vec3d(50,60,0));
    Transition::Prepared staged;std::string error;
    REQUIRE(Transition::prepare(volume->mesh(),fixture.mesh.its,fixture.result,fixture.snapshot,fixture.bundle,{},staged,error));
    const auto saved=State::encode(staged.result);
    REQUIRE(GUI::LocalPrintRecipeApplication::apply_painting(*volume,std::move(staged.native.painting),error));
    DynamicPrintConfig config=staged.native.bundle->full_config();
    ScopedTemporaryDir backup("transition-source");model.set_backup_path(backup.string());
    ScopedTemporaryFile file(".3mf");const auto path=file.string();
    PlateData plate;plate.plate_index=0;
    StoreParams params;params.path=path.c_str();params.model=&model;params.config=&config;
    params.strategy=SaveStrategy::Zip64|SaveStrategy::Silence;params.plate_data_list.push_back(&plate);
    REQUIRE(store_bbs_3mf(params));
    Model restored;ScopedTemporaryDir restored_backup("transition-restored");restored.set_backup_path(restored_backup.string());
    DynamicPrintConfig restored_config;ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Enable};
    PlateDataPtrs plates;std::vector<Preset*> presets;bool is_bbl=false,is_orca=false;Semver version;
    const bool loaded=load_bbs_3mf(path.c_str(),&restored_config,&substitutions,&restored,&plates,
        &presets,&is_bbl,&is_orca,&version,nullptr,LoadStrategy::LoadModel|LoadStrategy::LoadConfig);
    release_PlateData_list(plates);for(auto* preset:presets) delete preset;
    REQUIRE(loaded);REQUIRE(restored.objects.size()==1);REQUIRE(restored.objects[0]->volumes.size()==1);
    const auto* reopened=restored.objects[0]->volumes[0];
    bool paints_match=true;
    for(size_t face=0;face<fixture.result.face_count;++face)
        paints_match=paints_match && reopened->mmu_segmentation_facets.get_triangle_as_string(face)==
            volume->mmu_segmentation_facets.get_triangle_as_string(face);
    REQUIRE(paints_match);
    // Retain installed material/printer/print presets; reload native project
    // arrays from 3MF. This is not the wx preset-import or full GUI transaction.
    PresetBundle reopened_bundle(*staged.native.bundle);
    for(const auto& key:reopened_bundle.project_config.keys())
        if(const auto* option=restored_config.option(key)) reopened_bundle.project_config.set_key_value(key,option->clone());
    auto fresh=GUI::OrcaPrintPaletteSnapshot::capture(reopened_bundle);
    Transition::retain_surface_constraints(fresh.sublayer_process,fixture.snapshot.sublayer_process);
    AI::LocalPrintColorResult decoded;
    REQUIRE(State::decode(saved,fixture.result.source_sha256,fixture.result.geometry_id,fresh.material_fingerprint,
        fresh.process_fingerprint,decoded,error,fixture.result.algorithm_version));
    AI::LocalPrintColorResult obsolete;
    CHECK_FALSE(State::decode(State::encode(fixture.result),fixture.result.source_sha256,fixture.result.geometry_id,
        fresh.material_fingerprint,fresh.process_fingerprint,obsolete,error,fixture.result.algorithm_version));
    Transition::Prepared reused;
    REQUIRE(Transition::prepare(reopened->mesh(),fixture.mesh.its,decoded,fresh,reopened_bundle,{},reused,error));
    CHECK(reused.native.added_slots==0);
    CHECK(reused.native.target_slots==std::vector<size_t>{3,3,0});
    AI::LocalPrintColorResult displayed;
    REQUIRE(State::restore_confirmed(decoded,reused.result,paints_match,displayed,error));
    CHECK(State::encode(displayed)==saved);
    CHECK(displayed.user_overrides==fixture.result.user_overrides);
    CHECK(displayed.regions[0].protect_color);
    // Exercise the actual read-only recovery entry point, including its
    // canonical native painting comparison, after native 3MF deserialization.
    AI::LocalPrintColorResult recovered;
    REQUIRE(GUI::LocalPrintColorRestore::restore(decoded, decoded, fresh, reopened_bundle, {},
        reopened->mesh(), fixture.mesh.its, reopened->mmu_segmentation_facets.get_data(), {}, recovered, error));
    CHECK(State::encode(recovered) == saved);
}

TEST_CASE("recipe recovery requires independently supplied current color evidence and never mutates the project", "[LocalPrintNativeRestore]")
{
    const auto components = GENERATE(2u, 3u);
    const auto evidence = GENERATE(AI::ColorEvidence::Estimated, AI::ColorEvidence::Measured, AI::ColorEvidence::Interpolated);
    RecipeApplicationFixture fixture(components); capture_native_recipe_fixture(fixture, components, evidence);
    GUI::LocalPrintRecipeTransition::Prepared staged; std::string error;
    REQUIRE(GUI::LocalPrintRecipeTransition::prepare(fixture.mesh, fixture.mesh.its, fixture.result,
        fixture.snapshot, fixture.bundle, {}, staged, error));
    const auto original = GUI::LocalPrintRecipeApplication::identity(*staged.native.bundle);
    const auto saved = GUI::LocalPrintColorState::encode(staged.result);
    std::vector<GUI::LocalPrintColorRecipes::Calibration> calibration;
    if (evidence != AI::ColorEvidence::Estimated) {
        // The independent fixture observation supplies color/provenance;
        // never copy these fields out of the saved confirmation.
        const auto catalog = GUI::LocalPrintColorRecipes::enumerate(
            GUI::LocalPrintColorRecipes::from_workspace(staged.snapshot));
        REQUIRE(catalog.ok());
        for (const auto& candidate : catalog.candidates)
            calibration.push_back({candidate.id, "synthetic-observation", std::string(64, 'd'),
                evidence, {.23f, .37f, .41f}, 1.25});
        AI::LocalPrintColorResult untouched; untouched.parent_version = "visible";
        CHECK_FALSE(GUI::LocalPrintColorRestore::restore(staged.result, staged.result, staged.snapshot,
            *staged.native.bundle, {}, fixture.mesh, fixture.mesh.its, staged.native.painting.data,
            {}, untouched, error));
        CHECK(untouched.parent_version == "visible");
    }
    AI::LocalPrintColorResult restored;
    REQUIRE(GUI::LocalPrintColorRestore::restore(staged.result, staged.result, staged.snapshot,
        *staged.native.bundle, {}, fixture.mesh, fixture.mesh.its, staged.native.painting.data,
        calibration, restored, error));
    CHECK(GUI::LocalPrintColorState::encode(restored) == saved);
    CHECK(GUI::LocalPrintColorState::encode(staged.result) == saved);
    CHECK(GUI::LocalPrintRecipeApplication::identity(*staged.native.bundle) == original);
}

TEST_CASE("recipe recovery rejects changed painting source and native constraints without replacing visible confirmation", "[LocalPrintNativeRestore]")
{
    RecipeApplicationFixture fixture(2); capture_native_recipe_fixture(fixture, 2, AI::ColorEvidence::Estimated);
    GUI::LocalPrintRecipeTransition::Prepared staged; std::string error;
    REQUIRE(GUI::LocalPrintRecipeTransition::prepare(fixture.mesh, fixture.mesh.its, fixture.result,
        fixture.snapshot, fixture.bundle, {}, staged, error));
    auto current = staged.result;
    auto paint = staged.native.painting.data;
    const auto fault = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
    if (fault == 0) staged.snapshot.sublayer_process.z_resolution_mm = 0;
    if (fault == 1) staged.snapshot.sublayer_process.surface_condition.clear();
    if (fault == 2) staged.native.bundle->project_config.option<ConfigOptionStrings>("filament_mixed_sublayer_ratios")->values.back() = "0.5,0.5";
    if (fault == 3) {
        TriangleSelector selector(fixture.mesh); selector.deserialize(paint);
        selector.set_facet(0, EnforcerBlockerType::Extruder1); paint = selector.serialize();
    }
    if (fault == 4) current.source_sha256 = std::string(64, 'b');
    if (fault == 5) fixture.mesh.its.vertices[0].x() += .1f;
    if (fault == 6) staged.snapshot.sublayer_materials[0].min_layer_mm = .01;
    if (fault == 7) staged.snapshot.material_metadata_complete = false;
    const auto original = GUI::LocalPrintRecipeApplication::identity(*staged.native.bundle);
    const auto saved = GUI::LocalPrintColorState::encode(staged.result);
    auto displayed = staged.result; displayed.parent_version = "visible-confirmation";
    const auto before = GUI::LocalPrintColorState::encode(displayed);
    CHECK_FALSE(GUI::LocalPrintColorRestore::restore(staged.result, current, staged.snapshot,
        *staged.native.bundle, {}, fixture.mesh, fixture.mesh.its, paint, {}, displayed, error));
    CHECK_FALSE(error.empty());
    CHECK(GUI::LocalPrintColorState::encode(displayed) == before);
    CHECK(GUI::LocalPrintColorState::encode(staged.result) == saved);
    CHECK(GUI::LocalPrintRecipeApplication::identity(*staged.native.bundle) == original);
}

TEST_CASE("recipe recovery never creates missing native virtual slots", "[LocalPrintNativeRestore]")
{
    RecipeApplicationFixture fixture(2); capture_native_recipe_fixture(fixture, 2, AI::ColorEvidence::Estimated);
    GUI::LocalPrintRecipeApplication::Prepared expected; std::string error;
    REQUIRE(GUI::LocalPrintRecipeApplication::prepare(fixture.mesh, fixture.mesh.its, fixture.result,
        fixture.snapshot, fixture.bundle, expected, error));
    REQUIRE(expected.added_slots == 1);
    const auto before = GUI::LocalPrintRecipeApplication::identity(fixture.bundle);
    AI::LocalPrintColorResult displayed; displayed.parent_version = "visible";
    CHECK_FALSE(GUI::LocalPrintColorRestore::restore(fixture.result, fixture.result, fixture.snapshot,
        fixture.bundle, {}, fixture.mesh, fixture.mesh.its, expected.painting.data, {}, displayed, error));
    CHECK(error.find("slots are absent or changed") != std::string::npos);
    CHECK(displayed.parent_version == "visible");
    CHECK(GUI::LocalPrintRecipeApplication::identity(fixture.bundle) == before);
}

TEST_CASE("confirmed recipes stage native virtual slots and exact painting without changing physical materials", "[LocalPrintRecipeApplication]")
{
    const size_t components=GENERATE(2u,3u);
    RecipeApplicationFixture fixture(components);
    auto& bundle=fixture.bundle;
    bundle.project_config.option<ConfigOptionStrings>("filament_multi_colour")->values[0]="#123456;#ABCDEF";
    const auto original=GUI::LocalPrintRecipeApplication::identity(bundle);
    GUI::LocalPrintRecipeApplication::Prepared staged;std::string error;
    REQUIRE(GUI::LocalPrintRecipeApplication::prepare(fixture.mesh,fixture.mesh.its,fixture.result,fixture.snapshot,bundle,staged,error));
    CHECK(GUI::LocalPrintRecipeApplication::identity(bundle)==original);
    CHECK(staged.input_identity==original);
    REQUIRE(staged.bundle);
    CHECK(staged.added_slots==1);
    CHECK(staged.target_slots==std::vector<size_t>{3,3});
    CHECK(staged.bundle->num_physical_filaments()==3);
    CHECK(staged.bundle->filament_presets.size()==4);
    CHECK(staged.bundle->project_config.opt_string("filament_multi_colour",0u)=="#123456;#ABCDEF");
    for(size_t i=0;i<3;++i) {
        CHECK(staged.bundle->filament_presets[i]==bundle.filament_presets[i]);
        CHECK(staged.bundle->project_config.option<ConfigOptionStrings>("filament_colour")->values[i]==
              bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values[i]);
        for(const auto* key:{"filament_map","filament_nozzle_map","filament_volume_map"})
            CHECK(staged.bundle->project_config.option<ConfigOptionInts>(key)->values[i]==
                  bundle.project_config.option<ConfigOptionInts>(key)->values[i]);
        for(size_t j=0;j<3;++j)
            CHECK_THAT(staged.bundle->project_config.option<ConfigOptionFloats>("flush_volumes_matrix")->values[i*4+j],
                Catch::Matchers::WithinAbs(bundle.project_config.option<ConfigOptionFloats>("flush_volumes_matrix")->values[i*3+j],1e-12));
    }
    const auto& config=staged.bundle->project_config;
    const auto ids=parse_mixed_components(config.opt_string("filament_mixed_components",3));
    const auto ratios=parse_mixed_ratios(config.opt_string("filament_mixed_sublayer_ratios",3),components);
    REQUIRE(ids.size()==components);REQUIRE(ratios.size()==components);
    for(size_t c=0;c<components;++c) {
        CHECK(ids[c]==fixture.result.targets[0].recipe->components[c].slot+1);
        CHECK_THAT(ratios[c],Catch::Matchers::WithinAbs(fixture.result.targets[0].recipe->components[c].ratio,1e-12));
    }
    CHECK(staged.painting.base_extruder==4);
    TriangleSelector selector(fixture.mesh);selector.deserialize(staged.painting.data);
    CHECK(selector.num_facets(EnforcerBlockerType::Extruder4)==int(fixture.result.face_count));
    // Semantic definitions survive different harmless textual ratio formats.
    staged.bundle->project_config.option<ConfigOptionStrings>("filament_mixed_sublayer_ratios")->values[3]+=" ";
    GUI::LocalPrintRecipeApplication::Prepared reused;
    REQUIRE(GUI::LocalPrintRecipeApplication::prepare(fixture.mesh,fixture.mesh.its,fixture.result,fixture.snapshot,*staged.bundle,reused,error));
    CHECK(reused.added_slots==0);CHECK(reused.target_slots==staged.target_slots);
}

TEST_CASE("native recipe definitions and painted slots survive a 3MF round trip", "[LocalPrintRecipeApplication]")
{
    RecipeApplicationFixture fixture(GENERATE(2u,3u));
    Model model;
    auto* object=model.add_object();object->name="synthetic recipe round trip";
    auto* volume=object->add_volume(TriangleMesh(fixture.mesh),ModelVolumeType::MODEL_PART,false);
    object->add_instance()->set_offset(Vec3d(50,60,0));
    GUI::LocalPrintRecipeApplication::Prepared staged;std::string error;
    REQUIRE(GUI::LocalPrintRecipeApplication::prepare(volume->mesh(),fixture.mesh.its,fixture.result,
        fixture.snapshot,fixture.bundle,staged,error));
    REQUIRE(GUI::LocalPrintRecipeApplication::apply_painting(*volume,std::move(staged.painting),error));
    DynamicPrintConfig config=staged.bundle->full_config();
    REQUIRE(config.option<ConfigOptionStrings>("filament_settings_id")->values.size()==4);
    ScopedTemporaryDir backup("recipe-source");model.set_backup_path(backup.string());
    ScopedTemporaryFile file(".3mf");const auto path=file.string();
    PlateData plate;plate.plate_index=0;
    StoreParams params;params.path=path.c_str();params.model=&model;params.config=&config;
    params.strategy=SaveStrategy::Zip64|SaveStrategy::Silence;params.plate_data_list.push_back(&plate);
    REQUIRE(store_bbs_3mf(params));
    Model restored;ScopedTemporaryDir restored_backup("recipe-restored");restored.set_backup_path(restored_backup.string());
    DynamicPrintConfig restored_config;ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Enable};
    PlateDataPtrs plates;std::vector<Preset*> presets;bool is_bbl=false,is_orca=false;Semver version;
    const bool loaded=load_bbs_3mf(path.c_str(),&restored_config,&substitutions,&restored,&plates,
        &presets,&is_bbl,&is_orca,&version,nullptr,LoadStrategy::LoadModel|LoadStrategy::LoadConfig);
    release_PlateData_list(plates);for(auto* preset:presets) delete preset;
    REQUIRE(loaded);REQUIRE(restored.objects.size()==1);REQUIRE(restored.objects[0]->volumes.size()==1);
    for(const auto* key:{"filament_colour","filament_is_mixed","filament_mixed_components",
                        "filament_mixed_sublayer_ratios","filament_mixed_gradient","filament_mixed_gradient_range",
                        "filament_mixed_gradient_curve","filament_mixed_gradient_per_part"}) {
        INFO(key);REQUIRE(restored_config.option(key));
        CHECK(restored_config.option(key)->serialize()==config.option(key)->serialize());
    }
    const auto* reopened=restored.objects[0]->volumes[0];
    // The native loader promotes a single volume's extruder to its object.
    // Resolve that inheritance instead of dereferencing an optional setting.
    CHECK(reopened->extruder_id()==4);
    CHECK(reopened->mmu_segmentation_facets.get_facets(*reopened,EnforcerBlockerType::Extruder4).indices.size()==fixture.result.face_count);
    REQUIRE(Application::same_surface_partition(fixture.mesh.its,reopened->mesh().its));
    PresetBundle reopened_bundle(*staged.bundle);
    for(const auto& key:reopened_bundle.project_config.keys())
        if(const auto* option=restored_config.option(key))
            reopened_bundle.project_config.set_key_value(key,option->clone());
    GUI::LocalPrintRecipeApplication::Prepared reused;
    REQUIRE(GUI::LocalPrintRecipeApplication::prepare(reopened->mesh(),fixture.mesh.its,fixture.result,
        fixture.snapshot,reopened_bundle,reused,error));
    CHECK(reused.added_slots==0);CHECK(reused.target_slots==staged.target_slots);
}

TEST_CASE("invalid or changed recipe evidence leaves the staged destination and live bundle untouched", "[LocalPrintRecipeApplication]")
{
    RecipeApplicationFixture fixture(2);
    SECTION("unconfirmed result") {fixture.result.confirmed=false;}
    SECTION("modified proof") {fixture.result.targets.back().recipe_proof->sublayer_heights_mm[0][0]+=.01;}
    SECTION("changed physical color") {fixture.bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values[0]="#111111";}
    SECTION("changed process limits") {fixture.snapshot.sublayer_process.z_resolution_mm=.03;}
    SECTION("missing native array") {fixture.bundle.project_config.option<ConfigOptionStrings>("filament_mixed_components")->values.clear();}
    SECTION("sublayer mode disabled") {fixture.snapshot.sublayer_process.sublayers_enabled=false;}
    SECTION("changed source geometry") {fixture.mesh.its.vertices[0].x()+=.01f;}
    const auto original=GUI::LocalPrintRecipeApplication::identity(fixture.bundle);
    GUI::LocalPrintRecipeApplication::Prepared staged;staged.input_identity="previous";
    std::string error;
    CHECK_FALSE(GUI::LocalPrintRecipeApplication::prepare(fixture.mesh,fixture.mesh.its,fixture.result,fixture.snapshot,fixture.bundle,staged,error));
    CHECK(staged.input_identity=="previous");CHECK_FALSE(staged.bundle);
    CHECK(GUI::LocalPrintRecipeApplication::identity(fixture.bundle)==original);CHECK_FALSE(error.empty());
}

TEST_CASE("gradient slots are not reused for a constant recipe with the same display color", "[LocalPrintRecipeApplication]")
{
    RecipeApplicationFixture fixture(2);std::string error;
    GUI::LocalPrintRecipeApplication::Prepared first,second;
    REQUIRE(GUI::LocalPrintRecipeApplication::prepare(fixture.mesh,fixture.mesh.its,fixture.result,fixture.snapshot,fixture.bundle,first,error));
    first.bundle->project_config.option<ConfigOptionBools>("filament_mixed_gradient")->values[3]=true;
    first.bundle->project_config.option<ConfigOptionStrings>("filament_mixed_gradient_range")->values[3]="0.9,0.1";
    REQUIRE(GUI::LocalPrintRecipeApplication::prepare(fixture.mesh,fixture.mesh.its,fixture.result,fixture.snapshot,*first.bundle,second,error));
    CHECK(second.target_slots==std::vector<size_t>{4,4});CHECK(second.added_slots==1);
    CHECK(second.bundle->project_config.opt_bool("filament_mixed_gradient",3));
    CHECK_FALSE(second.bundle->project_config.opt_bool("filament_mixed_gradient",4));
}
