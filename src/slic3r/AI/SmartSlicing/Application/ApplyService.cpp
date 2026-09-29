#include "ApplyService.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <set>
#include <sstream>
#include <string_view>
#include <tuple>

namespace Slic3r::AI::SmartSlicing {
namespace {

template<class Result> Result rejected(ApplyRejectionCode code, std::string detail = {})
{
    Result result;
    result.rejection = code;
    if (!detail.empty())
        result.diagnostic_details.push_back(std::move(detail));
    return result;
}

bool canonicalize_confirmations(const std::vector<RiskConfirmationKind>& input,
                                std::vector<RiskConfirmationKind>& output)
{
    output = input;
    if (std::any_of(output.begin(), output.end(), [](RiskConfirmationKind value) {
            return std::string_view(risk_confirmation_kind_name(value)) == "unknown";
        }))
        return false;
    std::sort(output.begin(), output.end());
    return std::adjacent_find(output.begin(), output.end()) == output.end();
}

ApplyRejectionCode compare_bindings(const ReadyApplyBinding& actual, const ReadyApplyBinding& expected)
{
    if (actual.workflow_id != expected.workflow_id) return ApplyRejectionCode::WorkflowChanged;
    if (actual.attempt_id != expected.attempt_id) return ApplyRejectionCode::AttemptChanged;
    if (actual.base_revision != expected.base_revision) return ApplyRejectionCode::WorkspaceChanged;
    if (actual.goal != expected.goal) return ApplyRejectionCode::GoalChanged;
    if (actual.goal_task_candidate_id != expected.goal_task_candidate_id)
        return ApplyRejectionCode::GoalTaskCandidateChanged;
    if (actual.selected_candidate_id != expected.selected_candidate_id)
        return ApplyRejectionCode::SelectedCandidateChanged;
    if (actual.strategy_version != expected.strategy_version)
        return ApplyRejectionCode::StrategyVersionChanged;
    if (actual.selection_policy_version != expected.selection_policy_version)
        return ApplyRejectionCode::SelectionPolicyVersionChanged;
    if (actual.parameter_policy_version != expected.parameter_policy_version)
        return ApplyRejectionCode::ParameterPolicyVersionChanged;
    if (actual.risk_evidence_digest != expected.risk_evidence_digest)
        return ApplyRejectionCode::RiskEvidenceChanged;
    if (actual.risk_confirmation_policy_version != expected.risk_confirmation_policy_version ||
        actual.risk_confirmation_evidence_digest != expected.risk_confirmation_evidence_digest)
        return ApplyRejectionCode::RequiredConfirmationsChanged;
    std::vector<RiskConfirmationKind> actual_confirmations;
    std::vector<RiskConfirmationKind> expected_confirmations;
    if (!canonicalize_confirmations(actual.required_confirmations, actual_confirmations) ||
        !canonicalize_confirmations(expected.required_confirmations, expected_confirmations) ||
        actual_confirmations != expected_confirmations)
        return ApplyRejectionCode::RequiredConfirmationsChanged;
    if (actual.intent_evidence_revision != expected.intent_evidence_revision)
        return ApplyRejectionCode::IntentConstraintChanged;
    if (actual.machine_registry_version != expected.machine_registry_version ||
        actual.machine_capability_evidence_digest != expected.machine_capability_evidence_digest)
        return ApplyRejectionCode::MachineCapabilityChanged;
    if (actual.material_registry_version != expected.material_registry_version ||
        actual.material_compatibility_evidence_digest != expected.material_compatibility_evidence_digest)
        return ApplyRejectionCode::MaterialCompatibilityChanged;
    if (actual.candidate_payload_digest != expected.candidate_payload_digest)
        return ApplyRejectionCode::SelectedCandidateChanged;
    return ApplyRejectionCode::None;
}

ReadyApplyBindingResult current_binding(const ApplyExpectedContext& context)
{
    if (std::string_view(recommendation_goal_id(context.goal)).empty())
        return rejected<ReadyApplyBindingResult>(ApplyRejectionCode::InvalidContract,
                                                 "recommendation_goal_invalid");
    if (!context.session.workspace_revision.valid() || !context.workspace.revision.valid() ||
        context.session.workflow_id == 0 || context.session.attempt_id == 0)
        return rejected<ReadyApplyBindingResult>(ApplyRejectionCode::InvalidContract);
    if (context.session.state != RecommendationSessionState::Ready)
        return rejected<ReadyApplyBindingResult>(ApplyRejectionCode::GoalNotReady);
    if (context.session.workspace_revision != context.workspace.revision)
        return rejected<ReadyApplyBindingResult>(ApplyRejectionCode::WorkspaceChanged);

    const GoalResult& goal = context.session.recommendation.goal_result(context.goal);
    if (goal.status != GoalResultStatus::Ready || !goal.evidence || goal.selected_candidate_id.empty())
        return rejected<ReadyApplyBindingResult>(ApplyRejectionCode::GoalNotReady);
    if (goal.candidate_id.empty())
        return rejected<ReadyApplyBindingResult>(ApplyRejectionCode::GoalTaskCandidateChanged);
    if (goal.selected_candidate_id != context.selected_candidate.id)
        return rejected<ReadyApplyBindingResult>(ApplyRejectionCode::SelectedCandidateChanged);
    if (context.selected_candidate.status != CandidateStatus::Ready)
        return rejected<ReadyApplyBindingResult>(ApplyRejectionCode::CandidateNotReady);
    if (context.selected_candidate.base_revision != context.session.workspace_revision)
        return rejected<ReadyApplyBindingResult>(ApplyRejectionCode::WorkspaceChanged);
    const std::vector<std::string> evidence_errors = validate_recommendation_evidence(*goal.evidence);
    if (!evidence_errors.empty())
        return rejected<ReadyApplyBindingResult>(ApplyRejectionCode::RiskEvidenceChanged,
                                                 evidence_errors.front());
    if (context.session.strategy_version.empty())
        return rejected<ReadyApplyBindingResult>(ApplyRejectionCode::StrategyVersionChanged);
    if (goal.evidence->selection_policy_version.empty())
        return rejected<ReadyApplyBindingResult>(ApplyRejectionCode::SelectionPolicyVersionChanged);
    if (context.selected_candidate.parameters.policy_version.empty() ||
        context.selected_candidate.parameters.goal != context.goal)
        return rejected<ReadyApplyBindingResult>(ApplyRejectionCode::ParameterPolicyVersionChanged);
    if (context.workspace.machine_capability.registry_version.empty())
        return rejected<ReadyApplyBindingResult>(ApplyRejectionCode::MachineCapabilityChanged,
                                                 "machine_registry_version_missing");
    if (context.workspace.material_compatibility.registry_version.empty())
        return rejected<ReadyApplyBindingResult>(ApplyRejectionCode::MaterialCompatibilityChanged,
                                                 "material_registry_version_missing");

    if (!goal.risk_confirmation_contract)
        return rejected<ReadyApplyBindingResult>(ApplyRejectionCode::RequiredConfirmationsChanged,
                                                 "risk_confirmation_contract_missing");
    const OwnerRiskConfirmationContract& confirmation = *goal.risk_confirmation_contract;
    const RiskConfirmationPublicationIdentity expected_publication{
        context.session.workflow_id,
        context.session.attempt_id,
        context.session.workspace_revision,
        context.goal,
        goal.candidate_id,
        goal.selected_candidate_id,
    };
    const RiskConfirmationContractValidationCode confirmation_validation =
        validate_owner_risk_confirmation_contract(
            confirmation, expected_publication, *goal.evidence);
    const RiskConfirmationPublicationIdentity& published = confirmation.publication_identity();
    if (confirmation_validation ==
            RiskConfirmationContractValidationCode::InvalidRecommendationEvidence ||
        confirmation_validation ==
            RiskConfirmationContractValidationCode::RecommendationEvidenceMismatch)
        return rejected<ReadyApplyBindingResult>(ApplyRejectionCode::RiskEvidenceChanged);
    if (confirmation_validation ==
            RiskConfirmationContractValidationCode::PublicationIdentityMismatch &&
        published.workflow_id != expected_publication.workflow_id)
        return rejected<ReadyApplyBindingResult>(ApplyRejectionCode::WorkflowChanged);
    if (confirmation_validation ==
            RiskConfirmationContractValidationCode::PublicationIdentityMismatch &&
        published.attempt_id != expected_publication.attempt_id)
        return rejected<ReadyApplyBindingResult>(ApplyRejectionCode::AttemptChanged);
    if (confirmation_validation ==
            RiskConfirmationContractValidationCode::PublicationIdentityMismatch &&
        published.workspace_revision != expected_publication.workspace_revision)
        return rejected<ReadyApplyBindingResult>(ApplyRejectionCode::WorkspaceChanged);
    if (confirmation_validation ==
            RiskConfirmationContractValidationCode::PublicationIdentityMismatch &&
        published.goal != expected_publication.goal)
        return rejected<ReadyApplyBindingResult>(ApplyRejectionCode::GoalChanged);
    if (confirmation_validation ==
            RiskConfirmationContractValidationCode::PublicationIdentityMismatch &&
        published.goal_task_candidate_id != expected_publication.goal_task_candidate_id)
        return rejected<ReadyApplyBindingResult>(ApplyRejectionCode::GoalTaskCandidateChanged);
    if (confirmation_validation ==
            RiskConfirmationContractValidationCode::PublicationIdentityMismatch &&
        published.selected_candidate_id != expected_publication.selected_candidate_id)
        return rejected<ReadyApplyBindingResult>(ApplyRejectionCode::SelectedCandidateChanged);
    if (confirmation_validation != RiskConfirmationContractValidationCode::None)
        return rejected<ReadyApplyBindingResult>(ApplyRejectionCode::RequiredConfirmationsChanged,
                                                 "risk_confirmation_contract_invalid");
    const std::string current_risk_digest = recommendation_risk_evidence_digest(*goal.evidence);
    if (confirmation.recommendation_evidence_digest() != current_risk_digest)
        return rejected<ReadyApplyBindingResult>(ApplyRejectionCode::RiskEvidenceChanged);

    std::vector<RiskConfirmationKind> confirmations;
    if (!canonicalize_confirmations(confirmation.required_confirmations(), confirmations))
        return rejected<ReadyApplyBindingResult>(ApplyRejectionCode::RequiredConfirmationsChanged);

    ReadyApplyBinding binding;
    binding.workflow_id = context.session.workflow_id;
    binding.attempt_id = context.session.attempt_id;
    binding.base_revision = context.session.workspace_revision;
    binding.goal = context.goal;
    binding.goal_task_candidate_id = goal.candidate_id;
    binding.selected_candidate_id = goal.selected_candidate_id;
    binding.strategy_version = context.session.strategy_version;
    binding.selection_policy_version = goal.evidence->selection_policy_version;
    binding.parameter_policy_version = context.selected_candidate.parameters.policy_version;
    binding.risk_evidence_digest = current_risk_digest;
    binding.risk_confirmation_policy_version = confirmation.policy_version();
    binding.risk_confirmation_evidence_digest = confirmation.source_evidence_digest();
    binding.required_confirmations = std::move(confirmations);
    binding.intent_evidence_revision = intent_constraints_revision(context.workspace.intent_constraints);
    binding.machine_registry_version = context.workspace.machine_capability.registry_version;
    binding.material_registry_version = context.workspace.material_compatibility.registry_version;
    binding.machine_capability_evidence_digest =
        apply_machine_capability_evidence_digest(context.workspace.machine_capability);
    binding.material_compatibility_evidence_digest =
        apply_material_compatibility_evidence_digest(context.workspace.material_compatibility);
    binding.candidate_payload_digest = apply_candidate_payload_digest(context.selected_candidate);

    ReadyApplyBindingResult result;
    result.binding = std::move(binding);
    return result;
}

uint64_t fnv1a64(const std::string& value)
{
    uint64_t hash = 14695981039346656037ull;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string confirmation_token_id(const ReadyApplyBinding& binding)
{
    std::ostringstream canonical;
    canonical.imbue(std::locale::classic());
    const auto text = [&](const std::string& value) { canonical << value.size() << ':' << value; };
    const auto integer = [&](uint64_t value) { canonical << value << ';'; };
    integer(binding.workflow_id);
    integer(binding.attempt_id);
    integer(binding.base_revision.model_revision);
    integer(binding.base_revision.config_revision);
    integer(binding.base_revision.plate_revision);
    text(binding.base_revision.fingerprint);
    text(recommendation_goal_id(binding.goal));
    text(binding.goal_task_candidate_id);
    text(binding.selected_candidate_id);
    text(binding.strategy_version);
    text(binding.selection_policy_version);
    text(binding.parameter_policy_version);
    text(binding.risk_evidence_digest);
    text(binding.risk_confirmation_policy_version);
    text(binding.risk_confirmation_evidence_digest);
    std::vector<RiskConfirmationKind> confirmations = binding.required_confirmations;
    std::sort(confirmations.begin(), confirmations.end());
    integer(confirmations.size());
    for (const RiskConfirmationKind confirmation : confirmations)
        text(risk_confirmation_kind_name(confirmation));
    integer(binding.intent_evidence_revision);
    text(binding.machine_registry_version);
    text(binding.machine_capability_evidence_digest);
    text(binding.material_registry_version);
    text(binding.material_compatibility_evidence_digest);
    text(binding.candidate_payload_digest);
    std::ostringstream result;
    result.imbue(std::locale::classic());
    result << "risk-confirmation-" << std::hex << std::setfill('0') << std::setw(16)
           << fnv1a64(canonical.str());
    return result.str();
}

ApplyRejectionCode validate_placement(const SliceCandidate& candidate,
                                      const IntentConstraintSnapshot& intents,
                                      std::string& detail)
{
    if (candidate.repair) {
        detail = "candidate_repair_not_supported";
        return ApplyRejectionCode::InvalidPlacement;
    }
    std::set<std::pair<uint64_t, uint64_t>> seen;
    for (const ObjectTransform& transform : candidate.placement.transforms) {
        const auto& matrix = transform.matrix;
        const double determinant =
            matrix[0] * (matrix[5] * matrix[10] - matrix[6] * matrix[9]) -
            matrix[1] * (matrix[4] * matrix[10] - matrix[6] * matrix[8]) +
            matrix[2] * (matrix[4] * matrix[9] - matrix[5] * matrix[8]);
        if (transform.object_id == 0 || transform.instance_id == 0 ||
            !seen.emplace(transform.object_id, transform.instance_id).second ||
            std::any_of(matrix.begin(), matrix.end(), [](double value) { return !std::isfinite(value); }) ||
            std::abs(determinant) <= 1e-12 || std::abs(matrix[12]) > 1e-12 ||
            std::abs(matrix[13]) > 1e-12 || std::abs(matrix[14]) > 1e-12 ||
            std::abs(matrix[15] - 1.0) > 1e-12) {
            detail = "candidate_transform_invalid";
            return ApplyRejectionCode::InvalidPlacement;
        }
        for (const IntentConstraintRecord& intent : intents.records) {
            if (!intent.is_hard_constraint())
                continue;
            if (intent.type == IntentConstraintType::PlatePlacementLock ||
                (intent.type == IntentConstraintType::ObjectPlacementLock &&
                 intent.object_id == transform.object_id) ||
                (intent.type == IntentConstraintType::InstancePlacementLock &&
                 intent.object_id == transform.object_id && intent.instance_id == transform.instance_id)) {
                detail = intent_constraint_type_name(intent.type);
                return ApplyRejectionCode::PlacementIntentConflict;
            }
        }
    }
    return ApplyRejectionCode::None;
}

} // namespace

ReadyApplyBindingResult ApplyService::capture_ready_binding(const ApplyExpectedContext& context) const
{
    return current_binding(context);
}

RiskConfirmationTokenResult ApplyService::issue_confirmation_token(
    const ReadyApplyBinding& binding, const ApplyExpectedContext& current,
    const RiskConfirmationSubmission& submission) const
{
    ReadyApplyBindingResult current_result = current_binding(current);
    if (!current_result.accepted())
        return rejected<RiskConfirmationTokenResult>(current_result.rejection,
            current_result.diagnostic_details.empty() ? std::string{} : current_result.diagnostic_details.front());
    const ApplyRejectionCode binding_error = compare_bindings(binding, *current_result.binding);
    if (binding_error != ApplyRejectionCode::None)
        return rejected<RiskConfirmationTokenResult>(binding_error);

    std::vector<RiskConfirmationKind> submitted;
    if (!canonicalize_confirmations(submission.confirmed_risks, submitted) ||
        submitted != binding.required_confirmations)
        return rejected<RiskConfirmationTokenResult>(ApplyRejectionCode::RiskConfirmationRequired);

    RiskConfirmationTokenResult result;
    result.token = RiskConfirmationToken(confirmation_token_id(binding), binding);
    return result;
}

ApplyGuardResult ApplyService::make_atomic_plan(const ApplyCommand& command,
                                                const ReadyApplyBinding& binding,
                                                const ApplyExpectedContext& current)
{
    if (command.schema != APPLY_COMMAND_SCHEMA || command.version != APPLY_COMMAND_VERSION)
        return rejected<ApplyGuardResult>(ApplyRejectionCode::InvalidContract);
    if (!valid_recommendation_code(command.command_id))
        return rejected<ApplyGuardResult>(ApplyRejectionCode::InvalidCommandId);
    if (!m_consumed_command_ids.emplace(command.command_id).second)
        return rejected<ApplyGuardResult>(ApplyRejectionCode::CommandAlreadyConsumed);

    const ApplyRejectionCode command_binding_error = compare_bindings(command.binding, binding);
    if (command_binding_error != ApplyRejectionCode::None)
        return rejected<ApplyGuardResult>(command_binding_error);

    ReadyApplyBindingResult current_result = current_binding(current);
    if (!current_result.accepted())
        return rejected<ApplyGuardResult>(current_result.rejection,
            current_result.diagnostic_details.empty() ? std::string{} : current_result.diagnostic_details.front());
    const ApplyRejectionCode current_binding_error = compare_bindings(binding, *current_result.binding);
    if (current_binding_error != ApplyRejectionCode::None)
        return rejected<ApplyGuardResult>(current_binding_error);

    if (!command.confirmation_token)
        return rejected<ApplyGuardResult>(ApplyRejectionCode::RiskConfirmationRequired);
    const RiskConfirmationToken& token = *command.confirmation_token;
    if (token.schema() != RISK_CONFIRMATION_TOKEN_SCHEMA || token.version() != RISK_CONFIRMATION_TOKEN_VERSION ||
        token.binding() != binding || token.token_id() != confirmation_token_id(binding))
        return rejected<ApplyGuardResult>(ApplyRejectionCode::RiskConfirmationTokenMismatch);

    if (current.parameter_validation.goal != current.goal)
        return rejected<ApplyGuardResult>(ApplyRejectionCode::GoalChanged);
    if (intent_constraints_revision(current.parameter_validation.intent_constraints) !=
        intent_constraints_revision(current.workspace.intent_constraints))
        return rejected<ApplyGuardResult>(ApplyRejectionCode::IntentConstraintChanged);

    if (!current.workspace.machine_capability.enabled())
        return rejected<ApplyGuardResult>(ApplyRejectionCode::MachineCapabilityUnavailable);
    if (!current.workspace.material_compatibility.compatible())
        return rejected<ApplyGuardResult>(ApplyRejectionCode::UnsupportedMaterialCombination);
    if (current.activity.source_version.empty())
        return rejected<ApplyGuardResult>(ApplyRejectionCode::InvalidContract, "activity_source_unavailable");
    if (current.activity.model_tool_active)
        return rejected<ApplyGuardResult>(ApplyRejectionCode::ActiveModelTool);
    if (current.activity.official_transaction_active)
        return rejected<ApplyGuardResult>(ApplyRejectionCode::ActiveOfficialTransaction);

    std::string placement_detail;
    const ApplyRejectionCode placement_error = validate_placement(
        current.selected_candidate, current.workspace.intent_constraints, placement_detail);
    if (placement_error != ApplyRejectionCode::None)
        return rejected<ApplyGuardResult>(placement_error, std::move(placement_detail));

    if (!current.selected_candidate.parameters.entries.empty()) {
        const ParameterValidationResult parameters = ParameterProposalValidator().revalidate_for_apply(
            current.selected_candidate.parameters, current.parameter_validation);
        if (!parameters.accepted())
            return rejected<ApplyGuardResult>(ApplyRejectionCode::ParameterRevalidationFailed,
                                              parameters.rejections.front().diagnostic_code.empty() ?
                                                  parameter_rejection_code_name(parameters.rejections.front().code) :
                                                  parameters.rejections.front().diagnostic_code);
    } else if (current.selected_candidate.placement.transforms.empty()) {
        return rejected<ApplyGuardResult>(ApplyRejectionCode::InvalidContract,
                                          "candidate_has_no_applicable_changes");
    }

    ApplyGuardResult result;
    AtomicApplyPlan plan;
    plan.plan_id = "apply-plan:" + command.command_id;
    plan.binding = binding;
    plan.candidate = current.selected_candidate;
    plan.confirmation_token_id = token.token_id();
    result.plan = std::move(plan);
    return result;
}

} // namespace Slic3r::AI::SmartSlicing
