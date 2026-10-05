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
    auto* nav = new WorkbenchPanel(shell);
    nav->SetBackgroundColour(wxColour(32, 32, 35));
    nav->SetMinSize(FromDIP(wxSize(68, -1)));
    auto* navigation = new wxBoxSizer(wxVERTICAL);
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
    navigation->AddSpacer(FromDIP(30));
    m_workbench_logo = new wxStaticBitmap(nav, wxID_ANY, create_scaled_bitmap("workbench_logo", nav, 36));
    m_workbench_logo->SetName("ai_content_color");
    m_workbench_logo->SetBackgroundColour(nav->GetBackgroundColour());
    m_workbench_logo->SetMinSize(FromDIP(wxSize(36, 36)));
    navigation->Add(m_workbench_logo, 0, wxALIGN_CENTER_HORIZONTAL);
    navigation->AddSpacer(FromDIP(39));
    auto* assets = command(nav, navigation, _L("资产"), [this](wxCommandEvent&) {
        set_finishing_workbench(false); m_preview_book->SetSelection(1);
    });
    assets->SetIcon("workbench_assets"); assets->SetVertical(true);
    auto* images = command(nav, navigation, _L("图像"), [this](wxCommandEvent&) { set_finishing_workbench(false); });
    images->SetIcon("workbench_image"); images->SetVertical(true);
    auto* current_module = command(nav, navigation, _L("3D模型"), [](wxCommandEvent&) {});
    current_module->SetTextColorNormal(wxColour(235, 235, 235));
    current_module->SetIcon("workbench_object"); current_module->SetVertical(true);
    navigation->Detach(current_module);
    auto* active_navigation = new wxPanel(nav);
    active_navigation->SetBackgroundColour(nav->GetBackgroundColour());
    auto* active_row = new wxBoxSizer(wxHORIZONTAL);
    auto* active_marker = new WorkbenchPanel(active_navigation);
    active_marker->SetBackgroundColour(wxColour(255, 194, 39));
    active_marker->SetMinSize(FromDIP(wxSize(3, -1)));
    active_row->Add(active_marker, 0, wxEXPAND);
    current_module->Reparent(active_navigation);
    active_row->Add(current_module, 1, wxEXPAND);
    active_navigation->SetSizer(active_row);
    navigation->Add(active_navigation, 0, wxEXPAND | wxBOTTOM, FromDIP(18));
    auto* print_navigation = command(nav, navigation, _L("打印"), [this](wxCommandEvent& event) { on_import(event); });
    print_navigation->SetIcon("workbench_printer"); print_navigation->SetVertical(true);
    print_navigation->SetToolTip(_L("导入当前模型到准备页"));
    m_workbench_print_navigation = print_navigation;
    for (auto* button : {assets, images, current_module, print_navigation}) {
        button->SetBackgroundColor(StateColor(
            std::pair<wxColour, int>(wxColour(48, 48, 52), StateColor::Hovered),
            std::pair<wxColour, int>(nav->GetBackgroundColour(), StateColor::Normal)));
        button->SetPaddingSize(FromDIP(wxSize(4, 8)));
        button->SetMinSize(FromDIP(wxSize(52, 53)));
    }
    for (auto* button : {assets, images, print_navigation}) {
        button->SetTextColorNormal(wxColour(150, 150, 154));
        navigation->GetItem(button)->SetBorder(FromDIP(18));
    }
    navigation->AddStretchSpacer();
    auto footer_icon = [this, nav, navigation](const wxString& name, const std::string& asset,
                                              int edge, auto action) {
        auto* button = new WorkbenchButton(nav, wxEmptyString);
        button->SetName(name);
        button->SetToolTip(name);
        button->SetBorderWidth(0);
        button->SetCornerRadius(0);
        button->SetPaddingSize(wxSize(0, 0));
        button->set_scaled_icon(asset, edge);
        button->SetMinSize(FromDIP(wxSize(40, edge)));
        button->SetMaxSize(FromDIP(wxSize(40, edge)));
        button->SetBackgroundColor(StateColor(
            std::pair<wxColour, int>(wxColour(48, 48, 52), StateColor::Hovered),
            std::pair<wxColour, int>(nav->GetBackgroundColour(), StateColor::Normal)));
        button->Bind(wxEVT_BUTTON, action);
        navigation->Add(button, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, FromDIP(30));
        return button;
    };
    footer_icon(_L("账户"), "topbar_account", 24, [](wxCommandEvent&) {
        wxGetApp().request_login(true);
    });
    auto* notifications = footer_icon(_L("通知中心暂不可用"), "workbench_notification", 22,
                                      [](wxCommandEvent&) {});
    notifications->Enable(false);
    notifications->EnableTooltipEvenDisabled();
    footer_icon(_L("Preferences"), "workbench_settings", 23, [](wxCommandEvent&) {
        wxGetApp().open_preferences();
    });
    auto* help = footer_icon(_L("Help"), "workbench_help", 23, [nav](wxCommandEvent&) {
        wxMenu menu;
        auto add_help = [&menu](const wxString& label, auto action) {
            const int id = wxWindow::NewControlId();
            menu.Append(id, label);
            menu.Bind(wxEVT_MENU, action, id);
        };
        add_help(_L("Keyboard Shortcuts"), [](wxCommandEvent&) { wxGetApp().keyboard_shortcuts(); });
        add_help(_L("Troubleshoot Center"), [](wxCommandEvent&) { wxGetApp().troubleshoot(); });
        menu.AppendSeparator();
        add_help(wxString::Format(_L("&About %s"), SLIC3R_APP_FULL_NAME),
                 [](wxCommandEvent&) { Slic3r::GUI::about(); });
        nav->PopupMenu(&menu);
    });
    navigation->GetItem(help)->SetBorder(FromDIP(8));
    command(nav, navigation, _L("返回"), [this](wxCommandEvent&) { set_finishing_workbench(false); });
    nav->SetSizer(navigation);
    row->Add(nav, 0, wxEXPAND | wxALL, FromDIP(6));

    auto* settings = new WorkbenchPanel(shell);
    m_workbench_settings = settings;
    settings->SetMinSize(FromDIP(wxSize(280, -1)));
    settings->SetBackgroundColour(wxColour(32, 32, 35));
    auto* settings_root = new wxBoxSizer(wxVERTICAL);
    auto* scroll = new WorkbenchScrolledWindow(settings, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_workbench_settings_scroll = scroll;
    scroll->SetScrollRate(0, FromDIP(12));
    scroll->SetBackgroundColour(wxColour(32, 32, 35));
    auto* sections = m_workbench_settings_sections = new wxBoxSizer(wxVERTICAL);
    for (auto*& group : m_workbench_settings_groups) {
        group = new wxBoxSizer(wxVERTICAL);
        sections->Add(group, 0, wxEXPAND);
    }
    auto* controls = m_workbench_settings_groups[0];
    auto heading = [this, scroll, &controls](const wxString& label,
        wxStaticBitmap** info = nullptr, const wxString& tooltip = wxEmptyString) {
        auto* text = new wxStaticText(scroll, wxID_ANY, label);
        text->SetForegroundColour(wxColour(220, 220, 222));
        text->SetFont(wxGetApp().bold_font());
        auto* title_row = new wxBoxSizer(wxHORIZONTAL);
        title_row->Add(text, 0, wxALIGN_CENTER_VERTICAL);
        if (info) {
            auto* icon = *info = new wxStaticBitmap(scroll, wxID_ANY,
                create_scaled_bitmap("workbench_info", scroll, 12));
            icon->SetName("ai_content_color");
            icon->SetBackgroundColour(scroll->GetBackgroundColour());
            icon->SetMinSize(FromDIP(wxSize(12, 12)));
            icon->SetToolTip(tooltip);
            title_row->Add(icon, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(5));
        }
        controls->Add(title_row, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(12));
        return text;
    };
    heading(_L("检查修复"), &m_workbench_check_info, _L("检查当前模型的网格质量"));
    auto* check_panel = new WorkbenchPanel(scroll);
    check_panel->SetBackgroundColour(wxColour(22, 22, 25));
    check_panel->SetMinSize(FromDIP(wxSize(-1, 42)));
    auto* check_row = new wxBoxSizer(wxHORIZONTAL);
    auto* check_label = new wxStaticText(check_panel, wxID_ANY, _L("一键检查"));
    check_label->SetForegroundColour(wxColour(220, 220, 222));
    check_row->Add(check_label, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    auto* check_button = command(check_panel, check_row, _L("开始"), [this](wxCommandEvent& event) {
        on_recheck_model(event); refresh_post_generation_workbench();
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
    m_workbench_check_status = new wxStaticText(scroll, wxID_ANY, _L("尚未检查"));
    m_workbench_check_status->SetForegroundColour(wxColour(170, 170, 176));
    controls->Add(m_workbench_check_status, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
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
    auto* original_row = new wxBoxSizer(wxHORIZONTAL);
    auto* original_label = new wxStaticText(scroll, wxID_ANY, _L("查看原色"));
    original_label->SetForegroundColour(wxColour(220, 220, 222));
    auto* original = new WorkbenchSwitch(scroll, _L("查看原色"));
    m_workbench_original = original;
    original->Bind(wxEVT_TOGGLEBUTTON, [this, original](wxCommandEvent&) {
        original->SetValue(original->GetValue());
        m_model_preview->set_beauty_original_view(original->GetValue());
        if (m_beauty_controls) m_beauty_controls->synchronize_preview_options();
    });
    original_row->Add(original_label, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    original_row->Add(original, 0, wxALIGN_CENTER_VERTICAL);
    controls->Add(original_row, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    m_workbench_palette_status = new wxStaticText(scroll, wxID_ANY, wxEmptyString);
    m_workbench_palette_status->SetForegroundColour(wxColour(170, 170, 176));
    m_model_preview->set_color_trial_changed_callback([this](size_t count) {
        if (m_workbench_palette_status)
            m_workbench_palette_status->SetLabel(wxString::Format(_L("当前色卡 · %llu 色"),
                static_cast<unsigned long long>(count)));
    });
    controls->Add(m_workbench_palette_status, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    controls = m_workbench_settings_groups[2];
    m_workbench_beauty_heading = heading(_L("3D 美颜"), &m_workbench_beauty_info,
        _L("打开美颜工具，预览后接受修改；原件保留。"));
    m_workbench_edit = command(scroll, controls, _L("3D 美颜工作台"), [this](wxCommandEvent&) {
        m_workbench_editing = true;
        m_model_preview->set_color_controls_visible(false);
        update_finishing_selection();
        refresh_model_finishing(); refresh_post_generation_workbench();
    });
    auto* beauty_entry = static_cast<WorkbenchButton*>(m_workbench_edit);
    beauty_entry->SetMinSize(FromDIP(wxSize(-1, 42)));
    beauty_entry->set_navigation_assets({}, "workbench_native_arrow");
    beauty_entry->SetToolTip(_L("打开美颜工具，预览后接受修改；原件保留。"));
    auto* base = m_workbench_base = command(scroll, controls, _L("增加底座"), [](wxCommandEvent&) {});
    base->Disable(); base->SetToolTip(_L("生成后底座几何编辑待实现"));
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
    auto* slicing = command(slice_panel, slice_root, _L("开始切片"), [this](wxCommandEvent& event) {
        m_open_smart_slicing_after_import = true;
        on_import(event);
    });
    slice_root->GetItem(slicing)->SetFlag(wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM);
    slice_root->GetItem(slicing)->SetBorder(FromDIP(12));
    slicing->SetMinSize(FromDIP(wxSize(-1, 28)));
    slicing->SetBackgroundColor(StateColor(
        std::pair<wxColour, int>(wxColour(48, 48, 52), StateColor::Disabled),
        std::pair<wxColour, int>(wxColour(94, 94, 96), StateColor::Hovered),
        std::pair<wxColour, int>(wxColour(77, 77, 79), StateColor::Normal)));
    slice_panel->SetSizer(slice_root);
    controls->Add(slice_panel, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    m_workbench_slicing = slicing;
    slicing->SetToolTip(_L("导入当前模型到准备页，随后打开智能切片；候选比较与切片需在该页显式启动。"));
    slicing->SetIcon("workbench_printer");
    auto* native_entry = command(scroll, controls, _L("orca原生"), [this](wxCommandEvent&) {
        set_finishing_workbench(false);
        if (auto* plater = wxGetApp().plater()) plater->show_smart_slicing(false);
        if (m_prepare_navigation) m_prepare_navigation();
    });
    native_entry->SetMinSize(FromDIP(wxSize(-1, 42)));
    native_entry->SetBackgroundColor(StateColor(
        std::pair<wxColour, int>(wxColour(48, 48, 52), StateColor::Hovered),
        std::pair<wxColour, int>(wxColour(19, 19, 21), StateColor::Normal)));
    native_entry->set_navigation_assets("workbench_native_emoji", "workbench_native_arrow");
    auto* scroll_content = new wxBoxSizer(wxVERTICAL);
    scroll_content->Add(sections, 0, wxEXPAND | wxRIGHT, FromDIP(8));
    scroll->SetSizer(scroll_content);
    settings_root->Add(scroll, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
    m_workbench_print = command(settings, settings_root, _L("去打印"), [this](wxCommandEvent& event) { on_import(event); });
    auto* print = static_cast<Button*>(m_workbench_print);
    print->SetBackgroundColor(StateColor(std::pair<wxColour, int>(wxColour(75, 75, 80), StateColor::Disabled),
        std::pair<wxColour, int>(wxColour(255, 194, 39), StateColor::Normal)));
    print->SetTextColor(StateColor(wxColour(22, 22, 25)));
    settings_root->GetItem(print)->SetBorder(FromDIP(12));
    settings_root->GetItem(print)->SetFlag(wxEXPAND | wxALL);
    settings->SetSizer(settings_root);
    row->Add(settings, 0, wxEXPAND | wxTOP | wxBOTTOM | wxRIGHT, FromDIP(12));

    auto* workspace = m_workbench_view_host = new wxPanel(shell);
    workspace->SetBackgroundColour(wxColour(49, 49, 54));
    auto* workspace_sizer = new wxBoxSizer(wxVERTICAL);
    auto* toolbar = new wxBoxSizer(wxHORIZONTAL);
    m_workbench_model_name = new wxStaticText(workspace, wxID_ANY, wxEmptyString,
        wxDefaultPosition, FromDIP(wxSize(100, 24)), wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
    m_workbench_model_name->SetForegroundColour(wxColour(230, 230, 234));
    toolbar->Add(m_workbench_model_name, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    m_workbench_model_status = new wxStaticText(workspace, wxID_ANY, wxEmptyString,
        wxDefaultPosition, FromDIP(wxSize(100, 24)), wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
    m_workbench_model_status->SetForegroundColour(wxColour(255, 194, 39));
    toolbar->Add(m_workbench_model_status, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    m_workbench_history_toggle = command(workspace, toolbar, wxEmptyString, [this](wxCommandEvent&) {
        m_workbench_history_user_open = !m_workbench_history_panel->IsShown();
        m_workbench_history_collapsed = !m_workbench_history_user_open;
        m_workbench_editing = false;
        update_finishing_selection();
        refresh_model_finishing(); refresh_post_generation_workbench();
    });
    command(workspace, toolbar, _L("结果对照"), [this](wxCommandEvent&) { set_finishing_workbench(false); });
    workspace_sizer->Add(toolbar, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
    m_workbench_state_status = new wxStaticText(workspace, wxID_ANY, wxEmptyString);
    m_workbench_state_status->SetForegroundColour(wxColour(255, 194, 39));
    workspace_sizer->Add(m_workbench_state_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    m_workbench_state_status->Hide();
    auto* view_tools = new wxBoxSizer(wxHORIZONTAL);
    auto icon_command = [&command, this, workspace, view_tools](const wxString& label, const wxString& icon, auto action) {
        auto* button = command(workspace, view_tools, wxEmptyString, action);
        button->SetName(label);
        button->SetToolTip(label);
        button->SetIcon(icon);
        button->SetMinSize(FromDIP(wxSize(38, 38)));
        button->SetMaxSize(FromDIP(wxSize(38, 38)));
        return button;
    };
    icon_command(_L("正面视图"), "workbench_front", [this](wxCommandEvent&) { m_model_preview->front_view(); });
    icon_command(_L("重置三维视图"), "workbench_object", [this](wxCommandEvent&) { m_model_preview->reset_view(); });
    auto* grid_label = new wxStaticText(workspace, wxID_ANY, _L("网格"));
    grid_label->SetForegroundColour(wxColour(220, 220, 222));
    view_tools->Add(grid_label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
    auto* grid = new WorkbenchSwitch(workspace, _L("网格"));
    grid->SetValue(true);
    grid->Bind(wxEVT_TOGGLEBUTTON, [this, grid](wxCommandEvent&) {
        grid->SetValue(grid->GetValue());
        m_model_preview->set_workbench_grid_visible(grid->GetValue());
    });
    view_tools->Add(grid, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(8));
    icon_command(_L("模型信息"), "workbench_info", [this](wxCommandEvent&) {
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
    workspace_sizer->Add(view_tools, 0, wxALIGN_CENTER_HORIZONTAL);
    m_workbench_footer = new WorkbenchPanel(workspace);
    m_workbench_footer->SetBackgroundColour(wxColour(32, 32, 35));
    m_beauty_controls->attach_actions(m_workbench_footer);
    m_finishing_compare_model->GetContainingSizer()->Detach(m_finishing_compare_model);
    m_finishing_compare_model->Reparent(m_workbench_footer);
    m_workbench_footer->GetSizer()->Add(m_finishing_compare_model, 0, wxRIGHT, FromDIP(4));
    workspace_sizer->Add(m_workbench_footer, 0, wxEXPAND | wxALL, FromDIP(8));
    workspace->SetSizer(workspace_sizer);
    row->Add(workspace, 1, wxEXPAND);
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
    auto* info = m_workbench_model_info = new wxPanel(parameter_host);
    info->SetName("ai_content_color");
    info->SetFont(shell->GetFont());
    info->SetBackgroundColour(wxColour(32, 32, 35));
    auto* info_grid = new wxFlexGridSizer(2, FromDIP(10), FromDIP(14));
    info_grid->AddGrowableCol(1);
    const std::array<wxString, 4> info_labels {{_L("拓扑"), _L("面数"), _L("顶点数"), _L("尺寸")}};
    for (size_t index = 0; index < info_labels.size(); ++index) {
        auto* label = new wxStaticText(info, wxID_ANY, info_labels[index]);
        label->SetForegroundColour(wxColour(170, 170, 176));
        info_grid->Add(label, 0, wxALIGN_CENTER_VERTICAL);
        auto* value = m_workbench_model_info_values[index] = new wxStaticText(info, wxID_ANY, "--");
        value->SetForegroundColour(wxColour(220, 220, 222));
        info_grid->Add(value, 0, wxALIGN_RIGHT | wxALIGN_CENTER_VERTICAL);
    }
    auto* info_root = new wxBoxSizer(wxVERTICAL);
    info_root->Add(info_grid, 0, wxEXPAND | wxALL, FromDIP(12));
    info->SetSizer(info_root);
    info->Hide();
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
        layout_workbench_parameters(); event.Skip();
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
    m_workbench_logo->SetBitmap(create_scaled_bitmap("workbench_logo", m_workbench_logo, 36));
    m_workbench_logo->SetMinSize(FromDIP(wxSize(36, 36)));
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
        wrap_workbench_text(m_workbench_state_status,
            std::max(FromDIP(200), m_workbench_view_host->GetClientSize().x - FromDIP(16)));
    }
    if (m_workbench_settings_editing != m_workbench_editing) {
        // Move whole sizer groups, keeping their controls and Beauty session alive.
        for (auto* group : m_workbench_settings_groups)
            m_workbench_settings_sections->Detach(group);
        for (size_t index = 0; index < m_workbench_settings_groups.size(); ++index)
            m_workbench_settings_sections->Insert(index,
                m_workbench_settings_groups[m_workbench_editing ? 2 - index : index], 0, wxEXPAND);
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
    m_workbench_check->Enable(m_recheck_model->IsEnabled());
    m_workbench_palette_details->Enable(state.can_edit && m_finishing_candidate.empty());
    m_model_preview->set_workbench_palette_editable(state.can_edit && m_finishing_candidate.empty());
    m_workbench_original->SetValue(m_model_preview->beauty_original_view());
    m_workbench_edit->SetLabel(m_workbench_editing ? _L("一键美化") : _L("3D 美颜工作台"));
    m_workbench_print->Enable(state.can_import && is_nonempty_model(m_displayed_model_path));
    m_workbench_print_navigation->Enable(m_workbench_print->IsEnabled());
    m_workbench_slicing->Enable(m_workbench_print->IsEnabled());
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
    const bool local_version = m_displayed_model_job_id.rfind("finish-", 0) == 0;
    m_workbench_check->SetToolTip(local_version
        ? _L("本地处理版本请导入准备页，检查实际打印条件。")
        : m_recheck_model->GetToolTipText());
    m_workbench_check_status->SetLabel(m_model_quality.available ? m_model_quality_status->GetLabel()
        : local_version ? _L("本地版本 · 请在准备页检查") : _L("尚未检查"));
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
        request_library_thumbnails();
    m_refreshing_workbench_layout = false;
}

void ModelGenerationPanel::on_discard(wxCommandEvent&)
{
    if (m_busy || m_finishing_running || !m_finishing_candidate.empty()) return;
    if (m_ready || m_model_preview_ready || m_style_preview_ready) {
        wxMessageDialog choice(this,
            _L("要先看看重新设计的建议吗？当前历史模型会保留，图片和描述可继续使用。"),
            _L("重新开始"), wxYES_NO | wxCANCEL | wxICON_QUESTION);
        choice.SetYesNoCancelLabels(_L("直接重新开始"), _L("先看建议"), _L("留在当前作品"));
        const int answer = choice.ShowModal();
        if (answer == wxID_CANCEL) return;
        if (answer == wxID_NO) {
            wxString advice;
            if (m_model_refinement.available && !m_model_refinement.summary.empty())
                advice = _L("已有检查建议：\n") + wxString::FromUTF8(m_model_refinement.summary.c_str());
            else
                advice = _L("尚无这件作品的检查结论。可先按下面的方向对照原图：\n\n"
                    "• 人物不像：使用清晰正面参考，描述中明确五官、发型和姿态。\n"
                    "• 形体不完整：确认图片主体完整、遮挡较少，明确底座和连接部位。\n"
                    "• 颜色杂乱：先在3D美颜里试六色、保留嘴唇和服装等关键色。\n"
                    "• 表面小凹凸：先试局部美颜，通常不需要重新生成。\n\n"
                    "这些是设计建议，未运行新的AI分析。");
            wxMessageDialog guidance(this, advice, _L("重新设计建议"), wxOK | wxCANCEL | wxICON_INFORMATION);
            guidance.SetOKCancelLabels(_L("继续重新开始"), _L("返回调整作品"));
            if (guidance.ShowModal() != wxID_OK) return;
        }
    }
    const bool reuse_palette = m_palette_source->GetSelection() == 2 && !current_palette().empty();
    boost::system::error_code reference_error;
    if (!m_reference_image_path.empty() && boost::filesystem::is_regular_file(m_reference_image_path, reference_error)) {
        m_selected_image_path = m_reference_image_path;
        m_selected_image->SetLabel(wxString(m_selected_image_path.filename().wstring()));
    } else if (m_library_model_loaded) {
        m_selected_image_path.clear();
        m_selected_image->SetLabel(_L("未选择图片"));
    }
    if (m_finishing_workbench) set_finishing_workbench(false);
    reset(!m_ready);
    m_palette_recommendation_confirmed = reuse_palette;
    refresh_controls();
    m_prompt->SetFocus();
}

wxWindow* ModelGenerationPanel::build_model_finishing(wxWindow* parent)
{
    auto* scroll = new WorkbenchScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(280), FromDIP(480)), wxVSCROLL | wxBORDER_NONE);
    scroll->SetMinSize(wxSize(FromDIP(280), FromDIP(200)));
    scroll->SetScrollRate(0, FromDIP(12));
    scroll->set_rounded_corners(true);
    m_finishing_panel = scroll;
    scroll->SetBackgroundColour(wxColour(32, 32, 35));
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* title_row = new wxBoxSizer(wxHORIZONTAL);
    auto* title = new wxStaticText(m_finishing_panel, wxID_ANY, _L("3D 美颜工作台"));
    title->SetFont(wxFontInfo(10).FaceName("HONOR Sans Design").Weight(wxFONTWEIGHT_MEDIUM));
    title_row->Add(title, 1, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, FromDIP(8));
    auto* close_editor = workbench_button(m_finishing_panel, _L("返回"));
    close_editor->SetFont(wxFontInfo(8).FaceName("HONOR Sans Design").Weight(wxFONTWEIGHT_MEDIUM));
    close_editor->SetPaddingSize(FromDIP(wxSize(12, 4)));
    close_editor->SetMinSize(FromDIP(wxSize(45, 27)));
    close_editor->SetCornerRadius(FromDIP(9));
    close_editor->SetBackgroundColor(StateColor(
        std::pair<wxColour, int>(wxColour(96, 96, 100), StateColor::Hovered),
        std::pair<wxColour, int>(wxColour(77, 77, 79), StateColor::Normal)));
    close_editor->SetTextColor(StateColor(*wxWHITE));
    close_editor->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        m_workbench_editing = false; update_finishing_selection(); refresh_model_finishing();
    });
    title_row->Add(close_editor, 0, wxTOP | wxBOTTOM, FromDIP(8));
    sizer->AddSpacer(FromDIP(17));
    sizer->Add(title_row, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    auto* hint = new wxStaticText(m_finishing_panel, wxID_ANY,
        _L("Alt＋左键旋转，右键平移，滚轮缩放。原件保留，修改可撤销。"));
    wrap_workbench_text(hint, FromDIP(260));
    hint->Hide();
    auto* base_placeholder = new wxButton(m_finishing_panel, wxID_ANY, _L("增加底座（待实现）"));
    base_placeholder->Disable();
    base_placeholder->SetToolTip(_L("生成后底座几何编辑将在后续版本提供；当前不会改变模型。"));
    base_placeholder->Hide();
    m_finishing_tool = new wxChoice(m_finishing_panel, wxID_ANY);
    for (const auto& label : {_L("整体美颜"), _L("局部修整"), _L("多色试色"), _L("网格修复"), _L("统一这块颜色"), _L("清理小杂点")})
        m_finishing_tool->Append(label);
    m_finishing_tool->SetSelection(0);
    sizer->Add(m_finishing_tool, 0, wxEXPAND | wxALL, FromDIP(10));
    m_finishing_gray = new wxCheckBox(m_finishing_panel, wxID_ANY, _L("灰模观察凹凸（仅显示）"));
    sizer->Add(m_finishing_gray, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    m_finishing_gray->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { m_model_preview->set_gray_view(m_finishing_gray->GetValue()); });
    m_finishing_selection_section = workbench_button(m_finishing_panel, _L("选择区域"));
    m_finishing_selection_section->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        m_finishing_selection_open = !m_finishing_selection_open;
        refresh_model_finishing();
    });
    sizer->Add(m_finishing_selection_section, 0, wxEXPAND | wxALL, FromDIP(10));
    m_finishing_selection_controls = new wxPanel(m_finishing_panel);
    auto* selection = new wxBoxSizer(wxVERTICAL);
    auto* selection_title = new wxStaticText(m_finishing_selection_controls, wxID_ANY, _L("选择区域"));
    selection_title->SetFont(wxGetApp().bold_font());
    selection->Add(selection_title, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    auto* selection_hint = new wxStaticText(m_finishing_selection_controls, wxID_ANY,
        _L("圈选当前可见表面，再涂抹补选或保护细节。橙色参与处理，蓝色受保护；不穿透背面。"));
    wrap_workbench_text(selection_hint, FromDIP(260));
    selection_hint->Hide();
    m_finishing_selection_operation = workbench_choice(m_finishing_selection_controls);
    for (const auto& label : {_L("圈选要修改的范围"), _L("涂抹补选"), _L("涂抹保护"), _L("点选相近颜色"), _L("转动模型")})
        m_finishing_selection_operation->Append(label);
    m_finishing_selection_operation->SetSelection(0);
    selection->Add(m_finishing_selection_operation, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    selection->Add(new wxStaticText(m_finishing_selection_controls, wxID_ANY, _L("笔刷大小")), 0);
    m_finishing_radius = new WorkbenchSlider(m_finishing_selection_controls, wxID_ANY, 3, 1, 10,
        wxColour(61, 127, 255), "workbench_parameter_thumb");
    selection->Add(m_finishing_radius, 0, wxEXPAND);
    m_finishing_selection_status = new wxStaticText(m_finishing_selection_controls, wxID_ANY, _L("尚未选区 · 点击模型开始"));
    selection->Add(m_finishing_selection_status, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    auto* selection_actions = new wxBoxSizer(wxHORIZONTAL);
    auto* undo_selection = workbench_button(m_finishing_selection_controls, _L("撤销选区"));
    auto* clear_selection = workbench_button(m_finishing_selection_controls, _L("清空选区"));
    selection_actions->Add(undo_selection, 0, wxRIGHT, FromDIP(6));
    selection_actions->Add(clear_selection);
    selection->Add(selection_actions);
    auto* redo_selection = workbench_button(m_finishing_selection_controls, _L("重做选区"));
    selection->Add(redo_selection, 0, wxTOP, FromDIP(6));
    redo_selection->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_model_preview->redo_selection(); });
    undo_selection->Hide();
    redo_selection->Hide();
    auto* refine_selection = workbench_button(m_finishing_selection_controls, _L("贴合选区边界"));
    refine_selection->SetToolTip(_L("圈选后，在要修改处涂抹补选、在要保留处涂抹保护，再沿颜色和表面边界修正。不会扩大到范围之外。"));
    selection->Add(refine_selection, 0, wxEXPAND | wxTOP, FromDIP(6));
    refine_selection->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_model_preview->refine_selection_boundary(); });
    auto* focus_selection = workbench_button(m_finishing_selection_controls, _L("放大选区"));
    focus_selection->SetToolTip(_L("将选中的区域放到画面中央；“完整显示模型”可恢复全貌。"));
    selection->Add(focus_selection, 0, wxEXPAND | wxTOP, FromDIP(8));
    auto* overlay_row = m_finishing_overlay_row = new wxPanel(m_finishing_panel);
    auto* overlay_layout = new wxBoxSizer(wxHORIZONTAL);
    overlay_layout->Add(new wxStaticText(overlay_row, wxID_ANY, _L("显示选区高亮")),
        1, wxALIGN_CENTER_VERTICAL);
    auto* show_selection = m_finishing_overlay = new WorkbenchSwitch(overlay_row, _L("显示选区高亮"));
    overlay_layout->Add(show_selection, 0, wxALIGN_CENTER_VERTICAL);
    overlay_row->SetSizer(overlay_layout);
    show_selection->SetValue(true);
    show_selection->SetToolTip(_L("关闭可看清选区内原本的颜色和细节；选区仍然有效。"));
    focus_selection->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (!m_model_preview->focus_selection()) {
            m_finishing_status->SetLabel(_L("请先点选模型上的区域，再放大查看。"));
            refresh_model_finishing();
        }
    });
    show_selection->Bind(wxEVT_TOGGLEBUTTON, [this, show_selection](wxCommandEvent&) {
        show_selection->SetValue(show_selection->GetValue());
        m_model_preview->set_selection_preview_suppressed(false);
        m_model_preview->set_selection_overlay_visible(show_selection->GetValue());
    });
    m_finishing_selection_controls->SetSizer(selection);
    sizer->Add(m_finishing_selection_controls, 0, wxEXPAND | wxALL, FromDIP(10));
    sizer->Add(overlay_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    m_finishing_selection_operation->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { update_finishing_selection(); });
    m_finishing_radius->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) { update_finishing_selection(); });
    undo_selection->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_model_preview->undo_selection(); });
    clear_selection->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_model_preview->clear_selection(); });
    m_finishing_preset = new wxChoice(m_finishing_panel, wxID_ANY);
    for (const auto& label : {_L("轻柔 · 保留细节"), _L("均衡 · 表面柔化"), _L("加强 · 平滑凹凸"), _L("仅修复 · 保留造型")})
        m_finishing_preset->Append(label);
    m_finishing_preset->SetSelection(0);
    sizer->Add(m_finishing_preset, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    m_finishing_smooth = new wxCheckBox(m_finishing_panel, wxID_ANY, _L("表面美化（保护边界和锐边）"));
    m_finishing_smooth->SetValue(true);
    sizer->Add(m_finishing_smooth, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    m_finishing_strength = new WorkbenchSlider(m_finishing_panel, wxID_ANY, 15, 0, 100,
        wxColour(61, 127, 255), "workbench_parameter_thumb");
    m_finishing_cleanup_hint = new wxStaticText(m_finishing_panel, wxID_ANY,
        _L("先圈住杂点及周围主色，保护眼睛、花纹等细节。只合并孤立小色块；连续条带可用“统一这块颜色”。"));
    wrap_workbench_text(m_finishing_cleanup_hint, FromDIP(260));
    sizer->Add(m_finishing_cleanup_hint, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    // Native wxSL_LABELS creates sibling labels that overlap after tool panels
    // are shown/hidden. Keep the value in our sizer with the rest of the form.
    m_finishing_strength_value = new wxStaticText(m_finishing_panel, wxID_ANY, _L("处理强度：15%"));
    sizer->Add(m_finishing_strength_value, 0, wxLEFT | wxRIGHT, FromDIP(10));
    m_finishing_strength->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
        m_finishing_strength_value->SetLabel(wxString::Format(_L("处理强度：%d%%"), m_finishing_strength->GetValue()));
    });
    m_finishing_strength->SetToolTip(_L("强度越高，柔化越明显。保护轮廓与细小结构；可随时调整并重新预览。"));
    sizer->Add(m_finishing_strength, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    m_finishing_repair = new wxCheckBox(m_finishing_panel, wxID_ANY, _L("网格清理与面朝向修复"));
    m_finishing_repair->SetValue(false);
    sizer->Add(m_finishing_repair, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    // Keep action targets stable on narrow side rails. Two columns leave
    // enough room for Chinese labels and let the sizer collapse hidden
    // actions without overlapping adjacent controls.
    auto* actions = new wxGridSizer(0, 2, FromDIP(4), FromDIP(4));
    auto button = [&](wxButton*& target, const wxString& label) {
        target = new wxButton(m_finishing_panel, wxID_ANY, label);
        actions->Add(target, 0, wxEXPAND);
    };
    button(m_finishing_preview, _L("预览处理效果"));
    button(m_finishing_compare, _L("查看处理前"));
    button(m_finishing_accept, _L("接受并保存新版本"));
    button(m_finishing_discard, _L("放弃预览"));
    button(m_finishing_undo, _L("返回上个版本"));
    button(m_finishing_redo, _L("重做已保存修整"));
    button(m_finishing_cancel, _L("取消处理"));
    sizer->Add(actions, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    m_finishing_status = new wxStaticText(m_finishing_panel, wxID_ANY, wxEmptyString);
    wrap_workbench_text(m_finishing_status, FromDIP(260));
    wrap_workbench_text(m_finishing_selection_status, FromDIP(240));
    sizer->Add(m_finishing_status, 0, wxEXPAND | wxALL, FromDIP(10));
    m_beauty_controls = new BeautyWorkbenchControls(m_finishing_panel, m_model_preview, m_palette_provider,
        [this] { if (m_finishing_panel) { m_finishing_panel->Layout(); static_cast<wxScrolledWindow*>(m_finishing_panel)->FitInside(); } });
    m_beauty_controls->on_original_view_changed = [this](bool enabled) {
        if (m_workbench_original) m_workbench_original->SetValue(enabled);
    };
    m_beauty_controls->on_boundary_adjust = [this] {
        if (m_model_preview) m_model_preview->set_selection_preview_suppressed(false);
        if (m_model_preview) m_model_preview->refine_selection_boundary();
        if (m_beauty_controls) m_beauty_controls->set_dirty(true);
        update_finishing_selection();
    };
    m_beauty_controls->on_operation_changed = [this](int operation) {
        if (!m_finishing_tool) return;
        // Keep the legacy implementation as an internal compatibility facade;
        // Beauty users only see the unified operation selector.
        if (operation < 0 || operation >= int(std::size(beauty_finishing_tools))) return;
        m_finishing_tool->SetSelection(beauty_finishing_tools[operation]);
        m_finishing_smooth->SetValue(beauty_finishing_tools[operation] == 1);
        m_finishing_repair->SetValue(beauty_finishing_tools[operation] == 3);
        refresh_local_recolor_controls();
        update_finishing_selection();
        refresh_model_finishing();
    };
    m_beauty_controls->on_undo = [this] {
        if (m_beauty_transactions && m_beauty_transactions->undo_count()) {
            if (!m_beauty_transactions->undo())
                m_finishing_status->SetLabel(_L("无法撤销：历史模型文件缺失或已变更，当前预览保持不变。"));
            refresh_model_finishing(); return;
        }
        if (!m_finishing_undo_path.empty()) undo_model_finishing();
        else if (m_model_preview) m_model_preview->undo_selection();
        refresh_model_finishing();
    };
    m_beauty_controls->on_redo = [this] {
        if (m_beauty_transactions && m_beauty_transactions->redo_count()) {
            if (!m_beauty_transactions->redo())
                m_finishing_status->SetLabel(_L("无法重做：历史候选文件缺失或已变更，当前预览保持不变。"));
            refresh_model_finishing(); return;
        }
        if (!m_finishing_redo_path.empty()) redo_model_finishing();
        else if (m_model_preview) m_model_preview->redo_selection();
        refresh_model_finishing();
    };
    m_beauty_controls->on_auto_match = [this](const std::string& region) {
        if (!m_model_preview) return size_t(0);
        const auto before = m_model_preview->selection_state();
        // If the immutable editor is still being prepared, let the preview's
        // deferred path retain its native selection history. Once ready, the
        // controller owns the operation-level history entry.
        const bool deferred = !m_model_preview->beauty_editor();
        const size_t count = m_model_preview->select_semantic_region(region, deferred);
        if (count) m_model_preview->set_selection_preview_suppressed(false);
        if (count && m_beauty_transactions) {
            const auto after = m_model_preview->selection_state();
            m_beauty_transactions->record({
                BeautyWorkbenchTransactionController::OperationKind::Selection,
                "semantic region selection",
                [this, before] { if (m_model_preview) m_model_preview->restore_selection_state(before); },
                [this, after] { if (m_model_preview) m_model_preview->restore_selection_state(after); }
            });
        }
        return count;
    };
    m_beauty_controls->on_auto_detail_match = [this](const std::string& detail) {
        if (!m_model_preview) return size_t(0);
        const auto before = m_model_preview->selection_state();
        const bool deferred = !m_model_preview->beauty_editor();
        const size_t count = m_model_preview->select_semantic_detail(
            detail, deferred, m_beauty_controls && m_beauty_controls->preview_protected_details());
        if (count) m_model_preview->set_selection_preview_suppressed(false);
        if (count && m_beauty_transactions) {
            const auto after = m_model_preview->selection_state();
            m_beauty_transactions->record({
                BeautyWorkbenchTransactionController::OperationKind::Selection,
                "semantic detail selection",
                [this, before] { if (m_model_preview) m_model_preview->restore_selection_state(before); },
                [this, after] { if (m_model_preview) m_model_preview->restore_selection_state(after); }
            });
        }
        return count;
    };
    m_beauty_controls->on_import_secondary_evidence = [this] {
        if (!m_model_preview) return;
        wxFileDialog dialog(this, _L("导入只读二级证据"), wxEmptyString, wxEmptyString,
            _L("Evidence JSON (*.json)|*.json|所有文件 (*.*)|*.*"), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        dialog.SetMessage(_L("选择当前模型的二级语义证据 JSON；六视角 render-manifest.json 不能直接导入"));
        if (dialog.ShowModal() != wxID_OK) return;
        std::string error;
        if (!m_model_preview->import_secondary_region_evidence(
                boost::filesystem::path(dialog.GetPath().ToStdWstring()), error)) {
            const wxString message = _L("二级证据未导入：") + wxString::FromUTF8(error);
            m_finishing_status->SetLabel(message);
            wxMessageDialog result(this, message, _L("二级证据导入失败"), wxOK | wxICON_WARNING);
            result.ShowModal();
            refresh_model_finishing();
            return;
        }
        const wxString message = _L("二级证据已只读导入；未修改模型或材料树。\n现在可以选择二级细节，或勾选“预览全部二级证据（忽略授权门控）”。");
        m_finishing_status->SetLabel(message);
        wxMessageDialog result(this, message, _L("二级证据导入成功"), wxOK | wxICON_INFORMATION);
        result.ShowModal();
        refresh_model_finishing();
    };
    m_beauty_controls->on_regenerate_readonly_evidence = [this] {
        if (!m_model_preview) return;
        m_finishing_status->SetLabel(_L("正在生成只读六视角渲染包；模型和材料树保持不变。"));
        refresh_model_finishing();
        boost::filesystem::path output;
        std::string error;
        if (!m_model_preview->regenerate_readonly_evidence(output, error)) {
            const wxString message = _L("证据生成失败：") + wxString::FromUTF8(error);
            m_finishing_status->SetLabel(message);
            wxMessageDialog result(this, message, _L("只读证据生成失败"), wxOK | wxICON_WARNING);
            result.ShowModal();
        } else {
            const wxString message = _L("当前模型的只读六视角渲染包已生成。它还没有二级语义区域，不能直接导入为选区。\n输出目录：") +
                wxString::FromUTF8(output.generic_string());
            m_finishing_status->SetLabel(message);
            wxMessageDialog result(this, message, _L("只读证据已生成"), wxOK | wxICON_INFORMATION);
            result.ShowModal();
        }
        refresh_model_finishing();
    };
    m_beauty_controls->on_reoptimize = [this] {
        if (!m_model_preview) return false;
        if (m_beauty_transactions && !m_beauty_transactions->begin(
                BeautyWorkbenchTransactionController::OperationKind::SemanticReoptimization)) {
            m_finishing_status->SetLabel(_L("当前仍有 Beauty 处理正在进行，请先完成或取消。"));
            refresh_model_finishing();
            return false;
        }
        m_beauty_reoptimization_before = std::make_shared<BeautyCandidateSnapshot>(capture_beauty_candidate());
        m_model_preview->set_semantic_completion_callback([this](bool success) {
            if (!success) {
                const auto error = m_model_preview->semantic_error();
                if (auto before = std::move(m_beauty_reoptimization_before)) restore_beauty_candidate(*before);
                if (m_beauty_transactions) m_beauty_transactions->finish(false, false, "semantic optimization failed");
                if (m_finishing_status) m_finishing_status->SetLabel(
                    _L("人像区域优化未完成，当前模型和选区保持不变：") + error);
                refresh_model_finishing();
                return;
            }
            export_semantic_candidate();
        });
        if (!m_model_preview->request_semantic_reoptimization()) {
            m_model_preview->set_semantic_completion_callback({});
            if (auto before = std::move(m_beauty_reoptimization_before)) restore_beauty_candidate(*before);
            if (m_beauty_transactions) m_beauty_transactions->finish(false, false, "semantic request unavailable");
            m_finishing_status->SetLabel(_L("未重新识别人像区域：") + m_model_preview->semantic_reoptimization_reason());
            refresh_model_finishing();
            return false;
        }
        refresh_model_finishing();
        return true;
    };
    m_beauty_controls->on_save = [this] {
        if (!m_finishing_candidate.empty()) { accept_model_finishing(); return; }
        // Submit the currently selected Beauty operation through the existing
        // finishing facade. This never triggers semantic recognition.
        preview_model_finishing();
    };
    m_beauty_controls->on_preview = [this] { preview_model_finishing(); };
    m_beauty_controls->on_accept = [this] { accept_model_finishing(); };
    m_beauty_controls->on_discard = [this] { discard_model_finishing(); };
    m_beauty_controls->on_cancel = [this] {
        if (m_beauty_controls && m_beauty_controls->partitioning()) {
            m_beauty_controls->cancel_partition();
            if (m_beauty_transactions) m_beauty_transactions->request_cancel();
            m_finishing_status->SetLabel(_L("正在取消自动划区，原有分区和候选保持不变。"));
            refresh_model_finishing();
            return;
        }
        if (m_model_preview && m_model_preview->cancel_selection_calculation()) {
            m_finishing_status->SetLabel(_L("已取消选区边界计算，原有选区和候选保持不变。"));
            refresh_model_finishing();
            return;
        }
        if (m_finishing_canceled) m_finishing_canceled->store(true);
        if (m_beauty_transactions && m_beauty_transactions->processing() && !m_finishing_running && m_model_preview)
            m_model_preview->cancel_semantic_request();
        if (m_beauty_transactions) m_beauty_transactions->request_cancel();
        m_finishing_status->SetLabel(_L("正在取消，本次处理不会替换当前模型。"));
    };
    m_beauty_controls->on_partition_started = [this] {
        if (m_beauty_transactions && !m_beauty_transactions->begin(
                BeautyWorkbenchTransactionController::OperationKind::Selection)) return false;
        m_finishing_status->SetLabel(_L("正在划分模型区域，可旋转、缩放、查看和取消。"));
        refresh_model_finishing();
        return true;
    };
    m_beauty_controls->on_partition_finished = [this](bool success) {
        if (m_beauty_transactions) m_beauty_transactions->finish(success, false,
            success ? std::string {} : "partition canceled or failed");
        m_finishing_status->SetLabel(success ? _L("分区已就绪；点击模型上的区域，再补选、保护、改色或拉伸。")
            : _L("分区未完成，原有分区保持不变。"));
        refresh_model_finishing();
    };
    m_beauty_controls->on_pick_mode = [this] {
        m_model_preview->set_selection_preview_suppressed(false);
        m_finishing_selection_operation->SetSelection(3);
        update_region_mode();
        m_model_preview->set_selection_enabled(true);
    };
    m_beauty_controls->on_record = [this](const std::string& label,
        std::function<void()> undo, std::function<void()> redo) {
        if (m_beauty_transactions) m_beauty_transactions->record({
            BeautyWorkbenchTransactionController::OperationKind::Selection,
            label, std::move(undo), std::move(redo)
        });
    };
    m_beauty_controls->on_available_colors = [this] { return local_recolor_palette(); };
    m_beauty_controls->on_color_slot_changed = [this](size_t slot) {
        m_region_color_index = int(slot);
        refresh_local_recolor_controls();
    };
    sizer->Insert(1, m_beauty_controls, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    m_finishing_panel->SetSizer(sizer);
    m_finishing_preview->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { preview_model_finishing(); });
    m_finishing_accept->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { accept_model_finishing(); });
    m_finishing_discard->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { discard_model_finishing(); });
    m_finishing_undo->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { undo_model_finishing(); });
    m_finishing_redo->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { redo_model_finishing(); });
    m_finishing_strength->Bind(wxEVT_SCROLL_THUMBRELEASE, [this](wxScrollEvent&) {
        if (!m_busy && m_finishing_workbench && (m_finishing_tool->GetSelection() < 2 || m_finishing_tool->GetSelection() == 5)) preview_model_finishing();
    });
    // These controls belong to the separate post-generation workbench and
    // are created after this legacy finishing panel. Bindings are installed
    // when the controls are created; keep this panel safe during construction.
    m_finishing_tool->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        const int tool = m_finishing_tool->GetSelection();
        m_finishing_smooth->SetValue(tool != 3 && tool != 5);
        m_finishing_repair->SetValue(tool == 3);
        if (tool == 5) {
            m_finishing_strength->SetValue(35);
        }
        // Color tools must show the surface colors, even when the previous
        // geometry tool was inspected as a gray model.
        if (tool == 2 || tool == 4 || tool == 5) {
            m_finishing_gray->SetValue(false);
            m_model_preview->set_gray_view(false);
        }
        m_finishing_status->SetLabel(tool == 2 ? _L("在模型下方试色，可切回原色。选定方案会带入导入配色，最终按实际耗材确认。")
            : tool == 5 ? _L("适用于衣物、头发、皮肤、底座等区域。先保护需要保留的细节，再预览清理结果。")
            : tool == 4 ? _L("点选模型，再用右侧工具换色。改色保存为新版本，原件保留。")
            : tool == 1 ? _L("先点选局部区域，再调整强度。边缘渐变柔化，未选区域保持原样。")
            : tool == 3 ? _L("清理重复／退化面并校正面朝向；不自动补洞或删除部件。")
            : _L("保守柔化整体小凹凸；按当前强度预览后可对比。"));
        refresh_local_recolor_controls(); update_finishing_selection(); refresh_model_finishing();
    });
    m_finishing_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_finishing_canceled) m_finishing_canceled->store(true);
        if (m_beauty_transactions && m_beauty_transactions->processing() && !m_finishing_running && m_model_preview)
            m_model_preview->cancel_semantic_request();
        if (m_beauty_transactions) m_beauty_transactions->request_cancel();
        m_finishing_status->SetLabel(_L("正在取消，本次处理不会替换当前模型。"));
        m_finishing_cancel->Disable();
    });
    auto compare = [this](wxCommandEvent&) {
        if (m_finishing_candidate.empty() || m_busy) return;
        if (show_finishing_version(m_finishing_before ? m_finishing_candidate : m_finishing_source)) {
            m_finishing_before = !m_finishing_before;
            m_finishing_compare->SetLabel(m_finishing_before ? _L("查看处理后") : _L("查看处理前"));
            m_finishing_compare_model->SetLabel(m_finishing_compare->GetLabel());
            m_model_preview_message->SetLabel(m_finishing_before
                ? _L("处理前 · 当前已保存版本") : _L("处理后 · 尚未接受"));
            refresh_model_finishing();
        }
    };
    m_finishing_compare->Bind(wxEVT_BUTTON, compare);
    m_finishing_compare_model->Bind(wxEVT_BUTTON, [this, compare](wxCommandEvent& event) {
        if (m_finishing_workbench) compare(event);
    });
    m_finishing_compare_model->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) {
        if (m_finishing_workbench) { event.Skip(); return; }
        if (m_busy || m_finishing_candidate.empty() || m_finishing_before) return;
        if (show_finishing_version(m_finishing_source)) {
            m_finishing_compare_held = true;
            m_finishing_compare_model->CaptureMouse();
            m_model_preview_message->SetLabel(_L("处理前 · 松开恢复处理后"));
            refresh_model_finishing();
        }
    });
    auto release_compare = [this] {
        if (!m_finishing_compare_held) return;
        m_finishing_compare_held = false;
        if (m_finishing_compare_model->HasCapture()) m_finishing_compare_model->ReleaseMouse();
        if (!m_finishing_candidate.empty()) show_finishing_version(m_finishing_candidate);
        m_model_preview_message->SetLabel(_L("处理后 · 尚未保存"));
        refresh_model_finishing();
    };
    m_finishing_compare_model->Bind(wxEVT_LEFT_UP, [this, release_compare](wxMouseEvent& event) {
        if (m_finishing_workbench) event.Skip();
        else release_compare();
    });
    m_finishing_compare_model->Bind(wxEVT_MOUSE_CAPTURE_LOST, [release_compare](wxMouseCaptureLostEvent&) { release_compare(); });
    m_finishing_smooth->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { refresh_model_finishing(); });
    m_finishing_preset->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        const int preset = m_finishing_preset->GetSelection();
        m_finishing_smooth->SetValue(preset != 3);
        m_finishing_repair->SetValue(preset == 3 && m_finishing_tool->GetSelection() != 1);
        m_finishing_strength->SetValue(preset == 0 ? 15 : preset == 2 ? 65 : 35);
        refresh_model_finishing();
    });
    m_finishing_panel->Bind(wxEVT_SIZE, [this, hint](wxSizeEvent& event) {
        const int width = std::max(FromDIP(80), m_finishing_panel->GetClientSize().x - FromDIP(34));
        wrap_workbench_text(hint, width); wrap_workbench_text(m_finishing_status, width); event.Skip();
    });
    for (wxWindow* child : m_finishing_panel->GetChildren())
        child->SetMaxSize(wxSize(FromDIP(280), -1));
    for (wxWindow* child : m_finishing_selection_controls->GetChildren())
        child->SetMaxSize(wxSize(FromDIP(260), -1));
    close_editor->SetMaxSize(FromDIP(wxSize(45, 27)));
    m_finishing_panel->Hide();
    // The old selector remains as an internal compatibility facade. Beauty
    // users choose operations from BeautyWorkbenchControls below.
    m_finishing_tool->Hide();
    return m_finishing_panel;
}

