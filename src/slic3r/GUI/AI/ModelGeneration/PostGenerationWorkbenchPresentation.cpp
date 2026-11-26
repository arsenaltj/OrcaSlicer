#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "ModelGenerationPresentation.hpp"
#include "ModelPreview3D.hpp"
#include "BeautyWorkbenchControls.hpp"
#include "BeautyWorkbenchTransactionController.hpp"
#include "WorkbenchImportColorDialog.hpp"
#include "WorkbenchStyle.hpp"
#include "WorkbenchTopologyDetails.hpp"
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
#include "libslic3r/TextureToColor/ColorUtils.hpp"
#include "slic3r/GUI/I18N.hpp"
#include <array>
#include <limits>
#include <set>
#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/display.h>
#include <wx/filedlg.h>
#include <wx/notebook.h>
#include <wx/numformatter.h>
#include <wx/msgdlg.h>
#include <wx/menu.h>
#include <wx/textctrl.h>
#include <wx/tglbtn.h>
#include <wx/toplevel.h>
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

class WorkbenchFloatingCardEventFilter final : public wxEventFilter {
public:
    explicit WorkbenchFloatingCardEventFilter(ModelGenerationPanel* panel) : m_panel(panel) {}

    int FilterEvent(wxEvent& event) override
    {
        if (!m_panel) return wxEventFilter::Event_Skip;
        auto* window = dynamic_cast<wxTopLevelWindow*>(event.GetEventObject());
        if (!window) return wxEventFilter::Event_Skip;
        if (event.GetEventType() == wxEVT_SHOW) {
            if (static_cast<wxShowEvent&>(event).IsShown()) m_panel->raise_workbench_modal();
        } else if (event.GetEventType() == wxEVT_ACTIVATE) {
            if (static_cast<wxActivateEvent&>(event).GetActive()) m_panel->raise_workbench_modal();
        }
        return wxEventFilter::Event_Skip;
    }

private:
    wxWeakRef<ModelGenerationPanel> m_panel;
};

wxColour workbench_status_colour(PostGenerationStatusTone tone)
{
    switch (tone) {
    case PostGenerationStatusTone::Neutral: return wxColour(190, 190, 196);
    case PostGenerationStatusTone::Accent: return wxColour(255, 194, 39);
    case PostGenerationStatusTone::Success: return wxColour(91, 205, 137);
    case PostGenerationStatusTone::Information: return wxColour(97, 174, 255);
    case PostGenerationStatusTone::Error: return wxColour(255, 128, 112);
    }
    return wxColour(190, 190, 196);
}

bool set_workbench_label(wxWindow* control, const wxString& text)
{
    if (control->GetLabel() == text) return false;
    control->SetLabel(text);
    return true;
}

void set_workbench_tooltip(wxWindow* control, const wxString& text)
{
    if (control->GetToolTipText() != text) control->SetToolTip(text);
}

bool set_workbench_enabled(wxWindow* control, bool enabled)
{
    if (control->IsEnabled() == enabled) return false;
    control->Enable(enabled);
    return true;
}

bool set_workbench_shown(wxWindow* control, bool shown)
{
    if (control->IsShown() == shown) return false;
    control->Show(shown);
    return true;
}

// Native wrapping may keep an entire CJK sentence as one word. Measure the
// displayed text so a narrow tool panel never truncates its instructions.
bool set_wrapped_workbench_text(wxStaticText* label, const wxString& text, int width,
                                bool preserve_lines = false)
{
    wxString source = text, line, result;
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
    bool changed = set_workbench_label(label, wrapped);
    wxClientDC dc(label);
    dc.SetFont(label->GetFont());
    const wxSize minimum(1, dc.GetMultiLineTextExtent(wrapped).y + label->FromDIP(2));
    if (label->GetMinSize() != minimum) {
        label->SetMinSize(minimum);
        changed = true;
    }
    return changed;
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

void invalidate_workbench_controls(wxWindow* window, wxWindow* viewport)
{
    if (!window || window == viewport ||
        !window->IsShownOnScreen() || window->IsFrozen()) return;
    window->Refresh(false);
    for (wxWindow* child : window->GetChildren())
        invalidate_workbench_controls(child, viewport);
}

void present_workbench_controls(wxWindow* root, wxWindow* viewport = nullptr)
{
    if (!root || !root->IsShownOnScreen() || root->IsFrozen()) return;
    invalidate_workbench_controls(root, viewport);
#ifdef __WXMSW__
    // Flush the native subtree once, rather than synchronously committing each
    // composited child separately. The GL canvas is submitted by its owner
    // after overlays and controls, using the same final viewport dimensions.
    if (viewport) ::ValidateRect(viewport->GetHandle(), nullptr);
    ::RedrawWindow(root->GetHandle(), nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN);
#else
    root->Update();
#endif
}
}

void ModelGenerationPanel::begin_workbench_check_timing()
{
    if (!m_workbench_check_timing && std::getenv("ORCASLICER_UI_LATENCY_TRACE")) {
        m_workbench_check_timing = std::make_shared<DiagnosticStageTimer>(
            "ORCASLICER_UI_LATENCY_TRACE", "WorkbenchCheckUsable", ModelGenerationPresentation::new_request_id());
        m_workbench_check_timing_armed = false;
        (*m_workbench_check_timing)("accepted");
    }
}

void ModelGenerationPanel::observe_workbench_check_usable()
{
    if (!m_workbench_check_timing || !m_model_preview) return;
    const bool processing = m_native_workbench_partition_running || m_workbench_check_running ||
        (m_workbench_auto_repair && m_finishing_running) || m_workbench_evidence_running;
    const auto outcome = workbench_check_outcome(m_workbench_check_result, processing);
    if (outcome == WorkbenchCheckOutcome::Failed) {
        finish_workbench_check_timing("failed-or-canceled");
        return;
    }
    if (m_workbench_check_timing_armed) return;
    if (outcome != WorkbenchCheckOutcome::Passed || !post_generation_ui_state().can_edit ||
        !m_workbench_presented || m_workbench_preparing || m_preview_loading ||
        !m_model_preview_ready || m_model_preview->workbench_input_locked()) return;
    m_workbench_check_timing_armed = true;
    const auto timing = m_workbench_check_timing;
    const auto revision = m_sequence;
    const auto model_hash = m_model_preview->model_sha256();
    (*timing)("controls-ready");
    wxWeakRef<ModelGenerationPanel> weak(this);
    m_model_preview->observe_diagnostic_frame([weak, timing, revision, model_hash] {
        if (!weak || weak->m_shutdown || weak->m_workbench_check_timing != timing) return;
        weak->m_workbench_check_timing_armed = false;
        // This callback is already bound to a complete GPU frame, viewport and DPI.
        // Recheck the business permission without granting or changing it.
        if (revision != weak->m_sequence || model_hash != weak->m_model_preview->model_sha256() ||
            !weak->m_workbench_presented || weak->m_workbench_preparing ||
            weak->m_model_preview->workbench_input_locked() || !weak->post_generation_ui_state().can_edit) return;
        weak->finish_workbench_check_timing("usable-first-frame");
    });
}

void ModelGenerationPanel::finish_workbench_check_timing(const char* stage)
{
    if (m_workbench_check_timing) (*m_workbench_check_timing)(stage);
    m_workbench_check_timing.reset();
    m_workbench_check_timing_armed = false;
    if (m_model_preview) m_model_preview->observe_diagnostic_frame({});
}

void ModelGenerationPanel::queue_workbench_surface_refresh(bool barrier, Presentation::Dirty flags)
{
    Presentation::assert_gui_thread();
    if (m_shutdown) return;
    if (Presentation::has(flags, Presentation::Dirty::Layout) ||
        Presentation::has(flags, Presentation::Dirty::Viewport) || Presentation::has(flags, Presentation::Dirty::Visibility))
        ++m_workbench_surface_revision;
    m_workbench_surface_refresh.configure([this] {
        if (m_shutdown || (!m_workbench_presented && !m_workbench_preparing) ||
            !m_finishing_workbench || !m_workbench_shell || !m_workbench_shell->IsShownOnScreen()) return false;
        const auto* owner = wxDynamicCast(wxGetTopLevelParent(m_workbench_shell), wxTopLevelWindow);
        return !owner || !owner->IsIconized();
    }, [this](Presentation::Dirty flags) { commit_workbench_surface(flags); }, "workbench");
    using Presentation::Dirty;
    if (barrier || m_workbench_preparing) flags = flags | Dirty::Barrier;
    m_workbench_surface_refresh.request(flags);
}

