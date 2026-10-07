#include <catch2/catch_all.hpp>

#include <algorithm>
#include <array>
#include <functional>
#include <stdexcept>

#include "slic3r/AI/SmartSlicing/Application/SmartSlicingCoordinator.hpp"
#include "slic3r/GUI/AI/SmartSlicing/SmartSlicingViewModel.hpp"
#include "slic3r/GUI/AI/SmartSlicing/SmartSlicingPresenter.hpp"
#include "slic3r/GUI/AI/Orca/OrcaSmartSlicingAdapter.hpp"

using namespace Slic3r::AI::SmartSlicing;

namespace {

WorkspaceContext context_with_revision(std::string fingerprint)
{
    WorkspaceContext context;
    context.revision          = {1, 2, 3, std::move(fingerprint)};
    context.plate_index       = 0;
    context.printer_preset_id = "printer";
    context.process_preset_id = "process";
    context.objects.push_back({42, "cube", 1, 12, 0, false});
    context.machine_capability.registry_version = "test-machine-registry";
    context.machine_capability.support_status = MachineSupportStatus::Enabled;
    context.material_compatibility.registry_version = "test-material-registry";
    context.material_compatibility.combination_status = MaterialCombinationStatus::Compatible;
    context.material_compatibility.common_family = MaterialFamily::PLA;
    context.native_validation_available = true;
    return context;
}

class FakeWorkspace final : public IOrcaWorkspace
{
public:
    WorkspaceContext context = context_with_revision("revision-a");
    mutable size_t capture_count{0};
    bool throw_on_revision{false};
    bool throw_on_capture{false};

    WorkspaceRevision current_revision() const override
    {
        if (throw_on_revision)
            throw std::runtime_error("revision unavailable");
        return context.revision;
    }

    WorkspaceContext capture_context() const override
    {
        if (throw_on_capture)
            throw std::runtime_error("capture unavailable");
        ++capture_count;
        return context;
    }
};

} // namespace

TEST_CASE("smart slicing coordinator captures and preflights without applying workspace changes", "[AI][SmartSlicing]")
{
    FakeWorkspace workspace;
    workspace.context.materials.push_back({"material", "#FFFFFF"});
    SmartSlicingCoordinator coordinator(workspace);

    coordinator.start();

    CHECK(workspace.capture_count == 1);
    CHECK(coordinator.snapshot().state == WorkflowState::ReadyForCandidatePlanning);
    REQUIRE(coordinator.snapshot().report);
    CHECK(coordinator.snapshot().report->revision == workspace.context.revision);
}

TEST_CASE("blocking printability issues require a decision", "[AI][SmartSlicing]")
{
    FakeWorkspace workspace;
    workspace.context.materials.push_back({"material", "#FFFFFF"});
    workspace.context.objects.front().open_edge_count = 3;
    SmartSlicingCoordinator coordinator(workspace);

    coordinator.start();

    CHECK(coordinator.snapshot().state == WorkflowState::AwaitingRiskDecision);
    CHECK(coordinator.snapshot().report->has_blocking_issue());
}

TEST_CASE("smart slicing coordinator supports cancel and restart", "[AI][SmartSlicing]")
{
    FakeWorkspace workspace;
    workspace.context.materials.push_back({"material", "#FFFFFF"});
    SmartSlicingCoordinator coordinator(workspace);
    coordinator.start();

    coordinator.cancel();
    CHECK(coordinator.snapshot().state == WorkflowState::Canceled);
    CHECK(coordinator.snapshot().can_start());

    coordinator.start();
    CHECK(coordinator.snapshot().workflow_id == 2);
    CHECK(coordinator.snapshot().state == WorkflowState::ReadyForCandidatePlanning);
}

TEST_CASE("keeping a reviewed open mesh retains its warning and unlocks planning", "[SmartSlicing][SmartSlicingPreflight]")
{
    FakeWorkspace workspace;
    workspace.context.materials.push_back({"material", "#FFFFFF"});
    workspace.context.objects.front().open_edge_count = 3;
    SmartSlicingCoordinator coordinator(workspace);
    coordinator.start();
    const auto revision = coordinator.snapshot().report->revision;
    REQUIRE(coordinator.keep_current_mesh(revision));
    CHECK(coordinator.snapshot().state == WorkflowState::ReadyForCandidatePlanning);
    REQUIRE(coordinator.snapshot().report);
    CHECK_FALSE(coordinator.snapshot().report->has_blocking_issue());
    CHECK(coordinator.snapshot().report->readiness == Readiness::NeedsAttention);
    CHECK(coordinator.snapshot().report->issues.front().code == IssueCode::OpenMesh);
    CHECK(workspace.context.objects.front().open_edge_count == 3);
    CHECK_FALSE(coordinator.keep_current_mesh(revision));
    coordinator.cancel();
    coordinator.start();
    CHECK(coordinator.snapshot().report->has_blocking_issue());
}

