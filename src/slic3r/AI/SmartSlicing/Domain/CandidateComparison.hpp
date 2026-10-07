#pragma once

#include "SliceCandidate.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

// One read-only baseline plus the three visible priority alternatives.
inline constexpr size_t MAX_COMPARABLE_CANDIDATES = 4;

struct CandidateComparison
{
    CandidateId recommended_candidate_id;
    std::vector<CandidateId> ordered_candidate_ids;
    std::vector<CandidateId> excluded_candidate_ids;
    std::vector<CandidateId> missing_metric_candidate_ids;
    std::vector<std::string> recommendation_evidence_codes;
    std::string algorithm_id, algorithm_version;
};

struct CandidateScoringStrategy {
    using CompareFn = std::function<CandidateComparison(const std::vector<SliceCandidate>&, CandidateGoal)>;
    CompareFn compare;
    std::string algorithm_id {"candidate-comparison"};
    std::string algorithm_version {"candidate-comparison-v1"};
};

CandidateComparison compare_candidates(const std::vector<SliceCandidate>& candidates,
                                       CandidateGoal goal,
                                       size_t maximum_candidates = MAX_COMPARABLE_CANDIDATES);

CandidateComparison compare_candidates_with_strategy(const std::vector<SliceCandidate>& candidates,
    CandidateGoal goal, const CandidateScoringStrategy& strategy);

} // namespace Slic3r::AI::SmartSlicing