void ModelGenerationPanel::commit_workbench_surface(Presentation::Dirty flags)
{
    wxWeakRef<ModelGenerationPanel> weak(this);
    const bool barrier = Presentation::has(flags, Presentation::Dirty::Barrier);
    const auto started = std::chrono::steady_clock::now();
    // Paint only after all synchronous layout/visibility changes have
    // settled. Hidden editor children must not paint an intermediate frame.
    if (Presentation::has(flags, Presentation::Dirty::Content)) weak->apply_workbench_snapshot();
    else if (!weak->m_workbench_preparing) weak->update_workbench_check_card();
    if (Presentation::has(flags, Presentation::Dirty::Layout) || Presentation::has(flags, Presentation::Dirty::Viewport))
        weak->layout_workbench_history_toggle(true);
    auto* viewport = weak->workbench_import_viewport();
    // Finish the exposed native backgrounds before their overlapping leaf.
    // No other presentation layer may flush this subtree independently.
    if (barrier) present_workbench_controls(weak->m_workbench_shell, viewport);
    else weak->m_workbench_shell->Refresh(false);
    if (barrier) present_workbench_controls(weak->m_workbench_check_card);
    else if (weak->m_workbench_check_card && weak->m_workbench_check_card->IsShownOnScreen())
        weak->m_workbench_check_card->Refresh(false);
    const auto controls_presented = std::chrono::steady_clock::now();
    if (weak->m_workbench_preparing && weak->m_model_preview) {
        const auto revision = weak->m_workbench_surface_revision;
        const auto model_path = weak->m_displayed_model_path;
        // Installing the observer invalidates the frame once. Sample afterwards
        // so the stamp describes the frame we are actually waiting for.
        auto stamp = std::make_shared<Presentation::PresentationStamp>();
        weak->m_model_preview->observe_presentation_frame([weak, revision, model_path, stamp] {
            if (!weak || weak->m_shutdown || !weak->m_workbench_preparing) return;
            if (revision != weak->m_workbench_surface_revision ||
                model_path != weak->m_displayed_model_path ||
                !(*stamp == weak->m_model_preview->presentation_stamp(
                    weak->m_workbench_presentation_sequence, weak->m_workbench_surface_revision))) {
                weak->queue_workbench_surface_refresh();
                return;
            }
            if (!weak->m_model_preview_ready || weak->m_preview_loading ||
                !weak->m_model_preview->showing_model(model_path)) return;
            auto ready = weak->m_workbench_preparation_ready;
            if (ready) ready();
        });
        *stamp = weak->m_model_preview->presentation_stamp(
            weak->m_workbench_presentation_sequence, revision);
    }
    observe_workbench_check_usable();
    // The floating leaf overlaps the native preview subtree. Its repaint must
    // follow both the final GL swap and XYZ relocation, never precede them.
    auto* handle = weak->m_workbench_history_toggle;
    if (handle && handle->IsShownOnScreen() && weak->m_model_preview) {
        const auto stamp = weak->m_model_preview->presentation_stamp(
            weak->m_workbench_presentation_sequence, weak->m_workbench_surface_revision);
        wxWeakRef<wxWindow> parent(weak->m_workbench_shell->GetParent());
        wxWeakRef<wxWindow> leaf(handle);
        const auto bounds = handle->GetRect();
        weak->m_model_preview->observe_overlay_frame([weak, parent, leaf, stamp, bounds] {
            if (!weak || weak->m_shutdown || !parent || !leaf ||
                (!weak->m_workbench_presented && !weak->m_workbench_preparing) ||
                weak->m_workbench_shell->GetParent() != parent.get() ||
                leaf->GetParent() != weak->m_workbench_shell || !leaf->IsShownOnScreen() ||
                leaf->GetRect() != bounds || !(stamp == weak->m_model_preview->presentation_stamp(
                    weak->m_workbench_presentation_sequence, weak->m_workbench_surface_revision))) return;
            leaf->Raise();
            leaf->Refresh(false);
        });
    }
    // One complete GL frame follows the native controls in this same pass.
    // A top-level restore can then expose the window without an old frame.
    if (viewport && viewport->IsShownOnScreen()) {
        viewport->Refresh(false);
        if (barrier) viewport->Update();
    }
    if (barrier && handle && handle->IsShownOnScreen()) {
        handle->Raise();
        handle->Refresh(false);
        handle->Update();
    }
    if (barrier && weak->m_model_preview) weak->m_model_preview->present_workbench_palette_details();
    wxString trace;
    if (wxGetEnv("ORCASLICER_UI_LATENCY_TRACE", &trace) && trace == "1")
        BOOST_LOG_TRIVIAL(info) << "Workbench surface presented: controls_ms="
            << std::chrono::duration<double, std::milli>(controls_presented - started).count()
            << ", total_ms="
            << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count()
            << ", history_open=" << weak->m_workbench_history_state.workbench_open;
}

bool ModelGenerationPanel::position_workbench_check_card(bool preparing)
{
    if (!m_workbench_check_card || !m_model_preview ||
        (!preparing && !m_workbench_check_card->IsShown())) return false;
    auto* canvas = m_model_preview->workbench_viewport_window();
    if (!canvas || !canvas->IsShownOnScreen()) return false;
    const wxSize viewport_size = canvas->GetClientSize();
    if (viewport_size.x <= 0 || viewport_size.y <= 0) return false;
    const wxRect viewport(canvas->ClientToScreen(wxPoint()), viewport_size);
    const wxSize card_size = m_workbench_check_card->GetSize();
    const wxPoint preferred(
        viewport.x + std::max(0, (viewport.width - card_size.x) / 2),
        viewport.y + std::max(0, (viewport.height - card_size.y) / 2));
    const wxRect owner = wxGetTopLevelParent(canvas)->GetScreenRect();
    const wxPoint position(
        std::clamp(preferred.x, owner.x, owner.x + std::max(0, owner.width - card_size.x)),
        std::clamp(preferred.y, owner.y, owner.y + std::max(0, owner.height - card_size.y)));
    if (m_workbench_check_card->GetPosition() != position) {
        const bool exposed = m_workbench_check_card->IsShown();
        m_workbench_check_card->Move(position);
        if (exposed) queue_workbench_surface_refresh();
    }
    raise_workbench_check_card();
    return true;
}

void ModelGenerationPanel::raise_workbench_check_card()
{
    raise_workbench_floating_card(m_workbench_check_card);
}

void ModelGenerationPanel::install_workbench_card_layer_guard()
{
    if (m_workbench_card_event_filter) return;
    m_workbench_card_event_filter = new WorkbenchFloatingCardEventFilter(this);
    wxEvtHandler::AddFilter(m_workbench_card_event_filter);
}

void ModelGenerationPanel::remove_workbench_card_layer_guard()
{
    m_workbench_card_raise_pending = false;
    if (!m_workbench_card_event_filter) return;
    wxEvtHandler::RemoveFilter(m_workbench_card_event_filter);
    delete m_workbench_card_event_filter;
    m_workbench_card_event_filter = nullptr;
}

void ModelGenerationPanel::raise_workbench_modal()
{
    if (m_shutdown || !m_workbench_presented || !m_workbench_check_card ||
        !m_workbench_check_card->IsShown() || m_workbench_card_raise_pending) return;
    if (!wxGetApp().IsActive()) return;
    m_workbench_card_raise_pending = true;
    wxWeakRef<ModelGenerationPanel> weak(this);
    wxGetApp().CallAfter([weak] {
        if (!weak) return;
        weak->m_workbench_card_raise_pending = false;
        if (weak->m_shutdown || !weak->m_workbench_presented || !weak->m_workbench_check_card ||
            !weak->m_workbench_check_card->IsShown() || !wxGetApp().IsActive()) return;
        weak->position_workbench_check_card();
    });
}

void ModelGenerationPanel::detach_workbench_floating_card()
{
    m_native_workbench_partition_retry.reset();
    finish_workbench_check_timing("presentation-left");
    cancel_workbench_presentation();
    m_workbench_presented = false;
    ++m_workbench_presentation_sequence;
    if (m_workbench_check_running && m_workbench_check_cancel)
        m_workbench_check_cancel->store(true);
    remove_workbench_card_layer_guard();
    if (m_model_preview) m_model_preview->set_honor_suite_semantic_progress_callback({});
    m_workbench_honor_semantic_active = false;
    if (m_workbench_card_overlay_host) {
        m_workbench_card_overlay_host->Unbind(wxEVT_SIZE,
            &ModelGenerationPanel::on_workbench_card_overlay_size, this);
        m_workbench_card_overlay_host = nullptr;
    }
    if (m_workbench_card_owner) {
        m_workbench_card_owner->Unbind(wxEVT_MOVE,
            &ModelGenerationPanel::on_workbench_card_owner_move, this);
        m_workbench_card_owner->Unbind(wxEVT_ACTIVATE,
            &ModelGenerationPanel::on_workbench_card_owner_activate, this);
        m_workbench_card_owner = nullptr;
    }
    hide_workbench_check_card();
}

void ModelGenerationPanel::on_workbench_card_owner_move(wxMoveEvent& event)
{
    position_workbench_check_card();
    event.Skip();
}

void ModelGenerationPanel::on_workbench_card_owner_activate(wxActivateEvent& event)
{
    if (event.GetActive()) raise_workbench_modal();
    event.Skip();
}

void ModelGenerationPanel::on_workbench_card_overlay_size(wxSizeEvent& event)
{
    queue_workbench_surface_refresh();
    event.Skip();
}

void ModelGenerationPanel::hide_workbench_check_card()
{
    m_workbench_check_card_visible = false;
    if (m_workbench_honor_semantic_active && m_finishing_workbench && m_workbench_presented &&
        m_workbench_shell && m_workbench_shell->IsShownOnScreen()) {
        update_workbench_check_card();
        return;
    }
    remove_workbench_card_layer_guard();
    m_workbench_floating_card_mode = WorkbenchFloatingCardMode::None;
    if (m_workbench_check_progress) m_workbench_check_progress->set_running(false);
    if (m_workbench_check_progress) m_workbench_check_progress->set_progress(std::nullopt);
    if (m_model_preview) m_model_preview->set_workbench_input_locked(false);
    if (m_workbench_shell && !m_workbench_shell->IsEnabled()) m_workbench_shell->Enable();
    if (m_workbench_check_card && m_workbench_check_card->IsShown()) {
        m_workbench_check_card->Hide();
        queue_workbench_surface_refresh();
    }
}

