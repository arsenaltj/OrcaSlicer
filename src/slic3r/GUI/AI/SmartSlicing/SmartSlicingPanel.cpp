#include "SmartSlicingPanel.hpp"
#include "slic3r/GUI/AI/AIWindowAppearance.hpp"
#include "slic3r/GUI/AI/Orca/OrcaModelPreparationPanel.hpp"
#include "slic3r/GUI/AI/Orca/OrcaPrintConfirmation.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelGenerationInputStyle.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include <wx/statbmp.h>

#include "slic3r/AI/SmartSlicing/Application/SmartSlicingCoordinator.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <wx/button.h>
#include <wx/choice.h>
#include <wx/collpane.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <sstream>
#include <type_traits>

namespace Slic3r::GUI {
namespace {

Label* wrapped_label(wxWindow* parent, const wxString& text)
{
    auto* label = new Label(parent, text, LB_AUTO_WRAP);
    label->SetMinSize(wxSize(1, -1));
    return label;
}

Button* action_button(wxWindow* parent, const wxString& label, bool primary = false)
{
    auto* button = new Button(parent, label);
    button->SetName(primary ? "input_primary" : "input_field");
    button->SetPaddingSize(parent->FromDIP(wxSize(12, 8)));
    button->SetMinSize(parent->FromDIP(wxSize(-1, 36)));
    return button;
}

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
    if (key == "applied_revision_unavailable")
        return _L("无法核实结果对应的工程版本，请重新检查。撤销请使用 Orca 历史。");
    if (key == "apply_undo_unavailable")
        return _L("本次应用已无法单独撤销。请查看 Orca 撤销历史，或重新检查当前工程。");
    if (key == "apply_undo_failed")
        return _L("撤销未完成，请重试或查看 Orca 撤销历史。");
    if (key == "close_active_model_tool")
        return _L("请先结束当前模型编辑工具，再重新检查并应用方案。");
    if (key == "baseline_trial_failed")
        return _L("当前方案试切失败，请先处理原生切片提示");
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
        return _L("原生配置校验待正式切片");
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
    return volume_mm3 ? wxString::Format(wxString::FromUTF8("%.2f cm³"), *volume_mm3 / 1000.0) : _L("不可用");
}

wxString format_delta(const std::optional<double>& value, double scale, const wxString& unit)
{
    return value ? wxString::Format("%+.2f %s", *value / scale, unit.c_str()) : _L("—");
}

wxString candidate_name(const SmartSlicingCandidateView& candidate)
{
    if (candidate.id == "baseline")
        return _L("当前方案");
    if (candidate.explanation == "layer_height_balanced_candidate")
        return _L("均衡方案");
    if (candidate.explanation == "layer_height_speed_candidate")
        return _L("速度优先");
    if (candidate.explanation == "layer_height_quality_candidate")
        return _L("质量优先");
    if (candidate.explanation == "small_or_slender_footprint_brim_candidate")
        return _L("稳定性方案");
    if (candidate.placement_change_count > 0)
        return _L("排布方案");
    return candidate.recommended ? _L("推荐方案") : _L("候选方案");
}

bool is_priority_profile(const SmartSlicingCandidateView& candidate)
{
    return candidate.explanation == "layer_height_balanced_candidate" ||
        candidate.explanation == "layer_height_speed_candidate" ||
        candidate.explanation == "layer_height_quality_candidate";
}

wxString candidate_secondary(const SmartSlicingCandidateView& candidate)
{
    if (candidate.failed) return _L("试切失败");
    for (const auto& entry : candidate.parameter_changes)
        if (entry.key == "layer_height")
            if (const auto* value = std::get_if<double>(&entry.new_value))
                return wxString::Format("%.2f mm", *value);
    return is_priority_profile(candidate) ? _L("当前层高") : format_duration(candidate.estimated_time_seconds);
}

void style_candidate(Button* button, bool update_fonts = false)
{
    ModelGenerationInputStyle::apply_control(button, ModelGenerationInputStyle::Role::QuietAction, update_fonts, true);
    const wxColour fill(78, 78, 81);
    button->SetBorderColor(StateColor(
        std::make_pair(ModelGenerationInputStyle::yellow, int(StateColor::Focused)),
        std::make_pair(button->IsSelected() ? wxColour(255, 194, 39) : fill, int(StateColor::Normal))));
}

wxString candidate_reason(const SmartSlicingCandidateView& candidate)
{
    if (candidate.failed) {
        if (candidate.id == "baseline")
            return _L("当前方案试切失败，暂不可应用。请检查原生设置后重新检查。");
        return candidate.can_retry ? _L("试切失败，可单独重试此方案。") :
                                     _L("试切失败，暂不可应用。");
    }
    if (candidate.id == "baseline")
        return _L("当前正式工作区的只读基线。");
    wxString reason = candidate.recommended ? _L("推荐方案。") : _L("可选方案。");
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
    }
    return reason;
}