TEST_CASE("a mesh decision cannot acknowledge a changed engineering revision", "[SmartSlicing][SmartSlicingPreflight]")
{
    FakeWorkspace workspace;
    workspace.context.materials.push_back({"material", "#FFFFFF"});
    workspace.context.objects.front().open_edge_count = 3;
    SmartSlicingCoordinator coordinator(workspace);
    coordinator.start();
    auto revision = coordinator.snapshot().report->revision;
    revision.fingerprint = "other";
    CHECK_FALSE(coordinator.keep_current_mesh(revision));
    CHECK(coordinator.snapshot().report->has_blocking_issue());
    revision = coordinator.snapshot().report->revision;
    workspace.context.revision.fingerprint = "changed";
    CHECK_FALSE(coordinator.keep_current_mesh(revision));
    CHECK(coordinator.snapshot().state == WorkflowState::Stale);
    CHECK(coordinator.snapshot().report->has_blocking_issue());
}

TEST_CASE("a mesh decision cannot bypass incompatible material or misplaced objects", "[SmartSlicing][SmartSlicingPreflight]")
{
    FakeWorkspace workspace;
    workspace.context.materials.push_back({"material", "#FFFFFF"});
    workspace.context.objects.front().open_edge_count = 3;
    const bool material_block = GENERATE(false, true);
    if (material_block) workspace.context.material_compatibility.combination_status = MaterialCombinationStatus::Unsupported;
    else workspace.context.objects.front().outside_build_volume = true;
    SmartSlicingCoordinator coordinator(workspace);
    coordinator.start();
    CHECK_FALSE(coordinator.keep_current_mesh(coordinator.snapshot().report->revision));
    CHECK(coordinator.snapshot().state == WorkflowState::AwaitingRiskDecision);
    CHECK(coordinator.snapshot().report->has_blocking_issue());
}

TEST_CASE("a mesh decision fails closed when the current revision is unavailable", "[SmartSlicing][SmartSlicingPreflight]")
{
    FakeWorkspace workspace;
    workspace.context.materials.push_back({"material", "#FFFFFF"});
    workspace.context.objects.front().open_edge_count = 3;
    SmartSlicingCoordinator coordinator(workspace);
    coordinator.start();
    workspace.throw_on_revision = true;
    CHECK_FALSE(coordinator.keep_current_mesh(coordinator.snapshot().report->revision));
    CHECK(coordinator.snapshot().state == WorkflowState::Failed);
    CHECK(coordinator.snapshot().report->has_blocking_issue());
}

TEST_CASE("revision refresh makes an existing workflow stale", "[AI][SmartSlicing]")
{
    FakeWorkspace workspace;
    workspace.context.materials.push_back({"material", "#FFFFFF"});
    SmartSlicingCoordinator coordinator(workspace);
    coordinator.start();

    workspace.context = context_with_revision("revision-b");
    CHECK(coordinator.refresh_revision());
    CHECK(coordinator.snapshot().state == WorkflowState::Stale);
    REQUIRE(coordinator.snapshot().context);
    CHECK(coordinator.snapshot().context->revision.fingerprint == "revision-a");
}

TEST_CASE("four-stage view model and legacy projection share coordinator state", "[AI][SmartSlicing]")
{
    FakeWorkspace workspace;
    workspace.context.materials.push_back({"material", "#FFFFFF"});
    SmartSlicingCoordinator coordinator(workspace);
    coordinator.start();

    const Slic3r::GUI::SmartSlicingViewModel view = Slic3r::GUI::SmartSlicingViewModel::from_snapshot(coordinator.snapshot());
    CHECK(view.stages[0].status == Slic3r::GUI::SmartSlicingStageStatus::Complete);
    CHECK(view.stages[1].status == Slic3r::GUI::SmartSlicingStageStatus::Complete);
    CHECK(view.stages[2].status == Slic3r::GUI::SmartSlicingStageStatus::Disabled);
    CHECK(view.legacy_steps[1] == Slic3r::GUI::LegacyAIWorkflowStatus::Success);
    CHECK(view.legacy_steps[4] == Slic3r::GUI::LegacyAIWorkflowStatus::Waiting);
}

TEST_CASE("minimum printability report uses stable issue codes", "[AI][SmartSlicing]")
{
    WorkspaceContext context = context_with_revision("revision-a");
    context.materials.push_back({"material", "#FFFFFF"});
    context.validation_warnings.push_back("Existing Print validation warning.");

    const PrintabilityReport report = PrintabilityInspector().inspect(context);

    REQUIRE(report.issues.size() == 1);
    CHECK(report.issues.front().code == IssueCode::ConfigurationValidationWarning);
    CHECK(std::string(issue_code_name(report.issues.front().code)) == "configuration_validation_warning");
    CHECK(report.readiness == Readiness::NeedsAttention);
    CHECK_FALSE(report.has_blocking_issue());
}

