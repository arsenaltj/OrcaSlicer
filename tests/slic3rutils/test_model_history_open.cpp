#include "slic3r/GUI/AI/ModelGeneration/ModelHistoryPreview.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelLibraryRepository.hpp"
#include "slic3r/GUI/AI/ModelGeneration/WorkbenchModelInspection.hpp"
#include "slic3r/GUI/Redesign/RedesignModelRoute.hpp"
#include "../test_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <future>
#include <thread>

using namespace Slic3r;
using namespace Slic3r::GUI;
namespace WA = Slic3r::GUI::WorkbenchArtifacts;
namespace {
struct HistoryFixture {
    ScopedTemporaryDir temporary {"orca-history-open"};
    boost::filesystem::path root {temporary.path() / "models"};
    std::string old_data {Slic3r::data_dir()};
    HistoryFixture() { boost::filesystem::create_directories(root / "downloads"); Slic3r::set_data_dir(temporary.path().string()); }
    ~HistoryFixture() { Slic3r::set_data_dir(old_data); }
    boost::filesystem::path record(const ModelLibraryEntry& entry) const {
        return root / "downloads" / ("orcaslicer-ai-" + entry.job_id + ".json");
    }
    nlohmann::json read(const ModelLibraryEntry& entry) const {
        boost::filesystem::ifstream input(record(entry)); return nlohmann::json::parse(input);
    }
    void write(const ModelLibraryEntry& entry, const nlohmann::json& data) const {
        boost::filesystem::ofstream output(record(entry)); output << data.dump(); REQUIRE(output.good());
    }
    ModelLibraryEntry model(const std::string& id, const ModelLibraryEntry* source = nullptr,
                            bool repaired = true, bool certificate = true, int date = 1) {
        ModelLibraryEntry entry;
        entry.job_id = id; entry.model_path = root / "downloads" / ("orcaslicer-ai-" + id + ".obj");
        indexed_triangle_set mesh;
        mesh.vertices = {{0,0,0},{10,0,0},{0,10,0},{0,0,10}};
        mesh.indices = {{0,2,1},{0,1,3},{0,3,2},{1,2,3}};
        std::string error;
        REQUIRE(AI::write_model_artifact(entry.model_path, mesh, std::vector<RGBA>(4, {.2f,.6f,.8f,1.f}), error));
        nlohmann::json metadata {{"job_id",id},{"schema_version",4},{"source",source ? "local_finishing" : "local_import"},
            {"model_path",entry.model_path.lexically_relative(root).generic_string()},
            {"model_sha256",AI::model_artifact_sha256(entry.model_path)}, {"generated_at",date},
            {"triangle_count",4},{"finishing",{{"repair_mesh",repaired},{"repair_geometry_verified",repaired}}}};
        if (source) {
            metadata["source_model"] = source->model_path.lexically_relative(root).generic_string();
            metadata["source_sha256"] = AI::model_artifact_sha256(source->model_path);
        }
        write(entry, metadata);
        if (certificate && source) {
            ModelPreview3D::PreparedModel prepared;
            REQUIRE(ModelPreview3D::prepare_model(entry.model_path, prepared, error, {}, record(entry)));
            const auto check = inspect_workbench_model(entry.model_path);
            REQUIRE(workbench_topology_safe(check));
            const auto appearance = prepared.semantic_source ? prepared.semantic_source->content_id : prepared.artifact_sha256;
            metadata["workbench_check_certificate"] = WA::encode_workbench_check_certificate(check,
                WorkbenchCheckPurpose::SavedModelReadOnly, prepared.geometry_id, appearance,
                boost::nowide::narrow(record(entry).wstring()), metadata);
            write(entry, metadata);
        }
        return entry;
    }
    std::shared_ptr<ModelHistoryOpenRequest> open(const ModelLibraryEntry& selected) const {
        std::atomic<bool> canceled {false};
        const auto entries = ModelLibraryRepository::read(root, canceled);
        REQUIRE_FALSE(entries.empty());
        auto request = std::make_shared<ModelHistoryOpenRequest>();
        request->root = root; request->entry = selected; request->repairs = entries.front().repair_index;
        return request;
    }
    bool prepare(const std::shared_ptr<ModelHistoryOpenRequest>& request) const {
        auto prepared = std::make_shared<ModelPreview3D::PreparedModel>();
        auto record = std::make_shared<ModelHistoryMetadata>();
        std::string error;
        const bool loaded = prepare_history_preview(prepared, request, record, error,
            [request] { return request->canceled.load(); });
        INFO(error);
        return loaded;
    }
};
}

