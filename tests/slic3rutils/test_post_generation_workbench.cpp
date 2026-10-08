#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "slic3r/GUI/AI/ModelGeneration/PostGenerationUiState.hpp"
#include "slic3r/GUI/AI/ModelGeneration/WorkbenchImportSession.hpp"
#include "slic3r/GUI/AI/ModelGeneration/WorkbenchProjectColor.hpp"
#include "slic3r/GUI/AI/ModelGeneration/WorkbenchModelInspection.hpp"
#include "slic3r/GUI/AI/SmartSlicing/SmartSlicingWorkbenchState.hpp"
#include "slic3r/GUI/AI/Orca/OrcaPlateRevisionConfig.hpp"
#include "slic3r/GUI/AI/Orca/WorkbenchModelDecode.hpp"
#include <future>

using namespace Slic3r::GUI;

TEST_CASE("background workbench decoding preserves native geometry units and texture handoff", "[PostGenerationWorkbench][UiRedesign][TextureImport]")
{
    using namespace Slic3r;
    const std::string file = GENERATE("20mm_cube.obj", "model_artifact/textured.obj", "model_artifact/vertex-material-color.obj",
        "model_artifact/textured.glb", "model_artifact/outward-textured.glb", "model_artifact/vertex-material-color.glb",
        "model_artifact/transformed.glb", "model_artifact/nested-negative-nodes.glb");
    const auto path = boost::filesystem::path(std::string(TEST_DATA_DIR)) / file;
    DecodedWorkbenchModel decoded;
    std::string error;
    const bool loaded = std::async(std::launch::async, [&] { return decode_workbench_model(path, decoded, error); }).get();
    INFO(error);
    REQUIRE(loaded);
    const bool meters = path.extension() == ".glb";
    if (meters) decoded.mesh.scale(1000.f);
    auto actual = assemble_workbench_model(decoded, path, meters);
    auto expected = Model::read_from_file(path.string(), nullptr, nullptr, LoadStrategy::LoadModel);
    if (meters) expected.convert_from_meters(false);
    REQUIRE(actual->objects.size() == expected.objects.size());
    const auto* av = actual->objects.front()->volumes.front();
    const auto* ev = expected.objects.front()->volumes.front();
    const auto& am = av->mesh().its;
    const auto& em = ev->mesh().its;
    REQUIRE(am.vertices.size() == em.vertices.size());
    REQUIRE(am.indices.size() == em.indices.size());
    for (size_t i = 0; i < am.vertices.size(); ++i)
        CHECK_THAT((am.vertices[i].cast<double>() + av->get_offset() - em.vertices[i].cast<double>() - ev->get_offset()).norm(),
            Catch::Matchers::WithinAbs(0., 1e-4));
    for (size_t i = 0; i < am.indices.size(); ++i) CHECK(am.indices[i] == em.indices[i]);
    CHECK(av->source.is_converted_from_meters == ev->source.is_converted_from_meters);
    CHECK(av->source.input_file == ev->source.input_file);
    REQUIRE(bool(actual->texture_mesh) == bool(expected.texture_mesh));
    if (actual->texture_mesh) {
        const auto& at = *actual->texture_mesh;
        const auto& et = *expected.texture_mesh;
        CHECK(at.vertices == et.vertices);
        CHECK(at.indices == et.indices);
        CHECK(at.uvs == et.uvs);
        CHECK(at.uv_coords == et.uv_coords);
        CHECK(at.uv_indices == et.uv_indices);
        CHECK(at.material_ids == et.material_ids);
        CHECK(at.material_colors == et.material_colors);
        CHECK(at.material_texture_map == et.material_texture_map);
        CHECK(at.precomputed_face_colors == et.precomputed_face_colors);
        CHECK(at.precomputed_vertex_colors == et.precomputed_vertex_colors);
        REQUIRE(at.textures.size() == et.textures.size());
        for (size_t i = 0; i < at.textures.size(); ++i) CHECK(at.textures[i].data == et.textures[i].data);
    }
}

