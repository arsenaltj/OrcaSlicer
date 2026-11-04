#pragma once

#include "slic3r/GUI/AI/ModelGeneration/WorkbenchModelInspection.hpp"
#include "slic3r/AI/Contracts/IModelArtifactConsumer.hpp"
#include "libslic3r/MeshSeamRepair.hpp"
#include "libslic3r/MeshRepair.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Geometry.hpp"
#include "slic3r/GUI/AI/Model/SurfaceSelectionState.hpp"

namespace Slic3r::GUI {

// Operates on a detached mesh, after whole-face and subface painting. Exact
// seam joining preserves every ordered triangle corner and painted face ID.
// Manual imports retain the workbench candidate acceptance contract.
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

// Same repair and painting transfer used by ModelVolume::repair(true), on a
// detached copy. Commit happens only after topology and painted-slot checks.
inline bool prepare_native_portrait_repair(const TriangleMesh& source,
    const TriangleSelector::TriangleSplittingData& source_painting,
    TriangleMesh& repaired, TriangleSelector::TriangleSplittingData& painting,
    AI::ModelImportResult& result, std::string& error,
    const std::function<bool()>& canceled = {}, const std::function<void(int)>& progress = {})
{
    const auto stop = [&] { return canceled && canceled(); };
    if (stop()) { error = "修复已取消，已配色模型保留。"; return false; }
    const auto before_id = AI::SurfaceSelectionPersistence::geometry_fingerprint(source.its);
    const auto original_painting = source_painting;
    for (size_t slot = 6; slot < original_painting.used_states.size(); ++slot)
        if (original_painting.used_states[slot]) {
            error = "人物配色使用了前五槽以外的耗材，已回滚导入。"; return false;
        }
    repaired = source;
    MeshRepairStats stats;
    if (!repair_mesh(repaired, &stats, &error, [&](int value) {
        if (stop()) throw std::runtime_error("修复已取消，已配色模型保留。");
        if (progress) progress(std::min(value, 95));
    }) || stop()) return false;
    if (progress) progress(96);
    const auto after_id = AI::SurfaceSelectionPersistence::geometry_fingerprint(repaired.its);
    painting = before_id == after_id ? original_painting :
        TriangleSelector::remap_painting(source.its, original_painting, repaired.its,
            Geometry::translation_transform(repaired.get_init_shift()), std::nullopt);
    if (stop()) { error = "颜色迁移已取消，已配色模型保留。"; return false; }
    if (progress) progress(97);
    const auto areas = [](const TriangleMesh& mesh, const auto& data) {
        TriangleSelector selector(mesh); selector.deserialize(data);
        std::array<double, 7> values {};
        for (size_t slot = 0; slot < values.size(); ++slot) {
            const auto facets = selector.get_facets_strict(static_cast<EnforcerBlockerType>(slot));
            for (const auto& f : facets.indices)
                values[slot] += 0.5 * double((facets.vertices[f[1]] - facets.vertices[f[0]])
                    .cross(facets.vertices[f[2]] - facets.vertices[f[0]]).norm());
        }
        return values;
    };
    const auto before = areas(source, original_painting), after = areas(repaired, painting);
    if (before[6] > 0 || after[6] > 0) { error = "人物修复检测到支撑专用槽被用于模型，已回滚导入。"; return false; }
    for (size_t slot = 1; slot <= 5; ++slot) {
        // Even very small eye/lip regions must survive native remapping.
        if ((before[slot] > 1e-9 && after[slot] < before[slot] * 0.75) ||
            (before[slot] > 1e-9 && after[slot] > before[slot] * 1.25)) {
            error = "原生修复后耗材槽 " + std::to_string(slot) + " 的颜色迁移未通过，已回滚导入。";
            return false;
        }
    }
    if (progress) progress(99);
    if (!prepare_workbench_import_topology(repaired, false, result, error, canceled)) return false;
    result.native_repair_completed = true;
    result.native_geometry_id = AI::SurfaceSelectionPersistence::geometry_fingerprint(repaired.its);
    result.native_repair_changed_geometry = before_id != result.native_geometry_id;
    return true;
}

} // namespace Slic3r::GUI
