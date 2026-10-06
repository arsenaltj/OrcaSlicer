#include "DesktopWorkspaceNavigation.hpp"
#include "../Redesign/PrinterWorkspace.hpp"
#include "../Redesign/RedesignControls.hpp"
#include "AIDesktopFeatureHost.hpp"
#include "ModelGeneration/ModelGenerationInputStyle.hpp"
#include "Orca/OrcaPrintConfirmation.hpp"
#include "Orca/SoftwareViewportExport.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include <wx/filedlg.h>
#include <wx/glcanvas.h>
#include <wx/weakref.h>
#include <wx/mstream.h>
#include "Orca/OrcaSmartSlicingAdapter.hpp"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/DeviceCore/DevStorage.h"
#include "slic3r/GUI/Monitor.hpp"
#include "slic3r/GUI/Tabbook.hpp"
#include "libslic3r/PresetBundle.hpp"
#include <wx/scrolwin.h>
#include <wx/wrapsizer.h>
#include <chrono>
#include <cmath>
#include "Orca/OrcaFilamentSelection.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/GUI/Notebook.hpp"
#include <wx/statbmp.h>
#include <wx/generic/statbmpg.h>
#include <wx/menu.h>
#include <wx/toplevel.h>
#include <boost/log/trivial.hpp>
namespace Slic3r::GUI {
namespace {
void bind_desktop_window_geometry(wxTopLevelWindow& window)
{
    window.Bind(wxEVT_SIZE, [&window, last_client = wxSize(-1, -1), last_dpi = wxSize(-1, -1)](wxSizeEvent& event) mutable {
        const wxSize client = window.GetClientSize();
        const wxSize dpi = window.GetDPI();
        if (client != last_client || dpi != last_dpi) {
            const wxSize frame = window.GetSize();
            const wxSize dip = window.ToDIP(client);
            const wxSize roundtrip = window.FromDIP(dip);
            BOOST_LOG_TRIVIAL(info) << "ux_window_geometry frame_px=" << frame.x << "x" << frame.y
                << " client_px=" << client.x << "x" << client.y
                << " client_dip=" << dip.x << "x" << dip.y
                << " dpi=" << dpi.x << "x" << dpi.y
                << " scale=" << window.GetDPIScaleFactor()
                << " roundtrip_px=" << roundtrip.x << "x" << roundtrip.y
                << " maximized=" << window.IsMaximized();
            last_client = client; last_dpi = dpi;
        }
        event.Skip();
    });
}
const wxString print_center_page = "ai_print_center";
enum class DesktopDestination { Library, Image, Model, Print };
void show_software_viewport_capture(wxWindow* parent)
{
    using namespace ModelGenerationInputStyle;
    auto* plater = wxGetApp().plater();
    auto* canvas = plater ? plater->get_current_canvas3D() : nullptr;
    const bool slice = canvas && canvas == plater->get_preview_canvas3D();
    ThumbnailData frame;
    bool captured = false;
    try { captured = canvas && canvas->capture_viewport(frame); }
    catch (const std::exception&) { frame.reset(); }
    if (!captured) {
        wxMessageBox(_L("当前视口尚未就绪或正在拖动。请返回工程或切片预览，结束操作后再截图。"),
                     _L("尚未截图"), wxOK | wxICON_INFORMATION, parent);
        return;
    }
    wxImage image(frame.width, frame.height);
    for (unsigned int y = 0; y < frame.height; ++y)
        for (unsigned int x = 0; x < frame.width; ++x) {
            const auto* pixel = frame.pixels.data() + 4 * ((frame.height - 1 - y) * frame.width + x);
            image.SetRGB(x, y, pixel[0], pixel[1], pixel[2]);
        }
    wxMemoryOutputStream encoded;
    if (!image.SaveFile(encoded, wxBITMAP_TYPE_PNG) || !encoded.IsOk()) {
        wxMessageBox(_L("截图编码失败，未写入文件。请返回视口后重试。"), _L("尚未导出"), wxOK, parent);
        return;
    }
    std::vector<unsigned char> png(encoded.GetSize());
    encoded.CopyTo(png.data(), png.size());
    wxDialog dialog(wxGetTopLevelParent(parent), wxID_ANY, _L("软件视口截图"), wxDefaultPosition,
                    wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    auto* root = new wxBoxSizer(wxVERTICAL);
    auto* source = new Label(&dialog, slice ? _L("来源：软件切片预览 · 不是设备相机画面") :
                                              _L("来源：软件工程视口 · 不是设备相机画面"), LB_AUTO_WRAP);
    source->SetName("input_accent");
    root->Add(source, 0, wxEXPAND | wxALL, dialog.FromDIP(12));
    auto* picture = new wxGenericStaticBitmap(&dialog, wxID_ANY, wxBitmap(image));
    picture->SetScaleMode(wxStaticBitmapBase::Scale_AspectFit);
    picture->Bind(wxEVT_SIZE, [picture](wxSizeEvent& event) {
        picture->Refresh();
        event.Skip();
    });
    picture->SetMinSize(wxSize(1, 1));
    root->Add(picture, 1, wxEXPAND | wxLEFT | wxRIGHT, dialog.FromDIP(12));
    auto* status = new Label(&dialog, wxString::Format(_L("已截取 %u × %u 像素；尚未导出。导出为 PNG，不会覆盖已有文件。"),
                                                      frame.width, frame.height), LB_AUTO_WRAP);
    status->SetName("input_secondary");
    root->Add(status, 0, wxEXPAND | wxALL, dialog.FromDIP(12));
    auto* actions = new wxWrapSizer(wxHORIZONTAL);
    auto* save = new Button(&dialog, _L("导出 PNG…"));
    save->SetName("input_primary");
    auto* back = new Button(&dialog, _L("返回视口"));
    back->SetName("input_field");
    for (auto* button : {save, back}) {
        button->SetPaddingSize(dialog.FromDIP(wxSize(14, 10)));
        actions->Add(button, 0, wxALL, dialog.FromDIP(6));
    }
    root->Add(actions, 0, wxEXPAND | wxALL, dialog.FromDIP(6));
    save->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
        wxFileDialog file(&dialog, _L("导出软件视口截图"), wxEmptyString,
                          slice ? "slice-viewport.png" : "engineering-viewport.png",
                          "PNG (*.png)|*.png", wxFD_SAVE);
        if (file.ShowModal() != wxID_OK) return;
        try {
            auto path = file.GetPath();
            if (!path.Lower().EndsWith(".png")) path += ".png";
            export_software_viewport_png(boost::filesystem::path(path.ToStdWstring()), png);
            status->SetLabel(_L("已导出当前截图。调整视角后可返回视口重新截图。"));
        } catch (const boost::filesystem::filesystem_error&) {
            status->SetLabel(_L("保存位置当前不可写、文件已存在或不支持安全导出。截图保留，请更换名称或位置后重试。"));
        } catch (const std::exception& error) {
            status->SetLabel(_L("尚未导出。当前截图保留，可更改名称或位置后重试。") + "\n" + wxString::FromUTF8(error.what()));
        }
        dialog.Layout();
    });
    back->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) { dialog.EndModal(wxID_CANCEL); });
    dialog.Bind(wxEVT_CHAR_HOOK, [&](wxKeyEvent& event) {
        if (event.GetKeyCode() == WXK_ESCAPE) dialog.EndModal(wxID_CANCEL);
        else event.Skip();
    });
    dialog.SetSizer(root);
    apply(&dialog, true);
    apply_control(save, Role::PrimaryAction, true);
    apply_control(back, Role::QuietAction, true);
    // Aspect-fit the complete captured frame; exported bytes retain native pixels.
    dialog.SetMinSize(dialog.FromDIP(wxSize(420, 320)));
    dialog.SetSize(dialog.FromDIP(wxSize(640, 460)));
    dialog.CentreOnParent();
    dialog.ShowModal();
    // Restore after the stack dialog is destroyed and MSW restores its parent focus.
    wxWeakRef<wxWindow> return_canvas(canvas->get_wxglcanvas());
    parent->CallAfter([return_canvas] {
        if (return_canvas && return_canvas->IsShownOnScreen() && return_canvas->IsEnabled())
            return_canvas->SetFocus();
    });
}
}
DesktopWorkspaceNavigation::DesktopWorkspaceNavigation(wxWindow* parent, Notebook* book, AIDesktopFeatureHost& host)
    : wxPanel(parent), m_tabpanel(book), m_ai_feature_host(&host)
{
    if (auto* frame = wxDynamicCast(wxGetTopLevelParent(parent), wxTopLevelWindow))
        bind_desktop_window_geometry(*frame);
    using namespace ModelGenerationInputStyle;
    using namespace ModelGenerationPresentation;
    SetBackgroundColour(background);
    parent->SetBackgroundColour(background);
    m_tabpanel->GetBtnsListCtrl()->SetExternalNavigation(true);
    auto* navigation = new RoundedPanel(this);
    navigation->SetMinSize(FromDIP(wxSize(92, 1)));
    m_body = navigation;
    auto* root = new wxBoxSizer(wxVERTICAL);
    root->Add(navigation, 1, wxEXPAND);
    SetSizer(root);
    build_print_center();
    auto* column = new wxBoxSizer(wxVERTICAL);
    column->Add(new wxStaticBitmap(navigation, wxID_ANY,
        redesign_resource_bitmap(navigation, "redesign_logo.png", wxSize(48, 48))), 0,
        wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(16));
    column->AddSpacer(FromDIP(78));
    struct Entry { const char* icon; wxString label; DesktopDestination destination; };
    const std::array<Entry, 4> entries {{{"redesign_nav_assets.png", _L("资产"), DesktopDestination::Library},
        {"redesign_nav_image.png", _L("图像"), DesktopDestination::Image},
        {"redesign_nav_model.png", _L("3D模型"), DesktopDestination::Model},
        {"redesign_nav_print.png", _L("打印"), DesktopDestination::Print}}};
    for (const auto& entry : entries) {
        auto* button = new RedesignNavigationButton(navigation, entry.label, entry.icon);
        button->SetMinSize(FromDIP(wxSize(92, 70)));
        button->SetName("input_navigation");
        button->EnableTooltipEvenDisabled();
        column->Add(button, 0, wxEXPAND | wxBOTTOM, FromDIP(24));
        m_workspace_buttons.push_back(button);
        button->Bind(wxEVT_BUTTON, [this, button, destination = entry.destination](wxCommandEvent&) {
            if (destination == DesktopDestination::Print) {
                navigate_workspace_page(print_center_page);
            } else {
                const auto action = destination == DesktopDestination::Library ? WorkspaceAction::ShowLibrary :
                    destination == DesktopDestination::Image ? WorkspaceAction::ShowImage : WorkspaceAction::ShowModel;
                navigate_workspace_page(TAB_ID_GENERATE_3D);
                m_ai_feature_host->navigate_generation(action);
            }
            refresh();
            // Page selection can focus a hidden native notebook control. Keep
            // the activated public entry as the keyboard traversal anchor.
            button->SetFocus();
        });
    }
    column->AddStretchSpacer();
    for (const auto& entry : std::array<std::pair<wxString, wxString>, 3>{{
             {_L("工程准备"), TAB_ID_PREPARE}, {_L("切片预览"), TAB_ID_PREVIEW}, {_L("更多"), ""}}}) {
        const char* icon = entry.second == TAB_ID_PREPARE ? "redesign_nav_model.png" :
            entry.second == TAB_ID_PREVIEW ? "redesign_nav_print.png" : "redesign_nav_settings.png";
        auto* button = new RedesignNavigationButton(navigation, entry.first, icon, false);
        button->SetMinSize(FromDIP(wxSize(92, 56)));
        button->SetName("input_navigation");
        column->Add(button, 0, wxEXPAND | wxBOTTOM, FromDIP(20));
        m_workspace_buttons.push_back(button);
        button->Bind(wxEVT_BUTTON, [this, button, id = entry.second](wxCommandEvent&) {
            if (id.empty()) show_workspace_pages();
            else navigate_workspace_page(id);
            button->SetFocus();
        });
    }
    navigation->SetSizer(column);
    // The notebook and the public navigation are siblings in a frame, which
    // does not provide panel traversal between them. Join only their Tab
    // boundaries; controls, popups and Ctrl+Tab keep their native handling.
    wxWeakRef<DesktopWorkspaceNavigation> weak_navigation(this);
    m_tabpanel->Bind(wxEVT_NAVIGATION_KEY, [weak_navigation](wxNavigationKeyEvent& event) {
        if (weak_navigation && event.IsFromTab() && !event.IsWindowChange() &&
            event.GetEventObject() != weak_navigation->m_tabpanel &&
            event.GetEventObject() != weak_navigation->m_tabpanel->GetParent() &&
            !weak_navigation->m_workspace_buttons.empty()) {
            auto* target = event.GetDirection()
                ? weak_navigation->m_workspace_buttons.front()
                : weak_navigation->m_workspace_buttons.back();
            if (target->IsShownOnScreen() && target->IsEnabled()) {
                target->SetFocus();
                return;
            }
        }
        event.Skip();
    });
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& event) {
        if (event.CmdDown() && !event.AltDown() && !event.ShiftDown() &&
            (event.GetKeyCode() == WXK_PAGEUP || event.GetKeyCode() == WXK_PAGEDOWN)) {
            // Public entries are siblings of the notebook. Keep its native page
            // change path, and leave dialogs and page controls on their own paths.
            for (auto* button : m_workspace_buttons) {
                if (button == wxWindow::FindFocus()) {
                    m_tabpanel->AdvanceSelection(event.GetKeyCode() == WXK_PAGEDOWN);
                    return;
                }
            }
        }
        event.Skip();
    });
    Bind(wxEVT_NAVIGATION_KEY, [this](wxNavigationKeyEvent& event) {
        if (event.IsFromTab() && !event.IsWindowChange() &&
            event.GetEventObject() != this && event.GetEventObject() != GetParent()) {
            if (auto* page = m_tabpanel->GetCurrentPage();
                page && page->IsShownOnScreen() && page->IsEnabled()) {
                wxNavigationKeyEvent into_page(event);
                into_page.SetEventObject(m_tabpanel);
                into_page.SetCurrentFocus(m_tabpanel);
                if (!page->HandleWindowEvent(into_page)) page->SetFocus();
                return;
            }
        }
        event.Skip();
    });
    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        update_compact_layout();
        event.Skip();
    });
    m_ai_feature_host->set_workspace_changed_handler([this] { refresh(); });
    refresh_ai_appearance(navigation, true);
}

