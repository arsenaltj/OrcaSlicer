#include "slic3r/GUI/Redesign/RedesignCommand.hpp"
#include "slic3r/GUI/Redesign/RedesignFeatureFlags.hpp"
#include "slic3r/GUI/Redesign/OrcaBusinessAdapter.hpp"
#include "slic3r/GUI/Redesign/RedesignState.hpp"
#include "slic3r/GUI/Redesign/ImageHistoryPagination.hpp"
#include "slic3r/GUI/Redesign/AssetsWorkspacePolicy.hpp"
#include "slic3r/GUI/Redesign/ModelFaceLimitChoice.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelGenerationHost.hpp"
#include "slic3r/GUI/Redesign/RedesignModelRoute.hpp"
#include "slic3r/GUI/Redesign/ImageDesignDraft.hpp"
#include "slic3r/GUI/AI/ModelGeneration/DesignGenerationAvailability.hpp"
#include "slic3r/GUI/AI/ModelGeneration/PostGenerationWorkbenchState.hpp"
#include "slic3r/GUI/AIModelGenerationClient.hpp"
#include "slic3r/GUI/Redesign/RedesignMessageDialog.hpp"
#include "slic3r/GUI/AI/SmartSlicing/SmartSlicingWorkbenchState.hpp"
#include "slic3r/GUI/Redesign/RedesignSlicingDetails.hpp"
#include "slic3r/GUI/Redesign/RedesignPageTransition.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstdlib>
#include <string>

using namespace Slic3r::GUI;

TEST_CASE("Reviewed estimates survive trial cancellation only for their bound native input and goal", "[UiRedesign][SliceDetails]")
{
    using namespace Slic3r::AI::SmartSlicing;
    const WorkspaceRevision reviewed{1, 2, 3, "reviewed"};
    const WorkspaceRevision applied{1, 4, 5, "applied"};
    SliceRecommendation plan;
    plan.mode = SliceMode::SPEED;
    plan.layer_height_mm = 0.26;
    ExactEstimateView trial;
    trial.status = ExactEstimateStatus::Ready;
    trial.revision = reviewed;
    trial.goal = RecommendationGoal::Speed;
    trial.parameters = plan;
    trial.estimated_time_seconds = 7200.0;
    trial.material_grams = 110.42;
    WorkbenchSliceEstimate display;
    display.capture(trial, reviewed, applied, trial.goal, plan);
    trial = {}; // Cancellation clears the worker view, not the reviewed display.
    const auto retained = display.for_slice(applied, RecommendationGoal::Speed, plan);
    REQUIRE(retained);
    CHECK(retained->revision == reviewed);
    CHECK(retained->status == ExactEstimateStatus::Ready);
    REQUIRE(retained->estimated_time_seconds);
    CHECK_THAT(*retained->estimated_time_seconds, Catch::Matchers::WithinAbs(7200.0, 0.001));
    REQUIRE(retained->material_grams);
    CHECK_THAT(*retained->material_grams, Catch::Matchers::WithinAbs(110.42, 0.001));
    CHECK_FALSE(display.for_slice(reviewed, RecommendationGoal::Speed, plan));
    CHECK_FALSE(display.for_slice(applied, RecommendationGoal::Quality, plan));
    auto edited = applied;
    ++edited.model_revision;
    CHECK_FALSE(display.for_slice(edited, RecommendationGoal::Speed, plan));
    edited = applied;
    ++edited.config_revision;
    CHECK_FALSE(display.for_slice(edited, RecommendationGoal::Speed, plan));
    edited = applied;
    ++edited.plate_revision;
    CHECK_FALSE(display.for_slice(edited, RecommendationGoal::Speed, plan));
    plan.layer_height_mm = 0.12;
    CHECK_FALSE(display.for_slice(applied, RecommendationGoal::Speed, plan));
}

TEST_CASE("Only completed estimates for the reviewed recommendation can be retained", "[UiRedesign][SliceDetails]")
{
    using namespace Slic3r::AI::SmartSlicing;
    const WorkspaceRevision revision{1, 2, 3, "current"};
    SliceRecommendation plan;
    ExactEstimateView trial;
    trial.status = ExactEstimateStatus::Ready;
    trial.revision = revision;
    trial.parameters = plan;
    WorkbenchSliceEstimate display;
    display.capture(trial, revision, revision, trial.goal, plan);
    REQUIRE(display.for_slice(revision, trial.goal, plan));
    for (const auto status : {ExactEstimateStatus::None, ExactEstimateStatus::Running, ExactEstimateStatus::Failed}) {
        trial.status = status;
        display.capture(trial, revision, revision, trial.goal, plan);
        CHECK_FALSE(display.for_slice(revision, trial.goal, plan));
    }
    trial.status = ExactEstimateStatus::Ready;
    trial.revision.fingerprint = "stale";
    display.capture(trial, revision, revision, trial.goal, plan);
    CHECK_FALSE(display.for_slice(revision, trial.goal, plan));
    trial.revision = revision;
    plan.wall_loops += 1;
    display.capture(trial, revision, revision, trial.goal, plan);
    CHECK_FALSE(display.for_slice(revision, trial.goal, plan));
}

TEST_CASE("Formal statistics update the metrics while every recommendation detail remains visible", "[UiRedesign][SliceDetails]")
{
    Slic3r::AI::SmartSlicing::SliceRecommendation plan;
    plan.layer_height_mm = 0.26;
    plan.wall_loops = 2;
    plan.sparse_infill_density_percent = 10;
    plan.sparse_infill_pattern = "grid";
    plan.outer_wall_speed_mm_s = 200;
    plan.enable_support = true;
    plan.support_threshold_angle_deg = 60;
    plan.adaptive_layer_height = false;
    plan.arachne_wall_generator = true;
    ExactEstimateView estimate;
    estimate.status = ExactEstimateStatus::Ready;
    estimate.estimated_time_seconds = 7200;
    estimate.material_grams = 100;
    const auto before = exact_estimate_detail_rows(plan, estimate);
    const auto slicing = exact_estimate_detail_rows(plan, estimate, {}, true, true);
    const std::vector<SliceDetailRow> statistics{
        {_L("打印时间"), _L("6 小时 9 分钟"), _L("正式切片结果")},
        {_L("实际耗材"), "110.42 g", _L("正式切片结果")}};
    // Even a cancelled/missing trial must not add AI fallback rows to a formal result.
    estimate.status = ExactEstimateStatus::Failed;
    const auto completed = exact_estimate_detail_rows(plan, estimate, statistics);
    REQUIRE(before.size() == 12);
    REQUIRE(slicing.size() == before.size());
    REQUIRE(completed.size() == before.size());
    CHECK(slicing[0].value == before[0].value);
    CHECK(slicing[1].value == before[1].value);
    CHECK(slicing[0].label == _L("切片前预计时间"));
    for (size_t i = 0; i < statistics.size(); ++i) {
        CHECK(completed[i].label == statistics[i].label);
        CHECK(completed[i].value == statistics[i].value);
    }
    for (size_t i = 2; i < before.size(); ++i) {
        CHECK(slicing[i].label == before[i].label);
        CHECK(slicing[i].value == before[i].value);
        CHECK(completed[i].label == before[i].label);
        CHECK(completed[i].value == before[i].value);
        CHECK(completed[i].section == before[i].section);
    }
}

TEST_CASE("A native slice without completed estimates waits for its own result", "[UiRedesign][SliceDetails]")
{
    Slic3r::AI::SmartSlicing::SliceRecommendation plan;
    const auto rows = exact_estimate_detail_rows(plan, {}, {}, false, true);
    REQUIRE(rows.size() == 12);
    CHECK(rows[0].value == _L("等待切片结果"));
    CHECK(rows[1].value == _L("等待切片结果"));
}

TEST_CASE("Pending cloud recommendations do not inherit completion from local baselines", "[UiRedesign][CloudRecommendationPresentation]")
{
    SmartSlicingWorkbenchState state;
    state.lightweight_analysis.emplace();
    state.lightweight_analysis->completed = true;
    const auto goal = Slic3r::AI::SmartSlicing::RecommendationGoal::Balanced;
    using Stage = RecommendationPresentationStage;
    CHECK(workbench_recommendation_stage(state, goal) == Stage::EstimatingLocal);
    state.recommendation_source = SlicingRecommendationSource::Cloud;
    CHECK(workbench_recommendation_stage(state, goal) == Stage::Idle);
    state.cloud_recommendation.status = CloudRecommendationStatus::Preparing;
    CHECK(workbench_recommendation_stage(state, goal) == Stage::PreparingCloud);
    state.cloud_recommendation.status = CloudRecommendationStatus::Requesting;
    CHECK(workbench_recommendation_stage(state, goal) == Stage::RequestingCloud);
    state.analyzing = true;
    CHECK(workbench_recommendation_stage(state, goal) == Stage::RequestingCloud);
    state.cloud_recommendation.status = CloudRecommendationStatus::Ready;
    CHECK(workbench_recommendation_stage(state, goal) == Stage::Analyzing);
    state.analyzing = false;
    CHECK(workbench_recommendation_stage(state, goal) == Stage::EstimatingCloud);
    state.exact_estimate.status = ExactEstimateStatus::Ready;
    state.exact_estimate.parameters = state.lightweight_analysis->recommendations[0];
    CHECK(workbench_recommendation_stage(state, goal) == Stage::CloudReady);
    state.cloud_recommendation.status = CloudRecommendationStatus::Failed;
    state.recommendation_source = SlicingRecommendationSource::Local;
    CHECK(workbench_recommendation_stage(state, goal) == Stage::CloudFailed);
}