TEST_CASE("recommendation goals have stable identifiers and reject unknown identifiers", "[AI][SmartSlicing]")
{
    const RecommendationRequest default_request;
    CHECK(default_request.contract_version == 1);
    CHECK(default_request.purpose == UsagePurpose::General);
    CHECK(default_request.requested_goal_ids == std::vector<std::string>{"balanced", "speed", "quality"});

    for (const RecommendationGoal goal : RECOMMENDATION_GOALS) {
        const std::string goal_id = recommendation_goal_id(goal);
        const RecommendationGoalParseResult parsed = parse_recommendation_goal_id(goal_id);
        REQUIRE(parsed);
        CHECK(*parsed.goal == goal);
        CHECK_FALSE(parsed.error);
    }

    for (const std::string_view unknown : {"stability", "material_saving", "unknown"}) {
        const RecommendationGoalParseResult rejected = parse_recommendation_goal_id(unknown);
        CHECK_FALSE(rejected);
        REQUIRE(rejected.error);
        CHECK(*rejected.error == RecommendationErrorCode::UnsupportedGoalId);
    }
    CHECK(std::string(recommendation_error_code_name(RecommendationErrorCode::UnsupportedGoalId)) ==
          "unsupported_goal_id");
}

TEST_CASE("recommendation view keeps the baseline separate from three fixed independent goal slots",
          "[AI][SmartSlicing][ViewModel][D5T2]")
{
    WorkflowSnapshot snapshot;
    snapshot.recommendation.emplace();
    snapshot.recommendation->baseline = {"baseline", GoalResultStatus::Ready, {}};
    snapshot.recommendation->goal_result(RecommendationGoal::Balanced).status = GoalResultStatus::Ready;
    snapshot.recommendation->goal_result(RecommendationGoal::Balanced).candidate_id = "balanced-task";
    snapshot.recommendation->goal_result(RecommendationGoal::Balanced).selected_candidate_id = "balanced-1";
    RecommendationEvidence evidence;
    evidence.selection_policy_version = "candidate-selection-policy/v1";
    evidence.explanation_codes = {"estimated_time_reduced"};
    evidence.estimated_time_ratio = {EvidenceAvailability::Available, 0.9, "orca_estimated_time_ratio"};
    snapshot.recommendation->goal_result(RecommendationGoal::Balanced).evidence = evidence;
    snapshot.recommendation->goal_result(RecommendationGoal::Speed).status = GoalResultStatus::Unavailable;
    snapshot.recommendation->goal_result(RecommendationGoal::Speed).diagnostic_codes = {"no_meaningful_candidate"};
    snapshot.recommendation->goal_result(RecommendationGoal::Quality).status = GoalResultStatus::Failed;
    snapshot.recommendation->goal_result(RecommendationGoal::Quality).evidence = evidence;

    const Slic3r::GUI::SmartSlicingViewModel view = Slic3r::GUI::SmartSlicingViewModel::from_snapshot(snapshot);

    CHECK(view.has_recommendation_contract);
    CHECK(view.recommendation_contract_version == 1);
    CHECK(view.baseline.candidate_id == "baseline");
    CHECK(view.baseline.status == GoalResultStatus::Ready);
    REQUIRE(view.goal_results.size() == 3);
    CHECK(view.goal_results[0].goal_id == "balanced");
    CHECK(view.goal_results[0].task_candidate_id == "balanced-task");
    CHECK(view.goal_results[0].candidate_id == "balanced-1");
    CHECK(view.goal_results[0].status == GoalResultStatus::Ready);
    REQUIRE(view.goal_results[0].evidence);
    REQUIRE(view.goal_results[0].evidence->estimated_time_ratio.value);
    CHECK(*view.goal_results[0].evidence->estimated_time_ratio.value == Catch::Approx(0.9));
    CHECK(view.goal_results[1].goal_id == "speed");
    CHECK(view.goal_results[1].status == GoalResultStatus::Unavailable);
    CHECK(view.goal_results[1].diagnostic_codes == std::vector<std::string>{"no_meaningful_candidate"});
    CHECK(view.goal_results[2].goal_id == "quality");
    CHECK(view.goal_results[2].status == GoalResultStatus::Failed);
    CHECK_FALSE(view.goal_results[2].evidence.has_value());

    snapshot.recommendation->goal_result(RecommendationGoal::Balanced).status = GoalResultStatus::Applied;
    snapshot.recommendation->goal_result(RecommendationGoal::Speed).status = GoalResultStatus::Stale;
    snapshot.recommendation->goal_result(RecommendationGoal::Quality).status = GoalResultStatus::Analyzing;
    const auto changed = Slic3r::GUI::SmartSlicingViewModel::from_snapshot(snapshot);
    CHECK(changed.goal_results[0].status == GoalResultStatus::Applied);
    CHECK(changed.goal_results[1].status == GoalResultStatus::Stale);
    CHECK(changed.goal_results[2].status == GoalResultStatus::Analyzing);
    CHECK(changed.goal_results[0].evidence.has_value());
    CHECK_FALSE(changed.goal_results[2].evidence.has_value());
}