void DesktopWorkspaceNavigation::build_print_center()
{
    m_print_center = new ModelGenerationInputStyle::Surface(m_tabpanel);
    m_print_center->SetMinSize(wxSize(1, 1));
    m_printer_workspace = new PrinterWorkspace(m_print_center, wxGetApp().plater());
    auto* root = new wxBoxSizer(wxVERTICAL);
    root->Add(m_printer_workspace, 1, wxEXPAND | wxALL, FromDIP(12));
    m_print_center->SetSizer(root);
    m_print_center->Bind(wxEVT_SHOW, [this](wxShowEvent& event) {
        m_printer_workspace->set_active(event.IsShown() && m_print_center->IsShownOnScreen());
        event.Skip();
    });
    m_tabpanel->AddPage(print_center_page, m_print_center, _L("打印"));
}

bool DesktopWorkspaceNavigation::route_printer_page_request(const wxString& id)
{
    if (!m_printer_workspace || !m_tabpanel) return false;
    if (id == TAB_ID_MONITOR) {
        navigate_workspace_page(print_center_page);
        if (m_tabpanel->GetSelectedPageName() != print_center_page) return false;
        m_printer_workspace->show_monitor();
        return true;
    }
    // Import and slice callbacks retain the workspace that initiated them.
    // Explicit native-page navigation still goes through the notebook event.
    if (m_tabpanel->GetSelectedPageName() == print_center_page &&
        (id == TAB_ID_PREPARE || id == TAB_ID_PREVIEW)) {
        m_printer_workspace->show_prepare();
        return true;
    }
    return false;
}