TEST_CASE("Cloud readiness distinguishes the current exact estimate from unrelated trials", "[UiRedesign][CloudRecommendationPresentation]")
{
    SmartSlicingWorkbenchState state;
    state.recommendation_source = SlicingRecommendationSource::Cloud;
    state.cloud_recommendation.status = CloudRecommendationStatus::Ready;
    state.session.workspace_revision = {1, 2, 3, "current"};
    state.exact_estimate.revision = state.session.workspace_revision;
    state.exact_estimate.status = ExactEstimateStatus::Running;
    const auto goal = state.exact_estimate.goal;
    using Stage = RecommendationPresentationStage;
    CHECK(workbench_recommendation_stage(state, goal) == Stage::EstimatingCloud);
    state.exact_estimate.revision.fingerprint = "old";
    CHECK(workbench_recommendation_stage(state, goal) == Stage::EstimatingCloud);
    state.exact_estimate.revision = state.session.workspace_revision;
    CHECK(workbench_recommendation_stage(state, Slic3r::AI::SmartSlicing::RecommendationGoal::Speed) == Stage::EstimatingCloud);
    state.exact_estimate.status = ExactEstimateStatus::Ready;
    CHECK(workbench_recommendation_stage(state, goal) == Stage::CloudReady);
    state.exact_estimate.status = ExactEstimateStatus::Failed;
    CHECK(workbench_recommendation_stage(state, goal) == Stage::CloudReady);
}

TEST_CASE("Local recommendations remain estimating until their matching preview reaches a terminal state", "[UiRedesign][CloudRecommendationPresentation]")
{
    SmartSlicingWorkbenchState state;
    state.session.workspace_revision = {1, 2, 3, "current"};
    state.lightweight_analysis.emplace();
    state.lightweight_analysis->completed = true;
    const auto goal = state.selected_goal;
    using Stage = RecommendationPresentationStage;
    CHECK(workbench_recommendation_stage(state, goal) == Stage::EstimatingLocal);
    CHECK_FALSE(workbench_show_reanalyze(state));
    state.exact_estimate.status = ExactEstimateStatus::Running;
    CHECK(workbench_recommendation_stage(state, goal) == Stage::EstimatingLocal);
    state.exact_estimate.revision = state.session.workspace_revision;
    state.exact_estimate.parameters = state.lightweight_analysis->recommendations[0];
    state.exact_estimate.status = ExactEstimateStatus::Ready;
    CHECK(workbench_recommendation_stage(state, goal) == Stage::LocalReady);
    CHECK(workbench_show_reanalyze(state));
    state.exact_estimate.status = ExactEstimateStatus::Failed;
    CHECK(workbench_recommendation_stage(state, goal) == Stage::LocalReady);
    CHECK(workbench_show_reanalyze(state));
    state.exact_estimate.revision.fingerprint = "old";
    CHECK(workbench_recommendation_stage(state, goal) == Stage::EstimatingLocal);
}

TEST_CASE("Matching official statistics end preview busy state without waiting for a discarded trial", "[UiRedesign][CloudRecommendationPresentation]")
{
    SmartSlicingWorkbenchState state;
    state.lightweight_analysis.emplace();
    state.lightweight_analysis->completed = true;
    state.exact_estimate.status = ExactEstimateStatus::Running;
    state.official.phase = Slic3r::AI::SmartSlicing::OfficialSlicePhase::Completed;
    state.official_statistics_ready = true;
    CHECK_FALSE(workbench_estimate_pending(state, state.selected_goal));
    CHECK_FALSE(workbench_analysis_busy(state));
    CHECK(workbench_show_reanalyze(state));
    CHECK(workbench_recommendation_stage(state, state.selected_goal) == RecommendationPresentationStage::LocalReady);
    CHECK(workbench_estimate_pending(state, Slic3r::AI::SmartSlicing::RecommendationGoal::Quality));
    // The host clears this binding for changed input or a different recommendation.
    state.official_statistics_ready = false;
    CHECK(workbench_analysis_busy(state));
    CHECK_FALSE(workbench_show_reanalyze(state));
    CHECK(workbench_recommendation_stage(state, state.selected_goal) == RecommendationPresentationStage::EstimatingLocal);
}

TEST_CASE("Design inputs follow the selected provider catalog independently of other providers", "[UiRedesign][GenerationProviderAvailability]")
{
    AIModelGenerationClient::ProviderCatalog yoyo {"image", "yoyoclaw", "ready", "image-a",
        {{"image-a", "Image A", true}, {"image-b", "Image B", true}, {"disabled", "Disabled", false}}, "", "", false};
    AIModelGenerationClient::ProviderCatalog gpt {"image", "gpt", "not_configured", "", {}, "", "", false};
    yoyo.connection_state = "connected";
    gpt.connection_state = "not_configured";
    PostGenerationUiState workbench;
    ModelGenerationUIInput input;
    input.image_path = "reference.png";
    CHECK(design_generation_available(input, yoyo.supports_model("image-a"), false, workbench, true));
    CHECK_FALSE(design_generation_available(input, gpt.supports_model(""), false, workbench, true));
    CHECK(yoyo.supports_model("image-b"));
    CHECK_FALSE(yoyo.supports_model("disabled"));
    CHECK_FALSE(yoyo.supports_model("missing"));
    CHECK_FALSE(yoyo.supports_model(""));
    CHECK(yoyo.remotely_connected());
    input.image_path.clear();
    CHECK_FALSE(design_generation_available(input, yoyo.supports_model("image-a"), false, workbench, true));
    input.prompt = "a small sculpture";
    CHECK(design_generation_available(input, yoyo.supports_model("image-a"), false, workbench, true));
    CHECK_FALSE(design_generation_available(input, yoyo.supports_model("image-a"), true, workbench, true));
    gpt = {"image", "gpt", "ready", "gpt", {{"gpt", "GPT", true}}, "", "", false};
    gpt.connection_state = "configured";
    yoyo.state = "error";
    CHECK(design_generation_available(input, gpt.supports_model(""), false, workbench, true));
    CHECK(gpt.ready());
    CHECK_FALSE(gpt.remotely_connected());
    CHECK_FALSE(yoyo.supports_model("image-a"));
}

TEST_CASE("Geometry catalogs require an available model and remain independent of image catalogs", "[UiRedesign][GenerationProviderAvailability]")
{
    AIModelGenerationClient::ProviderCatalog geometry {"geometry", "yoyoclaw", "ready", "mesh",
        {{"mesh", "Mesh", true}}, "", "", false};
    AIModelGenerationClient::ProviderCatalog image {"image", "yoyoclaw", "empty", "", {}, "", "", false};
    geometry.connection_state = "connected";
    CHECK(geometry.supports_model("mesh"));
    CHECK_FALSE(geometry.supports_model("image-a"));
    CHECK_FALSE(image.ready());
    geometry.models[0].available = false;
    CHECK_FALSE(geometry.ready());
    CHECK_FALSE(geometry.supports_model("mesh"));
    geometry.state = "checking";
    geometry.models[0].available = true;
    CHECK_FALSE(geometry.supports_model("mesh"));
}

TEST_CASE("Preview auto-fit waits for a ready viewport and frames each result once", "[UiRedesign][PreviewAutoFit]")
{
    PreviewAutoFitState state;
    state.request();
    CHECK(state.pending());
    CHECK_FALSE(state.consume(41, false, true, true));
    CHECK_FALSE(state.consume(41, true, false, true));
    CHECK_FALSE(state.consume(41, true, true, false));
    CHECK(state.pending());

    CHECK(state.consume(41, true, true, true));
    CHECK_FALSE(state.pending());
    REQUIRE(state.last_result_id());
    CHECK(*state.last_result_id() == 41);

    state.request();
    CHECK_FALSE(state.consume(41, true, true, true));
    CHECK_FALSE(state.pending());

    state.request();
    CHECK(state.consume(42, true, true, true));
    CHECK(*state.last_result_id() == 42);
}

TEST_CASE("Slicing navigation waits for analysis and workspace operations to finish", "[UiRedesign][WorkbenchNavigation]")
{
    CHECK(workbench_mode_switch_allowed(false, false, false, false, false));
    CHECK_FALSE(workbench_mode_switch_allowed(false, false, false, false, true));
    CHECK_FALSE(workbench_mode_switch_allowed(true, false, false, false, false));
    CHECK_FALSE(workbench_mode_switch_allowed(false, true, false, false, false));
    CHECK_FALSE(workbench_mode_switch_allowed(false, false, true, false, false));
    CHECK_FALSE(workbench_mode_switch_allowed(false, false, false, true, false));
    CHECK(workbench_navigation_allowed(false, true));
    CHECK(workbench_navigation_allowed(true, false));
    CHECK_FALSE(workbench_navigation_allowed(true, true));
}

