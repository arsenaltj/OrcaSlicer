#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/AI/Orca/OrcaFilamentSelection.hpp"
#include "ModelGenerationPresentation.hpp"
#include "ModelPreview3D.hpp"
#include "BeautyWorkbenchControls.hpp"
#include "BeautyWorkbenchTransactionController.hpp"
#include "WorkbenchStyle.hpp"
#include "slic3r/GUI/Redesign/RedesignFeatureFlags.hpp"
#include "slic3r/GUI/AI/Model/BeautyDocument.hpp"
#include "slic3r/GUI/AI/Model/BeautySurface.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/AI/Orca/OrcaFilamentSelection.hpp"
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
#include <wx/gauge.h>
#include <wx/notebook.h>
#include <wx/numformatter.h>
#include <wx/msgdlg.h>
#include <wx/menu.h>
#include <wx/textctrl.h>
#include <wx/textdlg.h>
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
wxWindow* ModelGenerationPanel::build_post_generation_workbench(wxWindow* parent)
{
    auto* shell = new wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                       wxHSCROLL | wxBORDER_NONE | wxCLIP_CHILDREN);
    m_workbench_shell = shell;
    // Preserve the viewport and rail widths; narrow windows scroll the body.
    shell->SetMinSize(wxSize(1, 1));
    shell->SetScrollRate(FromDIP(12), 0);
    shell->ShowScrollbars(wxSHOW_SB_DEFAULT, wxSHOW_SB_NEVER);
    shell->SetName("ai_content_color");
    shell->SetBackgroundColour(wxColour(49, 49, 54));
    shell->SetFont(wxFontInfo(9).FaceName("HONOR Sans Design"));
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    auto command = [this](wxWindow* owner, wxSizer* sizer, const wxString& label, auto action) {
        auto* button = new WorkbenchButton(owner, label);
        button->SetCornerRadius(FromDIP(9));
        button->SetBorderWidth(0);
        button->SetBackgroundColor(StateColor(std::pair<wxColour, int>(wxColour(48, 48, 52), StateColor::Hovered),
            std::pair<wxColour, int>(wxColour(22, 22, 25), StateColor::Normal)));
        button->SetTextColor(StateColor(std::pair<wxColour, int>(wxColour(105, 105, 110), StateColor::Disabled),
            std::pair<wxColour, int>(wxColour(235, 235, 235), StateColor::Normal)));
        button->SetMinSize(FromDIP(wxSize(-1, 38)));
        button->Bind(wxEVT_BUTTON, action);
        sizer->Add(button, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
        return button;
    };
    auto apply_slice_action_style = [this](WorkbenchButton* button) {
        if (!button) return;
        button->SetMinSize(FromDIP(wxSize(-1, 28)));
        button->SetMaxSize(FromDIP(wxSize(-1, 28)));
        button->SetCornerRadius(FromDIP(9));
        button->SetBorderWidth(0);
        button->SetBackgroundColor(StateColor(
            std::pair<wxColour, int>(wxColour(48, 48, 52), StateColor::Disabled),
            std::pair<wxColour, int>(wxColour(94, 94, 96), StateColor::Hovered),
            std::pair<wxColour, int>(wxColour(77, 77, 79), StateColor::Normal)));
        button->SetTextColor(StateColor(
            std::pair<wxColour, int>(wxColour(105, 105, 110), StateColor::Disabled),
            std::pair<wxColour, int>(wxColour(235, 235, 235), StateColor::Normal)));
    };

    auto* settings = new WorkbenchPanel(shell);
    m_workbench_settings = settings;
    settings->SetMinSize(FromDIP(wxSize(280, -1)));
    settings->SetBackgroundColour(wxColour(32, 32, 35));
    auto* settings_root = new wxBoxSizer(wxVERTICAL);
    auto* check_host = m_workbench_check_host = new wxPanel(settings);
    check_host->SetBackgroundColour(settings->GetBackgroundColour());
    auto* check_contents = new wxBoxSizer(wxVERTICAL);
    check_host->SetSizer(check_contents);
    settings_root->Add(check_host, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
    auto* scroll = new WorkbenchScrolledWindow(settings, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_workbench_settings_scroll = scroll;
    scroll->SetScrollRate(0, FromDIP(12));
    scroll->SetBackgroundColour(wxColour(32, 32, 35));
    auto* sections = m_workbench_settings_sections = new wxBoxSizer(wxVERTICAL);
    for (size_t index = 0; index < m_workbench_settings_groups.size(); ++index) {
        auto* group = m_workbench_settings_groups[index] = new wxBoxSizer(wxVERTICAL);
        (index == 0 ? check_contents : sections)->Add(group, 0, wxEXPAND);
    }
    auto* controls = m_workbench_settings_groups[0];
    wxWindow* section_parent = check_host;
    auto heading = [this, &section_parent, &controls](const wxString& label,
        wxStaticBitmap** info = nullptr, const wxString& tooltip = wxEmptyString) {
        auto* text = new wxStaticText(section_parent, wxID_ANY, label);
        text->SetForegroundColour(wxColour(220, 220, 222));
        text->SetFont(wxGetApp().bold_font());
        auto* title_row = new wxBoxSizer(wxHORIZONTAL);
        title_row->Add(text, 0, wxALIGN_CENTER_VERTICAL);
        if (info) {
            auto* icon = *info = new wxStaticBitmap(section_parent, wxID_ANY,
                create_scaled_bitmap("workbench_info", section_parent, 12));
            icon->SetName("ai_content_color");
            icon->SetBackgroundColour(section_parent->GetBackgroundColour());
            icon->SetMinSize(FromDIP(wxSize(12, 12)));
            icon->SetToolTip(tooltip);
            title_row->Add(icon, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(5));
        }
        controls->Add(title_row, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(12));
        return text;
    };
    heading(_L("检查修复"), &m_workbench_check_info, _L("检查当前模型的网格质量"));
    auto* check_panel = new WorkbenchPanel(check_host);
    check_panel->SetBackgroundColour(wxColour(22, 22, 25));
    check_panel->SetMinSize(FromDIP(wxSize(-1, 42)));
    auto* check_row = new wxBoxSizer(wxHORIZONTAL);
    auto* check_label = new wxStaticText(check_panel, wxID_ANY, _L("一键检查"));
    check_label->SetForegroundColour(wxColour(220, 220, 222));
    check_row->Add(check_label, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    auto* check_button = command(check_panel, check_row, _L("开始"), [this](wxCommandEvent& event) {
        if (m_workbench_check_running && m_workbench_check_cancel) m_workbench_check_cancel->store(true);
        else request_check_workbench();
    });
    m_workbench_check = check_button;
    check_button->SetMinSize(FromDIP(wxSize(60, 27)));
    check_button->SetBackgroundColor(StateColor(
        std::pair<wxColour, int>(wxColour(48, 48, 52), StateColor::Disabled),
        std::pair<wxColour, int>(wxColour(94, 94, 96), StateColor::Hovered),
        std::pair<wxColour, int>(wxColour(77, 77, 79), StateColor::Normal)));
    m_workbench_check->SetToolTip(_L("检查当前模型的网格质量"));
    check_row->GetItem(m_workbench_check)->SetFlag(wxALIGN_CENTER_VERTICAL | wxRIGHT);
    check_row->GetItem(m_workbench_check)->SetBorder(FromDIP(12));
    check_panel->SetSizer(check_row);
    controls->Add(check_panel, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    m_workbench_check_status = new wxStaticText(check_host, wxID_ANY, _L("尚未检查"));
    m_workbench_check_status->SetForegroundColour(wxColour(170, 170, 176));
    controls->Add(m_workbench_check_status, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    check_contents->Add(build_portrait_optimization(check_host), 0, wxEXPAND | wxBOTTOM, FromDIP(8));

    // The check surface is a child of the existing GL host. It is sized over
    // the single canvas and raised only while a check needs user attention;
    // no second canvas or bitmap is introduced.
    if (auto* overlay_host = m_model_preview->workbench_overlay_parent()) {
        m_workbench_check_overlay = new WorkbenchPaintedPanel(
            overlay_host, wxColour(24, 24, 27));
        // This subtree owns its dark palette. Exclude it from the legacy
        // appearance walk, which otherwise replaces owner-painted label
        // backgrounds with the system window colour on Windows.
        m_workbench_check_overlay->SetName("ai_content_color");
        auto* overlay_root = new wxBoxSizer(wxVERTICAL);
        overlay_root->AddStretchSpacer(1);
        auto* center_row = new wxBoxSizer(wxHORIZONTAL);
        center_row->AddStretchSpacer(1);
        m_workbench_check_modal = new WorkbenchPaintedPanel(
            m_workbench_check_overlay, wxColour(45, 45, 49), 8, wxColour(72, 72, 78));
        m_workbench_check_modal->SetMinSize(FromDIP(wxSize(410, 174)));
        auto* modal_root = new wxBoxSizer(wxVERTICAL);
        m_workbench_check_phase = new WorkbenchLabel(m_workbench_check_modal, _L("检查模型"));
        m_workbench_check_phase->SetFont(wxGetApp().bold_font());
        m_workbench_check_phase->SetForegroundColour(wxColour(238, 240, 244));
        modal_root->Add(m_workbench_check_phase, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM,
                        FromDIP(16));
        m_workbench_check_message = new WorkbenchLabel(m_workbench_check_modal);
        m_workbench_check_message->SetForegroundColour(wxColour(205, 207, 214));
        m_workbench_check_message->Wrap(FromDIP(376));
        modal_root->Add(m_workbench_check_message, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
        m_workbench_check_progress = new WorkbenchIndeterminateProgress(m_workbench_check_modal);
        modal_root->Add(m_workbench_check_progress, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
        auto* modal_actions = new wxBoxSizer(wxHORIZONTAL);
        m_workbench_check_cancel_button = new WorkbenchButton(m_workbench_check_modal, _L("取消检查"));
        m_workbench_check_continue_button = new WorkbenchButton(m_workbench_check_modal, _L("仍要继续"));
        m_workbench_check_return_button = new WorkbenchButton(m_workbench_check_modal, _L("返回 2D 设计图"));
        for (auto* button : {m_workbench_check_cancel_button, m_workbench_check_continue_button,
                             m_workbench_check_return_button}) {
            button->SetMinSize(FromDIP(wxSize(124, 32)));
            button->SetMaxSize(FromDIP(wxSize(156, 32)));
            button->SetCornerRadius(FromDIP(7));
            button->SetBackgroundColor(StateColor(
                std::pair<wxColour, int>(wxColour(255, 214, 72), StateColor::Hovered),
                std::pair<wxColour, int>(wxColour(255, 194, 39), StateColor::Normal),
                std::pair<wxColour, int>(wxColour(82, 82, 88), StateColor::Disabled)));
            button->SetTextColor(StateColor(wxColour(35, 35, 30)));
        }
        modal_actions->AddStretchSpacer(1);
        modal_actions->Add(m_workbench_check_cancel_button, 0, wxRIGHT, FromDIP(8));
        modal_actions->Add(m_workbench_check_continue_button, 0, wxRIGHT, FromDIP(8));
        modal_actions->Add(m_workbench_check_return_button, 0);
        modal_root->Add(modal_actions, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
        m_workbench_check_modal->SetSizer(modal_root);
        center_row->Add(m_workbench_check_modal, 0, wxALIGN_CENTER_VERTICAL);
        center_row->AddStretchSpacer(1);
        overlay_root->Add(center_row, 0, wxEXPAND);
        overlay_root->AddStretchSpacer(1);
        m_workbench_check_overlay->SetSizer(overlay_root);
        m_workbench_check_overlay->Hide();
        m_workbench_check_overlay->Bind(wxEVT_CHAR_HOOK, [](wxKeyEvent& event) {
            if (event.GetKeyCode() != WXK_ESCAPE) event.Skip();
        });
        m_workbench_check_cancel_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { cancel_workbench_check(); });
        m_workbench_check_continue_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { acknowledge_workbench_risk(); });
        m_workbench_check_return_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            hide_workbench_check_overlay();
            if (m_workbench_return_to_design) m_workbench_return_to_design();
        });
        overlay_host->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
            if (m_workbench_check_overlay) {
                const wxRect rect(wxPoint(0, 0), event.GetSize());
                if (m_workbench_check_overlay->GetRect() != rect)
                    m_workbench_check_overlay->SetSize(rect);
                m_workbench_check_overlay->Layout();
            }
            event.Skip();
        });
        m_workbench_check_overlay->SetSize(overlay_host->GetClientRect());
    }
    section_parent = scroll;
    controls = m_workbench_settings_groups[1];
    m_workbench_palette_heading = heading(_L("多色模型"));
    controls->Add(m_model_preview->build_workbench_palette(scroll), 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    m_workbench_palette_details = command(scroll, controls, _L("色卡详情"), [this](wxCommandEvent&) {
        const auto state = post_generation_ui_state();
        if (state.can_edit && m_finishing_candidate.empty()) {
            m_model_preview->show_workbench_palette_details();
            refresh_model_finishing();
            refresh_post_generation_workbench();
        }
    });
    m_workbench_palette_status = new wxStaticText(scroll, wxID_ANY, wxEmptyString);
    m_workbench_palette_status->SetForegroundColour(wxColour(170, 170, 176));
    m_model_preview->set_color_trial_changed_callback([this](size_t count) {
        if (m_workbench_palette_status)
            m_workbench_palette_status->SetLabel(
                m_printer_filament_trial_active
                    ? wxString::Format(_L("打印机耗材试色 · %llu 色"), static_cast<unsigned long long>(count))
                    : wxString::Format(_L("当前色卡 · %llu 色"), static_cast<unsigned long long>(count)));
        if (m_printer_filament_trial_active && m_model_preview &&
            m_model_preview->color_trial_state().source != 1) {
            m_printer_filament_trial_active = false;
            m_printer_filament_trial_geometry.clear();
            m_printer_filament_trial_signature.clear();
            m_workbench_operation_notice = "已退出打印机耗材试色，模型恢复为当前色卡外观。";
        }
        if (m_beauty_controls && m_model_preview)
            m_beauty_controls->synchronize_automatic_basis(
                m_model_preview->automatic_color_basis_signature());
    });
    controls->Add(m_workbench_palette_status, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    heading(_L("打印机耗材"));
    m_workbench_printer_filament_trial = new WorkbenchButton(scroll, _L("将当前色卡导入到打印机耗材"));
    // Keep the primary action directly below the section heading and match
    // the import-to-slicing action's neutral surface and disabled state.
    m_workbench_apply_project_filaments = m_workbench_printer_filament_trial;
    apply_slice_action_style(m_workbench_printer_filament_trial);
    m_workbench_printer_filament_trial->SetName("workbench_printer_filament_trial");
    m_workbench_printer_filament_trial->SetIcon("workbench_printer");
    m_workbench_printer_filament_trial->SetToolTip(_L("将当前色卡预载到原生耗材配置；点击完成后进入打印机耗材试色。"));
    m_workbench_printer_filament_trial->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        toggle_printer_filament_trial();
    });
    controls->Add(m_workbench_printer_filament_trial, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    auto* filament_scroll = new WorkbenchScrolledWindow(scroll, wxID_ANY, wxDefaultPosition,
        wxDefaultSize, wxVSCROLL | wxBORDER_NONE);
    filament_scroll->SetScrollRate(0, FromDIP(12));
    m_workbench_project_colors = filament_scroll;
    m_workbench_project_colors->SetBackgroundColour(scroll->GetBackgroundColour());
    m_workbench_project_colors->SetSizer(new wxBoxSizer(wxVERTICAL));
    controls->Add(m_workbench_project_colors, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    m_workbench_project_summary = new wxStaticText(scroll, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxST_NO_AUTORESIZE);
    m_workbench_project_summary->SetForegroundColour(wxColour(190, 190, 196));
    controls->Add(m_workbench_project_summary, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    filament_scroll->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        auto* contents = m_workbench_project_colors->GetSizer();
        if (contents && contents->GetItemCount()) {
            if (auto* grid = dynamic_cast<wxFlexGridSizer*>(contents->GetItem(size_t(0))->GetSizer())) {
                if (grid->GetCols() != 1) {
                    grid->RemoveGrowableCol(0);
                    if (grid->GetCols() == 2) grid->RemoveGrowableCol(1);
                    grid->SetCols(1);
                    grid->AddGrowableCol(0);
                    m_workbench_project_colors->Layout();
                    static_cast<wxScrolledWindow*>(m_workbench_project_colors)->FitInside();
                }
            }
        }
        event.Skip();
    });
    auto filament_action = [&](const wxString& label, auto handler) {
        auto* button = new WorkbenchButton(scroll,label);
        button->SetMinSize(FromDIP(wxSize(1,36))); button->Bind(wxEVT_BUTTON,handler);
        button->Hide(); return button;
    };
    m_workbench_configure_filaments = filament_action(_L("配置耗材"),[this](wxCommandEvent&){
        if(m_project_color_edit && workbench_snapshot().can_edit_project_colors)m_project_color_edit(size_t(-1));
    });
    m_workbench_add_filament = filament_action(_L("增加耗材"),[this](wxCommandEvent&){
        if(!workbench_snapshot().can_edit_project_colors || !wxGetApp().plater())return;
        if (show_orca_filament_selection(wxGetTopLevelParent(m_workbench_shell),*wxGetApp().plater(),size_t(-1),true,
            m_model_preview ? m_model_preview->configuration_color_sources() : std::vector<FilamentColorPack>{}))
            synchronize_workbench_project_palette(true);
    });
    m_workbench_sync_filaments = filament_action(_L("同步设备耗材"),[this](wxCommandEvent&){
        auto* plater=wxGetApp().plater();
        if(!workbench_snapshot().can_edit_project_colors || !plater || !plater->get_machine_sync_status())return;
        plater->sidebar().sync_ams_list();
        synchronize_workbench_project_palette(true);
    });
    auto* save_filament_pack = filament_action(_L("保存为色卡组合"), [this](wxCommandEvent&) {
        const auto snapshot = workbench_snapshot();
        auto* bundle = wxGetApp().preset_bundle;
        if (!bundle || !snapshot.printer_filaments.ready) {
            wxMessageBox(_L("请先完成并提交所有兼容的打印机耗材配置。"), _L("保存色卡组合"), wxOK | wxICON_INFORMATION, wxGetTopLevelParent(m_workbench_shell));
            return;
        }
        wxTextEntryDialog naming(wxGetTopLevelParent(m_workbench_shell), _L("为当前打印机耗材命名，之后可在配置窗口导入。"), _L("保存色卡组合"));
        if (naming.ShowModal() != wxID_OK || naming.GetValue().Strip(wxString::both).empty()) return;
        const std::string name = into_u8(naming.GetValue().Strip(wxString::both));
        if (name == young_portrait_color_pack().name || name == u8"上次应用前色卡") {
            wxMessageBox(_L("请使用其他名称，预制色卡和恢复色卡由系统保留。"), _L("保存色卡组合"), wxOK | wxICON_INFORMATION, wxGetTopLevelParent(m_workbench_shell));
            return;
        }
        auto pack = capture_current_filament_color_pack(*bundle, name);
        if (!save_filament_color_pack(pack)) {
            wxMessageBox(_L("色卡组合保存失败，请检查本地配置目录或耗材槽位。"), _L("保存色卡组合"), wxOK | wxICON_ERROR, wxGetTopLevelParent(m_workbench_shell));
            return;
        }
        if (m_model_preview) m_model_preview->refresh_saved_color_packs();
        wxMessageBox(_L("色卡组合已保存，可在“配置耗材”的导入来源中选择：") + "\n" + from_u8(name) +
            "\n\n" + _L("保存位置：") + "\n" + from_u8((boost::filesystem::path(data_dir()) / "filament_color_packs.json").string()),
            _L("保存色卡组合"), wxOK | wxICON_INFORMATION, wxGetTopLevelParent(m_workbench_shell));
    });
    save_filament_pack->SetToolTip(_L("保存已确认的实体耗材颜色和材料预设；混色虚拟槽不会保存。"));
    // The workbench beauty group is intentionally absent from the sidebar.
    // Beauty remains available from the fixed top-bar entry, while the check
    // and palette sections retain their existing layout and state.
    controls = m_workbench_settings_groups[2];
    controls = new wxBoxSizer(wxVERTICAL);
    sections->Add(controls, 0, wxEXPAND);
    heading(_L("切片"));
    auto* slice_panel = new WorkbenchPanel(scroll);
    slice_panel->SetBackgroundColour(wxColour(22, 22, 25));
    auto* slice_root = new wxBoxSizer(wxVERTICAL);
    auto* slice_title = new wxStaticText(slice_panel, wxID_ANY, _L("AI 智能切片"));
    slice_title->SetForegroundColour(wxColour(220, 220, 222));
    slice_root->Add(slice_title, 0, wxEXPAND | wxALL, FromDIP(12));
    auto* slice_grid = new wxFlexGridSizer(2, FromDIP(12), FromDIP(12));
    slice_grid->AddGrowableCol(1);
    const std::array<wxString, 7> slice_labels {{_L("层高"), _L("墙厚"), _L("填充率"), _L("打印速度"),
        _L("支撑"), _L("预计耗时"), _L("耗材")}};
    for (size_t index = 0; index < slice_labels.size(); ++index) {
        auto* label = new wxStaticText(slice_panel, wxID_ANY, slice_labels[index]);
        label->SetForegroundColour(wxColour(170, 170, 176));
        slice_grid->Add(label, 0, wxALIGN_CENTER_VERTICAL);
        auto* value = m_workbench_slice_values[index] = new wxStaticText(slice_panel, wxID_ANY, "--");
        value->SetForegroundColour(wxColour(220, 220, 222));
        slice_grid->Add(value, 0, wxALIGN_RIGHT | wxALIGN_CENTER_VERTICAL);
    }
    m_workbench_slice_values[3]->SetToolTip(_L("当前工艺的外墙打印速度"));
    for (size_t index : {size_t(5), size_t(6)})
        m_workbench_slice_values[index]->SetToolTip(_L("当前模型所在打印板的切片合计；导入后在准备页显式切片。"));
    slice_root->Add(slice_grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    auto* slicing = command(slice_panel, slice_root, _L("导入切片"), [this](wxCommandEvent&) {
        request_workbench_color_matching();
    });
    slice_root->GetItem(slicing)->SetFlag(wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM);
    slice_root->GetItem(slicing)->SetBorder(FromDIP(12));
    apply_slice_action_style(static_cast<WorkbenchButton*>(slicing));
    slice_panel->SetSizer(slice_root);
    controls->Add(slice_panel, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    m_workbench_slicing = slicing;
    slicing->SetToolTip(_L("导入当前模型到准备页，随后打开智能切片；候选比较与切片需在该页显式启动。"));
    slicing->SetIcon("workbench_printer");
    if (RedesignFeatureFlags::model_workflow_review_enabled()) {
        slice_grid->ShowItems(false);
        slice_title->SetLabel(_L("导入后选择切片方案"));
    }
    auto* native_entry = command(scroll, controls, _L("orca原生"), [this](wxCommandEvent&) {
        request_workbench_color_matching();
    });
    native_entry->SetMinSize(FromDIP(wxSize(-1, 42)));
    native_entry->SetBackgroundColor(StateColor(
        std::pair<wxColour, int>(wxColour(48, 48, 52), StateColor::Hovered),
        std::pair<wxColour, int>(wxColour(19, 19, 21), StateColor::Normal)));
    native_entry->set_navigation_assets("workbench_native_emoji", "workbench_native_arrow");
    native_entry->Show(!RedesignFeatureFlags::model_workflow_review_enabled());
    auto* scroll_content = new wxBoxSizer(wxVERTICAL);
    scroll_content->Add(sections, 0, wxEXPAND | wxRIGHT, FromDIP(8));
    scroll->SetSizer(scroll_content);
    settings_root->Add(scroll, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
    m_workbench_print = command(settings, settings_root, _L("去打印"), [](wxCommandEvent&) {});
    m_workbench_print->Disable();
    m_workbench_print->SetToolTip(_L("设备打印尚未接入，请在切片区域导入模型并预览、导出 G-code。"));
    auto* print = static_cast<Button*>(m_workbench_print);
    print->SetBackgroundColor(StateColor(std::pair<wxColour, int>(wxColour(75, 75, 80), StateColor::Disabled),
        std::pair<wxColour, int>(wxColour(255, 194, 39), StateColor::Normal)));
    print->SetTextColor(StateColor(wxColour(22, 22, 25)));
    settings_root->GetItem(print)->SetBorder(FromDIP(12));
    settings_root->GetItem(print)->SetFlag(wxEXPAND | wxALL);
    settings->SetSizer(settings_root);
    m_workbench_sync_timer.SetOwner(this, wxWindow::NewControlId());
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        if (m_shutdown || !m_workbench_shell || !m_workbench_shell->IsShownOnScreen()) return;
        synchronize_workbench_project_palette();
        ensure_workbench_check();
    }, m_workbench_sync_timer.GetId());
    m_workbench_sync_timer.Start(500);
    row->Add(settings, 0, wxEXPAND | wxTOP | wxBOTTOM | wxRIGHT, FromDIP(12));

    auto* workspace = m_workbench_view_host = new wxPanel(shell, wxID_ANY, wxDefaultPosition,
        wxDefaultSize, wxTAB_TRAVERSAL | wxCLIP_CHILDREN);
    workspace->SetBackgroundColour(wxColour(49, 49, 54));
    auto* workspace_sizer = new wxBoxSizer(wxVERTICAL);
    auto* toolbar_host = new wxPanel(workspace, wxID_ANY, wxDefaultPosition, wxDefaultSize,
        wxBORDER_NONE | wxCLIP_CHILDREN);
    toolbar_host->SetName("ai_content_color");
    toolbar_host->SetBackgroundColour(workspace->GetBackgroundColour());
#ifdef __WXMSW__
    toolbar_host->SetDoubleBuffered(true);
#endif
    auto* toolbar = new wxBoxSizer(wxHORIZONTAL);
    toolbar_host->SetSizer(toolbar);
    auto toolbar_command = [this, toolbar_host, toolbar](const wxString& label, auto action) {
        auto* button = new WorkbenchButton(toolbar_host, label);
        button->SetFont(wxGetApp().bold_font());
        button->SetCornerRadius(FromDIP(8));
        button->SetBorderWidth(0);
        button->SetBackgroundColor(StateColor(
            std::pair<wxColour, int>(wxColour(92, 86, 55), StateColor::Disabled),
            std::pair<wxColour, int>(wxColour(237, 174, 20), StateColor::Pressed),
            std::pair<wxColour, int>(wxColour(255, 214, 72), StateColor::Hovered),
            std::pair<wxColour, int>(wxColour(255, 194, 39), StateColor::Normal)));
        button->SetTextColor(StateColor(
            std::pair<wxColour, int>(wxColour(125, 120, 92), StateColor::Disabled),
            std::pair<wxColour, int>(wxColour(35, 35, 30), StateColor::Normal)));
        button->SetMinSize(FromDIP(wxSize(140, 38)));
        button->SetMaxSize(FromDIP(wxSize(140, 38)));
        button->Bind(wxEVT_BUTTON, action);
        toolbar->Add(button, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        return button;
    };
    m_workbench_return_to_design_button = toolbar_command(_L("返回图像设计"), [this](wxCommandEvent&) {
        if (m_workbench_return_to_design) m_workbench_return_to_design();
    });
    m_workbench_edit = toolbar_command(_L("3D 美颜工作台"), [this](wxCommandEvent&) {
        request_enter_beauty();
    });
    m_workbench_edit->SetToolTip(_L("打开美颜工具，预览后接受修改；原件保留。"));
    m_workbench_model_status = new wxStaticText(toolbar_host, wxID_ANY, wxEmptyString,
        wxDefaultPosition, FromDIP(wxSize(100, 24)), wxALIGN_CENTER_HORIZONTAL | wxST_NO_AUTORESIZE);
    m_workbench_model_status->SetFont(wxGetApp().bold_font());
    m_workbench_model_status->SetMinSize(FromDIP(wxSize(100, 24)));
    m_workbench_model_status->SetMaxSize(FromDIP(wxSize(100, 24)));
    m_workbench_model_status->SetForegroundColour(wxColour(190, 190, 196));
    toolbar->Add(m_workbench_model_status, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    m_workbench_history_toggle = command(toolbar_host, toolbar, wxEmptyString, [this](wxCommandEvent&) {
        if (m_workbench_import_feedback || m_workbench_import_running) return;
        m_workbench_history_user_open = !m_workbench_history_panel->IsShown();
        m_workbench_history_collapsed = !m_workbench_history_user_open;
        m_workbench_editing = false;
        update_finishing_selection();
        refresh_model_finishing(); refresh_post_generation_workbench();
    });
    workspace_sizer->Add(toolbar_host, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
    m_workbench_state_status = new wxStaticText(workspace, wxID_ANY, wxEmptyString);
    m_workbench_state_status->SetForegroundColour(wxColour(255, 194, 39));
    workspace_sizer->Add(m_workbench_state_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    m_workbench_state_status->Hide();
    auto* view_tools_host = new wxPanel(workspace, wxID_ANY, wxDefaultPosition, wxDefaultSize,
        wxBORDER_NONE | wxCLIP_CHILDREN);
    view_tools_host->SetName("ai_content_color");
    view_tools_host->SetBackgroundColour(workspace->GetBackgroundColour());
#ifdef __WXMSW__
    view_tools_host->SetDoubleBuffered(true);
#endif
    auto* view_tools = new wxBoxSizer(wxHORIZONTAL);
    view_tools_host->SetSizer(view_tools);
    auto tool_region = [view_tools_host, view_tools] {
        auto* region = new wxPanel(view_tools_host, wxID_ANY, wxDefaultPosition, wxDefaultSize,
            wxBORDER_NONE | wxCLIP_CHILDREN);
        region->SetName("ai_content_color");
        region->SetBackgroundColour(view_tools_host->GetBackgroundColour());
        region->SetMinSize(view_tools_host->FromDIP(wxSize(220, -1)));
        view_tools->Add(region, 1, wxEXPAND);
        return region;
    };
    auto* switches_host = tool_region();
    auto* icons_host = tool_region();
    tool_region();

    auto* switches = new wxBoxSizer(wxHORIZONTAL);
    switches_host->SetSizer(switches);
    auto add_switch = [this, switches_host, switches](const wxString& label) {
        auto* text = new wxStaticText(switches_host, wxID_ANY, label);
        text->SetForegroundColour(wxColour(220, 220, 222));
        switches->Add(text, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
        auto* toggle = new WorkbenchSwitch(switches_host, label);
        switches->Add(toggle, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
        return toggle;
    };
    auto* base = m_workbench_base = add_switch(_L("增加底座"));
    base->SetValue(false);
    base->Bind(wxEVT_TOGGLEBUTTON, [this, base](wxCommandEvent& event) {
        std::string error;
        m_model_preview->set_round_base_preview(event.IsChecked(), error);
        const bool enabled = m_model_preview->round_base_preview_enabled();
        // Native toggle handling updates its value before this event reaches
        // us; call the override unconditionally so its owner-drawn bitmap is
        // rebuilt from the final preview state.
        base->SetValue(enabled);
        if (!error.empty()) m_workbench_operation_notice = error;
        refresh_post_generation_workbench();
    });
    auto* original = m_workbench_original = add_switch(_L("原色对照"));
    original->SetValue(false);
    original->Bind(wxEVT_TOGGLEBUTTON, [this, original](wxCommandEvent& event) {
        m_model_preview->set_original_comparison(event.IsChecked());
        original->SetValue(m_model_preview->original_comparison_enabled());
        refresh_post_generation_workbench();
    });
    auto* grid = m_workbench_grid = add_switch(_L("网格"));
    grid->SetValue(true);
    grid->Bind(wxEVT_TOGGLEBUTTON, [this, grid](wxCommandEvent& event) {
        const bool visible = event.IsChecked();
        grid->SetValue(visible);
        m_model_preview->set_workbench_grid_visible(visible);
    });

    auto* icons = new wxBoxSizer(wxHORIZONTAL);
    icons_host->SetSizer(icons);
    icons->AddStretchSpacer();
    auto icon_command = [&command, this, icons_host, icons](const wxString& label, const wxString& icon, auto action) {
        auto* button = command(icons_host, icons, wxEmptyString, action);
        button->SetName(label);
        button->SetToolTip(label);
        button->SetIcon(icon);
        button->SetMinSize(FromDIP(wxSize(38, 38)));
        button->SetMaxSize(FromDIP(wxSize(38, 38)));
        icons->GetItem(button)->SetFlag(wxALIGN_CENTER_VERTICAL | wxRIGHT);
        icons->GetItem(button)->SetBorder(FromDIP(4));
        return button;
    };
    icon_command(_L("正面视图"), "workbench_front", [this](wxCommandEvent&) { m_model_preview->front_view(); });
    icon_command(_L("重置三维视图"), "workbench_object", [this](wxCommandEvent&) { m_model_preview->reset_view(); });
    auto* model_info = icon_command(_L("模型信息"), "workbench_info", [this](wxCommandEvent&) {
        wxDialog dialog(this, wxID_ANY, _L("模型信息"), wxDefaultPosition, FromDIP(wxSize(520, 380)),
                        wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
        dialog.SetName("ai_content_color");
        dialog.SetBackgroundColour(wxColour(32, 32, 35));
        dialog.SetFont(m_workbench_shell->GetFont());
        auto* root = new wxBoxSizer(wxVERTICAL);
        auto* scroll = new WorkbenchScrolledWindow(&dialog, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
        scroll->SetBackgroundColour(dialog.GetBackgroundColour());
        scroll->SetScrollRate(0, FromDIP(12));
        auto* contents = new wxBoxSizer(wxVERTICAL);
        const wxString details = m_model_stats->GetLabel() + "\n\n" + m_model_quality_summary->GetLabel();
        auto* text = new wxStaticText(scroll, wxID_ANY, details);
        text->SetForegroundColour(wxColour(235, 235, 235));
        text->Wrap(FromDIP(460));
        contents->Add(text, 0, wxEXPAND | wxALL, FromDIP(12));
        scroll->SetSizer(contents);
        root->Add(scroll, 1, wxEXPAND | wxALL, FromDIP(12));
        scroll->Bind(wxEVT_SIZE, [scroll, text, details](wxSizeEvent& event) {
            text->SetLabel(details);
            wrap_workbench_text(text, std::max(scroll->FromDIP(80), scroll->GetClientSize().x - scroll->FromDIP(34)), true);
            event.Skip();
        });
        auto* close = workbench_button(&dialog, _L("完成"));
        close->Bind(wxEVT_BUTTON, [&dialog](wxCommandEvent&) { dialog.EndModal(wxID_OK); });
        root->Add(close, 0, wxALIGN_RIGHT | wxALL, FromDIP(12));
        dialog.SetSizer(root);
        dialog.CenterOnParent();
        dialog.ShowModal();
    });
    icons->GetItem(model_info)->SetFlag(wxALIGN_CENTER_VERTICAL);
    icons->GetItem(model_info)->SetBorder(0);
    switches->AddStretchSpacer();
    icons->AddStretchSpacer();
    workspace_sizer->Add(view_tools_host, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));
    m_workbench_footer = new WorkbenchPanel(workspace);
    m_workbench_footer->SetBackgroundColour(wxColour(32, 32, 35));
    m_beauty_controls->attach_actions(m_workbench_footer);
    m_finishing_compare_model->GetContainingSizer()->Detach(m_finishing_compare_model);
    m_finishing_compare_model->Reparent(m_workbench_footer);
    m_workbench_footer->GetSizer()->Add(m_finishing_compare_model, 0, wxRIGHT, FromDIP(4));
    workspace_sizer->Add(m_workbench_footer, 0, wxEXPAND | wxALL, FromDIP(8));
    workspace->SetSizer(workspace_sizer);
    row->Add(workspace, 1, wxEXPAND);
    row->Add(build_workbench_history(shell), 0, wxEXPAND | wxTOP | wxBOTTOM | wxLEFT,
             FromDIP(12));
    shell->SetSizer(row);
    auto* parameter_host = m_model_preview->workbench_overlay_parent();
    toolbar->Detach(m_workbench_history_toggle);
    m_workbench_history_toggle->Reparent(parameter_host);
    auto* history_toggle = static_cast<WorkbenchButton*>(m_workbench_history_toggle);
    history_toggle->SetName("ai_content_color");
    history_toggle->SetCornerRadius(0);
    history_toggle->SetBackgroundColour(wxColour(49, 49, 54));
    history_toggle->SetPaddingSize(FromDIP(wxSize(0, 0)));
    history_toggle->SetMinSize(FromDIP(wxSize(19, 98)));
    history_toggle->SetMaxSize(FromDIP(wxSize(19, 98)));
    history_toggle->set_drawer_assets();
    auto* parameters = new WorkbenchScrolledWindow(parameter_host, wxID_ANY, wxDefaultPosition,
        wxDefaultSize, wxVSCROLL | wxBORDER_NONE);
    m_workbench_parameters = parameters;
    parameters->SetName("ai_content_color");
    parameters->SetFont(shell->GetFont());
    parameters->SetBackgroundColour(wxColour(40, 40, 43));
    parameters->SetScrollRate(0, FromDIP(12));
    auto* parameter_sizer = new wxBoxSizer(wxVERTICAL);
    parameter_sizer->AddSpacer(FromDIP(12));
    parameters->SetSizer(parameter_sizer);
    m_beauty_controls->attach_parameters(parameters, m_finishing_strength_value, m_finishing_strength);
    // Keep the geometry controls from the post-generation design visible in
    // the same floating surface. Topology-changing operations remain
    // disabled, while smoothing controls use the existing texture-safe path.
    auto* geometry_heading = new wxStaticText(parameters, wxID_ANY, _L("模型美化"));
    geometry_heading->SetForegroundColour(wxColour(235, 235, 235));
    parameter_sizer->Add(geometry_heading, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    auto add_geometry_slider = [&](const wxString& label, int value, int minimum, int maximum,
                                   wxSlider** target, bool pending) {
        auto* text = new wxStaticText(parameters, wxID_ANY, label);
        text->SetForegroundColour(wxColour(210, 210, 214));
        parameter_sizer->Add(text, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
        auto* slider = new WorkbenchSlider(parameters, wxID_ANY, value, minimum, maximum,
            wxColour(61, 127, 255), "workbench_parameter_thumb");
        slider->SetToolTip(pending ? _L("该几何处理入口将在后端支持后启用。")
                                   : _L("控制纹理保真的表面平滑迭代次数，预览后生成候选版本。"));
        if (target) *target = slider;
        if (pending) slider->Disable();
        parameter_sizer->Add(slider, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    };
    add_geometry_slider(_L("减面强度"), 0, 0, 100, nullptr, true);
    add_geometry_slider(_L("平滑迭代"), 4, 1, 12, &m_workbench_smoothing_iterations, false);

    auto add_geometry_switch = [&](const wxString& label, wxToggleButton** target, bool pending) {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        auto* text = new wxStaticText(parameters, wxID_ANY, label);
        text->SetForegroundColour(wxColour(210, 210, 214));
        row->Add(text, 1, wxALIGN_CENTER_VERTICAL);
        auto* toggle = new WorkbenchSwitch(parameters, label);
        toggle->SetToolTip(pending ? _L("该几何处理入口将在后端支持后启用。")
                                   : _L("固定大于 55° 的折角，避免表面平滑抹平硬边。"));
        if (target) *target = toggle;
        toggle->SetValue(!pending);
        if (pending) toggle->Disable();
        row->Add(toggle, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
        parameter_sizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
    };
    add_geometry_switch(_L("自动补洞"), nullptr, true);
    add_geometry_switch(_L("保留硬边"), &m_workbench_preserve_hard_edges, false);
    m_workbench_smoothing_iterations->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
        refresh_model_finishing();
    });
    m_workbench_preserve_hard_edges->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent&) {
        refresh_model_finishing();
    });

    m_workbench_parameter_divider = new wxStaticBitmap(parameters, wxID_ANY,
        create_scaled_bitmap("workbench_parameter_divider", parameters, 12));
    m_workbench_parameter_divider->SetName("ai_content_color");
    parameter_sizer->Add(m_workbench_parameter_divider, 0, wxALIGN_CENTER_HORIZONTAL);
    auto* parameter_counts = new wxFlexGridSizer(2, FromDIP(16), FromDIP(8));
    parameter_counts->AddGrowableCol(1);
    for (const auto& row : std::array<std::pair<wxString, wxStaticText**>, 2> {{
             {_L("当前面数"), &m_workbench_parameter_faces},
             {_L("预估输出"), &m_workbench_parameter_output}}}) {
        parameter_counts->Add(new wxStaticText(parameters, wxID_ANY, row.first),
            0, wxALIGN_CENTER_VERTICAL);
        *row.second = new wxStaticText(parameters, wxID_ANY, "--");
        parameter_counts->Add(*row.second, 0, wxALIGN_RIGHT | wxALIGN_CENTER_VERTICAL);
    }
    m_workbench_parameter_output->SetToolTip(_L("预览完成后显示候选版本的实际面数。"));
    parameter_sizer->Add(parameter_counts, 0, wxEXPAND | wxALL, FromDIP(12));
    apply_workbench_parameter_theme(parameters);
    parameters->Hide();
    parameter_host->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        layout_workbench_parameters(true); event.Skip();
    });
#ifdef __WXMSW__
    Bind(wxEVT_DPI_CHANGED, [this](wxDPIChangedEvent& event) {
        wxWeakRef<ModelGenerationPanel> weak(this);
        wxGetApp().CallAfter([weak] {
            if (weak && !weak->m_shutdown) weak->rescale_post_generation_workbench();
        });
        event.Skip();
    });
#endif
    shell->Hide();
    return shell;
}
} // namespace Slic3r::GUI
