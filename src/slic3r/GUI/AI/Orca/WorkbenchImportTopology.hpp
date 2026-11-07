#pragma once

#include "slic3r/GUI/AI/ModelGeneration/WorkbenchModelInspection.hpp"
#include "slic3r/GUI/AI/Orca/WorkbenchNativeImportTopology.hpp"
#include "slic3r/AI/Contracts/IModelArtifactConsumer.hpp"
#include "libslic3r/MeshSeamRepair.hpp"

namespace Slic3r::GUI {

// Operates on a detached mesh, after whole-face and subface painting. Exact
// seam joining preserves every ordered triangle corner and painted face ID.
// Manual imports retain the workbench candidate acceptance contract.
inline bool prepare_workbench_import_topology(TriangleMesh& mesh, bool confirmed_risk,
    AI::ModelImportResult& result, std::string& error,
    const std::function<bool()>& canceled = {}, const AI::ModelImportRequest* request = nullptr)
{
    if (canceled && canceled()) { error = "Model import canceled."; return false; }
    if (request && has_native_import_topology_proof(*request)) {
        std::string rejection;
        if (restore_certified_native_import_topology(mesh, *request, result, rejection, canceled)) {
            error.clear();
            return true;
        }
        // A claimed but invalid proof still gets the complete existing check.
        // Its result cannot authorize changed identities, even for a safe cube.
        TriangleMesh inspected = mesh;
        if (its_num_open_edges(inspected.its) != 0) stitch_exact_mesh_seams(inspected);
        const auto checked = inspect_workbench_mesh(inspected, canceled);
        result.topology_risk = result.manual_repair_required = true;
        result.topology_risk_reason = checked.summary;
        error = "Native portrait import certificate rejected: " + rejection + "; " + checked.summary;
        return false;
    }
    if (its_num_open_edges(mesh.its) != 0) stitch_exact_mesh_seams(mesh);
    const auto check = inspect_workbench_mesh(mesh, canceled);
    const size_t open = its_num_open_edges(mesh.its);
    const bool safe = open == 0 && workbench_topology_safe(check);
    result.topology_risk = !safe;
    result.manual_repair_required = !safe;
    result.topology_risk_reason = safe ? std::string {} : check.summary +
        " 原生未连接边 " + std::to_string(open) + "。";
    if (safe) return true;
    // Explicit exceptions never masquerade as a successfully repaired model.
    if (confirmed_risk && check.status != WorkbenchCheckStatus::Failed) return true;
    error = "导入前最终网格复检未通过，请回工作台修复并接受候选后重新导入。" + result.topology_risk_reason;
    return false;
}

} // namespace Slic3r::GUI