TEST_CASE("cancelled decoding leaves no model or texture to commit", "[PostGenerationWorkbench][UiRedesign]")
{
    DecodedWorkbenchModel decoded;
    std::string error;
    const auto path = boost::filesystem::path(std::string(TEST_DATA_DIR)) / "20mm_cube.obj";
    int polls = 0;
    CHECK_FALSE(decode_workbench_model(path, decoded, error, [&] { return ++polls >= 3; }));
    CHECK_FALSE(error.empty());
    CHECK(decoded.mesh.empty());
    CHECK_FALSE(decoded.texture);
}

TEST_CASE("cancelling model preparation prevents a late project commit", "[PostGenerationWorkbench][UiRedesign]")
{
    const auto phase = GENERATE(WorkbenchImportPhase::Reading, WorkbenchImportPhase::Colors, WorkbenchImportPhase::Placement);
    WorkbenchImportSession session;
    REQUIRE(session.advance(phase));
    REQUIRE(session.can_cancel());
    REQUIRE(session.cancel());
    CHECK_FALSE(session.advance(WorkbenchImportPhase::Committing));
    CHECK_FALSE(session.advance(WorkbenchImportPhase::UpdatingView));
    CHECK_FALSE(session.cancel());
    CHECK(session.cancelled());
}

TEST_CASE("project import commits once and cannot cancel an adopted model", "[PostGenerationWorkbench][UiRedesign]")
{
    WorkbenchImportSession session;
    REQUIRE(session.advance(WorkbenchImportPhase::Placement));
    REQUIRE(session.advance(WorkbenchImportPhase::Committing));
    CHECK_FALSE(session.advance(WorkbenchImportPhase::Committing));
    CHECK_FALSE(session.can_cancel());
    CHECK_FALSE(session.cancel());
    REQUIRE(session.advance(WorkbenchImportPhase::UpdatingView));
    CHECK_FALSE(session.advance(WorkbenchImportPhase::Colors));
    REQUIRE(session.advance(WorkbenchImportPhase::Completed));
    CHECK_FALSE(session.advance(WorkbenchImportPhase::Committing));
}

TEST_CASE("closed import sessions reject queued UI updates and commits", "[PostGenerationWorkbench][UiRedesign]")
{
    WorkbenchImportSession session;
    REQUIRE(session.advance(WorkbenchImportPhase::Colors));
    session.invalidate();
    CHECK_FALSE(session.valid());
    CHECK(session.cancelled());
    CHECK_FALSE(session.advance(WorkbenchImportPhase::Placement));
    CHECK_FALSE(session.advance(WorkbenchImportPhase::Committing));
}

TEST_CASE("unavailable AI slicing uses native parameters while valid AI and retry retain priority", "[SmartSlicing][UiRedesign]")
{
    CHECK(workbench_slice_route(false, false, false, false, true) == WorkbenchSliceRoute::Native);
    CHECK(workbench_slice_route(false, false, false, true, true) == WorkbenchSliceRoute::AiCandidate);
    CHECK(workbench_slice_route(false, false, true, true, true) == WorkbenchSliceRoute::AiRetry);
    CHECK(workbench_slice_route(true, false, true, true, true) == WorkbenchSliceRoute::Native);
    CHECK(workbench_slice_route(true, false, true, true, false) == WorkbenchSliceRoute::Unavailable);
    CHECK(workbench_slice_route(false, false, false, false, false) == WorkbenchSliceRoute::Unavailable);
    CHECK(workbench_slice_route(false, true, true, true, true) == WorkbenchSliceRoute::Unavailable);
}

TEST_CASE("ordinary region optimization remains available without portrait protection", "[PostGenerationWorkbench]")
{
    PostGenerationWorkbenchState state;
    state.portrait_enabled = GENERATE(false, true);
    state.portrait_available = false;
    state.actions.can_edit = true;
    CHECK(workbench_region_optimization_allowed(state.actions, false, true));
    CHECK_FALSE(workbench_region_optimization_allowed(state.actions, true, true));
    CHECK_FALSE(workbench_region_optimization_allowed(state.actions, false, false));
    state.actions.can_edit = false;
    CHECK_FALSE(workbench_region_optimization_allowed(state.actions, false, true));
    CHECK_FALSE(state.can_print);
}

