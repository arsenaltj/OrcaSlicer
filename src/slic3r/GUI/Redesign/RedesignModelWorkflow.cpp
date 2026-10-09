#include <glad/gl.h>
#include "RedesignShell.hpp"
#include "RedesignMessageDialog.hpp"
#include "RedesignFeatureFlags.hpp"
#include "RedesignModelRoute.hpp"
#include "RedesignTheme.hpp"
#include "AssetsWorkspace.hpp"
#include "AssetsWorkspacePolicy.hpp"
#include "PrinterWorkspace.hpp"
#include "OrcaPrinterAdapter.hpp"
#include "../AI/ModelGeneration/ModelGenerationFeatureHost.hpp"
#include "../AI/ModelGeneration/WorkbenchStyle.hpp"
#include "../AI/SmartSlicing/SmartSlicingFeatureHost.hpp"
#include "../GUI_App.hpp"
#include "../I18N.hpp"
#include "../MainFrame.hpp"
#include "../BBLTopbar.hpp"
#include "../Plater.hpp"
#include "../GLCanvas3D.hpp"
#include "../PresetComboBoxes.hpp"
#include "../Widgets/SpinInput.hpp"
#include "../Widgets/Label.hpp"
#include "slic3r/AI/SmartSlicing/Domain/RiskConfirmationPolicy.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <wx/choice.h>
#include <wx/dcclient.h>
#include <wx/glcanvas.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/weakref.h>
#include <wx/activityindicator.h>
#include <boost/log/trivial.hpp>
#include <sstream>