void DesktopWorkspaceNavigation::update_compact_layout()
{
    if (!m_body || m_workspace_buttons.size() != 7) return;
    const bool compact = GetClientSize().y < FromDIP(820);
    if (compact == m_compact) return;
    m_compact = compact;
    auto* column = m_body->GetSizer();
    column->GetItem(size_t(0))->SetBorder(FromDIP(compact ? 8 : 16));
    column->GetItem(size_t(1))->SetMinSize(wxSize(0, FromDIP(compact ? 16 : 78)));
    for (size_t index = 0; index < m_workspace_buttons.size(); ++index) {
        auto* button = m_workspace_buttons[index];
        const bool primary = index < 4;
        button->SetMinSize(FromDIP(wxSize(92, compact ? (primary ? 58 : 44) : (primary ? 70 : 56))));
        column->GetItem(button)->SetBorder(FromDIP(compact ? 4 : primary ? 24 : 20));
    }
    // Keep all destinations reachable; the original logo and icons retain their sizes.
    m_body->Layout();
}

void DesktopWorkspaceNavigation::navigate_workspace_page(const wxString& id)
{
    const int index = m_tabpanel->FindPageByName(id);
    if (index == wxNOT_FOUND) return;
    // Use the native tab event, including its preview-only and device-sync guards.
    wxCommandEvent event(wxCUSTOMEVT_NOTEBOOK_SEL_CHANGED, index);
    m_tabpanel->GetEventHandler()->ProcessEvent(event);
    if (id == TAB_ID_PREPARE && m_tabpanel->GetSelectedPageName() == id)
        if (auto* plater = wxGetApp().plater()) plater->show_smart_slicing(true);
    refresh();
}

