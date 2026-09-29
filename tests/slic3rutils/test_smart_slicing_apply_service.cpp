#include <catch2/catch_all.hpp>

#include "slic3r/AI/SmartSlicing/Application/ApplyService.hpp"
#include "slic3r/AI/SmartSlicing/Application/VersionedApplyWorkflow.hpp"
#include "slic3r/AI/SmartSlicing/Ports/IOfficialSliceGateway.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <locale>

using namespace Slic3r::AI::SmartSlicing;

namespace {

EvidenceValue<double> available(double value, const char* source)
{
    return {EvidenceAvailability::Available, value, source};
}

template<class T> EvidenceValue<T> known(T value, const char* source)
{
    return {EvidenceAvailability::Available, std::move(value), source};
}

void populate_complete_capability(WorkspaceContext& workspace)
{
    MachineCapabilityEvidence& evidence = workspace.machine_capability.evidence;
    evidence.independently_addressable = known(true, "machine_profile");
    evidence.nozzles.push_back({1, 0.4, known(NozzleOffset{1.0, 2.0}, "offset"),
        known(ReachableArea{0.0, 200.0, 0.0, 200.0}, "reach")});
    evidence.nozzles.push_back({2, 0.4, known(NozzleOffset{3.0, 4.0}, "offset"),
        known(ReachableArea{1.0, 199.0, 1.0, 199.0}, "reach")});
    evidence.collision_clearance = known(CollisionClearanceLimits{1.0, 2.0, 3.0}, "collision");
    evidence.tool_change_gcode = known(ToolChangeGcodeCapability{"T[next]", true}, "tool_change");
    evidence.preheat_behavior = known(PreheatBehavior{5.0, 190.0}, "preheat");
    evidence.standby_behavior = known(StandbyBehavior{150.0, 2.0}, "standby");
    evidence.retraction_behavior = known(RetractionBehavior{1.5, 30.0}, "retraction");
    evidence.tool_change_time_model = known(ToolChangeTimeModel{2.0, 3.0}, "time_model");
    evidence.prime_or_wipe = known(PrimeOrWipeCapability{true, true}, "prime_wipe");
    evidence.flush_matrix = known(FlushMatrix{2, {0.0, 10.0, 20.0, 0.0}}, "flush");
    evidence.wipe_tower_space_constraints =
        known(WipeTowerSpaceConstraints{15.0, 20.0, 3.0}, "wipe_tower");
    evidence.specialized_validation_complete = known(true, "validation");
    workspace.machine_capability.profile.inheritance_chain = {"machine-parent-a", "machine-parent-b"};
    workspace.machine_capability.reasons = {MachineCapabilityReason::MissingNozzleOffset,
                                            MachineCapabilityReason::MissingReachableArea};

    MaterialCapabilitySnapshot material;
    material.profile.setting_id = "material-a";
    material.profile.inheritance_chain = {"material-parent-a", "material-parent-b"};
    material.profile.fingerprint = "material-fingerprint-a";
    material.family = MaterialFamily::PLA;
    material.support_status = MaterialSupportStatus::Enabled;
    material.speed_policy.may_increase_process_speed = true;
    material.boundary_policy.allowed_machine_setting_ids = {"machine-b", "machine-a"};
    material.boundary_policy.allowed_nozzle_diameters_mm = {0.6, 0.4};
    material.boundary_policy.temperature_boundary_source = "temperature-profile";
    material.boundary_policy.flow_boundary_source = "flow-profile";
    material.boundary_policy.validated = true;
    material.reasons = {MaterialCompatibilityReason::RestrictedVariant,
                        MaterialCompatibilityReason::ProfileFingerprintMismatch};
    workspace.material_compatibility.materials = {material};
    workspace.material_compatibility.reasons = {MaterialCompatibilityReason::RestrictedVariant,
                                                MaterialCompatibilityReason::ProfileFingerprintMismatch};
}

RecommendationEvidence ready_evidence()
{
    RecommendationEvidence evidence;
    evidence.selection_policy_version = CANDIDATE_SELECTION_POLICY_VERSION;
    evidence.explanation_codes = {"appearance_risk_reduced"};
    evidence.estimated_time_ratio = available(1.05, "trial_time");
    evidence.material_ratio = available(1.02, "trial_material");
    evidence.appearance_risk = available(0.1, "risk_appearance");
    evidence.dimensional_risk = available(0.2, "risk_dimensional");
    evidence.strength_risk = available(0.1, "risk_strength");
    evidence.retained_strength_ratio = available(0.98, "risk_strength_retained");
    evidence.reliability_risk = available(0.1, "risk_reliability");
    evidence.protected_region_risk = available(0.1, "risk_protected_region");
    REQUIRE(validate_recommendation_evidence(evidence).empty());
    return evidence;
}

RiskConfirmationPublicationIdentity publication_identity(const ApplyExpectedContext& context)
{
    const GoalResult& goal = context.session.recommendation.goal_result(context.goal);
    return {context.session.workflow_id,
            context.session.attempt_id,
            context.session.workspace_revision,
            context.goal,
            goal.candidate_id,
            goal.selected_candidate_id};
}

void publish_risk_confirmation_contract(ApplyExpectedContext& context,
                                        bool support_contact = true, bool seam = true,
                                        bool reverse_facts = false)
{
    RiskConfirmationMappingInput input;
    input.publication_identity = publication_identity(context);
    input.published_evidence =
        *context.session.recommendation.goal_result(context.goal).evidence;
    input.facts = {
        {RiskConfirmationKind::ProtectedRegionSupportContact,
         RiskConfirmationFactAvailability::Known, support_contact,
         "protected_region_support_contact", "protected-region-owner/v1"},
        {RiskConfirmationKind::ProtectedRegionSeam,
         RiskConfirmationFactAvailability::Known, seam, "protected_region_seam",
         "protected-region-owner/v1"},
    };
    if (reverse_facts)
        std::reverse(input.facts.begin(), input.facts.end());
    const RiskConfirmationMappingResult mapped = RiskConfirmationMapper{}.map(input);
    REQUIRE(mapped.accepted());
    context.session.recommendation.goal_result(context.goal).risk_confirmation_contract =
        mapped.contract;
}

ApplyExpectedContext ready_context()
{
    ApplyExpectedContext context;
    context.session.workflow_id = 41;
    context.session.attempt_id = 7;
    context.session.workspace_revision = {1, 2, 3, "revision-ready"};
    context.session.state = RecommendationSessionState::Ready;
    context.session.strategy_version = "candidate-search-strategy/v1";
    context.session.recommendation.baseline = {"baseline", GoalResultStatus::Ready, {}};
    GoalResult& goal = context.session.recommendation.goal_result(RecommendationGoal::Balanced);
    goal.candidate_id = "balanced-task";
    goal.status = GoalResultStatus::Ready;
    goal.evidence = ready_evidence();
    goal.selected_candidate_id = "balanced-selected";

    context.goal = RecommendationGoal::Balanced;
    context.selected_candidate.id = "balanced-selected";
    context.selected_candidate.base_revision = context.session.workspace_revision;
    context.selected_candidate.status = CandidateStatus::Ready;
    context.selected_candidate.parameters.goal = RecommendationGoal::Balanced;
    context.selected_candidate.parameters.policy_version = PARAMETER_POLICY_VERSION;
    context.selected_candidate.parameters.entries.push_back(
        {ConfigScope::Plate, PresetOwner::Process, 0, "enable_support", false, true,
         "support_required"});

    context.workspace.revision = context.session.workspace_revision;
    context.workspace.machine_capability.registry_version = "machine-registry/v1";
    context.workspace.machine_capability.profile.setting_id = "machine-profile";
    context.workspace.machine_capability.profile.fingerprint = "machine-fingerprint";
    context.workspace.machine_capability.support_status = MachineSupportStatus::Enabled;
    context.workspace.material_compatibility.registry_version = "material-registry/v1";
    context.workspace.material_compatibility.combination_status = MaterialCombinationStatus::Compatible;
    context.workspace.material_compatibility.common_family = MaterialFamily::PLA;

    context.parameter_validation.goal = RecommendationGoal::Balanced;
    context.parameter_validation.current_values.push_back(
        {ConfigScope::Plate, PresetOwner::Process, 0, "enable_support", false});
    context.parameter_validation.native_validator = [](const ParameterProposal&) {
        return NativeParameterValidationResult{true, {}};
    };
    context.activity.source_version = "orca-owner-activity/v1";
    publish_risk_confirmation_contract(context);
    return context;
}

RiskConfirmationToken issue_token(ApplyService& service, const ReadyApplyBinding& binding,
                                  const ApplyExpectedContext& context)
{
    const auto token = service.issue_confirmation_token(
        binding, context,
        {{RiskConfirmationKind::ProtectedRegionSeam,
          RiskConfirmationKind::ProtectedRegionSupportContact}});
    REQUIRE(token.accepted());
    return *token.token;
}

ApplyCommand command_for(std::string id, const ReadyApplyBinding& binding,
                         const RiskConfirmationToken& token)
{
    ApplyCommand command;
    command.command_id = std::move(id);
    command.binding = binding;
    command.confirmation_token = token;
    return command;
}

class CountingGateway final : public IOfficialSliceGateway
{
public:
    OfficialSliceResult prepare(const SliceCandidate&, const WorkspaceRevision&) override
    {
        ++calls;
        return {};
    }
    OfficialSliceResult commit(const SliceCandidate&, const WorkspaceRevision&) override
    {
        ++calls;
        return {};
    }
    OfficialSliceResult commit_plan(const AtomicApplyPlan&) override
    {
        ++calls;
        return {};
    }
    OfficialSliceResult poll() override
    {
        ++calls;
        return {};
    }
    bool undo_last_apply() override
    {
        ++calls;
        return false;
    }