wxString candidate_changes(const SmartSlicingCandidateView& candidate)
{
    using namespace AI::SmartSlicing;
    auto value_text = [](const ConfigValue& value) {
        return std::visit([](const auto& item) -> wxString {
            if constexpr (std::is_same_v<std::decay_t<decltype(item)>, bool>)
                return item ? _L("开启") : _L("关闭");
            else {
                std::ostringstream text;
                text << item;
                return from_u8(text.str());
            }
        }, value);
    };
    wxString text;
    if (candidate.explanation == "layer_height_balanced_candidate")
        text = _L("按当前喷嘴选择中等层高，兼顾层纹细度与预计时间。\n");
    else if (candidate.explanation == "layer_height_speed_candidate")
        text = _L("按当前喷嘴限制选择较厚层高，减少层数；请比较真实试切时间。\n");
    else if (candidate.explanation == "layer_height_quality_candidate")
        text = _L("按当前喷嘴限制选择较薄层高，细化层纹；不代表实物质量已验证。\n");
    else if (candidate.explanation == "small_or_slender_footprint_brim_candidate")
        text = _L("增加裙边宽度，改善小底面或细高模型的附着。\n");
    else if (candidate.placement_change_count > 0)
        text = _L("调整模型摆放；请结合试切时间与支撑用量选择。\n");
    for (const auto& entry : candidate.parameter_changes) {
        const wxString name = entry.key == "brim_width" ? _L("裙边宽度（mm）") :
            entry.key == "layer_height" ? _L("层高（mm）") : from_u8(entry.key);
        const wxString scope = entry.scope == ConfigScope::Plate ? _L("当前打印板") :
            entry.scope == ConfigScope::Object ? _L("对象") :
            entry.scope == ConfigScope::Material ? _L("材料") : _L("工程");
        text += scope + _L(" · ") + name + ": " + value_text(entry.expected_value) +
                _L(" 改为 ") + value_text(entry.new_value) + "\n";
    }
    if (candidate.placement_change_count > 0)
        text += wxString::Format(_L("摆放方案涉及 %llu 个模型实例\n"),
            static_cast<unsigned long long>(candidate.placement_change_count));
    return text;
}

wxString issue_action(const std::string& code)
{
    if (code == "empty_plate") return _L("请先添加模型，再重新检查。");
    if (code == "missing_printer" || code == "missing_process" || code == "missing_material")
        return _L("请打开 Orca 原生设置，选择打印机、工艺和材料，再重新检查。");
    if (code == "outside_build_volume") return _L("请移动或缩小模型，使其位于打印板范围内。");
    if (code == "open_mesh") return _L("请使用模型修复工具处理开放边，再重新检查。");
    if (code == "native_validation_unavailable")
        return _L("当前打印板尚无有效正式切片；可继续比较方案，正式切片完成后复核配置。");
    return _L("请查看准备页的模型和配置提示，处理后重新检查；诊断详情可展开查看。");
}

} // namespace

