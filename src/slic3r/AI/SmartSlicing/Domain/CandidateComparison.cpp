#include "CandidateComparison.hpp"

#include <algorithm>
#include <optional>

namespace Slic3r::AI::SmartSlicing {
namespace {

using OptionalMetric = std::optional<double>;

OptionalMetric primary_metric(const SliceCandidate& candidate, CandidateGoal goal)
{
    if (!candidate.metrics)
        return std::nullopt;

    switch (goal) {
    case CandidateGoal::Speed: return candidate.metrics->estimated_time_seconds;
    case CandidateGoal::MaterialSaving: return candidate.metrics->total_material_volume_mm3();
    case CandidateGoal::Quality: return candidate.metrics->support_volume_mm3;
    case CandidateGoal::Stability: return static_cast<double>(candidate.metrics->warning_codes.size());
    }
    return std::nullopt;
}

bool less_optional(const OptionalMetric& lhs, const OptionalMetric& rhs)
{
    if (lhs.has_value() != rhs.has_value())
        return lhs.has_value();
    return lhs && rhs && *lhs < *rhs;
}

bool equal_optional(const OptionalMetric& lhs, const OptionalMetric& rhs)
{
    return lhs.has_value() == rhs.has_value() && (!lhs || *lhs == *rhs);
}

bool greater_optional(const OptionalMetric& lhs, const OptionalMetric& rhs)
{
    if (lhs.has_value() != rhs.has_value())
        return lhs.has_value();
    return lhs && rhs && *lhs > *rhs;
}

OptionalMetric metric_or_missing(const SliceCandidate& candidate, const OptionalMetric SlicingMetrics::* member)
{
    return candidate.metrics ? candidate.metrics.value().*member : std::nullopt;
}

OptionalMetric count_or_missing(const SliceCandidate& candidate, const std::optional<size_t> SlicingMetrics::* member)
{
    if (!candidate.metrics)
        return std::nullopt;
    const std::optional<size_t>& value = candidate.metrics.value().*member;
    return value ? OptionalMetric(static_cast<double>(*value)) : std::nullopt;
}

bool candidate_less(const SliceCandidate* lhs, const SliceCandidate* rhs, CandidateGoal goal)
{
    const OptionalMetric lhs_primary = primary_metric(*lhs, goal);
    const OptionalMetric rhs_primary = primary_metric(*rhs, goal);
    if (!equal_optional(lhs_primary, rhs_primary))
        return less_optional(lhs_primary, rhs_primary);

    const OptionalMetric lhs_warnings = lhs->metrics ? OptionalMetric(lhs->metrics->warning_codes.size()) : std::nullopt;
    const OptionalMetric rhs_warnings = rhs->metrics ? OptionalMetric(rhs->metrics->warning_codes.size()) : std::nullopt;
    if (!equal_optional(lhs_warnings, rhs_warnings))
        return less_optional(lhs_warnings, rhs_warnings);

    const OptionalMetric lhs_adhesion_risk = metric_or_missing(*lhs, &SlicingMetrics::bed_adhesion_risk_score);
    const OptionalMetric rhs_adhesion_risk = metric_or_missing(*rhs, &SlicingMetrics::bed_adhesion_risk_score);
    if (goal == CandidateGoal::Stability && !equal_optional(lhs_adhesion_risk, rhs_adhesion_risk))
        return less_optional(lhs_adhesion_risk, rhs_adhesion_risk);

    const OptionalMetric lhs_brim = metric_or_missing(*lhs, &SlicingMetrics::brim_volume_mm3);
    const OptionalMetric rhs_brim = metric_or_missing(*rhs, &SlicingMetrics::brim_volume_mm3);
    const bool adhesion_risk_present =
        lhs_adhesion_risk.value_or(0.0) >= BED_ADHESION_RISK_ATTENTION_THRESHOLD ||
        rhs_adhesion_risk.value_or(0.0) >= BED_ADHESION_RISK_ATTENTION_THRESHOLD;
    if (goal == CandidateGoal::Stability && adhesion_risk_present && !equal_optional(lhs_brim, rhs_brim))
        return greater_optional(lhs_brim, rhs_brim);

    const OptionalMetric lhs_tool_changes = count_or_missing(*lhs, &SlicingMetrics::tool_changes);
    const OptionalMetric rhs_tool_changes = count_or_missing(*rhs, &SlicingMetrics::tool_changes);
    if (!equal_optional(lhs_tool_changes, rhs_tool_changes))
        return less_optional(lhs_tool_changes, rhs_tool_changes);

    const OptionalMetric lhs_flush = metric_or_missing(*lhs, &SlicingMetrics::flush_volume_mm3);
    const OptionalMetric rhs_flush = metric_or_missing(*rhs, &SlicingMetrics::flush_volume_mm3);
    if (!equal_optional(lhs_flush, rhs_flush))
        return less_optional(lhs_flush, rhs_flush);

    const OptionalMetric lhs_wipe_tower = metric_or_missing(*lhs, &SlicingMetrics::wipe_tower_volume_mm3);
    const OptionalMetric rhs_wipe_tower = metric_or_missing(*rhs, &SlicingMetrics::wipe_tower_volume_mm3);
    if (!equal_optional(lhs_wipe_tower, rhs_wipe_tower))
        return less_optional(lhs_wipe_tower, rhs_wipe_tower);

    const OptionalMetric lhs_support = metric_or_missing(*lhs, &SlicingMetrics::support_volume_mm3);
    const OptionalMetric rhs_support = metric_or_missing(*rhs, &SlicingMetrics::support_volume_mm3);
    if (!equal_optional(lhs_support, rhs_support))
        return less_optional(lhs_support, rhs_support);

    const OptionalMetric lhs_time = metric_or_missing(*lhs, &SlicingMetrics::estimated_time_seconds);
    const OptionalMetric rhs_time = metric_or_missing(*rhs, &SlicingMetrics::estimated_time_seconds);
    if (!equal_optional(lhs_time, rhs_time))
        return less_optional(lhs_time, rhs_time);

    const OptionalMetric lhs_material = metric_or_missing(*lhs, &SlicingMetrics::filament_volume_mm3);
    const OptionalMetric rhs_material = metric_or_missing(*rhs, &SlicingMetrics::filament_volume_mm3);
    if (!equal_optional(lhs_material, rhs_material))
        return less_optional(lhs_material, rhs_material);

    return lhs->id < rhs->id;
}

std::string recommendation_evidence(const SliceCandidate& winner, const SliceCandidate& runner_up, CandidateGoal goal)
{
    const OptionalMetric winner_primary = primary_metric(winner, goal);
    const OptionalMetric runner_primary = primary_metric(runner_up, goal);
    if (!equal_optional(winner_primary, runner_primary)) {
        switch (goal) {
        case CandidateGoal::Stability: return "fewer_slice_warnings";
        case CandidateGoal::Quality: return "less_support_material";
        case CandidateGoal::Speed: return "shorter_print_time";
        case CandidateGoal::MaterialSaving: return "less_total_material_including_multicolor_waste";
        }
    }
    const OptionalMetric winner_adhesion_risk =
        metric_or_missing(winner, &SlicingMetrics::bed_adhesion_risk_score);
    const OptionalMetric runner_adhesion_risk =
        metric_or_missing(runner_up, &SlicingMetrics::bed_adhesion_risk_score);
    if (goal == CandidateGoal::Stability && !equal_optional(winner_adhesion_risk, runner_adhesion_risk))
        return "lower_bed_adhesion_risk";
    const OptionalMetric winner_brim = metric_or_missing(winner, &SlicingMetrics::brim_volume_mm3);
    const OptionalMetric runner_brim = metric_or_missing(runner_up, &SlicingMetrics::brim_volume_mm3);
    const bool adhesion_risk_present =
        winner_adhesion_risk.value_or(0.0) >= BED_ADHESION_RISK_ATTENTION_THRESHOLD ||
        runner_adhesion_risk.value_or(0.0) >= BED_ADHESION_RISK_ATTENTION_THRESHOLD;
    if (goal == CandidateGoal::Stability && adhesion_risk_present && !equal_optional(winner_brim, runner_brim))
        return "stronger_bed_adhesion_aid";
    const OptionalMetric winner_tool_changes = count_or_missing(winner, &SlicingMetrics::tool_changes);
    const OptionalMetric runner_tool_changes = count_or_missing(runner_up, &SlicingMetrics::tool_changes);
    if (!equal_optional(winner_tool_changes, runner_tool_changes))
        return "fewer_tool_changes";
    const OptionalMetric winner_flush = metric_or_missing(winner, &SlicingMetrics::flush_volume_mm3);
    const OptionalMetric runner_flush = metric_or_missing(runner_up, &SlicingMetrics::flush_volume_mm3);
    if (!equal_optional(winner_flush, runner_flush))
        return "lower_flush_volume";
    const OptionalMetric winner_wipe = metric_or_missing(winner, &SlicingMetrics::wipe_tower_volume_mm3);
    const OptionalMetric runner_wipe = metric_or_missing(runner_up, &SlicingMetrics::wipe_tower_volume_mm3);
    if (!equal_optional(winner_wipe, runner_wipe))
        return "lower_wipe_tower_volume";
    return "deterministic_tie_break";
}

bool usable(const SliceCandidate& candidate)
{
    return candidate.status == CandidateStatus::Ready && candidate.metrics.has_value() &&
           candidate.metrics->physical_slots_compatible != false && candidate.metrics->color_mapping_degraded != true;
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
