#include "AI/ModelGeneration/ModelGenerationInputStyle.hpp"
#include "AI/ModelGeneration/ModelGenerationConfirmation.hpp"
#include "ModelGenerationPanel.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelGenerationImageInput.hpp"
#include "AI/ColorMatching/LocalPrintColorPanel.hpp"
#include "AI/ModelGeneration/BeautyWorkbenchControls.hpp"

#include "3DScene.hpp"
#include "AI/Model/VertexColorRegionEditor.hpp"
#include "AI/Model/ModelArtifact.hpp"
#include "AI/AIWindowAppearance.hpp"
#include "AI/ModelGeneration/ModelGenerationPresentation.hpp"
#include "AI/ModelGeneration/ModelPreview3D.hpp"
#include "AI/ModelGeneration/ModelViewportFacts.hpp"
#include "AI/ModelGeneration/ModelViewportToolbar.hpp"
#include "AI/ModelGeneration/LocalModelImportState.hpp"
#include "AI/ModelGeneration/ModelImageDisplayCopy.hpp"
#include "AI/ModelGeneration/ModelGenerationStatusText.hpp"
#include "AISidecarClient.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_Utils.hpp"
#include "GLModel.hpp"
#include "GLShader.hpp"
#include "GuiColor.hpp"
#include "MsgDialog.hpp"
#include "OpenGLManager.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/TextInput.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/log/trivial.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <nlohmann/json.hpp>

#include <glad/gl.h>

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/clipbrd.h>
#include <wx/collpane.h>
#include <wx/dataobj.h>
#include <wx/dcbuffer.h>
#include <wx/dcclient.h>
#include <wx/datetime.h>
#include <wx/filedlg.h>
#include <wx/gauge.h>
#include <wx/glcanvas.h>
#include <wx/image.h>
#include <wx/simplebook.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/wrapsizer.h>
#include <wx/spinctrl.h>
#include <wx/stdpaths.h>
#include <wx/statbmp.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/numformatter.h>
#include <wx/textctrl.h>
#include <wx/tglbtn.h>
#include <wx/utils.h>
#include <wx/weakref.h>
#include <wx/wupdlock.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <utility>

namespace Slic3r::GUI {
using namespace ModelGenerationStatusText;
using namespace ModelGenerationPresentation;
namespace {

constexpr int POLL_TIMER_ID = wxID_HIGHEST + 913;

// A sibling of the existing GL host: changing facts or layout never recreates
// the canvas, reloads a model or changes its camera.
class ModelViewportInfo final : public wxPanel, public AIThemeOwner {
public:
    explicit ModelViewportInfo(wxWindow* parent) : wxPanel(parent)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        auto* grid = m_grid = new wxFlexGridSizer(3, 2, FromDIP(14), FromDIP(12));
        grid->AddGrowableCol(1);
        const std::array<wxString, 3> labels {_L("拓扑"), _L("面数"), _L("原始色值")};
        for (size_t row = 0; row < labels.size(); ++row) {
            m_labels[row] = new wxStaticText(this, wxID_ANY, labels[row]);
            m_values[row] = new wxStaticText(this, wxID_ANY, wxEmptyString,
                wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT | wxST_NO_AUTORESIZE);
            m_values[row]->SetMinSize(wxSize(1, -1));
            grid->Add(m_labels[row], 0, wxALIGN_CENTER_VERTICAL);
            grid->Add(m_values[row], 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
        }
        auto* root = new wxBoxSizer(wxVERTICAL);
        root->Add(grid, 1, wxEXPAND | wxALL, FromDIP(12));
        SetSizer(root);
        apply_ai_theme(true);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(ModelGenerationInputStyle::background)); dc.Clear();
            dc.SetPen(wxPen(ModelGenerationInputStyle::secondary.ChangeLightness(45)));
            dc.SetBrush(wxBrush(ModelGenerationInputStyle::panel));
            dc.DrawRoundedRectangle(GetClientRect(), FromDIP(8));
        });
        SetToolTip(_L("当前视口模型的实际三角面与原始色值统计；尺寸和加载说明见左侧当前模型信息。"));
        Hide();
    }

    void apply_ai_theme(bool fonts) override
    {
        using namespace ModelGenerationInputStyle;
        SetBackgroundColour(panel);
        for (size_t row = 0; row < m_labels.size(); ++row) {
            for (auto* control : {m_labels[row], m_values[row]}) {
                control->SetBackgroundColour(panel);
                if (fonts) {
                    auto font = wxGetApp().normal_font(); font.SetFaceName(font_face);
                    control->SetFont(font);
                }
            }
            m_labels[row]->SetForegroundColour(secondary);
            m_values[row]->SetForegroundColour(text);
        }
        Refresh(false);
    }

    void set_compact(bool compact)
    {
        if (compact == m_compact) return;
        m_compact = compact;
        m_grid->Clear(false);
        for (int col = 0; col < m_grid->GetCols(); ++col)
            if (m_grid->IsColGrowable(col)) m_grid->RemoveGrowableCol(col);
        m_grid->SetRows(compact ? 2 : 3);
        m_grid->SetCols(compact ? 3 : 2);
        m_grid->SetVGap(FromDIP(compact ? 6 : 14));
        if (compact) {
            for (int col = 0; col < 3; ++col) m_grid->AddGrowableCol(col);
            for (auto* label : m_labels) m_grid->Add(label, 0, wxALIGN_CENTER_VERTICAL);
            for (auto* value : m_values) m_grid->Add(value, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
        } else {
            m_grid->AddGrowableCol(1);
            for (size_t row = 0; row < m_labels.size(); ++row) {
                m_grid->Add(m_labels[row], 0, wxALIGN_CENTER_VERTICAL);
                m_grid->Add(m_values[row], 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
            }
        }
        Layout(); Refresh(false);
    }

    void set_facts(const ModelViewportFacts& facts)
    {
        const std::array<wxString, 3> values {_L("三角面"),
            wxNumberFormatter::ToString(static_cast<wxLongLong_t>(facts.triangles), wxNumberFormatter::Style_WithThousandsSep),
            wxNumberFormatter::ToString(static_cast<wxLongLong_t>(facts.colors), wxNumberFormatter::Style_WithThousandsSep)};
        bool changed = false;
        for (size_t row = 0; row < values.size(); ++row) {
            if (m_values[row]->GetLabel() == values[row]) continue;
            m_values[row]->SetLabel(values[row]); changed = true;
        }
        if (changed) { Layout(); Refresh(false); }
    }

private:
    wxFlexGridSizer* m_grid {nullptr};
    bool m_compact {false};
    std::array<wxStaticText*, 3> m_labels {};
    std::array<wxStaticText*, 3> m_values {};
};

// Native scrolling copies pixels, including stale OpenGL contents. Invalidate
// the page after moving its child windows so controls below the canvas repaint.
class ModelResultScrolledWindow final : public wxScrolledWindow
{
public:
    using wxScrolledWindow::wxScrolledWindow;
    void ScrollWindow(int dx, int dy, const wxRect* rect = nullptr) override
    {
        wxScrolledWindow::ScrollWindow(dx, dy, rect);
        Refresh();
    }
};

// Keep native scrolling and child focus behavior. Only cover the bright
// non-client scrollbar with a dark, draggable track in migrated UX panels.
class DarkScrollTrack final : public wxPanel
{
public:
    DarkScrollTrack(wxWindow* parent, wxScrolledWindow* scroll, bool layout_column = false)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE),
          m_scroll(scroll), m_layout_column(layout_column)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(wxColour(34, 34, 37)));
            dc.Clear();
            const wxRect thumb = thumb_rect();
            if (thumb.height <= 0) return;
            const int inset = FromDIP(5);
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(m_dragging ? wxColour(153, 153, 157) : wxColour(103, 103, 108)));
            dc.DrawRoundedRectangle(inset, thumb.y, std::max(FromDIP(4), GetClientSize().x - 2 * inset),
                thumb.height, FromDIP(3));
        });
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) {
            const wxRect thumb = thumb_rect();
            m_drag_offset = thumb.Contains(event.GetPosition()) ? event.GetY() - thumb.y : thumb.height / 2;
            m_dragging = true;
            CaptureMouse();
            scroll_to_track_y(event.GetY() - m_drag_offset);
        });
        Bind(wxEVT_MOTION, [this](wxMouseEvent& event) {
            if (m_dragging && event.LeftIsDown()) scroll_to_track_y(event.GetY() - m_drag_offset);
        });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { stop_drag(); });
        Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { m_dragging = false; Refresh(); });
        Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& event) {
            int x = 0, y = 0;
            m_scroll->GetViewStart(&x, &y);
            const int lines = event.GetWheelRotation() / event.GetWheelDelta() * event.GetLinesPerAction();
            m_scroll->Scroll(-1, std::max(0, y - lines));
            Refresh();
        });
    }

    void sync_geometry()
    {
        if (m_layout_column) {
            Refresh();
            return;
        }
        const wxSize outer = m_scroll->GetSize();
        const wxSize client = m_scroll->GetClientSize();
        const int width = outer.x - client.x;
        if (width <= 0) {
            Hide();
            return;
        }
        const wxPoint origin = m_scroll->GetPosition();
        SetSize(origin.x + client.x, origin.y, width, client.y);
        Show();
        Raise();
        Refresh();
    }

    void cover_during_layout()
    {
        if (m_layout_column) {
            Refresh();
            Update();
            return;
        }
        // The native bar can paint while the card sizer is rebuilt, before
        // its final client/virtual sizes are available to sync_geometry().
        const wxSize outer = m_scroll->GetSize();
        const wxSize client = m_scroll->GetClientSize();
        if (outer.x <= 0 || outer.y <= 0) return;
        const int width = outer.x - client.x;
        if (width <= 0) return;
        const wxPoint origin = m_scroll->GetPosition();
        SetSize(origin.x + client.x, origin.y, width, client.y);
        Show();
        Raise();
        Refresh();
        Update();
    }

private:
    wxRect thumb_rect() const
    {
        const int height = GetClientSize().y;
        const int viewport = m_scroll->GetClientSize().y;
        const int total = m_scroll->GetVirtualSize().y;
        if (height <= 0 || total <= viewport || viewport <= 0) return {};
        const int length = std::min(height, std::max(FromDIP(28), int(std::lround(double(height) * viewport / total))));
        int unit_x = 0, unit_y = 0;
        m_scroll->GetScrollPixelsPerUnit(&unit_x, &unit_y);
        const int start = m_scroll->GetViewStart().y * std::max(1, unit_y);
        const int top = int(std::lround(double(height - length) * std::clamp(start, 0, total - viewport) /
            (total - viewport)));
        return wxRect(0, top, GetClientSize().x, length);
    }

    void scroll_to_track_y(int top)
    {
        const wxRect thumb = thumb_rect();
        const int travel = GetClientSize().y - thumb.height;
        if (travel <= 0) return;
        const int remaining = m_scroll->GetVirtualSize().y - m_scroll->GetClientSize().y;
        int unit_x = 0, unit_y = 0;
        m_scroll->GetScrollPixelsPerUnit(&unit_x, &unit_y);
        const int pixels = int(std::lround(double(std::clamp(top, 0, travel)) * remaining / travel));
        m_scroll->Scroll(-1, int(std::lround(double(pixels) / std::max(1, unit_y))));
        Refresh();
    }

    void stop_drag()
    {
        m_dragging = false;
        if (HasCapture()) ReleaseMouse();
        Refresh();
    }

    wxScrolledWindow* m_scroll;
    bool m_layout_column;
    int m_drag_offset {0};
    bool m_dragging {false};
};

} // namespace

ModelGenerationPanel::ModelGenerationPanel(wxWindow* parent, AI::IModelArtifactConsumer& artifact_consumer,
                                           AI::IPrintablePaletteProvider& palette_provider)
    : wxPanel(parent)
    , m_artifact_consumer(artifact_consumer)
    , m_palette_provider(palette_provider)
    , m_client(AISidecarClient::default_endpoint())
    , m_poll_timer(this, POLL_TIMER_ID)
{
    SetBackgroundColour(*wxWHITE);
    Bind(wxEVT_TIMER, &ModelGenerationPanel::on_poll, this, POLL_TIMER_ID);
    // A tab may be selected before the frame is shown. Idle covers that first
    // visibility transition without constructing the hidden page at startup.
    Bind(wxEVT_IDLE, &ModelGenerationPanel::on_first_visible_idle, this);
    Bind(wxEVT_SHOW, [this](wxShowEvent& event) {
        if (event.IsShown()) {
            wxWeakRef<ModelGenerationPanel> weak(this);
            wxGetApp().CallAfter([weak] {
                if (!weak || weak->m_shutdown || !weak->IsShownOnScreen()) return;
                if (!weak->m_page_initialized) {
                    weak->initialize_page();
                    return;
                }
                weak->refresh_controls();
                weak->restore_latest_job();
                if (weak->m_library_refresh_pending) weak->load_library_entries();
                if (weak->m_model_preview_ready && weak->m_model_preview != nullptr)
                    weak->m_model_preview->refresh();
            });
        }
        event.Skip();
    });
    BOOST_LOG_TRIVIAL(info) << "AI model generation panel: initialization deferred until visible";
}

void ModelGenerationPanel::on_first_visible_idle(wxIdleEvent& event)
{
    initialize_page();
    event.Skip();
}

void ModelGenerationPanel::initialize_page()
{
    if (m_shutdown || m_page_initialized || !IsShownOnScreen()) return;
    // Paint the completed page after creating and configuring all its controls.
    wxWindowUpdateLocker update_locker(this);
    wxString trace_value;
    const bool trace = wxGetEnv("ORCASLICER_UI_LATENCY_TRACE", &trace_value) && trace_value == "1";
    const auto started = std::chrono::steady_clock::now();
    auto trace_stage = [trace, &started](const char* stage) {
        if (trace)
            BOOST_LOG_TRIVIAL(info) << "AI page first-open: " << stage << " elapsed_ms="
                << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    };
    BOOST_LOG_TRIVIAL(info) << "AI model generation panel: build visible page";
    build_page();
    trace_stage("build_page");
    attach_input_form_scroll_track();
    m_page_initialized = true;
    Unbind(wxEVT_IDLE, &ModelGenerationPanel::on_first_visible_idle, this);
    m_status->SetLabel(_L("正在检查本地 3D 生成服务..."));
    m_result_summary->SetLabel(_L("本地服务就绪后即可使用 3D 生成功能。"));
    if (m_service_availability_known)
        set_service_availability(m_service_available);
    else
        refresh_controls();
    trace_stage("refresh_controls");
    refresh_ai_appearance(this);
    trace_stage("refresh_ai_appearance");
    refresh_input_layout();
    Layout();
    trace_stage("layout");
    BOOST_LOG_TRIVIAL(info) << "AI model generation panel: visible page initialized";
}

ModelGenerationPanel::~ModelGenerationPanel()
{
    shutdown();
}

void ModelGenerationPanel::set_service_availability(bool available, const std::string& message)
{
    if (m_shutdown)
        return;
    m_service_available = available;
    m_service_availability_known = true;
    if (!available && !message.empty())
        BOOST_LOG_TRIVIAL(warning) << "AI model generation service unavailable: " << message;
    if (!m_page_initialized) return;
    if (available && !m_busy) {
        m_status->SetLabel(_L("本地 3D 生成服务已就绪。"));
        m_result_summary->SetLabel(_L("输入描述、选择参考图，或同时提供两者即可开始。"));
        update_workflow();
        restore_latest_job();
    } else if (!m_busy) {
        m_status->SetLabel(_L("本地生成服务未启动。点击“重新检测服务”即可恢复。"));
        m_result_summary->SetLabel(_L("服务恢复后会自动载入最近任务，当前本地模型不会丢失。"));
    }
    refresh_controls();
}

void ModelGenerationPanel::set_service_retry_handler(std::function<void()> handler)
{
    m_service_retry_handler = std::move(handler);
    refresh_controls();
}

void ModelGenerationPanel::show_input_hint(const wxString& message, wxWindow* focus)
{
    m_status->SetLabel(message);
    m_status->InvalidateBestSize();
    m_status->GetParent()->Layout();
    m_workflow_panel->Layout();
    Layout();
    if (focus != nullptr) focus->SetFocus();
}

void ModelGenerationPanel::restore_latest_job()
{
    if (m_shutdown || !m_page_initialized || !IsShownOnScreen() ||
        !m_service_available || m_restore_checked || !m_job_id.empty())
        return;
    m_restore_checked = true;
    const uint64_t sequence = ++m_sequence;
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.get_latest(
        [weak, sequence](std::optional<AIModelGenerationClient::JobStatus> status) mutable {
            if (!weak || !status) return;
            wxGetApp().CallAfter([weak, sequence, status = std::move(*status)]() mutable {
                if (weak) weak->restore_job(std::move(status), sequence);
            });
        },
        [weak, sequence](std::string error) {
            if (!weak) return;
            BOOST_LOG_TRIVIAL(warning) << "Unable to restore the latest generated-model job: " << error;
            wxGetApp().CallAfter([weak, sequence, error = std::move(error)] {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence || !weak->m_job_id.empty()) return;
                if (is_transient_sidecar_poll_error(error)) {
                    weak->m_restore_checked = false;
                    weak->set_service_availability(false, error);
                    if (weak->m_service_retry_handler) weak->m_service_retry_handler();
                }
            });
        });
}

void ModelGenerationPanel::restore_job(AIModelGenerationClient::JobStatus status, uint64_t sequence)
{
    if (m_shutdown || sequence != m_sequence || !m_job_id.empty())
        return;
    if (m_prepare_base) m_prepare_base->SetValue(false);
    m_job_palette = status.palette;
    m_job_palette_color_count = status.palette_color_count;
    m_legacy_generation_state.restore_palette_color_count(status.palette_color_count);
    m_job_palette_roles = status.palette_roles.empty() ? automatic_palette_roles(status.palette) : status.palette_roles;
    m_palette_roles = m_job_palette_roles;
    m_palette_roles_source = status.palette;
    if (status.palette_recommendation.confirmed && !status.palette.empty()) {
        m_palette_recommendation_confirmed = true;
        m_custom_palette = status.palette;
        m_legacy_generation_state.palette_source = 2;
    }
    m_job_use_printable_colors = !status.palette.empty() || status.palette_recommendation.available;
    m_job_preview_expected = status.source == "image" || status.preview_ready || status.raw_preview_ready ||
                             status.model_reference_ready ||
                             !status.palette.empty();
    m_palette = status.palette;
    m_job_style = status.style;
    m_job_custom_style = status.custom_style;
    m_palette_quality_ok = status.palette_quality_ok;
    m_material_fragmentation_ok = status.material_fragmentation_ok;
    m_model_input_eligible = status.model_input_eligible;
    m_model_input_primary_blocker = status.model_input_blockers.empty() ? std::string() : status.model_input_blockers.front();
    m_meaningful_palette_count = status.meaningful_palette_count;
    m_meaningful_subject_color_count = status.meaningful_subject_color_count;
    m_job_print_settings = status.print_settings;
    m_job_face_limit = status.generation_options.face_limit;
    m_job_generation_options = status.generation_options;
    m_legacy_generation_defaults = status.face_limit != m_job_face_limit;
    m_job_generation_profile = status.generation_profile == "performance" ? "performance" : "quality";
    m_job_prompt = wxString::FromUTF8(status.user_prompt);
    if (m_prompt != nullptr)
        m_prompt->SetValue(m_job_prompt);
    if (m_style != nullptr) {
        m_style->SetSelection(style_selection(status.style));
        m_stylized_style->SetSelection(stylized_style_selection(status.style));
        m_style_user_selected = true;
    }
    if (m_custom_style != nullptr)
        m_custom_style->SetValue(wxString::FromUTF8(status.custom_style));
    m_custom_palette = status.palette;
    m_legacy_generation_state.palette_source = m_job_use_printable_colors ? 2 : 1;
    m_palette_recommendation_confirmed = !status.palette.empty();
    if (m_provider) m_provider->SetSelection(status.generation_options.provider == "hunyuan" ? 1 : 0);
    refresh_provider_options();
    if (m_quality != nullptr) {
        m_quality->SetSelection(m_job_face_limit <= 300000 ? 0 : m_job_face_limit == 2000000 && m_quality->GetCount() == 3 ? 2 : 1);
    }
    if (m_geometry_quality) m_geometry_quality->SetSelection(m_geometry_quality->GetCount() > 1 && status.generation_options.geometry_quality == "detailed" ? 1 : 0);
    if (m_texture_quality) m_texture_quality->SetSelection(m_texture_quality->GetCount() == 1 ? 0 : status.generation_options.texture_quality == "extreme" ? 2 :
                                                         status.generation_options.texture_quality == "detailed" ? 1 : 0);
    if (m_output_format) m_output_format->SetSelection(status.generation_options.output_format == "obj" ? 1 : 0);
    m_legacy_generation_state.restore_print_settings(status.print_settings.width_mm,
        status.print_settings.nozzle_mm, status.print_settings.line_width_mm,
        status.print_settings.minimum_feature_mm);
    m_job_image_path.clear();
    if (status.source == "image" && status.input_ready) {
        m_job_image_path = temp_path(status.id + "-input", "png");
        m_selected_image_path = m_job_image_path;
        m_restoring_input = true;
    }
    // Widget setters and legacy numeric restoration may normalize saved values.
    // Rebase the comparison snapshot on the restored values so opening
    // a completed preview cannot immediately mark that same preview as stale.
    m_job_prompt = m_prompt->GetValue();
    m_job_style = current_style();
    m_job_custom_style = current_custom_style();
    m_job_use_printable_colors = use_printable_colors();
    m_job_palette = current_palette();
    m_job_print_settings = current_print_settings();
    m_status->SetLabel(_L("正在恢复上次模型生成任务..."));
    handle_status(std::move(status), sequence);
    // The persisted job owns the confirmed semantic material mapping. Widget
    // refreshes may infer generic light/chroma defaults while restore is still
    // asynchronous; never let that overwrite skin-versus-garment ownership.
    m_palette_roles = m_job_palette_roles;
    m_palette_roles_source = m_job_palette;
    if (m_restoring_input)
        download_restored_input(sequence);
}

void ModelGenerationPanel::download_restored_input(uint64_t sequence)
{
    const std::string job_id = m_job_id;
    const boost::filesystem::path path = m_job_image_path;
    if (job_id.empty() || path.empty())
        return;
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.download_input(job_id, path,
        [weak, sequence](boost::filesystem::path restored) {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, restored = std::move(restored)]() mutable {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence) return;
                weak->m_selected_image_path = restored;
                weak->m_job_image_path = restored;
                weak->m_reference_image_path = restored;
                weak->m_restoring_input = false;
                weak->m_selected_image->SetLabel(_L("已恢复上次参考图"));
                weak->show_selected_image_preview();
                // The restore is now fully materialized. Capture the effective UI
                // values once more after loading the image so asynchronous restore
                // order cannot turn the just-restored job into a stale job.
                weak->m_job_prompt = weak->m_prompt->GetValue();
                weak->m_job_style = weak->current_style();
                weak->m_job_custom_style = weak->current_custom_style();
                weak->m_job_use_printable_colors = weak->use_printable_colors();
                weak->m_job_palette = weak->current_palette();
                weak->m_palette_roles = weak->m_job_palette_roles;
                weak->m_palette_roles_source = weak->m_job_palette;
                weak->m_job_print_settings = weak->current_print_settings();
                if (!weak->m_preview_output_available && !weak->m_style_preview_ready &&
                    (weak->m_job_state == "failed" || weak->m_job_state == "stopped" ||
                     weak->m_job_state == "cancelled")) {
                    weak->m_style_preview_placeholder = _L("预览不可用");
                    weak->m_preview_message->SetLabel(weak->m_job_state == "failed"
                        ? _L("历史任务未生成可用的 AI 设计图。")
                        : _L("历史任务已停止，未生成可用的 AI 设计图。"));
                    weak->update_preview_view();
                }
                // Restoring an already chosen style must not issue a new
                // recommendation: that request cancels concurrent downloads.
                weak->refresh_style_recommendation();
                if (!weak->m_awaiting_palette_confirmation && weak->m_preview_output_available &&
                    !weak->m_preview_download_in_flight && !weak->m_style_preview_ready)
                    weak->download_preview(sequence);
                weak->refresh_controls();
            });
        },
        [weak, sequence](std::string error) {
            if (!weak) return;
            BOOST_LOG_TRIVIAL(warning) << "Unable to restore the generated-model input image: " << error;
            wxGetApp().CallAfter([weak, sequence]() {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence) return;
                weak->m_restoring_input = false;
                // Http cancellation/error must not trigger a speculative
                // preview request when the persisted job has no preview.
                const bool preview_available = weak->m_preview_output_available;
                weak->m_selected_image_path.clear();
                weak->m_job_image_path.clear();
                weak->m_reference_image_path.clear();
                weak->m_preview_path.clear();
                weak->m_style_preview_ready = false;
                weak->m_preview_message->SetLabel(_L("历史参考图恢复失败，当前预览不可用。"));
                const wxString previous_status = weak->m_status->GetLabel();
                weak->m_status->SetLabel(_L("无法恢复历史参考图。") +
                    (previous_status.empty() ? wxString() : wxString("\n") + previous_status));
                if (preview_available) {
                    weak->m_preview_message->SetLabel(_L("历史参考图恢复失败，正在加载已保存的 AI 设计图。"));
                    const wxString terminal_summary = weak->m_result_summary->GetLabel();
                    const wxString recovery_hint = _L("原始任务原因已保留；AI 设计图仍可查看，恢复参考图后可继续操作。");
                    weak->m_result_summary->SetLabel(terminal_summary.empty()
                        ? recovery_hint : terminal_summary + _L("\n") + recovery_hint);
                    weak->download_preview(sequence);
                } else {
                    weak->m_style_preview_placeholder = _L("预览不可用");
                    const wxString terminal_summary = weak->m_result_summary->GetLabel();
                    const wxString recovery_hint = _L("历史任务和原始失败原因已保留；请重新选择图片后再试。");
                    weak->m_result_summary->SetLabel(terminal_summary.empty()
                        ? recovery_hint : terminal_summary + _L("\n") + recovery_hint);
                }
                weak->refresh_controls();
            });
        });
}

void ModelGenerationPanel::shutdown()
{
    if (m_shutdown)
        return;
    m_shutdown = true;
    if (m_preview_canceled) *m_preview_canceled = true;
    if (auto* local = local_model_import_state(m_library_import); local && local->cancel)
        local->cancel->store(true);
    if (m_workbench_color_matching) m_workbench_color_matching->shutdown();
    stop_library_loading();
    stop_model_finishing();
    if (m_preview_worker.joinable()) m_preview_worker.join();
    if (m_library_import_worker.joinable()) m_library_import_worker.join();
    if (auto* local = local_model_import_state(m_library_import); local && local->cancel && local->rollback_after_join)
        local->rollback_after_join();
    ++m_sequence;
    m_poll_timer.Stop();
    const bool preview_download_was_active = m_preview_download_in_flight;
    m_preview_download_in_flight = false;
    if (preview_download_was_active) {
        m_preview_path.clear();
        m_style_preview_ready = false;
    }
    m_client.cancel_current();
    cleanup_files();
}

