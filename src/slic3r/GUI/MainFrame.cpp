#include "MainFrame.hpp"

#include <wx/event.h>
#include "slic3r/GUI/Event.hpp"
#include <wx/gdicmn.h>
#include <wx/dcclient.h>
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include <wx/filehistory.h>
#include "slic3r/GUI/BBLTopbar.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include <boost/lexical_cast.hpp>
#include <optional>
#include "slic3r/GUI/GLToolbar.hpp"
#include "libslic3r/Preset.hpp"
#include <functional>
#include <wx/busycursor.h>
#include <cstddef>
#include "slic3r/GUI/Project.hpp"
#include <wx/bookctrl.h>
#include "slic3r/GUI/LazyPage.hpp"
#include "slic3r/GUI/Monitor.hpp"
#include "slic3r/GUI/PrinterWebView.hpp"
#include "slic3r/GUI/MultiMachinePage.hpp"
#include "slic3r/GUI/CalibrationPanel.hpp"
#include "slic3r/GUI/ReleaseNote.hpp"
#include "slic3r/GUI/GUI.hpp"
#include <vector>
#include "libslic3r/PublishSettings.hpp"
#include "libslic3r/Config.hpp"
#include "slic3r/GUI/Widgets/StateColor.hpp"
#include <wx/colour.h>
#include "slic3r/GUI/FilamentGroupPopup.hpp"
#include "slic3r/GUI/Widgets/SideMenuPopup.hpp"
#include <utility>
#include "libslic3r/libslic3r.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <wx/dirdlg.h>
#include <exception>
#include <wx/filedlg.h>
#include "slic3r/GUI/Lazy.hpp"
#include <wx/filefn.h>
#include <string>
#include <sstream>
#include <wx/base64.h>
#include <ctime>
#include <boost/filesystem/operations.hpp>
#include "slic3r/GUI/calib_dlg.hpp"
#include <wx/panel.h>
#include <wx/settings.h>
#include <wx/textentry.h>
#include <wx/notebook.h>
#include <wx/listbook.h>
#include <wx/simplebook.h>
#include <wx/icon.h>
#include <wx/sizer.h>
#include <wx/menu.h>
#include <wx/progdlg.h>
#include <wx/tooltip.h>
//#include <wx/glcanvas.h>
#include <wx/filename.h>
#include <wx/debug.h>
#include <wx/toplevel.h>
#include <wx/utils.h>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>
#include <wx/webview.h>

#include "libslic3r/Print.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/PresetBundle.hpp"

#include "Tab.hpp"
#include "ProgressStatusBar.hpp"
#include "3DScene.hpp"
#include "ParamsDialog.hpp"
#include "PrintHostDialogs.hpp"
#include "libslic3r_version.h"
#include "wxExtensions.hpp"
#include "GUI_ObjectList.hpp"
#include "Mouse3DController.hpp"
//#include "RemovableDriveManager.hpp"
#include "InstanceCheck.hpp"
#include "I18N.hpp"
#include "GLCanvas3D.hpp"
#include "Plater.hpp"
#include "AI/AIDesktopFeatureHost.hpp"
#include "AI/AIWindowAppearance.hpp"
#include "Redesign/RedesignShell.hpp"
#include "WebViewDialog.hpp"
#include "../Utils/Process.hpp"
// BBS
#include "PartPlate.hpp"
#include "Preferences.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/ProgressDialog.hpp"
#include "Widgets/StaticBox.hpp"
#include "BindDialog.hpp"
#include "../Utils/MacDarkMode.hpp"
#include "../Utils/NetworkAgentFactory.hpp"
#include "../Utils/PrintHost.hpp"

#include <fstream>
#include <string_view>
#include <chrono>
#include <cstdlib>
#include <cstring>

#include "GUI_App.hpp"
#include "Shortcuts.hpp"
#include "UnsavedChangesDialog.hpp"
#include "PublishSettingsDialog.hpp"
#include "MsgDialog.hpp"
#include "Notebook.hpp"
#include "GUI_Factories.hpp"
#include "NotificationManager.hpp"
#include "MarkdownTip.hpp"
#include "NetworkTestDialog.hpp"
#include "SceneBenchmark.hpp"
#include "ConfigWizard.hpp"
#include "Widgets/WebView.hpp"
#include "DailyTips.hpp"
#include "FilamentMapDialog.hpp"

#include "DeviceCore/DevManager.h"

#ifdef _WIN32
#include <dbt.h>
#include <shlobj.h>
#include <shellapi.h>
#endif // _WIN32

#ifdef __WXGTK__
#include <gtk/gtk.h>
#include <wx/glcanvas.h>
#endif // __WXGTK__
#include <slic3r/GUI/CreatePresetsDialog.hpp>
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Model.hpp"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosManager.hpp"
#include "slic3r/GUI/IdleScheduler.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"
#include "slic3r/GUI/KeyChord.hpp"
#include "slic3r/GUI/ParamsPanel.hpp"
#include "slic3r/GUI/Tabbook.hpp"
#include "slic3r/GUI/Widgets/SideButton.hpp"
#include "slic3r/plugin/host/PluginPages.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include <wx/defs.h>
#include <cassert>
#include <wx/display.h>
#include <wx/window.h>


using json = nlohmann::json;

namespace Slic3r {
namespace GUI {

wxDEFINE_EVENT(EVT_SELECT_TAB, wxCommandEvent);
wxDEFINE_EVENT(EVT_HTTP_ERROR, wxCommandEvent);
wxDEFINE_EVENT(EVT_USER_LOGIN, wxCommandEvent);
wxDEFINE_EVENT(EVT_USER_LOGIN_HANDLE, wxCommandEvent);
wxDEFINE_EVENT(EVT_CHECK_PRIVACY_VER, wxCommandEvent);
wxDEFINE_EVENT(EVT_CHECK_PRIVACY_SHOW, wxCommandEvent);
wxDEFINE_EVENT(EVT_SHOW_IP_DIALOG, wxCommandEvent);
wxDEFINE_EVENT(EVT_UPDATE_MACHINE_LIST, wxCommandEvent);
wxDEFINE_EVENT(EVT_UPDATE_PRESET_CB, SimpleEvent);



// BBS: backup
wxDEFINE_EVENT(EVT_BACKUP_POST, wxCommandEvent);
wxDEFINE_EVENT(EVT_LOAD_URL, wxCommandEvent);
wxDEFINE_EVENT(EVT_LOAD_PRINTER_URL, LoadPrinterViewEvent);

enum class ERescaleTarget
{
    Mainframe,
    SettingsDialog
};

namespace {

// Space opens the speed dial, but it is the activation key for buttons, checkboxes and other
// controls. CHAR_HOOK runs before the focused child, so only take Space when the focused window has
// no keyboard-activation meaning of its own. Canvases (GLCanvas3D) and panels are not controls and
// fall through to "open"; the Notebook itself does too, so Space still opens the dial on any page.
bool focus_keeps_space(wxWindow* focus)
{
    if (!focus)
        return false;
    if (dynamic_cast<wxTextEntryBase*>(focus))
        return true; // typing a space into a text field
    if (dynamic_cast<wxWebView*>(focus))
        return true; // web content scrolls and hosts its own text fields
    if (dynamic_cast<::Button*>(focus))
        return true; // custom button: Space clicks it (it is a wxWindow, not a wxControl)
    if (dynamic_cast<StaticBox*>(focus))
        return true; // custom composites (ComboBox, SpinInput, ...) activate with Space and are wxWindow
    if (dynamic_cast<wxControl*>(focus) && !dynamic_cast<Notebook*>(focus))
        return true; // stock button/checkbox/choice/list/etc. keep Space
    return false;
}

} // namespace

#ifdef __WXGTK__
// A thin transparent panel placed at a window edge to handle resize.
// Works regardless of underlying content (GLCanvas3D, wxWebView, etc.)
// because these panels are Raise()'d above all siblings, so their GDK
// windows receive pointer events even over WebKit2GTK or GL surfaces.
class ResizeEdgePanel : public wxPanel
{
public:
    enum Edge { Bottom, Left, Right };

    static constexpr int BORDER_PX = 5;

    ResizeEdgePanel(MainFrame* frame, Edge edge)
        : wxPanel(frame, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                  wxBORDER_NONE)
        , m_frame(frame)
        , m_edge(edge)
    {
        SetBackgroundStyle(wxBG_STYLE_TRANSPARENT);
        Bind(wxEVT_MOTION,       &ResizeEdgePanel::OnCursorUpdate, this);
        Bind(wxEVT_ENTER_WINDOW, &ResizeEdgePanel::OnCursorUpdate, this);
        Bind(wxEVT_LEFT_DOWN,    &ResizeEdgePanel::OnLeftDown,     this);
        Bind(wxEVT_LEAVE_WINDOW, &ResizeEdgePanel::OnLeave,        this);
        Bind(wxEVT_PAINT,        &ResizeEdgePanel::OnPaint,        this);
    }

private:
    void OnPaint(wxPaintEvent&)
    {
        wxPaintDC dc(this);
        // Transparent — draw nothing
    }

    GdkWindowEdge get_gdk_edge(const wxPoint& pos) const
    {
        wxSize size = GetSize();
        switch (m_edge) {
        case Bottom:
            if (pos.x < BORDER_PX)           return GDK_WINDOW_EDGE_SOUTH_WEST;
            if (pos.x > size.x - BORDER_PX)  return GDK_WINDOW_EDGE_SOUTH_EAST;
            return GDK_WINDOW_EDGE_SOUTH;
        case Left:
            if (pos.y < BORDER_PX)            return GDK_WINDOW_EDGE_NORTH_WEST;
            if (pos.y > size.y - BORDER_PX)   return GDK_WINDOW_EDGE_SOUTH_WEST;
            return GDK_WINDOW_EDGE_WEST;
        case Right:
            if (pos.y < BORDER_PX)            return GDK_WINDOW_EDGE_NORTH_EAST;
            if (pos.y > size.y - BORDER_PX)   return GDK_WINDOW_EDGE_SOUTH_EAST;
            return GDK_WINDOW_EDGE_EAST;
        }
        return GDK_WINDOW_EDGE_SOUTH;
    }

    void OnCursorUpdate(wxMouseEvent& evt)
    {
        GdkWindowEdge edge = get_gdk_edge(evt.GetPosition());
        const char* name;
        switch (edge) {
        case GDK_WINDOW_EDGE_NORTH:       name = "n-resize";  break;
        case GDK_WINDOW_EDGE_SOUTH:       name = "s-resize";  break;
        case GDK_WINDOW_EDGE_WEST:        name = "w-resize";  break;
        case GDK_WINDOW_EDGE_EAST:        name = "e-resize";  break;
        case GDK_WINDOW_EDGE_NORTH_WEST:  name = "nw-resize"; break;
        case GDK_WINDOW_EDGE_NORTH_EAST:  name = "ne-resize"; break;
        case GDK_WINDOW_EDGE_SOUTH_WEST:  name = "sw-resize"; break;
        case GDK_WINDOW_EDGE_SOUTH_EAST:  name = "se-resize"; break;
        default:                          name = "s-resize";  break;
        }
        if (name == m_last_cursor_name) return;
        m_last_cursor_name = name;

        GdkDisplay* display = gtk_widget_get_display(m_widget);
        GdkCursor*  cursor  = gdk_cursor_new_from_name(display, name);
        if (cursor) {
            gdk_window_set_cursor(gtk_widget_get_window(m_widget), cursor);
            g_object_unref(cursor);
        }
    }

    void OnLeave(wxMouseEvent&)
    {
        m_last_cursor_name = nullptr;
        gdk_window_set_cursor(gtk_widget_get_window(m_widget), nullptr);
    }

    void OnLeftDown(wxMouseEvent& evt)
    {
        if (m_frame->IsMaximized() || m_frame->IsFullScreen())
            return;

        GdkWindowEdge edge = get_gdk_edge(evt.GetPosition());
        wxPoint mouse = ClientToScreen(evt.GetPosition());

        gtk_window_begin_resize_drag(
            GTK_WINDOW(m_frame->m_widget),
            edge,
            1,  // left button
            mouse.x, mouse.y,
            gtk_get_current_event_time());
    }