SmartSlicingPanel::SmartSlicingPanel(wxWindow* parent, AI::SmartSlicing::SmartSlicingCoordinator& coordinator,
                                     PlanCandidatesFn plan_candidates, CancelTrialFn cancel_trial,
                                     std::function<void()> add_model, Plater* plater,
                                     ExecuteTrialFn execute_trial, OwnerDispatchFn owner_dispatch,
                                     ModeChangedFn mode_changed, PurposeChangedFn purpose_changed,
                                     std::function<void()> native_settings)
    : wxPanel(parent, wxID_ANY)
    , m_coordinator(coordinator)
    , m_plan_candidates(std::move(plan_candidates))
    , m_cancel_trial(std::move(cancel_trial))
    , m_execute_trial(std::move(execute_trial))
    , m_owner_dispatch(std::move(owner_dispatch))
    , m_mode_changed(std::move(mode_changed))
    , m_purpose_changed(std::move(purpose_changed))
    , m_revision_timer(this)
{
    if (!m_owner_dispatch) {
        m_owner_dispatch = [](std::function<void()> callback) {
            if (wxIsMainThread())
                callback();
            else
                wxGetApp().CallAfter(std::move(callback));
        };
    }
    m_content = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxTAB_TRAVERSAL);
    m_content->SetMinSize(wxSize(1, 1));
    m_content->SetScrollRate(0, FromDIP(12));
    m_footer = new wxPanel(this);
    SetBackgroundColour(ModelGenerationInputStyle::panel);
    auto* root        = new wxBoxSizer(wxVERTICAL);
    auto* title       = new wxStaticText(m_content, wxID_ANY, _L("智能切片"));
    wxFont title_font = title->GetFont();
    title_font.SetWeight(wxFONTWEIGHT_BOLD);
    title_font.SetPointSize(title_font.GetPointSize() + 2);
    title->SetFont(title_font);
    root->Add(title, 0, wxEXPAND | wxALL, FromDIP(16));

    auto* selector_sizer = new wxFlexGridSizer(2, 2, FromDIP(8), FromDIP(8));
    selector_sizer->Add(new wxStaticText(m_content, wxID_ANY, _L("模式")), 0, wxALIGN_CENTER_VERTICAL);
    m_mode_choice = new wxChoice(m_content, wxID_ANY);
    m_mode_choice->Append(_L("AI 智能切片"));
    m_mode_choice->Append(_L("Orca 原生"));
    m_mode_choice->SetSelection(0);
    selector_sizer->Add(m_mode_choice, 1, wxEXPAND);
    selector_sizer->Add(new wxStaticText(m_content, wxID_ANY, _L("用途")), 0, wxALIGN_CENTER_VERTICAL);
    m_purpose_choice = new wxChoice(m_content, wxID_ANY);
    m_purpose_choice->Append(_L("装饰"));
    m_purpose_choice->Append(_L("通用"));
    m_purpose_choice->Append(_L("功能"));
    m_purpose_choice->SetSelection(1);
    selector_sizer->Add(m_purpose_choice, 1, wxEXPAND);
    selector_sizer->AddGrowableCol(1, 1);
    root->Add(selector_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

    m_mode_label = new wxStaticText(m_content, wxID_ANY, _L("模式：AI 智能切片"));
    m_purpose_label = new wxStaticText(m_content, wxID_ANY, _L("用途：通用"));
    m_baseline_label = new wxStaticText(m_content, wxID_ANY, _L("Orca 原生基线：尚未捕获"));
    for (wxStaticText* label : {m_mode_label, m_purpose_label, m_baseline_label}) {
        label->Wrap(FromDIP(330));
        root->Add(label, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    }
    for (size_t index = 0; index < m_goal_cards.size(); ++index) {
        m_goal_cards[index] = new wxStaticText(m_content, wxID_ANY, _L("目标分析中"));
        m_goal_cards[index]->Wrap(FromDIP(330));
        root->Add(m_goal_cards[index], 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    }
    m_mode_choice->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        if (m_mode_changed)
            m_mode_changed(m_mode_choice->GetSelection() == 1 ? SmartSlicingMode::Orca : SmartSlicingMode::AI);
    });
    m_purpose_choice->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        if (!m_purpose_changed)
            return;
        switch (m_purpose_choice->GetSelection()) {
        case 0: m_purpose_changed(SmartSlicingPurpose::Decoration); break;
        case 2: m_purpose_changed(SmartSlicingPurpose::Functional); break;
        case 1: m_purpose_changed(SmartSlicingPurpose::General); break;
        default: m_purpose_changed(SmartSlicingPurpose::General); break;
        }
    });

    if (plater != nullptr) {
        auto* toggle = action_button(m_content, _L("展开尺寸与底座（可选）"));
        auto* preparation = new OrcaModelPreparationPanel(m_content, *plater,
            [this] { return m_worker_running.load(std::memory_order_acquire); },
            [this] { m_coordinator.refresh_revision(); },
            [this, toggle] {
                toggle->SetLabel(_L("收起尺寸与底座"));
                m_content->Layout(); m_content->FitInside(); Layout();
            });
        m_preparation = preparation;
        root->Add(toggle, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
        root->Add(preparation, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
        preparation->Hide();
        toggle->Bind(wxEVT_BUTTON, [this, toggle, preparation](wxCommandEvent&) {
            const bool expand = !preparation->IsShown();
            preparation->Show(expand);
            if (expand) preparation->refresh_selection();
            toggle->SetLabel(expand ? _L("收起尺寸与底座") : _L("展开尺寸与底座（可选）"));
            m_content->Layout(); m_content->FitInside(); Layout();
        });
    }

    m_summary = wrapped_label(m_content, _L("从当前打印板开始智能切片"));
    root->Add(m_summary, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

    const std::array<wxString, 4> stage_names{_L("1. 模型与材料"), _L("2. 健康与准备"), _L("3. 优化方案"), _L("4. 检查并切片")};
    for (size_t index = 0; index < stage_names.size(); ++index) {
        m_stage_labels[index] = new wxStaticText(m_content, wxID_ANY, stage_names[index] + _L("  等待"));
        root->Add(m_stage_labels[index], 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));
    }

    root->Add(new wxStaticLine(m_content), 0, wxEXPAND | wxALL, FromDIP(16));
    m_issues = wrapped_label(m_content, _L("尚未运行检查"));
    root->Add(m_issues, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
    m_add_model = action_button(m_content, _L("添加模型…"));
    root->Add(m_add_model, 0, wxEXPAND | wxALL, FromDIP(16));
    m_add_model->Bind(wxEVT_BUTTON, [this, add_model](wxCommandEvent&) {
        if (m_can_recheck) m_coordinator.cancel();
        if (add_model) add_model();
    });
    m_diagnostics = new wxPanel(m_content);
    auto* diagnostic_toggle = action_button(m_diagnostics, _L("展开诊断详情"));
    m_diagnostic_text = wrapped_label(m_diagnostics, "");
    auto* diagnostic_sizer = new wxBoxSizer(wxVERTICAL);
    diagnostic_sizer->Add(diagnostic_toggle, 0, wxEXPAND);
    diagnostic_sizer->Add(m_diagnostic_text, 0, wxEXPAND | wxTOP, FromDIP(8));
    m_diagnostics->SetSizer(diagnostic_sizer);
    m_diagnostic_text->Hide();
    root->Add(m_diagnostics, 0, wxEXPAND | wxALL, FromDIP(16));
    diagnostic_toggle->Bind(wxEVT_BUTTON, [this, diagnostic_toggle](wxCommandEvent&) {
        const bool expand = !m_diagnostic_text->IsShown();
        m_diagnostic_text->Show(expand);
        diagnostic_toggle->SetLabel(expand ? _L("收起诊断详情") : _L("展开诊断详情"));
        m_diagnostics->Layout(); m_content->Layout(); m_content->FitInside(); Layout();
    });

    m_p0_notice = wrapped_label(m_content, _L("预检与候选试切均在隔离副本中执行。"));
    m_p0_notice->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    m_p0_notice->SetName("input_secondary");
    root->Add(m_p0_notice, 0, wxEXPAND | wxALL, FromDIP(16));

    m_candidate_section = new wxPanel(m_content);
    auto* candidate_root = new wxBoxSizer(wxVERTICAL);
    auto* candidate_grid = new wxFlexGridSizer(2, FromDIP(12), FromDIP(12));
    candidate_grid->AddGrowableCol(0);
    candidate_grid->AddGrowableCol(1);
    for (size_t index = 0; index < m_candidate_controls.size(); ++index) {
        CandidateControls& controls = m_candidate_controls[index];
        controls.panel = new wxPanel(m_candidate_section);
        auto* box = new wxBoxSizer(wxVERTICAL);
        controls.selector = new Button(controls.panel, "", "figma-ux/slice-candidate", 0, 24);
        controls.selector->SetName("input_quiet");
        controls.selector->SetPaddingSize(FromDIP(wxSize(6, 12)));
        controls.selector->SetMinSize(FromDIP(wxSize(128, 64)));
        controls.selector->SetMaxSize(FromDIP(wxSize(168, -1)));
        controls.retry = action_button(controls.panel, _L("重试此方案"));
        box->Add(controls.selector, 0, wxEXPAND);
        box->Add(controls.retry, 0, wxEXPAND | wxTOP, FromDIP(6));
        controls.panel->SetSizer(box);
        candidate_grid->Add(controls.panel, 1, wxEXPAND);
        controls.selector->Bind(wxEVT_BUTTON, [this, index](wxCommandEvent&) {
            if (!m_candidate_ids[index].empty())
                m_coordinator.select_candidate(m_candidate_ids[index]);
        });
        controls.retry->Bind(wxEVT_BUTTON, [this, index](wxCommandEvent&) {
            if (!m_candidate_ids[index].empty() &&
                !run_trial_task(m_coordinator.begin_candidate_retry(m_candidate_ids[index])))
                m_coordinator.cancel();
        });
    }
    candidate_root->Add(candidate_grid, 0, wxEXPAND);
    m_candidate_hint = wrapped_label(m_candidate_section, "");
    m_candidate_hint->SetName("input_secondary");
    candidate_root->Add(m_candidate_hint, 0, wxEXPAND | wxTOP, FromDIP(12));
    m_candidate_details = new ModelGenerationInputStyle::RoundedPanel(m_candidate_section);
    auto* details = new wxBoxSizer(wxVERTICAL);
    m_candidate_title = wrapped_label(m_candidate_details, "");
    m_candidate_metrics = wrapped_label(m_candidate_details, "");
    m_candidate_reason = wrapped_label(m_candidate_details, "");
    details->Add(m_candidate_title, 0, wxEXPAND | wxALL, FromDIP(12));
    details->Add(m_candidate_metrics, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    details->Add(m_candidate_reason, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    m_candidate_details->SetSizer(details);
    candidate_root->Add(m_candidate_details, 0, wxEXPAND | wxTOP, FromDIP(12));
    auto* footer = new wxBoxSizer(wxVERTICAL);
    auto* candidate_actions = new wxBoxSizer(wxHORIZONTAL);
    m_keep_baseline = action_button(m_footer, _L("保留当前方案"));
    m_undo_apply    = action_button(m_footer, _L("撤销本次应用"));
    m_apply         = action_button(m_footer, _L("应用并切片"), true);
    candidate_actions->Add(m_keep_baseline, 1, wxEXPAND | wxRIGHT, FromDIP(8));
    footer->Add(m_undo_apply, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    candidate_actions->Add(m_apply, 1, wxEXPAND);
    footer->Add(candidate_actions, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    m_candidate_section->SetSizer(candidate_root);
    root->Add(m_candidate_section, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    m_candidate_section->Hide();
    m_keep_baseline->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_coordinator.select_candidate("baseline"); });
    m_undo_apply->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_coordinator.undo_applied_candidate(); });
    m_apply->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_coordinator.apply_selected_candidate(); });

    auto* actions = new wxBoxSizer(wxHORIZONTAL);
    m_cancel      = action_button(m_footer, _L("取消"));
    m_start       = action_button(m_footer, _L("开始检查"), true);
    actions->Add(m_cancel, 0, wxRIGHT, FromDIP(8));
    actions->Add(m_start, 1, wxEXPAND);
    footer->Add(actions, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    if (native_settings) {
        auto* native_row = new ModelGenerationInputStyle::RoundedPanel(m_footer);
        auto* native_sizer = new wxBoxSizer(wxHORIZONTAL);
        auto* native = action_button(native_row, _L("Orca 原生设置"));
        auto* arrow = new wxStaticBitmap(native_row, wxID_ANY,
            create_scaled_bitmap("figma-ux/native-settings-arrow", native_row, 24));
        native_sizer->Add(native, 1, wxEXPAND);
        native_sizer->Add(arrow, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        native_row->SetSizer(native_sizer);
        native->Bind(wxEVT_BUTTON, [native_settings](wxCommandEvent&) { native_settings(); });
        for (wxWindow* target : std::array<wxWindow*, 2>{native_row, arrow})
            target->Bind(wxEVT_LEFT_UP, [native_settings](wxMouseEvent&) { native_settings(); });
        footer->Add(native_row, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    }
    if (plater) {
        m_confirm = action_button(m_footer, _L("查看打印确认…"));
        footer->Add(m_confirm, 0, wxEXPAND);
        m_confirm->Bind(wxEVT_BUTTON, [this, plater](wxCommandEvent&) { show_orca_print_confirmation(this, *plater); });
    }
    m_content->SetSizer(root);
    m_footer->SetSizer(footer);
    auto* layout = new wxBoxSizer(wxVERTICAL);
    layout->Add(m_content, 1, wxEXPAND);
    layout->Add(m_footer, 0, wxEXPAND | wxALL, FromDIP(12));
    SetSizer(layout);

    m_start->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_can_plan_candidates) {
            std::vector<AI::SmartSlicing::SliceCandidate> candidates;
            try {
                if (m_plan_candidates)
                    candidates = m_plan_candidates();
            } catch (...) {
                candidates.clear();
            }
            if (!run_trial_task(m_coordinator.begin_candidate_trials(
                    std::move(candidates), AI::SmartSlicing::CandidateGoal::Stability)))
                m_coordinator.cancel();
        } else {
            if (m_can_recheck) m_coordinator.cancel();
            m_coordinator.start();
        }
    });
    m_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_cancel_trial)
            m_cancel_trial();
        m_coordinator.cancel();
    });
    Bind(wxEVT_SHOW, [this](wxShowEvent& event) {
        if (!event.IsShown() && m_worker_running.load(std::memory_order_acquire)) {
            if (m_cancel_trial)
                m_cancel_trial();
            m_coordinator.cancel();
        }
        event.Skip();
    });
    Bind(wxEVT_TIMER, &SmartSlicingPanel::on_revision_timer, this, m_revision_timer.GetId());
    refresh_ai_appearance(this);
}

