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

void repaint_workbench_surface(wxWindow* window)
{
    if (!window || !window->IsShownOnScreen() || window->IsFrozen()) return;
    window->Refresh();
    window->Update();
    for (wxWindow* child : window->GetChildren())
        repaint_workbench_surface(child);
}
}

void ModelGenerationPanel::position_workbench_check_card()
{
    if (!m_workbench_check_card || !m_model_preview || !m_workbench_check_card->IsShown()) return;
    auto* host = m_model_preview->workbench_overlay_parent();
    if (!host || !host->IsShownOnScreen()) return;
    const int viewport_top = m_model_preview->workbench_viewport_top();
    const wxSize host_size = host->GetClientSize();
    const wxRect viewport(host->ClientToScreen(wxPoint(0, viewport_top)),
        wxSize(host_size.x, std::max(0, host_size.y - viewport_top)));
    const wxSize card_size = m_workbench_check_card->GetSize();
    m_workbench_check_card->Move(
        viewport.x + std::max(0, (viewport.width - card_size.x) / 2),
        viewport.y + std::max(0, (viewport.height - card_size.y) / 2));
    raise_workbench_check_card();
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
    position_workbench_check_card();
    event.Skip();
}

void ModelGenerationPanel::hide_workbench_check_card()
{
    m_workbench_check_card_visible = false;
    if (m_workbench_honor_semantic_active && m_finishing_workbench && m_workbench_presented) {
        update_workbench_check_card();
        return;
    }
    remove_workbench_card_layer_guard();
    m_workbench_floating_card_mode = WorkbenchFloatingCardMode::None;
    if (m_workbench_check_progress) m_workbench_check_progress->set_running(false);
    if (m_workbench_check_progress) m_workbench_check_progress->set_progress(std::nullopt);
    if (m_model_preview) m_model_preview->set_workbench_input_locked(false);
    if (m_workbench_shell && !m_workbench_shell->IsEnabled()) m_workbench_shell->Enable();
    if (m_workbench_check_card && m_workbench_check_card->IsShown())
        m_workbench_check_card->Hide();
}

void ModelGenerationPanel::cancel_workbench_check()
{
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
    m_workbench_operation_notice = "已忽略轻微拓扑风险；后续可正常导入切片。";
    hide_workbench_check_card();
    refresh_post_generation_workbench();
    publish_workbench_state();
}