TEST_CASE("project color changes use physical slot identity and preserve unrelated config", "[PostGenerationWorkbench][ProjectConfigUndo]")
{
    using namespace Slic3r;
    AI::PrintablePaletteSnapshot palette;
    AI::PhysicalFilamentChannel first, fourth;
    first.slot = 0; first.display_color = "#FFFFFF";
    fourth.slot = 3; fourth.display_color = "#FFFFFF";
    palette.physical_channels = {first, fourth};
    DynamicPrintConfig config;
    config.set_key_value("filament_colour", new ConfigOptionStrings({"#FFFFFF", "#111111", "#222222", "#FFFFFF"}));
    config.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    bool changed = true;
    std::string error;
    REQUIRE(prepare_workbench_project_color(config, palette.physical_channels, 3, "#ab12Cd", changed, error));
    CHECK(changed);
    const auto& colors = config.option<ConfigOptionStrings>("filament_colour")->values;
    CHECK(colors.at(0) == "#FFFFFF");
    CHECK(colors.at(3) == "#AB12CD");
    CHECK(config.option<ConfigOptionFloat>("layer_height")->value == 0.2);
    REQUIRE(prepare_workbench_project_color(config, palette.physical_channels, 3, "#ab12cd", changed, error));
    CHECK_FALSE(changed);
    const auto before = config;
    CHECK_FALSE(prepare_workbench_project_color(config, palette.physical_channels, 1, "#000000", changed, error));
    CHECK_FALSE(error.empty());
    CHECK(before.diff(config).empty());
    CHECK_FALSE(prepare_workbench_project_color(config, palette.physical_channels, 0, "#GG0000", changed, error));
    CHECK(before.diff(config).empty());
}

TEST_CASE("all physical project slots remain editable beyond the generation limit", "[PostGenerationWorkbench][ProjectConfigUndo]")
{
    using namespace Slic3r;
    DynamicPrintConfig config;
    config.set_key_value("filament_colour", new ConfigOptionStrings(
        {"#FFFFFF", "#111111", "#222222", "#333333", "#444444", "#555555", "#FFFFFF", "#777777"}));
    config.set_key_value("filament_is_mixed", new ConfigOptionBools({false, true, false, false, false, false, false, false}));
    const auto channels = workbench_project_channels(config);
    REQUIRE(channels.size() == 7);
    CHECK(channels.front().slot == 0);
    CHECK(channels.back().slot == 7);
    CHECK_FALSE(workbench_physical_slot_exists(channels, 1));
    bool changed = false;
    std::string error;
    REQUIRE(prepare_workbench_project_color(config, channels, 6, "#ab12cd", changed, error));
    CHECK(changed);
    const auto& colors = config.option<ConfigOptionStrings>("filament_colour")->values;
    CHECK(colors.at(0) == "#FFFFFF");
    CHECK(colors.at(6) == "#AB12CD");
    CHECK(colors.at(1) == "#111111");
}

TEST_CASE("safe automatic repair rejects texture and identity bound edits", "[PostGenerationWorkbench]")
{
    CHECK(workbench_safe_repair_allowed("obj", false, false, false));
    CHECK_FALSE(workbench_safe_repair_allowed("glb", false, false, false));
    CHECK_FALSE(workbench_safe_repair_allowed("obj", true, false, false));
    CHECK_FALSE(workbench_safe_repair_allowed("obj", false, true, false));
    CHECK_FALSE(workbench_safe_repair_allowed("obj", false, false, true));
}