TEST_CASE("Redesign page transitions reject overlap and ignore late readiness callbacks",
          "[UiRedesign][WorkbenchNavigation]")
{
    RedesignPageTransition transition;
    const auto first = transition.begin({1, 0}, {2, 1});
    REQUIRE(first);
    CHECK(transition.active());
    CHECK_FALSE(transition.begin({1, 0}, {3, 0}));
    CHECK_FALSE(transition.commit(*first));
    REQUIRE(transition.mark_ready(*first));
    CHECK(transition.commit(*first));
    CHECK_FALSE(transition.active());

    const auto second = transition.begin({2, 1}, {3, 0});
    REQUIRE(second);
    CHECK(*second > *first);
    CHECK_FALSE(transition.mark_ready(*first));
    CHECK_FALSE(transition.rollback(*first));
    CHECK(transition.rollback(*second));
    CHECK_FALSE(transition.current(*second));
}

TEST_CASE("Changing the model face target selects compatible geometry and retains other options", "[UiRedesign][ModelFaceLimitChoice]")
{
    ModelGenerationUIOptions options;
    CHECK(options.face_limit == 1000000);
    CHECK(options.geometry_quality == "standard");
    CHECK((model_face_limit_choices(options) == std::vector<int> {1000000, 2000000}));
    options.texture_quality = "detailed";
    options.output_format = "obj";
    const auto high = model_generation_options_for_face_limit(options, 2000000);
    CHECK(high.face_limit == 2000000);
    CHECK(high.geometry_quality == "detailed");
    CHECK(high.texture_quality == options.texture_quality);
    CHECK(high.output_format == options.output_format);
    CHECK(high.provider == options.provider);
    CHECK(model_generation_options_for_face_limit(high, 1000000) == options);
}

TEST_CASE("Primary action separates changed 2D inputs from changed 3D options", "[UiRedesign][ModelGenerationAction]")
{
    ModelGenerationUIState state;
    state.stage = ModelGenerationUIStage::ModelReady;
    state.design_ready = true;
    state.inputs_match_job = true;
    state.model_options_match = false;
    CHECK(redesign_primary_action(state, state.stage) == RedesignPrimaryAction::Model);

    state.inputs_match_job = false;
    CHECK(redesign_primary_action(state, state.stage) == RedesignPrimaryAction::Design);

    state.inputs_match_job = true;
    state.model_options_match = false;
    state.stage = ModelGenerationUIStage::DesignReady;
    CHECK(redesign_primary_action(state, state.stage) == RedesignPrimaryAction::Model);

    state.model_options_match = true;
    CHECK(redesign_primary_action(state, state.stage) == RedesignPrimaryAction::Model);

    state.stage = ModelGenerationUIStage::ModelReady;
    CHECK(redesign_primary_action(state, state.stage) == RedesignPrimaryAction::Design);

    state.model_options_match = false;
    state.design_ready = false;
    CHECK(redesign_primary_action(state, state.stage) == RedesignPrimaryAction::Design);

    state.stage = ModelGenerationUIStage::DesignReady;
    state.inputs_match_job = false;
    CHECK(redesign_primary_action(state, state.stage) == RedesignPrimaryAction::Design);

    state.stage = ModelGenerationUIStage::Saving3DOptions;
    state.design_ready = true;
    state.inputs_match_job = true;
    CHECK(redesign_primary_action(state, state.stage) == RedesignPrimaryAction::Model);

    state.inputs_match_job = false;
    CHECK(redesign_primary_action(state, state.stage) == RedesignPrimaryAction::Design);
}

TEST_CASE("Historical face targets are displayed without rewriting retained generation options", "[UiRedesign][ModelFaceLimitChoice]")
{
    ModelGenerationUIOptions historical;
    historical.face_limit = 300000;
    historical.geometry_quality = "detailed";
    const auto retained = historical;
    CHECK((model_face_limit_choices(historical) == std::vector<int> {1000000, 2000000, 300000}));
    CHECK(historical == retained);
    const auto new_options = model_generation_options_for_face_limit(historical, 1000000);
    CHECK(new_options.geometry_quality == "standard");
    CHECK((model_face_limit_choices(new_options) == std::vector<int> {1000000, 2000000}));
    historical.face_limit = 1000000;
    CHECK((model_face_limit_choices(historical) == std::vector<int> {1000000, 2000000}));
    CHECK(historical.geometry_quality == "detailed");
}

TEST_CASE("Non-Tripo face choices exclude unsupported detailed geometry and retain provider models", "[UiRedesign][ModelFaceLimitChoice]")
{
    ModelGenerationUIOptions options;
    options.provider = "yoyoclaw";
    options.design_provider = "yoyoclaw";
    options.design_model = "retained-image-model";
    options.geometry_model = "retained-geometry-model";
    options.output_format = "obj";
    CHECK(model_face_limit_choices(options) == std::vector<int> {1000000});
    options.texture_quality = "detailed";
    const auto compatible = model_generation_options_for_face_limit(options, 2000000);
    CHECK(compatible.face_limit == 1000000);
    CHECK(compatible.geometry_quality == "standard");
    CHECK(compatible.texture_quality == "standard");
    CHECK(compatible.provider == options.provider);
    CHECK(compatible.design_provider == options.design_provider);
    CHECK(compatible.design_model == options.design_model);
    CHECK(compatible.geometry_model == options.geometry_model);
    CHECK(compatible.output_format == options.output_format);
    options.face_limit = 300000;
    const auto retained = options;
    CHECK((model_face_limit_choices(options) == std::vector<int> {1000000, 300000}));
    CHECK(options == retained);
}

TEST_CASE("A retained model candidate permits a new design but active work and unavailable services do not", "[UiRedesign][ImageDesignDraft]")
{
    ModelGenerationUIInput input;
    input.image_path = "reference.png";
    input.style = "sculpture";
    PostGenerationUiState workbench;
    for (auto status : {PostGenerationUiState::Status::Ready, PostGenerationUiState::Status::Editing,
                       PostGenerationUiState::Status::CandidateReady, PostGenerationUiState::Status::ComparingBefore}) {
        workbench.status = status;
        CHECK(design_generation_available(input, true, false, workbench, true));
        CHECK_FALSE(design_generation_available(input, false, false, workbench, true));
        CHECK_FALSE(design_generation_available(input, true, true, workbench, true));
    }
    for (auto status : {PostGenerationUiState::Status::Loading, PostGenerationUiState::Status::Processing}) {
        workbench.status = status;
        CHECK_FALSE(design_generation_available(input, true, false, workbench, true));
    }
    workbench.status = PostGenerationUiState::Status::Ready;
    input.image_path.clear();
    CHECK_FALSE(design_generation_available(input, true, false, workbench, true));
    input.prompt = "new design";
    CHECK(design_generation_available(input, true, false, workbench, true));
    input.style = "custom";
    CHECK_FALSE(design_generation_available(input, true, false, workbench, true));
    input.custom_style = "clay";
    CHECK(design_generation_available(input, true, false, workbench, true));
    CHECK_FALSE(design_generation_available(input, true, false, workbench, false));
}

TEST_CASE("An oversized UTF-8 draft disables design generation even with an image and recovers after editing", "[UiRedesign][ImageDesignDraft]")
{
    ModelGenerationUIInput input;
    input.style = "sculpture";
    PostGenerationUiState workbench;
    workbench.status = PostGenerationUiState::Status::Ready;
    const std::string character = "\xe7\x8c\xab";
    for (const auto& image : {"", "reference.png"}) {
        input.image_path = image;
        input.prompt.clear();
        for (int index = 0; index < 666; ++index) input.prompt += character;
        input.prompt += "ab";
        REQUIRE(input.prompt.size() == ModelGenerationPresentation::MAX_MODEL_INPUT_BYTES);
        CHECK(design_generation_available(input, true, false, workbench, true));
        input.prompt += character;
        const auto retained = input.prompt;
        CHECK_FALSE(design_generation_available(input, true, false, workbench, true));
        CHECK(input.prompt == retained);
        input.prompt = character;
        CHECK(design_generation_available(input, true, false, workbench, true));
        input.prompt.assign(ModelGenerationPresentation::MAX_MODEL_INPUT_BYTES + 1, 'a');
        CHECK_FALSE(design_generation_available(input, true, false, workbench, true));
    }
}

TEST_CASE("A new image design supplies a missing historical style without altering its source record", "[UiRedesign][ImageDesignDraft]")
{
    ModelGenerationUIInput historical;
    historical.image_path = "history/reference.png";
    historical.prompt = "retained prompt";
    historical.style.clear();
    const auto editable = image_design_input_for_editing(historical);
    CHECK(editable.style == "sculpture");
    CHECK(editable.image_path == historical.image_path);
    CHECK(editable.prompt == historical.prompt);
    CHECK(historical.style.empty());

    for (const auto& style : {"sculpture", "realistic", "cartoon", "custom"}) {
        historical.style = style;
        historical.custom_style = "user's custom style";
        CHECK(image_design_input_for_editing(historical) == historical);
    }
    historical.style = "unknown-legacy-style";
    CHECK(image_design_input_for_editing(historical).style == "sculpture");
}

