#pragma once

#include "TrialMetrics.hpp"

#include <optional>
#include <string>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

inline constexpr const char* RISK_ASSESSMENT_SCHEMA = "orcaslicer.smart-slicing.risk-assessment";
inline constexpr const char* RISK_ASSESSMENT_VERSION = "v1";
inline constexpr const char* RISK_ASSESSMENT_POLICY_VERSION = "risk-assessment-policy/v1";

struct RiskEvidence
{
    std::string code;
    std::string raw_value;
};

struct RiskComponent
{
    MetricAvailability availability{MetricAvailability::Unknown};
    std::vector<RiskEvidence> evidence;
    std::optional<double> normalized_risk;
    std::optional<double> relative_to_baseline_change;
    std::optional<double> retained_strength_ratio;
    std::optional<bool> hard_gate_acceptable;
};

struct RiskAssessment
{
    std::string schema{RISK_ASSESSMENT_SCHEMA};
    std::string version{RISK_ASSESSMENT_VERSION};
    std::string policy_version{RISK_ASSESSMENT_POLICY_VERSION};
    RiskComponent appearance;
    RiskComponent dimensional;
    RiskComponent strength;
    RiskComponent reliability;
    RiskComponent protected_region;
};

struct RiskAssessmentNormalizationResult
{
    RiskAssessment assessment;
    std::vector<std::string> validation_codes;

    bool valid() const { return validation_codes.empty(); }
};

RiskAssessmentNormalizationResult normalize_risk_assessment(const RiskAssessment& assessment);

} // namespace Slic3r::AI::SmartSlicing