namespace Slic3r::GUI {
namespace {
using namespace AI::SmartSlicing;

void style_native_sidebar(wxWindow* window, const std::vector<wxWindow*>& swatches,
                          const wxColour& surface = RedesignTheme::panel_colour())
{
    if (std::find(swatches.begin(), swatches.end(), window) != swatches.end()) return;
    window->SetName("ai_content_color");
    window->SetBackgroundColour(surface);
    window->SetForegroundColour(wxColour(230, 230, 233));
    wxColour child_surface = surface;
    if (auto* box = dynamic_cast<StaticBox*>(window)) {
        child_surface = dynamic_cast<TextInput*>(window) || dynamic_cast<SpinInput*>(window)
            ? wxColour(70, 70, 73) : surface;
        box->SetBackgroundColor(child_surface);
        box->SetBorderColor(wxColour(82, 82, 87));
        box->SetCornerRadius(window->FromDIP(6));
    }
    if (auto* input = dynamic_cast<TextInput*>(window)) {
        input->SetTextColor(wxColour(230, 230, 233));
        input->SetLabelColor(wxColour(230, 230, 233));
        if (auto* combo = dynamic_cast<ComboBox*>(input)) {
            auto& drop = combo->GetDropDown();
            drop.SetBackgroundColour(child_surface);
            drop.SetTextColor(wxColour(230, 230, 233));
            drop.SetSelectorBackgroundColor(wxColour(85, 85, 90));
        }
    } else if (auto* input = dynamic_cast<SpinInput*>(window)) {
        input->SetTextColor(wxColour(230, 230, 233));
        input->SetLabelColor(wxColour(190, 190, 196));
    } else if (auto* button = dynamic_cast<Button*>(window)) {
        button->SetTextColor(wxColour(230, 230, 233));
    }
    for (wxWindow* child : window->GetChildren()) style_native_sidebar(child, swatches, child_surface);
    window->Refresh(false);
}

void refresh_native_sidebar(Plater& plater)
{
    std::vector<wxWindow*> swatches;
    for (auto* combo : plater.sidebar().combos_filament())
        if (combo && combo->clr_picker) swatches.push_back(combo->clr_picker);
    style_native_sidebar(&plater.sidebar(), swatches);
}

WorkbenchButton* command(wxWindow* parent, const wxString& label, bool accent = false)
{
    auto* button = new WorkbenchButton(parent, label);
    button->SetCornerRadius(parent->FromDIP(6));
    button->SetBorderWidth(0);
    button->SetMinSize(parent->FromDIP(wxSize(-1, 36)));
    button->SetBackgroundColor(StateColor(
        std::pair<wxColour, int>(wxColour(55, 55, 58), StateColor::Disabled),
        std::pair<wxColour, int>(accent ? wxColour(255, 210, 78) : wxColour(85, 85, 89), StateColor::Hovered),
        std::pair<wxColour, int>(accent ? RedesignTheme::accent_colour() : wxColour(70, 70, 73), StateColor::Normal)));
    button->SetTextColor(StateColor(
        std::pair<wxColour, int>(wxColour(126, 126, 130), StateColor::Disabled),
        std::pair<wxColour, int>(accent ? wxColour(22, 22, 25) : RedesignTheme::primary_text_colour(), StateColor::Normal)));
    RedesignTheme::style_text(button, RedesignTheme::primary_text_colour(), 10);
    return button;
}

void set_wrapped_label(wxStaticText* label, const wxString& text, int width)
{
    wxClientDC dc(label);
    dc.SetFont(label->GetFont());
    wxString wrapped;
    const wxSize extent = Label::split_lines(dc, width, text, wrapped);
    label->SetLabel(wrapped);
    // MSW can retain the previous control height after wrapping a longer label.
    label->SetMinSize(wxSize(-1, extent.y + label->FromDIP(2)));
}

wxString goal_status(GoalResultStatus status)
{
    switch (status) {
    case GoalResultStatus::Ready: return _L("已就绪");
    case GoalResultStatus::Analyzing: return _L("分析中");
    case GoalResultStatus::Stale: return _L("已失效");
    case GoalResultStatus::Applied: return _L("已应用");
    case GoalResultStatus::Failed: return _L("失败");
    default: return _L("不可用");
    }
}

wxString capability_reason(MachineCapabilityReason reason)
{
    switch (reason) {
    case MachineCapabilityReason::ProfileNotAllowlisted: return _L("机型尚未支持");
    case MachineCapabilityReason::ProfileFingerprintMismatch: return _L("机型预设身份未验证");
    case MachineCapabilityReason::UnsupportedNozzleCount: return _L("喷嘴数量未支持");
    case MachineCapabilityReason::SixNozzlePendingValidation: return _L("六喷嘴待验证");
    case MachineCapabilityReason::UnsupportedNozzleDiameter: return _L("喷嘴直径未支持");
    case MachineCapabilityReason::InvalidPhysicalNumbering: return _L("喷嘴编号无效");
    case MachineCapabilityReason::MissingIndependentAddressing: return _L("喷嘴寻址待验证");
    case MachineCapabilityReason::MissingNozzleOffset: return _L("缺少喷嘴偏移标定");
    case MachineCapabilityReason::MissingReachableArea: return _L("缺少喷嘴可达范围");
    case MachineCapabilityReason::MissingCollisionClearance: return _L("缺少碰撞间距标定");
    case MachineCapabilityReason::MissingToolChangeGcode: return _L("换喷嘴指令待验证");
    case MachineCapabilityReason::MissingPreheatBehavior:
    case MachineCapabilityReason::MissingStandbyBehavior: return _L("喷嘴温控待验证");
    case MachineCapabilityReason::MissingRetractionBehavior: return _L("回抽行为待验证");
    case MachineCapabilityReason::MissingToolChangeTimeModel: return _L("换喷嘴时间待验证");
    case MachineCapabilityReason::MissingPrimeOrWipeCapability:
    case MachineCapabilityReason::MissingFlushMatrix:
    case MachineCapabilityReason::MissingWipeTowerSpaceConstraints: return _L("擦料与预充待验证");
    case MachineCapabilityReason::SpecializedValidationIncomplete: return _L("机型专项验证未完成");
    }
    return _L("机型能力待验证");
}

wxString capability_reason(MaterialCompatibilityReason reason)
{
    switch (reason) {
    case MaterialCompatibilityReason::ProfileNotAllowlisted: return _L("耗材预设尚未支持");
    case MaterialCompatibilityReason::ProfileFingerprintMismatch: return _L("耗材预设身份未验证");
    case MaterialCompatibilityReason::RestrictedVariant: return _L("该耗材变体尚未支持");
    case MaterialCompatibilityReason::NoMaterialInUse: return _L("未选择实际使用的耗材");
    case MaterialCompatibilityReason::MixedMaterialFamilies: return _L("不同耗材类型的组合未支持");
    }
    return _L("耗材兼容性待验证");
}

template<class Reason>
wxString capability_reasons(const std::vector<Reason>& reasons)
{
    if (reasons.empty()) return _L("缺少验证证据");
    wxString text;
    std::vector<wxString> descriptions;
    for (const auto reason : reasons) {
        auto description = capability_reason(reason);
        if (std::find(descriptions.begin(), descriptions.end(), description) != descriptions.end()) continue;
        if (!text.empty()) text += _L("、");
        text += description;
        descriptions.push_back(std::move(description));
    }
    return text;
}

wxString parameter_value(const ConfigPatchEntry& entry)
{
    const auto* definition = print_config_def.get(entry.key);
    if (const auto* enabled = std::get_if<bool>(&entry.new_value))
        return *enabled ? _L("启用") : _L("关闭");
    std::ostringstream value;
    std::visit([&](const auto& item) { value << item; }, entry.new_value);
    const auto serialized = value.str();
    if (definition) {
        auto found = std::find(definition->enum_values.begin(), definition->enum_values.end(), serialized);
        if (found != definition->enum_values.end()) {
            const auto index = std::distance(definition->enum_values.begin(), found);
            if (size_t(index) < definition->enum_labels.size()) return _(definition->enum_labels[index]);
        }
    }
    return wxString::FromUTF8(serialized) + (definition && !definition->sidetext.empty()
        ? " " + _(definition->sidetext) : wxString());
}

wxString candidate_details(const std::vector<ConfigPatchEntry>& parameters, const std::optional<TrialMetrics>& metrics)
{
    wxString result;
    wxString category;
    for (const auto& entry : parameters) {
        const auto* definition = print_config_def.get(entry.key);
        const auto next_category = definition ? _(definition->category) : wxString();
        if (!next_category.empty() && next_category != category) {
            result += "\n" + next_category + "\n";
            category = next_category;
        }
        const auto label = definition && !definition->label.empty() ? _(definition->label)
            : wxString::FromUTF8(entry.key);
        result += label + "  " + parameter_value(entry) + "\n";
    }
    if (metrics) {
        if (metrics->estimated_time_seconds.known())
            result += wxString::Format(_L("预计时间  %.0f 分钟\n"), *metrics->estimated_time_seconds.value / 60.);
        if (metrics->total_material_volume_mm3.known())
            result += wxString::Format(_L("耗材体积  %.1f cm³\n"), *metrics->total_material_volume_mm3.value / 1000.);
        if (metrics->tool_change_count.known())
            result += wxString::Format(_L("换料次数  %zu\n"), *metrics->tool_change_count.value);
    }
    return result;
}
}

bool RedesignShell::owns_model_workflow() const
{
    return m_workbench_page != nullptr && m_plater != nullptr;
}

bool RedesignShell::native_workspace_visible() const
{
    return owns_model_workflow() && m_active_page == Page::Model &&
        (m_model_view == ModelView::Slicing || m_model_view == ModelView::Preview);
}

void RedesignShell::start_slicing_from_workspace()
{
    if (!m_import_in_progress && native_workspace_visible()) start_workbench_slice();
}

void RedesignShell::build_model_workflow()
{
    if (!RedesignFeatureFlags::model_workflow_review_enabled() || !m_model_generation_host || !m_plater) return;
    auto* history = command(m_model_page, _L("打开历史模型"));
    history->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { open_model_workbench(); });
    m_model_page->GetSizer()->Insert(2, history, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(42));
    m_workbench_page = new wxPanel(m_content_host);
    m_workbench_page->SetBackgroundColour(RedesignTheme::background_colour());
    m_workbench_page->SetSizer(new wxBoxSizer(wxVERTICAL));
    m_content_host->GetSizer()->Add(m_workbench_page, 1, wxEXPAND);
    m_workbench_page->Hide();
    m_model_generation_host->mount_workbench(m_workbench_page);

