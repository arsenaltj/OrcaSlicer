#include "slic3r/GUI/AI/ModelGeneration/WorkbenchModelContext.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelArtifactAcquisition.hpp"
#include "slic3r/GUI/AI/ModelGeneration/AdvancedModelTaskFlow.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelGenerationPresentation.hpp"
#include "slic3r/GUI/Redesign/RedesignModelRoute.hpp"
#include "../test_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace Slic3r;
using namespace Slic3r::GUI;

TEST_CASE("The selected task keeps its own progress and recovery across workbench reentry", "[UnifiedWorkbench]")
{
    ModelGenerationUIState smart, advanced;
    smart.smart_mode = true; smart.revision = 300; smart.model_generation_session = 30;
    smart.job_id = "smart"; smart.progress = 27; smart.model_elapsed_seconds = 80;
    advanced.revision = 2; advanced.model_generation_session = 1;
    advanced.job_id = "advanced"; advanced.progress = 68; advanced.model_elapsed_seconds = 40;
    advanced.can_retry_model = true;
    const bool use_advanced = GENERATE(false, true);
    const auto& selected = selected_model_presentation(use_advanced, smart, advanced);
    CHECK(selected.job_id == (use_advanced ? "advanced" : "smart"));
    CHECK(selected.progress == (use_advanced ? 68 : 27));
    CHECK(selected.model_elapsed_seconds == (use_advanced ? 40 : 80));
    CHECK(selected.can_retry_model == use_advanced);
    CHECK(selected.smart_mode == !use_advanced);
}

TEST_CASE("History and saved versions retain the editor return source until another explicit result opens", "[UnifiedWorkbench]")
{
    const bool origin = GENERATE(false, true);
    const std::optional<bool> anchor = origin;
    CHECK(workbench_return_advanced(true, anchor, !origin) == origin);
    CHECK(workbench_return_advanced(false, anchor, !origin) == !origin);
    CHECK(workbench_return_advanced(true, std::nullopt, origin) == origin);
}

TEST_CASE("Editor import metadata follows the opened asset independently of generation inputs", "[UnifiedWorkbench]")
{
    WorkbenchModelContext current;
    current.path = "advanced.glb"; current.asset_id = "four-view";
    current.format = "glb"; current.color_encoding = "textures_or_vertex_colors";
    current.palette = {"#ff0000", "#00ff00"}; current.palette_roles = {{"primary", "#ff0000"}};
    current.use_printable_colors = true;
    current.color_intent_path = "advanced-color.json";
    current.color_intent_schema = "schema"; current.color_intent_sha256 = std::string(64, 'a');
    auto first = current.artifact();
    current.path = "finish-version.obj"; current.asset_id = "saved";
    current.format = "obj"; current.color_encoding = "vertex_colors";
    current.palette.clear(); current.use_printable_colors = false; current.color_intent_path.clear();
    const auto saved = current.artifact();
    CHECK(saved.local_path == boost::filesystem::path("finish-version.obj"));
    CHECK(saved.job_id == "saved"); CHECK(saved.format == "obj");
    CHECK(saved.generation_palette.empty()); CHECK_FALSE(saved.used_printable_colors);
    CHECK_FALSE(saved.color_intent_manifest);
    CHECK(first.job_id == "four-view"); CHECK(first.format == "glb");
    REQUIRE(first.color_intent_manifest);
    CHECK(first.color_intent_manifest->local_path == "advanced-color.json");
    CHECK(first.generation_palette.size() == 2);
}

TEST_CASE("Acquisition keeps the existing supported model and color contract for both sources", "[UnifiedWorkbench]")
{
    CHECK(ModelArtifactAcquisition::supported("glb", "textures_or_vertex_colors"));
    CHECK(ModelArtifactAcquisition::supported("obj", "vertex_colors"));
    CHECK_FALSE(ModelArtifactAcquisition::supported("obj", "textures"));
    CHECK_FALSE(ModelArtifactAcquisition::supported("stl", "vertex_colors"));
}

