#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
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
#include "slic3r/GUI/Redesign/RedesignMessageDialog.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "slic3r/GUI/I18N.hpp"
#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/dialog.h>
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

void ModelGenerationPanel::queue_workbench_surface_refresh()
{
    if (m_shutdown || m_workbench_surface_refresh_pending) return;
    m_workbench_surface_refresh_pending = true;
    wxWeakRef<ModelGenerationPanel> weak(this);
    CallAfter([weak] {
        if (!weak) return;
        // Requests made by the final layout/card placement belong to this
        // update too; release the coalescing flag only after they have settled.
        Slic3r::ScopeGuard pending_guard([weak] {
            if (weak) weak->m_workbench_surface_refresh_pending = false;
        });
        (void)pending_guard;
        if (weak->m_shutdown || !weak->m_workbench_presented ||
            !weak->m_finishing_workbench || !weak->m_workbench_shell ||
            !weak->m_workbench_shell->IsShownOnScreen()) return;
        const auto* owner = wxDynamicCast(wxGetTopLevelParent(weak->m_workbench_shell), wxTopLevelWindow);
        if (owner && owner->IsIconized()) return;
        const auto started = std::chrono::steady_clock::now();
        // Paint only after all synchronous layout/visibility changes have
        // settled. Hidden editor children must not paint an intermediate frame.
        weak->layout_workbench_history_toggle(true);
        weak->update_workbench_check_card();
        auto* viewport = weak->workbench_import_viewport();
        auto* handle = weak->m_workbench_history_toggle;
        // Finish the exposed native backgrounds before their overlapping leaf.
        // No other presentation layer may flush this subtree independently.
        present_workbench_controls(weak->m_workbench_shell, viewport);
        if (handle && handle->IsShownOnScreen()) {
            handle->Raise();
            handle->Refresh(false);
            handle->Update();
        }
        present_workbench_controls(weak->m_workbench_check_card);
        const auto controls_presented = std::chrono::steady_clock::now();
        // One complete GL frame follows the native controls in this same pass.
        // A top-level restore can then expose the window without an old frame.
        if (viewport && viewport->IsShownOnScreen()) {
            viewport->Refresh(false);
            viewport->Update();
        }
        wxString trace;
        if (wxGetEnv("ORCASLICER_UI_LATENCY_TRACE", &trace) && trace == "1")
            BOOST_LOG_TRIVIAL(info) << "Workbench surface presented: controls_ms="
                << std::chrono::duration<double, std::milli>(controls_presented - started).count()
                << ", total_ms="
                << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count()
                << ", history_open=" << weak->m_workbench_history_state.workbench_open;
    });
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
    const wxPoint position(
        viewport.x + std::max(0, (viewport.width - card_size.x) / 2),
        viewport.y + std::max(0, (viewport.height - card_size.y) / 2));
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

void ModelGenerationPanel::acknowledge_workbench_risk()
{
    if (m_workbench_check_running || m_workbench_check_result.status != WorkbenchCheckStatus::Attention ||
        !m_workbench_check_result.force_import_risk || m_workbench_check_result.model_sha256.empty()) return;
    m_workbench_force_import_ack_hash = m_workbench_check_result.model_sha256;
    m_workbench_force_import_ack_revision = m_sequence;
    m_workbench_operation_notice = "已确认开放边界风险；可点击“去切片”继续，导入后请复核逐层切片结果。";
    hide_workbench_check_card();
    refresh_post_generation_workbench();
    publish_workbench_state();
}

void ModelGenerationPanel::show_workbench_check_details()
{
    if (!m_workbench_shell) return;
    hide_workbench_check_card();
    const wxString detail_text = from_u8(workbench_check_detail_text(m_workbench_check_result,
        workbench_snapshot().force_import_acknowledged));
    std::vector<RedesignDialogAction> actions;
    if (!m_workbench_check_result.issue_faces.empty())
        actions.push_back({wxID_APPLY, _L("定位问题区域"), false});
    actions.push_back({wxID_OK, _L("关闭"), false});
    if (show_redesign_details(m_workbench_shell, detail_text, _L("检查修复详情"), actions) == wxID_APPLY &&
        m_model_preview && AI::model_artifact_sha256(m_displayed_model_path) == m_workbench_check_result.model_sha256) {
        m_workbench_editing = true;
        refresh_post_generation_workbench();
        m_model_preview->select_face_evidence(m_workbench_check_result.issue_faces);
        publish_workbench_state();
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
    } else if (m_workbench_auto_repair && m_finishing_running) phase = _L("保存修复结果");
    else phase = result.status == WorkbenchCheckStatus::Normal ? _L("检查通过") : _L("检查失败");
    bool modal_layout_changed = set_workbench_label(m_workbench_check_phase, phase);
    wxString message = native_repair
        ? _L("正在通过 Orca 原生纹理链路严格修复模型并建立固定原子色区，请稍候……")
        : honor_semantic ? m_workbench_honor_semantic_message : from_u8(result.summary);
    if (!native_repair && !honor_semantic && !m_workbench_check_running && !result.repair_reason.empty())
        message += "\n" + from_u8(result.repair_reason);
    if (honor_semantic && message.empty())
        message = _L("正在识别人像区域并匹配肤色、头发、眼白、牙齿和嘴唇，请稍候……");
    else if (!honor_semantic && m_workbench_auto_repair && m_finishing_running)
        message = _L("安全修复已通过复检，正在自动保存……");
    if (set_workbench_label(m_workbench_check_message, message)) {
        m_workbench_check_message->Wrap(FromDIP(376));
        modal_layout_changed = true;
    }
    const bool running = native_repair || honor_semantic || m_workbench_check_running ||
        (m_workbench_auto_repair && m_finishing_running);
    modal_layout_changed = set_workbench_shown(m_workbench_check_cancel_button,
        native_repair || (!honor_semantic && (m_workbench_check_running ||
            (m_workbench_auto_repair && m_finishing_running)))) || modal_layout_changed;
    const bool cancel_requested = native_repair
        ? m_native_workbench_partition_cancel && m_native_workbench_partition_cancel->load()
        : m_workbench_auto_repair && m_finishing_running
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
    for (wxWindow* control : {m_workbench_return_to_design_button, m_workbench_edit}) {
        control->SetFont(wxGetApp().bold_font());
        control->SetMinSize(FromDIP(wxSize(140, 38)));
        control->SetMaxSize(FromDIP(wxSize(140, 38)));
    }
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
            model_status = _L("检查通过"); status_tone = PostGenerationStatusTone::Success; break;
        case WorkbenchCheckOutcome::Failed:
            model_status = _L("检查失败"); status_tone = PostGenerationStatusTone::Error; break;
        }
    }
    set_workbench_label(m_workbench_model_status, model_status);
    set_workbench_tooltip(m_workbench_model_status, model_status);
    const wxColour status_colour = workbench_status_colour(status_tone);
    if (m_workbench_model_status->GetForegroundColour() != status_colour) {
        m_workbench_model_status->SetForegroundColour(status_colour);
        m_workbench_model_status->Refresh(false);
    }
    const bool show_status = m_workbench_load_error || m_preview_loading;
    layout_changed = set_workbench_shown(m_workbench_state_status, show_status) || layout_changed;
    if (show_status) {
        set_workbench_tooltip(m_workbench_state_status,
            m_workbench_load_error ? m_result_summary->GetLabel() : wxString());
        layout_changed = set_wrapped_workbench_text(m_workbench_state_status, m_status->GetLabel(),
            std::max(FromDIP(200), m_workbench_view_host->GetClientSize().x - FromDIP(16))) || layout_changed;
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
    set_workbench_enabled(m_workbench_edit, state.can_edit && !m_workbench_editing);
    if (m_workbench_project_summary) {
        const auto& filaments = snapshot.printer_filaments;
        const wxString summary = wxString::FromUTF8(filaments.printer)+
            wxString::Format(_L("\n%llu 个喷嘴 · %llu 个实体槽位"),static_cast<unsigned long long>(filaments.nozzles),
                static_cast<unsigned long long>(filaments.physical_count)) +
            (filaments.idle_nozzles ? wxString::Format(_L(" · %llu 个喷嘴空闲"), static_cast<unsigned long long>(filaments.idle_nozzles)) : wxString{}) +
            (model_unavailable || snapshot.apply_filaments_reason.empty()
                ? wxString{} : "\n" + wxString::FromUTF8(snapshot.apply_filaments_reason));
        layout_changed = set_wrapped_workbench_text(m_workbench_project_summary, summary,
            FromDIP(235), true) || layout_changed;
    }
    set_workbench_enabled(m_workbench_configure_filaments, snapshot.can_edit_project_colors);
    set_workbench_enabled(m_workbench_add_filament, snapshot.can_edit_project_colors &&
        wxGetApp().preset_bundle && wxGetApp().preset_bundle->filament_presets.size() < 32 &&
        snapshot.printer_filaments.physical_count < snapshot.printer_filaments.physical_capacity);
    const bool device = wxGetApp().plater() && wxGetApp().plater()->get_machine_sync_status();
    set_workbench_enabled(m_workbench_sync_filaments, snapshot.can_edit_project_colors && device);
    set_workbench_tooltip(m_workbench_sync_filaments,
        device ? _L("使用原生设备耗材同步流程") : _L("当前没有可同步的同型号设备；可离线配置耗材"));
    if (m_workbench_return_to_design_button)
        layout_changed |= set_workbench_label(m_workbench_return_to_design_button,
            m_shell_smart_creation ? _L("返回AI创作") : _L("返回图像设计"));
    if (m_workbench_return_to_design_button)
        set_workbench_enabled(m_workbench_return_to_design_button,
            post_generation_return_to_design_allowed(state));
    const bool check_processing = m_native_workbench_partition_running || m_workbench_check_running ||
        (m_workbench_auto_repair && m_finishing_running);
    const bool topology_normal = !check_processing && snapshot.check.status == WorkbenchCheckStatus::Normal &&
        snapshot.check.candidate_status != RepairCandidateStatus::Ready && snapshot.candidate_path.empty() && !snapshot.dirty;
    layout_changed = set_workbench_label(m_workbench_check,
        m_workbench_check_running ? _L("取消") : topology_normal ? _L("通过") : _L("检查")) || layout_changed;
    wxColour check_colour(190, 190, 196);
    if (check_processing || snapshot.check.candidate_status == RepairCandidateStatus::Ready ||
        snapshot.check.status == WorkbenchCheckStatus::Running ||
        snapshot.check.status == WorkbenchCheckStatus::Attention)
        check_colour = wxColour(255, 194, 39);
    else if (snapshot.check.status == WorkbenchCheckStatus::Normal)
        check_colour = wxColour(91, 205, 137);
    else if (snapshot.check.status == WorkbenchCheckStatus::Invalid ||
             snapshot.check.status == WorkbenchCheckStatus::Failed)
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
    if (m_workbench_printer_filament_trial) {
        const auto* honor_suite = m_model_preview ? m_model_preview->active_honor_filament_suite() : nullptr;
        const auto* bundle = wxGetApp().preset_bundle;
        const auto suite_compatibility = honor_suite && bundle
            ? PrinterFilamentConfig::honor_suite_compatibility(*bundle, *honor_suite)
            : PrinterFilamentConfig::HonorSuiteCompatibility{};
        const auto card = m_model_preview
            ? m_model_preview->model_color_pack_for_filament_import() : std::nullopt;
        const bool has_card = card && card->valid_for_workbench_import();
        const bool suite_allowed = !honor_suite || suite_compatibility.compatible;
        const bool can_trial = workbench_printer_trial_action_allowed(
            m_printer_filament_trial_active, m_apply_filaments_saving,
            snapshot.can_edit_project_colors, has_card) &&
            (m_printer_filament_trial_active || suite_allowed);
        set_workbench_enabled(m_workbench_printer_filament_trial, can_trial);
        layout_changed = set_workbench_label(m_workbench_printer_filament_trial,
            m_printer_filament_trial_active ? _L("退出打印机耗材试色") : _L("将模型配色导入到耗材")) || layout_changed;
        const wxString disabled_reason = model_unavailable ? model_hint : honor_suite && !suite_allowed
            ? wxString::FromUTF8(suite_compatibility.reason.empty()
                ? "当前打印机不支持六实体色荣耀耗材套装" : suite_compatibility.reason)
            : !has_card
            ? _L("当前没有 1 至 32 色的有效色卡，请先生成 AI配色或完成试色。")
            : !snapshot.can_edit_project_colors
                ? _L("模型处理中或存在未接受候选，请先完成当前操作。") : wxString{};
        set_workbench_tooltip(m_workbench_printer_filament_trial, m_printer_filament_trial_active
            ? _L("退出试色并恢复进入前的模型外观；已提交的耗材配置保留。")
            : can_trial ? honor_suite
                ? _L("按套装顺序绑定六个真实实体耗材；不生成叠色配方。")
                : _L("将模型配色直接生成实体耗材与叠色配方；成功后一次提交并进入试色。")
                        : disabled_reason);
        if (m_workbench_printer_filament_feedback) {
            const wxString feedback = model_unavailable ? wxString{} : !m_workbench_printer_filament_notice.empty()
                ? wxString::FromUTF8(m_workbench_printer_filament_notice) : disabled_reason;
            const bool feedback_error = !m_workbench_printer_filament_notice.empty()
                ? m_workbench_printer_filament_notice_error : false;
            const wxColour color = feedback_error ? wxColour(255, 150, 146) :
                (!m_workbench_printer_filament_notice.empty() ? wxColour(91, 205, 137) : wxColour(190, 190, 196));
            if (m_workbench_printer_filament_feedback->GetForegroundColour() != color) {
                m_workbench_printer_filament_feedback->SetForegroundColour(color);
                m_workbench_printer_filament_feedback->Refresh(false);
            }
            layout_changed = set_wrapped_workbench_text(m_workbench_printer_filament_feedback,
                feedback, FromDIP(235), true) || layout_changed;
            layout_changed = set_workbench_shown(m_workbench_printer_filament_feedback,
                !feedback.empty()) || layout_changed;
        }
    }
    set_workbench_enabled(m_workbench_palette_details,
        state.can_edit && m_finishing_candidate.empty() && original_appearance_available);
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
            ? _L("预览 3 mm 圆形底座；导入切片时与当前模型一并生成")
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
    const bool can_import = (snapshot.can_import_for_slicing || snapshot.can_force_import) && is_nonempty_model(m_displayed_model_path);
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
    wxString check_text = m_native_workbench_partition_running ? _L("正在修复模型……") :
        m_workbench_auto_repair && m_finishing_running ? _L("正在保存修复结果……") :
        from_u8(workbench_check_status_line(snapshot.check, snapshot.force_import_acknowledged));
    const wxString check_details = from_u8(workbench_check_detail_text(
        snapshot.check, snapshot.force_import_acknowledged));
    set_workbench_tooltip(m_workbench_check_status, check_details);
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
    const wxString palette_status = from_u8(workbench_palette_status_line(
        appearance_state, honor_suite_active,
        m_model_preview->honor_suite_used_color_count(), trial.colors.size(),
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
        m_workbench_settings_scroll->Layout();
        m_comparison_panel->Layout();
        m_workbench_view_host->Layout();
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