SmartSlicingPanel::~SmartSlicingPanel()
{
    shutdown_async();
}

void SmartSlicingPanel::refresh_preparation_selection()
{
    if (m_preparation) m_preparation->refresh_selection();
}

void SmartSlicingPanel::apply_ai_theme(bool update_fonts)
{
    ModelGenerationInputStyle::apply(this, update_fonts);
    for (auto& controls : m_candidate_controls)
        style_candidate(controls.selector, update_fonts);
    Refresh(false);
}

wxString mode_text(SmartSlicingMode mode)
{
    return mode == SmartSlicingMode::Orca ? _L("Orca 原生") : _L("AI 智能切片");
}

wxString purpose_text(SmartSlicingPurpose purpose)
{
    switch (purpose) {
    case SmartSlicingPurpose::Decoration: return _L("装饰");
    case SmartSlicingPurpose::Functional: return _L("功能");
    case SmartSlicingPurpose::General: return _L("通用");
    }
    return _L("通用");
}

wxString goal_state_text(SmartSlicingGoalState state)
{
    switch (state) {
    case SmartSlicingGoalState::Analyzing: return _L("分析中");
    case SmartSlicingGoalState::Ready: return _L("可比较");
    case SmartSlicingGoalState::Unavailable: return _L("不可用");
    case SmartSlicingGoalState::Failed: return _L("失败");
    case SmartSlicingGoalState::Stale: return _L("已过期");
    case SmartSlicingGoalState::Applied: return _L("已应用");
    case SmartSlicingGoalState::OfficialSlicing: return _L("正式切片中");
    case SmartSlicingGoalState::ApplyFailed: return _L("应用失败");
    }
    return _L("分析中");
}