void ModelGenerationPanel::show_workbench_check_details()
{
    if (!m_workbench_shell) return;
    wxDialog dialog(wxGetTopLevelParent(m_workbench_shell), wxID_ANY, _L("检查修复详情"),
        wxDefaultPosition, FromDIP(wxSize(560, 480)), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    dialog.SetName("ai_content_color");
    dialog.SetBackgroundColour(wxColour(32, 32, 35));
    dialog.SetForegroundColour(wxColour(238, 240, 244));
    auto* root = new wxBoxSizer(wxVERTICAL);
    const wxString detail_text = from_u8(workbench_check_detail_text(m_workbench_check_result,
        workbench_snapshot().force_import_acknowledged));
    auto* details = new wxTextCtrl(&dialog, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxDefaultSize,
        wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2 | wxBORDER_NONE);
    details->SetName("ai_content_color");
    const wxColour detail_background(40, 40, 43);
    const wxColour detail_foreground(238, 240, 244);
    details->SetBackgroundColour(detail_background);
    details->SetForegroundColour(detail_foreground);
    details->SetFont(m_workbench_shell->GetFont());
    details->SetDefaultStyle(wxTextAttr(detail_foreground, detail_background));
    details->ChangeValue(detail_text);
    details->SetStyle(0, details->GetLastPosition(), wxTextAttr(detail_foreground, detail_background));
    root->Add(details, 1, wxEXPAND | wxALL, FromDIP(12));
    auto* close = new WorkbenchButton(&dialog, _L("关闭"));
    close->SetMinSize(FromDIP(wxSize(92, 32)));
    close->Bind(wxEVT_BUTTON, [&dialog](wxCommandEvent&) { dialog.EndModal(wxID_OK); });
    root->Add(close, 0, wxALIGN_RIGHT | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    dialog.SetSizer(root);
    dialog.CenterOnParent();
    dialog.ShowModal();
}

void ModelGenerationPanel::update_workbench_check_card()
{
    if (!m_workbench_check_card || !m_workbench_check_modal) return;
    const auto& result = m_workbench_check_result;
    const auto mode = workbench_floating_card_mode(m_finishing_workbench, m_workbench_presented,
        m_workbench_honor_semantic_active, m_workbench_check_card_visible,
        m_workbench_check_running, m_workbench_auto_repair && m_finishing_running, result);
    if (mode == WorkbenchFloatingCardMode::None || !m_workbench_shell ||
        !m_workbench_shell->IsShownOnScreen()) {
        hide_workbench_check_card();
        return;
    }
    m_workbench_floating_card_mode = mode;
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
    if (!m_workbench_check_card->IsShown()) {
        apply_workbench_floating_card_shape(m_workbench_check_card, 8, wxColour(45, 45, 49));
        m_workbench_check_card->Show();
    }
    install_workbench_card_layer_guard();
    wxString phase;
    if (honor_semantic) phase = _L("正在应用荣耀耗材套装");
    else if (m_workbench_check_running) {
        switch (result.phase) {
        case WorkbenchCheckPhase::Repairing: phase = _L("修复模型"); break;
        case WorkbenchCheckPhase::Rechecking: phase = _L("复检模型"); break;
        default: phase = _L("检查模型"); break;
        }
    } else if (m_workbench_auto_repair && m_finishing_running) phase = _L("保存修复结果");
    else phase = result.status == WorkbenchCheckStatus::Attention ? _L("检查完成") : _L("检查未通过");
    bool modal_layout_changed = set_workbench_label(m_workbench_check_phase, phase);
    wxString message = honor_semantic
        ? m_workbench_honor_semantic_message : from_u8(result.summary);
    if (honor_semantic && message.empty())
        message = _L("正在识别人像区域并匹配肤色、头发、眼白、牙齿和嘴唇，请稍候……");
    else if (!honor_semantic && m_workbench_auto_repair && m_finishing_running)
        message = _L("安全修复已通过复检，正在自动接受并保存……");
    else if (!honor_semantic && !m_workbench_check_running &&
             result.status == WorkbenchCheckStatus::Attention && result.force_import_risk)
        message += _L("\n这是轻微开放边界风险；可忽略后继续导入切片，也可以返回 2D 设计图。\n导入许可仅绑定当前模型版本。")
            ;
    else if (!honor_semantic && !m_workbench_check_running &&
             result.status != WorkbenchCheckStatus::Attention)
        message += _L("\n当前结果不能强制导入，只能返回 2D 设计图。")
            ;
    if (set_workbench_label(m_workbench_check_message, message)) {
        m_workbench_check_message->Wrap(FromDIP(376));
        modal_layout_changed = true;
    }
    const bool running = honor_semantic || m_workbench_check_running ||
        (m_workbench_auto_repair && m_finishing_running);
    modal_layout_changed = set_workbench_shown(m_workbench_check_cancel_button,
        !honor_semantic && m_workbench_check_running) || modal_layout_changed;
    set_workbench_enabled(m_workbench_check_cancel_button, m_workbench_check_running &&
        (!m_workbench_check_cancel || !m_workbench_check_cancel->load()));
    const bool attention = !honor_semantic && !m_workbench_check_running &&
        result.status == WorkbenchCheckStatus::Attention &&
        result.force_import_risk;
    modal_layout_changed = set_workbench_shown(m_workbench_check_continue_button, attention) || modal_layout_changed;
    modal_layout_changed = set_workbench_shown(m_workbench_check_return_button,
        !honor_semantic && !running && !attention) || modal_layout_changed;
    modal_layout_changed = set_workbench_shown(m_workbench_check_progress, running) || modal_layout_changed;
    m_workbench_check_progress->set_progress(honor_semantic && m_workbench_honor_semantic_progress > 0
        ? std::optional<int>(m_workbench_honor_semantic_progress) : std::nullopt);
    m_workbench_check_progress->set_running(running);
    if (modal_layout_changed) {
        m_workbench_check_modal->Layout();
        m_workbench_check_card->Fit();
        apply_workbench_floating_card_shape(m_workbench_check_card, 8, wxColour(45, 45, 49));
    }
    position_workbench_check_card();
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
    bool layout_needed = force;
    auto* overlay_host = m_model_preview->workbench_overlay_parent();
    const auto host_size = overlay_host->GetClientSize();
    const int viewport_top = m_model_preview->workbench_viewport_top();
    const int viewport_height = std::max(0, host_size.y - viewport_top);
    const wxSize toggle_size = FromDIP(wxSize(19, 98));
    layout_needed = set_workbench_shown(m_workbench_history_toggle, m_finishing_workbench) || layout_needed;
    const wxRect toggle_rect(std::max(0, host_size.x - toggle_size.x),
        viewport_top + std::max(0, (viewport_height - toggle_size.y) / 2), toggle_size.x, toggle_size.y);
    if (m_workbench_history_toggle->GetRect() != toggle_rect) {
        m_workbench_history_toggle->SetSize(toggle_rect);
        layout_needed = true;
    }
    if (layout_needed) m_workbench_history_toggle->Raise();
}

void ModelGenerationPanel::refresh_post_generation_workbench()
{
    if (!m_workbench_shell || !m_finishing_workbench || m_refreshing_workbench_layout) return;
    // Layout sends synchronous size events back to this refresh function.
    m_refreshing_workbench_layout = true;
    bool layout_changed = false;
    const auto state = post_generation_ui_state();
    update_workbench_check_card();
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
    set_workbench_label(m_workbench_model_status, model_status);
    set_workbench_tooltip(m_workbench_model_status, model_status);
    const wxColour status_colour = workbench_status_colour(post_generation_status_tone(
        state.status, m_model_preview && m_model_preview->original_comparison_enabled()));
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
    if (!m_workbench_import_feedback)
        layout_changed = set_workbench_shown(m_workbench_history_panel,
            !m_workbench_editing && !m_workbench_history_collapsed) || layout_changed;
    const wxString history_action = m_workbench_editing ? _L("返回历史模型") :
        m_workbench_history_panel->IsShown() ? _L("收起历史模型") : _L("展开历史模型");
    if (m_workbench_history_toggle->GetName() != history_action)
        m_workbench_history_toggle->SetName(history_action);
    set_workbench_tooltip(m_workbench_history_toggle, history_action);
    static_cast<WorkbenchButton*>(m_workbench_history_toggle)->set_drawer_open(
        m_workbench_editing || m_workbench_history_panel->IsShown());
    set_workbench_enabled(m_workbench_history_toggle, !m_workbench_import_feedback);
    set_workbench_enabled(m_workbench_edit, state.can_edit && !m_workbench_editing);
    const auto snapshot = workbench_snapshot();
    if (m_workbench_project_summary) {
        const auto& filaments = snapshot.printer_filaments;
        const wxString summary = wxString::FromUTF8(filaments.printer)+
            wxString::Format(_L("\n%llu 个喷嘴 · %llu 个实体槽位"),static_cast<unsigned long long>(filaments.nozzles),
                static_cast<unsigned long long>(filaments.physical_count)) +
            (filaments.idle_nozzles ? wxString::Format(_L(" · %llu 个喷嘴空闲"), static_cast<unsigned long long>(filaments.idle_nozzles)) : wxString{}) +
            (snapshot.apply_filaments_reason.empty() ? wxString{} : "\n" + wxString::FromUTF8(snapshot.apply_filaments_reason));
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
        set_workbench_enabled(m_workbench_return_to_design_button,
            post_generation_return_to_design_allowed(state));
    const bool topology_normal = snapshot.check.status == WorkbenchCheckStatus::Normal &&
        snapshot.check.candidate_status != RepairCandidateStatus::Ready && snapshot.candidate_path.empty() && !snapshot.dirty;
    layout_changed = set_workbench_label(m_workbench_check,
        m_workbench_check_running ? _L("取消") : topology_normal ? _L("正常") : _L("检查")) || layout_changed;
    wxColour check_colour(190, 190, 196);
    if (snapshot.check.candidate_status == RepairCandidateStatus::Ready ||
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
        m_workbench_check_running || (state.can_edit && m_finishing_candidate.empty() && !snapshot.dirty));
    for (wxWindow* color : m_workbench_project_colors->GetChildren())
        set_workbench_enabled(color, snapshot.can_edit_project_colors);
    if (m_workbench_apply_project_filaments) {
        const bool can_map = snapshot.can_apply_printer_filaments && !m_apply_filaments_saving;
        set_workbench_enabled(m_workbench_apply_project_filaments, can_map);
        set_workbench_tooltip(m_workbench_apply_project_filaments, can_map
            ? _L("以当前实体耗材与叠色槽为完整逻辑色卡，重建模型配色并自动保存新版本。")
            : wxString::FromUTF8(snapshot.apply_filaments_reason.empty()
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
        const wxString disabled_reason = honor_suite && !suite_allowed
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
            const wxString feedback = !m_workbench_printer_filament_notice.empty()
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
    const bool original_appearance_available = m_model_preview->original_comparison_available();
    const wxString original_appearance_reason = original_appearance_available
        ? wxString{} : from_u8(m_model_preview->original_appearance_error().empty()
            ? "原始配色证据不可用" : m_model_preview->original_appearance_error());
    m_model_preview->set_workbench_original_appearance_error(original_appearance_reason);
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
        const bool comparison_available = m_model_preview->original_comparison_available();
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
    set_workbench_enabled(m_workbench_print, false);
    const bool can_import = (snapshot.can_import_for_slicing || snapshot.can_force_import) && is_nonempty_model(m_displayed_model_path);
    if (auto* portrait = FindWindowByName("portrait_r6_entry", m_workbench_shell)) {
        set_workbench_enabled(portrait, snapshot.actions.can_edit && snapshot.portrait_available);
        layout_changed = set_workbench_label(portrait,
            snapshot.portrait_enabled ? _L("退出人像保护（R6）") : _L("人像保护（R6）")) || layout_changed;
        set_workbench_tooltip(portrait, snapshot.portrait_available
            ? _L("可选人像保护；退出保留已保存的保护边界。")
            : wxString::FromUTF8(snapshot.portrait_unavailable_reason));
    }
    if (m_workbench_print_navigation)
        set_workbench_enabled(m_workbench_print_navigation, can_import);
    set_workbench_enabled(m_workbench_slicing, can_import);
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
            layout_changed = set_workbench_label(m_workbench_slice_values[index++], value) || layout_changed;
        }
    }
    wxString estimated_time = _L("未切片");
    wxString estimated_filament = _L("未切片");
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
                    estimated_time = wxString::FromUTF8(statistics.estimated_normal_print_time);
                if (statistics.total_used_filament > 0.0)
                    estimated_filament = wxString::Format("%.1f m / %.0f g",
                        statistics.total_used_filament / 1000.0, statistics.total_weight);
            }
        }
    }
    layout_changed = set_workbench_label(m_workbench_slice_values[5], estimated_time) || layout_changed;
    layout_changed = set_workbench_label(m_workbench_slice_values[6], estimated_filament) || layout_changed;
    set_workbench_tooltip(m_workbench_check, topology_normal
        ? _L("拓扑正常；点击重新检查") : _L("检查拓扑并生成临时修复候选；原模型保留，接受前不会覆盖。"));
    const wxString check_text = from_u8(workbench_check_status_line(
        snapshot.check, snapshot.force_import_acknowledged));
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
        original_appearance_available, honor_suite_active,
        m_model_preview->honor_suite_used_color_count(), trial.colors.size(),
        into_u8(trial.honor_suite_fallback_reason)));
    const wxColour palette_status_color = original_appearance_available
        ? wxColour(170, 170, 176) : wxColour(255, 150, 146);
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
    if (layout_changed) {
        m_workbench_settings_scroll->Layout();
        m_comparison_panel->Layout();
        m_workbench_view_host->Layout();
        m_workbench_shell->Layout();
        static_cast<wxScrolledWindow*>(m_workbench_shell)->FitInside();
        layout_workbench_history_toggle(true);
    } else layout_workbench_history_toggle();
    if (!history_was_shown && m_workbench_history_scroller->IsShownOnScreen())
        request_library_thumbnails(true);
    publish_workbench_state();
    m_refreshing_workbench_layout = false;
}
} // namespace Slic3r::GUI
