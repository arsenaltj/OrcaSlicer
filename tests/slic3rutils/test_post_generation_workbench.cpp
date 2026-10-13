#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "slic3r/GUI/AI/ModelGeneration/PostGenerationUiState.hpp"
#include "slic3r/GUI/AI/ModelGeneration/WorkbenchImportSession.hpp"
#include "slic3r/GUI/AI/ModelGeneration/WorkbenchImportUiGuard.hpp"
#include "slic3r/GUI/AI/ModelGeneration/WorkbenchProjectColor.hpp"
#include "slic3r/GUI/AI/ModelGeneration/WorkbenchModelInspection.hpp"
#include "slic3r/GUI/AI/ModelGeneration/BeautyColorPaint.hpp"
#include "slic3r/GUI/AI/ModelGeneration/BeautyWorkbenchTransactionController.hpp"
#include "slic3r/GUI/AI/SmartSlicing/SmartSlicingWorkbenchState.hpp"
#include "slic3r/GUI/AI/Orca/OrcaPlateRevisionConfig.hpp"
#include "slic3r/GUI/AI/Orca/WorkbenchModelDecode.hpp"
#include "slic3r/GUI/AI/Orca/SlicingModelPalette.hpp"
#include "slic3r/GUI/Redesign/RedesignModelRoute.hpp"
#include "slic3r/GUI/AI/ModelGeneration/PrinterFilamentPreview.hpp"
#include "slic3r/GUI/AI/Orca/WorkbenchPrinterFilaments.hpp"
#include "slic3r/GUI/PrinterFilamentSlotRemap.hpp"
#include <future>
#include <memory>
#include <type_traits>
#include <vector>

using namespace Slic3r::GUI;

TEST_CASE("background import handoffs preserve the UI tracker list and source guard", "[WorkbenchImportUiGuard][PostGenerationWorkbench]")
{
    struct Window : wxTrackable {};
    using Guard = WorkbenchImportUiGuard<Window>;
    static_assert(!std::is_copy_constructible_v<Guard>);
    auto window = std::make_unique<Window>();
    bool source_current = true;
    auto guard = std::make_shared<Guard>(window.get(),
        [weak = wxWeakRef<Window>(window.get()), &source_current] { return weak && source_current; });
    auto* trackers = window->GetFirst();
    auto pending = std::async(std::launch::async, [guard] {
        std::vector<std::function<bool()>> callbacks;
        for (size_t index = 0; index < 256; ++index)
            callbacks.emplace_back([guard] { return guard->current(); });
        return callbacks;
    }).get();
    CHECK(window->GetFirst() == trackers);
    for (const auto& callback : pending) CHECK(callback());
    source_current = false;
    CHECK_FALSE(pending.back()());
    pending.clear();
    guard.reset();
    CHECK(window->GetFirst() == nullptr);
}

TEST_CASE("a delayed import callback skips a destroyed window without consulting its source", "[WorkbenchImportUiGuard][PostGenerationWorkbench]")
{
    struct Window : wxTrackable {};
    auto window = std::make_unique<Window>();
    size_t source_checks = 0;
    auto guard = std::make_shared<WorkbenchImportUiGuard<Window>>(window.get(), [&] { ++source_checks; return true; });
    auto pending = std::async(std::launch::async, [guard] {
        return std::function<bool()>([guard] { return guard->current(); });
    }).get();
    window.reset();
    CHECK(guard->window() == nullptr);
    CHECK_FALSE(pending());
    CHECK(source_checks == 0);
}

TEST_CASE("partition filling preserves protected and unrelated face colors", "[BeautyManualColor][PostGenerationWorkbench]")
{
    using namespace Slic3r::AI::SurfaceSelectionPersistence;
    const std::array<float, 3> red {1.f, 0.f, 0.f}, blue {0.f, 0.f, 1.f};
    const FaceColorOverrides before {{1, blue}, {3, blue}};
    SelectionState selection;
    selection.selected = {1, 1, 1, 0};
    selection.protected_faces = {0, 1, 0, 0};
    const auto result = paint_beauty_face_colors(before, selection, {0, 1, 2, 3, 99}, red);
    REQUIRE(result.changed == 2);
    CHECK(result.colors == FaceColorOverrides({{0, red}, {1, blue}, {2, red}, {3, blue}}));
    CHECK(selection.protected_faces == std::vector<uint8_t>({0, 1, 0, 0}));
    const std::string geometry(64, 'a');
    FaceColorOverrides restored;
    std::string error;
    REQUIRE(decode_colors(encode_colors(result.colors, 4, geometry), 4, geometry, restored, error));
    CHECK(restored == result.colors);
    CHECK_FALSE(decode_colors(encode_colors(result.colors, 4, geometry), 4, std::string(64, 'b'), restored, error));
}

TEST_CASE("a paint stroke changes only touched faces even when a whole partition is selected", "[BeautyManualColor][PostGenerationWorkbench]")
{
    using namespace Slic3r::AI::SurfaceSelectionPersistence;
    SelectionState selection;
    selection.selected = {1, 1, 1, 1};
    const std::array<float, 3> red {1.f, 0.f, 0.f};
    const auto result = paint_beauty_face_colors({}, selection, {1, 1, 99}, red);
    REQUIRE(result.changed == 1);
    CHECK(result.colors == FaceColorOverrides({{1, red}}));
    CHECK(paint_beauty_face_colors(result.colors, selection, {1}, red).changed == 0);
    CHECK(paint_beauty_face_colors(result.colors, selection, {2}, {-1.f, 0.f, 0.f}).changed == 0);
    CHECK(paint_beauty_face_colors(result.colors, selection, {2}, {NAN, 0.f, 0.f}).colors == result.colors);
}