wxWindow* ModelGenerationPanel::build_preview_panel(wxWindow* parent)
{
    wxString trace_value;
    const bool trace = wxGetEnv("ORCASLICER_UI_LATENCY_TRACE", &trace_value) && trace_value == "1";
    const auto started = std::chrono::steady_clock::now();
    auto trace_stage = [trace, &started](const char* stage) {
        if (trace)
            BOOST_LOG_TRIVIAL(info) << "AI preview build: " << stage << " elapsed_ms="
                << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    };
    auto* panel = new ModelGenerationInputStyle::RoundedPanel(parent);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* header = new wxWrapSizer(wxHORIZONTAL);
    m_preview_kind = new wxStaticText(panel, wxID_ANY, _L("结果对照"));
    m_preview_kind->SetForegroundColour(wxColour(91, 104, 107));
    m_preview_details_pane = new wxCollapsiblePane(panel, wxID_ANY, _L("多视图"), wxDefaultPosition,
        wxDefaultSize, wxCP_DEFAULT_STYLE | wxCP_NO_TLW_RESIZE);
    wxWindow* preview_details_parent = m_preview_details_pane->GetPane();
    wxArrayString preview_stages;
    preview_stages.Add(_L("AI 设计图"));
    preview_stages.Add(_L("模型多视图"));
    m_preview_stage = new wxChoice(preview_details_parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, preview_stages);
    m_preview_stage->SetSelection(0);
    m_preview_stage_hint = new Label(panel, _L("生成后可在这里确认图片效果。"), LB_AUTO_WRAP);
    m_preview_stage_hint->SetMinSize(wxSize(1, -1));
    m_preview_stage_hint->SetForegroundColour(wxColour(91, 104, 107));
    m_zoom_out = new Button(panel, "-");
    m_zoom_out->SetName("input_field");
    m_zoom_out->SetPaddingSize(FromDIP(wxSize(8, 5)));
    m_zoom_fit = new Button(panel, _L("图片适应"));
    m_zoom_fit->SetName("input_field");
    m_zoom_fit->SetPaddingSize(FromDIP(wxSize(8, 5)));
    m_zoom_in = new Button(panel, "+");
    m_zoom_in->SetName("input_field");
    m_zoom_in->SetPaddingSize(FromDIP(wxSize(8, 5)));
    m_preview_zoom = new wxStaticText(panel, wxID_ANY, "100%", wxDefaultPosition, wxSize(FromDIP(48), -1), wxALIGN_CENTER_HORIZONTAL);
    m_zoom_out->SetToolTip(_L("缩小图片预览"));
    m_zoom_fit->SetToolTip(_L("完整显示图片"));
    m_zoom_in->SetToolTip(_L("放大图片预览"));
    header->Add(m_preview_kind, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    header->Add(m_zoom_out, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    header->Add(m_zoom_fit, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(4));
    header->Add(m_zoom_in, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(4));
    header->Add(m_preview_zoom, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
    sizer->Add(header, 0, wxEXPAND | wxALL, FromDIP(18));
    sizer->Add(m_preview_stage_hint, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(18));
    auto* preview_stage_row = new wxBoxSizer(wxHORIZONTAL);
    preview_stage_row->Add(new wxStaticText(preview_details_parent, wxID_ANY, _L("查看")), 0,
                           wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    preview_stage_row->Add(m_preview_stage, 0, wxALIGN_CENTER_VERTICAL);
    m_preview_technical_details = new wxStaticText(
        preview_details_parent, wxID_ANY, _L("多视图来自生成模型，用于查看不同角度的形体。"));
    m_preview_technical_details->SetForegroundColour(wxColour(91, 104, 107));
    m_preview_technical_details->Wrap(FromDIP(740));
    auto* preview_details_sizer = new wxBoxSizer(wxVERTICAL);
    preview_details_sizer->Add(preview_stage_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
    preview_details_sizer->Add(m_preview_technical_details, 0, wxEXPAND | wxALL, FromDIP(8));
    preview_details_parent->SetSizerAndFit(preview_details_sizer);
    m_preview_details_pane->Collapse(true);
    m_preview_details_pane->Bind(wxEVT_COLLAPSIBLEPANE_CHANGED, [panel, preview_details_parent](wxCollapsiblePaneEvent& event) {
        preview_details_parent->Layout();
        panel->Layout();
        if (panel->GetParent() != nullptr)
            panel->GetParent()->Layout();
        event.Skip();
    });
    sizer->Add(m_preview_details_pane, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(18));
    trace_stage("header_and_details");

    m_preview_book = new wxSimplebook(panel, wxID_ANY);
    m_preview_book->SetMinSize(wxSize(1, 1));
    auto* model_page = new ModelResultScrolledWindow(
        m_preview_book, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxHSCROLL | wxVSCROLL);
    model_page->SetBackgroundColour(wxColour(241, 244, 245));
    model_page->SetScrollRate(FromDIP(12), FromDIP(12));
    model_page->SetMinSize(wxSize(1, 1));
    auto* model_sizer = new wxBoxSizer(wxVERTICAL);
    auto* comparison_panel = new wxPanel(model_page);
    m_comparison_panel = comparison_panel;
    m_model_page = model_page;
    comparison_panel->SetBackgroundColour(wxColour(241, 244, 245));
    auto* comparison_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_preview_area = new wxScrolledWindow(
        comparison_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxHSCROLL | wxVSCROLL);
    m_preview_area->SetBackgroundColour(wxColour(241, 244, 245));
    m_preview_area->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_preview_area->SetMinSize(wxSize(1, 1));
    m_preview_area->SetScrollRate(FromDIP(12), FromDIP(12));
    m_preview_area->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(m_preview_area);
        dc.SetBackground(wxBrush(m_preview_area->GetBackgroundColour()));
        dc.Clear();
        if (m_reference_preview_pane.IsEmpty() && m_style_preview_pane.IsEmpty())
            return;
        int view_x = 0;
        int view_y = 0;
        int unit_x = 1;
        int unit_y = 1;
        m_preview_area->GetViewStart(&view_x, &view_y);
        m_preview_area->GetScrollPixelsPerUnit(&unit_x, &unit_y);
        const wxPoint offset(view_x * unit_x, view_y * unit_y);
        const int label_height = FromDIP(32);

        auto draw_pane = [&](const wxRect& virtual_rect, const wxString& label, const wxBitmap& bitmap,
                             const wxString& placeholder, bool ai_result) {
            if (virtual_rect.IsEmpty())
                return;
            wxRect rect = virtual_rect;
            rect.Offset(-offset.x, -offset.y);
            dc.SetPen(wxPen(ModelGenerationInputStyle::background));
            dc.SetBrush(wxBrush(ModelGenerationInputStyle::field));
            dc.DrawRectangle(rect);

            const wxRect label_rect(rect.x, rect.y, rect.width, label_height);
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(ai_result ? ModelGenerationInputStyle::selected : ModelGenerationInputStyle::panel));
            dc.DrawRectangle(label_rect);
            wxFont label_font = dc.GetFont();
            label_font.SetWeight(wxFONTWEIGHT_BOLD);
            dc.SetFont(label_font);
            dc.SetTextForeground(ai_result ? ModelGenerationInputStyle::yellow : ModelGenerationInputStyle::text);
            const wxSize label_size = dc.GetTextExtent(label);
            dc.DrawText(label, label_rect.x + FromDIP(10), label_rect.y + (label_rect.height - label_size.y) / 2);

            const wxRect image_rect(rect.x, rect.y + label_height, rect.width, rect.height - label_height);
            if (bitmap.IsOk()) {
                const int x = image_rect.x + (image_rect.width - bitmap.GetWidth()) / 2;
                const int y = image_rect.y + (image_rect.height - bitmap.GetHeight()) / 2;
                // Paint beneath alpha in either pane; never composite it into
                // the reference or generation file.
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.SetBrush(wxBrush(wxColour(160, 160, 160)));
                dc.DrawRectangle(x, y, bitmap.GetWidth(), bitmap.GetHeight());
                dc.DrawBitmap(bitmap, x, y, true);
            } else if (!placeholder.empty()) {
                wxFont placeholder_font = dc.GetFont();
                placeholder_font.SetWeight(wxFONTWEIGHT_NORMAL);
                dc.SetFont(placeholder_font);
                dc.SetTextForeground(ModelGenerationInputStyle::secondary);
                const wxSize text_size = dc.GetTextExtent(placeholder);
                dc.DrawText(placeholder,
                            image_rect.x + std::max(FromDIP(8), (image_rect.width - text_size.x) / 2),
                            image_rect.y + std::max(FromDIP(8), (image_rect.height - text_size.y) / 2));
            }
        };

        const wxString reference_placeholder = m_library_model_loaded
            ? _L("该历史记录未保存原图")
            : _L("文字生成，无原图");
        draw_pane(m_reference_preview_pane, _L("原图"), m_reference_bitmap, reference_placeholder, false);
        wxString result_label = m_preview_stage != nullptr && m_preview_stage->GetSelection() != wxNOT_FOUND
            ? m_preview_stage->GetStringSelection() : _L("AI 生成图");
        if (m_design_preview_stale) result_label += _L(" · 旧版");
        draw_pane(m_style_preview_pane, result_label, m_style_preview_bitmap, m_style_preview_placeholder, true);
    });
    m_preview_area->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        update_preview_view();
        event.Skip();
    });
    trace_stage("image_controls");
    auto* model_card = new wxPanel(comparison_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    model_card->SetBackgroundColour(*wxWHITE);
    // Let the toolbar and canvas determine the minimum height. A fixed 560 DIP
    // card clips the model below the visible result page on short windows.
    model_card->SetMinSize(wxSize(1, 1));
    auto* model_card_sizer = new wxBoxSizer(wxVERTICAL);
    auto* model_toolbar = new wxWrapSizer(wxHORIZONTAL);
    m_model_stats = new Label(model_card, _L("3D 模型 · 生成后将在这里显示"), LB_AUTO_WRAP);
    m_model_stats->SetMinSize(wxSize(1, -1));
    m_model_stats->SetName("input_secondary");
    m_model_stats->SetForegroundColour(wxColour(91, 104, 107));
    m_front_model_view = new Button(model_card, _L("完整显示模型"));
    m_front_model_view->SetName("input_field");
    m_front_model_view->SetMinSize(FromDIP(wxSize(-1, 30)));
    m_front_model_view->SetPaddingSize(FromDIP(wxSize(12, 4)));
    m_front_model_view->SetToolTip(_L("恢复规范正面并自动适应画布大小"));
    m_reset_model_view = new Button(model_card, _L("三维视角"));
    m_reset_model_view->SetName("input_field");
    m_reset_model_view->SetMinSize(FromDIP(wxSize(-1, 30)));
    m_reset_model_view->SetPaddingSize(FromDIP(wxSize(12, 4)));
    m_reset_model_view->SetToolTip(_L("恢复便于检查侧面和底座的三维观察角度"));
    model_toolbar->Add(m_front_model_view, 0, wxLEFT, FromDIP(8));
    model_toolbar->Add(m_reset_model_view, 0, wxLEFT, FromDIP(6));
    model_card_sizer->Add(model_toolbar, 0, wxEXPAND | wxALL, FromDIP(10));
    model_card_sizer->Add(m_model_stats, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    m_finishing_compare_model = new Button(model_card, _L("查看处理前"));
    m_finishing_compare_model->SetName("input_field");
    m_finishing_compare_model->SetMinSize(FromDIP(wxSize(-1, 30)));
    m_finishing_compare_model->SetPaddingSize(FromDIP(wxSize(12, 4)));
    m_finishing_compare_model->Hide();
    model_card_sizer->Add(m_finishing_compare_model, 0, wxLEFT | wxBOTTOM, FromDIP(10));
    trace_stage("model_toolbar");
    m_model_preview = new ModelPreview3D(model_card);
    trace_stage("model_canvas");
    m_model_preview->set_preview_background(ModelGenerationInputStyle::background);
    m_model_preview->SetMinSize(wxSize(1, 1));
    model_card_sizer->Add(m_model_preview, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    // Keep the overlay beside the native GL child, not above an ancestor panel:
    // GL composition on Windows can cover an overlay in the outer card.
    auto* viewport_info = new ModelViewportInfo(m_model_preview);
    auto show_viewport_info = std::make_shared<bool>(true);
    auto* viewport_tools = new ModelViewportToolbar(m_model_preview);
    auto* viewport_info_toggle = new Button(model_card, _L("隐藏信息"));
    viewport_info_toggle->SetName("input_field");
    viewport_info_toggle->SetMinSize(FromDIP(wxSize(-1, 30)));
    viewport_info_toggle->SetPaddingSize(FromDIP(wxSize(12, 4)));
    viewport_info_toggle->SetToolTip(_L("显示或隐藏当前视口模型统计，不改变模型、视角和编辑草稿。"));
    model_toolbar->Add(viewport_info_toggle, 0, wxLEFT, FromDIP(6));
    auto sync_viewport_info = [this, viewport_info, show_viewport_info, viewport_tools] {
        const wxSize available=m_model_preview->GetClientSize();
        const int toolbar_width=std::min(FromDIP(407),available.x-FromDIP(16));
        const int toolbar_height=viewport_tools->height_for_width(toolbar_width);
        const bool toolbar_visible=m_model_preview_ready && !m_preview_loading &&
            m_workspace_view==WorkspaceView::Model && !m_finishing_workbench &&
            m_model_preview->IsShown() && toolbar_width>=FromDIP(56) &&
            available.y>=toolbar_height+FromDIP(16);
        viewport_tools->Show(toolbar_visible);
        if(toolbar_visible) {
            const wxRect bounds((available.x-toolbar_width)/2,
                available.y-toolbar_height-FromDIP(8),toolbar_width,toolbar_height);
            if(viewport_tools->GetRect()!=bounds)viewport_tools->SetSize(bounds);
            viewport_tools->arrange(toolbar_width);
            viewport_tools->Raise();
        } else m_model_preview->set_auto_rotation(false);
        const bool enabled=toolbar_visible && !m_busy;
        viewport_tools->set_state(ModelViewportToolbar::Texture,enabled,!m_model_preview->gray_view());
        viewport_tools->set_state(ModelViewportToolbar::Mesh,enabled,m_model_preview->wireframe_view());
        viewport_tools->set_state(ModelViewportToolbar::Info,enabled,*show_viewport_info);
        viewport_tools->set_state(ModelViewportToolbar::Rotate,enabled,m_model_preview->auto_rotation());
        const auto* facts = dynamic_cast<const ModelViewportFacts*>(m_model_stats->GetClientObject());
        const wxRect canvas = m_model_preview->GetRect();
        const int inset = FromDIP(8);
        const bool visible = *show_viewport_info && m_model_preview_ready && !m_preview_loading && facts &&
            (m_workspace_view == WorkspaceView::Model || m_finishing_workbench) &&
            m_model_preview->IsShown() && canvas.width >= FromDIP(160);
        const bool changed = viewport_info->Show(visible);
        const int bottom = toolbar_visible ? toolbar_height + 2 * inset : 0;
        if (!visible) { m_model_preview->set_overlay_insets(0, bottom); return; }
        const bool compact = available.x < FromDIP(520) || available.y < FromDIP(480);
        viewport_info->set_compact(compact);
        viewport_info->set_facts(*facts);
        const int width = std::min(FromDIP(compact ? 407 : 250), canvas.width - 2 * inset);
        const wxRect bounds(wxPoint(inset, inset),
            wxSize(width, viewport_info->GetSizer()->CalcMin().y));
        if (viewport_info->GetRect() != bounds) {
            viewport_info->SetSize(bounds); viewport_info->Layout(); viewport_info->Raise();
            BOOST_LOG_TRIVIAL(info) << "AI viewport facts layout: canvas="
                << canvas.width << 'x' << canvas.height << ", card="
                << bounds.x << ',' << bounds.y << ',' << bounds.width << ',' << bounds.height;
        } else if (changed) viewport_info->Raise();
        m_model_preview->set_overlay_insets(bounds.GetBottom() + 1 + inset, bottom);
    };
    viewport_tools->on_tool=[this,viewport_info_toggle,show_viewport_info,sync_viewport_info](ModelViewportToolbar::Tool tool) {
        if(m_busy || !m_model_preview_ready || m_finishing_workbench)return;
        if(tool==ModelViewportToolbar::Texture) {
            const bool gray=!m_model_preview->gray_view();m_model_preview->set_gray_view(gray);
            if(m_finishing_gray)m_finishing_gray->SetValue(gray);
        } else if(tool==ModelViewportToolbar::Mesh)
            m_model_preview->set_wireframe_view(!m_model_preview->wireframe_view());
        else if(tool==ModelViewportToolbar::Info) {
            *show_viewport_info=!*show_viewport_info;
            viewport_info_toggle->SetLabel(*show_viewport_info?_L("隐藏信息"):_L("显示信息"));
        } else if(tool==ModelViewportToolbar::Rotate)
            m_model_preview->set_auto_rotation(!m_model_preview->auto_rotation());
        sync_viewport_info();
    };
    viewport_info_toggle->Bind(wxEVT_BUTTON, [viewport_info_toggle, show_viewport_info, sync_viewport_info](wxCommandEvent&) {
        *show_viewport_info = !*show_viewport_info;
        viewport_info_toggle->SetLabel(*show_viewport_info ? _L("隐藏信息") : _L("显示信息"));
        sync_viewport_info();
    });
    m_model_preview->Bind(wxEVT_SIZE, [sync_viewport_info](wxSizeEvent& event) {
        sync_viewport_info(); event.Skip();
    });
    model_card->Bind(wxEVT_UPDATE_UI, [sync_viewport_info](wxUpdateUIEvent& event) {
        sync_viewport_info(); event.Skip();
    });
    model_card->SetSizer(model_card_sizer);
    comparison_sizer->Add(model_card, 5, wxEXPAND | wxRIGHT, FromDIP(10));
    comparison_sizer->Add(m_preview_area, 4, wxEXPAND);
    comparison_sizer->Add(build_model_finishing(comparison_panel), 0, wxEXPAND | wxLEFT, FromDIP(10));
    trace_stage("finishing_controls");
    comparison_panel->SetSizer(comparison_sizer);
    attach_finishing_editor_scroll_track();
    auto* expand_images = m_expand_images = new wxToggleButton(panel, wxID_ANY, _L("展开图片"));
    header->Add(expand_images, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
    expand_images->Bind(wxEVT_TOGGLEBUTTON, [this, expand_images](wxCommandEvent&) {
        select_workspace_view(expand_images->GetValue() ? WorkspaceView::Image : WorkspaceView::Model);
    });
    model_page->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        refresh_comparison_layout();
        event.Skip();
    });
    model_sizer->Add(comparison_panel, 1, wxEXPAND | wxALL, FromDIP(12));
    auto* finishing_shortcut = m_finishing_shortcut = new Button(panel, _L("3D 美颜工作台"));
    finishing_shortcut->SetName("input_field");
    finishing_shortcut->SetPaddingSize(FromDIP(wxSize(12, 8)));
    header->Add(finishing_shortcut, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
    finishing_shortcut->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_finishing_workbench) {
            set_finishing_workbench(false);
            return;
        }
        if (workbench_access(m_model_preview_ready,
                is_nonempty_model(m_displayed_model_path), m_preview_loading || m_preview_download_in_flight,
                m_busy) != WorkbenchAccess::Available) {
            show_input_hint(m_finishing_shortcut->GetToolTipText());
            return;
        }
        open_model_finishing();
    });
    trace_stage("comparison_shortcuts");

    m_model_decision_panel = new wxPanel(
        model_page, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
    auto* decision_sizer = new wxBoxSizer(wxVERTICAL);
    m_model_decision_status = new wxStaticText(m_model_decision_panel, wxID_ANY, _L("尚未检查"));
    wxFont decision_font = m_model_decision_status->GetFont();
    decision_font.SetWeight(wxFONTWEIGHT_BOLD);
    m_model_decision_status->SetFont(decision_font);
    m_model_decision_summary = new wxStaticText(
        m_model_decision_panel, wxID_ANY, _L("模型生成或加载后会显示是否适合继续导入。"));
    m_model_decision_summary->Wrap(FromDIP(620));
    m_model_decision_status->Hide();
    m_model_decision_summary->Hide();
    m_model_decision_panel->SetSizer(decision_sizer);
    m_model_decision_panel->Hide();
    wxWindow* model_advanced_parent = model_page;
    m_model_quality_panel = new wxPanel(
        model_advanced_parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
    auto* quality_sizer = new wxBoxSizer(wxVERTICAL);
    auto* quality_header = new wxBoxSizer(wxHORIZONTAL);
    m_model_quality_status = new wxStaticText(m_model_quality_panel, wxID_ANY, _L("尚未检查"));
    wxFont quality_font = m_model_quality_status->GetFont();
    quality_font.SetWeight(wxFONTWEIGHT_BOLD);
    m_model_quality_status->SetFont(quality_font);
    m_recheck_model = new wxButton(m_model_quality_panel, wxID_ANY, _L("重新检查"));
    m_recheck_model->SetToolTip(_L("使用本地结构门禁重新检查当前模型，不会调用付费 AI"));
    m_locate_thin_regions = new wxButton(m_model_quality_panel, wxID_ANY, _L("定位薄壁"));
    m_locate_thin_regions->SetToolTip(
        _L("高亮本地厚度采样命中的薄壁面片；结果用于复核，不会自动修改模型"));
    m_locate_overhang_regions = new wxButton(m_model_quality_panel, wxID_ANY, _L("定位悬垂面"));
    m_locate_overhang_regions->SetToolTip(
        _L("高亮显著的离床向下面，便于旋转检查；不会自动添加支撑或改变切片参数"));
    quality_header->Add(m_model_quality_status, 1, wxALIGN_CENTER_VERTICAL);
    quality_header->Add(m_locate_thin_regions, 0, wxLEFT, FromDIP(12));
    quality_header->Add(m_locate_overhang_regions, 0, wxLEFT, FromDIP(12));
    quality_header->Add(m_recheck_model, 0, wxLEFT, FromDIP(12));
    quality_sizer->Add(quality_header, 0, wxEXPAND | wxALL, FromDIP(10));
    m_model_quality_summary = new wxStaticText(m_model_quality_panel, wxID_ANY, _L("模型生成或加载后可进行结构检查。"));
    m_model_quality_summary->Wrap(FromDIP(500));
    quality_sizer->Add(m_model_quality_summary, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    m_model_quality_details_pane = new wxCollapsiblePane(m_model_quality_panel, wxID_ANY, _L("查看检查指标"), wxDefaultPosition,
        wxDefaultSize, wxCP_DEFAULT_STYLE | wxCP_NO_TLW_RESIZE);
    auto* quality_details_sizer = new wxBoxSizer(wxVERTICAL);
    m_model_quality_details = new wxStaticText(m_model_quality_details_pane->GetPane(), wxID_ANY, wxEmptyString);
    m_model_quality_details->Wrap(FromDIP(480));
    quality_details_sizer->Add(m_model_quality_details, 0, wxEXPAND | wxALL, FromDIP(8));
    m_model_quality_details_pane->GetPane()->SetSizer(quality_details_sizer);
    quality_sizer->Add(m_model_quality_details_pane, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    m_model_quality_details_pane->Bind(wxEVT_COLLAPSIBLEPANE_CHANGED, [this, model_page](wxCollapsiblePaneEvent& event) {
        m_model_quality_panel->Layout();
        model_page->Layout();
        model_page->FitInside();
        if (m_model_preview != nullptr)
            m_model_preview->refresh();
        event.Skip();
    });
    auto* visual_header = new wxBoxSizer(wxHORIZONTAL);
    m_visual_quality_status = new wxStaticText(m_model_quality_panel, wxID_ANY, _L("AI 视觉复核：未运行"));
    wxFont visual_font = m_visual_quality_status->GetFont();
    visual_font.SetWeight(wxFONTWEIGHT_BOLD);
    m_visual_quality_status->SetFont(visual_font);
    m_visual_review_model = new wxButton(m_model_quality_panel, wxID_ANY, _L("AI 视觉复核"));
    m_visual_review_model->SetToolTip(_L("生成最终模型五视图，对照原图检查人脸、主体和材料串色；检查仅作提示，不阻止导入"));
    visual_header->Add(m_visual_quality_status, 1, wxALIGN_CENTER_VERTICAL);
    visual_header->Add(m_visual_review_model, 0, wxLEFT, FromDIP(12));
    quality_sizer->Add(visual_header, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
    m_visual_quality_summary = new wxStaticText(m_model_quality_panel, wxID_ANY,
        _L("模型准备好后可按需生成五视图并进行 AI 外观复核。"));
    m_visual_quality_summary->Wrap(FromDIP(500));
    quality_sizer->Add(m_visual_quality_summary, 0, wxEXPAND | wxALL, FromDIP(10));

    m_model_refinement_panel = new wxPanel(m_model_quality_panel);
    auto* refinement_sizer = new wxBoxSizer(wxVERTICAL);
    auto* refinement_header = new wxBoxSizer(wxHORIZONTAL);
    m_model_refinement_status = new wxStaticText(
        m_model_refinement_panel, wxID_ANY, _L("下一次生成优化"));
    wxFont refinement_font = m_model_refinement_status->GetFont();
    refinement_font.SetWeight(wxFONTWEIGHT_BOLD);
    m_model_refinement_status->SetFont(refinement_font);
    m_apply_model_refinement = new wxButton(
        m_model_refinement_panel, wxID_ANY, _L("应用到下一次生成"));
    m_apply_model_refinement->SetToolTip(
        _L("把本地质量建议加入文字输入；不会立即调用 Image2、Tripo 或其他付费服务"));
    refinement_header->Add(m_model_refinement_status, 1, wxALIGN_CENTER_VERTICAL);
    refinement_header->Add(m_apply_model_refinement, 0, wxLEFT, FromDIP(12));
    refinement_sizer->Add(refinement_header, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
    m_model_refinement_summary = new wxStaticText(m_model_refinement_panel, wxID_ANY, wxEmptyString);
    m_model_refinement_summary->SetForegroundColour(wxColour(91, 104, 107));
    m_model_refinement_summary->Wrap(FromDIP(500));
    refinement_sizer->Add(m_model_refinement_summary, 0, wxEXPAND | wxALL, FromDIP(10));
    m_model_refinement_panel->SetSizer(refinement_sizer);
    m_model_refinement_panel->Hide();
    quality_sizer->Add(m_model_refinement_panel, 0, wxEXPAND);
    m_model_quality_panel->SetSizer(quality_sizer);
    m_model_quality_panel->Hide();
    trace_stage("quality_controls");
    m_model_preview_message = new wxStaticText(
        model_page, wxID_ANY, _L("拖动模型旋转，滚轮缩放；点击“完整显示模型”恢复全貌。图片缩放请切换到图像页。"));
    m_model_preview_message->SetForegroundColour(wxColour(91, 104, 107));
    model_sizer->Add(m_model_preview_message, 0, wxEXPAND | wxALL, FromDIP(12));
    model_page->SetSizer(model_sizer);
    model_page->FitInside();
    m_preview_book->AddPage(model_page, _L("结果对照"), true);
    trace_stage("result_layout");
    m_library_view_page = build_model_library(m_preview_book);
    m_preview_book->AddPage(m_library_view_page, _L("历史资产"), false);
    trace_stage("library_controls");
    sizer->Add(m_preview_book, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(18));

    m_preview_message = new Label(panel, _L("请先输入描述或选择参考图。"), LB_AUTO_WRAP);
    m_preview_message->SetMinSize(wxSize(1, -1));
    m_preview_message->SetForegroundColour(wxColour(91, 104, 107));
    sizer->Add(m_preview_message, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(18));

    m_result_summary = new Label(panel, _L("尚未生成模型。"), LB_AUTO_WRAP);
    m_result_summary->SetMinSize(wxSize(1, -1));
    sizer->Add(m_result_summary, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(18));
    panel->SetSizer(sizer);

    m_zoom_out->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { set_preview_zoom(m_preview_zoom_factor / 1.25); });
    m_zoom_fit->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { set_preview_zoom(1.0); });
    m_zoom_in->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { set_preview_zoom(m_preview_zoom_factor * 1.25); });
    m_preview_stage->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { apply_preview_stage(true); });
    m_reset_model_view->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_model_preview != nullptr)
            m_model_preview->reset_view();
    });
    m_front_model_view->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_model_preview != nullptr)
            m_model_preview->front_view();
    });
    m_recheck_model->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_recheck_model, this);
    m_locate_thin_regions->Bind(wxEVT_BUTTON, [this, model_page](wxCommandEvent&) {
        if (m_model_preview == nullptr)
            return;
        const bool has_ranked_region = !m_model_quality.thin_local_regions.empty() &&
            !m_model_quality.thin_local_regions.front().face_indices.empty();
        size_t region_index = 0;
        const std::vector<size_t>* evidence = &m_model_quality.thin_local_face_indices;
        if (has_ranked_region) {
            region_index = m_thin_region_navigation_active
                ? (m_thin_region_navigation_index + 1) % m_model_quality.thin_local_regions.size()
                : 0;
            evidence = &m_model_quality.thin_local_regions[region_index].face_indices;
        }
        const size_t localized = m_model_preview->select_face_evidence(*evidence);
        if (localized == 0) {
            if (m_model_preview->region_selection_preparing()) {
                m_status->SetLabel(_L("正在准备局部选择，完成后自动定位。")); return;
            }
            m_status->SetLabel(_L("当前质量报告没有可定位的局部薄壁证据。"));
            return;
        }
        m_thin_region_navigation_active = has_ranked_region;
        m_thin_region_navigation_index = region_index;
        model_page->Layout();
        model_page->FitInside();
        if (has_ranked_region) {
            const size_t region_count = std::max(
                m_model_quality.thin_local_region_count, m_model_quality.thin_local_regions.size());
            wxString preview_message = wxString::Format(
                _L("已高亮薄壁风险区 %llu/%llu 的 %llu 个证据面（共识别 %llu 个）"),
                static_cast<unsigned long long>(region_index + 1),
                static_cast<unsigned long long>(m_model_quality.thin_local_regions.size()),
                static_cast<unsigned long long>(localized),
                static_cast<unsigned long long>(region_count));
            const auto& region = m_model_quality.thin_local_regions[region_index];
            const wxString metrics = thin_local_region_metrics(
                region,
                m_model_quality.local_wall_thickness_threshold_available,
                m_model_quality.minimum_local_wall_thickness_mm);
            if (!metrics.empty())
                preview_message += _L(" · ") + metrics;
            preview_message += _L("；可旋转复核或手动增减。");
            m_model_preview_message->SetLabel(preview_message);
            m_status->SetLabel(thin_local_region_status(
                region_index,
                m_model_quality.thin_local_regions.size(),
                region,
                m_model_quality.local_wall_thickness_threshold_available,
                m_model_quality.minimum_local_wall_thickness_mm));
        } else {
            m_model_preview_message->SetLabel(wxString::Format(
                _L("已高亮 %llu 个局部薄壁采样面；可旋转复核，或在局部区域工具中手动增减。"),
                static_cast<unsigned long long>(localized)));
            m_status->SetLabel(_L("已定位局部薄壁证据；这里只做风险复核，不会自动修改模型。"));
        }
    });
    m_locate_overhang_regions->Bind(wxEVT_BUTTON, [this, model_page](wxCommandEvent&) {
        if (m_model_preview == nullptr)
            return;
        const size_t localized = m_model_preview->select_elevated_overhang_regions();
        if (localized == 0) {
            if (m_model_preview->region_selection_preparing()) {
                m_status->SetLabel(_L("正在准备局部选择，完成后自动定位。")); return;
            }
            m_status->SetLabel(_L("当前模型没有达到显著阈值的离床悬垂区域。"));
            return;
        }
        model_page->Layout();
        model_page->FitInside();
        m_model_preview_message->SetLabel(wxString::Format(
            _L("已高亮 %llu 个悬垂三角面；可旋转检查，或在局部区域工具中手动增减。"),
            static_cast<unsigned long long>(localized)));
        m_status->SetLabel(_L("已定位显著局部悬垂；这里只做风险复核，不会自动生成支撑。"));
    });
    m_visual_review_model->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_visual_review_model, this);
    m_apply_model_refinement->Bind(wxEVT_BUTTON, &ModelGenerationPanel::on_apply_model_refinement, this);
    m_model_preview->set_selection_changed_callback([this](size_t selected_faces) {
        if (m_finishing_workbench) return;
        bool matched_region = false;
        size_t matched_region_index = 0;
        if (m_model_preview != nullptr) {
            for (size_t index = 0; index < m_model_quality.thin_local_regions.size(); ++index) {
                if (!m_model_preview->selection_matches_face_evidence(
                        m_model_quality.thin_local_regions[index].face_indices))
                    continue;
                m_thin_region_navigation_active = true;
                m_thin_region_navigation_index = index;
                matched_region = true;
                matched_region_index = index;
                break;
            }
        }
        if (!matched_region) {
            m_thin_region_navigation_active = false;
            m_thin_region_navigation_index = 0;
        }
        if (m_model_preview_message != nullptr) {
            m_model_preview_message->SetLabel(selected_faces == 0
                ? _L("生成完成后可拖动旋转模型，并使用滚轮缩放。")
                : wxString::Format(_L("当前选区包含 %llu 个三角面；可继续检查或手动增减。"),
                                   static_cast<unsigned long long>(selected_faces)));
        }
        if (m_status != nullptr) {
            if (matched_region) {
                m_status->SetLabel(thin_local_region_status(
                    matched_region_index,
                    m_model_quality.thin_local_regions.size(),
                    m_model_quality.thin_local_regions[matched_region_index],
                    m_model_quality.local_wall_thickness_threshold_available,
                    m_model_quality.minimum_local_wall_thickness_mm));
            } else {
                m_status->SetLabel(selected_faces == 0
                    ? _L("当前未选择局部区域。")
                    : _L("已选择局部区域；可继续复核或手动增减。"));
            }
        }
        refresh_local_recolor_controls();
    });
    m_preview_book->Bind(wxEVT_BOOKCTRL_PAGE_CHANGED, [this, panel, expand_images](wxBookCtrlEvent& event) {
        const int selection = event.GetSelection();
        const bool result_page = selection == m_preview_book->FindPage(m_model_page);
        const bool library_page = selection == m_preview_book->FindPage(m_library_view_page);
        if (!m_selecting_workspace) {
            m_workspace_view = library_page ? WorkspaceView::Library : result_page ? m_result_view : WorkspaceView::Model;
        }
        m_zoom_out->Show(result_page && !m_finishing_workbench);
        m_zoom_fit->Show(result_page && !m_finishing_workbench);
        m_zoom_in->Show(result_page && !m_finishing_workbench);
        m_preview_zoom->Show(result_page && !m_finishing_workbench);
        expand_images->Show(result_page && !m_finishing_workbench);

        m_preview_details_pane->Show(result_page && m_model_views_available);
        m_preview_kind->SetLabel(result_page ? (m_finishing_workbench ? _L("美颜工作台") : _L("结果对照"))
            : selection == 1 ? _L("历史资产") : _L("颜色匹配"));
        if (library_page) {
            wxWeakRef<ModelGenerationPanel> weak(this);
            wxGetApp().CallAfter([weak] {
                if (weak && !weak->m_shutdown) weak->load_library_entries();
            });
        } else cancel_library_loading();
        panel->Layout();
        if (result_page && m_model_preview != nullptr) m_model_preview->refresh();
        if (m_page_initialized && !m_selecting_workspace) refresh_controls();
        event.Skip();
    });
    trace_stage("event_bindings");
    return panel;
}

