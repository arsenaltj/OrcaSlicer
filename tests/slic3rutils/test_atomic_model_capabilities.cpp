#include <catch2/catch_test_macros.hpp>
#include "test_utils.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelWorkspaceCapabilities.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelLibraryLoader.hpp"
#include "slic3r/GUI/AI/ModelGeneration/WorkbenchHistorySnapshot.hpp"
#include "slic3r/GUI/AI/ModelGeneration/PostGenerationWorkbenchState.hpp"
#include "slic3r/GUI/AI/ModelGeneration/WorkbenchImportPresentation.hpp"
#include "slic3r/GUI/AI/ModelGeneration/BeautyWorkbenchSession.hpp"
#include "slic3r/GUI/AI/ColorMatching/ColorMatchingComputation.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <condition_variable>
#include <chrono>

using namespace Slic3r;
using namespace Slic3r::GUI;

TEST_CASE("Saved versions in one task retain independent asset and thumbnail identities", "[AtomicModelCapabilities][WorkbenchHistory]")
{
    ModelLibraryEntry original, saved;
    original.job_id = "task"; original.model_path = "original.obj";
    saved = original; saved.model_path = "saved.obj";
    CHECK(workbench_history_key(original) != workbench_history_key(saved));
    const auto identity = workbench_history_key(original);
    original.title = "Renamed";
    CHECK(workbench_history_key(original) == identity);
    ModelGenerationUIHistoryEntry previous, next;
    previous.asset_key = workbench_history_key(saved); previous.thumbnail_source = "source96";
    previous.thumbnail_ready = true; previous.thumbnail_revision = 7;
    previous.thumbnail_width = 1; previous.thumbnail_height = 1; previous.thumbnail_rgb = {10, 20, 30};
    next.asset_key = previous.asset_key; next.thumbnail_source = previous.thumbnail_source;
    REQUIRE(retain_workbench_thumbnail(next, previous));
    CHECK(next.thumbnail_rgb == previous.thumbnail_rgb); CHECK(next.thumbnail_revision == 7);
    next.asset_key = workbench_history_key(original);
    CHECK_FALSE(retain_workbench_thumbnail(next, previous));
}

TEST_CASE("Changed thumbnail sources DPI and unfinished decodes invalidate only their cached image", "[AtomicModelCapabilities][WorkbenchHistory]")
{
    ModelGenerationUIHistoryEntry previous, next;
    previous.asset_key = next.asset_key = "model";
    previous.thumbnail_source = next.thumbnail_source = "stamp72";
    CHECK_FALSE(retain_workbench_thumbnail(next, previous));
    previous.thumbnail_ready = true;
    CHECK(retain_workbench_thumbnail(next, previous)); // Unavailable thumbnail is also a completed decode.
    next.thumbnail_source = "stamp96";
    CHECK_FALSE(retain_workbench_thumbnail(next, previous));
    ModelLibraryEntry entry;
    entry.ai_image_path = "missing-primary.png"; entry.reference_image_path = "missing-fallback.png";
    const auto signature = workbench_thumbnail_source(entry, "library", 72);
    CHECK(workbench_thumbnail_source(entry, "library", 96) != signature);
    entry.reference_image_path = "different.png";
    CHECK(workbench_thumbnail_source(entry, "library", 72) != signature);
    CHECK(fit_workbench_thumbnail_size(640, 480, 96, 96) == wxSize(96, 72));
    CHECK(fit_workbench_thumbnail_size(480, 640, 96, 96) == wxSize(72, 96));
    CHECK(fit_workbench_thumbnail_size(0, 640, 96, 96) == wxSize());
}

TEST_CASE("Workbench check summary stays compact while details retain diagnostics",
          "[AtomicModelCapabilities][PostGenerationWorkbench]")
{
    WorkbenchCheckResult result;
    CHECK(workbench_check_status_line(result) == "模型尚未检查");
    result.status = WorkbenchCheckStatus::Running;
    result.phase = WorkbenchCheckPhase::Repairing;
    CHECK(workbench_check_status_line(result) == "正在修复模型……");
    result.status = WorkbenchCheckStatus::Normal;
    result.phase = WorkbenchCheckPhase::Idle;
    result.summary = "开放边 0；非流形边 0。";
    CHECK(workbench_check_status_line(result) == "模型检查已完成，未发现问题");
    result.repair_reason = "已清理 2 个面。";
    result.before_faces = 10; result.after_faces = 8;
    result.before_vertices = 12; result.after_vertices = 11;
    result.removed_faces = 2;
    CHECK(workbench_check_status_line(result) == "模型修复已完成");
    const auto details = workbench_check_detail_text(result);
    CHECK(details.find("修复前：10 面 / 12 点") != std::string::npos);
    CHECK(details.find("拓扑指标") != std::string::npos);
    result.status = WorkbenchCheckStatus::Attention;
    CHECK(workbench_check_status_line(result, true) == "已忽略轻微问题，可继续导入切片");
}

