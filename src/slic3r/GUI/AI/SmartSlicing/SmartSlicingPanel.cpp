#include "SmartSlicingPanel.hpp"

#include "slic3r/AI/SmartSlicing/Application/SmartSlicingCoordinator.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <wx/app.h>
#include <wx/button.h>
#include <wx/choice.h>
#include <wx/radiobut.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/weakref.h>

namespace Slic3r::GUI {
namespace {

wxString summary_text(const std::string& key)
{
    if (key == "capturing_workspace")
        return _L("正在读取当前打印板与配置…");
    if (key == "inspecting_printability")
        return _L("正在执行可打印性检查…");
    if (key == "printability_action_required")
        return _L("发现需要处理的可打印性问题");
    if (key == "preflight_complete_with_warnings")
        return _L("检查完成，但仍有提示需要留意");
    if (key == "preflight_complete")
        return _L("检查完成，可以进入候选优化");
    if (key == "planning_candidates")
        return _L("正在生成确定性候选方案…");
    if (key == "trial_slicing_baseline")
        return _L("正在试切当前基线方案…");
    if (key == "trial_slicing_candidates")
        return _L("正在顺序试切候选方案…");
    if (key == "candidates_ready")
        return _L("试切完成，请比较并选择方案");
    if (key == "applying_candidate")
        return _L("正在事务式应用所选方案…");
    if (key == "official_slicing")
        return _L("方案已应用，正在执行正式切片…");
    if (key == "official_slice_complete")
        return _L("正式切片完成，已进入预览");
    if (key == "official_slice_failed")
        return _L("正式切片失败，可一键撤销本次应用");
    if (key == "official_slice_failed_no_recovery")
        return _L("方案应用或正式切片失败，请检查工程状态");
    if (key == "official_slice_failed_applied")
        return _L("方案已写入，但正式切片失败且无法安全一键撤销，请检查工程状态");
    if (key == "canceling")
        return _L("正在取消…");
    if (key == "canceled")
        return _L("检查已取消");
    if (key == "workspace_changed")
        return _L("工程已变化，需要重新检查");
    if (key == "preflight_failed")
        return _L("检查失败，请重试");
    if (key == "interrupted_workflow_recovered")
        return _L("上次智能切片被中断，已安全清理临时候选；请重新检查");
    return _L("从当前打印板开始智能切片");
}

wxString status_text(SmartSlicingStageStatus status)
{
    switch (status) {
    case SmartSlicingStageStatus::Active: return _L("进行中");
    case SmartSlicingStageStatus::Complete: return _L("完成");
    case SmartSlicingStageStatus::NeedsAttention: return _L("需处理");
    case SmartSlicingStageStatus::Disabled: return _L("尚未开始");
    case SmartSlicingStageStatus::Waiting: return _L("等待");
    }
    return _L("等待");
}

wxString issue_name(const std::string& code)
{
    if (code == "empty_plate")
        return _L("当前打印板为空");
    if (code == "open_mesh")
        return _L("网格存在开放边");
    if (code == "outside_build_volume")
        return _L("对象超出打印空间");
    if (code == "missing_printer")
        return _L("未选择打印机");
    if (code == "missing_process")
        return _L("未选择工艺预设");
    if (code == "missing_material")
        return _L("未选择材料");
    if (code == "incompatible_physical_slots")
        return _L("物理槽位材料温区不兼容");
    if (code == "invalid_material_temperature_range")
        return _L("材料推荐温区无效");
    if (code == "color_mapping_degraded")
        return _L("颜色到物理槽位映射不完整");
    if (code == "multicolor_evidence_unavailable")
        return _L("多色兼容证据不可用");
    if (code == "native_validation_unavailable")
        return _L("当前原生配置校验尚不可用");
    if (code == "configuration_validation_error")
        return _L("配置校验错误");
    if (code == "configuration_validation_warning")
        return _L("配置校验警告");
    return _L("可打印性问题");
}

wxString format_duration(const std::optional<double>& seconds)
{
    if (!seconds)
        return _L("不可用");
    const long long total_minutes = static_cast<long long>(std::llround(*seconds / 60.0));
    return wxString::Format("%lldh %02lldm", total_minutes / 60, total_minutes % 60);
}

void set_wrapped_label(wxStaticText& label, const wxString& text, int width)
{
    label.SetLabel(text);
    // wxStaticText caches the last Wrap() width even after SetLabel(). Reset it so dynamic labels are rewrapped.
    label.Wrap(-1);
    label.Wrap(width);
    label.InvalidateBestSize();
}

wxString format_volume(const std::optional<double>& volume_mm3)
{
    return volume_mm3 ? wxString::Format("%.2f cm³", *volume_mm3 / 1000.0) : _L("不可用");
}

wxString format_delta(const std::optional<double>& value, double scale, const wxString& unit)
{
    return value ? wxString::Format("%+.2f %s", *value / scale, unit.c_str()) : _L("—");
}

wxString candidate_failure_text(const std::string& diagnostic_code)
{
    if (diagnostic_code == "workflow_timeout")
        return _L("超过本次试切时间预算");
    if (diagnostic_code == "workflow_memory_budget_exceeded")
        return _L("候选副本超过内存预算");
    if (diagnostic_code == "workflow_disk_budget_exceeded")
        return _L("临时切片文件超过磁盘预算");
    if (diagnostic_code == "trial_slice_canceled" || diagnostic_code == "retry_canceled")
        return _L("本次试切已取消");
    if (diagnostic_code == "retry_revision_unavailable")
        return _L("无法确认当前工程版本");
    if (diagnostic_code == "invalid_candidate_placement" ||
        diagnostic_code == "trial_no_printable_objects")
        return _L("候选摆放或可打印对象无效");
    if (diagnostic_code == "current_plate_locked")
        return _L("当前打印盘已锁定，不能应用候选摆放");
    if (diagnostic_code == "candidate_repair_unsupported")
        return _L("当前版本尚不能安全试切或应用网格修复");
    if (diagnostic_code.rfind("parameter_", 0) == 0)
        return _L("候选参数未通过 Orca 安全校验");
    if (diagnostic_code == "invalid_candidate_metrics")
        return _L("试切指标不完整或无效");
    if (diagnostic_code == "trial_slice_executor_exception" ||
        diagnostic_code == "retry_executor_exception" || diagnostic_code == "trial_slice_exception")
        return _L("隔离试切执行失败");
    if (diagnostic_code == "trial_validation_failed")
        return _L("候选未通过 Orca 原生打印校验");
    if (diagnostic_code == "trial_result_mismatch")
        return _L("试切结果已过期或不属于当前候选");
    return _L("试切未完成");
}

wxString candidate_reason_text_impl(const SmartSlicingCandidateView& candidate)
{
    if (candidate.failed)
        return _L("试切失败：") + candidate_failure_text(candidate.diagnostic_code) +
               _L("。可单独重试；基线仍然可用。");
    if (candidate.excluded) {
        if (candidate.exclusion_reason_code == "incompatible_physical_slots")
            return _L("物理槽位与材料温区不兼容，已排除且不能应用。");
        if (candidate.exclusion_reason_code == "color_mapping_degraded")
            return _L("颜色到物理槽位的映射发生退化，已排除且不能应用。");
        return _L("候选缺少安全比较所需的有效证据，已排除且不能应用。");
    }
    wxString reason;
    if (candidate.id == "baseline") {
        reason = _L("当前正式工作区的只读基线。");
        if (candidate.recommended)
            reason += _L(" 根据真实试切指标，推荐保留当前方案。");
    } else {
        reason = candidate.recommended ? _L("推荐方案。") : _L("可选方案。");
        if (candidate.explanation == "native_arrange_stability_candidate")
            reason += _L(" 使用 Orca 原生摆盘约束生成。");
        else if (candidate.explanation == "native_auto_orientation_stability_candidate")
            reason += _L(" 使用 Orca 原生多指标自动朝向生成。");
        else if (candidate.explanation == "small_or_slender_footprint_brim_candidate")
            reason += _L(" 针对小底面或细长模型增强首层附着。");
    }
    for (const std::string& evidence : candidate.evidence_codes) {
        if (evidence == "fewer_slice_warnings")
            reason += _L(" 切片警告更少。");
        else if (evidence == "lower_estimated_time" || evidence == "shorter_print_time")
            reason += _L(" 预计时间更短。");
        else if (evidence == "lower_filament_volume" || evidence == "less_material")
            reason += _L(" 材料用量更低。");
        else if (evidence == "lower_support_volume" || evidence == "less_support_material")
            reason += _L(" 支撑用量更低。");
        else if (evidence == "less_total_material_including_multicolor_waste")
            reason += _L(" 包含冲刷和擦料塔在内的总材料更少。");
        else if (evidence == "fewer_tool_changes")
            reason += _L(" 换料次数更少。");
        else if (evidence == "lower_flush_volume")
            reason += _L(" 冲刷废料更少。");
        else if (evidence == "lower_wipe_tower_volume")
            reason += _L(" 擦料塔用料更少。");
        else if (evidence == "lower_bed_adhesion_risk")
            reason += _L(" 几何附着风险更低。");
        else if (evidence == "stronger_bed_adhesion_aid")
            reason += _L(" 对高风险底面提供了更多实际 brim 附着量。");
        else if (evidence == "more_complete_trial_evidence")
            reason += _L(" 试切证据更完整。");
    }
    return reason;
}

wxString candidate_change_summary(const SmartSlicingCandidateView& candidate)
{
    wxString summary;
    const auto append_line = [&summary](const wxString& line) {
        if (!summary.empty())
            summary += "\n";
        summary += _L("• ") + line;
    };
    if (candidate.repair_operation_count > 0) {
        wxString repair = wxString::Format(_L("网格修复：%llu 项"),
                                           static_cast<unsigned long long>(candidate.repair_operation_count));
        if (candidate.repair_changes_geometry_semantics)
            repair += _L("（会改变几何语义）");
        append_line(repair);
    }
    if (candidate.transformed_instance_count > 0) {
        const wxString action = candidate.explanation == "native_auto_orientation_stability_candidate" ?
            _L("自动朝向") : _L("方向或摆盘");
        append_line(wxString::Format(_L("%s：%llu 个实例"), action.c_str(),
                                     static_cast<unsigned long long>(candidate.transformed_instance_count)));
    }
    size_t described_plate_parameters = 0;
    if (candidate.brim_width_before && candidate.brim_width_after) {
        append_line(wxString::Format(_L("打印板参数 · 附着边宽度：%.2f mm → %.2f mm"),
                                     *candidate.brim_width_before, *candidate.brim_width_after));
        ++described_plate_parameters;
    }
    if (candidate.brim_type_before && candidate.brim_type_after) {
        append_line(_L("打印板参数 · 附着边策略：") + from_u8(*candidate.brim_type_before) + _L(" → ") +
                    from_u8(*candidate.brim_type_after));
        ++described_plate_parameters;
    }
    if (candidate.plate_parameter_change_count > described_plate_parameters)
        append_line(wxString::Format(_L("打印板参数 · 其他已校验变更：%llu 项"),
                                     static_cast<unsigned long long>(candidate.plate_parameter_change_count -
                                                                     described_plate_parameters)));
    if (candidate.object_parameter_change_count > 0)
        append_line(wxString::Format(_L("对象参数 · 已校验变更：%llu 项"),
                                     static_cast<unsigned long long>(candidate.object_parameter_change_count)));
    if (candidate.material_parameter_change_count > 0)
        append_line(wxString::Format(_L("材料参数 · 已校验变更：%llu 项"),
                                     static_cast<unsigned long long>(candidate.material_parameter_change_count)));
    if (candidate.workspace_parameter_change_count > 0)
        append_line(wxString::Format(_L("工程参数 · 已校验变更：%llu 项"),
                                     static_cast<unsigned long long>(candidate.workspace_parameter_change_count)));
    if (!candidate.warning_codes.empty())
        append_line(wxString::Format(_L("试切仍有结构化警告：%llu 项"),
                                     static_cast<unsigned long long>(candidate.warning_codes.size())));
    if (candidate.id != "baseline")
        append_line(_L("确认后：创建一次 Orca 撤销事务，应用以上变更并执行正式切片；成功后进入预览"));
    return summary;
}

} // namespace

wxString smart_slicing_summary_text(const std::string& key) { return summary_text(key); }

wxString smart_slicing_candidate_failure_text(const std::string& diagnostic_code)
{
    return candidate_failure_text(diagnostic_code);
}

wxString smart_slicing_candidate_reason_text(const SmartSlicingCandidateView& candidate)
{
    return candidate_reason_text_impl(candidate);
}

AI::SmartSlicing::CandidateGoal smart_slicing_goal_from_selection(int selection)
{
    using AI::SmartSlicing::CandidateGoal;
    switch (selection) {
    case 1: return CandidateGoal::Quality;
    case 2: return CandidateGoal::Speed;
    case 3: return CandidateGoal::MaterialSaving;
    default: return CandidateGoal::Stability;
    }
}

SmartSlicingHideAction smart_slicing_hide_action(bool shown, bool worker_running, bool can_cancel)
{
    if (shown)
        return SmartSlicingHideAction::None;
    if (worker_running)
        return SmartSlicingHideAction::RequestBackgroundCancel;
    return can_cancel ? SmartSlicingHideAction::CancelDirectly : SmartSlicingHideAction::None;
}

bool smart_slicing_workflow_command_allowed(bool worker_running) { return !worker_running; }

SmartSlicingPanel::SmartSlicingPanel(wxWindow* parent, AI::SmartSlicing::SmartSlicingCoordinator& coordinator,
                                     PrepareCandidatesFn prepare_candidates, CancelTrialFn cancel_trial,
                                     FinalizeBackgroundFn finalize_background, FocusIssueFn focus_issue)
    : wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL)
    , m_coordinator(coordinator)
    , m_prepare_candidates(std::move(prepare_candidates))
    , m_cancel_trial(std::move(cancel_trial))
    , m_finalize_background(std::move(finalize_background))
    , m_focus_issue(std::move(focus_issue))
    , m_revision_timer(this)
{
    SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
    SetScrollRate(0, FromDIP(10));
    auto* root        = new wxBoxSizer(wxVERTICAL);
    auto* title       = new wxStaticText(this, wxID_ANY, _L("智能切片"));
    wxFont title_font = title->GetFont();
    title_font.SetWeight(wxFONTWEIGHT_BOLD);
    title_font.SetPointSize(title_font.GetPointSize() + 2);
    title->SetFont(title_font);
    root->Add(title, 0, wxEXPAND | wxALL, FromDIP(16));

    m_summary = new wxStaticText(this, wxID_ANY, _L("从当前打印板开始智能切片"), wxDefaultPosition,
                                 wxDefaultSize, wxST_NO_AUTORESIZE);
    m_summary->Wrap(FromDIP(330));
    root->Add(m_summary, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

    const std::array<wxString, 4> stage_names{_L("1. 模型与材料"), _L("2. 健康与准备"), _L("3. 优化方案"), _L("4. 检查并切片")};
    for (size_t index = 0; index < stage_names.size(); ++index) {
        m_stage_labels[index] = new wxStaticText(this, wxID_ANY, stage_names[index] + _L("  等待"));
        root->Add(m_stage_labels[index], 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));
    }

    auto* goal_row = new wxBoxSizer(wxHORIZONTAL);
    goal_row->Add(new wxStaticText(this, wxID_ANY, _L("优化目标")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT,
                  FromDIP(8));
    m_goal = new wxChoice(this, wxID_ANY);
    m_goal->Append(_L("稳定打印"));
    m_goal->Append(_L("质量优先"));
    m_goal->Append(_L("速度优先"));
    m_goal->Append(_L("节省材料"));
    m_goal->SetSelection(0);
    goal_row->Add(m_goal, 1, wxEXPAND);
    root->Add(goal_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    root->Add(new wxStaticLine(this), 0, wxEXPAND | wxALL, FromDIP(16));
    m_issues = new wxStaticText(this, wxID_ANY, _L("尚未运行检查"), wxDefaultPosition, wxDefaultSize,
                                wxST_NO_AUTORESIZE);
    m_issues->Wrap(FromDIP(330));
    root->Add(m_issues, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
    for (size_t index = 0; index < m_issue_focus_buttons.size(); ++index) {
        wxButton* button = new wxButton(this, wxID_ANY, _L("定位对象"));
        button->Hide();
        button->Bind(wxEVT_BUTTON, [this, index](wxCommandEvent&) {
            if (m_focus_issue && m_issue_object_ids[index] != 0)
                m_focus_issue(m_issue_object_ids[index]);
        });
        m_issue_focus_buttons[index] = button;
        root->Add(button, 0, wxALIGN_LEFT | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));
    }

    m_p0_notice = new wxStaticText(this, wxID_ANY, _L("预检与候选试切均在隔离副本中执行。"),
                                   wxDefaultPosition, wxDefaultSize, wxST_NO_AUTORESIZE);
    m_p0_notice->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    m_p0_notice->Wrap(FromDIP(330));
    root->Add(m_p0_notice, 0, wxEXPAND | wxALL, FromDIP(16));

    m_candidate_section = new wxPanel(this);
    auto* candidate_root = new wxBoxSizer(wxVERTICAL);
    for (size_t index = 0; index < m_candidate_controls.size(); ++index) {
        CandidateControls& controls = m_candidate_controls[index];
        controls.panel = new wxPanel(m_candidate_section);
        auto* box = new wxStaticBoxSizer(wxVERTICAL, controls.panel, _L("候选方案"));
        controls.selector = new wxRadioButton(controls.panel, wxID_ANY, _L("选择此方案"), wxDefaultPosition,
                                              wxDefaultSize, index == 0 ? wxRB_GROUP : 0);
        controls.metrics = new wxStaticText(controls.panel, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                            wxST_NO_AUTORESIZE);
        controls.reason  = new wxStaticText(controls.panel, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                            wxST_NO_AUTORESIZE);
        controls.reason->Wrap(FromDIP(300));
        controls.changes = new wxStaticText(controls.panel, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                            wxST_NO_AUTORESIZE);
        controls.changes->Wrap(FromDIP(300));
        controls.details = new wxButton(controls.panel, wxID_ANY, _L("查看全部变更"));
        controls.retry = new wxButton(controls.panel, wxID_ANY, _L("重试此方案"));
        box->Add(controls.selector, 0, wxEXPAND | wxALL, FromDIP(8));
        box->Add(controls.metrics, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
        box->Add(controls.reason, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
        box->Add(controls.changes, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
        box->Add(controls.details, 0, wxALIGN_RIGHT | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
        box->Add(controls.retry, 0, wxALIGN_RIGHT | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
        controls.panel->SetSizer(box);
        candidate_root->Add(controls.panel, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
        controls.selector->Bind(wxEVT_RADIOBUTTON, [this, index](wxCommandEvent&) {
            if (smart_slicing_workflow_command_allowed(
                    m_worker_running.load(std::memory_order_acquire)) &&
                !m_candidate_ids[index].empty())
                m_coordinator.select_candidate(m_candidate_ids[index]);
        });
        controls.retry->Bind(wxEVT_BUTTON, [this, index](wxCommandEvent&) {
            if (smart_slicing_workflow_command_allowed(
                    m_worker_running.load(std::memory_order_acquire)) &&
                !m_candidate_ids[index].empty()) {
                run_in_background([this, candidate_id = m_candidate_ids[index]] {
                    m_coordinator.retry_candidate(candidate_id, true, [this] {
                        return m_cancel_requested.load(std::memory_order_acquire);
                    });
                });
            }
        });
        controls.details->Bind(wxEVT_BUTTON, [this, index](wxCommandEvent&) {
            m_candidate_details_expanded[index] = !m_candidate_details_expanded[index];
            m_candidate_controls[index].changes->Show(m_candidate_details_expanded[index]);
            m_candidate_controls[index].details->SetLabel(
                m_candidate_details_expanded[index] ? _L("收起变更") : _L("查看全部变更"));
            m_candidate_controls[index].panel->Layout();
            Layout();
            FitInside();
        });
    }
    auto* candidate_actions = new wxBoxSizer(wxHORIZONTAL);
    m_keep_baseline = new wxButton(m_candidate_section, wxID_ANY, _L("保留当前方案"));
    m_undo_apply    = new wxButton(m_candidate_section, wxID_ANY, _L("撤销本次应用"));
    m_apply         = new wxButton(m_candidate_section, wxID_ANY, _L("确认并应用"));
    candidate_actions->Add(m_keep_baseline, 0, wxRIGHT, FromDIP(8));
    candidate_actions->Add(m_undo_apply, 0, wxRIGHT, FromDIP(8));
    candidate_actions->Add(m_apply, 1, wxEXPAND);
    candidate_root->Add(candidate_actions, 0, wxEXPAND);
    m_candidate_section->SetSizer(candidate_root);
    root->Add(m_candidate_section, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    m_candidate_section->Hide();
    m_keep_baseline->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (smart_slicing_workflow_command_allowed(m_worker_running.load(std::memory_order_acquire)))
            m_coordinator.select_candidate("baseline");
    });
    m_undo_apply->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (smart_slicing_workflow_command_allowed(m_worker_running.load(std::memory_order_acquire)))
            m_coordinator.undo_applied_candidate();
    });
    m_apply->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (smart_slicing_workflow_command_allowed(m_worker_running.load(std::memory_order_acquire)))
            m_coordinator.apply_selected_candidate();
    });
    root->AddStretchSpacer();

    auto* actions = new wxBoxSizer(wxHORIZONTAL);
    m_cancel      = new wxButton(this, wxID_CANCEL, _L("取消"));
    m_start       = new wxButton(this, wxID_ANY, _L("开始检查"));
    actions->Add(m_cancel, 0, wxRIGHT, FromDIP(8));
    actions->Add(m_start, 1, wxEXPAND);
    root->Add(actions, 0, wxEXPAND | wxALL, FromDIP(16));
    SetSizer(root);

    m_start->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (!smart_slicing_workflow_command_allowed(m_worker_running.load(std::memory_order_acquire)))
            return;
        if (m_can_accept_risk) {
            m_coordinator.accept_printability_risk();
        } else if (m_can_plan_candidates) {
            const AI::SmartSlicing::CandidateGoal goal =
                smart_slicing_goal_from_selection(m_goal->GetSelection());
            CandidatePlanTask plan_task;
            if (m_prepare_candidates) {
                try {
                    plan_task = m_prepare_candidates();
                } catch (...) {
                    // Project GUI-thread capture failures through the Coordinator below.
                }
                if (!plan_task) {
                    m_coordinator.fail_candidate_preparation();
                    return;
                }
            }
            const bool started = run_in_background([this, goal, plan_task = std::move(plan_task)]() mutable {
                const CancelPredicate canceled = [this] {
                    return m_cancel_requested.load(std::memory_order_acquire);
                };
                std::vector<AI::SmartSlicing::SliceCandidate> candidates =
                    plan_task ? plan_task(canceled) : std::vector<AI::SmartSlicing::SliceCandidate>{};
                if (canceled()) {
                    m_coordinator.cancel();
                    return;
                }
                m_coordinator.plan_and_slice_candidates(std::move(candidates), goal, true, canceled);
            });
            if (started) {
                m_start->Enable(false);
                m_start->SetLabel(_L("正在生成方案…"));
            }
        } else {
            m_coordinator.start();
        }
    });
    m_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        m_cancel_requested.store(true, std::memory_order_release);
        if (m_cancel_trial)
            m_cancel_trial();
        if (!m_worker_running.load(std::memory_order_acquire))
            m_coordinator.cancel();
    });
    Bind(wxEVT_SHOW, [this](wxShowEvent& event) {
        const bool worker_running = m_worker_running.load(std::memory_order_acquire);
        const bool can_cancel = !worker_running && m_coordinator.snapshot().can_cancel();
        switch (smart_slicing_hide_action(event.IsShown(), worker_running, can_cancel)) {
        case SmartSlicingHideAction::RequestBackgroundCancel:
            m_cancel_requested.store(true, std::memory_order_release);
            if (m_cancel_trial)
                m_cancel_trial();
            break;
        case SmartSlicingHideAction::CancelDirectly:
            m_coordinator.cancel();
            break;
        case SmartSlicingHideAction::None:
            break;
        }
        event.Skip();
    });
    Bind(wxEVT_TIMER, &SmartSlicingPanel::on_revision_timer, this, m_revision_timer.GetId());
}

