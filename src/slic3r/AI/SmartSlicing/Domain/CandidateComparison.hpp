#pragma once

#include "SliceCandidate.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

inline constexpr size_t MAX_COMPARABLE_CANDIDATES = 3;

struct CandidateComparison
{
    CandidateId recommended_candidate_id;
    std::vector<CandidateId> ordered_candidate_ids;
    std::vector<CandidateId> excluded_candidate_ids;
    std::vector<CandidateId> missing_metric_candidate_ids;
    std::vector<std::string> recommendation_evidence_codes;

    bool is_eligible(const CandidateId& candidate_id) const
    {
        return std::find(ordered_candidate_ids.begin(), ordered_candidate_ids.end(), candidate_id) !=
               ordered_candidate_ids.end();
    }
};

CandidateComparison compare_candidates(const std::vector<SliceCandidate>& candidates,
                                       CandidateGoal goal,
                                       size_t maximum_candidates = MAX_COMPARABLE_CANDIDATES);

} // namespace Slic3r::AI::SmartSlicing