void DesktopWorkspaceNavigation::show_workspace_pages()
{
    wxMenu menu;
    for (size_t index = 0; index < m_tabpanel->GetPageCount(); ++index) {
        const wxString id = m_tabpanel->GetPageName(index);
        wxString label = m_tabpanel->GetPageText(index);
        if (id == TAB_ID_GENERATE_3D) label = _L("图像与 3D 工作台");
        else if (id == TAB_ID_PROJECT) label = _L("项目信息");
        if (label.empty()) label = id == TAB_ID_HOME ? _L("首页") : id;
        if (label.empty()) continue;
        auto* item = menu.AppendCheckItem(wxWindow::NewControlId(), label);
        item->Check(id == m_tabpanel->GetSelectedPageName());
        menu.Bind(wxEVT_MENU, [this, id](wxCommandEvent&) { navigate_workspace_page(id); }, item->GetId());
    }
    menu.AppendSeparator();
    auto* filaments = menu.Append(wxWindow::NewControlId(), _L("耗材与颜色…"));
    menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        if (auto* plater = wxGetApp().plater()) show_orca_filament_selection(this, *plater);
    }, filaments->GetId());
    auto* screenshot = menu.Append(wxWindow::NewControlId(), _L("截取当前软件视口…"));
    auto* current = wxGetApp().plater() ? wxGetApp().plater()->get_current_canvas3D() : nullptr;
    screenshot->Enable(current && current->get_wxglcanvas()->IsShownOnScreen());
    menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) { show_software_viewport_capture(this); }, screenshot->GetId());
    auto* print = menu.Append(wxWindow::NewControlId(), _L("打印确认…"));
    menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        if (auto* plater = wxGetApp().plater()) show_orca_print_confirmation(this, *plater);
    }, print->GetId());
    m_body->PopupMenu(&menu);
}

