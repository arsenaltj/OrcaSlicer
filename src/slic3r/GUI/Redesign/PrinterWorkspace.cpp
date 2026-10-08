#include "PrinterWorkspace.hpp"
#include "RedesignWidgets.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "RedesignMessageDialog.hpp"
#include "../GUI_App.hpp"
#include "../DeviceManager.hpp"
#include "../DeviceCore/DevManager.h"
#include "../MediaPlayCtrl.h"
#include "../Printer/PrinterFileSystem.h"
#include "../wxExtensions.hpp"
#include "libslic3r/Utils.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <boost/lexical_cast.hpp>
#include <boost/make_shared.hpp>
#include <wx/dcbuffer.h>
#include <wx/dir.h>
#include <wx/dialog.h>
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/graphics.h>
#include <wx/menu.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/utils.h>
#include <wx/weakref.h>

namespace Slic3r::GUI {
namespace {

wxString text(const char* value) { return wxString::FromUTF8(value); }
wxString text(const std::string& value) { return wxString::FromUTF8(value); }

wxColour surface_colour(wxWindow* parent)
{
    if (auto* rounded = dynamic_cast<RoundedPanel*>(parent)) return rounded->face_colour();
    return parent->GetBackgroundColour();
}

wxStaticText* label(wxWindow* parent, const wxString& value, int points = 11, bool secondary = false)
{
    auto* result = new wxStaticText(parent, wxID_ANY, value, wxDefaultPosition, wxDefaultSize,
                                    wxST_NO_AUTORESIZE | wxST_ELLIPSIZE_END);
    result->SetBackgroundColour(surface_colour(parent));
    RedesignTheme::style_text(result, secondary ? RedesignTheme::secondary_text_colour() :
                              RedesignTheme::primary_text_colour(), points);
    return result;
}

wxString phase_label(PrinterTaskPhase phase)
{
    switch (phase) {
    case PrinterTaskPhase::Idle: return text("等待打印");
    case PrinterTaskPhase::Preparing: return text("正在准备打印");
    case PrinterTaskPhase::Printing: return text("正在打印");
    case PrinterTaskPhase::Paused: return text("打印已暂停");
    case PrinterTaskPhase::Complete: return text("打印完成");
    case PrinterTaskPhase::Failed: return text("打印已停止或失败");
    case PrinterTaskPhase::Unknown: return text("等待设备状态");
    }
    return {};
}

std::string path_component(const std::string& value)
{
    // Device ids must not become paths or mingle another device's captures.
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (unsigned char c : value) { result += hex[c >> 4]; result += hex[c & 15]; }
    return result.empty() ? "local" : result;
}

std::string media_scope(const PrinterWorkspaceSnapshot& state)
{
    return state.device_id.empty() ? "profile:" + state.device_name : "device:" + state.device_id;
}

bool video_extension(const wxString& extension)
{
    const auto ext = extension.Lower();
    for (const auto* allowed : {"mp4", "avi", "mkv", "wmv", "3gp", "mov", "webm", "mpg", "mpeg", "m4v"})
        if (ext == allowed) return true;
    return false;
}

} // namespace

class PrinterToggle final : public wxPanel
{
public:
    PrinterToggle(wxWindow* parent, const wxString& name, std::function<void(bool)> action)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, parent->FromDIP(wxSize(42, 24))), m_action(std::move(action))
    {
        SetMinSize(FromDIP(wxSize(42, 24)));
        SetName(name);
        SetLabel(name);
        SetCanFocus(true);
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(surface_colour(GetParent()))); dc.Clear();
            auto gc = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc));
            if (!gc) return;
            const auto size = GetClientSize();
            gc->SetPen(FindFocus() == this ? wxPen(RedesignTheme::accent_colour()) : *wxTRANSPARENT_PEN);
            gc->SetBrush(wxBrush(!IsEnabled() ? wxColour(63, 63, 67) :
                m_value ? RedesignTheme::accent_colour() : wxColour(76, 76, 81)));
            gc->DrawRoundedRectangle(1, 1, size.x - 2, size.y - 2, size.y / 2.0);
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->SetBrush(wxBrush(IsEnabled() ? *wxWHITE : wxColour(145, 145, 148)));
            const int diameter = size.y - FromDIP(6);
            gc->DrawEllipse(m_value ? size.x - diameter - FromDIP(3) : FromDIP(3), FromDIP(3), diameter, diameter);
        });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { if (IsEnabled()) { SetFocus(); m_action(!m_value); } });
        Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
            if (IsEnabled() && (e.GetKeyCode() == WXK_SPACE || e.GetKeyCode() == WXK_RETURN)) m_action(!m_value);
            else e.Skip();
        });
        Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& e) { Refresh(); e.Skip(); });
        Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) { Refresh(); e.Skip(); });
    }
    void set_value(bool value) { if (m_value != value) { m_value = value; Refresh(); } }
private:
    bool m_value { false };
    std::function<void(bool)> m_action;
};

class PrinterProgress final : public wxPanel
{
public:
    explicit PrinterProgress(wxWindow* parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, parent->FromDIP(wxSize(92, 92)))
    {
        SetMinSize(FromDIP(wxSize(92, 92)));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        RedesignTheme::style_text(this, RedesignTheme::primary_text_colour(), 12);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(surface_colour(GetParent()))); dc.Clear();
            auto gc = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc));
            if (!gc) return;
            const auto size = GetClientSize();
            const double radius = std::min(size.x, size.y) / 2.0 - FromDIP(5);
            const double cx = size.x / 2.0, cy = size.y / 2.0;
            gc->SetBrush(*wxTRANSPARENT_BRUSH);
            gc->SetPen(wxPen(wxColour(65, 67, 75), FromDIP(4)));
            gc->DrawEllipse(cx - radius, cy - radius, radius * 2, radius * 2);
            if (m_percent && *m_percent > 0) {
                auto arc = gc->CreatePath();
                constexpr double pi = 3.14159265358979323846;
                arc.AddArc(cx, cy, radius, -pi / 2, -pi / 2 + 2 * pi * *m_percent / 100.0, true);
                gc->SetPen(wxPen(wxColour(26, 108, 255), FromDIP(4))); gc->StrokePath(arc);
            }
            dc.SetFont(GetFont()); dc.SetTextForeground(RedesignTheme::primary_text_colour());
            const wxString value = m_percent ? wxString::Format("%d%%", *m_percent) : "--";
            const auto extent = dc.GetTextExtent(value);
            dc.DrawText(value, (size.x - extent.x) / 2, (size.y - extent.y) / 2);
        });
    }
    void set_value(std::optional<int> percent)
    {
        if (percent != m_percent) { m_percent = percent; SetLabel(percent ? wxString::Format("%d%%", *percent) : "--"); Refresh(); }
    }
private:
    std::optional<int> m_percent;
};