TEST_CASE("Reacquiring an artifact preserves its first generation date and import feedback", "[UnifiedWorkbench]")
{
    ScopedTemporaryDir temporary {"orca-unified-acquisition"};
    const auto old_data = data_dir();
    set_data_dir(temporary.path().string());
    const auto record_path = ModelGenerationPresentation::library_metadata_path("job");
    struct Restore { std::string path; ~Restore() { set_data_dir(path); } } restore{old_data};
    REQUIRE(ModelArtifactAcquisition::publish_library_record("job",
        {{"job_id", "job"}, {"generated_at", 100}, {"imported_at", 200}, {"print_feedback", "kept"}}));
    REQUIRE(ModelArtifactAcquisition::publish_library_record("job",
        {{"job_id", "job"}, {"generated_at", 900}, {"model_path", "downloads/model.glb"}}));
    const auto record = ModelGenerationPresentation::read_json(record_path);
    CHECK(record.at("generated_at") == 100);
    CHECK(record.at("imported_at") == 200);
    CHECK(record.at("print_feedback") == "kept");
    CHECK(record.at("model_path") == "downloads/model.glb");
}

namespace {
struct TaskHarness {
    using Status = AIModelGenerationClient::JobStatus;
    using Error = AIModelGenerationClient::ErrorFn;
    struct Query { std::string id; AIModelGenerationClient::StatusFn done; Error error; };
    struct Download { std::string id; std::function<void(AI::GeneratedModelArtifact)> done; Error error;
        std::function<void(size_t, size_t)> progress; };
    struct Preparation { std::string id; boost::filesystem::path reference;
        std::function<void(ModelLibraryEntry)> done; Error error; };
    std::vector<Query> queries;
    std::vector<Download> downloads;
    std::vector<Preparation> preparations;
    std::vector<ModelGenerationUIState> states;
    int cancels {0}, scheduled {0};
    std::unique_ptr<AdvancedModelTaskFlow> flow;
    TaskHarness() {
        flow = std::make_unique<AdvancedModelTaskFlow>(AdvancedModelTaskFlow::Ports{
            [this](const auto& job, auto done, auto error) { queries.push_back({job, done, error}); },
            [this](const auto& status, auto done, auto error, auto progress) {
                downloads.push_back({status.id, done, error, progress});
            },
            [this](auto artifact, const auto&, const auto& reference, auto done, auto error) {
                preparations.push_back({artifact.job_id, reference, done, error});
            },
            [this](int delay) { scheduled = delay; }, [this] { ++cancels; scheduled = 0; }
        });
        flow->set_listener([this](const auto& state) { states.push_back(state); });
        flow->set_available(true);
    }
    static Status status(std::string job, std::string state = "generating", double updated = 10) {
        Status value; value.id = std::move(job); value.state = std::move(state);
        value.creation_kind = "advanced_multiview_v1";
        value.phase = value.state == "ready" ? "ready" : "generating";
        value.provider_name = "tripo"; value.provider_task_id = "provider-task";
        value.updated_at = updated; value.progress = 50;
        value.artifact_ready = value.state == "ready";
        value.artifact_format = "glb"; value.artifact_color_encoding = "textures_or_vertex_colors";
        return value;
    }
    static AI::GeneratedModelArtifact artifact(std::string job) {
        AI::GeneratedModelArtifact value; value.job_id = job; value.local_path = job + ".glb";
        value.format = "glb"; return value;
    }
    static ModelLibraryEntry entry(std::string job) {
        ModelLibraryEntry value; value.job_id = job; value.model_path = job + ".glb"; return value;
    }
    void ready() { flow->observe("advanced", "front.png"); queries.back().done(status("advanced", "ready")); }
};
}

TEST_CASE("Advanced observation follows one explicit job without restarting it on page reentry", "[UnifiedWorkbench]")
{
    TaskHarness h;
    h.flow->observe("advanced", "front.png");
    const auto session = h.flow->snapshot().model_generation_session;
    h.flow->poll(); h.flow->observe("advanced", "front.png");
    REQUIRE(h.queries.size() == 1);
    CHECK(h.queries[0].id == "advanced");
    CHECK(h.flow->snapshot().model_generation_session == session);
    CHECK(h.flow->snapshot().stage == ModelGenerationUIStage::GeneratingModel);
    CHECK_FALSE(h.flow->snapshot().smart_mode);
    CHECK(h.downloads.empty());
}

