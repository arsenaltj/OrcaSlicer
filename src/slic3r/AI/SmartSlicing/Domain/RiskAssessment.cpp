#include "RiskAssessment.hpp"

#include <algorithm>
#include <cmath>
#include <tuple>

namespace Slic3r::AI::SmartSlicing {
namespace {

void normalize_component(RiskComponent& component, const char* name, std::vector<std::string>& codes)
{
    std::sort(component.evidence.begin(), component.evidence.end(), [](const auto& lhs, const auto& rhs) {
        return std::tie(lhs.code, lhs.raw_value) < std::tie(rhs.code, rhs.raw_value);
    });
    component.evidence.erase(
        std::unique(component.evidence.begin(), component.evidence.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.code == rhs.code && lhs.raw_value == rhs.raw_value;
        }),
        component.evidence.end());

    if (component.availability != MetricAvailability::Unknown && component.evidence.empty())
        codes.emplace_back(std::string("risk_missing_evidence:") + name);

    if (component.availability == MetricAvailability::Known) {
        if (component.evidence.empty())
            codes.emplace_back(std::string("known_risk_missing_evidence:") + name);
        if (!component.normalized_risk || !std::isfinite(*component.normalized_risk) ||
            *component.normalized_risk < 0.0 || *component.normalized_risk > 1.0)
            codes.emplace_back(std::string("known_risk_invalid_normalized_value:") + name);
    } else if (component.normalized_risk || component.relative_to_baseline_change ||
               component.retained_strength_ratio || component.hard_gate_acceptable) {
        codes.emplace_back(std::string("non_known_risk_has_value:") + name);
    }

    if (component.relative_to_baseline_change && !std::isfinite(*component.relative_to_baseline_change))
        codes.emplace_back(std::string("risk_invalid_relative_change:") + name);
    if (component.retained_strength_ratio &&
        (!std::isfinite(*component.retained_strength_ratio) || *component.retained_strength_ratio < 0.0))
        codes.emplace_back(std::string("risk_invalid_strength_ratio:") + name);
    for (const RiskEvidence& evidence : component.evidence) {
        if (evidence.code.empty())
            codes.emplace_back(std::string("risk_evidence_missing_code:") + name);
        if (evidence.raw_value.empty())
            codes.emplace_back(std::string("risk_evidence_missing_raw_value:") + name);
    }
}

} // namespace

RiskAssessmentNormalizationResult normalize_risk_assessment(const RiskAssessment& input)
{
    RiskAssessmentNormalizationResult result;
    result.assessment = input;
    if (result.assessment.schema != RISK_ASSESSMENT_SCHEMA)
        result.validation_codes.emplace_back("unsupported_risk_assessment_schema");
    if (result.assessment.version != RISK_ASSESSMENT_VERSION)
        result.validation_codes.emplace_back("unsupported_risk_assessment_version");
    if (result.assessment.policy_version != RISK_ASSESSMENT_POLICY_VERSION)
        result.validation_codes.emplace_back("unsupported_risk_assessment_policy_version");

    normalize_component(result.assessment.appearance, "appearance", result.validation_codes);
    normalize_component(result.assessment.dimensional, "dimensional", result.validation_codes);
    normalize_component(result.assessment.strength, "strength", result.validation_codes);
    normalize_component(result.assessment.reliability, "reliability", result.validation_codes);
    normalize_component(result.assessment.protected_region, "protected_region", result.validation_codes);

    std::sort(result.validation_codes.begin(), result.validation_codes.end());
    result.validation_codes.erase(std::unique(result.validation_codes.begin(), result.validation_codes.end()),
                                  result.validation_codes.end());
    return result;
}

} // namespace Slic3r::AI::SmartSlicing
