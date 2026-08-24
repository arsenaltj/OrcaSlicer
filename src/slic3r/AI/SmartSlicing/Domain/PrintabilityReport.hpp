#pragma once

#include "SmartSlicingTypes.hpp"
#include "WorkspaceRevision.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

struct PrintabilityIssue
{
    IssueCode code{IssueCode::EmptyPlate};
    Severity severity{Severity::Info};
    IssueScope scope{IssueScope::Workspace};
    uint64_t object_id{0};
    std::string evidence;
    std::vector<std::string> resolution_codes;
    bool blocks_trial_slice{false};
    bool requires_user_decision{false};
};

struct PrintabilityReport
{
    WorkspaceRevision revision;
    std::vector<PrintabilityIssue> issues;
    Readiness readiness{Readiness::Ready};

    bool has_blocking_issue() const
    {
        for (const PrintabilityIssue& issue : issues)
            if (issue.blocks_trial_slice)
                return true;
        return false;
    }
};

} // namespace Slic3r::AI::SmartSlicing
