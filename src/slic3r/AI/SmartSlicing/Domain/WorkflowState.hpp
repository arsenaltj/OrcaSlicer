#pragma once

#include "PrintabilityReport.hpp"
#include "WorkspaceContext.hpp"

#include <optional>
#include <string>

namespace Slic3r::AI::SmartSlicing {

enum class WorkflowState {
    Idle,
    CapturingContext,
    Preflighting,
    AwaitingRiskDecision,
    ReadyForCandidatePlanning,
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
    std::string detail;

    bool can_start() const
    {
        return state == WorkflowState::Idle || state == WorkflowState::Canceled || state == WorkflowState::Stale ||
               state == WorkflowState::Failed;
    }
    bool can_cancel() const
    {
        return state == WorkflowState::CapturingContext || state == WorkflowState::Preflighting ||
               state == WorkflowState::AwaitingRiskDecision || state == WorkflowState::ReadyForCandidatePlanning;
    }
};

} // namespace Slic3r::AI::SmartSlicing