    size_t calls{0};
};

class VersionedCountingGateway final : public IOfficialSliceGateway
{
public:
    OfficialSliceResult prepare(const SliceCandidate&, const WorkspaceRevision&) override { return {}; }
    OfficialSliceResult commit(const SliceCandidate&, const WorkspaceRevision&) override { return {}; }
    OfficialSliceResult commit_plan(const AtomicApplyPlan& plan) override
    {
        ++apply_calls;
        OfficialApplyTransactionIdentity identity;
        identity.plan_id = plan.plan_id;
        identity.candidate_id = plan.candidate.id;
        identity.applied_revision = {2, 3, 4, "applied"};
        identity.snapshot = {10, 11, "Apply Smart Slicing Candidate"};
        identity.workflow_id = plan.binding.workflow_id;
        identity.attempt_id = plan.binding.attempt_id;
        active = identity;
        return {OfficialSlicePhase::Applied, {}, true, true, identity};
    }
    OfficialSliceResult start_committed_plan_slice(const OfficialApplyTransactionIdentity& identity) override
    {
        REQUIRE(active.has_value());
        REQUIRE(*active == identity);
        ++slice_calls;
        return {OfficialSlicePhase::Slicing, {}, true, true, active};
    }
    OfficialSliceResult retry_official_slice(const OfficialApplyTransactionIdentity& identity) override
    {
        REQUIRE(active.has_value());
        REQUIRE(*active == identity);
        ++retry_calls;
        return {OfficialSlicePhase::Slicing, {}, true, true, active};
    }
    OfficialSliceResult complete_official_slice(const OfficialApplyTransactionIdentity& identity,
                                                bool success, std::string diagnostic) override
    {
        REQUIRE(active.has_value());
        REQUIRE(*active == identity);
        return {success ? OfficialSlicePhase::Completed : OfficialSlicePhase::Failed,
                success ? std::string{} : std::move(diagnostic), true, true, active,
                !success, success};
    }
    OfficialSliceResult poll_committed_plan(const OfficialApplyTransactionIdentity&) override { return {}; }
    OfficialSliceResult undo_committed_plan(const OfficialApplyTransactionIdentity&) override
    {
        ++undo_calls;
        active.reset();
        return {OfficialSlicePhase::Prepared, "apply_undone", false, false};
    }
    OfficialSliceResult poll() override { return {}; }
    bool undo_last_apply() override { return false; }