    m_slicing_page = new wxPanel(m_content_host);
    m_slicing_page->SetBackgroundColour(RedesignTheme::background_colour());
    auto* page = new wxBoxSizer(wxVERTICAL);
    m_slicing_page->SetSizer(page);
    m_native_slice_commands = new wxPanel(m_slicing_page);
    m_native_slice_commands->SetBackgroundColour(RedesignTheme::panel_colour());
    auto* native_commands = new wxBoxSizer(wxHORIZONTAL);
    m_native_slice_commands->SetSizer(native_commands);
    auto* native_back = command(m_native_slice_commands, _L("返回模型总览"));
    native_back->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { open_model_workbench(); });
    native_commands->Add(native_back, 0, wxRIGHT, FromDIP(8));
    auto* native_ai_tab = command(m_native_slice_commands, _L("AI 智能切片"));
    native_ai_tab->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { select_slicing_tab(false); });
    native_commands->Add(native_ai_tab, 0, wxRIGHT, FromDIP(8));
    m_native_slice_status = new wxStaticText(m_native_slice_commands, wxID_ANY, wxEmptyString);
    RedesignTheme::style_text(m_native_slice_status, RedesignTheme::secondary_text_colour(), 9);
    native_commands->Add(m_native_slice_status, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
    native_commands->AddStretchSpacer();
    m_native_slice_start = command(m_native_slice_commands, _L("开始切片"), true);
    m_native_slice_start->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_model_view == ModelView::Preview) show_model_view(ModelView::Slicing);
        else start_workbench_slice();
    });
    native_commands->Add(m_native_slice_start, 0, wxRIGHT, FromDIP(8));
    m_native_slice_save = command(m_native_slice_commands, _L("保存工程 3MF"));
    m_native_slice_save->SetToolTip(_L("保存模型、耗材颜色、摆放和打印参数；无需先切片。"));
    m_native_slice_save->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { save_print_project(); });
    native_commands->Add(m_native_slice_save, 0, wxRIGHT, FromDIP(8));
    m_native_slice_export = command(m_native_slice_commands, _L("导出 G-code"), true);
    m_native_slice_export->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        auto* plate = m_plater->get_partplate_list().get_curr_plate();
        if (plate && OrcaPrinterAdapter(m_plater).snapshot().gcode_ready) m_plater->export_gcode(false);
    });
    native_commands->Add(m_native_slice_export, 0);
    m_native_slice_print = command(m_native_slice_commands, _L("去打印"), true);
    native_commands->Add(m_native_slice_print, 0, wxLEFT, FromDIP(8));
    m_native_slice_print->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { open_print_preparation(); });
    page->Add(m_native_slice_commands, 0, wxEXPAND | wxALL, FromDIP(8));
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    page->Add(row, 1, wxEXPAND);
    auto* settings = m_slicing_settings = new wxPanel(m_slicing_page);
    settings->SetBackgroundColour(RedesignTheme::panel_colour());
    settings->SetMinSize(FromDIP(wxSize(280, -1)));
    auto* root = new wxBoxSizer(wxVERTICAL);
    settings->SetSizer(root);
    row->Add(settings, 0, wxEXPAND | wxRIGHT, FromDIP(8));
    auto* scroll = new WorkbenchScrolledWindow(settings, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    scroll->SetBackgroundColour(RedesignTheme::panel_colour());
    scroll->SetScrollRate(0, FromDIP(12));
    auto* controls = new wxBoxSizer(wxVERTICAL);
    scroll->SetSizer(controls);
    root->Add(scroll, 1, wxEXPAND | wxALL, FromDIP(12));

    auto heading = [&](const wxString& title) {
        auto* label = new wxStaticText(scroll, wxID_ANY, title);
        RedesignTheme::style_text(label, RedesignTheme::primary_text_colour(), 10);
        controls->Add(label, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(10));
    };
    heading(_L("检查修复"));
    m_slice_check_status = new wxStaticText(scroll, wxID_ANY, _L("未执行检查"));
    RedesignTheme::style_text(m_slice_check_status, RedesignTheme::primary_text_colour(), 9);
    controls->Add(m_slice_check_status, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    heading(_L("多色模型"));
    m_slice_palette = new wxBoxSizer(wxVERTICAL);
    controls->Add(m_slice_palette, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    auto* colors = command(scroll, _L("配色确认"));
    controls->Add(colors, 0, wxEXPAND);
    colors->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { open_model_workbench(); });
    heading(_L("3D 美颜"));
    auto* beauty = command(scroll, _L("3D 美颜工作台"));
    controls->Add(beauty, 0, wxEXPAND);
    beauty->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (open_model_workbench()) m_model_generation_host->request_enter_beauty();
    });
    heading(_L("切片"));
    auto* tabs = new wxBoxSizer(wxHORIZONTAL);
    m_ai_slicing_tab = command(scroll, _L("AI 智能切片"));
    m_native_slicing_tab = command(scroll, _L("Orca 原生"));
    tabs->Add(m_ai_slicing_tab, 1, wxEXPAND | wxRIGHT, FromDIP(4));
    tabs->Add(m_native_slicing_tab, 1, wxEXPAND);
    controls->Add(tabs, 0, wxEXPAND | wxBOTTOM, FromDIP(12));
    m_ai_slicing_tab->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { select_slicing_tab(false); });
    m_native_slicing_tab->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { select_slicing_tab(true); });

    m_ai_slicing_controls = new wxPanel(scroll);
    m_ai_slicing_controls->SetBackgroundColour(RedesignTheme::control_colour());
    auto* ai = new wxBoxSizer(wxVERTICAL);
    m_ai_slicing_controls->SetSizer(ai);
    auto* goals = new wxGridSizer(2, FromDIP(8), FromDIP(8));
    const std::array<wxString, 3> titles {_L("综合最优"), _L("速度优先"), _L("质量优先")};
    for (size_t i = 0; i < titles.size(); ++i) {
        auto* button = command(m_ai_slicing_controls, titles[i]);
        button->set_multiline_label();
        button->SetMinSize(FromDIP(wxSize(112, 72)));
        button->SetBackgroundColour(m_ai_slicing_controls->GetBackgroundColour());
        button->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent&) {
            if (m_slicing_host) m_slicing_host->select_goal(RECOMMENDATION_GOALS[i]);
        });
        m_slice_goals[i] = button;
        goals->Add(button, 0, wxEXPAND);
    }
    ai->Add(goals, 0, wxEXPAND | wxALL, FromDIP(8));
    m_slice_details = new wxStaticText(m_ai_slicing_controls, wxID_ANY, _L("尚未分析"));
    RedesignTheme::style_text(m_slice_details, RedesignTheme::primary_text_colour(), 9);
    ai->Add(m_slice_details, 0, wxEXPAND | wxALL, FromDIP(10));
    m_slice_analyze = command(m_ai_slicing_controls, _L("分析三种方案"));
    ai->Add(m_slice_analyze, 0, wxEXPAND | wxALL, FromDIP(8));
    m_slice_analyze->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_slicing_host) { m_slicing_host->analyze_workbench(); apply_slicing_state(m_slicing_host->workbench_snapshot()); }
    });
    controls->Add(m_ai_slicing_controls, 0, wxEXPAND);
    m_slice_status = new wxStaticText(scroll, wxID_ANY, _L("准备切片"));
    RedesignTheme::style_text(m_slice_status, RedesignTheme::secondary_text_colour(), 9);
    controls->Add(m_slice_status, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(12));
    m_slice_cancel = command(scroll, _L("取消分析"));
    controls->Add(m_slice_cancel, 0, wxEXPAND);
    m_slice_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { if (m_slicing_host) m_slicing_host->cancel_workbench_analysis(); });
    m_slice_keep_mesh = command(scroll, _L("保留当前网格并分析"));
    controls->Add(m_slice_keep_mesh, 0, wxEXPAND | wxTOP, FromDIP(8));
    m_slice_keep_mesh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (!m_slicing_host) return;
        const auto reviewed = m_slicing_host->workbench_snapshot();
        if (!reviewed.can_keep_current_mesh || !reviewed.preflight) return;
        if (show_redesign_confirmation(this, _L("网格存在开放边。保留当前网格继续分析可能产生缺面或悬空路径，请在切片预览中检查结果。"),
                _L("保留当前网格"), {wxYES_NO | wxNO_DEFAULT}) != wxID_YES) return;
        m_slicing_host->keep_current_mesh_and_analyze(reviewed.preflight->revision);
    });
    m_slice_start = command(settings, _L("开始切片"), true);
    root->Add(m_slice_start, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    m_slice_start->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { start_workbench_slice(); });
    m_slice_save = command(settings, _L("保存工程（3MF）"));
    m_slice_save->SetToolTip(_L("保存模型、耗材颜色、摆放和打印参数；G-code 需切片后单独导出。"));
    root->Add(m_slice_save, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    m_slice_save->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { save_print_project(); });
    m_slice_export = command(settings, _L("导出 G-code"), true);
    root->Add(m_slice_export, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    m_slice_export->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        auto* plate = m_plater->get_partplate_list().get_curr_plate();
        if (plate && OrcaPrinterAdapter(m_plater).snapshot().gcode_ready) m_plater->export_gcode(false);
    });
    m_slice_print = command(settings, _L("去打印"), true);
    root->Add(m_slice_print, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    m_slice_print->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { open_print_preparation(); });
    auto* return_slice = m_return_slice = command(settings, _L("返回切片参数"));
    root->Add(return_slice, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    return_slice->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { show_model_view(ModelView::Slicing); });
    m_native_plater_host = new wxPanel(m_slicing_page);
    m_native_plater_host->SetMinSize(wxSize(1, 1));
    m_native_plater_host->SetSizer(new wxBoxSizer(wxVERTICAL));
    row->Add(m_native_plater_host, 1, wxEXPAND);
    m_content_host->GetSizer()->Add(m_slicing_page, 1, wxEXPAND);
    m_slicing_page->Hide();
    m_slice_model_stats = new wxStaticText(scroll, wxID_ANY, wxEmptyString);
    RedesignTheme::style_text(m_slice_model_stats, RedesignTheme::secondary_text_colour(), 9);
    controls->Add(m_slice_model_stats, 0, wxEXPAND | wxTOP, FromDIP(12));
    m_plater_original_parent = m_plater->GetParent();
    m_saved_sidebar_collapsed = m_plater->is_sidebar_collapsed();
    if (auto* sizer = m_plater->GetContainingSizer()) sizer->Detach(m_plater);
    m_plater->Reparent(m_native_plater_host);
    m_native_plater_host->GetSizer()->Add(m_plater, 1, wxEXPAND);
    m_plater->Hide();
    m_slicing_host = m_plater->smart_slicing_feature_host();

    m_import_loading = new wxPanel(m_content_host);
    m_import_loading->SetBackgroundColour(RedesignTheme::background_colour());
    m_import_loading->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_import_loading->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        wxPaintDC dc(m_import_loading);
        dc.SetBackground(wxBrush(RedesignTheme::background_colour()));
        dc.Clear();
    });
    m_import_loading->SetName("workbench_import_loading");
    auto* loading = new wxBoxSizer(wxVERTICAL);
    m_import_loading->SetSizer(loading);
    loading->AddStretchSpacer();
    auto* activity = new wxActivityIndicator(m_import_loading, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(36, 36)));
    activity->SetForegroundColour(RedesignTheme::accent_colour());
    activity->SetBackgroundColour(RedesignTheme::background_colour());
    activity->Start();
    loading->Add(activity, 0, wxALIGN_CENTER | wxBOTTOM, FromDIP(20));
    m_import_stage = new wxStaticText(m_import_loading, wxID_ANY, _L("正在读取与验证模型…"));
    RedesignTheme::style_text(m_import_stage, RedesignTheme::primary_text_colour(), 12);
    loading->Add(m_import_stage, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, FromDIP(16));
    m_import_elapsed = new wxStaticText(m_import_loading, wxID_ANY, wxEmptyString);
    RedesignTheme::style_text(m_import_elapsed, RedesignTheme::secondary_text_colour(), 10);
    loading->Add(m_import_elapsed, 0, wxALIGN_CENTER | wxTOP | wxBOTTOM, FromDIP(12));
    m_import_cancel = command(m_import_loading, _L("取消"));
    m_import_cancel->SetMinSize(FromDIP(wxSize(160, 36)));
    loading->Add(m_import_cancel, 0, wxALIGN_CENTER);
    m_import_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_import_session && m_import_session->cancel()) {
            m_import_stage->SetLabel(_L("正在取消导入…"));
            m_import_cancel->Disable();
        }
    });
    loading->AddStretchSpacer();
    m_import_loading->Hide();
    m_content_host->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        event.Skip();
        if (m_import_loading && m_import_in_progress) {
            m_import_loading->SetSize(m_content_host->GetClientRect());
            m_import_loading->Layout();
        }
    });
    m_import_timer.SetOwner(this, wxWindow::NewControlId());
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        m_import_elapsed->SetLabel(wxString::Format(_L("已用时 %.1f 秒"),
            std::chrono::duration<double>(std::chrono::steady_clock::now() - m_import_started).count()));
        m_import_loading->Layout();
        check_import_first_frame();
    }, m_import_timer.GetId());

    wxWeakRef<RedesignShell> weak(this);
    m_plater->get_view3D_canvas3D()->get_wxglcanvas()->Bind(wxEVT_PAINT, [weak](wxPaintEvent& event) {
        event.Skip();
        if (weak && weak->m_import_awaiting_frame)
            weak->CallAfter([weak] { if (weak) weak->check_import_first_frame(); });
    });
    auto* frame = dynamic_cast<MainFrame*>(wxGetTopLevelParent(this));
    if (auto* topbar = frame ? frame->topbar() : nullptr) {
        topbar->Bind(wxEVT_UPDATE_UI, [weak](wxUpdateUIEvent& event) {
            if (!weak) { event.Skip(); return; }
            const bool workbench = weak->m_active_page == Page::Model &&
                weak->m_model_view == ModelView::Workbench && !weak->m_workbench_state.editing &&
                weak->m_workbench_state.can_edit_project_colors;
            event.Enable(!weak->m_import_in_progress && (workbench || (weak->native_workspace_visible() &&
                weak->m_model_view == ModelView::Slicing)) && weak->m_plater->can_undo());
        }, wxID_UNDO);
        topbar->Bind(wxEVT_UPDATE_UI, [weak](wxUpdateUIEvent& event) {
            if (!weak) { event.Skip(); return; }
            const bool workbench = weak->m_active_page == Page::Model &&
                weak->m_model_view == ModelView::Workbench && !weak->m_workbench_state.editing &&
                weak->m_workbench_state.can_edit_project_colors;
            event.Enable(!weak->m_import_in_progress && (workbench || (weak->native_workspace_visible() &&
                weak->m_model_view == ModelView::Slicing)) && weak->m_plater->can_redo());
        }, wxID_REDO);
    }
    Bind(wxEVT_CHAR_HOOK, [weak](wxKeyEvent& event) {
        const auto key = event.GetKeyCode();
        if (!weak || weak->m_active_page != Page::Model || weak->m_model_view != ModelView::Workbench ||
            !event.CmdDown() || event.AltDown() || event.ShiftDown() || (key != 'Z' && key != 'Y') ||
            dynamic_cast<wxTextEntry*>(wxWindow::FindFocus())) {
            event.Skip();
            return;
        }
    if (weak->m_import_in_progress) return;
        // Beauty owns preview history even before a candidate file exists.
        if (weak->m_workbench_state.editing || !weak->m_workbench_state.can_edit_project_colors) {
            event.Skip();
            return;
        }
        if (key == 'Z' && weak->m_plater->can_undo()) weak->m_plater->undo();
        if (key == 'Y' && weak->m_plater->can_redo()) weak->m_plater->redo();
    });
    m_plater->sidebar().Bind(wxEVT_CHILD_FOCUS, [weak](wxChildFocusEvent& event) {
        event.Skip();
        if (weak && weak->native_workspace_visible() && weak->m_native_slicing)
            weak->CallAfter([weak] { if (weak && weak->m_plater) refresh_native_sidebar(*weak->m_plater); });
    });
    m_model_generation_host->set_workbench_listener([weak](const auto& state) { if (weak) weak->apply_workbench_state(state); });
    Bind(wxEVT_IDLE, [weak](wxIdleEvent& event) {
        event.Skip();
        if (!weak || weak->m_active_page != Page::Model || weak->m_model_view != ModelView::Preview ||
            weak->m_slicing_state.official.phase != OfficialSlicePhase::Completed) return;
        // The native preview finishes loading after the completion callback.
        // Refresh only when its result changes, without re-running analysis.
        const bool outside = OrcaPrinterAdapter(weak->m_plater).preview_toolpath_outside();
        if (outside != weak->m_preview_toolpath_outside)
            weak->apply_slicing_state(weak->m_slicing_state);
    });
    m_model_generation_host->set_workbench_results_handler([weak] { if (weak) weak->show_model_view(ModelView::Result); });
    m_model_generation_host->set_workbench_return_to_design_handler([weak] {
        if (weak) weak->return_to_image_design();
    });
    m_model_generation_host->set_workbench_import_handler([weak](const auto& request) { if (weak) weak->confirm_workbench_import(request); });
    if (m_slicing_host) m_slicing_host->set_workbench_listener([weak](const auto& state) { if (weak) weak->apply_slicing_state(state); });
}