TEST_CASE("D7 view model exposes stable mode purpose baseline and progressive goal states",
          "[AI][SmartSlicing][ViewModel][D7T1]")
{
    WorkflowSnapshot snapshot;
    snapshot.candidates.push_back(SliceCandidate{});
    snapshot.candidates.front().id = "baseline";
    snapshot.recommendation.emplace();
    snapshot.recommendation->baseline = {"baseline", GoalResultStatus::Ready, {}};
    snapshot.recommendation->goal_result(RecommendationGoal::Quality).status = GoalResultStatus::Ready;
    snapshot.recommendation->goal_result(RecommendationGoal::Quality).selected_candidate_id = "quality-1";
    snapshot.recommendation->goal_result(RecommendationGoal::Speed).status = GoalResultStatus::Unavailable;
    snapshot.recommendation->goal_result(RecommendationGoal::Speed).diagnostic_codes = {"unsupported_machine_profile"};
    snapshot.recommendation->goal_result(RecommendationGoal::Balanced).status = GoalResultStatus::Failed;

    const auto view = Slic3r::GUI::SmartSlicingViewModel::from_snapshot(snapshot);
    CHECK(view.mode == Slic3r::GUI::SmartSlicingMode::AI);
    CHECK(view.purpose == Slic3r::GUI::SmartSlicingPurpose::General);
    CHECK(view.mode_id == "ai");
    CHECK(view.purpose_id == "general");
    CHECK(view.native_baseline_available);
    CHECK(view.baseline.summary_key == "native_baseline");
    CHECK(view.goal_results[0].goal_id == "balanced");
    CHECK(view.goal_results[0].state == Slic3r::GUI::SmartSlicingGoalState::Failed);
    CHECK(view.goal_results[1].state == Slic3r::GUI::SmartSlicingGoalState::Unavailable);
    CHECK(view.goal_results[1].actions.can_reanalyze);
    CHECK(view.goal_results[2].state == Slic3r::GUI::SmartSlicingGoalState::Ready);

    snapshot.state = WorkflowState::OfficialSlicing;
    const auto slicing_view = Slic3r::GUI::SmartSlicingViewModel::from_snapshot(snapshot);
    for (const auto& goal : slicing_view.goal_results)
        CHECK(goal.state == Slic3r::GUI::SmartSlicingGoalState::OfficialSlicing);
}

TEST_CASE("D7 purpose selection defaults to General and rejects Unknown",
          "[AI][SmartSlicing][ViewModel][D7T2]")
{
    Slic3r::GUI::OrcaSmartSlicingAdapter adapter(nullptr);
    CHECK(adapter.usage_purpose() == UsagePurpose::General);
    CHECK(adapter.set_usage_purpose(UsagePurpose::Decoration));
    CHECK(adapter.usage_purpose() == UsagePurpose::Decoration);
    CHECK(adapter.set_usage_purpose(UsagePurpose::Functional));
    CHECK(adapter.usage_purpose() == UsagePurpose::Functional);
    CHECK_FALSE(adapter.set_usage_purpose(UsagePurpose::Unknown));
    CHECK(adapter.usage_purpose() == UsagePurpose::General);
}

TEST_CASE("D3 Orca user-marked region bridge fails closed without current owner geometry",
          "[AI][SmartSlicing][ProtectedRegions][D3T2]")
{
    Slic3r::GUI::OrcaSmartSlicingAdapter adapter(nullptr);
    const auto marked = adapter.mark_user_marked_region(11, 22, {{0, 2}});
    CHECK(marked.status == ProtectedRegionBindingStatus::Rejected);
    const auto queried = adapter.query_user_marked_region(11, 22);
    CHECK(queried.status == ProtectedRegionSourceStatus::Rejected);
    CHECK_FALSE(adapter.clear_user_marked_region(11, 22));
    const auto captured = adapter.capture_candidate_search_input();
    CHECK(captured.status == Slic3r::GUI::OrcaCandidateSearchCaptureStatus::InvalidInput);
    CHECK_FALSE(captured.input.has_value());
}