void ModelGenerationPanel::cancel_workbench_check()
{
    if (m_workbench_auto_repair && m_finishing_running && m_finishing_canceled) {
        m_finishing_canceled->store(true);
        if (m_workbench_check_message)
            m_workbench_check_message->SetLabel(_L("正在取消保存，当前模型保持不变……"));
        if (m_workbench_check_cancel_button) m_workbench_check_cancel_button->Disable();
        return;
    }
    if (m_native_workbench_partition_running && m_native_workbench_partition_cancel) {
        m_native_workbench_partition_cancel->store(true);
        if (m_model_preview) m_model_preview->fail_model_installation_frame();
        if (m_workbench_check_message)
            m_workbench_check_message->SetLabel(_L("正在取消严格修复，当前模型保持不变……"));
        if (m_workbench_check_cancel_button) m_workbench_check_cancel_button->Disable();
        return;
    }
    if (!m_workbench_check_running || !m_workbench_check_cancel) return;
    m_workbench_check_cancel->store(true);
    if (m_workbench_check_message)
        m_workbench_check_message->SetLabel(_L("正在取消检查，当前模型保持不变……"));
    if (m_workbench_check_cancel_button) m_workbench_check_cancel_button->Disable();
}

const char* workbench_import_color_name(uint32_t rgb)
{
    constexpr struct {
        uint32_t rgb;
        const char* label;
    } names[] {
        {0x101010, L("黑色")}, {0x282828, L("深灰色")}, {0x777777, L("灰色")},
        {0xB8B8B8, L("浅灰色")}, {0xDEDEDE, L("灰白色")}, {0xFAFAFA, L("白色")},
        {0x8D2029, L("暗红色")}, {0xD52833, L("红色")}, {0xED7770, L("珊瑚色")},
        {0xDE6397, L("粉色")}, {0xF3B9C8, L("浅粉色")}, {0xBA9D9E, L("灰粉色")},
        {0xDFC9CA, L("浅灰粉色")}, {0xB7776D, L("砖红色")}, {0xC36535, L("砖橙色")}, {0xF58723, L("橙色")},
        {0xF5AD7C, L("蜜桃色")}, {0xE9AD21, L("金黄色")}, {0xF2DE3D, L("黄色")},
        {0xFAE78B, L("浅黄色")}, {0xF1E5C8, L("米白色")}, {0xD6C1A1, L("米色")},
        {0xAC917F, L("灰棕色")}, {0xA77B52, L("浅棕色")}, {0x844D31, L("棕色")},
        {0x4F3327, L("深棕色")}, {0xAAB544, L("黄绿色")}, {0x7D813E, L("橄榄绿")},
        {0x32934A, L("绿色")}, {0x8ABB84, L("浅绿色")}, {0x3D6847, L("深绿色")},
        {0x829784, L("灰绿色")}, {0x219990, L("青色")}, {0x9ED4C7, L("薄荷绿")},
        {0x4ABACB, L("湖蓝色")}, {0x8CC8EA, L("浅蓝色")}, {0x2868C4, L("蓝色")},
        {0x253D70, L("深蓝色")}, {0x798DAA, L("灰蓝色")}, {0x7D4DB0, L("紫色")},
        {0xC0A9DF, L("浅紫色")}, {0x925F88, L("灰紫色")}, {0x553661, L("深紫色")}
    };
    const auto srgb = [](uint32_t color) -> tex2color::color_utils::ColorDouble {
        return {double((color >> 16) & 255) / 255.0, double((color >> 8) & 255) / 255.0,
                double(color & 255) / 255.0};
    };
    const auto sample = srgb(rgb);
    const char* label = names[0].label;
    double best = std::numeric_limits<double>::max();
    for (const auto& candidate : names) {
        const double distance = tex2color::color_utils::calc_rgb_color_difference_by_ciede2000_srgb01(
            sample, srgb(candidate.rgb));
        if (distance < best) {
            best = distance;
            label = candidate.label;
        }
    }
    return label;
}

namespace {
class WorkbenchImportColorRows final : public wxPanel {
public:
    WorkbenchImportColorRows(wxWindow* parent, const WorkbenchImportColorComparison& colors)
        : wxPanel(parent), m_colors(colors)
    {
        SetName("ai_content_color");
        SetBackgroundColour(parent->GetBackgroundColour());
        m_scroll = new WorkbenchScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
        m_scroll->SetName("ai_content_color");
        m_scroll->SetBackgroundColour(GetBackgroundColour());
        m_scroll->SetFont(parent->GetFont());
        m_scroll->SetMinSize(wxSize(0, 0));
        auto* content = new wxBoxSizer(wxVERTICAL);
        // The scroll extent represents every swatch, while painting only the
        // current rows also accommodates models with baked texture colours.
        m_extent = content->Add(0, 0);
        m_scroll->SetSizer(content);
        m_scroll->set_viewport_layout_callback([this] { m_scroll->Refresh(false); });
        m_scroll->Bind(wxEVT_PAINT, [this](wxPaintEvent&) { draw_visible_rows(); });
        auto* viewport = new wxBoxSizer(wxVERTICAL);
        viewport->Add(m_scroll, 1, wxEXPAND);
        SetSizer(viewport);
    }

    int row_height() const { return std::max(FromDIP(32), m_scroll->GetCharHeight() + FromDIP(12)); }
    size_t row_count() const { return m_colors.missing.size() + m_colors.matched.size() + m_colors.unused.size(); }

    void layout_rows()
    {
        const int height = row_height();
        m_extent->SetMinSize(0, int(row_count()) * height);
        m_scroll->SetScrollRate(0, height);
        m_scroll->Layout();
    }

    void draw_header(wxDC& dc, const wxRect& rect) const
    {
        dc.SetFont(wxGetApp().bold_font());
        dc.SetTextForeground(wxColour(183, 183, 189));
        const auto columns = column_bounds(rect);
        draw_cell(dc, columns[0], _L("模型配色"));
        draw_cell(dc, columns[1], _L("打印机耗材"));
        draw_cell(dc, columns[2], _L("状态"));
    }

private:
    void draw_visible_rows()
    {
        wxAutoBufferedPaintDC dc(m_scroll);
        dc.SetBackground(wxBrush(GetBackgroundColour()));
        dc.Clear();
        dc.SetFont(m_scroll->GetFont());
        const auto viewport = m_scroll->GetClientSize();
        const int height = row_height();
        const size_t first = size_t(m_scroll->GetViewStart().y);
        for (size_t index = first; index < row_count(); ++index) {
            const int y = int(index - first) * height;
            if (y >= viewport.y) break;
            draw_row(dc, wxRect(0, y, viewport.x, height), index);
        }
    }

    void draw_row(wxDC& dc, const wxRect& rect, size_t index) const
    {
        const bool missing = index < m_colors.missing.size();
        const size_t matched_end = m_colors.missing.size() + m_colors.matched.size();
        const bool unused = index >= matched_end;
        const auto* filament = missing ? nullptr : unused ? &m_colors.unused[index - matched_end]
            : &m_colors.matched[index - m_colors.missing.size()];
        const uint32_t rgb = missing ? m_colors.missing[index] : PrinterFilamentPreview::rgb8(filament->color);
        const wxString name = _L(workbench_import_color_name(rgb));
        const auto columns = column_bounds(rect);
        dc.SetTextForeground(wxColour(238, 240, 244));
        if (unused) draw_cell(dc, columns[0], _L("—"));
        else draw_cell(dc, columns[0], name, rgb);
        if (missing) draw_cell(dc, columns[1], _L("无对应色"));
        else draw_cell(dc, columns[1], filament->mixed ? wxString::Format(_L("%s · 叠色"), name) : name, rgb);
        dc.SetTextForeground(missing ? wxColour(255, 194, 39) : wxColour(183, 183, 189));
        draw_cell(dc, columns[2], missing ? _L("缺少") : unused ? _L("模型未使用") : _L("已匹配"));
        dc.SetPen(wxPen(wxColour(51, 51, 55)));
        dc.DrawLine(rect.x, rect.GetBottom(), rect.GetRight() + 1, rect.GetBottom());
    }

    std::array<wxRect, 3> column_bounds(const wxRect& rect) const
    {
        const int width = std::max(0, rect.width - FromDIP(10));
        const int colors_width = std::max(0, width - FromDIP(94));
        return {wxRect(rect.x, rect.y, colors_width / 2, rect.height),
                wxRect(rect.x + colors_width / 2, rect.y, colors_width - colors_width / 2, rect.height),
                wxRect(rect.x + colors_width, rect.y, width - colors_width, rect.height)};
    }

