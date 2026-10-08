#include <catch2/catch_all.hpp>

#include "slic3r/AI/SmartSlicing/Application/CandidateSearchSessionPlanner.hpp"
#include "slic3r/GUI/AI/Orca/OrcaCandidateSearchAdapter.hpp"

#include <algorithm>

using namespace Slic3r;
using namespace Slic3r::AI::SmartSlicing;
using namespace Slic3r::GUI;

namespace {

std::array<double, 16> identity_transform()
{
    return {1.0, 0.0, 0.0, 0.0,
            0.0, 1.0, 0.0, 0.0,
            0.0, 0.0, 1.0, 0.0,
            0.0, 0.0, 0.0, 1.0};
}

OrcaCandidateSearchCaptureSource capture_source()
{
    OrcaCandidateSearchCaptureSource source;
    source.context.revision = {11, 12, 13, "workspace-v1"};
    source.context.machine_capability.support_status = MachineSupportStatus::Enabled;
    source.context.material_compatibility.combination_status =
        MaterialCombinationStatus::Compatible;
    source.context.material_compatibility.common_family = MaterialFamily::PLA;
    MaterialCapabilitySnapshot material;
    material.support_status = MaterialSupportStatus::Enabled;
    material.speed_policy.may_increase_process_speed = true;
    source.context.material_compatibility.materials.push_back(std::move(material));
    source.plate_id = 71;
    source.objects.push_back({101, 201, identity_transform()});
    source.process_profile.identity.setting_id = "process-setting-id";
    source.process_profile.identity.fingerprint = std::string(64, 'a');
    source.process_profile.process_config = DynamicPrintConfig::full_print_config();
    source.process_profile.effective_config = source.process_profile.process_config;
    source.process_profile.process_config.set("brim_width", 0.0);
    source.process_profile.effective_config.set("brim_width", 0.0);
    return source;
}

const CandidateSearchParameterInput* parameter(const CandidateSearchInput& input,
                                                const std::string& key)
{
    const auto found = std::find_if(input.profile_parameters.begin(),
                                    input.profile_parameters.end(),
                                    [&](const CandidateSearchParameterInput& value) {
                                        return value.key == key;
                                    });
    return found == input.profile_parameters.end() ? nullptr : &*found;
}

ParameterProposal brim_proposal(double replacement)
{
    ParameterProposal proposal;
    proposal.entries.push_back({ConfigScope::Plate, PresetOwner::Process, 71,
                                "brim_width", 0.0, replacement, "test_brim"});
    return proposal;
}

CandidateSearchResult schedulable_search_result()
{
    CandidateSearchResult result;
    result.budget_version = "candidate-search-budget/v1";
    result.trial_cost_policy_version = CANDIDATE_TRIAL_COST_POLICY_VERSION;
    result.baseline.candidate_id = "baseline-1";
    result.baseline.workspace_revision = {1, 2, 3, "revision-1"};
    for (size_t index = 0; index < RECOMMENDATION_GOALS.size(); ++index) {
        const RecommendationGoal goal = RECOMMENDATION_GOALS[index];
        GoalCandidateDrafts& goal_result = result.goals[index];
        goal_result.goal = goal;
        CandidateSearchDraft draft;
        draft.candidate_id = std::string(recommendation_goal_id(goal)) + "-draft-1";
        draft.goal = goal;
        draft.status = CandidateStatus::Draft;
        goal_result.selected_for_trial.push_back(std::move(draft));
    }
    return result;
}

} // namespace

TEST_CASE("Orca candidate capture keeps stable current transforms and filters orientation evidence",
          "[AI][SmartSlicing][CandidateCapture][Orca]")
{
    OrcaCandidateSearchCaptureSource source = capture_source();
    OrientationSearchOption unverified;
    unverified.option_id = "unverified-stable";
    unverified.strategy = OrientationStrategy::StablePlane;
    unverified.transform = identity_transform();
    unverified.evidence_availability = FeatureAvailability::Known;
    source.objects.front().verified_orientation_options.push_back(unverified);

    OrientationSearchOption verified = unverified;
    verified.option_id = "verified-low-support";
    verified.strategy = OrientationStrategy::LowSupport;
    verified.evidence_source = "fixture_geometry";
    verified.evidence_version = "fixture_geometry/v1";
    verified.transform[3] = 5.0;
    source.objects.front().verified_orientation_options.push_back(verified);

    const OrcaCandidateSearchCaptureResult result = OrcaCandidateSearchAdapter::capture(source);
    INFO(result.diagnostic_code);
    REQUIRE(result.completed());
    REQUIRE(result.input->objects.size() == 1);
    const CandidateSearchObjectInput& object = result.input->objects.front();
    CHECK(object.object_id == 101);
    CHECK(object.instance_id == 201);
    CHECK(object.current_transform == identity_transform());
    CHECK_FALSE(object.locked);
    REQUIRE(object.orientation_options.size() == 2);
    CHECK(object.orientation_options[0].strategy == OrientationStrategy::Current);
    CHECK(object.orientation_options[1].option_id == "verified-low-support");

    source.plate_locked = true;
    const auto locked = OrcaCandidateSearchAdapter::capture(source);
    REQUIRE(locked.completed());
    CHECK(locked.input->objects.front().locked);
    REQUIRE(locked.input->objects.front().orientation_options.size() == 1);
    CHECK(locked.input->objects.front().orientation_options.front().strategy ==
          OrientationStrategy::Current);
}