TEST_CASE("D7 presenter bridges recommendation states while preserving mode and purpose",
          "[AI][SmartSlicing][ViewModel][D7T3]")
{
    FakeWorkspace workspace;
    SmartSlicingCoordinator coordinator(workspace);
    Slic3r::GUI::SmartSlicingPresenter presenter(coordinator);
    presenter.set_mode(Slic3r::GUI::SmartSlicingMode::Orca);
    presenter.set_purpose(Slic3r::GUI::SmartSlicingPurpose::Functional);

    RecommendationSnapshot recommendation;
    recommendation.baseline = {"baseline", GoalResultStatus::Analyzing, {}};
    recommendation.goal_result(RecommendationGoal::Balanced).status = GoalResultStatus::Analyzing;
    recommendation.goal_result(RecommendationGoal::Speed).status = GoalResultStatus::Ready;
    recommendation.goal_result(RecommendationGoal::Quality).status = GoalResultStatus::Unavailable;
    presenter.publish_recommendation_snapshot(recommendation);
    CHECK(presenter.view_model().mode == Slic3r::GUI::SmartSlicingMode::Orca);
    CHECK(presenter.view_model().purpose == Slic3r::GUI::SmartSlicingPurpose::Functional);
    CHECK(presenter.view_model().goal_results[0].status == GoalResultStatus::Analyzing);
    CHECK(presenter.view_model().goal_results[1].status == GoalResultStatus::Ready);
    CHECK(presenter.view_model().goal_results[2].status == GoalResultStatus::Unavailable);

    recommendation.goal_result(RecommendationGoal::Balanced).status = GoalResultStatus::Stale;
    recommendation.goal_result(RecommendationGoal::Speed).status = GoalResultStatus::Failed;
    recommendation.goal_result(RecommendationGoal::Quality).status = GoalResultStatus::Ready;
    presenter.publish_recommendation_snapshot(recommendation);
    CHECK(presenter.view_model().goal_results[0].status == GoalResultStatus::Stale);
    CHECK(presenter.view_model().goal_results[1].status == GoalResultStatus::Failed);
    CHECK(presenter.view_model().goal_results[2].status == GoalResultStatus::Ready);
}

TEST_CASE("D7 presenter owner dispatch defers coordinator observer publication",
          "[AI][SmartSlicing][ViewModel][D7T3]")
{
    FakeWorkspace workspace;
    SmartSlicingCoordinator coordinator(workspace);
    std::function<void()> deferred;
    Slic3r::GUI::SmartSlicingPresenter presenter(coordinator, [&](std::function<void()> callback) {
        deferred = std::move(callback);
    });
    size_t view_changes = 0;
    presenter.set_view_changed([&](const auto&) { ++view_changes; });
    coordinator.start();
    REQUIRE(deferred);
    const size_t before_dispatch = view_changes;
    deferred();
    CHECK(view_changes == before_dispatch + 1);
}

TEST_CASE("D7 recommendation bridge clears superseded goal results on an early exit",
          "[AI][SmartSlicing][ViewModel][D7T3]")
{
    FakeWorkspace workspace;
    SmartSlicingCoordinator coordinator(workspace);
    Slic3r::GUI::SmartSlicingPresenter presenter(coordinator);
    RecommendationSnapshot ready;
    ready.goal_result(RecommendationGoal::Balanced).status = GoalResultStatus::Ready;
    ready.goal_result(RecommendationGoal::Speed).status = GoalResultStatus::Ready;
    ready.goal_result(RecommendationGoal::Quality).status = GoalResultStatus::Ready;
    presenter.publish_recommendation_snapshot(ready);
    CHECK(presenter.view_model().goal_results[0].status == GoalResultStatus::Ready);

    presenter.publish_recommendation_snapshot(RecommendationSnapshot{});
    for (const auto& goal : presenter.view_model().goal_results)
        CHECK(goal.status == GoalResultStatus::Analyzing);
}

TEST_CASE("workspace revision equality includes every revision component", "[AI][SmartSlicing]")
{
    const WorkspaceRevision baseline{1, 2, 3, "fingerprint"};
    CHECK((baseline == WorkspaceRevision{1, 2, 3, "fingerprint"}));
    CHECK((baseline != WorkspaceRevision{9, 2, 3, "fingerprint"}));
    CHECK((baseline != WorkspaceRevision{1, 9, 3, "fingerprint"}));
    CHECK((baseline != WorkspaceRevision{1, 2, 9, "fingerprint"}));
    CHECK((baseline != WorkspaceRevision{1, 2, 3, "different"}));
}

TEST_CASE("manual intent distinguishes hard constraints from unavailable placement locks", "[AI][SmartSlicing]")
{
    IntentConstraintRecord plate_lock;
    plate_lock.type = IntentConstraintType::PlatePlacementLock;
    plate_lock.source = IntentConstraintSource::OrcaPartPlate;
    plate_lock.state = IntentConstraintState::Active;
    CHECK(plate_lock.is_hard_constraint());

    IntentConstraintRecord object_lock;
    object_lock.type = IntentConstraintType::ObjectPlacementLock;
    object_lock.source = IntentConstraintSource::UnavailableInOrcaModel;
    object_lock.state = IntentConstraintState::Unknown;
    CHECK_FALSE(object_lock.is_hard_constraint());
    CHECK(std::string(intent_constraint_source_name(object_lock.source)) == "unavailable_in_orca_model");

    IntentConstraintRecord instance_lock = object_lock;
    instance_lock.type = IntentConstraintType::InstancePlacementLock;
    CHECK_FALSE(instance_lock.is_hard_constraint());

    ProcessParameterBaselineSnapshot process_baseline;
    process_baseline.parameter_keys = {"layer_height", "wall_loops"};
    CHECK(process_baseline.is_optimizable_baseline());
    CHECK_FALSE(process_baseline.is_hard_constraint());
}