bool RedesignShell::open_model_workbench()
{
    if (m_import_in_progress || !owns_model_workflow() || !m_model_generation_host->request_open_workbench()) return false;
    navigate_to(Page::Model);
    show_model_view(ModelView::Workbench);
    return true;
}

void RedesignShell::show_model_view(ModelView view)
{
    if (!owns_model_workflow() || (m_import_in_progress && !m_import_switching_view)) return;

    // Route changes can arrive synchronously from both the button handler and
    // the model-generation state listener. Keep an already selected view
    // idempotent so a nested callback cannot repeatedly mutate the wx layout
    // while the original event is still being dispatched.
    if (m_active_page == Page::Model && m_model_view == view) {
        refresh_workflow_layout();
        if (view == ModelView::Result)
            update_model_page(m_model_generation_state);
        return;
    }

    m_model_view = view;
    m_active_tab_id = view == ModelView::Preview ? TAB_ID_PREVIEW :
        view == ModelView::Slicing ? TAB_ID_PREPARE : TAB_ID_GENERATE_3D;
    if (m_slicing_host) m_slicing_host->set_workbench_active(view == ModelView::Slicing || view == ModelView::Preview);
    if (view == ModelView::Slicing || view == ModelView::Preview) {
        m_plater->select_view_3D(view == ModelView::Preview ? "Preview" : "3D");
        m_plater->get_current_canvas3D()->use_workbench_appearance();
        m_plater->collapse_sidebar(!m_native_slicing);
        if (m_native_slicing) {
            if (auto* parameters = wxGetApp().params_panel()) parameters->OnActivate();
            refresh_native_sidebar(*m_plater);
        }
    }
    refresh_workflow_layout();
    if (view == ModelView::Result) update_model_page(m_model_generation_state);
}

