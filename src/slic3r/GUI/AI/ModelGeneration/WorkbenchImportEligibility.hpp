#pragma once

#include "PostGenerationWorkbenchState.hpp"
#include "slic3r/AI/Contracts/IModelArtifactConsumer.hpp"

namespace Slic3r::GUI {

// Both the workbench and its asynchronous host must recognize the explicitly
// confirmed risk path without treating it as a topology-safe import.
inline bool workbench_import_request_allowed(const PostGenerationWorkbenchState& state,
    const AI::ModelImportRequest& request)
{
    if (!state.actions.can_import || state.model_path.empty() ||
        state.model_path != request.artifact.local_path || !state.candidate_path.empty() || state.dirty ||
        state.check.status == WorkbenchCheckStatus::Running ||
        state.check.candidate_status == RepairCandidateStatus::Generating ||
        state.check.candidate_status == RepairCandidateStatus::Ready)
        return false;
    if (request.topology_check_sha256 != state.check.model_sha256) return false;
    if (state.can_import_for_slicing) return true;
    // Only a confirmed, low-severity boundary risk can use the exception path.
    // Invalid, failed, unchecked and changed reports are never force-importable.
    return state.can_force_import && state.force_import_acknowledged &&
        state.check.status == WorkbenchCheckStatus::Attention &&
        state.check.force_import_risk && request.topology_risk_confirmed &&
        !state.check.model_sha256.empty();
}

} // namespace Slic3r::GUI