    void draw_cell(wxDC& dc, const wxRect& bounds, const wxString& text, std::optional<uint32_t> rgb = {}) const
    {
        int x = bounds.x + FromDIP(12);
        if (rgb) {
            const wxColour color(uint8_t(*rgb >> 16), uint8_t(*rgb >> 8), uint8_t(*rgb));
            const int size = FromDIP(16);
            dc.SetPen(wxPen(wxColour(112, 112, 118)));
            dc.SetBrush(wxBrush(color));
            dc.DrawRoundedRectangle(x, bounds.y + (bounds.height - size) / 2, size, size, FromDIP(3));
            x += FromDIP(24);
        }
        const auto label = wxControl::Ellipsize(text, dc, wxELLIPSIZE_END,
            std::max(1, bounds.GetRight() - x - FromDIP(5)));
        dc.DrawText(label, x, bounds.y + (bounds.height - dc.GetCharHeight()) / 2);
    }

    const WorkbenchImportColorComparison& m_colors;
    WorkbenchScrolledWindow* m_scroll {nullptr};
    wxSizerItem* m_extent {nullptr};
};
}

WorkbenchImportColorAction show_workbench_import_color_comparison(
    wxWindow* parent, const WorkbenchImportColorComparison& comparison)
{
    auto* owner = wxGetTopLevelParent(parent);
    WorkbenchFloatingCard dialog(owner);
    dialog.SetTitle(_L("配色不匹配"));
    dialog.SetFont(parent->GetFont());
    const auto label = [&](const wxString& text, bool emphasized = false, long style = 0) {
        auto* control = new WorkbenchStaticText(&dialog, wxID_ANY, text, wxDefaultPosition, wxDefaultSize, style);
        control->SetForegroundColour(emphasized ? wxColour(238, 240, 244) : wxColour(183, 183, 189));
        if (emphasized) control->SetFont(wxGetApp().bold_font());
        return control;
    };

    auto* root = new wxBoxSizer(wxVERTICAL);
    auto* heading = new wxBoxSizer(wxHORIZONTAL);
    heading->Add(label(dialog.GetTitle(), true), 1, wxALIGN_CENTER_VERTICAL);
    auto* close = workbench_button(&dialog, _L("关闭"));
    heading->Add(close, 0, wxALIGN_CENTER_VERTICAL);
    root->Add(heading, 0, wxEXPAND | wxBOTTOM);

    wxString names;
    std::set<wxString> shown_names;
    const size_t shown = std::min<size_t>(3, comparison.missing.size());
    for (size_t index = 0; index < shown; ++index) {
        const wxString name = _L(workbench_import_color_name(comparison.missing[index]));
        if (!shown_names.insert(name).second) continue;
        if (!names.empty()) names += _L("、");
        names += name;
    }
    const auto summary = wxString::Format(comparison.missing.size() > shown
        ? _L("当前耗材缺少%s等颜色。") : _L("当前耗材缺少%s。"), names);
    auto* summary_label = label(wxEmptyString, false, wxST_NO_AUTORESIZE);
    root->Add(summary_label, 0, wxEXPAND | wxBOTTOM);

    auto* table = new wxPanel(&dialog, wxID_ANY);
    table->SetName("ai_content_color");
    table->SetBackgroundColour(wxColour(36, 36, 40));
    auto* header = new wxPanel(table);
    header->SetName("ai_content_color");
    header->SetBackgroundColour(wxColour(41, 41, 45));
    header->SetBackgroundStyle(wxBG_STYLE_PAINT);
    auto* rows = new WorkbenchImportColorRows(table, comparison);
    header->Bind(wxEVT_PAINT, [header, rows](wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(header);
        dc.SetBackground(wxBrush(header->GetBackgroundColour()));
        dc.Clear();
        rows->draw_header(dc, header->GetClientRect());
    });
    auto* table_sizer = new wxBoxSizer(wxVERTICAL);
    table_sizer->Add(header, 0, wxEXPAND);
    table_sizer->Add(rows, 1, wxEXPAND);
    table->SetSizer(table_sizer);
    root->Add(table, 1, wxEXPAND | wxBOTTOM);

    const wxString next_step = _L("处理配色后，再点击「去切片」。");
    auto* next_step_label = label(wxEmptyString, false, wxST_NO_AUTORESIZE);
    root->Add(next_step_label, 0, wxEXPAND | wxBOTTOM);
    auto* actions = new wxBoxSizer(wxHORIZONTAL);
    auto* map_to_model = workbench_button(&dialog, _L("将耗材色映射到模型"));
    map_to_model->SetBackgroundColor(StateColor(
        std::pair<wxColour, int>(wxColour(255, 208, 80), StateColor::Hovered),
        std::pair<wxColour, int>(wxColour(255, 194, 39), StateColor::Normal)));
    map_to_model->SetTextColor(StateColor(wxColour(25, 25, 28)));
    auto* import_to_filaments = workbench_button(&dialog, _L("将模型配色导入到耗材"));
    actions->Add(map_to_model, 1, wxEXPAND);
    auto* action_gap = actions->AddSpacer(0);
    actions->Add(import_to_filaments, 1, wxEXPAND);
    root->Add(actions, 0, wxEXPAND);

    auto* outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(root, 1, wxEXPAND | wxALL);
    dialog.SetSizer(outer);
    const auto resize = [&] {
        const int display = wxDisplay::GetFromWindow(dialog.IsShown() ? &dialog : owner);
        wxRect area = wxDisplay(display == wxNOT_FOUND ? 0 : display).GetClientArea();
        const wxRect owner_area(owner->ClientToScreen(wxPoint()), owner->GetClientSize());
        if (area.Intersects(owner_area)) area.Intersect(owner_area);
        const int margin = dialog.FromDIP(14);
        const int inset = dialog.FromDIP(12);
        const int width = std::min(dialog.FromDIP(480), std::max(1, area.width - 2 * margin));
        const int text_width = std::max(1, width - 2 * margin);
        outer->GetItem(root)->SetBorder(margin);
        for (auto* item : {root->GetItem(heading), root->GetItem(summary_label),
                           root->GetItem(table), root->GetItem(next_step_label)})
            item->SetBorder(inset);
        set_wrapped_workbench_text(summary_label, summary, text_width);
        set_wrapped_workbench_text(next_step_label, next_step, text_width);
        close->SetMinSize(dialog.FromDIP(wxSize(68, 32)));
        map_to_model->SetMinSize(dialog.FromDIP(wxSize(-1, 32)));
        import_to_filaments->SetMinSize(dialog.FromDIP(wxSize(-1, 32)));
        const int action_width = std::max(map_to_model->GetBestSize().x, import_to_filaments->GetBestSize().x);
        actions->SetOrientation(2 * action_width + inset <= text_width ? wxHORIZONTAL : wxVERTICAL);
        action_gap->SetMinSize(wxSize(inset, inset));
        const int row_height = rows->row_height();
        header->SetMinSize(wxSize(0, row_height));
        rows->SetMinSize(wxSize(0, 0));
        const int available = area.height - 2 * margin - outer->CalcMin().y;
        const int row_count = int(std::min<size_t>(rows->row_count(), 8));
        rows->SetMinSize(wxSize(0, std::min(row_count * row_height, std::max(row_height, available))));
        const auto minimum = outer->CalcMin();
        dialog.SetClientSize(wxSize(std::max(width, minimum.x), minimum.y));
        dialog.Layout();
        rows->layout_rows();
        header->Refresh(false);
        apply_workbench_floating_card_shape(&dialog, 8, dialog.GetBackgroundColour());
        const auto size = dialog.GetSize();
        dialog.Move(area.x + (area.width - size.x) / 2, area.y + (area.height - size.y) / 2);
    };

    close->Bind(wxEVT_BUTTON, [&dialog](wxCommandEvent&) { dialog.EndModal(wxID_CANCEL); });
    map_to_model->Bind(wxEVT_BUTTON, [&dialog](wxCommandEvent&) { dialog.EndModal(wxID_APPLY); });
    import_to_filaments->Bind(wxEVT_BUTTON, [&dialog](wxCommandEvent&) { dialog.EndModal(wxID_OK); });
    dialog.Bind(wxEVT_CHAR_HOOK, [&dialog](wxKeyEvent& event) {
        if (event.GetKeyCode() == WXK_ESCAPE) dialog.EndModal(wxID_CANCEL);
        else event.Skip();
    });
    dialog.Bind(wxEVT_DPI_CHANGED, [&dialog, resize](wxDPIChangedEvent& event) {
        event.Skip();
        dialog.CallAfter(resize);
    });
    resize();
    close->SetFocus();
    switch (dialog.ShowModal()) {
    case wxID_APPLY: return WorkbenchImportColorAction::MapToModel;
    case wxID_OK: return WorkbenchImportColorAction::ImportToFilaments;
    default: return WorkbenchImportColorAction::Close;
    }
}