    size_t apply_calls{0};
    size_t slice_calls{0};
    size_t retry_calls{0};
    size_t undo_calls{0};
    std::optional<OfficialApplyTransactionIdentity> active;
};

ApplyGuardResult make_plan(ApplyService& service, std::string id, const ReadyApplyBinding& binding,
                           const RiskConfirmationToken& token, const ApplyExpectedContext& context)
{
    return service.make_atomic_plan(command_for(std::move(id), binding, token), binding, context);
}

} // namespace

TEST_CASE("versioned application commits once and retries only the official slice", "[AI][SmartSlicing][D6T4]")
{
    ApplyExpectedContext context = ready_context();
    VersionedCountingGateway gateway;
    VersionedApplyWorkflow workflow(gateway);
    VersionedApplyRequest request;
    request.command_id = "apply-command-d6t4";
    request.confirmation.confirmed_risks = {
        RiskConfirmationKind::ProtectedRegionSeam,
        RiskConfirmationKind::ProtectedRegionSupportContact};

    const OfficialSliceResult started = workflow.start(request, context);
    REQUIRE(started.phase == OfficialSlicePhase::Slicing);
    REQUIRE(workflow.active_transaction());
    CHECK(gateway.apply_calls == 1);
    CHECK(gateway.slice_calls == 1);
    CHECK(workflow.start(request, context).diagnostic_code == "official_transaction_active");
    CHECK(gateway.apply_calls == 1);

    const OfficialSliceResult failed = workflow.notify_slice_completed(false, "official_slice_failed");
    CHECK(failed.phase == OfficialSlicePhase::Failed);
    CHECK(workflow.retry(*workflow.active_transaction()).phase == OfficialSlicePhase::Slicing);
    CHECK(gateway.retry_calls == 1);
    CHECK(gateway.apply_calls == 1);
}

namespace {

class CommaNumpunct final : public std::numpunct<char>
{
protected:
    char do_decimal_point() const override { return ','; }
    char do_thousands_sep() const override { return '_'; }
    std::string do_grouping() const override { return "\3"; }
};

} // namespace

TEST_CASE("D6 apply guard produces one versioned plan without entering the gateway",
          "[AI][SmartSlicing][D6T1][ApplyGuard]")
{
    ApplyExpectedContext context = ready_context();
    ApplyService service;
    const ReadyApplyBindingResult captured = service.capture_ready_binding(context);
    REQUIRE(captured.accepted());
    const RiskConfirmationToken token = issue_token(service, *captured.binding, context);
    CountingGateway gateway;

    const ApplyGuardResult result = make_plan(service, "command-1", *captured.binding, token, context);
    REQUIRE(result.accepted());
    CHECK(result.rejection == ApplyRejectionCode::None);
    CHECK(result.plan->schema == APPLY_PLAN_SCHEMA);
    CHECK(result.plan->version == APPLY_PLAN_VERSION);
    CHECK(result.plan->plan_id == "apply-plan:command-1");
    CHECK(result.plan->binding == *captured.binding);
    CHECK(result.plan->candidate.id == "balanced-selected");
    CHECK(result.plan->confirmation_token_id == token.token_id());
    CHECK(service.consumed_command_count() == 1);
    CHECK(gateway.calls == 0);
}

