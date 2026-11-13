#include "RedesignShell.hpp"
#include "InlineModelGenerationView.hpp"
#include "GenerationCompletionToast.hpp"
#include "AdvancedCreationWorkspace.hpp"
#include "../AI/ModelGeneration/ModelGenerationFeatureHost.hpp"
#include "../AI/ModelGeneration/AdvancedCreationFeatureHost.hpp"
#include <wx/sizer.h>

namespace Slic3r::GUI {

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
    if (m_model_generation_host)
        m_model_generation_host->set_smart_creation_presented(m_smart_creation && m_active_page == Page::Image &&
            !m_advanced_mode && !m_page_transition.active() && !m_workbench_transition && !m_history_open_request);
    if (!m_smart_generation_view) return;
    const bool content = m_smart_generation_view->has_content();
    const bool complete = content && m_smart_generation_view->ready();
    m_smart_generation_view->Show(content);
    if (content) { m_guide_panel->Hide(); m_preview_host->Hide(); }
    if (m_smart_completion_actions) {
        m_smart_completion_actions->Show(complete);
        m_smart_completion_actions->set_enabled(m_generate_button->IsEnabled(),
            m_model_generation_state.can_open_workbench && !m_page_transition.active());
        m_generate_button->Show(!complete);
        if (m_smart_action_bottom_padding) m_smart_action_bottom_padding->Show(!complete);
        if (complete) m_secondary_action_button->Hide();
        m_smart_completion_actions->GetParent()->Layout();
    }
    if (m_image_page) m_image_page->Layout();
    m_smart_generation_view->set_presented(content && m_active_page == Page::Image && !m_advanced_mode &&
        !m_workbench_transition);
    if (m_advanced_workspace)
        m_advanced_workspace->set_presented(m_active_page == Page::Image && m_advanced_mode && !m_workbench_transition);
    if (m_generation_toast && (m_active_page != Page::Image || m_page_transition.active()))
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
    if (!owns_model_workflow() || !m_model_generation_host || m_page_transition.active()) return;
    const auto request = source == CreationSource::Smart ?
        (m_smart_generation_view ? m_smart_generation_view->displayed_artifact() : std::nullopt) :
        (m_advanced_workspace ? m_advanced_workspace->displayed_artifact() : std::nullopt);
    if (!request) return;
    const auto& state = source == CreationSource::Smart ? m_model_generation_state : m_advanced_model_state;
    if (state.busy || !state.can_open_workbench) return;
    if (request->entry.job_id != state.model_asset_id) return;
    const auto current = m_model_generation_host->workbench_snapshot();
    m_pending_workbench_return_advanced = source == CreationSource::Advanced;
    bool handled = false;
    // Existing edits belong to the workbench. Reopening the same generated
    // asset restores that editing session rather than reloading its original.
    if (current.asset_id == request->entry.job_id && !current.model_path.empty())
        handled = open_model_workbench();
    else handled = m_model_generation_host->request_open_workbench_artifact(*request);
    if (!handled) m_pending_workbench_return_advanced.reset();
}

void RedesignShell::show_generation_completion()
{
    if (m_active_page != Page::Image || m_page_transition.active()) return;
    if (!m_generation_toast) m_generation_toast = new GenerationCompletionToast(this);
    m_generation_toast->present();
}
} // namespace Slic3r::GUI