void RedesignShell::refresh_workflow_layout()
{
    if (!owns_model_workflow()) return;
    const bool model = m_active_page == Page::Model;
    const bool slicing = model && (m_model_view == ModelView::Slicing || m_model_view == ModelView::Preview);
    m_model_page->Show(model && m_model_view == ModelView::Result);
    m_workbench_page->Show(model && m_model_view == ModelView::Workbench);
    m_slicing_page->Show(slicing);
    m_plater->Show(slicing);
    m_slicing_settings->Show(!m_native_slicing);
    m_native_slice_commands->Show(m_native_slicing);
    m_native_slice_start->SetLabel(m_model_view == ModelView::Preview ? _L("返回切片参数") : _L("开始切片"));
    m_native_slice_start->Enable(m_model_view == ModelView::Preview || m_slice_start->IsEnabled());
    m_plater->get_current_canvas3D()->enable_render(slicing);
    if (slicing) m_plater->get_current_canvas3D()->set_as_dirty();
    m_slice_export->Show(m_model_view == ModelView::Preview);
    m_slice_print->Show(m_model_view == ModelView::Preview);
    m_native_slice_print->Show(m_model_view == ModelView::Preview);
    m_slice_start->Show(m_model_view != ModelView::Preview);
    m_return_slice->Show(m_model_view == ModelView::Preview);
    if (m_slicing_host) m_slicing_host->set_workbench_active(slicing);
    m_content_host->Layout();
    Layout();
    if (m_import_in_progress && m_import_loading) {
        m_import_loading->Raise();
        m_import_loading->Refresh(false);
        m_import_loading->Update();
    }
}

void RedesignShell::apply_workbench_state(const PostGenerationWorkbenchState& state)
{
    m_workbench_state = state;
    refresh_project_save_actions();
    if (m_model_back_to_design_button && m_model_back_to_design_button->IsShown())
        m_model_back_to_design_button->Enable(can_return_to_image_design());
    if (m_image_page_view_only)
        apply_model_generation_state(m_model_generation_state);
    if (m_slice_check_status) {
        set_wrapped_label(m_slice_check_status, state.check.summary.empty()
            ? _L("未执行检查") : wxString::FromUTF8(state.check.summary), FromDIP(246));
    }
    if (!m_pending_workbench_job.empty() && state.asset_id == m_pending_workbench_job && state.actions.can_edit) {
        m_pending_workbench_job.clear();
        open_model_workbench();
    }
}

