#include "RedesignShell.hpp"
#include "InlineModelGenerationView.hpp"
#include "GenerationCompletionToast.hpp"
#include "AdvancedCreationWorkspace.hpp"
#include "../AI/ModelGeneration/ModelGenerationFeatureHost.hpp"
#include "../AI/ModelGeneration/AdvancedCreationFeatureHost.hpp"
#include <wx/sizer.h>
#include <wx/weakref.h>

namespace Slic3r::GUI {

void RedesignShell::refresh_creation_workflow()
{
    m_image_page_view_only = m_page_coordinator.page() == Page::Image && m_model_generation_host &&
        m_model_generation_host->creation_editing_locked();
    apply_model_generation_state(m_model_generation_state);
    if (m_advanced_workspace) m_advanced_workspace->render();
}

void RedesignShell::on_advanced_model_job(const AdvancedCreation::ModelJobEvent& event)
{
    if (m_presentation_stopping || !m_advanced_host || !m_advanced_host->session().matches(event.context)) return;
    using Phase = AdvancedCreation::ModelJobEvent::Phase;
    if (event.phase == Phase::Requested) {
        const auto& group = m_advanced_host->session().group();
        m_inline_model_request = group && event.job_id != group->model_job_id ?
            std::optional<AdvancedCreation::ModelJobEvent>(event) : std::nullopt;
        return;
    }
    if (!m_inline_model_request || !event.same_request(*m_inline_model_request)) return;
    m_inline_model_request.reset();
    // Acceptance is published before the monitor observes the job. Wait for its
    // current snapshot; task notifications never change the visible route.
    wxWeakRef<RedesignShell> weak(this);
    Presentation::post_gui([weak, event] {
        if (!weak || weak->m_presentation_stopping || !weak->m_advanced_host ||
            !weak->m_advanced_host->session().matches(event.context)) return;
        const auto& state = weak->m_advanced_host->model_state();
        if (state.job_id == event.job_id) weak->apply_advanced_model_state(state, true);
    });
}

void RedesignShell::apply_advanced_model_state(const ModelGenerationUIState& state, bool new_generation)
{
    if (state.revision < m_advanced_model_state.revision ||
        state.model_generation_session < m_advanced_model_state.model_generation_session) return;
    m_advanced_model_state = state;
    if (m_advanced_workspace) m_advanced_workspace->apply_model_state(state, new_generation);
}

void RedesignShell::apply_smart_inline_generation()
{
    if (!m_smart_generation_view) return;
    m_smart_generation_view->apply_snapshot({CreationSource::Smart,"",0}, m_model_generation_state,
        m_model_generation_host ? m_model_generation_host->snapshot_generated_artifact() : std::nullopt);
}

void RedesignShell::refresh_inline_generation()
{
    const bool presenting_workbench = m_workbench_transition && !m_workbench_presentation_detached;
    if (m_model_generation_host)
        m_model_generation_host->set_smart_creation_presented(m_smart_creation && m_page_coordinator.page() == Page::Image &&
            !m_page_coordinator.advanced() && !m_page_coordinator.transition().active() && !presenting_workbench);
    if (!m_smart_generation_view) return;
    const bool content = m_smart_generation_view->has_content();
    const bool complete = content && m_smart_generation_view->ready();
    if (m_smart_creation && m_generate_button) {
        const bool enabled = m_image_page_view_only || (m_model_generation_state.can_click_generate_smart &&
            generation_input_editable() && !m_prompt_polish_state.active() && !m_smart_generation_view->loading());
        if (m_generate_button->IsEnabled() != enabled) m_generate_button->Enable(enabled);
    }
    bool layout = false;
    const auto show = [&layout](wxWindow* window, bool visible) {
        if (window && window->IsShown() != visible) { window->Show(visible); layout = true; }
    };
    show(m_smart_generation_view, content);
    if (content) { show(m_guide_panel, false); show(m_preview_host, false); }
    if (m_smart_completion_actions) {
        const bool readonly = m_model_generation_host && m_model_generation_host->creation_editing_locked();
        m_smart_completion_actions->set_read_only(readonly);
        show(m_smart_completion_actions, complete);
        m_smart_completion_actions->set_enabled(readonly ? can_return_to_image_design() : m_generate_button->IsEnabled(),
            readonly || (m_model_generation_state.can_open_workbench && !m_page_coordinator.transition().active()));
        show(m_generate_button, !complete);
        if (m_smart_action_bottom_padding && m_smart_action_bottom_padding->IsShown() != !complete) {
            m_smart_action_bottom_padding->Show(!complete); layout = true;
        }
        if (complete) show(m_secondary_action_button, false);
        if (layout) m_smart_completion_actions->GetParent()->Layout();
    }
    if (layout && m_image_page) m_image_page->Layout();
    m_smart_generation_view->set_presented(content && m_page_coordinator.page() == Page::Image && !m_page_coordinator.advanced() &&
        !presenting_workbench);
    if (m_advanced_workspace)
        m_advanced_workspace->set_presented(m_page_coordinator.page() == Page::Image && m_page_coordinator.advanced() && !presenting_workbench);
    if (m_generation_toast && (m_page_coordinator.page() != Page::Image || m_page_coordinator.transition().active()))
        m_generation_toast->dismiss();
}

void RedesignShell::recover_smart_generation()
{
    if (!m_model_generation_host) return;
    const auto& state = m_model_generation_state;
    bool handled = false;
    if (state.can_retry_model) handled = m_model_generation_host->request_retry_model();
    else if (state.can_retry_service) handled = m_model_generation_host->request_retry_service();
    else if (state.can_restore_latest) handled = m_model_generation_host->request_restore_latest();
    if (handled) apply_model_generation_state(m_model_generation_host->snapshot());
}

void RedesignShell::open_creation_artifact(CreationSource source)
{
    if (m_model_generation_host && m_model_generation_host->creation_editing_locked()) {
        navigate_to_workbench();
        return;
    }
    if (!owns_model_workflow() || !m_model_generation_host || m_page_coordinator.transition().active()) return;
    const auto request = source == CreationSource::Smart ?
        (m_smart_generation_view ? m_smart_generation_view->displayed_artifact() : std::nullopt) :
        (m_advanced_workspace ? m_advanced_workspace->displayed_artifact() : std::nullopt);
    if (!request) return;
    const auto& state = source == CreationSource::Smart ? m_model_generation_state : m_advanced_model_state;
    if (state.busy || !state.can_open_workbench) return;
    if (request->entry.job_id != state.model_asset_id) return;
    const auto current = m_model_generation_host->workbench_snapshot();
    // Existing edits belong to the workbench. Reopening the same generated
    // asset restores that editing session rather than reloading its original.
    if (current.asset_id == request->entry.job_id && !current.model_path.empty()) {
        if (source == CreationSource::Smart) {
            m_saved_creation_input = current_generation_input();
            m_saved_creation_options = current_generation_options();
        }
        m_model_generation_host->commit_current_creation(*request);
        open_model_workbench();
    } else m_model_generation_host->request_open_workbench_artifact(*request);
}

void RedesignShell::show_generation_completion()
{
    if (m_page_coordinator.page() != Page::Image || m_page_coordinator.transition().active()) return;
    if (!m_generation_toast) m_generation_toast = new GenerationCompletionToast(this);
    m_generation_toast->present();
}

void RedesignShell::show_generation_error(const std::string& reason)
{
    if (reason.empty()) { if (m_generation_toast) m_generation_toast->dismiss(); return; }
    if (m_page_coordinator.page() != Page::Image || m_page_coordinator.transition().active() || m_presentation_stopping) return;
    if (!m_generation_toast) m_generation_toast = new GenerationCompletionToast(this);
    m_generation_toast->present_error(wxGetTranslation(wxString::FromUTF8(generation_rejection_message(reason))));
}
} // namespace Slic3r::GUI