SmartSlicingPanel::~SmartSlicingPanel()
{
    m_revision_timer.Stop();
    m_cancel_requested.store(true, std::memory_order_release);
    if (m_worker_running.load(std::memory_order_acquire) && m_cancel_trial)
        m_cancel_trial();
    if (m_worker.joinable())
        m_worker.join();
}

bool SmartSlicingPanel::run_in_background(std::function<void()> work)
{
    if (!work || m_worker_running.exchange(true, std::memory_order_acq_rel))
        return false;
    // The worker has exclusive Coordinator ownership until it publishes its terminal state. Disable synchronously
    // so a queued GUI event cannot act on the previous view before the first background transition is rendered.
    disable_workflow_commands();
    m_cancel_requested.store(false, std::memory_order_release);
    if (m_worker.joinable())
        m_worker.join();
    const wxWeakRef<SmartSlicingPanel> weak_panel(this);
    m_worker = std::thread([this, weak_panel, work = std::move(work)] {
        try {
            work();
        } catch (...) {
            m_coordinator.cancel();
        }
        m_worker_running.store(false, std::memory_order_release);
        if (wxTheApp != nullptr) {
            wxTheApp->CallAfter([weak_panel] {
                if (!weak_panel)
                    return;
                if (weak_panel->m_cancel_requested.load(std::memory_order_acquire))
                    weak_panel->m_coordinator.cancel();
                if (weak_panel->m_finalize_background)
                    weak_panel->m_finalize_background();
            });
        }
    });
    return true;
}

