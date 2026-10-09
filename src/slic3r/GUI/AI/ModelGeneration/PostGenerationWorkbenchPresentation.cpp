#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "ModelGenerationPresentation.hpp"
#include "ModelPreview3D.hpp"
#include "BeautyWorkbenchControls.hpp"
#include "BeautyWorkbenchTransactionController.hpp"
#include "WorkbenchStyle.hpp"
#include "slic3r/GUI/AI/Model/BeautyDocument.hpp"
#include "slic3r/GUI/AI/Model/BeautySurface.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/Widgets/TextInput.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "slic3r/GUI/I18N.hpp"
#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/filedlg.h>
#include <wx/notebook.h>
#include <wx/numformatter.h>
#include <wx/msgdlg.h>
#include <wx/menu.h>
#include <wx/textctrl.h>
#include <wx/tglbtn.h>
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/weakref.h>
#include <wx/wrapsizer.h>
#include <wx/wupdlock.h>

namespace Slic3r::GUI {
using namespace ModelGenerationPresentation;
namespace {
constexpr int beauty_finishing_tools[] = {4, 1, 3, 5, 1};
// Native wrapping may keep an entire CJK sentence as one word. Measure the
// displayed text so a narrow tool panel never truncates its instructions.
void wrap_workbench_text(wxStaticText* label, int width, bool preserve_lines = false)
{
    wxString source = label->GetLabel(), line, result;
    source.Replace("\r", "");
    if (!preserve_lines) source.Replace("\n", "");
    for (wxUniChar character : source) {
        if (character == '\n') {
            result += line + "\n"; line.clear();
            continue;
        }
        wxString next = line; next += character;
        if (!line.empty() && label->GetTextExtent(next).x > width) {
            result += line + "\n"; line.clear();
        }
        line += character;
    }
    const wxString wrapped = result + line;
    label->SetLabel(wrapped);
    wxClientDC dc(label);
    dc.SetFont(label->GetFont());
    label->SetMinSize(wxSize(1, dc.GetMultiLineTextExtent(wrapped).y + label->FromDIP(2)));
}

void apply_workbench_theme(wxWindow* window, bool enabled,
                           const wxColour& enabled_surface = wxColour(32, 32, 35))
{
    if (!window) return;
    const wxColour surface = enabled ? enabled_surface : *wxWHITE;
    const wxColour text = enabled ? wxColour(238, 240, 244) : wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
    window->SetBackgroundColour(surface);
    window->SetForegroundColour(text);
    for (wxWindow* child : window->GetChildren())
        apply_workbench_theme(child, enabled, enabled_surface);
    if (auto* toggle = dynamic_cast<WorkbenchSwitch*>(window)) toggle->rescale_workbench();
}

void apply_workbench_parameter_theme(wxWindow* parameters)
{
    apply_workbench_theme(parameters, true, wxColour(40, 40, 43));
}

void repaint_workbench_surface(wxWindow* window)
{
    if (!window || !window->IsShownOnScreen() || window->IsFrozen()) return;
    window->Refresh();
    window->Update();
    for (wxWindow* child : window->GetChildren())
        repaint_workbench_surface(child);
}
}
void ModelGenerationPanel::rescale_post_generation_workbench()
{
    if (!m_workbench_shell) return;
    const auto rescale = [](auto&& self, wxWindow* window) -> void {
        if (!window) return;
        window->SetFont(window->GetFont());
        if (auto* button = dynamic_cast<WorkbenchButton*>(window)) button->rescale_workbench();
        else if (auto* toggle = dynamic_cast<WorkbenchSwitch*>(window)) toggle->rescale_workbench();
        else if (auto* button = dynamic_cast<Button*>(window)) button->Rescale();
        if (auto* choice = dynamic_cast<ComboBox*>(window)) choice->Rescale();
        else if (auto* input = dynamic_cast<TextInput*>(window)) input->Rescale();
        if (auto* panel = dynamic_cast<WorkbenchPanel*>(window)) panel->rescale_workbench();
        for (wxWindow* child : window->GetChildren()) self(self, child);
        window->InvalidateBestSize();
    };
    rescale(rescale, m_workbench_shell);
    rescale(rescale, m_workbench_parameters);
    rescale(rescale, m_workbench_model_info);
    rescale(rescale, m_workbench_history_toggle);
    if (m_workbench_logo) {
        m_workbench_logo->SetBitmap(create_scaled_bitmap("workbench_logo", m_workbench_logo, 36));
        m_workbench_logo->SetMinSize(FromDIP(wxSize(36, 36)));
    }
    for (auto* info : {m_workbench_check_info, m_workbench_beauty_info}) {
        info->SetBitmap(create_scaled_bitmap("workbench_info", info, 12));
        info->SetMinSize(FromDIP(wxSize(12, 12)));
    }
    m_workbench_parameter_divider->SetBitmap(create_scaled_bitmap(
        "workbench_parameter_divider", m_workbench_parameter_divider, 12));
    m_workbench_model_name->SetMinSize(FromDIP(wxSize(100, 24)));
    m_workbench_model_status->SetMinSize(FromDIP(wxSize(100, 24)));
    auto* search = static_cast<TextInput*>(m_workbench_history_search->GetParent());
    auto search_icon = ScalableBitmap(search, "workbench_search", 12).bmp().ConvertToImage();
    search_icon.Replace(0, 0, 0, 235, 235, 235);
    search->SetIcon(wxBitmap(search_icon));
    search->SetCornerRadius(FromDIP(13));
    search->SetMinSize(FromDIP(wxSize(180, 26)));
    search->SetMaxSize(FromDIP(wxSize(-1, 26)));
    auto* search_text = search->GetTextCtrl();
    search_text->InvalidateBestSize();
    const int text_height = search_text->GetBestSize().y;
    search_text->SetMinSize(wxSize(-1, text_height));
    search_text->SetSize(wxSize(search_text->GetSize().x, text_height));
    search->SetSize(wxSize(search->GetSize().x, FromDIP(26)));
    m_workbench_settings->SetMinSize(FromDIP(wxSize(280, -1)));
    m_workbench_history_panel->SetMinSize(FromDIP(wxSize(280, 200)));
    static_cast<wxScrolledWindow*>(m_workbench_shell)->SetScrollRate(FromDIP(12), 0);
    if (m_finishing_workbench) {
        m_model_preview->GetParent()->SetMinSize(FromDIP(wxSize(640, 480)));
        m_model_preview->SetMinSize(FromDIP(wxSize(640, 480)));
    }
    refresh_workbench_history();
    refresh_post_generation_workbench();
    Layout();
    Refresh(false);
}

void ModelGenerationPanel::layout_workbench_parameters()
{
    if (!m_workbench_parameters) return;
    const bool visible = m_finishing_workbench && m_workbench_editing;
    auto* overlay_host = m_model_preview->workbench_overlay_parent();
    // The reference viewport is about 778 DIP wide at the workbench scale.
    // Keep its floating controls there, while docking at the 640 DIP minimum.
    const bool dock_parameters = overlay_host->GetClientSize().x < FromDIP(720);
    static_cast<WorkbenchScrolledWindow*>(m_workbench_parameters)->set_rounded_corners(!dock_parameters);
    wxWindow* parameter_parent = dock_parameters ? m_finishing_panel : overlay_host;
    if (m_workbench_parameters->GetParent() != parameter_parent) {
        if (auto* sizer = m_workbench_parameters->GetContainingSizer())
            sizer->Detach(m_workbench_parameters);
        m_workbench_parameters->Reparent(parameter_parent);
        if (dock_parameters)
            m_finishing_panel->GetSizer()->Insert(1, m_workbench_parameters, 0, wxEXPAND | wxBOTTOM, FromDIP(12));
    }
    m_workbench_parameters->Show(visible);
    const auto host_size = overlay_host->GetClientSize();
    const int inset = FromDIP(12);
    const int right_inset = FromDIP(40);
    const wxSize toggle_size = FromDIP(wxSize(19, 98));
    m_workbench_history_toggle->Show(m_finishing_workbench);
    m_workbench_history_toggle->SetSize(std::max(0, host_size.x - toggle_size.x),
        std::max(0, (host_size.y - toggle_size.y) / 2), toggle_size.x, toggle_size.y);
    m_workbench_history_toggle->Raise();
    const wxSize info_size = m_workbench_model_info->GetSizer()->CalcMin();
    const int info_width = std::max(FromDIP(220), info_size.x);
    const bool show_info = m_finishing_workbench && m_model_preview_ready &&
        host_size.x >= info_width + 2 * inset &&
        (!visible || dock_parameters || host_size.x >= info_width + FromDIP(248) + 2 * inset + right_inset);
    m_workbench_model_info->Show(show_info);
    if (show_info) {
        m_workbench_model_info->SetSize(inset, inset, info_width, info_size.y);
        m_workbench_model_info->Layout();
        m_workbench_model_info->Raise();
    }
    if (!visible) return;
    auto* surface = static_cast<wxScrolledWindow*>(m_workbench_parameters);
    if (dock_parameters) {
        surface->SetMinSize(wxSize(-1, surface->GetSizer()->CalcMin().y));
        m_finishing_panel->Layout();
        static_cast<wxScrolledWindow*>(m_finishing_panel)->FitInside();
        surface->Layout();
        surface->FitInside();
        return;
    }
    surface->SetMinSize(wxDefaultSize);
    const int width = std::min(FromDIP(248), std::max(FromDIP(200), host_size.x - inset - right_inset));
    const int content_height = surface->GetSizer()->CalcMin().y;
    const int height = std::min(content_height, std::max(FromDIP(120), host_size.y - 2 * inset));
    surface->SetSize(std::max(inset, host_size.x - width - right_inset), inset, width, height);
    surface->Layout();
    surface->FitInside();
    surface->Raise();
}

void ModelGenerationPanel::refresh_post_generation_workbench()
{
    if (!m_workbench_shell || !m_finishing_workbench || m_refreshing_workbench_layout) return;
    // Layout sends synchronous size events back to this refresh function.
    m_refreshing_workbench_layout = true;
    const auto state = post_generation_ui_state();
    const auto title_path = !m_finishing_candidate.empty() && !m_finishing_source.empty()
        ? m_finishing_source : m_displayed_model_path;
    wxString model_name = title_path.empty() ? _L("未加载模型")
        : wxString(title_path.filename().wstring());
    bool accepted = !m_finishing_accepted_path.empty() && title_path == m_finishing_accepted_path;
    for (const auto& entry : m_library_entries) {
        if (entry.model_path != title_path) continue;
        if (!entry.title.empty()) model_name = entry.title;
        accepted = accepted || entry.accepted_finishing;
        break;
    }
    wxString model_status;
    switch (state.status) {
    case PostGenerationUiState::Status::Empty: model_status = _L("未加载"); break;
    case PostGenerationUiState::Status::Loading: model_status = _L("加载中"); break;
    case PostGenerationUiState::Status::Ready: model_status = accepted ? _L("已接受") : _L("就绪"); break;
    case PostGenerationUiState::Status::Editing: model_status = _L("编辑中"); break;
    case PostGenerationUiState::Status::Processing: model_status = _L("处理中"); break;
    case PostGenerationUiState::Status::CandidateReady: model_status = _L("候选就绪"); break;
    case PostGenerationUiState::Status::ComparingBefore: model_status = _L("处理前"); break;
    case PostGenerationUiState::Status::Error: model_status = _L("加载失败"); break;
    }
    m_workbench_model_name->SetLabel(model_name);
    m_workbench_model_name->SetToolTip(model_name);
    m_workbench_model_status->SetLabel(model_status);
    m_workbench_model_status->SetToolTip(model_status);
    const bool show_status = m_workbench_load_error || m_preview_loading;
    m_workbench_state_status->Show(show_status);
    if (show_status) {
        m_workbench_state_status->SetLabel(m_status->GetLabel());
        m_workbench_state_status->SetToolTip(m_workbench_load_error ? m_result_summary->GetLabel() : wxString());
        wrap_workbench_text(m_workbench_state_status,
            std::max(FromDIP(200), m_workbench_view_host->GetClientSize().x - FromDIP(16)));
    }
    if (m_workbench_settings_editing != m_workbench_editing) {
        // Move whole sizer groups, keeping their controls and Beauty session alive.
        for (size_t index = 1; index < m_workbench_settings_groups.size(); ++index)
            m_workbench_settings_sections->Detach(m_workbench_settings_groups[index]);
        for (size_t index = 0; index < 2; ++index)
            m_workbench_settings_sections->Insert(index,
                m_workbench_settings_groups[m_workbench_editing ? 2 - index : 1 + index], 0, wxEXPAND);
        m_workbench_settings_editing = m_workbench_editing;
        m_workbench_beauty_heading->SetLabel(m_workbench_editing ? _L("模型美化") : _L("3D 美颜"));
        m_workbench_palette_heading->SetLabel(m_workbench_editing ? _L("目标色数") : _L("多色模型"));
        m_workbench_base->Show(!m_workbench_editing);
        m_workbench_settings_scroll->Scroll(-1, 0);
    }
    m_workbench_settings->Show();
    m_finishing_panel->Show(m_workbench_editing);
    m_workbench_footer->Show(m_workbench_editing || m_finishing_running || !m_finishing_candidate.empty() ||
                            (m_beauty_transactions && m_beauty_transactions->processing()));
    const bool history_was_shown = m_workbench_history_panel->IsShown();
    m_workbench_history_panel->Show(!m_workbench_editing && !m_workbench_history_collapsed);
    const wxString history_action = m_workbench_editing ? _L("返回历史模型") :
        m_workbench_history_panel->IsShown() ? _L("收起历史模型") : _L("展开历史模型");
    m_workbench_history_toggle->SetName(history_action);
    m_workbench_history_toggle->SetToolTip(history_action);
    static_cast<WorkbenchButton*>(m_workbench_history_toggle)->set_drawer_open(
        m_workbench_editing || m_workbench_history_panel->IsShown());
    m_workbench_edit->Enable(m_model_preview_ready || m_finishing_running);
    const auto snapshot = workbench_snapshot();
    if (m_workbench_return_to_design_button)
        m_workbench_return_to_design_button->Enable(post_generation_return_to_design_allowed(state));
    m_workbench_check->SetLabel(m_workbench_check_running ? _L("取消") : _L("检查"));
    m_workbench_check->Enable(m_workbench_check_running || (state.can_edit && m_finishing_candidate.empty() && !snapshot.dirty));
    for (wxWindow* color : m_workbench_project_colors->GetChildren()) color->Enable(snapshot.can_edit_project_colors);
    m_workbench_palette_details->Enable(state.can_edit && m_finishing_candidate.empty());
    m_model_preview->set_workbench_palette_editable(state.can_edit && m_finishing_candidate.empty());
    m_workbench_original->SetValue(m_model_preview->beauty_original_view());
    m_workbench_edit->SetLabel(m_workbench_editing ? _L("一键美化") : _L("3D 美颜工作台"));
    m_workbench_print->Disable();
    const bool can_import = snapshot.can_import_for_slicing && is_nonempty_model(m_displayed_model_path);
    if (auto* portrait = FindWindowByName("portrait_r6_entry", m_workbench_shell)) {
        portrait->Enable(snapshot.actions.can_edit && snapshot.portrait_available);
        portrait->SetLabel(snapshot.portrait_enabled ? _L("退出人像保护（R6）") : _L("人像保护（R6）"));
        portrait->SetToolTip(snapshot.portrait_available ? _L("可选人像保护；退出保留已保存的保护边界。") :
            wxString::FromUTF8(snapshot.portrait_unavailable_reason));
    }
    if (m_workbench_print_navigation)
        m_workbench_print_navigation->Enable(can_import);
    m_workbench_slicing->Enable(can_import);
    m_workbench_settings_scroll->Layout();
    if (wxGetApp().preset_bundle) {
        const auto config = wxGetApp().preset_bundle->full_config();
        size_t index = 0;
        for (const auto& item : std::array<std::pair<const char*, wxString>, 5> {{
                 {"layer_height", _L("层高")}, {"wall_loops", _L("墙层")},
                 {"sparse_infill_density", _L("填充率")}, {"outer_wall_speed", _L("外墙速度")},
                 {"enable_support", _L("支撑")} }}) {
            const auto* option = config.option(item.first);
            wxString value = option ? wxString::FromUTF8(option->serialize()) : "--";
            if (const auto* toggle = dynamic_cast<const ConfigOptionBool*>(option))
                value = toggle->value ? _L("开启") : _L("关闭");
            else if (option && std::string(item.first) == "layer_height") value += " mm";
            else if (option && std::string(item.first) == "wall_loops") value += _L(" 层");
            else if (option && std::string(item.first) == "outer_wall_speed") {
                if (const auto* speeds = dynamic_cast<const ConfigOptionFloats*>(option); speeds && !speeds->values.empty())
                    value = wxString::Format("%.0f", speeds->values.front());
                else if (const auto* speeds = dynamic_cast<const ConfigOptionFloatsNullable*>(option); speeds && !speeds->values.empty())
                    value = wxString::Format("%.0f", speeds->values.front());
                value += " mm/s";
            }
            m_workbench_slice_values[index++]->SetLabel(value);
        }
    }
    m_workbench_slice_values[5]->SetLabel(_L("未切片"));
    m_workbench_slice_values[6]->SetLabel(_L("未切片"));
    if (auto* plater = wxGetApp().plater(); plater &&
        !m_displayed_model_path.empty() && m_last_imported_model_path == m_displayed_model_path &&
        !plater->is_background_process_slicing()) {
        auto* plate = plater->get_partplate_list().get_curr_plate();
        if (plate && plate->is_slice_result_valid() && plate->fff_print()) {
            // GLB imports use a content-addressed OBJ; unrelated plate results
            // must never be presented as estimates for the displayed asset.
            const bool glb = AI::model_artifact_format(m_displayed_model_path) == "glb";
            const std::string hash = glb ? AI::model_artifact_sha256(m_displayed_model_path) : std::string();
            const std::string imported_name = hash.empty() ? std::string() : "orcaslicer-ai-glb-" + hash + ".obj";
            bool matching_source = false;
            for (const auto* object : plate->get_objects_on_this_plate()) {
                if (!object) continue;
                boost::system::error_code error;
                const boost::filesystem::path source(object->input_file);
                matching_source = glb ? !imported_name.empty() && source.filename().string() == imported_name
                    : !source.empty() && boost::filesystem::equivalent(source, m_displayed_model_path, error) && !error;
                if (matching_source) break;
            }
            if (matching_source) {
                const auto& statistics = plate->fff_print()->print_statistics();
                if (!statistics.estimated_normal_print_time.empty())
                    m_workbench_slice_values[5]->SetLabel(wxString::FromUTF8(statistics.estimated_normal_print_time));
                if (statistics.total_used_filament > 0.0)
                    m_workbench_slice_values[6]->SetLabel(wxString::Format("%.1f m / %.0f g",
                        statistics.total_used_filament / 1000.0, statistics.total_weight));
            }
        }
    }
    m_workbench_check->SetToolTip(_L("检查拓扑并执行支持的安全修复；壁厚、悬垂和自交未检查。"));
    wxString check_text;
    switch (snapshot.check.status) {
    case WorkbenchCheckStatus::NotRun: check_text = _L("尚未检查"); break;
    case WorkbenchCheckStatus::Running:
        check_text = snapshot.check.phase == WorkbenchCheckPhase::Repairing ? _L("安全修复中…") :
            snapshot.check.phase == WorkbenchCheckPhase::Rechecking ? _L("复检中…") : _L("检查中…");
        break;
    case WorkbenchCheckStatus::Normal: check_text = _L("拓扑正常"); break;
    case WorkbenchCheckStatus::Attention: check_text = _L("需注意"); break;
    case WorkbenchCheckStatus::Invalid: check_text = _L("异常"); break;
    case WorkbenchCheckStatus::Failed: check_text = _L("检查失败"); break;
    }
    if (!snapshot.check.summary.empty()) check_text += "\n" + from_u8(snapshot.check.summary);
    if (!snapshot.check.repair_reason.empty()) check_text += "\n" + from_u8(snapshot.check.repair_reason);
    m_workbench_check_status->SetToolTip(check_text);
    m_workbench_check_status->SetLabel(check_text);
    wrap_workbench_text(m_workbench_check_status, FromDIP(242), true);
    const auto trial = m_model_preview->color_trial_state();
    m_workbench_palette_status->SetLabel(wxString::Format(_L("当前色卡 · %llu 色"),
        static_cast<unsigned long long>(trial.colors.size())));
    m_comparison_panel->Layout();
    m_workbench_view_host->Layout();
    m_workbench_shell->Layout();
    static_cast<wxScrolledWindow*>(m_workbench_shell)->FitInside();
    m_workbench_parameter_faces->SetLabel(wxNumberFormatter::ToString(
        static_cast<wxLongLong_t>(m_model_preview->triangle_count())));
    const bool candidate_ready = !m_finishing_candidate.empty() && m_finishing_result.success &&
        m_model_preview_ready && !m_preview_loading && !m_finishing_running;
    m_workbench_parameter_output->SetLabel(candidate_ready
        ? wxNumberFormatter::ToString(static_cast<wxLongLong_t>(m_finishing_result.faces_after)) : "--");
    m_workbench_model_info_values[0]->SetLabel(_L("三角面"));
    m_workbench_model_info_values[1]->SetLabel(wxString::Format("%llu",
        static_cast<unsigned long long>(m_model_preview->triangle_count())));
    m_workbench_model_info_values[2]->SetLabel(wxString::Format("%llu",
        static_cast<unsigned long long>(m_model_preview->vertex_count())));
    const auto& dimensions = m_model_preview->model_dimensions();
    m_workbench_model_info_values[3]->SetLabel(wxString::Format(wxString::FromUTF8("%.1f × %.1f × %.1f mm"),
        dimensions.x(), dimensions.y(), dimensions.z()));
    layout_workbench_parameters();
    if (!history_was_shown && m_workbench_history_scroller->IsShownOnScreen())
        request_library_thumbnails(true);
    publish_workbench_state();
    m_refreshing_workbench_layout = false;
}
} // namespace Slic3r::GUI