void ModelGenerationPanel::show_workbench_check_details()
{
    if (!m_workbench_shell) return;
    hide_workbench_check_card();
    const auto& check = m_workbench_check_result;
    const auto model_hash = check.model_sha256;
    const auto preview_hash = !m_preview_loading && m_model_preview ? m_model_preview->model_sha256() : std::string();
    const bool can_locate = workbench_check_can_locate(check, preview_hash);
    const auto issue_faces = can_locate ? check.issue_faces : std::vector<size_t>{};
    const auto face_counts = workbench_check_face_counts(check);
    const wxString next_step = from_u8(workbench_check_next_step(check));
    const auto topology_details = workbench_topology_details(check, preview_hash);

    WorkbenchFloatingCard dialog(m_workbench_shell);
    dialog.SetTitle(_L("检查修复详情"));
    dialog.SetFont(m_workbench_shell->GetFont());
    const auto text = [](wxWindow* parent, const wxString& value, bool emphasized, long style = 0) {
        auto* label = new WorkbenchStaticText(parent, wxID_ANY, value, wxDefaultPosition, wxDefaultSize, style);
        label->SetForegroundColour(emphasized ? wxColour(238, 240, 244) : wxColour(183, 183, 189));
        if (emphasized) label->SetFont(wxGetApp().bold_font());
        return label;
    };

    auto* root = new wxBoxSizer(wxVERTICAL);
    auto* title = text(&dialog, dialog.GetTitle(), true);
    root->Add(title, 0, wxEXPAND | wxBOTTOM);

    auto* result = new wxPanel(&dialog);
    result->SetName("ai_content_color");
    result->SetBackgroundColour(wxColour(36, 36, 40));
    auto* result_rows = new wxBoxSizer(wxVERTICAL);
    result_rows->Add(text(result, from_u8(workbench_check_status_line(check)), true), 0, wxEXPAND);

    const auto add_face_count = [&](const wxString& label, size_t count) {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(text(result, label, false), 1, wxALIGN_CENTER_VERTICAL);
        const auto number = wxNumberFormatter::ToString(static_cast<wxLongLong_t>(count));
        row->Add(text(result, wxString::Format(_L("%s 面"), number), true), 0, wxALIGN_CENTER_VERTICAL);
        result_rows->Add(row, 0, wxEXPAND | wxTOP);
    };
    if (face_counts.before_repair) {
        add_face_count(_L("修复前"), face_counts.before_repair);
        add_face_count(_L("修复后"), face_counts.current);
    } else if (face_counts.current) {
        add_face_count(_L("三角面数"), face_counts.current);
    }

    std::vector<std::pair<WorkbenchStaticText*, wxString>> wrapped_rows;
    const auto add_wrapped_row = [&](wxBoxSizer* rows, const wxString& value) {
        auto* label = text(result, value, false, wxST_NO_AUTORESIZE);
        rows->Add(label, 0, wxEXPAND | wxTOP);
        wrapped_rows.emplace_back(label, value);
    };
    auto* topology_rows = new wxBoxSizer(wxVERTICAL);
    topology_rows->Add(text(result, _L("拓扑检查"), true), 0, wxEXPAND);
    if (topology_details.rows.empty())
        add_wrapped_row(topology_rows, from_u8(topology_details.summary));
    else
        for (const auto& row : topology_details.rows) add_wrapped_row(topology_rows, from_u8(row));
    result_rows->Add(topology_rows, 0, wxEXPAND | wxTOP);

    if (!next_step.empty()) add_wrapped_row(result_rows, next_step);

    auto* result_outer = new wxBoxSizer(wxVERTICAL);
    result_outer->Add(result_rows, 1, wxEXPAND | wxALL);
    result->SetSizer(result_outer);
    root->Add(result, 0, wxEXPAND | wxBOTTOM);

    auto* footer = new wxBoxSizer(wxHORIZONTAL);
    WorkbenchButton* locate = nullptr;
    if (can_locate) {
        locate = workbench_button(&dialog, _L("定位问题区域"));
        locate->Bind(wxEVT_BUTTON, [&dialog](wxCommandEvent&) { dialog.EndModal(wxID_APPLY); });
        footer->Add(locate, 0, wxALIGN_CENTER_VERTICAL);
    }
    footer->AddStretchSpacer();
    auto* close = workbench_button(&dialog, _L("关闭"));
    close->Bind(wxEVT_BUTTON, [&dialog](wxCommandEvent&) { dialog.EndModal(wxID_CLOSE); });
    footer->Add(close, 0, wxALIGN_CENTER_VERTICAL);
    root->Add(footer, 0, wxEXPAND);

    auto* outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(root, 1, wxEXPAND | wxALL);
    dialog.SetSizer(outer);

    const auto resize = [&] {
        const int display = wxDisplay::GetFromWindow(dialog.IsShown() ? &dialog : wxGetTopLevelParent(m_workbench_shell));
        const wxRect area = wxDisplay(display == wxNOT_FOUND ? 0 : display).GetClientArea();
        const int margin = dialog.FromDIP(14), inset = dialog.FromDIP(12);
        const int width = std::min(dialog.FromDIP(360), area.width);
        outer->GetItem(root)->SetBorder(margin);
        root->GetItem(title)->SetBorder(inset);
        root->GetItem(result)->SetBorder(inset);
        result_outer->GetItem(result_rows)->SetBorder(inset);
        for (auto* row : result_rows->GetChildren()) row->SetBorder(dialog.FromDIP(8));
        for (auto* row : topology_rows->GetChildren()) row->SetBorder(dialog.FromDIP(4));
        const int text_width = std::max(1, width - 2 * (margin + inset));
        for (const auto& [label, value] : wrapped_rows) set_wrapped_workbench_text(label, value, text_width);
        close->SetMinSize(dialog.FromDIP(wxSize(68, 32)));
        if (locate) locate->SetMinSize(dialog.FromDIP(wxSize(120, 32)));
        const auto minimum = outer->CalcMin();
        dialog.SetClientSize(wxSize(std::max(width, minimum.x), minimum.y));
        dialog.Layout();
        apply_workbench_floating_card_shape(&dialog, 8, dialog.GetBackgroundColour());
    };
    dialog.Bind(wxEVT_CHAR_HOOK, [&dialog](wxKeyEvent& event) {
        if (event.GetKeyCode() == WXK_ESCAPE) dialog.EndModal(wxID_CANCEL);
        else event.Skip();
    });
    dialog.Bind(wxEVT_DPI_CHANGED, [&dialog, resize](wxDPIChangedEvent& event) {
        event.Skip();
        dialog.CallAfter(resize);
    });
    resize();
    dialog.CenterOnParent();
    close->SetFocus();
    if (dialog.ShowModal() == wxID_APPLY && !m_preview_loading &&
        m_model_preview && m_model_preview->model_sha256() == model_hash) {
        m_model_preview->show_workbench_issue_faces(issue_faces);
    }
}