bool RedesignShell::can_return_to_image_design() const
{
    return model_generation_return_to_design_allowed(
        m_model_generation_state, m_workbench_state.actions,
        m_import_in_progress || m_submit_in_progress || m_model_preview_loading);
}

bool RedesignShell::return_to_image_design()
{
    if (!can_return_to_image_design())
        return false;

    if (m_workbench_state.dirty || !m_workbench_state.candidate_path.empty()) {
        if (show_redesign_confirmation(this,
                _L("当前 3D 工作台存在未保存修改或尚未接受的候选。返回图像设计后，如果开始新的生成任务，这些修改可能无法继续恢复。仍要返回吗？"),
                _L("返回图像设计"), {wxYES_NO | wxNO_DEFAULT}) != wxID_YES)
            return false;
    }

    const bool route_locked = m_model_route_locked;
    const bool view_only = m_image_page_view_only;
    const auto route_session = m_model_route_session;
    const auto route_id = m_model_route_id;
    m_model_route_locked = false;
    m_model_route_session = 0;
    m_model_route_id.clear();
    m_image_page_view_only = false;
    if (!navigate_to(Page::Image)) {
        m_model_route_locked = route_locked;
        m_image_page_view_only = view_only;
        m_model_route_session = route_session;
        m_model_route_id = route_id;
        return false;
    }
    // A missing historical style becomes a valid default only when the user
    // explicitly returns to editing. This never restarts or submits a task.
    if (current_generation_input().style != m_model_generation_state.input.style)
        synchronize_generation_input();
    apply_model_generation_state(m_model_generation_state);
    return true;
}

void RedesignShell::confirm_workbench_import(const AI::ModelImportRequest& request)
{
    if (m_import_in_progress || !m_workbench_state.actions.can_import) return;
    m_import_in_progress = true;
    m_import_started = std::chrono::steady_clock::now();
    m_import_session = std::make_shared<WorkbenchImportSession>();
    m_import_loading->SetSize(m_content_host->GetClientRect());
    m_import_elapsed->SetLabel(_L("已用时 0.0 秒"));
    m_import_loading->Show();
    m_import_loading->Raise();
    update_import_loading(WorkbenchImportPhase::Reading);
    m_import_loading->Update();
    BOOST_LOG_TRIVIAL(info) << "[WorkbenchImport] feedback_ms=" <<
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_import_started).count();
    m_workbench_page->Disable();
    m_slicing_page->Disable();
    m_import_timer.Start(100);
    wxWeakRef<RedesignShell> weak(this);
    const auto session = m_import_session;
    CallAfter([weak, session, request] {
        if (!weak || !session->valid()) return;
        if (session->cancelled()) { weak->finish_import_loading(); return; }
        try {
            const bool started = weak->m_model_generation_host && weak->m_model_generation_host->import_workbench_model_async(request, session,
                [weak, session](auto phase, const auto& message) {
                    if (weak && session->valid()) weak->update_import_loading(phase, message);
                },
                [weak, session](const auto& result) {
                    if (!weak || !session->valid()) return;
                    if (!result.imported()) {
                        weak->finish_import_loading();
                        if (result.outcome != AI::ModelImportOutcome::Cancelled)
                            wxMessageBox(wxString::FromUTF8(result.error.empty() ? "模型导入失败，工作副本已保留。" : result.error),
                                _L("模型导入"), wxOK | wxICON_ERROR, weak.get());
                        return;
                    }
                    weak->update_import_loading(WorkbenchImportPhase::UpdatingView);
                    weak->m_import_switching_view = true;
                    weak->navigate_to(Page::Model);
                    weak->show_model_view(ModelView::Slicing);
                    weak->m_import_switching_view = false;
                    auto* canvas = weak->m_plater->get_current_canvas3D();
                    weak->m_import_frame_baseline = canvas->rendered_frames();
                    weak->m_import_view_started = std::chrono::steady_clock::now();
                    weak->m_import_awaiting_frame = true;
                    canvas->set_as_dirty();
                    canvas->get_wxglcanvas()->Refresh(false);
                });
            if (!started) {
                weak->finish_import_loading();
                wxMessageBox(_L("模型或工程状态已变化，或正在执行其他处理任务。请完成当前操作后重新导入。"),
                    _L("模型导入"), wxOK | wxICON_WARNING, weak.get());
            }
        } catch (const std::exception& error) {
            if (!weak) return;
            weak->finish_import_loading();
            wxMessageBox(wxString::FromUTF8(error.what()), _L("模型导入"), wxOK | wxICON_ERROR, weak.get());
        }
    });
}

void RedesignShell::update_import_loading(WorkbenchImportPhase phase, const std::string& message)
{
    if (!m_import_in_progress) return;
    wxString label;
    switch (phase) {
    case WorkbenchImportPhase::Reading: label = _L("正在读取与验证模型…"); break;
    case WorkbenchImportPhase::Colors: label = _L("正在准备配色…"); break;
    case WorkbenchImportPhase::Placement: label = _L("正在校验模型摆放…"); break;
    case WorkbenchImportPhase::Committing: label = _L("正在加入工程…"); break;
    case WorkbenchImportPhase::UpdatingView: label = _L("正在更新模型视口…"); break;
    default: break;
    }
    if (!message.empty()) label = wxString::FromUTF8(message);
    if (!label.empty()) m_import_stage->SetLabel(label);
    m_import_cancel->Enable(m_import_session && m_import_session->can_cancel());
    m_import_loading->Layout();
    m_import_loading->Refresh(false);
    if (phase == WorkbenchImportPhase::Committing) m_import_loading->Update();
}

void RedesignShell::finish_import_loading()
{
    BOOST_LOG_TRIVIAL(info) << "[WorkbenchImport] total_ms=" <<
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_import_started).count();
    m_import_timer.Stop();
    m_import_awaiting_frame = false;
    m_import_in_progress = false;
    m_import_switching_view = false;
    if (m_import_session) m_import_session->advance(WorkbenchImportPhase::Completed);
    m_import_session.reset();
    m_import_loading->Hide();
    m_workbench_page->Enable();
    m_slicing_page->Enable();
    refresh_workflow_layout();
    if (m_slicing_host) apply_slicing_state(m_slicing_host->workbench_snapshot());
}

void RedesignShell::check_import_first_frame()
{
    if (!m_import_awaiting_frame || !m_plater) return;
    auto* canvas = m_plater->get_current_canvas3D();
    if (canvas->is_initialized() && canvas->rendered_frames() > m_import_frame_baseline) {
        finish_import_loading();
    } else if (std::chrono::steady_clock::now() - m_import_view_started > std::chrono::seconds(15)) {
        finish_import_loading();
        wxMessageBox(_L("模型已导入工程，但视口尚未完成显示。请重新进入切片页面；无需重复导入。"),
            _L("模型视口"), wxOK | wxICON_WARNING, this);
    }
}

