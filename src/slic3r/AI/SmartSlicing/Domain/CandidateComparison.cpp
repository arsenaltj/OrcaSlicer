#include "CandidateComparison.hpp"

#include <algorithm>
#include <optional>
#include <vector>

namespace Slic3r::AI::SmartSlicing {
namespace {

using OptionalMetric = std::optional<double>;

enum class ComparisonDimension
{
    Warnings,
    AdhesionRisk,
    BrimAssistance,
    ToolChanges,
    FlushVolume,
    WipeTowerVolume,
    SupportVolume,
    EstimatedTime,
    FilamentVolume,
    TotalMaterial
};

enum class PreferredDirection { Lower, Higher };
enum class Applicability { Always, StabilityBrimWhenRiskPresent };

struct DimensionRule
{
    ComparisonDimension dimension;
    PreferredDirection direction;
    const char* evidence_code;
    Applicability applicability = Applicability::Always;
};

struct PolicyDifference
{
    const DimensionRule* rule;
    OptionalMetric lhs;
    OptionalMetric rhs;
    bool lhs_preferred;
};

const std::vector<DimensionRule>& comparison_policy(CandidateGoal goal)
{
    static const std::vector<DimensionRule> stability{
        {ComparisonDimension::Warnings, PreferredDirection::Lower, "fewer_slice_warnings"},
        {ComparisonDimension::AdhesionRisk, PreferredDirection::Lower, "lower_bed_adhesion_risk"},
        {ComparisonDimension::BrimAssistance, PreferredDirection::Higher, "stronger_bed_adhesion_aid",
         Applicability::StabilityBrimWhenRiskPresent},
        {ComparisonDimension::ToolChanges, PreferredDirection::Lower, "fewer_tool_changes"},
        {ComparisonDimension::FlushVolume, PreferredDirection::Lower, "lower_flush_volume"},
        {ComparisonDimension::WipeTowerVolume, PreferredDirection::Lower, "lower_wipe_tower_volume"},
        {ComparisonDimension::SupportVolume, PreferredDirection::Lower, "lower_support_volume"},
        {ComparisonDimension::EstimatedTime, PreferredDirection::Lower, "lower_estimated_time"},
        {ComparisonDimension::FilamentVolume, PreferredDirection::Lower, "lower_filament_volume"},
    };
    static const std::vector<DimensionRule> quality{
        {ComparisonDimension::SupportVolume, PreferredDirection::Lower, "less_support_material"},
        {ComparisonDimension::Warnings, PreferredDirection::Lower, "fewer_slice_warnings"},
        {ComparisonDimension::EstimatedTime, PreferredDirection::Lower, "lower_estimated_time"},
        {ComparisonDimension::FilamentVolume, PreferredDirection::Lower, "lower_filament_volume"},
        {ComparisonDimension::ToolChanges, PreferredDirection::Lower, "fewer_tool_changes"},
        {ComparisonDimension::FlushVolume, PreferredDirection::Lower, "lower_flush_volume"},
        {ComparisonDimension::WipeTowerVolume, PreferredDirection::Lower, "lower_wipe_tower_volume"},
    };
    static const std::vector<DimensionRule> speed{
        {ComparisonDimension::EstimatedTime, PreferredDirection::Lower, "shorter_print_time"},
        {ComparisonDimension::Warnings, PreferredDirection::Lower, "fewer_slice_warnings"},
        {ComparisonDimension::ToolChanges, PreferredDirection::Lower, "fewer_tool_changes"},
        {ComparisonDimension::FlushVolume, PreferredDirection::Lower, "lower_flush_volume"},
        {ComparisonDimension::WipeTowerVolume, PreferredDirection::Lower, "lower_wipe_tower_volume"},
        {ComparisonDimension::SupportVolume, PreferredDirection::Lower, "lower_support_volume"},
        {ComparisonDimension::FilamentVolume, PreferredDirection::Lower, "lower_filament_volume"},
    };
    static const std::vector<DimensionRule> material_saving{
        {ComparisonDimension::TotalMaterial, PreferredDirection::Lower,
         "less_total_material_including_multicolor_waste"},
        {ComparisonDimension::Warnings, PreferredDirection::Lower, "fewer_slice_warnings"},
        {ComparisonDimension::FlushVolume, PreferredDirection::Lower, "lower_flush_volume"},
        {ComparisonDimension::WipeTowerVolume, PreferredDirection::Lower, "lower_wipe_tower_volume"},
        {ComparisonDimension::ToolChanges, PreferredDirection::Lower, "fewer_tool_changes"},
        {ComparisonDimension::SupportVolume, PreferredDirection::Lower, "lower_support_volume"},
        {ComparisonDimension::EstimatedTime, PreferredDirection::Lower, "lower_estimated_time"},
        {ComparisonDimension::FilamentVolume, PreferredDirection::Lower, "lower_filament_volume"},
    };

    switch (goal) {
    case CandidateGoal::Stability: return stability;
    case CandidateGoal::Quality: return quality;
    case CandidateGoal::Speed: return speed;
    case CandidateGoal::MaterialSaving: return material_saving;
    }
    return stability;
}

OptionalMetric metric(const SliceCandidate& candidate, ComparisonDimension dimension)
{
    if (!candidate.metrics)
        return std::nullopt;

    const SlicingMetrics& metrics = *candidate.metrics;
    switch (dimension) {
    case ComparisonDimension::Warnings:
        return static_cast<double>(metrics.warning_codes.size());
    case ComparisonDimension::AdhesionRisk: return metrics.bed_adhesion_risk_score;
    case ComparisonDimension::BrimAssistance: return metrics.brim_volume_mm3;
    case ComparisonDimension::ToolChanges:
        return metrics.tool_changes ? OptionalMetric(static_cast<double>(*metrics.tool_changes)) : std::nullopt;
    case ComparisonDimension::FlushVolume: return metrics.flush_volume_mm3;
    case ComparisonDimension::WipeTowerVolume: return metrics.wipe_tower_volume_mm3;
    case ComparisonDimension::SupportVolume: return metrics.support_volume_mm3;
    case ComparisonDimension::EstimatedTime: return metrics.estimated_time_seconds;
    case ComparisonDimension::FilamentVolume: return metrics.filament_volume_mm3;
    case ComparisonDimension::TotalMaterial: return metrics.total_material_volume_mm3();
    }
    return std::nullopt;
}

bool equal_metric(const OptionalMetric& lhs, const OptionalMetric& rhs)
{
    return lhs.has_value() == rhs.has_value() && (!lhs || *lhs == *rhs);
}

bool rule_applies(const DimensionRule& rule, const SliceCandidate& lhs, const SliceCandidate& rhs)
{
    if (rule.applicability == Applicability::Always)
        return true;
    const OptionalMetric lhs_risk = metric(lhs, ComparisonDimension::AdhesionRisk);
    const OptionalMetric rhs_risk = metric(rhs, ComparisonDimension::AdhesionRisk);
    return lhs_risk.value_or(0.0) >= BED_ADHESION_RISK_ATTENTION_THRESHOLD ||
           rhs_risk.value_or(0.0) >= BED_ADHESION_RISK_ATTENTION_THRESHOLD;
}

bool lhs_is_preferred(const OptionalMetric& lhs,
                      const OptionalMetric& rhs,
                      PreferredDirection direction)
{
    if (lhs.has_value() != rhs.has_value())
        return lhs.has_value();
    if (!lhs)
        return false;
    return direction == PreferredDirection::Lower ? *lhs < *rhs : *lhs > *rhs;
}

std::optional<PolicyDifference> first_policy_difference(const SliceCandidate& lhs,
                                                        const SliceCandidate& rhs,
                                                        CandidateGoal goal)
{
    for (const DimensionRule& rule : comparison_policy(goal)) {
        if (!rule_applies(rule, lhs, rhs))
            continue;
        const OptionalMetric lhs_value = metric(lhs, rule.dimension);
        const OptionalMetric rhs_value = metric(rhs, rule.dimension);
        if (!equal_metric(lhs_value, rhs_value))
            return PolicyDifference{&rule, lhs_value, rhs_value,
                                    lhs_is_preferred(lhs_value, rhs_value, rule.direction)};
    }
    return std::nullopt;
}

OptionalMetric primary_metric(const SliceCandidate& candidate, CandidateGoal goal)
{
    return metric(candidate, comparison_policy(goal).front().dimension);
}

bool candidate_less(const SliceCandidate* lhs, const SliceCandidate* rhs, CandidateGoal goal)
{
    if (const auto difference = first_policy_difference(*lhs, *rhs, goal))
        return difference->lhs_preferred;
    return lhs->id < rhs->id;
}

std::string recommendation_evidence(const SliceCandidate& winner,
                                    const SliceCandidate& runner_up,
                                    CandidateGoal goal)
{
    if (const auto difference = first_policy_difference(winner, runner_up, goal)) {
        if (difference->lhs.has_value() != difference->rhs.has_value())
            return "more_complete_trial_evidence";
        return difference->rule->evidence_code;
    }
    return "deterministic_tie_break";
}

bool usable(const SliceCandidate& candidate)
{
    if (candidate.status != CandidateStatus::Ready || !candidate.metrics ||
        candidate.metrics->physical_slots_compatible == false || candidate.metrics->color_mapping_degraded == true)
        return false;
    return candidate.metrics->has_valid_measurements();
}

} // namespace

CandidateComparison compare_candidates(const std::vector<SliceCandidate>& candidates,
                                       CandidateGoal goal,
                                       size_t maximum_candidates)
{
    CandidateComparison comparison;
    std::vector<const SliceCandidate*> usable_candidates;
    usable_candidates.reserve(candidates.size());

    for (const SliceCandidate& candidate : candidates) {
        if (!usable(candidate)) {
            comparison.excluded_candidate_ids.push_back(candidate.id);
            continue;
        }
        if (!primary_metric(candidate, goal))
            comparison.missing_metric_candidate_ids.push_back(candidate.id);
        usable_candidates.push_back(&candidate);
    }

    std::sort(comparison.excluded_candidate_ids.begin(), comparison.excluded_candidate_ids.end());
    std::sort(comparison.missing_metric_candidate_ids.begin(), comparison.missing_metric_candidate_ids.end());
    std::sort(usable_candidates.begin(), usable_candidates.end(),
              [goal](const SliceCandidate* lhs, const SliceCandidate* rhs) { return candidate_less(lhs, rhs, goal); });

    const size_t count = std::min(maximum_candidates, usable_candidates.size());
    comparison.ordered_candidate_ids.reserve(count);
    for (size_t index = 0; index < count; ++index)
        comparison.ordered_candidate_ids.push_back(usable_candidates[index]->id);

    if (!comparison.ordered_candidate_ids.empty()) {
        comparison.recommended_candidate_id = comparison.ordered_candidate_ids.front();
        if (count > 1)
            comparison.recommendation_evidence_codes.push_back(
                recommendation_evidence(*usable_candidates[0], *usable_candidates[1], goal));
    }
    return comparison;
}

} // namespace Slic3r::AI::SmartSlicing
