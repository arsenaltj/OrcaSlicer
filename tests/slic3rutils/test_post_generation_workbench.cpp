#include "slic3r/GUI/TextureImportModel.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "slic3r/GUI/AI/ModelGeneration/PostGenerationUiState.hpp"
#include "slic3r/GUI/AI/ModelGeneration/PostGenerationWorkbenchState.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelPreviewTextureColor.hpp"
#include "slic3r/GUI/AI/ModelGeneration/WorkbenchImportSession.hpp"
#include "slic3r/GUI/AI/ModelGeneration/WorkbenchImportEligibility.hpp"
#include "slic3r/GUI/AI/ModelGeneration/WorkbenchImportUiGuard.hpp"
#include "slic3r/GUI/AI/ModelGeneration/WorkbenchProjectColor.hpp"
#include "slic3r/GUI/AI/ModelGeneration/WorkbenchModelInspection.hpp"
#include "slic3r/GUI/AI/ModelGeneration/WorkbenchRoundBasePreview.hpp"
#include "slic3r/GUI/AI/Model/ModelFinishing.hpp"
#include "slic3r/GUI/AI/Model/BeautyDocument.hpp"
#include "slic3r/GUI/AI/Orca/AIImportSeamRepair.hpp"
#include "slic3r/GUI/AI/Orca/WorkbenchImportTopology.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/FilamentMixer.hpp"
#include "test_utils.hpp"
#include <boost/filesystem/fstream.hpp>
#include <cstdlib>
#include "slic3r/GUI/AI/ModelGeneration/BeautyColorPaint.hpp"
#include "slic3r/GUI/AI/ModelGeneration/BeautyWorkbenchTransactionController.hpp"
#include "slic3r/GUI/AI/SmartSlicing/SmartSlicingWorkbenchState.hpp"
#include "slic3r/GUI/AI/Orca/OrcaPlateRevisionConfig.hpp"
#include "slic3r/GUI/AI/Orca/WorkbenchModelDecode.hpp"
#include "slic3r/GUI/AI/Orca/LocalPrintModelImport.hpp"
#include "slic3r/GUI/AI/Orca/SlicingModelPalette.hpp"
#include "slic3r/GUI/Redesign/RedesignModelRoute.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelPreviewColorState.hpp"
#include "slic3r/GUI/AI/ModelGeneration/PrinterFilamentPreview.hpp"
#include "slic3r/GUI/AI/Orca/WorkbenchPrinterFilaments.hpp"
#include "slic3r/GUI/PrinterFilamentSlotRemap.hpp"
#ifdef __WXMSW__
#include "slic3r/GUI/AI/ModelGeneration/WorkbenchStyle.hpp"
#endif
#include <future>
#include <limits>
#include <memory>
#include <type_traits>
#include <vector>

using namespace Slic3r::GUI;

TEST_CASE("Workbench palette status keeps Honor fallback reasons visible",
          "[PostGenerationWorkbench][ModelPreviewPalette][UiRedesign]")
{
    CHECK(workbench_palette_status_line(false, true, 4, 4) == "原始配色证据不可用");
    CHECK(workbench_palette_status_line(true, false, 0, 6) == "当前色卡 · 6 色");
    CHECK(workbench_palette_status_line(true, true, 5, 6) ==
        "荣耀耗材套装 · 模型当前使用 5/6 色");
    CHECK(workbench_palette_status_line(true, true, 4, 4,
        "未检测到人物，已回退为全局相近色匹配。") ==
        "未检测到人物，已回退为全局相近色匹配。\n模型当前使用 4/6 色");
}

#ifdef __WXMSW__
TEST_CASE("Workbench slider custom draw ignores tooltip window notifications",
          "[PostGenerationWorkbench][UiRedesign]")
{
    int slider_storage = 0;
    int tooltip_storage = 0;
    const WXHWND slider = reinterpret_cast<WXHWND>(&slider_storage);
    NMHDR notification {};
    notification.code = NM_CUSTOMDRAW;
    notification.hwndFrom = reinterpret_cast<HWND>(slider);
    CHECK(workbench_slider_owns_custom_draw(slider, notification));

    notification.hwndFrom = reinterpret_cast<HWND>(&tooltip_storage);
    CHECK_FALSE(workbench_slider_owns_custom_draw(slider, notification));
    notification.code = NM_CUSTOMDRAW + 1;
    notification.hwndFrom = reinterpret_cast<HWND>(slider);
    CHECK_FALSE(workbench_slider_owns_custom_draw(slider, notification));
}
#endif

TEST_CASE("AI palette auto-application waits for a safe automatic check completion",
          "[PostGenerationWorkbench][ModelPreviewPalette]")
{
    CHECK(workbench_ai_palette_should_apply(true, WorkbenchCheckSource::Automatic, true,
        RepairCandidateStatus::None, false));
    CHECK(workbench_ai_palette_should_apply(true, WorkbenchCheckSource::Automatic, true,
        RepairCandidateStatus::Ready, true));

    CHECK_FALSE(workbench_ai_palette_should_apply(false, WorkbenchCheckSource::Automatic, true,
        RepairCandidateStatus::None, false));
    CHECK_FALSE(workbench_ai_palette_should_apply(true, WorkbenchCheckSource::Manual, true,
        RepairCandidateStatus::None, false));
    CHECK_FALSE(workbench_ai_palette_should_apply(true, WorkbenchCheckSource::Automatic, false,
        RepairCandidateStatus::None, false));
    CHECK_FALSE(workbench_ai_palette_should_apply(true, WorkbenchCheckSource::Automatic, true,
        RepairCandidateStatus::Ready, false));
    CHECK_FALSE(workbench_ai_palette_should_apply(true, WorkbenchCheckSource::Automatic, true,
        RepairCandidateStatus::Failed, true));
}

TEST_CASE("Round base preview is reversible and bound to one geometry identity",
          "[PostGenerationWorkbench][OrcaModelPreparation]")
{
    Slic3r::BoundingBoxf3 bounds;
    bounds.merge(Slic3r::Vec3d(-10., -20., 5.));
    bounds.merge(Slic3r::Vec3d(10., 20., 45.));
    WorkbenchRoundBasePreviewState preview;
    preview.reset("geometry-a");
    CHECK_FALSE(preview.enabled("geometry-a"));
    REQUIRE(preview.available(bounds));

    std::string error;
    REQUIRE(preview.set_enabled(true, bounds, error));
    CHECK(error.empty());
    REQUIRE(preview.enabled("geometry-a"));
    REQUIRE_FALSE(preview.enabled("geometry-b"));
    const auto layout = preview.layout("geometry-a", bounds);
    REQUIRE(layout);
    CHECK_THAT(layout->height, Catch::Matchers::WithinAbs(3.0, 1e-9));
    CHECK_THAT(layout->body_scale, Catch::Matchers::WithinAbs(0.93, 1e-9));
    CHECK_THAT(layout->radius, Catch::Matchers::WithinAbs(20.6, 1e-9));
    CHECK_THAT((layout->body_transform * Slic3r::Vec3d(0, 0, 5)).z(),
        Catch::Matchers::WithinAbs(7.8, 1e-9));
    CHECK_THAT((layout->body_transform * Slic3r::Vec3d(0, 0, 45)).z(),
        Catch::Matchers::WithinAbs(45.0, 1e-9));
    CHECK_THAT((layout->base_transform * Slic3r::Vec3d(0, 0, 0)).z(),
        Catch::Matchers::WithinAbs(5.0, 1e-9));

    REQUIRE(preview.set_enabled(false, bounds, error));
    CHECK_FALSE(preview.layout("geometry-a", bounds));
    preview.reset("geometry-b");
    CHECK_FALSE(preview.enabled("geometry-b"));
}