void RedesignShell::open_print_preparation()
{
    if (m_import_in_progress || !m_print_page || !OrcaPrinterAdapter(m_plater).snapshot().gcode_ready) return;
    if (navigate_to(Page::Print)) m_print_page->show_prepare();
}

void RedesignShell::select_slicing_tab(bool native)
{
    if (m_import_in_progress || !m_slicing_host || m_slicing_state.official.phase == OfficialSlicePhase::Slicing) return;
    const bool changed = m_native_slicing != native;
    m_native_slicing = native;
    m_ai_slicing_controls->Show(!native);
    m_plater->collapse_sidebar(!native);
    if (native) {
        if (auto* parameters = wxGetApp().params_panel()) parameters->OnActivate();
        refresh_native_sidebar(*m_plater);
    }
    if (native) m_slicing_host->cancel_workbench_analysis();
    else if (changed || m_slicing_state.session.state != RecommendationSessionState::Ready) m_slicing_host->analyze_workbench();
    apply_slicing_state(m_slicing_host->workbench_snapshot());
    m_slicing_page->Layout();
    refresh_workflow_layout();
}

void RedesignShell::apply_slicing_state(const SmartSlicingWorkbenchState& state)
{
    const bool completed = state.official.phase == OfficialSlicePhase::Completed &&
        m_slicing_state.official.phase != OfficialSlicePhase::Completed;
    m_slicing_state = state;
    if (!m_slice_start) return;
    auto* plate = m_plater->get_partplate_list().get_curr_plate();
    size_t faces = 0, vertices = 0;
    const auto& objects = m_plater->model().objects;
    for (size_t i = 0; plate && i < objects.size(); ++i) {
        const auto* object = objects[i];
        if (!object) continue;
        bool on_plate = false;
        for (size_t j = 0; j < object->instances.size() && !on_plate; ++j)
            on_plate = plate->contain_instance(static_cast<int>(i), static_cast<int>(j));
        if (!on_plate) continue;
        for (const auto* volume : object->volumes) {
            if (!volume || !volume->is_model_part()) continue;
            faces += volume->mesh().facets_count();
            vertices += volume->mesh().its.vertices.size();
        }
    }
    m_slice_model_stats->SetLabel(wxString::Format(_L("面数  %zu\n顶点数  %zu"), faces, vertices));
    if (state.palette != m_slice_displayed_palette) {
        m_slice_displayed_palette = state.palette;
        m_slice_palette->Clear(true);
        for (size_t i = 0; i < state.palette.size(); ++i) {
            auto* parent = m_slice_model_stats->GetParent();
            auto* line = new wxBoxSizer(wxHORIZONTAL);
            auto* swatch = new wxPanel(parent, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(18, 18)));
            swatch->SetBackgroundColour(wxColour(wxString::FromUTF8(state.palette[i])));
            line->Add(swatch, 0, wxRIGHT, FromDIP(8));
            auto* label = new wxStaticText(parent, wxID_ANY, wxString::Format("%zu  %s", i + 1,
                wxString::FromUTF8(state.palette[i])));
            RedesignTheme::style_text(label, RedesignTheme::primary_text_colour(), 9);
            line->Add(label, 1, wxALIGN_CENTER_VERTICAL);
            m_slice_palette->Add(line, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
        }
    }
    const std::array<wxString, 3> titles {_L("综合最优"), _L("速度优先"), _L("质量优先")};
    for (size_t i = 0; i < titles.size(); ++i) {
        const auto& goal = state.session.recommendation.goal_result(RECOMMENDATION_GOALS[i]);
        wxString layer = _L("层高待分析");
        for (const auto& parameter : state.effective_parameters[i])
            if (parameter.key == "layer_height") { layer = parameter_value(parameter); break; }
        const wxString availability = !state.analyzing && !state.candidates[i] &&
            state.diagnostic == "printability_action_required" ? _L("暂不可用") :
            !state.analyzing && !state.candidates[i] && goal.status == GoalResultStatus::Analyzing
                ? _L("待分析") : goal_status(goal.status);
        m_slice_goals[i]->SetLabel(titles[i] + "\n" + layer + "\n" + availability);
        m_slice_goals[i]->Enable(state.official.phase != OfficialSlicePhase::Slicing && !state.official.workspace_mutated);
        auto* button = static_cast<WorkbenchButton*>(m_slice_goals[i]);
        button->SetBorderWidth(state.selected_goal == RECOMMENDATION_GOALS[i] ? FromDIP(1) : 0);
        button->SetBorderColor(RedesignTheme::accent_colour());
    }
    const size_t selected = static_cast<size_t>(state.selected_goal);
    set_wrapped_label(m_slice_details, state.candidates[selected]
        ? candidate_details(state.effective_parameters[selected], state.metrics[selected]) : _L("暂无可用方案"), FromDIP(230));
    wxString status;
    if (state.analyzing) status = state.session.state == RecommendationSessionState::Canceled
        ? _L("正在取消分析…") : _L("正在分析三种方案…");
    else if (state.diagnostic == "printability_action_required") {
        status = _L("请处理以下问题后重新分析：");
        if (state.preflight) for (const auto& issue : state.preflight->issues) {
            if (!issue.blocks_trial_slice) continue;
            switch (issue.code) {
            case IssueCode::OpenMesh: status += _L("\n网格存在开放边：返回美颜修复，或明确保留当前网格。"); break;
            case IssueCode::OutsideBuildVolume: status += _L("\n模型超出打印空间：请调整尺寸和摆放。"); break;
            case IssueCode::UnsupportedMaterialCombination:
                status += _L("\n耗材兼容性未通过验证：") + capability_reasons(state.material_reasons);
                break;
            case IssueCode::IncompatiblePhysicalSlots: status += _L("\n耗材不兼容：请在 Orca 原生页选择兼容耗材。"); break;
            case IssueCode::MachineCapabilityUnavailable:
                status += _L("\nAI 机型能力未通过验证：") + capability_reasons(state.machine_reasons) +
                    _L("。可使用 Orca 原生切片。");
                break;
            default: status += "\n" + wxString::FromUTF8(issue.evidence); break;
            }
        }
    } else if (state.diagnostic == "workspace_changed") status = _L("工程已变化，请重新分析。");
    else if (!state.diagnostic.empty()) status = _L("分析未完成：") + wxString::FromUTF8(state.diagnostic);
    if (state.official.phase == OfficialSlicePhase::Slicing) status = _L("正在正式切片…");
    else if (state.official.phase == OfficialSlicePhase::Completed) status = _L("切片完成");
    else if (!state.official.diagnostic_code.empty()) {
        const auto& diagnostic = state.official.diagnostic_code;
        wxString detail;
        if (diagnostic == "workspace_changed") detail = _L("工程已变化，请重新切片。");
        else if (diagnostic == "official_slice_canceled") detail = _L("切片已取消。");
        else if (diagnostic == "official_slice_not_started") detail = _L("未能开始切片，请检查模型与打印配置。");
        else if (diagnostic == "official_slice_failed") detail = _L("切片失败，请检查模型与打印配置。");
        else detail = wxString::FromUTF8(diagnostic);
        status = (state.official.workspace_mutated ? _L("方案已应用。") : wxString()) + detail;
    }
    m_preview_toolpath_outside = OrcaPrinterAdapter(m_plater).preview_toolpath_outside();
    if (state.official.phase == OfficialSlicePhase::Completed && plate) {
        const auto* result = plate->get_slice_result();
        if (m_preview_toolpath_outside || (result && result->toolpath_outside))
            status = _L("路径超出热床，请调整模型或擦料塔位置后重新切片。");
        else if (!plate->is_slice_result_ready_for_export())
            status = _L("切片完成，导出前请处理预览中的错误。");
    }
    set_wrapped_label(m_slice_status, status.empty() ? _L("准备切片") : status, FromDIP(246));
    set_wrapped_label(m_native_slice_status, m_native_slicing && state.official.phase == OfficialSlicePhase::Rejected &&
        state.official.diagnostic_code.empty() ? _L("准备切片") : status, FromDIP(290));
    m_slice_keep_mesh->Show(state.can_keep_current_mesh && !state.analyzing);
    const bool running = state.official.phase == OfficialSlicePhase::Slicing;
    const auto route = workbench_slice_route(m_native_slicing, running || m_import_in_progress,
        state.can_retry, state.can_start, state.can_start_native);
    if (!m_native_slicing && route == WorkbenchSliceRoute::Native) {
        status += (status.empty() ? wxString() : "\n") + _L("将使用当前工程参数切片。");
        set_wrapped_label(m_slice_status, status, FromDIP(246));
    } else if (route == WorkbenchSliceRoute::Unavailable && !running && !state.native_blocked_reason.empty()) {
        status += (status.empty() ? wxString() : "\n") + wxString::FromUTF8(state.native_blocked_reason);
        set_wrapped_label(m_slice_status, status, FromDIP(246));
    }
    m_slice_start->Enable(route != WorkbenchSliceRoute::Unavailable);
    m_slice_start->SetLabel(state.can_retry && !m_native_slicing ? _L("重试切片") : _L("开始切片"));
    m_native_slice_start->Enable(!running && (m_model_view == ModelView::Preview || m_slice_start->IsEnabled()));
    m_slice_analyze->Enable(state.can_analyze);
    m_slice_cancel->Show(state.analyzing);
    m_ai_slicing_tab->Enable(!running);
    m_native_slicing_tab->Enable(!running);
    m_slice_export->Enable(OrcaPrinterAdapter(m_plater).snapshot().gcode_ready);
    m_native_slice_export->Enable(m_slice_export->IsEnabled());
    refresh_project_save_actions();
    const bool can_open_print = !m_import_in_progress && OrcaPrinterAdapter(m_plater).snapshot().gcode_ready;
    m_slice_print->Enable(can_open_print);
    m_native_slice_print->Enable(can_open_print);
    if (completed && m_model_view == ModelView::Slicing && m_active_page == Page::Model)
        show_model_view(ModelView::Preview);
    m_slice_status->GetParent()->Layout();
    if (auto* scroll = dynamic_cast<wxScrolledWindow*>(m_slice_status->GetParent())) scroll->FitInside();
    m_native_slice_commands->Layout();
}