class PrinterImage final : public wxPanel
{
public:
    PrinterImage(wxWindow* parent, const wxString& empty, bool grid = false, int padding = 16)
        : wxPanel(parent), m_empty(empty), m_grid(grid), m_padding(padding)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(RedesignTheme::flow_background_colour());
        SetMinSize(FromDIP(wxSize(160, 160)));
        RedesignTheme::style_text(this, RedesignTheme::secondary_text_colour(), 11);
        Bind(wxEVT_SIZE, [this](wxSizeEvent& e) { m_scaled = {}; Refresh(); e.Skip(); });
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this); dc.SetBackground(wxBrush(GetBackgroundColour())); dc.Clear();
            const auto size = GetClientSize();
            if (m_grid) {
                dc.SetPen(wxPen(wxColour(63, 63, 70)));
                const int base = size.y * 3 / 4;
                for (int row = 0; row < 7; ++row) {
                    int y = base + row * row * FromDIP(2);
                    dc.DrawLine(size.x / 6, y, size.x * 5 / 6, y);
                }
                for (int column = -6; column <= 6; ++column)
                    dc.DrawLine(size.x / 2 + column * FromDIP(10), base,
                                size.x / 2 + column * FromDIP(38), std::min(size.y, base + FromDIP(75)));
            }
            if (!m_image.IsOk()) {
                dc.SetFont(GetFont()); dc.SetTextForeground(RedesignTheme::secondary_text_colour());
                const auto extent = dc.GetTextExtent(m_empty);
                dc.DrawText(m_empty, std::max(0, (size.x - extent.x) / 2), (size.y - extent.y) / 2); return;
            }
            if (!m_scaled.IsOk()) {
                const double scale = (m_grid ? 0.58 : 1.0) *
                    std::min(double(std::max(1, size.x - FromDIP(m_padding * 2))) / m_image.GetWidth(),
                             double(std::max(1, size.y - FromDIP(m_padding * 2))) / m_image.GetHeight());
                m_scaled = wxBitmap(m_image.Scale(std::max(1, int(m_image.GetWidth() * scale)),
                                                  std::max(1, int(m_image.GetHeight() * scale)), wxIMAGE_QUALITY_HIGH));
            }
            dc.DrawBitmap(m_scaled, (size.x - m_scaled.GetWidth()) / 2, (size.y - m_scaled.GetHeight()) / 2, true);
        });
    }
    void set_image(const wxImage& image) { m_image = image; m_scaled = {}; Refresh(); }
    bool has_image() const { return m_image.IsOk(); }
private:
    wxImage m_image;
    wxBitmap m_scaled;
    wxString m_empty;
    bool m_grid;
    int m_padding;
};

PrinterWorkspace::PrinterWorkspace(wxWindow* parent, Plater* plater)
    : wxPanel(parent), m_adapter(plater), m_timer(this)
{
    SetBackgroundColour(RedesignTheme::flow_background_colour());
    build_workspace();
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { if (!m_destroying) refresh_state(); });
    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { update_compact_layout(); event.Skip(); });
}

PrinterWorkspace::~PrinterWorkspace()
{
    m_destroying = true;
    m_active = false;
    m_timer.Stop();
    disconnect_media();
    // MediaPlayCtrl's worker uses the video window; destroy it first.
    delete m_media_play; m_media_play = nullptr;
    delete m_media_ctrl; m_media_ctrl = nullptr;
}

