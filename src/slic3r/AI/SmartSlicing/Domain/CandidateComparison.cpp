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
    case CandidateGoal::MaterialSaving: return candidate.metrics->filament_volume_mm3;
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

OptionalMetric metric_or_missing(const SliceCandidate& candidate, const OptionalMetric SlicingMetrics::* member)
{
    return candidate.metrics ? candidate.metrics.value().*member : std::nullopt;
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
        case CandidateGoal::MaterialSaving: return "less_material";
        }
    }
    return "deterministic_tie_break";
}

bool usable(const SliceCandidate& candidate)
{
    return candidate.status == CandidateStatus::Ready && candidate.metrics.has_value();
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