TEST_CASE("automatic nozzle writeback preserves slice input identity", "[SmartSlicing][UiRedesign]")
{
    using namespace Slic3r;
    const auto mode = GENERATE(fmmAutoForFlush, fmmAutoForMatch);
    DynamicPrintConfig before;
    before.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    auto after = before;
    after.set_key_value("filament_map", new ConfigOptionInts({1, 2}));
    after.set_key_value("filament_volume_map", new ConfigOptionInts({0, 0}));
    after.set_key_value("filament_nozzle_map", new ConfigOptionInts({1, 2}));
    CHECK(slice_input_plate_config(before, mode).diff(slice_input_plate_config(after, mode)).empty());
    CHECK(after.has("filament_map"));
    after.set_key_value("layer_height", new ConfigOptionFloat(0.28));
    CHECK_FALSE(slice_input_plate_config(before, mode).diff(slice_input_plate_config(after, mode)).empty());
}

TEST_CASE("manual routing remains part of slice input identity", "[SmartSlicing][UiRedesign]")
{
    using namespace Slic3r;
    const auto mode = GENERATE(fmmManual, fmmNozzleManual);
    DynamicPrintConfig before;
    before.set_key_value("filament_map", new ConfigOptionInts({1, 2}));
    before.set_key_value("filament_volume_map", new ConfigOptionInts({0, 0}));
    before.set_key_value("filament_nozzle_map", new ConfigOptionInts({1, 2}));
    auto after = before;
    after.set_key_value(mode == fmmNozzleManual ? "filament_nozzle_map" : "filament_map",
        new ConfigOptionInts({2, 1}));
    CHECK_FALSE(slice_input_plate_config(before, mode).diff(slice_input_plate_config(after, mode)).empty());
    after = before;
    after.set_key_value("filament_volume_map", new ConfigOptionInts({1, 0}));
    CHECK_FALSE(slice_input_plate_config(before, mode).diff(slice_input_plate_config(after, mode)).empty());
    after = before;
    after.set_key_value("filament_nozzle_map", new ConfigOptionInts({2, 1}));
    CHECK(slice_input_plate_config(before, mode).diff(slice_input_plate_config(after, mode)).empty()
        == (mode != fmmNozzleManual));
}

TEST_CASE("Each slicing goal overlays its actual proposal on the captured parameter values", "[SmartSlicing][UiRedesign]")
{
    using namespace Slic3r::AI::SmartSlicing;
    const std::vector<ConfigPatchEntry> baseline {
        {ConfigScope::Plate, PresetOwner::Process, 1, "layer_height", 0.2, 0.2, {}},
        {ConfigScope::Plate, PresetOwner::Process, 2, "layer_height", 0.3, 0.3, {}},
        {ConfigScope::Plate, PresetOwner::Process, 1, "wall_loops", int64_t(2), int64_t(2), {}}};
    for (const double height : {0.2, 0.28, 0.12}) {
        ParameterProposal proposal;
        proposal.entries.push_back({ConfigScope::Plate, PresetOwner::Process, 1,
            "layer_height", 0.2, height, {}});
        const auto effective = effective_candidate_parameters(baseline, proposal);
        REQUIRE(effective.size() == baseline.size());
        CHECK_THAT(std::get<double>(effective[0].new_value), Catch::Matchers::WithinAbs(height, 1e-9));
        CHECK_THAT(std::get<double>(effective[1].new_value), Catch::Matchers::WithinAbs(0.3, 1e-9));
        CHECK(std::get<int64_t>(effective[2].new_value) == 2);
        CHECK_THAT(std::get<double>(baseline[0].new_value), Catch::Matchers::WithinAbs(0.2, 1e-9));
    }
}

TEST_CASE("every pending model operation blocks replacement even outside the editor", "[PostGenerationWorkbench]")
{
    const int blocker = GENERATE(-1, 0, 1, 2, 3, 4, 5);
    CHECK(post_generation_asset_switch_allowed(blocker == 0, blocker == 1, blocker == 2,
        blocker == 3, blocker == 4, blocker == 5) == (blocker == -1));
}