TEST_CASE("Round base preview rejects unsafe dimensions without retaining the toggle",
          "[PostGenerationWorkbench][OrcaModelPreparation]")
{
    Slic3r::BoundingBoxf3 bounds;
    bounds.merge(Slic3r::Vec3d(0., 0., 0.));
    bounds.merge(Slic3r::Vec3d(20., 20., 3.));
    WorkbenchRoundBasePreviewState preview;
    preview.reset("geometry");
    std::string error;
    CHECK_FALSE(preview.set_enabled(true, bounds, error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(preview.enabled("geometry"));
}

TEST_CASE("Workbench filament rows use native compact height and mixed-slot identity mapping",
          "[PostGenerationWorkbench][PrinterFilaments]")
{
    CHECK(PrinterFilamentConfig::workbench_row_height_dip == 28);
    const std::vector<size_t> mixed {3, 6, 9};
    CHECK(PrinterFilamentConfig::mixed_panel_index(mixed, 3) == 0);
    CHECK(PrinterFilamentConfig::mixed_panel_index(mixed, 6) == 1);
    CHECK(PrinterFilamentConfig::mixed_panel_index(mixed, 9) == 2);
    CHECK_FALSE(PrinterFilamentConfig::mixed_panel_index(mixed, 4));
}

TEST_CASE("Workbench import keeps round base preparation opt-in",
          "[PostGenerationWorkbench][OrcaModelPreparation]")
{
    Slic3r::AI::ModelImportRequest request;
    CHECK_FALSE(request.apply_round_base_preparation);
    request.apply_round_base_preparation = true;
    CHECK(request.apply_round_base_preparation);
}

TEST_CASE("Workbench placement adds a round base only to its detached import copy",
          "[PostGenerationWorkbench][OrcaModelPreparation][LocalPrintModelImport]")
{
    using namespace Slic3r;
    Model source;
    auto* object = source.add_object();
    object->input_file = "portrait.glb";
    auto* body = object->add_volume(TriangleMesh(its_make_cube(20, 30, 40)));
    body->source.input_file = object->input_file;
    body->config.set("extruder", 3);

    std::unique_ptr<Model> prepared;
    std::string error;
    REQUIRE(LocalPrintModelImport::prepare(*object, {100, 100}, {250, 250}, prepared, error, true));
    REQUIRE(error.empty());
    REQUIRE(prepared);
    REQUIRE(prepared->objects.size() == 1);
    const auto* imported = prepared->objects.front();
    REQUIRE(imported->instances.size() == 1);
    REQUIRE(imported->volumes.size() == 2);
    CHECK(imported->volumes.back()->name == "AI round base");
    CHECK(imported->volumes.back()->extruder_id() == 3);
    CHECK_THAT(imported->instance_bounding_box(0).size().z(), Catch::Matchers::WithinAbs(40., 0.001));
    TriangleMesh base_mesh = imported->volumes.back()->mesh();
    base_mesh.transform(imported->instances.front()->get_matrix() * imported->volumes.back()->get_matrix());
    const auto base_bounds = base_mesh.bounding_box();
    CHECK_THAT(base_bounds.min.z(), Catch::Matchers::WithinAbs(0., 0.001));
    CHECK_THAT(base_bounds.max.z(), Catch::Matchers::WithinAbs(3., 0.001));
    TriangleMesh body_mesh = imported->volumes.front()->mesh();
    body_mesh.transform(imported->instances.front()->get_matrix() * imported->volumes.front()->get_matrix());
    const auto body_bounds = body_mesh.bounding_box();
    CHECK_THAT(body_bounds.min.z(), Catch::Matchers::WithinAbs(2.8, 0.001));
    CHECK_THAT(base_bounds.size().x(), Catch::Matchers::WithinAbs(base_bounds.size().y(), 0.001));
    CHECK_THAT(base_bounds.size().x(), Catch::Matchers::WithinAbs(
        std::max(body_bounds.size().x(), body_bounds.size().y()) + 4.0, 0.001));
    CHECK(object->instances.empty());
    REQUIRE(object->volumes.size() == 1);

    Model too_short;
    auto* unsafe = too_short.add_object();
    unsafe->input_file = "unsafe.glb";
    auto* unsafe_body = unsafe->add_volume(TriangleMesh(its_make_cube(10, 10, 3)));
    unsafe_body->source.input_file = unsafe->input_file;
    std::unique_ptr<Model> rejected;
    CHECK_FALSE(LocalPrintModelImport::prepare(*unsafe, {100, 100}, {250, 250}, rejected, error, true));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(rejected);
    CHECK(unsafe->instances.empty());
    REQUIRE(unsafe->volumes.size() == 1);
}

TEST_CASE("An OBJ repair snapshot saves physical face colors through the appearance pipeline", "[WorkbenchTopology][BeautyAppearance]")
{
    using namespace Slic3r;
    ScopedTemporaryDir directory("repair-appearance");
    const boost::filesystem::path source = boost::filesystem::path(directory.string()) / "repaired.obj";
    const auto format = GENERATE("obj", "glb");
    const auto destination = source.parent_path() / (std::string("applied.") + format);
    const auto cube = its_make_cube(10, 10, 10);
    std::string error;
    REQUIRE(AI::write_model_artifact(source, cube, std::vector<RGBA>(cube.vertices.size(), {0.2f, 0.4f, 0.6f, 1.f}), error));
    const auto source_hash = AI::model_artifact_sha256(source);
    TriangleMesh original; ObjInfo colors;
    REQUIRE(AI::load_model_artifact(source, original, colors, error));
    AI::BeautyDocument document;
    document.geometry_id = AI::SurfaceSelectionPersistence::geometry_fingerprint(original.its);
    document.face_count = original.its.indices.size(); document.face_patch.assign(document.face_count, 0);
    AI::ModelFinishingOptions options;
    options.smooth_surface = options.repair_mesh = false; options.beauty_appearance = true;
    options.beauty_document = document.encode();
    options.appearance.face_weights.assign(document.face_count, 1.f);
    for (size_t face = 0; face < document.face_count; ++face) {
        options.selected_faces.push_back(face);
        options.appearance.face_target_colors.push_back(face % 2 ? std::array<float, 3>{1.f, 0.f, 0.f} : std::array<float, 3>{0.f, 1.f, 0.f});
    }
    const auto saved = AI::finish_model_artifact(source, destination, options);
    INFO(saved.error);
    REQUIRE(saved.success);
    CHECK(saved.source_sha256 == source_hash);
    TriangleMesh output; ObjInfo painted;
    REQUIRE(AI::load_model_artifact(destination, output, painted, error));
    REQUIRE(output.its.indices.size() == original.its.indices.size());
    for (size_t face = 0; face < output.its.indices.size(); ++face) for (int corner = 0; corner < 3; ++corner) {
        const auto id = output.its.indices[face][corner];
        CHECK((output.its.vertices[id] - original.its.vertices[original.its.indices[face][corner]]).norm() < 1e-4f);
        for (int channel = 0; channel < 3; ++channel)
            CHECK_THAT(painted.vertex_colors[id][channel], Catch::Matchers::WithinAbs(options.appearance.face_target_colors[face][channel], 0.005f));
    }
    AI::ModelImportResult imported;
    REQUIRE(prepare_workbench_import_topology(output, false, imported, error));
    CHECK_FALSE(imported.topology_risk);
    CHECK(AI::model_artifact_sha256(source) == source_hash);
    const auto cancelled_path = source.parent_path() / (std::string("cancelled.") + format);
    const auto cancelled = AI::finish_model_artifact(source, cancelled_path, options, [] { return true; });
    CHECK(cancelled.canceled);
    CHECK_FALSE(boost::filesystem::exists(cancelled_path));
}

TEST_CASE("A local repaired model remains closed through native color import", "[.WorkbenchRepairQA]")
{
    using namespace Slic3r;
    const char* fixture = std::getenv("ORCASLICER_MODEL_ARTIFACT_FIXTURE");
    REQUIRE(fixture);
    const boost::filesystem::path source(fixture);
    const auto source_hash = AI::model_artifact_sha256(source);
    const auto directory = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("orca-repair-qa-%%%%%%%%");
    boost::filesystem::create_directories(directory);
    struct Cleanup {
        boost::filesystem::path path;
        ~Cleanup() { boost::system::error_code ec; boost::filesystem::remove_all(path, ec); }
    } cleanup {directory};
    const auto candidate = directory / "repaired.obj";
    AI::ModelFinishingOptions options;
    options.smooth_surface = false;
    options.repair_mesh = true;
    const auto repaired = AI::finish_model_artifact(source, candidate, options);
    INFO(repaired.error);
    REQUIRE(repaired.success);
    REQUIRE(repaired.changed());
    if (const char* output = std::getenv("ORCASLICER_REPAIR_QA_OUTPUT")) {
        boost::filesystem::create_directories(output);
        boost::filesystem::copy_file(candidate, boost::filesystem::path(output) / "candidate.obj");
    }
    const auto check = inspect_workbench_model(candidate);
    INFO(check.summary);
    if (const char* report = std::getenv("ORCASLICER_REPAIR_QA_REPORT")) {
        boost::filesystem::ofstream output(report);
        output << nlohmann::json {{"source_sha256", source_hash}, {"faces_before", repaired.faces_before},
            {"faces_after", repaired.faces_after}, {"summary", check.summary}, {"status", int(check.status)}}.dump(2);
    }
    CHECK(check.boundary_edges == 0);
    CHECK(check.nonmanifold_edges == 0);
    CHECK(check.nonmanifold_vertices == 0);
    CHECK(check.self_intersections == 0);
    CHECK(check.status == WorkbenchCheckStatus::Normal);
    DecodedWorkbenchModel decoded;
    std::string error;
    REQUIRE(decode_workbench_model(candidate, decoded, error));
    CHECK(its_num_open_edges(decoded.mesh.its) == 0);
    auto model = assemble_workbench_model(decoded, candidate, false);
    REQUIRE(model->texture_mesh);
    auto* volume = model->objects.front()->volumes.front();
    const auto face_corners = volume->mesh().its;
    PaintedMesh painted;
    painted.vertices = model->texture_mesh->vertices;
    painted.indices = model->texture_mesh->indices;
    painted.cluster_colors = {{204,128,77}, {242,242,242}};
    for (size_t face = 0; face < painted.indices.size(); ++face)
        painted.face_colors.push_back(painted.cluster_colors[face % 2]);
    std::vector<FilamentMatch> matches(2);
    for (int i = 0; i < 2; ++i) { matches[i].cluster_index = i; matches[i].filament_index = i; }
    REQUIRE(apply_painted_mesh_to_volume(painted, matches, *volume, true));
    const auto before_painting = volume->mmu_segmentation_facets.get_data();
    AI::ModelImportResult imported;
    TriangleMesh final_mesh = volume->mesh();
    const bool topology_ok = prepare_workbench_import_topology(final_mesh, false, imported, error);
    INFO(error);
    REQUIRE(topology_ok);
    CHECK_FALSE(imported.topology_risk);
    CHECK(its_num_open_edges(final_mesh.its) == 0);
    REQUIRE(LocalPrintColorApplication::same_surface_partition(face_corners, final_mesh.its, false));
    volume->set_mesh(std::move(final_mesh));
    CHECK(volume->mmu_segmentation_facets.get_data() == before_painting);
    model->texture_mesh.reset();
    model->objects.front()->add_instance();
    ScopedTemporaryDir backup("repaired-qa-backup"); model->set_backup_path(backup.string());
    const auto native = directory / "repaired.3mf";
    const std::string native_path = native.string();
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    PlateData plate; plate.plate_index = 0;
    StoreParams params; params.path = native_path.c_str(); params.model = model.get(); params.config = &config;
    params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence; params.plate_data_list.push_back(&plate);
    REQUIRE(store_bbs_3mf(params));
    Model restored; ScopedTemporaryDir restored_backup("repaired-qa-restored"); restored.set_backup_path(restored_backup.string());
    DynamicPrintConfig restored_config;
    ConfigSubstitutionContext substitutions {ForwardCompatibilitySubstitutionRule::Enable};
    PlateDataPtrs plates; std::vector<Preset*> presets; bool bbl = false, orca = false; Semver version;
    const bool loaded = load_bbs_3mf(native_path.c_str(), &restored_config, &substitutions, &restored, &plates,
        &presets, &bbl, &orca, &version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig);
    release_PlateData_list(plates); for (auto* preset : presets) delete preset;
    REQUIRE(loaded);
    REQUIRE(restored.objects.size() == 1);
    auto* reopened = restored.objects.front()->volumes.front();
    CHECK(its_num_open_edges(reopened->mesh().its) == 0);
    CHECK_FALSE(MeshBoolean::cgal::does_self_intersect(reopened->mesh()));
    CHECK(reopened->mmu_segmentation_facets.get_data() == before_painting);
    if (const char* output = std::getenv("ORCASLICER_REPAIR_QA_OUTPUT")) {
        const boost::filesystem::path output_directory(output);
        boost::filesystem::create_directories(output_directory);
        boost::filesystem::copy_file(candidate, output_directory / "repaired.obj");
        boost::filesystem::copy_file(native, output_directory / "repaired.3mf");
    }
    CHECK(AI::model_artifact_sha256(source) == source_hash);
}

TEST_CASE("Native workbench painting preserves distinct near-coincident repaired coordinates", "[WorkbenchTopology][WorkbenchImportUnits]")
{
    using namespace Slic3r;
    // Separate closed tetrahedra face each other across a representable gap.
    // Subtracting their bbox center in float would erase that gap and join
    // their faces during the final seam pass.
    indexed_triangle_set raw;
    raw.vertices = {{0,0,0}, {0,1,0}, {0,0,1}, {-10,0,0},
                    {1e-8f,0,0}, {1e-8f,1,0}, {1e-8f,0,1}, {1,0,0}};
    raw.indices = {{0,1,2}, {0,3,1}, {0,2,3}, {1,3,2},
                   {4,6,5}, {4,5,7}, {4,7,6}, {5,6,7}};
    REQUIRE(workbench_topology_safe(inspect_workbench_mesh(TriangleMesh(raw))));
    DecodedWorkbenchModel decoded;
    decoded.mesh = TriangleMesh(raw);
    auto model = assemble_workbench_model(decoded, "near-coincident.obj", false);
    auto* volume = model->objects.front()->volumes.front();
    CHECK(volume->mesh().its.vertices == raw.vertices);
    PaintedMesh painted;
    painted.cluster_colors = {{200,100,50}, {50,100,200}};
    for (size_t face = 0; face < raw.indices.size(); ++face) {
        const int begin = int(painted.vertices.size());
        for (int vertex : raw.indices[face]) {
            const auto& p = raw.vertices[vertex]; painted.vertices.push_back({p.x(),p.y(),p.z()});
        }
        painted.indices.push_back({begin,begin+1,begin+2});
        painted.face_colors.push_back(painted.cluster_colors[face % 2]);
    }
    std::vector<FilamentMatch> matches(2);
    for (int i = 0; i < 2; ++i) { matches[i].cluster_index = i; matches[i].filament_index = i; }
    REQUIRE(apply_painted_mesh_to_volume(painted, matches, *volume, true));
    const auto painting = volume->mmu_segmentation_facets.get_data();
    TriangleMesh final_mesh = volume->mesh();
    AI::ModelImportResult result; std::string error;
    const bool safe = prepare_workbench_import_topology(final_mesh, false, result, error);
    INFO(error);
    REQUIRE(safe);
    CHECK_FALSE(result.topology_risk);
    CHECK(final_mesh.its.indices.size() == raw.indices.size());
    volume->set_mesh(std::move(final_mesh));
    CHECK(volume->mmu_segmentation_facets.get_data() == painting);
    std::unique_ptr<Model> placed;
    REQUIRE(LocalPrintModelImport::prepare(*model->objects.front(), {100,100}, {300,300}, placed, error));
    CHECK(workbench_topology_safe(inspect_workbench_mesh(placed->objects.front()->volumes.front()->mesh())));
    // Save a shared-mesh copy too: archive loading must preserve the source
    // frame for both the first volume and subsequent shared volumes.
    placed->add_object(*placed->objects.front());
    ScopedTemporaryDir directory("source-frame-roundtrip");
    ScopedTemporaryDir backup("source-frame-backup"); placed->set_backup_path(backup.string());
    const std::string native_path = (boost::filesystem::path(directory.string()) / "placed.3mf").string();
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    PlateData plate; plate.plate_index = 0;
    StoreParams params; params.path = native_path.c_str(); params.model = placed.get(); params.config = &config;
    params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SplitModel | SaveStrategy::ShareMesh;
    params.plate_data_list.push_back(&plate);
    REQUIRE(store_bbs_3mf(params));
    Model restored; ScopedTemporaryDir restored_backup("source-frame-restored"); restored.set_backup_path(restored_backup.string());
    DynamicPrintConfig restored_config;
    ConfigSubstitutionContext substitutions {ForwardCompatibilitySubstitutionRule::Enable};
    PlateDataPtrs plates; std::vector<Preset*> presets; bool bbl = false, orca = false; Semver version;
    const bool loaded = load_bbs_3mf(native_path.c_str(), &restored_config, &substitutions, &restored, &plates,
        &presets, &bbl, &orca, &version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig);
    release_PlateData_list(plates); for (auto* preset : presets) delete preset;
    REQUIRE(loaded);
    REQUIRE(restored.objects.size() == 2);
    CHECK(restored.objects[0]->volumes[0]->get_mesh_shared_ptr() == restored.objects[1]->volumes[0]->get_mesh_shared_ptr());
    for (const auto* object : restored.objects) {
        const auto* reopened = object->volumes.front();
        CHECK(workbench_topology_safe(inspect_workbench_mesh(reopened->mesh())));
        CHECK(reopened->mmu_segmentation_facets.get_data() == painting);
        CHECK(reopened->mesh().its.vertices == volume->mesh().its.vertices);
        CHECK_THAT((object->instance_bounding_box(0).center() - placed->objects.front()->instance_bounding_box(0).center()).norm(),
            Catch::Matchers::WithinAbs(0., 1e-6));
    }
}

TEST_CASE("A checked local artifact retains its topology through native placement and painting", "[.WorkbenchArtifactImportQA]")
{
    using namespace Slic3r;
    const char* fixture = std::getenv("ORCASLICER_MODEL_ARTIFACT_FIXTURE");
    REQUIRE(fixture);
    const boost::filesystem::path path(fixture);
    const auto original_hash = AI::model_artifact_sha256(path);
    std::string error;
    TriangleMesh source; ObjInfo colors;
    REQUIRE(AI::load_model_artifact(path, source, colors, error));
    const auto describe = [&](const char* stage, const TriangleMesh& mesh) {
        const auto check = inspect_workbench_mesh(mesh);
        std::cout << stage << ": " << check.summary << " native_open=" << its_num_open_edges(mesh.its) << '\n';
        return check;
    };
    REQUIRE(workbench_topology_safe(describe("artifact", source)));
    DecodedWorkbenchModel decoded;
    REQUIRE(decode_workbench_model(path, decoded, error));
    REQUIRE(workbench_topology_safe(describe("decoded", decoded.mesh)));
    auto model = assemble_workbench_model(decoded, path, false);
    auto* volume = model->objects.front()->volumes.front();
    REQUIRE(workbench_topology_safe(describe("assembled", volume->mesh())));
    REQUIRE(model->texture_mesh);
    PaintedMesh painted;
    painted.vertices = model->texture_mesh->vertices;
    painted.indices = model->texture_mesh->indices;
    painted.cluster_colors = {{204,128,77}, {242,242,242}};
    for (size_t face = 0; face < painted.indices.size(); ++face)
        painted.face_colors.push_back(painted.cluster_colors[face % 2]);
    std::vector<FilamentMatch> matches(2);
    for (int i = 0; i < 2; ++i) { matches[i].cluster_index = i; matches[i].filament_index = i; }
    REQUIRE(apply_painted_mesh_to_volume(painted, matches, *volume, true));
    REQUIRE(workbench_topology_safe(describe("painted", volume->mesh())));
    const auto painting = volume->mmu_segmentation_facets.get_data();
    TriangleMesh final_mesh = volume->mesh();
    AI::ModelImportResult result;
    const bool safe = prepare_workbench_import_topology(final_mesh, false, result, error);
    describe("final", final_mesh);
    INFO(error);
    REQUIRE(safe);
    CHECK_FALSE(result.topology_risk);
    CHECK(its_num_open_edges(final_mesh.its) == 0);
    REQUIRE(LocalPrintColorApplication::same_surface_partition(source.its, final_mesh.its, false));
    volume->set_mesh(std::move(final_mesh));
    model->texture_mesh.reset();
    std::unique_ptr<Model> placed;
    REQUIRE(LocalPrintModelImport::prepare(*model->objects.front(), {100,100}, {300,300}, placed, error));
    auto* placed_volume = placed->objects.front()->volumes.front();
    REQUIRE(workbench_topology_safe(describe("placed", placed_volume->mesh())));
    CHECK(placed_volume->mmu_segmentation_facets.get_data() == painting);
    ScopedTemporaryDir directory("artifact-import-qa");
    ScopedTemporaryDir backup("artifact-import-backup"); placed->set_backup_path(backup.string());
    const std::string native_path = (boost::filesystem::path(directory.string()) / "imported.3mf").string();
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    PlateData plate; plate.plate_index = 0;
    StoreParams params; params.path = native_path.c_str(); params.model = placed.get(); params.config = &config;
    params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence; params.plate_data_list.push_back(&plate);
    REQUIRE(store_bbs_3mf(params));
    Model restored; ScopedTemporaryDir restored_backup("artifact-import-restored"); restored.set_backup_path(restored_backup.string());
    DynamicPrintConfig restored_config;
    ConfigSubstitutionContext substitutions {ForwardCompatibilitySubstitutionRule::Enable};
    PlateDataPtrs plates; std::vector<Preset*> presets; bool bbl = false, orca = false; Semver version;
    const bool loaded = load_bbs_3mf(native_path.c_str(), &restored_config, &substitutions, &restored, &plates,
        &presets, &bbl, &orca, &version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig);
    release_PlateData_list(plates); for (auto* preset : presets) delete preset;
    REQUIRE(loaded);
    REQUIRE(restored.objects.size() == 1);
    auto* reopened = restored.objects.front()->volumes.front();
    REQUIRE(workbench_topology_safe(describe("reopened", reopened->mesh())));
    CHECK(its_num_open_edges(reopened->mesh().its) == 0);
    CHECK(reopened->mesh().its.indices.size() == source.its.indices.size());
    CHECK(reopened->mmu_segmentation_facets.get_data() == painting);
    CHECK(AI::model_artifact_sha256(path) == original_hash);
}

TEST_CASE("Final color import stitches exact seams without changing faces or subface painting", "[WorkbenchTopology]")
{
    using namespace Slic3r;
    const auto source = its_make_cube(10, 10, 10);
    indexed_triangle_set seams;
    for (const auto& face : source.indices) {
        const int offset = int(seams.vertices.size());
        for (int corner : face) seams.vertices.push_back(source.vertices[corner]);
        seams.indices.emplace_back(offset, offset + 1, offset + 2);
    }
    Model model;
    auto* volume = model.add_object()->add_volume(TriangleMesh(seams));
    TriangleSelector selector(volume->mesh());
    for (size_t face = 0; face < seams.indices.size(); ++face)
        selector.set_facet(int(face), face % 2 ? EnforcerBlockerType::Extruder1 : EnforcerBlockerType::Extruder2);
    volume->mmu_segmentation_facets.set(selector);
    const auto painting = volume->mmu_segmentation_facets.get_data();
    const auto original = volume->mesh().its;
    TriangleMesh candidate = volume->mesh();
    AI::ModelImportResult result;
    std::string error;
    REQUIRE(prepare_workbench_import_topology(candidate, false, result, error));
    CHECK_FALSE(result.topology_risk);
    CHECK(its_num_open_edges(candidate.its) == 0);
    REQUIRE(LocalPrintColorApplication::same_surface_partition(original, candidate.its, false));
    volume->set_mesh(std::move(candidate));
    CHECK(volume->mmu_segmentation_facets.get_data() == painting);
}

TEST_CASE("A genuine hole cannot pass normal import and a confirmed exception stays risky", "[WorkbenchTopology]")
{
    using namespace Slic3r;
    auto its = its_make_cube(10, 10, 10);
    its.indices.pop_back();
    TriangleMesh mesh(its);
    AI::ModelImportResult result;
    std::string error;
    CHECK_FALSE(prepare_workbench_import_topology(mesh, false, result, error));
    CHECK(result.topology_risk);
    CHECK_FALSE(error.empty());
    CHECK(mesh.its.vertices == its.vertices);
    CHECK(mesh.its.indices == its.indices);
    error.clear();
    REQUIRE(prepare_workbench_import_topology(mesh, true, result, error));
    CHECK(result.topology_risk);
    CHECK(result.manual_repair_required);
    CHECK_FALSE(result.topology_risk_reason.empty());
}

TEST_CASE("Closed fans sharing a vertex remain unsafe while independent closed shells can import", "[WorkbenchTopology]")
{
    using namespace Slic3r;
    auto first = its_make_cube(10, 10, 10);
    auto second = its_make_cube(10, 10, 10);
    for (auto& vertex : second.vertices) vertex += Vec3f(10, 10, 10);
    its_merge(first, second);
    TriangleMesh touching(first);
    its_merge_vertices(touching.its);
    CHECK(its_num_open_edges(touching.its) == 0);
    const auto bad = inspect_workbench_mesh(touching);
    CHECK(bad.nonmanifold_vertices == 1);
    CHECK_FALSE(workbench_topology_safe(bad));
    for (auto& vertex : second.vertices) vertex += Vec3f(10, 10, 10);
    auto disjoint = its_make_cube(10, 10, 10); its_merge(disjoint, second);
    const auto good = inspect_workbench_mesh(TriangleMesh(disjoint));
    CHECK(good.isolated_components == 2);
    CHECK(workbench_topology_safe(good));
}

TEST_CASE("Canceled or unavailable final topology checks cannot authorize an import", "[WorkbenchTopology]")
{
    using namespace Slic3r;
    AI::ModelImportResult result;
    std::string error;
    TriangleMesh empty;
    CHECK_FALSE(prepare_workbench_import_topology(empty, true, result, error));
    TriangleMesh mesh(its_make_cube(10, 10, 10));
    CHECK_FALSE(prepare_workbench_import_topology(mesh, true, result, error, [] { return true; }));
}

TEST_CASE("confirmed risky imports reach the asynchronous host without claiming topology safety", "[WorkbenchImportEligibility]")
{
    PostGenerationWorkbenchState state;
    state.actions.can_import = true;
    state.model_path = "generated/portrait.glb";
    state.can_force_import = true;
    state.check.status = WorkbenchCheckStatus::Attention;
    state.check.force_import_risk = true;
    state.check.model_sha256 = std::string(64, 'a');
    state.force_import_acknowledged = true;
    Slic3r::AI::ModelImportRequest request;
    request.artifact.local_path = state.model_path;
    request.topology_check_sha256 = state.check.model_sha256;
    CHECK_FALSE(workbench_import_request_allowed(state, request));
    request.topology_risk_confirmed = true;
    CHECK(workbench_import_request_allowed(state, request));
    CHECK_FALSE(state.can_import_for_slicing);

    SECTION("a changed model or topology report invalidates confirmation") {
        request.artifact.local_path = "generated/other.glb";
        CHECK_FALSE(workbench_import_request_allowed(state, request));
        request.artifact.local_path = state.model_path;
        request.topology_check_sha256 = std::string(64, 'b');
        CHECK_FALSE(workbench_import_request_allowed(state, request));
    }
    SECTION("pending edits and repair processing remain blocked") {
        state.dirty = true;
        CHECK_FALSE(workbench_import_request_allowed(state, request));
        state.dirty = false;
        state.candidate_path = "generated/repair.obj";
        CHECK_FALSE(workbench_import_request_allowed(state, request));
        state.candidate_path.clear();
        state.check.candidate_status = RepairCandidateStatus::Ready;
        CHECK_FALSE(workbench_import_request_allowed(state, request));
        state.check.candidate_status = RepairCandidateStatus::Generating;
        CHECK_FALSE(workbench_import_request_allowed(state, request));
        state.check.candidate_status = RepairCandidateStatus::None;
        state.check.status = WorkbenchCheckStatus::Running;
        CHECK_FALSE(workbench_import_request_allowed(state, request));
    }
    SECTION("busy projects and invalid filament matching cannot use the exception") {
        state.actions.can_import = false;
        CHECK_FALSE(workbench_import_request_allowed(state, request));
        state.actions.can_import = true;
        state.can_force_import = false;
        CHECK_FALSE(workbench_import_request_allowed(state, request));
    }

    SECTION("only an acknowledged attention result can use the exception") {
        for (const auto status : {WorkbenchCheckStatus::NotRun, WorkbenchCheckStatus::Invalid,
                                  WorkbenchCheckStatus::Failed}) {
            state.check.status = status;
            CHECK_FALSE(workbench_import_request_allowed(state, request));
        }
        state.check.status = WorkbenchCheckStatus::Attention;
        state.force_import_acknowledged = false;
        CHECK_FALSE(workbench_import_request_allowed(state, request));
        state.force_import_acknowledged = true;
        state.check.force_import_risk = false;
        CHECK_FALSE(workbench_import_request_allowed(state, request));
    }
}

TEST_CASE("closed and explicitly unchecked imports use their own current evidence", "[WorkbenchImportEligibility]")
{
    PostGenerationWorkbenchState state;
    state.actions.can_import = true;
    state.model_path = "generated/closed.obj";
    state.check.status = WorkbenchCheckStatus::Normal;
    state.check.model_sha256 = std::string(64, 'c');
    state.can_import_for_slicing = true;
    Slic3r::AI::ModelImportRequest request;
    request.artifact.local_path = state.model_path;
    request.topology_check_sha256 = state.check.model_sha256;
    CHECK(workbench_import_request_allowed(state, request));

    state.can_import_for_slicing = false;
    state.can_force_import = true;
    state.check.status = WorkbenchCheckStatus::Attention;
    state.check.force_import_risk = true;
    state.check.model_sha256 = std::string(64, 'd');
    state.force_import_acknowledged = true;
    request.topology_check_sha256 = state.check.model_sha256;
    CHECK_FALSE(workbench_import_request_allowed(state, request));
    request.topology_risk_confirmed = true;
    CHECK(workbench_import_request_allowed(state, request));
    state.check.model_sha256 = std::string(64, 'e');
    CHECK_FALSE(workbench_import_request_allowed(state, request));
}

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
    CAPTURE(file);
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
    CHECK_THAT(av->source.mesh_offset.norm(),
        Catch::Matchers::WithinAbs(0., 1e-6));
    CHECK(av->source.input_file == ev->source.input_file);
    REQUIRE(bool(actual->texture_mesh) == bool(expected.texture_mesh));
    if (actual->texture_mesh) {
        const auto& at = *actual->texture_mesh;
        const auto& et = *expected.texture_mesh;
        if (path.extension() == ".obj") {
            // The legacy OBJ loader centers texture vertices. Workbench
            // painting retains raw source coordinates, matching GLB.
            REQUIRE(at.vertices.size() == et.vertices.size());
            for (size_t i = 0; i < at.vertices.size(); ++i)
                for (int axis = 0; axis < 3; ++axis)
                    CHECK_THAT(at.vertices[i][axis] - float(ev->source.mesh_offset[axis]),
                        Catch::Matchers::WithinAbs(et.vertices[i][axis], 1e-5f));
        } else CHECK(at.vertices == et.vertices);
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
        // The workbench retains the source frame; ordinary native imports
        // still center the mesh. Compare their world geometry, not the
        // different local coordinates produced by those two policies.
        const auto source_geometry = am;
        const Vec3d original_offset = av->get_offset();
        REQUIRE(apply_painted_mesh_to_volume(painted, {match}, *actual->objects.front()->volumes.front(), true));
        REQUIRE(apply_painted_mesh_to_volume(painted, {match}, *expected.objects.front()->volumes.front()));
        const auto& painted_actual = av->mesh().its;
        const auto& painted_expected = ev->mesh().its;
        REQUIRE(painted_actual.vertices.size() == painted_expected.vertices.size());
        REQUIRE(painted_actual.indices.size() == painted_expected.indices.size());
        CHECK_THAT((av->get_offset() - original_offset).norm(), Catch::Matchers::WithinAbs(0., 1e-6));
        // UV seams may duplicate texture vertices that the printable mesh
        // welded. Preserve ordered face corners independently of that aliasing.
        REQUIRE(painted_actual.indices.size() == source_geometry.indices.size());
        for (size_t face = 0; face < painted_actual.indices.size(); ++face)
            for (int corner = 0; corner < 3; ++corner)
                CHECK_THAT((painted_actual.vertices[painted_actual.indices[face][corner]] -
                    source_geometry.vertices[source_geometry.indices[face][corner]]).norm(),
                    Catch::Matchers::WithinAbs(0., 1e-6));
        for (size_t i = 0; i < painted_actual.vertices.size(); ++i) {
            CHECK_THAT((painted_actual.vertices[i].cast<double>() + av->get_offset() -
                painted_expected.vertices[i].cast<double>() - ev->get_offset()).norm(),
                Catch::Matchers::WithinAbs(0., 1e-4));
        }
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

TEST_CASE("native background slicing keeps the workbench running through a stale phase", "[SmartSlicing][UiRedesign]")
{
    using namespace Slic3r::AI::SmartSlicing;
    CHECK(workbench_slice_running(true, OfficialSlicePhase::Rejected));
    CHECK(workbench_slice_running(true, OfficialSlicePhase::Failed));
    CHECK(workbench_slice_running(false, OfficialSlicePhase::Slicing));
    CHECK_FALSE(workbench_slice_running(false, OfficialSlicePhase::Completed));
    CHECK_FALSE(workbench_slice_running(false, OfficialSlicePhase::Failed));
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

TEST_CASE("successful slicing remembers its goal without treating config changes as model changes",
          "[SmartSlicing][UiRedesign]")
{
    using namespace Slic3r::AI::SmartSlicing;
    WorkbenchSliceGoalHistory history;
    WorkspaceRevision revision{11, 21, 31, "project"};
    history.begin(RecommendationGoal::Quality);
    history.complete(true, revision);
    CHECK(history.last_sliced_goal(revision) == RecommendationGoal::Quality);

    auto config_changed = revision;
    ++config_changed.config_revision;
    CHECK(history.last_sliced_goal(config_changed) == RecommendationGoal::Quality);

    auto model_changed = revision;
    ++model_changed.model_revision;
    CHECK_FALSE(history.last_sliced_goal(model_changed));
    auto plate_changed = revision;
    ++plate_changed.plate_revision;
    CHECK_FALSE(history.last_sliced_goal(plate_changed));

    WorkbenchSliceGoalHistory guarded;
    guarded.begin(RecommendationGoal::Speed, revision);
    guarded.complete(true, model_changed);
    CHECK_FALSE(guarded.last_sliced_goal(model_changed));
}

TEST_CASE("failed slicing does not replace the last successful goal and current selection stays distinct",
          "[SmartSlicing][UiRedesign]")
{
    using namespace Slic3r::AI::SmartSlicing;
    WorkbenchSliceGoalHistory history;
    WorkspaceRevision revision{12, 22, 32, "project"};
    history.begin(RecommendationGoal::Balanced);
    history.complete(true, revision);
    history.begin(RecommendationGoal::Speed);
    history.complete(false, revision);
    CHECK(history.last_sliced_goal(revision) == RecommendationGoal::Balanced);

    SmartSlicingWorkbenchState state;
    state.has_selected_goal = true;
    state.selected_goal = RecommendationGoal::Speed;
    state.pending_slice_goal = RecommendationGoal::Quality;
    state.last_sliced_goal = RecommendationGoal::Balanced;
    CHECK(workbench_goal_is_currently_selected(state, RecommendationGoal::Speed));
    CHECK_FALSE(workbench_goal_is_currently_selected(state, RecommendationGoal::Balanced));
    CHECK(workbench_goal_was_last_sliced(state, RecommendationGoal::Balanced));
    CHECK_FALSE(workbench_goal_was_last_sliced(state, RecommendationGoal::Speed));
    CHECK(workbench_goal_is_pending(state, RecommendationGoal::Quality));
    CHECK_FALSE(workbench_goal_is_pending(state, RecommendationGoal::Balanced));
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

TEST_CASE("Printer trial actions cannot interrupt model palette publication",
          "[PostGenerationWorkbench][PrinterFilamentPreview]")
{
    CHECK(workbench_printer_trial_action_allowed(false, false, true, true));
    CHECK(workbench_printer_trial_action_allowed(true, false, false, false));
    CHECK_FALSE(workbench_printer_trial_action_allowed(false, false, true, false));
    CHECK_FALSE(workbench_printer_trial_action_allowed(false, false, false, true));
    CHECK_FALSE(workbench_printer_trial_action_allowed(false, true, true, true));
    CHECK_FALSE(workbench_printer_trial_action_allowed(true, true, true, true));
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

TEST_CASE("OBJ and GLB repairs use candidates while pending versions and unsaved edits stay blocked", "[WorkbenchTopology][PostGenerationWorkbench]")
{
    CHECK(workbench_safe_repair_allowed("obj", false, false, false));
    CHECK(workbench_safe_repair_allowed("glb", false, false, false));
    CHECK(workbench_safe_repair_allowed("obj", true, false, false));
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

TEST_CASE("post-generation status tones preserve semantic meaning during original comparison",
          "[PostGenerationWorkbench][UiRedesign]")
{
    using Status = PostGenerationUiState::Status;
    CHECK(post_generation_status_tone(Status::Empty) == PostGenerationStatusTone::Neutral);
    CHECK(post_generation_status_tone(Status::Loading) == PostGenerationStatusTone::Accent);
    CHECK(post_generation_status_tone(Status::Processing) == PostGenerationStatusTone::Accent);
    CHECK(post_generation_status_tone(Status::CandidateReady) == PostGenerationStatusTone::Accent);
    CHECK(post_generation_status_tone(Status::Ready) == PostGenerationStatusTone::Success);
    CHECK(post_generation_status_tone(Status::Editing) == PostGenerationStatusTone::Information);
    CHECK(post_generation_status_tone(Status::ComparingBefore) == PostGenerationStatusTone::Information);
    CHECK(post_generation_status_tone(Status::Error) == PostGenerationStatusTone::Error);
    CHECK(post_generation_status_tone(Status::Ready, true) == PostGenerationStatusTone::Information);
    CHECK(post_generation_status_tone(Status::Loading, true) == PostGenerationStatusTone::Accent);
    CHECK(post_generation_status_tone(Status::Error, true) == PostGenerationStatusTone::Error);
}

TEST_CASE("original comparison requires an imported material appearance",
          "[PostGenerationWorkbench][UiRedesign][TextureImport]")
{
    Slic3r::AI::ModelArtifactTextureSurface surface;
    CHECK_FALSE(preview_has_source_appearance(false, false, surface));
    CHECK(preview_has_source_appearance(true, false, surface));
    CHECK(preview_has_source_appearance(false, true, surface));
    surface.faces.emplace_back();
    CHECK(preview_has_source_appearance(false, false, surface));
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

TEST_CASE("printer filament readiness validates loaded spools and permits idle nozzles", "[PrinterFilaments][PostGenerationWorkbench]")
{
    using namespace Slic3r;
    const size_t nozzles=GENERATE(1u,4u,6u);
    auto bundle=printer_filament_fixture(nozzles,nozzles);
    auto state=PrinterFilamentConfig::capture(bundle);
    REQUIRE(state.ready); CHECK(state.nozzles==nozzles); CHECK(state.physical_count==nozzles);
    for(const auto& slot:state.slots) CHECK(slot.removable == (nozzles > 1));
    auto draft=bundle;
    draft.project_config.option<ConfigOptionStrings>("filament_colour")->values.back().clear();
    CHECK_FALSE(PrinterFilamentConfig::capture(draft).ready);
    CHECK(bundle.project_config!=draft.project_config);
    CHECK(PrinterFilamentConfig::capture(bundle).ready);
    draft.project_config.option<ConfigOptionBools>("filament_is_mixed")->values.back()=true;
    state=PrinterFilamentConfig::capture(draft);
    CHECK(state.physical_count==nozzles-1); CHECK(state.slots.size()==nozzles-1); CHECK(state.ready == (nozzles > 1));
}

TEST_CASE("multiple nozzle spools can be removed safely while at least one remains", "[PrinterFilaments][PostGenerationWorkbench]")
{
    using namespace Slic3r;
    const size_t nozzles = GENERATE(4u, 6u);
    auto bundle = printer_filament_fixture(nozzles, nozzles + 1);
    bundle.printers.get_edited_preset().config.option<ConfigOptionBool>("single_extruder_multi_material")->value = true;
    REQUIRE(bundle.can_remove_physical_filament(0));
    const auto state = PrinterFilamentConfig::capture(bundle);
    for (size_t i = 0; i < nozzles; ++i) CHECK(state.slots[i].removable);
    CHECK(state.slots.back().removable);
    std::string error;
    auto original = bundle;
    CHECK(PrinterFilamentConfig::remove(original, 0, nozzles, error));
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

TEST_CASE("printer trial keeps slot bindings through color and preset edits", "[PrinterFilaments][PrinterFilamentPreview][PostGenerationWorkbench]")
{
    using Color = PreviewPalette::Color;
    auto bundle = printer_filament_fixture(1, 2);
    const auto initial = PrinterFilamentConfig::capture(bundle);
    REQUIRE(initial.ready);

    Slic3r::AI::ColorTrialPersistence::State trial;
    trial.colors = {{1, 0, 0}, {0, 0, 1}};
    trial.mapping_colors = trial.colors;
    trial.count = 2;
    trial.source = 2;
    const Color original {.0705882f, .203922f, .337255f};
    REQUIRE(PrinterFilamentPreview::apply_bound(trial, {0, 1}, {original, original}, initial.identity));

    bundle.project_config.option<Slic3r::ConfigOptionStrings>("filament_colour")->values[0] = "#00FFFF";
    const auto recolored = PrinterFilamentConfig::capture(bundle);
    CHECK(recolored.layout == initial.layout);
    CHECK(recolored.identity != initial.identity);
    CHECK(recolored.signature != initial.signature);
    const Color cyan {0, 1, 1};
    REQUIRE(Slic3r::AI::ColorTrialPersistence::synchronize_project_targets(
        trial, {0, 1}, {cyan, original}, recolored.identity));
    CHECK(trial.project_color_slots == std::vector<size_t>{0, 1});
    CHECK(trial.colors[0] == cyan);

    auto alternate = bundle.filaments.find_preset("Loaded PLA", false)->config;
    alternate.option<Slic3r::ConfigOptionInts>("nozzle_temperature")->values = {215};
    bundle.filaments.load_preset("", "Loaded PLA Alternate", alternate, false);
    bundle.filament_presets[1] = "Loaded PLA Alternate";
    const auto preset_changed = PrinterFilamentConfig::capture(bundle);
    REQUIRE(preset_changed.ready);
    CHECK(preset_changed.layout == recolored.layout);
    CHECK(preset_changed.identity != recolored.identity);
    REQUIRE(Slic3r::AI::ColorTrialPersistence::synchronize_project_targets(
        trial, {0, 1}, {cyan, original}, preset_changed.identity));
    CHECK(trial.project_color_slots == std::vector<size_t>{0, 1});

    bundle.set_num_filaments(3, "#ABCDEF");
    bundle.filament_presets[2] = "Loaded PLA";
    const auto expanded = PrinterFilamentConfig::capture(bundle);
    REQUIRE(expanded.ready);
    CHECK(expanded.layout != preset_changed.layout);
    const Color appended {.670588f, .803922f, .937255f};
    REQUIRE(Slic3r::AI::ColorTrialPersistence::synchronize_project_targets(
        trial, {0, 1, 2}, {cyan, original, appended}, expanded.identity));
    CHECK(trial.project_color_slots == std::vector<size_t>{0, 1});

    bundle.update_num_filaments(1);
    const auto reduced = PrinterFilamentConfig::capture(bundle);
    CHECK_FALSE(Slic3r::AI::ColorTrialPersistence::synchronize_project_targets(
        trial, {0}, {cyan}, reduced.identity));
}

TEST_CASE("draft filament additions preserve live configuration and cannot exceed fixed nozzle capacity", "[PrinterFilaments][PostGenerationWorkbench]")
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
    CHECK(PrinterFilamentConfig::capture(draft).ready == (nozzles == 1));
    std::string error;
    auto alternative = draft;
    CHECK(PrinterFilamentConfig::remove(alternative,0,inserted,error));
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

TEST_CASE("mixed filament recipes can be removed from the suffix with a native slot remap", "[PrinterFilaments][PostGenerationWorkbench]")
{
    using namespace Slic3r;
    auto live = printer_filament_fixture(4, 4);
    live.set_num_filaments(5, "#8A6A5A");
    live.filament_presets[4] = live.filament_presets[0];
    live.project_config.option<ConfigOptionBools>("filament_is_mixed")->values[4] = true;
    live.project_config.option<ConfigOptionStrings>("filament_mixed_components")->values[4] = "1,2";
    live.project_config.option<ConfigOptionStrings>("filament_mixed_sublayer_ratios")->values[4] = "0.5,0.5";
    const auto before = live;
    auto staged = live;
    std::string error;
    REQUIRE(PrinterFilamentConfig::remove_mixed(staged, 4, 0, error));
    CHECK(staged.filament_presets.size() == 4);
    CHECK(PrinterFilamentConfig::capture(staged).mixed.empty());
    CHECK(live.filament_presets.size() == before.filament_presets.size());
    CHECK(live.project_config == before.project_config);
    staged = live;
    CHECK_FALSE(PrinterFilamentConfig::remove_mixed(staged, 3, 0, error));
    CHECK(staged.filament_presets.size() == live.filament_presets.size());
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

TEST_CASE("Exact printer coverage normalizes RGB8 and preserves duplicate slot identity",
          "[PrinterFilamentPreview][PrinterFilamentImport][PostGenerationWorkbench]")
{
    using Color = PreviewPalette::Color;
    const auto red_key = PrinterFilamentPreview::rgb8("#aA0011");
    const auto blue_key = PrinterFilamentPreview::rgb8("#0000fF");
    REQUIRE(red_key);
    REQUIRE(blue_key);
    CHECK(PrinterFilamentPreview::rgb8_hex(*red_key) == "#AA0011");
    CHECK(PrinterFilamentPreview::rgb8(PrinterFilamentPreview::rgb8_color(*red_key)) == *red_key);

    const Color red = PrinterFilamentPreview::rgb8_color(*red_key);
    const Color blue = PrinterFilamentPreview::rgb8_color(*blue_key);
    const Color missing = PrinterFilamentPreview::rgb8_color(0x010203);
    const std::vector<PrinterFilamentPreview::NativeColorSlot> candidates {
        {2, red, false}, {7, red, false}, {11, blue, true}
    };
    const auto covered = PrinterFilamentPreview::bind_exact_colors(
        {red, blue}, candidates, {std::optional<size_t>{7}, {}});
    REQUIRE(covered.complete(2));
    CHECK(covered.slots == std::vector<size_t>{7, 11});

    const auto binding = PrinterFilamentPreview::bind_exact_colors(
        {red, red, blue, missing, missing}, candidates,
        {std::optional<size_t>{7}, std::optional<size_t>{99}, std::optional<size_t>{11}, {}, {}});

    CHECK_FALSE(binding.complete(5));
    REQUIRE(binding.slots.size() == 5);
    CHECK(binding.slots[0] == 7);
    CHECK(binding.slots[1] == 2);
    CHECK(binding.slots[2] == 11);
    CHECK(binding.slots[3] == size_t(-1));
    CHECK(binding.slots[4] == size_t(-1));
    CHECK(binding.missing_colors == std::vector<std::string>{"#010203"});
}

TEST_CASE("Printer color basis retains every native logical slot without inventing model regions",
          "[PrinterFilamentPreview][ModelPreviewPalette][PostGenerationWorkbench]")
{
    using Color = PreviewPalette::Color;
    for (const size_t count : {size_t(1), size_t(4), size_t(9), size_t(32)}) {
        INFO("native slot count = " << count);
        PreviewPalette::AutomaticColorBasis basis;
        basis.geometry_id = "geometry";
        basis.source_color_signature = "source";
        basis.requested_count = count;
        basis.source_palette = count == 1
            ? std::vector<Color>{{1.f, 0.f, 0.f}}
            : std::vector<Color>{{1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}};
        basis.source_area.assign(basis.source_palette.size(), 1.0);
        basis.basis_signature = PreviewPalette::automatic_basis_signature(basis);
        REQUIRE(basis.valid());

        std::vector<Color> native_colors;
        std::vector<size_t> native_slots;
        native_colors.reserve(count);
        native_slots.reserve(count);
        for (size_t index = 0; index < count; ++index) {
            const float value = count == 1 ? 0.f : float(index) / float(count - 1);
            native_colors.push_back({value, 1.f - value, float((index * 37) % count) / float(count)});
            native_slots.push_back(index);
        }

        Slic3r::AI::ColorTrialPersistence::State state;
        state.colors = basis.source_palette;
        state.mapping_colors = basis.source_palette;
        state.count = int(basis.source_palette.size());
        state.source = 2;
        REQUIRE(PrinterFilamentPreview::apply_automatic_basis(
            state, basis, native_slots, native_colors, "printer-layout"));
        CHECK(state.source == 1);
        CHECK(state.enabled);
        CHECK(state.count == int(count));
        CHECK(state.project_count == int(count));
        CHECK(state.colors.size() == basis.source_palette.size());
        CHECK(state.mapping_colors == basis.source_palette);
        CHECK(state.project_color_slots.size() == basis.source_palette.size());
        CHECK(state.semantic_palette == state.colors);
        CHECK(state.semantic_mapping_palette == state.mapping_colors);
        CHECK(state.project_semantic_slots == state.project_color_slots);
        CHECK(state.color_card_target_palette.size() == count);
        CHECK(state.color_card_target_slot_ids == native_slots);
        CHECK(Slic3r::AI::ColorTrialPersistence::detail::valid_state(state));

        auto changed_colors = native_colors;
        changed_colors.front() = {0.25f, 0.5f, 0.75f};
        REQUIRE(Slic3r::AI::ColorTrialPersistence::synchronize_project_targets(
            state, native_slots, changed_colors, "printer-layout"));
        CHECK(state.enabled);
        CHECK(state.color_card_target_palette.front() == changed_colors.front());
        CHECK(Slic3r::AI::ColorTrialPersistence::detail::valid_state(state));
    }
}

TEST_CASE("Printer mappings bind implicit semantic colors to the same native slots",
          "[PrinterFilamentPreview][PrinterFilamentImport][PostGenerationWorkbench]")
{
    using Color = PreviewPalette::Color;
    const std::vector<Color> source {{0.9f, 0.1f, 0.1f}, {0.1f, 0.1f, 0.9f}};
    const std::vector<Color> targets {{0.8f, 0.2f, 0.2f}, {0.2f, 0.2f, 0.8f}};
    const std::vector<size_t> slots {3, 7};

    for (const bool explicit_binding : {false, true}) {
        INFO("explicit binding = " << explicit_binding);
        Slic3r::AI::ColorTrialPersistence::State state;
        state.colors = source;
        state.mapping_colors = source;
        state.count = 2;
        state.source = 2;
        const bool applied = explicit_binding
            ? PrinterFilamentPreview::apply_bound(state, slots, targets, "printer-layout")
            : PrinterFilamentPreview::apply(state, slots, targets, "printer-layout");
        REQUIRE(applied);
        CHECK(state.semantic_palette == state.colors);
        CHECK(state.semantic_mapping_palette == state.mapping_colors);
        CHECK(state.project_semantic_slots == state.project_color_slots);
        CHECK(Slic3r::AI::ColorTrialPersistence::detail::valid_state(state));
    }
}

TEST_CASE("Legacy printer previews repair missing implicit semantic slot bindings",
          "[PrinterFilamentPreview][ModelPreviewState][PostGenerationWorkbench]")
{
    Slic3r::AI::ColorTrialPersistence::State state;
    state.source = 1;
    state.enabled = true;
    state.count = 2;
    state.colors = {{0.8f, 0.1f, 0.1f}, {0.1f, 0.1f, 0.8f}};
    state.mapping_colors = {{0.7f, 0.2f, 0.2f}, {0.2f, 0.2f, 0.7f}};
    state.semantic_palette = state.colors;
    state.semantic_mapping_palette = state.mapping_colors;
    state.project_slot_identity = "printer-layout";
    state.project_color_slots = {2, 6};
    REQUIRE_FALSE(Slic3r::AI::ColorTrialPersistence::detail::valid_state(state));

    PrinterFilamentPreview::bind_implicit_semantic_palette(state);

    CHECK(state.project_semantic_slots == state.project_color_slots);
    CHECK(Slic3r::AI::ColorTrialPersistence::detail::valid_state(state));
}

TEST_CASE("Baked printer candidates keep slot identity without applying their mapping twice",
          "[PrinterFilamentPreview][ModelPreviewPalette][PostGenerationWorkbench]")
{
    using Color = PreviewPalette::Color;
    PreviewPalette::AutomaticColorBasis basis;
    basis.geometry_id = "geometry";
    basis.source_color_signature = "source";
    basis.requested_count = 9;
    basis.source_palette = {{0.15f, 0.20f, 0.25f}, {0.70f, 0.55f, 0.40f},
                            {0.85f, 0.75f, 0.65f}, {0.10f, 0.05f, 0.02f}};
    basis.source_area.assign(basis.source_palette.size(), 1.0);
    basis.basis_signature = PreviewPalette::automatic_basis_signature(basis);
    REQUIRE(basis.valid());

    std::vector<Color> native_colors;
    std::vector<size_t> native_slots;
    for (size_t index = 0; index < 9; ++index) {
        native_colors.push_back({float(index) / 9.f, float(8 - index) / 9.f,
                                 float((index * 5) % 9) / 9.f});
        native_slots.push_back(index);
    }
    ModelPreviewColorState state;
    state.colors = basis.source_palette;
    state.mapping_colors = basis.source_palette;
    state.count = int(basis.source_palette.size());
    state.source = 2;
    REQUIRE(PrinterFilamentPreview::apply_automatic_basis(
        state, basis, native_slots, native_colors, "printer-layout"));
    REQUIRE(state.mapping_colors != state.colors);

    const auto baked = PrinterFilamentPreview::normalize_baked_candidate_state(state);
    STATIC_REQUIRE((std::is_same_v<std::decay_t<decltype(baked)>, ModelPreviewColorState>));
    CHECK(baked.mapping_colors == baked.colors);
    CHECK(baked.project_color_slots == state.project_color_slots);
    CHECK(baked.project_slot_identity == state.project_slot_identity);
    CHECK(baked.color_card_target_palette == state.color_card_target_palette);
    CHECK(baked.color_card_target_slot_ids == native_slots);
    CHECK(baked.count == 9);
    CHECK(Slic3r::AI::ColorTrialPersistence::detail::valid_state(baked));

    auto ordinary = state;
    ordinary.source = 2;
    ordinary.project_color_slots.clear();
    ordinary.project_slot_identity.clear();
    ordinary.color_card_target_palette.clear();
    ordinary.color_card_target_slot_ids.clear();
    ordinary.source_to_target_slot.clear();
    ordinary.color_card_weighted_error = 0.0;
    CHECK(PrinterFilamentPreview::normalize_baked_candidate_state(ordinary).mapping_colors ==
        ordinary.mapping_colors);
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

TEST_CASE("short native filament tables leave nozzles idle and reject incompatible or oversized configurations", "[PrinterFilaments][PostGenerationWorkbench]")
{
    using namespace Slic3r;
    const size_t nozzles=GENERATE(4u,6u);
    auto bundle=printer_filament_fixture(nozzles,nozzles);
    bundle.filament_presets.resize(1);
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values.resize(1);
    auto state=PrinterFilamentConfig::capture(bundle);
    CHECK(state.ready); CHECK(state.physical_count==1); REQUIRE(state.slots.size()==1);
    CHECK(state.idle_nozzles == nozzles - 1);
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

TEST_CASE("Honor young portrait suite is a frozen six physical color catalog entry",
          "[HonorFilamentSuite][PrinterFilaments][ModelPreviewPalette][PostGenerationWorkbench]")
{
    const auto& suite = young_portrait_honor_suite();
    REQUIRE(suite.valid());
    CHECK(suite.id == "honor.people.young-six");
    CHECK(suite.revision == "2");
    CHECK(suite.category == u8"人物");
    CHECK(suite.name == u8"年轻向六色");
    REQUIRE(suite.physical_slots.size() == 6);
    CHECK(suite.color_pack().colors == std::vector<std::string>{
        "#ECC3B2", "#282629", "#F6F7F9", "#EA9A92", "#668CB6", "#958B86"});
    CHECK(suite.color_pack().colors == young_portrait_color_pack().colors);
    CHECK(suite.semantic_role_slots == std::map<std::string, std::string>{
        {"Skin", "skin-light"}, {"Hair", "charcoal"}, {"Iris", "charcoal"},
        {"Eyebrow", "charcoal"}, {"EyeSclera", "milk-white"},
        {"Teeth", "milk-white"}, {"Lips", "peach"}, {"MouthInterior", "peach"}});
    CHECK(suite.semantic_role_slot_indices() == std::map<std::string, size_t>{
        {"Skin", 0}, {"Hair", 1}, {"Iris", 1}, {"Eyebrow", 1},
        {"EyeSclera", 2}, {"Teeth", 2}, {"Lips", 3}, {"MouthInterior", 3}});
    CHECK(find_honor_filament_suite(suite.id, "1") == nullptr);
    const auto* catalog_suite = find_honor_filament_suite(suite.id, suite.revision);
    REQUIRE(catalog_suite);
    CHECK(catalog_suite->physical_slots.size() == suite.physical_slots.size());
    CHECK(catalog_suite->semantic_role_slots == suite.semantic_role_slots);
    std::set<std::string> identities;
    for (const auto& slot : suite.physical_slots) identities.insert(slot.id);
    CHECK(identities.size() == 6);
}

TEST_CASE("Honor suite stages six real slots without creating mixed recipes",
          "[PrinterFilamentImport][PrinterFilaments][PostGenerationWorkbench]")
{
    auto live = printer_filament_fixture(6, 4);
    const auto config_before = live.project_config;
    const auto presets_before = live.filament_presets;
    PrinterFilamentConfig::StagedColorPackImport staged;
    std::string error;

    REQUIRE(PrinterFilamentConfig::stage_honor_physical_suite_import(
        live, young_portrait_honor_suite(), staged, error));
    REQUIRE(staged.configuration);
    REQUIRE(staged.bindings.size() == 6);
    CHECK(std::set<size_t>(staged.bindings.begin(), staged.bindings.end()).size() == 6);
    CHECK(staged.physical_count == 6);
    CHECK(staged.mixed_count == 0);
    const auto imported = PrinterFilamentConfig::capture(*staged.configuration);
    REQUIRE(imported.ready);
    CHECK(imported.physical_count == 6);
    CHECK(imported.mixed.empty());
    for (size_t index = 0; index < staged.bindings.size(); ++index) {
        const size_t slot = staged.bindings[index];
        CHECK_FALSE(staged.configuration->is_mixed_filament(slot));
        CHECK(staged.configuration->project_config.opt_string("filament_colour", unsigned(slot)) ==
            young_portrait_honor_suite().physical_slots[index].color);
    }
    CHECK(live.project_config == config_before);
    CHECK(live.filament_presets == presets_before);
}

TEST_CASE("Honor suite preserves extra physical slots and independent duplicate identities",
          "[PrinterFilamentImport][PrinterFilaments][PostGenerationWorkbench]")
{
    auto live = printer_filament_fixture(8, 7);
    auto suite = young_portrait_honor_suite();
    suite.physical_slots[5].color = suite.physical_slots[4].color;
    const auto seventh_color = live.project_config.opt_string("filament_colour", unsigned(6));
    PrinterFilamentConfig::StagedColorPackImport staged;
    std::string error;

    REQUIRE(PrinterFilamentConfig::stage_honor_physical_suite_import(live, suite, staged, error));
    REQUIRE(staged.configuration);
    CHECK(staged.configuration->filament_presets.size() == live.filament_presets.size());
    CHECK(staged.configuration->project_config.opt_string("filament_colour", unsigned(6)) == seventh_color);
    REQUIRE(staged.bindings.size() == 6);
    CHECK(staged.bindings[4] != staged.bindings[5]);
    CHECK(staged.mixed_count == 0);
}

TEST_CASE("Honor suite keeps existing mixed recipes outside its six physical bindings",
          "[PrinterFilamentImport][PrinterFilaments][PrinterFilamentRecipes][PostGenerationWorkbench]")
{
    auto live = printer_filament_fixture(6, 5);
    live.project_config.option<Slic3r::ConfigOptionBools>("filament_is_mixed")->values[4] = true;
    live.project_config.option<Slic3r::ConfigOptionStrings>("filament_mixed_components")->values[4] = "1,2";
    live.project_config.option<Slic3r::ConfigOptionStrings>("filament_mixed_sublayer_ratios")->values[4] = "0.5,0.5";
    const auto config_before = live.project_config;
    const auto presets_before = live.filament_presets;
    PrinterFilamentConfig::StagedColorPackImport staged;
    std::string error;

    REQUIRE(PrinterFilamentConfig::stage_honor_physical_suite_import(
        live, young_portrait_honor_suite(), staged, error));
    REQUIRE(staged.configuration);
    const auto imported = PrinterFilamentConfig::capture(*staged.configuration);
    REQUIRE(imported.ready);
    CHECK(imported.physical_count == 6);
    REQUIRE(imported.mixed.size() == 1);
    CHECK(imported.mixed.front().slot == 6);
    CHECK(imported.mixed.front().valid);
    CHECK(staged.configuration->project_config.option<Slic3r::ConfigOptionStrings>(
        "filament_mixed_components")->values[6] == "1,2");
    CHECK(staged.bindings == std::vector<size_t>{0, 1, 2, 3, 4, 5});
    CHECK(staged.mixed_count == 0);
    CHECK(live.project_config == config_before);
    CHECK(live.filament_presets == presets_before);
}

TEST_CASE("Four-head and single-nozzle printers cannot apply a six-physical Honor suite",
          "[PrinterFilamentImport][PrinterFilaments][PostGenerationWorkbench]")
{
    const auto& suite = young_portrait_honor_suite();
    auto four = printer_filament_fixture(4, 4);
    const auto four_before = four.project_config;
    const auto four_presets = four.filament_presets;
    auto compatibility = PrinterFilamentConfig::honor_suite_compatibility(four, suite);
    CHECK_FALSE(compatibility.compatible);
    CHECK(compatibility.reason == u8"当前打印机为 4 头，此套装需要 6 个实体耗材");
    PrinterFilamentConfig::StagedColorPackImport staged;
    std::string error;
    CHECK_FALSE(PrinterFilamentConfig::stage_honor_physical_suite_import(four, suite, staged, error));
    CHECK_FALSE(staged.configuration);
    CHECK(four.project_config == four_before);
    CHECK(four.filament_presets == four_presets);

    auto semm = printer_filament_fixture(6, 4);
    semm.printers.get_edited_preset().config.option<Slic3r::ConfigOptionBool>(
        "single_extruder_multi_material")->value = true;
    compatibility = PrinterFilamentConfig::honor_suite_compatibility(semm, suite);
    CHECK_FALSE(compatibility.compatible);
    CHECK(compatibility.reason.find(u8"单喷嘴多材料") != std::string::npos);
}

TEST_CASE("Honor suite staging rejects incompatible material without touching the live project",
          "[PrinterFilamentImport][PrinterFilaments][PostGenerationWorkbench]")
{
    auto live = printer_filament_fixture(6, 4);
    live.filaments.find_preset("Loaded PLA", false)->is_compatible = false;
    const auto config_before = live.project_config;
    const auto presets_before = live.filament_presets;
    PrinterFilamentConfig::StagedColorPackImport staged;
    std::string error;
    CHECK_FALSE(PrinterFilamentConfig::stage_honor_physical_suite_import(
        live, young_portrait_honor_suite(), staged, error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(staged.configuration);
    CHECK(live.project_config == config_before);
    CHECK(live.filament_presets == presets_before);

    live = printer_filament_fixture(6, 6);
    auto petg = live.filaments.find_preset("Loaded PLA", false)->config;
    petg.option<Slic3r::ConfigOptionStrings>("filament_type")->values = {"PETG"};
    live.filaments.load_preset("", "Loaded PETG", petg, false);
    live.filament_presets.assign(6, "Loaded PETG");
    REQUIRE(PrinterFilamentConfig::capture(live).ready);
    const auto petg_config_before = live.project_config;
    const auto petg_presets_before = live.filament_presets;
    CHECK_FALSE(PrinterFilamentConfig::stage_honor_physical_suite_import(
        live, young_portrait_honor_suite(), staged, error));
    CHECK(error.find("PLA") != std::string::npos);
    CHECK_FALSE(staged.configuration);
    CHECK(live.project_config == petg_config_before);
    CHECK(live.filament_presets == petg_presets_before);
}

TEST_CASE("workbench cards accept up to thirty two colors without widening legacy saved cards",
          "[PrinterFilamentImport][PostGenerationWorkbench]")
{
    const size_t count = GENERATE(1u, 6u, 7u, 9u, 12u, 32u);
    const auto pack = filament_import_card(count);
    INFO(count);
    CHECK(pack.valid_for_workbench_import());
    CHECK(pack.valid() == (count <= 6));

    const auto oversized = filament_import_card(33);
    CHECK_FALSE(oversized.valid_for_workbench_import());
}

TEST_CASE("model color import recovers the pre-printer palette without importing printer state",
          "[PrinterFilamentImport][PrinterFilamentPreview][PostGenerationWorkbench]")
{
    Slic3r::AI::ColorTrialPersistence::State state;
    state.source = 0;
    state.count = 3;
    state.colors = {{{1.f, 0.f, 0.f}}, {{0.f, 1.f, 0.f}}, {{1.f, 0.f, 0.f}}};
    state.mapping_colors = state.colors;

    const auto card = PrinterFilamentPreview::model_color_pack(state);
    REQUIRE(card);
    CHECK(card->colors == std::vector<std::string>{"#FF0000", "#00FF00", "#FF0000"});
    CHECK(card->labels.size() == 3);

    state.source = 1;
    CHECK_FALSE(PrinterFilamentPreview::model_color_pack(state));

    state.source = 2;
    state.mapping_colors.pop_back();
    CHECK_FALSE(PrinterFilamentPreview::model_color_pack(state));

    state.mapping_colors = state.colors;
    state.colors.front()[0] = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(PrinterFilamentPreview::model_color_pack(state));
}

TEST_CASE("direct workbench card import creates one unique native identity per color",
          "[PrinterFilamentImport][PrinterFilamentRecipes][PostGenerationWorkbench]")
{
    const size_t count = GENERATE(6u, 7u, 9u, 12u, 32u);
    auto live = printer_filament_fixture(4, 4);
    const auto project_before = live.project_config;
    const auto presets_before = live.filament_presets;
    const auto pack = filament_import_card(count);
    PrinterFilamentConfig::StagedColorPackImport staged;
    std::string error;

    INFO(count);
    const bool staged_ok = PrinterFilamentConfig::stage_workbench_color_pack_import(live, pack, staged, error);
    INFO(error);
    REQUIRE(staged_ok);
    REQUIRE(staged.configuration);
    const auto imported = PrinterFilamentConfig::capture(*staged.configuration);
    REQUIRE(imported.ready);
    CHECK(staged.physical_count == 4);
    CHECK(staged.mixed_count == count - 4);
    CHECK(imported.physical_count == 4);
    CHECK(imported.mixed.size() == count - 4);
    REQUIRE(staged.bindings.size() == count);
    CHECK(std::set<size_t>(staged.bindings.begin(), staged.bindings.end()).size() == count);
    for (const size_t slot : staged.bindings) CHECK(slot < count);
    CHECK(PrinterFilamentSlotRemap::validate(staged.changes, 4, count));

    // The extended card deliberately repeats #123456. Equal RGB entries must
    // still retain separate native identities rather than collapsing together.
    if (count >= 9) {
        CHECK(pack.colors[6] == pack.colors[7]);
        CHECK(pack.colors[7] == pack.colors[8]);
        CHECK(staged.bindings[6] != staged.bindings[7]);
        CHECK(staged.bindings[7] != staged.bindings[8]);
        CHECK(staged.bindings[6] != staged.bindings[8]);
    }
    CHECK(live.project_config == project_before);
    CHECK(live.filament_presets == presets_before);
}

TEST_CASE("direct workbench card staging supports repeated expansion and compression",
          "[PrinterFilamentImport][PrinterFilamentRecipes][PostGenerationWorkbench]")
{
    auto live = printer_filament_fixture(4, 4);
    for (const size_t count : {9u, 6u, 9u, 4u}) {
        INFO("stage " << count << " colors");
        PrinterFilamentConfig::StagedColorPackImport staged;
        std::string error;
        const bool staged_ok = PrinterFilamentConfig::stage_workbench_color_pack_import(
            live, filament_import_card(count), staged, error);
        INFO(error);
        REQUIRE(staged_ok);
        REQUIRE(staged.configuration);
        const auto imported = PrinterFilamentConfig::capture(*staged.configuration);
        REQUIRE(imported.ready);
        CHECK(imported.physical_count == std::min(size_t(4), count));
        CHECK(imported.mixed.size() == count - std::min(size_t(4), count));
        CHECK(staged.bindings.size() == count);
        CHECK(std::set<size_t>(staged.bindings.begin(), staged.bindings.end()).size() == count);
        live = *staged.configuration;
    }
}

TEST_CASE("single nozzle direct import keeps four configured spools and creates five mixtures",
          "[PrinterFilamentImport][PrinterFilamentRecipes][PostGenerationWorkbench]")
{
    auto live = printer_filament_fixture(1, 4);
    PrinterFilamentConfig::StagedColorPackImport staged;
    std::string error;
    const bool staged_ok = PrinterFilamentConfig::stage_workbench_color_pack_import(
        live, filament_import_card(9), staged, error);
    INFO(error);
    REQUIRE(staged_ok);
    REQUIRE(staged.configuration);
    const auto imported = PrinterFilamentConfig::capture(*staged.configuration);
    REQUIRE(imported.ready);
    CHECK(staged.physical_count == 4);
    CHECK(staged.mixed_count == 5);
    CHECK(imported.physical_count == 4);
    CHECK(imported.mixed.size() == 5);
    CHECK(staged.bindings.size() == 9);
    CHECK(std::set<size_t>(staged.bindings.begin(), staged.bindings.end()).size() == 9);
}

TEST_CASE("automatic slot replacement uses nearest Lab color and stable native slot ties",
          "[PrinterFilamentImport][PostGenerationWorkbench]")
{
    auto target = [](size_t slot, const std::string& color) {
        WorkbenchFilamentSlot item;
        item.slot = slot;
        item.color = color;
        item.exists = item.configured = item.compatible = true;
        return item;
    };
    auto nearest = PrinterFilamentConfig::nearest_replacement_slot("#101010",
        {target(5, "#FFFFFF"), target(3, "#000000")});
    REQUIRE(nearest);
    CHECK(*nearest == 3);

    nearest = PrinterFilamentConfig::nearest_replacement_slot("#808080",
        {target(7, "#808080"), target(2, "#808080")});
    REQUIRE(nearest);
    CHECK(*nearest == 2);
}

TEST_CASE("failed direct workbench staging leaves the live preset bundle unchanged",
          "[PrinterFilamentImport][PrinterFilamentRecipes][PostGenerationWorkbench]")
{
    auto live = printer_filament_fixture(4, 4);
    live.filaments.find_preset("Loaded PLA", false)->is_compatible = false;
    const auto project_before = live.project_config;
    const auto presets_before = live.filament_presets;
    PrinterFilamentConfig::StagedColorPackImport staged;
    std::string error;

    CHECK_FALSE(PrinterFilamentConfig::stage_workbench_color_pack_import(
        live, filament_import_card(9), staged, error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(staged.configuration);
    CHECK(staged.bindings.empty());
    CHECK(staged.changes.empty());
    CHECK(live.project_config == project_before);
    CHECK(live.filament_presets == presets_before);
}

TEST_CASE("partially loaded printers retain their spools through native normalization", "[PrinterFilamentRecipes][PrinterFilaments]")
{
    const size_t nozzles = GENERATE(4u, 6u);
    const size_t spools = nozzles - 2;
    auto bundle = printer_filament_fixture(nozzles, spools);
    bundle.update_filament_count();
    bundle.update_multi_material_filament_presets();
    const auto state = PrinterFilamentConfig::capture(bundle);
    CHECK(state.ready);
    CHECK(state.physical_count == spools);
    CHECK(state.idle_nozzles == 2);
    CHECK(bundle.filament_presets.size() == spools);
    CHECK(bundle.get_printer_extruder_count() == nozzles);
    std::string error;
    const bool valid = ProjectConfigRestore::validate(bundle.project_config, bundle.filament_presets, bundle, error);
    INFO(error);
    CHECK(valid);
}

TEST_CASE("over capacity color packs choose native reconstruction bases and create only virtual extras", "[PrinterFilamentRecipes][PrinterFilamentImport]")
{
    const size_t nozzles = GENERATE(4u, 6u);
    auto live = printer_filament_fixture(nozzles, 2);
    auto staged = live;
    auto pack = filament_import_card(nozzles + 2);
    pack.colors = {"#FFFFFF", "#000000", "#FF0000", "#FFFF00", "#FF8080", "#FF8000", "#0000FF", "#8080FF"};
    pack.colors.resize(nozzles + 2);
    const auto bases = WorkbenchFilamentRecipes::select_bases(pack.colors, nozzles);
    REQUIRE(bases.size() == nozzles);
    CHECK(std::is_sorted(bases.begin(), bases.end()));
    CHECK(WorkbenchFilamentRecipes::select_bases(pack.colors, nozzles) == bases);
    std::vector<size_t> first(nozzles); std::iota(first.begin(), first.end(), 0);
    CHECK(WorkbenchFilamentRecipes::reconstruction_error(pack.colors, bases) <=
        WorkbenchFilamentRecipes::reconstruction_error(pack.colors, first) + 1e-6);
    std::vector<PrinterFilamentConfig::SlotChange> changes;
    std::string error;
    REQUIRE(PrinterFilamentConfig::import_color_pack(staged, pack, changes, error));
    const auto state = PrinterFilamentConfig::capture(staged);
    INFO(error);
    REQUIRE(state.ready);
    CHECK(state.physical_count == nozzles);
    for (size_t i = 0; i < nozzles; ++i) CHECK(state.slots[i].color == pack.colors[bases[i]]);
    REQUIRE_FALSE(state.mixed.empty());
    for (const auto& recipe : state.mixed) CHECK(recipe.valid);
    CHECK(PrinterFilamentSlotRemap::validate(changes, live.filament_presets.size(), staged.filament_presets.size()));
    CHECK(live.filament_presets.size() == 2);
    auto before = staged.project_config;
    auto names = staged.filament_presets;
    std::vector<size_t> bindings;
    REQUIRE(WorkbenchFilamentRecipes::stage_targets(staged, pack.colors, bindings, error));
    CHECK(staged.project_config == before);
    CHECK(staged.filament_presets == names); // Repeated application reuses recipes.
}

TEST_CASE("large packs select deterministic bases without losing duplicate slot identities", "[PrinterFilamentRecipes]")
{
    auto colors = filament_import_card(12).colors;
    const auto bases = WorkbenchFilamentRecipes::select_bases(colors, 4);
    CHECK(bases.size() == 4);
    CHECK(WorkbenchFilamentRecipes::select_bases(colors, 4) == bases);
    std::vector<std::string> same(9, "#123456");
    CHECK(WorkbenchFilamentRecipes::select_bases(same, 6) == std::vector<size_t>{0,1,2,3,4,5});
    auto staged = printer_filament_fixture(4, 2);
    std::vector<size_t> bindings;
    std::string error;
    const bool valid = WorkbenchFilamentRecipes::stage_targets(staged, {"#123456", "#123456"}, bindings, error, {1,0});
    INFO(error);
    REQUIRE(valid);
    CHECK(bindings == std::vector<size_t>{1,0});
    CHECK(staged.filament_presets.size() == 2);
}

TEST_CASE("automatic recipes fail atomically for incompatible material or temperature", "[PrinterFilamentRecipes]")
{
    using namespace Slic3r;
    const bool different_type = GENERATE(false, true);
    auto staged = printer_filament_fixture(4, 2);
    auto material = staged.filaments.find_preset("Loaded PLA", false)->config;
    if (different_type) material.option<ConfigOptionStrings>("filament_type")->values = {"PETG"};
    else material.option<ConfigOptionInts>("nozzle_temperature")->values = {260};
    staged.filaments.load_preset("", "Other material", material, false);
    staged.filament_presets[1] = "Other material";
    const auto before = staged.project_config;
    const auto names = staged.filament_presets;
    std::vector<size_t> bindings {31};
    std::string error;
    CHECK_FALSE(WorkbenchFilamentRecipes::stage_targets(staged, {"#777777"}, bindings, error));
    CHECK_FALSE(error.empty());
    CHECK(staged.project_config == before);
    CHECK(staged.filament_presets == names);
    CHECK(bindings == std::vector<size_t>{31});
}

TEST_CASE("explicit mixed slot matching preserves groups and survives color trial persistence", "[PrinterFilamentRecipes]")
{
    Slic3r::AI::ColorTrialPersistence::State state;
    state.colors = {{1,0,0}, {0,0,1}};
    state.mapping_colors = state.colors;
    state.semantic_palette = state.colors;
    state.semantic_mapping_palette = state.colors;
    state.count = 2; state.source = 2;
    const auto before = state;
    const std::vector<PreviewPalette::Color> targets {{1,0,0}, {.5f,0,.5f}, {1,0,0}, {.5f,0,.5f}};
    REQUIRE(PrinterFilamentPreview::apply_bound(state, {1,7,1,7}, targets, "printer-with-recipe"));
    CHECK(state.mapping_colors == before.mapping_colors);
    CHECK(state.project_color_slots == std::vector<size_t>{1,7});
    CHECK(state.project_semantic_slots == std::vector<size_t>{1,7});
    Slic3r::AI::ColorTrialPersistence::State restored;
    std::string error;
    REQUIRE(Slic3r::AI::ColorTrialPersistence::decode(Slic3r::AI::ColorTrialPersistence::encode(state, 12, std::string(64,'a')),
        12, std::string(64,'a'), restored, error));
    CHECK(restored.project_color_slots == state.project_color_slots);
    CHECK_FALSE(PrinterFilamentPreview::apply_bound(state, {1}, targets, "stale"));
    CHECK(state.project_color_slots == restored.project_color_slots);
}

TEST_CASE("portrait semantic palette shares one printer slot binding per logical color", "[PrinterFilamentPreview][PostGenerationWorkbench]")
{
    Slic3r::AI::ColorTrialPersistence::State state;
    state.colors = {{1,0,0}, {0,1,0}, {0,0,1}, {1,1,0}, {1,0,1}, {0,1,1}};
    state.mapping_colors = state.colors;
    state.semantic_palette = {{.9f,0,0}, {0,.9f,0}, {0,0,.9f}, {.9f,.9f,0}, {.9f,0,.9f}, {0,.9f,.9f}};
    state.semantic_mapping_palette = state.semantic_palette;
    state.count = 6; state.source = 1; state.enabled = true;
    const std::vector<size_t> bindings {0, 1, 2, 3, 4, 5};
    REQUIRE(PrinterFilamentPreview::apply_bound(state, bindings, state.colors, "four-nozzle-printer"));
    CHECK(state.project_color_slots == bindings);
    CHECK(state.project_semantic_slots == bindings);
    CHECK(state.colors.size() == 6);
    CHECK(state.semantic_palette.size() == 6);
    CHECK(state.semantic_palette == state.colors);
}

TEST_CASE("publication accepts only the checked closed candidate and binds its own hash", "[WorkbenchCandidateAcceptance]")
{
    using namespace Slic3r;
    ScopedTemporaryFile source(".obj"), candidate(".obj");
    auto cube = its_make_cube(10,10,10);
    std::vector<std::array<float,4>> colors(cube.vertices.size(), {1,0,0,1});
    std::string error;
    REQUIRE(AI::write_model_artifact(source.path(), cube, colors, error));
    REQUIRE(AI::write_model_artifact(candidate.path(), cube, colors, error));
    auto repair = inspect_workbench_model(candidate.path());
    repair.source_sha256 = AI::model_artifact_sha256(source.path());
    repair.candidate_sha256 = repair.model_sha256;
    repair.candidate_status = RepairCandidateStatus::Ready;
    auto accepted = inspect_workbench_publication(candidate.path(), source.path(), &repair);
    CHECK(accepted.candidate_status == RepairCandidateStatus::Accepted);
    CHECK(accepted.status == WorkbenchCheckStatus::Normal);
    CHECK(accepted.model_sha256 == repair.candidate_sha256);
    CHECK_THROWS(inspect_workbench_publication(candidate.path(), source.path(), &repair, [] { return true; }));
    auto stale = repair; stale.source_sha256 = std::string(64,'b');
    CHECK_THROWS(inspect_workbench_publication(candidate.path(), source.path(), &stale));
    { boost::filesystem::ofstream out(candidate.path(), std::ios::app); out << "\n# changed\n"; }
    CHECK_THROWS(inspect_workbench_publication(candidate.path(), source.path(), &repair));
}

TEST_CASE("existing legal recipes refresh estimated colors and retain slot identities", "[PrinterFilamentRecipes]")
{
    using namespace Slic3r;
    auto draft = printer_filament_fixture(4, 4);
    const auto estimated = blend_color_multi({"#FF0000", "#0000FF"}, {50,50});
    draft.project_config.option<ConfigOptionStrings>("filament_colour")->values = {"#FF0000", "#0000FF", "#876543", estimated};
    for (size_t i : {2u, 3u}) {
        draft.project_config.option<ConfigOptionBools>("filament_is_mixed")->values[i] = true;
        draft.project_config.option<ConfigOptionStrings>("filament_mixed_components")->values[i] = "1,2";
        draft.project_config.option<ConfigOptionStrings>("filament_mixed_sublayer_ratios")->values[i] = "0.5,0.5";
    }
    REQUIRE(WorkbenchFilamentRecipes::valid_recipe(draft, 2));
    auto expected = draft.project_config;
    expected.option<ConfigOptionStrings>("filament_colour")->values[2] = estimated;
    expected.option<ConfigOptionStrings>("filament_multi_colour")->values[2] = estimated;
    std::vector<size_t> bindings;
    std::string error;
    REQUIRE(WorkbenchFilamentRecipes::stage_targets(draft, {estimated, estimated}, bindings, error, {2,3}));
    CHECK(bindings == std::vector<size_t>{2,3});
    CHECK(draft.project_config == expected);
    CHECK(draft.filament_presets.size() == 4);
}

TEST_CASE("color card imports grow physical slots to the entire ordered palette", "[PrinterFilamentImport][PostGenerationWorkbench]")
{
    const size_t nozzles = 1u;
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

TEST_CASE("color card imports shrink extra slots and allow idle nozzles", "[PrinterFilamentImport][PostGenerationWorkbench]")
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
    CHECK(state.physical_count == std::min(nozzles > 1 ? nozzles : size_t(32), colors));
    CHECK(choices == 8 - std::min(nozzles > 1 ? nozzles : size_t(32), colors));
    CHECK(PrinterFilamentSlotRemap::validate(changes, 8, draft.filament_presets.size()));
    CHECK(live.filament_presets.size() == 8);
}

TEST_CASE("switching an over-capacity color card keeps total slots equal to card colors", "[PrinterFilamentImport][PostGenerationWorkbench]")
{
    auto draft = printer_filament_fixture(4, 4);
    std::vector<PrinterFilamentConfig::SlotChange> changes;
    std::string error;
    REQUIRE(PrinterFilamentConfig::import_color_pack(draft, filament_import_card(6), changes, error));
    auto state = PrinterFilamentConfig::capture(draft);
    INFO(error);
    REQUIRE(state.ready);
    CHECK(state.physical_count == 4);
    CHECK(state.mixed.size() == 2);
    CHECK(draft.filament_presets.size() == 6);

    changes.clear();
    REQUIRE(PrinterFilamentConfig::import_color_pack(draft, filament_import_card(4), changes, error));
    state = PrinterFilamentConfig::capture(draft);
    INFO(error);
    REQUIRE(state.ready);
    CHECK(state.physical_count == 4);
    CHECK(state.mixed.empty());
    CHECK(draft.filament_presets.size() == 4);
    CHECK(PrinterFilamentSlotRemap::validate(changes, 6, 4));
}

TEST_CASE("color card cleanup compacts retained mixed recipes before tail deletion", "[PrinterFilamentImport][PrinterFilamentRecipes][PostGenerationWorkbench]")
{
    using namespace Slic3r;
    auto draft = printer_filament_fixture(4, 7);
    for (size_t slot = 4; slot < 7; ++slot) {
        draft.project_config.option<ConfigOptionBools>("filament_is_mixed")->values[slot] = true;
        draft.project_config.option<ConfigOptionStrings>("filament_mixed_components")->values[slot] = "1,2";
        draft.project_config.option<ConfigOptionStrings>("filament_mixed_sublayer_ratios")->values[slot] = "0.5,0.5";
        draft.project_config.option<ConfigOptionStrings>("filament_colour")->values[slot] = "#8A6A5A";
        draft.project_config.option<ConfigOptionStrings>("filament_multi_colour")->values[slot] = "#8A6A5A";
    }
    std::vector<size_t> bindings {0, 1, 2, 3, 6};
    std::vector<PrinterFilamentConfig::SlotChange> changes;
    std::string error;
    REQUIRE(PrinterFilamentConfig::trim_unused_mixed_slots(draft, bindings, 5, changes, error));
    INFO(error);
    REQUIRE(draft.filament_presets.size() == 5);
    CHECK(draft.is_mixed_filament(4));
    CHECK(std::find(bindings.begin(), bindings.end(), 4) != bindings.end());
    REQUIRE(changes.size() == 2);
    CHECK(changes.front().removed == 6);
    CHECK(changes.front().replacement == 4);
    CHECK(changes.back().removed == 5);
    CHECK(PrinterFilamentSlotRemap::validate(changes, 7, 5));
}

TEST_CASE("printer palette cleanup retains a mixed binding in addition to all physical slots", "[PrinterFilamentImport][PrinterFilamentRecipes][PostGenerationWorkbench]")
{
    using namespace Slic3r;
    auto draft = printer_filament_fixture(4, 6);
    for (size_t slot = 4; slot < 6; ++slot) {
        draft.project_config.option<ConfigOptionBools>("filament_is_mixed")->values[slot] = true;
        draft.project_config.option<ConfigOptionStrings>("filament_mixed_components")->values[slot] = "1,2";
        draft.project_config.option<ConfigOptionStrings>("filament_mixed_sublayer_ratios")->values[slot] = "0.5,0.5";
        draft.project_config.option<ConfigOptionStrings>("filament_colour")->values[slot] = "#8A6A5A";
        draft.project_config.option<ConfigOptionStrings>("filament_multi_colour")->values[slot] = "#8A6A5A";
    }
    std::vector<size_t> bindings {0, 1, 2, 5};
    std::vector<PrinterFilamentConfig::SlotChange> changes;
    std::string error;
    REQUIRE(PrinterFilamentConfig::trim_unused_mixed_slots(draft, bindings, 4, changes, error));
    INFO(error);
    const auto state = PrinterFilamentConfig::capture(draft);
    CHECK(state.physical_count == 4);
    REQUIRE(state.mixed.size() == 1);
    CHECK(state.mixed.front().slot == 4);
    CHECK(draft.filament_presets.size() == 5);
    CHECK(bindings == std::vector<size_t>{0, 1, 2, 4});
    REQUIRE(changes.size() == 1);
    CHECK(changes.front().removed == 5);
    CHECK(changes.front().replacement == 4);
    CHECK(PrinterFilamentSlotRemap::validate(changes, 6, 5));
}

TEST_CASE("printer palette application trims surplus physical slots before mixed cleanup", "[PrinterFilamentImport][PrinterFilamentRecipes][PostGenerationWorkbench]")
{
    using namespace Slic3r;
    auto draft = printer_filament_fixture(4, 6);
    std::vector<size_t> bindings {0, 1, 2, 3};
    std::vector<PrinterFilamentConfig::SlotChange> changes;
    std::string error;
    REQUIRE(PrinterFilamentConfig::trim_unused_physical_slots(draft, bindings, 4, changes, error));
    INFO(error);
    CHECK(PrinterFilamentConfig::capture(draft).physical_count == 4);
    CHECK(draft.filament_presets.size() == 4);
    CHECK(PrinterFilamentSlotRemap::validate(changes, 6, 4));
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
    auto live = printer_filament_fixture(1, 6);
    live.project_config.option<ConfigOptionBools>("filament_is_mixed")->values.back() = true;
    live.project_config.option<ConfigOptionStrings>("filament_mixed_components")->values.back() = "1,5";
    live.project_config.option<ConfigOptionStrings>("filament_mixed_sublayer_ratios")->values.back() = "0.5,0.5";
    auto draft = live;
    std::vector<PrinterFilamentConfig::SlotChange> changes;
    std::string error;
    REQUIRE(PrinterFilamentConfig::import_color_pack(draft, filament_import_card(8), changes, error));
    CHECK(PrinterFilamentConfig::capture(draft).physical_count == 8);
    CHECK(draft.filament_presets.size() == 8);
    CHECK_FALSE(draft.is_mixed_filament(7));
    REQUIRE(PrinterFilamentConfig::import_color_pack(draft, filament_import_card(4), changes, error,
        [](const auto&, const auto& targets) -> std::optional<size_t> { return targets.back().slot; }));
    CHECK(PrinterFilamentConfig::capture(draft).physical_count == 4);
    CHECK(draft.filament_presets.size() == 4);
    CHECK_FALSE(draft.is_mixed_filament(3));
    CHECK(PrinterFilamentSlotRemap::validate(changes, 6, 4));

    draft = live;
    changes.clear();
    REQUIRE(PrinterFilamentConfig::import_color_pack(draft, filament_import_card(32), changes, error));
    const auto full = PrinterFilamentConfig::capture(draft);
    REQUIRE(full.ready);
    CHECK(full.physical_count == 32);
    CHECK(full.mixed.empty());
    CHECK(draft.filament_presets.size() == 32);
    CHECK(PrinterFilamentSlotRemap::validate(changes, 6, 32));
}

TEST_CASE("card import leaves missing or incompatible material choices pending without dropping colors", "[PrinterFilamentImport][PostGenerationWorkbench]")
{
    using namespace Slic3r;
    auto draft = printer_filament_fixture(1, 5);
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
    auto draft = printer_filament_fixture(1, 8);
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
    REQUIRE(PrinterFilamentSlotRemap::validate(changes, 8, 6));
    PrinterFilamentSlotRemap::prepare(model, changes, 8, mixed);
    CHECK(object->config.opt_int("extruder") == 4);
    CHECK(volume->config.opt_int("extruder") == 1);
    TriangleSelector painted(volume->mesh());
    painted.deserialize(volume->mmu_segmentation_facets.get_data());
    CHECK_FALSE(painted.get_facets(EnforcerBlockerType::Extruder4).indices.empty());
    CHECK_FALSE(painted.get_facets(EnforcerBlockerType::Extruder1).indices.empty());
    CHECK(painted.get_facets(EnforcerBlockerType::Extruder8).indices.empty());
    CHECK(volume->mesh().its.vertices == vertices);
    DynamicPrintConfig plate;
    plate.set_key_value("first_layer_print_sequence", new ConfigOptionInts({1,2,3,4,5,6,7,8}));
    for (const auto& change : changes) PrinterFilamentSlotRemap::plate_config(plate, change);
    auto sequence = plate.option<ConfigOptionInts>("first_layer_print_sequence")->values;
    std::sort(sequence.begin(), sequence.end());
    CHECK(sequence == std::vector<int>{1,2,3,4,5,6});
}

TEST_CASE("returning to an identical filament card can still change model references", "[PrinterFilamentImport][PostGenerationWorkbench]")
{
    using namespace Slic3r;
    auto live = printer_filament_fixture(1, 8);
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
