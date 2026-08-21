#pragma once

#include "slic3r/AI/SmartSlicing/Domain/SliceCandidate.hpp"

#include <string>

namespace Slic3r::AI::SmartSlicing {

enum class OfficialSlicePhase { Rejected, Prepared, Slicing, Completed, Failed };

struct OfficialSliceResult
{
    OfficialSlicePhase phase{OfficialSlicePhase::Rejected};
    std::string diagnostic_code;
    bool workspace_mutated{false};
    bool can_undo{false};
};

class IOfficialSliceGateway
{
public:
    virtual ~IOfficialSliceGateway() = default;

    virtual OfficialSliceResult prepare(const SliceCandidate& candidate,
                                        const WorkspaceRevision& expected_revision) = 0;
    virtual OfficialSliceResult commit(const SliceCandidate& candidate,
                                       const WorkspaceRevision& expected_revision) = 0;
    virtual OfficialSliceResult poll() = 0;
    virtual bool undo_last_apply() = 0;
};

} // namespace Slic3r::AI::SmartSlicing
