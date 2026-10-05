#include "DesktopWorkspaceNavigation.hpp"
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
struct DeviceMediaSource {
    OrcaDeviceMediaFacts facts;
    std::string device_id;
};
DeviceMediaSource current_device_media_source()
{
    DeviceMediaSource source;
    auto* bundle = wxGetApp().preset_bundle;
    if (!bundle) return source;
    source.facts.connection.native_backend = bundle->use_bbl_device_tab() &&
        !wxGetApp().app_config->get_bool("use_printer_agents");
    if (!source.facts.connection.native_backend) return source;
    auto* manager = wxGetApp().getDeviceManager();
    auto* machine = manager ? manager->get_selected_machine() : nullptr;
    if (!machine) return source;
    auto& facts = source.facts;
    source.device_id = machine->get_dev_id();
    facts.connection.native_selected = true;
    facts.connection.native_online = machine->is_online();
    facts.connection.native_connected = machine->is_connected();
    if (machine->last_update_time.time_since_epoch().count() > 0)
        facts.connection.telemetry_age = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now() - machine->last_update_time);
    facts.information_ready = machine->is_info_ready() && machine->m_push_count > 0;
    const auto* storage = machine->GetStorage();
    facts.storage_readable = storage && storage->get_sdcard_state() == DevStorage::HAS_SDCARD_NORMAL;
    facts.file_protocol_supported = machine->file_local || (!machine->is_lan_mode_printer() && machine->get_file_remote());
    facts.busy = machine->is_camera_busy_off();
    return source;
}

wxString device_media_reason(OrcaDeviceMediaState state)
{
    switch (state) {
    case OrcaDeviceMediaState::Available: return _L("可查看所选设备存储中的已有视频。列表和下载由设备存储页处理。");
    case OrcaDeviceMediaState::InformationPending: return _L("设备信息尚未就绪。请先到设备页核实状态，再重新打开。");
    case OrcaDeviceMediaState::Unsupported: return _L("当前设备或连接模式没有可用的文件浏览协议。请核对固件与连接方式。");
    case OrcaDeviceMediaState::StorageUnavailable: return _L("当前设备没有可读取的正常存储。请到设备页核对存储状态。");
    case OrcaDeviceMediaState::Busy: return _L("设备媒体正在忙碌。请等待设备释放后重新打开。");
    default: return _L("尚未取得所选设备的有效连接信息。配置并连接设备后，再查看已有媒体。");
    }
}