    MainFrame*    m_frame;
    Edge          m_edge;
    const char*   m_last_cursor_name{nullptr};
};
#endif // __WXGTK__

#ifdef __APPLE__
class OrcaSlicerTaskBarIcon : public wxTaskBarIcon
{
public:
    OrcaSlicerTaskBarIcon(wxTaskBarIconType iconType = wxTBI_DEFAULT_TYPE) : wxTaskBarIcon(iconType) {}
    wxMenu *CreatePopupMenu() override {
        wxMenu *menu = new wxMenu;
        if (wxGetApp().app_config->get("single_instance") == "false") {
            // Only allow opening a new PrusaSlicer instance on OSX if "single_instance" is disabled,
            // as starting new instances would interfere with the locking mechanism of "single_instance" support.
            append_menu_item(menu, wxID_ANY, _L("New Window"), _L("Open a new window"),
            [](wxCommandEvent&) { start_new_slicer(); }, "", nullptr);
        }
//        append_menu_item(menu, wxID_ANY, _L("G-code Viewer") + dots, _L("Open G-code Viewer"),
//            [](wxCommandEvent&) { start_new_gcodeviewer_open_file(); }, "", nullptr);
        return menu;
    }
};
/*class GCodeViewerTaskBarIcon : public wxTaskBarIcon
{
public:
    GCodeViewerTaskBarIcon(wxTaskBarIconType iconType = wxTBI_DEFAULT_TYPE) : wxTaskBarIcon(iconType) {}
    wxMenu *CreatePopupMenu() override {
        wxMenu *menu = new wxMenu;
        append_menu_item(menu, wxID_ANY, _L("Open PrusaSlicer"), _L("Open a new PrusaSlicer"),
            [](wxCommandEvent&) { start_new_slicer(nullptr, true); }, "", nullptr);
        //append_menu_item(menu, wxID_ANY, _L("G-code Viewer") + dots, _L("Open new G-code Viewer"),
        //    [](wxCommandEvent&) { start_new_gcodeviewer_open_file(); }, "", nullptr);
        return menu;
    }
};*/
#endif // __APPLE__

// Load the icon either from the exe, or from the ico file.
static wxIcon main_frame_icon(GUI_App::EAppMode app_mode)
{
#if _WIN32
    std::wstring path(size_t(MAX_PATH), wchar_t(0));
    int len = int(::GetModuleFileName(nullptr, path.data(), MAX_PATH));
    if (len > 0 && len < MAX_PATH) {
        path.erase(path.begin() + len, path.end());
        //BBS: remove GCodeViewer as seperate APP logic
        /*if (app_mode == GUI_App::EAppMode::GCodeViewer) {
            // Only in case the slicer was started with --gcodeviewer parameter try to load the icon from prusa-gcodeviewer.exe
            // Otherwise load it from the exe.
            for (const std::wstring_view exe_name : { std::wstring_view(L"prusa-slicer.exe"), std::wstring_view(L"prusa-slicer-console.exe") })
                if (boost::iends_with(path, exe_name)) {
                    path.erase(path.end() - exe_name.size(), path.end());
                    path += L"prusa-gcodeviewer.exe";
                    break;
                }
        }*/
    }
    return wxIcon(path, wxBITMAP_TYPE_ICO);
#else // _WIN32
    return wxIcon(Slic3r::var("OrcaSlicer_128px.png"), wxBITMAP_TYPE_PNG);
#endif // _WIN32
}

// BBS
#ifndef __APPLE__
#define BORDERLESS_FRAME_STYLE (wxRESIZE_BORDER | wxMINIMIZE_BOX | wxMAXIMIZE_BOX | wxCLOSE_BOX)
#else
#define BORDERLESS_FRAME_STYLE (wxMINIMIZE_BOX | wxMAXIMIZE_BOX | wxCLOSE_BOX)
#endif

wxDEFINE_EVENT(EVT_SYNC_CLOUD_PRESET,     SimpleEvent);

#ifdef __APPLE__
static const wxString ctrl = ("Ctrl+");
// FIXME: maybe should be using GUI::shortkey_ctrl_prefix() or equivalent?
static const std::string ctrl_t = u8"\u2318+"; // "⌘" (Mac Command)
#else
static const wxString ctrl = _L("Ctrl+");
// FIXME: maybe should be using GUI::shortkey_ctrl_prefix() or equivalent?
static const wxString ctrl_t = ctrl;
#endif
static const wxString shift = _L("Shift+");

MainFrame::~MainFrame()
{
    // Member-owned hosts are destroyed before wxWidgets destroys child windows.
    if (m_redesign_shell != nullptr)
        m_redesign_shell->disconnect_model_generation_host();
}

MainFrame::MainFrame() :
DPIFrame(NULL, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, BORDERLESS_FRAME_STYLE, "mainframe")
    , m_printhost_queue_dlg(new PrintHostQueueDialog(this))
    // BBS
    , m_recent_projects(18)
    , m_settings_dialog(this)
    , m_idle([] { return wxGetApp().input_idle_ms(); })
    , m_diff_dialog("compare_presets", 100, [this] { return make_diff_dialog(); })
{
#ifdef __WXOSX__
    set_miniaturizable(GetHandle());
#endif

#ifdef __WXGTK__
    m_gdkDecor = 0;

    m_edge_bottom = new ResizeEdgePanel(this, ResizeEdgePanel::Bottom);
    m_edge_left   = new ResizeEdgePanel(this, ResizeEdgePanel::Left);
    m_edge_right  = new ResizeEdgePanel(this, ResizeEdgePanel::Right);
#endif

#ifdef __WXMSW__
    if (HWND hWnd = GetHandle(); hWnd != nullptr) {
        LONG_PTR style = GetWindowLongPtr(hWnd, GWL_STYLE);
        if ((style & WS_CAPTION) != 0) {
            SetWindowLongPtr(hWnd, GWL_STYLE, style & ~WS_CAPTION);
            SetWindowPos(hWnd, nullptr, 0, 0, 0, 0,
                         SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        }
    }
#endif

    if (!wxGetApp().app_config->has("user_mode")) {
        wxGetApp().app_config->set("user_mode", "simple");
        wxGetApp().app_config->set_bool("developer_mode", false);
        wxGetApp().app_config->save();
    }

    wxGetApp().app_config->set_bool("internal_developer_mode", false);

    wxString max_recent_count_str = wxGetApp().app_config->get("max_recent_count");
    long max_recent_count = 18;
    if (max_recent_count_str.ToLong(&max_recent_count))
        set_max_recent_count((int)max_recent_count);

    //reset log level
    auto loglevel = wxGetApp().app_config->get("log_severity_level");
    Slic3r::set_logging_level(Slic3r::level_string_to_boost(loglevel));

    // BBS
    m_recent_projects.SetMenuPathStyle(wxFH_PATH_SHOW_ALWAYS);
    MarkdownTip::Recreate(this);

    // Fonts were created by the DPIFrame constructor for the monitor, on which the window opened.
    wxGetApp().update_fonts(this);

#ifndef __APPLE__
    m_topbar         = new BBLTopbar(this);
#else
    auto panel_topbar = new wxPanel(this, wxID_ANY);
    panel_topbar->SetBackgroundColour(wxColour(38, 46, 48));
    auto sizer_tobar = new wxBoxSizer(wxVERTICAL);
    panel_topbar->SetSizer(sizer_tobar);
    panel_topbar->Layout();
#endif

    //wxAuiToolBar* toolbar = new wxAuiToolBar();
/*
#ifndef __WXOSX__ // Don't call SetFont under OSX to avoid name cutting in ObjectList
    this->SetFont(this->normal_font());
#endif
    // Font is already set in DPIFrame constructor
*/

#ifdef __APPLE__
	m_reset_title_text_colour_timer = new wxTimer();
	m_reset_title_text_colour_timer->SetOwner(this);
	Bind(wxEVT_TIMER, [this](auto& e) {
		set_title_colour_after_set_title(GetHandle());
		m_reset_title_text_colour_timer->Stop();
	});
	this->Bind(wxEVT_FULLSCREEN, [this](wxFullScreenEvent& e) {
		set_tag_when_enter_full_screen(e.IsFullScreen());
		if (!e.IsFullScreen()) {
            if (m_reset_title_text_colour_timer) {
                m_reset_title_text_colour_timer->Stop();
                m_reset_title_text_colour_timer->Start(500);
            }
            m_mac_fullscreen = false;
        } else {
            m_mac_fullscreen = true;
        }
        auto int_event = new IntEvent(EVT_NOTICE_FULL_SCREEN_CHANGED, e.IsFullScreen() ? 1 : 0);
        wxQueueEvent(wxGetApp().plater(), int_event);
		e.Skip();
	});
#endif

#ifdef __APPLE__
    // Initialize the docker task bar icon.
    switch (wxGetApp().get_app_mode()) {
    default:
    case GUI_App::EAppMode::Editor:
        m_taskbar_icon = std::make_unique<OrcaSlicerTaskBarIcon>(wxTBI_DOCK);
        m_taskbar_icon->SetIcon(wxIcon(Slic3r::var("OrcaSlicer-mac_256px.ico"), wxBITMAP_TYPE_ICO), "OrcaSlicer");
        break;
    case GUI_App::EAppMode::GCodeViewer:
        break;
    }
#endif // __APPLE__

    // Load the icon either from the exe, or from the ico file.
    SetIcon(main_frame_icon(wxGetApp().get_app_mode()));

    // Decide the initial surface before the first layout. Once the redesign
    // shell is shown it becomes a one-way navigation boundary; legacy pages
    // remain available only as hidden migration hosts.
    m_redesign_shell_requested = wxGetApp().should_start_with_redesign_shell();

    // initialize tabpanel and menubar
    init_tabpanel();
    if (wxGetApp().is_gcode_viewer())
        init_menubar_as_gcodeviewer();
    else
        init_menubar_as_editor();

    start_workflow_services();

    // BBS
#if 0
    // This is needed on Windows to fake the CTRL+# of the window menu when using the numpad
    wxAcceleratorEntry entries[6];
    entries[0].Set(wxACCEL_CTRL, WXK_NUMPAD1, wxID_HIGHEST + 1);
    entries[1].Set(wxACCEL_CTRL, WXK_NUMPAD2, wxID_HIGHEST + 2);
    entries[2].Set(wxACCEL_CTRL, WXK_NUMPAD3, wxID_HIGHEST + 3);
    entries[3].Set(wxACCEL_CTRL, WXK_NUMPAD4, wxID_HIGHEST + 4);
    entries[4].Set(wxACCEL_CTRL, WXK_NUMPAD5, wxID_HIGHEST + 5);
    entries[5].Set(wxACCEL_CTRL, WXK_NUMPAD6, wxID_HIGHEST + 6);
    wxAcceleratorTable accel(6, entries);
    SetAcceleratorTable(accel);
#endif // _WIN32

    // BBS
    //wxAcceleratorEntry entries[13];
    //int index = 0;
    //entries[index++].Set(wxACCEL_CTRL, (int)'N', wxID_HIGHEST + wxID_NEW);
    //entries[index++].Set(wxACCEL_CTRL, (int)'O', wxID_HIGHEST + wxID_OPEN);
    //entries[index++].Set(wxACCEL_CTRL, (int)'S', wxID_HIGHEST + wxID_SAVE);
    //entries[index++].Set(wxACCEL_CTRL | wxACCEL_SHIFT, (int)'S', wxID_HIGHEST + wxID_SAVEAS);
    //entries[index++].Set(wxACCEL_CTRL, (int)'X', wxID_HIGHEST + wxID_CUT);
    ////entries[index++].Set(wxACCEL_CTRL, (int)'I', wxID_HIGHEST + wxID_ADD);
    //entries[index++].Set(wxACCEL_CTRL, (int)'A', wxID_HIGHEST + wxID_SELECTALL);
    //entries[index++].Set(wxACCEL_NORMAL, (int)27 /* escape */, wxID_HIGHEST + wxID_CANCEL);
    //entries[index++].Set(wxACCEL_CTRL, (int)'Z', wxID_HIGHEST + wxID_UNDO);
    //entries[index++].Set(wxACCEL_CTRL, (int)'Y', wxID_HIGHEST + wxID_REDO);
    //entries[index++].Set(wxACCEL_CTRL, (int)'C', wxID_HIGHEST + wxID_COPY);
    //entries[index++].Set(wxACCEL_CTRL, (int)'V', wxID_HIGHEST + wxID_PASTE);
    //entries[index++].Set(wxACCEL_CTRL, (int)'P', wxID_HIGHEST + wxID_PREFERENCES);
    //entries[index++].Set(wxACCEL_CTRL, (int)'I', wxID_HIGHEST + wxID_FILE6);
    //wxAcceleratorTable accel(sizeof(entries) / sizeof(entries[0]), entries);
    //SetAcceleratorTable(accel);

    //Bind(wxEVT_MENU, [this](wxCommandEvent&) { m_plater->new_project(); }, wxID_HIGHEST + wxID_NEW);
    //Bind(wxEVT_MENU, [this](wxCommandEvent&) { m_plater->load_project(); }, wxID_HIGHEST + wxID_OPEN);
    //// BBS: close save project
    //Bind(wxEVT_MENU, [this](wxCommandEvent&) { if (route_legacy_command_to_redesign("Save Project", TAB_ID_PROJECT)) return; if (m_plater) m_plater->save_project(); }, wxID_HIGHEST + wxID_SAVE);
    //Bind(wxEVT_MENU, [this](wxCommandEvent&) { if (route_legacy_command_to_redesign("Save Project as", TAB_ID_PROJECT)) return; if (m_plater) m_plater->save_project(true); }, wxID_HIGHEST + wxID_SAVEAS);
    ////Bind(wxEVT_MENU, [this](wxCommandEvent&) { if (m_plater) m_plater->add_model(); }, wxID_HIGHEST + wxID_ADD);
    ////Bind(wxEVT_MENU, [this](wxCommandEvent&) { if (route_legacy_command_to_redesign("Delete Selected", TAB_ID_PROJECT)) return; m_plater->remove_selected(); }, wxID_HIGHEST + wxID_DELETE);
    //Bind(wxEVT_MENU, [this](wxCommandEvent&) {
    //        if (!can_add_models())
    //            return;
    //        if (m_plater) {
    //            m_plater->add_model();
    //        }
    //    }, wxID_HIGHEST + wxID_FILE6);
    //Bind(wxEVT_MENU, [this](wxCommandEvent&) { m_plater->select_all(); }, wxID_HIGHEST + wxID_SELECTALL);
    //Bind(wxEVT_MENU, [this](wxCommandEvent&) { m_plater->deselect_all(); }, wxID_HIGHEST + wxID_CANCEL);
    //Bind(wxEVT_MENU, [this](wxCommandEvent&) {
    //    if (m_plater->is_view3D_shown())
    //        m_plater->undo();
    //    }, wxID_HIGHEST + wxID_UNDO);
    //Bind(wxEVT_MENU, [this](wxCommandEvent&) {
    //    if (m_plater->is_view3D_shown())
    //        m_plater->redo();
    //    }, wxID_HIGHEST + wxID_REDO);
    //Bind(wxEVT_MENU, [this](wxCommandEvent&) { if (route_legacy_command_to_redesign("Copy", TAB_ID_PROJECT)) return; m_plater->copy_selection_to_clipboard(); }, wxID_HIGHEST + wxID_COPY);
    //Bind(wxEVT_MENU, [this](wxCommandEvent&) { if (route_legacy_command_to_redesign("Paste", TAB_ID_PROJECT)) return; m_plater->paste_from_clipboard(); }, wxID_HIGHEST + wxID_PASTE);
    //Bind(wxEVT_MENU, [this](wxCommandEvent&) { m_plater->cut_selection_to_clipboard(); }, wxID_HIGHEST + wxID_CUT);
    Bind(wxEVT_SIZE, [this](wxSizeEvent&) {
            BOOST_LOG_TRIVIAL(trace) << "mainframe: size changed, is maximized = " << this->IsMaximized();
#ifndef __APPLE__
            if (this->IsMaximized()) {
                m_topbar->SetWindowSize();
            } else {
                m_topbar->SetMaximizedSize();
            }
#endif
        Refresh();
        Layout();
#ifdef __WXGTK__
        update_edge_panels();
#endif
        if ((!m_redesign_shell_active || is_prepare_or_preview_tab()) && wxGetApp().plater() != nullptr)
            wxQueueEvent(wxGetApp().plater(), new SimpleEvent(EVT_NOTICE_CHILDE_SIZE_CHANGED));

        if (!m_redesign_shell_active)
            fit_tab_labels(); // ORCA on resize
    });

    //BBS
    Bind(EVT_SELECT_TAB, [this](wxCommandEvent& evt) {
        select_tab(evt.GetString());
    });

    Bind(EVT_SYNC_CLOUD_PRESET, &MainFrame::on_select_default_preset, this);

//    Bind(wxEVT_MENU,
//        [this](wxCommandEvent&)
//        {
//            PreferencesDialog dlg(this);
//            dlg.ShowModal();
//#if ENABLE_GCODE_LINES_ID_IN_H_SLIDER
//            if (dlg.seq_top_layer_only_changed() || dlg.seq_seq_top_gcode_indices_changed())
//#else
//            if (dlg.seq_top_layer_only_changed())
//#endif // ENABLE_GCODE_LINES_ID_IN_H_SLIDER
//                plater()->refresh_print();
//        }, wxID_HIGHEST + wxID_PREFERENCES);


    // set default tooltip timer in msec
    // SetAutoPop supposedly accepts long integers but some bug doesn't allow for larger values
    // (SetAutoPop is not available on GTK.)
    wxToolTip::SetAutoPop(32767);

    m_loaded = true;

    // initialize layout
    m_main_sizer = new wxBoxSizer(wxVERTICAL);
    wxSizer* sizer = new wxBoxSizer(wxVERTICAL);
#ifndef __APPLE__
     sizer->Add(m_topbar, 0, wxEXPAND);
#else
     sizer->Add(panel_topbar, 0, wxEXPAND);
#endif // __WINDOWS__


    sizer->Add(m_main_sizer, 1, wxEXPAND);
    SetSizerAndFit(sizer);
    // initialize layout from config
    update_layout();
    sizer->SetSizeHints(this);

    #ifdef __WXMSW__
    // SetMaximize causes the window to overlap the taskbar, due to the fact this window has wxMAXIMIZE_BOX off
    // https://forums.wxwidgets.org/viewtopic.php?t=50634
    // Fix it here
    this->Bind(wxEVT_MAXIMIZE, [this](auto &e) {
        wxDisplay display(this);
        auto      size = display.GetClientArea().GetSize();
        auto      pos  = display.GetClientArea().GetPosition();
        HWND      hWnd = GetHandle();
        RECT      borderThickness;
        SetRectEmpty(&borderThickness);
        AdjustWindowRectEx(&borderThickness, GetWindowLongPtr(hWnd, GWL_STYLE), FALSE, 0);
        const auto max_size = size + wxSize{-borderThickness.left + borderThickness.right, -borderThickness.top + borderThickness.bottom};
        const auto current_size = GetSize();
        SetSize({std::min(max_size.x, current_size.x), std::min(max_size.y, current_size.y)});
        Move(pos + wxPoint{borderThickness.left, borderThickness.top});
        e.Skip();
    });

#endif // __WXMSW__
 

    // BBS
    Fit();

    const wxSize min_size = wxGetApp().get_min_size(); //wxSize(76*wxGetApp().em_unit(), 49*wxGetApp().em_unit());

    SetMinSize(min_size/*wxSize(760, 490)*/);
    SetSize(wxSize(FromDIP(1200), FromDIP(800)));

    Layout();

    update_title();

    // declare events
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< ": mainframe received close_widow event";
        if (event.CanVeto() && m_plater->get_view3D_canvas3D()->get_gizmos_manager().is_in_editing_mode(true)) {
            // prevents to open the save dirty project dialog
            event.Veto();
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< "cancelled by gizmo in editing";
            return;
        }

        //BBS:
        //if (event.CanVeto() && !wxGetApp().check_and_save_current_preset_changes(_L("Application is closing"), _L("Closing Application while some presets are modified."))) {
        //    event.Veto();
        //    return;
        //}
        auto check = [](bool yes_or_no) {
            if (yes_or_no)
                return true;
            return wxGetApp().check_and_save_current_preset_changes(_L("Closing application"), _L("Closing Application while some presets are modified."));
        };

        // BBS: close save project
        int result;
        if (event.CanVeto() && ((result = m_plater->close_with_confirm(check)) == wxID_CANCEL)) {
            event.Veto();
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< "cancelled by close_with_confirm selection";
            return;
        }
        if (event.CanVeto() && !wxGetApp().check_print_host_queue()) {
            event.Veto();
            return;
        }

    #if 0 // BBS
        //if (m_plater != nullptr) {
        //    int saved_project = m_plater->save_project_if_dirty(_L("Closing Application. Current project is modified."));
        //    if (saved_project == wxID_CANCEL) {
        //        event.Veto();
        //        return;
        //    }
        //    // check unsaved changes only if project wasn't saved
        //    else if (plater()->is_project_dirty() && saved_project == wxID_NO && event.CanVeto() &&
        //             (plater()->is_presets_dirty() && !wxGetApp().check_and_save_current_preset_changes(_L("Application is closing"), _L("Closing Application while some presets are modified.")))) {
        //        event.Veto();
        //        return;
        //    }
        //}
    #endif

        MarkdownTip::ExitTip();
        // Prevent queued selection/UI refresh work from running as normal during reset.
        wxGetApp().set_closing(true);

        m_plater->reset();
        this->shutdown();
        // propagate event

        wxGetApp().remove_mall_system_dialog();
        event.Skip();
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< ": mainframe finished process close_widow event";
    });