bool RedesignShell::can_save_print_project() const
{
    return print_project_save_available(m_plater && !m_plater->model().objects.empty(), m_import_in_progress,
        m_slicing_state.official.phase == AI::SmartSlicing::OfficialSlicePhase::Slicing,
        m_workbench_state.dirty, m_workbench_state.actions.status);
}

void RedesignShell::refresh_project_save_actions()
{
    const bool available = can_save_print_project();
    if (m_assets_workspace) m_assets_workspace->set_project_available(available);
    if (m_slice_save) m_slice_save->Enable(available);
    if (m_native_slice_save) m_native_slice_save->Enable(available);
}

void RedesignShell::save_print_project()
{
    if (!can_save_print_project()) {
        wxMessageBox(_L("请先保存美颜结果并导入切片工程，或等待当前操作完成。"),
            _L("保存打印工程"), wxOK | wxICON_INFORMATION, this);
        return;
    }
    // The native Save As command owns 3MF serialization, overwrite confirmation,
    // preset metadata and the project's saved/dirty state.
    try {
        m_plater->save_project(true);
    } catch (const boost::filesystem::filesystem_error&) {
        wxMessageBox(_L("无法写入安装目录下的 models/projects 文件夹，请检查写入权限。"),
            _L("保存打印工程"), wxOK | wxICON_ERROR, this);
    }
    refresh_project_save_actions();
}

void RedesignShell::start_workbench_slice()
{
    if (m_import_in_progress || !m_slicing_host) return;
    const auto reviewed = m_slicing_host->workbench_snapshot();
    const auto route = workbench_slice_route(m_native_slicing, reviewed.official.phase == OfficialSlicePhase::Slicing,
        reviewed.can_retry, reviewed.can_start, reviewed.can_start_native);
    if (route == WorkbenchSliceRoute::Unavailable) { apply_slicing_state(reviewed); return; }
    if (route == WorkbenchSliceRoute::Native) {
        BOOST_LOG_TRIVIAL(info) << "[WorkbenchSlice] native=" << m_native_slicing << " fallback_reason=" << reviewed.diagnostic;
        m_slicing_host->start_native_slice();
        return;
    }
    std::vector<RiskConfirmationKind> confirmations;
    const auto& goal = reviewed.session.recommendation.goal_result(reviewed.selected_goal);
    if (goal.risk_confirmation_contract) confirmations = goal.risk_confirmation_contract->required_confirmations();
    if (!confirmations.empty()) {
        wxString risks;
        for (auto risk : confirmations) risks += risk == RiskConfirmationKind::ProtectedRegionSupportContact ?
            _L("保护区域将接触支撑。\n") : _L("保护区域将出现接缝。\n");
        if (show_redesign_confirmation(this, risks, _L("切片风险确认"), {wxYES_NO | wxNO_DEFAULT}) != wxID_YES) return;
    }
    const auto result = m_slicing_host->start_workbench_slice(reviewed, confirmations);
    if (result.phase == OfficialSlicePhase::Rejected)
        wxMessageBox(wxString::FromUTF8(result.diagnostic_code), _L("切片方案已变化"), wxOK | wxICON_WARNING, this);
}
}