TEST_CASE("Partial thumbnail requests publish into their original asset slots", "[AtomicModelCapabilities][WorkbenchHistory]")
{
    ModelLibraryLoader loader({{}, [](const auto&, const auto&, int, const auto&) {
        ModelLibraryLoader::Thumbnail result {0, 0}; result.width = result.height = 1; result.rgb = {4, 5, 6}; return result;
    }});
    auto request = std::make_shared<ModelLibraryLoader::Request>();
    request->revision = 23; request->for_shell = true; request->entries.resize(2); request->indices = {3, 8};
    request->indices.pop_back(); CHECK_FALSE(loader.submit(request));
    request->indices.push_back(8);
    REQUIRE(loader.submit(request));
    std::optional<ModelLibraryLoader::Snapshot> snapshot;
    std::vector<ModelLibraryLoader::Thumbnail> results, batch;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (results.size() < 2 && std::chrono::steady_clock::now() < deadline) {
        loader.take(snapshot, batch); results.insert(results.end(), batch.begin(), batch.end()); std::this_thread::yield();
    }
    loader.stop(); REQUIRE(results.size() == 2);
    CHECK(results[0].index == 3); CHECK(results[1].index == 8);
    CHECK(results[0].revision == 23); CHECK(results[1].for_shell);
}

TEST_CASE("Import feedback remains inside the center viewport across sidebar sizes and clipping", "[AtomicModelCapabilities][WorkbenchImportPresentation]")
{
    const wxRect content(0, 0, 1200, 800);
    const auto center = workbench_import_feedback_bounds(wxRect(280, 12, 628, 776), content);
    CHECK(center == wxRect(280, 12, 628, 776));
    CHECK(center.GetRight() < 920); // Visible right asset rail.
    CHECK(workbench_import_feedback_bounds(wxRect(-100, 10, 600, 780), content) == wxRect(0, 10, 500, 780));
    CHECK(workbench_import_feedback_bounds(wxRect(280, 0, 900, 900), wxRect(0, 0, 900, 600)) == wxRect(280, 0, 620, 600));
    CHECK(workbench_import_feedback_bounds(wxRect(1300, 0, 100, 100), content).IsEmpty());
}
namespace Matching = LocalPrintColorMatching;

namespace {
Matching::Input input()
{
    Matching::Input value;
    value.identity.source_sha256 = std::string(64, 'a'); value.identity.geometry_id = std::string(64, 'b');
    value.identity.material_fingerprint = "materials"; value.identity.process_fingerprint = "process";
    value.identity.requested_color_count = 2; value.identity.face_count = 2;
    value.identity.physical_channels = {{0, "#FFFFFF", "PLA", true}, {1, "#000000", "PLA", true}};
    value.faces = {{{1.f, 1.f, 1.f}, 1.0}, {{0.f, 0.f, 0.f}, 1.0}};
    return value;
}
struct Snapshot { boost::filesystem::path candidate, source; int value; };
}

TEST_CASE("Minimal model capabilities compose without creating a window or generation panel", "[AtomicModelCapabilities]")
{
    int opened = 0;
    ModelWorkspaceCapabilities capabilities("fixture-root", {{}, [&](ModelLibraryCommand kind, const ModelLibraryEntry& item) {
        CHECK(kind == ModelLibraryCommand::OpenModel); CHECK(item.job_id == "existing"); ++opened; return true;
    }});
    ModelLibraryEntry asset; asset.job_id = "existing"; asset.model_path = "existing.glb";
    capabilities.library->replace({asset});
    CHECK(capabilities.library->execute(ModelLibraryCommand::OpenModel, capabilities.library->token(0)));
    capabilities.colors->draft().input = input();
    auto result = Matching::compute(capabilities.colors->draft().input); REQUIRE(result.ok());
    CHECK(capabilities.colors->publish(0, std::string(64, 'a'), std::string(64, 'b'), std::move(result)));
    CHECK(opened == 1); CHECK(capabilities.colors->draft().has_result);
}