    //FIXME it seems this method is not called on application start-up, at least not on Windows. Why?
    // The same applies to wxEVT_CREATE, it is not being called on startup on Windows.
    Bind(wxEVT_ACTIVATE, [this](wxActivateEvent& event) {
        if (m_plater != nullptr && event.GetActive())
            m_plater->on_activate();
        event.Skip();
    });

// OSX specific issue:
// When we move application between Retina and non-Retina displays, The legend on a canvas doesn't redraw
// So, redraw explicitly canvas, when application is moved
//FIXME maybe this is useful for __WXGTK3__ as well?
#if __APPLE__
    Bind(wxEVT_MOVE, [](wxMoveEvent& event) {
        wxGetApp().plater()->get_current_canvas3D()->set_as_dirty();
        wxGetApp().plater()->get_current_canvas3D()->request_extra_frame();
        event.Skip();
    });
#endif

    update_ui_from_settings();    // FIXME (?)

    if (m_plater != nullptr) {
        // BBS
        update_slice_print_status(eEventSliceUpdate, true, true);

        // BBS: backup project
        if (wxGetApp().app_config->get("backup_switch") == "true") {
            std::string backup_interval;
            if (!wxGetApp().app_config->get("app", "backup_interval", backup_interval))
                backup_interval = "10";
            Slic3r::set_backup_interval(boost::lexical_cast<long>(backup_interval));
        } else {
            Slic3r::set_backup_interval(0);
        }
        Slic3r::set_backup_callback([this](int action) {
            if (action == 0) {
                wxPostEvent(this, wxCommandEvent(EVT_BACKUP_POST));
            }
            else if (action == 1) {
                if (!m_plater->up_to_date(false, true)) {
                    m_plater->export_3mf(m_plater->model().get_backup_path() + "/.3mf", SaveStrategy::Backup);
                    m_plater->up_to_date(true, true);
                }
            }
         });
        Bind(EVT_BACKUP_POST, [](wxCommandEvent& e) {
            Slic3r::run_backup_ui_tasks();
            });
;    }
    bind_workspace_shortcuts();

    Bind(wxEVT_SHOW, [](wxShowEvent &evt) {
        DeviceManager *manger = wxGetApp().getDeviceManager();
        if (manger) {
            evt.IsShown() ? manger->start_refresher() : manger->stop_refresher();
        }
    });

#ifdef _MSW_DARK_MODE
    wxGetApp().UpdateDarkUIWin(this);
#endif // _MSW_DARK_MODE

    wxGetApp().persist_window_geometry(this, true);
    wxGetApp().persist_window_geometry(&m_settings_dialog, true);
}

bool MainFrame::handle_global_shortcut(const KeyChord& chord)
{
    // ImGui draws its text fields (the text tool's box, a sketch value field) on the GL canvas, so
    // while one of them is typed into the focused wx window is that canvas or an in-canvas frame:
    // neither is a wxControl, so focus_keeps_space() cannot see it and CHAR_HOOK takes the chord
    // here first -- Space opened the Speed Dial, Ctrl+P the Preferences, and the field never saw
    // the key. ImGui raises WantCaptureKeyboard while one of its items owns the keyboard, so leave
    // the key to it then. A focused wxControl (text field, list, web view) is untouched: those
    // keep their own keys, and ImGui's flag can outlive the focus moving to one of them.
    wxWindow* const focus = wxWindow::FindFocus();
    ImGuiWrapper* const imgui = wxGetApp().imgui();
    if (focus != nullptr && imgui != nullptr && imgui->want_keyboard() && dynamic_cast<wxControl*>(focus) == nullptr)
        return false;

    const std::optional<Shortcut> shortcut = wxGetApp().shortcuts().lookup(ShortcutContext::Global, chord);
    if (!shortcut.has_value())
        return false;

    switch (*shortcut) {
    case Shortcut::SlicePlate:
        if (m_slice_enable) {
            wxGetApp().plater()->update(true, true);
            wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_SLICE_PLATE));
            m_tabpanel->SelectPageByName(TAB_ID_PREVIEW);
        }
        break;
    case Shortcut::PrintPlate:
        m_plater->apply_background_progress();
        m_print_enable = get_enable_print_status();
        m_print_btn->Enable(m_print_enable);
        if (m_print_enable) {
            if (wxGetApp().preset_bundle->use_bbl_network() || wxGetApp().app_config->get_bool("use_printer_agents"))
                wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_PRINT_PLATE));
            else
                wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_SEND_GCODE));
        }
        return false;
    case Shortcut::ExportSlicedFile:
        if (can_export_gcode())
            wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_EXPORT_SLICED_FILE));
        return false;
    case Shortcut::PrintHostQueue: m_printhost_queue_dlg->Show(); break;
    case Shortcut::SpeedDial:
        if (!wxGetApp().app_config->get_bool("enable_speed_dial") || (chord == KeyChord{ WXK_SPACE } && focus_keeps_space(wxWindow::FindFocus())))
            return false;
        // Deferred out of the native key-event stack: open_speed_dial() may create a WebView and run script.
        CallAfter([] { wxGetApp().open_speed_dial(); });
        break;
    case Shortcut::NewProject: m_plater->new_project(); break;
    case Shortcut::OpenProject: m_plater->load_project(); break;
    case Shortcut::SaveProjectAs:
        if (can_save_as())
            m_plater->save_project(true);
        break;
    case Shortcut::SaveProject:
        if (can_save())
            m_plater->save_project();
        break;
    case Shortcut::Search:
        if (m_plater && is_prepare_or_preview_tab())
            m_plater->sidebar().can_search();
        return false;
    case Shortcut::Preferences:
        // Orca: Use GUI_App::open_preferences instead of direct call so windows associations are updated on exit
        wxGetApp().open_preferences();
        plater()->get_current_canvas3D()->force_set_focus();
        break;
    case Shortcut::ImportModel:
        if (can_add_models() && m_plater)
            m_plater->add_file();
        break;
    case Shortcut::Publish3mf:
        if (can_export_model())
            publish_project();
        break;
    case Shortcut::ShowLabels:
        if (m_plater && m_plater->is_view3D_shown()) {
            m_plater->show_view3D_labels(!m_plater->are_view3D_labels_shown());
            m_plater->get_current_canvas3D()->post_event(SimpleEvent(wxEVT_PAINT));
        }
        break;
    case Shortcut::ViewDefault:
        if (m_plater) {
            select_view("plate");
            m_plater->get_current_canvas3D()->zoom_to_bed();
        }
        break;
    case Shortcut::ViewTop:    select_view("top"); break;
    case Shortcut::ViewBottom: select_view("bottom"); break;
    case Shortcut::ViewFront:  select_view("front"); break;
    case Shortcut::ViewRear:   select_view("rear"); break;
    case Shortcut::ViewLeft:   select_view("left"); break;
    case Shortcut::ViewRight:  select_view("right"); break;
    case Shortcut::ViewPlate:
        if (m_plater)
            m_plater->get_current_canvas3D()->select_plate();
        break;
    default: return false;
    }
    return true;
}

DiffPresetDialog* MainFrame::make_diff_dialog()
{
    auto* dialog = new DiffPresetDialog(this);

    auto get_tab = [](Preset::Type type) {
        Tab* null_tab = nullptr;
        for (Tab* tab : wxGetApp().tabs_list)
            if (tab->type() == type)
                return tab;
        return null_tab;
    };

    auto transfer = [dialog, get_tab](Preset::Type type) {
        get_tab(type)->transfer_options(dialog->get_left_preset_name(type),
                                        dialog->get_right_preset_name(type),
                                        dialog->get_selected_options(type));
    };

    auto process_options = [dialog](std::function<void(Preset::Type)> process) {
        const Preset::Type diff_dlg_type = dialog->view_type();
        if (diff_dlg_type == Preset::TYPE_INVALID) {
            for (const Preset::Type& type : dialog->types_list() )
                process(type);
        }
        else
            process(diff_dlg_type);
    };

    dialog->Bind(EVT_DIFF_DIALOG_TRANSFER, [process_options, transfer](SimpleEvent&) { process_options(transfer); });
    return dialog;
}