void DesktopWorkspaceNavigation::refresh()
{
    if (!m_body || m_workspace_buttons.size() != 7 || !m_ai_feature_host) return;
    using namespace ModelGenerationInputStyle;
    using namespace ModelGenerationPresentation;
    const wxString page = m_tabpanel->GetSelectedPageName();
    const bool generation = page == TAB_ID_GENERATE_3D;
    const auto view = m_ai_feature_host->generation_view();
    const std::array<bool, 7> selected {{
        generation && view == WorkspaceView::Library,
        generation && view == WorkspaceView::Image,
        generation && view == WorkspaceView::Model,
        page == print_center_page || page == TAB_ID_MONITOR || page == TAB_ID_MONITOR_WEB,
        page == TAB_ID_PREPARE, page == TAB_ID_PREVIEW,
        !generation && page != TAB_ID_PREPARE && page != TAB_ID_PREVIEW &&
            page != print_center_page && page != TAB_ID_MONITOR && page != TAB_ID_MONITOR_WEB}};
    m_workspace_buttons[2]->Enable();
    m_workspace_buttons[2]->SetToolTip(m_ai_feature_host->has_generation_model() ?
        _L("查看当前模型") : _L("打开 3D 工作区，从图像创作或已有资产开始。"));
    m_workspace_buttons[3]->SetToolTip(_L("查看打印状态和确认入口；不会自动上传或启动打印。"));
    if (m_printer_workspace) m_printer_workspace->set_active(page == print_center_page && m_print_center->IsShownOnScreen());
    for (size_t index = 0; index < selected.size(); ++index) {
        auto* button = m_workspace_buttons[index];
        button->SetName(selected[index] ? "input_navigation_selected" : "input_navigation");
        static_cast<RedesignNavigationButton*>(button)->set_selected(selected[index]);
    }
    // Keep native slice/send controls, without a second public tab bar.
    m_tabpanel->ShowPageBar(page == TAB_ID_PREPARE || page == TAB_ID_PREVIEW);
}

void DesktopWorkspaceNavigation::apply_ai_theme(bool fonts)
{
    SetBackgroundColour(ModelGenerationInputStyle::background);
    refresh_ai_appearance(m_body, fonts);
    if (m_print_center) refresh_ai_appearance(m_print_center, fonts);
}
}
