#pragma once

#include "PostGenerationUiState.hpp"
#include "PortraitOptimization.hpp"
#include "slic3r/AI/Contracts/IPrintablePaletteProvider.hpp"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace Slic3r::GUI {

enum class WorkbenchCheckStatus { NotRun, Running, Normal, Attention, Invalid, Failed };
enum class WorkbenchCheckPhase { Idle, Inspecting, Repairing, Rechecking };
enum class RepairCandidateStatus { None, Generating, Ready, Accepted, Rejected, Failed };
enum class WorkbenchFloatingCardMode { None, ModelCheck, HonorSuiteSemantic };
// Automatic checks are run when a model first becomes visible in the 3D
// workbench; manual checks are initiated from the sidebar and retain the
// existing candidate workflow.
enum class WorkbenchCheckSource { Automatic, Manual };

struct WorkbenchCheckResult {
    WorkbenchCheckStatus status {WorkbenchCheckStatus::NotRun};
    WorkbenchCheckSource source {WorkbenchCheckSource::Automatic};
    std::string model_sha256;
    std::string summary;
    size_t boundary_edges {0};
    size_t nonmanifold_edges {0};
    WorkbenchCheckPhase phase {WorkbenchCheckPhase::Idle};
    std::string repair_reason;
    size_t removed_faces {0};
    size_t reversed_faces {0};
    size_t degenerate_faces {0}, duplicate_faces {0}, inconsistent_edges {0};
    // Boundary and topology details used by the post-generation import gate.
    size_t boundary_loops {0};
    std::vector<size_t> boundary_loop_sizes;
    size_t nonmanifold_vertices {0};
    size_t self_intersections {0};
    size_t isolated_components {0};
    // A repair is always a temporary candidate.  The original check remains
    // available while the candidate is being reviewed.
    RepairCandidateStatus candidate_status {RepairCandidateStatus::None};
    std::string candidate_path;
    std::string source_sha256;
    std::string candidate_sha256;
    size_t before_faces {0}, before_vertices {0};
    size_t after_faces {0}, after_vertices {0};
    bool appearance_mapped {false};
    double appearance_confidence {0.0};
    std::string appearance_reason;
    bool force_import_risk {false};
};

inline bool workbench_automatic_check_should_start(bool workbench_open, bool workbench_presented,
                                                   bool model_ready)
{
    return workbench_open && workbench_presented && model_ready;
}

inline bool workbench_ai_palette_should_apply(bool pending, WorkbenchCheckSource source,
                                              bool topology_safe, RepairCandidateStatus candidate_status,
                                              bool repair_candidate_loaded)
{
    if (!pending || source != WorkbenchCheckSource::Automatic || !topology_safe) return false;
    return repair_candidate_loaded ? candidate_status == RepairCandidateStatus::Ready
                                   : candidate_status == RepairCandidateStatus::None;
}

inline bool workbench_check_card_should_show(bool workbench_open, bool workbench_presented,
                                             bool requested_visible,
                                             bool check_running, bool automatic_repair_saving,
                                             const WorkbenchCheckResult& result)
{
    if (!workbench_open || !workbench_presented) return false;
    const bool automatic = result.source == WorkbenchCheckSource::Automatic;
    const bool repair_phase = result.phase == WorkbenchCheckPhase::Repairing ||
        result.phase == WorkbenchCheckPhase::Rechecking;
    bool show = requested_visible;
    if (check_running && (automatic || repair_phase)) show = true;
    if (automatic_repair_saving) show = true;
    if (!check_running && !automatic_repair_saving && !automatic &&
        result.candidate_status == RepairCandidateStatus::Ready) show = false;
    if (!check_running && !automatic_repair_saving && automatic &&
        (result.status == WorkbenchCheckStatus::Normal || result.status == WorkbenchCheckStatus::NotRun) &&
        result.candidate_status != RepairCandidateStatus::Ready) show = false;
    return show;
}