TEST_CASE("manual strokes undo independently and a new stroke replaces the redo branch", "[BeautyManualColor][PostGenerationWorkbench]")
{
    using namespace Slic3r::AI::SurfaceSelectionPersistence;
    FaceColorOverrides current;
    BeautyWorkbenchTransactionController history;
    SelectionState selection;
    selection.selected = {1, 1, 1};
    const auto stroke = [&](size_t face, const std::array<float, 3>& color) {
        const auto before = current;
        const auto result = paint_beauty_face_colors(current, selection, {face}, color);
        if (!result.changed) return;
        current = result.colors;
        BeautyWorkbenchTransactionController::Entry entry;
        entry.kind = BeautyWorkbenchTransactionController::OperationKind::AppearanceRecolor;
        entry.undo = [&, before] { current = before; };
        entry.redo = [&, after = current] { current = after; };
        history.record(std::move(entry));
    };
    const std::array<float, 3> red {1.f, 0.f, 0.f}, blue {0.f, 0.f, 1.f};
    stroke(0, red);
    stroke(1, blue);
    stroke(1, blue);
    CHECK(history.undo_count() == 2);
    REQUIRE(history.undo());
    CHECK(current == FaceColorOverrides({{0, red}}));
    REQUIRE(history.redo());
    CHECK(current == FaceColorOverrides({{0, red}, {1, blue}}));
    REQUIRE(history.undo());
    stroke(2, blue);
    CHECK_FALSE(history.redo());
    CHECK(current == FaceColorOverrides({{0, red}, {2, blue}}));
    REQUIRE(history.undo());
    REQUIRE(history.undo());
    CHECK(current.empty());
}

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
    CHECK_THAT((av->source.mesh_offset - ev->source.mesh_offset).norm(), Catch::Matchers::WithinAbs(0., 1e-12));
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

        // Exercise the color handoff as well as initial geometry: a wrong
        // source offset can be hidden by later recentering of the replacement.
        PaintedMesh painted;
        painted.vertices = at.vertices;
        painted.indices = at.indices;
        painted.cluster_colors = {{255, 0, 0}};
        painted.face_colors.assign(painted.indices.size(), painted.cluster_colors.front());
        FilamentMatch match;
        match.cluster_index = 0;
        match.filament_index = 0;
        REQUIRE(apply_painted_mesh_to_volume(painted, {match}, *actual->objects.front()->volumes.front()));
        REQUIRE(apply_painted_mesh_to_volume(painted, {match}, *expected.objects.front()->volumes.front()));
        const auto& painted_actual = av->mesh().its;
        const auto& painted_expected = ev->mesh().its;
        REQUIRE(painted_actual.vertices.size() == painted_expected.vertices.size());
        REQUIRE(painted_actual.indices.size() == painted_expected.indices.size());
        for (size_t i = 0; i < painted_actual.vertices.size(); ++i)
            CHECK_THAT((painted_actual.vertices[i] - painted_expected.vertices[i]).norm(), Catch::Matchers::WithinAbs(0., 1e-6));
        for (size_t i = 0; i < painted_actual.indices.size(); ++i) CHECK(painted_actual.indices[i] == painted_expected.indices[i]);
        CHECK(av->mmu_segmentation_facets.get_data() == ev->mmu_segmentation_facets.get_data());
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

TEST_CASE("AI slicing requires a candidate and native slicing requires its explicit tab", "[SmartSlicing][UiRedesign]")
{
    CHECK(workbench_slice_route(false, false, false, false, true) == WorkbenchSliceRoute::Unavailable);
    CHECK(workbench_slice_route(false, false, false, true, true) == WorkbenchSliceRoute::AiCandidate);
    CHECK(workbench_slice_route(false, false, true, true, true) == WorkbenchSliceRoute::AiRetry);
    CHECK(workbench_slice_route(true, false, true, true, true) == WorkbenchSliceRoute::Native);
    CHECK(workbench_slice_route(true, false, true, true, false) == WorkbenchSliceRoute::Unavailable);
    CHECK(workbench_slice_route(false, false, false, false, false) == WorkbenchSliceRoute::Unavailable);
    CHECK(workbench_slice_route(false, true, true, true, true) == WorkbenchSliceRoute::Unavailable);
}

TEST_CASE("workbench goals require explicit selection in the reviewed workspace", "[SmartSlicing][UiRedesign]")
{
    using namespace Slic3r::AI::SmartSlicing;
    WorkbenchGoalSelection selection;
    WorkspaceRevision revision{1, 2, 3, "project"};
    CHECK_FALSE(selection.selected(revision));
    CHECK_FALSE(selection.select(RecommendationGoal::Balanced, {}));
    REQUIRE(selection.select(RecommendationGoal::Quality, revision));
    CHECK(selection.selected(revision) == RecommendationGoal::Quality);
    auto changed = revision;
    ++changed.config_revision;
    CHECK_FALSE(selection.selected(changed));
    ++changed.model_revision;
    CHECK_FALSE(selection.selected(changed));
    selection.clear();
    CHECK_FALSE(selection.selected(revision));
}

