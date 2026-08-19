#include "SmartSlicingPanel.hpp"

#include "slic3r/AI/SmartSlicing/Application/SmartSlicingCoordinator.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"

#include <algorithm>
#include <wx/button.h>
#include <wx/settings.h>
#include <wx/sizer.h>
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
    case SmartSlicingStageStatus::Disabled: return _L("P1 启用");
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

} // namespace

SmartSlicingPanel::SmartSlicingPanel(wxWindow* parent, AI::SmartSlicing::SmartSlicingCoordinator& coordinator)
    : wxPanel(parent), m_coordinator(coordinator), m_revision_timer(this)
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

    m_p0_notice = new wxStaticText(this, wxID_ANY, _L("P0 仅执行只读预检；候选比较与事务应用将在 P1 启用。"));
    m_p0_notice->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    m_p0_notice->Wrap(FromDIP(330));
    root->Add(m_p0_notice, 0, wxEXPAND | wxALL, FromDIP(16));
    root->AddStretchSpacer();

    auto* actions = new wxBoxSizer(wxHORIZONTAL);
    m_cancel      = new wxButton(this, wxID_CANCEL, _L("取消"));
    m_start       = new wxButton(this, wxID_ANY, _L("开始检查"));
    actions->Add(m_cancel, 0, wxRIGHT, FromDIP(8));
    actions->Add(m_start, 1, wxEXPAND);
    root->Add(actions, 0, wxEXPAND | wxALL, FromDIP(16));
    SetSizer(root);

    m_start->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_coordinator.start(); });
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
    m_start->Enable(view_model.can_start);
    m_start->SetLabel(view_model.is_stale ? _L("重新检查") : _L("开始检查"));
    m_cancel->Enable(view_model.can_cancel);
    if (view_model.can_cancel && !m_revision_timer.IsRunning())
        m_revision_timer.Start(1000);
    else if (!view_model.can_cancel && m_revision_timer.IsRunning())
        m_revision_timer.Stop();
    Layout();
}

void SmartSlicingPanel::on_revision_timer(wxTimerEvent&) { m_coordinator.refresh_revision(); }

} // namespace Slic3r::GUI
