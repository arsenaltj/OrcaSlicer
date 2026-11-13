#include <catch2/catch_test_macros.hpp>
#include "slic3r/GUI/Redesign/InlineModelGenerationState.hpp"

using namespace Slic3r::GUI;
namespace {
ModelGenerationUIState generating(bool smart = true) {
    ModelGenerationUIState result;
    result.smart_mode = smart; result.smart_flow_id = smart ? "flow" : "";
    result.job_id = "job"; result.model_generation_context = true;
    result.model_asset_id = "job";
    result.model_generation_session = 3; result.revision = 10;
    result.stage = smart ? ModelGenerationUIStage::GeneratingDesign : ModelGenerationUIStage::GeneratingModel;
    result.busy = true; return result;
}
InlineModelArtifact artifact(std::string hash = "hash-a") {
    InlineModelArtifact result;
    result.request.entry.job_id = "job"; result.request.entry.model_path = "model.obj";
    result.request.artifact.job_id = "job"; result.request.artifact.local_path = "model.obj";
    result.request.expected_sha256 = std::move(hash); return result;
}
void ready(InlineModelGenerationState& view) {
    REQUIRE(view.advance(view.generation(), InlinePreviewPhase::Preparing));
    REQUIRE(view.advance(view.generation(), InlinePreviewPhase::AwaitingFirstFrame));
    REQUIRE(view.advance(view.generation(), InlinePreviewPhase::Ready));
}
}

TEST_CASE("A remote result enables completion only after its current preview frame", "[InlineModelGeneration]") {
    for (bool smart : {false,true}) {
        DYNAMIC_SECTION((smart ? "smart" : "advanced")) {
            InlineModelGenerationState view; auto snapshot = generating(smart);
            GenerationBinding source {smart ? CreationSource::Smart : CreationSource::Advanced,"group",1};
            REQUIRE(view.apply(source, snapshot, {}));
            view.generation_started();
            CHECK(view.has_content()); CHECK_FALSE(view.ready());
            snapshot.stage = ModelGenerationUIStage::ModelReady; snapshot.model_ready = true; snapshot.busy = false; ++snapshot.revision;
            REQUIRE(view.apply(source, snapshot, artifact()));
            CHECK_FALSE(view.ready()); CHECK_FALSE(view.consume_completion(true));
            CHECK_FALSE(view.advance(view.generation(), InlinePreviewPhase::Ready));
            ready(view); CHECK(view.ready());
            CHECK_FALSE(view.consume_completion(false)); CHECK(view.consume_completion(true));
            CHECK_FALSE(view.consume_completion(true));
            REQUIRE(view.apply(source, snapshot, artifact())); CHECK(view.ready());
            CHECK_FALSE(view.consume_completion(true));
        }
    }
}

TEST_CASE("Smart preparation retains its flow when the final model job arrives", "[InlineModelGeneration]") {
    InlineModelGenerationState view; auto snapshot = generating(); snapshot.job_id.clear();
    const GenerationBinding source;
    REQUIRE(view.apply(source,snapshot,{}));
    view.generation_started();
    snapshot.job_id="new-model-job"; snapshot.stage=ModelGenerationUIStage::GeneratingModel; ++snapshot.revision;
    REQUIRE(view.apply(source,snapshot,{}));
    snapshot.stage=ModelGenerationUIStage::ModelReady; snapshot.model_ready=true; snapshot.busy=false; ++snapshot.revision;
    REQUIRE(view.apply(source,snapshot,artifact())); ready(view);
    CHECK(view.consume_completion(true));
}

TEST_CASE("Late preparation and first frames cannot complete a replacement artifact", "[InlineModelGeneration]") {
    InlineModelGenerationState view; auto snapshot=generating();
    snapshot.stage=ModelGenerationUIStage::ModelReady; snapshot.model_ready=true; snapshot.busy=false;
    REQUIRE(view.apply({},snapshot,artifact())); const auto old=view.generation();
    REQUIRE(view.advance(old,InlinePreviewPhase::Preparing));
    REQUIRE(view.apply({},snapshot,artifact("hash-b")));
    CHECK_FALSE(view.advance(old,InlinePreviewPhase::AwaitingFirstFrame));
    CHECK_FALSE(view.advance(old,InlinePreviewPhase::Ready)); CHECK_FALSE(view.ready());
    ready(view); CHECK(view.ready());
    REQUIRE(view.artifact());
    CHECK(view.artifact()->request.expected_sha256=="hash-b");
}