TEST_CASE("An original history model resolves the newest verified repair without creating another result", "[ModelHistoryOpen]")
{
    HistoryFixture fixture;
    const auto original = fixture.model("original", nullptr, false, false);
    fixture.model("finish-old", &original, true, true, 2);
    const auto latest = fixture.model("finish-new", &original, true, true, 3);
    fixture.open(original); // Initialize the existing summary cache before counting outputs.
    const auto count = std::distance(boost::filesystem::directory_iterator(fixture.root / "downloads"),
                                     boost::filesystem::directory_iterator());
    const auto hash = AI::model_artifact_sha256(fixture.record(latest));
    for (int round = 0; round < 3; ++round) {
        auto request = fixture.open(original); // A fresh scan also exercises restart recovery.
        REQUIRE(fixture.prepare(request));
        CHECK(request->entry.model_path == latest.model_path);
        REQUIRE(request->check);
        CHECK(request->check->status == WorkbenchCheckStatus::Normal);
        CHECK(request->check->candidate_status == RepairCandidateStatus::None);
        CHECK(request->check->model_sha256 == AI::model_artifact_sha256(latest.model_path));
    }
    CHECK(AI::model_artifact_sha256(fixture.record(latest)) == hash);
    CHECK(std::distance(boost::filesystem::directory_iterator(fixture.root / "downloads"),
                        boost::filesystem::directory_iterator()) == count);
}

TEST_CASE("Task and downloaded copies share repairs only for the same job and unchanged bytes", "[ModelHistoryOpen]")
{
    HistoryFixture fixture;
    const auto original = fixture.model("original", nullptr, false, false);
    const auto repaired = fixture.model("finish-repaired", &original);
    const auto change = GENERATE(0, 1, 2, 3);
    auto selected = original;
    if (change == 3) selected = fixture.model("unrelated", nullptr, false, false);
    const auto task = fixture.root / selected.job_id / "model-vertex-color.obj";
    boost::filesystem::create_directories(task.parent_path());
    boost::filesystem::copy_file(selected.model_path, task);
    selected.model_path = task;
    if (change == 1 || change == 2) {
        boost::filesystem::ofstream changed(change == 1 ? task : original.model_path, std::ios::app);
        changed << "\n# changed source copy\n";
    }
    auto request = fixture.open(selected);
    REQUIRE(fixture.prepare(request));
    CHECK(request->entry.model_path == (change == 0 ? repaired.model_path : selected.model_path));
    CHECK(bool(request->check) == (change == 0));
}

TEST_CASE("An explicitly selected checked recolor keeps that exact version", "[ModelHistoryOpen]")
{
    HistoryFixture fixture;
    const auto original = fixture.model("original", nullptr, false, false);
    const auto repaired = fixture.model("finish-repaired", &original, true, true, 2);
    const auto colored = fixture.model("finish-colored", &repaired, false, true, 3);
    fixture.model("finish-later-repair", &colored, true, true, 4);
    auto request = fixture.open(colored);
    REQUIRE(fixture.prepare(request));
    CHECK(request->entry.model_path == colored.model_path);
    REQUIRE(request->check);
    CHECK(request->check->status == WorkbenchCheckStatus::Normal);
}

TEST_CASE("A newly published repair is reused before the sidebar finishes refreshing", "[ModelHistoryOpen]")
{
    HistoryFixture fixture;
    const auto original = fixture.model("original", nullptr, false, false);
    fixture.model("finish-older", &original, true, true, 2);
    auto request = fixture.open(original);
    const auto latest = fixture.model("finish-latest", &original, true, true, 3);
    REQUIRE_FALSE(request->repairs->current());
    REQUIRE(fixture.prepare(request));
    CHECK(request->entry.model_path == latest.model_path);
    REQUIRE(request->check);
}