wxString goal_title(const std::string& goal_id)
{
    if (goal_id == "speed") return _L("Speed · 速度");
    if (goal_id == "quality") return _L("Quality · 质量");
    return _L("Balanced · 平衡");
}

void SmartSlicingPanel::shutdown_async()
{
    m_callback_gate.assert_owner_thread();
    if (m_shutdown)
        return;
    m_shutdown = true;
    m_callback_gate.close();
    m_revision_timer.Stop();
    if (m_worker_running.load(std::memory_order_acquire) && m_cancel_trial)
        m_cancel_trial();
    m_coordinator.cancel();
    if (m_worker.joinable())
        m_worker.join();
    m_worker_running.store(false, std::memory_order_release);
}

bool SmartSlicingPanel::run_trial_task(std::optional<AI::SmartSlicing::CandidateTrialTask> task)
{
    m_callback_gate.assert_owner_thread();
    if (m_shutdown || !task || !m_execute_trial ||
        m_worker_running.exchange(true, std::memory_order_acq_rel))
        return false;
    if (m_worker.joinable())
        m_worker.join();

    auto result = std::make_shared<std::optional<AI::SmartSlicing::TrialSliceResult>>();
    std::function<void()> completion = m_callback_gate.guard(
        [this, task = *task, result] () mutable {
            if (!*result)
                return;
            complete_trial_task(std::move(task), std::move(**result));
        });
    ExecuteTrialFn execute_trial = m_execute_trial;
    OwnerDispatchFn owner_dispatch = m_owner_dispatch;
    m_worker = std::thread([task = std::move(*task), result, execute_trial = std::move(execute_trial),
                            owner_dispatch = std::move(owner_dispatch), completion = std::move(completion)]() mutable {
        try {
            *result = execute_trial(task.candidate);
        } catch (const std::exception& error) {
            *result = AI::SmartSlicing::TrialSliceResult{task.candidate.id, task.candidate.base_revision,
                AI::SmartSlicing::TrialSliceStatus::Failed, std::nullopt, error.what()};
        } catch (...) {
            *result = AI::SmartSlicing::TrialSliceResult{task.candidate.id, task.candidate.base_revision,
                AI::SmartSlicing::TrialSliceStatus::Failed, std::nullopt, "unknown_trial_error"};
        }
        owner_dispatch(std::move(completion));
    });
    return true;
}