wxWindow* ModelGenerationPanel::build_model_library(wxWindow* parent)
{
    class LibraryScrollWindow final : public wxScrolledWindow {
    public:
        using wxScrolledWindow::wxScrolledWindow;
        bool Layout() override {
            if (!GetSizer()) return wxScrolledWindow::Layout();
            // wxScrollHelper's default layout uses client height when the native
            // bar is hidden. Our separate track still needs the full virtual row.
            const wxSize client = GetClientSize();
            GetSizer()->SetDimension(CalcScrolledPosition(wxPoint(0, 0)),
                wxSize(client.x, std::max(client.y, GetVirtualSize().y)));
            return true;
        }
    };
    class LibrarySearch final : public TextInput, public AIThemeOwner {
    public:
        explicit LibrarySearch(wxWindow* parent) : TextInput(parent, wxEmptyString, wxEmptyString,
            "figma-ux/library-search", wxDefaultPosition, parent->FromDIP(wxSize(340, 36))) {}
        void apply_ai_theme(bool fonts) override {
            using namespace ModelGenerationInputStyle;
            SetBackgroundColour(panel);
            const wxColour search_fill = field;
            SetBackgroundColor(search_fill);
            SetBorderColor(StateColor(std::make_pair(yellow, int(StateColor::Focused)),
                std::make_pair(secondary, int(StateColor::Normal))));
            SetCornerRadius(FromDIP(18));
            SetTextColor(text);
            GetTextCtrl()->SetBackgroundColour(search_fill);
            GetTextCtrl()->SetForegroundColour(text);
            if (fonts) { auto font = GetTextCtrl()->GetFont(); font.SetFaceName(font_face); GetTextCtrl()->SetFont(font); }
        }
    };
    auto* panel = new ModelGenerationInputStyle::RoundedPanel(parent);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto action = [this, panel](const wxString& label, const wxString& icon = wxEmptyString) {
        auto* button = new Button(panel, label, icon, 0, icon.empty() ? 0 : 20);
        button->SetName("input_quiet");
        button->SetMinSize(FromDIP(wxSize(-1, 36)));
        button->SetPaddingSize(FromDIP(wxSize(14, 6)));
        return button;
    };
    auto* heading = new wxBoxSizer(wxHORIZONTAL);
    m_library_title = section_label(panel, _L("我的资产"));
    heading->Add(m_library_title, 1, wxALIGN_CENTER_VERTICAL);
    m_library_import = action(_L("导入模型"), "figma-ux/library-import");
    m_library_import->SetClientObject(new LocalModelImportState);
    m_library_import->SetToolTip(_L("从本机导入模型，原始文件保留。"));
    m_library_import->Bind(wxEVT_BUTTON, [this](wxCommandEvent& event) {
        const wxString before = m_status->GetLabel();
        if (auto* local=local_model_import_state(m_library_import); local && local->cancel)
            on_stop(event);
        else choose_local_model();
        show_library_action_feedback(before);
    });
    heading->Add(m_library_import, 0);
    m_library_full_controls.push_back(m_library_import);
    sizer->Add(heading, 0, wxEXPAND | wxALL, FromDIP(16));
    auto* filter_row = new wxBoxSizer(wxHORIZONTAL);
    auto* search_label = new wxStaticText(panel, wxID_ANY, _L("搜索名称 / Task ID"));
    filter_row->Add(search_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
    m_library_full_controls.push_back(search_label);
    auto* search = new LibrarySearch(panel);
    search->SetMinSize(FromDIP(wxSize(160, 36)));
    m_library_search = search->GetTextCtrl();
    m_library_search->SetName("library_search_text");
    m_library_search->SetToolTip(_L("搜索完整名称、本地 ID、Task ID 或转换任务 ID；不区分英文大小写。"));
    filter_row->Add(search, 1, wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(12));
    const std::array<wxString, 4> filters {_L("全部"), _L("我的图片"), _L("我的模型"), _L("美颜版本")};
    for (size_t i = 0; i < filters.size(); ++i) {
        auto* button = action(filters[i]);
        button->SetName(i == 0 ? "input_quiet" : "input_field");
        m_library_filter_buttons[i] = button;
        m_library_full_controls.push_back(button);
        button->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent&) {
            m_library_category = static_cast<ModelLibraryCategory>(i);
            apply_library_filter();
        });
        filter_row->Add(button, 0, wxRIGHT, FromDIP(8));
    }
    auto* clear = action(_L("重置筛选"));
    clear->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        m_library_search->ChangeValue(wxEmptyString);
        m_library_category = ModelLibraryCategory::All;
        apply_library_filter();
    });
    filter_row->Add(clear, 0);
    m_library_full_controls.push_back(clear);
    sizer->Add(filter_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    m_library_filter_timer.SetOwner(this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { apply_library_filter(); }, m_library_filter_timer.GetId());
    m_library_search->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { m_library_filter_timer.StartOnce(250); });
    m_library_search->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { apply_library_filter(); });
    m_library_search->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) {
        if (event.GetKeyCode() == WXK_ESCAPE && !m_library_search->IsEmpty()) {
            m_library_search->ChangeValue(wxEmptyString);
            apply_library_filter();
        } else event.Skip();
    });
    auto* session = new wxStaticText(panel, wxID_ANY,
        _L("本地设计与模型 · 单击选中，再加载使用；原件和已保存版本分别保留。"));
    session->SetName("input_secondary");
    m_library_full_controls.push_back(session);
    sizer->Add(session, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    m_library_empty = new wxStaticText(panel, wxID_ANY, _L("还没有保存的设计图或模型。"), wxDefaultPosition, wxSize(1, -1), wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
    m_library_empty->SetName("input_secondary");
    sizer->Add(m_library_empty, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    m_library_scroller = new LibraryScrollWindow(panel, wxID_ANY, wxDefaultPosition,
        FromDIP(wxSize(-1, 180)), wxVSCROLL | wxBORDER_NONE | wxCLIP_CHILDREN);
    m_library_scroller->SetMinSize(wxSize(1, 1));
    m_library_scroller->SetScrollRate(0, FromDIP(12));
    // Keep scrolling native, but let the adjacent dark track own its layout.
    // An overlapping sibling cannot reliably cover MSW non-client repaints.
    m_library_scroller->ShowScrollbars(wxSHOW_SB_DEFAULT, wxSHOW_SB_NEVER);
    m_library_sizer = new wxBoxSizer(wxVERTICAL);
    m_library_scroller->SetSizer(m_library_sizer);
    m_library_scroller->Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& event) {
        const int key = event.GetKeyCode();
        if (event.HasModifiers() || (key != WXK_PAGEUP && key != WXK_PAGEDOWN &&
            key != WXK_HOME && key != WXK_END)) {
            event.Skip();
            return;
        }
        // On MSW a thumbnail panel's PageUp/Down can become notebook navigation.
        // Only the card list owns these unmodified keys; search and shortcuts
        // outside this scroller keep their native handling.
        int unit_x = 0, unit_y = 0, view_x = 0, view_y = 0;
        m_library_scroller->GetScrollPixelsPerUnit(&unit_x, &unit_y);
        m_library_scroller->GetViewStart(&view_x, &view_y);
        if (unit_y <= 0) return;
        const int height = m_library_scroller->GetClientSize().y;
        const int last = (std::max(0, m_library_scroller->GetVirtualSize().y - height) + unit_y - 1) / unit_y;
        const int page = std::max(1, height / unit_y - 1);
        const int next = key == WXK_HOME ? 0 : key == WXK_END ? last :
            view_y + (key == WXK_PAGEUP ? -page : page);
        m_library_scroller->Scroll(view_x, std::clamp(next, 0, last));
        if (m_library_scroll_track) m_library_scroll_track->Refresh();
    });
    auto* scroll_row = new wxBoxSizer(wxHORIZONTAL);
    scroll_row->Add(m_library_scroller, 1, wxEXPAND);
    auto* scroll_track = new DarkScrollTrack(panel, m_library_scroller, true);
    m_library_scroll_track = scroll_track;
    scroll_track->SetMinSize(FromDIP(wxSize(17, 1)));
    scroll_row->Add(scroll_track, 0, wxEXPAND);
    sizer->Add(scroll_row, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
    const auto update_track = [scroll_track](wxScrollWinEvent& event) {
        scroll_track->Refresh();
        event.Skip();
    };
    m_library_scroller->Bind(wxEVT_SCROLLWIN_TOP, update_track);
    m_library_scroller->Bind(wxEVT_SCROLLWIN_BOTTOM, update_track);
    m_library_scroller->Bind(wxEVT_SCROLLWIN_LINEUP, update_track);
    m_library_scroller->Bind(wxEVT_SCROLLWIN_LINEDOWN, update_track);
    m_library_scroller->Bind(wxEVT_SCROLLWIN_PAGEUP, update_track);
    m_library_scroller->Bind(wxEVT_SCROLLWIN_PAGEDOWN, update_track);
    m_library_scroller->Bind(wxEVT_SCROLLWIN_THUMBTRACK, update_track);
    m_library_scroller->Bind(wxEVT_SCROLLWIN_THUMBRELEASE, update_track);
    auto* navigation = new wxBoxSizer(wxHORIZONTAL);
    auto* refresh = action(_L("刷新"));
    refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { load_library_entries(); });
    navigation->Add(refresh, 0, wxRIGHT, FromDIP(12));
    m_library_full_controls.push_back(refresh);
    m_library_page_label = new wxStaticText(panel, wxID_ANY, _L("正在读取本地资产…"));
    m_library_page_label->SetName("input_secondary");
    navigation->Add(m_library_page_label, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    m_library_previous = action(_L("上一页"));
    m_library_previous->Disable();
    m_library_previous->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_library_page == 0) return;
        --m_library_page;
        m_library_scroller->Scroll(0, 0);
        refresh_library();
    });
    navigation->Add(m_library_previous, 0, wxRIGHT, FromDIP(8));
    m_library_next = action(_L("下一页"));
    m_library_next->Disable();
    m_library_next->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if ((m_library_page + 1) * m_library_page_size >= m_library_filtered_indices.size()) return;
        ++m_library_page;
        m_library_scroller->Scroll(0, 0);
        refresh_library();
    });
    navigation->Add(m_library_next, 0);
    sizer->Add(navigation, 0, wxEXPAND | wxALL, FromDIP(16));
    m_library_full_controls.push_back(m_library_page_label);
    m_library_full_controls.push_back(m_library_previous);
    m_library_full_controls.push_back(m_library_next);
    m_library_drawer_footer = new ModelGenerationInputStyle::RoundedPanel(panel);
    auto* drawer_footer = new wxBoxSizer(wxVERTICAL);
    m_library_drawer_selection = new wxStaticText(m_library_drawer_footer, wxID_ANY,
        _L("选择图片，再打开历史设计"), wxDefaultPosition, FromDIP(wxSize(1, 22)), wxST_ELLIPSIZE_END);
    drawer_footer->Add(m_library_drawer_selection, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    m_library_drawer_use = new Button(m_library_drawer_footer, _L("打开设计"));
    m_library_drawer_use->SetName("input_primary");
    m_library_drawer_use->SetMinSize(FromDIP(wxSize(-1, 36)));
    m_library_drawer_use->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_busy || m_design_history_loading || m_finishing_running) return;
        const auto entry = std::find_if(m_library_entries.begin(), m_library_entries.end(),
            [this](const auto& value) { return value.job_id == m_library_selected_asset_id; });
        if (entry == m_library_entries.end()) return;
        const wxString before = m_status->GetLabel();
        if (entry->design_only) load_design_library_entry(entry->job_id);
        else {
            // Copy the path before callbacks can rebuild the shared snapshot.
            const auto image = entry->ai_image_path;
            if (replace_input_image(image)) {
                set_library_drawer(false);
                select_workspace_view(ModelGenerationPresentation::WorkspaceView::Image);
            }
        }
        show_library_action_feedback(before);
    });
    drawer_footer->Add(m_library_drawer_use, 0, wxEXPAND | wxBOTTOM, FromDIP(12));
    auto* drawer_pager = new wxBoxSizer(wxHORIZONTAL);
    for (size_t i = 0; i < 4; ++i) {
        if (i == 2) {
            m_library_drawer_pages = new wxStaticText(m_library_drawer_footer, wxID_ANY, "1 / 1");
            drawer_pager->Add(m_library_drawer_pages, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(12));
        }
        auto* button = new Button(m_library_drawer_footer, wxEmptyString,
            i == 0 || i == 3 ? "figma-ux/drawer-arrows" : "figma-ux/drawer-arrow", 0, 16);
        if (i < 2) {
            auto bitmap = create_scaled_bitmap(i == 0 ? "figma-ux/drawer-arrows" : "figma-ux/drawer-arrow", panel, 16);
            // Same original asset; horizontal reflection is the Figma wrapper transform.
            button->SetIcon(wxBitmap(bitmap.ConvertToImage().Mirror(true)));
        }
        button->SetName("input_field");
        button->SetMinSize(FromDIP(wxSize(32, 36)));
        button->SetPaddingSize(FromDIP(wxSize(8, 6)));
        const std::array<wxString, 4> labels {_L("第一页"), _L("上一页"), _L("下一页"), _L("最后一页")};
        button->SetToolTip(labels[i]);
        button->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent&) {
            const size_t last = m_library_filtered_indices.empty() ? 0 : (m_library_filtered_indices.size()-1)/m_library_page_size;
            m_library_page = i == 0 ? 0 : i == 1 ? (m_library_page ? m_library_page-1 : 0) : i == 2 ? std::min(last,m_library_page+1) : last;
            m_library_scroller->Scroll(0, 0);
            refresh_library();
        });
        button->SetName("input_quiet");
        m_library_drawer_pager[i] = button;
        drawer_pager->Add(button, 0, wxLEFT, FromDIP(6));
    }
    drawer_footer->Add(drawer_pager, 0, wxEXPAND);
    m_library_drawer_footer->SetSizer(drawer_footer);
    sizer->Add(m_library_drawer_footer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    m_library_drawer_footer->Hide();
    m_library_timer.SetOwner(this);
    Bind(wxEVT_TIMER, &ModelGenerationPanel::on_library_timer, this, m_library_timer.GetId());
    m_library_scroller->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        if (m_library_refresh_pending) cover_library_scroll_track_during_layout();
        else sync_library_scroll_track();
        // Reflow only this page after native resize settles; do not rescan disk.
        if (!m_shutdown && !m_library_refresh_pending && m_library_scroller->IsShownOnScreen())
            m_library_timer.Start(100);
        event.Skip();
    });
    m_library_scroller->Bind(wxEVT_SHOW, [this](wxShowEvent& event) {
        if (!event.IsShown()) cancel_library_loading();
        event.Skip();
    });
    panel->SetSizer(sizer);
    return panel;
}

void ModelGenerationPanel::sync_library_scroll_track()
{
    if (m_library_scroll_track && m_library_scroller)
        static_cast<DarkScrollTrack*>(m_library_scroll_track)->sync_geometry();
}

void ModelGenerationPanel::cover_library_scroll_track_during_layout()
{
    if (m_library_scroll_track && m_library_scroller)
        static_cast<DarkScrollTrack*>(m_library_scroll_track)->cover_during_layout();
}

void ModelGenerationPanel::attach_model_overview_scroll_track()
{
    auto* overview = static_cast<wxScrolledWindow*>(m_model_overview);
    auto* track = new DarkScrollTrack(overview->GetParent(), overview);
    m_model_overview_scroll_track = track;
    track->Hide();
    const auto update_track = [track](wxScrollWinEvent& event) {
        track->Refresh();
        event.Skip();
    };
    overview->Bind(wxEVT_SCROLLWIN_TOP, update_track);
    overview->Bind(wxEVT_SCROLLWIN_BOTTOM, update_track);
    overview->Bind(wxEVT_SCROLLWIN_LINEUP, update_track);
    overview->Bind(wxEVT_SCROLLWIN_LINEDOWN, update_track);
    overview->Bind(wxEVT_SCROLLWIN_PAGEUP, update_track);
    overview->Bind(wxEVT_SCROLLWIN_PAGEDOWN, update_track);
    overview->Bind(wxEVT_SCROLLWIN_THUMBTRACK, update_track);
    overview->Bind(wxEVT_SCROLLWIN_THUMBRELEASE, update_track);
    overview->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        sync_model_overview_scroll_track();
        event.Skip();
    });
}

void ModelGenerationPanel::sync_model_overview_scroll_track()
{
    if (!m_model_overview_scroll_track || !m_model_overview) return;
    if (!m_model_overview->IsShown()) {
        m_model_overview_scroll_track->Hide();
        return;
    }
    static_cast<DarkScrollTrack*>(m_model_overview_scroll_track)->sync_geometry();
}

void ModelGenerationPanel::attach_input_form_scroll_track()
{
    auto* form = static_cast<wxScrolledWindow*>(m_input_form);
    auto* track = new DarkScrollTrack(form->GetParent(), form);
    m_input_form_scroll_track = track;
    track->Hide();
    const auto update_track = [track](wxScrollWinEvent& event) {
        track->Refresh();
        event.Skip();
    };
    form->Bind(wxEVT_SCROLLWIN_TOP, update_track);
    form->Bind(wxEVT_SCROLLWIN_BOTTOM, update_track);
    form->Bind(wxEVT_SCROLLWIN_LINEUP, update_track);
    form->Bind(wxEVT_SCROLLWIN_LINEDOWN, update_track);
    form->Bind(wxEVT_SCROLLWIN_PAGEUP, update_track);
    form->Bind(wxEVT_SCROLLWIN_PAGEDOWN, update_track);
    form->Bind(wxEVT_SCROLLWIN_THUMBTRACK, update_track);
    form->Bind(wxEVT_SCROLLWIN_THUMBRELEASE, update_track);
    form->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        sync_input_form_scroll_track();
        event.Skip();
    });
    form->Bind(wxEVT_PAINT, [this](wxPaintEvent& event) {
        event.Skip();
        wxWeakRef<ModelGenerationPanel> weak(this);
        CallAfter([weak] {
            if (weak && weak->m_input_form->IsShownOnScreen())
                weak->sync_input_form_scroll_track();
        });
    });
}

void ModelGenerationPanel::sync_input_form_scroll_track()
{
    if (!m_input_form_scroll_track || !m_input_form) return;
    if (!m_input_form->IsShownOnScreen()) {
        m_input_form_scroll_track->Hide();
        return;
    }
    static_cast<DarkScrollTrack*>(m_input_form_scroll_track)->sync_geometry();
}

void ModelGenerationPanel::attach_finishing_editor_scroll_track()
{
    auto* editor = m_finishing_editor_scroll;
    auto* track = new DarkScrollTrack(editor->GetParent(), editor);
    m_finishing_editor_scroll_track = track;
    track->Hide();
    const auto update_track = [track](wxScrollWinEvent& event) {
        track->Refresh();
        event.Skip();
    };
    editor->Bind(wxEVT_SCROLLWIN_TOP, update_track);
    editor->Bind(wxEVT_SCROLLWIN_BOTTOM, update_track);
    editor->Bind(wxEVT_SCROLLWIN_LINEUP, update_track);
    editor->Bind(wxEVT_SCROLLWIN_LINEDOWN, update_track);
    editor->Bind(wxEVT_SCROLLWIN_PAGEUP, update_track);
    editor->Bind(wxEVT_SCROLLWIN_PAGEDOWN, update_track);
    editor->Bind(wxEVT_SCROLLWIN_THUMBTRACK, update_track);
    editor->Bind(wxEVT_SCROLLWIN_THUMBRELEASE, update_track);
    editor->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        sync_finishing_editor_scroll_track();
        event.Skip();
    });
    editor->Bind(wxEVT_PAINT, [this](wxPaintEvent& event) {
        event.Skip();
        wxWeakRef<ModelGenerationPanel> weak(this);
        CallAfter([weak] {
            if (weak && weak->m_finishing_editor_scroll->IsShownOnScreen())
                weak->sync_finishing_editor_scroll_track();
        });
    });
}

void ModelGenerationPanel::sync_finishing_editor_scroll_track()
{
    if (!m_finishing_editor_scroll_track || !m_finishing_editor_scroll) return;
    if (!m_finishing_panel->IsShown()) {
        m_finishing_editor_scroll_track->Hide();
        return;
    }
    static_cast<DarkScrollTrack*>(m_finishing_editor_scroll_track)->sync_geometry();
}

void ModelGenerationPanel::on_choose_image(wxCommandEvent&)
{
    if (!input_editable()) return;
    wxString initial_directory;
    const boost::filesystem::path generated_root = generated_models_root();
    const auto is_user_image_directory = [&generated_root](const boost::filesystem::path& directory) {
        if (!boost::filesystem::is_directory(directory))
            return false;
        boost::system::error_code root_ec;
        boost::system::error_code directory_ec;
        const boost::filesystem::path canonical_root = boost::filesystem::canonical(generated_root, root_ec);
        const boost::filesystem::path canonical_directory = boost::filesystem::canonical(directory, directory_ec);
        return root_ec || directory_ec ||
               (canonical_directory != canonical_root && !path_is_inside(canonical_root, canonical_directory));
    };
    const boost::filesystem::path selected_directory = m_selected_image_path.parent_path();
    if (is_user_image_directory(selected_directory))
        initial_directory = wxString::FromUTF8(selected_directory.string());
    if (wxGetApp().app_config != nullptr) {
        const std::string saved = wxGetApp().app_config->get("model_generation_image_directory");
        if (initial_directory.empty() && !saved.empty() &&
            is_user_image_directory(boost::filesystem::path(saved)))
            initial_directory = wxString::FromUTF8(saved);
    }
    if (initial_directory.empty())
        initial_directory = wxStandardPaths::Get().GetUserDir(wxStandardPaths::Dir_Pictures);
    wxFileDialog dialog(this, _L("选择参考图"), initial_directory, wxEmptyString,
                        _L("PNG 和 JPEG 图片 (*.png;*.jpg;*.jpeg)|*.png;*.jpg;*.jpeg"), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK)
        return;
    replace_input_image(boost::filesystem::path(dialog.GetPath().ToStdWstring()));
}

bool ModelGenerationPanel::input_editable() const
{
    return m_page_initialized && !m_shutdown && !m_busy && !m_preview_download_in_flight &&
           !m_preview_loading && !m_finishing_running && !m_design_history_loading && !m_saving_generation_options;
}

bool ModelGenerationPanel::accept_input_files(const wxArrayString& paths)
{
    if (!input_editable()) return false;
    if (paths.size() != 1) {
        show_input_hint(_L("一次只能选择一张参考图；当前图片未改变。"));
        return false;
    }
    return replace_input_image(boost::filesystem::path(paths.front().ToStdWstring()));
}

void ModelGenerationPanel::paste_input_image()
{
    if (!input_editable()) return;
    wxClipboardLocker clipboard;
    if (!clipboard) {
        show_input_hint(_L("暂时无法读取剪贴板，请稍后重试。"));
        return;
    }
    // File copies retain the original format, size and EXIF; don't re-encode them.
    if (wxTheClipboard->IsSupported(wxDF_FILENAME)) {
        wxFileDataObject files;
        if (wxTheClipboard->GetData(files)) accept_input_files(files.GetFilenames());
        return;
    }
    if (!wxTheClipboard->IsSupported(wxDF_BITMAP)) {
        show_input_hint(_L("剪贴板中没有图片。文字请粘贴到描述词框。"));
        return;
    }
    wxBitmapDataObject bitmap;
    if (!wxTheClipboard->GetData(bitmap) || !bitmap.GetBitmap().IsOk()) {
        show_input_hint(_L("无法读取剪贴板图片；当前图片未改变。"));
        return;
    }
    const auto size = bitmap.GetBitmap().GetSize();
    if (size.x < 64 || size.y < 64 || uint64_t(size.x) * uint64_t(size.y) > MAX_INPUT_IMAGE_PIXELS) {
        show_input_hint(_L("图片宽高至少为 64 px，总像素不能超过 1677 万；当前图片未改变。"));
        return;
    }
    const auto path = temp_path("clipboard-" + new_request_id(), "png");
    const bool saved = bitmap.GetBitmap().ConvertToImage().SaveFile(path.wstring(), wxBITMAP_TYPE_PNG);
    if (!saved) show_input_hint(_L("无法保存剪贴板图片，请检查本地目录空间和权限；当前图片未改变。"));
    if (!saved || !replace_input_image(path)) {
        boost::system::error_code ignored;
        boost::filesystem::remove(path, ignored);
    }
}