TEST_CASE("every captured manual intent category participates in its stable revision", "[AI][SmartSlicing]")
{
    const std::array<IntentConstraintType, 11> types{
        IntentConstraintType::PlatePlacementLock,
        IntentConstraintType::ObjectPlacementLock,
        IntentConstraintType::InstancePlacementLock,
        IntentConstraintType::SupportPainting,
        IntentConstraintType::SeamPainting,
        IntentConstraintType::MulticolorPainting,
        IntentConstraintType::FuzzySkinPainting,
        IntentConstraintType::ModifierVolume,
        IntentConstraintType::ObjectConfigOverride,
        IntentConstraintType::LayerHeightRange,
        IntentConstraintType::VariableLayerHeight,
    };

    IntentConstraintSnapshot baseline;
    baseline.global_process_parameters.parameter_keys = {"wall_loops", "layer_height"};
    for (size_t index = 0; index < types.size(); ++index) {
        IntentConstraintRecord record;
        record.type = types[index];
        record.source = IntentConstraintSource::ModelVolumeAnnotation;
        record.state = IntentConstraintState::Active;
        record.object_id = 10;
        record.instance_id = 20;
        record.volume_id = 30 + index;
        record.source_revision = 40 + index;
        record.parameter_keys = {"z-key", "a-key"};
        baseline.records.push_back(std::move(record));
    }
    const uint64_t baseline_revision = intent_constraints_revision(baseline);

    for (size_t index = 0; index < baseline.records.size(); ++index) {
        IntentConstraintSnapshot changed = baseline;
        ++changed.records[index].source_revision;
        INFO(intent_constraint_type_name(changed.records[index].type));
        CHECK(intent_constraints_revision(changed) != baseline_revision);
    }

    IntentConstraintSnapshot reordered = baseline;
    std::reverse(reordered.records.begin(), reordered.records.end());
    std::reverse(reordered.global_process_parameters.parameter_keys.begin(),
                 reordered.global_process_parameters.parameter_keys.end());
    CHECK(intent_constraints_revision(reordered) == baseline_revision);

    IntentConstraintSnapshot changed_process_baseline = baseline;
    changed_process_baseline.global_process_parameters.parameter_keys.push_back("top_shell_layers");
    CHECK(intent_constraints_revision(changed_process_baseline) != baseline_revision);
    CHECK_FALSE(changed_process_baseline.global_process_parameters.is_hard_constraint());
}

TEST_CASE("invalid or unavailable workspace capture fails without leaving the workflow stuck", "[AI][SmartSlicing]")
{
    FakeWorkspace workspace;
    workspace.context.revision.fingerprint.clear();
    SmartSlicingCoordinator coordinator(workspace);

    coordinator.start();
    CHECK(coordinator.snapshot().state == WorkflowState::Failed);
    CHECK(coordinator.snapshot().detail == "invalid_workspace_revision");
    CHECK(coordinator.snapshot().can_start());

    workspace.context = context_with_revision("revision-b");
    workspace.context.materials.push_back({"material", "#FFFFFF"});
    coordinator.start();
    CHECK(coordinator.snapshot().workflow_id == 2);
    CHECK(coordinator.snapshot().state == WorkflowState::ReadyForCandidatePlanning);

    coordinator.cancel();
    workspace.throw_on_capture = true;
    coordinator.start();
    CHECK(coordinator.snapshot().state == WorkflowState::Failed);
    CHECK(coordinator.snapshot().detail == "capture unavailable");
}

TEST_CASE("revision refresh is stable for equal or temporarily unavailable revisions", "[AI][SmartSlicing]")
{
    FakeWorkspace workspace;
    workspace.context.materials.push_back({"material", "#FFFFFF"});
    SmartSlicingCoordinator coordinator(workspace);
    coordinator.start();

    CHECK_FALSE(coordinator.refresh_revision());
    CHECK(coordinator.snapshot().state == WorkflowState::ReadyForCandidatePlanning);

    workspace.throw_on_revision = true;
    CHECK_FALSE(coordinator.refresh_revision());
    CHECK(coordinator.snapshot().state == WorkflowState::ReadyForCandidatePlanning);
}

