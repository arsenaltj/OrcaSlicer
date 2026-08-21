#pragma once

#include "slic3r/AI/SmartSlicing/Domain/WorkflowState.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

struct WorkflowRuntimeCandidate
{
    CandidateId id;
    CandidateGoal goal{CandidateGoal::Stability};
    CandidateStatus status{CandidateStatus::Draft};
};

struct WorkflowRuntimeRecord
{
    WorkflowId workflow_id{0};
    WorkflowState state{WorkflowState::Idle};
    WorkspaceRevision revision;
    std::vector<WorkflowRuntimeCandidate> candidates;
    std::string detail;
    int64_t updated_at_epoch_seconds{0};
};

class IWorkflowRuntimeStore
{
public:
    virtual ~IWorkflowRuntimeStore() = default;
    virtual std::optional<WorkflowRuntimeRecord> load() = 0;
    virtual void save(const WorkflowRuntimeRecord& record) = 0;
    virtual void clear(WorkflowId workflow_id) = 0;
};

} // namespace Slic3r::AI::SmartSlicing
