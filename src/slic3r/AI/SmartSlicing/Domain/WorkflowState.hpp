#pragma once

#include "CandidateComparison.hpp"
#include "PrintabilityReport.hpp"
#include "SliceCandidate.hpp"
#include "WorkspaceContext.hpp"

#include <optional>
#include <string>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

enum class WorkflowState {
    Idle,
    CapturingContext,
    Preflighting,
    AwaitingRiskDecision,
    ReadyForCandidatePlanning,
    PlanningCandidates,
    TrialSlicingBaseline,
    TrialSlicingCandidates,
    ReadyToApply,
    Applying,
    OfficialSlicing,
    Completed,
    ApplyFailed,
    Canceling,
    Canceled,
    Stale,
    Failed
};

struct WorkflowSnapshot
{
    WorkflowId workflow_id{0};
    WorkflowState state{WorkflowState::Idle};
    std::optional<WorkspaceContext> context;
    std::optional<PrintabilityReport> report;
    std::vector<SliceCandidate> candidates;
    std::optional<CandidateComparison> comparison;
    CandidateId selected_candidate_id;
    bool can_undo_apply{false};
    CandidateGoal goal{CandidateGoal::Stability};
    std::string detail;

    bool can_start() const
    {
        return state == WorkflowState::Idle || state == WorkflowState::Canceled || state == WorkflowState::Stale ||
               state == WorkflowState::Failed || state == WorkflowState::Completed ||
               (state == WorkflowState::ApplyFailed && !can_undo_apply);
    }
    bool can_cancel() const
    {
        return state == WorkflowState::CapturingContext || state == WorkflowState::Preflighting ||
               state == WorkflowState::AwaitingRiskDecision || state == WorkflowState::ReadyForCandidatePlanning ||
               state == WorkflowState::PlanningCandidates || state == WorkflowState::TrialSlicingBaseline ||
               state == WorkflowState::TrialSlicingCandidates || state == WorkflowState::ReadyToApply;
    }
};

} // namespace Slic3r::AI::SmartSlicing
