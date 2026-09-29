#pragma once

#include "slic3r/AI/SmartSlicing/Domain/ApplyContract.hpp"
#include "slic3r/AI/SmartSlicing/Domain/SliceCandidate.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace Slic3r::AI::SmartSlicing {

enum class OfficialSlicePhase { Rejected, Prepared, Applied, Slicing, Completed, Failed };

struct OfficialApplySnapshotIdentity
{
    uint64_t action_snapshot_time{0};
    uint64_t applied_snapshot_time{0};
    std::string action_name;

    bool valid() const
    {
        return action_snapshot_time < applied_snapshot_time && !action_name.empty();
    }
};

struct OfficialApplyTransactionIdentity
{
    std::string plan_id;
    CandidateId candidate_id;
    WorkspaceRevision applied_revision;
    OfficialApplySnapshotIdentity snapshot;
    WorkflowId workflow_id{0};
    AttemptId attempt_id{0};

    bool valid() const
    {
        return !plan_id.empty() && !candidate_id.empty() && applied_revision.valid() && snapshot.valid() &&
               workflow_id != 0 && attempt_id != 0;
    }
};

inline bool operator==(const OfficialApplyTransactionIdentity& lhs,
                       const OfficialApplyTransactionIdentity& rhs)
{
    return lhs.plan_id == rhs.plan_id && lhs.candidate_id == rhs.candidate_id &&
           lhs.applied_revision == rhs.applied_revision &&
           lhs.snapshot.action_snapshot_time == rhs.snapshot.action_snapshot_time &&
           lhs.snapshot.applied_snapshot_time == rhs.snapshot.applied_snapshot_time &&
           lhs.snapshot.action_name == rhs.snapshot.action_name &&
           lhs.workflow_id == rhs.workflow_id && lhs.attempt_id == rhs.attempt_id;
}

inline bool operator!=(const OfficialApplyTransactionIdentity& lhs,
                       const OfficialApplyTransactionIdentity& rhs)
{
    return !(lhs == rhs);
}

struct OfficialSliceResult
{
    OfficialSlicePhase phase{OfficialSlicePhase::Rejected};
    std::string diagnostic_code;
    bool workspace_mutated{false};
    bool can_undo{false};
    std::optional<OfficialApplyTransactionIdentity> apply_transaction;
    bool can_retry_slice{false};
    bool can_print{false};
};

class IOfficialSliceGateway
{
public:
    virtual ~IOfficialSliceGateway() = default;

    virtual OfficialSliceResult prepare(const SliceCandidate& candidate,
                                        const WorkspaceRevision& expected_revision) = 0;
    virtual OfficialSliceResult commit(const SliceCandidate& candidate,
                                       const WorkspaceRevision& expected_revision) = 0;
    virtual OfficialSliceResult commit_plan(const AtomicApplyPlan&)
    {
        return {OfficialSlicePhase::Rejected, "versioned_apply_plan_not_supported", false, false};
    }
    virtual OfficialSliceResult start_committed_plan_slice(const OfficialApplyTransactionIdentity&)
    {
        return {OfficialSlicePhase::Rejected, "versioned_official_slice_not_supported", false, false};
    }
    virtual OfficialSliceResult retry_official_slice(const OfficialApplyTransactionIdentity&)
    {
        return {OfficialSlicePhase::Rejected, "slice_retry_not_allowed", false, false};
    }
    virtual OfficialSliceResult complete_official_slice(const OfficialApplyTransactionIdentity&, bool,
                                                        std::string = {})
    {
        return {OfficialSlicePhase::Rejected, "versioned_official_slice_not_supported", false, false};
    }
    virtual OfficialSliceResult poll_committed_plan(const OfficialApplyTransactionIdentity&)
    {
        return {OfficialSlicePhase::Rejected, "versioned_official_slice_not_supported", false, false};
    }
    virtual OfficialSliceResult undo_committed_plan(const OfficialApplyTransactionIdentity&)
    {
        return {OfficialSlicePhase::Rejected, "apply_undo_unavailable", false, false};
    }
    virtual OfficialSliceResult poll() = 0;
    virtual bool undo_last_apply() = 0;
};

} // namespace Slic3r::AI::SmartSlicing