TEST_CASE("D6 confirmation tokens require the exact current risk set and cannot self attest",
          "[AI][SmartSlicing][D6T1][ApplyGuard]")
{
    ApplyExpectedContext context = ready_context();
    ApplyService service;
    const auto captured = service.capture_ready_binding(context);
    REQUIRE(captured.accepted());

    CHECK(service.issue_confirmation_token(*captured.binding, context, {}).rejection ==
          ApplyRejectionCode::RiskConfirmationRequired);
    CHECK(service.issue_confirmation_token(*captured.binding, context,
        {{RiskConfirmationKind::ProtectedRegionSupportContact,
          RiskConfirmationKind::ProtectedRegionSupportContact,
          RiskConfirmationKind::ProtectedRegionSeam}}).rejection ==
          ApplyRejectionCode::RiskConfirmationRequired);
    CHECK(service.issue_confirmation_token(*captured.binding, context,
        {{RiskConfirmationKind::ProtectedRegionSupportContact}}).rejection ==
          ApplyRejectionCode::RiskConfirmationRequired);
    CHECK(service.issue_confirmation_token(*captured.binding, context,
        {{RiskConfirmationKind::ProtectedRegionSupportContact,
          RiskConfirmationKind::ProtectedRegionSeam,
          static_cast<RiskConfirmationKind>(99)}}).rejection ==
          ApplyRejectionCode::RiskConfirmationRequired);

    const RiskConfirmationToken token = issue_token(service, *captured.binding, context);
    ApplyCommand missing;
    missing.command_id = "missing-token";
    missing.binding = *captured.binding;
    CHECK(service.make_atomic_plan(missing, *captured.binding, context).rejection ==
          ApplyRejectionCode::RiskConfirmationRequired);

    ApplyExpectedContext other = context;
    other.session.workflow_id = 99;
    publish_risk_confirmation_contract(other);
    const auto other_binding = service.capture_ready_binding(other);
    REQUIRE(other_binding.accepted());
    const RiskConfirmationToken other_token = issue_token(service, *other_binding.binding, other);
    CHECK(make_plan(service, "wrong-token", *captured.binding, other_token, context).rejection ==
          ApplyRejectionCode::RiskConfirmationTokenMismatch);

    context.session.recommendation.goal_result(context.goal).evidence->appearance_risk.value = 0.3;
    CHECK(make_plan(service, "expired-token", *captured.binding, token, context).rejection ==
          ApplyRejectionCode::RiskEvidenceChanged);
}

TEST_CASE("D6 malformed goal enum fails closed without throwing",
          "[AI][SmartSlicing][D6T1][ApplyGuard]")
{
    ApplyService service;
    ApplyExpectedContext invalid_goal = ready_context();
    invalid_goal.goal = static_cast<RecommendationGoal>(99);
    const auto goal_result = service.capture_ready_binding(invalid_goal);
    CHECK_FALSE(goal_result.accepted());
    CHECK(goal_result.rejection == ApplyRejectionCode::InvalidContract);
    REQUIRE(goal_result.diagnostic_details.size() == 1);
    CHECK(goal_result.diagnostic_details.front() == "recommendation_goal_invalid");

}

TEST_CASE("D6 binding requires a ready candidate and versioned registries",
          "[AI][SmartSlicing][D6T1][ApplyGuard]")
{
    ApplyService service;
    ApplyExpectedContext candidate = ready_context();
    candidate.selected_candidate.status = GENERATE(CandidateStatus::Draft, CandidateStatus::Stale);
    CHECK(service.capture_ready_binding(candidate).rejection == ApplyRejectionCode::CandidateNotReady);

    ApplyExpectedContext machine = ready_context();
    machine.workspace.machine_capability.registry_version.clear();
    const auto missing_machine = service.capture_ready_binding(machine);
    CHECK(missing_machine.rejection == ApplyRejectionCode::MachineCapabilityChanged);
    REQUIRE(missing_machine.diagnostic_details.size() == 1);
    CHECK(missing_machine.diagnostic_details.front() == "machine_registry_version_missing");

    ApplyExpectedContext material = ready_context();
    material.workspace.material_compatibility.registry_version.clear();
    const auto missing_material = service.capture_ready_binding(material);
    CHECK(missing_material.rejection == ApplyRejectionCode::MaterialCompatibilityChanged);
    REQUIRE(missing_material.diagnostic_details.size() == 1);
    CHECK(missing_material.diagnostic_details.front() == "material_registry_version_missing");
}

TEST_CASE("D6 command ids use the stable bounded ASCII code grammar",
          "[AI][SmartSlicing][D6T1][ApplyGuard]")
{
    const ApplyExpectedContext context = ready_context();
    ApplyService issuer;
    const auto binding = issuer.capture_ready_binding(context);
    REQUIRE(binding.accepted());
    const auto token = issue_token(issuer, *binding.binding, context);

    ApplyService service;
    const std::vector<std::string> invalid_ids{"", "contains\nnewline", "Uppercase", std::string(129, 'a')};
    for (const std::string& id : invalid_ids) {
        const auto result = make_plan(service, id, *binding.binding, token, context);
        CHECK(result.rejection == ApplyRejectionCode::InvalidCommandId);
        CHECK_FALSE(result.accepted());
    }
    CHECK(service.consumed_command_count() == 0);
    CHECK(make_plan(service, "valid-command_1.0", *binding.binding, token, context).accepted());
    CHECK(service.consumed_command_count() == 1);
}