TEST_CASE("Library actions reject changed snapshots and active edits without invoking ports", "[AtomicModelCapabilities]")
{
    bool editable = true; int calls = 0;
    ModelLibraryApplication library("root", {[&] { return editable; }, [&](auto, const auto&) { ++calls; return true; }});
    ModelLibraryEntry asset; asset.job_id = "asset"; asset.model_path = "asset.glb";
    library.replace({asset}); const auto token = library.token(0);
    SECTION("list replacement") { library.replace({asset}); }
    SECTION("active edit") { editable = false; }
    SECTION("design cannot be imported") { asset.design_only = true; library.replace({asset}); }
    CHECK_FALSE(library.execute(ModelLibraryCommand::Import, token)); CHECK(calls == 0);
}

TEST_CASE("Library actions preserve the selected asset across synchronous list replacement", "[AtomicModelCapabilities]")
{
    ModelLibraryApplication library("root");
    ModelLibraryEntry asset; asset.job_id = "source"; library.replace({asset});
    library.set_ports({{}, [&](auto, const ModelLibraryEntry& entry) {
        library.replace({}); CHECK(entry.job_id == "source"); return true;
    }});
    CHECK(library.execute(ModelLibraryCommand::OpenImage, library.token(0)));
}

TEST_CASE("Library permission callbacks cannot execute detached or replaced assets", "[AtomicModelCapabilities]")
{
    ModelLibraryApplication library("root");
    ModelLibraryEntry asset; asset.job_id = "source"; library.replace({asset});
    int calls = 0;
    SECTION("list changes while permission is checked") {
        library.set_ports({[&] { library.replace({}); return true; }, [&](auto, const auto&) { ++calls; return true; }});
        CHECK_FALSE(library.execute(ModelLibraryCommand::OpenModel, library.token(0)));
    }
    SECTION("ports detach while permission is checked") {
        library.set_ports({[&] { library.set_ports({}); return true; }, [&](auto, const auto&) { ++calls; return true; }});
        CHECK_FALSE(library.execute(ModelLibraryCommand::OpenModel, library.token(0)));
    }
    SECTION("local import detaches while permission is checked") {
        library.set_ports({[&] { library.set_ports({}); return true; }, {}, [&] { ++calls; }});
        library.choose_local();
    }
    SECTION("old token cannot invoke a new policy") {
        const auto token = library.token(0);
        library.set_ports({[&] { ++calls; return true; }, [&](auto, const auto&) { ++calls; return true; }});
        CHECK_FALSE(library.execute(ModelLibraryCommand::OpenModel, token));
    }
    CHECK(calls == 0);
}

TEST_CASE("Library worker cancellation cannot publish an old scan into a new request", "[AtomicModelCapabilities]")
{
    std::mutex mutex; std::condition_variable wake; bool first_started = false, release = false;
    std::atomic<bool> observed_cancellation {false};
    ModelLibraryLoader loader({[&](const boost::filesystem::path& root, const std::atomic<bool>& cancelled) {
        if (root == "old") {
            std::unique_lock<std::mutex> lock(mutex); first_started = true; wake.notify_one();
            wake.wait(lock, [&] { return release; }); observed_cancellation = cancelled.load();
        }
        ModelLibraryEntry entry; entry.job_id = root.string(); return std::vector<ModelLibraryEntry>{entry};
    }, {}});
    struct ReleaseWorker {
        std::mutex& mutex; std::condition_variable& wake; bool& release;
        ~ReleaseWorker() { { std::lock_guard<std::mutex> lock(mutex); release = true; } wake.notify_one(); }
    } release_worker {mutex, wake, release};
    auto old = std::make_shared<ModelLibraryLoader::Request>(); old->scan = true; old->revision = 1; old->root = "old";
    REQUIRE(loader.submit(old));
    { std::unique_lock<std::mutex> lock(mutex); REQUIRE(wake.wait_for(lock, std::chrono::seconds(3), [&] { return first_started; })); }
    auto current = std::make_shared<ModelLibraryLoader::Request>(); current->scan = true; current->revision = 2; current->root = "current";
    REQUIRE(loader.submit(current));
    { std::lock_guard<std::mutex> lock(mutex); release = true; } wake.notify_one();
    std::optional<ModelLibraryLoader::Snapshot> result; std::vector<ModelLibraryLoader::Thumbnail> thumbnails;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!result && std::chrono::steady_clock::now() < deadline) { loader.take(result, thumbnails); std::this_thread::yield(); }
    loader.stop(); REQUIRE(result); CHECK(result->revision == 2); REQUIRE(result->entries.size() == 1);
    CHECK(result->entries[0].job_id == "current"); CHECK_FALSE(loader.submit(current));
    CHECK(observed_cancellation.load());
}