bool ModelGenerationPanel::replace_input_image(const boost::filesystem::path& path)
{
    if (!input_editable()) return false;
    wxImage image;
    const auto error = prepare_model_input_replacement(path, image,
        [this] { return !preserve_unsaved_finishing(); });
    if (error == ImageInputError::Cancelled) return false;
    if (error != ImageInputError::None) {
        wxString reason;
        switch (error) {
        case ImageInputError::Unreadable: reason = _L("图片不存在、为空或无法读取。"); break;
        case ImageInputError::Unsupported: reason = _L("仅支持 PNG/JPEG，暂不支持 WebP 等格式。"); break;
        case ImageInputError::ExtensionMismatch: reason = _L("图片内容与文件后缀不一致，请用图片软件重新导出。"); break;
        case ImageInputError::TooLarge: reason = _L("图片超过 20 MB，请压缩后重试。"); break;
        case ImageInputError::TooSmall: reason = _L("图片宽和高均需至少 64 px。"); break;
        case ImageInputError::TooManyPixels: reason = _L("图片总像素超过 1677 万，请缩小分辨率后重试。"); break;
        default: reason = _L("图片损坏或无法完整解码，请重新导出 PNG/JPEG。"); break;
        }
        show_input_hint(reason + _L("\n当前图片和描述未改变。"));
        return false;
    }
    // History and local imports have a ready model but no active job ID.
    // Replacing their reference must also detach that model from the new input.
    if (m_ready || m_model_preview_ready || (!m_job_id.empty() && !m_awaiting_palette_confirmation))
        reset(true);
    m_selected_image_path = path;
    if (m_prepare_base) m_prepare_base->SetValue(false);
    m_style_user_selected = false;
    m_style_recommendation_available = false;
    if (wxGetApp().app_config != nullptr)
        wxGetApp().app_config->set(
            "model_generation_image_directory", m_selected_image_path.parent_path().string());
    m_style_preview_ready = false;
    m_preview_download_in_flight = false;
    m_preview_download_cancelled = false;
    m_preview_output_available = false;
    m_raw_preview_available = false;
    m_model_reference_available = false;
    m_strict_preview_available = false;
    m_model_views_available = false;
    m_heatmap_available = false;
    boost::system::error_code size_error;
    const auto bytes = boost::filesystem::file_size(m_selected_image_path, size_error);
    m_selected_image->SetLabel(wxString::FromUTF8(m_selected_image_path.filename().string()) +
                               (size_error ? wxString() : wxString::Format(" (%llu KB)", static_cast<unsigned long long>((bytes + 1023) / 1024))));
    m_status->SetLabel(_L("空闲"));
    show_selected_image_preview(&image);
    select_workspace_view(WorkspaceView::Image);
    request_style_recommendation();
    refresh_controls();
    return true;
}

void ModelGenerationPanel::on_clear_image(wxCommandEvent&)
{
    if (!input_editable()) return;
    if (preserve_unsaved_finishing()) return;
    if (m_ready || m_model_preview_ready || (!m_job_id.empty() && !m_awaiting_palette_confirmation))
        reset(true);
    m_selected_image_path.clear();
    ++m_style_recommendation_sequence;
    if (m_prepare_base) m_prepare_base->SetValue(false);
    m_style_recommendation_loading = false;
    m_style_recommendation_available = false;
    m_style_recommendation = {};
    m_selected_image->SetLabel(_L("未选择图片"));
    m_status->SetLabel(_L("空闲"));
    set_preview_empty(_L("请输入描述、选择参考图，或同时提供两者。"));
    refresh_controls();
}

void ModelGenerationPanel::on_recommend_palette(wxCommandEvent&)
{
    if (m_busy || m_shutdown)
        return;
    const std::string prompt = m_prompt->GetValue().ToUTF8().data();
    if (prompt.size() > MAX_MODEL_INPUT_BYTES) {
        show_input_hint(_L("描述超过 2000 字节，请精简后生成；文字已完整保留。"), m_prompt);
        return;
    }
    const bool image_mode = has_image_input();
    const size_t palette_color_count = current_palette_color_count();
    if (prompt.empty() && !image_mode) {
        show_input_hint(_L("请先输入描述或选择参考图。"), m_prompt);
        return;
    }
    if (current_style() == "custom" && current_custom_style().empty()) {
        show_input_hint(_L("请描述希望使用的自定义风格。"), m_custom_style);
        return;
    }
    reset(true);
    m_legacy_generation_state.palette_source = 2;
    m_job_palette.clear();
    m_job_palette_roles.clear();
    m_job_palette_color_count = palette_color_count;
    m_job_use_printable_colors = true;
    m_job_prompt = m_prompt->GetValue();
    m_job_style = current_style();
    m_job_custom_style = current_custom_style();
    m_job_generation_profile = current_generation_profile();
    m_job_face_limit = current_face_limit();
    m_job_generation_options = current_generation_options();
    m_job_print_settings = current_print_settings();
    m_job_image_path = m_selected_image_path;
    m_palette_recommendation_confirmed = false;
    m_awaiting_palette_confirmation = false;
    m_job_preview_expected = true;
    m_busy = true;
    const uint64_t sequence = ++m_sequence;
    update_progress(3, 1, _L("推荐打印配色"));
    m_status->SetLabel(_L("AI 正在分析主体、风格和打印色区..."));
    m_result_summary->SetLabel(_L("推荐完成后直接生成 AI 设计图，配色会一起显示；不会自动生成 3D。"));
    refresh_controls();

    wxWeakRef<ModelGenerationPanel> weak(this);
    auto success = [weak, sequence](AIModelGenerationClient::JobStatus status) mutable {
        if (!weak) return;
        wxGetApp().CallAfter([weak, sequence, status = std::move(status)]() mutable {
            if (weak) weak->handle_status(std::move(status), sequence);
        });
    };
    auto failure = [weak, sequence](std::string error) mutable {
        if (!weak) return;
        wxGetApp().CallAfter([weak, sequence, error = std::move(error)]() {
            if (weak) weak->handle_error(error, sequence);
        });
    };
    if (image_mode) {
        m_client.recommend_image_palette(new_request_id(), prompt, m_selected_image_path, m_job_style,
                                         m_job_custom_style, m_job_palette_color_count, m_job_print_settings,
                                         std::move(success), std::move(failure), true, m_job_generation_options);
    } else {
        m_client.recommend_text_palette(new_request_id(), prompt, m_job_style, m_job_custom_style,
                                        m_job_palette_color_count, m_job_print_settings,
                                        std::move(success), std::move(failure), true, m_job_generation_options);
    }
}

void ModelGenerationPanel::on_confirm_recommended_palette(wxCommandEvent& event)
{
    if (!m_awaiting_palette_confirmation || m_job_id.empty() || m_custom_palette.empty())
        return;
    if (!job_base_inputs_match()) {
        MessageDialog confirm(
            this,
            _L("输入内容已经变化。要保留当前推荐配色，并用新的输入生成图片预览吗？\n\n此操作可能消耗 API 额度。"),
            _L("继续使用当前配色"), wxYES_NO | wxICON_QUESTION);
        if (confirm.ShowModal() != wxID_YES)
            return;
        on_preprocess(event);
        return;
    }
    // Requesting the recommendation already required one quota confirmation,
    // and this handler runs only after the explicit "use palette and generate"
    // click. A second modal repeated the same decision on the happy path.
    m_job_palette = current_palette();
    m_job_palette_roles = current_palette_roles();
    m_palette_recommendation_confirmed = true;
    m_awaiting_palette_confirmation = false;
    m_job_preview_expected = true;
    m_busy = true;
    m_client.record_journey_event("preview_requested", m_job_id);
    const uint64_t sequence = m_sequence;
    update_progress(10, 2, _L("生成AI 设计图"));
    m_status->SetLabel(_L("正在根据当前配色生成 AI 设计图..."));
    refresh_controls();
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.confirm_palette(
        m_job_id, m_job_palette, m_job_palette_roles,
        [weak, sequence](AIModelGenerationClient::JobStatus status) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, status = std::move(status)]() mutable {
                if (weak) weak->handle_status(std::move(status), sequence);
            });
        },
        [weak, sequence](std::string error) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, error = std::move(error)]() {
                if (weak) weak->handle_error(error, sequence);
            });
        });
}

void ModelGenerationPanel::on_preprocess(wxCommandEvent& event)
{
    if (!input_editable() || !m_service_available)
        return;
    if (can_reload_design_preview()) {
        m_preview_download_cancelled = false;
        m_style_preview_placeholder = _L("正在加载 AI 设计图...");
        m_preview_message->SetLabel(_L("正在重新加载当前任务的设计图；不会创建新生成任务。"));
        m_status->SetLabel(_L("正在重新加载已生成的 2D 设计图..."));
        download_preview(m_sequence);
        refresh_controls();
        return;
    }
    if (current_style().empty()) {
        show_input_hint(_L("请先选择生成风格。"), m_style);
        return;
    }
    const std::string entered_prompt = m_prompt->GetValue().ToUTF8().data();
    if (entered_prompt.size() > MAX_MODEL_INPUT_BYTES) {
        show_input_hint(_L("描述超过 2000 字节，请精简后生成；文字已完整保留。"), m_prompt);
        return;
    }
    const bool image_mode = has_image_input();
    if (entered_prompt.empty() && !image_mode) {
        show_input_hint(_L("请先输入描述或选择参考图。"), m_prompt);
        return;
    }
    const std::string custom_style = current_custom_style();
    if (current_style() == "custom" && custom_style.empty()) {
        show_input_hint(_L("请描述希望使用的自定义风格。"), m_custom_style);
        return;
    }
    const bool ai_palette_source = use_printable_colors() && m_legacy_generation_state.palette_source == 2;
    if (ai_palette_source && !m_job_id.empty() &&
        current_palette_color_count() != m_job_palette_color_count) {
        on_recommend_palette(event);
        return;
    }
    if (ai_palette_source && m_awaiting_palette_confirmation && job_base_inputs_match()) {
        on_confirm_recommended_palette(event);
        return;
    }
    if (ai_palette_source && !m_palette_recommendation_confirmed && !m_awaiting_palette_confirmation) {
        on_recommend_palette(event);
        return;
    }
    const std::string prompt = entered_prompt;
    const std::vector<std::string> palette = current_palette();
    const AIModelGenerationClient::PaletteRoles palette_roles = current_palette_roles();
    if (use_printable_colors() && palette.empty()) {
        show_input_hint(_L("生成可打印模型前，请至少配置一种有效耗材颜色。"));
        return;
    }
    if (use_printable_colors() && m_legacy_generation_state.minimum_feature_mm < m_legacy_generation_state.line_width_mm) {
        show_input_hint(_L("最小特征不能小于挤出线宽。建议设置为两条线宽，例如 0.8 mm。"));
        return;
    }
    const bool regenerating_preview = m_style_preview_ready || m_awaiting_confirmation;
    const auto confirmation_inputs = [this]() {
        const auto options = current_generation_options();
        const auto settings = current_print_settings();
        return nlohmann::json::array({m_sequence, m_job_id, m_prompt->GetValue().ToStdString(),
            m_selected_image_path.generic_string(), current_style(), current_custom_style(),
            use_printable_colors(), current_palette(), current_palette_roles(), current_palette_color_count(),
            m_palette_recommendation_confirmed, m_awaiting_palette_confirmation,
            current_generation_profile(), options.provider, options.face_limit, options.geometry_quality,
            options.texture_quality, options.output_format, settings.width_mm, settings.nozzle_mm,
            settings.line_width_mm, settings.minimum_feature_mm, settings.color_distance,
            settings.print_mode, settings.shadow_color});
    };
    const auto confirmed_inputs = confirmation_inputs();
    if (image_mode) {
        static const std::regex absolute_path(R"(^\s*(?:[A-Za-z]:[\\/]|/).*)");
        if (!entered_prompt.empty() && std::regex_match(entered_prompt, absolute_path)) {
            show_input_hint(_L("请描述希望 AI 如何处理图片，不要在描述中粘贴本地文件路径。"), m_prompt);
            return;
        }
        wxString message;
        if (regenerating_preview) {
            message << _L("使用当前风格重新生成 AI 设计图吗？\n\n")
                    << _L("会调用 1 次图片服务生成适合 3D 建模的设计图；不会创建 3D 任务。");
        } else {
            message << _L("要使用这张图片生成 AI 设计图吗？\n\n")
                    << wxString::FromUTF8(m_selected_image_path.filename().string()) << "\n"
                    << _L("仅发送这张图片和文字描述，调用 1 次图片服务；此操作消耗 API 额度。");
        }
        if ((current_style() == "realistic" || current_style() == "portrait_sketch") && use_printable_colors())
            message << _L("\n若识别到真人，优先保留脸型、五官和姿态。");
        message += _L("\n\n费用未报价，金额未知；实际按图片服务账户套餐或账单结算。\n取消不发送请求；停止只停止本地等待，远端请求可能继续运行并计费。");
        ModelGenerationConfirmation confirm(this, message,
                              regenerating_preview ? _L("重新生成图片预览") : _L("生成风格预览"),
                              _L("生成 2D 设计图"));
        if (confirm.ShowModal() != wxID_YES)
            return;
    } else {
        wxString message = use_printable_colors()
                ? _L("要根据文字生成 AI 设计图吗？\n\n会生成适合 3D 建模的高质量设计图，并保留所选配色供后续模型使用。此操作消耗 API 额度。")
                : _L("要根据文字生成 AI 设计图吗？\n\n会先生成并检查图片，再用于后续 3D 生成；此操作可能消耗 API 额度。");
        message += _L("\n\n费用未报价，金额未知；实际按图片服务账户套餐或账单结算。\n取消不发送请求；停止只停止本地等待，远端请求可能继续运行并计费。");
        ModelGenerationConfirmation confirm(this, message, _L("生成图片预览"), _L("生成 2D 设计图"));
        if (confirm.ShowModal() != wxID_YES)
            return;
    }

    // A modal keeps dispatching service/restore events. Never submit a mixture
    // of the input shown in the confirmation and a newer owner state.
    if (!input_editable() || !m_service_available || confirmation_inputs() != confirmed_inputs) {
        show_input_hint(_L("任务或输入已变化，本次没有提交。请核对当前输入后重新确认。"));
        return;
    }
    const std::string previous_job_id = m_job_id;
    const bool palette_was_ai_recommended = ai_palette_source && m_palette_recommendation_confirmed;
    if (regenerating_preview)
        m_client.record_journey_event("preview_regenerated", previous_job_id);
    reset(true);
    m_client.record_journey_event("preview_requested");
    m_job_palette = palette;
    m_job_palette_color_count = current_palette_color_count();
    // reset() refreshes the controls and may rebuild inferred role defaults.
    // Preserve the explicit semantic mapping that was visible at confirmation;
    // swapping portrait skin and garment roles here causes exactly the kind of
    // skin-on-sleeve material bleed the preview gate is meant to prevent.
    m_palette_roles = palette_roles;
    m_palette_roles_source = palette;
    m_job_palette_roles = palette_roles;
    m_job_use_printable_colors = use_printable_colors();
    m_job_prompt = m_prompt->GetValue();
    m_job_style = current_style();
    m_job_custom_style = custom_style;
    m_job_generation_profile = current_generation_profile();
    m_job_face_limit = current_face_limit();
    m_job_generation_options = current_generation_options();
    m_job_print_settings = current_print_settings();
    m_job_image_path = m_selected_image_path;
    m_job_preview_expected = true;
    m_palette_recommendation_confirmed = palette_was_ai_recommended;
    m_busy = true;
    const bool preview_mode = true;
    if (preview_mode) {
        m_style_preview_placeholder = _L("正在生成...");
        if (m_reference_image.IsOk()) {
            m_preview_message->SetLabel(
                wxString::Format(_L("原图 %d × %d px  ·  正在生成 AI 图"),
                                 m_reference_image.GetWidth(), m_reference_image.GetHeight()));
        } else
            m_preview_message->SetLabel(_L("正在根据文字生成 AI 设计图..."));
        update_preview_view();
    }
    const uint64_t sequence = ++m_sequence;
    const wxString prepare_phase = preview_mode ? _L("生成AI 设计图") : _L("准备提示词");
    update_progress(10, 2, prepare_phase);
    m_workflow_phase->SetLabel(prepare_phase);
    m_status->SetLabel(preview_mode ? _L("正在生成高质量 AI 设计图...") : _L("正在准备 3D 提示词..."));
    m_result_summary->SetLabel(preview_mode ? _L("完成后可对照原图，确认形体与细节，再生成 3D。")
                                            : _L("正在整理用于 3D 生成的提示词。"));
    refresh_controls();

    wxWeakRef<ModelGenerationPanel> weak(this);
    auto success = [weak, sequence](AIModelGenerationClient::JobStatus status) mutable {
        if (!weak)
            return;
        wxGetApp().CallAfter([weak, sequence, status = std::move(status)]() mutable {
            if (weak)
                weak->handle_status(std::move(status), sequence);
        });
    };
    auto failure = [weak, sequence](std::string error) mutable {
        if (!weak)
            return;
        wxGetApp().CallAfter([weak, sequence, error = std::move(error)]() {
            if (weak)
                weak->handle_error(error, sequence);
        });
    };
    if (image_mode)
        m_client.preprocess_image(new_request_id(), prompt, m_selected_image_path, m_job_palette, m_job_palette_roles,
                                  m_palette_recommendation_confirmed, m_job_style, m_job_custom_style,
                                  m_job_print_settings,
                                  std::move(success), std::move(failure), m_job_generation_options);
    else
        m_client.preprocess_text(new_request_id(), prompt, m_job_palette, m_job_palette_roles,
                                 m_palette_recommendation_confirmed, m_job_style, m_job_custom_style,
                                 m_job_print_settings,
                                 std::move(success), std::move(failure), m_job_generation_options);
}

void ModelGenerationPanel::on_retexture_from_library(const std::string& geometry_job_id,
                                                      const wxString& title)
{
    if (m_busy || !m_service_available || m_job_id.empty() || geometry_job_id.empty() ||
        geometry_job_id == m_job_id || !m_job_preview_expected || (!m_ready && !m_awaiting_confirmation))
        return;
    const nlohmann::json geometry_metadata = read_json(library_metadata_path(geometry_job_id));
    if (geometry_metadata.is_object() && geometry_metadata.value("provider", std::string("tripo")) == "hunyuan") {
        show_input_hint(_L("腾讯混元3D 历史模型暂不支持复用造型重新上色。可以查看、导入或使用本地编辑。"));
        return;
    }
    wxString message = _L("要保留历史模型“") + title + _L("”的脸部和整体造型，只使用当前确认图片重新生成颜色吗？");
    message += _L("\n\n将创建 1 个付费 Tripo 纹理任务，不会重建网格；因此能保留已经满意的脸和姿态，"
                  "但也不会修复历史模型原有的形状问题。"
                  "\n费用：由当前模型服务商账户按其套餐或额度结算；OrcaSlicer 无法读取具体金额。"
                  "\n预计耗时：通常 3–10 分钟。"
                  "\n停止说明：停止按钮只终止本地等待；已经提交的远端任务可能继续运行并计费。");
    MessageDialog confirm(this, message, _L("确认复用造型并重新上色"), wxYES_NO | wxICON_QUESTION);
    if (confirm.ShowModal() != wxID_YES)
        return;

    const std::string reference_job_id = m_job_id;
    m_client.record_journey_event("model_submitted", reference_job_id);
    m_journey_model_submitted = true;
    m_busy = true;
    m_awaiting_confirmation = false;
    m_ready = false;
    m_artifact_download_started = false;
    m_model_preview_ready = false;
    m_library_model_loaded = false;
    m_artifact_path.clear();
    m_color_intent_path.clear();
    m_color_intent_schema.clear();
    m_color_intent_sha256.clear();
    m_displayed_model_path.clear();
    m_displayed_model_job_id.clear();
    m_displayed_model_palette.clear();
    m_displayed_model_palette_roles.clear();
    clear_model_quality();
    if (m_model_preview != nullptr)
        m_model_preview->clear();
    const uint64_t sequence = m_sequence;
    update_progress(40, 3, _L("保留造型并上色"));
    m_workflow_phase->SetLabel(_L("保留造型并上色"));
    m_status->SetLabel(_L("正在提交保留造型的纹理任务..."));
    refresh_controls();

    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.retexture(reference_job_id, geometry_job_id,
        [weak, sequence](AIModelGenerationClient::JobStatus status) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, status = std::move(status)]() mutable {
                if (weak) weak->handle_status(std::move(status), sequence);
            });
        },
        [weak, sequence](std::string error) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, error = std::move(error)]() {
                if (weak) weak->handle_error(error, sequence);
            });
        });
}

void ModelGenerationPanel::on_stop(wxCommandEvent&)
{
    if (m_saving_generation_options) return;
    if (auto* local = local_model_import_state(m_library_import); local && local->cancel) {
        local->cancel->store(true);
        const wxString progress = local->open_beauty
            ? _L("正在取消创建副本，原模型和当前编辑保持不变……")
            : _L("正在取消本地导入，原文件和当前模型保持不变……");
        m_status->SetLabel(progress);
        if (local->open_beauty) m_model_preview_message->SetLabel(progress);
        refresh_controls();
        return;
    }
    if (m_design_history_loading) {
        ++m_design_history_sequence;
        m_design_history_loading = false;
        m_busy = false;
        refresh_controls();
        m_status->SetLabel(_L("已取消历史设计加载，当前内容已保留。"));
        return;
    }
    if (m_job_id.empty())
        return;
    m_poll_timer.Stop();
    const bool preview_download_was_active = m_preview_download_in_flight;
    m_preview_download_in_flight = false;
    m_preview_download_cancelled = preview_download_was_active;
    if (preview_download_was_active) {
        m_preview_path.clear();
        m_style_preview_ready = false;
    }
    m_client.cancel_current();
    if (preview_download_was_active) {
        ++m_sequence;
        if (m_preview_canceled) *m_preview_canceled = true;
        // The client cancels all its HTTP downloads. Allow an interrupted
        // model download to be reloaded too, without losing a running job.
        if (m_artifact_download_started && !m_model_preview_ready)
            m_artifact_download_started = false;
        if (m_ready) m_busy = false;
        m_style_preview_placeholder = _L("预览加载已取消");
        m_preview_message->SetLabel(_L("图片预览加载已取消，输入已保留；可重新加载已生成的设计图。"));
        m_status->SetLabel(_L("已取消图片预览加载，未停止远端任务。"));
        m_result_summary->SetLabel(_L("输入和服务端结果已保留；重新加载只下载当前任务结果，不会创建新生成任务。"));
        update_preview_view();
        refresh_controls();
        if (m_busy) m_poll_timer.StartOnce(1500);
        return;
    }
    if (m_ready && m_artifact_download_started && !m_model_preview_ready) {
        ++m_sequence;
        if (m_preview_canceled) *m_preview_canceled = true;
        m_busy = false;
        m_restoring_input = false;
        m_artifact_download_started = false;
        m_status->SetLabel(_L("已取消本地模型加载。"));
        m_result_summary->SetLabel(_L("生成结果仍然保留，可点击“重新加载 3D 模型”继续。"));
        m_model_stats->SetLabel(_L("模型尚未加载"));
        refresh_controls();
        return;
    }
    m_status->SetLabel(_L("正在停止本地任务；已提交的远端任务可能仍会继续运行并计费。"));
    const uint64_t sequence = m_sequence;
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.stop(m_job_id,
        [weak, sequence](AIModelGenerationClient::JobStatus status) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, status = std::move(status)]() mutable {
                if (weak) weak->handle_status(std::move(status), sequence);
            });
        },
        [weak, sequence](std::string error) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, error = std::move(error)]() {
                if (weak) weak->handle_poll_error(error, sequence);
            });
        });
}

void ModelGenerationPanel::on_retry_service(wxCommandEvent&)
{
    if (m_service_available || m_busy || !m_service_retry_handler)
        return;
    m_retry_service->Disable();
    m_status->SetLabel(_L("正在重新检测本地 3D 生成服务..."));
    m_result_summary->SetLabel(_L("检测完成后会自动恢复可用功能和最近任务。"));
    m_service_retry_handler();
}

void ModelGenerationPanel::on_import(wxCommandEvent& event)
{
    if (m_model_preview_ready && m_workspace_view == WorkspaceView::Image) {
        dispatch_workspace_action(WorkspaceAction::ShowModel);
        return;
    }
    if (m_model_preview_ready && !m_ready && m_job_id.empty() && m_prepare_navigation &&
        !m_last_imported_model_path.empty() && m_last_imported_model_path == m_displayed_model_path) {
        dispatch_workspace_action(WorkspaceAction::Prepare);
        return;
    }
    if (m_finishing_running || !m_finishing_candidate.empty()) return;
    if (preserve_unsaved_finishing()) return;
    if (m_ready && !m_model_preview_ready && !m_artifact_download_started) {
        m_artifact_download_started = true;
        download_model_preview(m_sequence);
        return;
    }
    download_and_import(wxDynamicCast(event.GetEventObject(), wxWindow));
}
void ModelGenerationPanel::on_poll(wxTimerEvent&) { schedule_poll(); }

bool ModelGenerationPanel::can_reload_design_preview() const
{
    return m_job_preview_expected && !m_preview_download_in_flight && !m_restoring_input &&
        design_preview_reload_available(m_job_id, job_inputs_match(),
                                        m_preview_output_available, m_style_preview_ready);
}

void ModelGenerationPanel::download_preview(uint64_t sequence)
{
    if (m_shutdown || sequence != m_sequence || m_job_id.empty() ||
        m_preview_download_in_flight || m_preview_download_cancelled || m_style_preview_ready ||
        !m_preview_output_available)
        return;
    m_preview_download_in_flight = true;
    m_preview_path = temp_path(m_job_id, "png");
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.download_image_output(m_job_id, m_preview_output, m_preview_path,
        [weak, sequence](boost::filesystem::path path) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, path = std::move(path)]() {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence ||
                    !weak->m_preview_download_in_flight)
                    return;
                weak->m_preview_download_in_flight = false;
                weak->m_preview_download_cancelled = false;
                wxImage image(path.wstring());
                if (!image.IsOk()) {
                    weak->m_preview_path.clear();
                    weak->m_style_preview_ready = false;
                    weak->show_input_hint(_L("无法显示 AI 风格预览，请重试。"));
                    weak->show_preview_failure(_L("AI 设计图无法显示，输入已保留。"));
                    weak->m_client.record_journey_event("preview_failed", weak->m_job_id);
                    weak->refresh_controls();
                    return;
                }
                weak->m_clean_preview_image = image;
                weak->m_preview_zoom_factor = 1.0;
                weak->m_style_preview_ready = true;
                weak->m_client.record_journey_event("preview_ready", weak->m_job_id);
                weak->m_style_preview_placeholder.clear();
                weak->m_preview_kind->SetLabel(_L("结果对照"));
                weak->apply_preview_stage();
                if (weak->m_reference_image.IsOk()) {
                    weak->m_preview_message->SetLabel(
                        wxString::Format(_L("原图 %d × %d px  ·  AI 生成图 %d × %d px"),
                                         weak->m_reference_image.GetWidth(), weak->m_reference_image.GetHeight(),
                                         image.GetWidth(), image.GetHeight()));
                } else {
                    weak->m_preview_message->SetLabel(
                        wxString::Format(_L("AI 生成图 · %d × %d px"), image.GetWidth(), image.GetHeight()));
                }
                weak->m_status->SetLabel(weak->m_job_phase == "stopped" || weak->m_job_phase == "failed"
                    ? _L("AI 设计图已保留，当前任务已结束；可修改输入重新生成。")
                    : _L("AI 设计图已生成，确认后可继续生成 3D 模型。"));
                weak->update_preview_view(true);
                weak->refresh_controls();
                weak->Layout();
                weak->download_auxiliary_previews(sequence);
            });
        },
        [weak, sequence](std::string error) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, error = std::move(error)]() {
                if (weak && !weak->m_shutdown && sequence == weak->m_sequence &&
                    weak->m_preview_download_in_flight) {
                    weak->m_preview_download_in_flight = false;
                    weak->m_preview_download_cancelled = false;
                    weak->m_preview_path.clear();
                    weak->m_style_preview_ready = false;
                    // A failed local GET does not revoke the saved server result.
                    // Keep it reloadable without another paid generation request.
                    weak->show_input_hint(_L("风格预览下载失败：") + wxString::FromUTF8(error));
                    weak->show_preview_failure(_L("AI 设计图下载未完成，输入已保留。"));
                    weak->m_client.record_journey_event("preview_failed", weak->m_job_id);
                    weak->refresh_controls();
                }
            });
        });
}

void ModelGenerationPanel::download_auxiliary_previews(uint64_t sequence, int stage)
{
    if (m_shutdown || sequence != m_sequence || m_job_id.empty() || stage >= 3)
        return;
    const bool available[] = {
        m_raw_preview_available, m_model_reference_available, m_model_views_available
    };
    const char* routes[] = {"raw-preview", "model-reference", "model-view-sheet"};
    const char* suffixes[] = {"raw", "model-reference", "model-views"};
    if (!available[stage]) {
        download_auxiliary_previews(sequence, stage + 1);
        return;
    }
    const boost::filesystem::path path = temp_path(m_job_id + "-" + suffixes[stage], "png");
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.download_image_output(m_job_id, routes[stage], path,
        [weak, sequence, stage](boost::filesystem::path downloaded) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, stage, downloaded = std::move(downloaded)]() mutable {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence) return;
                wxImage image(downloaded.wstring());
                if (image.IsOk()) {
                    if (stage == 0) {
                        weak->m_raw_preview_image = image;
                        weak->m_raw_preview_path = downloaded;
                        weak->m_history_display_source = downloaded;
                        weak->m_history_display_image = load_model_image_display_copy(downloaded);
                    }
                    else if (stage == 1) weak->m_model_reference_image = image;
                    else weak->m_model_views_image = image;
                    weak->apply_preview_stage();
                }
                weak->download_auxiliary_previews(sequence, stage + 1);
            });
        },
        [weak, sequence, stage](std::string error) {
            if (!weak) return;
            BOOST_LOG_TRIVIAL(warning) << "Unable to download printable preview stage: " << error;
            wxGetApp().CallAfter([weak, sequence, stage]() {
                if (weak && !weak->m_shutdown && sequence == weak->m_sequence)
                    weak->download_auxiliary_previews(sequence, stage + 1);
            });
        });
}