TEST_CASE("Context changes and stale revisions preserve the selected source", "[InlineModelGeneration]") {
    InlineModelGenerationState smart, advanced; auto a=generating(); auto b=generating(false);
    REQUIRE(smart.apply({CreationSource::Smart,"",0},a,{}));
    REQUIRE(advanced.apply({CreationSource::Advanced,"group-a",2},b,{}));
    auto stale=a; --stale.revision; stale.stage=ModelGenerationUIStage::Failed;
    CHECK_FALSE(smart.apply({CreationSource::Smart,"",0},stale,{}));
    CHECK(smart.snapshot().stage==ModelGenerationUIStage::GeneratingDesign);
    b.stage=ModelGenerationUIStage::ModelReady; b.busy=false; b.model_ready=true; ++b.revision;
    REQUIRE(advanced.apply({CreationSource::Advanced,"group-a",2},b,artifact()));
    const auto old=advanced.generation();
    b={}; REQUIRE(advanced.apply({CreationSource::Advanced,"group-b",3},b,{}));
    CHECK_FALSE(advanced.advance(old,InlinePreviewPhase::Ready)); CHECK_FALSE(advanced.has_content());
    CHECK(smart.has_content());
}

TEST_CASE("A changed result palette requires a frame of that appearance before opening", "[InlineModelGeneration]") {
    InlineModelGenerationState view; auto snapshot = generating();
    snapshot.stage = ModelGenerationUIStage::ModelReady; snapshot.model_ready = true; snapshot.busy = false;
    auto original = artifact(); original.request.entry.palette = {"#FF0000"};
    REQUIRE(view.apply({},snapshot,original)); ready(view);
    const auto previous_frame = view.generation();
    auto corrected = original; corrected.request.entry.palette = {"#00FF00"};
    ++snapshot.revision;
    REQUIRE(view.apply({},snapshot,corrected));
    CHECK_FALSE(view.ready());
    CHECK_FALSE(view.advance(previous_frame,InlinePreviewPhase::Ready));
    ready(view);
    REQUIRE(view.artifact());
    CHECK(view.artifact()->request.entry.palette == corrected.request.entry.palette);
    REQUIRE(view.apply({},snapshot,corrected));
    CHECK(view.ready());
}

TEST_CASE("Reload and historical presentation do not invent new generation notices", "[InlineModelGeneration]") {
    InlineModelGenerationState view; auto snapshot=generating();
    snapshot.stage=ModelGenerationUIStage::ModelReady; snapshot.busy=false; snapshot.model_ready=true;
    REQUIRE(view.apply({},snapshot,artifact())); ready(view);
    CHECK_FALSE(view.consume_completion(true));
    const auto old=view.generation(); view.retry(); CHECK_FALSE(view.ready());
    CHECK_FALSE(view.advance(old,InlinePreviewPhase::Ready));
    REQUIRE(view.advance(view.generation(),InlinePreviewPhase::Preparing));
    REQUIRE(view.advance(view.generation(),InlinePreviewPhase::Failed));
    CHECK_FALSE(view.advance(view.generation(),InlinePreviewPhase::Ready));
    CHECK_FALSE(view.consume_completion(true));
    view.retry(); ready(view); CHECK_FALSE(view.consume_completion(true));
}