TEST_CASE("D6 command binding rejects every identity and policy mismatch before gateway",
          "[AI][SmartSlicing][D6T1][ApplyGuard]")
{
    ApplyExpectedContext context = ready_context();
    ApplyService issuer;
    const auto captured = issuer.capture_ready_binding(context);
    REQUIRE(captured.accepted());
    const RiskConfirmationToken token = issue_token(issuer, *captured.binding, context);
    CountingGateway gateway;

    auto check = [&](const char* id, const std::function<void(ReadyApplyBinding&)>& mutate,
                     ApplyRejectionCode expected) {
        ApplyService service;
        ApplyCommand command = command_for(id, *captured.binding, token);
        mutate(command.binding);
        const ApplyGuardResult result = service.make_atomic_plan(command, *captured.binding, context);
        CHECK(result.rejection == expected);
        CHECK_FALSE(result.accepted());
        CHECK(gateway.calls == 0);
    };
    check("workflow", [](auto& value) { ++value.workflow_id; }, ApplyRejectionCode::WorkflowChanged);
    check("attempt", [](auto& value) { ++value.attempt_id; }, ApplyRejectionCode::AttemptChanged);
    check("revision", [](auto& value) { value.base_revision.fingerprint = "old"; },
          ApplyRejectionCode::WorkspaceChanged);
    check("goal", [](auto& value) { value.goal = RecommendationGoal::Speed; },
          ApplyRejectionCode::GoalChanged);
    check("task", [](auto& value) { value.goal_task_candidate_id = "wrong-task"; },
          ApplyRejectionCode::GoalTaskCandidateChanged);
    check("selected", [](auto& value) { value.selected_candidate_id = "wrong-selected"; },
          ApplyRejectionCode::SelectedCandidateChanged);
    check("strategy", [](auto& value) { value.strategy_version = "future"; },
          ApplyRejectionCode::StrategyVersionChanged);
    check("selection-policy", [](auto& value) { value.selection_policy_version = "future"; },
          ApplyRejectionCode::SelectionPolicyVersionChanged);
    check("parameter-policy", [](auto& value) { value.parameter_policy_version = "future"; },
          ApplyRejectionCode::ParameterPolicyVersionChanged);
    check("risk", [](auto& value) { value.risk_evidence_digest = "changed"; },
          ApplyRejectionCode::RiskEvidenceChanged);
    check("confirmations", [](auto& value) { value.required_confirmations.pop_back(); },
          ApplyRejectionCode::RequiredConfirmationsChanged);
}

TEST_CASE("D6 current expected context rejects stale late and changed evidence",
          "[AI][SmartSlicing][D6T1][ApplyGuard]")
{
    ApplyExpectedContext original = ready_context();
    ApplyService issuer;
    const auto captured = issuer.capture_ready_binding(original);
    REQUIRE(captured.accepted());
    const RiskConfirmationToken token = issue_token(issuer, *captured.binding, original);
    CountingGateway gateway;

    auto check = [&](const char* id, const std::function<void(ApplyExpectedContext&)>& mutate,
                     ApplyRejectionCode expected) {
        ApplyExpectedContext current = original;
        mutate(current);
        ApplyService service;
        const auto result = make_plan(service, id, *captured.binding, token, current);
        CHECK(result.rejection == expected);
        CHECK_FALSE(result.accepted());
        CHECK(gateway.calls == 0);
    };
    check("late-attempt", [](auto& value) { ++value.session.attempt_id; },
          ApplyRejectionCode::AttemptChanged);
    check("stale-revision", [](auto& value) {
        value.session.workspace_revision.fingerprint = "new";
        value.workspace.revision = value.session.workspace_revision;
    }, ApplyRejectionCode::WorkspaceChanged);
    check("not-ready", [](auto& value) {
        value.session.recommendation.goal_result(value.goal).status = GoalResultStatus::Applied;
    }, ApplyRejectionCode::GoalNotReady);
    check("intent", [](auto& value) {
        IntentConstraintRecord record;
        record.type = IntentConstraintType::SupportPainting;
        record.state = IntentConstraintState::Active;
        value.workspace.intent_constraints.records.push_back(record);
        value.parameter_validation.intent_constraints = value.workspace.intent_constraints;
    }, ApplyRejectionCode::IntentConstraintChanged);
    check("machine-version", [](auto& value) {
        value.workspace.machine_capability.registry_version = "machine-registry/v2";
    }, ApplyRejectionCode::MachineCapabilityChanged);
    check("machine-status", [](auto& value) {
        value.workspace.machine_capability.support_status = MachineSupportStatus::PendingValidation;
    }, ApplyRejectionCode::MachineCapabilityChanged);
    check("material-version", [](auto& value) {
        value.workspace.material_compatibility.registry_version = "material-registry/v2";
    }, ApplyRejectionCode::MaterialCompatibilityChanged);
    check("required-risk", [](auto& value) {
        publish_risk_confirmation_contract(value, true, false);
    },
          ApplyRejectionCode::RequiredConfirmationsChanged);
    check("active-tool", [](auto& value) { value.activity.model_tool_active = true; },
          ApplyRejectionCode::ActiveModelTool);
    check("active-transaction", [](auto& value) { value.activity.official_transaction_active = true; },
          ApplyRejectionCode::ActiveOfficialTransaction);
}