enum class MediaDestination { None, DeviceStorage, Connection, Prepare, CapturePrepare };
MediaDestination choose_print_media_source(wxWindow* parent)
{
    using namespace ModelGenerationInputStyle;
    const auto original = current_device_media_source();
    const auto state = orca_device_media_state(original.facts);
    wxDialog dialog(wxGetTopLevelParent(parent), wxID_ANY, _L("视频与截图 · 来源"), wxDefaultPosition,
                    wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    auto* root = new wxBoxSizer(wxVERTICAL);
    auto* content = new wxScrolledWindow(&dialog, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    content->SetScrollRate(0, dialog.FromDIP(16));
    content->SetMinSize(wxSize(1, 1));
    auto* column = new wxBoxSizer(wxVERTICAL);
    auto text = [&](const wxString& value, const char* role) {
        auto* label = new Label(content, value, LB_AUTO_WRAP);
        label->SetName(role); label->SetMinSize(wxSize(1, -1));
        column->Add(label, 0, wxEXPAND | wxALL, dialog.FromDIP(12));
        return label;
    };
    auto action = [&](wxWindow* owner, wxSizer* sizer, const wxString& label, std::function<void()> callback) {
        auto* button = new Button(owner, label);
        button->SetName("input_field"); button->SetPaddingSize(dialog.FromDIP(wxSize(14, 10)));
        sizer->Add(button, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, dialog.FromDIP(12));
        button->Bind(wxEVT_BUTTON, [callback = std::move(callback)](wxCommandEvent&) { callback(); });
        return button;
    };
    MediaDestination destination = MediaDestination::None;
    text(_L("视频与截图"), "input_accent");
    text(_L("进入此面板不会截图、录制或分享。媒体来源与当前切片预览分别显示。"), "input_secondary");
    text(_L("设备已有媒体"), "");
    auto* status = text(device_media_reason(state), "input_secondary");
    auto* browse = action(content, column, _L("查看设备已有媒体"), [&] {
        const auto current = current_device_media_source();
        if (current.device_id != original.device_id || orca_device_media_state(current.facts) != OrcaDeviceMediaState::Available) {
            status->SetLabel(_L("设备或连接状态已变化。请返回后重新核对媒体来源。"));
            content->Layout(); content->FitInside(); return;
        }
        destination = MediaDestination::DeviceStorage;
        dialog.EndModal(wxID_OK);
    });
    browse->Enable(state == OrcaDeviceMediaState::Available);
    browse->EnableTooltipEvenDisabled();
    browse->SetToolTip(device_media_reason(state));
    action(content, column, _L("查看设备与连接"), [&] { destination = MediaDestination::Connection; dialog.EndModal(wxID_OK); });
    text(_L("设备即时截图 · 尚未接入"), "");
    text(_L("需要真实相机画面与设备截图能力。已有录制文件不表示可以即时截图。"), "input_secondary");
    text(_L("软件视口截图"), "");
    text(_L("软件画面不是打印机照片。截图前可返回工程调整视角；截图会先预览，再选择导出位置。"), "input_secondary");
    action(content, column, _L("前往工程视口"), [&] { destination = MediaDestination::Prepare; dialog.EndModal(wxID_OK); });
    action(content, column, _L("截图工程视口…"), [&] { destination = MediaDestination::CapturePrepare; dialog.EndModal(wxID_OK); });
    text(_L("客户端录制 · 当前不可用"), "");
    text(_L("尚未提供新的客户端录制与导出，不会自动创建视频或示例文件。"), "input_secondary");
    content->SetSizer(column);
    content->Bind(wxEVT_SIZE, [content](wxSizeEvent& event) { content->FitInside(); event.Skip(); });
    root->Add(content, 1, wxEXPAND);
    auto* back = action(&dialog, root, _L("返回打印中心"), [&] { dialog.EndModal(wxID_CANCEL); });
    dialog.SetSizer(root);
    dialog.Bind(wxEVT_CHAR_HOOK, [&](wxKeyEvent& event) {
        if (event.GetKeyCode() == WXK_ESCAPE) dialog.EndModal(wxID_CANCEL);
        else event.Skip();
    });
    apply(&dialog, true);
    apply_control(back, Role::QuietAction, true);
    dialog.SetMinSize(dialog.FromDIP(wxSize(420, 300)));
    dialog.SetSize(dialog.FromDIP(wxSize(520, 480)));
    dialog.CentreOnParent();
    dialog.ShowModal();
    parent->SetFocus();
    return destination;

}

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
    m_body = navigation;
    auto* root = new wxBoxSizer(wxVERTICAL);
    root->Add(navigation, 1, wxEXPAND);
    SetSizer(root);
    build_print_center();
    auto* column = new wxBoxSizer(wxVERTICAL);
    column->Add(new wxStaticBitmap(navigation, wxID_ANY,
        create_scaled_bitmap("figma-ux/logo", navigation, 48)), 0,
        wxALIGN_CENTER_HORIZONTAL | wxTOP | wxBOTTOM, FromDIP(20));
    struct Entry { const char* icon; wxString label; DesktopDestination destination; };
    const std::array<Entry, 4> entries {{{"library", _L("资产"), DesktopDestination::Library},
        {"image", _L("图像"), DesktopDestination::Image},
        {"object", _L("3D模型"), DesktopDestination::Model},
        {"printer", _L("打印"), DesktopDestination::Print}}};
    for (const auto& entry : entries) {
        auto* button = new Button(navigation, entry.label, wxString("figma-ux/") + entry.icon, wxBORDER_NONE, 24);
        button->SetVertical();
        button->SetPaddingSize(FromDIP(wxSize(4, 8)));
        button->SetMinSize(FromDIP(wxSize(76, 70)));
        button->SetName("input_navigation");
        button->EnableTooltipEvenDisabled();
        column->Add(button, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
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
        auto* button = new Button(navigation, entry.first, "", wxBORDER_NONE);
        button->SetPaddingSize(FromDIP(wxSize(4, 4)));
        button->SetMinSize(FromDIP(wxSize(76, 36)));
        button->SetName("input_navigation");
        column->Add(button, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
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
    using namespace ModelGenerationInputStyle;
    // Native state remains authoritative; this view owns no device commands or GL.
    m_print_refresh.SetOwner(this);
    m_print_center = new Surface(m_tabpanel);
    m_print_center->SetMinSize(wxSize(1, 1));
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    m_print_sidebar = new RoundedPanel(m_print_center);
    m_print_sidebar->SetMinSize(FromDIP(wxSize(331, 1)));
    auto* left = new wxBoxSizer(wxVERTICAL);
    m_print_details = new wxScrolledWindow(m_print_sidebar, wxID_ANY, wxDefaultPosition,
                                          wxDefaultSize, wxVSCROLL);
    m_print_details->SetMinSize(wxSize(1, 1));
    m_print_details->SetScrollRate(0, FromDIP(16));
    auto* details = new wxBoxSizer(wxVERTICAL);
    auto add_text = [&](wxWindow* parent, wxBoxSizer* column, const wxString& text, const char* role = "") {
        auto* label = new Label(parent, text, LB_AUTO_WRAP);
        label->SetName(role);
        label->SetMinSize(wxSize(1, -1));
        column->Add(label, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
        return label;
    };
    details->AddSpacer(FromDIP(16));
    add_text(m_print_details, details, _L("打印机"));
    m_print_preset = add_text(m_print_details, details, wxEmptyString, "input_field");
    m_print_connection = add_text(m_print_details, details, wxEmptyString, "input_secondary");
    auto add_action = [&](wxWindow* parent, wxSizer* column, const wxString& text,
                          bool primary, std::function<void()> action) {
        auto* button = new Button(parent, text);
        button->SetName(primary ? "input_primary" : "input_field");
        button->SetPaddingSize(FromDIP(wxSize(14, 10)));
        column->Add(button, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
        button->Bind(wxEVT_BUTTON, [action = std::move(action)](wxCommandEvent&) { action(); });
        return button;
    };
    add_action(m_print_details, details, _L("连接设置…"), false, [this] {
        navigate_workspace_page(TAB_ID_PREPARE);
        if (auto* plater = wxGetApp().plater()) plater->show_smart_slicing(false);
    });
    add_text(m_print_details, details, _L("关键信息"));
    m_print_job = add_text(m_print_details, details, wxEmptyString, "input_field");
    add_text(m_print_details, details,
        _L("打印前调平 · 待设备确认\n摄像头录制 · 待设备确认\nAI 失败预警 · 待设备确认"), "input_secondary");
    add_text(m_print_details, details, _L("视频与截图"));
    add_text(m_print_details, details, _L("设备文件、相机截图与软件视口分别标明来源。"), "input_secondary");
    add_action(m_print_details, details, _L("查看视频与截图…"), false, [this] {
        const auto destination = choose_print_media_source(m_print_center);
        if (destination == MediaDestination::CapturePrepare) {
            navigate_workspace_page(TAB_ID_PREPARE);
            CallAfter([this] {
                if (m_tabpanel->GetSelectedPageName() == TAB_ID_PREPARE) show_software_viewport_capture(this);
            });
        }
        else if (destination == MediaDestination::Prepare) navigate_workspace_page(TAB_ID_PREPARE);
        else if (destination == MediaDestination::Connection) navigate_workspace_page(TAB_ID_MONITOR);
        else if (destination == MediaDestination::DeviceStorage) {
            const auto source = current_device_media_source();
            if (orca_device_media_state(source.facts) != OrcaDeviceMediaState::Available) return;
            navigate_workspace_page(TAB_ID_MONITOR);
            if (m_tabpanel->GetSelectedPageName() == TAB_ID_MONITOR) {
                if (auto* monitor = wxGetApp().mainframe->m_monitor)
                    monitor->get_tabpanel()->SetSelection(MonitorPanel::PT_MEDIA);
            }
        }
        refresh_print_center();
    });
    m_print_details->SetSizer(details);
    m_print_details->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        m_print_details->FitInside(); event.Skip();
    });
    left->Add(m_print_details, 1, wxEXPAND);
    m_print_action_hint = add_text(m_print_sidebar, left, wxEmptyString, "input_secondary");
    m_print_review = add_action(m_print_sidebar, left, _L("继续到打印确认…"), true, [this] {
        if (auto* plater = wxGetApp().plater()) show_orca_print_confirmation(m_print_center, *plater);
        refresh_print_center();
    });
    m_print_review->EnableTooltipEvenDisabled();
    m_print_sidebar->SetSizer(left);
    row->Add(m_print_sidebar, 0, wxEXPAND | wxALL, FromDIP(12));

    auto* workspace = new RoundedPanel(m_print_center);
    workspace->SetMinSize(wxSize(1, 1));
    auto* body = new wxBoxSizer(wxVERTICAL);
    body->AddSpacer(FromDIP(24));
    add_text(workspace, body, _L("当前打印作业"), "input_accent");
    m_print_status = add_text(workspace, body, wxEmptyString);
    add_text(workspace, body,
        _L("设备在线状态与当前工程分别核对。进入此页不会上传或开始打印；相机、耗材与 AI 状态以所选设备实时反馈为准。"),
        "input_secondary");
    body->AddStretchSpacer();
    m_print_preview = new wxStaticBitmap(workspace, wxID_ANY, wxNullBitmap);
    m_print_preview->SetName("ai_content_color");
    m_print_preview->Hide();
    body->Add(m_print_preview, 0, wxALIGN_CENTER_HORIZONTAL | wxALL, FromDIP(12));
    body->AddStretchSpacer();
    add_action(workspace, body, _L("返回工程准备"), false, [this] { navigate_workspace_page(TAB_ID_PREPARE); });
    add_action(workspace, body, _L("查看设备与连接"), false, [this] { navigate_workspace_page(TAB_ID_MONITOR); });
    add_action(workspace, body, _L("查看确认详情…"), false, [this] {
        if (auto* plater = wxGetApp().plater()) show_orca_print_confirmation(m_print_center, *plater);
        refresh_print_center();
    });
    workspace->SetSizer(body);
    row->Add(workspace, 1, wxEXPAND | wxTOP | wxRIGHT | wxBOTTOM, FromDIP(12));
    m_print_center->SetSizer(row);
    m_print_center->Bind(wxEVT_SHOW, [this](wxShowEvent& event) {
        if (event.IsShown()) { refresh_print_center(); m_print_refresh.Start(1000); }
        else m_print_refresh.Stop();
        event.Skip();
    });
    m_print_center->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        const int width = std::clamp(m_print_center->GetClientSize().x / 2, FromDIP(240), FromDIP(331));
        m_print_sidebar->SetMinSize(wxSize(width, 1));
        event.Skip();
    });
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { refresh_print_center(); }, m_print_refresh.GetId());
    m_tabpanel->AddPage(print_center_page, m_print_center, _L("打印"));
    refresh_ai_appearance(m_print_center, true);
}

void DesktopWorkspaceNavigation::refresh_print_center()
{
    if (!m_print_status || !m_print_review) return;
    auto* plater = wxGetApp().plater();
    auto* plate = plater ? plater->get_partplate_list().get_curr_plate() : nullptr;
    auto* bundle = wxGetApp().preset_bundle;
    const bool slicing = plater && plater->is_background_process_slicing();
    bool ready = plate && !slicing && plate->is_slice_result_ready_for_export();
    if (ready) {
        OrcaSmartSlicingAdapter workspace(plater);
        ready = capture_orca_print_confirmation_revision(workspace).has_value();
    }
    auto* result = ready ? plate->get_slice_result() : nullptr;
    OrcaPrintConnectionFacts facts;
    if (bundle) {
        const bool agents = wxGetApp().app_config->get_bool("use_printer_agents");
        facts.native_backend = bundle->use_bbl_device_tab() && !agents;
        const auto* host = bundle->printers.get_edited_preset().config.option<ConfigOptionString>("print_host");
        facts.web_entry_configured = host && !host->value.empty();
    }
    wxString device_name;
    if (facts.native_backend) {
        if (auto* manager = wxGetApp().getDeviceManager()) {
            if (auto* machine = manager->get_selected_machine()) {
                facts.native_selected = true;
                facts.native_online = machine->is_online();
                facts.native_connected = machine->is_connected();
                device_name = from_u8(machine->get_dev_name());
                if (machine->last_update_time.time_since_epoch().count() > 0)
                    facts.telemetry_age = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now() - machine->last_update_time);
            }
        }
    }
    const auto connection = orca_print_connection_state(facts);
    wxString connection_text;
    switch (connection) {
    case OrcaPrintConnectionState::Unconfigured: connection_text = _L("尚未配置设备连接。离线切片与本地导出仍可用。"); break;
    case OrcaPrintConnectionState::SelectionRequired: connection_text = _L("尚未选择设备。请到设备页选择并核实连接。"); break;
    case OrcaPrintConnectionState::Offline: connection_text = device_name + _L(" · 离线，无法核实当前作业。"); break;
    case OrcaPrintConnectionState::Online: connection_text = device_name + _L(" · 已连接。作业与能力请到设备页核实。"); break;
    case OrcaPrintConnectionState::Unknown: connection_text = facts.native_selected ?
        device_name + _L(" · 数据缺失或已过期，当前状态未知。") :
        _L("已配置连接入口，在线与作业状态尚未核实。"); break;
    }
    bool changed = false;
    auto set_text = [&](wxStaticText* label, const wxString& value) {
        if (label->GetLabel() != value) { label->SetLabel(value); changed = true; }
    };
    set_text(m_print_preset, bundle ? from_u8(bundle->printers.get_edited_preset().name) : _L("未选择打印机预设"));
    set_text(m_print_connection, connection_text);
    wxString job = plate ? wxString::Format(_L("打印板 %d"), plate->get_index() + 1) : _L("未选择打印板");
    if (result) {
        const auto minutes = static_cast<long long>(std::llround(result->print_statistics.modes[0].time / 60.0));
        job += wxString::Format(_L("\n预计耗时：%lld 小时 %lld 分钟\n使用耗材：%llu 个\nG-code：当前正式切片，尚未发送"),
            minutes / 60, minutes % 60,
            static_cast<unsigned long long>(result->print_statistics.total_volumes_per_extruder.size()));
    } else job += slicing ? _L("\nG-code：正在切片\n预计耗时：待切片完成") : _L("\nG-code：没有有效正式切片\n预计耗时：尚不可用");
    set_text(m_print_job, job);
    set_text(m_print_status, slicing ? _L("正在切片，完成后可查看当前正式切片预览。") : ready ?
        _L("当前打印板已有正式切片。核对预览后，可确认设备或先导出 G-code；尚未向设备发送。") :
        _L("当前没有可用的正式切片结果。返回工程完成切片；旧文件不能作为当前作业依据。"));
    const wxString hint = ready ? _L("连接缺失或离线时，打印确认仍可本地导出。") : _L("先完成当前工程正式切片，再确认或导出。");
    set_text(m_print_action_hint, hint);
    if (m_print_review->IsEnabled() != ready) {
        m_print_review->Enable(ready);
        ModelGenerationInputStyle::apply_control(m_print_review, ModelGenerationInputStyle::Role::PrimaryAction, false, true);
        changed = true;
    }
    m_print_review->SetToolTip(hint);
    const bool show_preview = ready && plate->thumbnail_data.is_valid();
    const int preview_size = std::clamp(m_print_preview->GetParent()->GetClientSize().y / 4, FromDIP(80), FromDIP(220));
    if (show_preview && (m_print_preview_result != result || m_print_preview_result_id != result->id ||
                         !m_print_preview->GetBitmap().IsOk() || m_print_preview->GetBitmap().GetWidth() != preview_size)) {
        const auto& thumbnail = plate->thumbnail_data;
        wxImage preview(thumbnail.width, thumbnail.height);
        preview.InitAlpha();
        for (unsigned int y = 0; y < thumbnail.height; ++y)
            for (unsigned int x = 0; x < thumbnail.width; ++x) {
                const auto* pixel = thumbnail.pixels.data() + 4 * ((thumbnail.height - 1 - y) * thumbnail.width + x);
                preview.SetRGB(x, y, pixel[0], pixel[1], pixel[2]);
                preview.SetAlpha(x, y, pixel[3]);
            }
        const int size = preview_size;
        wxBitmap bitmap(preview.Scale(size, size, wxIMAGE_QUALITY_HIGH));
        bitmap.SetScaleFactor(GetDPIScaleFactor());
        m_print_preview->SetBitmap(bitmap);
        m_print_preview_result = result;
        m_print_preview_result_id = result->id;
        changed = true;
    }
    if (m_print_preview->IsShown() != show_preview) { m_print_preview->Show(show_preview); changed = true; }
    if (!show_preview) { m_print_preview_result = nullptr; m_print_preview_result_id = 0; }
    if (changed) { m_print_center->Layout(); m_print_details->FitInside(); }
}

void DesktopWorkspaceNavigation::update_compact_layout()
{
    if (!m_body || m_workspace_buttons.size() != 7) return;
    const bool compact = GetClientSize().y < FromDIP(550);
    if (compact == m_compact) return;
    m_compact = compact;
    auto* column = m_body->GetSizer();
    column->GetItem(size_t(0))->SetBorder(FromDIP(compact ? 8 : 20));
    for (size_t index = 0; index < m_workspace_buttons.size(); ++index) {
        auto* button = m_workspace_buttons[index];
        const bool primary = index < 4;
        button->SetPaddingSize(FromDIP(wxSize(4, primary && !compact ? 8 : 4)));
        button->SetMinSize(FromDIP(wxSize(76, primary ? (compact ? 58 : 70) : (compact ? 32 : 36))));
        column->GetItem(button)->SetBorder(FromDIP(compact ? 4 : 8));
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
    if (page == print_center_page) refresh_print_center();
    for (size_t index = 0; index < selected.size(); ++index) {
        auto* button = m_workspace_buttons[index];
        button->SetName(selected[index] ? "input_navigation_selected" : "input_navigation");
        apply_control(button, selected[index] ? Role::SelectedNavigation : Role::Navigation);
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
