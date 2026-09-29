#include "RiskConfirmationPolicy.hpp"

#include <algorithm>
#include <array>
#include <iomanip>
#include <locale>
#include <memory>
#include <sstream>
#include <string_view>
#include <utility>

namespace Slic3r::AI::SmartSlicing {
namespace {

RiskConfirmationMappingResult rejected(RiskConfirmationMappingCode code)
{
    return {nullptr, code, risk_confirmation_mapping_code_name(code)};
}

std::optional<size_t> kind_index(RiskConfirmationKind kind)
{
    switch (kind) {
    case RiskConfirmationKind::ProtectedRegionSupportContact: return 0;
    case RiskConfirmationKind::ProtectedRegionSeam: return 1;
    }
    return std::nullopt;
}

bool valid_source_version(const std::string& value)
{
    return !value.empty() && value.size() <= 128 &&
           std::all_of(value.begin(), value.end(), [](unsigned char character) {
               return (character >= 'a' && character <= 'z') ||
                      (character >= '0' && character <= '9') || character == '_' ||
                      character == '-' || character == '.' || character == '/';
           });
}

bool same_fact(const RiskConfirmationFact& lhs, const RiskConfirmationFact& rhs)
{
    return lhs.kind == rhs.kind && lhs.availability == rhs.availability &&
           lhs.unavoidable == rhs.unavoidable && lhs.evidence_code == rhs.evidence_code &&
           lhs.source_version == rhs.source_version;
}

bool valid_publication_identity(const RiskConfirmationPublicationIdentity& identity)
{
    return identity.workflow_id != 0 && identity.attempt_id != 0 &&
           identity.workspace_revision.valid() &&
           !std::string_view(recommendation_goal_id(identity.goal)).empty() &&
           !identity.goal_task_candidate_id.empty() && !identity.selected_candidate_id.empty();
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

std::string source_evidence_digest(const std::array<RiskConfirmationFact, 2>& facts)
{
    std::ostringstream canonical;
    canonical.imbue(std::locale::classic());
    const auto text = [&](const std::string& value) { canonical << value.size() << ':' << value; };
    for (const RiskConfirmationFact& fact : facts) {
        canonical << static_cast<int>(fact.kind) << ';' << static_cast<int>(fact.availability) << ';'
                  << (fact.unavoidable.has_value() ? 1 : 0) << ';';
        if (fact.unavoidable)
            canonical << (*fact.unavoidable ? 1 : 0) << ';';
        text(fact.evidence_code);
        text(fact.source_version);
    }
    std::ostringstream result;
    result.imbue(std::locale::classic());
    result << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16)
           << fnv1a64(canonical.str());
    return result.str();
}

} // namespace

bool operator==(const RiskConfirmationPublicationIdentity& lhs,
                const RiskConfirmationPublicationIdentity& rhs)
{
    return lhs.workflow_id == rhs.workflow_id && lhs.attempt_id == rhs.attempt_id &&
           lhs.workspace_revision == rhs.workspace_revision && lhs.goal == rhs.goal &&
           lhs.goal_task_candidate_id == rhs.goal_task_candidate_id &&
           lhs.selected_candidate_id == rhs.selected_candidate_id;
}

OwnerRiskConfirmationContract::OwnerRiskConfirmationContract(
    RiskConfirmationPublicationIdentity identity, std::string recommendation_evidence_digest,
    std::string source_evidence_digest, std::vector<RiskConfirmationKind> required_confirmations)
    : m_publication_identity(std::move(identity))
    , m_recommendation_evidence_digest(std::move(recommendation_evidence_digest))
    , m_source_evidence_digest(std::move(source_evidence_digest))
    , m_required_confirmations(std::move(required_confirmations))
{}

OwnerRiskConfirmationContractData OwnerRiskConfirmationContract::data() const
{
    return {m_schema,
            m_version,
            m_policy_version,
            m_publication_identity,
            m_recommendation_evidence_digest,
            m_source_evidence_digest,
            m_required_confirmations};
}

std::shared_ptr<const OwnerRiskConfirmationContract>
OwnerRiskConfirmationContract::from_untrusted_data(OwnerRiskConfirmationContractData data)
{
    auto contract = std::shared_ptr<OwnerRiskConfirmationContract>(
        new OwnerRiskConfirmationContract(
            std::move(data.publication_identity),
            std::move(data.recommendation_evidence_digest),
            std::move(data.source_evidence_digest),
            std::move(data.required_confirmations)));
    contract->m_schema = std::move(data.schema);
    contract->m_version = std::move(data.version);
    contract->m_policy_version = std::move(data.policy_version);
    return contract;
}

RiskConfirmationContractValidationCode validate_owner_risk_confirmation_contract(
    const OwnerRiskConfirmationContract& contract,
    const RiskConfirmationPublicationIdentity& expected_identity,
    const RecommendationEvidence& published_evidence)
{
    if (!validate_recommendation_evidence(published_evidence).empty())
        return RiskConfirmationContractValidationCode::InvalidRecommendationEvidence;
    if (contract.schema() != RISK_CONFIRMATION_CONTRACT_SCHEMA)
        return RiskConfirmationContractValidationCode::InvalidSchema;
    if (contract.version() != RISK_CONFIRMATION_CONTRACT_VERSION)
        return RiskConfirmationContractValidationCode::InvalidVersion;
    if (contract.policy_version() != RISK_CONFIRMATION_POLICY_VERSION)
        return RiskConfirmationContractValidationCode::InvalidPolicy;
    if (!valid_publication_identity(contract.publication_identity()) ||
        !valid_publication_identity(expected_identity))
        return RiskConfirmationContractValidationCode::InvalidPublicationIdentity;
    if (contract.publication_identity() != expected_identity)
        return RiskConfirmationContractValidationCode::PublicationIdentityMismatch;
    if (contract.recommendation_evidence_digest() !=
        recommendation_risk_evidence_digest(published_evidence))
        return RiskConfirmationContractValidationCode::RecommendationEvidenceMismatch;
    if (contract.source_evidence_digest().empty())
        return RiskConfirmationContractValidationCode::InvalidSourceEvidenceDigest;

    std::array<bool, 2> seen{};
    for (const RiskConfirmationKind confirmation : contract.required_confirmations()) {
        const std::optional<size_t> index = kind_index(confirmation);
        if (!index || seen[*index])
            return RiskConfirmationContractValidationCode::InvalidRequiredConfirmations;
        seen[*index] = true;
    }
    return RiskConfirmationContractValidationCode::None;
}

const char* risk_confirmation_mapping_code_name(RiskConfirmationMappingCode code)
{
    switch (code) {
    case RiskConfirmationMappingCode::None: return "none";
    case RiskConfirmationMappingCode::UnknownPolicy: return "risk_confirmation_policy_unknown";
    case RiskConfirmationMappingCode::InvalidPublicationIdentity:
        return "risk_confirmation_publication_identity_invalid";
    case RiskConfirmationMappingCode::InvalidRecommendationEvidence:
        return "risk_confirmation_recommendation_evidence_invalid";
    case RiskConfirmationMappingCode::MissingEvidence: return "risk_confirmation_evidence_missing";
    case RiskConfirmationMappingCode::UnknownEvidence: return "risk_confirmation_evidence_unknown";
    case RiskConfirmationMappingCode::UnavailableEvidence:
        return "risk_confirmation_evidence_unavailable";
    case RiskConfirmationMappingCode::InvalidEvidence: return "risk_confirmation_evidence_invalid";
    case RiskConfirmationMappingCode::ConflictingEvidence:
        return "risk_confirmation_evidence_conflict";
    }
    return "risk_confirmation_mapping_failed";
}

RiskConfirmationMappingResult RiskConfirmationMapper::map(const RiskConfirmationMappingInput& input) const
{
    if (input.policy_version != RISK_CONFIRMATION_POLICY_VERSION)
        return rejected(RiskConfirmationMappingCode::UnknownPolicy);
    if (!valid_publication_identity(input.publication_identity))
        return rejected(RiskConfirmationMappingCode::InvalidPublicationIdentity);
    if (!validate_recommendation_evidence(input.published_evidence).empty())
        return rejected(RiskConfirmationMappingCode::InvalidRecommendationEvidence);

    std::array<std::optional<RiskConfirmationFact>, 2> collected;
    for (const RiskConfirmationFact& fact : input.facts) {
        const std::optional<size_t> index = kind_index(fact.kind);
        if (!index)
            return rejected(RiskConfirmationMappingCode::InvalidEvidence);
        if (collected[*index])
            return rejected(same_fact(*collected[*index], fact) ?
                                RiskConfirmationMappingCode::InvalidEvidence :
                                RiskConfirmationMappingCode::ConflictingEvidence);
        collected[*index] = fact;
    }
    if (!collected[0] || !collected[1])
        return rejected(RiskConfirmationMappingCode::MissingEvidence);

    std::array<RiskConfirmationFact, 2> facts{*collected[0], *collected[1]};
    std::vector<RiskConfirmationKind> required;
    for (const RiskConfirmationFact& fact : facts) {
        switch (fact.availability) {
        case RiskConfirmationFactAvailability::Unknown:
            return rejected(RiskConfirmationMappingCode::UnknownEvidence);
        case RiskConfirmationFactAvailability::Unavailable:
            return rejected(RiskConfirmationMappingCode::UnavailableEvidence);
        case RiskConfirmationFactAvailability::Known: break;
        default: return rejected(RiskConfirmationMappingCode::InvalidEvidence);
        }
        if (!fact.unavoidable || !valid_recommendation_code(fact.evidence_code) ||
            !valid_source_version(fact.source_version))
            return rejected(RiskConfirmationMappingCode::InvalidEvidence);
        if (*fact.unavoidable)
            required.push_back(fact.kind);
    }

    RiskConfirmationMappingResult result;
    result.contract = std::shared_ptr<const OwnerRiskConfirmationContract>(
        new OwnerRiskConfirmationContract(
            input.publication_identity,
            recommendation_risk_evidence_digest(input.published_evidence),
            source_evidence_digest(facts), std::move(required)));
    return result;
}

} // namespace Slic3r::AI::SmartSlicing
