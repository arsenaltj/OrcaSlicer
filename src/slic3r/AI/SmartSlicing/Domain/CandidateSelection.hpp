#pragma once

#include "CandidateEvaluation.hpp"
#include "WorkspaceRevision.hpp"

#include <optional>
#include <string>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

inline constexpr double CANDIDATE_SELECTION_EPSILON = 1e-9;

enum class CandidateSelectionStatus { Ready, Unavailable, InvalidInput };

struct CandidateSelectionBinding
{
    WorkflowId workflow_id{0};
    AttemptId attempt_id{0};
    WorkspaceRevision workspace_revision;
    CandidateId baseline_candidate_id;
    CandidateId goal_task_candidate_id;
    std::string strategy_version;
    std::string goal_contract_version{GOAL_CONTRACT_VERSION};
    std::string evaluation_policy_version{CANDIDATE_EVALUATION_POLICY_VERSION};
    std::string selection_policy_version{CANDIDATE_SELECTION_POLICY_VERSION};
};

struct EvaluatedCandidate
{
    CandidateSelectionBinding binding;
    CandidateId candidate_id;
    CandidateEvaluationInput evaluation_input;
    CandidateEvaluationResult evaluation;
};

struct CandidateSelectionInput
{
    CandidateSelectionBinding binding;
    RecommendationGoal goal{RecommendationGoal::Balanced};
    TrialMetrics baseline_metrics;
    RiskAssessment baseline_risks;
    std::vector<EvaluatedCandidate> candidates;
};

struct CandidateSelectionResult
{
    CandidateSelectionBinding binding;
    RecommendationGoal goal{RecommendationGoal::Balanced};
    CandidateSelectionStatus status{CandidateSelectionStatus::Unavailable};
    CandidateId selected_candidate_id;
    std::vector<CandidateId> pareto_candidate_ids;
    std::vector<CandidateId> excluded_candidate_ids;
    std::vector<std::string> diagnostic_codes;
    std::optional<RecommendationEvidence> evidence;
};

CandidateSelectionResult select_candidate(const CandidateSelectionInput& input);

} // namespace Slic3r::AI::SmartSlicing