#ifdef __WXMSW__

// Orca: Fix maximized window overlaps taskbar when taskbar auto hide is enabled (#8085)
// Adopted from https://gist.github.com/MortenChristiansen/6463580
static void AdjustWorkingAreaForAutoHide(const HWND hWnd, MINMAXINFO* mmi)
{
    const auto taskbarHwnd = FindWindowA("Shell_TrayWnd", nullptr);
    if (!taskbarHwnd) {
        return;
    }
    const auto monitorContainingApplication = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONULL);
    const auto monitorWithTaskbarOnIt = MonitorFromWindow(taskbarHwnd, MONITOR_DEFAULTTONULL);
    if (monitorContainingApplication != monitorWithTaskbarOnIt) {
        return;
    }
    APPBARDATA abd;
    abd.cbSize = sizeof(APPBARDATA);
    abd.hWnd   = taskbarHwnd;

    // Find if task bar has auto-hide enabled
    const auto uState = (UINT) SHAppBarMessage(ABM_GETSTATE, &abd);
    if ((uState & ABS_AUTOHIDE) != ABS_AUTOHIDE) {
        return;
    }

    RECT borderThickness;
    SetRectEmpty(&borderThickness);
    AdjustWindowRectEx(&borderThickness, GetWindowLongPtr(hWnd, GWL_STYLE) & ~WS_CAPTION, FALSE, 0);

    // Determine taskbar position
    SHAppBarMessage(ABM_GETTASKBARPOS, &abd);
    const auto& rc = abd.rc;
    if (rc.top == rc.left && rc.bottom > rc.right) {
        // Left
        const auto offset = borderThickness.left + 2;
        mmi->ptMaxPosition.x += offset;
        mmi->ptMaxTrackSize.x -= offset;
        mmi->ptMaxSize.x -= offset;
    } else if (rc.top == rc.left && rc.bottom < rc.right) {
        // Top
        const auto offset = borderThickness.top + 2;
        mmi->ptMaxPosition.y += offset;
        mmi->ptMaxTrackSize.y -= offset;
        mmi->ptMaxSize.y -= offset;
    } else if (rc.top > rc.left) {
        // Bottom
        const auto offset = borderThickness.bottom + 2;
        mmi->ptMaxSize.y -= offset;
        mmi->ptMaxTrackSize.y -= offset;
    } else {
        // Right
        const auto offset = borderThickness.right + 2;
        mmi->ptMaxSize.x -= offset;
        mmi->ptMaxTrackSize.x -= offset;
    }
}



#endif

void  MainFrame::show_log_window()
{
    m_log_window = new wxLogWindow(this, _L("Logging"), true, false);
    m_log_window->Show();
}

//BBS GUI refactor: remove unused layout new/dlg










































#ifdef __WXGTK__
void MainFrame::update_edge_panels()
{
    if (!m_edge_bottom) return;

    bool hide = IsMaximized() || IsFullScreen();
    m_edge_bottom->Show(!hide);
    m_edge_left->Show(!hide);
    m_edge_right->Show(!hide);
    if (hide) return;

    constexpr int B = ResizeEdgePanel::BORDER_PX;
    wxSize cs = GetClientSize();
    m_edge_bottom->SetSize(0, cs.y - B, cs.x, B);
    m_edge_left->SetSize(0, 0, B, cs.y);
    m_edge_right->SetSize(cs.x - B, 0, B, cs.y);

    m_edge_bottom->Raise();
    m_edge_left->Raise();
    m_edge_right->Raise();
}
#endif

// Called when closing the application and when switching the application language.
void MainFrame::shutdown()
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "MainFrame::shutdown enter";
    m_idle.stop();
    if (ProjectPanel* project = ProjectPanel::if_built())
        project->shutdown();
    m_plugin_pages.shutdown();
    if (m_plater != nullptr)
        m_plater->remove_dock_panes();
#ifdef SLIC3R_CAD
    if (DesignPanel* design = DesignPanel::if_built())
        design->shutdown();
#endif
#ifdef __WXGTK__
    // Edge panels are child windows — wxWidgets destroys them automatically.
    m_edge_bottom = nullptr;
    m_edge_left   = nullptr;
    m_edge_right  = nullptr;
#endif
    // BBS: backup
    Slic3r::set_backup_callback(nullptr);
    if (m_redesign_shell != nullptr)
        m_redesign_shell->disconnect_model_generation_host();
    if (m_ai_feature_host != nullptr)
        m_ai_feature_host->shutdown();
#ifdef _WIN32
	if (m_hDeviceNotify) {
		::UnregisterDeviceNotification(HDEVNOTIFY(m_hDeviceNotify));
		m_hDeviceNotify = nullptr;
	}
 	if (m_ulSHChangeNotifyRegister) {
        SHChangeNotifyDeregister(m_ulSHChangeNotifyRegister);
        m_ulSHChangeNotifyRegister = 0;
 	}
#endif // _WIN32

    if (m_plater != nullptr) {
        m_plater->get_ui_job_worker().cancel_all();

        // Unbinding of wxWidgets event handling in canvases needs to be done here because on MAC,
        // when closing the application using Command+Q, a mouse event is triggered after this lambda is completed,
        // causing a crash
        m_plater->unbind_canvas_event_handlers();

        // Cleanup of canvases' volumes needs to be done here or a crash may happen on some Linux Debian flavours
        m_plater->reset_canvas_volumes();
    }

    // Weird things happen as the Paint messages are floating around the windows being destructed.
    // Avoid the Paint messages by hiding the main window.
    // Also the application closes much faster without these unnecessary screen refreshes.
    // In addition, there were some crashes due to the Paint events sent to already destructed windows.
    this->Show(false);

    if (m_settings_dialog.IsShown())
        // call Close() to trigger call to lambda defined into GUI_App::persist_window_geometry()
        m_settings_dialog.Close();

    if (m_plater != nullptr) {
        // Stop the background thread (Windows and Linux).
        // Disconnect from a 3DConnextion driver (OSX).
        m_plater->get_mouse3d_controller().shutdown();
        // Store the device parameter database back to appconfig.
        m_plater->get_mouse3d_controller().save_config(*wxGetApp().app_config);
    }

    // stop agent
    NetworkAgent* agent = wxGetApp().getAgent();
    if (agent)
        agent->track_enable(false);

    // Stop the background thread of the removable drive manager, so that no new updates will be sent to the Plater.
    //wxGetApp().removable_drive_manager()->shutdown();
	//stop listening for messages from other instances
	wxGetApp().other_instance_message_handler()->shutdown(this);
    // Save the slic3r.ini.Usually the ini file is saved from "on idle" callback,
    // but in rare cases it may not have been called yet.
    if(wxGetApp().app_config->dirty())
        wxGetApp().app_config->save();
//         if (m_plater)
//             m_plater->print = undef;
//         Slic3r::GUI::deregister_on_request_update_callback();

    // set to null tabs and a plater
    // to avoid any manipulations with them from App->wxEVT_IDLE after of the mainframe closing
    wxGetApp().tabs_list.clear();
    wxGetApp().model_tabs_list.clear();
    wxGetApp().shutdown();
    // BBS: why clear ?
    //wxGetApp().plater_ = nullptr;

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "MainFrame::shutdown exit";
}

void MainFrame::update_filament_tab_ui()
{
    wxGetApp().get_tab(Preset::Type::TYPE_FILAMENT)->reload_config();
    wxGetApp().get_tab(Preset::Type::TYPE_FILAMENT)->update_dirty();
    wxGetApp().get_tab(Preset::Type::TYPE_FILAMENT)->update_tab_ui();
}

void MainFrame::update_title()
{
    return;
}

void MainFrame::show_publish_button(bool show)
{
    // m_publish_btn->Show(show);
    // Layout();
}

void MainFrame::update_title_colour_after_set_title()
{
#ifdef __APPLE__
    set_title_colour_after_set_title(GetHandle());
#endif
}

void MainFrame::show_option(bool show)
{
    if (!show) {
        if (m_slice_btn->IsShown()) {
            m_slice_btn->Hide();
            m_print_btn->Hide();
            m_slice_option_btn->Hide();
            m_print_option_btn->Hide();
            Layout();
        }
    } else {
        if (!m_slice_btn->IsShown()) {
            m_slice_btn->Show();
            m_print_btn->Show();
            m_slice_option_btn->Show();
            m_print_option_btn->Show();
            Layout();
        }
    }
}





// SoftFever
void MainFrame::show_device(bool should_use_native) {
    auto idx = -1;

    const bool use_printer_agents = wxGetApp().app_config->get_bool("use_printer_agents");

    // The web Device page is the extra tab printer-agents mode shows alongside the native one.
    // Printers that drive the native Bambu device tab have nothing to put in it, so they don't
    // get it — otherwise a Bambu user sees two Device tabs, one of them permanently empty.
    const bool want_web_device_tab = use_printer_agents && wxGetApp().preset_bundle != nullptr &&
                                     !wxGetApp().preset_bundle->use_bbl_device_tab();

    // Remove the extra page before switching to any layout that shouldn't have it.
    if (!want_web_device_tab) {
        if ((idx = m_tabpanel->FindPageByName(TAB_ID_MONITOR_WEB)) != wxNOT_FOUND) {
            m_printer_view_page->Show(false);
            m_tabpanel->RemovePage(idx);
        }
    }

    if (use_printer_agents) {
        if (!m_monitor_page->in_book()) {
            if ((idx = m_tabpanel->FindPage(m_printer_view_page)) != wxNOT_FOUND) {
                m_printer_view_page->Show(false);
                m_tabpanel->RemovePage(idx);
            }
            m_monitor_page->Show(false);
            m_tabpanel->InsertPage(m_tabpanel->PositionAfter({TAB_ID_PREVIEW}), TAB_ID_MONITOR, m_monitor_page,
                                   _L("Device"), "tab_monitor_active");
        }

        if (wxGetApp().is_enable_multi_machine()) {
            // TODO: change the bitmap
            if (!m_multi_machine_page->in_book()) {
                m_multi_machine_page->Show(false);
                // Past the web Device tab when it is already there, so enabling multi-machine
                // later can't wedge this page between the two Device tabs.
                m_tabpanel->InsertPage(m_tabpanel->PositionAfter({TAB_ID_MONITOR_WEB, TAB_ID_MONITOR}),
                                       TAB_ID_MULTI_DEVICE, m_multi_machine_page, _L("Multi-device"), "tab_multi_active");
            }
        }
        if (!m_calibration_page->in_book()) {
            m_calibration_page->Show(false);
            m_tabpanel->InsertPage(m_tabpanel->PositionAfter({TAB_ID_PROJECT}), TAB_ID_CALIBRATION, m_calibration_page,
                                   _L("Calibration"), "tab_calibration_active");
        }

        if (want_web_device_tab) {
            if ((idx = m_tabpanel->FindPage(m_printer_view_page)) == wxNOT_FOUND) {
                m_printer_view_page->Show(false);
                // Immediately right of the native Device tab, not at the end of the tab bar.
                m_tabpanel->InsertPage(m_tabpanel->PositionAfter({TAB_ID_MONITOR}), TAB_ID_MONITOR_WEB,
                                       m_printer_view_page, _L("Device (Web)"), "tab_monitor_active");
            } else {
                m_tabpanel->SetPageText(idx, _L("Device (Web)"));
            }
        }

#ifdef _MSW_DARK_MODE
        wxGetApp().UpdateDarkUIWin(this);
#endif // _MSW_DARK_MODE

        fit_tab_labels(); // ORCA on printer change
        m_plugin_pages.relayout(); // re-sync plugin tabs against the native tabs just mutated above
        if (m_prebuild_started)
            m_idle.start();
        return;
    }

    if (should_use_native) {
        if (m_monitor_page->in_book()) {
            fit_tab_labels(); // ORCA on printer change - same button layout
            return;
        }
        // Remove printer view
        if ((idx = m_tabpanel->FindPage(m_printer_view_page)) != wxNOT_FOUND) {
            m_printer_view_page->Show(false);
            m_tabpanel->RemovePage(idx);
        }

        // Insert monitor page
        m_monitor_page->Show(false);
        m_tabpanel->InsertPage(m_tabpanel->PositionAfter({TAB_ID_PREVIEW}), TAB_ID_MONITOR, m_monitor_page,
                               _L("Device"), "tab_monitor_active");

        if (wxGetApp().is_enable_multi_machine()) {
            // TODO: change the bitmap
            m_multi_machine_page->Show(false);
            m_tabpanel->InsertPage(m_tabpanel->PositionAfter({TAB_ID_MONITOR}), TAB_ID_MULTI_DEVICE, m_multi_machine_page,
                                   _L("Multi-device"), "tab_multi_active");
        }
        m_calibration_page->Show(false);
        // Last of the built-in tabs, but plugin tabs already sit past it — anchor rather than
        // append, so its position doesn't depend on the relayout() below running afterwards.
        m_tabpanel->InsertPage(m_tabpanel->PositionAfter({TAB_ID_PROJECT}), TAB_ID_CALIBRATION, m_calibration_page,
                               _L("Calibration"), "tab_calibration_active");

#ifdef _MSW_DARK_MODE
        wxGetApp().UpdateDarkUIWin(this);
#endif // _MSW_DARK_MODE

    } else {
        if (m_printer_view_page->in_book()) {
            fit_tab_labels(); // ORCA on printer change - same button layout
            return;
        }
        if ((idx = m_tabpanel->FindPage(m_calibration_page)) != wxNOT_FOUND) {
            m_calibration_page->Show(false);
            m_tabpanel->RemovePage(idx);
        }
        if ((idx = m_tabpanel->FindPage(m_multi_machine_page)) != wxNOT_FOUND) {
            m_multi_machine_page->Show(false);
            m_tabpanel->RemovePage(idx);
        }
        if ((idx = m_tabpanel->FindPage(m_monitor_page)) != wxNOT_FOUND) {
            m_monitor_page->Show(false);
            m_tabpanel->RemovePage(idx);
        }
        m_printer_view_page->Show(false);
        m_tabpanel->InsertPage(m_tabpanel->PositionAfter({TAB_ID_PREVIEW}), TAB_ID_MONITOR, m_printer_view_page,
                               _L("Device"), "tab_monitor_active");
    }
    fit_tab_labels(); // ORCA on printer change
    m_plugin_pages.relayout(); // re-sync plugin tabs against the native tabs just mutated above
    if (m_prebuild_started)
        m_idle.start();
}