void PrinterWorkspace::build_workspace()
{
    using namespace RedesignTheme;
    auto* root = new wxBoxSizer(wxHORIZONTAL);
    m_settings = new RoundedPanel(this, FromDIP(wxSize(373, -1)), panel_colour(), flow_background_colour(), 12);
    m_settings->SetMinSize(FromDIP(wxSize(373, -1)));
    auto* settings = new wxBoxSizer(wxVERTICAL);
    m_settings_scroll = new wxScrolledWindow(m_settings, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_settings_scroll->SetBackgroundColour(panel_colour());
    m_settings_scroll->SetScrollRate(0, FromDIP(12));
    auto* sections = new wxBoxSizer(wxVERTICAL);
    m_prepare = new wxPanel(m_settings_scroll); m_prepare->SetBackgroundColour(panel_colour());
    auto* prepare = new wxBoxSizer(wxVERTICAL);
    prepare->Add(label(m_prepare, text("打印机"), 14), 0, wxEXPAND | wxBOTTOM, FromDIP(20));
    m_printer_choice = new RoundedActionButton(m_prepare, text("选择打印机"), false, 56);
    m_printer_choice->set_alignment(wxALIGN_LEFT);
    style_medium_text(m_printer_choice, primary_text_colour(), 14);
    m_printer_choice->set_icon(create_scaled_bitmap("redesign_print_arrow", m_printer_choice, 20));
    m_printer_choice->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { choose_printer(); });
    prepare->Add(m_printer_choice, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    m_device_status = label(m_prepare, text("请选择已连接的打印机"), 9, true);
    prepare->Add(m_device_status, 0, wxEXPAND | wxBOTTOM, FromDIP(20));
    prepare->Add(label(m_prepare, text("关键信息"), 14), 0, wxEXPAND | wxBOTTOM, FromDIP(12));
    auto* options = new RoundedPanel(m_prepare, wxDefaultSize, control_colour(), panel_colour(), 12);
    auto* rows = new wxBoxSizer(wxVERTICAL);
    const std::array<wxString, 3> names { text("打印前自动调平"), text("开启摄像头录制"), text("AI失败预警") };
    for (size_t i = 0; i < names.size(); ++i) {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(label(options, names[i], 12), 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        m_options[i] = new PrinterToggle(options, names[i], [this, i](bool value) { request_option(int(i), value); });
        row->Add(m_options[i], 0, wxALIGN_CENTER_VERTICAL);
        rows->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));
    }
    for (int i = 0; i < 2; ++i) {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(label(options, i == 0 ? "G-code" : text("预计打印时间"), 9, true), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
        auto* value = label(options, "--", 9, true);
        if (i == 0) m_gcode = value; else m_estimate = value;
        row->Add(value, 1, wxALIGN_CENTER_VERTICAL);
        rows->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));
    }
    rows->AddSpacer(FromDIP(16)); options->SetSizer(rows);
    prepare->Add(options, 0, wxEXPAND | wxBOTTOM, FromDIP(24));
    prepare->Add(label(m_prepare, text("视频截图"), 14), 0, wxEXPAND | wxBOTTOM, FromDIP(12));
    auto* media = new RoundedActionButton(m_prepare, text("录制视频与截图"), false, 48);
    media->set_alignment(wxALIGN_LEFT);
    style_text(media, primary_text_colour(), 12);
    media->set_icon(create_scaled_bitmap("redesign_print_arrow", media, 20));
    media->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { show_media(); });
    prepare->Add(media, 0, wxEXPAND | wxBOTTOM, FromDIP(12));
    auto* live = new RoundedActionButton(m_prepare, text("实时监控"), false, 40);
    live->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { show_monitor(); });
    prepare->Add(live, 0, wxEXPAND);
    m_prepare_back = new RoundedActionButton(m_prepare, text("返回打印准备"), false, 40);
    m_prepare_back->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { show_prepare(); });
    prepare->Add(m_prepare_back, 0, wxEXPAND | wxTOP, FromDIP(12)); m_prepare_back->Hide();
    m_prepare->SetSizer(prepare);
    sections->Add(m_prepare, 0, wxEXPAND);

    m_monitor = new wxPanel(m_settings_scroll); m_monitor->SetBackgroundColour(panel_colour());
    auto* monitor = new wxBoxSizer(wxVERTICAL);
    monitor->Add(label(m_monitor, text("智能监控"), 14), 0, wxEXPAND | wxBOTTOM, FromDIP(20));
    auto* metrics = new RoundedPanel(m_monitor, wxDefaultSize, control_colour(), panel_colour(), 12);
    auto* metric_rows = new wxBoxSizer(wxVERTICAL);
    m_progress = new PrinterProgress(metrics);
    metric_rows->Add(m_progress, 0, wxALIGN_CENTER | wxTOP | wxBOTTOM, FromDIP(20));
    auto* telemetry = new wxBoxSizer(wxHORIZONTAL);
    const std::array<wxString, 3> headings { text("喷头"), text("热床"), text("剩余时间") };
    for (size_t i = 0; i < headings.size(); ++i) {
        auto* column = new wxBoxSizer(wxVERTICAL);
        column->Add(label(metrics, headings[i], 12, true), 0, wxALIGN_CENTER | wxBOTTOM, FromDIP(12));
        m_telemetry[i] = label(metrics, "--", 18);
        column->Add(m_telemetry[i], 0, wxALIGN_CENTER);
        telemetry->Add(column, 1, wxEXPAND);
    }
    metric_rows->Add(telemetry, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    m_task_status = label(metrics, text("等待设备状态"), 9, true);
    metric_rows->Add(m_task_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    metrics->SetSizer(metric_rows);
    monitor->Add(metrics, 0, wxEXPAND | wxBOTTOM, FromDIP(20));
    auto* health = new RoundedPanel(m_monitor, wxDefaultSize, control_colour(), panel_colour(), 12);
    auto* health_rows = new wxBoxSizer(wxVERTICAL);
    const std::array<wxString, 2> health_headings { text("AI视觉检测"), text("耗材余量") };
    for (size_t i = 0; i < health_headings.size(); ++i) {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(label(health, health_headings[i], 12), 1, wxALIGN_CENTER_VERTICAL);
        m_health_status[i] = label(health, "--", 9, true);
        row->Add(m_health_status[i], 0, wxALIGN_CENTER_VERTICAL);
        health_rows->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));
        m_health_details[i] = label(health, text("未上报"), 9, true);
        health_rows->Add(m_health_details[i], 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));
    }
    health_rows->AddSpacer(FromDIP(16)); health->SetSizer(health_rows);
    monitor->Add(health, 0, wxEXPAND | wxBOTTOM, FromDIP(16));
    auto* back = new RoundedActionButton(m_monitor, text("打印机设置与媒体"), false, 40);
    back->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { set_view(View::Media); });
    monitor->Add(back, 0, wxEXPAND);
    m_monitor->SetSizer(monitor);
    sections->Add(m_monitor, 0, wxEXPAND); m_monitor->Hide();
    m_settings_scroll->SetSizer(sections);
    settings->Add(m_settings_scroll, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(21));
    m_error = label(m_settings, {}, 9, true);
    m_error->SetForegroundColour(wxColour(255, 172, 100)); m_error->Hide();
    settings->Add(m_error, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(21));
    m_start = new RoundedActionButton(m_settings, text("开始打印"), true, 48);
    style_medium_text(m_start, wxColour(20, 20, 20), 14);
    m_start->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { request_start(); });
    settings->Add(m_start, 0, wxEXPAND | wxALL, FromDIP(16));
    m_pause = new RoundedActionButton(m_settings, text("暂停打印"), false, 48);
    style_medium_text(m_pause, primary_text_colour(), 14);
    m_pause->set_secondary_face(wxColour(80, 80, 84));
    m_pause->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        request_task_action(m_state.phase == PrinterTaskPhase::Paused ? PrinterTaskAction::Resume : PrinterTaskAction::Pause);
    });
    settings->Add(m_pause, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16)); m_pause->Hide();
    m_stop = new RoundedActionButton(m_settings, text("停止打印"), true, 48);
    style_medium_text(m_stop, wxColour(20, 20, 20), 14);
    m_stop->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { request_task_action(PrinterTaskAction::Stop); });
    settings->Add(m_stop, 0, wxEXPAND | wxALL, FromDIP(16)); m_stop->Hide();
    m_settings->SetSizer(settings);
    root->Add(m_settings, 0, wxEXPAND | wxRIGHT, FromDIP(12));

    m_stage = new wxPanel(this); m_stage->SetBackgroundColour(flow_background_colour());
    auto* stage = new wxBoxSizer(wxVERTICAL);
    m_prepare_visual = new wxPanel(m_stage); m_prepare_visual->SetBackgroundColour(flow_background_colour());
    auto* preview = new wxBoxSizer(wxVERTICAL);
    m_model_image = new PrinterImage(m_prepare_visual, text("请先导入模型并完成切片"), true);
    preview->Add(m_model_image, 1, wxEXPAND);
    m_import = new RoundedActionButton(m_prepare_visual, text("导入模型"), false, 40);
    m_import->SetMinSize(FromDIP(wxSize(180, 40)));
    m_import->SetBackgroundColour(flow_background_colour());
    m_import->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_modal) return;
        m_modal = true;
        m_adapter.import_model();
        m_modal = false;
        m_preview_key.clear();
        refresh_state();
    });
    preview->Add(m_import, 0, wxALIGN_CENTER | wxBOTTOM, FromDIP(24));
    m_prepare_visual->SetSizer(preview); stage->Add(m_prepare_visual, 1, wxEXPAND);
    m_video_panel = new RoundedPanel(m_stage, wxDefaultSize, panel_colour(), flow_background_colour(), 12);
    auto* video = new wxBoxSizer(wxVERTICAL);
    auto* video_header = new wxBoxSizer(wxHORIZONTAL);
    video_header->Add(label(m_video_panel, text("实时视频"), 14), 1, wxALIGN_CENTER_VERTICAL);
    auto* camera = new RoundedActionButton(m_video_panel, {}, false, 40);
    camera->SetMinSize(FromDIP(wxSize(40, 40))); camera->SetName(text("保存视频截图"));
    camera->set_icon(create_scaled_bitmap("redesign_print_camera", camera, 32));
    camera->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { request_snapshot(); }); m_camera = camera;
    video_header->Add(camera, 0, wxALIGN_CENTER_VERTICAL);
    video->Add(video_header, 0, wxEXPAND | wxALL, FromDIP(16));
    // Decode/network workers are created only when this view is first visited.
    m_video_panel->SetSizer(video); stage->Add(m_video_panel, 1, wxEXPAND); m_video_panel->Hide();
    m_media_visual = new wxPanel(m_stage); m_media_visual->SetBackgroundColour(flow_background_colour());
    auto* media_view = new wxBoxSizer(wxVERTICAL);
    m_media_image = new PrinterImage(m_media_visual, text("选择右侧视频或图片查看"));
    media_view->Add(m_media_image, 1, wxEXPAND);
    m_file_toggle = new RoundedActionButton(m_media_visual, text("播放视频"), false, 40);
    m_file_toggle->SetMinSize(FromDIP(wxSize(140, 40)));
    m_file_toggle->SetBackgroundColour(flow_background_colour());
    m_file_toggle->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (!m_file_media || !m_active || m_view != View::Media || !m_file_media_requested ||
            m_file_media_device != media_scope(m_adapter.snapshot())) return;
        const bool accepted = m_file_media->GetState() == wxMEDIASTATE_PLAYING ?
            m_file_media->Pause() : m_file_media->Play();
        if (!accepted) set_error(text("视频播放失败，可导出后使用本地播放器查看。"));
    });
    media_view->Add(m_file_toggle, 0, wxALIGN_RIGHT | wxRIGHT | wxBOTTOM, FromDIP(16)); m_file_toggle->Hide();
    m_media_visual->SetSizer(media_view); stage->Add(m_media_visual, 1, wxEXPAND); m_media_visual->Hide();
    m_stage->SetSizer(stage); root->Add(m_stage, 1, wxEXPAND);

    m_gallery = new RoundedPanel(this, FromDIP(wxSize(373, -1)), panel_colour(), flow_background_colour(), 12);
    m_gallery->SetMinSize(FromDIP(wxSize(373, -1)));
    auto* gallery = new wxBoxSizer(wxVERTICAL);
    auto* tabs = new wxBoxSizer(wxHORIZONTAL);
    const std::array<wxString, 3> filters { text("全部"), text("视频"), text("图片") };
    for (size_t i = 0; i < filters.size(); ++i) {
        auto* button = new RoundedActionButton(m_gallery, filters[i], false, 40);
        button->set_secondary_face(panel_colour()); m_filter_buttons[i] = button;
        style_medium_text(button, primary_text_colour(), 12);
        button->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent&) {
            m_filter = static_cast<Filter>(i); m_media_limit = 100; rebuild_media();
        });
        tabs->Add(button, 1, wxEXPAND);
    }
    gallery->Add(tabs, 0, wxEXPAND | wxALL, FromDIP(12));
    m_gallery_status = label(m_gallery, {}, 9, true);
    gallery->Add(m_gallery_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    m_gallery_scroll = new wxScrolledWindow(m_gallery, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_gallery_scroll->SetBackgroundColour(panel_colour()); m_gallery_scroll->SetScrollRate(0, FromDIP(12));
    m_gallery_items = new wxBoxSizer(wxVERTICAL); m_gallery_scroll->SetSizer(m_gallery_items);
    gallery->Add(m_gallery_scroll, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    m_gallery->SetSizer(gallery); root->Add(m_gallery, 0, wxEXPAND | wxLEFT, FromDIP(12)); m_gallery->Hide();
    SetSizer(root);
}

void PrinterWorkspace::set_active(bool active)
{
    if (m_active == active) return;
    m_active = active;
    if (active) { m_preview_key.clear(); m_timer.Start(1000); refresh_state(); }
    else {
        m_timer.Stop();
        if (m_media_play) m_media_play->SetMachineObject(nullptr);
        m_file_media_requested = false;
        if (m_file_media) m_file_media->Stop();
        disconnect_media();
    }
}

void PrinterWorkspace::apply_ai_theme(bool)
{
    // This subtree already owns the Figma palette and point sizes. A legacy
    // recursive theme refresh must not repaint its controls or media pixels.
    Refresh(false);
}

void PrinterWorkspace::update_compact_layout()
{
    if (!m_settings || !m_gallery || GetClientSize().x <= 0) return;
    const int width = GetClientSize().x;
    const int settings_width = std::clamp(width / (m_view == View::Media ? 3 : 2), FromDIP(280), FromDIP(373));
    const int gallery_width = std::clamp(width / 4, FromDIP(180), FromDIP(373));
    if (m_settings->GetMinSize().x == settings_width && m_gallery->GetMinSize().x == gallery_width) return;
    m_settings->SetMinSize(wxSize(settings_width, -1));
    m_gallery->SetMinSize(wxSize(gallery_width, -1));
    Layout();
    m_settings_scroll->FitInside();
    m_gallery_scroll->FitInside();
}

void PrinterWorkspace::show_prepare() { set_view(View::Prepare); refresh_state(); }
void PrinterWorkspace::show_monitor() { set_view(View::Monitor); refresh_state(); }
void PrinterWorkspace::show_media() { set_view(View::Media); refresh_state(); }
bool PrinterWorkspace::monitoring() const { return m_view != View::Prepare; }

void PrinterWorkspace::set_view(View view)
{
    m_view = view;
    const bool monitor = view == View::Monitor;
    if (monitor && !m_media_ctrl) {
        m_media_ctrl = new wxMediaCtrl3(m_video_panel);
        m_media_ctrl->SetMinSize(FromDIP(wxSize(160, 120)));
        m_media_play = new MediaPlayCtrl(m_video_panel, m_media_ctrl);
        m_media_play->SetBackgroundColour(RedesignTheme::panel_colour());
        for (auto* child : m_media_play->GetChildren()) {
            child->SetBackgroundColour(RedesignTheme::panel_colour());
            child->SetForegroundColour(RedesignTheme::primary_text_colour());
            if (auto* button = dynamic_cast<::Button*>(child)) {
                button->SetBackgroundColorNormal(RedesignTheme::panel_colour());
                button->SetBorderColorNormal(RedesignTheme::panel_colour());
            }
        }
        m_video_panel->GetSizer()->Add(m_media_ctrl, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
        m_video_panel->GetSizer()->Add(m_media_play, 0, wxEXPAND | wxALL, FromDIP(16));
    }
    if (!monitor && m_media_play) m_media_play->SetMachineObject(nullptr);
    if (view != View::Media) {
        m_file_media_requested = false;
        if (m_file_media) { m_file_media->Stop(); m_file_media->Hide(); m_media_image->Show(); }
        m_file_toggle->Hide();
        disconnect_media();
    }
    m_prepare->Show(!monitor); m_monitor->Show(monitor);
    m_prepare_back->Show(view == View::Media);
    m_start->Show(!monitor); m_pause->Show(monitor); m_stop->Show(monitor);
    m_prepare_visual->Show(view == View::Prepare); m_video_panel->Show(monitor); m_media_visual->Show(view == View::Media);
    m_gallery->Show(view == View::Media);
    update_compact_layout();
    m_settings_scroll->FitInside(); m_settings->Layout(); m_stage->Layout(); Layout();
    if (view == View::Media) { m_media_dirty = true; connect_media(); rebuild_media(); }
}

void PrinterWorkspace::refresh_state()
{
    if (m_destroying) return;
    const auto state = m_adapter.snapshot();
    const bool device_changed = media_scope(state) != media_scope(m_state);
    const bool connection_lost = m_state.info_ready && !state.info_ready;
    if (device_changed) {
        m_pending.reset(); m_ever_printing = false; m_preview_key.clear(); set_error({});
        disconnect_media(); m_media_image->set_image({}); m_media_message.clear();
        m_local_thumbnails.clear(); m_media_limit = 100; m_file_media_requested = false;
        if (m_file_media) { m_file_media->Stop(); m_file_media->Hide(); m_media_image->Show(); }
        m_file_toggle->Hide();
    }
    if (connection_lost) {
        disconnect_media(); m_media_message = text("打印机未连接，仅显示本地媒体");
    }
    m_state = state;
    if (m_pending) {
        const bool target_changed = state.device_id != m_pending_target.device_id || state.task_id != m_pending_target.task_id;
        const bool acknowledged = (*m_pending == PrinterTaskAction::Pause && state.phase == PrinterTaskPhase::Paused) ||
            (*m_pending == PrinterTaskAction::Resume && state.phase == PrinterTaskPhase::Printing) ||
            (*m_pending == PrinterTaskAction::Stop && state.info_ready && !printer_task_active(state.phase));
        if (target_changed || acknowledged) { m_pending.reset(); set_error({}); }
        else if (std::chrono::steady_clock::now() - m_pending_since > std::chrono::seconds(15)) {
            m_pending.reset(); set_error(text("设备尚未确认操作，请检查连接后重试。"));
        }
    }
    if (m_active && !m_modal) {
        if (printer_task_active(state.phase)) {
            if ((!m_ever_printing || device_changed) && m_view != View::Media) set_view(View::Monitor);
            m_ever_printing = true;
        } else if (m_ever_printing && state.info_ready && state.phase == PrinterTaskPhase::Complete) {
            m_ever_printing = false; set_view(View::Media);
        }
    }
    apply_state(state);
    if (m_active && m_view == View::Monitor && m_media_play) {
        auto* manager = wxGetApp().getDeviceManager();
        auto* machine = manager ? manager->get_selected_machine() : nullptr;
        m_media_play->SetMachineObject(state.info_ready && state.has_camera ? machine : nullptr);
        m_camera->Enable(state.info_ready && state.has_camera && m_media_ctrl->GetState() == wxMEDIASTATE_PLAYING);
    }
    if (m_active && m_view == View::Media) { connect_media(); if (m_media_dirty && !m_modal) rebuild_media(); }
    if (m_active && m_view == View::Media && m_file_media && m_file_media_requested) {
        // Windows media backends can omit loaded/state events for local files.
        const auto video_state = m_file_media->GetState();
        const auto caption = video_state == wxMEDIASTATE_PLAYING ? text("暂停播放") : text("播放视频");
        if (m_file_toggle->GetLabel() != caption) {
            m_file_toggle->SetLabel(caption); m_file_toggle->Refresh();
            BOOST_LOG_TRIVIAL(info) << "[PrinterWorkspace] local video polled state=" << int(video_state);
        }
    }
    if (m_view == View::Prepare) update_preview();
}

void PrinterWorkspace::apply_state(const PrinterWorkspaceSnapshot& state)
{
    m_printer_choice->SetLabel(state.device_name.empty() ? text("选择打印机") : text(state.device_name));
    m_printer_choice->SetToolTip(text(state.device_name)); m_printer_choice->Refresh();
    wxString status = state.connecting ? text("正在连接…") : state.info_ready ? phase_label(state.phase) :
        state.connected ? text("等待设备状态…") : state.device_id.empty() && !state.native_dispatch && state.configured_host ?
        text("已配置打印主机") : state.device_id.empty() ? text("请选择已连接的打印机") : text("打印机已离线");
    if (state.slicing) status = text("正在切片…");
    else if (!state.gcode_ready && !printer_task_active(state.phase)) status += text(" · 等待切片文件");
    m_device_status->SetLabel(status);
    m_gcode->SetLabel(state.gcode_ready ? text(state.gcode_name) : text("未生成"));
    m_gcode->SetToolTip(text(state.gcode_name));
    m_estimate->SetLabel(state.estimated_seconds ? text(printer_duration(*state.estimated_seconds)) : "--");
    const bool editable = !m_modal && !printer_task_active(state.phase) && !m_pending;
    const std::array<bool, 3> enabled { state.can_level, state.can_record, state.can_detect };
    const std::array<bool, 3> values { state.bed_leveling, state.recording, state.detection_enabled };
    for (size_t i = 0; i < enabled.size(); ++i) {
        m_options[i]->set_value(values[i]); m_options[i]->Enable(editable && state.info_ready && enabled[i]);
        m_options[i]->SetToolTip(!state.info_ready ? text("请连接打印机并等待状态上报") :
            enabled[i] ? wxString() : text("当前设备或存储状态不支持此选项")); m_options[i]->Refresh();
    }
    m_start->SetLabel(m_view == View::Media ? text("再次准备打印") : state.slicing ? text("正在切片…") :
                     printer_can_slice(state) ? text("切片并准备打印") : text("开始打印"));
    m_start->Enable(!m_modal && !m_pending && (m_view == View::Media || printer_can_review(state) || printer_can_slice(state)));
    m_start->SetToolTip(m_view == View::Media ? text("返回准备页，重新核对当前工程与设备") :
        state.gcode_ready ? text("核对当前切片后，进入设备确认或本地导出") : text("先完成当前工程的正式切片"));
    m_start->Refresh();
    const bool show_import = !state.gcode_ready && !state.can_slice && !state.slicing;
    if (m_import->IsShown() != show_import) { m_import->Show(show_import); m_prepare_visual->Layout(); }
    m_import->Enable(!m_modal && m_adapter.can_import_model());
    m_progress->set_value(state.progress);
    m_telemetry[0]->SetLabel(state.nozzle_temperature ? wxString::Format("%d°", *state.nozzle_temperature) : "--");
    m_telemetry[1]->SetLabel(state.bed_temperature ? wxString::Format("%d°", *state.bed_temperature) : "--");
    m_telemetry[2]->SetLabel(state.remaining_seconds ? text(printer_duration(*state.remaining_seconds)) : "--");
    m_task_status->SetLabel(m_pending ? text("等待打印机确认…") : !state.info_ready ? text("打印机未连接或尚未上报状态") :
                            printer_task_active(state.phase) && state.task_id.empty() ? text("任务标识未上报，暂不可控制") :
                            phase_label(state.phase));
    m_task_status->SetToolTip(text(state.task_name));
    m_health_status[0]->SetLabel(!state.info_ready ? text("未上报") : state.device_warning ? text("设备告警") :
                               state.detection_enabled ? text("已开启") : text("未开启"));
    m_health_status[0]->SetForegroundColour(state.device_warning ? wxColour(255, 149, 86) :
        state.detection_enabled && state.info_ready ? wxColour(56, 204, 150) : RedesignTheme::secondary_text_colour());
    m_health_details[0]->SetLabel(state.device_warning ? text("请查看打印机告警并检查打印状态") : text("检测详情由设备上报"));
    m_health_status[1]->SetLabel(state.filament_percent ? wxString::Format("%d%%", *state.filament_percent) : text("未上报"));
    m_health_details[1]->SetLabel(state.filament_percent ? text("当前耗材剩余比例") : text("设备未提供耗材余量"));
    const PrinterCommandTarget target { state.device_id, state.task_id };
    const auto action = state.phase == PrinterTaskPhase::Paused ? PrinterTaskAction::Resume : PrinterTaskAction::Pause;
    m_pause->SetLabel(action == PrinterTaskAction::Resume ? text("继续打印") : text("暂停打印"));
    m_pause->Enable(!m_modal && !m_pending && printer_command_allowed(state, target, action)); m_pause->Refresh();
    m_stop->Enable(!m_modal && !m_pending && printer_command_allowed(state, target, PrinterTaskAction::Stop)); m_stop->Refresh();
    m_prepare->Layout(); m_monitor->Layout(); m_settings_scroll->FitInside(); m_settings->Layout();
}

void PrinterWorkspace::choose_printer()
{
    const auto devices = m_adapter.devices();
    if (devices.empty()) { set_error(text("暂无可用打印机，请先登录账号或添加局域网设备。")); return; }
    wxMenu menu;
    for (const auto& device : devices) {
        const int id = wxWindow::NewControlId();
        menu.AppendRadioItem(id, text(device.name) + (device.online ? wxString() : text("（离线）")));
        menu.Check(id, device.id == m_state.device_id);
        menu.Bind(wxEVT_MENU, [this, target = device.id](wxCommandEvent&) {
            if (!m_adapter.select_device(target)) set_error(text("设备列表已变化，请重新选择打印机。"));
            refresh_state();
        }, id);
    }
    PopupMenu(&menu);
}

void PrinterWorkspace::request_start()
{
    if (m_modal || m_pending) return;
    if (m_view == View::Media) { show_prepare(); return; }
    const bool slicing = printer_can_slice(m_adapter.snapshot());
    m_modal = true;
    apply_state(m_adapter.snapshot());
    const bool accepted = slicing ? m_adapter.start_slice() : m_adapter.review_print(this);
    m_modal = false;
    if (!accepted) set_error(slicing ? text("切片未启动，请检查模型、耗材和打印设置。") :
                                     text("请连接打印机并完成当前盘的切片后再开始打印。"));
    else set_error({});
    refresh_state();
}

void PrinterWorkspace::request_task_action(PrinterTaskAction action)
{
    if (m_modal || m_pending) return;
    const auto state = m_adapter.snapshot();
    const PrinterCommandTarget target { state.device_id, state.task_id };
    if (!printer_command_allowed(state, target, action)) { refresh_state(); return; }
    if (action != PrinterTaskAction::Resume) {
        m_modal = true; apply_state(state);
        const bool pause = action == PrinterTaskAction::Pause;
        auto* top = wxGetTopLevelParent(this);
        wxDialog shade(top, wxID_ANY, {}, wxDefaultPosition, wxDefaultSize,
                       wxBORDER_NONE | wxFRAME_NO_TASKBAR | wxFRAME_FLOAT_ON_PARENT);
        shade.SetBackgroundColour(*wxBLACK);
        shade.SetSize(top->GetScreenRect());
        const bool dimmed = shade.SetTransparent(140);
        if (dimmed) shade.ShowWithoutActivating();
        RedesignMessageDialog dialog(dimmed ? static_cast<wxWindow*>(&shade) : this,
                                     pause ? text("是否暂停打印？") : text("是否停止当前打印？停止后无法继续此任务。"),
                                     pause ? text("暂停打印") : text("停止打印"), wxYES_NO | wxNO_DEFAULT, true);
        const int result = dialog.ShowModal(); m_modal = false;
        if (result != wxID_YES) { refresh_state(); return; }
    }
    if (!m_adapter.control_task(target, action)) set_error(text("设备或任务状态已变化，操作未发送。请检查后重试。"));
    else {
        m_pending = action; m_pending_target = target; m_pending_since = std::chrono::steady_clock::now(); set_error({});
    }
    refresh_state();
}

void PrinterWorkspace::request_option(int index, bool value)
{
    if (m_modal || m_pending || printer_task_active(m_state.phase)) return;
    const auto id = m_state.device_id;
    const bool success = index == 0 ? m_adapter.set_bed_leveling(id, value) :
        index == 1 ? m_adapter.set_recording(id, value) : m_adapter.set_detection(id, value);
    set_error(success ? wxString() : text("设置未生效，请检查设备连接及存储状态。"));
    refresh_state();
}

void PrinterWorkspace::set_error(const wxString& message)
{
    m_error->SetLabel(message); m_error->Wrap(FromDIP(325)); m_error->Show(!message.empty()); m_settings->Layout();
}

void PrinterWorkspace::update_preview()
{
    const auto key = m_state.gcode_name + (m_state.gcode_ready ? ":ready" : ":unready") +
                     (m_state.slicing ? ":slicing" : "");
    if (key != m_preview_key || !m_model_image->has_image()) {
        m_preview_key = key; m_model_image->set_image(m_adapter.plate_preview());
    }
}

wxString PrinterWorkspace::media_directory() const
{
    wxFileName directory;
    directory.AssignDir(text(data_dir()));
    directory.AppendDir("printer-media");
    directory.AppendDir(text(path_component(m_state.device_id.empty() ? m_state.device_name : m_state.device_id)));
    return directory.GetPath();
}

void PrinterWorkspace::request_snapshot()
{
    if (!m_media_ctrl || !m_active || m_view != View::Monitor || !m_state.info_ready || m_modal) return;
    const auto current = m_adapter.snapshot();
    if (current.device_id != m_state.device_id || !current.info_ready) { refresh_state(); return; }
    const auto frame = m_media_ctrl->GetCurrentFrame();
    if (!frame.IsOk()) { set_error(text("暂无可保存的视频画面，请等待视频连接。")); return; }
    const auto directory = media_directory();
    const wxString name = "image_" + wxDateTime::UNow().Format("%Y%m%d_%H%M%S") +
                          wxString::Format("_%03d.png", wxDateTime::UNow().GetMillisecond());
    if ((!wxDirExists(directory) && !wxFileName::Mkdir(directory, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) ||
        !frame.SaveFile(wxFileName(directory, name).GetFullPath(), wxBITMAP_TYPE_PNG)) {
        set_error(text("截图保存失败，请检查文件夹权限。")); return;
    }
    m_media_dirty = true; set_error(text("截图已保存，可在‘录制视频与截图’中查看。"));
}

void PrinterWorkspace::rebuild_media()
{
    if (m_destroying || m_modal) { m_media_dirty = true; return; }
    m_media_dirty = false;
    std::vector<MediaEntry> entries;
    wxDir directory;
    if (wxDirExists(media_directory())) directory.Open(media_directory());
    if (directory.IsOpened()) {
        wxString name;
        for (bool more = directory.GetFirst(&name, {}, wxDIR_FILES); more; more = directory.GetNext(&name)) {
            wxFileName file(media_directory(), name);
            const auto extension = file.GetExt().Lower();
            const bool video = video_extension(extension);
            if (!video && extension != "png" && extension != "jpg" && extension != "jpeg") continue;
            MediaEntry entry;
            entry.name = name.ToUTF8().data(); entry.path = file.GetFullPath();
            entry.video = video;
            entry.time = file.GetModificationTime().IsValid() ? file.GetModificationTime().GetTicks() : 0;
            entries.push_back(std::move(entry));
        }
    }
    bool loading = false, failed = false;
    for (size_t i = 0; i < m_filesystems.size(); ++i) {
        const auto& fs = m_filesystems[i]; if (!fs) continue;
        loading |= fs->GetStatus() != PrinterFileSystem::ListReady && fs->GetStatus() != PrinterFileSystem::Failed;
        failed |= fs->GetStatus() == PrinterFileSystem::Failed;
        const size_t count = fs->GetCount();
        fs->SetFocusRange(0, std::min(count, m_media_limit));
        for (size_t j = 0; j < count; ++j) {
            const auto& file = fs->GetFile(j);
            auto local = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
                return entry.path == text(file.local_path);
            });
            if (!file.local_path.empty() && local != entries.end()) {
                if (file.thumbnail.IsOk()) local->thumbnail = file.thumbnail;
                if (file.time) local->time = file.time;
                continue;
            }
            entries.push_back({file.name, text(file.local_path), file.thumbnail, file.time, true, int(i), j});
        }
    }
    for (auto& entry : entries) entry.scope = media_scope(m_state);
    std::stable_sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.time > b.time; });
    m_gallery_scroll->Freeze(); m_gallery_items->Clear(true);
    size_t total = 0;
    for (auto& entry : entries) {
        if ((m_filter == Filter::Video && !entry.video) || (m_filter == Filter::Image && entry.video)) continue;
        if (++total > m_media_limit) continue;
        if (!entry.video && !entry.thumbnail.IsOk()) {
            auto cached = m_local_thumbnails.find(entry.path);
            if (cached == m_local_thumbnails.end() || cached->second.first != entry.time) {
                wxImage thumbnail;
                wxBitmap bitmap;
                if (thumbnail.LoadFile(entry.path)) {
                    const double scale = std::min(120.0 / thumbnail.GetWidth(), 90.0 / thumbnail.GetHeight());
                    bitmap = wxBitmap(thumbnail.Scale(std::max(1, int(thumbnail.GetWidth() * scale)),
                                                       std::max(1, int(thumbnail.GetHeight() * scale)), wxIMAGE_QUALITY_HIGH));
                }
                cached = m_local_thumbnails.insert_or_assign(entry.path, std::make_pair(entry.time, bitmap)).first;
            }
            entry.thumbnail = cached->second.second;
        }
        auto* card = new RoundedPanel(m_gallery_scroll, FromDIP(wxSize(-1, 90)), RedesignTheme::control_colour(),
                                      RedesignTheme::panel_colour(), 8);
        card->SetMinSize(FromDIP(wxSize(-1, 90)));
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        auto* thumb = new PrinterImage(card, entry.video ? text("视频") : text("图片"), false, 0);
        thumb->SetBackgroundColour(RedesignTheme::control_colour()); thumb->SetMinSize(FromDIP(wxSize(120, 90)));
        if (entry.thumbnail.IsOk()) thumb->set_image(entry.thumbnail.ConvertToImage());
        row->Add(thumb, 0, wxEXPAND);
        auto* info = new wxBoxSizer(wxVERTICAL);
        auto* name = label(card, text(entry.name), 12); name->SetToolTip(text(entry.name));
        name->SetMinSize(wxSize(1, -1));
        info->Add(name, 0, wxEXPAND | wxTOP, FromDIP(12));
        const wxString date = entry.time ? wxDateTime(entry.time).Format("%Y-%m-%d %H:%M") : text("日期未上报");
        auto* timestamp = label(card, date, 9, true);
        timestamp->SetMinSize(wxSize(1, -1));
        timestamp->SetToolTip(date);
        info->Add(timestamp, 0, wxEXPAND | wxTOP, FromDIP(8));
        auto* export_button = new RoundedActionButton(card, {}, false, 24);
        export_button->SetMinSize(FromDIP(wxSize(24, 24))); export_button->SetName(text("导出媒体"));
        export_button->SetToolTip(text("导出到本地"));
        export_button->set_icon(create_scaled_bitmap("redesign_print_export", export_button, 16));
        export_button->Bind(wxEVT_BUTTON, [this, entry](wxCommandEvent&) { export_media(entry); });
        info->Add(export_button, 0, wxALIGN_RIGHT | wxTOP, FromDIP(4));
        row->Add(info, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12)); card->SetSizer(row);
        for (auto* window : std::array<wxWindow*, 3>{card, thumb, name})
            window->Bind(wxEVT_LEFT_UP, [this, entry](wxMouseEvent&) { open_media(entry); });
        card->SetCanFocus(true);
        card->Bind(wxEVT_KEY_DOWN, [this, entry](wxKeyEvent& e) {
            if (e.GetKeyCode() == WXK_RETURN || e.GetKeyCode() == WXK_SPACE) open_media(entry); else e.Skip();
        });
        m_gallery_items->Add(card, 0, wxEXPAND | wxBOTTOM, FromDIP(12));
    }
    if (total > m_media_limit) {
        auto* more = new RoundedActionButton(m_gallery_scroll, text("加载更多"), false, 40);
        more->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            m_media_limit += 100; m_media_dirty = true;
            // Rebuild on the timer, after this button's event has returned.
        });
        m_gallery_items->Add(more, 0, wxEXPAND | wxBOTTOM, FromDIP(12));
    }
    for (size_t i = 0; i < m_filter_buttons.size(); ++i)
        m_filter_buttons[i]->set_text_colour(static_cast<size_t>(m_filter) == i ? RedesignTheme::accent_colour() :
                                             RedesignTheme::secondary_text_colour());
    m_gallery_status->SetLabel(!m_media_message.empty() ? m_media_message : loading ? text("正在读取打印机媒体…") :
        failed ? text("设备媒体读取失败，可重新进入此页面重试") : total == 0 ? text("暂无媒体文件") :
        wxString::Format(text("%zu 个文件"), total));
    m_gallery_scroll->FitInside(); m_gallery_scroll->Layout(); m_gallery_scroll->Thaw(); m_gallery->Layout();
}