TEST_CASE("coordinator publishes deterministic synchronous preflight transitions", "[AI][SmartSlicing]")
{
    FakeWorkspace workspace;
    workspace.context.materials.push_back({"material", "#FFFFFF"});
    SmartSlicingCoordinator coordinator(workspace);
    std::vector<WorkflowState> states;
    coordinator.set_observer([&states](const WorkflowSnapshot& snapshot) { states.push_back(snapshot.state); });

    coordinator.start();

    REQUIRE(states.size() == 4);
    CHECK(states[0] == WorkflowState::Idle);
    CHECK(states[1] == WorkflowState::CapturingContext);
    CHECK(states[2] == WorkflowState::Preflighting);
    CHECK(states[3] == WorkflowState::ReadyForCandidatePlanning);
}

TEST_CASE("minimum printability prerequisites use stable issue ordering", "[AI][SmartSlicing]")
{
    WorkspaceContext context;
    context.revision                    = {1, 2, 3, "revision-a"};
    context.native_validation_available = true;

    const PrintabilityReport report = PrintabilityInspector().inspect(context);

    REQUIRE(report.issues.size() == 6);
    CHECK(report.issues[0].code == IssueCode::EmptyPlate);
    CHECK(report.issues[1].code == IssueCode::MissingPrinter);
    CHECK(report.issues[2].code == IssueCode::MissingProcess);
    CHECK(report.issues[3].code == IssueCode::MissingMaterial);
    CHECK(report.issues[4].code == IssueCode::MachineCapabilityUnavailable);
    CHECK(report.issues[5].code == IssueCode::UnsupportedMaterialCombination);
    CHECK(report.readiness == Readiness::Blocked);
    CHECK(report.has_blocking_issue());
}

TEST_CASE("unavailable native validation is explicit and non-blocking", "[AI][SmartSlicing]")
{
    WorkspaceContext context = context_with_revision("revision-a");
    context.materials.push_back({"material", "#FFFFFF"});
    context.native_validation_available = false;

    const PrintabilityReport report = PrintabilityInspector().inspect(context);

    REQUIRE(report.issues.size() == 1);
    CHECK(report.issues.front().code == IssueCode::NativeValidationUnavailable);
    CHECK(report.readiness == Readiness::NeedsAttention);
    CHECK_FALSE(report.has_blocking_issue());

    WorkflowSnapshot snapshot;
    snapshot.state                                = WorkflowState::ReadyForCandidatePlanning;
    snapshot.context                              = context;
    snapshot.report                               = report;
    const Slic3r::GUI::SmartSlicingViewModel view = Slic3r::GUI::SmartSlicingViewModel::from_snapshot(snapshot);
    CHECK(view.summary_key == "preflight_complete_with_warnings");
    CHECK(view.stages[1].status == Slic3r::GUI::SmartSlicingStageStatus::NeedsAttention);
    CHECK(view.legacy_steps[1] == Slic3r::GUI::LegacyAIWorkflowStatus::Warning);
}

TEST_CASE("empty material preset entries do not satisfy printability prerequisites", "[AI][SmartSlicing]")
{
    WorkspaceContext context = context_with_revision("revision-a");
    context.materials.push_back({"", "#FFFFFF"});

    const PrintabilityReport report = PrintabilityInspector().inspect(context);

    REQUIRE(report.issues.size() == 1);
    CHECK(report.issues.front().code == IssueCode::MissingMaterial);
    CHECK(report.has_blocking_issue());
}

TEST_CASE("multicolor preflight makes compatibility and mapping evidence explicit", "[AI][SmartSlicing][Multicolor]")
{
    WorkspaceContext context = context_with_revision("revision-a");
    context.materials.push_back({"pla", "#FFFFFF"});
    context.materials.push_back({"petg", "#000000"});
    context.multicolor.used_logical_filament_ids = {1, 2};
    context.multicolor.filament_to_physical_slot = {1, 1};
    context.multicolor.first_layer_tool_sequence = {1, 2};
    context.multicolor.other_layer_tool_sequences.push_back({1, 25, {2, 1}});
    context.multicolor.physical_slot_compatibility = PhysicalSlotCompatibility::Incompatible;
    context.multicolor.color_mapping_degraded = true;

    const PrintabilityReport blocked = PrintabilityInspector().inspect(context);
    REQUIRE(blocked.issues.size() == 2);
    CHECK(blocked.issues[0].code == IssueCode::IncompatiblePhysicalSlots);
    CHECK(blocked.issues[1].code == IssueCode::ColorMappingDegraded);
    CHECK(blocked.has_blocking_issue());
    CHECK(blocked.readiness == Readiness::Blocked);
    CHECK(context.multicolor.first_layer_tool_sequence == std::vector<int>{1, 2});
    CHECK(context.multicolor.other_layer_tool_sequences.front().logical_filament_ids == std::vector<int>{2, 1});

    context.multicolor.color_mapping_degraded = false;
    context.multicolor.physical_slot_compatibility = PhysicalSlotCompatibility::Unavailable;
    const PrintabilityReport unavailable = PrintabilityInspector().inspect(context);
    REQUIRE(unavailable.issues.size() == 1);
    CHECK(unavailable.issues.front().code == IssueCode::MulticolorEvidenceUnavailable);
    CHECK_FALSE(unavailable.has_blocking_issue());
    CHECK(unavailable.readiness == Readiness::NeedsAttention);
}