TEST_CASE("D6 expected value scope owner and native checks are rerun at apply time",
          "[AI][SmartSlicing][D6T1][ApplyGuard]")
{
    const auto check = [](ApplyExpectedContext context, const char* id, const char* detail) {
        ApplyService service;
        const auto binding = service.capture_ready_binding(context);
        REQUIRE(binding.accepted());
        const RiskConfirmationToken token = issue_token(service, *binding.binding, context);
        const auto result = make_plan(service, id, *binding.binding, token, context);
        CHECK(result.rejection == ApplyRejectionCode::ParameterRevalidationFailed);
        REQUIRE(result.diagnostic_details.size() == 1);
        CHECK(result.diagnostic_details.front() == detail);
    };

    ApplyExpectedContext expected_changed = ready_context();
    expected_changed.parameter_validation.current_values.front().value = true;
    check(expected_changed, "expected-value", "parameter_expected_value_changed");

    ApplyExpectedContext wrong_scope = ready_context();
    wrong_scope.selected_candidate.parameters.entries.front().scope = ConfigScope::Object;
    wrong_scope.selected_candidate.parameters.entries.front().target_id = 1;
    wrong_scope.parameter_validation.current_values.front().scope = ConfigScope::Object;
    wrong_scope.parameter_validation.current_values.front().target_id = 1;
    check(wrong_scope, "scope", "parameter_scope_not_allowed");

    ApplyExpectedContext wrong_owner = ready_context();
    wrong_owner.selected_candidate.parameters.entries.front().owner = PresetOwner::Printer;
    wrong_owner.parameter_validation.current_values.front().owner = PresetOwner::Printer;
    check(wrong_owner, "owner", "parameter_owner_not_allowed");

    ApplyExpectedContext native_rejected = ready_context();
    native_rejected.parameter_validation.native_validator = [](const ParameterProposal&) {
        return NativeParameterValidationResult{false, "native_apply_rejected"};
    };
    check(native_rejected, "native", "native_apply_rejected");
}

TEST_CASE("D6 parameter apply context must match owner workspace intent and goal",
          "[AI][SmartSlicing][D6T1][ApplyGuard]")
{
    ApplyExpectedContext stale_intent = ready_context();
    IntentConstraintRecord painting;
    painting.type = IntentConstraintType::SupportPainting;
    painting.state = IntentConstraintState::Active;
    stale_intent.workspace.intent_constraints.records.push_back(painting);
    ApplyService intent_service;
    const auto intent_binding = intent_service.capture_ready_binding(stale_intent);
    REQUIRE(intent_binding.accepted());
    const auto intent_token = issue_token(intent_service, *intent_binding.binding, stale_intent);
    CHECK(make_plan(intent_service, "stale-parameter-intent", *intent_binding.binding, intent_token,
                    stale_intent).rejection == ApplyRejectionCode::IntentConstraintChanged);

    ApplyExpectedContext wrong_goal = ready_context();
    wrong_goal.parameter_validation.goal = RecommendationGoal::Speed;
    ApplyService goal_service;
    const auto goal_binding = goal_service.capture_ready_binding(wrong_goal);
    REQUIRE(goal_binding.accepted());
    const auto goal_token = issue_token(goal_service, *goal_binding.binding, wrong_goal);
    CHECK(make_plan(goal_service, "wrong-parameter-goal", *goal_binding.binding, goal_token,
                    wrong_goal).rejection == ApplyRejectionCode::GoalChanged);
}

TEST_CASE("D6 capability digest covers complete machine and material evidence deterministically",
          "[AI][SmartSlicing][D6T1][ApplyGuard]")
{
    ApplyExpectedContext context = ready_context();
    populate_complete_capability(context.workspace);
    const MachineCapabilitySnapshot machine = context.workspace.machine_capability;
    const MaterialCompatibilitySnapshot materials = context.workspace.material_compatibility;
    const std::string machine_digest = apply_machine_capability_evidence_digest(machine);
    const std::string material_digest = apply_material_compatibility_evidence_digest(materials);

    const auto machine_changes = std::vector<std::function<void(MachineCapabilitySnapshot&)>>{
        [](auto& value) { value.evidence.independently_addressable.value = false; },
        [](auto& value) { value.evidence.nozzles.front().offset.value->x_mm += 1.0; },
        [](auto& value) { value.evidence.nozzles.front().reachable_area.value->maximum_x_mm -= 1.0; },
        [](auto& value) { value.evidence.collision_clearance.value->vertical_clearance_mm += 1.0; },
        [](auto& value) { value.evidence.tool_change_gcode.value->command_template += "x"; },
        [](auto& value) { value.evidence.preheat_behavior.value->lead_time_seconds += 1.0; },
        [](auto& value) { value.evidence.standby_behavior.value->standby_temperature_c += 1.0; },
        [](auto& value) { value.evidence.retraction_behavior.value->length_mm += 1.0; },
        [](auto& value) { value.evidence.tool_change_time_model.value->fixed_seconds += 1.0; },
        [](auto& value) { value.evidence.prime_or_wipe.value->precharge_supported = false; },
        [](auto& value) { value.evidence.flush_matrix.value->volume_mm3[1] += 1.0; },
        [](auto& value) { value.evidence.wipe_tower_space_constraints.value->clearance_mm += 1.0; },
        [](auto& value) { value.evidence.specialized_validation_complete.value = false; },
        [](auto& value) { value.profile.inheritance_chain.front() += "x"; },
        [](auto& value) { std::swap(value.evidence.nozzles[0], value.evidence.nozzles[1]); },
    };
    for (const auto& change : machine_changes) {
        MachineCapabilitySnapshot changed = machine;
        change(changed);
        CHECK(apply_machine_capability_evidence_digest(changed) != machine_digest);
    }

    const auto material_changes = std::vector<std::function<void(MaterialCompatibilitySnapshot&)>>{
        [](auto& value) { value.materials.front().profile.inheritance_chain.front() += "x"; },
        [](auto& value) { value.materials.front().speed_policy.may_increase_process_speed = false; },
        [](auto& value) { value.materials.front().boundary_policy.allowed_machine_setting_ids.push_back("c"); },
        [](auto& value) { value.materials.front().boundary_policy.allowed_nozzle_diameters_mm.push_back(0.8); },
        [](auto& value) { value.materials.front().boundary_policy.temperature_boundary_source += "x"; },
        [](auto& value) { value.materials.front().boundary_policy.flow_boundary_source += "x"; },
        [](auto& value) { value.materials.front().boundary_policy.validated = false; },
        [](auto& value) { value.materials.front().reasons.pop_back(); },
    };
    for (const auto& change : material_changes) {
        MaterialCompatibilitySnapshot changed = materials;
        change(changed);
        CHECK(apply_material_compatibility_evidence_digest(changed) != material_digest);
    }

    MaterialCompatibilitySnapshot reordered = materials;
    std::reverse(reordered.materials.front().boundary_policy.allowed_machine_setting_ids.begin(),
                 reordered.materials.front().boundary_policy.allowed_machine_setting_ids.end());
    std::reverse(reordered.materials.front().boundary_policy.allowed_nozzle_diameters_mm.begin(),
                 reordered.materials.front().boundary_policy.allowed_nozzle_diameters_mm.end());
    std::reverse(reordered.materials.front().reasons.begin(), reordered.materials.front().reasons.end());
    std::reverse(reordered.reasons.begin(), reordered.reasons.end());
    CHECK(apply_material_compatibility_evidence_digest(reordered) == material_digest);
}

