#include "CandidateSelection.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace Slic3r::AI::SmartSlicing {
namespace {

struct SelectionCandidate
{
    const EvaluatedCandidate* source{nullptr};
    TrialMetrics metrics;
    RiskAssessment risks;
};

bool same_binding(const CandidateSelectionBinding& lhs, const CandidateSelectionBinding& rhs)
{
    return lhs.workflow_id == rhs.workflow_id && lhs.attempt_id == rhs.attempt_id &&
           lhs.workspace_revision == rhs.workspace_revision &&
           lhs.baseline_candidate_id == rhs.baseline_candidate_id &&
           lhs.goal_task_candidate_id == rhs.goal_task_candidate_id &&
           lhs.strategy_version == rhs.strategy_version &&
           lhs.goal_contract_version == rhs.goal_contract_version &&
           lhs.evaluation_policy_version == rhs.evaluation_policy_version &&
           lhs.selection_policy_version == rhs.selection_policy_version;
}

bool valid_binding(const CandidateSelectionBinding& binding)
{
    return binding.workflow_id != 0 && binding.attempt_id != 0 &&
           binding.workspace_revision.valid() && !binding.baseline_candidate_id.empty() &&
           !binding.goal_task_candidate_id.empty() && !binding.strategy_version.empty() &&
           binding.goal_contract_version == GOAL_CONTRACT_VERSION &&
           binding.evaluation_policy_version == CANDIDATE_EVALUATION_POLICY_VERSION &&
           binding.selection_policy_version == CANDIDATE_SELECTION_POLICY_VERSION;
}

bool known_positive(const MetricValue<double>& value)
{
    return value.known() && std::isfinite(*value.value) &&
           *value.value > CANDIDATE_RATIO_DENOMINATOR_MINIMUM;
}

bool known_risk(const RiskComponent& risk)
{
    return risk.availability == MetricAvailability::Known && risk.normalized_risk &&
           std::isfinite(*risk.normalized_risk);
}

bool required_selection_evidence(const TrialMetrics& metrics, const RiskAssessment& risks)
{
    return known_positive(metrics.estimated_time_seconds) &&
           known_positive(metrics.total_material_volume_mm3) && known_risk(risks.appearance) &&
           risks.dimensional.availability != MetricAvailability::Unknown && known_risk(risks.strength) &&
           risks.strength.retained_strength_ratio &&
           std::isfinite(*risks.strength.retained_strength_ratio) && known_risk(risks.reliability) &&
           risks.protected_region.availability != MetricAvailability::Unknown;
}

bool comparable_optional_risks(const RiskAssessment& baseline, const RiskAssessment& candidate)
{
    return baseline.dimensional.availability == candidate.dimensional.availability &&
           baseline.protected_region.availability == candidate.protected_region.availability;
}

struct Dimension
{
    MetricAvailability availability{MetricAvailability::Unknown};
    double value{0.0};
};

Dimension risk_dimension(const RiskComponent& risk)
{
    return {risk.availability, risk.normalized_risk.value_or(0.0)};
}

std::vector<Dimension> pareto_dimensions(const SelectionCandidate& candidate)
{
    return {
        risk_dimension(candidate.risks.appearance),
        risk_dimension(candidate.risks.dimensional),
        risk_dimension(candidate.risks.strength),
        risk_dimension(candidate.risks.reliability),
        risk_dimension(candidate.risks.protected_region),
        {MetricAvailability::Known, *candidate.metrics.estimated_time_seconds.value},
        {MetricAvailability::Known, *candidate.metrics.total_material_volume_mm3.value},
    };
}

bool dominates(const SelectionCandidate& lhs, const SelectionCandidate& rhs)
{
    const std::vector<Dimension> lhs_dimensions = pareto_dimensions(lhs);
    const std::vector<Dimension> rhs_dimensions = pareto_dimensions(rhs);
    bool strictly_better = false;
    for (size_t index = 0; index < lhs_dimensions.size(); ++index) {
        const Dimension& left = lhs_dimensions[index];
        const Dimension& right = rhs_dimensions[index];
        if (left.availability != right.availability)
            return false;
        if (left.availability == MetricAvailability::Unknown)
            return false;
        if (left.availability == MetricAvailability::NotApplicable)
            continue;
        if (left.value > right.value + CANDIDATE_SELECTION_EPSILON)
            return false;
        strictly_better = strictly_better ||
                          left.value + CANDIDATE_SELECTION_EPSILON < right.value;
    }
    return strictly_better;
}

std::optional<double> comparable_improvement(const RiskComponent& baseline,
                                             const RiskComponent& candidate)
{
    if (baseline.availability != candidate.availability)
        return std::nullopt;
    if (baseline.availability == MetricAvailability::NotApplicable)
        return std::nullopt;
    if (!known_risk(baseline) || !known_risk(candidate))
        return std::nullopt;
    return *baseline.normalized_risk - *candidate.normalized_risk;
}

double quality_benefit(const RiskAssessment& baseline, const RiskAssessment& candidate)
{
    std::vector<double> improvements;
    for (const auto& pair : {std::pair<const RiskComponent*, const RiskComponent*>{&baseline.appearance,
                                                                                  &candidate.appearance},
                             {&baseline.dimensional, &candidate.dimensional},
                             {&baseline.strength, &candidate.strength}}) {
        const std::optional<double> improvement = comparable_improvement(*pair.first, *pair.second);
        if (improvement)
            improvements.push_back(*improvement);
    }
    if (improvements.empty())
        return 0.0;
    double total = 0.0;
    for (double improvement : improvements)
        total += improvement;
    return total / static_cast<double>(improvements.size());
}

double ratio(const MetricValue<double>& candidate, const MetricValue<double>& baseline)
{
    return *candidate.value / *baseline.value;
}

double balanced_score(const SelectionCandidate& candidate,
                      const TrialMetrics& baseline_metrics,
                      const RiskAssessment& baseline_risks)
{
    const double quality = quality_benefit(baseline_risks, candidate.risks);
    const double reliability = comparable_improvement(baseline_risks.reliability,
                                                       candidate.risks.reliability).value_or(0.0);
    const double time = 1.0 - ratio(candidate.metrics.estimated_time_seconds,
                                    baseline_metrics.estimated_time_seconds);
    const double material = 1.0 - ratio(candidate.metrics.total_material_volume_mm3,
                                        baseline_metrics.total_material_volume_mm3);
    return 0.45 * quality + 0.25 * reliability + 0.20 * time + 0.10 * material;
}

template<class Value>
void retain_near_minimum(std::vector<const SelectionCandidate*>& candidates, Value value)
{
    if (candidates.size() < 2)
        return;
    const auto minimum = *std::min_element(candidates.begin(), candidates.end(),
                                           [&](const auto* lhs, const auto* rhs) {
                                               return value(*lhs) < value(*rhs);
                                           });
    const double minimum_value = value(*minimum);
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&](const auto* candidate) {
                         return value(*candidate) > minimum_value + CANDIDATE_SELECTION_EPSILON;
                     }),
                     candidates.end());
}

