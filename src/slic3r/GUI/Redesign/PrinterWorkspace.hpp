#pragma once

#include "OrcaPrinterAdapter.hpp"

#include <array>
#include <chrono>
#include <ctime>
#include <map>
#include <boost/shared_ptr.hpp>
#include <wx/bitmap.h>
#include <wx/panel.h>
#include <wx/timer.h>

class wxBoxSizer;
class wxScrolledWindow;
class wxStaticText;
class wxMediaCtrl3;
class wxMediaCtrl;
class PrinterFileSystem;

namespace Slic3r::GUI {

class MediaPlayCtrl;
class PrinterToggle;
class PrinterProgress;
class PrinterImage;
class RoundedActionButton;

class PrinterWorkspace final : public wxPanel
{
public:
    PrinterWorkspace(wxWindow* parent, Plater* plater);
    ~PrinterWorkspace() override;
    void set_active(bool active);
    void show_prepare();
    void show_monitor();
    void show_media();
    void refresh_state();
    bool monitoring() const;

private:
    enum class View { Prepare, Monitor, Media };
    enum class Filter { All, Video, Image };
    struct MediaEntry
    {
        std::string name;
        wxString path;
        wxBitmap thumbnail;
        std::time_t time { 0 };
        bool video { false };
        int filesystem { -1 };
        size_t remote_index { 0 };
        std::string scope;
    };

    void build_workspace();
    void set_view(View view);
    void apply_state(const PrinterWorkspaceSnapshot& state);
    void choose_printer();
    void request_start();
    void request_task_action(PrinterTaskAction action);
    void request_option(int index, bool value);
    void request_snapshot();
    void set_error(const wxString& message);
    void update_preview();
    wxString media_directory() const;
    void rebuild_media();
    void open_media(const MediaEntry& entry);
    void open_local_video(const wxString& path);
    void export_media(const MediaEntry& entry);
    void connect_media();
    void disconnect_media();
    void fetch_media_url(int index);

    OrcaPrinterAdapter m_adapter;
    PrinterWorkspaceSnapshot m_state;
    wxTimer m_timer;
    bool m_active { false };
    bool m_modal { false };
    bool m_ever_printing { false };
    bool m_media_dirty { true };
    bool m_destroying { false };
    bool m_file_media_requested { false };
    size_t m_media_limit { 100 };
    View m_view { View::Prepare };
    Filter m_filter { Filter::All };
    std::optional<PrinterTaskAction> m_pending;
    PrinterCommandTarget m_pending_target;
    std::chrono::steady_clock::time_point m_pending_since;
    std::string m_media_device;
    std::string m_preview_key;
    std::string m_open_after_download;
    wxString m_media_message;
    std::map<wxString, std::pair<std::time_t, wxBitmap>> m_local_thumbnails;

    wxPanel* m_settings { nullptr };
    wxScrolledWindow* m_settings_scroll { nullptr };
    wxPanel* m_prepare { nullptr };
    wxPanel* m_monitor { nullptr };
    wxPanel* m_stage { nullptr };
    wxPanel* m_video_panel { nullptr };
    wxPanel* m_prepare_visual { nullptr };
    wxPanel* m_media_visual { nullptr };
    wxPanel* m_gallery { nullptr };
    wxScrolledWindow* m_gallery_scroll { nullptr };
    wxBoxSizer* m_gallery_items { nullptr };
    wxStaticText* m_gallery_status { nullptr };
    wxStaticText* m_device_status { nullptr };
    wxStaticText* m_gcode { nullptr };
    wxStaticText* m_estimate { nullptr };
    wxStaticText* m_error { nullptr };
    wxStaticText* m_task_status { nullptr };
    std::array<wxStaticText*, 3> m_telemetry {};
    std::array<wxStaticText*, 2> m_health_details {};
    std::array<wxStaticText*, 2> m_health_status {};
    std::array<PrinterToggle*, 3> m_options {};
    std::array<RoundedActionButton*, 3> m_filter_buttons {};
    PrinterProgress* m_progress { nullptr };
    PrinterImage* m_model_image { nullptr };
    PrinterImage* m_media_image { nullptr };
    RoundedActionButton* m_printer_choice { nullptr };
    RoundedActionButton* m_start { nullptr };
    RoundedActionButton* m_import { nullptr };
    RoundedActionButton* m_prepare_back { nullptr };
    RoundedActionButton* m_file_toggle { nullptr };
    RoundedActionButton* m_pause { nullptr };
    RoundedActionButton* m_stop { nullptr };
    wxWindow* m_camera { nullptr };
    wxMediaCtrl3* m_media_ctrl { nullptr };
    MediaPlayCtrl* m_media_play { nullptr };
    wxMediaCtrl* m_file_media { nullptr };
    std::string m_file_media_device;
    std::array<boost::shared_ptr<PrinterFileSystem>, 2> m_filesystems;
};

} // namespace Slic3r::GUI