void ModelGenerationPanel::set_finishing_workbench(bool enabled)
{
    if (m_finishing_workbench == enabled) return;
    // Reparent, theme and lay out the shared viewport before exposing the new mode.
    wxWindowUpdateLocker mode_updates(this);
    m_finishing_workbench = enabled;
    if (enabled) {
        m_model_page->GetSizer()->Detach(m_comparison_panel);
        m_comparison_panel->Reparent(m_workbench_view_host);
        m_workbench_view_host->GetSizer()->Insert(1, m_comparison_panel, 1, wxEXPAND);
    } else {
        m_workbench_view_host->GetSizer()->Detach(m_comparison_panel);
        m_comparison_panel->Reparent(m_model_page);
        m_model_page->GetSizer()->Insert(0, m_comparison_panel, 1, wxEXPAND | wxALL, FromDIP(12));
    }
    GetSizer()->Show(m_generation_content, !enabled, true);
    m_generation_header->Show(!enabled);
    m_workbench_shell->Show(enabled);
    SetBackgroundColour(enabled ? wxColour(49, 49, 54) : *wxWHITE);
    m_model_preview->set_selection_preview_suppressed(enabled &&
        ((!m_finishing_candidate.empty() && !m_finishing_before) || (!m_finishing_accepted_path.empty() &&
         m_displayed_model_path == m_finishing_accepted_path)));
    m_model_preview->set_beauty_view(enabled);
    if (!enabled) {
        m_workbench_model_info->Hide();
        m_workbench_history_toggle->Hide();
    }
    m_model_preview->set_color_controls_visible(!enabled);
    m_model_stats->Show(!enabled);
    m_front_model_view->Show(!enabled);
    m_reset_model_view->Show(!enabled);
    apply_workbench_theme(m_finishing_panel, enabled);
    // The parameter surface can be reparented outside the inspector while hidden.
    apply_workbench_parameter_theme(m_workbench_parameters);
    apply_workbench_theme(m_workbench_footer, enabled);
    apply_workbench_theme(m_workbench_history_panel, enabled);
    if (enabled && m_workbench_history_search) {
        m_workbench_history_search->SetBackgroundColour(wxColour(49, 49, 54));
        m_workbench_history_search->SetForegroundColour(wxColour(235, 235, 235));
    }
    if (m_workbench_history_panel) {
        m_workbench_history_panel->Show(enabled);
    }
    if (m_model_preview && m_model_preview->GetParent()) {
        auto* model_card = m_model_preview->GetParent();
        model_card->SetWindowStyleFlag(enabled ? wxBORDER_NONE : wxBORDER_SIMPLE);
        model_card->SetMinSize(enabled ? wxSize(FromDIP(640), FromDIP(480))
                                       : wxSize(FromDIP(440), FromDIP(560)));
        m_model_preview->SetMinSize(enabled ? wxSize(FromDIP(640), FromDIP(480))
                                            : wxSize(FromDIP(420), FromDIP(280)));
        model_card->SetBackgroundColour(enabled ? wxColour(49, 49, 54) : *wxWHITE);
        if (model_card->GetParent())
            model_card->GetParent()->SetBackgroundColour(enabled ? wxColour(49, 49, 54) : wxColour(241, 244, 245));
    }
    // Switching views is not a model transaction. Keep the current Beauty
    // timeline so returning to the workbench can still undo its edits.
    if (enabled && m_preview_book) { m_preview_book->SetSelection(0); show_model_comparison(); }
    m_workflow_panel->Show(!enabled);
    m_preview_area->Show(!enabled);
    m_expand_images->SetValue(false);
    m_expand_images->Show(!enabled);
    m_zoom_out->Show(!enabled); m_zoom_in->Show(!enabled);
    m_zoom_fit->Show(!enabled); m_preview_zoom->Show(!enabled);
    m_finishing_shortcut->SetLabel(enabled ? _L("返回作品／导入") : _L("3D 美颜工作台"));
    m_preview_stage_hint->Show(!enabled);
    m_preview_message->Show(!enabled);
    m_result_summary->Show(!enabled);
    m_preview_kind->SetLabel(enabled ? _L("美颜工作台") : _L("结果对照"));
    if (enabled) {
        m_local_recolor_toggle->SetValue(false);
        const int operation = m_beauty_controls ? m_beauty_controls->operation_index() : 0;
        if (m_finishing_tool && operation >= 0 && operation < int(std::size(beauty_finishing_tools)))
            m_finishing_tool->SetSelection(beauty_finishing_tools[operation]);
    }
    if (m_finishing_tool) m_finishing_tool->Show(!enabled);
    m_model_decision_panel->Hide();
    refresh_local_recolor_controls();
    if (!enabled) { m_finishing_gray->SetValue(false); m_model_preview->set_gray_view(false); }
    update_finishing_selection();
    refresh_model_finishing();
    Layout();
    m_comparison_panel->Layout();
    // Re-evaluate the three-column breakpoint immediately when switching
    // modes; the panel size itself may not change during the toggle.
    m_comparison_panel->SendSizeEvent();
    m_model_page->Layout(); m_model_page->FitInside(); m_model_page->Scroll(0, 0);
    // Thumbnail targets depend on which history surface is visible after the switch.
    if (enabled) refresh_workbench_history();
    if (enabled && m_library_entries.empty())
        load_library_entries();
}

