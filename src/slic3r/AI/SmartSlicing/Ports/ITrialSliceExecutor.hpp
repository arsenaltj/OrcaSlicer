#pragma once

#include "slic3r/AI/SmartSlicing/Domain/SliceCandidate.hpp"

#include <optional>
#include <string>

namespace Slic3r::AI::SmartSlicing {

enum class TrialSliceStatus { Succeeded, Canceled, Failed };

struct TrialSliceResult
{
    WorkflowId workflow_id{0};
    CandidateId candidate_id;
    WorkspaceRevision base_revision;
    TrialSliceStatus status{TrialSliceStatus::Failed};
    std::optional<SlicingMetrics> metrics;
    std::string diagnostic_code;
};

class ITrialSliceExecutor
{
public:
    virtual ~ITrialSliceExecutor() = default;
    virtual TrialSliceResult execute_trial_slice(const SliceCandidate& candidate) = 0;
    virtual void cancel_trial_slice() = 0;
};

} // namespace Slic3r::AI::SmartSlicing