TEST_CASE("Library read failures preserve the request identity", "[AtomicModelCapabilities]")
{
    ModelLibraryLoader loader({[](const auto&, const auto&) -> std::vector<ModelLibraryEntry> { throw std::runtime_error("offline failure"); }, {}});
    auto request = std::make_shared<ModelLibraryLoader::Request>(); request->scan = true; request->revision = 7;
    REQUIRE(loader.submit(request));
    std::optional<ModelLibraryLoader::Snapshot> result; std::vector<ModelLibraryLoader::Thumbnail> thumbnails;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!result && std::chrono::steady_clock::now() < deadline) { loader.take(result, thumbnails); std::this_thread::yield(); }
    loader.stop(); REQUIRE(result); CHECK(result->failed); CHECK(result->revision == 7);
}

TEST_CASE("Malformed library thumbnails preserve assets and cancellation rejects deferred publication", "[AtomicModelCapabilities]")
{
    ModelLibraryLoader loader({{}, [](const auto&, const auto&, int, const auto&) -> ModelLibraryLoader::Thumbnail {
        throw std::runtime_error("malformed image");
    }});
    auto request = std::make_shared<ModelLibraryLoader::Request>(); request->revision = 9;
    ModelLibraryEntry asset; asset.job_id = "retained"; request->entries.push_back(asset);
    REQUIRE(loader.submit(request));
    std::optional<ModelLibraryLoader::Snapshot> result; std::vector<ModelLibraryLoader::Thumbnail> thumbnails;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (thumbnails.empty() && std::chrono::steady_clock::now() < deadline) { loader.take(result, thumbnails); std::this_thread::yield(); }
    REQUIRE(thumbnails.size() == 1); CHECK(thumbnails[0].revision == 9); CHECK(thumbnails[0].rgb.empty());
    REQUIRE(request->entries.size() == 1); CHECK(request->entries[0].job_id == "retained");
    const auto late = thumbnails[0]; loader.cancel(); loader.defer(late); thumbnails.clear();
    loader.take(result, thumbnails); CHECK_FALSE(result); CHECK(thumbnails.empty()); loader.stop();
}

TEST_CASE("Color session history retains drafts and blocks restoration during computation", "[AtomicModelCapabilities]")
{
    ColorMatchingSession session; session.draft().input = input(); session.checkpoint();
    session.draft().input.identity.requested_color_count = 1;
    session.operation().busy = true; CHECK_FALSE(session.restore(false));
    session.operation().busy = false; REQUIRE(session.restore(false)); CHECK(session.draft().input.identity.requested_color_count == 2);
    REQUIRE(session.restore(true)); CHECK(session.draft().input.identity.requested_color_count == 1);
    CHECK_FALSE(session.draft().has_result);
}

TEST_CASE("Color session rejects cancellation stale geometry and late results", "[AtomicModelCapabilities]")
{
    ColorMatchingSession session; session.draft().input = input(); session.operation().revision = 4;
    auto result = Matching::compute(session.draft().input); REQUIRE(result.ok());
    uint64_t revision = 4; std::string source(64, 'a'), geometry(64, 'b');
    SECTION("late result") { revision = 3; }
    SECTION("cancellation") { session.operation().cancel = true; }
    SECTION("changed source") { source = std::string(64, 'c'); }
    SECTION("changed geometry") { geometry = std::string(64, 'c'); }
    SECTION("shutdown") { session.shutdown(); }
    CHECK_FALSE(session.publish(revision, source, geometry, std::move(result))); CHECK_FALSE(session.draft().has_result);
}

