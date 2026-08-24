#pragma once

#include "slic3r/AI/SmartSlicing/Ports/IOfficialSliceGateway.hpp"

namespace Slic3r::AI::SmartSlicing {

class ApplyWorkflow
{
public:
    OfficialSliceResult start(const SliceCandidate& candidate, const WorkspaceRevision& expected_revision,
                              const WorkspaceRevision& current_revision, IOfficialSliceGateway& gateway) const
    {
        if (candidate.base_revision != expected_revision || current_revision != expected_revision)
            return {OfficialSlicePhase::Rejected, "stale_revision", false, false};
        if (candidate.status != CandidateStatus::Ready)
            return {OfficialSlicePhase::Rejected, "candidate_not_ready", false, false};

        OfficialSliceResult prepared = gateway.prepare(candidate, expected_revision);
        if (prepared.phase != OfficialSlicePhase::Prepared)
            return prepared;
        return gateway.commit(candidate, expected_revision);
    }
};

} // namespace Slic3r::AI::SmartSlicing