TEST_CASE("Invalid newest repairs fall back to an older verified publication", "[ModelHistoryOpen]")
{
    HistoryFixture fixture;
    const auto original = fixture.model("original", nullptr, false, false);
    const auto older = fixture.model("finish-older", &original, true, true, 2);
    const auto latest = fixture.model("finish-latest", &original, true, true, 3);
    const auto change = GENERATE(0, 1, 2, 3, 4);
    auto metadata = fixture.read(latest);
    if (change == 0) metadata["workbench_check_certificate"]["geometry_id"] = "changed";
    if (change == 1) metadata["workbench_check_certificate"]["appearance_id"] = "changed";
    if (change == 2) metadata["finishing"]["repair_geometry_verified"] = false;
    if (change == 3) metadata["transaction_status"] = "cancelled";
    if (change == 4) metadata["source_sha256"] = std::string(64,'a');
    fixture.write(latest, metadata);
    auto request = fixture.open(original);
    REQUIRE(fixture.prepare(request));
    CHECK(request->entry.model_path == older.model_path);
    REQUIRE(request->check);
}

TEST_CASE("Changed sources records and models cannot borrow a repair certificate", "[ModelHistoryOpen]")
{
    HistoryFixture fixture;
    const auto original = fixture.model("original", nullptr, false, false);
    const auto repaired = fixture.model("finish-repaired", &original);
    auto request = fixture.open(original);
    const auto change = GENERATE(0, 1, 2);
    if (change == 0 || change == 1) {
        boost::filesystem::ofstream output(change == 0 ? original.model_path : repaired.model_path, std::ios::app);
        output << "\n# changed after the scan\n";
    } else {
        auto metadata = fixture.read(repaired);
        metadata["workbench_check_certificate"]["faces"] = 99;
        fixture.write(repaired, metadata);
    }
    REQUIRE(fixture.prepare(request));
    CHECK(request->entry.model_path == original.model_path);
    CHECK_FALSE(request->check);
}

TEST_CASE("Source chains require every intermediate link and reject cycles", "[ModelHistoryOpen]")
{
    HistoryFixture fixture;
    const auto original = fixture.model("original", nullptr, false, false);
    const auto middle = fixture.model("finish-middle", &original, false, false, 2);
    const auto repaired = fixture.model("finish-repaired", &middle, true, true, 3);
    auto request = fixture.open(original);
    const auto change = GENERATE(0, 1, 2);
    if (change == 1) {
        boost::filesystem::ofstream output(middle.model_path, std::ios::app); output << "\n# changed\n";
    } else if (change == 2) {
        auto metadata = fixture.read(middle);
        metadata["source_model"] = repaired.model_path.lexically_relative(fixture.root).generic_string();
        metadata["source_sha256"] = AI::model_artifact_sha256(repaired.model_path);
        fixture.write(middle, metadata);
    }
    REQUIRE(fixture.prepare(request));
    CHECK(request->entry.model_path == (change == 0 ? repaired.model_path : original.model_path));
    CHECK(bool(request->check) == (change == 0));
}

TEST_CASE("Canceled history preparation never produces an installable replacement", "[ModelHistoryOpen]")
{
    HistoryFixture fixture;
    const auto original = fixture.model("original", nullptr, false, false);
    fixture.model("finish-repaired", &original);
    auto request = fixture.open(original);
    request->canceled = true;
    CHECK_FALSE(fixture.prepare(request));
    CHECK_FALSE(request->check);
    CHECK_FALSE(request->installed);
    CHECK(request->entry.model_path == original.model_path);
}

