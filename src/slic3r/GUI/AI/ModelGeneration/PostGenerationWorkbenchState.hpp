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
enum class WorkbenchCheckPurpose { GeneralImport, PortraitSourceRisk, PortraitPostColorReadOnly, SavedModelReadOnly };
// Both entry points inspect, repair and atomically publish without user review.
enum class WorkbenchCheckSource { Automatic, Manual };

struct WorkbenchCheckResult {
    WorkbenchCheckStatus status {WorkbenchCheckStatus::NotRun};
    WorkbenchCheckSource source {WorkbenchCheckSource::Automatic};
    WorkbenchCheckPurpose purpose {WorkbenchCheckPurpose::GeneralImport};
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
    bool self_intersections_checked {false};
    size_t small_faces {0};
    std::vector<size_t> issue_faces;
    size_t isolated_components {0};
    // A temporary repair is internal until its verified publication completes.
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

// Geometry inspection is independent of Beauty's filament matching gate.
// Shared processing and unsaved work still protect the current asset.
inline bool workbench_check_start_allowed(bool presented, bool model_ready,
    bool processing, bool candidate_pending, bool unsaved_changes)
{
    return presented && model_ready && !processing && !candidate_pending && !unsaved_changes;
}

enum class WorkbenchCheckOutcome { Pending, Processing, Passed, Failed };

inline WorkbenchCheckOutcome workbench_check_outcome(const WorkbenchCheckResult& check,
    bool processing = false)
{
    if (processing || check.status == WorkbenchCheckStatus::Running)
        return WorkbenchCheckOutcome::Processing;
    if (check.status == WorkbenchCheckStatus::Attention || check.status == WorkbenchCheckStatus::Invalid ||
        check.status == WorkbenchCheckStatus::Failed)
        return WorkbenchCheckOutcome::Failed;
    if (check.candidate_status == RepairCandidateStatus::Generating ||
        check.candidate_status == RepairCandidateStatus::Ready)
        return WorkbenchCheckOutcome::Processing;
    if (check.status == WorkbenchCheckStatus::NotRun) return WorkbenchCheckOutcome::Pending;
    return check.status == WorkbenchCheckStatus::Normal
        ? WorkbenchCheckOutcome::Passed : WorkbenchCheckOutcome::Failed;
}

inline bool workbench_check_allows_topology_repair(bool native_partition_requested = false)
{
    // Shared partitioning owns repair before clustering; checks are read-only until then.
    return !native_partition_requested;
}

inline bool workbench_initial_partition_requested(bool automatic, bool appearance_available,
    bool native_partition_valid, bool persisted_colors, bool bound_edits)
{
    return automatic && appearance_available && !native_partition_valid && !persisted_colors && !bound_edits;
}


inline bool workbench_native_partition_should_start(bool pending, WorkbenchCheckSource source,
                                                     WorkbenchCheckStatus status,
                                                     RepairCandidateStatus candidate_status,
                                                     bool repair_candidate_loaded)
{
    const bool readable_terminal_check = status == WorkbenchCheckStatus::Normal ||
        status == WorkbenchCheckStatus::Attention || status == WorkbenchCheckStatus::Invalid;
    // Attention and Invalid are topology risks, not failures to read the
    // source. The native partition must receive them so its strict pre-cluster
    // repair can run exactly once.
    return pending && source == WorkbenchCheckSource::Automatic && readable_terminal_check &&
        !repair_candidate_loaded && candidate_status == RepairCandidateStatus::None;
}

inline bool workbench_ai_palette_should_resume_after_accept(bool pending,
    WorkbenchCheckSource source, bool topology_safe, RepairCandidateStatus candidate_status)
{
    return pending && source == WorkbenchCheckSource::Automatic && topology_safe &&
        candidate_status == RepairCandidateStatus::Ready;
}

inline bool workbench_default_ai_palette_ready(bool enabled, int source,
    int requested_count, bool suite_active, bool has_native_basis)
{
    return enabled && source == 0 && requested_count == 6 && !suite_active && has_native_basis;
}

inline bool workbench_honor_suite_requires_native_partition(bool native_partition_valid)
{
    return !native_partition_valid;
}

inline bool workbench_check_card_should_show(bool workbench_open, bool workbench_presented,
                                             bool requested_visible,
                                             bool check_running, bool automatic_repair_saving,
                                             const WorkbenchCheckResult& result)
{
    if (!workbench_open || !workbench_presented) return false;
    // Portrait checks are internal gates. Their terminal result is represented
    // in the sidebar and flow ledger, never as the generic import/repair modal.
    if (!check_running && !automatic_repair_saving &&
        result.purpose != WorkbenchCheckPurpose::GeneralImport)
        return false;
    // Failed/invalid general checks remain available in the inline status and
    // details view. A terminal failure card looks like it stopped the strict
    // native repair even when that separate operation is still progressing.
    if (!check_running && !automatic_repair_saving &&
        (result.status == WorkbenchCheckStatus::Invalid ||
         result.status == WorkbenchCheckStatus::Failed))
        return false;
    const bool automatic = result.source == WorkbenchCheckSource::Automatic;
    bool show = requested_visible;
    // Both entry checks and explicit checks need an immediate, cancellable
    // progress card, including the initial inspection phase.
    if (check_running) show = true;
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
    bool check_running, bool automatic_repair_saving, bool native_partition_running,
    const WorkbenchCheckResult& result)
{
    if (!workbench_open || !workbench_presented) return WorkbenchFloatingCardMode::None;
    if (native_partition_running) return WorkbenchFloatingCardMode::ModelCheck;
    if (honor_semantic_active) return WorkbenchFloatingCardMode::HonorSuiteSemantic;
    return workbench_check_card_should_show(workbench_open, workbench_presented, requested_check_visible,
        check_running, automatic_repair_saving, result)
        ? WorkbenchFloatingCardMode::ModelCheck : WorkbenchFloatingCardMode::None;
}

inline bool workbench_modal_navigation_blocked(bool workbench_open, bool workbench_presented,
    bool honor_semantic_active, bool requested_check_visible, bool check_running,
    bool automatic_repair_saving, bool native_partition_running,
    const WorkbenchCheckResult& result)
{
    if (!workbench_open || !workbench_presented) return false;
    return check_running || automatic_repair_saving || native_partition_running ||
        workbench_floating_card_mode(workbench_open, workbench_presented, honor_semantic_active,
            requested_check_visible, check_running, automatic_repair_saving,
            native_partition_running, result) !=
            WorkbenchFloatingCardMode::None;
}

inline bool workbench_navigation_allowed(bool modal_blocking, bool leaves_workbench)
{
    return !modal_blocking || !leaves_workbench;
}

inline std::string workbench_check_status_line(const WorkbenchCheckResult& check,
                                               bool risk_acknowledged = false)
{
    if (check.candidate_status == RepairCandidateStatus::Ready)
        return "修复复检通过，正在保存……";
    switch (check.status) {
    case WorkbenchCheckStatus::NotRun: return "模型尚未检查";
    case WorkbenchCheckStatus::Running:
        switch (check.phase) {
        case WorkbenchCheckPhase::Repairing: return "正在修复模型……";
        case WorkbenchCheckPhase::Rechecking: return "正在复检模型……";
        default: return "正在检查模型……";
        }
    case WorkbenchCheckStatus::Normal: return "检查通过";
    case WorkbenchCheckStatus::Attention:
    case WorkbenchCheckStatus::Invalid:
    case WorkbenchCheckStatus::Failed: return "检查失败";
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
    if (check.after_faces || check.after_vertices) {
        text << "\n\n修复统计\n修复前：" << check.before_faces << " 面 / " << check.before_vertices
             << " 点\n修复后：" << check.after_faces << " 面 / " << check.after_vertices << " 点"
             << "\n清理面：" << check.removed_faces << "\n校正面：" << check.reversed_faces;
    } else if (check.before_faces || check.before_vertices) {
        text << "\n\n模型统计\n当前模型：" << check.before_faces << " 面 / " << check.before_vertices << " 点";
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
         << "\n微小但非退化面：" << check.small_faces
         << "\n自交检测：" << (check.self_intersections_checked
             ? (check.self_intersections ? "发现自交" : "未发现自交") : "尚未完成，需先修复拓扑");
    if (!check.boundary_loop_sizes.empty()) {
        text << "\n边界环规模：";
        for (size_t index = 0; index < check.boundary_loop_sizes.size(); ++index) {
            if (index) text << "、";
            text << check.boundary_loop_sizes[index];
        }
    }
    if (!check.issue_faces.empty()) {
        text << "\n问题面序号（从1开始，最多定位4096面）：";
        for (size_t i = 0; i < std::min(size_t(32), check.issue_faces.size()); ++i)
            text << (i ? "、" : "") << check.issue_faces[i] + 1;
        if (check.issue_faces.size() > 32) text << "…";
    }
    if (check.appearance_mapped || !check.appearance_reason.empty() || check.appearance_confidence > 0.0) {
        text << "\n\n外观映射\n状态：" << (check.appearance_mapped ? "已验证" : "未验证")
             << "\n置信度：" << std::fixed << std::setprecision(0)
             << std::clamp(check.appearance_confidence, 0.0, 1.0) * 100.0 << "%";
        if (!check.appearance_reason.empty()) text << "\n说明：" << check.appearance_reason;
    }
    if (risk_acknowledged && check.status == WorkbenchCheckStatus::Attention)
        text << "\n\n风险处理\n开放边界风险已由用户确认，许可仅绑定当前模型检查结果。";
    return text.str();
}

enum class WorkbenchOriginalAppearanceState { Empty, Loading, Available, Uncolored, Invalid, ModelError };

// Availability alone also returns false before a model has finished loading.
// Only diagnose its original appearance after the current model is ready;
// a replacement load may still have the previous model in the preview.
inline WorkbenchOriginalAppearanceState workbench_original_appearance_state(
    PostGenerationUiState::Status status, bool preview_loading, bool model_ready,
    bool original_appearance_available, bool has_original_appearance_error)
{
    if (preview_loading || status == PostGenerationUiState::Status::Loading)
        return WorkbenchOriginalAppearanceState::Loading;
    if (status == PostGenerationUiState::Status::Error)
        return WorkbenchOriginalAppearanceState::ModelError;
    if (!model_ready || status == PostGenerationUiState::Status::Empty)
        return WorkbenchOriginalAppearanceState::Empty;
    if (original_appearance_available) return WorkbenchOriginalAppearanceState::Available;
    return has_original_appearance_error ? WorkbenchOriginalAppearanceState::Invalid
                                         : WorkbenchOriginalAppearanceState::Uncolored;
}

inline std::string workbench_original_appearance_hint(WorkbenchOriginalAppearanceState state)
{
    switch (state) {
    case WorkbenchOriginalAppearanceState::Empty: return "请从右侧选择或打开3D模型";
    case WorkbenchOriginalAppearanceState::Loading: return "正在加载模型…";
    case WorkbenchOriginalAppearanceState::Uncolored: return "当前模型不含原始颜色，原色对照不可用";
    case WorkbenchOriginalAppearanceState::Invalid: return "原始配色证据不可用";
    case WorkbenchOriginalAppearanceState::ModelError: return "模型加载失败，请重新打开模型";
    case WorkbenchOriginalAppearanceState::Available: return {};
    }
    return {};
}

inline std::string workbench_palette_status_line(WorkbenchOriginalAppearanceState appearance,
                                                 bool honor_suite_active,
                                                 size_t honor_suite_used_colors,
                                                 size_t palette_color_count,
                                                 const std::string& honor_fallback_reason = {})
{
    // The existing load-error surface already explains model failures.
    if (appearance == WorkbenchOriginalAppearanceState::ModelError) return {};
    if (appearance != WorkbenchOriginalAppearanceState::Available)
        return workbench_original_appearance_hint(appearance);
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
    std::string source_sha256;
    std::string geometry_id;
    std::string appearance_id;
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
    bool automatic_palette_active {false};
    size_t automatic_palette_requested_count {0};
    size_t faces {0};
    size_t vertices {0};
    bool dirty {false};
    bool can_reoptimize_regions {false};
    std::string reoptimization_reason;
    bool can_edit_project_colors {false};
    bool can_import_for_slicing {false};
    bool deferred_native_repair {false};
    std::string native_repair_source_sha256;
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