TEST_CASE("Finalizing an already visible preview refreshes image history once", "[UiRedesign][ImageDesignDraft]")
{
    ModelGenerationUIState preview;
    preview.job_id = "design-job";
    preview.design_ready = true;
    preview.design_image_path = "design/preview.png";
    preview.stage = ModelGenerationUIStage::GeneratingDesign;
    auto ready = preview;
    ready.stage = ModelGenerationUIStage::DesignReady;
    CHECK(image_design_refreshes_history(ready, preview));
    ++ready.revision;
    CHECK_FALSE(image_design_refreshes_history(ready, ready));
    auto changed = ready;
    changed.design_image_path = "design/final.png";
    CHECK(image_design_refreshes_history(changed, ready));
    changed = ready;
    changed.job_id = "another-design";
    CHECK(image_design_refreshes_history(changed, ready));
    auto not_ready = ready;
    not_ready.design_ready = false;
    CHECK(image_design_refreshes_history(ready, not_ready));
    CHECK_FALSE(image_design_refreshes_history(not_ready, ready));
    auto model = ready;
    model.stage = ModelGenerationUIStage::GeneratingModel;
    CHECK_FALSE(image_design_refreshes_history(model, ready));
}

TEST_CASE("Explicit history switches restore both image inputs while refreshes retain a new draft", "[UiRedesign][ImageDesignDraft]")
{
    ModelGenerationUIState current;
    current.revision = 10;
    current.model_generation_session = 1;
    current.input.image_path = "A/reference.png";
    current.design_image_path = "A/design.png";
    auto history = current;
    ++history.revision;
    ++history.model_generation_session;
    history.model_generation_context = true;
    history.model_asset_id = "model-B";
    history.stage = ModelGenerationUIStage::ModelReady;
    history.input.image_path = "B/reference.png";
    history.design_image_path = "B/design.png";
    CHECK(image_design_restores_input(history, current));
    CHECK(model_generation_route_action(history, current, false, 0, {}) == RedesignModelRouteAction::Model);

    ImageDesignDraft draft;
    draft.begin(history); // User uploads C after returning from B.
    auto refresh = history;
    ++refresh.revision;
    draft.observe(refresh);
    CHECK_FALSE(image_design_restores_input(refresh, history));
    CHECK(draft.pending());

    auto no_reference = history;
    ++no_reference.revision;
    ++no_reference.model_generation_session;
    no_reference.model_asset_id = "model-without-images";
    no_reference.input.image_path.clear();
    no_reference.design_image_path.clear();
    CHECK(image_design_restores_input(no_reference, history));
    draft.observe(no_reference);
    CHECK_FALSE(draft.pending());
}

TEST_CASE("Saving a local version and undoing or redoing it preserve the workbench and reject older context", "[UiRedesign][ModelGenerationRoute]")
{
    ModelGenerationUIState current;
    current.revision = 10;
    current.model_generation_session = 1;
    current.model_generation_context = true;
    current.model_ready = true;
    current.model_asset_id = "original";
    current.stage = ModelGenerationUIStage::ModelReady;
    for (const auto& asset : {"saved-version", "original", "saved-version"}) {
        auto next = current;
        ++next.revision;
        ++next.model_generation_session;
        next.model_asset_id = asset;
        // Undo can restore the original generation job as well as its asset.
        next.job_id = next.model_asset_id == "original" ? "original-task" : "";
        const auto route = model_generation_route_action(next, current, true,
            current.model_generation_session, model_generation_route_id(current));
        CHECK(route == RedesignModelRouteAction::Model);
        CHECK(model_generation_preserves_workbench(next, route, true));
        auto delayed = current;
        delayed.revision = next.revision + 1;
        CHECK(model_generation_route_action(delayed, next, true,
            next.model_generation_session, model_generation_route_id(next)) == RedesignModelRouteAction::Ignore);
        current = next;
    }
}

TEST_CASE("Repeated returns to image editing do not depend on retaining the model route lock", "[UiRedesign][ModelGenerationRoute]")
{
    ModelGenerationUIState state;
    state.revision = 10;
    state.model_generation_session = 1;
    state.model_generation_context = true;
    state.model_asset_id = "retained-model";
    state.stage = ModelGenerationUIStage::ModelReady;
    PostGenerationUiState workbench;
    workbench.status = PostGenerationUiState::Status::Ready;
    for (int visit = 0; visit < 2; ++visit) {
        CHECK(model_generation_return_to_design_allowed(state, workbench, false));
        auto refresh = state;
        ++refresh.revision;
        CHECK(model_generation_route_action(refresh, state, false, 0, {}) == RedesignModelRouteAction::Refresh);
        state = refresh;
    }
    state.busy = true;
    CHECK_FALSE(model_generation_return_to_design_allowed(state, workbench, false));
}

TEST_CASE("A successful upload starts an empty design draft without changing the retained model", "[UiRedesign][ImageDesignDraft]")
{
    ModelGenerationUIState retained;
    retained.revision = 20;
    retained.model_generation_session = 3;
    retained.stage = ModelGenerationUIStage::ModelReady;
    retained.model_generation_context = true;
    retained.model_ready = true;
    retained.model_asset_id = "historical-model";
    retained.model_path = "history/model.glb";
    retained.design_ready = true;
    retained.design_image_path = "history/design.png";
    ImageDesignDraft draft;
    draft.begin(retained);
    CHECK(draft.stage(retained) == ModelGenerationUIStage::Input);
    CHECK(draft.pending());
    CHECK(retained.model_ready);
    CHECK(retained.model_path == "history/model.glb");
    CHECK(retained.design_image_path == "history/design.png");

    // Repeated selection of the same file is an upload event, not a path change.
    draft.begin(retained);
    for (auto stage : {ModelGenerationUIStage::ModelReady, ModelGenerationUIStage::DesignReady,
                       ModelGenerationUIStage::Failed, ModelGenerationUIStage::Stopped}) {
        auto refresh = retained;
        ++refresh.revision;
        refresh.stage = stage;
        draft.observe(refresh);
        CHECK(draft.pending());
        CHECK(draft.stage(refresh) == ModelGenerationUIStage::Input);
    }
}

TEST_CASE("Cancelling design confirmation keeps the new draft empty until a confirmed submission", "[UiRedesign][ImageDesignDraft]")
{
    ModelGenerationUIState state;
    state.revision = 20;
    state.stage = ModelGenerationUIStage::DesignReady;
    ImageDesignDraft draft;
    draft.begin(state);
    // Cancelled confirmation and incidental option changes retain the old stage.
    draft.begin_submission();
    ++state.revision;
    draft.end_submission(state);
    CHECK(draft.pending());
    draft.begin_submission();
    state.stage = ModelGenerationUIStage::GeneratingDesign;
    ++state.revision;
    draft.end_submission(state);
    CHECK_FALSE(draft.pending());
    CHECK(draft.stage(state) == ModelGenerationUIStage::GeneratingDesign);
    state.stage = ModelGenerationUIStage::Failed;
    ++state.revision;
    draft.observe(state);
    CHECK(draft.stage(state) == ModelGenerationUIStage::Failed);
    state.stage = ModelGenerationUIStage::DesignReady;
    CHECK(draft.stage(state) == ModelGenerationUIStage::DesignReady);
}

TEST_CASE("Replacing a design retains regeneration intent when upload clears the old preview", "[UiRedesign][ImageDesignDraft]")
{
    ModelGenerationUIState state;
    state.revision = 20;
    state.model_generation_session = 3;
    state.design_image_path = "history/design.png";
    ImageDesignDraft draft;
    draft.begin(state);
    state.design_image_path.clear();
    ++state.revision;
    draft.observe(state);
    CHECK(draft.replaces_design());

    // Another upload and a cancelled confirmation still replace that design.
    draft.begin(state);
    draft.begin_submission();
    ++state.revision;
    draft.end_submission(state);
    CHECK(draft.replaces_design());

    ++state.revision;
    ++state.model_generation_session;
    draft.observe(state);
    CHECK_FALSE(draft.replaces_design());
    draft.begin(state);
    CHECK_FALSE(draft.replaces_design());
}

TEST_CASE("A history selection replaces the draft while old notifications cannot restore its result", "[UiRedesign][ImageDesignDraft]")
{
    ModelGenerationUIState state;
    state.revision = 20;
    state.model_generation_session = 3;
    ImageDesignDraft draft;
    draft.begin(state);
    auto stale = state;
    stale.stage = ModelGenerationUIStage::GeneratingDesign;
    draft.observe(stale);
    CHECK(draft.pending());
    ++stale.revision;
    draft.observe(stale); // No explicit submission of this draft.
    CHECK(draft.pending());
    ++state.revision;
    ++state.model_generation_session;
    state.stage = ModelGenerationUIStage::DesignReady;
    draft.observe(state);
    CHECK_FALSE(draft.pending());
    CHECK(draft.stage(state) == ModelGenerationUIStage::DesignReady);
}

TEST_CASE("Closing a three-way model confirmation cancels instead of selecting its alternate action", "[UiRedesign][ConfirmationDialog]")
{
    // NO can mean restart with advice, add another model, or record a print
    // issue. Closing must not choose any of those operations.
    CHECK(redesign_dialog_dismiss_result(wxYES_NO | wxCANCEL) == wxID_CANCEL);
    CHECK(redesign_dialog_dismiss_result(wxOK | wxCANCEL) == wxID_CANCEL);
    CHECK(redesign_dialog_dismiss_result(wxYES_NO) == wxID_NO);
}

