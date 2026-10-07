#pragma once

#include "ApplyContract.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

inline constexpr const char* RISK_CONFIRMATION_CONTRACT_SCHEMA =
    "orcaslicer.smart-slicing.owner-risk-confirmation";
inline constexpr const char* RISK_CONFIRMATION_CONTRACT_VERSION = "v1";
inline constexpr const char* RISK_CONFIRMATION_POLICY_VERSION = "risk-confirmation-policy/v1";

enum class RiskConfirmationFactAvailability { Known, Unknown, Unavailable };

struct RiskConfirmationFact
{
    RiskConfirmationKind kind{RiskConfirmationKind::ProtectedRegionSupportContact};
    RiskConfirmationFactAvailability availability{RiskConfirmationFactAvailability::Unknown};
    std::optional<bool> unavoidable;
    std::string evidence_code;
    std::string source_version;
};

struct RiskConfirmationPublicationIdentity
{
    WorkflowId workflow_id{0};
    AttemptId attempt_id{0};
    WorkspaceRevision workspace_revision;
    RecommendationGoal goal{RecommendationGoal::Balanced};
    CandidateId goal_task_candidate_id;
    CandidateId selected_candidate_id;
};

bool operator==(const RiskConfirmationPublicationIdentity& lhs,
                const RiskConfirmationPublicationIdentity& rhs);
inline bool operator!=(const RiskConfirmationPublicationIdentity& lhs,
                       const RiskConfirmationPublicationIdentity& rhs)
{
    return !(lhs == rhs);
}

class RiskConfirmationMapper;

struct OwnerRiskConfirmationContractData
{
    std::string schema;
    std::string version;
    std::string policy_version;
    RiskConfirmationPublicationIdentity publication_identity;
    std::string recommendation_evidence_digest;
    std::string source_evidence_digest;
    std::vector<RiskConfirmationKind> required_confirmations;
};

class OwnerRiskConfirmationContract
{
public:
    const std::string& schema() const { return m_schema; }
    const std::string& version() const { return m_version; }
    const std::string& policy_version() const { return m_policy_version; }
    const RiskConfirmationPublicationIdentity& publication_identity() const { return m_publication_identity; }
    const std::string& recommendation_evidence_digest() const { return m_recommendation_evidence_digest; }
    const std::string& source_evidence_digest() const { return m_source_evidence_digest; }
    const std::vector<RiskConfirmationKind>& required_confirmations() const { return m_required_confirmations; }
    OwnerRiskConfirmationContractData data() const;

    // Reconstituted data is untrusted until validate_owner_risk_confirmation_contract succeeds.
    static std::shared_ptr<const OwnerRiskConfirmationContract> from_untrusted_data(
        OwnerRiskConfirmationContractData data);

private:
    friend class RiskConfirmationMapper;
    OwnerRiskConfirmationContract(RiskConfirmationPublicationIdentity identity,
                                  std::string recommendation_evidence_digest,
                                  std::string source_evidence_digest,
                                  std::vector<RiskConfirmationKind> required_confirmations);

    std::string m_schema{RISK_CONFIRMATION_CONTRACT_SCHEMA};
    std::string m_version{RISK_CONFIRMATION_CONTRACT_VERSION};
    std::string m_policy_version{RISK_CONFIRMATION_POLICY_VERSION};
    RiskConfirmationPublicationIdentity m_publication_identity;
    std::string m_recommendation_evidence_digest;
    std::string m_source_evidence_digest;
    std::vector<RiskConfirmationKind> m_required_confirmations;
};

enum class RiskConfirmationMappingCode {
    None,
    UnknownPolicy,
    InvalidPublicationIdentity,
    InvalidRecommendationEvidence,
    MissingEvidence,
    UnknownEvidence,
    UnavailableEvidence,
    InvalidEvidence,
    ConflictingEvidence,
};

enum class RiskConfirmationContractValidationCode {
    None,
    InvalidRecommendationEvidence,
    InvalidSchema,
    InvalidVersion,
    InvalidPolicy,
    InvalidPublicationIdentity,
    PublicationIdentityMismatch,
    RecommendationEvidenceMismatch,
    InvalidSourceEvidenceDigest,
    InvalidRequiredConfirmations,
};

RiskConfirmationContractValidationCode validate_owner_risk_confirmation_contract(
    const OwnerRiskConfirmationContract& contract,
    const RiskConfirmationPublicationIdentity& expected_identity,
    const RecommendationEvidence& published_evidence);

const char* risk_confirmation_mapping_code_name(RiskConfirmationMappingCode code);

struct RiskConfirmationMappingInput
{
    std::string policy_version{RISK_CONFIRMATION_POLICY_VERSION};
    RiskConfirmationPublicationIdentity publication_identity;
    RecommendationEvidence published_evidence;
    std::vector<RiskConfirmationFact> facts;
};

struct RiskConfirmationMappingResult
{
    std::shared_ptr<const OwnerRiskConfirmationContract> contract;
    RiskConfirmationMappingCode code{RiskConfirmationMappingCode::None};
    std::string diagnostic_code;

    bool accepted() const { return contract != nullptr; }
};

class RiskConfirmationMapper
{
public:
    RiskConfirmationMappingResult map(const RiskConfirmationMappingInput& input) const;
};

} // namespace Slic3r::AI::SmartSlicing
