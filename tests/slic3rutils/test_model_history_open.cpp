#include "slic3r/GUI/AI/ModelGeneration/ModelHistoryPreview.hpp"
#include "slic3r/GUI/AI/ModelGeneration/WorkbenchOpenRequest.hpp"
#include "slic3r/GUI/AI/ModelGeneration/VerifiedModelFiles.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelLibraryRepository.hpp"
#include "slic3r/GUI/AI/ModelGeneration/WorkbenchModelInspection.hpp"
#include "slic3r/GUI/Redesign/RedesignModelRoute.hpp"
#include "../test_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <future>
#include <thread>
#include <wx/init.h>
#include "slic3r/GUI/Presentation/UiRuntime.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;

TEST_CASE("A prepared file proof rejects a changed optional companion and stays failed after a bad digest", "[ModelHistoryOpen][UiLifecycle]")
{
    ScopedTemporaryDir temporary {"orca-prepared-file-proof"};
    const auto model = temporary.path() / "model.obj";
    const auto record = temporary.path() / "model.json";
    { boost::filesystem::ofstream output(model); output << "verified model bytes"; }
    const auto hash = AI::model_artifact_sha256(model);
    VerifiedModelFiles proof;
    REQUIRE(proof.verify(model, hash));
    REQUIRE(proof.verify(record, {}, true));
    REQUIRE(proof.current());
    { boost::filesystem::ofstream output(record); output << "{}"; }
    REQUIRE_FALSE(proof.current());

    VerifiedModelFiles rejected;
    REQUIRE_FALSE(rejected.verify(model, std::string(64, '0')));
    REQUIRE_FALSE(rejected.verify(record, AI::model_artifact_sha256(record)));
    REQUIRE_FALSE(rejected.current());
}