TEST_CASE("slicing palette prepares subface colors atomically without changing other models", "[UiRedesign][ProjectConfigUndo]")
{
    using namespace Slic3r;
    Model model;
    auto* object = model.add_object("target", "target.obj", TriangleMesh(its_make_cube(20., 20., 20.)));
    auto* volume = object->volumes.front();
    auto* other = model.add_object("other", "other.obj", TriangleMesh(its_make_cube(10., 10., 10.)));
    SlicingModelPalette::Selector selector(volume->mesh());
    REQUIRE(selector.set_facet_midpoint_subfaces(0, EnforcerBlockerType::Extruder1,
        {{1, 0, EnforcerBlockerType::Extruder2}}));
    volume->mmu_segmentation_facets.set_data(selector.serialize());
    const auto before = volume->mmu_segmentation_facets.get_data();
    const auto unrelated = other->volumes.front()->mmu_segmentation_facets.get_data();
    LocalPrintColorApplication::PreparedPainting painting;
    std::vector<size_t> slots;
    std::string error;
    const std::vector<PreviewPalette::Color> colors{{1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}};
    CHECK_FALSE(SlicingModelPalette::prepare(volume->mesh(), before, 1, colors, {false, false},
        {colors[1]}, painting, slots, error));
    CHECK(volume->mmu_segmentation_facets.get_data() == before);
    REQUIRE(SlicingModelPalette::prepare(volume->mesh(), before, 1, colors, {true, true},
        {colors[1]}, painting, slots, error));
    CHECK(slots == std::vector<size_t>{1});
    LocalPrintColorCommit::Prepared prepared;
    REQUIRE(LocalPrintColorCommit::prepare(*volume, std::move(painting), "target.obj", prepared, error));
    REQUIRE(LocalPrintColorCommit::validate(*volume, prepared, error));
    CHECK(volume->mmu_segmentation_facets.get_data() == before);
    LocalPrintColorCommit::commit(*volume, prepared);
    CHECK(SlicingModelPalette::base_extruder(*volume) == 2);
    CHECK_FALSE(volume->mmu_segmentation_facets.get_data() == before);
    CHECK(other->volumes.front()->mmu_segmentation_facets.get_data() == unrelated);
    CHECK_FALSE(LocalPrintColorCommit::validate(*volume, prepared, error));
}