void ModelGenerationPanel::download_and_import(wxWindow* entry_window)
{
    if (!m_ready || !m_model_preview_ready || m_busy)
        return;
    if ((m_artifact_format != "obj" && m_artifact_format != "glb")) {
        m_status->SetLabel(_L("只能导入生成的模型。"));
        return;
    }
    if (m_artifact_format == "obj" && m_artifact_color_encoding != "vertex_colors") {
        m_status->SetLabel(_L("生成的模型不包含受支持的顶点颜色。"));
        return;
    }
    boost::filesystem::path local_path;
    if (is_nonempty_model(m_artifact_path))
        local_path = m_artifact_path;
    else if (!m_job_id.empty()) {
        const boost::filesystem::path downloaded_path = temp_path(m_job_id, m_artifact_format);
        if (is_nonempty_model(downloaded_path))
            local_path = downloaded_path;
        else
            m_artifact_path = downloaded_path;
    }

    if (local_path.empty() && m_job_id.empty()) {
        m_status->SetLabel(_L("本地模型已不存在。"));
        m_result_summary->SetLabel(_L("请从模型库重新加载有效模型后再导入准备页。"));
        refresh_controls();
        return;
    }

    // Capture the initiating control before disabling the import actions.
    const wxWeakRef<wxWindow> entry(entry_window);
    m_busy = true;
    update_progress(96, 5, _L("导入准备页"));
    m_workflow_phase->SetLabel(_L("导入准备页"));
    const uint64_t sequence = m_sequence;
    m_status->SetLabel(local_path.empty() ? _L("正在从本地服务读取生成的模型...")
                                         : _L("正在读取本地模型..."));
    refresh_controls();

    if (!local_path.empty()) {
        import_local_artifact(local_path, sequence, entry);
        return;
    }

    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.download_artifact(m_job_id, m_artifact_format, m_artifact_path,
        [weak, sequence, entry](boost::filesystem::path path) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, entry, path = std::move(path)]() {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence)
                    return;
                weak->import_local_artifact(path, sequence, entry);
            });
        },
        [weak, sequence, entry](std::string error) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, entry, error = std::move(error)]() {
                if (!weak) return;
                weak->cleanup_files();
                weak->handle_error(error, sequence);
                weak->restore_import_focus(entry, sequence);
            });
        });
}

void ModelGenerationPanel::restore_import_focus(wxWeakRef<wxWindow> entry, uint64_t sequence)
{
    wxWeakRef<ModelGenerationPanel> weak(this);
    // Defer until refresh_controls has finished re-enabling and laying out the page.
    CallAfter([weak, entry, sequence]() {
        if (!weak || weak->m_shutdown || weak->m_busy || sequence != weak->m_sequence ||
            !entry || !weak->IsDescendant(entry.get()) ||
            !entry->IsShownOnScreen() || !entry->IsEnabled())
            return;
        entry->SetFocus();
    });
}

void ModelGenerationPanel::import_local_artifact(const boost::filesystem::path& path, uint64_t sequence, wxWeakRef<wxWindow> entry)
{
    if (m_shutdown || sequence != m_sequence)
        return;
    if (!is_nonempty_model(path)) {
        m_busy = false;
        m_status->SetLabel(_L("本地模型无效或已不存在。"));
        m_result_summary->SetLabel(_L("请从模型库重新加载有效模型后再导入准备页。"));
        refresh_controls();
        restore_import_focus(entry, sequence);
        return;
    }

    m_artifact_path = path;
    update_progress(98, 5, _L("导入模型"));
    m_status->SetLabel(_L("正在导入颜色、检查模型并自动摆放..."));
    refresh_controls();

    const int color_selection = m_import_color_mode != nullptr ? m_import_color_mode->GetSelection() : 0;
    AI::ModelImportRequest request;
    request.artifact.local_path = path;
    request.artifact.job_id = m_job_id;
    request.artifact.format = m_artifact_format;
    request.artifact.color_encoding = m_artifact_color_encoding;
    request.artifact.generation_palette =
        !m_displayed_model_palette.empty() ? m_displayed_model_palette : m_job_palette;
    request.artifact.used_printable_colors = m_job_use_printable_colors;
    if (m_model_preview && m_displayed_model_path == path) {
        const auto trial = m_model_preview->import_color_mapping(
            m_import_color_source == nullptr || m_import_color_source->GetSelection() == 0);
        AI::ModelColorTrial choice {trial.mapping_colors, trial.target_colors};
        if (trial.enabled && choice.valid()) request.color_trial = std::move(choice);
        request.face_color_overrides = m_model_preview->import_face_color_overrides(
            m_import_color_source == nullptr || m_import_color_source->GetSelection() == 0);
        for (const auto& item : m_model_preview->import_subface_color_overrides(
                 m_import_color_source == nullptr || m_import_color_source->GetSelection() == 0))
            request.subface_color_overrides.push_back(
                {item.face_id, item.path.depth, item.path.value, item.color});
        request.face_color_geometry_id = m_model_preview->geometry_id();
    }
    try {BeautyWorkbenchControls::prepare_import(path,request);}
    catch(const std::exception& e) {m_busy=false;m_status->SetLabel(wxString::FromUTF8(e.what()));refresh_controls();restore_import_focus(entry, sequence);return;}
    const bool has_color_intent = !m_color_intent_path.empty() || !m_color_intent_schema.empty() ||
                                  !m_color_intent_sha256.empty();
    if (has_color_intent) {
        AI::ColorIntentManifestRef manifest {
            m_color_intent_path.string(), m_color_intent_schema, m_color_intent_sha256
        };
        if (!AI::is_valid_color_intent_manifest_ref(manifest) ||
            !AIModelGenerationClient::validate_color_intent_manifest_file(
                m_color_intent_path, m_color_intent_schema, m_color_intent_sha256, path)) {
            m_color_intent_path.clear(); m_color_intent_schema.clear(); m_color_intent_sha256.clear();
            m_result_summary->SetLabel(_L("颜色清单不匹配；将使用模型自身颜色继续导入。"));
        } else {
            request.artifact.color_intent_manifest = std::move(manifest);
        }
    }
    request.color_mode = color_selection == 2
        ? AI::ImportColorMode::SingleColor
        : color_selection == 1 ? AI::ImportColorMode::AutoMap
        : color_selection == 3 ? AI::ImportColorMode::ManualMatch : AI::ImportColorMode::NativeMatch;
    request.suggest_base_preparation = m_prepare_base && m_prepare_base->GetValue();
    const AI::ModelImportResult result = m_artifact_consumer.import_artifact(request);
    if (!result.imported()) {
        m_busy = false;
        if (result.outcome == AI::ModelImportOutcome::Cancelled) {
            m_status->SetLabel(_L("已取消导入。"));
            m_result_summary->SetLabel(_L("模型仍保留在本地，可以稍后重新导入。"));
        } else if (result.outcome == AI::ModelImportOutcome::InvalidArtifact) {
            m_status->SetLabel(result.error.empty()
                ? _L("无法准备本地模型以供导入。") : from_u8(result.error));
            m_result_summary->SetLabel(result.error.empty()
                ? _L("请从模型库重新加载模型后重试。") : from_u8(result.error));
        } else if (result.outcome == AI::ModelImportOutcome::RepairFailed) {
            m_status->SetLabel(_L("自动网格修复失败，模型未导入。"));
            m_result_summary->SetLabel(
                _L("原始模型和修复诊断已保留在 generated_models。") + from_u8(result.error));
        } else {
            m_status->SetLabel(_L("无法导入生成的模型。"));
            m_result_summary->SetLabel(_L("模型已保留在本地，请调整耗材配置后重试。"));
        }
        refresh_controls();
        restore_import_focus(entry, sequence);
        return;
    }

    const std::string job_id = m_job_id;
    if (m_prepare_base) m_prepare_base->SetValue(false);
    const std::string library_job_id = !m_displayed_model_job_id.empty()
        ? m_displayed_model_job_id : job_id;
    update_library_import_status(library_job_id);
    m_last_imported_model_path = path;
    m_client.record_journey_event("model_imported", library_job_id);
    // The displayed work remains available after import, including its reference
    // image for starting another design from the same input.
    const auto reference_image_path = m_reference_image_path;
    cleanup_files();
    m_reference_image_path = reference_image_path;
    // Import ends the active UI task, not the saved generation. Keep its job
    // state so history can restore the source design and provider identity.
    m_poll_timer.Stop();
    m_job_id.clear();
    m_job_palette.clear();
    m_job_palette_roles.clear();
    m_job_use_printable_colors = false;
    m_job_prompt.clear();
    m_job_style.clear();
    m_job_custom_style.clear();
    m_job_image_path.clear();
    m_job_preview_expected = false;
    m_artifact_format.clear();
    m_artifact_color_encoding.clear();
    m_busy = false;
    m_awaiting_confirmation = false;
    m_awaiting_palette_confirmation = false;
    m_palette_recommendation_confirmed = false;
    m_ready = false;
    m_artifact_download_started = false;
    m_library_model_loaded = false;

    m_workflow_phase->SetLabel(result.manual_repair_required
                                   ? _L("手动修复")
                                   : result.manual_coloring_required ? _L("手动上色") : _L("已导入准备页"));
    update_progress(100, 5, _L("已导入准备页"));
    if (!result.error.empty()) {
        m_status->SetLabel(_L("模型已导入，但无法切换到准备页。"));
        m_result_summary->SetLabel(from_u8(result.error));
    } else if (result.manual_repair_required) {
        m_status->SetLabel(_L("模型已导入准备页；检测到开放边，请先检查再切片。"));
        m_result_summary->SetLabel(_L("已保留模型几何与颜色，可在准备页按需修复。"));
    } else if (result.manual_coloring_required) {
        if (result.color_mapping_collapsed) {
            m_status->SetLabel(_L("多种模型颜色只匹配到一个耗材槽，模型已导入准备页。"));
            m_result_summary->SetLabel(_L("请重新匹配至少两个耗材槽，或在准备页手动上色后再切片。"));
        } else if (result.color_mode == AI::ImportColorMode::ManualMatch ||
                   result.color_mode == AI::ImportColorMode::NativeMatch) {
            m_status->SetLabel(_L("颜色匹配未完成，模型已导入准备页。"));
            m_result_summary->SetLabel(_L("请完成颜色匹配或手动上色后再切片。"));
        } else {
            m_status->SetLabel(_L("自动匹配当前耗材失败，模型已导入准备页。"));
            m_result_summary->SetLabel(_L("请改用手动匹配或在准备页手动上色。"));
        }
    } else if (result.color_mode == AI::ImportColorMode::SingleColor) {
        m_status->SetLabel(_L("模型已按单色导入并自动摆放，已进入准备页。"));
        m_result_summary->SetLabel(_L("模型已忽略原有颜色；可在准备页调整模型、耗材或后续切片设置。"));
    } else if (result.color_mode == AI::ImportColorMode::AutoMap) {
        m_status->SetLabel(_L("模型已自动匹配耗材颜色并进入准备页。"));
        m_result_summary->SetLabel(_L("可打印模型已自动摆放；请在准备页确认颜色和模型状态。"));
    } else {
        m_status->SetLabel(_L("颜色匹配已确认，模型已进入准备页。"));
        m_result_summary->SetLabel(_L("模型颜色已匹配到当前打印机耗材槽；请在准备页确认结果。"));
    }
    load_library_entries();
    refresh_controls();
}

void ModelGenerationPanel::update_adaptive_text_height(wxTextCtrl* control, int minimum_lines, int maximum_lines)
{
    if (control == nullptr || minimum_lines <= 0 || maximum_lines < minimum_lines)
        return;

    wxClientDC dc(control);
    dc.SetFont(control->GetFont());
    const int line_height = std::max(1, dc.GetTextExtent("Ag").GetHeight() + FromDIP(3));
    int available_width = control->GetClientSize().GetWidth() - FromDIP(18);
    if (available_width < FromDIP(120))
        available_width = FromDIP(280);

    const wxString value = control->GetValue();
    int visual_lines = 0;
    size_t start = 0;
    while (start <= value.length()) {
        const size_t end = value.find('\n', start);
        const wxString line = end == wxString::npos ? value.Mid(start) : value.Mid(start, end - start);
        const int line_width = dc.GetTextExtent(line.empty() ? " " : line).GetWidth();
        visual_lines += std::max(1, (line_width + available_width - 1) / available_width);
        if (end == wxString::npos)
            break;
        start = end + 1;
    }

    const int rows = std::clamp(visual_lines, minimum_lines, maximum_lines);
    const int desired_height = rows * line_height + FromDIP(8);
    if (control->GetMinSize().GetHeight() == desired_height &&
        control->GetMaxSize().GetHeight() == desired_height)
        return;
    control->SetMinSize(wxSize(-1, desired_height));
    control->SetMaxSize(wxSize(-1, desired_height));
    control->InvalidateBestSize();
}

void ModelGenerationPanel::refresh_controls()
{
    if (m_shutdown || !m_page_initialized)
        return;
    const auto* local_import = local_model_import_state(m_library_import);
    const bool importing_local = local_import && local_import->cancel;
    const bool canceling_local = importing_local && local_import->cancel->load();
    if (importing_local || m_preview_loading || m_finishing_running || m_design_history_loading || m_saving_generation_options) m_busy = true;
    const bool busy = m_busy || m_preview_download_in_flight;
    update_adaptive_text_height(m_prompt, 3, 3);
    update_adaptive_text_height(m_custom_style, 2, 5);
    refresh_palette();
    const bool image_input = has_image_input();
    const bool image_job = m_job_preview_expected;
    const bool style_selected = m_style->GetSelection() != wxNOT_FOUND;
    const bool custom_style_selected = current_style() == "custom";
    const bool custom_style_ready = !custom_style_selected || !current_custom_style().empty();
    const auto prompt_bytes = m_prompt->GetValue().ToUTF8();
    const bool prompt_valid = prompt_bytes && prompt_bytes.length() <= MAX_MODEL_INPUT_BYTES;
    const bool valid_input = prompt_valid && (!m_prompt->GetValue().Strip(wxString::both).empty() || image_input) && style_selected && custom_style_ready;
    const bool printable_colors = use_printable_colors();
    const bool ai_palette_source = printable_colors && m_legacy_generation_state.palette_source == 2;
    const bool palette_matches = !printable_colors || m_awaiting_palette_confirmation || m_job_id.empty() ||
        (printable_colors == m_job_use_printable_colors && (!printable_colors || m_palette == m_job_palette));
    const bool stale_job = !m_restoring_input && !m_job_id.empty() &&
                           (!job_inputs_match() || !palette_matches);
    // Import finishes the active task, but its local preview remains viewable.
    // Viewing that preview must not depend on permission to import it again.
    const bool view_local_model = m_workspace_view == WorkspaceView::Image &&
        m_model_preview_ready && !stale_job;
    const bool return_to_preparation = m_workspace_view == WorkspaceView::Model &&
        m_model_preview_ready && !m_ready && m_job_id.empty() && m_prepare_navigation &&
        !m_last_imported_model_path.empty() && m_last_imported_model_path == m_displayed_model_path;
    const bool show_review = m_awaiting_confirmation && !image_job && !stale_job;
    const bool local_artifact = is_nonempty_model(m_artifact_path) ||
        (!m_job_id.empty() && is_nonempty_model(temp_path(m_job_id, m_artifact_format)));
    const bool preview_quality_ok = m_palette_quality_ok && m_model_input_eligible;
    const bool multiview_retry = m_job_phase == "multiview_retry";

    const bool reload_design = !stale_job && can_reload_design_preview();
    m_preprocess->SetLabel(reload_design ? _L("重新加载 2D 设计图")
                               : m_awaiting_confirmation && image_job && !stale_job
                               ? _L("重新生成 2D 设计图")
                               : ai_palette_source && m_awaiting_palette_confirmation && !stale_job
                               ? _L("采用配色并生成预览")
                               : ai_palette_source && !m_palette_recommendation_confirmed
                               ? _L("AI 推荐并生成")
                               : ai_palette_source && m_palette_recommendation_confirmed
                               ? _L("使用当前配色生成预览")
                               : m_awaiting_confirmation && !preview_quality_ok
                               ? _L("重新生成图片预览")
                               : image_input && printable_colors ? _L("生成多色图片预览")
                               : image_input ? _L("生成风格图片预览")
                               : _L("生成 AI 设计图"));
    m_preprocess->SetToolTip(reload_design
        ? _L("下载当前任务已生成的设计图，不重新生成或创建新的付费任务。")
        : ai_palette_source && !m_palette_recommendation_confirmed
        ? _L("推荐配色后直接生成 1 张 AI 设计图，消耗 API 额度；不自动创建 3D 任务。")
        : _L("生成适合 3D 建模的高质量 AI 设计图。"));
    m_generate->SetLabel(multiview_retry ? _L("重试生成 3D")
                                         : image_job ? _L("生成 3D") : _L("确认提示词并生成 3D"));
    m_generate->SetToolTip(multiview_retry
                               ? _L("重新准备四视图；通过检查后才会创建 1 个付费 3D 任务")
                               : image_job ? _L("确认当前图片并创建 1 个付费 3D 任务")
                               : _L("确认当前提示词并创建 1 个付费 3D 任务"));
    const bool local_model_loading = m_ready && m_artifact_download_started && !m_model_preview_ready;
    m_stop->SetLabel(importing_local
                        ? canceling_local ? _L("正在取消…")
                          : local_import->open_beauty ? _L("取消创建副本") : _L("取消本地导入")
                        : (local_model_loading || m_preview_download_in_flight || m_design_history_loading) ? _L("取消加载") : _L("停止生成"));
    m_stop->SetToolTip(importing_local
                           ? _L("只取消本次本地导入或副本创建，原件和当前编辑保留；不会发送远程停止请求。转换结束后清理本次临时文件。")
                           : m_design_history_loading
                           ? _L("取消打开历史设计，保留当前内容")
                           : local_model_loading || m_preview_download_in_flight
                           ? _L("只取消当前本地下载和预览加载；已生成的远端模型会保留，可稍后重新加载")
                           : _L("停止本地任务；已经提交给远端的生成任务可能仍会继续运行并计费"));
    m_import->SetLabel(!m_model_preview_ready
                           ? _L("重新加载 3D 模型")
                           : m_workspace_view == WorkspaceView::Image ? _L("打开 3D 工作台")
                           : return_to_preparation ? _L("返回工程准备")
                           : _L("将此模型加入工程"));
    m_import->SetToolTip(m_model_preview_ready && m_workspace_view == WorkspaceView::Image
                             ? _L("查看已下载的模型；不会导入工程或开始切片。")
                             : m_visual_quality.available && !m_visual_quality.import_recommended
                             ? _L("外观检查仅供参考，可继续导入；请对照原图确认效果。")
                             : wxString());
    m_discard->SetLabel(_L("修改输入，重新设计"));
    m_clear_image->Show(image_input);
    m_upload_notice->Show(image_input);
    m_model_settings_panel->Show(m_awaiting_confirmation && !stale_job);
    m_import_settings_panel->Show(m_ready && !stale_job);
    m_preprocess_section->Show(show_review);
    m_prepared_prompt_label->Show(show_review);
    m_prepared_prompt->Show(show_review);

    m_prompt->Enable(!busy);
    m_style->Enable(!busy);
    m_stylized_style->Show(m_style->GetSelection() == 2);
    m_stylized_style->Enable(!busy);
    m_custom_style_panel->Show(custom_style_selected);
    m_custom_style->Enable(!busy && custom_style_selected);
    refresh_style_recommendation();
    m_provider->Enable(!busy);
    m_quality->Enable(!busy);
    const bool hunyuan = m_provider->GetSelection() == 1;
    m_geometry_quality->Enable(!busy && !hunyuan);
    m_texture_quality->Enable(!busy && !hunyuan);
    m_output_format->Enable(!busy);
    wxString generation_cost = generation_options_summary(m_job_id.empty() || m_job_preview_expected);
    if (m_legacy_generation_defaults)
        generation_cost += _L("\n旧记录未指定几何档位，重新生成沿用标准几何的 100 万面上限；历史模型不变。");
    wxClientDC generation_cost_dc(m_generation_cost);
    generation_cost_dc.SetFont(m_generation_cost->GetFont());
    const int generation_cost_width = m_generation_cost->GetClientSize().x > FromDIP(120)
        ? m_generation_cost->GetClientSize().x : FromDIP(280);
    wxString wrapped_generation_cost;
    const wxSize generation_cost_size = Label::split_lines(
        generation_cost_dc, generation_cost_width, generation_cost, wrapped_generation_cost);
    m_generation_cost->SetLabel(wrapped_generation_cost);
    m_generation_cost->SetMinSize(wxSize(1, generation_cost_size.y));
    m_generation_cost->InvalidateBestSize();
    if(m_library_import) {
        m_library_import->Enable(importing_local ? !canceling_local : !busy);
        m_library_import->SetLabel(importing_local
            ? canceling_local ? _L("正在取消…") : _L("取消本地导入") : _L("导入模型"));
        m_library_import->SetToolTip(importing_local
            ? _L("取消本次本地导入，保留原文件、当前模型和已有历史记录。")
            : _L("从本机导入模型，原始文件保留。"));
    }
    m_choose_image->Enable(!busy);
    m_paste_image->Enable(!busy);
    m_clear_image->Enable(!busy);
    m_import_color_mode->Enable(!busy);
    m_import_color_source->Enable(!busy && m_import_color_mode->GetSelection() == 0);
    m_preprocess->Enable(m_service_available && !busy && valid_input &&
                         (ai_palette_source || !printable_colors || !m_palette.empty()));
    m_prepared_prompt->Enable(m_service_available && !busy && show_review);
    if (m_prepare_base) m_prepare_base->Enable(!busy);
    m_generate->Enable(generation_options_valid() && m_service_available && !busy && m_awaiting_confirmation && !stale_job &&
                       (!image_job || m_style_preview_ready));
    m_stop->Enable(!m_saving_generation_options && (importing_local
                       ? !canceling_local
                       : m_design_history_loading || (busy && !m_job_id.empty() && (local_model_loading || m_preview_download_in_flight || m_service_available))));
    m_retry_service->Enable(!m_service_available && !busy && static_cast<bool>(m_service_retry_handler));
    // Retain the business qualification independently of beauty's temporary
    // edit guard, so undoing back to a saved version can restore the action.
    m_model_import_available = !busy && (view_local_model || return_to_preparation || ((local_artifact || m_service_available) &&
                     m_ready && !stale_job &&
                     (m_model_preview_ready || !m_artifact_download_started)));
    m_import->Enable(m_model_import_available);
    m_recheck_model->Enable(m_service_available && !busy && !m_quality_check_busy && !m_visual_check_busy &&
                            m_model_preview_ready && !m_displayed_model_job_id.empty());
    m_visual_review_model->Enable(m_service_available && !busy && !m_quality_check_busy && !m_visual_check_busy &&
                                  m_model_preview_ready && !m_displayed_model_job_id.empty());
    m_visual_review_model->SetLabel(m_reference_image.IsOk() ? _L("AI 对照原图") : _L("AI 视觉复核"));
    const wxString refinement_suffix = from_u8(m_model_refinement.prompt_suffix);
    const bool refinement_already_applied = !refinement_suffix.empty() &&
        m_prompt->GetValue().Find(refinement_suffix) != wxNOT_FOUND;
    m_apply_model_refinement->Enable(!busy && m_model_refinement.available &&
                                     !m_model_refinement.prompt_suffix.empty() &&
                                     !refinement_already_applied);
    const bool has_restartable_work = !m_job_id.empty() || m_ready || m_model_preview_ready || m_style_preview_ready;
    m_discard->Enable(!busy && has_restartable_work);

    const bool show_preprocess = !busy && (reload_design || ((!m_ready || stale_job) &&
        (m_job_id.empty() || stale_job || (!m_awaiting_confirmation && !m_ready) ||
         (m_awaiting_confirmation && image_job))));
    m_preprocess->Show(m_service_available && show_preprocess &&
        (reload_design || (!view_local_model && !return_to_preparation)));
    m_generate->Show(m_service_available && !busy && m_awaiting_confirmation);
    m_stop->Show(busy && !m_saving_generation_options);
    m_retry_service->Show(!m_service_available && !busy);
    m_import->Show(!busy && (view_local_model || return_to_preparation || (m_ready && !stale_job)));
    m_discard->Show(!busy && has_restartable_work);
    if (!busy && ((m_job_id.empty() && !m_ready) || stale_job))
        update_progress(0, 1, _L("输入"));
    if (!busy && stale_job)
        m_status->SetLabel(m_awaiting_palette_confirmation
                               ? _L("输入已变化；可重新推荐，或继续使用当前配色。")
                               : _L("输入或颜色已变化，请重新生成图片预览。"));
    if (m_ready && !stale_job) {
        if (!busy) {
            update_progress(100, 4, _L("模型已生成 · 可查看"));
            m_generation_progress->Hide();
            m_progress_percent->Hide();
        }
        wxString ready_guidance = m_workspace_view == WorkspaceView::Image
            ? _L("打开 3D 工作台查看模型，再决定是否加入工程")
            : _L("检查当前模型；加入工程前可选择配色方式");
        if (m_visual_quality.available && !m_visual_quality.import_recommended) {
            const bool identity_risk = std::find(
                m_visual_quality.blocking_warnings.begin(), m_visual_quality.blocking_warnings.end(),
                "visual_identity_mismatch") != m_visual_quality.blocking_warnings.end();
            const bool material_risk = std::find(
                m_visual_quality.blocking_warnings.begin(), m_visual_quality.blocking_warnings.end(),
                "visual_material_color_mixing") != m_visual_quality.blocking_warnings.end();
            ready_guidance = identity_risk && material_risk
                ? _L("人脸和材料边界需复核，建议重新优化")
                : identity_risk
                ? _L("人脸相似度需复核，建议对照原图检查")
                : material_risk
                ? _L("材料边界需复核，请检查手臂、衣物和底座")
                : _L("外观检查未通过，请展开风险项复核");
        }
        m_workflow_steps->SetLabel(ready_guidance);
        m_workflow_steps->Wrap(FromDIP(330));
        m_workflow_steps->InvalidateBestSize();
    }
    else if (m_awaiting_palette_confirmation)
        m_workflow_steps->SetLabel(stale_job
                                       ? _L("输入已变化：重新推荐或确认继续使用当前配色")
                                       : _L("修改或确认 AI 推荐的设计目标色"));
    else if (m_awaiting_confirmation && !stale_job)
        m_workflow_steps->SetLabel(multiview_retry
                                       ? _L("付费前四视图检查未通过，可直接重试")
                                       : preview_quality_ok
                                       ? _L("确认右侧图片效果，并选择 3D 模型精度")
                                       : _L("图片存在质量提醒，确认后仍可生成 3D"));
    else if (!busy)
        m_workflow_steps->SetLabel(custom_style_selected && !custom_style_ready
                                       ? _L("请补充自定义风格描述")
                                       : valid_input ? _L("下一步：生成图片预览")
                                                     : _L("输入文字、图片，或同时使用两者"));
    m_workflow_steps->Wrap(FromDIP(330));
    m_workflow_steps->InvalidateBestSize();
    if (stale_job && (m_awaiting_confirmation || m_ready))
        m_result_summary->SetLabel(_L("输入内容或颜色模式发生变化，请重新生成预览后继续。"));

    const std::string submission_error = m_submission_state.error(
        {m_job_id, current_generation_options().provider, m_sequence});
    if (!submission_error.empty() && !stale_job && !m_busy && !m_ready) {
        m_status->SetLabel(localized_service_error(submission_error) + "\n" + _L("诊断 ID：") + from_u8(m_job_id));
        m_result_summary->SetLabel(is_model_provider_not_configured(submission_error)
            ? _L("设计图已保留；请先配置或切换模型服务，再手动确认生成。")
            : _L("生成请求未完成，当前设计图已保留。请先处理上方错误；程序不会自动重新提交。"));
        m_workflow_steps->SetLabel(_L("生成请求未完成 · 请处理错误后手动继续"));
        m_workflow_steps->Wrap(FromDIP(330));
        m_generate->SetLabel(_L("手动重试生成 3D"));
    }

    const bool has_preview = m_reference_image.IsOk() || m_style_preview_image.IsOk();
    const bool image_page_active = m_preview_book != nullptr && m_preview_book->GetSelection() == 0;
    if (m_preview_details_pane != nullptr)
        m_preview_details_pane->Show(image_page_active && m_model_views_available);
    if (m_model_decision_panel != nullptr)
        m_model_decision_panel->Hide();
    if (m_model_decision_panel != nullptr) {
        if (auto* model_page = dynamic_cast<wxScrolledWindow*>(m_model_decision_panel->GetParent())) {
            // These sections are hidden until a model is ready. Refit the
            // scrolled page when they appear so they are placed below the
            // comparison row instead of painting over the image panes.
            model_page->Layout();
            model_page->FitInside();
        }
    }
    m_zoom_out->Enable(has_preview && m_preview_zoom_factor > MIN_PREVIEW_ZOOM);
    m_zoom_fit->Enable(has_preview && std::abs(m_preview_zoom_factor - 1.0) > 0.001);
    m_zoom_in->Enable(has_preview && m_preview_zoom_factor < MAX_PREVIEW_ZOOM);
    m_front_model_view->Enable(m_model_preview_ready);
    m_reset_model_view->Enable(m_model_preview_ready);
    refresh_local_recolor_controls();
    refresh_model_finishing();
    refresh_comparison_layout();
    if (m_model_preview != nullptr && m_model_preview->GetParent() != nullptr)
        m_model_preview->GetParent()->Layout();
    if (m_preview_details_pane != nullptr && m_preview_details_pane->GetParent() != nullptr)
        m_preview_details_pane->GetParent()->Layout();
    if (auto* scroll = dynamic_cast<wxScrolledWindow*>(m_prompt->GetParent())) {
        scroll->Layout();
        scroll->FitInside();
    }
    if (!busy && !m_ready && !m_last_imported_model_path.empty() &&
        m_last_imported_model_path == m_displayed_model_path && m_job_id.empty()) {
        update_progress(100, 4, _L("该结果已导入过"));
        m_workflow_steps->SetLabel(_L("返回当前工程继续调整尺寸、底座与打印适配。生成原件保留在模型库。"));
        m_workflow_steps->Wrap(FromDIP(330));
    }
    refresh_preview_stage_hint(stale_job);
    refresh_input_layout();
    Layout();
}