inline WorkbenchFloatingCardMode workbench_floating_card_mode(bool workbench_open,
    bool workbench_presented, bool honor_semantic_active, bool requested_check_visible,
    bool check_running, bool automatic_repair_saving, const WorkbenchCheckResult& result)
{
    if (!workbench_open || !workbench_presented) return WorkbenchFloatingCardMode::None;
    if (honor_semantic_active) return WorkbenchFloatingCardMode::HonorSuiteSemantic;
    return workbench_check_card_should_show(workbench_open, workbench_presented, requested_check_visible,
        check_running, automatic_repair_saving, result)
        ? WorkbenchFloatingCardMode::ModelCheck : WorkbenchFloatingCardMode::None;
}

inline bool workbench_modal_navigation_blocked(bool workbench_open, bool workbench_presented,
    bool honor_semantic_active, bool requested_check_visible, bool check_running,
    bool automatic_repair_saving, const WorkbenchCheckResult& result)
{
    if (!workbench_open || !workbench_presented) return false;
    return check_running || automatic_repair_saving ||
        workbench_floating_card_mode(workbench_open, workbench_presented, honor_semantic_active,
            requested_check_visible, check_running, automatic_repair_saving, result) !=
            WorkbenchFloatingCardMode::None;
}

inline bool workbench_navigation_allowed(bool modal_blocking, bool leaves_workbench)
{
    return !modal_blocking || !leaves_workbench;
}

inline std::string workbench_check_status_line(const WorkbenchCheckResult& check,
                                               bool risk_acknowledged = false)
{
    if (risk_acknowledged && check.status == WorkbenchCheckStatus::Attention)
        return "已忽略轻微问题，可继续导入切片";
    if (check.candidate_status == RepairCandidateStatus::Ready)
        return "修复候选已生成，等待确认";
    switch (check.status) {
    case WorkbenchCheckStatus::NotRun: return "模型尚未检查";
    case WorkbenchCheckStatus::Running:
        switch (check.phase) {
        case WorkbenchCheckPhase::Repairing: return "正在修复模型……";
        case WorkbenchCheckPhase::Rechecking: return "正在复检模型……";
        default: return "正在检查模型……";
        }
    case WorkbenchCheckStatus::Normal:
        return !check.repair_reason.empty() || check.removed_faces || check.reversed_faces ||
                       (check.before_faces && check.after_faces && check.before_faces != check.after_faces)
            ? "模型修复已完成" : "模型检查已完成，未发现问题";
    case WorkbenchCheckStatus::Attention: return "模型检查完成，存在轻微问题";
    case WorkbenchCheckStatus::Invalid: return "模型检查未通过";
    case WorkbenchCheckStatus::Failed: return "模型检查失败";
    }
    return "模型尚未检查";
}

inline std::string workbench_check_detail_text(const WorkbenchCheckResult& check,
                                               bool risk_acknowledged = false)
{
    std::ostringstream text;
    text << workbench_check_status_line(check, risk_acknowledged);
    if (!check.summary.empty()) text << "\n\n检查说明\n" << check.summary;
    if (!check.repair_reason.empty()) text << "\n\n修复说明\n" << check.repair_reason;
    if (check.before_faces || check.before_vertices || check.after_faces || check.after_vertices) {
        text << "\n\n修复统计\n修复前：" << check.before_faces << " 面 / " << check.before_vertices
             << " 点\n修复后：" << check.after_faces << " 面 / " << check.after_vertices << " 点"
             << "\n清理面：" << check.removed_faces << "\n校正面：" << check.reversed_faces;
    }
    text << "\n\n拓扑指标"
         << "\n开放边：" << check.boundary_edges
         << "\n边界环：" << check.boundary_loops
         << "\n非流形边：" << check.nonmanifold_edges
         << "\n非流形顶点：" << check.nonmanifold_vertices
         << "\n退化面：" << check.degenerate_faces
         << "\n重复面：" << check.duplicate_faces
         << "\n方向不一致：" << check.inconsistent_edges
         << "\n孤立组件：" << check.isolated_components
         << "\n自交风险：" << check.self_intersections;
    if (!check.boundary_loop_sizes.empty()) {
        text << "\n边界环规模：";
        for (size_t index = 0; index < check.boundary_loop_sizes.size(); ++index) {
            if (index) text << "、";
            text << check.boundary_loop_sizes[index];
        }
    }
    if (check.appearance_mapped || !check.appearance_reason.empty() || check.appearance_confidence > 0.0) {
        text << "\n\n外观映射\n状态：" << (check.appearance_mapped ? "已验证" : "未验证")
             << "\n置信度：" << std::fixed << std::setprecision(0)
             << std::clamp(check.appearance_confidence, 0.0, 1.0) * 100.0 << "%";
        if (!check.appearance_reason.empty()) text << "\n说明：" << check.appearance_reason;
    }
    if (risk_acknowledged && check.status == WorkbenchCheckStatus::Attention)
        text << "\n\n风险处理\n轻微问题已由用户忽略，许可仅绑定当前模型检查结果。";
    return text.str();
}