TEST_CASE("Confirmation defaults retain risk refusal and explicit cancellation", "[UiRedesign][ConfirmationDialog]")
{
    CHECK(redesign_dialog_default_result(wxYES_NO | wxNO_DEFAULT) == wxID_NO);
    CHECK(redesign_dialog_default_result(wxYES_NO) == wxID_YES);
    CHECK(redesign_dialog_default_result(wxYES_NO | wxCANCEL | wxCANCEL_DEFAULT) == wxID_CANCEL);
    CHECK(redesign_dialog_default_result(wxOK | wxCANCEL) == wxID_OK);
    CHECK(redesign_dialog_default_result(wxOK | wxCANCEL | wxCANCEL_DEFAULT) == wxID_CANCEL);
}

TEST_CASE("The model gallery embeds only its installed page and delegates external links", "[UiRedesign][AssetsWorkspace]")
{
    CHECK(gallery_navigation("file", "", true) == GalleryNavigation::Embedded);
    CHECK(gallery_navigation("file", "localhost", true) == GalleryNavigation::Embedded);
    CHECK(gallery_navigation("file", "") == GalleryNavigation::Blocked);
    CHECK(gallery_navigation("file", "models.example.com", true) == GalleryNavigation::Blocked);
    CHECK(gallery_navigation("https", "arsenaltj.github.io") == GalleryNavigation::Browser);
    CHECK(gallery_navigation("https", "models.example.com") == GalleryNavigation::Browser);
    CHECK(gallery_navigation("https", "arsenaltj.github.io.example.com") == GalleryNavigation::Browser);
    for (const auto& scheme : {"file", "javascript", "bbl", "wxfs", "http", ""})
        CHECK(gallery_navigation(scheme, "arsenaltj.github.io") == GalleryNavigation::Blocked);
    CHECK(gallery_navigation("https", "") == GalleryNavigation::Blocked);
}

TEST_CASE("Saving the print project waits for import and unsaved model edits", "[UiRedesign][AssetsWorkspace]")
{
    using Status = PostGenerationUiState::Status;
    CHECK(print_project_save_available(true, false, false, false, Status::Ready));
    CHECK(print_project_save_available(true, false, false, false, Status::Empty));
    CHECK_FALSE(print_project_save_available(false, false, false, false, Status::Ready));
    CHECK_FALSE(print_project_save_available(true, true, false, false, Status::Ready));
    CHECK_FALSE(print_project_save_available(true, false, true, false, Status::Ready));
    CHECK_FALSE(print_project_save_available(true, false, false, true, Status::Ready));
    for (const auto status : {Status::Editing, Status::Processing, Status::CandidateReady, Status::ComparingBefore})
        CHECK_FALSE(print_project_save_available(true, false, false, false, status));
}

TEST_CASE("Image history pagination exposes each record exactly once", "[UiRedesign][ImageHistoryPagination]")
{
    ImageHistoryPagination pages;
    pages.set_count(23);
    REQUIRE(pages.pages() == 3);
    size_t next = 0;
    for (size_t page = 0; page < pages.pages(); ++page) {
        pages.go_to(page);
        CHECK(pages.begin() == next);
        for (size_t i = pages.begin(); i < pages.end(); ++i) {
            CHECK(pages.contains(i));
            ++next;
        }
        CHECK_FALSE(pages.contains(pages.end()));
    }
    CHECK(next == 23);
    pages.go_to(999);
    CHECK(pages.page() == 2);
    CHECK(pages.begin() == 20);
    CHECK_FALSE(pages.contains(0)); // Hit/keyboard actions must not target another page.
}

TEST_CASE("Removing the last image on the last page returns to a valid page", "[UiRedesign][ImageHistoryPagination]")
{
    ImageHistoryPagination pages;
    pages.set_count(21); pages.go_to(2);
    pages.set_count(20);
    CHECK(pages.page() == 1);
    CHECK(pages.begin() == 10);
    CHECK(pages.end() == 20);
    pages.set_count(0);
    CHECK(pages.pages() == 0);
    CHECK(pages.begin() == pages.end());
    CHECK_FALSE(pages.contains(0));
    pages.go_to(999);
    CHECK(pages.page() == 0);
}

TEST_CASE("Searching image history starts at the first matching page", "[UiRedesign][ImageHistoryPagination]")
{
    ImageHistoryPagination pages;
    pages.set_count(100); pages.go_to(8);
    pages.set_count(25, true);
    CHECK(pages.page() == 0);
    CHECK(pages.pages() == 3);
    pages.go_to(1);
    pages.set_count(25); // Reopening/refetching retains the user's page.
    CHECK(pages.page() == 1);
}

TEST_CASE("Resizing image history keeps the former first record on screen", "[UiRedesign][ImageHistoryPagination]")
{
    ImageHistoryPagination pages;
    pages.set_count(37); pages.go_to(2);
    pages.set_capacity(6);
    CHECK(pages.contains(20));
    CHECK(pages.pages() == 7);
    const size_t first = pages.begin();
    pages.set_capacity(10);
    CHECK(pages.contains(first));
    pages.set_capacity(0);
    CHECK(pages.capacity() == 1);
}

TEST_CASE("Image history fits whole square rows above the fixed pager", "[UiRedesign][ImageHistoryPagination]")
{
    const auto design = ImageHistoryGrid::fit(312, 798, 12);
    CHECK(design.rows == 5);
    CHECK(design.edge == 150);
    CHECK(ImageHistoryGrid::fit(312, 797, 12).rows == 4);
    for (int scale : {1, 2, 3}) {
        for (int height : {50, 150, 300, 620, 798, 1200}) {
            const auto grid = ImageHistoryGrid::fit(312 * scale, height * scale, 12 * scale);
            CHECK(grid.rows >= 1);
            CHECK(grid.rows <= 5);
            CHECK(grid.rows * grid.edge + (grid.rows - 1) * 12 * scale <= height * scale);
            CHECK(2 * grid.edge + 12 * scale <= 312 * scale);
        }
    }
}

namespace {

void set_environment(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value)
        setenv(name, value, 1);
    else
        unsetenv(name);
#endif
}

class ScopedEnvironment final
{
public:
    explicit ScopedEnvironment(const char* name) : m_name(name)
    {
        if (const char* value = std::getenv(name)) {
            m_had_value = true;
            m_value = value;
        }
    }

    ~ScopedEnvironment()
    {
        set_environment(m_name, m_had_value ? m_value.c_str() : nullptr);
    }

private:
    const char* m_name;
    bool m_had_value { false };
    std::string m_value;
};

}

TEST_CASE("Image home is the default surface with an explicit legacy fallback", "[UiRedesign]")
{
    ScopedEnvironment global("ORCASLICER_UI_REDESIGN");
    ScopedEnvironment image_home("ORCASLICER_UI_REDESIGN_IMAGE_HOME");
    set_environment("ORCASLICER_UI_REDESIGN", nullptr);
    set_environment("ORCASLICER_UI_REDESIGN_IMAGE_HOME", nullptr);
    CHECK(RedesignFeatureFlags::surface_enabled("IMAGE_HOME"));
    CHECK_FALSE(RedesignFeatureFlags::surface_enabled("OTHER"));

    set_environment("ORCASLICER_UI_REDESIGN_IMAGE_HOME", "0");
    CHECK_FALSE(RedesignFeatureFlags::surface_enabled("IMAGE_HOME"));
    set_environment("ORCASLICER_UI_REDESIGN_IMAGE_HOME", nullptr);
    set_environment("ORCASLICER_UI_REDESIGN", "0");
    CHECK_FALSE(RedesignFeatureFlags::surface_enabled("IMAGE_HOME"));
    set_environment("ORCASLICER_UI_REDESIGN_IMAGE_HOME", "1");
    CHECK(RedesignFeatureFlags::surface_enabled("IMAGE_HOME"));
}

TEST_CASE("Image home startup respects explicit 3D routes", "[UiRedesign]")
{
    ScopedEnvironment global("ORCASLICER_UI_REDESIGN");
    ScopedEnvironment shell("ORCASLICER_UI_REDESIGN_SHELL");
    ScopedEnvironment image_home("ORCASLICER_UI_REDESIGN_IMAGE_HOME");
    set_environment("ORCASLICER_UI_REDESIGN", nullptr);
    set_environment("ORCASLICER_UI_REDESIGN_SHELL", nullptr);
    set_environment("ORCASLICER_UI_REDESIGN_IMAGE_HOME", nullptr);

    const bool shell_active = RedesignFeatureFlags::shell_on_startup(true);
    CHECK(shell_active);
    CHECK_FALSE(RedesignFeatureFlags::shell_on_startup(false));
    CHECK(RedesignFeatureFlags::image_home_on_startup(shell_active, false, false));
    CHECK_FALSE(RedesignFeatureFlags::image_home_on_startup(false, false, false));
    CHECK_FALSE(RedesignFeatureFlags::image_home_on_startup(shell_active, true, false));
    CHECK_FALSE(RedesignFeatureFlags::image_home_on_startup(shell_active, false, true));

    set_environment("ORCASLICER_UI_REDESIGN_IMAGE_HOME", "0");
    CHECK(RedesignFeatureFlags::shell_on_startup(true));
    CHECK_FALSE(RedesignFeatureFlags::image_home_on_startup(true, false, false));
    set_environment("ORCASLICER_UI_REDESIGN_IMAGE_HOME", nullptr);
    set_environment("ORCASLICER_UI_REDESIGN", "0");
    CHECK_FALSE(RedesignFeatureFlags::shell_on_startup(true));
    set_environment("ORCASLICER_UI_REDESIGN_SHELL", "1");
    CHECK(RedesignFeatureFlags::shell_on_startup(true));
}