void MainFrame::fit_tab_labels()
{
    if (!m_tabpanel || !m_slice_option_btn) // ignore layout change while slice/print buttons not visible
        return;

    auto* ctrl  = m_tabpanel->GetBtnsListCtrl();
    auto* sizer = ctrl->GetBtnsSizer();
    int   count = sizer->GetItemCount();

    // Restore all
    for (size_t i = 1; i < count; ++i)
        ctrl->SetCompact(i, false);
    m_tabpanel->Refresh();
    Layout();

    // Compact (last to first)
    for (size_t i = count - 1; i >= 1; --i) {
        int right = ScreenToClient(m_slice_option_btn->ClientToScreen({})).x;
        int left  = sizer->GetSize().GetWidth();
        // The overflow button sits between the tab buttons and the side tools, so it is not free space.
        if (const wxWindow* overflow = ctrl->GetOverflowButton())
            left += overflow->GetSize().GetWidth();
        if (right - left - FromDIP(15) > 0) return;
        ctrl->SetCompact(i, true);
        m_tabpanel->Refresh();
        Layout();
    }
}

bool MainFrame::preview_only_hint()
{
    if (m_plater && (m_plater->only_gcode_mode() || (m_plater->using_exported_file()))) {
        BOOST_LOG_TRIVIAL(info) << boost::format("skipped tab switch from %1% to %2% in preview mode")%selected_tab_id() %wxString(TAB_ID_PREPARE);

        ConfirmBeforeSendDialog confirm_dlg(this, wxID_ANY, _L("Warning"));
        confirm_dlg.Bind(EVT_SECONDARY_CHECK_CONFIRM, [this](wxCommandEvent& e) {
            preview_only_to_editor = true;
        });
        confirm_dlg.update_btn_label(_L("Yes"), _L("No"));
        auto filename = m_plater->get_preview_only_filename();

        // TRN %s is a file name.
        confirm_dlg.update_text(wxString::Format(_L("%s will be closed before creating a new model. Do you want to continue?"), from_u8(filename)));
        confirm_dlg.on_show();
        if (preview_only_to_editor) {
            m_plater->new_project();
            preview_only_to_editor = false;
        }

        return false;
    }

    return true;
}

#ifdef WIN32
void MainFrame::register_win32_callbacks()
{
    //static GUID GUID_DEVINTERFACE_USB_DEVICE  = { 0xA5DCBF10, 0x6530, 0x11D2, 0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED };
    //static GUID GUID_DEVINTERFACE_DISK        = { 0x53f56307, 0xb6bf, 0x11d0, 0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b };
    //static GUID GUID_DEVINTERFACE_VOLUME      = { 0x71a27cdd, 0x812a, 0x11d0, 0xbe, 0xc7, 0x08, 0x00, 0x2b, 0xe2, 0x09, 0x2f };
    static GUID GUID_DEVINTERFACE_HID           = { 0x4D1E55B2, 0xF16F, 0x11CF, { 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 } };

    // Register USB HID (Human Interface Devices) notifications to trigger the 3DConnexion enumeration.
    DEV_BROADCAST_DEVICEINTERFACE NotificationFilter = { 0 };
    NotificationFilter.dbcc_size = sizeof(DEV_BROADCAST_DEVICEINTERFACE);
    NotificationFilter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    NotificationFilter.dbcc_classguid = GUID_DEVINTERFACE_HID;
    m_hDeviceNotify = ::RegisterDeviceNotification(this->GetHWND(), &NotificationFilter, DEVICE_NOTIFY_WINDOW_HANDLE);

// or register for file handle change?
//      DEV_BROADCAST_HANDLE NotificationFilter = { 0 };
//      NotificationFilter.dbch_size = sizeof(DEV_BROADCAST_HANDLE);
//      NotificationFilter.dbch_devicetype = DBT_DEVTYP_HANDLE;

    // Using Win32 Shell API to register for media insert / removal events.
    LPITEMIDLIST ppidl;
    if (SHGetSpecialFolderLocation(this->GetHWND(), CSIDL_DESKTOP, &ppidl) == NOERROR) {
        SHChangeNotifyEntry shCNE;
        shCNE.pidl       = ppidl;
        shCNE.fRecursive = TRUE;
        // Returns a positive integer registration identifier (ID).
        // Returns zero if out of memory or in response to invalid parameters.
        m_ulSHChangeNotifyRegister = SHChangeNotifyRegister(this->GetHWND(),        // Hwnd to receive notification
            SHCNE_DISKEVENTS,                                                       // Event types of interest (sources)
            SHCNE_MEDIAINSERTED | SHCNE_MEDIAREMOVED,
            //SHCNE_UPDATEITEM,                                                     // Events of interest - use SHCNE_ALLEVENTS for all events
            WM_USER_MEDIACHANGED,                                                   // Notification message to be sent upon the event
            1,                                                                      // Number of entries in the pfsne array
            &shCNE);                                                                // Array of SHChangeNotifyEntry structures that
                                                                                    // contain the notifications. This array should
                                                                                    // always be set to one when calling SHChnageNotifyRegister
                                                                                    // or SHChangeNotifyDeregister will not work properly.
        assert(m_ulSHChangeNotifyRegister != 0);    // Shell notification failed
    } else {
        // Failed to get desktop location
        assert(false);
    }

    {
        static constexpr int device_count = 1;
        RAWINPUTDEVICE devices[device_count] = {};
        // multi-axis mouse (SpaceNavigator, etc.)
        devices[0].usUsagePage = 0x01;
        devices[0].usUsage = 0x08;
        if (! RegisterRawInputDevices(devices, device_count, sizeof(RAWINPUTDEVICE)))
            BOOST_LOG_TRIVIAL(error) << "RegisterRawInputDevices failed";
    }
}
#endif // _WIN32

void MainFrame::create_preset_tabs()
{
    wxGetApp().update_label_colours_from_appconfig();

    //BBS: GUI refactor
    //m_param_panel = new ParamsPanel(m_tabpanel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBK_LEFT | wxTAB_TRAVERSAL);
    m_param_dialog = new ParamsDialog(m_plater);

    add_created_tab(new TabPrint(m_param_panel), "cog");
    add_created_tab(new TabPrintPlate(m_param_panel), "cog");
    add_created_tab(new TabPrintObject(m_param_panel), "cog");
    add_created_tab(new TabPrintPart(m_param_panel), "cog");
    add_created_tab(new TabPrintLayer(m_param_panel), "cog");
    add_created_tab(new TabFilament(m_param_dialog->panel()), "spool");
    /* BBS work around to avoid appearance bug */
    //add_created_tab(new TabSLAPrint(m_param_panel));
    //add_created_tab(new TabSLAMaterial(m_param_panel));
    add_created_tab(new TabPrinter(m_param_dialog->panel()), "printer");

    m_param_panel->rebuild_panels();
    m_param_dialog->panel()->rebuild_panels();
    //m_tabpanel->AddPage(m_param_panel, "Parameters", "notebook_presets_active");
    //m_tabpanel->InsertPage(tpSettings, m_param_panel, _L("Parameters"), std::string("cog"));
}

void MainFrame::add_created_tab(Tab* panel,  const std::string& bmp_name /*= ""*/)
{
    panel->create_preset_tab();

    if (panel->type() == Preset::TYPE_PLATE) {
        wxGetApp().tabs_list.pop_back();
        wxGetApp().plate_tab = panel;
    }
    // BBS: model config
    if (panel->type() == Preset::TYPE_MODEL) {
        wxGetApp().tabs_list.pop_back();
        wxGetApp().model_tabs_list.push_back(panel);
    }
}

wxBoxSizer* MainFrame::create_side_tools()
{
    enable_multi_machine = wxGetApp().is_enable_multi_machine();
    int em = em_unit();
    wxBoxSizer* sizer = m_native_side_tools = new wxBoxSizer(wxHORIZONTAL);

    m_slice_select = eSlicePlate;
    m_print_select = ePrintPlate;

    auto slice_panel = new wxPanel(this,wxID_ANY,wxDefaultPosition,wxDefaultSize);
    auto print_panel = new wxPanel(this,wxID_ANY,wxDefaultPosition,wxDefaultSize);
    slice_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#3B4446")));
    print_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#3B4446")));

    m_slice_btn = new SideButton(slice_panel, _L("Slice plate"), "");
    m_slice_option_btn = new SideButton(slice_panel, "", "sidebutton_dropdown", 0, 14);
    m_print_btn = new SideButton(print_panel, _L("Print plate"), "");
    m_print_option_btn = new SideButton(print_panel, "", "sidebutton_dropdown", 0, 14);

    // Orca: restore the last used print/export action if the user opted to remember it
    PrintSelectType remembered_print_select;
    if (get_remembered_print_select(remembered_print_select)) {
        m_print_select = remembered_print_select;
        m_print_btn->SetLabel(print_select_type_label(remembered_print_select));
        fit_tab_labels(); // ORCA on label change
    }

    auto slice_sizer = new wxBoxSizer(wxHORIZONTAL);
    slice_sizer->Add(m_slice_option_btn, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(1));
    slice_sizer->Add(m_slice_btn, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(1));
    slice_panel->SetSizer(slice_sizer);

    auto print_sizer = new wxBoxSizer(wxHORIZONTAL);
    print_sizer->Add(m_print_option_btn, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(1));
    print_sizer->Add(m_print_btn, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(1));
    print_panel->SetSizer(print_sizer);

    update_side_button_style();
    m_slice_option_btn->Enable();
    m_print_option_btn->Enable();
    //sizer->Add(FromDIP(15), 0, 0, 0, 0);
    sizer->Add(slice_panel);
    sizer->Add(FromDIP(15), 0, 0, 0, 0);
    sizer->Add(print_panel);
    sizer->Add(FromDIP(19), 0, 0, 0, 0);

    sizer->Layout();

    m_filament_group_popup = new FilamentGroupPopup(m_slice_btn);

    auto try_hover_pop_up = [this]() {
#ifdef __APPLE__
        if (!IsActive()) {
            return;
        }
#endif
        wxPoint pos = m_slice_btn->ClientToScreen(wxPoint(0, 0));
        pos.y += m_slice_btn->GetRect().height * 1.25;
        pos.x -= (m_slice_option_btn->GetRect().width + FromDIP(380) * 0.6);
        auto curr_plate = this->m_plater->get_partplate_list().get_curr_plate();
        m_filament_group_popup->SetPosition(pos);
        m_filament_group_popup->tryPopup(m_plater, curr_plate, m_slice_select == eSliceAll);
        };

#ifndef __linux__
// in linux plateform, the pop up will taker over the mouse event and make the slice button cannot handle click event
    // this pannel is used to trigger hover when button is disabled
    slice_panel->Bind(wxEVT_ENTER_WINDOW, [this,try_hover_pop_up](auto& event) {
        if(!m_slice_option_pop_up || !m_slice_option_pop_up->IsShown())
            try_hover_pop_up();
        });

    slice_panel->Bind(wxEVT_LEAVE_WINDOW, [this](auto& event) {
        m_filament_group_popup->tryClose();
        });

    m_slice_btn->Bind(wxEVT_ENTER_WINDOW, [this, try_hover_pop_up](auto& event) {
        if (!m_slice_option_pop_up || !m_slice_option_pop_up->IsShown())
            try_hover_pop_up();
        });

    m_slice_btn->Bind(wxEVT_LEAVE_WINDOW, [this](auto& event) {
        m_filament_group_popup->tryClose();
        });
#endif

    m_slice_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent& event)
        {
            if (route_legacy_command_to_redesign("Slice button", TAB_ID_PREVIEW))
                return;

            //this->m_plater->select_view_3D("Preview");
            m_plater->exit_gizmo();
            m_plater->update(true, true);
            m_slice_enable = get_enable_slice_status();
            m_slice_btn->Enable(m_slice_enable);
            if (!m_slice_enable)
                return;

            bool slice = true;

            auto curr_plate = m_plater->get_partplate_list().get_curr_plate();
            #ifdef __linux__
                PresetBundle* preset = wxGetApp().preset_bundle;
                bool force_show_fila_group_dlg        = (preset && preset->is_bbl_vendor() && preset->get_printer_extruder_count() == 2);
                slice = try_pop_up_before_slice(m_slice_select == eSliceAll, m_plater, curr_plate, force_show_fila_group_dlg);
            #else
                slice = try_pop_up_before_slice(m_slice_select == eSliceAll, m_plater, curr_plate, false);
            #endif

            if (slice) {
                if (m_slice_select == eSliceAll)
                    wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_SLICE_ALL));
                else
                    wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_SLICE_PLATE));
                select_tab(TAB_ID_PREVIEW);
            }
        });

    m_print_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent& event)
        {
            if (route_legacy_command_to_redesign("Print button", TAB_ID_PREVIEW))
                return;

            //this->m_plater->select_view_3D("Preview");
            if (m_print_select == ePrintAll || m_print_select == ePrintPlate || m_print_select == ePrintMultiMachine)
            {
                m_plater->apply_background_progress();
                // check valid of print
                m_print_enable = get_enable_print_status();
                m_print_btn->Enable(m_print_enable);
                if (m_print_enable) {
                    if (m_print_select == ePrintAll)
                        wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_PRINT_ALL));
                    if (m_print_select == ePrintPlate)
                        wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_PRINT_PLATE));
                    if(m_print_select == ePrintMultiMachine)
                         wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_PRINT_MULTI_MACHINE));
                }
            }
            else if (m_print_select == eExportGcode)
                wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_EXPORT_GCODE));
            else if (m_print_select == eSendGcode)
                wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_SEND_GCODE));
            else if (m_print_select == eUploadGcode)
                wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_UPLOAD_GCODE));
            else if (m_print_select == eExportSlicedFile)
                wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_EXPORT_SLICED_FILE));
            else if (m_print_select == eExportAllSlicedFile)
                wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_EXPORT_ALL_SLICED_FILE));
            else if (m_print_select == eSendToPrinter)
                wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_SEND_TO_PRINTER));
            else if (m_print_select == eSendToPrinterAll)
                wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_SEND_TO_PRINTER_ALL));
            /* else if (m_print_select == ePrintMultiMachine)
                 wxPostEvent(m_plater, SimpleEvent(EVT_GLTOOLBAR_PRINT_MULTI_MACHINE));*/
        });

    m_slice_option_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent& event)
        {
            if(m_slice_option_pop_up)
                delete m_slice_option_pop_up;
            m_slice_option_pop_up = new SidePopup(this);
            SideButton* slice_all_btn = new SideButton(m_slice_option_pop_up, _L("Slice all"), "");
            slice_all_btn->SetCornerRadius(0);
            SideButton* slice_plate_btn = new SideButton(m_slice_option_pop_up, _L("Slice plate"), "");
            slice_plate_btn->SetCornerRadius(0);

            slice_all_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
                m_slice_btn->SetLabel(_L("Slice all"));
                m_slice_select = eSliceAll;
                m_slice_enable = get_enable_slice_status();
                m_slice_btn->Enable(m_slice_enable);
                this->Layout();
                fit_tab_labels(); // ORCA on label change
                if(m_slice_option_pop_up)
                    m_slice_option_pop_up->Dismiss();
                });

            slice_plate_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
                m_slice_btn->SetLabel(_L("Slice plate"));
                m_slice_select = eSlicePlate;
                m_slice_enable = get_enable_slice_status();
                m_slice_btn->Enable(m_slice_enable);
                this->Layout();
                fit_tab_labels(); // ORCA on label change
                if(m_slice_option_pop_up)
                    m_slice_option_pop_up->Dismiss();
                });
            m_slice_option_pop_up->append_button(slice_all_btn);
            m_slice_option_pop_up->append_button(slice_plate_btn);
            m_slice_option_pop_up->Popup(m_slice_btn);
        }
    );

    m_print_option_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent& event)
        {
            SidePopup* p = new SidePopup(this);
            for (PrintSelectType type : available_print_actions()) {
                SideButton* btn = new SideButton(p, print_select_type_label(type), "");
                btn->SetCornerRadius(0);
                btn->Bind(wxEVT_BUTTON, [this, p, type](wxCommandEvent&) {
                    select_print_action(type);
                    p->Dismiss();
                    });
                p->append_button(btn);
            }
            p->Popup(m_print_btn);
        }
    );

    /*
    Button * aux_btn = new Button(this, _L("Auxiliary"));
    aux_btn->SetBackgroundColour(0x3B4446);
    aux_btn->Bind(wxEVT_BUTTON, [](auto e) {
        wxGetApp().sidebar().show_auxiliary_dialog();
    });
    sizer->Add(aux_btn, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, 1 * em / 10);
    */
    sizer->Add(FromDIP(19), 0, 0, 0, 0);

    return sizer;
}