TEST_CASE("Busy and failed tasks cannot expose a previous local artifact as complete", "[InlineModelGeneration]") {
    InlineModelGenerationState view; auto snapshot=generating(false);
    snapshot.stage=ModelGenerationUIStage::ModelReady; snapshot.model_ready=true; snapshot.busy=false;
    REQUIRE(view.apply({},snapshot,artifact())); ready(view);
    snapshot.stage=ModelGenerationUIStage::GeneratingModel; snapshot.busy=true; ++snapshot.model_generation_session; ++snapshot.revision;
    REQUIRE(view.apply({},snapshot,artifact())); CHECK_FALSE(view.artifact()); CHECK_FALSE(view.ready());
    snapshot.stage=ModelGenerationUIStage::Failed; snapshot.busy=false; ++snapshot.revision;
    REQUIRE(view.apply({},snapshot,artifact())); CHECK(view.has_content()); CHECK_FALSE(view.ready());
}

TEST_CASE("Observing an existing task never announces another generation", "[InlineModelGeneration]") {
    InlineModelGenerationState view;
    auto snapshot = generating(false);
    const GenerationBinding source {CreationSource::Advanced,"group",1};
    REQUIRE(view.apply(source,snapshot,{}));
    snapshot.stage = ModelGenerationUIStage::LoadingModel; ++snapshot.revision;
    REQUIRE(view.apply(source,snapshot,{}));
    snapshot.stage = ModelGenerationUIStage::ModelReady;
    snapshot.model_ready = true; snapshot.busy = false; ++snapshot.revision;
    REQUIRE(view.apply(source,snapshot,artifact())); ready(view);
    CHECK_FALSE(view.consume_completion(true));
}

TEST_CASE("A group refresh retains a new result notice without notifying twice", "[InlineModelGeneration]") {
    InlineModelGenerationState view;
    auto snapshot = generating(false);
    GenerationBinding source {CreationSource::Advanced,"group",1};
    REQUIRE(view.apply(source,snapshot,{})); view.generation_started();
    ++source.epoch; // The accepted submission's receipt refreshes this same group.
    REQUIRE(view.apply(source,snapshot,{}));
    snapshot.stage = ModelGenerationUIStage::ModelReady;
    snapshot.model_ready = true; snapshot.busy = false; ++snapshot.revision;
    REQUIRE(view.apply(source,snapshot,artifact())); ready(view);
    CHECK(view.consume_completion(true));
    ++source.epoch; ++snapshot.model_generation_session;
    REQUIRE(view.apply(source,snapshot,artifact())); ready(view);
    CHECK_FALSE(view.consume_completion(true));
}

TEST_CASE("A missing local model retains its source error without exposing completion", "[InlineModelGeneration]") {
    InlineModelGenerationState view; auto snapshot = generating();
    snapshot.stage = ModelGenerationUIStage::Input; snapshot.busy = false;
    snapshot.model_ready = false; snapshot.model_path = "missing.obj";
    REQUIRE(view.apply({},snapshot,artifact()));
    CHECK(view.has_content()); CHECK_FALSE(view.artifact()); CHECK_FALSE(view.ready());
    CHECK_FALSE(view.consume_completion(true));
}

TEST_CASE("A snapshot cannot display another task artifact returned after publication", "[InlineModelGeneration]") {
    InlineModelGenerationState view; auto snapshot = generating();
    snapshot.stage = ModelGenerationUIStage::ModelReady; snapshot.model_ready = true; snapshot.busy = false;
    auto other = artifact(); other.request.entry.job_id = "another-job";
    REQUIRE(view.apply({},snapshot,other));
    CHECK_FALSE(view.artifact()); CHECK_FALSE(view.ready());
    REQUIRE(view.apply({},snapshot,artifact())); ready(view);
    REQUIRE(view.artifact()); CHECK(view.artifact()->request.entry.job_id == snapshot.model_asset_id);
}

TEST_CASE("Invalidating a creation context hides its old progress and rejects late frames", "[InlineModelGeneration]") {
    InlineModelGenerationState view; auto snapshot = generating();
    REQUIRE(view.apply({},snapshot,{})); REQUIRE(view.has_content());
    const auto old = view.generation(); view.invalidate();
    CHECK_FALSE(view.has_content()); CHECK_FALSE(view.ready());
    CHECK_FALSE(view.advance(old,InlinePreviewPhase::Ready));
}