void SmartSlicingPanel::complete_trial_task(AI::SmartSlicing::CandidateTrialTask task,
                                            AI::SmartSlicing::TrialSliceResult result)
{
    m_callback_gate.assert_owner_thread();
    m_worker_running.store(false, std::memory_order_release);
    if (m_shutdown)
        return;
    AI::SmartSlicing::CandidateTrialAcceptance acceptance =
        m_coordinator.accept_candidate_trial_result(std::move(task), std::move(result));
    if (acceptance.next_task && !run_trial_task(std::move(acceptance.next_task)))
        m_coordinator.cancel();
}

void SmartSlicingPanel::render(const SmartSlicingViewModel& view_model)
{
    static const std::array<wxString, 4> names{_L("1. 模型与材料"), _L("2. 健康与准备"), _L("3. 优化方案"), _L("4. 检查并切片")};
    m_mode_choice->SetSelection(view_model.mode == SmartSlicingMode::Orca ? 1 : 0);
    switch (view_model.purpose) {
    case SmartSlicingPurpose::Decoration: m_purpose_choice->SetSelection(0); break;
    case SmartSlicingPurpose::Functional: m_purpose_choice->SetSelection(2); break;
    case SmartSlicingPurpose::General: m_purpose_choice->SetSelection(1); break;
    }
    const bool ai_mode = view_model.mode == SmartSlicingMode::AI;
    m_purpose_choice->Enable(ai_mode);
    for (wxStaticText* card : m_goal_cards)
        card->Enable(ai_mode);
    m_mode_label->SetLabel(_L("模式：") + mode_text(view_model.mode));
    m_purpose_label->SetLabel(_L("用途：") + purpose_text(view_model.purpose));
    if (!view_model.native_baseline_available) {
        m_baseline_label->SetLabel(_L("Orca 原生基线：尚未捕获"));
    } else {
        wxString baseline = _L("Orca 原生基线：已捕获");
        if (view_model.baseline.estimated_time_seconds)
            baseline += _L(" · 时间 ") + format_duration(view_model.baseline.estimated_time_seconds);
        if (view_model.baseline.filament_volume_mm3)
            baseline += _L(" · 材料 ") + format_volume(view_model.baseline.filament_volume_mm3);
        m_baseline_label->SetLabel(baseline);
    }
    for (size_t index = 0; index < view_model.goal_results.size(); ++index) {
        const SmartSlicingGoalView& goal = view_model.goal_results[index];
        wxString card = goal_title(goal.goal_id) + _L("：") + goal_state_text(goal.state);
        if (!goal.diagnostic_codes.empty())
            card += _L(" · ") + from_u8(goal.diagnostic_codes.front());
        else if (goal.actions.can_apply)
            card += _L(" · 可应用");
        else if (goal.actions.can_retry_slice)
            card += _L(" · 可重试正式切片");
        m_goal_cards[index]->SetLabel(card);
        m_goal_cards[index]->Wrap(FromDIP(330));
    }
    const bool awaiting_native_slice = view_model.issues.size() == 1 &&
                                       view_model.issues.front().first == "native_validation_unavailable";
    m_summary->SetLabel(awaiting_native_slice && view_model.summary_key == "preflight_complete_with_warnings" ?
        _L("预检查完成，可继续比较方案") : summary_text(view_model.summary_key));
    m_summary->Wrap(FromDIP(330));
    for (size_t index = 0; index < m_stage_labels.size(); ++index)
        m_stage_labels[index]->SetLabel(names[index] + _L("  ") +
            (index == 3 && view_model.summary_key == "candidates_ready" ? _L("等待应用") :
             status_text(view_model.stages[index].status)));
    if (view_model.summary_key == "baseline_trial_failed") {
        wxString failure = _L("当前基线未通过试切，不能应用或用于打印。原工程未被修改。");
        const auto baseline = std::find_if(view_model.candidates.begin(), view_model.candidates.end(),
            [](const SmartSlicingCandidateView& candidate) { return candidate.id == "baseline" && candidate.failed; });
        if (baseline != view_model.candidates.end() && !baseline->diagnostic_message.empty())
            failure += _L("\n原生切片原因：\n") + from_u8(baseline->diagnostic_message);
        failure += _L("\n请打开 Orca 原生设置按上述原因调整配置或摆放，再重新检查。");
        m_issues->SetLabel(failure);
    } else if (view_model.has_report && awaiting_native_slice) {
        m_issues->SetLabel(issue_name("native_validation_unavailable") + _L("\n") +
                           issue_action("native_validation_unavailable"));
    } else if (!view_model.has_report || view_model.issues.empty()) {
        m_issues->SetLabel(view_model.has_report ? _L("本次检查未发现问题") : _L("尚无有效检查结果"));
    } else {
        wxString issues = wxString::Format(_L("发现 %llu 项需要留意的问题"), static_cast<unsigned long long>(view_model.issue_count));
        const size_t visible_issue_count = std::min<size_t>(view_model.issues.size(), 5);
        for (size_t index = 0; index < visible_issue_count; ++index) {
            const auto& [code, evidence] = view_model.issues[index];
            issues += _L("\n• ") + issue_name(code) + "\n" + issue_action(code);
        }
        if (visible_issue_count < view_model.issues.size())
            issues += wxString::Format(_L("\n…另有 %llu 项"),
                                       static_cast<unsigned long long>(view_model.issues.size() - visible_issue_count));
        m_issues->SetLabel(issues);
    }
    wxString diagnostics;
    for (const auto& [code, evidence] : view_model.issues)
        diagnostics += from_u8(code) + ": " + from_u8(evidence) + "\n";
    for (const auto& candidate : view_model.candidates) {
        if (!candidate.explanation.empty())
            diagnostics += from_u8(candidate.id) + ": " + from_u8(candidate.explanation) + "\n";
        if (!candidate.diagnostic_code.empty())
            diagnostics += from_u8(candidate.id) + ": " + from_u8(candidate.diagnostic_code) + "\n";
        if (!candidate.diagnostic_message.empty())
            diagnostics += from_u8(candidate.diagnostic_message) + "\n";
    }
    m_diagnostic_text->SetLabel(diagnostics);
    m_diagnostics->Show(view_model.has_report && !diagnostics.empty());
    m_add_model->Show(view_model.can_add_model);
    m_can_recheck = view_model.can_recheck;
    m_can_plan_candidates = view_model.can_plan_candidates;
    m_start->Enable(view_model.can_start || view_model.can_plan_candidates || view_model.can_recheck);
    const bool has_candidates = !view_model.candidates.empty() && !view_model.is_stale;
    const bool slice_ready = view_model.summary_key == "official_slice_complete";
    m_start->SetName(has_candidates ? "input_field" : "input_primary");
    ModelGenerationInputStyle::apply_control(m_start, has_candidates ?
        ModelGenerationInputStyle::Role::Field : ModelGenerationInputStyle::Role::PrimaryAction);
    if (m_confirm) {
        m_confirm->SetName(slice_ready ? "input_primary" : "input_field");
        ModelGenerationInputStyle::apply_control(m_confirm, slice_ready ?
            ModelGenerationInputStyle::Role::PrimaryAction : ModelGenerationInputStyle::Role::Field);
    }
    m_start->SetLabel(view_model.can_plan_candidates ? _L("生成并试切方案") :
                      view_model.is_stale || view_model.can_recheck ? _L("重新检查") : _L("开始检查"));
    m_cancel->Enable(view_model.can_cancel);

    const bool show_candidates = !view_model.candidates.empty();
    m_candidate_section->Show(show_candidates);
    m_keep_baseline->Show(show_candidates);
    m_apply->Show(show_candidates);
    const bool trial_running = view_model.summary_key == "trial_slicing_baseline" ||
                               view_model.summary_key == "trial_slicing_candidates";
    m_candidate_hint->Show(view_model.can_apply || trial_running ||
                           view_model.summary_key == "baseline_trial_failed");
    m_candidate_hint->SetLabel(trial_running ?
        _L("正在试切候选方案。完成后才能选择和应用。") :
        view_model.summary_key == "baseline_trial_failed" ?
        _L("当前方案试切失败。请查看诊断，或返回原生设置处理。") :
        view_model.candidates.size() == 1 ?
        _L("本次只有当前方案可用。可保留此方案，或在原生设置中调整参数后重新检查。") :
        _L("选择卡片查看方案详情；选择不会修改工程。"));
    const bool has_profiles = std::any_of(view_model.candidates.begin(), view_model.candidates.end(), is_priority_profile);
    const SmartSlicingCandidateView* selected = nullptr;
    std::vector<const SmartSlicingCandidateView*> visible_candidates;
    for (const auto& candidate : view_model.candidates) {
        if (candidate.selected) selected = &candidate;
        if ((!has_profiles && view_model.candidates.size() <= m_candidate_controls.size()) || candidate.id != "baseline")
            visible_candidates.push_back(&candidate);
    }
    for (size_t index = 0; index < m_candidate_controls.size(); ++index) {
        CandidateControls& controls = m_candidate_controls[index];
        const bool visible = index < visible_candidates.size();
        controls.panel->Show(visible);
        m_candidate_ids[index].clear();
        if (!visible)
            continue;
        const SmartSlicingCandidateView& candidate = *visible_candidates[index];
        m_candidate_ids[index] = candidate.id;
        controls.selector->SetLabel(candidate_name(candidate) + "\n" +
            candidate_secondary(candidate));
        controls.selector->SetSelected(candidate.selected);
        controls.selector->Enable(candidate.can_select);
        controls.selector->SetToolTip(candidate_name(candidate) + "\n" +
            candidate_changes(candidate) + candidate_reason(candidate));
        style_candidate(controls.selector);
        controls.retry->Show(candidate.can_retry);
        if (candidate.selected)
            selected = &candidate;
    }
    m_candidate_details->Show(selected != nullptr);
    if (selected != nullptr) {
        const auto& candidate = *selected;
        m_candidate_title->SetLabel(_L("所选方案 · ") + candidate_name(candidate) +
            (candidate.recommended ? _L("（推荐）") : wxString()));
        wxString metrics = _L("时间：") + format_duration(candidate.estimated_time_seconds) +
                           _L("  材料：") + format_volume(candidate.filament_volume_mm3) +
                           _L("\n支撑：") + format_volume(candidate.support_volume_mm3) +
                           _L("  换料：") +
                           (candidate.tool_changes ?
                                wxString::Format("%llu", static_cast<unsigned long long>(*candidate.tool_changes)) :
                                _L("不可用")) +
                           _L("\n冲刷：") + format_volume(candidate.flush_volume_mm3) +
                           _L("  擦料塔：") + format_volume(candidate.wipe_tower_volume_mm3);
        if (candidate.layer_tool_sequence_count > 0)
            metrics += wxString::Format(_L("\n层级工具序列：%llu 组"),
                                        static_cast<unsigned long long>(candidate.layer_tool_sequence_count));
        if (candidate.color_mapping_degraded == true)
            metrics += _L("\n颜色映射：已退化");
        else if (candidate.color_mapping_degraded == false)
            metrics += _L("\n颜色映射：保持一致");
        if (candidate.id != "baseline") {
            const wxString tool_delta = candidate.tool_change_delta ?
                wxString::Format("%+lld", *candidate.tool_change_delta) : _L("—");
            metrics += _L("\n相对基线：时间 ") + format_delta(candidate.time_delta_seconds, 60.0, _L("分钟")) +
                       _L("，材料 ") + format_delta(candidate.filament_delta_mm3, 1000.0, _L("cm³")) +
                       _L("，支撑 ") + format_delta(candidate.support_delta_mm3, 1000.0, _L("cm³")) +
                       _L("，换料 ") + tool_delta +
                       _L("，冲刷 ") + format_delta(candidate.flush_delta_mm3, 1000.0, _L("cm³")) +
                       _L("，擦料塔 ") + format_delta(candidate.wipe_tower_delta_mm3, 1000.0, _L("cm³"));
        }
        m_candidate_metrics->SetLabel(_L("试切估算\n") + metrics);
        const wxString changes = candidate_changes(candidate);
        m_candidate_reason->SetLabel(
            (changes.empty() ? _L("应用时保留当前参数与摆放。\n") : changes) + candidate_reason(candidate));
    }
    m_keep_baseline->Enable(std::any_of(view_model.candidates.begin(), view_model.candidates.end(),
                                       [](const SmartSlicingCandidateView& candidate) {
                                           return candidate.id == "baseline" && !candidate.selected && candidate.can_select;
                                       }));
    m_apply->Enable(view_model.can_apply);
    ModelGenerationInputStyle::apply_control(m_apply, ModelGenerationInputStyle::Role::PrimaryAction);
    m_undo_apply->Show(view_model.can_undo_apply);
    m_undo_apply->Enable(view_model.can_undo_apply);
    m_p0_notice->SetLabel(view_model.summary_key == "baseline_trial_failed" ?
                         _L("试切在隔离副本中失败；原工程保留，可返回原生设置处理。") :
                         view_model.can_undo_apply ? _L("可撤销本次应用；若之后编辑了工程，请使用 Orca 撤销历史。") :
                         show_candidates ? _L("选择“应用并切片”后，将修改当前打印板并开始正式切片。") :
                                            _L("预检与候选试切均在隔离副本中执行。"));
    if ((view_model.can_cancel || view_model.needs_polling) && !m_revision_timer.IsRunning())
        m_revision_timer.Start(1000);
    else if (!view_model.can_cancel && !view_model.needs_polling && m_revision_timer.IsRunning())
        m_revision_timer.Stop();
    m_content->Layout();
    m_content->FitInside();
    m_footer->Layout();
    Layout();
}

void SmartSlicingPanel::on_revision_timer(wxTimerEvent&)
{
    m_coordinator.refresh_revision();
}

} // namespace Slic3r::GUI