TEST_CASE("Native publication flags only propose a candidate and cannot authorize an ordinary certificate", "[ModelHistoryOpen]")
{
    HistoryFixture fixture;
    const auto original = fixture.model("original", nullptr, false, false);
    const auto repaired = fixture.model("finish-native", &original);
    auto metadata = fixture.read(repaired);
    metadata["source"] = "native_workbench_partition";
    metadata["schema_version"] = 6;
    metadata["schema"] = "orca.native-workbench-partition-commit/v1";
    metadata["transaction_status"] = "committed";
    metadata["model_path"] = repaired.model_path.generic_string();
    metadata["source_model"] = original.model_path.generic_string();
    metadata.erase("job_id"); // Actual native publications use their record name as identity.
    fixture.write(repaired, metadata);
    auto request = fixture.open(original);
    const auto candidates = request->repairs->candidates(original.model_path);
    REQUIRE(candidates.size() == 1);
    CHECK(request->repairs->current_chain(candidates.front(), original.model_path));
    REQUIRE(fixture.prepare(request));
    CHECK(request->entry.model_path == original.model_path);
    CHECK_FALSE(request->check);
    metadata["transaction_status"] = "failed";
    fixture.write(repaired, metadata);
    CHECK_FALSE(request->repairs->current_chain(candidates.front(), original.model_path));
}

TEST_CASE("Copied history reuses unchanged publication evidence and rejects a changed original record", "[ModelHistoryOpen]")
{
    HistoryFixture published;
    const auto original = published.model("original", nullptr, false, false);
    const auto repaired = published.model("finish-repaired", &original);
    HistoryFixture copy;
    for (const auto& file : boost::filesystem::directory_iterator(published.root / "downloads"))
        boost::filesystem::copy_file(file.path(), copy.root / "downloads" / file.path().filename());
    auto selected = original;
    selected.model_path = copy.root / original.model_path.lexically_relative(published.root);
    auto request = copy.open(selected);
    const bool changed = GENERATE(false, true);
    if (changed) {
        auto metadata = published.read(repaired);
        metadata["triangle_count"] = 99;
        published.write(repaired, metadata);
    }
    REQUIRE(copy.prepare(request));
    CHECK(request->entry.model_path == (changed ? selected.model_path :
        copy.root / repaired.model_path.lexically_relative(published.root)));
    CHECK(bool(request->check) == !changed);
}

TEST_CASE("Copied native records map absolute source paths only within their original publication root", "[ModelHistoryOpen]")
{
    HistoryFixture published;
    const auto original = published.model("original", nullptr, false, false);
    const auto repaired = published.model("finish-native", &original);
    auto metadata = published.read(repaired);
    metadata["source"] = "native_workbench_partition";
    metadata["schema"] = "orca.native-workbench-partition-commit/v1";
    metadata["transaction_status"] = "committed";
    metadata["model_path"] = repaired.model_path.generic_string();
    metadata["source_model"] = original.model_path.generic_string();
    metadata.erase("job_id");
    published.write(repaired, metadata);
    HistoryFixture copy;
    for (const auto& file : boost::filesystem::directory_iterator(published.root / "downloads"))
        boost::filesystem::copy_file(file.path(), copy.root / "downloads" / file.path().filename());
    auto selected = original;
    selected.model_path = copy.root / original.model_path.lexically_relative(published.root);
    auto request = copy.open(selected);
    const auto candidates = request->repairs->candidates(selected.model_path);
    REQUIRE(candidates.size() == 1);
    CHECK(candidates.front().model == copy.root / repaired.model_path.lexically_relative(published.root));
    CHECK(request->repairs->current_chain(candidates.front(), selected.model_path));
    // Mapping locates bytes; it cannot turn this deliberately ordinary proof
    // into a valid native publication certificate.
    REQUIRE(copy.prepare(request));
    CHECK_FALSE(request->check);
    CHECK(request->entry.model_path == selected.model_path);
}

TEST_CASE("History source resolution rejects escaping paths and mismatched publication identities", "[ModelHistoryOpen]")
{
    HistoryFixture fixture;
    const auto original = fixture.model("original", nullptr, false, false);
    const auto repaired = fixture.model("finish-repaired", &original);
    auto metadata = fixture.read(repaired);
    const auto change = GENERATE(0, 1, 2);
    if (change == 0) metadata["source_model"] = "../outside.obj";
    if (change == 1) metadata["model_path"] = "downloads/unrelated.obj";
    if (change == 2) metadata["job_id"] = "different-job";
    fixture.write(repaired, metadata);
    auto request = fixture.open(original);
    REQUIRE(fixture.prepare(request));
    CHECK(request->entry.model_path == original.model_path);
    CHECK_FALSE(request->check);
}