TEST_CASE("D6 canonical apply digests are unambiguous and locale independent",
          "[AI][SmartSlicing][D6T1][ApplyGuard]")
{
    RecommendationEvidence first_evidence = ready_evidence();
    first_evidence.explanation_codes = {"a:b\n", "c"};
    RecommendationEvidence second_evidence = first_evidence;
    second_evidence.explanation_codes = {"a", "b\n:c"};
    CHECK(recommendation_risk_evidence_digest(first_evidence) !=
          recommendation_risk_evidence_digest(second_evidence));

    ApplyExpectedContext context = ready_context();
    context.selected_candidate.id = "selected:\nvalue";
    context.session.recommendation.goal_result(context.goal).selected_candidate_id =
        context.selected_candidate.id;
    context.selected_candidate.parameters.entries.front().key = "enable_support:\nkey";
    context.selected_candidate.parameters.entries.front().reason_code = "reason:\nvalue";
    publish_risk_confirmation_contract(context);
    const std::string candidate_digest = apply_candidate_payload_digest(context.selected_candidate);
    const std::string evidence_digest = recommendation_risk_evidence_digest(first_evidence);

    ApplyService service;
    const auto binding = service.capture_ready_binding(context);
    REQUIRE(binding.accepted());
    const auto token = issue_token(service, *binding.binding, context);

    const std::locale previous = std::locale();
    std::string localized_candidate;
    std::string localized_evidence;
    std::string localized_token;
    try {
        std::locale::global(std::locale(previous, new CommaNumpunct));
        localized_candidate = apply_candidate_payload_digest(context.selected_candidate);
        localized_evidence = recommendation_risk_evidence_digest(first_evidence);
        localized_token = issue_token(service, *binding.binding, context).token_id();
        std::locale::global(previous);
    } catch (...) {
        std::locale::global(previous);
        throw;
    }
    CHECK(localized_candidate == candidate_digest);
    CHECK(localized_evidence == evidence_digest);
    CHECK(localized_token == token.token_id());

    SliceCandidate repartitioned = context.selected_candidate;
    repartitioned.parameters.entries.front().key = "enable_support:";
    repartitioned.parameters.entries.front().reason_code = "\nkeyreason:\nvalue";
    CHECK(apply_candidate_payload_digest(repartitioned) != candidate_digest);
}

TEST_CASE("D6 confirmation token identity binds the complete ready apply binding",
          "[AI][SmartSlicing][D6T1][ApplyGuard]")
{
    const auto token_id_for = [](const ApplyExpectedContext& context) {
        ApplyService service;
        const auto binding = service.capture_ready_binding(context);
        REQUIRE(binding.accepted());
        return issue_token(service, *binding.binding, context).token_id();
    };

    const ApplyExpectedContext base = ready_context();
    const std::string base_token = token_id_for(base);

    ApplyExpectedContext reordered = base;
    publish_risk_confirmation_contract(reordered, true, true, true);
    CHECK(token_id_for(reordered) == base_token);

    std::vector<ApplyExpectedContext> changed;
    ApplyExpectedContext intent = base;
    IntentConstraintRecord record;
    record.type = IntentConstraintType::SeamPainting;
    record.state = IntentConstraintState::Active;
    intent.workspace.intent_constraints.records.push_back(record);
    intent.parameter_validation.intent_constraints = intent.workspace.intent_constraints;
    changed.push_back(intent);

    ApplyExpectedContext machine_registry = base;
    machine_registry.workspace.machine_capability.registry_version += "-changed";
    changed.push_back(machine_registry);
    ApplyExpectedContext machine_evidence = base;
    machine_evidence.workspace.machine_capability.evidence.independently_addressable =
        known(true, "changed_machine_evidence");
    changed.push_back(machine_evidence);

    ApplyExpectedContext material_registry = base;
    material_registry.workspace.material_compatibility.registry_version += "-changed";
    changed.push_back(material_registry);
    ApplyExpectedContext material_evidence = base;
    MaterialCapabilitySnapshot material;
    material.profile.setting_id = "material";
    material.profile.fingerprint = "fingerprint";
    material.family = MaterialFamily::PLA;
    material.support_status = MaterialSupportStatus::Enabled;
    material_evidence.workspace.material_compatibility.materials.push_back(material);
    changed.push_back(material_evidence);

    ApplyExpectedContext candidate_payload = base;
    candidate_payload.selected_candidate.parameters.entries.front().reason_code += "-changed";
    changed.push_back(candidate_payload);

    for (const ApplyExpectedContext& context : changed)
        CHECK(token_id_for(context) != base_token);
}

