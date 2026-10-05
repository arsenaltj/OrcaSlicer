#pragma once

#include "SliceCandidate.hpp"

#include <cstddef>
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
};

CandidateComparison compare_candidates(const std::vector<SliceCandidate>& candidates,
                                       CandidateGoal goal,
                                       size_t maximum_candidates = MAX_COMPARABLE_CANDIDATES);

} // namespace Slic3r::AI::SmartSlicing