TEST_CASE("native slicing rejects late completions and retains an active run until its callback", "[SmartSlicing][UiRedesign]")
{
    using namespace Slic3r::AI::SmartSlicing;
    NativeSliceSession session;
    const WorkspaceRevision original {1, 2, 3, "original"};
    const WorkspaceRevision changed {2, 2, 3, "changed"};
    session.start(original);
    CHECK_FALSE(session.reset());
    session.refresh(changed);
    CHECK(session.pending());
    CHECK(session.result().phase == OfficialSlicePhase::Failed);
    CHECK(session.result().diagnostic_code == "workspace_changed");
    session.complete(true, changed, {});
    CHECK_FALSE(session.pending());
    CHECK(session.result().phase == OfficialSlicePhase::Failed);
    CHECK_FALSE(session.result().can_print);
    REQUIRE(session.reset());
    session.start(changed);
    session.complete(true, changed, {});
    CHECK(session.result().phase == OfficialSlicePhase::Completed);
    session.refresh(original);
    CHECK(session.result().phase == OfficialSlicePhase::Failed);
    CHECK_FALSE(session.result().can_print);
}

TEST_CASE("a pending candidate or dirty edits prevent changing assets and project import", "[PostGenerationWorkbench]")
{
    const bool candidate = GENERATE(false, true);
    const bool dirty = GENERATE(false, true);
    const auto state = derive_post_generation_ui_state(PostGenerationUiState::Mode::Workbench,
        true, true, false, false, false, candidate, false, dirty, true, true);
    CHECK(state.can_switch_version == (!candidate && !dirty));
    CHECK(state.can_import == (!candidate && !dirty));
    CHECK(state.can_accept == candidate);
    CHECK(state.can_discard == candidate);
    CHECK(state.can_undo);
    CHECK(state.can_redo);
}

TEST_CASE("failed or processing models cannot commit or edit a working copy", "[PostGenerationWorkbench]")
{
    const int blocked = GENERATE(0, 1, 2, 3, 4);
    const auto state = derive_post_generation_ui_state(PostGenerationUiState::Mode::Workbench,
        blocked != 4, true, blocked == 0, blocked == 1, blocked == 2, true, blocked == 3, false, true, true, blocked == 4);
    CHECK_FALSE(state.can_edit);
    CHECK_FALSE(state.can_switch_version);
    CHECK_FALSE(state.can_import);
    CHECK_FALSE(state.can_accept);
    CHECK_FALSE(state.can_discard);
    CHECK_FALSE(state.can_undo);
    CHECK_FALSE(state.can_redo);
}

TEST_CASE("an empty or loading preview never exposes model actions", "[PostGenerationWorkbench]")
{
    const bool loading = GENERATE(false, true);
    const auto state = derive_post_generation_ui_state(PostGenerationUiState::Mode::Workbench,
        false, false, loading, false, false, false, false, false, false, false);
    CHECK(state.status == (loading ? PostGenerationUiState::Status::Loading : PostGenerationUiState::Status::Empty));
    CHECK_FALSE(state.can_edit);
    CHECK_FALSE(state.can_import);
    CHECK(state.can_switch_version == !loading);
}

TEST_CASE("a failed history replacement preserves actions for the retained valid model", "[PostGenerationWorkbench]")
{
    const auto retained = derive_post_generation_ui_state(PostGenerationUiState::Mode::Workbench,
        true, true, false, false, false, false, false, false, true, true, true);
    CHECK(retained.status == PostGenerationUiState::Status::Error);
    CHECK(retained.can_edit);
    CHECK(retained.can_switch_version);
    CHECK(retained.can_import);
    CHECK(retained.can_undo);
    CHECK(retained.can_redo);

    const auto recovered = derive_post_generation_ui_state(PostGenerationUiState::Mode::Workbench,
        true, true, false, false, false, false, false, false, true, true, false);
    CHECK(recovered.status == PostGenerationUiState::Status::Ready);
    CHECK(recovered.can_edit);
    CHECK(recovered.can_import);
}