PostGenerationUiState ModelGenerationPanel::post_generation_ui_state() const
{
    return derive_post_generation_ui_state(
        m_finishing_workbench ? PostGenerationUiState::Mode::Workbench
                              : PostGenerationUiState::Mode::Result,
        m_model_preview_ready,
        !m_displayed_model_path.empty(),
        m_busy || m_preview_loading,
        m_finishing_running,
        (m_beauty_transactions && m_beauty_transactions->processing()) ||
            (m_finishing_workbench && m_model_preview && m_model_preview->selection_busy()),
        !m_finishing_candidate.empty(),
        m_finishing_before || m_finishing_compare_held,
        m_finishing_workbench && m_beauty_controls && m_beauty_controls->has_changes(),
        !m_finishing_undo_path.empty() || (m_model_preview && m_model_preview->can_undo_selection()) ||
            (m_beauty_transactions && m_beauty_transactions->undo_count() > 0),
        !m_finishing_redo_path.empty() || (m_model_preview && m_model_preview->can_redo_selection()) ||
            (m_beauty_transactions && m_beauty_transactions->redo_count() > 0),
        m_workbench_load_error);
}

void ModelGenerationPanel::update_finishing_selection()
{
    const bool local = m_finishing_workbench && m_workbench_editing && (m_finishing_tool->GetSelection() == 1 || m_finishing_tool->GetSelection() == 4 || m_finishing_tool->GetSelection() == 5);
    m_model_preview->set_selection_enabled(local && !m_busy &&
        (m_finishing_candidate.empty() || m_finishing_workbench));
    if (!local) return;
    const auto selected = m_model_preview->selected_face_count();
    const auto protected_faces = m_model_preview->protected_face_count();
    m_finishing_selection_status->SetLabel(wxString::Format(_L("已选 %llu 个面 · 保护 %llu 个面"),
        static_cast<unsigned long long>(selected), static_cast<unsigned long long>(protected_faces)));
    update_region_mode();
}