template<class Value>
void retain_near_maximum(std::vector<const SelectionCandidate*>& candidates, Value value)
{
    if (candidates.size() < 2)
        return;
    const auto maximum = *std::max_element(candidates.begin(), candidates.end(),
                                           [&](const auto* lhs, const auto* rhs) {
                                               return value(*lhs) < value(*rhs);
                                           });
    const double maximum_value = value(*maximum);
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&](const auto* candidate) {
                         return value(*candidate) + CANDIDATE_SELECTION_EPSILON < maximum_value;
                     }),
                     candidates.end());
}

template<class Risk>
void retain_near_minimum_risk(std::vector<const SelectionCandidate*>& candidates, Risk risk)
{
    if (candidates.empty() || risk(*candidates.front()).availability != MetricAvailability::Known)
        return;
    retain_near_minimum(candidates, [&](const SelectionCandidate& candidate) {
        return *risk(candidate).normalized_risk;
    });
}

const SelectionCandidate* select_lexicographic(std::vector<const SelectionCandidate*> candidates,
                                               RecommendationGoal goal)
{
    if (goal == RecommendationGoal::Speed) {
        retain_near_minimum(candidates, [](const SelectionCandidate& candidate) {
            return *candidate.metrics.estimated_time_seconds.value;
        });
        retain_near_minimum_risk(candidates, [](const SelectionCandidate& candidate) -> const RiskComponent& {
            return candidate.risks.reliability;
        });
        retain_near_minimum_risk(candidates, [](const SelectionCandidate& candidate) -> const RiskComponent& {
            return candidate.risks.protected_region;
        });
        retain_near_minimum(candidates, [](const SelectionCandidate& candidate) {
            return *candidate.metrics.total_material_volume_mm3.value;
        });
        retain_near_minimum_risk(candidates, [](const SelectionCandidate& candidate) -> const RiskComponent& {
            return candidate.risks.appearance;
        });
        retain_near_minimum_risk(candidates, [](const SelectionCandidate& candidate) -> const RiskComponent& {
            return candidate.risks.dimensional;
        });
        retain_near_minimum_risk(candidates, [](const SelectionCandidate& candidate) -> const RiskComponent& {
            return candidate.risks.strength;
        });
    } else {
        retain_near_minimum_risk(candidates, [](const SelectionCandidate& candidate) -> const RiskComponent& {
            return candidate.risks.appearance;
        });
        retain_near_minimum_risk(candidates, [](const SelectionCandidate& candidate) -> const RiskComponent& {
            return candidate.risks.dimensional;
        });
        retain_near_minimum_risk(candidates, [](const SelectionCandidate& candidate) -> const RiskComponent& {
            return candidate.risks.strength;
        });
        retain_near_minimum_risk(candidates, [](const SelectionCandidate& candidate) -> const RiskComponent& {
            return candidate.risks.protected_region;
        });
        retain_near_minimum_risk(candidates, [](const SelectionCandidate& candidate) -> const RiskComponent& {
            return candidate.risks.reliability;
        });
        retain_near_minimum(candidates, [](const SelectionCandidate& candidate) {
            return *candidate.metrics.estimated_time_seconds.value;
        });
        retain_near_minimum(candidates, [](const SelectionCandidate& candidate) {
            return *candidate.metrics.total_material_volume_mm3.value;
        });
    }
    retain_near_maximum(candidates, [](const SelectionCandidate& candidate) {
        return *candidate.risks.strength.retained_strength_ratio;
    });
    return *std::min_element(candidates.begin(), candidates.end(), [](const auto* lhs, const auto* rhs) {
        return lhs->source->candidate_id < rhs->source->candidate_id;
    });
}