TEST_CASE("D6 command nonce is consumed on first processing without consuming a valid token",
          "[AI][SmartSlicing][D6T1][ApplyGuard]")
{
    ApplyExpectedContext context = ready_context();
    ApplyService service;
    const auto binding = service.capture_ready_binding(context);
    REQUIRE(binding.accepted());
    const RiskConfirmationToken token = issue_token(service, *binding.binding, context);

    ApplyCommand rejected;
    rejected.command_id = "one-shot";
    rejected.binding = *binding.binding;
    REQUIRE(service.make_atomic_plan(rejected, *binding.binding, context).rejection ==
            ApplyRejectionCode::RiskConfirmationRequired);

    rejected.confirmation_token = token;
    CHECK(service.make_atomic_plan(rejected, *binding.binding, context).rejection ==
          ApplyRejectionCode::CommandAlreadyConsumed);
    CHECK(make_plan(service, "fresh-command", *binding.binding, token, context).accepted());
    CHECK(service.make_atomic_plan(command_for("fresh-command", *binding.binding, token),
                                   *binding.binding, context).rejection ==
          ApplyRejectionCode::CommandAlreadyConsumed);
    CHECK(service.consumed_command_count() == 2);
}

TEST_CASE("D6 placement is rechecked for structure and current hard intent",
          "[AI][SmartSlicing][D6T1][ApplyGuard]")
{
    ApplyExpectedContext invalid = ready_context();
    invalid.selected_candidate.parameters.entries.clear();
    ObjectTransform transform;
    transform.object_id = 12;
    transform.instance_id = 2;
    transform.matrix[0] = std::numeric_limits<double>::quiet_NaN();
    invalid.selected_candidate.placement.transforms.push_back(transform);
    ApplyService invalid_service;
    const auto invalid_binding = invalid_service.capture_ready_binding(invalid);
    REQUIRE(invalid_binding.accepted());
    const auto invalid_token = issue_token(invalid_service, *invalid_binding.binding, invalid);
    CHECK(make_plan(invalid_service, "invalid-placement", *invalid_binding.binding, invalid_token,
                    invalid).rejection == ApplyRejectionCode::InvalidPlacement);

    const auto check_invalid_matrix = [](std::array<double, 16> matrix, const char* command_id) {
        ApplyExpectedContext context = ready_context();
        context.selected_candidate.parameters.entries.clear();
        ObjectTransform value;
        value.object_id = 12;
        value.instance_id = 2;
        value.matrix = matrix;
        context.selected_candidate.placement.transforms.push_back(value);
        ApplyService service;
        const auto binding = service.capture_ready_binding(context);
        REQUIRE(binding.accepted());
        const auto token = issue_token(service, *binding.binding, context);
        CHECK(make_plan(service, command_id, *binding.binding, token, context).rejection ==
              ApplyRejectionCode::InvalidPlacement);
    };
    check_invalid_matrix({}, "zero-matrix");
    std::array<double, 16> singular{};
    singular[0] = singular[5] = singular[15] = 1.0;
    check_invalid_matrix(singular, "singular-matrix");
    std::array<double, 16> non_affine{};
    non_affine[0] = non_affine[5] = non_affine[10] = non_affine[15] = 1.0;
    non_affine[12] = 0.1;
    check_invalid_matrix(non_affine, "non-affine-matrix");

    ApplyExpectedContext locked = ready_context();
    locked.selected_candidate.parameters.entries.clear();
    transform.matrix.fill(0.0);
    transform.matrix[0] = transform.matrix[5] = transform.matrix[10] = transform.matrix[15] = 1.0;
    locked.selected_candidate.placement.transforms.push_back(transform);
    IntentConstraintRecord lock;
    lock.type = IntentConstraintType::InstancePlacementLock;
    lock.state = IntentConstraintState::Active;
    lock.object_id = 12;
    lock.instance_id = 2;
    locked.workspace.intent_constraints.records.push_back(lock);
    locked.parameter_validation.intent_constraints = locked.workspace.intent_constraints;
    ApplyService lock_service;
    const auto lock_binding = lock_service.capture_ready_binding(locked);
    REQUIRE(lock_binding.accepted());
    const auto lock_token = issue_token(lock_service, *lock_binding.binding, locked);
    CHECK(make_plan(lock_service, "locked-placement", *lock_binding.binding, lock_token,
                    locked).rejection == ApplyRejectionCode::PlacementIntentConflict);
}