void ModelGenerationPanel::refresh_model_finishing()
{
    if (!m_finishing_panel) return;
    std::unique_ptr<wxWindowUpdateLocker> workbench_updates;
    if (m_finishing_workbench && m_workbench_shell)
        workbench_updates = std::make_unique<wxWindowUpdateLocker>(m_workbench_shell);
    if (!m_finishing_undo_path.empty() && m_displayed_model_path != m_finishing_accepted_path) {
        m_finishing_undo_path.clear(); m_finishing_accepted_path.clear();
    }
    if (!m_finishing_running && !m_finishing_candidate.empty() && m_displayed_model_path != m_finishing_source) {
        // Selecting a different library model invalidates only the unaccepted
        // local preview, never the selected file or its history record.
        boost::system::error_code ignored;
        if (m_displayed_model_path != m_finishing_candidate)
            boost::filesystem::remove(m_finishing_candidate, ignored);
        clear_unaccepted_beauty_candidates();
        m_beauty_session_source.reset();
        m_beauty_reoptimization_before.reset();
        m_finishing_candidate_region_evidence.reset();
        m_finishing_candidate_secondary_evidence.reset();
        m_finishing_candidate_secondary_error.clear();
        if (m_beauty_transactions) m_beauty_transactions->reset();
        m_finishing_candidate.clear(); m_finishing_source.clear(); m_finishing_undo_path.clear();
        m_model_preview->set_selection_preview_suppressed(false);
    }
    const bool pending = !m_finishing_candidate.empty();
    const bool ready = m_model_preview_ready && is_nonempty_model(m_displayed_model_path);
    const bool transaction_busy = (m_beauty_transactions && m_beauty_transactions->processing()) ||
        (m_finishing_workbench && m_model_preview->selection_busy());
    const bool repaint_layout = m_finishing_workbench &&
        (m_finishing_panel->IsShown() != m_workbench_editing ||
         m_workbench_history_panel->IsShown() != (!m_workbench_editing && !m_workbench_history_collapsed) ||
         m_workbench_footer->IsShown() != (m_workbench_editing || m_finishing_running || pending ||
                                          (m_beauty_transactions && m_beauty_transactions->processing())));
    m_finishing_panel->Show(m_finishing_workbench && m_workbench_editing && (ready || m_finishing_running || pending));
    const auto beauty_source = pending ? m_finishing_candidate : m_displayed_model_path;
    if (m_beauty_controls)
        m_beauty_controls->synchronize(beauty_source, ready && !m_busy && !transaction_busy &&
            !m_finishing_before && !m_finishing_compare_held,
            m_finishing_workbench, m_busy || m_finishing_running || transaction_busy, pending);
    const bool editable = ready && !m_busy && !transaction_busy && !m_model_preview->selection_busy();
    const int tool = m_finishing_tool->GetSelection();
    const bool cleanup = tool == 5;
    const bool local = tool == 1 || cleanup || tool == 4;
    const bool color = tool == 2 || tool == 4;
    if (!m_finishing_workbench || m_workbench_editing)
        m_model_preview->set_color_controls_visible(!m_finishing_workbench || tool == 2);
    const bool selection_visible = local || tool == 4;
    const bool geometry_smoothing_enabled = m_finishing_workbench && tool == 0;
    m_finishing_selection_section->Show(m_finishing_workbench && selection_visible);
    m_finishing_selection_section->SetLabel(m_finishing_selection_open ? _L("收起选择区域") : _L("选择区域"));
    m_finishing_selection_controls->Show(selection_visible && (!m_finishing_workbench || m_finishing_selection_open));
    m_finishing_overlay_row->Show(selection_visible && (!m_finishing_workbench || m_finishing_selection_open));
    m_finishing_overlay->Enable(editable && !pending);
    const bool beauty_editable = m_finishing_workbench && editable;
    m_finishing_selection_controls->Enable((editable && !pending) || beauty_editable);
    m_workbench_smoothing_iterations->Enable(beauty_editable && !pending && geometry_smoothing_enabled);
    m_workbench_preserve_hard_edges->Enable(beauty_editable && !pending && geometry_smoothing_enabled);
    m_finishing_tool->Enable(editable && !pending);
    m_finishing_preset->Show(!m_finishing_workbench && !color && tool != 3 && !cleanup);
    m_finishing_smooth->Show(!m_finishing_workbench && !color && tool != 3 && !cleanup);
    m_finishing_strength->Show(!color && tool != 3 && !(m_finishing_workbench && m_beauty_controls && m_beauty_controls->geometry_deform_selected()));
    m_finishing_strength_value->Show(!color && tool != 3 && !(m_finishing_workbench && m_beauty_controls && m_beauty_controls->geometry_deform_selected()));
    m_finishing_strength_value->SetLabel(wxString::Format(_L("处理强度：%d%%"), m_finishing_strength->GetValue()));
    m_finishing_cleanup_hint->Show(cleanup);
    m_finishing_gray->Show(!m_finishing_workbench && !color && !cleanup);
    m_finishing_strength->SetToolTip(cleanup
        ? _L("力度越大，可合并的杂色块越大。仅处理选区内部；不会自动识别五官、纽扣或花纹。")
        : _L("强度越高，柔化越明显。保护轮廓与细小结构；可随时调整并重新预览。"));
    m_finishing_repair->Show(!m_finishing_workbench && tool == 3);
    m_finishing_compare_model->Show(pending);
    m_finishing_compare_model->Enable(editable);
    m_finishing_compare_model->SetLabel(m_finishing_workbench
        ? (m_finishing_before ? _L("查看处理后") : _L("查看处理前"))
        : (m_finishing_before ? _L("当前为处理前") : _L("按住查看处理前")));
    m_finishing_compare_model->GetParent()->Layout();
    m_finishing_preview->Enable(editable);
    m_finishing_preview->SetLabel(pending ? _L("按当前强度重新预览") : cleanup ? _L("预览去杂效果") : _L("预览处理效果"));
    m_finishing_preset->Enable(editable);
    m_finishing_smooth->Enable(editable);
    m_finishing_repair->Enable(editable);
    m_finishing_strength->Enable(editable && (cleanup || m_finishing_smooth->GetValue()));
    for (wxButton* button : {m_finishing_compare, m_finishing_accept, m_finishing_discard}) {
        button->Show(pending && !m_finishing_workbench); button->Enable(editable);
    }
    m_finishing_preview->Show(!m_finishing_workbench && !m_finishing_running && (!color || (tool == 4 && pending)));
    m_finishing_cancel->Show(!m_finishing_workbench && (m_finishing_running || transaction_busy));
    m_finishing_undo->Show(!m_finishing_workbench && !m_finishing_undo_path.empty() && !pending && !m_finishing_running);
    m_finishing_undo->Enable(editable);
    if (!m_finishing_redo_path.empty() && m_displayed_model_path != m_finishing_redo_source)
        m_finishing_redo_path.clear();
    m_finishing_redo->Show(!m_finishing_workbench && !m_finishing_redo_path.empty() && !pending && !m_finishing_running);
    m_finishing_redo->Enable(editable);
    if (pending || m_finishing_running || transaction_busy) {
        m_status->SetLabel(m_finishing_running ? _L("正在本地处理，可切换页面或取消。")
            : transaction_busy ? _L("正在处理人像区域，可旋转、缩放、查看日志或取消。")
            : _L("美颜预览就绪，接受新版本后可导入。"));
        m_import->Disable(); m_recheck_model->Disable(); m_visual_review_model->Disable();
        if (!beauty_editable) m_local_recolor_panel->Hide();
        m_discard->Disable();
        m_preprocess->Disable(); m_generate->Disable();
        // Disabling selection cancels its worker; let the busy guard block new input.
        if (!beauty_editable && !m_model_preview->selection_busy()) m_model_preview->set_selection_enabled(false);
        if (local) m_finishing_selection_status->SetLabel(wxString::Format(_L("本次处理 %llu 个面 · 未选区域受保护"),
            static_cast<unsigned long long>(m_finishing_options.selected_faces.size())));
    }
    if (m_finishing_running) m_stop->Hide();
    if (m_displayed_model_job_id.rfind("finish-", 0) == 0) {
        m_recheck_model->Disable(); m_visual_review_model->Disable();
        m_recheck_model->SetToolTip(_L("本地处理版本请导入准备页，检查实际打印条件。"));
    }
    wrap_workbench_text(m_finishing_status,
        std::max(FromDIP(80), m_finishing_panel->GetClientSize().x - FromDIP(34)));
    wrap_workbench_text(m_finishing_selection_status, FromDIP(240));
    m_finishing_panel->Layout();
    static_cast<wxScrolledWindow*>(m_finishing_panel)->FitInside();
    if (auto* page = dynamic_cast<wxScrolledWindow*>(m_finishing_panel->GetParent())) {
        page->Layout(); page->FitInside();
    }
    if (m_workbench_history_scroller) {
        const bool can_switch_version = post_generation_ui_state().can_switch_version;
        if (m_workbench_history_upload) m_workbench_history_upload->Enable(can_switch_version);
        const bool switch_state_changed = m_workbench_history_scroller->IsEnabled() != can_switch_version;
        m_workbench_history_scroller->Enable(can_switch_version);
        if (switch_state_changed && m_finishing_workbench)
            refresh_workbench_history();
    }
    if (m_finishing_workbench) {
        const auto state = post_generation_ui_state();
        m_beauty_controls->set_history_permissions(state.can_undo, state.can_redo);
        refresh_post_generation_workbench();
    }
    // Paint the final visible surfaces after thawing, before returning from a mode switch.
    workbench_updates.reset();
    if (repaint_layout) repaint_workbench_surface(m_workbench_shell);
}