TEST_CASE("Startup splash is enabled by default with an explicit override", "[UiRedesign]")
{
    ScopedEnvironment global("ORCASLICER_UI_REDESIGN");
    ScopedEnvironment splash("ORCASLICER_UI_REDESIGN_STARTUP_SPLASH");
    set_environment("ORCASLICER_UI_REDESIGN", nullptr);
    set_environment("ORCASLICER_UI_REDESIGN_STARTUP_SPLASH", nullptr);
    CHECK(RedesignFeatureFlags::surface_enabled("STARTUP_SPLASH"));

    set_environment("ORCASLICER_UI_REDESIGN", "0");
    CHECK_FALSE(RedesignFeatureFlags::surface_enabled("STARTUP_SPLASH"));
    set_environment("ORCASLICER_UI_REDESIGN", nullptr);

    set_environment("ORCASLICER_UI_REDESIGN_STARTUP_SPLASH", "1");
    CHECK(RedesignFeatureFlags::surface_enabled("STARTUP_SPLASH"));
    CHECK(RedesignFeatureFlags::surface_enabled("IMAGE_HOME"));
    CHECK_FALSE(RedesignFeatureFlags::surface_enabled("OTHER"));

    set_environment("ORCASLICER_UI_REDESIGN", "0");
    CHECK(RedesignFeatureFlags::surface_enabled("STARTUP_SPLASH"));
    set_environment("ORCASLICER_UI_REDESIGN_STARTUP_SPLASH", "0");
    CHECK_FALSE(RedesignFeatureFlags::surface_enabled("STARTUP_SPLASH"));
}

TEST_CASE("Startup buffer defaults to the new layout and remains independently configurable", "[UiRedesign]")
{
    ScopedEnvironment global("ORCASLICER_UI_REDESIGN");
    ScopedEnvironment buffer("ORCASLICER_UI_REDESIGN_STARTUP_BUFFER");
    ScopedEnvironment splash("ORCASLICER_UI_REDESIGN_STARTUP_SPLASH");
    set_environment("ORCASLICER_UI_REDESIGN", nullptr);
    set_environment("ORCASLICER_UI_REDESIGN_STARTUP_BUFFER", nullptr);
    set_environment("ORCASLICER_UI_REDESIGN_STARTUP_SPLASH", nullptr);
    CHECK(RedesignFeatureFlags::surface_enabled("STARTUP_BUFFER"));
    set_environment("ORCASLICER_UI_REDESIGN_STARTUP_BUFFER", "1");
    CHECK(RedesignFeatureFlags::surface_enabled("STARTUP_BUFFER"));
    CHECK(RedesignFeatureFlags::surface_enabled("STARTUP_SPLASH"));
    CHECK(RedesignFeatureFlags::surface_enabled("IMAGE_HOME"));
    set_environment("ORCASLICER_UI_REDESIGN", "0");
    CHECK(RedesignFeatureFlags::surface_enabled("STARTUP_BUFFER"));
    CHECK_FALSE(RedesignFeatureFlags::surface_enabled("STARTUP_SPLASH"));
    set_environment("ORCASLICER_UI_REDESIGN_STARTUP_BUFFER", "0");
    CHECK_FALSE(RedesignFeatureFlags::surface_enabled("STARTUP_BUFFER"));
}

TEST_CASE("First-use setup is enabled by default and respects startup compatibility overrides", "[UiRedesign]")
{
    ScopedEnvironment global("ORCASLICER_UI_REDESIGN");
    ScopedEnvironment setup("ORCASLICER_UI_REDESIGN_STARTUP_SETUP");
    set_environment("ORCASLICER_UI_REDESIGN", nullptr);
    set_environment("ORCASLICER_UI_REDESIGN_STARTUP_SETUP", nullptr);
    CHECK(RedesignFeatureFlags::surface_enabled("STARTUP_SETUP"));
    set_environment("ORCASLICER_UI_REDESIGN_STARTUP_SETUP", "0");
    CHECK_FALSE(RedesignFeatureFlags::surface_enabled("STARTUP_SETUP"));
    set_environment("ORCASLICER_UI_REDESIGN_STARTUP_SETUP", nullptr);
    set_environment("ORCASLICER_UI_REDESIGN", "0");
    CHECK_FALSE(RedesignFeatureFlags::surface_enabled("STARTUP_SETUP"));
    set_environment("ORCASLICER_UI_REDESIGN_STARTUP_SETUP", "1");
    CHECK(RedesignFeatureFlags::surface_enabled("STARTUP_SETUP"));
}

TEST_CASE("The complete model workflow is enabled without a review launcher", "[UiRedesign]")
{
    ScopedEnvironment global("ORCASLICER_UI_REDESIGN");
    ScopedEnvironment workflow("ORCASLICER_UI_REDESIGN_MODEL_WORKFLOW");
    ScopedEnvironment review("ORCASLICER_MODEL_WORKFLOW_REVIEW");
    set_environment("ORCASLICER_UI_REDESIGN", nullptr);
    set_environment("ORCASLICER_UI_REDESIGN_MODEL_WORKFLOW", nullptr);
    set_environment("ORCASLICER_MODEL_WORKFLOW_REVIEW", nullptr);
    CHECK(RedesignFeatureFlags::model_workflow_review_enabled());
    set_environment("ORCASLICER_UI_REDESIGN", "0");
    CHECK_FALSE(RedesignFeatureFlags::model_workflow_review_enabled());
    set_environment("ORCASLICER_UI_REDESIGN_MODEL_WORKFLOW", "1");
    CHECK(RedesignFeatureFlags::model_workflow_review_enabled());
    set_environment("ORCASLICER_MODEL_WORKFLOW_REVIEW", "0");
    CHECK_FALSE(RedesignFeatureFlags::model_workflow_review_enabled());
    set_environment("ORCASLICER_UI_REDESIGN_MODEL_WORKFLOW", "0");
    set_environment("ORCASLICER_MODEL_WORKFLOW_REVIEW", "1");
    CHECK(RedesignFeatureFlags::model_workflow_review_enabled());
}

TEST_CASE("Redesign command registry executes registered commands", "[UiRedesign]")
{
    RedesignCommandRegistry registry;
    bool executed = false;

    REQUIRE(registry.register_command(
        RedesignCommandId::NewProject,
        [] { return true; },
        [&executed] { executed = true; }));
    CHECK(registry.contains(RedesignCommandId::NewProject));
    CHECK(registry.can_execute(RedesignCommandId::NewProject));
    CHECK(registry.execute(RedesignCommandId::NewProject));
    CHECK(executed);
    CHECK_FALSE(registry.execute(RedesignCommandId::OpenProject));
    CHECK_FALSE(registry.register_command(
        RedesignCommandId::NewProject,
        [] { return true; },
        [] {}));
}

TEST_CASE("Redesign command registry blocks commands that cannot execute", "[UiRedesign]")
{
    RedesignCommandRegistry registry;
    bool executed = false;

    REQUIRE(registry.register_command(
        RedesignCommandId::StartSlice,
        [] { return false; },
        [&executed] { executed = true; }));
    CHECK_FALSE(registry.can_execute(RedesignCommandId::StartSlice));
    CHECK_FALSE(registry.execute(RedesignCommandId::StartSlice));
    CHECK_FALSE(executed);
}

TEST_CASE("Redesign state store publishes monotonic snapshots", "[UiRedesign]")
{
    RedesignStateStore store;
    RedesignStateSnapshot received;
    int notifications = 0;
    store.subscribe([&](const RedesignStateSnapshot& snapshot) {
        received = snapshot;
        ++notifications;
    });

    RedesignStateSnapshot first;
    first.project_open = true;
    first.project_dirty = true;
    store.publish(first);
    CHECK(store.snapshot().revision == 1);
    CHECK(received.project_dirty);

    RedesignStateSnapshot second;
    second.revision = 1;
    second.slice_status = RedesignSliceStatus::Slicing;
    store.publish(second);
    CHECK(store.snapshot().revision == 2);
    CHECK(store.snapshot().slice_status == RedesignSliceStatus::Slicing);
    CHECK(notifications == 2);
}

TEST_CASE("Orca business adapter stays inert before it is attached", "[UiRedesign]")
{
    OrcaBusinessAdapter adapter;
    RedesignCommandRegistry registry;

    CHECK_FALSE(adapter.attached());
    adapter.register_commands(registry);
    CHECK(registry.size() == 0);

    const RedesignStateSnapshot state = adapter.snapshot();
    CHECK_FALSE(state.project_open);
    CHECK_FALSE(state.project_dirty);
    CHECK_FALSE(state.has_selection);
    CHECK_FALSE(state.can_undo);
    CHECK_FALSE(state.can_redo);
    CHECK(state.slice_status == RedesignSliceStatus::Idle);
}