TEST_CASE("idle view model initializes every stage and legacy step", "[AI][SmartSlicing]")
{
    const Slic3r::GUI::SmartSlicingViewModel view = Slic3r::GUI::SmartSlicingViewModel::from_snapshot(WorkflowSnapshot{});

    for (const Slic3r::GUI::SmartSlicingStageView& stage : view.stages)
        CHECK(stage.status == Slic3r::GUI::SmartSlicingStageStatus::Waiting);
    for (const Slic3r::GUI::LegacyAIWorkflowStatus status : view.legacy_steps)
        CHECK(status == Slic3r::GUI::LegacyAIWorkflowStatus::Waiting);
    CHECK(view.can_start);
    CHECK_FALSE(view.can_cancel);
    CHECK_FALSE(view.has_report);
    CHECK(view.can_add_model);
}

TEST_CASE("empty plates offer model import without claiming model preparation is complete", "[AI][SmartSlicing]")
{
    FakeWorkspace workspace;
    workspace.context.objects.clear();
    SmartSlicingCoordinator coordinator(workspace);
    coordinator.start();
    const auto view = Slic3r::GUI::SmartSlicingViewModel::from_snapshot(coordinator.snapshot());
    CHECK(view.has_report);
    CHECK(view.can_add_model);
    CHECK(view.can_recheck);
    CHECK(view.stages[0].status == Slic3r::GUI::SmartSlicingStageStatus::NeedsAttention);
    CHECK_FALSE(view.can_plan_candidates);
}

TEST_CASE("candidate cards preserve the proposed old and new values", "[AI][SmartSlicing]")
{
    WorkflowSnapshot snapshot;
    snapshot.state = WorkflowState::ReadyToApply;
    SliceCandidate candidate;
    candidate.id = "wider-brim";
    candidate.parameters.entries.push_back({ConfigScope::Plate, PresetOwner::Process, 12,
        "brim_width", 2.0, 5.0, "adhesion"});
    candidate.placement.transforms.resize(2);
    snapshot.candidates.push_back(candidate);
    const auto view = Slic3r::GUI::SmartSlicingViewModel::from_snapshot(snapshot);
    REQUIRE(view.candidates.size() == 1);
    REQUIRE(view.candidates.front().parameter_changes.size() == 1);
    const auto& change = view.candidates.front().parameter_changes.front();
    CHECK(change.key == "brim_width");
    CHECK(change.target_id == 12);
    CHECK_THAT(std::get<double>(change.expected_value), Catch::Matchers::WithinAbs(2.0, 0.001));
    CHECK_THAT(std::get<double>(change.new_value), Catch::Matchers::WithinAbs(5.0, 0.001));
    CHECK(view.candidates.front().placement_change_count == 2);
}

TEST_CASE("completed and failed applications expose the same undo action", "[AI][SmartSlicing]")
{
    WorkflowSnapshot snapshot;
    snapshot.state = GENERATE(WorkflowState::Completed, WorkflowState::ApplyFailed);
    snapshot.can_undo_apply = true;
    CHECK(Slic3r::GUI::SmartSlicingViewModel::from_snapshot(snapshot).can_undo_apply);
    snapshot.can_undo_apply = false;
    CHECK_FALSE(Slic3r::GUI::SmartSlicingViewModel::from_snapshot(snapshot).can_undo_apply);
}

TEST_CASE("undo errors do not relabel completed slicing as failed", "[AI][SmartSlicing]")
{
    WorkflowSnapshot snapshot;
    snapshot.state = WorkflowState::Completed;
    snapshot.can_undo_apply = true;
    snapshot.detail = "apply_undo_failed";
    const auto view = Slic3r::GUI::SmartSlicingViewModel::from_snapshot(snapshot);
    CHECK(view.summary_key == "apply_undo_failed");
    CHECK(view.stages.back().status == Slic3r::GUI::SmartSlicingStageStatus::Complete);
    CHECK(view.can_undo_apply);
}

TEST_CASE("canceled view model does not expose a report from an obsolete workspace", "[AI][SmartSlicing]")
{
    FakeWorkspace workspace;
    workspace.context.materials.push_back({"material", "#FFFFFF"});
    workspace.context.objects.front().open_edge_count = 3;
    SmartSlicingCoordinator coordinator(workspace);
    coordinator.start();
    REQUIRE(coordinator.snapshot().report);
    REQUIRE_FALSE(coordinator.snapshot().report->issues.empty());

    coordinator.cancel();

    const Slic3r::GUI::SmartSlicingViewModel view = Slic3r::GUI::SmartSlicingViewModel::from_snapshot(coordinator.snapshot());
    CHECK(view.summary_key == "canceled");
    CHECK(view.issue_count == 0);
    CHECK(view.issues.empty());
}