void ModelGenerationPanel::apply_model_quality(const AIModelGenerationClient::ModelQuality& quality)
{
    m_model_quality = quality;
    m_quality_check_failed = false;
    refresh_model_quality_card();
}

void ModelGenerationPanel::apply_visual_quality(const AIModelGenerationClient::VisualQuality& quality)
{
    m_visual_quality = quality;
    refresh_model_quality_card();
}

void ModelGenerationPanel::apply_model_refinement(
    const AIModelGenerationClient::ModelRefinementAdvice& refinement)
{
    m_model_refinement = refinement;
    refresh_model_quality_card();
}

void ModelGenerationPanel::clear_model_quality()
{
    m_model_quality = {};
    m_visual_quality = {};
    m_model_refinement = {};
    m_quality_check_busy = false;
    m_quality_check_failed = false;
    ++m_quality_request_revision;
    m_visual_check_busy = false;
    m_thin_region_navigation_active = false;
    m_thin_region_navigation_index = 0;
    refresh_model_quality_card();
}

void ModelGenerationPanel::refresh_model_quality_card()
{
    if (m_model_quality_panel == nullptr)
        return;
    wxColour foreground(91, 104, 107);
    wxColour background(246, 248, 248);
    const auto check_stage = model_check_stage(m_model_preview_ready, m_quality_check_busy,
                                               m_quality_check_failed, m_model_quality);
    wxString status = _L("尚未检查");
    wxString summary = check_stage == ModelCheckStage::NeedsModel
        ? _L("模型生成或加载后可进行结构检查。")
        : _L("当前保存版本尚无结构检查结果。能预览不代表可打印；导入时仍会校验几何，尺寸、方向和厚度需在准备页复核。");
    if (check_stage == ModelCheckStage::Checking) {
        status = _L("正在检查...");
        summary = _L("正在本地分析拓扑、组件、接地和悬垂，请稍候。");
        foreground = wxColour(31, 122, 116);
        background = wxColour(229, 244, 242);
    } else if (check_stage == ModelCheckStage::Failed) {
        status = _L("检查未完成");
        summary = _L("未取得当前保存版本的有效结构检查结果。模型与编辑已保留；可重试检查，不能据此判断可打印。");
        foreground = wxColour(174, 112, 22);
        background = wxColour(255, 246, 225);
    } else if (check_stage == ModelCheckStage::Passed || check_stage == ModelCheckStage::Review ||
               check_stage == ModelCheckStage::Rejected) {
        if (check_stage == ModelCheckStage::Passed) {
            status = _L("结构检查通过");
            summary = _L("本次分析未发现明显结构问题；仍需结合实际打印设置判断。");
            foreground = wxColour(31, 122, 90);
            background = wxColour(232, 246, 238);
        } else if (check_stage == ModelCheckStage::Review) {
            status = _L("建议复核");
            summary = _L("当前保存版本存在需要复核的结构风险；请结合检查指标及准备页设置判断。");
            foreground = wxColour(174, 112, 22);
            background = wxColour(255, 246, 225);
        } else if (check_stage == ModelCheckStage::Rejected) {
            status = _L("未通过结构检查");
            summary = _L("当前保存版本存在结构问题；请先查看检查指标并修复，再重新检查。");
            foreground = wxColour(188, 62, 54);
            background = wxColour(253, 235, 233);
        }
        const auto& codes = m_model_quality.status == "reject" ? m_model_quality.errors : m_model_quality.warnings;
        if (!codes.empty()) {
            summary.clear();
            const size_t visible = std::min<size_t>(2, codes.size());
            for (size_t index = 0; index < visible; ++index) {
                if (!summary.empty()) summary += "\n";
                summary += _L("• ") + model_quality_code_label(codes[index]);
            }
            if (codes.size() > visible)
                summary += wxString::Format(_L("\n另有 %llu 项，请展开查看。"),
                    static_cast<unsigned long long>(codes.size() - visible));
        }
    }
    m_model_quality_status->SetLabel(status);
    m_model_quality_status->SetForegroundColour(foreground);
    m_model_quality_panel->SetBackgroundColour(background);
    m_model_quality_summary->SetLabel(summary);
    if (m_model_decision_panel != nullptr) {
        m_model_decision_status->SetLabel(status);
        m_model_decision_status->SetForegroundColour(foreground);
        m_model_decision_summary->SetLabel(summary);
        m_model_decision_panel->SetBackgroundColour(background);
    }
    const bool has_check_result = check_stage == ModelCheckStage::Passed ||
        check_stage == ModelCheckStage::Review || check_stage == ModelCheckStage::Rejected;
    wxString details;
    if (has_check_result) {
        details << wxString::Format(_L("三角面：%llu · 顶点：%llu · 连通部件：%llu\n"),
                    static_cast<unsigned long long>(m_model_quality.face_count),
                    static_cast<unsigned long long>(m_model_quality.vertex_count),
                    static_cast<unsigned long long>(m_model_quality.component_count));
        if (m_model_quality.bed_contact_area_available) {
            details << wxString::Format(_L("最大部件占比：%.1f%% · 接地跨度：%.1f%% · 接地面积：%.1f%%\n"),
                        m_model_quality.largest_component_face_ratio * 100.0,
                        m_model_quality.contact_span_ratio * 100.0,
                        m_model_quality.bed_contact_area_ratio * 100.0);
            details << wxString::Format(
                        m_model_quality.elevated_downward_surface_ratio_available
                            ? _L("离床向下表面：%.1f%%") : _L("向下表面：%.1f%%"),
                        (m_model_quality.elevated_downward_surface_ratio_available
                            ? m_model_quality.elevated_downward_surface_ratio
                            : m_model_quality.downward_surface_ratio) * 100.0);
            if (m_model_quality.overhang_region_metrics_available)
                details << wxString::Format(_L(" · 显著局部悬垂：%llu 个"),
                            static_cast<unsigned long long>(m_model_quality.significant_overhang_region_count));
            if (m_model_quality.component_thickness_available &&
                m_model_quality.minimum_component_thickness_mm > 0.0)
                details << wxString::Format(_L("\n最薄组件：%.2f mm · 薄型组件：%llu 个"),
                            m_model_quality.minimum_component_thickness_mm,
                            static_cast<unsigned long long>(m_model_quality.thin_component_count));
            if (m_model_quality.local_thickness_available) {
                details << wxString::Format(_L("\n局部厚度采样：%llu 个"),
                            static_cast<unsigned long long>(m_model_quality.local_thickness_sample_count));
                if (m_model_quality.minimum_sampled_local_thickness_mm > 0.0)
                    details << wxString::Format(_L(" · 最薄命中：%.2f mm · 薄面样本：%llu 个"),
                                m_model_quality.minimum_sampled_local_thickness_mm,
                                static_cast<unsigned long long>(m_model_quality.thin_local_surface_sample_count));
                else
                    details << _L(" · 未发现阈值内相对表面");
                if (m_model_quality.thin_local_region_count > 0)
                    details << wxString::Format(_L("\n局部薄壁风险区：%llu 个 · 报告前 %llu 个"),
                                static_cast<unsigned long long>(m_model_quality.thin_local_region_count),
                                static_cast<unsigned long long>(m_model_quality.reported_thin_local_region_count));
            }
            if (m_model_quality.target_palette_metrics_available) {
                details << wxString::Format(
                    _L("\n最终模型目标色：显著 %llu/%llu · 建议至少 %llu · 覆盖 %.1f%%"),
                    static_cast<unsigned long long>(m_model_quality.meaningful_target_palette_color_count),
                    static_cast<unsigned long long>(m_model_quality.target_palette_color_count),
                    static_cast<unsigned long long>(
                        m_model_quality.required_meaningful_target_palette_color_count),
                    m_model_quality.target_palette_surface_coverage_ratio * 100.0);
                if (!m_model_quality.target_palette_surface_usage.empty()) {
                    details << _L("\n逐色表面积：");
                    for (size_t index = 0; index < m_model_quality.target_palette_surface_usage.size(); ++index) {
                        if (index > 0)
                            details << _L(" · ");
                        const auto& usage = m_model_quality.target_palette_surface_usage[index];
                        details << from_u8(usage.color)
                                << wxString::Format(_L(" %.1f%%"), usage.surface_ratio * 100.0);
                    }
                }
            }
        } else {
            details << wxString::Format(_L("最大部件占比：%.1f%% · 接地覆盖：%.1f%% · 向下表面：%.1f%%"),
                        m_model_quality.largest_component_face_ratio * 100.0,
                        m_model_quality.contact_span_ratio * 100.0,
                        m_model_quality.downward_surface_ratio * 100.0);
        }
        const auto& codes = m_model_quality.status == "reject" ? m_model_quality.errors : m_model_quality.warnings;
        for (const std::string& code : codes)
            details += _L("\n• ") + model_quality_code_label(code);
    } else {
        details = _L("尚无结构化质量指标。");
    }
    m_model_quality_details->SetLabel(details);
    if (m_model_overview_check_metrics)
        m_model_overview_check_metrics->SetLabel(model_check_scope(m_model_quality) + "\n\n" + details);
    refresh_model_overview_risks();
    m_model_quality_details_pane->Show(has_check_result);

    wxString visual_status = _L("AI 视觉复核：未运行");
    wxString visual_summary = m_model_preview_ready
        ? _L("质量档会自动生成五视图并检查主体、人脸和材料串色；也可点击上方按钮重新复核。")
        : _L("模型准备好后可按需生成五视图并进行 AI 外观复核。");
    wxColour visual_foreground(91, 104, 107);
    if (m_visual_check_busy) {
        visual_status = _L("AI 视觉复核中...");
        visual_summary = _L("正在生成前后左右和等轴视图，并对照原图检查主体、人脸、底座和材料色区，请稍候。");
        visual_foreground = wxColour(31, 122, 116);
    } else if (m_visual_quality.available) {
        if (m_visual_quality.status == "pass") {
            visual_status = wxString::Format(_L("AI 视觉复核通过 · %d 分"), m_visual_quality.score);
            visual_foreground = wxColour(31, 122, 90);
        } else if (m_visual_quality.status == "review") {
            visual_status = wxString::Format(
                m_visual_quality.import_recommended
                    ? _L("AI 建议人工复核 · %d 分")
                    : _L("AI 外观存在提醒 · %d 分"),
                m_visual_quality.score);
            visual_foreground = m_visual_quality.import_recommended
                ? wxColour(174, 112, 22) : wxColour(181, 62, 55);
        } else {
            visual_status = _L("AI 视觉复核暂不可用");
            visual_foreground = wxColour(174, 112, 22);
        }
        visual_summary = from_u8(m_visual_quality.summary);
        const auto& codes = m_visual_quality.status == "unavailable"
            ? m_visual_quality.errors
            : !m_visual_quality.import_recommended && !m_visual_quality.blocking_warnings.empty()
                ? m_visual_quality.blocking_warnings : m_visual_quality.warnings;
        const size_t visible = std::min<size_t>(3, codes.size());
        for (size_t index = 0; index < visible; ++index)
            visual_summary += _L("\n• ") + visual_quality_code_label(codes[index]);
    }
    m_visual_quality_status->SetLabel(visual_status);
    m_visual_quality_status->SetForegroundColour(visual_foreground);
    m_visual_quality_summary->SetLabel(visual_summary);
    // The compact decision card is the only quality summary visible without
    // expanding advanced details.  A blocking identity/material result must
    // therefore outrank non-blocking structural review warnings; otherwise a
    // bad portrait can appear to need only support or thin-wall inspection.
    if (m_model_decision_panel != nullptr &&
        !(m_model_quality.available && m_model_quality.status == "reject") &&
        m_visual_quality.available && !m_visual_quality.import_recommended) {
        m_model_decision_status->SetLabel(wxString::Format(
            _L("外观提醒 · %d 分"), m_visual_quality.score));
        m_model_decision_status->SetForegroundColour(wxColour(181, 62, 55));
        m_model_decision_summary->SetLabel(visual_summary);
        m_model_decision_summary->Wrap(FromDIP(620));
        m_model_decision_summary->InvalidateBestSize();
        m_model_decision_panel->SetBackgroundColour(wxColour(253, 235, 233));
    }
    if (m_model_refinement.available && !m_model_refinement.prompt_suffix.empty()) {
        wxString refinement_summary = from_u8(m_model_refinement.summary);
        const size_t visible = std::min<size_t>(3, m_model_refinement.issues.size());
        for (size_t index = 0; index < visible; ++index)
            refinement_summary += _L("\n• ") + from_u8(m_model_refinement.issues[index].title);
        if (m_model_refinement.issues.size() > visible)
            refinement_summary += wxString::Format(
                _L("\n• 另有 %llu 类建议"),
                static_cast<unsigned long long>(m_model_refinement.issues.size() - visible));
        m_model_refinement_summary->SetLabel(refinement_summary);
        m_model_refinement_panel->Show();
    } else {
        m_model_refinement_summary->SetLabel(wxEmptyString);
        m_model_refinement_panel->Hide();
    }
    m_model_quality_panel->Layout();
    m_model_quality_panel->GetParent()->Layout();
    if (m_model_decision_panel != nullptr)
        m_model_decision_panel->Layout();
}

void ModelGenerationPanel::on_recheck_model(wxCommandEvent&)
{
    if (m_quality_check_busy || m_displayed_model_job_id.empty() || !m_model_preview_ready)
        return;
    const std::string job_id = m_displayed_model_job_id;
    const uint64_t sequence = m_sequence;
    const auto model_path = m_displayed_model_path;
    std::string artifact_sha256;
    try { artifact_sha256 = AI::model_artifact_sha256(model_path); }
    catch (const std::exception&) { artifact_sha256.clear(); }
    if (artifact_sha256.empty()) {
        clear_model_quality();
        m_quality_check_failed = true;
        m_status->SetLabel(_L("模型文件不可读；请从资产库重新加载后检查。"));
        refresh_model_quality_card();
        refresh_controls();
        return;
    }
    const uint64_t revision = ++m_quality_request_revision;
    m_model_quality = {};
    m_quality_check_failed = false;
    m_thin_region_navigation_active = false;
    m_thin_region_navigation_index = 0;
    m_quality_check_busy = true;
    m_status->SetLabel(_L("正在重新检查当前 3D 模型..."));
    refresh_model_quality_card();
    refresh_controls();
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.check_saved_artifact(job_id, artifact_sha256,
        [weak, sequence, job_id, model_path, revision, artifact_sha256](AIModelGenerationClient::JobStatus status) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, job_id, model_path, revision, artifact_sha256, status = std::move(status)]() mutable {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence ||
                    weak->m_displayed_model_job_id != job_id || weak->m_displayed_model_path != model_path ||
                    weak->m_quality_request_revision != revision)
                    return;
                weak->m_quality_check_busy = false;
                bool current_artifact = false;
                try { current_artifact = AI::model_artifact_sha256(model_path) == artifact_sha256; }
                catch (const std::exception&) {}
                if (!current_artifact || status.id != job_id ||
                    !model_quality_matches_artifact(status.model_quality, artifact_sha256)) {
                    weak->m_quality_check_failed = true;
                    weak->m_status->SetLabel(_L("模型版本或检查单位不匹配；已保留模型与编辑，请重新加载后检查。"));
                    weak->refresh_model_quality_card();
                    weak->refresh_controls();
                    return;
                }
                weak->apply_model_quality(status.model_quality);
                weak->m_quality_check_failed = !status.model_quality.available;
                weak->refresh_model_quality_card();
                weak->m_status->SetLabel(!status.model_quality.available
                    ? _L("检查未完成，模型与编辑已保留。") : status.model_quality.status == "pass"
                    ? _L("结构检查通过。") : status.model_quality.status == "review"
                    ? _L("结构检查完成，建议复核提示项。") : status.model_quality.status == "reject"
                    ? _L("结构检查未通过，请先复核结构问题。") : _L("检查未完成，模型与编辑已保留。"));
                weak->refresh_controls();
            });
        },
        [weak, sequence, job_id, model_path, revision, artifact_sha256](std::string error) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, job_id, model_path, revision, error = std::move(error)]() {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence ||
                    weak->m_displayed_model_job_id != job_id || weak->m_displayed_model_path != model_path ||
                    weak->m_quality_request_revision != revision)
                    return;
                weak->m_quality_check_busy = false;
                weak->m_quality_check_failed = true;
                BOOST_LOG_TRIVIAL(warning) << "Saved-model check failed: " << error;
                weak->m_status->SetLabel(model_check_failure_message(error));
                weak->refresh_model_quality_card();
                weak->refresh_controls();
            });
        });
}

void ModelGenerationPanel::on_visual_review_model(wxCommandEvent&)
{
    if (m_visual_check_busy || m_quality_check_busy || m_displayed_model_job_id.empty() || !m_model_preview_ready)
        return;
    MessageDialog confirm(
        this,
        _L("要生成当前模型的五视图并调用 AI 对照检查吗？\n\n"
           "会重点检查主体/人脸与原图的相似度，以及肤色、衣物、头发和底座是否串色。\n"
           "五视图会发送给 AI 服务，此操作可能消耗 API 额度；严重的人脸偏差或材料串色会提示不建议导入，但仍可手动确认继续。"),
        _L("确认 AI 视觉复核"), wxYES_NO | wxICON_QUESTION);
    if (confirm.ShowModal() != wxID_YES)
        return;
    const std::string job_id = m_displayed_model_job_id;
    const uint64_t sequence = m_sequence;
    m_visual_check_busy = true;
    m_status->SetLabel(_L("正在生成多视角并进行 AI 外观复核..."));
    refresh_model_quality_card();
    refresh_controls();
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.visual_review(job_id,
        [weak, sequence, job_id](AIModelGenerationClient::JobStatus status) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, job_id, status = std::move(status)]() mutable {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence ||
                    weak->m_displayed_model_job_id != job_id)
                    return;
                weak->m_visual_check_busy = false;
                weak->apply_visual_quality(status.visual_quality);
                weak->apply_model_refinement(status.refinement);
                weak->m_status->SetLabel(status.visual_quality.status == "pass"
                    ? _L("AI 视觉复核完成，未发现明显外观风险。")
                    : status.visual_quality.status == "review"
                    ? status.visual_quality.import_recommended
                        ? _L("AI 视觉复核完成，建议人工确认提示项。")
                        : _L("AI 外观存在提醒，不建议直接导入；请查看人脸和串色风险。")
                    : _L("AI 视觉复核暂不可用，可稍后重试。"));
                weak->refresh_controls();
            });
        },
        [weak, sequence, job_id](std::string error) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, job_id, error = std::move(error)]() {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence ||
                    weak->m_displayed_model_job_id != job_id)
                    return;
                weak->m_visual_check_busy = false;
                weak->m_status->SetLabel(_L("无法完成 AI 视觉复核：") + from_u8(error));
                weak->refresh_model_quality_card();
                weak->refresh_controls();
            });
        });
}

void ModelGenerationPanel::on_apply_model_refinement(wxCommandEvent&)
{
    if (m_busy || !m_model_refinement.available || m_model_refinement.prompt_suffix.empty())
        return;
    const wxString suffix = from_u8(m_model_refinement.prompt_suffix);
    wxString prompt = m_prompt->GetValue();
    if (prompt.Find(suffix) != wxNOT_FOUND) {
        m_status->SetLabel(_L("优化建议已在文字输入中，不会重复添加。"));
        refresh_controls();
        return;
    }
    const wxString candidate = prompt + (prompt.empty() ? wxString() : _L("\n\n")) + suffix;
    const auto encoded = candidate.ToUTF8();
    if (!encoded || encoded.length() > MAX_MODEL_INPUT_BYTES) {
        m_status->SetLabel(_L("文字输入接近长度上限，请先精简原描述再应用优化建议。"));
        return;
    }
    m_prompt->ChangeValue(candidate);
    m_prompt->SetInsertionPointEnd();
    m_prompt->SetFocus();
    refresh_controls();
    m_status->SetLabel(_L("优化建议已加入下一次输入；尚未调用付费服务，请检查后重新生成图片预览。"));
    m_result_summary->SetLabel(_L("当前模型和质量报告仍保留，可与下一次生成结果对比。"));
}

void ModelGenerationPanel::refresh_local_recolor_controls()
{
    if (m_locate_overhang_regions != nullptr)
        m_locate_overhang_regions->Enable(m_model_preview_ready && !m_busy);
    if (m_locate_thin_regions != nullptr) {
        m_locate_thin_regions->SetLabel(
            m_thin_region_navigation_active && m_model_quality.thin_local_regions.size() > 1
                ? _L("下一处薄壁") : _L("定位薄壁"));
        m_locate_thin_regions->Enable(
            m_model_preview_ready && !m_busy && !m_model_quality.thin_local_face_indices.empty());
    }
    if (m_model_preview != nullptr)
        m_model_preview->set_selection_enabled(m_finishing_workbench && m_beauty_controls &&
            m_model_preview_ready && !m_busy && m_finishing_candidate.empty());
}

std::vector<size_t> ModelGenerationPanel::valid_project_slots() const
{
    return m_palette_provider.printable_palette().valid_slots;
}

std::vector<size_t> ModelGenerationPanel::compatible_project_slots() const
{
    return m_palette_provider.printable_palette().compatible_slots;
}

std::vector<std::string> ModelGenerationPanel::project_palette() const
{
    return m_palette_provider.printable_palette().compatible_colors;
}

std::vector<std::string> ModelGenerationPanel::current_palette() const
{
    if (!use_printable_colors())
        return {};
    auto palette = m_legacy_generation_state.palette_source == 2 ? m_custom_palette : project_palette();
    if (current_style() == "sculpture" && palette.size() > 1)
        palette.resize(1);
    return palette;
}

size_t ModelGenerationPanel::current_palette_color_count() const
{
    if (current_style() == "sculpture")
        return 1;
    return m_legacy_generation_state.palette_color_count;
}

AIModelGenerationClient::PaletteRoles ModelGenerationPanel::current_palette_roles() const
{
    if (!use_printable_colors())
        return {};
    const std::vector<std::string> palette = current_palette();
    if (palette.empty())
        return {};
    const size_t expected_roles = std::min(palette.size(), PALETTE_ROLE_IDS.size());
    std::set<std::string> assigned_colors;
    AIModelGenerationClient::PaletteRoles active_roles;
    for (size_t index = 0; index < expected_roles; ++index) {
        const auto role = m_palette_roles.find(PALETTE_ROLE_IDS[index]);
        if (role == m_palette_roles.end()
            || std::find(palette.begin(), palette.end(), role->second) == palette.end()
            || !assigned_colors.insert(role->second).second)
            return automatic_palette_roles(palette);
        active_roles.emplace(role->first, role->second);
    }
    return active_roles;
}

void ModelGenerationPanel::refresh_palette_roles(const std::vector<std::string>& palette)
{
    if (palette != m_palette_roles_source) {
        m_palette_roles_source = palette;
        m_palette_roles = automatic_palette_roles(palette);
    }
}

void ModelGenerationPanel::request_style_recommendation()
{
    if (m_shutdown || m_selected_image_path.empty())
        return;
    const uint64_t sequence = ++m_style_recommendation_sequence;
    const boost::filesystem::path image_path = m_selected_image_path;
    m_style_recommendation_loading = m_service_available;
    m_style_recommendation_available = false;
    m_style_recommendation = {};
    refresh_style_recommendation();
    if (!m_service_available) {
        refresh_controls();
        return;
    }

    wxWeakRef<ModelGenerationPanel> weak(this);
    m_client.recommend_image_style(
        m_prompt->GetValue().ToUTF8().data(), image_path,
        [weak, sequence, image_path](AIModelGenerationClient::StyleRecommendation recommendation) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, image_path, recommendation = std::move(recommendation)]() mutable {
                if (!weak || weak->m_shutdown || sequence != weak->m_style_recommendation_sequence ||
                    image_path != weak->m_selected_image_path)
                    return;
                weak->m_style_recommendation_loading = false;
                weak->m_style_recommendation_available = true;
                weak->m_style_recommendation = std::move(recommendation);
                if (!weak->m_style_user_selected)
                    weak->select_style(weak->m_style_recommendation.primary, false);
                else
                    weak->refresh_controls();
            });
        },
        [weak, sequence, image_path](std::string error) {
            if (!weak) return;
            BOOST_LOG_TRIVIAL(warning) << "Local model style recommendation failed: " << error;
            wxGetApp().CallAfter([weak, sequence, image_path]() {
                if (!weak || weak->m_shutdown || sequence != weak->m_style_recommendation_sequence ||
                    image_path != weak->m_selected_image_path)
                    return;
                weak->m_style_recommendation_loading = false;
                weak->m_style_recommendation_available = false;
                weak->refresh_controls();
            });
        });
}

void ModelGenerationPanel::select_style(const std::string& style, bool user_selected)
{
    if (m_style == nullptr)
        return;
    m_style->SetToolTip(wxString());
    m_style->SetSelection(style_selection(style));
    m_stylized_style->SetSelection(stylized_style_selection(style));
    if (user_selected)
        m_style_user_selected = true;
    const bool multicolor = style_uses_printable_colors(current_style());
    if (m_import_color_mode != nullptr)
        m_import_color_mode->SetSelection(multicolor ? 0 : 2);
    refresh_palette();
    refresh_controls();
}

void ModelGenerationPanel::refresh_style_recommendation()
{
    if (m_style_recommendation_panel == nullptr)
        return;
    const bool show = has_image_input() &&
        (m_style_recommendation_loading || m_style_recommendation_available);
    m_style_recommendation_panel->Show(show);
    if (!show)
        return;

    const bool alternatives_visible = m_style_recommendation_available &&
        m_style_recommendation.alternatives.size() == m_style_recommendation_alternatives.size();
    if (m_style_recommendation_loading) {
        m_style_recommendation_title->SetLabel(_L("正在推荐风格..."));
        m_style_recommendation_reason->SetLabel(_L("只在本机分析，不会调用付费服务。"));
    } else if (m_style_recommendation_available) {
        m_style_recommendation_title->SetLabel(
            _L("推荐：") + style_label(m_style_recommendation.primary));
        m_style_recommendation_reason->SetLabel(
            style_recommendation_reason(m_style_recommendation.reason));
    } else {
        m_style_recommendation_title->SetLabel(_L("风格推荐暂不可用"));
        m_style_recommendation_reason->SetLabel(_L("可直接使用上方风格选择，不影响继续生成。"));
    }
    m_style_recommendation_reason->Wrap(FromDIP(290));
    m_style_recommendation_alternative_label->Show(alternatives_visible);
    for (size_t index = 0; index < m_style_recommendation_alternatives.size(); ++index) {
        wxButton* button = m_style_recommendation_alternatives[index];
        button->Show(alternatives_visible);
        if (alternatives_visible)
            button->SetLabel(style_label(m_style_recommendation.alternatives[index]));
        button->Enable(alternatives_visible && !m_busy);
    }
    m_style_recommendation_panel->Layout();
}

bool ModelGenerationPanel::use_printable_colors() const
{
    // Color constraints belong to post-generation Orca material matching.
    return false;
}

std::string ModelGenerationPanel::current_style() const
{
    if (m_style != nullptr && m_style->GetSelection() == wxNOT_FOUND)
        return {};
    return selected_style(m_style == nullptr ? 0 : m_style->GetSelection(),
                          m_stylized_style == nullptr ? 1 : m_stylized_style->GetSelection());
}

std::string ModelGenerationPanel::current_custom_style() const
{
    if (m_custom_style == nullptr || current_style() != "custom")
        return {};
    wxString value = m_custom_style->GetValue();
    value.Trim(true).Trim(false);
    return value.ToUTF8().data();
}

wxString ModelGenerationPanel::current_style_label() const
{
    return style_label(current_style());
}

int ModelGenerationPanel::current_face_limit() const
{
    const int selection = m_quality ? m_quality->GetSelection() : 1;
    return selection == 0 ? 300000 : selection == 2 && (!m_provider || m_provider->GetSelection() != 1) ? 2000000 : 1000000;
}

std::string ModelGenerationPanel::current_generation_profile() const
{
    return current_face_limit() <= 300000 ? "performance" : "quality";
}

wxString ModelGenerationPanel::current_generation_profile_label() const
{
    return current_generation_profile() == "performance" ? _L("高性能") : _L("高质量（推荐）");
}