ModelGenerationPanel::BeautyCandidateSnapshot ModelGenerationPanel::capture_beauty_candidate() const
{
    BeautyCandidateSnapshot snapshot;
    snapshot.source = m_finishing_source.empty() ? m_displayed_model_path : m_finishing_source;
    snapshot.candidate = m_finishing_candidate;
    snapshot.model_sha256 = snapshot.candidate.empty()
        ? (m_beauty_session_source && m_beauty_session_source->source == snapshot.source
            ? m_beauty_session_source->model_sha256 : AI::model_artifact_sha256(snapshot.source))
        : (!m_finishing_result.output_sha256.empty() ? m_finishing_result.output_sha256
            : AI::model_artifact_sha256(snapshot.candidate));
    snapshot.id = m_finishing_id;
    snapshot.geometry_id = m_model_preview->geometry_id();
    snapshot.result = m_finishing_result;
    snapshot.options = m_finishing_options;
    snapshot.selection = m_model_preview->selection_state();
    snapshot.color_trial = m_model_preview->color_trial_state();
    snapshot.face_overrides = m_finishing_candidate.empty()
        ? m_model_preview->face_color_overrides() : m_finishing_candidate_face_overrides;
    snapshot.semantic_faces = m_finishing_candidate.empty() && m_model_preview->semantic_result_active()
        ? m_model_preview->import_face_color_overrides(true) : m_finishing_candidate_semantic_faces;
    snapshot.semantic_subfaces = m_finishing_candidate.empty() && m_model_preview->semantic_result_active()
        ? m_model_preview->import_subface_color_overrides(true) : m_finishing_candidate_semantic_subfaces;
    snapshot.semantic_provenance = m_finishing_candidate_semantic_provenance;
    snapshot.region_evidence = m_model_preview->semantic_region_evidence();
    snapshot.region_evidence_error = m_model_preview->semantic_region_evidence_error();
    snapshot.secondary_evidence = m_model_preview->secondary_region_evidence();
    snapshot.secondary_evidence_error = m_model_preview->secondary_region_evidence_error();
    return snapshot;
}

bool ModelGenerationPanel::restore_beauty_candidate(const BeautyCandidateSnapshot& snapshot)
{
    const auto path = snapshot.candidate.empty() ? snapshot.source : snapshot.candidate;
    if (!is_nonempty_model(path)) return false;
    if (snapshot.model_sha256.empty() || AI::model_artifact_sha256(path) != snapshot.model_sha256) return false;
    m_finishing_source = snapshot.source;
    m_finishing_candidate = snapshot.candidate;
    m_finishing_id = snapshot.id;
    m_finishing_result = snapshot.result;
    m_finishing_options = snapshot.options;
    m_finishing_candidate_face_overrides = snapshot.face_overrides;
    m_finishing_candidate_semantic_faces = snapshot.semantic_faces;
    m_finishing_candidate_semantic_subfaces = snapshot.semantic_subfaces;
    m_finishing_candidate_semantic_provenance = snapshot.semantic_provenance;
    m_finishing_candidate_region_evidence = snapshot.region_evidence;
    m_finishing_candidate_region_error = snapshot.region_evidence_error;
    m_finishing_candidate_secondary_evidence = snapshot.secondary_evidence;
    m_finishing_candidate_secondary_error = snapshot.secondary_evidence_error;
    if (!show_finishing_version(path)) return false;
    m_model_preview->restore_semantic_region_evidence(snapshot.region_evidence, snapshot.region_evidence_error);
    m_model_preview->restore_secondary_region_evidence(snapshot.secondary_evidence, snapshot.secondary_evidence_error);
    m_model_preview->restore_color_trial_without_recognition(snapshot.color_trial);
    if (!snapshot.semantic_faces.empty() || !snapshot.semantic_subfaces.empty())
        m_model_preview->set_saved_semantic_result(snapshot.semantic_faces, snapshot.semantic_subfaces);
    if (snapshot.geometry_id == m_model_preview->geometry_id() &&
        snapshot.selection.selected.size() == m_model_preview->triangle_count())
        m_model_preview->restore_selection_state(snapshot.selection);
    m_finishing_selection_state = snapshot.selection;
    m_finishing_before = false;
    m_finishing_compare->SetLabel(_L("查看处理前"));
    if (m_beauty_transactions) {
        if (snapshot.candidate.empty()) m_beauty_transactions->mark_editing();
        else m_beauty_transactions->mark_preview_ready();
    }
    refresh_controls();
    update_finishing_selection();
    return true;
}

void ModelGenerationPanel::record_beauty_candidate(BeautyWorkbenchTransactionController::OperationKind kind,
                                                   std::shared_ptr<BeautyCandidateSnapshot> before)
{
    if (!m_finishing_workbench || !m_beauty_transactions || !before) return;
    auto after = std::make_shared<BeautyCandidateSnapshot>(capture_beauty_candidate());
    m_beauty_candidate_files.push_back(after->candidate);
    const auto valid = [](const BeautyCandidateSnapshot& snapshot) {
        const auto& path = snapshot.candidate.empty() ? snapshot.source : snapshot.candidate;
        return is_nonempty_model(path) && !snapshot.model_sha256.empty() &&
            AI::model_artifact_sha256(path) == snapshot.model_sha256;
    };
    m_beauty_transactions->record({kind, "Beauty candidate",
        [this, before] { restore_beauty_candidate(*before); },
        [this, after] { restore_beauty_candidate(*after); },
        [before, valid] { return valid(*before); },
        [after, valid] { return valid(*after); }});
}

void ModelGenerationPanel::clear_unaccepted_beauty_candidates(size_t start)
{
    if (start > m_beauty_candidate_files.size()) return;
    for (size_t index = start; index < m_beauty_candidate_files.size(); ++index) {
        const auto& path = m_beauty_candidate_files[index];
        if (path.empty() || path == m_finishing_accepted_path ||
            std::find(m_beauty_accepted_files.begin(), m_beauty_accepted_files.end(), path) != m_beauty_accepted_files.end())
            continue;
        boost::system::error_code ignored;
        boost::filesystem::remove(path, ignored);
    }
    m_beauty_candidate_files.resize(start);
}