TEST_CASE("Orca candidate capture exposes only the active scalar Process value without invented bounds",
          "[AI][SmartSlicing][CandidateCapture][Orca]")
{
    const OrcaCandidateSearchCaptureResult result =
        OrcaCandidateSearchAdapter::capture(capture_source());
    REQUIRE(result.completed());
    const CandidateSearchParameterInput* layer = parameter(*result.input, "layer_height");
    REQUIRE(layer != nullptr);
    REQUIRE(result.input->objects.front().orientation_options.size() == 1);
    CHECK(result.input->objects.front().orientation_options.front().strategy ==
          OrientationStrategy::Current);
    CHECK(layer->target_id == 71);
    CHECK(layer->current_source_code == "orca_plate_effective_config");
    CHECK_FALSE(layer->current_source_version.empty());
    REQUIRE(layer->options.size() == 1);
    CHECK(layer->options.front().value == layer->current_value);
    CHECK(layer->options.front().source_code == "orca_active_process_profile");
    CHECK_FALSE(layer->options.front().source_version.empty());
    CHECK(layer->bounds.empty());

    CHECK(std::all_of(result.input->profile_parameters.begin(),
                      result.input->profile_parameters.end(), [](const auto& value) {
                          return value.options.size() == 1 && value.bounds.empty();
                      }));
    CHECK(parameter(*result.input, "fan_max_speed") == nullptr);
    CHECK(parameter(*result.input, "object_orientation_strategy") == nullptr);
    CHECK(parameter(*result.input, "tool_change_sequence") == nullptr);
}

TEST_CASE("Orca candidate capture fails closed for capability and Process profile provenance",
          "[AI][SmartSlicing][CandidateCapture][Orca]")
{
    OrcaCandidateSearchCaptureSource source = capture_source();
    source.context.machine_capability.support_status = MachineSupportStatus::PendingValidation;
    CHECK(OrcaCandidateSearchAdapter::capture(source).status ==
          OrcaCandidateSearchCaptureStatus::MachineCapabilityPendingValidation);

    source = capture_source();
    source.context.material_compatibility.combination_status =
        MaterialCombinationStatus::Unsupported;
    CHECK(OrcaCandidateSearchAdapter::capture(source).status ==
          OrcaCandidateSearchCaptureStatus::MaterialCompatibilityUnavailable);

    source = capture_source();
    source.process_profile.dirty = true;
    CHECK(OrcaCandidateSearchAdapter::capture(source).status ==
          OrcaCandidateSearchCaptureStatus::ProcessProfileDirty);

    source = capture_source();
    source.process_profile.identity.setting_id.clear();
    CHECK(OrcaCandidateSearchAdapter::capture(source).status ==
          OrcaCandidateSearchCaptureStatus::ProcessProfileIdentityUnavailable);

    source = capture_source();
    source.process_profile.identity.fingerprint.clear();
    CHECK(OrcaCandidateSearchAdapter::capture(source).status ==
          OrcaCandidateSearchCaptureStatus::ProcessProfileFingerprintUnavailable);

    source = capture_source();
    source.process_profile.process_config.set_key_value(
        "line_width", new ConfigOptionFloatOrPercent(100.0, true));
    source.process_profile.effective_config.set_key_value(
        "line_width", new ConfigOptionFloatOrPercent(100.0, true));
    const OrcaCandidateSearchCaptureResult percent_width =
        OrcaCandidateSearchAdapter::capture(source);
    CHECK(percent_width.status == OrcaCandidateSearchCaptureStatus::ProfileEvidenceConflict);
    CHECK(percent_width.diagnostic_code ==
          "process_profile_parameter_value_invalid.line_width");
}