TEST_CASE("Advanced remote completion waits for download and local preparation before opening", "[UnifiedWorkbench]")
{
    TaskHarness h; h.ready();
    REQUIRE(h.downloads.size() == 1);
    CHECK_FALSE(h.flow->snapshot().model_ready);
    CHECK(h.flow->snapshot().stage == ModelGenerationUIStage::LoadingModel);
    h.downloads[0].progress(25, 100);
    CHECK(h.flow->snapshot().progress < 100);
    h.downloads[0].done(TaskHarness::artifact("advanced"));
    h.downloads[0].done(TaskHarness::artifact("advanced"));
    REQUIRE(h.preparations.size() == 1);
    CHECK(h.preparations[0].reference == boost::filesystem::path("front.png"));
    CHECK_FALSE(h.flow->snapshot().can_open_workbench);
    h.preparations[0].done(TaskHarness::entry("advanced"));
    REQUIRE(h.flow->artifact());
    CHECK(h.flow->snapshot().can_open_workbench);
    CHECK(h.flow->snapshot().progress == 100);
    const auto revision = h.flow->snapshot().revision;
    h.preparations[0].error("late failure");
    h.flow->poll();
    CHECK(h.flow->snapshot().revision == revision);
    CHECK(h.queries.size() == 1);
    CHECK(h.downloads.size() == 1);
}

TEST_CASE("A failed local download retries the same remote artifact without a new task", "[UnifiedWorkbench]")
{
    TaskHarness h; h.ready();
    h.downloads[0].error("disk failure");
    CHECK(h.flow->snapshot().stage == ModelGenerationUIStage::Failed);
    CHECK(h.flow->snapshot().can_retry_model);
    CHECK_FALSE(h.flow->snapshot().busy);
    h.flow->observe("advanced", "front.png"); h.flow->set_available(true);
    CHECK(h.downloads.size() == 1);
    h.flow->retry_local();
    REQUIRE(h.downloads.size() == 2);
    CHECK(h.downloads[1].id == "advanced");
    CHECK(h.queries.size() == 1);
    h.downloads[0].done(TaskHarness::artifact("advanced"));
    CHECK(h.preparations.empty());
    h.downloads[1].done(TaskHarness::artifact("advanced"));
    h.preparations[0].error("invalid geometry");
    CHECK_FALSE(h.flow->snapshot().model_ready);
    CHECK_FALSE(h.flow->artifact());
    CHECK(h.flow->snapshot().summary_text == "操作未能完成，请保留任务记录并查看诊断日志后重试。");
    CHECK(h.flow->snapshot().summary_text.find("invalid geometry") == std::string::npos);
}

TEST_CASE("Cancelling local acquisition rejects late download and parser completions", "[UnifiedWorkbench]")
{
    const bool parsing = GENERATE(false, true);
    TaskHarness h; h.ready();
    if (parsing) h.downloads[0].done(TaskHarness::artifact("advanced"));
    h.flow->stop_local();
    CHECK(h.flow->snapshot().stage == ModelGenerationUIStage::Stopped);
    CHECK(h.flow->snapshot().can_retry_model);
    const auto revision = h.flow->snapshot().revision;
    h.downloads[0].progress(100, 100);
    h.downloads[0].done(TaskHarness::artifact("advanced"));
    if (parsing) h.preparations[0].done(TaskHarness::entry("advanced"));
    CHECK(h.flow->snapshot().revision == revision);
    CHECK_FALSE(h.flow->artifact());
    h.flow->retry_local();
    CHECK(h.downloads.size() == 2);
    CHECK(h.queries.size() == 1);
}

TEST_CASE("New advanced workspaces ignore callbacks belonging to the previous job", "[UnifiedWorkbench]")
{
    const int stage = GENERATE(0, 1, 2);
    TaskHarness h;
    h.flow->observe("old", "old.png");
    if (stage > 0) h.queries[0].done(TaskHarness::status("old", "ready"));
    if (stage > 1) h.downloads[0].done(TaskHarness::artifact("old"));
    const auto old_session = h.flow->snapshot().model_generation_session;
    h.flow->reset(); h.flow->observe("new", "new.png");
    const auto revision = h.flow->snapshot().revision;
    h.queries[0].done(TaskHarness::status("old", "ready"));
    h.queries[0].error("old error");
    if (stage > 0) { h.downloads[0].done(TaskHarness::artifact("old")); h.downloads[0].error("old download"); }
    if (stage > 1) h.preparations[0].done(TaskHarness::entry("old"));
    CHECK(h.flow->snapshot().revision == revision);
    CHECK(h.flow->snapshot().job_id == "new");
    CHECK(h.flow->snapshot().model_generation_session > old_session);
    CHECK_FALSE(h.flow->artifact());
}

TEST_CASE("Destroyed advanced observers ignore all outstanding callbacks", "[UnifiedWorkbench]")
{
    TaskHarness h; h.ready(); h.downloads[0].done(TaskHarness::artifact("advanced"));
    const auto publications = h.states.size();
    h.flow.reset();
    h.queries[0].done(TaskHarness::status("advanced"));
    h.queries[0].error("old error");
    h.downloads[0].progress(100, 100);
    h.downloads[0].done(TaskHarness::artifact("advanced"));
    h.preparations[0].done(TaskHarness::entry("advanced"));
    h.preparations[0].error("late parser error");
    CHECK(h.states.size() == publications);
    CHECK(h.cancels >= 2);
}

