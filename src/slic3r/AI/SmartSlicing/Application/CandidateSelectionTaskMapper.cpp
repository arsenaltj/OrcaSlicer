#include "CandidateSelectionTaskMapper.hpp"

#include <algorithm>

namespace Slic3r::AI::SmartSlicing {

CandidateSelectionTaskMappingResult map_candidate_selection_task(
    const CandidateSelectionResult& selection,
    const CandidateSelectionTaskContext& expected_context)
{
    CandidateSelectionTaskMappingResult result;
    const RecommendationTaskIdentity& task_identity = expected_context.task_identity;
    const RecommendationGoalParseResult parsed = parse_recommendation_goal_id(task_identity.goal_id);
    const bool expected_context_valid = !expected_context.baseline_candidate_id.empty() &&
                                        !expected_context.strategy_version.empty();
    const bool binding_valid = selection.binding.workflow_id != 0 &&
                               selection.binding.attempt_id != 0 &&
                               selection.binding.workspace_revision.valid() &&
                               !selection.binding.baseline_candidate_id.empty() &&
                               !selection.binding.goal_task_candidate_id.empty() &&
                               !selection.binding.strategy_version.empty() &&
                               selection.binding.goal_contract_version == GOAL_CONTRACT_VERSION &&
                               selection.binding.evaluation_policy_version ==
                                   CANDIDATE_EVALUATION_POLICY_VERSION &&
                               selection.binding.selection_policy_version ==
                                   CANDIDATE_SELECTION_POLICY_VERSION;
    if (!expected_context_valid || !binding_valid || !parsed ||
        task_identity.workflow_id != selection.binding.workflow_id ||
        task_identity.attempt_id != selection.binding.attempt_id ||
        task_identity.workspace_revision != selection.binding.workspace_revision ||
        task_identity.candidate_id != selection.binding.goal_task_candidate_id ||
        expected_context.baseline_candidate_id != selection.binding.baseline_candidate_id ||
        expected_context.strategy_version != selection.binding.strategy_version ||
        *parsed.goal != selection.goal ||
        selection.binding.selection_policy_version != CANDIDATE_SELECTION_POLICY_VERSION) {
        result.diagnostic_codes.emplace_back("selection_task_binding_mismatch");
        return result;
    }

    RecommendationTaskResult task;
    task.identity = task_identity;
    task.diagnostic_codes = selection.diagnostic_codes;
    if (!std::all_of(task.diagnostic_codes.begin(), task.diagnostic_codes.end(),
                     [](const std::string& code) { return valid_recommendation_code(code); })) {
        result.diagnostic_codes.emplace_back("invalid_selection_diagnostic_codes");
        return result;
    }

    switch (selection.status) {
    case CandidateSelectionStatus::Ready:
        if (selection.selected_candidate_id.empty() || !selection.evidence ||
            std::find(selection.pareto_candidate_ids.begin(), selection.pareto_candidate_ids.end(),
                      selection.selected_candidate_id) == selection.pareto_candidate_ids.end() ||
            !validate_recommendation_evidence(*selection.evidence).empty()) {
            result.diagnostic_codes.emplace_back("invalid_ready_selection_result");
            return result;
        }
        task.outcome = RecommendationTaskOutcome::Ready;
        task.selected_candidate_id = selection.selected_candidate_id;
        task.evidence = selection.evidence;
        break;
    case CandidateSelectionStatus::Unavailable:
        if (!selection.selected_candidate_id.empty() || selection.evidence) {
            result.diagnostic_codes.emplace_back("invalid_unavailable_selection_result");
            return result;
        }
        task.outcome = RecommendationTaskOutcome::Unavailable;
        break;
    case CandidateSelectionStatus::InvalidInput:
        if (!selection.selected_candidate_id.empty() || selection.evidence) {
            result.diagnostic_codes.emplace_back("invalid_failed_selection_result");
            return result;
        }
        task.outcome = RecommendationTaskOutcome::Failed;
        break;
    }
    result.task_result = std::move(task);
    return result;
}

} // namespace Slic3r::AI::SmartSlicing