TEST_CASE("Orca candidate capture separates effective overrides from the active Process option",
          "[AI][SmartSlicing][CandidateCapture][Orca]")
{
    OrcaCandidateSearchCaptureSource source = capture_source();
    source.process_profile.effective_config.set("brim_width", 5.0);
    const OrcaCandidateSearchCaptureResult captured =
        OrcaCandidateSearchAdapter::capture(source);
    REQUIRE(captured.completed());
    const CandidateSearchParameterInput* brim = parameter(*captured.input, "brim_width");
    REQUIRE(brim != nullptr);
    CHECK(std::get<double>(brim->current_value) == Catch::Approx(5.0));
    REQUIRE(brim->options.size() == 1);
    CHECK(std::get<double>(brim->options.front().value) == Catch::Approx(0.0));
    CHECK(brim->current_source_code == "orca_plate_effective_config");
    CHECK(brim->options.front().source_code == "orca_active_process_profile");

    ParameterProposal restore_profile = brim_proposal(0.0);
    restore_profile.entries.front().expected_value = 5.0;
    CHECK(captured.input->native_validator(restore_profile).accepted);
}

TEST_CASE("captured object manual overrides remain hard intent constraints",
          "[AI][SmartSlicing][CandidateCapture][Orca]")
{
    OrcaCandidateSearchCaptureSource source = capture_source();
    IntentConstraintRecord object_override;
    object_override.type = IntentConstraintType::ObjectConfigOverride;
    object_override.source = IntentConstraintSource::ModelObjectConfig;
    object_override.state = IntentConstraintState::Active;
    object_override.object_id = 101;
    object_override.parameter_keys = {"wall_loops"};
    source.context.intent_constraints.records.push_back(object_override);
    const OrcaCandidateSearchCaptureResult captured =
        OrcaCandidateSearchAdapter::capture(source);
    REQUIRE(captured.completed());

    ParameterProposal proposal;
    proposal.entries.push_back({ConfigScope::Object, PresetOwner::Process, 101,
                                "wall_loops", int64_t{2}, int64_t{3}, "test_wall"});
    ParameterValidationContext context;
    context.current_values.push_back({ConfigScope::Object, PresetOwner::Process, 101,
                                      "wall_loops", int64_t{2}});
    context.intent_constraints = captured.input->intent_constraints;
    context.native_validator = [](const ParameterProposal&) {
        return NativeParameterValidationResult{true, {}};
    };
    const ParameterValidationResult validated =
        ParameterProposalValidator().validate(proposal, context);
    REQUIRE_FALSE(validated.accepted());
    CHECK(validated.rejections.front().code == ParameterRejectionCode::IntentConflict);
}

TEST_CASE("Orca candidate native validation starts each call from one immutable effective config",
          "[AI][SmartSlicing][CandidateCapture][Orca]")
{
    OrcaCandidateSearchCaptureSource source = capture_source();
    const OrcaCandidateSearchCaptureResult captured =
        OrcaCandidateSearchAdapter::capture(source);
    REQUIRE(captured.completed());

    const NativeParameterValidationResult first =
        captured.input->native_validator(brim_proposal(5.0));
    const NativeParameterValidationResult second =
        captured.input->native_validator(brim_proposal(6.0));
    CHECK(first.accepted);
    CHECK(second.accepted);
    CHECK(source.process_profile.effective_config.opt_float("brim_width") ==
          Catch::Approx(0.0));
}

TEST_CASE("candidate search session planning binds Draft identities without publishing Ready",
          "[AI][SmartSlicing][CandidateCapture][Session]")
{
    const CandidateSearchResult search = schedulable_search_result();
    const CandidateSearchSessionPlanResult planned =
        CandidateSearchSessionPlanner().plan(search, 41, 7);
    REQUIRE(planned.accepted());
    CHECK(planned.plan->start_command.workflow_id == 41);
    CHECK(planned.plan->start_command.attempt_id == 7);
    CHECK(planned.plan->trial_cost_policy_version == CANDIDATE_TRIAL_COST_POLICY_VERSION);
    CHECK(planned.plan->baseline_task.goal_id == BASELINE_GOAL_ID);
    for (size_t index = 0; index < RECOMMENDATION_GOALS.size(); ++index) {
        CHECK(planned.plan->goal_tasks[index].goal_id ==
              recommendation_goal_id(RECOMMENDATION_GOALS[index]));
        CHECK(planned.plan->goal_tasks[index].candidate_id ==
              search.goals[index].selected_for_trial.front().candidate_id);
    }

    RecommendationSessionCoordinator coordinator;
    coordinator.enqueue(planned.plan->start_command);
    REQUIRE(coordinator.process_all() == 1);
    CHECK(coordinator.snapshot().state == RecommendationSessionState::Recommending);
    CHECK(coordinator.snapshot().recommendation.baseline.status == GoalResultStatus::Analyzing);
    for (const RecommendationGoal goal : RECOMMENDATION_GOALS)
        CHECK(coordinator.snapshot().recommendation.goal_result(goal).status ==
              GoalResultStatus::Analyzing);
}