void PrinterWorkspace::open_media(const MediaEntry& entry)
{
    const auto state = m_adapter.snapshot();
    if (media_scope(state) != entry.scope) { refresh_state(); return; }
    m_open_after_download.clear();
    if (entry.filesystem >= 0) {
        if (state.media_state != OrcaDeviceMediaState::Available) {
            set_error(text("设备媒体状态已变化，请检查连接与存储后重试。")); refresh_state(); return;
        }
        const auto fs = m_filesystems[entry.filesystem];
        if (!fs || entry.remote_index >= fs->GetCount() || fs->GetFile(entry.remote_index).name != entry.name) {
            set_error(text("媒体列表已变化，请重新选择。")); return;
        }
        const auto path = text(fs->GetFile(entry.remote_index).local_path);
        if (path.empty() || !wxFileExists(path)) {
            const auto directory = media_directory();
            if (!wxDirExists(directory) && !wxFileName::Mkdir(directory, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) { set_error(text("无法创建媒体下载目录。")); return; }
            m_open_after_download = std::to_string(entry.filesystem) + ":" + entry.name;
            fs->DownloadFiles(entry.remote_index, directory.ToUTF8().data());
            set_error(text("正在下载视频，完成后打开。")); return;
        }
        open_local_video(path);
        return;
    }
    if (entry.video) { open_local_video(entry.path); return; }
    wxImage image;
    if (!image.LoadFile(entry.path)) { set_error(text("图片无法读取，请确认文件仍然存在。")); return; }
    m_file_media_requested = false;
    if (m_file_media) { m_file_media->Stop(); m_file_media->Hide(); }
    m_file_toggle->Hide();
    m_media_image->Show(); m_media_visual->Layout();
    m_media_image->set_image(image); set_error({});
}

void PrinterWorkspace::open_local_video(const wxString& path)
{
    if (!video_extension(wxFileName(path).GetExt())) {
        set_error(text("此文件格式不支持视频预览，可导出到本地。")); return;
    }
    if (!m_file_media) {
        auto* player = new wxMediaCtrl;
        if (!player->Create(m_media_visual, wxID_ANY)) {
            delete player;
            if (!wxLaunchDefaultApplication(path)) set_error(text("无法打开视频，请安装支持此格式的播放器。"));
            return;
        }
        m_file_media = player;
        player->SetMinSize(FromDIP(wxSize(160, 120)));
        player->SetBackgroundColour(*wxBLACK);
        player->ShowPlayerControls(wxMEDIACTRLPLAYERCONTROLS_NONE);
        m_media_visual->GetSizer()->Insert(1, player, 1, wxEXPAND | wxALL, FromDIP(16));
        player->Bind(wxEVT_MEDIA_LOADED, [this](wxMediaEvent&) {
            BOOST_LOG_TRIVIAL(info) << "[PrinterWorkspace] local video loaded";
            if (m_destroying || !m_active || !m_file_media_requested || !m_file_media->IsShown() ||
                m_view != View::Media || m_file_media_device != media_scope(m_state)) {
                m_file_media->Stop(); return;
            }
            m_media_visual->Layout();
            // Some Windows backends finish Load before returning and then stop.
            // Start after that event has returned, while rechecking the view.
            CallAfter([this] {
                if (m_destroying || !m_active || !m_file_media_requested || m_view != View::Media ||
                    m_file_media_device != media_scope(m_state)) return;
                m_file_toggle->Enable(true);
                if (!m_file_media->Play()) set_error(text("视频播放失败，可导出后使用本地播放器查看。"));
            });
        });
        player->Bind(wxEVT_MEDIA_STATECHANGED, [this](wxMediaEvent&) {
            const auto state = m_file_media->GetState();
            m_file_toggle->SetLabel(state == wxMEDIASTATE_PLAYING ? text("暂停播放") : text("播放视频"));
            m_file_toggle->Refresh();
            BOOST_LOG_TRIVIAL(info) << "[PrinterWorkspace] local video state=" << int(state);
        });
    }
    m_file_media->Stop(); m_file_media_device = media_scope(m_state); m_file_media_requested = true;
    m_media_image->Hide(); m_file_media->Show(); m_file_toggle->Show(); m_file_toggle->Enable(false);
    m_media_visual->Layout();
    if (!m_file_media->Load(path)) {
        m_file_media_requested = false;
        m_file_media->Hide(); m_media_image->Show(); m_file_toggle->Hide(); m_media_visual->Layout();
        if (!wxLaunchDefaultApplication(path)) set_error(text("此视频格式无法播放，可导出后使用本地播放器查看。"));
        return;
    }
    set_error({});
    m_file_toggle->Enable(true);
    // Load accepting a local file is sufficient to request playback. Waiting
    // exclusively for wxEVT_MEDIA_LOADED leaves some Windows files stopped.
    CallAfter([this] {
        if (m_destroying || !m_active || !m_file_media_requested || m_view != View::Media ||
            m_file_media_device != media_scope(m_state)) return;
        const bool playing = m_file_media->Play();
        BOOST_LOG_TRIVIAL(info) << "[PrinterWorkspace] local video play requested=" << playing;
        if (!playing) set_error(text("视频播放失败，可导出后使用本地播放器查看。"));
    });
}

void PrinterWorkspace::export_media(const MediaEntry& entry)
{
    if (m_modal) return;
    if (media_scope(m_adapter.snapshot()) != entry.scope) { refresh_state(); return; }
    m_modal = true;
    if (entry.filesystem >= 0) {
        const auto fs = m_filesystems[entry.filesystem];
        wxDirDialog dialog(this, text("选择媒体导出文件夹"));
        if (dialog.ShowModal() == wxID_OK) {
            const auto state = m_adapter.snapshot();
            if (media_scope(state) == entry.scope && state.media_state == OrcaDeviceMediaState::Available &&
                fs && fs == m_filesystems[entry.filesystem] && entry.remote_index < fs->GetCount() &&
                fs->GetFile(entry.remote_index).name == entry.name) {
                fs->DownloadFiles(entry.remote_index, dialog.GetPath().ToUTF8().data()); set_error(text("正在导出媒体…"));
            } else set_error(text("设备或媒体列表已变化，导出未开始。"));
        }
    } else {
        wxFileDialog dialog(this, text("导出媒体"), {}, text(entry.name), "All files (*.*)|*.*", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dialog.ShowModal() == wxID_OK && !wxCopyFile(entry.path, dialog.GetPath(), true)) set_error(text("媒体导出失败。"));
    }
    m_modal = false; refresh_state();
}

void PrinterWorkspace::disconnect_media()
{
    m_media_device.clear(); m_open_after_download.clear();
    for (auto& fs : m_filesystems) { if (fs) { fs->Stop(true); fs.reset(); } }
    m_media_dirty = true;
}

void PrinterWorkspace::connect_media()
{
    if (!m_active || m_view != View::Media) return;
    const auto state = m_adapter.snapshot();
    if (state.media_state != OrcaDeviceMediaState::Available || state.device_id != m_state.device_id) {
        if (!m_media_device.empty()) disconnect_media();
        const wxString reason = !state.info_ready ? text("设备信息缺失或已过期，仅显示本地媒体") :
            state.media_state == OrcaDeviceMediaState::StorageUnavailable ? text("设备存储不可用，仅显示本地媒体") :
            state.media_state == OrcaDeviceMediaState::Busy ? text("设备媒体正忙，仅显示本地媒体") :
            text("设备没有可用的文件浏览协议，仅显示本地媒体");
        if (m_media_message != reason) { m_media_message = reason; m_media_dirty = true; }
        return;
    }
    if (m_media_device == m_state.device_id) return;
    disconnect_media();
    auto* manager = wxGetApp().getDeviceManager(); auto* machine = manager ? manager->get_selected_machine() : nullptr;
    if (!machine || machine->get_dev_id() != m_state.device_id) return;
    m_media_device = m_state.device_id;
    if (!machine->file_local && !machine->get_file_remote()) {
        m_media_message = text("此设备暂不支持媒体浏览，本地截图仍可查看"); return;
    }
    m_media_message.clear();
    for (int i = 0; i < 2; ++i) {
        auto fs = boost::make_shared<PrinterFileSystem>(); m_filesystems[i] = fs;
        fs->SetFileType(i == 0 ? PrinterFileSystem::F_VIDEO : PrinterFileSystem::F_TIMELAPSE);
        fs->SetGroupMode(PrinterFileSystem::G_NONE);
        const wxWeakRef<PrinterWorkspace> weak(this);
        const boost::weak_ptr<PrinterFileSystem> weak_fs(fs);
        fs->Bind(EVT_STATUS_CHANGED, [weak, weak_fs, i](wxCommandEvent& event) {
            auto current = weak_fs.lock();
            if (!weak || weak->m_destroying || !current || current != weak->m_filesystems[i]) return;
            weak->m_media_dirty = true;
            if (event.GetInt() == PrinterFileSystem::Initializing) weak->fetch_media_url(i);
        });
        for (const auto type : {EVT_FILE_CHANGED, EVT_THUMBNAIL})
            fs->Bind(type, [weak, weak_fs, i](wxCommandEvent&) {
                if (weak && !weak->m_destroying && weak_fs.lock() == weak->m_filesystems[i]) weak->m_media_dirty = true;
            });
        fs->Bind(EVT_DOWNLOAD, [weak, weak_fs, i](wxCommandEvent& event) {
            auto current = weak_fs.lock();
            if (!weak || weak->m_destroying || !current || current != weak->m_filesystems[i]) return;
            weak->m_media_dirty = true;
            if (event.GetExtraLong() > 1) {
                weak->m_open_after_download.clear(); weak->set_error(text("媒体下载失败，请检查设备存储及网络连接。"));
            } else if (event.GetExtraLong() == 0 && event.GetInt() >= 0 && size_t(event.GetInt()) < current->GetCount()) {
                const auto& file = current->GetFile(event.GetInt());
                if (std::to_string(i) + ":" + file.name == weak->m_open_after_download && !file.local_path.empty()) {
                    weak->m_open_after_download.clear();
                    weak->open_local_video(text(file.local_path));
                } else weak->set_error(text("媒体已导出。"));
            }
        });
        fs->Attached(); fs->Start();
    }
}

extern void refresh_agora_url(char const*, char const*, char const*, void*, void (*)(void*, char const*));

void PrinterWorkspace::fetch_media_url(int index)
{
    const auto fs = m_filesystems[index]; if (!fs) return;
    auto* manager = wxGetApp().getDeviceManager(); auto* machine = manager ? manager->get_selected_machine() : nullptr;
    const auto state = m_adapter.snapshot();
    if (!m_active || m_view != View::Media || !machine || machine->get_dev_id() != m_media_device ||
        state.device_id != m_media_device || state.media_state != OrcaDeviceMediaState::Available) { fs->SetUrl("0"); return; }
    if (machine->is_camera_busy_off()) { m_media_message = text("设备正忙，请稍后重新进入媒体页面"); fs->SetUrl("0"); return; }
    auto* agent = wxGetApp().getAgent();
    const std::string version = agent ? agent->get_version() : "";
    const auto device = m_media_device, firmware = machine->get_ota_version();
    const int remote = machine->get_file_remote();
    if ((machine->is_lan_mode_printer() || !remote) && machine->file_local && !machine->get_dev_ip().empty()) {
        std::string url = "bambu:///local/" + machine->get_dev_ip() + ".?port=6000&user=bblp&passwd=" + machine->get_access_code();
        url += "&device=" + device + "&net_ver=" + version + "&dev_ver=" + firmware;
        url += "&cli_id=" + wxGetApp().app_config->get("slicer_uuid") + "&cli_ver=" + std::string(SLIC3R_VERSION);
        fs->SetUrl(url); return;
    }
    if (!agent || machine->is_lan_mode_printer() || remote < 1 || remote > 3) {
        m_media_message = text("无法连接设备媒体，请检查设备地址和网络"); fs->SetUrl("0"); return;
    }
    const std::array<std::string, 4> protocols { "", "\"tutk\"", "\"agora\"", "\"tutk\",\"agora\"" };
    const wxWeakRef<PrinterWorkspace> weak(this);
    const boost::weak_ptr<PrinterFileSystem> weak_fs(fs);
    agent->get_camera_url(device + "|" + firmware + "|" + protocols[remote],
        [weak, weak_fs, index, device, firmware, version](std::string url) {
            wxGetApp().CallAfter([weak, weak_fs, index, device, firmware, version, url = std::move(url)]() mutable {
                const auto current = weak_fs.lock();
                if (!weak || weak->m_destroying || !weak->m_active || !current ||
                    current != weak->m_filesystems[index] || weak->m_media_device != device) return;
                const auto state = weak->m_adapter.snapshot();
                if (state.device_id != device || state.media_state != OrcaDeviceMediaState::Available) {
                    current->SetUrl("0"); return;
                }
                if (url.rfind("bambu:///", 0) != 0) {
                    weak->m_media_message = text("设备媒体连接失败，请检查网络后重试"); current->SetUrl("0"); return;
                }
                url += "&device=" + device + "&net_ver=" + version + "&dev_ver=" + firmware;
                url += "&refresh_url=" + boost::lexical_cast<std::string>(&refresh_agora_url);
                url += "&cli_id=" + wxGetApp().app_config->get("slicer_uuid") + "&cli_ver=" + std::string(SLIC3R_VERSION);
                current->SetUrl(url);
            });
        }, wxGetApp().get_printer_cloud_provider());
}

} // namespace Slic3r::GUI