EvidenceValue<double> available(double value, std::string source)
{
    EvidenceValue<double> result;
    result.availability = EvidenceAvailability::Available;
    result.value = value;
    result.source_code = std::move(source);
    return result;
}

EvidenceValue<double> risk_evidence(const RiskComponent& risk, const char* source)
{
    if (risk.availability == MetricAvailability::Known)
        return available(*risk.normalized_risk, source);
    EvidenceValue<double> result;
    result.availability = risk.availability == MetricAvailability::NotApplicable ?
                              EvidenceAvailability::NotApplicable : EvidenceAvailability::Unavailable;
    result.source_code = risk.availability == MetricAvailability::NotApplicable ? "not_applicable" : "";
    return result;
}

RecommendationEvidence published_evidence(const SelectionCandidate& selected,
                                           const TrialMetrics& baseline_metrics,
                                           const RiskAssessment& baseline_risks)
{
    RecommendationEvidence evidence;
    evidence.selection_policy_version = CANDIDATE_SELECTION_POLICY_VERSION;
    evidence.estimated_time_ratio = available(
        ratio(selected.metrics.estimated_time_seconds, baseline_metrics.estimated_time_seconds),
        "orca_estimated_time_ratio");
    evidence.material_ratio = available(
        ratio(selected.metrics.total_material_volume_mm3, baseline_metrics.total_material_volume_mm3),
        "orca_total_material_ratio");
    evidence.appearance_risk = risk_evidence(selected.risks.appearance, "appearance_risk_assessment");
    evidence.dimensional_risk = risk_evidence(selected.risks.dimensional, "dimensional_risk_assessment");
    evidence.strength_risk = risk_evidence(selected.risks.strength, "strength_risk_assessment");
    evidence.retained_strength_ratio = available(*selected.risks.strength.retained_strength_ratio,
                                                 "strength_retention_assessment");
    evidence.reliability_risk = risk_evidence(selected.risks.reliability, "reliability_risk_assessment");
    evidence.protected_region_risk = risk_evidence(selected.risks.protected_region,
                                                   "protected_region_risk_assessment");

    const double reliability = comparable_improvement(baseline_risks.reliability,
                                                       selected.risks.reliability).value_or(0.0);
    const double time = 1.0 - *evidence.estimated_time_ratio.value;
    const double material = 1.0 - *evidence.material_ratio.value;
    const auto append_risk_improvement = [&](const RiskComponent& baseline,
                                             const RiskComponent& candidate,
                                             const char* code) {
        const std::optional<double> improvement = comparable_improvement(baseline, candidate);
        if (improvement && *improvement > CANDIDATE_SELECTION_EPSILON)
            evidence.explanation_codes.emplace_back(code);
    };
    append_risk_improvement(baseline_risks.appearance, selected.risks.appearance,
                            "appearance_risk_reduced");
    append_risk_improvement(baseline_risks.dimensional, selected.risks.dimensional,
                            "dimensional_risk_reduced");
    append_risk_improvement(baseline_risks.strength, selected.risks.strength,
                            "strength_risk_reduced");
    if (reliability > CANDIDATE_SELECTION_EPSILON)
        evidence.explanation_codes.emplace_back("reliability_risk_reduced");
    if (time > CANDIDATE_SELECTION_EPSILON)
        evidence.explanation_codes.emplace_back("estimated_time_reduced");
    if (material > CANDIDATE_SELECTION_EPSILON)
        evidence.explanation_codes.emplace_back("total_material_reduced");
    return evidence;
}

} // namespace