TEST_CASE("Advanced observation reconnects and discards older service snapshots", "[UnifiedWorkbench]")
{
    TaskHarness h; h.flow->observe("advanced", {});
    h.queries[0].error("connection lost");
    CHECK(h.flow->snapshot().busy);
    CHECK(h.scheduled == 1200);
    h.flow->set_available(false); h.flow->poll();
    CHECK(h.queries.size() == 1);
    h.flow->set_available(true);
    REQUIRE(h.queries.size() == 2);
    h.queries[1].done(TaskHarness::status("advanced", "generating", 20));
    const auto progress = h.flow->snapshot().progress;
    h.flow->poll();
    auto stale = TaskHarness::status("advanced", "failed", 10);
    stale.provider_error_code = "stale";
    h.queries[2].done(stale);
    CHECK(h.flow->snapshot().stage == ModelGenerationUIStage::GeneratingModel);
    CHECK(h.flow->snapshot().provider_error_code.empty());
    CHECK(h.flow->snapshot().progress == progress);
}

TEST_CASE("Stopped or failed advanced tasks only resume after the owning session explicitly restarts observation", "[UnifiedWorkbench]")
{
    const std::string terminal = GENERATE("stopped", "failed");
    TaskHarness h; h.flow->observe("advanced", {});
    auto status = TaskHarness::status("advanced", terminal);
    status.provider_error_code = "provider_error"; status.message = "safe diagnostic";
    h.queries[0].done(status);
    CHECK_FALSE(h.flow->snapshot().busy);
    CHECK(h.flow->snapshot().provider_error_code == "provider_error");
    h.flow->poll(); h.flow->retry_local(); h.flow->set_available(true);
    CHECK(h.queries.size() == 1);
    CHECK(h.downloads.empty());
    h.flow->observe("advanced", {}, true);
    REQUIRE(h.queries.size() == 2);
    CHECK(h.queries[1].id == "advanced");
    CHECK(h.flow->snapshot().busy);
}

TEST_CASE("Duplicate query callbacks cannot finish or release the next advanced status request", "[UnifiedWorkbench]")
{
    TaskHarness h; h.flow->observe("advanced", {});
    const auto first = h.queries[0];
    first.done(TaskHarness::status("advanced", "generating", 20));
    h.flow->poll();
    REQUIRE(h.queries.size() == 2);
    const auto revision = h.flow->snapshot().revision;
    first.error("duplicate connection error");
    first.done(TaskHarness::status("advanced", "failed", 99));
    CHECK(h.flow->snapshot().revision == revision);
    CHECK(h.flow->snapshot().stage == ModelGenerationUIStage::GeneratingModel);
    h.flow->poll();
    CHECK(h.queries.size() == 2);
    h.queries[1].done(TaskHarness::status("advanced", "ready", 30));
    REQUIRE(h.downloads.size() == 1);
    CHECK(h.downloads[0].id == "advanced");
}

TEST_CASE("Mismatched advanced responses never install or prepare another task", "[UnifiedWorkbench]")
{
    TaskHarness h; h.flow->observe("advanced", {});
    h.queries[0].done(TaskHarness::status("other", "ready"));
    CHECK(h.downloads.empty());
    h.flow->poll();
    h.queries[1].done(TaskHarness::status("advanced", "ready"));
    h.downloads[0].done(TaskHarness::artifact("other"));
    CHECK(h.preparations.empty());
    CHECK_FALSE(h.flow->snapshot().model_ready);
    CHECK(h.flow->snapshot().stage == ModelGenerationUIStage::Failed);
}

TEST_CASE("Advanced observation rejects a smart task even when its job identity matches", "[UnifiedWorkbench]")
{
    TaskHarness h; h.flow->observe("advanced", {});
    auto status = TaskHarness::status("advanced", "ready");
    status.creation_kind = "smart";
    h.queries[0].done(status);
    CHECK(h.downloads.empty());
    CHECK_FALSE(h.flow->snapshot().model_ready);
    CHECK(h.flow->snapshot().status_text == "正在重新连接当前任务");
    h.flow->poll();
    REQUIRE(h.queries.size() == 2);
    h.queries[1].done(TaskHarness::status("advanced", "ready"));
    CHECK(h.downloads.size() == 1);
}