AIModelGenerationClient::GenerationOptions ModelGenerationPanel::current_generation_options() const
{
    AIModelGenerationClient::GenerationOptions options;
    options.provider = m_provider && m_provider->GetSelection() == 1 ? "hunyuan" : "tripo";
    options.face_limit = current_face_limit();
    options.geometry_quality = options.provider == "tripo" && m_geometry_quality && m_geometry_quality->GetSelection() == 1 ? "detailed" : "standard";
    const int texture = options.provider == "tripo" && m_texture_quality ? m_texture_quality->GetSelection() : 0;
    options.texture_quality = texture == 2 ? "extreme" : texture == 1 ? "detailed" : "standard";
    options.output_format = m_output_format && m_output_format->GetSelection() == 1 ? "obj" : "glb";
    return options;
}

void ModelGenerationPanel::refresh_provider_options()
{
    if (!m_provider || !m_quality || !m_geometry_quality || !m_texture_quality || !m_output_format)
        return;
    const bool hunyuan = m_provider->GetSelection() == 1;
    if (hunyuan && m_quality->GetCount() == 3) {
        if (m_quality->GetSelection() == 2) m_quality->SetSelection(1);
        m_quality->Delete(2);
    } else if (!hunyuan && m_quality->GetCount() == 2) {
        m_quality->Append(_L("200 万面（需精细几何）"));
    }
    m_quality->SetToolTip(hunyuan
        ? _L("面数是生成目标，实际结果可能不同。腾讯混元3D 可选择 30 万或 100 万面。")
        : _L("面数是生成目标，实际结果可能不同。200 万面需 Tripo v3.1 精细几何；更多面数会增加下载、预览和导入耗时。"));
    if (hunyuan && m_geometry_quality->GetCount() != 1) {
        m_geometry_quality->Set(wxArrayString {_L("标准")});
        m_texture_quality->Set(wxArrayString {_L("标准")});
        m_geometry_quality->SetSelection(0);
        m_texture_quality->SetSelection(0);
    } else if (!hunyuan && m_geometry_quality->GetCount() == 1) {
        m_geometry_quality->Set(wxArrayString {_L("标准"), _L("精细")});
        m_texture_quality->Set(wxArrayString {_L("标准"), _L("高清"), _L("8K")});
        m_geometry_quality->SetSelection(0);
        m_texture_quality->SetSelection(0);
    }
    m_output_format->SetString(0, hunyuan ? wxString("GLB") : _L("GLB"));
    m_output_format->SetString(1, hunyuan ? wxString("OBJ") : _L("OBJ（另建转换任务）"));
}

bool ModelGenerationPanel::generation_options_valid() const
{
    const auto options = current_generation_options();
    return options.face_limit != 2000000 || options.geometry_quality == "detailed";
}

wxString ModelGenerationPanel::generation_options_summary(bool) const
{
    const auto options = current_generation_options();
    if (options.provider == "hunyuan") {
        return wxString::Format(_L("腾讯混元3D · %d 万面 · %s\n费用未报价，金额未知；按腾讯云账户套餐或额度结算，以实际账单为准。\nOBJ 使用生成结果，无额外格式转换任务；不含 2D 设计图费用。"),
            options.face_limit / 10000, options.output_format == "obj" ? "OBJ" : "GLB");
    }
    wxString summary = wxString::Format(_L("%d 万面 · %s几何 · %s纹理 · %s\n"), options.face_limit / 10000,
        options.geometry_quality == "detailed" ? _L("精细") : _L("标准"),
        options.texture_quality == "extreme" ? _L("8K") : options.texture_quality == "detailed" ? _L("高清") : _L("标准"),
        options.output_format == "obj" ? "OBJ" : "GLB");
    // A configurable provider/model has no quote in this client contract.
    // A public rate card is not a quote for the active account and request.
    summary += _L("Tripo 费用未报价，积分未知。\n不含 2D 设计图费用，实际以服务商账单为准。");
    if (!generation_options_valid())
        summary += _L("\n200 万面需精细几何。");
    return summary;
}

AIModelGenerationClient::ImagePrintSettings ModelGenerationPanel::current_print_settings() const
{
    AIModelGenerationClient::ImagePrintSettings settings;
    settings.width_mm = m_legacy_generation_state.print_width_mm;
    settings.nozzle_mm = m_legacy_generation_state.nozzle_mm;
    settings.line_width_mm = m_legacy_generation_state.line_width_mm;
    settings.minimum_feature_mm = m_legacy_generation_state.minimum_feature_mm;
    // The former shadow choice was never constructed; preserve the default.
    return settings;
}

bool ModelGenerationPanel::has_image_input() const
{
    return !m_selected_image_path.empty();
}

bool ModelGenerationPanel::job_uses_image() const
{
    return !m_job_image_path.empty();
}

bool ModelGenerationPanel::job_inputs_match() const
{
    return job_base_inputs_match() &&
           (!use_printable_colors() || m_awaiting_palette_confirmation || current_palette_roles() == m_job_palette_roles);
}

bool ModelGenerationPanel::job_base_inputs_match() const
{
    const auto settings = current_print_settings();
    const bool print_matches = std::abs(settings.width_mm - m_job_print_settings.width_mm) < 0.001 &&
        std::abs(settings.nozzle_mm - m_job_print_settings.nozzle_mm) < 0.001 &&
        std::abs(settings.line_width_mm - m_job_print_settings.line_width_mm) < 0.001 &&
        std::abs(settings.minimum_feature_mm - m_job_print_settings.minimum_feature_mm) < 0.001 &&
        settings.shadow_color == m_job_print_settings.shadow_color;
    const bool palette_count_matches = !use_printable_colors() || m_legacy_generation_state.palette_source != 2 ||
                                       current_palette_color_count() == m_job_palette_color_count;
    return m_job_id.empty() || (m_prompt->GetValue() == m_job_prompt && m_selected_image_path == m_job_image_path &&
                                 current_style() == m_job_style && current_custom_style() == m_job_custom_style &&
                                 palette_count_matches && print_matches);
}

void ModelGenerationPanel::refresh_palette()
{
    const auto palette = current_palette();
    refresh_palette_roles(palette);
    const bool custom = m_legacy_generation_state.palette_source != 0;
    if (palette != m_palette || custom != m_palette_is_custom) {
        m_palette = palette;
        m_palette_is_custom = custom;
    }
}

void ModelGenerationPanel::reset(bool remove_remote)
{
    if (m_preview_canceled) *m_preview_canceled = true;
    m_preserve_image_on_model_ready = false;
    m_saving_generation_options = false;
    ++m_design_history_sequence;
    m_design_history_loading = false;
    m_restoring_input = false;
    m_poll_timer.Stop();
    m_client.cancel_current();
    const std::string old_job = m_job_id;
    const bool keep_assets = !old_job.empty() && has_persisted_generation_assets(generated_models_root(), old_job);
    ++m_sequence;
    cleanup_files();
    m_job_id.clear();
    m_job_state.clear();
    m_job_phase.clear();
    m_job_provider_name.clear();
    m_job_provider_task_id.clear();
    m_job_provider_conversion_task_id.clear();
    m_job_palette.clear();
    m_job_palette_roles.clear();
    m_job_palette_color_count = Slic3r::AI::kLegacyDefaultTargetPaletteColors;
    m_job_use_printable_colors = false;
    m_job_prompt.clear();
    m_job_style.clear();
    m_job_custom_style.clear();
    m_job_face_limit = 1000000;
    m_job_generation_profile = "quality";
    m_job_generation_options = {};
    m_job_image_path.clear();
    m_job_preview_expected = false;
    m_artifact_format.clear();
    m_artifact_color_encoding.clear();
    m_busy = false;
    m_poll_connection_failures = 0;
    m_awaiting_confirmation = false;
    m_awaiting_palette_confirmation = false;
    m_palette_recommendation_confirmed = false;
    m_ready = false;
    m_artifact_download_started = false;
    m_model_preview_ready = false;
    m_library_model_loaded = false;
    m_displayed_model_path.clear();
    m_displayed_model_job_id.clear();
    m_displayed_model_palette.clear();
    m_displayed_model_palette_roles.clear();
    clear_model_quality();
    if (m_model_preview != nullptr)
        m_model_preview->clear();
    if (m_preview_book != nullptr)
        m_preview_book->SetSelection(0);
    if (m_model_stats != nullptr)
        m_model_stats->SetLabel(_L("模型生成后将在这里显示"));
    if (m_model_preview_message != nullptr)
        m_model_preview_message->SetLabel(_L("生成完成后可拖动旋转模型，并使用滚轮缩放。"));
    m_style_preview_ready = false;
    m_preview_download_in_flight = false;
    m_preview_download_cancelled = false;
    m_preview_output_available = false;
    m_raw_preview_available = false;
    m_model_reference_available = false;
    m_strict_preview_available = false;
    m_model_views_available = false;
    m_heatmap_available = false;
    m_palette_quality_ok = true;
    m_material_fragmentation_ok = true;
    m_model_input_eligible = true;
    m_model_input_primary_blocker.clear();
    m_meaningful_palette_count = 0;
    m_meaningful_subject_color_count = 0;
    m_generation_progress->SetValue(0);
    update_workflow();
    m_status->SetLabel(_L("空闲"));
    m_prepared_prompt->Clear();
    set_preview_empty(_L("请先输入描述或选择参考图。"));
    if (!m_selected_image_path.empty())
        show_selected_image_preview();
    m_result_summary->SetLabel(_L("尚未生成模型。"));
    if (remove_remote && !old_job.empty() && !keep_assets)
        m_client.remove(old_job, [] {}, [](std::string) {});
    refresh_controls();
}

void ModelGenerationPanel::cleanup_files()
{
    m_preview_path.clear();
    m_preview_output = "preview";
    m_reference_image_path.clear();
    m_raw_preview_path.clear();
    m_artifact_path.clear();
    m_color_intent_path.clear();
    m_color_intent_schema.clear();
    m_color_intent_sha256.clear();
}


void ModelGenerationPanel::save_library_entry(size_t artifact_size, size_t triangle_count, double width,
                                               double depth, double height, size_t color_count,
                                               double load_seconds)
{
    if (m_job_id.empty() || m_displayed_model_path.empty())
        return;
    const boost::filesystem::path metadata_path = library_metadata_path(m_job_id);
    const nlohmann::json previous = read_json(metadata_path);
    std::time_t generated_at = std::time(nullptr);
    // Automatic job recovery also saves this entry. Keep its first recorded generation time.
    if (previous.is_object()) {
        const auto saved_time = previous.find("generated_at");
        if (saved_time != previous.end() && saved_time->is_number_integer()) {
            const auto timestamp = saved_time->get<std::time_t>();
            if (timestamp > 0)
                generated_at = timestamp;
        }
    }
    const boost::filesystem::path root = generated_models_root();
    const boost::filesystem::path reference_image = archive_library_image(
        !m_reference_image_path.empty() ? m_reference_image_path : m_job_image_path,
        m_job_id, "reference");
    const boost::filesystem::path ai_image = archive_library_image(
        !m_raw_preview_path.empty() ? m_raw_preview_path : m_preview_path,
        m_job_id, "ai");
    if (!reference_image.empty())
        m_reference_image_path = reference_image;
    if (!ai_image.empty())
        m_raw_preview_path = ai_image;
    // Archive the presentation separately; raw provider output remains intact.
    const wxImage& display = m_model_reference_image.IsOk() ? m_model_reference_image : m_clean_preview_image;
    if (!ai_image.empty() && display.IsOk()) {
        auto display_path = ai_image; display_path += ".display.png";
        if (display.SaveFile(display_path.wstring(), wxBITMAP_TYPE_PNG)) {
            m_history_display_source = ai_image;
            m_history_display_image = display.Copy();
        }
    }
    nlohmann::json metadata {
        {"schema_version", 6},
        {"job_id", m_job_id},
        {"model_path", m_displayed_model_path.lexically_relative(root).generic_string()},
        {"source", job_uses_image() ? (m_job_prompt.empty() ? "image" : "text_image") : "text"},
        {"style", m_job_style},
        {"custom_style", m_job_custom_style},
        {"generation_profile", m_job_generation_profile},
        {"face_limit", m_job_face_limit},
        {"provider", m_job_generation_options.provider},
        {"geometry_quality", m_job_generation_options.geometry_quality},
        {"texture_quality", m_job_generation_options.texture_quality},
        {"output_format", m_job_generation_options.output_format},
        {"prompt", std::string(m_job_prompt.ToUTF8().data())},
        {"palette", m_job_palette},
        {"palette_roles", m_job_palette_roles},
        {"use_printable_colors", m_job_use_printable_colors},
        {"generated_at", generated_at},
        {"artifact_size", artifact_size},
        {"triangle_count", triangle_count},
        {"color_count", color_count},
        {"load_seconds", load_seconds},
        {"dimensions", {width, depth, height}}
    };
    // Reloading the generated artifact refreshes its statistics, not the user's
    // import and print history for this same job.
    if (previous.is_object()) {
        for (const char* key : {"imported_at", "print_feedback", "print_feedback_at"}) {
            const auto saved = previous.find(key);
            if (saved != previous.end())
                metadata[key] = *saved;
        }
    }
    if (!m_job_provider_task_id.empty()) {
        metadata["provider"] = m_job_provider_name;
        metadata["provider_task_id"] = m_job_provider_task_id;
        if (!m_job_provider_conversion_task_id.empty())
            metadata["provider_conversion_task_id"] = m_job_provider_conversion_task_id;
    }
    if (!m_preview_path.empty())
        metadata["preview_path"] = m_preview_path.lexically_relative(root).generic_string();
    if (!reference_image.empty())
        metadata["reference_image_path"] = reference_image.lexically_relative(root).generic_string();
    if (!ai_image.empty())
        metadata["ai_image_path"] = ai_image.lexically_relative(root).generic_string();
    if (!m_color_intent_path.empty() && path_is_inside(root, m_color_intent_path)) {
        metadata["color_intent_path"] = m_color_intent_path.lexically_relative(root).generic_string();
        metadata["color_intent_schema"] = m_color_intent_schema;
        metadata["color_intent_sha256"] = m_color_intent_sha256;
    }

    boost::filesystem::ofstream stream(metadata_path);
    if (!stream) {
        BOOST_LOG_TRIVIAL(warning) << "Unable to write generated model library metadata for " << m_job_id;
    } else {
        stream << metadata.dump(2);
        stream.close();
    }
    load_library_entries();
}

void ModelGenerationPanel::load_library_entry(const boost::filesystem::path& model_path,
                                               const boost::filesystem::path& reference_image_path,
                                               const boost::filesystem::path& ai_image_path,
                                               const std::vector<std::string>& palette,
                                               const AIModelGenerationClient::PaletteRoles& palette_roles,
                                               bool use_printable_colors,
                                               const boost::filesystem::path& color_intent_path,
                                               const std::string& color_intent_schema,
                                               const std::string& color_intent_sha256,
                                               const std::string& job_id, const wxString& title,
                                               bool open_beauty_after_load)
{
    if (m_busy || m_model_preview == nullptr)
        return;
    if (model_path.empty()) {
        load_design_library_entry(job_id);
        return;
    }
    if (preserve_unsaved_finishing()) return;
    // Clicking a history entry is an explicit context switch.  Detach the
    // current preview below, but do not delete its remote job or source input.
    // This keeps history loading to one action even when a restored image
    // preview is still waiting for its paid-generation decision.
    boost::system::error_code ec;
    if (!boost::filesystem::is_regular_file(model_path, ec)) {
        m_status->SetLabel(_L("历史模型文件已不存在。"));
        return;
    }

    const auto* local_import=local_model_import_state(m_library_import);
    const auto local_cancel=local_import && local_import->parsing ? local_import->cancel : nullptr;
    const wxString previous_model_stats = m_model_stats->GetLabel();
    auto history_metadata = std::make_shared<ModelHistoryMetadata>();
    m_status->SetLabel(_L("正在加载历史模型：") + title);
    m_model_stats->SetLabel(_L("正在解析模型..."));
    std::string loaded_artifact_sha256;
    try { loaded_artifact_sha256 = AI::model_artifact_sha256(model_path); }
    catch (const std::exception&) {}
    load_model_preview_async(model_path, palette,
        [=](size_t triangle_count, Vec3d dimensions, size_t color_count, double load_seconds) {
    // Avoid repainting intermediate control and notebook states while restoring history.
    // History restoration changes several layout inputs while repainting is
    // frozen. Fit the comparison once after all of those inputs are installed.
    const bool comparison_was_updating = m_updating_comparison_layout;
    m_updating_comparison_layout = true;
    ScopeGuard comparison_restore([this, comparison_was_updating] {
        m_updating_comparison_layout = comparison_was_updating;
    });
    wxWindowUpdateLocker update_locker(this);
    // Successful explicit history navigation starts a fresh editing context,
    // including when the user selects the same source file again.
    if (m_prepare_base) m_prepare_base->SetValue(false);
    if (!m_finishing_candidate.empty()) {
        boost::system::error_code ignored;
        if (m_finishing_candidate != model_path) boost::filesystem::remove(m_finishing_candidate, ignored);
        m_finishing_candidate.clear();
    }
    m_finishing_options.reset();
    m_finishing_before = false;
    const wxImage reference_image = reference_image_path.empty() ? wxImage() : wxImage(reference_image_path.wstring());
    const wxImage ai_image = ai_image_path.empty() ? wxImage() : wxImage(ai_image_path.wstring());
    m_history_display_image = load_model_image_display_copy(ai_image_path);
    m_history_display_source = ai_image_path;
    nlohmann::json metadata;
    std::map<std::string, std::string> encoded_fields;
    if (!history_metadata->take_if_current(library_metadata_path(job_id), metadata, encoded_fields))
        metadata = read_json(library_metadata_path(job_id));
    if (metadata.is_object()) {
        metadata["schema_version"] = std::max(4, metadata.value("schema_version", 0));
        metadata["triangle_count"] = triangle_count;
        metadata["color_count"] = color_count;
        metadata["load_seconds"] = load_seconds;
        metadata["dimensions"] = {dimensions.x(), dimensions.y(), dimensions.z()};
        const bool saved = encoded_fields.empty() ? write_json(library_metadata_path(job_id), metadata)
            : write_json_with_preencoded_fields(library_metadata_path(job_id), metadata, encoded_fields);
        if (!saved)
            BOOST_LOG_TRIVIAL(warning) << "Unable to update model load metrics for " << job_id;
    }

    m_poll_timer.Stop();
    m_client.cancel_current();
    ++m_sequence;
    // The old download callbacks are invalidated above. Clear their busy and
    // output state only after the historical model has loaded successfully.
    m_preview_download_in_flight = false;
    m_preview_download_cancelled = false;
    m_preview_output_available = false;
    m_preview_output = "preview";
    m_preview_path.clear();
    m_job_id.clear();
    m_job_palette = palette;
    m_job_palette_roles = palette_roles.empty() ? automatic_palette_roles(palette) : palette_roles;
    m_job_use_printable_colors = use_printable_colors;
    m_custom_palette = palette;
    m_palette_roles = m_job_palette_roles;
    m_palette_roles_source = palette;
    m_legacy_generation_state.palette_source = use_printable_colors && !palette.empty() ? 2 : 1;
    m_palette_recommendation_confirmed = !palette.empty();
    m_awaiting_palette_confirmation = false;
    // Replace the visible input atomically after the new model has loaded.
    // Legacy records may have no input context; never borrow it from the last model.
    const auto input_string = [&metadata](const char* key) {
        const auto value = metadata.find(key);
        return value != metadata.end() && value->is_string() ? value->get<std::string>() : std::string();
    };
    m_job_generation_options = AIModelGenerationClient::restore_generation_options(metadata);
    const std::string& texture_quality = m_job_generation_options.texture_quality;
    const auto saved_face_limit = metadata.find("face_limit");
    const int historical_face_limit = saved_face_limit != metadata.end() && saved_face_limit->is_number_integer()
        ? saved_face_limit->get<int>() : 1000000;
    m_job_face_limit = m_job_generation_options.face_limit;
    m_legacy_generation_defaults = historical_face_limit != m_job_face_limit;
    m_job_generation_profile = m_job_face_limit <= 300000 ? "performance" : "quality";
    m_provider->SetSelection(m_job_generation_options.provider == "hunyuan" ? 1 : 0);
    refresh_provider_options();
    m_quality->SetSelection(m_job_face_limit <= 300000 ? 0 : m_job_face_limit == 2000000 && m_quality->GetCount() == 3 ? 2 : 1);
    m_geometry_quality->SetSelection(m_geometry_quality->GetCount() > 1 && m_job_generation_options.geometry_quality == "detailed" ? 1 : 0);
    m_texture_quality->SetSelection(m_texture_quality->GetCount() == 1 ? 0 : texture_quality == "extreme" ? 2 : texture_quality == "detailed" ? 1 : 0);
    m_output_format->SetSelection(m_job_generation_options.output_format == "obj" ? 1 : 0);
    m_job_provider_name = m_job_generation_options.provider;
    m_job_provider_task_id = input_string("provider_task_id");
    m_job_provider_conversion_task_id = input_string("provider_conversion_task_id");
    std::string saved_prompt = input_string("prompt");
    if (saved_prompt == INTERNAL_DEFAULT_IMAGE_INSTRUCTION || input_string("source") == "local_finishing" || input_string("source")=="local_import")
        saved_prompt.clear();
    m_job_prompt = from_u8(saved_prompt);
    m_job_style = input_string("style");
    m_job_custom_style = input_string("custom_style");
    m_prompt->ChangeValue(m_job_prompt);
    m_prepared_prompt->ChangeValue(wxString());
    m_custom_style->ChangeValue(from_u8(m_job_custom_style));
    m_style->SetSelection(m_job_style.empty() ? wxNOT_FOUND : style_selection(m_job_style));
    m_stylized_style->SetSelection(m_job_style.empty() ? wxNOT_FOUND : stylized_style_selection(m_job_style));
    m_style->SetToolTip(m_job_style.empty() ? _L("该历史记录未保存风格；再次生成前请选择风格。") : wxString());
    m_style_user_selected = !m_job_style.empty();
    ++m_style_recommendation_sequence;
    m_style_recommendation_loading = false;
    m_style_recommendation_available = false;
    m_style_recommendation = {};
    m_restoring_input = false;
    m_job_image_path = reference_image.IsOk() ? reference_image_path : boost::filesystem::path();
    m_selected_image_path = m_job_image_path;
    m_selected_image->SetLabel(m_selected_image_path.empty()
        ? _L("该历史记录没有可用参考图") : wxString(m_selected_image_path.filename().wstring()));
    m_job_preview_expected = reference_image.IsOk() || ai_image.IsOk();
    m_reference_image_path = reference_image.IsOk() ? reference_image_path : boost::filesystem::path();
    m_raw_preview_path = ai_image.IsOk() ? ai_image_path : boost::filesystem::path();
    m_reference_image = reference_image.IsOk() ? reference_image.Copy() : wxImage();
    m_raw_preview_image = ai_image.IsOk() ? ai_image.Copy() : wxImage();
    m_model_reference_image = wxImage();
    m_strict_preview_image = wxImage();
    m_model_views_image = wxImage();
    m_clean_preview_image = ai_image.IsOk() ? ai_image.Copy() : wxImage();
    m_heatmap_image = wxImage();
    m_raw_preview_available = ai_image.IsOk();
    m_model_reference_available = false;
    m_strict_preview_available = false;
    m_model_views_available = false;
    m_heatmap_available = false;
    m_style_preview_ready = ai_image.IsOk();
    m_style_preview_placeholder = ai_image.IsOk()
        ? wxString()
        : _L("该历史记录未保存 AI 生成图");
    if (m_preview_stage != nullptr)
        m_preview_stage->SetSelection(0);
    m_artifact_path = model_path;
    m_artifact_format = AI::model_artifact_format(model_path);
    m_artifact_color_encoding = m_artifact_format == "glb" ? "textures_or_vertex_colors" : "vertex_colors";
    m_color_intent_path = color_intent_path;
    m_color_intent_schema = color_intent_schema;
    m_color_intent_sha256 = color_intent_sha256;
    m_busy = false;
    m_awaiting_confirmation = false;
    m_ready = true;
    m_artifact_download_started = true;
    m_displayed_model_path = model_path;
    m_displayed_model_job_id = job_id;
    m_displayed_model_palette = palette;
    m_displayed_model_palette_roles = m_job_palette_roles;
    m_model_preview_ready = true;
    if (m_finishing_workbench && !open_beauty_after_load)
        set_finishing_workbench(false);
    if (m_finishing_status != nullptr)
        m_finishing_status->SetLabel(m_artifact_format == "glb"
            ? _L("点选拼图改色，调整范围后保存新版本。原件和未改区域的纹理保留。")
            : _L("当前历史模型已加载，处理结果将另存为新版本。"));
    show_model_comparison();
    m_library_model_loaded = true;
    clear_model_quality();
    update_progress(100, 4, _L("检查并导入"));
    set_model_viewport_facts(m_model_stats, triangle_count, color_count);
    m_model_stats->SetLabel(wxString::Format(
        _L("%llu 个三角面 · %llu 个原始色值\n%.1f × %.1f × %.1f mm\n%s"),
        static_cast<unsigned long long>(triangle_count), static_cast<unsigned long long>(color_count),
        dimensions.x(), dimensions.y(), dimensions.z(),
        model_load_summary(triangle_count, load_seconds).c_str()));
    m_model_preview_message->SetLabel(_L("历史模型已自动摆正；可同时对照原图和 AI 图后再导入。"));
    m_status->SetLabel(_L("已加载历史模型：") + title);
    m_result_summary->SetLabel(_L("历史模型已加载到结果对照，可继续检查图片并导入准备页。"));
    m_preview_message->SetLabel(reference_image.IsOk() && ai_image.IsOk()
        ? _L("历史素材已恢复：原图与 AI 生成图可同屏对照。")
        : reference_image.IsOk()
            ? _L("已恢复原图；该历史记录未保存 AI 生成图。")
            : ai_image.IsOk()
                ? _L("已恢复 AI 生成图；该任务没有可用原图。")
                : _L("旧历史记录未保存关联图片，3D 模型仍可正常预览。"));
    if (m_preview_book != nullptr)
        m_preview_book->SetSelection(0);
    apply_preview_stage(true);
    m_model_preview->refresh();
    refresh_controls();
    m_updating_comparison_layout = comparison_was_updating;
    refresh_comparison_layout(true);
    if (open_beauty_after_load) {
        set_finishing_workbench(true);
        m_finishing_status->SetLabel(_L("已创建独立 GLB 美颜副本；原 OBJ 模型与 Task ID 保留。"));
    }
    const uint64_t sequence = m_sequence;
    wxWeakRef<ModelGenerationPanel> weak(this);
    const uint64_t quality_revision = m_quality_request_revision;
    const auto restore_saved_quality = [weak, sequence, job_id, model_path, loaded_artifact_sha256, quality_revision]() {
        if (!weak || weak->m_shutdown || sequence != weak->m_sequence ||
            weak->m_displayed_model_job_id != job_id || weak->m_displayed_model_path != model_path ||
            weak->m_quality_request_revision != quality_revision)
            return;
        weak->m_client.check_saved_artifact(job_id, loaded_artifact_sha256,
            [weak, sequence, job_id, model_path, loaded_artifact_sha256, quality_revision](AIModelGenerationClient::JobStatus saved) mutable {
                if (!weak) return;
                wxGetApp().CallAfter([weak, sequence, job_id, model_path, loaded_artifact_sha256, quality_revision,
                                     saved = std::move(saved)]() mutable {
                    if (!weak || weak->m_shutdown || sequence != weak->m_sequence ||
                        weak->m_displayed_model_job_id != job_id || weak->m_displayed_model_path != model_path ||
                        weak->m_quality_request_revision != quality_revision || saved.id != job_id ||
                        !model_quality_matches_artifact(saved.model_quality, loaded_artifact_sha256))
                        return;
                    try {
                        if (AI::model_artifact_sha256(model_path) != loaded_artifact_sha256) return;
                    } catch (const std::exception&) { return; }
                    weak->apply_model_quality(saved.model_quality);
                    weak->m_status->SetLabel(_L("已恢复当前保存版本的检查报告。"));
                    weak->refresh_controls();
                });
            }, [](std::string) {}, true);
    };
    m_client.get_status(job_id,
        [weak, sequence, job_id, model_path, loaded_artifact_sha256, quality_revision, restore_saved_quality](AIModelGenerationClient::JobStatus status) mutable {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, job_id, model_path, loaded_artifact_sha256, quality_revision, restore_saved_quality, status = std::move(status)]() mutable {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence ||
                    weak->m_displayed_model_job_id != job_id || weak->m_displayed_model_path != model_path ||
                    status.id != job_id)
                    return;
                if (!status.palette_roles.empty() && status.palette == weak->m_displayed_model_palette) {
                    weak->m_job_palette_roles = status.palette_roles;
                    weak->m_displayed_model_palette_roles = status.palette_roles;
                }
                weak->update_library_provider_tasks(job_id, status);
                if (weak->m_quality_request_revision == quality_revision && status.model_quality.available) {
                    bool current_artifact = false;
                    if (model_quality_matches_artifact(status.model_quality, loaded_artifact_sha256)) {
                        try { current_artifact = AI::model_artifact_sha256(model_path) == loaded_artifact_sha256; }
                        catch (const std::exception&) {}
                    }
                    if (current_artifact)
                        weak->apply_model_quality(status.model_quality);
                    else {
                        // A legacy or changed report is not a verdict for the loaded model.
                        weak->m_status->SetLabel(_L("历史检查报告无法对应当前模型或检查版本；请重新检查。"));
                        weak->refresh_model_quality_card();
                    }
                }
                weak->apply_visual_quality(status.visual_quality);
                weak->apply_model_refinement(status.refinement);
                weak->refresh_controls();
                if (!model_quality_matches_artifact(status.model_quality, loaded_artifact_sha256))
                    restore_saved_quality();
            });
        },
        [weak, sequence, job_id, quality_revision, restore_saved_quality](std::string) {
            if (!weak) return;
            wxGetApp().CallAfter([weak, sequence, job_id, quality_revision, restore_saved_quality]() {
                if (!weak || weak->m_shutdown || sequence != weak->m_sequence ||
                    weak->m_displayed_model_job_id != job_id)
                    return;
                weak->refresh_model_quality_card();
                weak->refresh_controls();
                restore_saved_quality();
            });
        });
    }, [this, previous_model_stats, open_beauty_after_load, local_cancel](std::string error) {
        m_model_stats->SetLabel(previous_model_stats);
        if(local_cancel && local_cancel->load()) {
            const wxString before=m_status->GetLabel();
            const wxString message=open_beauty_after_load
                ? _L("已取消创建副本，原模型和当前编辑已保留。")
                : _L("已取消本地导入，原文件和当前模型已保留。");
            m_status->SetLabel(message);
            if(open_beauty_after_load) m_model_preview_message->SetLabel(message);
            load_library_entries();
            refresh_controls();
            show_library_action_feedback(before);
            return;
        }
        m_status->SetLabel(open_beauty_after_load
            ? _L("GLB 副本已保存在模型库，但加载失败；原 OBJ 模型保持不变。")
            : _L("历史模型加载失败，保留当前模型与预览。"));
        m_result_summary->SetLabel(from_u8(error));
        refresh_controls();
    }, library_metadata_path(job_id), history_metadata);
}

