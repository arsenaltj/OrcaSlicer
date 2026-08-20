#pragma once

#include "slic3r/AI/SmartSlicing/Domain/CandidateComparison.hpp"
#include "slic3r/AI/SmartSlicing/Domain/SliceCandidate.hpp"
#include "slic3r/AI/SmartSlicing/Domain/WorkspaceContext.hpp"

#include <algorithm>
#include <set>
#include <utility>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

class CandidatePlanningWorkflow
{
public:
    std::vector<SliceCandidate> plan(const WorkspaceContext& context,
                                     std::vector<SliceCandidate> proposals,
                                     CandidateGoal goal = CandidateGoal::Stability) const
    {
        SliceCandidate baseline;
        baseline.id            = "baseline";
        baseline.base_revision = context.revision;
        baseline.goal          = goal;

        std::vector<SliceCandidate> planned;
        planned.reserve(std::min(MAX_COMPARABLE_CANDIDATES, proposals.size() + 1));
        planned.push_back(std::move(baseline));

        std::sort(proposals.begin(), proposals.end(),
                  [](const SliceCandidate& lhs, const SliceCandidate& rhs) { return lhs.id < rhs.id; });
        std::set<CandidateId> accepted_ids{"baseline"};
        for (SliceCandidate& proposal : proposals) {
            if (planned.size() == MAX_COMPARABLE_CANDIDATES)
                break;
            if (proposal.id.empty() || proposal.base_revision != context.revision || !accepted_ids.insert(proposal.id).second)
                continue;

            proposal.base_revision  = context.revision;
            proposal.goal           = goal;
            proposal.status         = CandidateStatus::Draft;
            proposal.metrics.reset();
            proposal.diagnostic_code.clear();
            planned.push_back(std::move(proposal));
        }
        return planned;
    }
};

} // namespace Slic3r::AI::SmartSlicing
