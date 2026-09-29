#pragma once

#include "RecommendationSessionCoordinator.hpp"
#include "slic3r/AI/SmartSlicing/Domain/CandidateSelection.hpp"

#include <optional>
#include <string>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

struct CandidateSelectionTaskMappingResult
{
    std::optional<RecommendationTaskResult> task_result;
    std::vector<std::string> diagnostic_codes;

    bool valid() const { return task_result.has_value(); }
};

struct CandidateSelectionTaskContext
{
    RecommendationTaskIdentity task_identity;
    CandidateId baseline_candidate_id;
    std::string strategy_version;
};

CandidateSelectionTaskMappingResult map_candidate_selection_task(
    const CandidateSelectionResult& selection,
    const CandidateSelectionTaskContext& expected_context);

} // namespace Slic3r::AI::SmartSlicing
