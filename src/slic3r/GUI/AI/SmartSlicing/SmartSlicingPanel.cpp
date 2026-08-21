#include "SmartSlicingPanel.hpp"

#include "slic3r/AI/SmartSlicing/Application/SmartSlicingCoordinator.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <wx/button.h>
#include <wx/radiobut.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/statline.h>
#include <wx/stattext.h>

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
    if (key == "canceling")
        return _L("正在取消…");
    if (key == "canceled")
        return _L("检查已取消");
    if (key == "workspace_changed")
        return _L("工程已变化，需要重新检查");
    if (key == "preflight_failed")
        return _L("检查失败，请重试");
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

wxString format_volume(const std::optional<double>& volume_mm3)
{
    return volume_mm3 ? wxString::Format("%.2f cm³", *volume_mm3 / 1000.0) : _L("不可用");
}

wxString format_delta(const std::optional<double>& value, double scale, const wxString& unit)
{
    return value ? wxString::Format("%+.2f %s", *value / scale, unit.c_str()) : _L("—");
}

wxString candidate_reason(const SmartSlicingCandidateView& candidate)
{
    if (candidate.failed)
        return _L("试切失败，可单独重试；基线仍然可用。");
    if (candidate.id == "baseline")
        return _L("当前正式工作区的只读基线。");
    wxString reason = candidate.recommended ? _L("推荐方案。") : _L("可选方案。");
    for (const std::string& evidence : candidate.evidence_codes) {
        if (evidence == "fewer_slice_warnings")
            reason += _L(" 切片警告更少。");
        else if (evidence == "lower_estimated_time")
            reason += _L(" 预计时间更短。");
        else if (evidence == "lower_filament_volume")
            reason += _L(" 材料用量更低。");
        else if (evidence == "lower_support_volume")
            reason += _L(" 支撑用量更低。");
    }
    return reason;
}

} // namespace

SmartSlicingPanel::SmartSlicingPanel(wxWindow* parent, AI::SmartSlicing::SmartSlicingCoordinator& coordinator,
                                     PlanCandidatesFn plan_candidates)
    : wxPanel(parent)
    , m_coordinator(coordinator)
    , m_plan_candidates(std::move(plan_candidates))
    , m_revision_timer(this)
{
    SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
    auto* root        = new wxBoxSizer(wxVERTICAL);
    auto* title       = new wxStaticText(this, wxID_ANY, _L("智能切片"));
    wxFont title_font = title->GetFont();
    title_font.SetWeight(wxFONTWEIGHT_BOLD);
    title_font.SetPointSize(title_font.GetPointSize() + 2);
    title->SetFont(title_font);
    root->Add(title, 0, wxEXPAND | wxALL, FromDIP(16));

    m_summary = new wxStaticText(this, wxID_ANY, _L("从当前打印板开始智能切片"));
    m_summary->Wrap(FromDIP(330));
    root->Add(m_summary, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

    const std::array<wxString, 4> stage_names{_L("1. 模型与材料"), _L("2. 健康与准备"), _L("3. 优化方案"), _L("4. 检查并切片")};
    for (size_t index = 0; index < stage_names.size(); ++index) {
        m_stage_labels[index] = new wxStaticText(this, wxID_ANY, stage_names[index] + _L("  等待"));
        root->Add(m_stage_labels[index], 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));
    }

    root->Add(new wxStaticLine(this), 0, wxEXPAND | wxALL, FromDIP(16));
    m_issues = new wxStaticText(this, wxID_ANY, _L("尚未运行检查"));
    m_issues->Wrap(FromDIP(330));
    root->Add(m_issues, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));

    m_p0_notice = new wxStaticText(this, wxID_ANY, _L("预检与候选试切均在隔离副本中执行。"));
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
        controls.metrics = new wxStaticText(controls.panel, wxID_ANY, "");
        controls.reason  = new wxStaticText(controls.panel, wxID_ANY, "");
        controls.reason->Wrap(FromDIP(300));
        controls.retry = new wxButton(controls.panel, wxID_ANY, _L("重试此方案"));
        box->Add(controls.selector, 0, wxEXPAND | wxALL, FromDIP(8));
        box->Add(controls.metrics, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
        box->Add(controls.reason, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
        box->Add(controls.retry, 0, wxALIGN_RIGHT | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
        controls.panel->SetSizer(box);
        candidate_root->Add(controls.panel, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
        controls.selector->Bind(wxEVT_RADIOBUTTON, [this, index](wxCommandEvent&) {
            if (!m_candidate_ids[index].empty())
                m_coordinator.select_candidate(m_candidate_ids[index]);
        });
        controls.retry->Bind(wxEVT_BUTTON, [this, index](wxCommandEvent&) {
            if (!m_candidate_ids[index].empty())
                m_coordinator.retry_candidate(m_candidate_ids[index]);
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
    m_keep_baseline->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_coordinator.select_candidate("baseline"); });
    m_undo_apply->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_coordinator.undo_applied_candidate(); });
    m_apply->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_coordinator.apply_selected_candidate(); });
    root->AddStretchSpacer();

    auto* actions = new wxBoxSizer(wxHORIZONTAL);
    m_cancel      = new wxButton(this, wxID_CANCEL, _L("取消"));
    m_start       = new wxButton(this, wxID_ANY, _L("开始检查"));
    actions->Add(m_cancel, 0, wxRIGHT, FromDIP(8));
    actions->Add(m_start, 1, wxEXPAND);
    root->Add(actions, 0, wxEXPAND | wxALL, FromDIP(16));
    SetSizer(root);

    m_start->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_can_plan_candidates)
            m_coordinator.plan_and_slice_candidates(m_plan_candidates ? m_plan_candidates() :
                                                                         std::vector<AI::SmartSlicing::SliceCandidate>{});
        else
            m_coordinator.start();
    });
    m_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_coordinator.cancel(); });
    Bind(wxEVT_TIMER, &SmartSlicingPanel::on_revision_timer, this, m_revision_timer.GetId());
}