void ModelGenerationPanel::preview_model_finishing()
{
    if (m_busy || m_shutdown || !m_model_preview_ready) return;
    if (m_model_preview->selection_busy()) {
        m_finishing_status->SetLabel(_L("正在更新选区，完成后即可预览；可按 Esc 取消选区计算。")); return;
    }
    const bool cleanup = m_finishing_tool->GetSelection() == 5;
    const bool recolor = m_finishing_tool->GetSelection() == 4;
    const bool local = m_finishing_tool->GetSelection() == 1 || cleanup || recolor;
    // Beauty edits always operate on the currently visible candidate and its
    // current selection. The legacy path keeps its saved selection snapshot
    // while a candidate is pending.
    auto selected_faces = local ? ((m_finishing_workbench || m_finishing_candidate.empty())
        ? m_model_preview->selected_face_indices() : m_finishing_options.selected_faces) : std::vector<size_t>{};
    if (local && selected_faces.empty()) {
        m_finishing_status->SetLabel(cleanup ? _L("请先圈住杂点及周围主色，涂抹保护花纹等细节；未选区域不会改变。")
            : recolor ? _L("请先圈选要换色的区域；涂抹保护可排除需要保留的细节。")
            : _L("请先在模型上选择要柔化的区域；未选区域不会改变。"));
        wrap_workbench_text(m_finishing_status,
            std::max(FromDIP(80), m_finishing_panel->GetClientSize().x - FromDIP(34)));
        m_finishing_panel->Layout();
        return;
    }
    const auto source = m_finishing_workbench && !m_finishing_candidate.empty()
        ? m_finishing_candidate : m_displayed_model_path;
    if (!is_nonempty_model(source)) {
        m_finishing_status->SetLabel(_L("模型文件已不存在，请从模型库重新加载。")); return;
    }
    auto before = m_finishing_workbench
        ? std::make_shared<BeautyCandidateSnapshot>(capture_beauty_candidate()) : nullptr;
    if (before && m_finishing_candidate.empty() && !m_beauty_session_source) {
        m_beauty_session_source = before;
        m_beauty_session_undo_base = m_beauty_transactions ? m_beauty_transactions->undo_count() : 0;
        m_beauty_session_file_base = m_beauty_candidate_files.size();
    }
    if (m_finishing_candidate.empty() || m_finishing_workbench) {
        m_finishing_selection_state = m_model_preview->selection_state();
        m_finishing_restore_selection = [this, selection_state = m_finishing_selection_state] {
            m_model_preview->restore_selection_state(selection_state);
        };
    }
    const int tool = m_finishing_tool->GetSelection();
    const bool geometry_smoothing = m_finishing_workbench && tool == 0;
    // Workbench operations are exclusive; hidden legacy checkboxes may still
    // contain the values from the result page.
    const bool smooth = m_finishing_workbench ? (tool == 0 || tool == 1) : m_finishing_smooth->GetValue();
    const bool repair = m_finishing_workbench ? tool == 3 : m_finishing_repair->GetValue();
    AI::ModelFinishingOptions options {!cleanup && !recolor && smooth, !local && repair, m_finishing_strength->GetValue() / 100.0};
    if (m_finishing_workbench && m_workbench_smoothing_iterations && m_workbench_preserve_hard_edges) {
        options.smoothing_iterations = geometry_smoothing ? m_workbench_smoothing_iterations->GetValue() : 0;
        options.preserve_hard_edges = !geometry_smoothing || m_workbench_preserve_hard_edges->GetValue();
    }
    options.selected_faces = selected_faces;
    options.clean_color_spots = cleanup;
    options.recolor_selected = recolor;
    if (AI::model_artifact_format(source) == "glb" && (options.repair_mesh || cleanup)) {
        m_finishing_status->SetLabel(cleanup
            ? _L("GLB 的保真保存暂不支持清理杂点。可圈选后统一这块颜色，并保留原版用于对照。")
            : _L("为保留 GLB 原始贴图，请在这里选择表面柔化。需要修复网格时，可先导入准备页，再使用修复功能。"));
        wrap_workbench_text(m_finishing_status,
            std::max(FromDIP(80), m_finishing_panel->GetClientSize().x - FromDIP(34)));
        m_finishing_panel->Layout();
        return;
    }
    if (recolor) {
        m_finishing_color_palette = local_recolor_palette();
        if (m_region_color_index < 0 || size_t(m_region_color_index) >= m_finishing_color_palette.size()) {
            m_finishing_status->SetLabel(_L("请先选择一个可用耗材颜色。")); return;
        }
        const wxColour target(from_u8(m_finishing_color_palette[m_region_color_index]));
        if (!target.IsOk()) return;
        options.target_color = {target.Red()/255.0f, target.Green()/255.0f, target.Blue()/255.0f, 1.0f};
        // Embedded-texture GLBs use the Beauty appearance path so local color
        // edits do not collapse the source texture into vertex colors. The
        // existing face overrides are still composed by ModelPreview3D after
        // the Beauty artifact is loaded.
        if (AI::model_artifact_format(source) == "glb") {
            const auto editor = m_model_preview->beauty_editor();
            if (!editor) {
                m_finishing_status->SetLabel(_L("正在准备局部 Beauty 编辑数据，请稍后重试。"));
                return;
            }
            auto surface = AI::BeautySurface::build(editor->mesh(), editor->vertex_colors());
            AI::BeautyDocument document;
            document.geometry_id = m_model_preview->geometry_id();
            document.face_count = editor->mesh().indices.size();
            document.face_patch = surface->face_patch;
            options.beauty_appearance = true;
            options.recolor_selected = false;
            options.smooth_surface = false;
            options.repair_mesh = false;
            options.beauty_surface = std::move(surface);
            options.beauty_document = document.encode();
            options.appearance.face_weights.assign(document.face_count, 0.f);
            options.appearance.face_target_colors.resize(document.face_count);
            for (size_t face : options.selected_faces)
                if (face < options.appearance.face_weights.size() &&
                    (face >= m_finishing_selection_state.protected_faces.size() ||
                     !m_finishing_selection_state.protected_faces[face])) {
                    options.appearance.face_weights[face] = 1.f;
                    options.appearance.face_target_colors[face] = {
                        options.target_color[0], options.target_color[1], options.target_color[2]};
                }
            options.beauty_protected_faces = m_finishing_selection_state.protected_faces;
        }
    }
    if (cleanup) options.cleanup_palette = m_finishing_candidate.empty()
        ? m_model_preview->color_trial_mapping().mapping_colors : m_finishing_options.cleanup_palette;
    options.selected_faces = std::move(selected_faces);
    if (m_finishing_workbench && m_beauty_controls && m_beauty_controls->geometry_deform_selected()) {
        if (AI::model_artifact_format(source) != "glb" || !m_beauty_controls->ready() ||
            m_beauty_controls->displacement_mm() == 0.) {
            m_finishing_status->SetLabel(_L("局部拉伸需要 GLB、有效选区和非零推拉距离。"));
            return;
        }
        m_beauty_controls->prepare_options(options, m_finishing_selection_state);
        options.beauty_appearance = false;
        options.beauty_deform = true;
        options.beauty_displacement_mm = m_beauty_controls->displacement_mm();
        options.beauty_falloff_mm = m_beauty_controls->falloff_mm();
    }
    if (!options.smooth_surface && !options.repair_mesh && !options.clean_color_spots && !recolor && !options.beauty_deform) {
        m_finishing_status->SetLabel(_L("请至少选择表面美化或网格修复。")); return;
    }
    if (!m_finishing_candidate.empty() && !m_finishing_workbench) {
        if (!m_finishing_before && !show_finishing_version(m_finishing_source)) return;
        boost::system::error_code ignored;
        boost::filesystem::remove(m_finishing_candidate, ignored);
        m_finishing_candidate.clear();
    }
    if (m_finishing_worker.joinable()) m_finishing_worker.join();
    const auto color_state = m_model_preview->color_trial_state();
    const auto automatic_semantic_faces = m_finishing_workbench && m_model_preview->semantic_result_active()
        ? m_model_preview->import_face_color_overrides(true) : ModelPreview3D::FaceColorOverrides {};
    const auto automatic_semantic_subfaces = m_finishing_workbench && m_model_preview->semantic_result_active()
        ? m_model_preview->import_subface_color_overrides(true) : ModelPreview3D::SubfaceColorOverrides {};
    auto face_overrides = options.repair_mesh ? ModelPreview3D::FaceColorOverrides {} : m_model_preview->face_color_overrides();
    if (cleanup && !face_overrides.empty()) {
        std::unordered_set<size_t> locked;
        for (const auto& entry : face_overrides) locked.insert(entry.first);
        auto& faces = options.selected_faces;
        faces.erase(std::remove_if(faces.begin(), faces.end(), [&](size_t face) { return locked.count(face) != 0; }), faces.end());
        if (faces.empty()) {
            m_finishing_status->SetLabel(_L("选区均为已指定颜色的表面，清理杂点会保留这些颜色。可用“统一这块颜色”重新指定。"));
            refresh_controls(); return;
        }
    }
    if (recolor) {
        std::map<size_t, std::array<float,3>> colors(face_overrides.begin(), face_overrides.end());
        for (size_t face : options.selected_faces) {
            colors[face] = {options.target_color[0], options.target_color[1], options.target_color[2]};
        }
        face_overrides.assign(colors.begin(), colors.end());
    }
    const bool intent_changed = face_overrides != m_model_preview->face_color_overrides();
    if (m_finishing_workbench && m_model_preview->semantic_result_active()) {
        const auto current_provenance = m_model_preview->semantic_color_metadata();
        if (!current_provenance.empty()) m_finishing_candidate_semantic_provenance = current_provenance;
        m_finishing_candidate_semantic_faces = AI::SemanticColoring::compose(
            automatic_semantic_faces, face_overrides, !automatic_semantic_faces.empty());
        m_finishing_candidate_semantic_subfaces = AI::SemanticColoring::compose_subfaces(
            automatic_semantic_subfaces, face_overrides, !automatic_semantic_subfaces.empty());
    } else {
        m_finishing_candidate_semantic_faces.clear();
        m_finishing_candidate_semantic_subfaces.clear();
        m_finishing_candidate_semantic_provenance = nlohmann::json::object();
    }
    const auto kind = options.beauty_deform ? BeautyWorkbenchTransactionController::OperationKind::SurfaceSoften
            : recolor ? BeautyWorkbenchTransactionController::OperationKind::AppearanceRecolor
            : cleanup ? BeautyWorkbenchTransactionController::OperationKind::SpotCleanup
            : options.repair_mesh ? BeautyWorkbenchTransactionController::OperationKind::MeshRepair
            : BeautyWorkbenchTransactionController::OperationKind::SurfaceSoften;
    if (m_finishing_workbench && m_beauty_transactions) {
        if (!m_beauty_transactions->begin(kind)) {
            m_finishing_status->SetLabel(_L("当前仍有 Beauty 处理正在进行，请先完成或取消。"));
            refresh_controls();
            return;
        }
    }
    m_finishing_candidate_face_overrides = face_overrides;
    m_finishing_options = options;
    m_finishing_redo_path.clear();
    if (m_finishing_source.empty() || !m_finishing_workbench) {
    m_finishing_source = source;
    m_finishing_source_context = [this, job = m_job_id, displayed = m_displayed_model_job_id,
        artifact = m_artifact_path, source, palette = m_job_palette, roles = m_job_palette_roles,
        display_palette = m_displayed_model_palette, display_roles = m_displayed_model_palette_roles,
        printable = m_job_use_printable_colors, manifest = m_color_intent_path,
        schema = m_color_intent_schema, hash = m_color_intent_sha256, format = m_artifact_format,
        encoding = m_artifact_color_encoding, quality = m_model_quality, visual = m_visual_quality,
        refinement = m_model_refinement, library = m_library_model_loaded, color_state] {
        m_job_id = job; m_displayed_model_job_id = displayed;
        m_artifact_path = artifact; m_displayed_model_path = source;
        m_job_palette = palette; m_job_palette_roles = roles;
        m_displayed_model_palette = display_palette; m_displayed_model_palette_roles = display_roles;
        m_job_use_printable_colors = printable; m_color_intent_path = manifest;
        m_color_intent_schema = schema; m_color_intent_sha256 = hash;
        m_artifact_format = format; m_artifact_color_encoding = encoding;
        m_model_quality = quality; m_visual_quality = visual; m_model_refinement = refinement;
        m_library_model_loaded = library; m_ready = true; m_artifact_download_started = true;
        m_model_preview_ready = true;
        if (m_finishing_workbench) m_model_preview->restore_color_trial_without_recognition(color_state);
        else m_model_preview->restore_color_trial(color_state);
    };
    }
    m_finishing_id = "finish-" + new_request_id();
    const auto destination = source.parent_path() / temp_path(m_finishing_id, AI::model_artifact_format(source)).filename();
    m_finishing_canceled = std::make_shared<std::atomic<bool>>(false);
    const auto canceled = m_finishing_canceled;
    m_finishing_running = true; m_busy = true;
    if (m_workbench_load_error) {
        m_status->SetLabel(wxEmptyString);
        m_result_summary->SetLabel(wxEmptyString);
    }
    m_workbench_load_error = false;
    m_finishing_cancel->Enable();
    m_finishing_status->SetLabel(recolor ? _L("正在生成局部颜色预览，可取消；选区之外保持原样……") : cleanup ? _L("正在清理选区内的小杂色块，可取消……") : _L("正在本地处理三维表面，原始模型保持不变……"));
    refresh_controls();
    wxWeakRef<ModelGenerationPanel> weak(this);
    const uint64_t sequence = m_sequence;
    try {
      m_finishing_worker = std::thread([weak, source, destination, options, canceled, sequence, color_state, intent_changed, before, kind, recolor, cleanup, face_overrides = std::move(face_overrides)] {
        const auto result = AI::finish_model_artifact(source, destination, options, [canceled] { return canceled->load(); });
        auto prepared = std::make_shared<ModelPreview3D::PreparedModel>();
        std::string preview_error;
        if (result.success && (result.changed() || intent_changed) && !canceled->load()) {
            try { ModelPreview3D::prepare_model(destination, *prepared, preview_error, face_overrides); }
            catch (const std::exception& e) { preview_error = e.what(); }
        }
        wxGetApp().CallAfter([weak, source, destination, result, sequence, canceled, prepared, preview_error, color_state, intent_changed, before, kind, recolor, cleanup] {
            if (!weak || weak->m_shutdown || sequence != weak->m_sequence) {
                if (result.success) { boost::system::error_code ignored; boost::filesystem::remove(destination, ignored); }
                return;
            }
            auto* self = weak.get();
            if (self->m_finishing_worker.joinable()) self->m_finishing_worker.join();
            self->m_finishing_running = false; self->m_busy = false;
            self->m_finishing_result = result;
            if (result.canceled || canceled->load()) {
                if (self->m_beauty_transactions) self->m_beauty_transactions->finish(false, false, "cancelled");
                if (result.success) { boost::system::error_code ignored; boost::filesystem::remove(destination, ignored); }
                if (before) self->restore_beauty_candidate(*before);
                self->m_finishing_status->SetLabel(_L("已取消，原始模型保持不变。"));
            }
            else if (!result.success) {
                if (self->m_beauty_transactions) self->m_beauty_transactions->finish(false, false, result.error);
                if (before) self->restore_beauty_candidate(*before);
                self->m_finishing_status->SetLabel(_L("处理未完成，原件已保留：") + from_u8(result.error));
            }
            else if (!result.changed() && !intent_changed) {
                if (self->m_beauty_transactions) self->m_beauty_transactions->finish(false, false, "no change");
                boost::system::error_code ignored; boost::filesystem::remove(destination, ignored);
                if (before) self->restore_beauty_candidate(*before);
                self->m_finishing_status->SetLabel(recolor
                    ? _L("选区已经是这个颜色，无需重复保存。可以选择其他颜色或继续编辑范围。")
                    : cleanup
                    ? _L("未找到可合并的小杂色块。可扩大选区包含周围主色，或用“统一这块颜色”处理连续色带。")
                    : _L("当前设置没有改变模型；可扩大选区或调整强度，边界与锐边保持保护。"));
            } else {
                const auto view = self->m_model_preview->view_state();
                size_t triangles = 0, colors = 0; Vec3d dimensions; std::string error = preview_error;
                if (!error.empty() || !self->m_model_preview->load_prepared_model(
                    std::move(*prepared), {}, triangles, dimensions, colors, error)) {
                    if (self->m_beauty_transactions) self->m_beauty_transactions->finish(false, false, error);
                    boost::system::error_code ignored; boost::filesystem::remove(destination, ignored);
                    if (before) self->restore_beauty_candidate(*before);
                    self->m_finishing_status->SetLabel(_L("预览未完成，原件已保留：") + from_u8(error));
                    self->refresh_controls(); return;
                }
                self->m_model_preview->restore_view(view);
                if (before) self->m_model_preview->restore_semantic_region_evidence(before->region_evidence, before->region_evidence_error);
                self->m_finishing_candidate_region_evidence = self->m_model_preview->semantic_region_evidence();
                self->m_finishing_candidate_region_error = self->m_model_preview->semantic_region_evidence_error();
                if (before) self->m_model_preview->transfer_secondary_region_evidence(
                    before->secondary_evidence, before->model_sha256);
                self->m_finishing_candidate_secondary_evidence = self->m_model_preview->secondary_region_evidence();
                self->m_finishing_candidate_secondary_error = self->m_model_preview->secondary_region_evidence_error();
                if (self->m_finishing_workbench &&
                    (!self->m_finishing_candidate_semantic_faces.empty() ||
                     !self->m_finishing_candidate_semantic_subfaces.empty())) {
                    self->m_model_preview->restore_color_trial_without_recognition(color_state);
                    if (!self->m_model_preview->set_saved_semantic_result(
                        self->m_finishing_candidate_semantic_faces,
                        self->m_finishing_candidate_semantic_subfaces)) {
                        boost::system::error_code ignored; boost::filesystem::remove(destination, ignored);
                        if (before) self->restore_beauty_candidate(*before);
                        if (self->m_beauty_transactions) self->m_beauty_transactions->finish(false, false, "semantic preview unavailable");
                        self->m_finishing_status->SetLabel(_L("语义预览无法加载，已保留处理前版本。"));
                        self->refresh_controls(); return;
                    }
                } else if (self->m_finishing_workbench)
                    self->m_model_preview->restore_color_trial_without_recognition(color_state);
                else self->m_model_preview->restore_color_trial(color_state);
                // Match the before-view summary before exposing comparison:
                // a stale loading row changes the viewport height on first compare.
                self->m_model_stats->SetLabel(wxString::Format(_L("%llu 个三角面 · %llu 个原始色值\n%.1f × %.1f × %.1f mm"),
                    static_cast<unsigned long long>(triangles), static_cast<unsigned long long>(colors),
                    dimensions.x(), dimensions.y(), dimensions.z()));
                self->m_finishing_candidate = destination; self->m_finishing_before = false;
                if (self->m_finishing_workbench)
                    self->m_model_preview->set_selection_preview_suppressed(true);
                if (self->m_beauty_transactions) self->m_beauty_transactions->finish(true, true);
                if (self->m_finishing_workbench) self->refresh_workbench_history();
                if (before) {
                    if (before->geometry_id == self->m_model_preview->geometry_id() &&
                        before->selection.selected.size() == self->m_model_preview->triangle_count())
                        self->m_model_preview->restore_selection_state(before->selection);
                    self->record_beauty_candidate(kind, before);
                }
                self->m_finishing_compare->SetLabel(_L("查看处理前"));
                self->m_model_preview_message->SetLabel(_L("处理后 · 尚未接受；可旋转模型并查看处理前对比。"));
                self->m_finishing_status->SetLabel(recolor && self->m_finishing_options.beauty_appearance
                    ? _L("局部颜色候选已生成。选区外与造型保持不变；对比后接受，或放弃预览继续调整范围。")
                    : recolor ? wxString::Format(
                    _L("已统一 %llu 个面的颜色。选区外与造型保持不变；对比后接受，或放弃预览继续调整范围。"),
                    static_cast<unsigned long long>(result.recolored_faces)) : cleanup ? wxString::Format(
                    _L("已清理 %llu 处小杂色块，调整 %llu 个顶点颜色。造型不变；请对比细节后接受新版本。"),
                    static_cast<unsigned long long>(result.cleaned_color_regions),
                    static_cast<unsigned long long>(result.recolored_vertices)) : self->m_finishing_options.beauty_deform ? wxString::Format(
                    _L("已拉伸 %llu 个顶点。请对比处理前后的局部造型，接受或放弃候选版本。"),
                    static_cast<unsigned long long>(result.moved_vertices)) : wxString::Format(
                    _L("已柔化 %llu 个顶点，清理 %llu 个面，校正 %llu 个面。开放边 %llu 条，非流形边 %llu 条（仅作提醒）。"),
                    static_cast<unsigned long long>(result.moved_vertices),
                    static_cast<unsigned long long>(result.removed_degenerate_faces + result.removed_duplicate_faces),
                    static_cast<unsigned long long>(result.reversed_faces),
                    static_cast<unsigned long long>(result.boundary_edges),
                    static_cast<unsigned long long>(result.nonmanifold_edges)));
            }
            self->m_status->SetLabel(self->m_finishing_status->GetLabel());
            self->refresh_controls();
            self->update_finishing_selection();
            if (self->m_finishing_candidate.empty() && self->m_finishing_restore_selection)
                self->m_finishing_restore_selection();
        });
      });
    } catch (const std::exception&) {
        m_finishing_running = false; m_busy = false;
        if (m_beauty_transactions) m_beauty_transactions->finish(false, false, "worker start failed");
        m_finishing_status->SetLabel(_L("暂时无法启动处理，请稍后重试。原始模型保持不变。"));
        refresh_controls();
    }
}