TEST_CASE("Color session confirmation guards use fresh material process and intent", "[AtomicModelCapabilities]")
{
    ColorMatchingSession session; session.draft().input = input(); auto result = Matching::compute(session.draft().input);
    REQUIRE(result.ok()); REQUIRE(session.publish(0, std::string(64, 'a'), std::string(64, 'b'), std::move(result)));
    AI::PrintablePaletteSnapshot current; current.material_fingerprint = "materials"; current.process_fingerprint = "process";
    CHECK(session.can_apply(current, 2, false, true));
    current.material_fingerprint = "changed"; CHECK_FALSE(session.can_apply(current, 2, false, true));
    current.material_fingerprint = "materials"; current.process_fingerprint = "changed"; CHECK_FALSE(session.can_apply(current, 2, false, true));
    current.process_fingerprint = "process"; CHECK_FALSE(session.can_apply(current, 1, false, true)); CHECK_FALSE(session.can_apply(current, 2, true, true));
}

TEST_CASE("Beauty session keeps accepted files and failed restoration on the undo timeline", "[AtomicModelCapabilities]")
{
    BeautyWorkbenchSession<Snapshot> session; int restored = 0; std::vector<std::string> removed;
    bool valid = true;
    auto before = std::make_shared<Snapshot>(Snapshot{{}, "source.glb", 1});
    auto after = std::make_shared<Snapshot>(Snapshot{"candidate.glb", "source.glb", 2});
    BeautyWorkbenchSession<Snapshot>::Ports ports {[&](const auto&) { return valid; }, [&](const auto& value) { restored = value.value; return true; },
        [&](const auto& path) { removed.push_back(path.string()); }};
    session.record(BeautyWorkbenchTransactionController::OperationKind::AppearanceRecolor, before, after, ports);
    valid = false; CHECK_FALSE(session.transactions().undo()); CHECK(session.transactions().undo_count() == 1); CHECK(restored == 0);
    valid = true; REQUIRE(session.transactions().undo()); CHECK(restored == 1); REQUIRE(session.transactions().redo()); CHECK(restored == 2);
    session.draft().accepted_files.push_back("candidate.glb"); session.draft().candidate_files.push_back("temporary.glb");
    session.clear_candidates(0, "source.glb", ports); REQUIRE(removed.size() == 1); CHECK(removed[0] == "temporary.glb");
}

TEST_CASE("Color computation fails safely without a frozen surface", "[AtomicModelCapabilities]")
{
    ColorMatchingComputationRequest request; request.input = input(); std::atomic<bool> cancelled {false};
    const auto result = compute_color_matching(std::move(request), cancelled);
    CHECK_FALSE(result.matching.ok()); CHECK_FALSE(result.matching.error.empty()); CHECK_FALSE(result.evidence);
}

TEST_CASE("Production color computation composes with source identity without a GUI", "[AtomicModelCapabilities]")
{
    namespace fs = boost::filesystem;
    ScopedTemporaryFile file(".obj"); const auto& path = file.path();
    const auto topology = std::make_shared<const indexed_triangle_set>(its_make_cube(10, 10, 10));
    { fs::ofstream output(path);
      for (const auto& vertex : topology->vertices) output << "v " << vertex.x() << ' ' << vertex.y() << ' ' << vertex.z() << " 1 1 1\n";
      for (const auto& face : topology->indices) output << "f " << face.x() + 1 << ' ' << face.y() + 1 << ' ' << face.z() + 1 << '\n'; }
    ColorMatchingComputationRequest request;
    request.input = input(); request.input.identity.source_sha256 = AI::model_artifact_sha256(path);
    request.input.identity.geometry_id = AI::SurfaceSelectionPersistence::geometry_fingerprint(*topology);
    request.input.identity.face_count = topology->indices.size();
    request.input.faces.clear();
    for (size_t index = 0; index < topology->indices.size(); ++index)
        request.input.faces.push_back({index % 2 ? AI::PrintRgb {1.f, 1.f, 1.f} : AI::PrintRgb {0.f, 0.f, 0.f}, 50.0});
    request.source = path; request.topology = topology;
    std::atomic<bool> cancelled {false};
    const auto result = compute_color_matching(request, cancelled);
    REQUIRE(result.matching.ok()); CHECK(result.matching.result.face_targets.size() == topology->indices.size());
    cancelled = true; CHECK(compute_color_matching(request, cancelled).matching.cancelled);
    cancelled = false; { fs::ofstream changed(path, std::ios::app); changed << "# source changed\n"; }
    CHECK_FALSE(compute_color_matching(request, cancelled).matching.ok());
}
