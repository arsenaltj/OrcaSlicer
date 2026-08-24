#pragma once

#include "ParameterProposal.hpp"
#include "PlacementCandidate.hpp"
#include "RepairPlan.hpp"
#include "SlicingMetrics.hpp"
#include "SmartSlicingTypes.hpp"
#include "WorkspaceRevision.hpp"

#include <optional>
#include <string>

namespace Slic3r::AI::SmartSlicing {

struct SliceCandidate
{
    CandidateId id;
    WorkspaceRevision base_revision;
    CandidateGoal goal{CandidateGoal::Stability};
    std::optional<RepairPlan> repair;
    PlacementCandidate placement;
    ParameterProposal parameters;
    std::string explanation;
    std::string diagnostic_code;
    CandidateStatus status{CandidateStatus::Draft};
    std::optional<SlicingMetrics> metrics;
};

} // namespace Slic3r::AI::SmartSlicing