void ModelGenerationPanel::export_semantic_candidate()
{
    if (!m_model_preview || !m_model_preview->semantic_regions_ready() ||
        !is_nonempty_model(m_finishing_candidate.empty() ? m_displayed_model_path : m_finishing_candidate)) {
        if (auto before = std::move(m_beauty_reoptimization_before)) restore_beauty_candidate(*before);
        if (m_beauty_transactions) m_beauty_transactions->finish(false, false, "semantic candidate source unavailable");
        if (m_finishing_status) m_finishing_status->SetLabel(_L("人像区域结果没有可用的模型源，当前版本保持不变。"));
        refresh_model_finishing();
        return;
    }
    if (m_finishing_worker.joinable()) m_finishing_worker.join();
    const auto source = m_finishing_candidate.empty() ? m_displayed_model_path : m_finishing_candidate;
    const auto region_evidence = m_model_preview->semantic_region_evidence();
    const auto secondary_evidence = m_model_preview->secondary_region_evidence();
    auto before = m_beauty_reoptimization_before ? std::move(m_beauty_reoptimization_before)
        : std::make_shared<BeautyCandidateSnapshot>(capture_beauty_candidate());
    if (m_finishing_candidate.empty() && !m_beauty_session_source) {
        m_beauty_session_source = before;
        m_beauty_session_undo_base = m_beauty_transactions ? m_beauty_transactions->undo_count() : 0;
        m_beauty_session_file_base = m_beauty_candidate_files.size();
    }
    const size_t faces = m_model_preview->triangle_count();
    const auto overrides = m_model_preview->import_face_color_overrides(true);
    const auto subfaces = m_model_preview->import_subface_color_overrides(true);
    const auto color_state = m_model_preview->color_trial_state();
    // Automatic semantic colors are baked into the candidate appearance. Keep
    // only explicit manual face locks as preview overrides so a later explicit
    // re-optimization can still replace automatic colors without overriding
    // the user's manual edits.
    const auto manual_overrides = m_model_preview->face_color_overrides();
    if (faces == 0 || overrides.empty()) {
        restore_beauty_candidate(*before);
        if (m_beauty_transactions) m_beauty_transactions->finish(false, false, "semantic result has no face colors");
        m_finishing_status->SetLabel(_L("人像区域没有产生可靠的面级颜色，当前模型保持不变。"));
        refresh_model_finishing();
        return;
    }
    AI::ModelFinishingOptions options;
    options.beauty_appearance = true;
    options.smooth_surface = false;
    options.repair_mesh = false;
    options.recolor_selected = false;
    options.selected_faces.reserve(overrides.size());
    options.appearance.face_weights.assign(faces, 0.f);
    options.appearance.face_target_colors.resize(faces, {0.f, 0.f, 0.f});
    for (const auto& item : overrides) {
        if (item.first >= faces) continue;
        options.selected_faces.push_back(item.first);
        options.appearance.face_weights[item.first] = 1.f;
        options.appearance.face_target_colors[item.first] = item.second;
    }
    if (options.selected_faces.empty()) {
        restore_beauty_candidate(*before);
        if (m_beauty_transactions) m_beauty_transactions->finish(false, false, "semantic face colors are out of range");
        m_finishing_status->SetLabel(_L("人像区域颜色与当前模型面数不一致，已保留原模型。"));
        refresh_model_finishing();
        return;
    }
    AI::BeautyDocument document;
    document.geometry_id = m_model_preview->geometry_id();
    document.face_count = faces;
    document.face_patch.assign(faces, 0);
    options.beauty_document = document.encode();
    m_finishing_options = options;
    m_finishing_candidate_face_overrides = manual_overrides;
    m_finishing_candidate_semantic_faces = overrides;
    m_finishing_candidate_semantic_subfaces = subfaces;
    m_finishing_candidate_semantic_provenance = m_model_preview->semantic_color_metadata();
    // Keep the original stable source for the candidate lifecycle. When a
    // candidate is re-optimized, its predecessor remains temporary and must
    // not become the new invalidation anchor.
    if (m_finishing_source.empty()) m_finishing_source = source;
    m_finishing_id = "finish-semantic-" + new_request_id();
    const auto destination = source.parent_path() /
        temp_path(m_finishing_id, "glb").filename();
    m_finishing_canceled = std::make_shared<std::atomic<bool>>(false);
    const auto canceled = m_finishing_canceled;
    m_finishing_running = true;
    m_busy = true;
    if (m_workbench_load_error) {
        m_status->SetLabel(wxEmptyString);
        m_result_summary->SetLabel(wxEmptyString);
    }
    m_workbench_load_error = false;
    m_finishing_status->SetLabel(_L("正在把人像语义结果写入新的 GLB 版本，可旋转查看或取消……"));
    refresh_controls();
    wxWeakRef<ModelGenerationPanel> weak(this);
    const uint64_t sequence = m_sequence;
    try {
        m_finishing_worker = std::thread([weak, source, destination, options, canceled, sequence, manual_overrides, color_state, before, region_evidence, secondary_evidence] {
            const auto result = AI::finish_model_artifact(source, destination, options,
                [canceled] { return canceled->load(); });
            auto prepared = std::make_shared<ModelPreview3D::PreparedModel>();
            std::string preview_error;
            if (result.success && !canceled->load()) {
                try { ModelPreview3D::prepare_model(destination, *prepared, preview_error, manual_overrides); }
                catch (const std::exception& e) { preview_error = e.what(); }
            }
            wxGetApp().CallAfter([weak, source, destination, result, sequence, canceled, prepared, preview_error, color_state, before, region_evidence, secondary_evidence] {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence) {
                    if (result.success) { boost::system::error_code ignored; boost::filesystem::remove(destination, ignored); }
                    return;
                }
                auto* self = weak.get();
                if (self->m_finishing_worker.joinable()) self->m_finishing_worker.join();
                self->m_finishing_running = false;
                self->m_busy = false;
                self->m_finishing_result = result;
                if (result.canceled || canceled->load()) {
                    if (result.success) { boost::system::error_code ignored; boost::filesystem::remove(destination, ignored); }
                    self->restore_beauty_candidate(*before);
                    if (self->m_beauty_transactions) self->m_beauty_transactions->finish(false, false, "cancelled");
                    self->m_finishing_status->SetLabel(_L("已取消语义 GLB 写出，当前模型保持不变。"));
                } else if (!result.success || !preview_error.empty()) {
                    if (result.success) { boost::system::error_code ignored; boost::filesystem::remove(destination, ignored); }
                    self->restore_beauty_candidate(*before);
                    if (self->m_beauty_transactions) self->m_beauty_transactions->finish(false, false,
                        preview_error.empty() ? result.error : preview_error);
                    self->m_finishing_status->SetLabel(_L("语义 GLB 写出失败，当前模型保持不变：") +
                        from_u8(preview_error.empty() ? result.error : preview_error));
                } else {
                    size_t triangles = 0, colors = 0; Vec3d dimensions; std::string error;
                    if (!self->m_model_preview->load_prepared_model(std::move(*prepared), {}, triangles,
                            dimensions, colors, error)) {
                        boost::system::error_code ignored; boost::filesystem::remove(destination, ignored);
                        self->restore_beauty_candidate(*before);
                        if (self->m_beauty_transactions) self->m_beauty_transactions->finish(false, false, error);
                        self->m_finishing_status->SetLabel(_L("语义 GLB 预览加载失败，当前模型保持不变：") + from_u8(error));
                    } else {
                        self->m_model_preview->restore_semantic_region_evidence(region_evidence);
                        self->m_finishing_candidate_region_evidence = self->m_model_preview->semantic_region_evidence();
                        self->m_finishing_candidate_region_error = self->m_model_preview->semantic_region_evidence_error();
                        self->m_model_preview->transfer_secondary_region_evidence(
                            secondary_evidence, before->model_sha256);
                        self->m_finishing_candidate_secondary_evidence = self->m_model_preview->secondary_region_evidence();
                        self->m_finishing_candidate_secondary_error = self->m_model_preview->secondary_region_evidence_error();
                        self->m_model_preview->restore_color_trial_without_recognition(color_state);
                        if (!self->m_model_preview->set_saved_semantic_result(
                                self->m_finishing_candidate_semantic_faces,
                                self->m_finishing_candidate_semantic_subfaces)) {
                            boost::system::error_code ignored;
                            boost::filesystem::remove(destination, ignored);
                            self->restore_beauty_candidate(*before);
                            if (self->m_beauty_transactions) self->m_beauty_transactions->finish(false, false,
                                "semantic result preview unavailable");
                            self->m_finishing_status->SetLabel(_L("语义候选预览失败，已保留处理前版本。"));
                            self->refresh_controls();
                            return;
                        }
                        self->m_finishing_candidate = destination;
                        self->m_finishing_before = false;
                        self->m_model_preview->set_selection_preview_suppressed(true);
                        self->m_finishing_compare->SetLabel(_L("查看处理前"));
                        if (self->m_beauty_transactions) self->m_beauty_transactions->finish(true, true);
                        if (before->geometry_id == self->m_model_preview->geometry_id() &&
                            before->selection.selected.size() == self->m_model_preview->triangle_count())
                            self->m_model_preview->restore_selection_state(before->selection);
                        self->record_beauty_candidate(
                            BeautyWorkbenchTransactionController::OperationKind::SemanticReoptimization, before);
                        if (self->m_beauty_controls && self->m_model_preview->secondary_regions_ready())
                            self->m_beauty_controls->request_secondary_partition(destination);
                        self->m_finishing_status->SetLabel(self->m_model_preview->secondary_regions_ready()
                            ? _L("人像区域和二级细节已就绪；正在自动划区，可对比、继续编辑或接受。")
                            : _L("人像区域已写入新的 GLB 候选版本；二级细节不可用，一级分区仍可编辑。"));
                        self->m_model_preview_message->SetLabel(_L("语义优化后 · 尚未接受"));
                    }
                }
                self->m_status->SetLabel(self->m_finishing_status->GetLabel());
                self->refresh_controls();
                self->update_finishing_selection();
            });
        });
    } catch (const std::exception& e) {
        m_finishing_running = false;
        m_busy = false;
        restore_beauty_candidate(*before);
        if (m_beauty_transactions) m_beauty_transactions->finish(false, false, e.what());
        m_finishing_status->SetLabel(_L("无法启动语义 GLB 写出，当前模型保持不变：") + from_u8(e.what()));
        refresh_controls();
    }
}