void SmartSlicingPanel::disable_workflow_commands()
{
    m_goal->Enable(false);
    m_start->Enable(false);
    m_keep_baseline->Enable(false);
    m_apply->Enable(false);
    m_undo_apply->Enable(false);
    for (CandidateControls& controls : m_candidate_controls) {
        controls.selector->Enable(false);
        controls.retry->Enable(false);
    }
}

void SmartSlicingPanel::render(const SmartSlicingViewModel& view_model)
{
    static const std::array<wxString, 4> names{_L("1. 模型与材料"), _L("2. 健康与准备"), _L("3. 优化方案"), _L("4. 检查并切片")};
    set_wrapped_label(*m_summary, smart_slicing_summary_text(view_model.summary_key), FromDIP(330));
    for (size_t index = 0; index < m_stage_labels.size(); ++index)
        m_stage_labels[index]->SetLabel(names[index] + _L("  ") + status_text(view_model.stages[index].status));
    wxString issue_text;
    if (view_model.issues.empty()) {
        issue_text = _L("未发现结构化问题");
    } else {
        wxString issues = wxString::Format(_L("发现 %llu 个结构化问题"), static_cast<unsigned long long>(view_model.issue_count));
        const size_t visible_issue_count = std::min<size_t>(view_model.issues.size(), 5);
        for (size_t index = 0; index < visible_issue_count; ++index) {
            const SmartSlicingIssueView& issue = view_model.issues[index];
            issues += _L("\n• ") + issue_name(issue.code) +
                      (issue.evidence.empty() ? wxString() : wxString(": ") + from_u8(issue.evidence));
        }
        if (visible_issue_count < view_model.issues.size())
            issues += wxString::Format(_L("\n…另有 %llu 项"),
                                       static_cast<unsigned long long>(view_model.issues.size() - visible_issue_count));
        issue_text = std::move(issues);
    }
    set_wrapped_label(*m_issues, issue_text, FromDIP(330));
    for (size_t index = 0; index < m_issue_focus_buttons.size(); ++index) {
        m_issue_object_ids[index] = 0;
        m_issue_focus_buttons[index]->Hide();
    }
    size_t focus_button_index = 0;
    for (const SmartSlicingIssueView& issue : view_model.issues) {
        if (focus_button_index == m_issue_focus_buttons.size())
            break;
        if (issue.object_id == 0)
            continue;
        m_issue_object_ids[focus_button_index] = issue.object_id;
        m_issue_focus_buttons[focus_button_index]->SetLabel(_L("定位对象：") + issue_name(issue.code));
        m_issue_focus_buttons[focus_button_index]->Show();
        ++focus_button_index;
    }
    m_can_accept_risk = view_model.can_accept_risk;
    m_can_plan_candidates = view_model.can_plan_candidates;
    const bool commands_allowed = smart_slicing_workflow_command_allowed(
        m_worker_running.load(std::memory_order_acquire));
    m_goal->Enable(view_model.can_plan_candidates && commands_allowed);
    m_start->Enable((view_model.can_start || view_model.can_plan_candidates || view_model.can_accept_risk) &&
                    commands_allowed);
    m_start->SetLabel(view_model.can_accept_risk ? _L("保留当前网格并继续") :
                      view_model.can_plan_candidates ? _L("生成并试切方案") :
                      view_model.is_stale ? _L("重新检查") : _L("开始检查"));
    m_cancel->Enable(view_model.can_cancel);
    m_cancel->SetLabel(view_model.can_accept_risk ? _L("先修复模型") : _L("取消"));

    const bool show_candidates = !view_model.candidates.empty();
    m_candidate_section->Show(show_candidates);
    for (size_t index = 0; index < m_candidate_controls.size(); ++index) {
        CandidateControls& controls = m_candidate_controls[index];
        const bool visible = index < view_model.candidates.size();
        controls.panel->Show(visible);
        const std::string previous_candidate_id = m_candidate_ids[index];
        m_candidate_ids[index].clear();
        if (!visible)
            continue;
        const SmartSlicingCandidateView& candidate = view_model.candidates[index];
        m_candidate_ids[index] = candidate.id;
        if (previous_candidate_id != candidate.id)
            m_candidate_details_expanded[index] = false;
        controls.selector->SetLabel(candidate.excluded ? _L("不可用候选") :
                                    candidate.id == "baseline" ? _L("当前方案（基线）") :
                                    candidate.recommended ? _L("推荐候选") : _L("候选方案"));
        controls.selector->SetValue(candidate.selected);
        controls.selector->Enable(candidate.can_select && commands_allowed);
        wxString metrics = _L("时间：") + format_duration(candidate.estimated_time_seconds) +
                           _L("  材料：") + format_volume(candidate.filament_volume_mm3) +
                           _L("\n支撑：") + format_volume(candidate.support_volume_mm3) +
                           _L("  Brim：") + format_volume(candidate.brim_volume_mm3) +
                           _L("  换料：") +
                           (candidate.tool_changes ?
                                wxString::Format("%llu", static_cast<unsigned long long>(*candidate.tool_changes)) :
                                _L("不可用")) +
                           _L("\n冲刷：") + format_volume(candidate.flush_volume_mm3) +
                           _L("  擦料塔：") + format_volume(candidate.wipe_tower_volume_mm3);
        if (candidate.bed_adhesion_risk_score)
            metrics += wxString::Format(_L("\n附着风险：%.2f（越低越稳）"), *candidate.bed_adhesion_risk_score);
        if (candidate.layer_tool_sequence_count > 0)
            metrics += wxString::Format(_L("\n层级工具序列：%llu 组"),
                                        static_cast<unsigned long long>(candidate.layer_tool_sequence_count));
        if (candidate.physical_slots_compatible == true)
            metrics += _L("\n物理槽位：兼容");
        else if (candidate.physical_slots_compatible == false)
            metrics += _L("\n物理槽位：不兼容");
        if (candidate.color_mapping_degraded == true)
            metrics += _L("\n颜色映射：已退化");
        else if (candidate.color_mapping_degraded == false)
            metrics += _L("\n颜色映射：保持一致");
        if (candidate.prime_tower_enabled == true)
            metrics += _L("\n擦料塔策略：启用");
        else if (candidate.prime_tower_enabled == false)
            metrics += _L("\n擦料塔策略：关闭");
        if (candidate.id != "baseline") {
            const wxString tool_delta = candidate.tool_change_delta ?
                wxString::Format("%+lld", *candidate.tool_change_delta) : _L("—");
            metrics += _L("\n相对基线：时间 ") + format_delta(candidate.time_delta_seconds, 60.0, _L("分钟")) +
                       _L("，材料 ") + format_delta(candidate.filament_delta_mm3, 1000.0, _L("cm³")) +
                       _L("，支撑 ") + format_delta(candidate.support_delta_mm3, 1000.0, _L("cm³")) +
                       _L("，Brim ") + format_delta(candidate.brim_volume_delta_mm3, 1000.0, _L("cm³")) +
                       _L("，换料 ") + tool_delta +
                       _L("，冲刷 ") + format_delta(candidate.flush_delta_mm3, 1000.0, _L("cm³")) +
                       _L("，擦料塔 ") + format_delta(candidate.wipe_tower_delta_mm3, 1000.0, _L("cm³"));
            if (candidate.bed_adhesion_risk_delta)
                metrics += wxString::Format(_L("，附着风险 %+.2f"), *candidate.bed_adhesion_risk_delta);
        }
        set_wrapped_label(*controls.metrics, metrics, FromDIP(300));
        set_wrapped_label(*controls.reason, smart_slicing_candidate_reason_text(candidate), FromDIP(300));
        const wxString changes = candidate_change_summary(candidate);
        set_wrapped_label(*controls.changes, changes, FromDIP(300));
        controls.changes->Show(!changes.empty() && m_candidate_details_expanded[index]);
        controls.details->SetLabel(m_candidate_details_expanded[index] ? _L("收起变更") : _L("查看全部变更"));
        controls.details->Show(candidate.id != "baseline" && !changes.empty());
        controls.retry->Show(candidate.can_retry);
        controls.retry->Enable(candidate.can_retry && commands_allowed);
    }
    m_keep_baseline->Enable(std::any_of(view_model.candidates.begin(), view_model.candidates.end(),
                                       [](const SmartSlicingCandidateView& candidate) {
                                           return candidate.id == "baseline" && !candidate.selected && candidate.can_select;
                                       }) && commands_allowed);
    m_apply->Enable(view_model.can_apply && commands_allowed);
    m_undo_apply->Show(view_model.can_undo_apply);
    m_undo_apply->Enable(view_model.can_undo_apply && commands_allowed);
    set_wrapped_label(*m_p0_notice,
                      show_candidates ? _L("确认前不会修改正式模型、配置或正式切片结果。") :
                                        _L("预检与候选试切均在隔离副本中执行。"),
                      FromDIP(330));
    if ((view_model.can_cancel || view_model.needs_polling) && !m_revision_timer.IsRunning())
        m_revision_timer.Start(1000);
    else if (!view_model.can_cancel && !view_model.needs_polling && m_revision_timer.IsRunning())
        m_revision_timer.Stop();
    Layout();
    FitInside();
}

void SmartSlicingPanel::on_revision_timer(wxTimerEvent&)
{
    if (!m_worker_running.load(std::memory_order_acquire))
        m_coordinator.refresh_revision();
}

} // namespace Slic3r::GUI
