#pragma once

#include "slic3r/GUI/AI/ModelGeneration/WorkbenchModelInspection.hpp"
#include "slic3r/AI/Contracts/IModelArtifactConsumer.hpp"
#include "libslic3r/MeshSeamRepair.hpp"

namespace Slic3r::GUI {

// Operates on a detached mesh, after whole-face and subface painting. Exact
// seam joining preserves every ordered triangle corner and painted face ID.
// Never run CGAL here: a topology-changing repair needs its own preview and
// explicit acceptance in the workbench.
inline bool prepare_workbench_import_topology(TriangleMesh& mesh, bool confirmed_risk,
    AI::ModelImportResult& result, std::string& error,
    const std::function<bool()>& canceled = {})
{
    if (canceled && canceled()) { error = "Model import canceled."; return false; }
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