void ModelGenerationPanel::update_workbench_check_card()
{
    if (!m_workbench_check_card || !m_workbench_check_modal) return;
    const auto& result = m_workbench_check_result;
    const bool native_repair = m_native_workbench_partition_running;
    const auto mode = workbench_floating_card_mode(m_finishing_workbench, m_workbench_presented,
        m_workbench_honor_semantic_active, m_workbench_check_card_visible,
        m_workbench_check_running, m_workbench_auto_repair && m_finishing_running,
        native_repair, result);
    if (mode == WorkbenchFloatingCardMode::None || !m_workbench_shell ||
        !m_workbench_shell->IsShownOnScreen()) {
        hide_workbench_check_card();
        return;
    }
    if (m_workbench_shell->IsFrozen()) {
        queue_workbench_surface_refresh();
        return;
    }
    m_workbench_floating_card_mode = mode;
    const bool was_shown = m_workbench_check_card->IsShown();
    const wxRect previous_bounds = was_shown ? m_workbench_check_card->GetScreenRect() : wxRect();
    const bool honor_semantic = mode == WorkbenchFloatingCardMode::HonorSuiteSemantic;
    const bool saving_repair = m_workbench_auto_repair && m_finishing_running;
    const bool running = native_repair || honor_semantic || m_workbench_check_running || saving_repair;
    if (!honor_semantic) m_workbench_check_card_visible = true;
    const wxColour modal_text(238, 240, 244);
    for (auto* label : {m_workbench_check_phase, m_workbench_check_message}) {
        if (label->GetForegroundColour() != modal_text) {
            label->SetForegroundColour(modal_text);
            label->Refresh(false);
        }
    }
    if (m_model_preview) m_model_preview->set_workbench_input_locked(true);
    if (m_workbench_shell && m_workbench_shell->IsEnabled()) m_workbench_shell->Disable();
    wxString phase;
    if (native_repair) phase = _L("修复模型");
    else if (honor_semantic) phase = _L("正在应用荣耀耗材套装");
    else if (m_workbench_check_running) {
        switch (result.phase) {
        case WorkbenchCheckPhase::Repairing: phase = _L("修复模型"); break;
        case WorkbenchCheckPhase::Rechecking: phase = _L("复检模型"); break;
        default: phase = _L("检查模型"); break;
        }
    } else if (saving_repair) phase = _L("保存修复结果");
    else phase = from_u8(workbench_check_status_line(result));
    bool modal_layout_changed = set_workbench_label(m_workbench_check_phase, phase);
    wxString message = native_repair
        ? from_u8(m_workbench_operation_notice)
        : honor_semantic ? m_workbench_honor_semantic_message
        : m_workbench_check_running ? from_u8(result.summary) : wxString();
    if (honor_semantic && message.empty())
        message = _L("正在识别人像区域并匹配肤色、头发、眼白、牙齿和嘴唇，请稍候……");
    else if (!honor_semantic && saving_repair)
        message = _L("安全修复已通过复检，正在自动保存……");
    else if (!running) {
        const auto preview_hash = !m_preview_loading && m_model_preview ? m_model_preview->model_sha256() : std::string();
        message = from_u8(workbench_topology_details(result, preview_hash).summary);
    }
    modal_layout_changed = set_workbench_shown(m_workbench_check_message, !message.empty()) || modal_layout_changed;
    if (set_workbench_label(m_workbench_check_message, message)) {
        m_workbench_check_message->Wrap(FromDIP(376));
        modal_layout_changed = true;
    }
    modal_layout_changed = set_workbench_shown(m_workbench_check_cancel_button,
        native_repair || (!honor_semantic && (m_workbench_check_running || saving_repair))) || modal_layout_changed;
    const bool cancel_requested = native_repair
        ? m_native_workbench_partition_cancel && m_native_workbench_partition_cancel->load()
        : saving_repair
            ? m_finishing_canceled && m_finishing_canceled->load()
            : m_workbench_check_cancel && m_workbench_check_cancel->load();
    set_workbench_enabled(m_workbench_check_cancel_button,
        running && !honor_semantic && !cancel_requested);
    const bool review = !honor_semantic && !running && result.candidate_status != RepairCandidateStatus::Ready;
    const bool retry = review && result.status != WorkbenchCheckStatus::Normal;
    set_workbench_label(m_workbench_check_continue_button, _L("重新检查修复"));
    modal_layout_changed = set_workbench_shown(m_workbench_check_continue_button, retry) || modal_layout_changed;
    modal_layout_changed = set_workbench_shown(m_workbench_check_close_button, review) || modal_layout_changed;
    modal_layout_changed = set_workbench_shown(m_workbench_check_details_button, review) || modal_layout_changed;
    modal_layout_changed = set_workbench_shown(m_workbench_check_return_button,
        !honor_semantic && !running) || modal_layout_changed;
    modal_layout_changed = set_workbench_shown(m_workbench_check_progress, running) || modal_layout_changed;
    m_workbench_check_progress->set_progress(honor_semantic && m_workbench_honor_semantic_progress > 0
        ? std::optional<int>(m_workbench_honor_semantic_progress) : std::nullopt);
    m_workbench_check_progress->set_running(running);
    if (modal_layout_changed) {
        // An explicit panel minimum bypasses wxPanel's sizer-derived best
        // size. Recompute it from the visible contents before fitting the card.
        wxSize content_size = m_workbench_check_modal->GetSizer()->GetMinSize();
        content_size.IncTo(FromDIP(wxSize(410, 174)));
        m_workbench_check_modal->SetMinSize(content_size);
        m_workbench_check_modal->InvalidateBestSize();
        m_workbench_check_card->InvalidateBestSize();
        m_workbench_check_card->Fit();
        m_workbench_check_card->Layout();
        m_workbench_check_modal->Layout();
        apply_workbench_floating_card_shape(m_workbench_check_card, 8, wxColour(45, 45, 49));
    }
    if (!position_workbench_check_card(true)) {
        hide_workbench_check_card();
        return;
    }
    if (!was_shown) {
        apply_workbench_floating_card_shape(m_workbench_check_card, 8, wxColour(45, 45, 49));
        m_workbench_check_card->Show();
    } else if (previous_bounds != m_workbench_check_card->GetScreenRect()) {
        queue_workbench_surface_refresh();
    }
    install_workbench_card_layer_guard();
    raise_workbench_check_card();
    // The first checking frame must be visible before the worker or model
    // presentation can occupy the next turn; stage changes use this same pass.
    if (!was_shown || modal_layout_changed) present_workbench_controls(m_workbench_check_card);
}
void ModelGenerationPanel::rescale_post_generation_workbench()
{
    if (!m_workbench_shell) return;
    const auto rescale = [](auto&& self, wxWindow* window) -> void {
        if (!window) return;
        window->SetFont(window->GetFont());
        if (auto* button = dynamic_cast<WorkbenchButton*>(window)) button->rescale_workbench();
        else if (auto* toggle = dynamic_cast<WorkbenchSwitch*>(window)) toggle->rescale_workbench();
        else if (auto* stepper = dynamic_cast<WorkbenchNumberStepper*>(window)) stepper->rescale_workbench();
        else if (auto* button = dynamic_cast<Button*>(window)) button->Rescale();
        if (auto* choice = dynamic_cast<ComboBox*>(window)) choice->Rescale();
        else if (auto* input = dynamic_cast<TextInput*>(window)) input->Rescale();
        if (auto* panel = dynamic_cast<WorkbenchPanel*>(window)) panel->rescale_workbench();
        for (wxWindow* child : window->GetChildren()) self(self, child);
        window->InvalidateBestSize();
    };
    rescale(rescale, m_workbench_shell);
    rescale(rescale, m_workbench_history_toggle);
    rescale(rescale, m_workbench_check_card);
    for (auto* control : {m_workbench_apply_project_filaments, m_workbench_printer_filament_trial}) {
        if (!control) continue;
        control->SetFont(Label::Body_14);
        static_cast<WorkbenchButton*>(control)->SetIconSpacing(FromDIP(5));
    }
    if (m_workbench_check_card) {
        m_workbench_check_card->Fit();
        apply_workbench_floating_card_shape(m_workbench_check_card, 8, wxColour(45, 45, 49));
        position_workbench_check_card();
    }
    if (m_workbench_logo) {
        m_workbench_logo->SetBitmap(create_scaled_bitmap("workbench_logo", m_workbench_logo, 36));
        m_workbench_logo->SetMinSize(FromDIP(wxSize(36, 36)));
    }
    if (m_workbench_beauty_info) {
        m_workbench_beauty_info->SetBitmap(create_scaled_bitmap("workbench_info", m_workbench_beauty_info, 12));
        m_workbench_beauty_info->SetMinSize(FromDIP(wxSize(12, 12)));
    }
    const wxSize status_size = FromDIP(wxSize(100, 24));
    m_workbench_model_status->SetFont(wxGetApp().bold_font());
    m_workbench_model_status->SetMinSize(status_size);
    m_workbench_model_status->SetMaxSize(status_size);
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
    layout_workbench_history_toggle(true);
    Layout();
    Refresh(false);
}

void ModelGenerationPanel::layout_workbench_history_toggle(bool force)
{
    if (!m_workbench_history_toggle || !m_model_preview) return;
    if (m_workbench_history_sidebar_mounted) {
        layout_mounted_workbench_history_sidebar();
        return;
    }
    if (!m_workbench_shell || m_workbench_history_home || m_shutdown ||
        !m_workbench_shell->IsShownOnScreen()) return;
    bool layout_needed = update_workbench_history_visibility() || force;
    auto* viewport = m_model_preview->workbench_viewport_window();
    if (!viewport) return;
    const wxRect viewport_rect(m_workbench_shell->ScreenToClient(viewport->ClientToScreen(wxPoint())),
        viewport->GetClientSize());
    const wxSize toggle_size = m_workbench_shell->FromDIP(workbench_history_handle_size());
    const auto toggle_rect = workbench_history_toggle_bounds(m_workbench_shell->GetClientSize(),
        viewport_rect, toggle_size, m_workbench_history_panel->IsShown()
            ? std::optional<int>(m_workbench_shell->ScreenToClient(
                m_workbench_history_panel->ClientToScreen(wxPoint())).x) : std::nullopt);
    if (!toggle_rect) return;
    m_workbench_history_toggle->SetMinSize(toggle_size);
    m_workbench_history_toggle->SetMaxSize(toggle_size);
    if (m_workbench_history_toggle->GetRect() != *toggle_rect) {
        m_workbench_history_toggle->SetSize(*toggle_rect);
        layout_needed = true;
    }
    if (layout_needed) {
        m_workbench_history_toggle->Raise();
        m_workbench_history_toggle->Refresh(false);
    }
}

void ModelGenerationPanel::refresh_post_generation_workbench()
{
    queue_workbench_surface_refresh(false, Presentation::Dirty::Content | Presentation::Dirty::Paint);
}