TEST_CASE("History presentation owns routing independently of check and edit eligibility", "[ModelHistoryOpen][UiRedesign]")
{
    ModelGenerationUIState current, next;
    current.revision = 1; current.model_generation_session = 3;
    next.revision = 2; next.model_generation_session = 4;
    next.model_generation_context = true; next.model_asset_id = "history-model";
    for (const auto stage : {ModelGenerationUIStage::LoadingModel, ModelGenerationUIStage::ModelReady}) {
        next.stage = stage;
        CHECK(model_generation_route_action(next, current, false, 0, {}, true) == RedesignModelRouteAction::Refresh);
        CHECK(model_generation_route_action(next, current, false, 0, {}) == RedesignModelRouteAction::Model);
    }
    next.revision = 0;
    CHECK(model_generation_route_action(next, current, false, 0, {}, true) == RedesignModelRouteAction::Ignore);
    next.revision = 2; next.model_generation_context = false; next.job_id = "image";
    CHECK(model_generation_route_action(next, current, false, 0, {}) == RedesignModelRouteAction::Image);
}

TEST_CASE("History cancellation retains its request until the running parser drains", "[ModelHistoryOpen]")
{
    HistoryFixture fixture;
    const auto model = fixture.model("original", nullptr, false, false);
    auto request = fixture.open(model);
    request->worker_pending = true;
    std::promise<void> reading, release;
    auto released = release.get_future().share();
    auto started = reading.get_future();
    request->progress = [&](ModelHistoryOpenEvent::Stage stage) {
        if (stage == ModelHistoryOpenEvent::Stage::Reading) {
            reading.set_value();
            released.wait();
        }
    };
    bool loaded = true;
    std::thread parser([&] {
        auto prepared = std::make_shared<ModelPreview3D::PreparedModel>();
        auto record = std::make_shared<ModelHistoryMetadata>();
        std::string error;
        loaded = prepare_history_preview(prepared, request, record, error, [request] { return request->canceled.load(); });
    });
    const auto began = started.wait_for(std::chrono::seconds(5));
    int restored = 0;
    const bool deferred = request->cancel_after_drain([&] { ++restored; });
    const bool duplicate_deferred = request->cancel_after_drain([&] { restored += 100; });
    const int before_drain = restored;
    const bool held = request->worker_pending;
    release.set_value();
    parser.join();
    CHECK(began == std::future_status::ready);
    CHECK(deferred);
    CHECK(duplicate_deferred);
    CHECK(held);
    CHECK(before_drain == 0);
    CHECK_FALSE(loaded);
    CHECK(request->worker_finished());
    CHECK(restored == 1);
    CHECK_FALSE(request->worker_pending);
    CHECK_FALSE(request->worker_finished());
    CHECK(restored == 1);
    CHECK_FALSE(request->installed);
}

TEST_CASE("Canceling a queued history request performs no preparation and releases on its deferred completion", "[ModelHistoryOpen]")
{
    HistoryFixture fixture;
    const auto model = fixture.model("original", nullptr, false, false);
    auto request = fixture.open(model);
    request->worker_pending = true;
    int restored = 0, stages = 0;
    request->progress = [&](ModelHistoryOpenEvent::Stage) { ++stages; };
    REQUIRE(request->cancel_after_drain([&] { ++restored; }));
    CHECK_FALSE(fixture.prepare(request));
    CHECK(stages == 0);
    CHECK(restored == 0);
    CHECK(request->worker_finished());
    CHECK(restored == 1);
    CHECK_FALSE(request->cancel_after_drain([&] { ++restored; }));
    CHECK(restored == 1);
}