void ModelGenerationPanel::update_library_provider_tasks(
    const std::string& job_id, const AIModelGenerationClient::JobStatus& status)
{
    if (job_id.empty() || (status.provider_name != "tripo" && status.provider_name != "hunyuan") ||
        !valid_provider_task_id(status.provider_task_id))
        return;
    const boost::filesystem::path metadata_path = library_metadata_path(job_id);
    nlohmann::json metadata = read_json(metadata_path);
    if (!metadata.is_object())
        metadata = nlohmann::json::object();
    const bool unchanged = metadata.value("provider", std::string()) == status.provider_name &&
        metadata.value("provider_task_id", std::string()) == status.provider_task_id &&
        metadata.value("provider_conversion_task_id", std::string()) == status.provider_conversion_task_id;
    if (unchanged)
        return;
    metadata["schema_version"] = std::max(5, metadata.value("schema_version", 0));
    metadata["job_id"] = job_id;
    metadata["provider"] = status.provider_name;
    metadata["provider_task_id"] = status.provider_task_id;
    if (!status.provider_conversion_task_id.empty())
        metadata["provider_conversion_task_id"] = status.provider_conversion_task_id;
    if (!write_json(metadata_path, metadata)) {
        BOOST_LOG_TRIVIAL(warning) << "Unable to backfill provider task id for " << job_id;
        return;
    }
    load_library_entries();
}

void ModelGenerationPanel::update_library_import_status(const std::string& job_id)
{
    if (job_id.empty())
        return;
    const boost::filesystem::path metadata_path = library_metadata_path(job_id);
    nlohmann::json metadata = read_json(metadata_path);
    if (!metadata.is_object())
        metadata = nlohmann::json::object();
    const std::time_t now = std::time(nullptr);
    metadata["schema_version"] = std::max(4, metadata.value("schema_version", 0));
    metadata["job_id"] = job_id;
    metadata["imported_at"] = now;
    metadata.erase("auto_slice_requested");
    metadata.erase("slice_requested_at");
    if (!write_json(metadata_path, metadata))
        BOOST_LOG_TRIVIAL(warning) << "Unable to update generated model import status for " << job_id;
}

void ModelGenerationPanel::record_library_print_feedback(const std::string& job_id,
                                                          const std::string& feedback)
{
    if (job_id.empty() || (feedback != "success" && feedback != "issue"))
        return;
    const boost::filesystem::path metadata_path = library_metadata_path(job_id);
    nlohmann::json metadata = read_json(metadata_path);
    if (!metadata.is_object())
        return;
    metadata["schema_version"] = std::max(4, metadata.value("schema_version", 0));
    metadata["print_feedback"] = feedback;
    metadata["print_feedback_at"] = std::time(nullptr);
    if (!write_json(metadata_path, metadata)) {
        m_status->SetLabel(_L("无法保存打印结果，请检查 generated_models 是否可写。"));
        return;
    }
    m_client.record_journey_event(
        feedback == "success" ? "print_feedback_success" : "print_feedback_issue", job_id);
    m_status->SetLabel(feedback == "success"
        ? _L("已记录实际打印结果：成功。")
        : _L("已记录实际打印结果：有问题，建议保留模型用于复盘。"));
    load_library_entries();
}

void ModelGenerationPanel::delete_library_entry(const GeneratedModelEntry& entry)
{
    if (m_busy || m_finishing_running || !m_finishing_candidate.empty())
        return;
    // Saved beauty versions and drafts require their original GLB. Read fresh
    // metadata rather than relying on a possibly older asynchronous list.
    try {
        const auto records = library_metadata_path(entry.job_id).parent_path();
        for (boost::filesystem::directory_iterator it(records), end; it != end; ++it) {
            if ((it->path().extension() != ".json" && it->path().extension() != ".draft") ||
                it->path() == library_metadata_path(entry.job_id)) continue;
            boost::filesystem::ifstream input(it->path());
            if (!input || ModelLibraryMetadata::references_base(input, entry.model_path.filename().string())) {
                m_status->SetLabel(_L("无法删除：其他美颜版本或草稿仍需要这个原始模型，请先保留原件。"));
                return;
            }
        }
    } catch (...) {
        m_status->SetLabel(_L("无法确认美颜版本的原件依赖，已保留文件；请刷新历史后重试。"));
        return;
    }
    MessageDialog confirm(
        this,
        _L("要删除以下资产的本地文件、预览和元数据吗？\n\n") +
            (entry.search_text.empty() ? entry.title : entry.search_text) +
            _L("\n本地 ID：") + wxString::FromUTF8(entry.job_id) +
            _L("\n\n此操作不会取消远端任务，也无法撤销。"),
        _L("删除本地资产"), wxYES_NO | wxICON_WARNING);
    // MsgDialog uses custom buttons, so explicitly choose the non-destructive
    // keyboard default and escape result for this irreversible action.
    confirm.SetButtonLabel(wxID_YES, _L("Delete"));
    confirm.SetButtonLabel(wxID_NO, _L("Cancel"), true);
    confirm.SetAffirmativeId(wxID_NO);
    confirm.SetEscapeId(wxID_NO);
    confirm.Bind(wxEVT_CHAR_HOOK, [&confirm](wxKeyEvent& event) {
        if (event.GetKeyCode() == WXK_ESCAPE) {
            confirm.EndModal(wxID_NO);
            return;
        }
        event.Skip();
    });
    if (confirm.ShowModal() != wxID_YES)
        return;

    const boost::filesystem::path root = generated_models_root();
    const auto asset_path = entry.design_only ? entry.preview_path : entry.model_path;
    boost::system::error_code ec;
    if (!boost::filesystem::is_directory(root, ec) || !path_is_inside(root, asset_path)) {
        m_status->SetLabel(_L("删除已阻止：模型路径不在 generated_models 中。"));
        return;
    }
    if (entry.design_only && !read_design_history_entry(root, entry.job_id, false)) {
        m_status->SetLabel(_L("设计记录已变化，请刷新历史后重试。"));
        return;
    }

    bool displayed_model_deleted = false;
    ec.clear();
    if (!m_displayed_model_path.empty())
        displayed_model_deleted = boost::filesystem::equivalent(
            m_displayed_model_path, entry.model_path, ec) && !ec;

    const boost::filesystem::path model_parent = entry.design_only ? root / entry.job_id : asset_path.parent_path();
    const boost::filesystem::path downloads = root / "downloads";
    ec.clear();
    const bool downloaded_model = boost::filesystem::is_directory(downloads, ec) &&
        boost::filesystem::equivalent(model_parent, downloads, ec) && !ec;
    size_t removed_count = 0;
    const bool shared_directory = std::any_of(m_library_entries.begin(), m_library_entries.end(),
        [&](const GeneratedModelEntry& other) {
            return other.job_id != entry.job_id && (path_is_inside(model_parent, other.model_path) ||
                path_is_inside(model_parent, other.preview_path) || path_is_inside(model_parent, other.reference_image_path) ||
                path_is_inside(model_parent, other.ai_image_path) || path_is_inside(model_parent, other.color_intent_path));
        });
    if (!shared_directory && !downloaded_model && model_parent.filename().string() == entry.job_id &&
        path_is_inside(root, model_parent)) {
        ec.clear();
        removed_count = boost::filesystem::remove_all(model_parent, ec);
    }
    std::vector<boost::filesystem::path> targets {
        entry.model_path,
        entry.color_intent_path,
        library_metadata_path(entry.job_id),
        boost::filesystem::path(library_metadata_path(entry.job_id).string() + ".draft"),
        temp_path(entry.job_id, "png"),
        temp_path(entry.job_id + "-raw", "png"),
        temp_path(entry.job_id + "-strict", "png"),
        temp_path(entry.job_id + "-clean", "png"),
        temp_path(entry.job_id + "-heatmap", "png"),
        temp_path(entry.job_id + "-input", "png")
    };
    if (entry.design_only) {
        targets.push_back(model_parent / "job.json");
        for (const auto& path : {entry.preview_path, entry.ai_image_path, entry.reference_image_path})
            if (path_is_inside(model_parent, path)) targets.push_back(path);
    }
    for (const boost::filesystem::path& image_path : {entry.reference_image_path, entry.ai_image_path}) {
        if (is_archived_library_image(image_path, entry.job_id))
            targets.push_back(image_path);
    }
    for (const boost::filesystem::path& target : targets) {
        if (std::any_of(m_library_entries.begin(), m_library_entries.end(), [&](const GeneratedModelEntry& other) {
            if (other.job_id == entry.job_id) return false;
            for (const auto& shared : {other.model_path, other.preview_path, other.reference_image_path,
                                      other.ai_image_path, other.color_intent_path}) {
                boost::system::error_code shared_ec;
                if (!target.empty() && !shared.empty() &&
                    boost::filesystem::equivalent(target, shared, shared_ec) && !shared_ec) return true;
            }
            return false;
        })) continue;
        boost::system::error_code target_ec;
        if (!boost::filesystem::exists(target, target_ec))
            continue;
        if (!path_is_inside(root, target)) {
            ec = boost::system::errc::make_error_code(boost::system::errc::permission_denied);
            break;
        }
        if (boost::filesystem::remove(target, target_ec))
            ++removed_count;
        if (target_ec) {
            ec = target_ec;
            break;
        }
    }

    if (ec) {
        m_status->SetLabel(_L("本地模型删除不完整，请检查文件权限。"));
        load_library_entries();
        return;
    }
    if (displayed_model_deleted) {
        if (m_model_preview != nullptr)
            m_model_preview->clear();
        m_displayed_model_path.clear();
        m_displayed_model_job_id.clear();
        m_displayed_model_palette.clear();
        m_displayed_model_palette_roles.clear();
        m_artifact_path.clear();
        m_color_intent_path.clear();
        m_color_intent_schema.clear();
        m_color_intent_sha256.clear();
        m_reference_image_path.clear();
        m_raw_preview_path.clear();
        m_reference_image = wxImage();
        m_raw_preview_image = wxImage();
        m_clean_preview_image = wxImage();
        m_style_preview_image = wxImage();
        m_style_preview_ready = false;
        m_model_preview_ready = false;
        m_library_model_loaded = false;
        if (m_job_id.empty())
            m_ready = false;
        m_model_stats->SetLabel(_L("模型尚未加载"));
        m_model_preview_message->SetLabel(_L("已删除当前显示的本地模型。"));
    }
    if (entry.design_only) {
        if (m_job_id == entry.job_id) {
            m_selected_image_path.clear();
            reset(false);
        }
        // Forget the stopped/completed local job as well as its files, so it cannot
        // be offered for recovery until the sidecar is restarted.
        m_client.remove(entry.job_id, [] {}, [](std::string) {});
    }
    m_status->SetLabel(removed_count > 0
        ? _L("历史模型的本地文件已删除，无法撤销。")
        : _L("没有找到可删除的本地文件。"));
    load_library_entries();
    refresh_controls();
}

void ModelGenerationPanel::show_selected_image_preview(const wxImage* validated)
{
    if (m_selected_image_path.empty())
        return;
    wxImage image;
    if (validated) image = *validated;
    else load_model_input_image(m_selected_image_path, image);
    if (!image.IsOk())
        return;
    m_reference_image = image;
    // A different source can have the same dimensions as the cached bitmap.
    m_reference_bitmap = wxNullBitmap;
    m_reference_image_path = m_selected_image_path;
    m_raw_preview_path.clear();
    m_raw_preview_image = wxImage();
    m_model_reference_image = wxImage();
    m_strict_preview_image = wxImage();
    m_model_views_image = wxImage();
    m_clean_preview_image = wxImage();
    m_heatmap_image = wxImage();
    m_style_preview_image = wxImage();
    m_style_preview_bitmap = wxNullBitmap;
    m_preview_zoom_factor = 1.0;
    m_style_preview_placeholder = _L("等待生成 AI 图");
    if (m_preview_stage != nullptr)
        m_preview_stage->SetSelection(0);
    m_preview_kind->SetLabel(_L("结果对照"));
    m_preview_message->SetLabel(
        wxString::Format(_L("原图 %d × %d px  ·  等待生成 AI 图"), image.GetWidth(), image.GetHeight()));
    update_preview_view(true);
}

void ModelGenerationPanel::apply_preview_stage(bool center)
{
    const bool show_views = m_preview_stage != nullptr && m_preview_stage->GetSelection() == 1;
    const wxImage* selected = nullptr;
    if (show_views && m_model_views_image.IsOk()) selected = &m_model_views_image;
    else if (!show_views && m_model_reference_image.IsOk()) selected = &m_model_reference_image;
    else if (!show_views && m_clean_preview_image.IsOk()) selected = &m_clean_preview_image;
    else if (!show_views && m_raw_preview_image.IsOk()) selected = &m_raw_preview_image;
    const bool has_display = !show_views && m_history_display_image.IsOk() &&
        m_history_display_source == m_raw_preview_path;
    if (has_display) selected = &m_history_display_image;
    m_style_preview_image = selected == nullptr ? wxImage() : selected->Copy();
    m_style_preview_bitmap = wxNullBitmap;
    if (selected != nullptr) m_style_preview_placeholder.clear();
    else if (show_views) m_style_preview_placeholder = _L("正在加载模型多视图...");
    else if (m_raw_preview_available) m_style_preview_placeholder = _L("正在加载 AI 设计图...");
    if (m_preview_stage != nullptr) m_preview_stage->Enable(m_model_views_available);
    refresh_preview_stage_hint(m_design_preview_stale);
    update_preview_view(center);
    if (m_preview_area != nullptr) m_preview_area->Update();
}

void ModelGenerationPanel::refresh_preview_stage_hint(bool stale_job)
{
    const bool has_design = m_style_preview_image.IsOk() && !m_job_id.empty();
    const bool stale_design = has_design && stale_job;
    const bool restored = m_design_preview_stale && !stale_design;
    const bool version_changed = m_design_preview_stale != stale_design;
    m_design_preview_stale = stale_design;
    const bool show_views = m_preview_stage != nullptr && m_preview_stage->GetSelection() == 1;
    if (m_preview_stage_hint != nullptr) {
        wxString hint = !has_design
            ? _L("生成后可在这里确认图片效果。")
            : m_design_preview_stale
            ? _L("描述、风格或颜色已变化。右侧是旧版设计图；恢复原输入可继续使用，或重新生成。")
            : show_views
            ? _L("使用“展开图片”放大多视图，检查各个角度的形体与结构。")
            : m_ready ? _L("设计图已保留。打开 3D 工作台查看、检查或美颜；加入工程需另行操作。")
                      : _L("确认主体、姿态与细节。生成 3D 时使用已确认的 AI 设计图。");
        if (!show_views && m_job_image_path.empty() && !m_job_prompt.empty()) {
            // Show the submitted source, never the possibly edited next input.
            wxString summary = m_job_prompt;
            summary.Replace("\r", " ");
            summary.Replace("\n", " ");
            if (summary.length() > 80) summary = summary.Left(80) + wxString::FromUTF8("…");
            hint = _L("文字来源：") + summary + "\n" + hint;
            m_preview_stage_hint->SetToolTip(m_job_prompt);
        } else {
            m_preview_stage_hint->UnsetToolTip();
        }
        m_preview_stage_hint->SetLabel(hint);
        m_preview_stage_hint->Wrap(FromDIP(760));
    }
    // Undoing input changes reuses the same accepted design. Restore only the
    // stale-input projection, without hiding a newer submission error or loading state.
    if (restored && has_design && !m_busy && !m_preview_download_in_flight &&
        m_awaiting_confirmation && !m_awaiting_palette_confirmation &&
        m_submission_state.error({m_job_id, current_generation_options().provider, m_sequence}).empty()) {
        m_status->SetLabel(_L("原输入已恢复，可继续使用这张 AI 设计图。"));
        m_result_summary->SetLabel(_L("确认图片效果后可继续生成 3D；无需重新生成 2D。"));
        update_progress(35, 2, _L("确认AI 设计图"));
    }
    if (version_changed && m_preview_area != nullptr) m_preview_area->Refresh();
}

void ModelGenerationPanel::update_preview_view(bool center)
{
    if (m_preview_area == nullptr || m_updating_preview)
        return;
    m_updating_preview = true;
    refresh_input_image_thumbnail();
    if (!m_reference_image.IsOk() && !m_style_preview_image.IsOk() && !m_model_preview_ready) {
        m_reference_bitmap = wxNullBitmap;
        m_style_preview_bitmap = wxNullBitmap;
        m_reference_preview_pane = wxRect();
        m_style_preview_pane = wxRect();
        m_preview_area->SetVirtualSize(m_preview_area->GetClientSize());
        m_preview_area->Refresh();
        m_updating_preview = false;
        return;
    }

    const wxSize client = m_preview_area->GetClientSize();
    const int padding = FromDIP(16);
    const int gap = FromDIP(16);
    const int label_height = FromDIP(32);
    const wxImage* comparison_image = m_reference_image.IsOk() ? &m_reference_image : nullptr;
    const bool show_views = m_preview_stage != nullptr && m_preview_stage->GetSelection() == 1;
    const bool comparison = !show_views && m_reference_image.IsOk() && m_style_preview_image.IsOk();
    const int base_pane_width = comparison
        ? std::max(1, (client.GetWidth() - 2 * padding - gap) / 2)
        : std::max(1, client.GetWidth() - 2 * padding);
    const int base_image_height = std::max(1, client.GetHeight() - 2 * padding - label_height);

    auto update_bitmap = [&](const wxImage& image, wxBitmap& bitmap) {
        if (!image.IsOk()) {
            bitmap = wxNullBitmap;
            return;
        }
        const double fit_scale = std::min({ 1.0,
            double(base_pane_width) / image.GetWidth(),
            double(base_image_height) / image.GetHeight() });
        double scale = fit_scale * m_preview_zoom_factor;
        scale = std::min(scale, double(MAX_PREVIEW_BITMAP_DIMENSION) / image.GetWidth());
        scale = std::min(scale, double(MAX_PREVIEW_BITMAP_DIMENSION) / image.GetHeight());
        const int width = std::max(1, int(std::lround(image.GetWidth() * scale)));
        const int height = std::max(1, int(std::lround(image.GetHeight() * scale)));
        if (!bitmap.IsOk() || bitmap.GetWidth() != width || bitmap.GetHeight() != height)
            bitmap = wxBitmap(image.Scale(width, height, wxIMAGE_QUALITY_HIGH));
    };

    if (comparison_image != nullptr)
        update_bitmap(*comparison_image, m_reference_bitmap);
    else
        m_reference_bitmap = wxNullBitmap;
    update_bitmap(m_style_preview_image, m_style_preview_bitmap);

    if (comparison) {
        const int reference_width = std::max(base_pane_width,
            m_reference_bitmap.IsOk() ? m_reference_bitmap.GetWidth() : 0);
        const int result_width = std::max(base_pane_width,
            m_style_preview_bitmap.IsOk() ? m_style_preview_bitmap.GetWidth() : 0);
        const int image_height = std::max({ base_image_height,
            m_reference_bitmap.IsOk() ? m_reference_bitmap.GetHeight() : 0,
            m_style_preview_bitmap.IsOk() ? m_style_preview_bitmap.GetHeight() : 0 });
        const int pane_height = label_height + image_height;
        m_reference_preview_pane = wxRect(padding, padding, reference_width, pane_height);
        m_style_preview_pane = wxRect(padding + reference_width + gap, padding, result_width, pane_height);
    } else {
        const wxBitmap& bitmap = m_style_preview_bitmap.IsOk() ? m_style_preview_bitmap : m_reference_bitmap;
        const int pane_width = std::max(base_pane_width, bitmap.IsOk() ? bitmap.GetWidth() : 0);
        const int image_height = std::max(base_image_height, bitmap.IsOk() ? bitmap.GetHeight() : 0);
        if (!show_views && m_reference_image.IsOk()) {
            m_reference_preview_pane = wxRect(padding, padding, pane_width, label_height + image_height);
            m_style_preview_pane = wxRect();
        } else {
            m_reference_preview_pane = wxRect();
            m_style_preview_pane = wxRect(padding, padding, pane_width, label_height + image_height);
        }
    }

    const wxRect content = m_style_preview_pane.IsEmpty() ? m_reference_preview_pane : m_style_preview_pane;
    const int virtual_width = std::max(client.GetWidth(), content.GetRight() + padding + 1);
    const int virtual_height = std::max(client.GetHeight(), content.GetBottom() + padding + 1);
    m_preview_area->SetVirtualSize(virtual_width, virtual_height);
    if (center) {
        int pixels_per_unit_x = 1;
        int pixels_per_unit_y = 1;
        m_preview_area->GetScrollPixelsPerUnit(&pixels_per_unit_x, &pixels_per_unit_y);
        const int scroll_x = std::max(0, virtual_width - client.GetWidth()) / std::max(1, 2 * pixels_per_unit_x);
        const int scroll_y = std::max(0, virtual_height - client.GetHeight()) / std::max(1, 2 * pixels_per_unit_y);
        m_preview_area->Scroll(scroll_x, scroll_y);
    }
    m_preview_zoom->SetLabel(wxString::Format("%d%%", int(std::lround(m_preview_zoom_factor * 100.0))));
    m_preview_area->Refresh();
    m_updating_preview = false;
}

void ModelGenerationPanel::set_preview_zoom(double zoom)
{
    if (!m_reference_image.IsOk() && !m_style_preview_image.IsOk())
        return;
    m_preview_zoom_factor = std::clamp(zoom, MIN_PREVIEW_ZOOM, MAX_PREVIEW_ZOOM);
    update_preview_view(true);
    refresh_controls();
}

void ModelGenerationPanel::update_progress(int value, int step, const wxString& phase)
{
    m_generation_progress->Show();
    m_progress_percent->Show();
    value = std::clamp(value, 0, 100);
    step = std::clamp(step, 1, 4);
    m_generation_progress->SetValue(value);
    m_workflow_phase->SetLabel(phase);
    m_workflow_phase->SetToolTip(wxString::Format(_L("第 %d 步，共 4 步"), step));
    m_progress_percent->SetLabel(wxString::Format("%d%%", value));
    for (size_t index = 0; index < m_step_labels.size(); ++index) {
        if (m_step_labels[index] == nullptr)
            continue;
        const int label_step = int(index) + 1;
        const bool active = label_step == step;
        const bool complete = label_step < step;
        m_step_labels[index]->SetName(active || complete ? "input_accent" : "input_secondary");
        ModelGenerationInputStyle::apply_control(m_step_labels[index], active || complete ?
            ModelGenerationInputStyle::Role::Accent : ModelGenerationInputStyle::Role::Secondary);
        wxFont font = m_step_labels[index]->GetFont();
        font.SetWeight(active ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL);
        if (m_step_labels[index]->GetFont() != font) m_step_labels[index]->SetFont(font);
    }
}

void ModelGenerationPanel::update_workflow(const AIModelGenerationClient::JobStatus* status)
{
    const bool image_mode = m_job_preview_expected ||
                            (m_job_id.empty() && (!m_prompt->GetValue().empty() || has_image_input()));
    wxString phase = _L("输入");
    wxString guidance = _L("输入文字、图片，或同时使用两者");
    int step = 1;
    int progress = 0;
    if (status != nullptr) {
        progress = display_progress(*status);
        if (status->state == "recommending_palette") {
            phase = _L("推荐打印配色");
            guidance = _L("AI 正在分析主体、风格和适合打印的大色区");
            step = 1;
        } else if (status->state == "awaiting_palette_confirmation") {
            phase = _L("确认目标配色");
            guidance = _L("修改或确认设计目标色，再生成图片预览");
            step = 1;
        } else if (status->state == "preprocessing") {
            phase = image_mode ? _L("生成AI 设计图") : _L("准备提示词");
            guidance = image_mode ? _L("AI 正在生成高质量设计图") : _L("AI 正在整理 3D 提示词");
            step = 2;
        } else if (status->phase == "preparing_multiview") {
            phase = _L("准备写实四视图");
            guidance = _L("正在核对人脸、姿态和材料边界；通过后才会提交付费任务");
            step = 3;
        } else if (status->state == "awaiting_confirmation" && status->phase == "multiview_retry") {
            phase = _L("四视图需重试");
            guidance = _L("当前预览已保留，可直接重试；尚未创建付费 3D 生成任务");
            step = 3;
        } else if (status->state == "awaiting_confirmation") {
            phase = image_mode ? _L("确认AI 设计图") : _L("确认提示词");
            guidance = image_mode ? _L("分别确认形体依据与材质分区，并选择 3D 模型精度") : _L("确认提示词并选择 3D 模型精度");
            step = 2;
        } else if (status->phase == "generating") {
            phase = _L("生成模型");
            guidance = _L("正在生成 3D 模型，可在这里查看进度");
            step = 3;
        } else if (status->phase == "texturing") {
            phase = _L("保留造型并上色");
            guidance = _L("正在保留历史模型的脸和姿态，并应用当前确认颜色");
            step = 3;
        } else if (status->phase == "converting" || status->phase == "downloading_artifact") {
            phase = _L("优化模型");
            guidance = _L("正在优化并下载 3D 模型");
            step = 3;
        } else if (status->phase == "checking_model") {
            phase = _L("修复材料并检查结构");
            guidance = _L("正在清理串色和杂色，并检查拓扑、底座与薄壁");
            step = 4;
        } else if (status->phase == "checking_visual") {
            phase = _L("对照原图检查外观");
            guidance = _L("正在核对人脸相似度、主体完整性和材料归属");
            step = 4;
        } else if (status->state == "ready") {
            phase = _L("查看模型");
            guidance = _L("模型生成完成；打开 3D 工作台查看与检查");
            step = 4;
        } else if (status->state == "stopping") {
            phase = _L("停止生成");
            guidance = _L("正在安全停止当前生成任务");
            step = 3;
        } else if (status->state == "failed") {
            phase = status->progress >= 10 ? _L("生成未完成") : _L("预览未完成");
            guidance = _L("请查看失败原因后重试；已确认的图片不会自动丢失");
            step = status->progress >= 10 ? 3 : 2;
        }
    }
    m_workflow_phase->SetLabel(phase);
    m_workflow_steps->SetLabel(guidance);
    update_progress(progress, step, phase);
}

void ModelGenerationPanel::set_preview_empty(const wxString& message)
{
    m_reference_image_path.clear();
    m_raw_preview_path.clear();
    m_reference_image = wxImage();
    m_raw_preview_image = wxImage();
    m_model_reference_image = wxImage();
    m_strict_preview_image = wxImage();
    m_model_views_image = wxImage();
    m_clean_preview_image = wxImage();
    m_heatmap_image = wxImage();
    m_style_preview_image = wxImage();
    m_reference_bitmap = wxNullBitmap;
    m_style_preview_bitmap = wxNullBitmap;
    m_reference_preview_pane = wxRect();
    m_style_preview_pane = wxRect();
    m_style_preview_placeholder.clear();
    if (m_preview_stage != nullptr)
        m_preview_stage->SetSelection(0);
    m_preview_metrics_available = false;
    m_preview_changed_pixel_ratio = 0.0;
    m_preview_minimum_feature_px = 0;
    if (m_preview_stage_hint != nullptr) {
        m_preview_stage_hint->SetLabel(_L("生成后可在这里确认图片效果。"));
        m_preview_stage_hint->UnsetToolTip();
    }
    if (m_preview_technical_details != nullptr)
        m_preview_technical_details->SetLabel(
            _L("完成预览后会显示颜色映射和小色块处理数据。"));
    m_preview_zoom_factor = 1.0;
    if (m_preview_kind != nullptr)
        m_preview_kind->SetLabel(_L("暂无预览"));
    if (m_preview_zoom != nullptr)
        m_preview_zoom->SetLabel("100%");
    m_preview_message->SetLabel(message);
    update_preview_view();
}

} // namespace Slic3r::GUI
