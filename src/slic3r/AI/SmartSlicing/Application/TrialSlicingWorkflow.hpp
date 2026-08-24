#pragma once

#include "slic3r/AI/SmartSlicing/Domain/SliceCandidate.hpp"
#include "slic3r/AI/SmartSlicing/Ports/ITrialSliceExecutor.hpp"

#include <utility>

namespace Slic3r::AI::SmartSlicing {

class TrialSlicingWorkflow
{
public:
    static bool result_matches(const SliceCandidate& candidate, const TrialSliceResult& result)
    {
        return result.candidate_id == candidate.id && result.base_revision == candidate.base_revision;
    }

    static bool accept_result(SliceCandidate& candidate, TrialSliceResult result)
    {
        if (!result_matches(candidate, result) || result.status != TrialSliceStatus::Succeeded || !result.metrics) {
            candidate.status          = CandidateStatus::Failed;
            candidate.metrics.reset();
            candidate.diagnostic_code = result_matches(candidate, result) ? result.diagnostic_code : "trial_result_mismatch";
            return false;
        }

        candidate.status          = CandidateStatus::Ready;
        candidate.metrics         = std::move(result.metrics);
        candidate.diagnostic_code = std::move(result.diagnostic_code);
        return true;
    }
};

} // namespace Slic3r::AI::SmartSlicing