CandidateSelectionResult select_candidate(const CandidateSelectionInput& input)
{
    CandidateSelectionResult result;
    result.binding = input.binding;
    result.goal = input.goal;
    if (!valid_binding(input.binding)) {
        result.status = CandidateSelectionStatus::InvalidInput;
        result.diagnostic_codes.emplace_back("invalid_selection_input");
        return result;
    }

    const TrialMetricsNormalizationResult baseline_metrics = normalize_trial_metrics(input.baseline_metrics);
    const RiskAssessmentNormalizationResult baseline_risks = normalize_risk_assessment(input.baseline_risks);
    if (!baseline_metrics.valid() || !baseline_risks.valid() ||
        !required_selection_evidence(baseline_metrics.metrics, baseline_risks.assessment)) {
        result.status = CandidateSelectionStatus::InvalidInput;
        result.diagnostic_codes.emplace_back("invalid_baseline_selection_evidence");
        return result;
    }

    std::set<CandidateId> candidate_ids;
    for (const EvaluatedCandidate& candidate : input.candidates) {
        if (!same_binding(input.binding, candidate.binding) ||
            candidate.evaluation_input.goal != input.goal ||
            candidate.evaluation_input.policy_version != input.binding.evaluation_policy_version ||
            candidate.candidate_id.empty() || !candidate_ids.insert(candidate.candidate_id).second) {
            result.status = CandidateSelectionStatus::InvalidInput;
            result.diagnostic_codes.emplace_back("invalid_selection_input");
            return result;
        }
    }

    std::vector<SelectionCandidate> eligible;
    for (const EvaluatedCandidate& candidate : input.candidates) {
        if (!candidate.evaluation.accepted() ||
            candidate.evaluation.policy_version != input.binding.evaluation_policy_version) {
            result.excluded_candidate_ids.push_back(candidate.candidate_id);
            continue;
        }
        CandidateEvaluationInput bound_evaluation = candidate.evaluation_input;
        bound_evaluation.baseline_metrics = baseline_metrics.metrics;
        bound_evaluation.baseline_risks = baseline_risks.assessment;
        const CandidateEvaluationResult verified_evaluation = evaluate_candidate(bound_evaluation);
        const TrialMetricsNormalizationResult metrics = normalize_trial_metrics(bound_evaluation.candidate_metrics);
        const RiskAssessmentNormalizationResult risks = normalize_risk_assessment(bound_evaluation.candidate_risks);
        if (!metrics.valid() || !risks.valid() ||
            !verified_evaluation.accepted() ||
            !required_selection_evidence(metrics.metrics, risks.assessment) ||
            !comparable_optional_risks(baseline_risks.assessment, risks.assessment)) {
            result.excluded_candidate_ids.push_back(candidate.candidate_id);
            continue;
        }
        eligible.push_back({&candidate, metrics.metrics, risks.assessment});
    }
    std::sort(result.excluded_candidate_ids.begin(), result.excluded_candidate_ids.end());

    for (const SelectionCandidate& candidate : eligible) {
        const bool dominated = std::any_of(eligible.begin(), eligible.end(), [&](const SelectionCandidate& other) {
            return other.source != candidate.source && dominates(other, candidate);
        });
        if (!dominated)
            result.pareto_candidate_ids.push_back(candidate.source->candidate_id);
    }
    std::sort(result.pareto_candidate_ids.begin(), result.pareto_candidate_ids.end());
    if (result.pareto_candidate_ids.empty()) {
        result.status = CandidateSelectionStatus::Unavailable;
        result.diagnostic_codes.emplace_back("no_eligible_candidate");
        return result;
    }

    std::vector<const SelectionCandidate*> frontier;
    for (const SelectionCandidate& candidate : eligible) {
        if (std::binary_search(result.pareto_candidate_ids.begin(), result.pareto_candidate_ids.end(),
                               candidate.source->candidate_id))
            frontier.push_back(&candidate);
    }

    const SelectionCandidate* selected = nullptr;
    if (input.goal == RecommendationGoal::Balanced) {
        const auto score = [&](const SelectionCandidate& candidate) {
            return balanced_score(candidate, baseline_metrics.metrics, baseline_risks.assessment);
        };
        const auto maximum = *std::max_element(frontier.begin(), frontier.end(),
                                               [&](const auto* lhs, const auto* rhs) {
                                                   return score(*lhs) < score(*rhs);
                                               });
        const double maximum_score = score(*maximum);
        if (maximum_score <= CANDIDATE_SELECTION_EPSILON) {
            result.status = CandidateSelectionStatus::Unavailable;
            result.diagnostic_codes.emplace_back("no_explainable_benefit");
            return result;
        }
        frontier.erase(std::remove_if(frontier.begin(), frontier.end(), [&](const auto* candidate) {
                           const double candidate_score = score(*candidate);
                           return candidate_score <= CANDIDATE_SELECTION_EPSILON ||
                                  candidate_score + CANDIDATE_SELECTION_EPSILON < maximum_score;
                       }),
                       frontier.end());
        selected = *std::min_element(frontier.begin(), frontier.end(), [](const auto* lhs, const auto* rhs) {
            return lhs->source->candidate_id < rhs->source->candidate_id;
        });
    } else if (input.goal == RecommendationGoal::Quality) {
        frontier.erase(std::remove_if(frontier.begin(), frontier.end(), [&](const auto* candidate) {
                           for (const auto& pair : {
                                    std::pair<const RiskComponent*, const RiskComponent*>{
                                        &baseline_risks.assessment.appearance, &candidate->risks.appearance},
                                    {&baseline_risks.assessment.dimensional, &candidate->risks.dimensional},
                                    {&baseline_risks.assessment.strength, &candidate->risks.strength}}) {
                               const std::optional<double> improvement =
                                   comparable_improvement(*pair.first, *pair.second);
                               if (improvement && *improvement > CANDIDATE_SELECTION_EPSILON)
                                   return false;
                           }
                           return true;
                       }),
                       frontier.end());
        if (frontier.empty()) {
            result.status = CandidateSelectionStatus::Unavailable;
            result.diagnostic_codes.emplace_back("no_explainable_quality_benefit");
            return result;
        }
        selected = select_lexicographic(std::move(frontier), input.goal);
    } else {
        selected = select_lexicographic(std::move(frontier), input.goal);
    }

    result.status = CandidateSelectionStatus::Ready;
    result.selected_candidate_id = selected->source->candidate_id;
    result.evidence = published_evidence(*selected, baseline_metrics.metrics, baseline_risks.assessment);
    result.diagnostic_codes.emplace_back("candidate_selected");
    return result;
}

} // namespace Slic3r::AI::SmartSlicing