void SmartSlicingPanel::render(const SmartSlicingViewModel& view_model)
{
    static const std::array<wxString, 4> names{_L("1. 模型与材料"), _L("2. 健康与准备"), _L("3. 优化方案"), _L("4. 检查并切片")};
    m_summary->SetLabel(summary_text(view_model.summary_key));
    for (size_t index = 0; index < m_stage_labels.size(); ++index)
        m_stage_labels[index]->SetLabel(names[index] + _L("  ") + status_text(view_model.stages[index].status));
    if (view_model.issues.empty()) {
        m_issues->SetLabel(_L("未发现结构化问题"));
    } else {
        wxString issues = wxString::Format(_L("发现 %llu 个结构化问题"), static_cast<unsigned long long>(view_model.issue_count));
        const size_t visible_issue_count = std::min<size_t>(view_model.issues.size(), 5);
        for (size_t index = 0; index < visible_issue_count; ++index) {
            const auto& [code, evidence] = view_model.issues[index];
            issues += _L("\n• ") + issue_name(code) + (evidence.empty() ? wxString() : wxString(": ") + from_u8(evidence));
        }
        if (visible_issue_count < view_model.issues.size())
            issues += wxString::Format(_L("\n…另有 %llu 项"),
                                       static_cast<unsigned long long>(view_model.issues.size() - visible_issue_count));
        m_issues->SetLabel(issues);
        m_issues->Wrap(FromDIP(330));
    }
    m_can_plan_candidates = view_model.can_plan_candidates;
    m_start->Enable(view_model.can_start || view_model.can_plan_candidates);
    m_start->SetLabel(view_model.can_plan_candidates ? _L("生成并试切方案") :
                      view_model.is_stale ? _L("重新检查") : _L("开始检查"));
    m_cancel->Enable(view_model.can_cancel);

    const bool show_candidates = !view_model.candidates.empty();
    m_candidate_section->Show(show_candidates);
    for (size_t index = 0; index < m_candidate_controls.size(); ++index) {
        CandidateControls& controls = m_candidate_controls[index];
        const bool visible = index < view_model.candidates.size();
        controls.panel->Show(visible);
        m_candidate_ids[index].clear();
        if (!visible)
            continue;
        const SmartSlicingCandidateView& candidate = view_model.candidates[index];
        m_candidate_ids[index] = candidate.id;
        controls.selector->SetLabel(candidate.id == "baseline" ? _L("当前方案（基线）") :
                                    candidate.recommended ? _L("推荐候选") : _L("候选方案"));
        controls.selector->SetValue(candidate.selected);
        controls.selector->Enable(!candidate.failed);
        wxString metrics = _L("时间：") + format_duration(candidate.estimated_time_seconds) +
                           _L("  材料：") + format_volume(candidate.filament_volume_mm3) +
                           _L("\n支撑：") + format_volume(candidate.support_volume_mm3) +
                           _L("  换料：") +
                           (candidate.tool_changes ?
                                wxString::Format("%llu", static_cast<unsigned long long>(*candidate.tool_changes)) :
                                _L("不可用"));
        if (candidate.id != "baseline") {
            const wxString tool_delta = candidate.tool_change_delta ?
                wxString::Format("%+lld", *candidate.tool_change_delta) : _L("—");
            metrics += _L("\n相对基线：时间 ") + format_delta(candidate.time_delta_seconds, 60.0, _L("分钟")) +
                       _L("，材料 ") + format_delta(candidate.filament_delta_mm3, 1000.0, _L("cm³")) +
                       _L("，支撑 ") + format_delta(candidate.support_delta_mm3, 1000.0, _L("cm³")) +
                       _L("，换料 ") + tool_delta;
        }
        controls.metrics->SetLabel(metrics);
        controls.reason->SetLabel(candidate_reason(candidate));
        controls.retry->Show(candidate.can_retry);
    }
    m_keep_baseline->Enable(std::any_of(view_model.candidates.begin(), view_model.candidates.end(),
                                       [](const SmartSlicingCandidateView& candidate) {
                                           return candidate.id == "baseline" && !candidate.selected && !candidate.failed;
                                       }));
    m_apply->Enable(view_model.can_apply);
    m_undo_apply->Show(view_model.can_undo_apply);
    m_undo_apply->Enable(view_model.can_undo_apply);
    m_p0_notice->SetLabel(show_candidates ? _L("确认前不会修改正式模型、配置或正式切片结果。") :
                                            _L("预检与候选试切均在隔离副本中执行。"));
    if ((view_model.can_cancel || view_model.needs_polling) && !m_revision_timer.IsRunning())
        m_revision_timer.Start(1000);
    else if (!view_model.can_cancel && !view_model.needs_polling && m_revision_timer.IsRunning())
        m_revision_timer.Stop();
    Layout();
}

void SmartSlicingPanel::on_revision_timer(wxTimerEvent&) { m_coordinator.refresh_revision(); }

} // namespace Slic3r::GUI