TEST_CASE("History progress keeps the selected identity while a verified repaired version is resolved", "[ModelHistoryOpen]")
{
    HistoryFixture fixture;
    const auto original = fixture.model("original", nullptr, false, false);
    const auto repaired = fixture.model("finish-repaired", &original);
    auto request = fixture.open(original);
    request->generation = 7;
    request->selected_asset = original.job_id;
    std::vector<ModelHistoryOpenEvent> stages;
    request->progress = [&](ModelHistoryOpenEvent::Stage stage) {
        stages.push_back({request->generation, request->selected_asset, ModelHistoryOpenEvent::Phase::Progress, stage});
    };
    REQUIRE(fixture.prepare(request));
    CHECK(request->entry.job_id == repaired.job_id);
    REQUIRE(request->check);
    REQUIRE_FALSE(stages.empty());
    bool read = false, validated_after_read = false;
    for (const auto& event : stages) {
        CHECK(event.matches(7, original.job_id));
        CHECK_FALSE(event.matches(6, original.job_id));
        CHECK_FALSE(event.matches(7, repaired.job_id));
        CHECK_FALSE(event.matches(0, original.job_id));
        if (event.stage == ModelHistoryOpenEvent::Stage::Reading) read = true;
        if (read && event.stage == ModelHistoryOpenEvent::Stage::Validating) validated_after_read = true;
    }
    CHECK(read);
    CHECK(validated_after_read);
    const ModelHistoryOpenEvent loaded {7, original.job_id, ModelHistoryOpenEvent::Phase::Loaded,
        ModelHistoryOpenEvent::Stage::Displaying, request->entry.job_id};
    CHECK(loaded.matches(7, original.job_id));
    CHECK_FALSE(loaded.matches(8, original.job_id));
    CHECK(loaded.resolved_asset == repaired.job_id);
}

TEST_CASE("Real history reuse restores a certificate without invoking model repair", "[.ModelHistoryOpenQA]")
{
    const auto model_name = boost::nowide::getenv("ORCASLICER_MODEL_ARTIFACT_FIXTURE");
    REQUIRE(model_name);
    const boost::filesystem::path model(model_name);
    const auto root = model.parent_path().parent_path();
    struct RestoreDataDir {
        std::string old {Slic3r::data_dir()};
        ~RestoreDataDir() { Slic3r::set_data_dir(old); }
    } restore;
    Slic3r::set_data_dir(root.parent_path().string());
    std::atomic<bool> canceled {false};
    const auto entries = ModelLibraryRepository::read(root, canceled);
    const auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) { return entry.model_path == model; });
    REQUIRE(found != entries.end());
    auto request = std::make_shared<ModelHistoryOpenRequest>();
    request->root = root; request->entry = *found; request->repairs = found->repair_index;
    const auto original_hash = AI::model_artifact_sha256(model);
    auto prepared = std::make_shared<ModelPreview3D::PreparedModel>();
    auto metadata = std::make_shared<ModelHistoryMetadata>();
    std::string error;
    REQUIRE(prepare_history_preview(prepared, request, metadata, error, [] { return false; }));
    REQUIRE(request->check);
    CHECK(request->check->status == WorkbenchCheckStatus::Normal);
    CHECK(request->check->candidate_status == RepairCandidateStatus::None);
    CHECK(request->check->model_sha256 == prepared->artifact_sha256);
    CHECK(AI::model_artifact_sha256(model) == original_hash);
    if (const auto expected = boost::nowide::getenv("ORCASLICER_HISTORY_EXPECTED_MODEL"))
        CHECK(request->entry.model_path == boost::filesystem::path(expected));
    if (const auto report = boost::nowide::getenv("ORCASLICER_HISTORY_OPEN_REPORT")) {
        boost::filesystem::ofstream output(report);
        output << nlohmann::json {{"selected", model.generic_string()},
            {"resolved", request->entry.model_path.generic_string()},
            {"certificate_restored", true}, {"model_sha256", request->check->model_sha256},
            {"record_sha256", request->record_hash}, {"repair_candidate", false},
            {"faces", request->check->before_faces}}.dump(2);
    }
}