TEST_CASE("candidate search planned task identities retain revision and cancellation late-result rejection",
          "[AI][SmartSlicing][CandidateCapture][Session]")
{
    const CandidateSearchSessionPlanResult planned =
        CandidateSearchSessionPlanner().plan(schedulable_search_result(), 51, 9);
    REQUIRE(planned.accepted());
    RecommendationSessionCoordinator coordinator;
    coordinator.enqueue(planned.plan->start_command);
    coordinator.process_all();

    WorkspaceRevision changed = planned.plan->start_command.workspace_revision;
    changed.plate_revision += 1;
    changed.fingerprint = "revision-2";
    coordinator.enqueue(WorkspaceRevisionChangedCommand{51, 9, changed});
    coordinator.process_all();
    CHECK(coordinator.snapshot().state == RecommendationSessionState::Stale);

    RecommendationTaskResult late;
    late.identity = planned.plan->goal_tasks.front();
    late.outcome = RecommendationTaskOutcome::Ready;
    coordinator.enqueue(std::move(late));
    coordinator.process_all();
    CHECK(coordinator.snapshot().state == RecommendationSessionState::Stale);
    CHECK(coordinator.discarded_result_count() == 1);

    RecommendationSessionCoordinator canceled;
    canceled.enqueue(planned.plan->start_command);
    canceled.process_all();
    canceled.enqueue(CancelRecommendationSessionCommand{
        51, 9, RecommendationCancellationReason::User});
    canceled.process_all();
    CHECK(canceled.snapshot().state == RecommendationSessionState::Canceled);
    RecommendationTaskResult canceled_late;
    canceled_late.identity = planned.plan->baseline_task;
    canceled_late.outcome = RecommendationTaskOutcome::Ready;
    canceled.enqueue(std::move(canceled_late));
    canceled.process_all();
    CHECK(canceled.snapshot().state == RecommendationSessionState::Canceled);
    CHECK(canceled.discarded_result_count() == 1);
}

TEST_CASE("candidate search session planning rejects missing or non-Draft goal identities",
          "[AI][SmartSlicing][CandidateCapture][Session]")
{
    CandidateSearchResult missing = schedulable_search_result();
    missing.goals.front().selected_for_trial.clear();
    CHECK_FALSE(CandidateSearchSessionPlanner().plan(missing, 1, 1).accepted());

    CandidateSearchResult ready = schedulable_search_result();
    ready.goals.front().selected_for_trial.front().status = CandidateStatus::Ready;
    const CandidateSearchSessionPlanResult rejected =
        CandidateSearchSessionPlanner().plan(ready, 1, 1);
    CHECK_FALSE(rejected.accepted());
    CHECK(rejected.diagnostic_code == "candidate_session_goal_draft_invalid");
}

TEST_CASE("new candidate attempts supersede active identities before capture or planning failure",
          "[AI][SmartSlicing][CandidateCapture][Session]")
{
    SECTION("capture failure") {
        const CandidateSearchSessionPlanResult active =
            CandidateSearchSessionPlanner().plan(schedulable_search_result(), 61, 1);
        REQUIRE(active.accepted());
        RecommendationSessionCoordinator coordinator;
        coordinator.enqueue(active.plan->start_command);
        coordinator.process_all();
        const auto old_token = coordinator.cancellation_token();

        REQUIRE(supersede_active_candidate_search_session(coordinator));
        CHECK(coordinator.snapshot().state == RecommendationSessionState::Canceled);
        CHECK(coordinator.snapshot().cancellation_reason ==
              RecommendationCancellationReason::Superseded);
        CHECK(old_token->reason() == RecommendationCancellationReason::Superseded);
    }

    SECTION("planner failure") {
        const CandidateSearchSessionPlanResult active =
            CandidateSearchSessionPlanner().plan(schedulable_search_result(), 62, 1);
        REQUIRE(active.accepted());
        RecommendationSessionCoordinator coordinator;
        coordinator.enqueue(active.plan->start_command);
        coordinator.process_all();

        REQUIRE(supersede_active_candidate_search_session(coordinator));
        CandidateSearchResult missing = schedulable_search_result();
        missing.goals.front().selected_for_trial.clear();
        CHECK_FALSE(CandidateSearchSessionPlanner().plan(missing, 62, 2).accepted());
        CHECK(coordinator.snapshot().state == RecommendationSessionState::Canceled);
        CHECK(coordinator.snapshot().attempt_id == 1);
        CHECK(coordinator.snapshot().cancellation_reason ==
              RecommendationCancellationReason::Superseded);
    }
}