TEST_CASE("Model generation snapshots distinguish styles within the same family", "[UiRedesign]")
{
    ModelGenerationUIState current;
    current.input.style = "cartoon";
    current.input.prompt = "a small cat";
    for (const std::string style : {"portrait_sketch", "low_poly", "relief", "ink_relief", "diorama", "custom"}) {
        DYNAMIC_SECTION(style) {
            auto updated = current;
            updated.input.style = style;
            CHECK_FALSE(current.input == updated.input);
            CHECK_FALSE(current.same_content(updated));
            current = updated;
            CHECK(current.same_content(updated));
        }
    }
}

TEST_CASE("Design timing changes are published even when progress stays unchanged", "[UiRedesign][DesignGenerationTiming]")
{
    ModelGenerationUIState current;
    auto updated = current;
    updated.design_elapsed_seconds = 12;
    CHECK_FALSE(current.same_content(updated));
    current = updated;
    CHECK(current.same_content(updated));
    updated.design_estimated_seconds = 60;
    CHECK_FALSE(current.same_content(updated));
}

TEST_CASE("Displayed model asset changes are published without an active generation job", "[UiRedesign][ModelGenerationRoute]")
{
    ModelGenerationUIState current;
    auto updated = current;
    updated.model_asset_id = "historical-model";
    CHECK_FALSE(current.same_content(updated));
    current = updated;
    CHECK(current.same_content(updated));
}

TEST_CASE("Workbench navigation remains available while model import is gated", "[UiRedesign][ModelGenerationRoute]")
{
    ModelGenerationUIState current;
    current.model_ready = true;
    current.can_import = false; // An unaccepted check/Beauty candidate is pending.
    current.can_open_workbench = true;
    CHECK_FALSE(current.same_content(ModelGenerationUIState{}));

    auto updated = current;
    updated.can_open_workbench = false;
    CHECK_FALSE(current.same_content(updated));
}

TEST_CASE("Custom style edits publish new snapshots while preserving the unfinished draft", "[UiRedesign]")
{
    ModelGenerationUIState current;
    current.input.style = "custom";
    current.input.custom_style = "soft clay";
    for (const std::string draft : {"", "soft clay ", "soft clay\nwith broad shapes"}) {
        DYNAMIC_SECTION(draft) {
            auto updated = current;
            updated.input.custom_style = draft;
            CHECK_FALSE(current.input == updated.input);
            CHECK_FALSE(current.same_content(updated));
            // Leaving the custom family keeps its draft available when switching back.
            auto another_style = updated;
            another_style.input.style = "low_poly";
            CHECK_FALSE(updated.same_content(another_style));
            another_style.input.style = "custom";
            CHECK(updated.same_content(another_style));
            CHECK(another_style.input.custom_style == draft);
        }
    }
}

TEST_CASE("Provider failures publish alongside unchanged design timing", "[UiRedesign][ModelGenerationSubmissionState]")
{
    ModelGenerationUIState current;
    current.design_elapsed_seconds = 120;
    current.design_estimated_seconds = 60;
    auto failed = current;
    failed.provider_error_code = "image_connection_failed";
    failed.provider_error_category = "image_preprocessing";
    failed.provider_error_retryable = true;
    failed.provider_error_ambiguous = true;
    CHECK_FALSE(current.same_content(failed));
    CHECK(failed.design_elapsed_seconds == current.design_elapsed_seconds);
    current = failed;
    failed.provider_error_ambiguous = false;
    CHECK_FALSE(current.same_content(failed));
}

TEST_CASE("Local design history replaces a locked model session without reusing its task", "[UiRedesign][ModelGenerationRoute]")
{
    ModelGenerationUIState current;
    current.revision = 10;
    current.model_generation_session = 3;
    current.model_generation_context = true;
    current.job_id = "previous-model";
    current.stage = ModelGenerationUIStage::ModelReady;
    auto design = current;
    ++design.revision;
    design.model_generation_context = false;
    design.stage = ModelGenerationUIStage::DesignReady;
    design.job_id = "selected-design";
    CHECK(model_generation_route_action(design, current, true, 3, current.job_id) == RedesignModelRouteAction::Ignore);
    ++design.model_generation_session;
    CHECK(model_generation_route_action(design, current, true, 3, current.job_id) == RedesignModelRouteAction::Image);
    CHECK(model_generation_route_action(current, design, false, 0, {}) == RedesignModelRouteAction::Ignore);
}

TEST_CASE("A design stays on its page until a confirmed model submission starts", "[UiRedesign][ModelGenerationRoute]")
{
    ModelGenerationUIState design;
    design.revision = 10;
    design.model_generation_session = 3;
    design.job_id = "current-design";
    design.stage = ModelGenerationUIStage::DesignReady;
    // Dismissing confirmation leaves the host state unchanged.
    CHECK(model_generation_route_action(design, design, false, 3, design.job_id) == RedesignModelRouteAction::Refresh);

    auto submitted = design;
    ++submitted.revision;
    ++submitted.model_generation_session;
    submitted.model_generation_context = true;
    submitted.busy = true;
    submitted.stage = ModelGenerationUIStage::GeneratingModel;
    CHECK(model_generation_route_action(submitted, design, false, 3, design.job_id) == RedesignModelRouteAction::Model);
    CHECK(model_generation_route_action(design, submitted, true, 4, submitted.job_id) == RedesignModelRouteAction::Ignore);
}

TEST_CASE("History preparation stays on the image page and routes only its confirmed child task", "[UiRedesign][ModelGenerationRoute]")
{
    ModelGenerationUIState design;
    design.revision = 10;
    design.model_generation_session = 3;
    design.job_id = "history-design";
    design.stage = ModelGenerationUIStage::DesignReady;
    auto preparing = design;
    ++preparing.revision;
    preparing.busy = true;
    CHECK(model_generation_route_action(preparing, design, false, 3, design.job_id) == RedesignModelRouteAction::Refresh);

    auto failed = preparing;
    ++failed.revision;
    failed.busy = false;
    CHECK(model_generation_route_action(failed, preparing, false, 3, design.job_id) == RedesignModelRouteAction::Refresh);

    auto submitted = preparing;
    ++submitted.revision;
    ++submitted.model_generation_session;
    submitted.job_id = "new-child-task";
    submitted.model_generation_context = true;
    submitted.stage = ModelGenerationUIStage::GeneratingModel;
    CHECK(model_generation_route_action(submitted, preparing, false, 3, design.job_id) == RedesignModelRouteAction::Model);

    // A late preparation notification must not undo the child task's route.
    preparing.revision = submitted.revision + 1;
    CHECK(model_generation_route_action(preparing, submitted, true, 4, submitted.job_id) == RedesignModelRouteAction::Ignore);
}

TEST_CASE("A restored failed model opens its result page after leaving a history workbench", "[UiRedesign][ModelGenerationRoute]")
{
    ModelGenerationUIState current;
    current.revision = 20;
    current.model_generation_session = 3;
    current.model_generation_context = true;
    current.job_id = "completed-model";
    current.stage = ModelGenerationUIStage::ModelReady;
    auto restored = current;
    restored.revision = 21;
    restored.model_generation_session = 4;
    restored.job_id = "failed-model";
    restored.stage = ModelGenerationUIStage::Failed;
    CHECK(model_generation_route_action(restored, current, true, 3, current.job_id) == RedesignModelRouteAction::Model);

    restored.model_generation_context = false;
    restored.stage = ModelGenerationUIStage::DesignReady;
    CHECK(model_generation_route_action(restored, current, true, 3, current.job_id) == RedesignModelRouteAction::Image);

    restored.job_id.clear();
    CHECK(model_generation_route_action(restored, current, true, 3, current.job_id) == RedesignModelRouteAction::Ignore);
}

TEST_CASE("A historical model routes by its asset identity without an active generation job", "[UiRedesign][ModelGenerationRoute]")
{
    ModelGenerationUIState current;
    current.revision = 20;
    current.model_generation_session = 3;
    current.model_generation_context = true;
    current.job_id = "completed-model";
    current.model_asset_id = current.job_id;
    current.stage = ModelGenerationUIStage::ModelReady;

    auto restored = current;
    restored.revision = 21;
    restored.model_generation_session = 4;
    restored.job_id.clear();
    restored.model_asset_id = "historical-model";
    const auto restored_action = model_generation_route_action(restored, current, true, 3,
        model_generation_route_id(current));
    CHECK(restored_action == RedesignModelRouteAction::Model);
    CHECK(model_generation_preserves_workbench(restored, restored_action, true));
    CHECK_FALSE(model_generation_preserves_workbench(restored, restored_action, false));

    auto stale = restored;
    ++stale.revision;
    stale.model_asset_id = "different-model";
    CHECK(model_generation_route_action(stale, restored, true, 4,
        model_generation_route_id(restored)) == RedesignModelRouteAction::Ignore);
}