void ModelGenerationPanel::apply_workbench_snapshot()
{
    if (!m_workbench_shell || !m_finishing_workbench || m_refreshing_workbench_layout) return;
    // Layout sends synchronous size events back to this refresh function.
    m_refreshing_workbench_layout = true;
    bool layout_changed = false;
    const auto state = post_generation_ui_state();
    const auto snapshot = workbench_snapshot();
    const auto appearance_state = workbench_original_appearance_state(state.status, m_preview_loading,
        m_model_preview_ready && m_model_preview->has_model() && !m_displayed_model_path.empty(),
        m_model_preview->original_comparison_available(), !m_model_preview->original_appearance_error().empty());
    const bool original_appearance_available = appearance_state == WorkbenchOriginalAppearanceState::Available;
    const bool model_unavailable = appearance_state == WorkbenchOriginalAppearanceState::Empty ||
        appearance_state == WorkbenchOriginalAppearanceState::Loading ||
        appearance_state == WorkbenchOriginalAppearanceState::ModelError;
    const wxString model_hint = from_u8(workbench_original_appearance_hint(appearance_state));
    const wxString original_appearance_reason = appearance_state == WorkbenchOriginalAppearanceState::Invalid
        ? model_hint + _L("：") + from_u8(m_model_preview->original_appearance_error()) : model_hint;
    m_model_preview->set_workbench_palette_hint(original_appearance_reason);
    update_workbench_history_actions();
    const auto title_path = !m_finishing_candidate.empty() && !m_finishing_source.empty()
        ? m_finishing_source : m_displayed_model_path;
    bool accepted = !m_finishing_accepted_path.empty() && title_path == m_finishing_accepted_path;
    for (const auto& entry : m_library_entries) {
        if (entry.model_path != title_path) continue;
        accepted = accepted || entry.accepted_finishing;
        break;
    }
    const wxString check_status = from_u8(workbench_check_status_line(snapshot.check));
    const bool check_canceled = snapshot.check.issue == WorkbenchCheckIssue::Canceled;
    wxString model_status;
    switch (state.status) {
    case PostGenerationUiState::Status::Empty: model_status = _L("未加载"); break;
    case PostGenerationUiState::Status::Loading: model_status = _L("加载中"); break;
    case PostGenerationUiState::Status::Ready: model_status = accepted ? _L("已处理") : _L("就绪"); break;
    case PostGenerationUiState::Status::Editing: model_status = _L("编辑中"); break;
    case PostGenerationUiState::Status::Processing: model_status = _L("处理中"); break;
    case PostGenerationUiState::Status::CandidateReady: model_status = _L("候选就绪"); break;
    case PostGenerationUiState::Status::ComparingBefore: model_status = _L("处理前"); break;
    case PostGenerationUiState::Status::Error: model_status = _L("加载失败"); break;
    }
    auto status_tone = post_generation_status_tone(
        state.status, m_model_preview && m_model_preview->original_comparison_enabled());
    if (state.status == PostGenerationUiState::Status::Ready ||
        (state.status == PostGenerationUiState::Status::CandidateReady && m_workbench_auto_repair)) {
        switch (workbench_check_outcome(snapshot.check, m_native_workbench_partition_running ||
                m_workbench_check_running || (m_workbench_auto_repair && m_finishing_running))) {
        case WorkbenchCheckOutcome::Pending:
            model_status = _L("等待检查"); status_tone = PostGenerationStatusTone::Accent; break;
        case WorkbenchCheckOutcome::Processing:
            model_status = _L("检查处理中"); status_tone = PostGenerationStatusTone::Accent; break;
        case WorkbenchCheckOutcome::Passed:
            model_status = check_status; status_tone = PostGenerationStatusTone::Success; break;
        case WorkbenchCheckOutcome::Failed:
            model_status = check_status;
            status_tone = check_canceled ? PostGenerationStatusTone::Neutral : PostGenerationStatusTone::Error;
            break;
        }
    }
    set_workbench_label(m_workbench_model_status, model_status);
    set_workbench_tooltip(m_workbench_model_status, model_status);
    const wxColour status_colour = workbench_status_colour(status_tone);
    if (m_workbench_model_status->GetForegroundColour() != status_colour) {
        m_workbench_model_status->SetForegroundColour(status_colour);
        m_workbench_model_status->Refresh(false);
    }
    if (m_workbench_model_info) set_workbench_enabled(m_workbench_model_info, !model_unavailable);
    const bool show_loading_status = m_workbench_load_error || m_preview_loading;
    const bool show_filament_error = !model_unavailable && !m_workbench_printer_filament_error.empty();
    const bool show_status = show_loading_status || show_filament_error;
    layout_changed = set_workbench_shown(m_workbench_state_status, show_status) || layout_changed;
    if (show_status) {
        const wxString message = show_loading_status ? m_status->GetLabel() : from_u8(m_workbench_printer_filament_error);
        set_workbench_tooltip(m_workbench_state_status,
            m_workbench_load_error ? m_result_summary->GetLabel() : message);
        layout_changed = set_wrapped_workbench_text(m_workbench_state_status, message,
            std::max(FromDIP(200), m_workbench_view_host->GetClientSize().x - FromDIP(16)), true) || layout_changed;
    }
    if (m_workbench_settings_editing != m_workbench_editing) {
        // Move whole sizer groups, keeping their controls and Beauty session alive.
        for (size_t index = 1; index < m_workbench_settings_groups.size(); ++index)
            m_workbench_settings_sections->Detach(m_workbench_settings_groups[index]);
        for (size_t index = 0; index < 2; ++index)
            m_workbench_settings_sections->Insert(index,
                m_workbench_settings_groups[m_workbench_editing ? 2 - index : 1 + index], 0, wxEXPAND);
        m_workbench_settings_editing = m_workbench_editing;
        if (m_workbench_beauty_heading)
            m_workbench_beauty_heading->SetLabel(m_workbench_editing ? _L("模型美化") : _L("3D 美颜"));
        m_workbench_palette_heading->SetLabel(m_workbench_editing ? _L("目标色数") : _L("多色模型"));
        m_workbench_settings_scroll->Scroll(-1, 0);
        layout_changed = true;
    }
    layout_changed = set_workbench_shown(m_workbench_settings, true) || layout_changed;
    const bool finishing_ready = m_model_preview_ready && is_nonempty_model(m_displayed_model_path);
    const bool show_finishing_panel = m_workbench_editing &&
        (finishing_ready || m_finishing_running || !m_finishing_candidate.empty());
    layout_changed = set_workbench_shown(m_finishing_panel, show_finishing_panel) || layout_changed;
    const bool show_beauty_actions = m_workbench_editing && !m_workbench_check_running &&
        !(m_workbench_auto_repair && (m_finishing_running || !m_finishing_candidate.empty()));
    layout_changed = set_workbench_shown(m_workbench_footer, show_beauty_actions) || layout_changed;
    const bool history_was_shown = m_workbench_history_panel->IsShown();
    layout_changed = update_workbench_history_visibility() || layout_changed;
    set_workbench_enabled(m_workbench_configure_filaments, snapshot.can_edit_project_colors);
    set_workbench_enabled(m_workbench_add_filament, snapshot.can_edit_project_colors &&
        wxGetApp().preset_bundle && wxGetApp().preset_bundle->filament_presets.size() < 32 &&
        snapshot.printer_filaments.physical_count < snapshot.printer_filaments.physical_capacity);
    const bool device = wxGetApp().plater() && wxGetApp().plater()->get_machine_sync_status();
    set_workbench_enabled(m_workbench_sync_filaments, snapshot.can_edit_project_colors && device);
    set_workbench_tooltip(m_workbench_sync_filaments,
        device ? _L("使用原生设备耗材同步流程") : _L("当前没有可同步的同型号设备；可离线配置耗材"));
    const bool check_processing = m_native_workbench_partition_running || m_workbench_check_running ||
        (m_workbench_auto_repair && m_finishing_running);
    const bool topology_normal = !check_processing && snapshot.check.status == WorkbenchCheckStatus::Normal &&
        snapshot.check.candidate_status != RepairCandidateStatus::Ready && snapshot.candidate_path.empty() && !snapshot.dirty;
    layout_changed = set_workbench_label(m_workbench_check,
        m_workbench_check_running ? _L("取消") : topology_normal ? _L("通过") :
        check_canceled ? _L("重新检查") : _L("检查")) || layout_changed;
    wxColour check_colour(190, 190, 196);
    if (check_processing || snapshot.check.candidate_status == RepairCandidateStatus::Ready ||
        snapshot.check.status == WorkbenchCheckStatus::Running ||
        snapshot.check.status == WorkbenchCheckStatus::Attention)
        check_colour = wxColour(255, 194, 39);
    else if (snapshot.check.status == WorkbenchCheckStatus::Normal)
        check_colour = wxColour(91, 205, 137);
    else if (!check_canceled && (snapshot.check.status == WorkbenchCheckStatus::Invalid ||
                                 snapshot.check.status == WorkbenchCheckStatus::Failed))
        check_colour = wxColour(255, 128, 112);
    if (m_workbench_check_status->GetForegroundColour() != check_colour) {
        static_cast<WorkbenchButton*>(m_workbench_check)->SetTextColor(StateColor(check_colour));
        m_workbench_check_status->SetForegroundColour(check_colour);
    }
    set_workbench_enabled(m_workbench_check,
        m_workbench_check_running || can_start_workbench_check());
    for (wxWindow* color : m_workbench_project_colors->GetChildren())
        set_workbench_enabled(color, snapshot.can_edit_project_colors);
    if (m_workbench_apply_project_filaments) {
        const bool can_map = snapshot.can_apply_printer_filaments && !m_apply_filaments_saving;
        set_workbench_enabled(m_workbench_apply_project_filaments, can_map);
        set_workbench_tooltip(m_workbench_apply_project_filaments, can_map
            ? _L("以当前实体耗材与叠色槽为完整逻辑色卡，重建模型配色并自动保存新版本。")
            : model_unavailable ? model_hint : wxString::FromUTF8(snapshot.apply_filaments_reason.empty()
                ? "模型正在保存或处理，请稍候。" : snapshot.apply_filaments_reason));
    }
    wxString suite_capacity_hint;
    if (m_workbench_printer_filament_trial) {
        const auto* honor_suite = m_model_preview ? m_model_preview->active_honor_filament_suite() : nullptr;
        const auto* catalog_choice = m_model_preview ? m_model_preview->selected_honor_filament_suite_in_catalog() : nullptr;
        const auto* capacity_suite = catalog_choice ? catalog_choice : honor_suite;
        const auto* bundle = wxGetApp().preset_bundle;
        const auto suite_compatibility = capacity_suite && bundle
            ? PrinterFilamentConfig::honor_suite_compatibility(*bundle, *capacity_suite)
            : PrinterFilamentConfig::HonorSuiteCompatibility{};
        const bool capacity_blocked = !model_unavailable && suite_compatibility.needs_more_physical_nozzles;
        const auto card = m_model_preview
            ? m_model_preview->model_color_pack_for_filament_import() : std::nullopt;
        const bool has_card = card && card->valid_for_workbench_import();
        const bool suite_allowed = !honor_suite || suite_compatibility.compatible;
        const bool can_trial = workbench_printer_trial_action_allowed(
            m_printer_filament_trial_active, m_apply_filaments_saving,
            snapshot.can_edit_project_colors, has_card) &&
            (m_printer_filament_trial_active || (suite_allowed && !capacity_blocked));
        set_workbench_enabled(m_workbench_printer_filament_trial, can_trial);
        layout_changed = set_workbench_label(m_workbench_printer_filament_trial,
            m_printer_filament_trial_active ? _L("退出打印机耗材试色") : _L("将模型配色导入到耗材")) || layout_changed;
        wxString disabled_reason = model_unavailable ? model_hint : capacity_blocked || (honor_suite && !suite_allowed)
            ? wxString::FromUTF8(suite_compatibility.reason.empty()
                ? "当前打印机不支持六实体色荣耀耗材套装" : suite_compatibility.reason)
            : !has_card
            ? _L("当前没有 1 至 32 色的有效色卡，请先生成 AI配色或完成试色。")
            : !snapshot.can_edit_project_colors
                ? _L("模型处理中或存在未接受候选，请先完成当前操作。") : wxString{};
        if (capacity_blocked) {
            disabled_reason += "\n" + _L("请在「设置 → 打印机」选择兼容机型。");
            suite_capacity_hint = disabled_reason;
        }
        set_workbench_tooltip(m_workbench_printer_filament_trial, m_printer_filament_trial_active
            ? _L("退出试色并恢复进入前的模型外观；已提交的耗材配置保留。")
            : can_trial ? honor_suite
                ? _L("按套装顺序绑定六个真实实体耗材；不生成叠色配方。")
                : _L("将模型配色直接生成实体耗材与叠色配方；成功后一次提交并进入试色。")
                        : disabled_reason);
    }
    if (m_workbench_project_summary) {
        const auto& filaments = snapshot.printer_filaments;
        wxString summary = wxString::FromUTF8(filaments.printer)+
            wxString::Format(_L("\n%llu 个喷嘴 · %llu 个实体槽位"),static_cast<unsigned long long>(filaments.nozzles),
                static_cast<unsigned long long>(filaments.physical_count)) +
            (filaments.idle_nozzles ? wxString::Format(_L(" · %llu 个喷嘴空闲"), static_cast<unsigned long long>(filaments.idle_nozzles)) : wxString{}) +
            (model_unavailable || snapshot.apply_filaments_reason.empty()
                ? wxString{} : "\n" + wxString::FromUTF8(snapshot.apply_filaments_reason));
        if (!suite_capacity_hint.empty())
            summary += "\n" + suite_capacity_hint;
        layout_changed = set_wrapped_workbench_text(m_workbench_project_summary, summary,
            FromDIP(235), true) || layout_changed;
    }
    set_workbench_enabled(m_workbench_palette_details,
        state.can_edit && m_finishing_candidate.empty() && original_appearance_available &&
        m_model_preview->workbench_palette_details_available());
    set_workbench_tooltip(m_workbench_palette_details, original_appearance_available
        ? _L("查看和调整当前模型配色") : original_appearance_reason);
    m_model_preview->set_workbench_palette_editable(state.can_edit && m_finishing_candidate.empty());
    if (m_workbench_base) {
        std::string base_reason;
        const bool base_available = m_model_preview->round_base_preview_available(&base_reason);
        const bool base_enabled = m_model_preview->round_base_preview_enabled();
        if (m_workbench_base->GetValue() != base_enabled)
            m_workbench_base->SetValue(base_enabled);
        set_workbench_enabled(m_workbench_base, state.can_edit && base_available);
        set_workbench_tooltip(m_workbench_base, base_available
            ? _L("添加 3 mm 圆形底座，保留主体尺寸")
            : wxString::FromUTF8(base_reason));
    }
    if (m_workbench_original) {
        const bool comparison_available = original_appearance_available;
        const bool comparison_enabled = m_model_preview->original_comparison_enabled();
        if (m_workbench_original->GetValue() != comparison_enabled)
            m_workbench_original->SetValue(comparison_enabled);
        set_workbench_enabled(m_workbench_original, comparison_available);
        set_workbench_tooltip(m_workbench_original, comparison_available
            ? _L("并排显示当前模型与导入时的原贴图、顶点色或材质色")
            : original_appearance_reason);
    }
    if (m_workbench_grid) {
        const bool grid_visible = m_model_preview->workbench_grid_visible();
        if (m_workbench_grid->GetValue() != grid_visible)
            m_workbench_grid->SetValue(grid_visible);
        set_workbench_enabled(m_workbench_grid, m_model_preview->has_model());
    }
    const bool can_import = snapshot.can_import_for_slicing && is_nonempty_model(m_displayed_model_path);
    if (auto* portrait = FindWindowByName("portrait_r6_entry", m_workbench_shell)) {
        set_workbench_enabled(portrait, snapshot.actions.can_edit && snapshot.portrait_available);
        layout_changed = set_workbench_label(portrait,
            snapshot.portrait_enabled ? _L("退出人像保护（R6）") : _L("人像保护（R6）")) || layout_changed;
        set_workbench_tooltip(portrait, snapshot.portrait_available
            ? _L("可选人像保护；退出保留已保存的保护边界。")
            : wxString::FromUTF8(snapshot.portrait_unavailable_reason));
    }
    layout_changed = set_workbench_label(m_workbench_slicing, _L("去切片")) || layout_changed;
    set_workbench_enabled(m_workbench_slicing, can_import);
    set_workbench_tooltip(m_workbench_check, topology_normal
        ? _L("拓扑正常；点击重新检查") : _L("检查并修复模型，复检通过后自动保存新版本；原模型保留。"));
    wxString check_text = m_native_workbench_partition_running ? from_u8(m_workbench_operation_notice) :
        m_workbench_auto_repair && m_finishing_running ? _L("正在保存修复结果……") :
        check_status;
    wxString check_tooltip = check_text;
    if (!m_native_workbench_partition_running && !m_finishing_running) {
        const auto next_step = workbench_check_next_step(snapshot.check);
        if (!next_step.empty()) check_tooltip += "\n" + from_u8(next_step);
    }
    set_workbench_tooltip(m_workbench_check_status, check_tooltip);
    layout_changed = set_wrapped_workbench_text(m_workbench_check_status, check_text,
        FromDIP(210), false) || layout_changed;
    if (m_workbench_check_info) {
        const bool has_details = snapshot.check.status != WorkbenchCheckStatus::NotRun ||
            !snapshot.check.summary.empty() || !snapshot.check.repair_reason.empty();
        set_workbench_enabled(m_workbench_check_info, has_details);
        set_workbench_tooltip(m_workbench_check_info,
            has_details ? _L("查看检查详情") : _L("完成一次模型检查后可查看详情"));
    }
    const auto trial = m_model_preview->color_trial_state();
    const bool honor_suite_active = m_model_preview->active_honor_filament_suite() != nullptr;
    const size_t palette_count = m_model_preview->workbench_palette_color_count();
    const wxString palette_status = m_model_preview->keeps_original_history_colors() && palette_count == 0 &&
        original_appearance_available ? _L("模型原色") : from_u8(workbench_palette_status_line(
        appearance_state, honor_suite_active,
        m_model_preview->honor_suite_used_color_count(), palette_count,
        into_u8(trial.honor_suite_fallback_reason)));
    const wxColour palette_status_color = appearance_state == WorkbenchOriginalAppearanceState::Invalid
        ? wxColour(255, 150, 146) : wxColour(170, 170, 176);
    if (m_workbench_palette_status->GetForegroundColour() != palette_status_color) {
        m_workbench_palette_status->SetForegroundColour(palette_status_color);
        m_workbench_palette_status->Refresh(false);
    }
    set_workbench_tooltip(m_workbench_palette_status, original_appearance_available
        ? trial.honor_suite_fallback_reason.empty()
            ? _L("当前预览使用的逻辑颜色数量") : trial.honor_suite_fallback_reason
        : original_appearance_reason);
    layout_changed = set_wrapped_workbench_text(m_workbench_palette_status, palette_status,
        FromDIP(210), false) || layout_changed;
    layout_changed = set_workbench_shown(m_workbench_palette_status, !palette_status.empty()) || layout_changed;
    if (layout_changed) {
        m_workbench_shell->Layout();
        layout_workbench_history_toggle(true);
    } else layout_workbench_history_toggle();
    if (!history_was_shown && m_workbench_history_scroller->IsShownOnScreen())
        request_library_thumbnails(true);
    // The card uses the final viewport, after footer/history layout is complete.
    update_workbench_check_card();
    publish_workbench_state();
    m_refreshing_workbench_layout = false;
    queue_workbench_check();
}
} // namespace Slic3r::GUI
