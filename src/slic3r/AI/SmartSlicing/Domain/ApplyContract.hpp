#pragma once

#include "MachineCapabilitySnapshot.hpp"
#include "MaterialCompatibility.hpp"
#include "RecommendationTypes.hpp"
#include "SliceCandidate.hpp"
#include "WorkspaceRevision.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

inline constexpr const char* APPLY_COMMAND_SCHEMA = "orcaslicer.smart-slicing.apply-command";
inline constexpr const char* APPLY_COMMAND_VERSION = "v1";
inline constexpr const char* APPLY_PLAN_SCHEMA = "orcaslicer.smart-slicing.atomic-apply-plan";
inline constexpr const char* APPLY_PLAN_VERSION = "v1";
inline constexpr const char* RISK_CONFIRMATION_TOKEN_SCHEMA =
    "orcaslicer.smart-slicing.risk-confirmation-token";
inline constexpr const char* RISK_CONFIRMATION_TOKEN_VERSION = "v1";

enum class RiskConfirmationKind { ProtectedRegionSupportContact, ProtectedRegionSeam };

const char* risk_confirmation_kind_name(RiskConfirmationKind kind);

struct ReadyApplyBinding
{
    WorkflowId workflow_id{0};
    AttemptId attempt_id{0};
    WorkspaceRevision base_revision;
    RecommendationGoal goal{RecommendationGoal::Balanced};
    CandidateId goal_task_candidate_id;
    CandidateId selected_candidate_id;
    std::string strategy_version;
    std::string selection_policy_version;
    std::string parameter_policy_version;
    std::string risk_evidence_digest;
    std::string risk_confirmation_policy_version;
    std::string risk_confirmation_evidence_digest;
    std::vector<RiskConfirmationKind> required_confirmations;
    uint64_t intent_evidence_revision{0};
    std::string machine_registry_version;
    std::string material_registry_version;
    std::string machine_capability_evidence_digest;
    std::string material_compatibility_evidence_digest;
    std::string candidate_payload_digest;
};

bool operator==(const ReadyApplyBinding& lhs, const ReadyApplyBinding& rhs);
inline bool operator!=(const ReadyApplyBinding& lhs, const ReadyApplyBinding& rhs) { return !(lhs == rhs); }

class ApplyService;

class RiskConfirmationToken
{
public:
    const std::string& token_id() const { return m_token_id; }
    const ReadyApplyBinding& binding() const { return m_binding; }
    const std::string& schema() const { return m_schema; }
    const std::string& version() const { return m_version; }

private:
    friend class ApplyService;
    RiskConfirmationToken(std::string token_id, ReadyApplyBinding binding);

    std::string m_schema{RISK_CONFIRMATION_TOKEN_SCHEMA};
    std::string m_version{RISK_CONFIRMATION_TOKEN_VERSION};
    std::string m_token_id;
    ReadyApplyBinding m_binding;
};

struct ApplyCommand
{
    std::string schema{APPLY_COMMAND_SCHEMA};
    std::string version{APPLY_COMMAND_VERSION};
    std::string command_id;
    ReadyApplyBinding binding;
    std::optional<RiskConfirmationToken> confirmation_token;
};

struct AtomicApplyPlan
{
    std::string schema{APPLY_PLAN_SCHEMA};
    std::string version{APPLY_PLAN_VERSION};
    std::string plan_id;
    ReadyApplyBinding binding;
    SliceCandidate candidate;
    std::string confirmation_token_id;
};

enum class ApplyRejectionCode {
    None,
    InvalidContract,
    InvalidCommandId,
    CommandAlreadyConsumed,
    WorkflowChanged,
    AttemptChanged,
    WorkspaceChanged,
    GoalChanged,
    GoalNotReady,
    CandidateNotReady,
    GoalTaskCandidateChanged,
    SelectedCandidateChanged,
    StrategyVersionChanged,
    SelectionPolicyVersionChanged,
    ParameterPolicyVersionChanged,
    RiskEvidenceChanged,
    RequiredConfirmationsChanged,
    RiskConfirmationRequired,
    RiskConfirmationTokenMismatch,
    IntentConstraintChanged,
    MachineCapabilityChanged,
    MaterialCompatibilityChanged,
    MachineCapabilityUnavailable,
    UnsupportedMaterialCombination,
    ActiveModelTool,
    ActiveOfficialTransaction,
    InvalidPlacement,
    PlacementIntentConflict,
    ParameterRevalidationFailed,
};

const char* apply_rejection_code_name(ApplyRejectionCode code);

std::string recommendation_risk_evidence_digest(const RecommendationEvidence& evidence);
std::string apply_candidate_payload_digest(const SliceCandidate& candidate);
std::string apply_machine_capability_evidence_digest(const MachineCapabilitySnapshot& machine);
std::string apply_material_compatibility_evidence_digest(const MaterialCompatibilitySnapshot& materials);
std::string apply_capability_evidence_digest(const MachineCapabilitySnapshot& machine,
                                             const MaterialCompatibilitySnapshot& materials);

} // namespace Slic3r::AI::SmartSlicing