TEST_CASE("A newly submitted model still leaves the design page for its result view", "[UiRedesign][ModelGenerationRoute]")
{
    ModelGenerationUIState design;
    design.revision = 10;
    design.model_generation_session = 3;
    design.job_id = "design-task";
    design.stage = ModelGenerationUIStage::DesignReady;
    auto submitted = design;
    ++submitted.revision;
    ++submitted.model_generation_session;
    submitted.stage = ModelGenerationUIStage::GeneratingModel;
    submitted.model_generation_context = true;
    submitted.job_id = "new-model-task";
    const auto action = model_generation_route_action(
        submitted, design, false, design.model_generation_session, design.job_id);
    CHECK(action == RedesignModelRouteAction::Model);
    const bool preserves_workbench = model_generation_preserves_workbench(submitted, action, true);
    CHECK_FALSE(preserves_workbench);
    CHECK(model_generation_route_revokes_workbench_presentation(action, preserves_workbench));

    WorkbenchCheckResult running_check;
    running_check.source = WorkbenchCheckSource::Automatic;
    running_check.status = WorkbenchCheckStatus::Running;
    running_check.phase = WorkbenchCheckPhase::Inspecting;
    CHECK_FALSE(workbench_automatic_check_should_start(true, false, true));
    CHECK_FALSE(workbench_check_card_should_show(
        true, false, true, true, false, running_check));

    // Result-page polls do not change presentation ownership. Only an explicit
    // workbench page commit can set it again.
    CHECK_FALSE(model_generation_route_revokes_workbench_presentation(
        RedesignModelRouteAction::Refresh, false));
}

TEST_CASE("Saved model versions keep an already presented workbench", "[UiRedesign][ModelGenerationRoute]")
{
    ModelGenerationUIState saved;
    saved.stage = ModelGenerationUIStage::ModelReady;
    saved.model_generation_context = true;
    saved.model_asset_id = "saved-version";
    const bool preserves_workbench = model_generation_preserves_workbench(
        saved, RedesignModelRouteAction::Model, true);
    CHECK(preserves_workbench);
    CHECK_FALSE(model_generation_route_revokes_workbench_presentation(
        RedesignModelRouteAction::Model, preserves_workbench));
}

TEST_CASE("Returning to image design is available only after model work is stable", "[UiRedesign][ModelGenerationRoute]")
{
    ModelGenerationUIState state;
    state.stage = ModelGenerationUIStage::ModelReady;
    PostGenerationUiState workbench;
    workbench.status = PostGenerationUiState::Status::Ready;
    CHECK(model_generation_return_to_design_allowed(state, workbench, false));

    for (const auto stage : {ModelGenerationUIStage::Saving3DOptions,
                             ModelGenerationUIStage::GeneratingDesign,
                             ModelGenerationUIStage::Stopping,
                             ModelGenerationUIStage::GeneratingModel,
                             ModelGenerationUIStage::LoadingModel}) {
        state.stage = stage;
        CHECK_FALSE(model_generation_return_to_design_allowed(state, workbench, false));
    }

    state.stage = ModelGenerationUIStage::ModelReady;
    state.busy = true;
    CHECK_FALSE(model_generation_return_to_design_allowed(state, workbench, false));
    state.busy = false;
    workbench.status = PostGenerationUiState::Status::Processing;
    CHECK_FALSE(model_generation_return_to_design_allowed(state, workbench, false));
    workbench.status = PostGenerationUiState::Status::Loading;
    CHECK_FALSE(model_generation_return_to_design_allowed(state, workbench, false));
    workbench.status = PostGenerationUiState::Status::Ready;
    CHECK_FALSE(model_generation_return_to_design_allowed(state, workbench, true));
}

TEST_CASE("An explicit return ignores same-session model refreshes but a new model session still routes", "[UiRedesign][ModelGenerationRoute]")
{
    ModelGenerationUIState model;
    model.revision = 20;
    model.model_generation_session = 3;
    model.model_generation_context = true;
    model.job_id = "completed-model";
    model.stage = ModelGenerationUIStage::ModelReady;

    auto same_session = model;
    ++same_session.revision;
    CHECK(model_generation_route_action(same_session, model, false, 0, {}) == RedesignModelRouteAction::Refresh);

    auto new_session = same_session;
    ++new_session.revision;
    ++new_session.model_generation_session;
    new_session.job_id = "new-model";
    new_session.stage = ModelGenerationUIStage::GeneratingModel;
    CHECK(model_generation_route_action(new_session, same_session, false, 0, {}) == RedesignModelRouteAction::Model);
}

TEST_CASE("Old revisions and task identities cannot replace the selected model route", "[UiRedesign][ModelGenerationRoute]")
{
    ModelGenerationUIState current;
    current.revision = 20;
    current.model_generation_session = 3;
    current.model_generation_context = true;
    current.job_id = "active-model";
    auto stale = current;
    stale.revision = 19;
    CHECK(model_generation_route_action(stale, current, true, 3, current.job_id) == RedesignModelRouteAction::Ignore);

    stale.revision = 21;
    stale.model_generation_session = 2;
    CHECK(model_generation_route_action(stale, current, true, 3, current.job_id) == RedesignModelRouteAction::Ignore);

    stale.model_generation_session = 3;
    stale.job_id = "previous-model";
    CHECK(model_generation_route_action(stale, current, true, 3, current.job_id) == RedesignModelRouteAction::Ignore);

    stale.job_id = current.job_id;
    stale.model_generation_context = false;
    CHECK(model_generation_route_action(stale, current, true, 3, current.job_id) == RedesignModelRouteAction::Ignore);
}

TEST_CASE("Task updates preserve the selected model view across failure and recovery", "[UiRedesign][ModelGenerationRoute]")
{
    ModelGenerationUIState current;
    current.model_generation_session = 3;
    current.model_generation_context = true;
    current.job_id = "active-model";
    auto update = current;
    update.revision = 1;
    for (const auto stage : {ModelGenerationUIStage::GeneratingModel, ModelGenerationUIStage::Failed,
                            ModelGenerationUIStage::Stopped, ModelGenerationUIStage::ModelReady}) {
        update.stage = stage;
        CHECK(model_generation_route_action(update, current, true, 3, current.job_id) == RedesignModelRouteAction::Refresh);
    }
}

TEST_CASE("Returning to the design page is preserved until a new model session starts", "[UiRedesign][ModelGenerationRoute]")
{
    ModelGenerationUIState current;
    current.model_generation_session = 3;
    current.model_generation_context = true;
    current.job_id = "active-model";
    auto update = current;
    update.revision = 1;
    CHECK(model_generation_route_action(update, current, false, 0, {}) == RedesignModelRouteAction::Refresh);

    ++update.model_generation_session;
    CHECK(model_generation_route_action(update, current, false, 0, {}) == RedesignModelRouteAction::Model);
}

TEST_CASE("Reanalysis stays hidden until current analysis and estimate reach a terminal state", "[UiRedesign][CloudRecommendationPresentation]")
{
    SmartSlicingWorkbenchState state;
    state.lightweight_analysis.emplace();
    state.lightweight_analysis->completed = true;
    state.can_analyze = true;
    state.recommendation_source = SlicingRecommendationSource::Cloud;
    state.cloud_recommendation.status = CloudRecommendationStatus::Requesting;
    state.analyzing = true;
    CHECK_FALSE(workbench_recommendation_parameters_ready(state));
    CHECK_FALSE(workbench_show_reanalyze(state));
    state.cloud_recommendation.status = CloudRecommendationStatus::Ready;
    state.analyzing = false;
    CHECK(workbench_recommendation_parameters_ready(state));
    CHECK_FALSE(workbench_show_reanalyze(state));
    state.exact_estimate.status = ExactEstimateStatus::Running;
    CHECK_FALSE(workbench_show_reanalyze(state));
    state.exact_estimate.status = ExactEstimateStatus::Ready;
    state.exact_estimate.parameters = state.lightweight_analysis->recommendations[0];
    CHECK(workbench_show_reanalyze(state));
    state.exact_estimate.status = ExactEstimateStatus::Failed;
    CHECK(workbench_show_reanalyze(state));
    state.cloud_recommendation.status = CloudRecommendationStatus::Failed;
    state.exact_estimate = {};
    CHECK(workbench_show_reanalyze(state));
    state.recommendation_source = SlicingRecommendationSource::Local;
    state.cloud_recommendation = {};
    CHECK_FALSE(workbench_show_reanalyze(state));
    state.exact_estimate.status = ExactEstimateStatus::Ready;
    state.exact_estimate.parameters = state.lightweight_analysis->recommendations[0];
    CHECK(workbench_show_reanalyze(state));
}

TEST_CASE("Changed recommendation parameters hide reanalysis until their own estimate finishes", "[UiRedesign][CloudRecommendationPresentation]")
{
    SmartSlicingWorkbenchState state;
    state.session.workspace_revision = {1, 2, 3, "current"};
    state.lightweight_analysis.emplace();
    state.lightweight_analysis->completed = true;
    auto& parameters = state.lightweight_analysis->recommendations[0];
    state.exact_estimate.status = ExactEstimateStatus::Ready;
    state.exact_estimate.revision = state.session.workspace_revision;
    state.exact_estimate.parameters = parameters;
    CHECK(workbench_show_reanalyze(state));
    parameters.layer_height_mm += 0.04;
    CHECK_FALSE(workbench_show_reanalyze(state));
    state.exact_estimate.parameters = parameters;
    CHECK(workbench_show_reanalyze(state));
    state.lightweight_analyzing = true;
    state.analyzing = true;
    CHECK_FALSE(workbench_show_reanalyze(state));
}