void MainFrame::update_side_button_style()
{
    // BBS
    int em = em_unit();

    /*m_slice_btn->SetLayoutStyle(1);
    m_slice_btn->SetTextLayout(SideButton::EHorizontalOrientation::HO_Center, FromDIP(15));
    m_slice_btn->SetMinSize(wxSize(-1, FromDIP(24)));
    m_slice_btn->SetCornerRadius(FromDIP(12));
    m_slice_btn->SetExtraSize(wxSize(FromDIP(38), FromDIP(10)));
    m_slice_btn->SetBottomColour(wxColour(0x3B4446));*/
    StateColor m_btn_bg_enable = StateColor(
        std::pair<wxColour, int>(wxColour(0, 137, 123), StateColor::Pressed),
        std::pair<wxColour, int>(wxColour(48, 221, 112), StateColor::Hovered),
        std::pair<wxColour, int>(wxColour(0, 150, 136), StateColor::Normal)
    );

    // m_publish_btn->SetMinSize(wxSize(FromDIP(125), FromDIP(24)));
    // m_publish_btn->SetCornerRadius(FromDIP(12));
    // m_publish_btn->SetBackgroundColor(m_btn_bg_enable);
    // m_publish_btn->SetBorderColor(m_btn_bg_enable);
    // m_publish_btn->SetBackgroundColour(wxColour(59,68,70));
    // m_publish_btn->SetTextColor(StateColor::darkModeColorFor("#FFFFFE"));

    m_slice_btn->SetTextLayout(SideButton::EHorizontalOrientation::HO_Left, FromDIP(15));
    m_slice_btn->SetCornerRadius(FromDIP(12));
    m_slice_btn->SetExtraSize(wxSize(FromDIP(38), FromDIP(10)));
    m_slice_btn->SetMinSize(wxSize(-1, FromDIP(24)));

    m_slice_option_btn->SetTextLayout(SideButton::EHorizontalOrientation::HO_Center);
    m_slice_option_btn->SetCornerRadius(FromDIP(12));
    m_slice_option_btn->SetExtraSize(wxSize(FromDIP(10), FromDIP(10)));
    m_slice_option_btn->SetIconOffset(FromDIP(2));
    m_slice_option_btn->SetMinSize(wxSize(FromDIP(24), FromDIP(24)));

    m_print_btn->SetTextLayout(SideButton::EHorizontalOrientation::HO_Left, FromDIP(15));
    m_print_btn->SetCornerRadius(FromDIP(12));
    m_print_btn->SetExtraSize(wxSize(FromDIP(38), FromDIP(10)));
    m_print_btn->SetMinSize(wxSize(-1, FromDIP(24)));

    m_print_option_btn->SetTextLayout(SideButton::EHorizontalOrientation::HO_Center);
    m_print_option_btn->SetCornerRadius(FromDIP(12));
    m_print_option_btn->SetExtraSize(wxSize(FromDIP(10), FromDIP(10)));
    m_print_option_btn->SetIconOffset(FromDIP(2));
    m_print_option_btn->SetMinSize(wxSize(FromDIP(24), FromDIP(24)));

    // Keep panel backgrounds in sync with SideButton's darkModeColorFor(#3B4446) bottom strip
    auto bg = StateColor::darkModeColorFor(wxColour("#3B4446"));
    m_slice_btn->GetParent()->SetBackgroundColour(bg);
    m_print_btn->GetParent()->SetBackgroundColour(bg);
}










void MainFrame::set_max_recent_count(int max)
{
    max = max < 0 ? 0 : max > 999 ? 999 : max;
    size_t count = m_recent_projects.GetCount();
    m_recent_projects.SetMaxFiles(max);
    if (count != m_recent_projects.GetCount()) {
        count = m_recent_projects.GetCount();
        std::vector<std::string> recent_projects;
        for (size_t i = 0; i < count; ++i) {
            recent_projects.push_back(into_u8(m_recent_projects.GetHistoryFile(i)));
        }
        wxGetApp().app_config->set_recent_projects(recent_projects);
        wxGetApp().app_config->save();
        if (WebViewPanel* home = WebViewPanel::if_built())
            home->SendRecentList(-1);
    }
}

void MainFrame::open_menubar_item(const wxString& menu_name,const wxString& item_name)
{
    if (m_menubar == nullptr)
        return;
    // Get menu object from menubar
    int     menu_index = m_menubar->FindMenu(menu_name);
    wxMenu* menu       = m_menubar->GetMenu(menu_index);
    if (menu == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "Mainframe open_menubar_item function couldn't find menu: " << menu_name;
        return;
    }
    // Get item id from menu
    int     item_id   = menu->FindItem(item_name);
    if (item_id == wxNOT_FOUND)
    {
        // try adding three dots char
        item_id = menu->FindItem(item_name + dots);
    }
    if (item_id == wxNOT_FOUND)
    {
        BOOST_LOG_TRIVIAL(error) << "Mainframe open_menubar_item function couldn't find item: " << item_name;
        return;
    }
    // wxEVT_MENU will trigger item
    wxPostEvent((wxEvtHandler*)menu, wxCommandEvent(wxEVT_MENU, item_id));
}

void MainFrame::init_menubar_as_gcodeviewer()
{
    //BBS do not show gcode viewer mebu
#if 0
    wxMenu* fileMenu = new wxMenu;
    {
        append_menu_item(fileMenu, wxID_ANY, _L("&Open G-code") + dots + "\t" + ctrl + "O", _L("Open a G-code file"),
            [this](wxCommandEvent&) { if (m_plater != nullptr) m_plater->load_gcode(); }, "open", nullptr,
            [this]() {return m_plater != nullptr; }, this);
#ifdef __APPLE__
        append_menu_item(fileMenu, wxID_ANY, _L("Re&load from Disk") + dots + "\t" + ctrl + shift + "R",
            _L("Reload the plater from disk"), [this](wxCommandEvent&) { m_plater->reload_gcode_from_disk(); },
            "", nullptr, [this]() { return !m_plater->get_last_loaded_gcode().empty(); }, this);
#else
        append_menu_item(fileMenu, wxID_ANY, _L("Re&load from Disk") + sep + "F5",
            _L("Reload the plater from disk"), [this](wxCommandEvent&) { m_plater->reload_gcode_from_disk(); },
            "", nullptr, [this]() { return !m_plater->get_last_loaded_gcode().empty(); }, this);
#endif // __APPLE__
        fileMenu->AppendSeparator();
        append_menu_item(fileMenu, wxID_ANY, _L("Export &Toolpaths as OBJ") + dots, _L("Export toolpaths as OBJ"),
            [this](wxCommandEvent&) { if (m_plater != nullptr) m_plater->export_toolpaths_to_obj(); }, "export_plater", nullptr,
            [this]() {return can_export_toolpaths(); }, this);
        append_menu_item(fileMenu, wxID_ANY, _L("Open &Slicer") + dots, _L("Open Slicer"),
            [](wxCommandEvent&) { start_new_slicer(); }, "", nullptr,
            []() {return true; }, this);
        fileMenu->AppendSeparator();
        append_menu_item(fileMenu, wxID_EXIT, _L("&Quit"), wxString::Format(_L("Quit %s"), SLIC3R_APP_NAME),
            [this](wxCommandEvent&) { Close(false); });
    }

    // View menu
    wxMenu* viewMenu = nullptr;
    if (m_plater != nullptr) {
        viewMenu = new wxMenu();
        add_common_view_menu_items(viewMenu, std::bind(&MainFrame::can_change_view, this));
    }

    // helpmenu
    auto helpMenu = generate_help_menu();

    m_menubar = new wxMenuBar();
    m_menubar->Append(fileMenu, _L("&File"));
    if (viewMenu != nullptr) m_menubar->Append(viewMenu, _L("&View"));
    // Add additional menus from C++
    wxGetApp().add_config_menu(m_menubar);
    m_menubar->Append(helpMenu, _L("&Help"));
    SetMenuBar(m_menubar);

#ifdef __APPLE__
    // This fixes a bug on Mac OS where the quit command doesn't emit window close events
    // wx bug: https://trac.wxwidgets.org/ticket/18328
    wxMenu* apple_menu = m_menubar->OSXGetAppleMenu();
    if (apple_menu != nullptr) {
        apple_menu->Bind(wxEVT_MENU, [this](wxCommandEvent&) {
            Close();
            }, wxID_EXIT);
    }
#endif // __APPLE__
#endif
}

void MainFrame::update_menubar()
{
    if (wxGetApp().is_gcode_viewer())
        return;

    const bool is_fff = plater()->printer_technology() == ptFFF;
}

void MainFrame::reslice_now()
{
    if (m_plater)
        m_plater->reslice();
}

struct ConfigsOverwriteConfirmDialog : MessageDialog
{
    ConfigsOverwriteConfirmDialog(wxWindow *parent, wxString name, bool exported)
        : MessageDialog(parent,
                        wxString::Format(exported ? _L("A file exists with the same name: %s. Do you want to overwrite it\?") :
                                                  _L("A config exists with the same name: %s. Do you want to overwrite it\?"),
                                         name),
                        exported ? _L("Overwrite file") : _L("Overwrite config"),
                        wxYES_NO | wxNO_DEFAULT)
    {
        add_button(wxID_YESTOALL, false, _L("Yes to All"));
        add_button(wxID_NOTOALL, false, _L("No to All"));
    }
};