inline std::string workbench_palette_status_line(bool original_appearance_available,
                                                 bool honor_suite_active,
                                                 size_t honor_suite_used_colors,
                                                 size_t palette_color_count,
                                                 const std::string& honor_fallback_reason = {})
{
    if (!original_appearance_available) return "原始配色证据不可用";
    if (!honor_suite_active)
        return "当前色卡 · " + std::to_string(palette_color_count) + " 色";
    const std::string usage = "模型当前使用 " + std::to_string(honor_suite_used_colors) + "/6 色";
    return honor_fallback_reason.empty()
        ? "荣耀耗材套装 · " + usage
        : honor_fallback_reason + "\n" + usage;
}

// Native workbench configuration is independent of the six-channel AI port.
// An absent slot is a UI placeholder; it is never a fabricated loaded spool.
struct WorkbenchFilamentSlot {
    size_t slot {0};
    std::string color, preset, material, reason;
    bool configured {false}, compatible {false}, removable {false}, exists {false};
};
struct WorkbenchMixedFilament {
    size_t slot {0};
    std::string color, recipe, reason;
    bool valid {false};
};
struct WorkbenchPrinterFilaments {
    std::string printer, identity, layout, signature, reason;
    size_t nozzles {0}, physical_count {0}, idle_nozzles {0}, physical_capacity {32};
    std::vector<WorkbenchFilamentSlot> slots;
    std::vector<WorkbenchMixedFilament> mixed;
    bool ready {false};
};

struct PostGenerationWorkbenchState {
    PostGenerationUiState actions;
    std::uint64_t revision {0};
    std::string asset_id;
    std::string model_path;
    std::string candidate_path;
    bool editing {false};
    bool workbench_presented {false};
    bool modal_blocking {false};
    SemanticMode semantic_mode {SemanticMode::General};
    PortraitOptimizationState portrait_optimization;
    bool portrait_enabled {false};
    bool portrait_available {false};
    std::string portrait_unavailable_reason;
    WorkbenchCheckResult check;
    std::vector<std::string> palette;
    size_t faces {0};
    size_t vertices {0};
    bool dirty {false};
    bool can_reoptimize_regions {false};
    std::string reoptimization_reason;
    bool can_edit_project_colors {false};
    bool can_import_for_slicing {false};
    bool can_force_import {false};
    bool force_import_acknowledged {false};
    bool import_has_topology_risk {false};
    std::string import_topology_reason;
    bool can_print {false};
    AI::PrintablePaletteSnapshot project_palette;
    std::vector<AI::PhysicalFilamentChannel> project_channels;
    WorkbenchPrinterFilaments printer_filaments;
    bool can_apply_printer_filaments {false};
    std::string apply_filaments_reason;
};

inline bool workbench_region_optimization_allowed(const PostGenerationUiState& actions,
    bool candidate_ready, bool semantic_available)
{
    return actions.can_edit && !candidate_ready && semantic_available;
}

inline bool workbench_printer_trial_action_allowed(bool trial_active, bool application_saving,
    bool can_edit_project_colors, bool has_valid_model_card)
{
    return !application_saving &&
        (trial_active || (can_edit_project_colors && has_valid_model_card));
}

inline bool workbench_physical_slot_exists(const std::vector<AI::PhysicalFilamentChannel>& channels, size_t slot)
{
    return std::any_of(channels.begin(), channels.end(),
        [slot](const auto& channel) { return channel.slot == slot; });
}

using PostGenerationWorkbenchListener = std::function<void(const PostGenerationWorkbenchState&)>;

}