bool ModelGenerationPanel::show_finishing_version(const boost::filesystem::path& path)
{
    const auto view = m_model_preview->view_state();
    const auto color_state = m_model_preview->color_trial_state();
    size_t triangles = 0, colors = 0; Vec3d dimensions = Vec3d::Zero(); std::string error;
    const bool session_source = m_finishing_workbench && m_beauty_session_source &&
        path == m_beauty_session_source->source;
    const auto explicit_overrides = path == m_finishing_candidate ? m_finishing_candidate_face_overrides
        : session_source ? m_beauty_session_source->face_overrides : ModelPreview3D::FaceColorOverrides {};
    if (!m_model_preview->load_model(path, {}, triangles, dimensions, colors, error, explicit_overrides)) {
        m_model_preview_ready = false;
        m_finishing_status->SetLabel(_L("预览加载失败，原件仍保留：") + from_u8(error));
        return false;
    }
    m_model_preview->restore_view(view);
    if (path == m_finishing_candidate)
        m_model_preview->restore_semantic_region_evidence(m_finishing_candidate_region_evidence, m_finishing_candidate_region_error);
    else if (session_source)
        m_model_preview->restore_semantic_region_evidence(m_beauty_session_source->region_evidence, m_beauty_session_source->region_evidence_error);
    if (path == m_finishing_candidate)
        m_model_preview->restore_secondary_region_evidence(m_finishing_candidate_secondary_evidence, m_finishing_candidate_secondary_error);
    else if (session_source)
        m_model_preview->restore_secondary_region_evidence(m_beauty_session_source->secondary_evidence, m_beauty_session_source->secondary_evidence_error);
    if (path == m_finishing_candidate &&
        (!m_finishing_candidate_semantic_faces.empty() || !m_finishing_candidate_semantic_subfaces.empty())) {
        m_model_preview->restore_color_trial_without_recognition(color_state);
        m_model_preview->set_saved_semantic_result(
            m_finishing_candidate_semantic_faces, m_finishing_candidate_semantic_subfaces);
    } else if (session_source && (!m_beauty_session_source->semantic_faces.empty() ||
                                  !m_beauty_session_source->semantic_subfaces.empty())) {
        m_model_preview->restore_color_trial_without_recognition(m_beauty_session_source->color_trial);
        m_model_preview->set_saved_semantic_result(
            m_beauty_session_source->semantic_faces, m_beauty_session_source->semantic_subfaces);
    } else if (m_finishing_workbench || m_model_preview->semantic_result_active())
        m_model_preview->restore_color_trial_without_recognition(color_state);
    else m_model_preview->restore_color_trial(color_state);
    m_model_preview->set_color_controls_visible(!m_finishing_workbench || m_finishing_tool->GetSelection() == 2);
    m_model_preview->set_selection_preview_suppressed(
        m_finishing_workbench && !m_finishing_candidate.empty() && path == m_finishing_candidate);
    m_model_preview_ready = true;
    if (m_workbench_load_error) {
        m_status->SetLabel(wxEmptyString);
        m_result_summary->SetLabel(wxEmptyString);
    }
    m_workbench_load_error = false;
    m_model_stats->SetLabel(wxString::Format(_L("%llu 个三角面 · %llu 个原始色值\n%.1f × %.1f × %.1f mm"),
        static_cast<unsigned long long>(triangles), static_cast<unsigned long long>(colors),
        dimensions.x(), dimensions.y(), dimensions.z()));
    return true;
}

void ModelGenerationPanel::select_local_finishing_version(const boost::filesystem::path& path, const std::string& id)
{
    ++m_sequence;
    m_poll_timer.Stop();
    m_job_id.clear(); m_job_palette.clear(); m_job_palette_roles.clear();
    m_job_use_printable_colors = false;
    m_artifact_path = m_displayed_model_path = path;
    m_displayed_model_job_id = id;
    m_displayed_model_palette.clear(); m_displayed_model_palette_roles.clear();
    m_color_intent_path.clear(); m_color_intent_schema.clear(); m_color_intent_sha256.clear();
    m_artifact_format = AI::model_artifact_format(path); m_artifact_color_encoding = "vertex_colors";
    m_ready = true; m_library_model_loaded = true; m_artifact_download_started = false;
    m_awaiting_confirmation = false; m_awaiting_palette_confirmation = false;
    m_last_imported_model_path.clear();
    clear_model_quality();
    m_visual_quality = {};
    m_model_refinement = {};
}

void ModelGenerationPanel::accept_model_finishing()
{
    if (m_busy || m_finishing_candidate.empty()) return;
    if (m_finishing_workbench && m_beauty_transactions &&
        !m_beauty_transactions->begin(BeautyWorkbenchTransactionController::OperationKind::AcceptCandidate)) {
        m_finishing_status->SetLabel(_L("当前仍有 Beauty 处理正在进行，请先完成或取消。"));
        return;
    }
    if (!show_finishing_version(m_finishing_candidate)) {
        if (m_beauty_transactions) m_beauty_transactions->finish(false, false, "candidate load failed");
        return;
    }
    const auto root = generated_models_root();
    nlohmann::json metadata {
        {"schema_version", 4}, {"history_index_required", true}, {"job_id", m_finishing_id},
        {"model_path", m_finishing_candidate.lexically_relative(root).generic_string()},
        {"source", "local_finishing"}, {"prompt", "3D 美颜与修复"},
        {"source_model", m_finishing_source.lexically_relative(root).generic_string()},
        {"source_sha256", m_finishing_result.source_sha256}, {"model_sha256", m_finishing_result.output_sha256},
        {"palette", nlohmann::json::array()}, {"palette_roles", nlohmann::json::object()},
        {"use_printable_colors", false}, {"generated_at", std::time(nullptr)},
        {"triangle_count", m_finishing_result.faces_after},
        {"dimensions", m_finishing_result.dimensions},
        {"finishing", {{"smooth_surface", m_finishing_options.smooth_surface}, {"repair_mesh", m_finishing_options.repair_mesh},
            {"recolor_selected", m_finishing_options.recolor_selected}, {"target_color", m_finishing_options.target_color},
            {"recolored_faces", m_finishing_result.recolored_faces},
            {"clean_color_spots", m_finishing_options.clean_color_spots}, {"cleanup_palette", m_finishing_options.cleanup_palette},
            {"cleaned_color_regions", m_finishing_result.cleaned_color_regions}, {"recolored_vertices", m_finishing_result.recolored_vertices},
            {"selected_faces", m_finishing_options.selected_faces},
            {"strength", m_finishing_options.strength}, {"moved_vertices", m_finishing_result.moved_vertices},
            {"protected_vertices", m_finishing_result.protected_vertices},
            {"removed_degenerate_faces", m_finishing_result.removed_degenerate_faces},
            {"removed_duplicate_faces", m_finishing_result.removed_duplicate_faces},
            {"reversed_faces", m_finishing_result.reversed_faces},
            {"boundary_edges", m_finishing_result.boundary_edges}, {"nonmanifold_edges", m_finishing_result.nonmanifold_edges},
            {"max_displacement_source_units", m_finishing_result.max_displacement}}}
    };
    if (m_finishing_options.recolor_selected) {
        metadata["recolor_target_palette"] = m_finishing_color_palette;
        metadata["preserves_unselected_face_colors"] = true;
    }
    metadata["face_color_intent"] = m_model_preview->face_color_metadata();
    metadata["color_trial"] = m_model_preview->color_trial_metadata();
    const auto provenance = m_model_preview->semantic_color_metadata();
    metadata["semantic_color_state"] = provenance.empty()
        ? m_finishing_candidate_semantic_provenance : provenance;
    const auto semantic_result = m_model_preview->semantic_result_metadata();
    if (!semantic_result.empty()) metadata["semantic_result"] = semantic_result;
    const auto region_reference = m_model_preview->semantic_region_evidence_metadata();
    const bool region_cache_unsaved = m_model_preview->semantic_regions_ready() && region_reference.empty();
    if (!region_reference.empty()) metadata["semantic_region_evidence"] = region_reference;
    const auto secondary_region_reference = m_model_preview->secondary_region_evidence_metadata();
    if (!secondary_region_reference.empty()) metadata["secondary_region_evidence"] = secondary_region_reference;
    if (m_finishing_options.beauty_appearance || m_finishing_options.beauty_deform || m_finishing_options.beauty_puzzle) {
        metadata["beauty_workbench"] = BeautyWorkbenchControls::accepted_document(
            m_finishing_options, m_model_preview->geometry_id());
        if (m_beauty_controls && !m_beauty_controls->partition_metadata().empty())
            metadata["beauty_workbench"]["puzzle"] = m_beauty_controls->partition_metadata();
    } else {
        AI::BeautyDocument beauty;
        beauty.geometry_id = m_model_preview->geometry_id();
        beauty.source_sha256 = m_finishing_result.source_sha256;
        beauty.face_count = m_finishing_result.faces_after;
        beauty.face_patch.assign(beauty.face_count, 0);
        metadata["beauty_workbench"] = beauty.encode();
    }
    metadata["beauty_workbench"]["model_sha256"] = m_finishing_result.output_sha256;
    if (!m_finishing_options.repair_mesh && m_finishing_selection_state.selected.size() == m_finishing_result.faces_after)
        metadata["local_selection"] = AI::SurfaceSelectionPersistence::encode(m_finishing_selection_state,
            m_finishing_result.faces_after, m_model_preview->geometry_id());
    if (!m_reference_image_path.empty() && path_is_inside(root, m_reference_image_path))
        metadata["reference_image_path"] = m_reference_image_path.lexically_relative(root).generic_string();
    if (!m_raw_preview_path.empty() && path_is_inside(root, m_raw_preview_path))
        metadata["ai_image_path"] = m_raw_preview_path.lexically_relative(root).generic_string();
    if (!write_json(library_metadata_path(m_finishing_id), metadata, -1)) {
        if (m_beauty_transactions) m_beauty_transactions->finish(false, false, "metadata write failed");
        m_finishing_status->SetLabel(_L("版本记录保存失败，尚未接受；请释放磁盘空间后重试。")); return;
    }
    nlohmann::json history_index = {
        {"schema", "orca.local-finishing-history/v1"},
        {"source", "local_finishing"},
        {"job_id", m_finishing_id},
        {"model_path", metadata.at("model_path")},
        {"model_sha256", m_finishing_result.output_sha256},
        {"generated_at", metadata.at("generated_at")},
        {"triangle_count", m_finishing_result.faces_after},
        {"prompt", metadata.at("prompt")},
        {"use_printable_colors", false}
    };
    for (const char* key : {"reference_image_path", "ai_image_path"})
        if (metadata.contains(key)) history_index[key] = metadata.at(key);
    auto history_index_path = library_metadata_path(m_finishing_id);
    history_index_path.replace_extension(".history.json");
    if (!write_json(history_index_path, history_index)) {
        if (m_beauty_transactions) m_beauty_transactions->finish(false, false, "history index write failed");
        m_finishing_status->SetLabel(_L("历史索引保存失败，尚未接受；请释放磁盘空间后重试。")); return;
    }
    m_finishing_undo_path = m_finishing_source;
    m_finishing_restore_context = m_finishing_source_context;
    m_finishing_accepted_path = m_finishing_candidate;
    if (m_finishing_workbench) m_beauty_accepted_files.push_back(m_finishing_candidate);
    select_local_finishing_version(m_finishing_candidate, m_finishing_id);
    m_finishing_candidate.clear();
    if (m_finishing_workbench) m_model_preview->set_selection_preview_suppressed(true);
    if (m_finishing_workbench) {
        m_finishing_source.clear();
        m_beauty_session_source.reset();
    }
    if (m_beauty_controls) m_beauty_controls->mark_saved();
    update_finishing_selection();
    m_finishing_status->SetLabel(_L("新版本已保存到模型库。可返回上个版本，或导入准备页重新检查打印条件。"));
    if (region_cache_unsaved) m_finishing_status->SetLabel(m_finishing_status->GetLabel() +
        _L("区域标签缓存保存失败；本次仍可编辑，重新打开后需要重新识别。"));
    m_status->SetLabel(_L("美颜新版本已保存，可继续导入。"));
    m_model_preview_message->SetLabel(_L("当前显示：已接受的三维处理版本。"));
    if (m_beauty_transactions) {
        m_beauty_transactions->finish(true, false);
        m_beauty_transactions->record({
            BeautyWorkbenchTransactionController::OperationKind::AcceptCandidate,
            "accept Beauty candidate",
            [this] { undo_model_finishing(); },
            [this] { redo_model_finishing(); }
        });
    }
    if (m_finishing_workbench && m_beauty_transactions)
        m_beauty_session_undo_base = m_beauty_transactions->undo_count();
    m_workbench_history_filter = 0;
    m_workbench_history_page = 0;
    if (m_workbench_history_search) m_workbench_history_search->ChangeValue(wxEmptyString);
    load_library_entries(); refresh_controls();
    if (!m_finishing_options.repair_mesh && m_finishing_restore_selection) m_finishing_restore_selection();
}

void ModelGenerationPanel::discard_model_finishing()
{
    if (m_busy || m_finishing_candidate.empty()) return;
    const auto discarded = m_finishing_candidate;
    const bool beauty = bool(m_beauty_session_source);
    if (m_beauty_session_source) {
        if (!restore_beauty_candidate(*m_beauty_session_source)) return;
    } else if (!show_finishing_version(m_finishing_source)) return;
    if (m_beauty_transactions) m_beauty_transactions->truncate_to(m_beauty_session_undo_base);
    clear_unaccepted_beauty_candidates(m_beauty_session_file_base);
    if (!beauty) {
        boost::system::error_code ignored;
        boost::filesystem::remove(discarded, ignored);
    }
    m_beauty_session_source.reset();
    m_finishing_candidate.clear();
    m_model_preview->set_selection_preview_suppressed(false);
    m_finishing_status->SetLabel(_L("已放弃预览，恢复处理前模型。"));
    m_status->SetLabel(m_finishing_status->GetLabel());
    m_model_preview_message->SetLabel(_L("当前显示：处理前模型。"));
    refresh_controls();
    update_finishing_selection();
    if (m_finishing_restore_selection)
        m_finishing_restore_selection();
}

void ModelGenerationPanel::undo_model_finishing()
{
    if (m_busy || m_finishing_undo_path.empty()) return;
    const auto redo_colors = m_model_preview->color_trial_state();
    if (!show_finishing_version(m_finishing_undo_path)) return;
    m_finishing_redo_preview = [this, redo_colors] { m_model_preview->restore_color_trial(redo_colors); };
    m_finishing_redo_path = m_finishing_accepted_path;
    m_finishing_redo_id = m_displayed_model_job_id;
    m_finishing_redo_source = m_finishing_undo_path;
    ++m_sequence;
    if (m_finishing_restore_context) m_finishing_restore_context();
    if (m_finishing_restore_selection) m_finishing_restore_selection();
    refresh_model_quality_card();
    m_finishing_undo_path.clear(); m_finishing_source.clear();
    m_finishing_status->SetLabel(_L("已返回上个版本。处理后的版本仍在模型库中，可随时重新选用。"));
    m_status->SetLabel(_L("已返回上个版本，可继续导入。"));
    m_model_preview_message->SetLabel(_L("当前显示：上个版本。"));
    refresh_controls();
}

void ModelGenerationPanel::redo_model_finishing()
{
    if (m_busy || m_finishing_redo_path.empty() || m_displayed_model_path != m_finishing_redo_source) return;
    if (!show_finishing_version(m_finishing_redo_path)) return;
    m_finishing_undo_path = m_finishing_redo_source;
    m_finishing_accepted_path = m_finishing_redo_path;
    select_local_finishing_version(m_finishing_redo_path, m_finishing_redo_id);
    if (m_finishing_workbench) m_model_preview->set_selection_preview_suppressed(true);
    if (m_finishing_redo_preview) m_finishing_redo_preview();
    m_finishing_redo_path.clear();
    m_finishing_status->SetLabel(_L("已重做修整，恢复已保存版本。"));
    m_status->SetLabel(m_finishing_status->GetLabel());
    m_model_preview_message->SetLabel(_L("当前显示：已接受的三维处理版本。"));
    refresh_controls(); update_finishing_selection();
}

void ModelGenerationPanel::stop_model_finishing()
{
    if (m_finishing_canceled) m_finishing_canceled->store(true);
    if (m_finishing_worker.joinable()) m_finishing_worker.join();
    m_finishing_running = false;
    clear_unaccepted_beauty_candidates();
    if (!m_finishing_candidate.empty() && m_finishing_candidate != m_finishing_accepted_path) {
        boost::system::error_code ignored; boost::filesystem::remove(m_finishing_candidate, ignored);
    }
    m_finishing_candidate.clear();
}
} // namespace Slic3r::GUI