void MainFrame::export_config()
{
    ExportConfigsDialog export_configs_dlg(nullptr);
    export_configs_dlg.ShowModal();
    return;

    // Generate a cummulative configuration for the selected print, filaments and printer.
    wxDirDialog dlg(this, _L("Choose a directory"),
        from_u8(!m_last_config.IsEmpty() ? get_dir_name(m_last_config) : wxGetApp().app_config->get_last_dir()), wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    wxString path;
    if (dlg.ShowModal() == wxID_OK)
        path = dlg.GetPath();
    if (!path.IsEmpty()) {
        // Export the config bundle.
        wxGetApp().app_config->update_config_dir(into_u8(path));
        try {
            auto files = wxGetApp().preset_bundle->export_current_configs(into_u8(path), [this](std::string const & name) {
                    ConfigsOverwriteConfirmDialog dlg(this, from_u8(name), true);
                    int res = dlg.ShowModal();
                    int ids[]{wxID_NO, wxID_YES, wxID_NOTOALL, wxID_YESTOALL};
                    return std::find(ids, ids + 4, res) - ids;
            }, false);
            if (!files.empty())
                m_last_config = from_u8(files.back());
            MessageDialog dlg(this, wxString::Format(_L_PLURAL("There is %d config exported. (Only non-system configs)",
                "There are %d configs exported. (Only non-system configs)", files.size()), files.size()),
                              _L("Export Result"), wxOK);
            dlg.ShowModal();
        } catch (const std::exception &ex) {
            show_error(this, ex.what());
        }
    }
}

// Load a config file containing a Print, Filament & Printer preset.
void MainFrame::load_config_file()
{
    //BBS do not load config file
 //   if (!wxGetApp().check_and_save_current_preset_changes(_L("Loading profile file"), "", false))
 //       return;
    wxFileDialog dlg(this, _L("Select profile to load:"),
        !m_last_config.IsEmpty() ? get_dir_name(m_last_config) : wxGetApp().app_config->get_last_dir(),
        "config.json", _L("Config files (*.json;*.zip;*.orca_printer;*.orca_bundle;*.orca_filament)|*.json;*.zip;*.orca_printer;*.orca_bundle;*.orca_filament"), wxFD_OPEN | wxFD_MULTIPLE | wxFD_FILE_MUST_EXIST);
     wxArrayString files;
    if (dlg.ShowModal() != wxID_OK)
        return;
    dlg.GetPaths(files);
    std::vector<std::string> cfiles;
    for (auto file : files) {
        cfiles.push_back(into_u8(file));
        m_last_config = file;
    }
    bool update = false;
    wxGetApp().preset_bundle->import_presets(cfiles, [this](std::string const & name) {
            ConfigsOverwriteConfirmDialog dlg(this, from_u8(name), false);
            int           res = dlg.ShowModal();
            int           ids[]{wxID_NO, wxID_YES, wxID_NOTOALL, wxID_YESTOALL};
            return std::find(ids, ids + 4, res) - ids;
        },
        ForwardCompatibilitySubstitutionRule::Enable,
        *wxGetApp().app_config);
    if (!cfiles.empty()) {
        wxGetApp().app_config->update_config_dir(get_dir_name(cfiles.back()));
        wxGetApp().load_current_presets();
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " presets has been import,and size is" << cfiles.size();
        NetworkAgent* agent = wxGetApp().getAgent();
        if (agent) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " user is: " << agent->get_user_id();
        }
    }
    wxGetApp().preset_bundle->update_compatible(PresetSelectCompatibleType::Always);
    update_side_preset_ui();
    auto msg = wxString::Format(_L_PLURAL("There is %d config imported. (Only non-system and compatible configs)",
        "There are %d configs imported. (Only non-system and compatible configs)", cfiles.size()), cfiles.size());
    if(cfiles.empty())
        msg += _L("\nHint: Make sure you have added the corresponding printer before importing the configs.");
    MessageDialog dlg2(this,msg ,
                        _L("Import result"), wxOK);
    dlg2.ShowModal();
}

// Load a config file containing a Print, Filament & Printer preset from command line.
bool MainFrame::load_config_file(const std::string &path)
{
    try {
        ConfigSubstitutions config_substitutions = wxGetApp().preset_bundle->load_config_file(path, ForwardCompatibilitySubstitutionRule::Enable);
        if (!config_substitutions.empty())
            show_substitutions_info(config_substitutions, path);
    } catch (const std::exception &ex) {
        show_error(this, ex.what());
        return false;
    }
    wxGetApp().load_current_presets();
    if (Plater *plater = wxGetApp().plater())
        plater->normalize_bed_types(false);
    return true;
}

//BBS: export current config bundle as BBL default reference
//void MainFrame::export_current_configbundle()
//{
    // BBS do not export profile
   // if (!wxGetApp().check_and_save_current_preset_changes(_L("Exporting current profile bundle"),
   //     _L("Some presets are modified and the unsaved changes will not be exported into profile bundle."), false, true))
   //     return;

   // // validate current configuration in case it's dirty
   // auto err = wxGetApp().preset_bundle->full_config().validate();
   // if (! err.empty()) {
   //     show_error(this, err);
   //     return;
   // }
   // // Ask user for a file name.
   // wxFileDialog dlg(this, _L("Save BBL Default bundle as:"),
   //     !m_last_config.IsEmpty() ? get_dir_name(m_last_config) : wxGetApp().app_config->get_last_dir(),
   //     "BBL_config_bundle.ini",
   //     file_wildcards(FT_INI), wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
   // wxString file;
   // if (dlg.ShowModal() == wxID_OK)
   //     file = dlg.GetPath();
   // if (!file.IsEmpty()) {
   //     // Export the config bundle.
   //     wxGetApp().app_config->update_config_dir(get_dir_name(file));
   //     try {
   //         wxGetApp().preset_bundle->export_current_configbundle(file.ToUTF8().data());
   //     } catch (const std::exception &ex) {
			//show_error(this, ex.what());
   //     }
   // }
//}

//BBS: export all the system preset configs to seperate files
/*void MainFrame::export_system_configs()
{
    // Ask user for a file name.
    wxDirDialog dlg(this, _L("choose a directory"),
        !m_last_config.IsEmpty() ? get_dir_name(m_last_config) : wxGetApp().app_config->get_last_dir(), wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    wxString path;
    if (dlg.ShowModal() == wxID_OK)
        path = dlg.GetPath();
    if (!path.IsEmpty()) {
        // Export the config bundle.
        wxGetApp().app_config->update_config_dir(path.ToStdString());
        try {
            wxGetApp().preset_bundle->export_system_configs(path.ToUTF8().data());
        } catch (const std::exception &ex) {
            show_error(this, ex.what());
        }
    }
}*/

//void MainFrame::export_configbundle(bool export_physical_printers /*= false*/)
//{
////    ; //BBS do not export config bundle
//}

// Loading a config bundle with an external file name used to be used
// to auto - install a config bundle on a fresh user account,
// but that behavior was not documented and likely buggy.
//void MainFrame::load_configbundle(wxString file/* = wxEmptyString, const bool reset_user_profile*/)
//{
//    ; //BBS do not import config bundle
//}

// Load a provied DynamicConfig into the Print / Filament / Printer tabs, thus modifying the active preset.
// Also update the plater with the new presets.
void MainFrame::load_config(const DynamicPrintConfig& config)
{
	PrinterTechnology printer_technology = wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology();
	const auto       *opt_printer_technology = config.option<ConfigOptionEnum<PrinterTechnology>>("printer_technology");
	if (opt_printer_technology != nullptr && opt_printer_technology->value != printer_technology) {
		printer_technology = opt_printer_technology->value;
		this->plater()->set_printer_technology(printer_technology);
	}
#if 0
	for (auto tab : wxGetApp().tabs_list)
		if (tab->supports_printer_technology(printer_technology)) {
			if (tab->type() == Slic3r::Preset::TYPE_PRINTER)
				static_cast<TabPrinter*>(tab)->update_pages();
			tab->load_config(config);
		}
    if (m_plater)
        m_plater->on_config_change(config);
#else
	// Load the currently selected preset into the GUI, update the preset selection box.
    //FIXME this is not quite safe for multi-extruder printers,
    // as the number of extruders is not adjusted for the vector values.
    // (see PresetBundle::update_multi_material_filament_presets())
    // Better to call PresetBundle::load_config() instead?
    for (auto tab : wxGetApp().tabs_list)
        if (tab->supports_printer_technology(printer_technology)) {
            // Only apply keys, which are present in the tab's config. Ignore the other keys.
			for (const std::string &opt_key : tab->get_config()->diff(config))
				// Ignore print_settings_id, printer_settings_id, filament_settings_id etc.
				if (! boost::algorithm::ends_with(opt_key, "_settings_id"))
					tab->get_config()->option(opt_key)->set(config.option(opt_key));
        }

    wxGetApp().load_current_presets();
#endif
}

//BBS: GUI refactor





//BBS GUI refactor: remove unused layout new/dlg


void MainFrame::request_select_tab(const wxString& id)
{
    wxCommandEvent* evt = new wxCommandEvent(EVT_SELECT_TAB);
    evt->SetString(id);
    wxQueueEvent(this, evt);
}

int MainFrame::get_calibration_curr_tab() {
    if (CalibrationPanel* calibration = CalibrationPanel::if_built())
        return calibration->get_tabpanel()->GetSelection();
    return -1;
}

// Set a camera direction, zoom to all objects.
void MainFrame::select_view(const std::string& direction)
{
     if (m_plater)
         m_plater->select_view(direction);
}

// #ys_FIXME_to_delete
void MainFrame::on_presets_changed(SimpleEvent &event)
{
    auto *tab = dynamic_cast<Tab*>(event.GetEventObject());
    wxASSERT(tab != nullptr);
    if (tab == nullptr) {
        return;
    }

    // Update preset combo boxes(Print settings, Filament, Material, Printer) from their respective tabs.
    auto presets = tab->get_presets();
    if (m_plater != nullptr && presets != nullptr) {

        // FIXME: The preset type really should be a property of Tab instead
        Slic3r::Preset::Type preset_type = tab->type();
        if (preset_type == Slic3r::Preset::TYPE_INVALID) {
            wxASSERT(false);
            return;
        }

        m_plater->on_config_change(*tab->get_config());

        m_plater->sidebar().update_presets(preset_type);
    }
}

// #ys_FIXME_to_delete
void MainFrame::on_value_changed(wxCommandEvent& event)
{
    auto *tab = dynamic_cast<Tab*>(event.GetEventObject());
    wxASSERT(tab != nullptr);
    if (tab == nullptr)
        return;

    auto opt_key = event.GetString();
    if (m_plater) {
        m_plater->on_config_change(*tab->get_config()); // propagate config change events to the plater
        if (opt_key == "extruders_count") {
            auto value = event.GetInt();
            m_plater->on_filament_count_change(value);
        }
    }
}

void MainFrame::on_config_changed(DynamicPrintConfig* config) const
{
    if (m_plater)
        m_plater->on_config_change(*config); // propagate config change events to the plater
}

void MainFrame::set_print_button_to_default(PrintSelectType select_type)
{
    // Orca: keep the user's remembered print/export action instead of resetting it to the computed
    // default. get_remembered_print_select() already rejects anything this printer does not offer.
    PrintSelectType remembered;
    if (get_remembered_print_select(remembered))
        select_type = remembered;

    if (select_type == eUploadGcode)
        return; // unsupported: no dropdown entry exists for this action

    m_print_btn->SetLabel(print_select_type_label(select_type));
    m_print_select = select_type;
    // get_enable_print_status() already applies can_send_gcode() to the actions that need it
    if (m_print_enable)
        m_print_enable = get_enable_print_status();
    m_print_btn->Enable(m_print_enable);
    this->Layout();
}

void MainFrame::add_to_recent_projects(const wxString& filename)
{
    if (wxFileExists(filename))
    {
        m_recent_projects.AddFileToHistory(filename);
        std::vector<std::string> recent_projects;
        size_t count = m_recent_projects.GetCount();
        for (size_t i = 0; i < count; ++i)
        {
            recent_projects.push_back(into_u8(m_recent_projects.GetHistoryFile(i)));
        }
        wxGetApp().app_config->set_recent_projects(recent_projects);
        if (WebViewPanel* home = WebViewPanel::if_built())
            home->SendRecentList(0);
    }
}

std::wstring MainFrame::FileHistory::GetThumbnailUrl(int index) const
{
    if (m_thumbnails[index].empty()) return L"";
    std::wstringstream wss;
    wss << L"data:image/png;base64,";
    wss << wxBase64Encode(m_thumbnails[index].data(), m_thumbnails[index].size());
    return wss.str();
}

bool MainFrame::FileHistory::GetPublished(int index) const
{
    return index >= 0 && index < static_cast<int>(m_published_files.size()) && m_published_files[index];
}

void MainFrame::FileHistory::AddFileToHistory(const wxString &file)
{
    if (this->m_fileMaxFiles == 0)
        return;
    wxFileHistory::AddFileToHistory(file);
    if (m_load_called) {
        m_thumbnails.push_front(bbs_3mf_get_thumbnail(into_u8(file).c_str()));
        m_published_files.push_front(bbs_3mf_is_published(into_u8(file)));
    } else {
        m_thumbnails.push_front("");
        m_published_files.push_front(false);
    }
}

void MainFrame::FileHistory::RemoveFileFromHistory(size_t i)
{
    if (i >= m_thumbnails.size()) // FIX zero max
        return;
    wxFileHistory::RemoveFileFromHistory(i);
    m_thumbnails.erase(m_thumbnails.begin() + i);
    m_published_files.erase(m_published_files.begin() + i);
}

size_t MainFrame::FileHistory::FindFileInHistory(const wxString & file)
{
    return m_fileHistory.Index(file);
}

void MainFrame::FileHistory::LoadThumbnails()
{
    tbb::parallel_for(tbb::blocked_range<size_t>(0, GetCount()), [this](tbb::blocked_range<size_t> range) {
        for (size_t i = range.begin(); i < range.end(); ++i) {
            auto thumbnail = bbs_3mf_get_thumbnail(into_u8(GetHistoryFile(i)).c_str());
            if (!thumbnail.empty()) {
                m_thumbnails[i] = thumbnail;
            }
            m_published_files[i] = bbs_3mf_is_published(into_u8(GetHistoryFile(i)));
        }
    });
    m_load_called = true;
}

inline void MainFrame::FileHistory::SetMaxFiles(int max)
{
    m_fileMaxFiles  = max;
    size_t numFiles = m_fileHistory.size();
    while (numFiles > m_fileMaxFiles)
        RemoveFileFromHistory(--numFiles);
}