#ifdef _WIN32
TEST_CASE("A prepared model read lease blocks writes and replacement until GUI installation releases it", "[ModelHistoryOpen][UiLifecycle]")
{
    ScopedTemporaryDir temporary {"orca-prepared-file-lease"};
    const auto model = temporary.path() / "模型.obj";
    const auto other = temporary.path() / "replacement.obj";
    { boost::filesystem::ofstream output(model); output << "verified model"; }
    { boost::filesystem::ofstream output(other); output << "changed model"; }
    const auto hash = AI::model_artifact_sha256(model);
    {
        VerifiedModelFiles proof;
        REQUIRE(proof.verify(model, hash));
        const HANDLE writable = ::CreateFileW(model.c_str(), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        const bool write_allowed = writable != INVALID_HANDLE_VALUE;
        if (write_allowed) ::CloseHandle(writable);
        REQUIRE_FALSE(write_allowed);
        REQUIRE_FALSE(::MoveFileExW(other.c_str(), model.c_str(), MOVEFILE_REPLACE_EXISTING));
        REQUIRE(proof.current());
    }
    REQUIRE(::MoveFileExW(other.c_str(), model.c_str(), MOVEFILE_REPLACE_EXISTING));
    REQUIRE(AI::model_artifact_sha256(model) != hash);
}
#else
TEST_CASE("An unlocked prepared file rejects changed bytes with the same size and modification time", "[ModelHistoryOpen][UiLifecycle]")
{
    ScopedTemporaryDir temporary {"orca-prepared-file-digest"};
    const auto model = temporary.path() / "model.obj";
    { boost::filesystem::ofstream output(model); output << "verified model bytes"; }
    const auto hash = AI::model_artifact_sha256(model);
    const auto timestamp = boost::filesystem::last_write_time(model);
    const auto size = boost::filesystem::file_size(model);
    VerifiedModelFiles proof;
    REQUIRE(proof.verify(model, hash));
    { boost::filesystem::ofstream output(model, std::ios::trunc); output << "tampered model bytes"; }
    boost::filesystem::last_write_time(model, timestamp);
    REQUIRE(boost::filesystem::file_size(model) == size);
    REQUIRE_FALSE(proof.current());
}
#endif

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

TEST_CASE("A frozen preview keeps its geometry and surface attributes across duplicate completion", "[ModelHistoryOpen][ModelPreviewLoad][PreviewFreeze]")
{
    wxInitializer init;
    REQUIRE(init.IsOk());
    HistoryFixture fixture;
    const auto entry = fixture.model("freeze-once", nullptr, false, false);
    const bool shared = GENERATE(false, true);
    ModelPreview3D::PreparedModel prepared;
    std::string error;
    REQUIRE(ModelPreview3D::prepare_model(entry.model_path, prepared, error));
    prepared.prepare_render_geometry({}, shared);
    const auto vertices = prepared.render_data().vertices;
    const auto indices = prepared.render_data().indices;
    const auto colors = prepared.surface_colors;
    prepared.native_workbench_checkpoint = std::make_shared<NativeWorkbenchCheckpoint>();
    REQUIRE_FALSE(vertices.empty());
    REQUIRE_FALSE(indices.empty());
    REQUIRE_NOTHROW(prepared.prepare_render_geometry({}, shared));
    CHECK(prepared.render_data().vertices == vertices);
    CHECK(prepared.render_data().indices == indices);
    CHECK(prepared.surface_colors == colors);
}

TEST_CASE("Cancelled or failed preview freezing never exposes an installable partial payload", "[ModelHistoryOpen][ModelPreviewLoad][PreviewFreeze]")
{
    wxInitializer init;REQUIRE(init.IsOk());HistoryFixture fixture;
    const auto entry=fixture.model("cancel-freeze",nullptr,false,false);
    ModelPreview3D::PreparedModel canceled;std::string error;
    REQUIRE(ModelPreview3D::prepare_model(entry.model_path,canceled,error));
    REQUIRE_THROWS(canceled.prepare_render_geometry([] {return true;}));
    CHECK_FALSE(canceled.installable());CHECK_FALSE(canceled.render_geometry);
    CHECK(canceled.render_preparation==ModelPreview3D::PreparedModel::RenderPreparation::Canceled);
    REQUIRE_NOTHROW(canceled.prepare_render_geometry());
    CHECK(canceled.installable());
    REQUIRE(canceled.render_geometry);
    ModelPreview3D::PreparedModel failed;
    REQUIRE_THROWS(failed.prepare_render_geometry());
    CHECK_FALSE(failed.installable());CHECK_FALSE(failed.render_geometry);
    CHECK(failed.render_preparation==ModelPreview3D::PreparedModel::RenderPreparation::Failed);
}

TEST_CASE("Cancellation after moving a preview resource restores every input for an explicit retry", "[ModelHistoryOpen][ModelPreviewLoad][PreviewFreeze]")
{
    wxInitializer init; REQUIRE(init.IsOk()); HistoryFixture fixture;
    const auto entry = fixture.model("cancel-after-move", nullptr, false, false);
    ModelPreview3D::PreparedModel prepared; std::string error;
    REQUIRE(ModelPreview3D::prepare_model(entry.model_path, prepared, error));
    const auto original = prepared.geometry;
    prepared.current_appearance_geometry = original;
    prepared.direct_original_geometry = original;
    const bool after_current = GENERATE(false, true);
    REQUIRE_THROWS(prepared.prepare_render_geometry([&] {
        return after_current ? prepared.current_appearance_geometry.is_empty() : prepared.geometry.is_empty();
    }));
    CHECK_FALSE(prepared.installable());
    CHECK_FALSE(prepared.render_geometry);
    CHECK_FALSE(prepared.current_appearance_render_geometry);
    CHECK(prepared.geometry.vertices == original.vertices);
    CHECK(prepared.geometry.indices == original.indices);
    CHECK(prepared.current_appearance_geometry.vertices == original.vertices);
    CHECK(prepared.direct_original_geometry.indices == original.indices);
    REQUIRE_NOTHROW(prepared.prepare_render_geometry());
    CHECK(prepared.installable());
    REQUIRE(prepared.render_geometry);
    REQUIRE(prepared.current_appearance_render_geometry);
    REQUIRE(prepared.direct_original_render_geometry);
    CHECK(prepared.render_data().vertices == original.vertices);
}

TEST_CASE("Async preview handoff returns the resolved renderable object instead of the empty initial allocation", "[ModelHistoryOpen][ModelPreviewLoad][UiLifecycle]")
{
    wxInitializer init;
    REQUIRE(init.IsOk());
    HistoryFixture fixture;
    const auto original=fixture.model("async-original",nullptr,false,false);
    const int route=GENERATE(0,1,2); // Ordinary history, retained rollback, repaired re-selection.
    const auto repaired=route ? fixture.model("async-repaired",&original,true,true,2) : original;
    const auto selected=route==1 ? repaired : original;
    auto request=fixture.open(selected);
    if (route==1) request->restoring=true;
    auto metadata=std::make_shared<ModelHistoryMetadata>();
    const auto empty=std::make_shared<ModelPreview3D::PreparedModel>();
    REQUIRE(empty->render_data().is_empty());
    std::promise<std::function<void()>> delivery;
    auto queued=delivery.get_future();
    Presentation::TaskRunner tasks([&](auto callback) { delivery.set_value(std::move(callback)); });
    Presentation::TaskScope scope;
    const auto context=scope.begin(selected.model_path.string());
    std::optional<ModelPreviewLoadResult> received;
    unsigned completions=0;
    const auto gui_thread=std::this_thread::get_id();
    REQUIRE(tasks.submit(context,[&](const auto&) {
        return prepare_model_preview_load(selected.model_path,fixture.record(selected),metadata,request,
            [] { return false; },false);
    },[&](Presentation::TaskResult<ModelPreviewLoadResult> outcome) {
        REQUIRE(std::this_thread::get_id()==gui_thread);
        REQUIRE(scope.matches(context));
        REQUIRE_FALSE(outcome.error);
        REQUIRE_FALSE(outcome.canceled);
        received=std::move(outcome.value); ++completions;
    }));
    REQUIRE(queued.wait_for(std::chrono::seconds(5))==std::future_status::ready);
    queued.get()(); // The real TaskRunner hands its final typed object to the GUI consumer.
    REQUIRE(tasks.idle());
    REQUIRE(completions==1);
    REQUIRE(received);
    auto& result=*received;
    INFO(result.error);
    REQUIRE(result.error.empty());
    REQUIRE(result.prepared);
    REQUIRE_FALSE(result.prepared->render_data().is_empty());
    REQUIRE(result.prepared->render_geometry);
    REQUIRE(result.prepared->path==repaired.model_path);
    REQUIRE(request->verified_files);
    REQUIRE(request->verified_files->current());
    REQUIRE(request->model_hash==result.prepared->artifact_sha256);
    REQUIRE(result.prepared->triangles==4);
    if (route) {
        REQUIRE(request->check);
        REQUIRE(request->check->status==WorkbenchCheckStatus::Normal);
    } else REQUIRE_FALSE(request->check);
    REQUIRE(empty->render_data().is_empty());
}

TEST_CASE("A queued model result cannot install after cancellation or a newer request generation", "[ModelHistoryOpen][ModelPreviewLoad][UiLifecycle]")
{
    wxInitializer init;
    REQUIRE(init.IsOk());
    HistoryFixture fixture;
    const auto original=fixture.model("async-superseded",nullptr,false,false);
    auto request=fixture.open(original);
    auto metadata=std::make_shared<ModelHistoryMetadata>();
    std::promise<std::function<void()>> delivery;
    auto queued=delivery.get_future();
    Presentation::TaskRunner tasks([&](auto callback) { delivery.set_value(std::move(callback)); });
    Presentation::TaskScope scope;
    const auto context=scope.begin(original.model_path.string());
    unsigned installs=0, completions=0;
    REQUIRE(tasks.submit(context,[&](const auto&) {
        return prepare_model_preview_load(original.model_path,fixture.record(original),metadata,request,
            [] { return false; },false);
    },[&](Presentation::TaskResult<ModelPreviewLoadResult> outcome) {
        ++completions;
        REQUIRE(outcome.value);
        REQUIRE_FALSE(outcome.value->prepared->render_data().is_empty());
        if (scope.matches(context) && !outcome.canceled) ++installs;
    }));
    REQUIRE(queued.wait_for(std::chrono::seconds(5))==std::future_status::ready);
    if (GENERATE(false,true)) scope.begin("newer-model");
    else scope.cancel();
    queued.get()();
    REQUIRE(tasks.idle());
    REQUIRE(completions==1);
    REQUIRE(installs==0);
}

TEST_CASE("Async preview handoff preserves canceled and invalid input failures without an installable frame", "[ModelHistoryOpen][ModelPreviewLoad][UiLifecycle]")
{
    HistoryFixture fixture;
    const auto original=fixture.model("async-failure",nullptr,false,false);
    auto request=fixture.open(original);
    const bool cancel=GENERATE(false,true);
    if (!cancel) request->entry.model_path=fixture.root/"missing.obj";
    auto metadata=std::make_shared<ModelHistoryMetadata>();
    const auto result=prepare_model_preview_load(request->entry.model_path,fixture.record(original),metadata,request,
        [cancel] { return cancel; },false);
    REQUIRE_FALSE(result.error.empty());
    REQUIRE(result.prepared);
    REQUIRE(result.prepared->render_data().is_empty());
    REQUIRE_FALSE(request->verified_files);
}

// Opt-in copies only: this covers the production CPU -> GUI data delivery,
// not GPU upload, a presented main window or visual acceptance.
TEST_CASE("A real history copy hands its final frozen preview and evidence to the GUI consumer", "[.ModelPreviewHandoffQA]")
{
    const auto name=boost::nowide::getenv("ORCASLICER_MODEL_ARTIFACT_FIXTURE");
    if (!name) SKIP("Set an immutable local model copy for the asynchronous handoff probe.");
    wxInitializer init;
    REQUIRE(init.IsOk());
    const boost::filesystem::path source(name);
    const auto root=source.parent_path().parent_path();
    struct DataScope {
        std::string previous = Slic3r::data_dir();
        ~DataScope() { Slic3r::set_data_dir(previous); }
    } data;
    Slic3r::set_data_dir(root.parent_path().string());
    const auto expected_name = boost::nowide::getenv("ORCASLICER_HISTORY_EXPECTED_MODEL");
    const boost::filesystem::path expected = expected_name ? boost::filesystem::path(expected_name) : source;
    const auto expected_hash = AI::model_artifact_sha256(expected);
    const auto record=AI::beauty_metadata_path(source,root);
    nlohmann::json fields;
    { boost::filesystem::ifstream input(record); input >> fields; }
    const auto source_hash=AI::model_artifact_sha256(source), record_hash=AI::model_artifact_sha256(record);
    auto request=std::make_shared<ModelHistoryOpenRequest>();
    request->root=root; request->entry.model_path=source;
    request->entry.job_id=fields.value("job_id", ModelGenerationPresentation::download_job_id(source));
    auto metadata=std::make_shared<ModelHistoryMetadata>();
    std::promise<std::function<void()>> delivery;
    auto queued=delivery.get_future();
    Presentation::TaskRunner tasks([&](auto callback) { delivery.set_value(std::move(callback)); });
    Presentation::TaskScope scope;
    const auto context=scope.begin(request->entry.job_id);
    std::optional<ModelPreviewLoadResult> received;
    REQUIRE(tasks.submit(context,[&](const auto&) {
        return prepare_model_preview_load(source,record,metadata,request,[] { return false; },false);
    },[&](Presentation::TaskResult<ModelPreviewLoadResult> outcome) {
        REQUIRE(scope.matches(context));
        REQUIRE_FALSE(outcome.error);
        REQUIRE_FALSE(outcome.canceled);
        received=std::move(outcome.value);
    },Presentation::TaskPriority::Visible));
    REQUIRE(queued.wait_for(std::chrono::seconds(60))==std::future_status::ready);
    queued.get()();
    REQUIRE(tasks.idle());
    REQUIRE(received);
    INFO(received->error);
    REQUIRE(received->error.empty());
    REQUIRE(received->prepared);
    REQUIRE(received->prepared->render_geometry);
    REQUIRE_FALSE(received->prepared->render_data().is_empty());
    REQUIRE(received->prepared->triangles>0);
    REQUIRE(request->verified_files);
    REQUIRE(request->verified_files->current());
    REQUIRE(request->model_hash==expected_hash);
    REQUIRE(received->prepared->path==expected);
    REQUIRE(AI::model_artifact_sha256(source)==source_hash);
    REQUIRE(AI::model_artifact_sha256(record)==record_hash);
    if (const auto report=boost::nowide::getenv("ORCASLICER_HISTORY_EVIDENCE_REPORT")) {
        boost::filesystem::ofstream output(report);
        output << nlohmann::json{{"scope","cpu_to_gui_handoff"},{"source_sha256",source_hash},
            {"metadata_sha256",record_hash},{"triangles",received->prepared->triangles},
            {"render_geometry",true},{"file_proof",true},{"check_restored",bool(request->check)},
            {"gpu_upload",false},{"main_window",false}}.dump(2);
    }
}

// Uses immutable copies of a source and its already committed publication.
// This exercises the producer's retry resolver, without invoking repair or GL.
TEST_CASE("A published workbench retry returns its verified frozen carrier without repairing again", "[.PublishedWorkbenchRetryQA]")
{
    const auto name = boost::nowide::getenv("ORCASLICER_MODEL_ARTIFACT_FIXTURE");
    if (!name) SKIP("Set an immutable source copy.");
    // nowide::getenv reuses thread-local storage on Windows. Own this path
    // before reading the next variable, or both inputs become the carrier.
    const boost::filesystem::path source(name);
    const auto expected_name = boost::nowide::getenv("ORCASLICER_HISTORY_EXPECTED_MODEL");
    if (!expected_name) SKIP("Set an immutable published carrier copy.");
    const boost::filesystem::path expected(expected_name);
    wxInitializer init;
    REQUIRE(init.IsOk());
    REQUIRE(source != expected);
    const auto root = source.parent_path().parent_path();
    struct DataScope {
        std::string previous = Slic3r::data_dir();
        ~DataScope() { Slic3r::set_data_dir(previous); }
    } data;
    Slic3r::set_data_dir(root.parent_path().string());
    const auto source_hash = AI::model_artifact_sha256(source);
    const auto carrier_hash = AI::model_artifact_sha256(expected);
    const auto record = AI::beauty_metadata_path(expected, root);
    const auto record_hash = AI::model_artifact_sha256(record);
    const auto index_entries = ModelLibraryRepository::read(root, std::atomic<bool>{false});
    REQUIRE_FALSE(index_entries.empty());
    nlohmann::json rounds = nlohmann::json::array();
    for (const bool shared : {false, true}) {
        const auto started = std::chrono::steady_clock::now();
        auto request = std::make_shared<ModelHistoryOpenRequest>();
        request->root = root;
        request->entry.model_path = source;
        request->entry.job_id = ModelGenerationPresentation::download_job_id(source);
        request->expected_model_sha256 = source_hash;
        const auto result = prepare_published_workbench_retry(request, [] { return false; }, shared);
        nlohmann::json diagnostic {{"resolved", request->entry.model_path.generic_string()},
            {"check", bool(request->check)}, {"shared", shared},
            {"candidates", index_entries.front().repair_index->candidates(source).size()},
            {"request_candidates", request->repairs ? request->repairs->candidates(source).size() : 0},
            {"expected_source", request->expected_model_sha256}};
        if (!result) {
            auto record = std::make_shared<ModelHistoryMetadata>();
            const auto probe = prepare_model_preview_load(source, {}, record, request, [] { return false; }, shared);
            diagnostic["probe"] = {{"error", probe.error}, {"stage", probe.stage}, {"check", bool(request->check)},
                {"checkpoint", bool(probe.prepared->native_workbench_checkpoint)},
                {"partition", bool(probe.prepared->native_portrait_partition)}};
        }
        INFO(diagnostic.dump());
        REQUIRE(result);
        INFO(result->error);
        REQUIRE(result->error.empty());
        REQUIRE(request->entry.model_path == expected);
        REQUIRE(request->check);
        REQUIRE(request->check->status == WorkbenchCheckStatus::Normal);
        REQUIRE(request->check->candidate_status == RepairCandidateStatus::None);
        REQUIRE(request->check->model_sha256 == carrier_hash);
        REQUIRE(request->verified_files);
        REQUIRE(request->verified_files->current());
        REQUIRE(result->prepared);
        auto& prepared = *result->prepared;
        INFO("native_checkpoint=" << bool(prepared.native_workbench_checkpoint));
        INFO("native_partition=" << bool(prepared.native_portrait_partition));
        REQUIRE(prepared.native_workbench_checkpoint);
        REQUIRE(prepared.native_portrait_partition);
        REQUIRE(prepared.native_portrait_partition->valid());
        REQUIRE(prepared.artifact_sha256 == carrier_hash);
        REQUIRE(prepared.render_preparation == ModelPreview3D::PreparedModel::RenderPreparation::Ready);
        REQUIRE(prepared.installable());
        const auto vertices = prepared.render_data().vertices.size();
        const auto indices = prepared.render_data().indices.size();
        REQUIRE(vertices > 0);
        REQUIRE(indices > 0);
        // The checkpoint is already attached. Duplicate completion must not
        // move an empty source geometry into the valid payload again.
        REQUIRE_NOTHROW(prepared.prepare_render_geometry({}, shared));
        CHECK(prepared.render_data().vertices.size() == vertices);
        CHECK(prepared.render_data().indices.size() == indices);
        rounds.push_back({{"shared_vertices", shared}, {"vertices", vertices}, {"indices", indices},
            {"milliseconds", std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count()}});
    }
    auto canceled = std::make_shared<ModelHistoryOpenRequest>();
    canceled->root = root; canceled->entry.model_path = source;
    CHECK_FALSE(prepare_published_workbench_retry(canceled, [] { return true; }, false));
    {
        // A missing presentation cache must report a preserved publication,
        // not fall through to another strict repair of the source.
        ScopedTemporaryDir no_cache {"orca-published-retry-no-cache"};
        Slic3r::set_data_dir(no_cache.string());
        auto request = std::make_shared<ModelHistoryOpenRequest>();
        request->root = root; request->entry.model_path = source;
        request->entry.job_id = ModelGenerationPresentation::download_job_id(source);
        request->expected_model_sha256 = source_hash;
        const auto result = prepare_published_workbench_retry(request, [] { return false; }, false);
        REQUIRE(request->check);
        REQUIRE(result);
        CHECK_FALSE(result->error.empty());
        Slic3r::set_data_dir(root.parent_path().string());
    }
    CHECK(AI::model_artifact_sha256(source) == source_hash);
    CHECK(AI::model_artifact_sha256(expected) == carrier_hash);
    CHECK(AI::model_artifact_sha256(record) == record_hash);
    if (const auto output = boost::nowide::getenv("ORCASLICER_HISTORY_EVIDENCE_REPORT")) {
        boost::filesystem::ofstream stream(output);
        stream << nlohmann::json{{"scope", "published_retry_cpu_payload"}, {"source_sha256", source_hash},
            {"carrier_sha256", carrier_hash}, {"record_sha256", record_hash}, {"rounds", rounds},
            {"repair_invoked", false}, {"gpu_upload", false}, {"main_window", false}}.dump(2);
    }
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

TEST_CASE("The application rejects another workbench before allocating editing state", "[SingleWorkbench]")
{
    REQUIRE(WorkbenchInstanceLease::live_instances() == 0);
    {
        WorkbenchInstanceLease application_owner;
        REQUIRE(WorkbenchInstanceLease::live_instances() == 1);
        CHECK_THROWS_AS(WorkbenchInstanceLease{}, std::logic_error);
        CHECK(WorkbenchInstanceLease::live_instances() == 1);
    }
    REQUIRE(WorkbenchInstanceLease::live_instances() == 0);
    WorkbenchInstanceLease next_application;
    CHECK(WorkbenchInstanceLease::live_instances() == 1);
}

TEST_CASE("Creator and history handoffs cannot mix an artifact with another asset", "[SingleWorkbench]")
{
    WorkbenchOpenRequest request;
    request.origin = GENERATE(WorkbenchOpenRequest::Origin::Smart, WorkbenchOpenRequest::Origin::Advanced,
        WorkbenchOpenRequest::Origin::Legacy, WorkbenchOpenRequest::Origin::History, WorkbenchOpenRequest::Origin::Library);
    request.entry.job_id = "actual-job";
    request.entry.model_path = "downloads/model.glb";
    request.artifact.job_id = request.entry.job_id;
    request.artifact.local_path = request.entry.model_path;
    CHECK(request.identifies_asset());
    SECTION("another model path") { request.artifact.local_path = "downloads/stale.glb"; }
    SECTION("another job identity") { request.artifact.job_id = "stale-job"; }
    SECTION("an empty asset") { request.entry.model_path.clear(); }
    CHECK_FALSE(request.identifies_asset());
}

TEST_CASE("A creator preview with changed source bytes cannot silently open an existing repair", "[SingleWorkbench][ModelHistoryOpen]")
{
    HistoryFixture fixture;
    const auto original = fixture.model("original", nullptr, false, false);
    fixture.model("repair", &original, true, true, 2);
    const auto metadata_before = fixture.read(original);
    auto request = fixture.open(original);
    request->expected_model_sha256 = std::string(64, '0');
    CHECK_FALSE(fixture.prepare(request));
    CHECK_FALSE(request->check.has_value());
    CHECK(request->entry.job_id == original.job_id);
    CHECK(fixture.read(original) == metadata_before);
}

TEST_CASE("An old creator handoff cannot install a different generation or result", "[SingleWorkbench]")
{
    WorkbenchOpenRequest request;
    request.origin = GENERATE(WorkbenchOpenRequest::Origin::Smart, WorkbenchOpenRequest::Origin::Advanced,
        WorkbenchOpenRequest::Origin::Legacy);
    request.entry.job_id = "current-result";
    request.entry.model_path = "downloads/model.glb";
    request.generation = 4;
    REQUIRE(request.matches_creator("current-result", 4));
    CHECK_FALSE(request.matches_creator("old-result", 4));
    CHECK_FALSE(request.matches_creator("current-result", 5));
    CHECK_FALSE(request.matches_creator({}, 4));
}

TEST_CASE("A matching creator preview reuses a verified repair through the history consumer", "[SingleWorkbench][ModelHistoryOpen]")
{
    HistoryFixture fixture;
    const auto original = fixture.model("original", nullptr, false, false);
    const auto repair = fixture.model("repair", &original, true, true, 2);
    const auto metadata_before = fixture.read(repair);
    for (int repeat = 0; repeat < 3; ++repeat) {
        auto request = fixture.open(original);
        request->expected_model_sha256 = AI::model_artifact_sha256(original.model_path);
        REQUIRE(fixture.prepare(request));
        CHECK(request->entry.job_id == repair.job_id);
        REQUIRE(request->check);
        CHECK(request->check->status == WorkbenchCheckStatus::Normal);
    }
    CHECK(fixture.read(repair) == metadata_before);
}

TEST_CASE("A downloaded creator handoff discovers repairs without a sidebar entry", "[SingleWorkbench][ModelHistoryOpen]")
{
    HistoryFixture fixture;
    const auto original = fixture.model("original", nullptr, false, false);
    const auto task = fixture.root / original.job_id / "model-vertex-color.obj";
    boost::filesystem::create_directories(task.parent_path());
    boost::filesystem::copy_file(original.model_path, task);
    const bool published_from_task = GENERATE(false, true);
    auto source = original;
    if (published_from_task) source.model_path = task;
    const auto repair = fixture.model("finish-repaired", &source);
    // The repository lists the task copy; the creator displays the downloaded
    // copy. Neither an exact sidebar path match nor its index is available.
    auto request = std::make_shared<ModelHistoryOpenRequest>();
    request->root = fixture.root;
    request->entry = original;
    request->expected_model_sha256 = AI::model_artifact_sha256(original.model_path);
    const auto metadata_before = fixture.read(repair);
    REQUIRE_FALSE(request->repairs);
    REQUIRE(fixture.prepare(request));
    REQUIRE(request->repairs);
    CHECK(request->entry.model_path == repair.model_path);
    REQUIRE(request->check);
    CHECK(request->check->status == WorkbenchCheckStatus::Normal);
    CHECK(request->check->candidate_status == RepairCandidateStatus::None);
    CHECK(AI::model_artifact_sha256(original.model_path) == request->expected_model_sha256);
    CHECK(fixture.read(repair) == metadata_before);
    if (published_from_task) {
        boost::filesystem::ofstream changed(task, std::ios::app);
        changed << "\n# changed task source\n";
        changed.close();
        auto stale = std::make_shared<ModelHistoryOpenRequest>();
        stale->root = fixture.root;
        stale->entry = original;
        stale->expected_model_sha256 = request->expected_model_sha256;
        REQUIRE(fixture.prepare(stale));
        CHECK_FALSE(stale->check);
        CHECK(stale->entry.model_path == original.model_path);
        CHECK(fixture.read(repair) == metadata_before);
    }
}