TEST_CASE("slicing automatic colors reduce painted regions and respect the selected source", "[UiRedesign][ProjectConfigUndo]")
{
    using namespace Slic3r;
    const std::vector<PreviewPalette::Color> colors{{0.f,0.f,0.f}, {1.f,1.f,1.f}, {0.f,1.f,0.f}, {1.f,0.f,0.f}};
    PreviewPalette::Histogram histogram;
    histogram.add(0x000000, 100.);
    histogram.add(0xffffff, 100.);
    histogram.add(0x00ff00, 100.);
    histogram.add(0xff0000, 100.);
    const auto automatic = SlicingModelPalette::select_targets(histogram, colors, {true,true,true,true}, 2, 0);
    REQUIRE(automatic.size() == 2);
    CHECK(SlicingModelPalette::select_targets(histogram, colors, {false,true,false,true}, 4, 1) ==
        std::vector<PreviewPalette::Color>{colors[1],colors[3]});
    CHECK(SlicingModelPalette::select_targets(histogram, colors, {true,true,true,true}, 2, 2, {colors[3]}) ==
        std::vector<PreviewPalette::Color>{colors[3]});
    CHECK(SlicingModelPalette::select_targets(histogram, colors, {true,true,true,true}, 2, 0, {}, {colors[2]}) ==
        std::vector<PreviewPalette::Color>{colors[2]});
    const TriangleMesh mesh(its_make_cube(20.,20.,20.));
    SlicingModelPalette::Selector selector(mesh);
    for (size_t face = 0; face < mesh.facets_count(); ++face)
        selector.set_facet(face, static_cast<EnforcerBlockerType>(face % 4 + 1));
    const auto original = selector.serialize();
    LocalPrintColorApplication::PreparedPainting prepared;
    std::vector<size_t> slots;
    std::string error;
    REQUIRE(SlicingModelPalette::prepare(mesh, original, 1, colors, {true,true,true,true}, automatic, prepared, slots, error));
    CHECK(slots.size() <= 2);
    CHECK_FALSE(prepared.data == original);
    CHECK(selector.serialize() == original);
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
    const std::vector<std::string> display_colors = {"#FFFFFF", "#111111 #222222", "#222222", "#FFFFFF"};
    config.set_key_value("filament_multi_colour", new ConfigOptionStrings(display_colors));
    config.set_key_value("filament_colour_type", new ConfigOptionStrings({"1", "0", "1", "0"}));
    config.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    bool changed = true;
    std::string error;
    REQUIRE(prepare_workbench_project_color(config, palette.physical_channels, 3, "#ab12Cd", changed, error));
    CHECK(changed);
    const auto& colors = config.option<ConfigOptionStrings>("filament_colour")->values;
    CHECK(colors.at(0) == "#FFFFFF");
    CHECK(colors.at(3) == "#AB12CD");
    const auto& display = config.option<ConfigOptionStrings>("filament_multi_colour")->values;
    const auto& types = config.option<ConfigOptionStrings>("filament_colour_type")->values;
    CHECK(display.at(3) == colors.at(3));
    CHECK(types.at(3) == "1");
    CHECK(display.at(1) == display_colors.at(1));
    CHECK(types.at(1) == "0");
    CHECK(config.option<ConfigOptionFloat>("layer_height")->value == 0.2);
    REQUIRE(prepare_workbench_project_color(config, palette.physical_channels, 3, "#ab12cd", changed, error));
    CHECK_FALSE(changed);
    // Older workbench edits could save the RGB value with stale native icon data.
    config.option<ConfigOptionStrings>("filament_multi_colour")->values.at(3) = "#FFFFFF";
    REQUIRE(prepare_workbench_project_color(config, palette.physical_channels, 3, "#ab12cd", changed, error));
    CHECK(changed);
    CHECK(display.at(3) == colors.at(3));
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

TEST_CASE("Returning to AI waits for native slicing to drain after a workspace change", "[SmartSlicing][UiRedesign]")
{
    NativeSliceSession session;
    const Slic3r::AI::SmartSlicing::WorkspaceRevision original {1,2,3,"original"};
    const Slic3r::AI::SmartSlicing::WorkspaceRevision changed {2,2,3,"changed"};
    session.start(original);
    session.refresh(changed);
    REQUIRE(session.result().phase == Slic3r::AI::SmartSlicing::OfficialSlicePhase::Failed);
    // The UI phase can already report failure while the native worker still
    // owns the project. A phase-only guard would allow a premature switch.
    CHECK_FALSE(workbench_mode_switch_allowed(false, session.pending(), false, false));
    session.complete(false, changed, "cancelled");
    CHECK(workbench_mode_switch_allowed(false, session.pending(), false, false));
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


static Slic3r::PresetBundle printer_filament_fixture(size_t nozzles, size_t slots)
{
    using namespace Slic3r;
    PresetBundle bundle;
    auto material = bundle.filaments.default_preset().config;
    material.set_key_value("filament_type", new ConfigOptionStrings({"PLA"}));
    material.set_key_value("nozzle_temperature", new ConfigOptionInts({210}));
    material.set_key_value("nozzle_temperature_range_low", new ConfigOptionInts({190}));
    material.set_key_value("nozzle_temperature_range_high", new ConfigOptionInts({230}));
    bundle.filaments.load_preset("", "Loaded PLA", material, false);
    bundle.printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter",true)->values.assign(nozzles,.4);
    bundle.printers.get_edited_preset().config.option<ConfigOptionBool>("single_extruder_multi_material",true)->value=false;
    bundle.set_num_filaments(unsigned(slots),"#123456");
    bundle.filament_presets.assign(slots,"Loaded PLA");
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values.assign(slots,"#123456");
    bundle.project_config.option<ConfigOptionStrings>("filament_multi_colour")->values.assign(slots,"#123456");
    return bundle;
}

TEST_CASE("printer filament readiness requires actual colors for every nozzle and excludes virtual slots", "[PrinterFilaments][PostGenerationWorkbench]")
{
    using namespace Slic3r;
    const size_t nozzles=GENERATE(1u,4u,6u);
    auto bundle=printer_filament_fixture(nozzles,nozzles);
    auto state=PrinterFilamentConfig::capture(bundle);
    REQUIRE(state.ready); CHECK(state.nozzles==nozzles); CHECK(state.physical_count==nozzles);
    for(const auto& slot:state.slots) CHECK_FALSE(slot.removable);
    auto draft=bundle;
    draft.project_config.option<ConfigOptionStrings>("filament_colour")->values.back().clear();
    CHECK_FALSE(PrinterFilamentConfig::capture(draft).ready);
    CHECK(bundle.project_config!=draft.project_config);
    CHECK(PrinterFilamentConfig::capture(bundle).ready);
    draft.project_config.option<ConfigOptionBools>("filament_is_mixed")->values.back()=true;
    state=PrinterFilamentConfig::capture(draft);
    CHECK(state.physical_count==nozzles-1); CHECK(state.slots.size()==nozzles); CHECK_FALSE(state.ready);
}

TEST_CASE("multiple nozzle base slots stay protected when native material removal is permissive", "[PrinterFilaments][PostGenerationWorkbench]")
{
    using namespace Slic3r;
    const size_t nozzles = GENERATE(4u, 6u);
    auto bundle = printer_filament_fixture(nozzles, nozzles + 1);
    bundle.printers.get_edited_preset().config.option<ConfigOptionBool>("single_extruder_multi_material")->value = true;
    REQUIRE(bundle.can_remove_physical_filament(0));
    const auto state = PrinterFilamentConfig::capture(bundle);
    for (size_t i = 0; i < nozzles; ++i) CHECK_FALSE(state.slots[i].removable);
    CHECK(state.slots.back().removable);
    std::string error;
    CHECK_FALSE(PrinterFilamentConfig::remove(bundle, 0, nozzles, error));
    REQUIRE(PrinterFilamentConfig::remove(bundle, nozzles, 0, error));
    CHECK(bundle.num_physical_filaments() == nozzles);
}

TEST_CASE("physical filament lists retain equal colors and more than six slot identities", "[PrinterFilaments][PostGenerationWorkbench]")
{
    auto bundle=printer_filament_fixture(1,9);
    auto state=PrinterFilamentConfig::capture(bundle);
    REQUIRE(state.ready); REQUIRE(state.slots.size()==9);
    for(size_t i=0;i<state.slots.size();++i){CHECK(state.slots[i].slot==i);CHECK(state.slots[i].color=="#123456");}
    const auto identity=state.identity;
    bundle.printers.get_edited_preset().config.option<Slic3r::ConfigOptionFloats>("nozzle_diameter")->values={.6};
    CHECK(PrinterFilamentConfig::capture(bundle).identity!=identity);
    bundle.filaments.find_preset("Loaded PLA",false)->is_compatible=false;
    CHECK_FALSE(PrinterFilamentConfig::capture(bundle).ready);
}

TEST_CASE("draft filament additions cancel without changing live colors and protect the nozzle floor", "[PrinterFilaments][PostGenerationWorkbench]")
{
    using namespace Slic3r;
    const size_t nozzles=GENERATE(1u,4u,6u);
    auto live=printer_filament_fixture(nozzles,nozzles);
    const auto before=live.project_config; const auto names=live.filament_presets;
    auto draft=live; const auto inserted=PrinterFilamentConfig::add(draft);
    CHECK(inserted==nozzles); CHECK(draft.filament_presets.size()==nozzles+1);
    CHECK_FALSE(PrinterFilamentConfig::capture(draft).ready);
    draft.filament_presets[inserted]="Loaded PLA";
    draft.project_config.option<ConfigOptionStrings>("filament_colour")->values[inserted]="#123456";
    REQUIRE(PrinterFilamentConfig::capture(draft).ready);
    std::string error; CHECK_FALSE(PrinterFilamentConfig::remove(draft,0,inserted,error));
    REQUIRE(PrinterFilamentConfig::remove(draft,inserted,0,error));
    CHECK(draft.filament_presets==names);
    CHECK(live.project_config==before); CHECK(live.filament_presets==names);
}

TEST_CASE("filament deletion substitutes mixed references or rejects duplicate components without live writes", "[PrinterFilaments][PostGenerationWorkbench]")
{
    using namespace Slic3r;
    auto live=printer_filament_fixture(1,4);
    live.project_config.option<ConfigOptionBools>("filament_is_mixed")->values[3]=true;
    live.project_config.option<ConfigOptionStrings>("filament_mixed_components")->values[3]="1,3";
    live.project_config.option<ConfigOptionStrings>("filament_mixed_sublayer_ratios")->values[3]="0.5,0.5";
    const auto before=live.project_config; std::string error;
    auto draft=live; REQUIRE(PrinterFilamentConfig::remove(draft,2,1,error));
    CHECK(draft.project_config.option<ConfigOptionStrings>("filament_mixed_components")->values[2]=="1,2");
    draft=live; CHECK_FALSE(PrinterFilamentConfig::remove(draft,2,0,error)); CHECK_FALSE(error.empty());
    CHECK(live.project_config==before);
    draft=live; const auto inserted=PrinterFilamentConfig::add(draft);
    CHECK(inserted==3); CHECK(draft.is_mixed_filament(4));
    CHECK(draft.project_config.option<ConfigOptionStrings>("filament_mixed_components")->values[4]=="1,3");
}

TEST_CASE("explicit compatible replacement can delete an extra spool of a different material", "[PrinterFilaments][PostGenerationWorkbench]")
{
    using namespace Slic3r;
    auto live = printer_filament_fixture(4,5);
    auto petg = live.filaments.find_preset("Loaded PLA", false)->config;
    petg.option<ConfigOptionStrings>("filament_type")->values = {"PETG"};
    live.filaments.load_preset("", "Loaded PETG", petg, false);
    live.filament_presets.back() = "Loaded PETG";
    const auto before = live.project_config;
    const auto names = live.filament_presets;
    auto staged = live;
    std::string error;
    REQUIRE(PrinterFilamentConfig::remove(staged,4,0,error));
    CHECK(staged.num_physical_filaments() == 4);
    CHECK(staged.filament_presets.front() == "Loaded PLA");
    CHECK(live.project_config == before);
    CHECK(live.filament_presets == names);
    staged = live;
    staged.filaments.find_preset("Loaded PLA", false)->is_compatible = false;
    CHECK_FALSE(PrinterFilamentConfig::remove(staged,4,0,error));
    CHECK(staged.filament_presets == names);
}

TEST_CASE("printer matching preserves source groups and semantic bindings and can use slots beyond six", "[PrinterFilaments][PostGenerationWorkbench]")
{
    using Color=PreviewPalette::Color;
    Slic3r::AI::ColorTrialPersistence::State state;
    const Color red{1,0,0}, blue{0,0,1};
    state.colors={red,blue};state.mapping_colors={Color{.8f,.1f,.1f},Color{.1f,.1f,.8f}};state.count=2;
    state.semantic_palette=state.colors;state.semantic_mapping_palette=state.mapping_colors;
    state.semantic_region_slots={0,1,0,1};state.source=2;
    const auto before=state;
    const std::vector<Color> actual={Color{.1f,.1f,.1f},red,red,Color{.3f,.3f,.3f},Color{.4f,.4f,.4f},Color{.5f,.5f,.5f},blue};
    const std::vector<size_t> slots={0,1,2,3,4,5,8};
    REQUIRE(PrinterFilamentPreview::apply(state,slots,actual,"printer-A"));
    CHECK(state.mapping_colors==before.mapping_colors);CHECK(state.semantic_mapping_palette==before.semantic_mapping_palette);
    CHECK(state.semantic_region_slots==before.semantic_region_slots);CHECK(state.project_color_slots==std::vector<size_t>{1,8});
    CHECK_FALSE(Slic3r::AI::ColorTrialPersistence::synchronize_project_targets(state,slots,actual,"printer-B"));
    state.project_color_slots[0]=2;state.project_semantic_slots[0]=2;
    REQUIRE(PrinterFilamentPreview::apply(state,slots,actual,"printer-A"));CHECK(state.project_color_slots[0]==2);
    BeautyWorkbenchTransactionController history;
    const auto after=state; history.record({BeautyWorkbenchTransactionController::OperationKind::AppearanceRecolor,"Apply printer filaments",
        [&]{state=before;},[&]{state=after;}});
    REQUIRE(history.undo());CHECK(state.mapping_colors==before.mapping_colors);CHECK(state.source==2);
    REQUIRE(history.redo());CHECK(state.project_color_slots==after.project_color_slots);
    const std::string geometry(64,'a');
    Slic3r::AI::ColorTrialPersistence::State restored;std::string error;
    REQUIRE(Slic3r::AI::ColorTrialPersistence::decode(Slic3r::AI::ColorTrialPersistence::encode(state,12,geometry),12,geometry,restored,error));
    CHECK(restored.project_color_slots==state.project_color_slots);CHECK(restored.mapping_colors==state.mapping_colors);
}

TEST_CASE("deleting a filament remaps object support painting and layer references without moving geometry", "[PrinterFilaments][PostGenerationWorkbench]")
{
    using namespace Slic3r;
    const int target=GENERATE(0,3);
    Model live;auto* object=live.add_object();auto* volume=object->add_volume(TriangleMesh(its_make_cube(10,10,10)));
    object->config.set("extruder",3);object->config.set("support_filament",3);object->config.set("support_interface_filament",0);
    volume->config.set("extruder",4);object->layer_config_ranges[{0.,1.}].set("extruder",3);
    TriangleSelector selector(volume->mesh());selector.set_facet(0,EnforcerBlockerType::Extruder3);selector.set_facet(1,EnforcerBlockerType::Extruder4);
    volume->mmu_segmentation_facets.set(selector);
    const auto geometry=volume->mesh().its.vertices;
    Model draft(live);PrinterFilamentSlotRemap::prepare(draft,{{},2,target},3,{});
    PrinterFilamentSlotRemap::commit(live,draft);
    const int expected=target==0?1:3;
    CHECK(object->config.opt_int("extruder")==expected);CHECK(object->config.opt_int("support_filament")==expected);
    CHECK(object->config.opt_int("support_interface_filament")==0);CHECK(volume->config.opt_int("extruder")==3);
    CHECK(object->layer_config_ranges.begin()->second.opt_int("extruder")==expected);
    TriangleSelector painted(volume->mesh());painted.deserialize(volume->mmu_segmentation_facets.get_data());
    CHECK(painted.get_facets(EnforcerBlockerType(expected)).indices.size()>=1);CHECK(painted.get_facets(EnforcerBlockerType::Extruder4).indices.empty());
    CHECK(volume->mesh().its.vertices==geometry);
}

TEST_CASE("plate sequences and routes follow inserted and substituted filament identities", "[PrinterFilaments][PostGenerationWorkbench]")
{
    using namespace Slic3r;
    DynamicPrintConfig config;
    config.set_key_value("filament_map",new ConfigOptionInts({1,2,3,4}));
    config.set_key_value("first_layer_print_sequence",new ConfigOptionInts({1,2,3,4}));
    config.set_key_value("other_layers_print_sequence_nums",new ConfigOptionInt(1));
    config.set_key_value("other_layers_print_sequence",new ConfigOptionInts({1,10,4,3,2,1}));
    PrinterFilamentSlotRemap::plate_config(config,{{},2,0});
    CHECK(config.option<ConfigOptionInts>("filament_map")->values==std::vector<int>{1,2,4});
    CHECK(config.option<ConfigOptionInts>("first_layer_print_sequence")->values==std::vector<int>{1,2,3});
    CHECK(config.option<ConfigOptionInts>("other_layers_print_sequence")->values==std::vector<int>{1,10,3,1,2});
    PrinterFilamentSlotRemap::plate_config(config,{{2},-1,-1});
    CHECK(config.option<ConfigOptionInts>("filament_map")->values==std::vector<int>{1,2,1,4});
    CHECK(config.option<ConfigOptionInts>("first_layer_print_sequence")->values==std::vector<int>{1,2,4,3});
}


TEST_CASE("new filament sequence entries follow later insertions before mixed slots", "[PrinterFilaments][PostGenerationWorkbench]")
{
    using namespace Slic3r;
    DynamicPrintConfig config;
    config.set_key_value("first_layer_print_sequence", new ConfigOptionInts({1,2,3,4}));
    config.set_key_value("other_layers_print_sequence_nums", new ConfigOptionInt(1));
    config.set_key_value("other_layers_print_sequence", new ConfigOptionInts({1,10,4,3,2,1}));
    // A pending nozzle slot was appended, then another physical slot was
    // inserted ahead of an existing virtual recipe. Both new identities survive.
    PrinterFilamentSlotRemap::plate_config(config, {{4,2},-1,-1});
    CHECK(config.option<ConfigOptionInts>("first_layer_print_sequence")->values == std::vector<int>{1,2,4,5,6,3});
    CHECK(config.option<ConfigOptionInts>("other_layers_print_sequence")->values == std::vector<int>{1,10,5,4,2,1,6,3});
}

TEST_CASE("short native filament tables show pending nozzle slots and refuse incompatible or oversized configurations", "[PrinterFilaments][PostGenerationWorkbench]")
{
    using namespace Slic3r;
    const size_t nozzles=GENERATE(4u,6u);
    auto bundle=printer_filament_fixture(nozzles,nozzles);
    bundle.filament_presets.resize(1);
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values.resize(1);
    auto state=PrinterFilamentConfig::capture(bundle);
    CHECK_FALSE(state.ready);CHECK(state.physical_count==1);REQUIRE(state.slots.size()==nozzles);
    for(size_t i=1;i<nozzles;++i){CHECK_FALSE(state.slots[i].exists);CHECK_FALSE(state.slots[i].configured);CHECK_FALSE(state.slots[i].removable);}
    bundle=printer_filament_fixture(1,33);
    state=PrinterFilamentConfig::capture(bundle);CHECK_FALSE(state.ready);CHECK(state.slots.size()==33);
    bundle=printer_filament_fixture(1,2);
    auto& bad=bundle.filaments.find_preset("Loaded PLA",false)->config;
    bad.option<ConfigOptionInts>("nozzle_temperature")->values={260};
    CHECK_FALSE(PrinterFilamentConfig::capture(bundle).ready);
}

static FilamentColorPack filament_import_card(size_t count)
{
    auto pack = young_portrait_color_pack();
    pack.colors.resize(count, "#123456");
    pack.labels.resize(count, "extra spool");
    return pack;
}

TEST_CASE("color card imports grow physical slots to the entire ordered palette", "[PrinterFilamentImport][PostGenerationWorkbench]")
{
    const size_t nozzles = GENERATE(1u, 4u, 6u);
    const size_t before = GENERATE(4u, 5u, 6u);
    const size_t colors = GENERATE(6u, 8u, 32u);
    auto live = printer_filament_fixture(nozzles, std::max(nozzles, before));
    const auto names = live.filament_presets;
    const auto project = live.project_config;
    auto draft = live;
    auto pack = filament_import_card(colors);
    std::vector<PrinterFilamentConfig::SlotChange> changes;
    std::string error;
    REQUIRE(PrinterFilamentConfig::import_color_pack(draft, pack, changes, error));
    const auto state = PrinterFilamentConfig::capture(draft);
    REQUIRE(state.ready);
    REQUIRE(state.physical_count == colors);
    REQUIRE(state.slots.size() == colors);
    for (size_t i = 0; i < colors; ++i) {
        CHECK(state.slots[i].slot == i);
        CHECK(state.slots[i].color == pack.colors[i]);
        CHECK(state.slots[i].preset == "Loaded PLA");
    }
    CHECK(PrinterFilamentSlotRemap::validate(changes, names.size(), colors));
    CHECK(live.project_config == project);
    CHECK(live.filament_presets == names);
}

TEST_CASE("color card imports shrink extra slots while retaining the printer nozzle floor", "[PrinterFilamentImport][PostGenerationWorkbench]")
{
    const size_t nozzles = GENERATE(1u, 4u, 6u);
    const size_t colors = GENERATE(2u, 4u, 6u);
    auto live = printer_filament_fixture(nozzles, 8);
    auto draft = live;
    std::vector<PrinterFilamentConfig::SlotChange> changes;
    std::string error;
    size_t choices = 0;
    REQUIRE(PrinterFilamentConfig::import_color_pack(draft, filament_import_card(colors), changes, error,
        [&](const auto& source, const auto& targets) -> std::optional<size_t> {
            CHECK(source.removable);
            ++choices;
            return targets.front().slot;
        }));
    const auto state = PrinterFilamentConfig::capture(draft);
    CHECK(state.ready);
    CHECK(state.physical_count == std::max(nozzles, colors));
    CHECK(choices == 8 - std::max(nozzles, colors));
    CHECK(PrinterFilamentSlotRemap::validate(changes, 8, draft.filament_presets.size()));
    CHECK(live.filament_presets.size() == 8);
}

TEST_CASE("cancelled or invalid card reductions preserve the entire draft and its pending changes", "[PrinterFilamentImport][PostGenerationWorkbench]")
{
    auto draft = printer_filament_fixture(4, 8);
    const auto project = draft.project_config;
    const auto names = draft.filament_presets;
    std::vector<PrinterFilamentConfig::SlotChange> changes;
    std::string error;
    size_t choices = 0;
    CHECK_FALSE(PrinterFilamentConfig::import_color_pack(draft, filament_import_card(4), changes, error,
        [&](const auto&, const auto& targets) -> std::optional<size_t> {
            if (++choices == 2) return std::nullopt;
            return targets.front().slot;
        }));
    CHECK(choices == 2);
    CHECK(draft.project_config == project);
    CHECK(draft.filament_presets == names);
    CHECK(changes.empty());
    CHECK_FALSE(error.empty());
    CHECK_FALSE(PrinterFilamentConfig::import_color_pack(draft, filament_import_card(4), changes, error,
        [](const auto&, const auto&) -> std::optional<size_t> { return 999; }));
    CHECK(draft.project_config == project);
    CHECK(draft.filament_presets == names);
}

TEST_CASE("virtual mixtures keep their identities and do not consume card colors", "[PrinterFilamentImport][PostGenerationWorkbench]")
{
    using namespace Slic3r;
    auto live = printer_filament_fixture(4, 6);
    live.project_config.option<ConfigOptionBools>("filament_is_mixed")->values.back() = true;
    live.project_config.option<ConfigOptionStrings>("filament_mixed_components")->values.back() = "1,5";
    live.project_config.option<ConfigOptionStrings>("filament_mixed_sublayer_ratios")->values.back() = "0.5,0.5";
    auto draft = live;
    std::vector<PrinterFilamentConfig::SlotChange> changes;
    std::string error;
    REQUIRE(PrinterFilamentConfig::import_color_pack(draft, filament_import_card(8), changes, error));
    CHECK(PrinterFilamentConfig::capture(draft).physical_count == 8);
    CHECK(draft.filament_presets.size() == 9);
    CHECK(draft.is_mixed_filament(8));
    CHECK(draft.project_config.option<ConfigOptionStrings>("filament_mixed_components")->values[8] == "1,5");
    REQUIRE(PrinterFilamentConfig::import_color_pack(draft, filament_import_card(4), changes, error,
        [](const auto&, const auto& targets) -> std::optional<size_t> { return targets.back().slot; }));
    CHECK(PrinterFilamentConfig::capture(draft).physical_count == 4);
    CHECK(draft.is_mixed_filament(4));
    CHECK(draft.project_config.option<ConfigOptionStrings>("filament_mixed_components")->values[4] == "1,4");
    CHECK(PrinterFilamentSlotRemap::validate(changes, 6, 5));

    draft = live;
    changes.clear();
    const auto project = draft.project_config;
    CHECK_FALSE(PrinterFilamentConfig::import_color_pack(draft, filament_import_card(32), changes, error));
    CHECK(draft.project_config == project);
    CHECK(changes.empty());
}

TEST_CASE("card import leaves missing or incompatible material choices pending without dropping colors", "[PrinterFilamentImport][PostGenerationWorkbench]")
{
    using namespace Slic3r;
    auto draft = printer_filament_fixture(4, 5);
    auto incompatible = draft.filaments.find_preset("Loaded PLA", false)->config;
    draft.filaments.load_preset("", "Incompatible PLA", incompatible, false);
    draft.filaments.find_preset("Incompatible PLA", false)->is_compatible = false;
    auto pack = filament_import_card(6);
    pack.presets = {"Loaded PLA", "missing", "Incompatible PLA", "Loaded PLA", "Loaded PLA", "Loaded PLA"};
    std::vector<PrinterFilamentConfig::SlotChange> changes;
    std::string error;
    REQUIRE(PrinterFilamentConfig::import_color_pack(draft, pack, changes, error));
    const auto state = PrinterFilamentConfig::capture(draft);
    CHECK_FALSE(state.ready);
    CHECK(state.physical_count == 6);
    for (size_t i = 0; i < 6; ++i) CHECK(state.slots[i].color == pack.colors[i]);
    CHECK(state.slots[1].preset.empty());
    CHECK(state.slots[2].preset.empty());
    draft.filament_presets[1] = "Loaded PLA";
    draft.filament_presets[2] = "Loaded PLA";
    CHECK(PrinterFilamentConfig::capture(draft).ready);
}

TEST_CASE("successive card imports replay additions and deletions through object painting and plate references", "[PrinterFilamentImport][PostGenerationWorkbench]")
{
    using namespace Slic3r;
    auto draft = printer_filament_fixture(4, 8);
    draft.project_config.option<ConfigOptionBools>("filament_is_mixed")->values[7] = true;
    draft.project_config.option<ConfigOptionStrings>("filament_mixed_components")->values[7] = "1,7";
    draft.project_config.option<ConfigOptionStrings>("filament_mixed_sublayer_ratios")->values[7] = "0.5,0.5";
    const auto mixed = draft.project_config.option<ConfigOptionBools>("filament_is_mixed")->values;
    Model model;
    auto* object = model.add_object();
    auto* volume = object->add_volume(TriangleMesh(its_make_cube(10,10,10)));
    object->config.set("extruder", 7);
    volume->config.set("extruder", 8);
    TriangleSelector selector(volume->mesh());
    selector.set_facet(0, EnforcerBlockerType::Extruder7);
    selector.set_facet(1, EnforcerBlockerType::Extruder8);
    volume->mmu_segmentation_facets.set(selector);
    const auto vertices = volume->mesh().its.vertices;
    std::vector<PrinterFilamentConfig::SlotChange> changes;
    std::string error;
    auto choose = [](const auto&, const auto& targets) -> std::optional<size_t> { return targets.back().slot; };
    REQUIRE(PrinterFilamentConfig::import_color_pack(draft, filament_import_card(4), changes, error, choose));
    REQUIRE(PrinterFilamentConfig::import_color_pack(draft, filament_import_card(8), changes, error, choose));
    REQUIRE(PrinterFilamentConfig::import_color_pack(draft, filament_import_card(6), changes, error, choose));
    REQUIRE(PrinterFilamentSlotRemap::validate(changes, 8, 7));
    PrinterFilamentSlotRemap::prepare(model, changes, 8, mixed);
    CHECK(object->config.opt_int("extruder") == 4);
    CHECK(volume->config.opt_int("extruder") == 7);
    TriangleSelector painted(volume->mesh());
    painted.deserialize(volume->mmu_segmentation_facets.get_data());
    CHECK_FALSE(painted.get_facets(EnforcerBlockerType::Extruder4).indices.empty());
    CHECK_FALSE(painted.get_facets(EnforcerBlockerType::Extruder7).indices.empty());
    CHECK(painted.get_facets(EnforcerBlockerType::Extruder8).indices.empty());
    CHECK(volume->mesh().its.vertices == vertices);
    DynamicPrintConfig plate;
    plate.set_key_value("first_layer_print_sequence", new ConfigOptionInts({1,2,3,4,5,6,7,8}));
    for (const auto& change : changes) PrinterFilamentSlotRemap::plate_config(plate, change);
    auto sequence = plate.option<ConfigOptionInts>("first_layer_print_sequence")->values;
    std::sort(sequence.begin(), sequence.end());
    CHECK(sequence == std::vector<int>{1,2,3,4,5,6,7});
}

TEST_CASE("returning to an identical filament card can still change model references", "[PrinterFilamentImport][PostGenerationWorkbench]")
{
    using namespace Slic3r;
    auto live = printer_filament_fixture(4, 8);
    const auto full_card = capture_current_filament_color_pack(live);
    auto shorter = full_card;
    shorter.colors.resize(4); shorter.labels.resize(4); shorter.presets.resize(4);
    shorter.multi_colors.resize(4); shorter.color_types.resize(4);
    auto draft = live;
    std::vector<PrinterFilamentConfig::SlotChange> changes;
    std::string error;
    REQUIRE(PrinterFilamentConfig::import_color_pack(draft, shorter, changes, error,
        [](const auto&, const auto& targets) -> std::optional<size_t> { return targets.back().slot; }));
    REQUIRE(PrinterFilamentConfig::import_color_pack(draft, full_card, changes, error));
    REQUIRE(draft.project_config == live.project_config);
    REQUIRE(draft.filament_presets == live.filament_presets);
    CHECK_FALSE(UndoRedo::ProjectConfigUndo::Change::capture(live.project_config, live.filament_presets,
        draft.project_config, draft.filament_presets));
    Model model;
    auto* object = model.add_object();
    object->add_volume(TriangleMesh(its_make_cube(10,10,10)));
    object->config.set("extruder", 7);
    REQUIRE(PrinterFilamentSlotRemap::validate(changes, 8, 8));
    PrinterFilamentSlotRemap::prepare(model, changes, 8, {});
    CHECK(object->config.opt_int("extruder") == 4);
}