void MainFrame::get_recent_projects(boost::property_tree::wptree &tree, int images)
{
    for (size_t i = 0; i < m_recent_projects.GetCount(); ++i) {
        boost::property_tree::wptree item;
        std::wstring proj = m_recent_projects.GetHistoryFile(i).ToStdWstring();
        item.put(L"project_name", proj.substr(proj.find_last_of(L"/\\") + 1));
        item.put(L"path", proj);
        item.put(L"published", m_recent_projects.GetPublished(i) ? L"1" : L"0");
        boost::system::error_code ec;
        std::time_t t = boost::filesystem::last_write_time(proj, ec);
        if (!ec) {
            std::wstring time = wxDateTime(t).FormatISOCombined(' ').ToStdWstring();
            item.put(L"time", time);
            if (i <= images) {
                auto thumbnail = m_recent_projects.GetThumbnailUrl(i);
                if (!thumbnail.empty()) item.put(L"image", thumbnail);
            }
        } else {
            item.put(L"time", _L("File is missing"));
        }
        tree.push_back({L"", item});
    }
}

void MainFrame::open_recent_project(size_t file_id, wxString const & filename)
{
    if (file_id == size_t(-1)) {
        file_id = m_recent_projects.FindFileInHistory(filename);
    }
    if (wxFileExists(filename)) {
        CallAfter([this, filename] {
            if (wxGetApp().can_load_project())
                m_plater->load_project(filename);
        });
    }
    else
    {
        MessageDialog msg(this, _L("The project is no longer available."), _L("Error"), wxOK | wxYES_DEFAULT);
        if (msg.ShowModal() == wxID_YES)
        {
            m_recent_projects.RemoveFileFromHistory(file_id);
            std::vector<std::string> recent_projects;
            size_t count = m_recent_projects.GetCount();
            for (size_t i = 0; i < count; ++i)
            {
                recent_projects.push_back(into_u8(m_recent_projects.GetHistoryFile(i)));
            }
            wxGetApp().app_config->set_recent_projects(recent_projects);
            if (WebViewPanel* home = WebViewPanel::if_built())
                home->SendRecentList(-1);
        }
    }
}

void MainFrame::remove_recent_project(size_t file_id, wxString const &filename)
{
    if (file_id == size_t(-1)) {
        if (filename.IsEmpty())
            while (m_recent_projects.GetCount() > 0)
                m_recent_projects.RemoveFileFromHistory(0);
        else
            file_id = m_recent_projects.FindFileInHistory(filename);
    }
    if (file_id != size_t(-1))
        m_recent_projects.RemoveFileFromHistory(file_id);
    std::vector<std::string> recent_projects;
    size_t count = m_recent_projects.GetCount();
    for (size_t i = 0; i < count; ++i)
    {
        recent_projects.push_back(into_u8(m_recent_projects.GetHistoryFile(i)));
    }
    wxGetApp().app_config->set_recent_projects(recent_projects);
    if (WebViewPanel* home = WebViewPanel::if_built())
        home->SendRecentList(-1);
}

void MainFrame::load_url(wxString url)
{
    BOOST_LOG_TRIVIAL(trace) << "load_url:" << url;
    auto evt = new wxCommandEvent(EVT_LOAD_URL, this->GetId());
    evt->SetString(url);
    wxQueueEvent(this, evt);
}

void MainFrame::load_printer_url(wxString url, wxString apikey)
{
    BOOST_LOG_TRIVIAL(trace) << "load_printer_url:" << url;
    auto evt = new LoadPrinterViewEvent(EVT_LOAD_PRINTER_URL, this->GetId());
    evt->SetString(url);
    evt->SetAPIkey(apikey);
    wxQueueEvent(this, evt);
}

void MainFrame::load_printer_url()
{
    PresetBundle &preset_bundle = *wxGetApp().preset_bundle;
    if (preset_bundle.use_bbl_device_tab() && !wxGetApp().app_config->get_bool("use_printer_agents"))
        return;

    auto     cfg = preset_bundle.printers.get_edited_preset().config;
    if (cfg.opt_string("print_host").empty()) {
        if (auto *device_manager = wxGetApp().getDeviceManager()) {
            auto *machine = device_manager->get_selected_machine();
            if (!machine) {
                auto machines = device_manager->get_my_machine_list(device_manager->get_current_printer_agent_id());
                if (machines.size() == 1)
                    machine = machines.begin()->second;
            }
            if (machine && !machine->get_dev_ip().empty())
                cfg.opt_string("print_host") = machine->get_dev_ip();
        }
    }
    wxString url = from_u8(PrintHost::get_print_host_webui(&cfg));
    wxString apikey;
    const auto host_type = cfg.option<ConfigOptionEnum<PrintHostType>>("host_type")->value;
    if (cfg.has("printhost_apikey") && host_type != htSimplyPrint)
        apikey = cfg.opt_string("printhost_apikey");
    if (!url.empty()) {
        load_printer_url(url, apikey);
    }
}

bool MainFrame::is_printer_view() const { return selected_tab_id() == TAB_ID_MONITOR; }


void MainFrame::refresh_plugin_tips()
{
    if (WebViewPanel* home = WebViewPanel::if_built())
        home->ShowNetpluginTip();
}

void MainFrame::RunScript(wxString js)
{
    if (WebViewPanel* home = WebViewPanel::if_built())
        home->RunScript(js);
}

void MainFrame::technology_changed()
{
    // update menu titles
    PrinterTechnology pt = plater()->printer_technology();
    if (int id = m_menubar->FindMenu(pt == ptFFF ? _omitL("Material Settings") : _L("Filament settings")); id != wxNOT_FOUND)
        m_menubar->SetMenuLabel(id, pt == ptSLA ? _omitL("Material Settings") : _L("Filament settings"));
}

// Opens the calibration wizard for `calib_kind`. Single source of truth for the wizard lifecycle:
// the Calibration menu handlers and the Speed Dial native commands both call it. Most wizards are
// cached members reused across launches; cornering/input-shaping build a fresh transient dialog.
// Call while the Prepare (3D) panel is shown.
void MainFrame::run_calibration(CalibKind calib_kind)
{
    switch (calib_kind) {
    case CalibKind::Temperature: {
        if (!m_temp_calib_dlg)
            m_temp_calib_dlg = new Temp_Calibration_Dlg((wxWindow*) this, wxID_ANY, m_plater);
        m_temp_calib_dlg->ShowModal();
        break;
    }
    case CalibKind::MaxVolumetric: {
        if (!m_vol_test_dlg)
            m_vol_test_dlg = new MaxVolumetricSpeed_Test_Dlg((wxWindow*) this, wxID_ANY, m_plater);
        m_vol_test_dlg->ShowModal();
        break;
    }
    case CalibKind::PressureAdvance: {
        if (!m_pa_calib_dlg)
            m_pa_calib_dlg = new PA_Calibration_Dlg((wxWindow*) this, wxID_ANY, m_plater);
        m_pa_calib_dlg->ShowModal();
        break;
    }
    case CalibKind::FlowRatio: {
        if (!m_plater)
            break;
        if (!m_flow_rate_calib_dlg)
            m_flow_rate_calib_dlg = new FlowRateCalibrationDialog((wxWindow*) this, wxID_ANY, m_plater);
        m_flow_rate_calib_dlg->ShowModal();
        break;
    }
    case CalibKind::Retraction: {
        if (!m_retraction_calib_dlg)
            m_retraction_calib_dlg = new Retraction_Test_Dlg((wxWindow*) this, wxID_ANY, m_plater);
        m_retraction_calib_dlg->ShowModal();
        break;
    }
    case CalibKind::Cornering: {
        auto dlg = new Cornering_Test_Dlg((wxWindow*) this, wxID_ANY, m_plater);
        dlg->ShowModal();
        dlg->Destroy();
        break;
    }
    case CalibKind::InputShapingFreq: {
        auto dlg = new Input_Shaping_Freq_Test_Dlg((wxWindow*) this, wxID_ANY, m_plater);
        dlg->ShowModal();
        dlg->Destroy();
        break;
    }
    case CalibKind::InputShapingDamp: {
        auto dlg = new Input_Shaping_Damp_Test_Dlg((wxWindow*) this, wxID_ANY, m_plater);
        dlg->ShowModal();
        dlg->Destroy();
        break;
    }
    case CalibKind::VFA: {
        if (!m_vfa_test_dlg)
            m_vfa_test_dlg = new VFA_Test_Dlg((wxWindow*) this, wxID_ANY, m_plater);
        m_vfa_test_dlg->ShowModal();
        break;
    }
    }
}


//
// Called after the Preferences dialog is closed and the program settings are saved.
// Update the UI based on the current preferences.
void MainFrame::update_ui_from_settings()
{
    if (m_plater)
        m_plater->update_ui_from_settings();
    for (auto tab: wxGetApp().tabs_list)
        tab->update_ui_from_settings();
}


void MainFrame::show_sync_dialog()
{
    SimpleEvent* evt = new SimpleEvent(EVT_SYNC_CLOUD_PRESET);
    wxQueueEvent(this, evt);
}

void MainFrame::update_side_preset_ui()
{
    // select last preset
    for (auto tab : wxGetApp().tabs_list) {
        tab->update_tab_ui();
    }

    //BBS: update the preset
    m_plater->sidebar().update_presets(Preset::TYPE_PRINTER);
    m_plater->sidebar().update_presets(Preset::TYPE_FILAMENT);


    //take off multi machine
    if (MultiMachinePage* multi_machine = MultiMachinePage::if_built())
        multi_machine->clear_page();
}

void MainFrame::on_select_default_preset(SimpleEvent& evt)
{
    MessageDialog dialog(this,
                    _L("Do you want to synchronize your personal data from Orca Cloud?\n"
                        "It contains the following information:\n"
                        "1. The Process presets\n"
                        "2. The Filament presets\n"
                        "3. The Printer presets"),
                    _L("Synchronization"),
                    wxCENTER |
                    wxYES_DEFAULT | wxYES_NO |
                    wxICON_INFORMATION);

    /* get setting list */
    NetworkAgent* agent = wxGetApp().getAgent();
    switch ( dialog.ShowModal() )
    {
        case wxID_YES: {
            wxGetApp().app_config->set_bool("sync_user_preset", true);
            wxGetApp().start_sync_user_preset(true);
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " sync_user_preset: true";
            break;
        }
        case wxID_NO:
            wxGetApp().app_config->set_bool("sync_user_preset", false);
            wxGetApp().stop_sync_user_preset();
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " sync_user_preset: false";
            break;
        default:
            break;
    }

    update_side_preset_ui();
}

std::string MainFrame::get_base_name(const wxString &full_name, const char *extension) const
{
    boost::filesystem::path filename = boost::filesystem::path(full_name.wx_str()).filename();
    if (extension != nullptr)
		filename = filename.replace_extension(extension);
    return filename.string();
}

std::string MainFrame::get_dir_name(const wxString &full_name) const
{
    return boost::filesystem::path(into_u8(full_name)).parent_path().string();
}


// ----------------------------------------------------------------------------
// SettingsDialog
// ----------------------------------------------------------------------------

SettingsDialog::SettingsDialog(MainFrame* mainframe)
:DPIDialog(NULL, wxID_ANY, wxString(SLIC3R_APP_NAME) + " - " + _L("Settings"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_FRAME_STYLE, "settings_dialog")
//: DPIDialog(mainframe, wxID_ANY, wxString(SLIC3R_APP_NAME) + " - " + _L("Settings"), wxDefaultPosition, wxDefaultSize,
//        wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER | wxMINIMIZE_BOX | wxMAXIMIZE_BOX, "settings_dialog"),
{
    if (wxGetApp().is_gcode_viewer())
        return;

#if defined(__WXMSW__)
    // ys_FIXME! temporary workaround for correct font scaling
    // Because of from wxWidgets 3.1.3 auto rescaling is implemented for the Fonts,
    // From the very beginning set dialog font to the wxSYS_DEFAULT_GUI_FONT
    this->SetFont(wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT));
#else
    this->SetFont(wxGetApp().normal_font());
    this->SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
#endif // __WXMSW__

    // Load the icon either from the exe, or from the ico file.
#if _WIN32
    {
        TCHAR szExeFileName[MAX_PATH];
        GetModuleFileName(nullptr, szExeFileName, MAX_PATH);
        SetIcon(wxIcon(szExeFileName, wxBITMAP_TYPE_ICO));
    }
#else
    SetIcon(wxIcon(var("OrcaSlicer_128px.png"), wxBITMAP_TYPE_PNG));
#endif // _WIN32

    //just hide the Frame on closing
    this->Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& evt) { this->Hide(); });

#ifdef _MSW_DARK_MODE
    if (wxGetApp().tabs_as_menu()) {
        // menubar
        //m_menubar = new wxMenuBar();
        //add_tabs_as_menu(m_menubar, mainframe, this);
        //this->SetMenuBar(m_menubar);
    }
#endif

    // initialize layout
    auto sizer = new wxBoxSizer(wxVERTICAL);
    sizer->SetSizeHints(this);
    SetSizer(sizer);
    Fit();

    const wxSize min_size = wxSize(85 * em_unit(), 50 * em_unit());
#ifdef __APPLE__
    // Using SetMinSize() on Mac messes up the window position in some cases
    // cf. https://groups.google.com/forum/#!topic/wx-users/yUKPBBfXWO0
    SetSize(min_size);
#else
    SetMinSize(min_size);
    SetSize(GetMinSize());
#endif
    Layout();
}

void SettingsDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    if (wxGetApp().is_gcode_viewer())
        return;

    const int& em = em_unit();
    const wxSize& size = wxSize(85 * em, 50 * em);

    // BBS
    m_tabpanel->Rescale();

    // update Tabs
    for (auto tab : wxGetApp().tabs_list)
        tab->msw_rescale();

    SetMinSize(size);
    Fit();
    Refresh();
}


#include "MainFrameWorkspace.ipp"

} // GUI
} // Slic3r
