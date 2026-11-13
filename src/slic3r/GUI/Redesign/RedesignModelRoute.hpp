#pragma once

#include "../AI/ModelGeneration/ModelGenerationHost.hpp"
#include "../AI/ModelGeneration/AdvancedCreationTypes.hpp"
#include "../AI/ModelGeneration/ModelGenerationStatusText.hpp"
#include "../AI/ModelGeneration/PostGenerationUiState.hpp"

#include <optional>

namespace Slic3r::GUI {

enum class RedesignModelRouteAction { Ignore, Refresh, Model, Image };

enum class RedesignPrimaryAction { Design, Model };

inline const ModelGenerationUIState& selected_model_presentation(bool advanced,
    const ModelGenerationUIState& smart, const ModelGenerationUIState& four_view)
{ return advanced ? four_view : smart; }

inline bool workbench_return_advanced(bool from_editor, std::optional<bool> anchor, bool task_advanced)
{ return from_editor && anchor ? *anchor : task_advanced; }
// Shell-owned foreground intent only; task state remains in Advanced Session.
class AdvancedModelRouteIntent {
public:
    void request(const AdvancedCreation::ModelJobEvent& event, std::uint64_t navigation) {
        if (event.phase != AdvancedCreation::ModelJobEvent::Phase::Requested || event.job_id.empty()) return;
        m_request = event; m_navigation = navigation; m_accepted = false;
    }
    void accept(const AdvancedCreation::ModelJobEvent& event) {
        if (event.phase == AdvancedCreation::ModelJobEvent::Phase::Accepted && m_request &&
            m_request->same_request(event)) m_accepted = true;
    }
    void cancel() { m_request.reset(); m_accepted = false; }
    bool pending() const { return m_request.has_value(); }
    bool ready(const AdvancedCreation::CallbackContext& current, const std::string& current_job,
               const ModelGenerationUIState& projection, std::uint64_t navigation,
               bool foreground, bool blocked) const {
        return m_request && m_accepted && foreground && !blocked && navigation == m_navigation &&
            current.epoch == m_request->context.epoch && current.group_id == m_request->context.group_id &&
            current_job == m_request->job_id && projection.job_id == m_request->job_id &&
            projection.model_generation_context;
    }
private:
    std::optional<AdvancedCreation::ModelJobEvent> m_request;
    std::uint64_t m_navigation {0};
    bool m_accepted {false};
};

class PreviewAutoFitState
{
public:
    void request() { m_pending = true; }

    bool consume(unsigned int result_id, bool preview_loaded, bool paths_ready, bool viewport_ready)
    {
        if (!m_pending || !preview_loaded || !paths_ready || !viewport_ready)
            return false;

        m_pending = false;
        if (m_last_result_id && *m_last_result_id == result_id)
            return false;

        m_last_result_id = result_id;
        return true;
    }

    bool pending() const { return m_pending; }
    const std::optional<unsigned int>& last_result_id() const { return m_last_result_id; }

private:
    bool m_pending {false};
    std::optional<unsigned int> m_last_result_id;
};

inline RedesignPrimaryAction redesign_primary_action(const ModelGenerationUIState& state,
    ModelGenerationUIStage effective_stage)
{
    // A changed 2D input always wins.  A changed 3D option can reuse the
    // confirmed design image and only fork a new model task from ModelReady.
    // Persisting a 3D option is a transient stage; keep the geometry controls
    // active so their provider catalog callback cannot be replaced by the
    // image-provider picker while the save is in flight.
    if (effective_stage == ModelGenerationUIStage::Saving3DOptions && state.design_ready &&
        state.inputs_match_job)
        return RedesignPrimaryAction::Model;
    if (effective_stage == ModelGenerationUIStage::DesignReady && state.inputs_match_job)
        return RedesignPrimaryAction::Model;
    if (effective_stage == ModelGenerationUIStage::ModelReady && state.design_ready && state.inputs_match_job &&
        !state.model_options_match)
        return RedesignPrimaryAction::Model;
    return RedesignPrimaryAction::Design;
}

inline bool workbench_mode_switch_allowed(bool importing, bool native_slicing, bool official_slicing, bool editing,
    bool analyzing = false)
{
    return !importing && !native_slicing && !official_slicing && !editing && !analyzing;
}

inline bool model_generation_return_to_design_allowed(const ModelGenerationUIState& state,
    const PostGenerationUiState& workbench, bool shell_operation_in_progress)
{
    if (shell_operation_in_progress || state.busy || !post_generation_return_to_design_allowed(workbench))
        return false;

    switch (state.stage) {
    case ModelGenerationUIStage::Saving3DOptions:
    case ModelGenerationUIStage::GeneratingDesign:
    case ModelGenerationUIStage::Stopping:
    case ModelGenerationUIStage::GeneratingModel:
    case ModelGenerationUIStage::LoadingModel:
        return false;
    case ModelGenerationUIStage::Input:
    case ModelGenerationUIStage::DesignReady:
    case ModelGenerationUIStage::ModelReady:
    case ModelGenerationUIStage::Failed:
    case ModelGenerationUIStage::Stopped:
        return true;
    }
    return false;
}

inline ModelGenerationUIState advanced_model_presentation_state(
    ModelGenerationUIState state, bool can_stop, bool can_resume)
{
    state.smart_mode = false;
    // Advanced mode starts from four completed design views. Even if an old
    // job record reports the shared preparation phase, never expose the smart
    // mode's 2D preparation stage in the advanced 3D route.
    if (state.stage == ModelGenerationUIStage::GeneratingDesign)
        state.stage = ModelGenerationUIStage::GeneratingModel;
    ModelGenerationStatusText::present_advanced_model(state);
    state.can_stop = can_stop;
    if (state.stage == ModelGenerationUIStage::Failed || state.stage == ModelGenerationUIStage::Stopped)
        state.can_retry_model = can_resume;
    return state;
}

inline const std::string& model_generation_route_id(const ModelGenerationUIState& state)
{
    if (state.smart_mode && !state.smart_flow_id.empty()) return state.smart_flow_id;
    return state.model_generation_context && !state.model_asset_id.empty() ? state.model_asset_id : state.job_id;
}

inline bool model_generation_preserves_workbench(const ModelGenerationUIState& state,
    RedesignModelRouteAction action, bool workbench_visible)
{
    return workbench_visible && action == RedesignModelRouteAction::Model &&
        state.stage == ModelGenerationUIStage::ModelReady &&
        !state.model_asset_id.empty();
}

inline bool model_generation_route_revokes_workbench_presentation(
    RedesignModelRouteAction action, bool preserve_workbench)
{
    return !preserve_workbench &&
        (action == RedesignModelRouteAction::Model || action == RedesignModelRouteAction::Image);
}

inline RedesignModelRouteAction model_generation_route_action(
    const ModelGenerationUIState& state, const ModelGenerationUIState& current,
    bool locked, std::uint64_t route_session, const std::string& route_id, bool retain_foreground = false)
{
    if (state.revision < current.revision ||
        state.model_generation_session < current.model_generation_session)
        return RedesignModelRouteAction::Ignore;

    // History presentation, editing and another creator own their foreground.
    // Background task updates still project progress without changing routes.
    if (retain_foreground) return RedesignModelRouteAction::Refresh;
    const bool new_session = state.model_generation_session > current.model_generation_session;
    const std::string& state_route_id = model_generation_route_id(state);
    if (new_session && state_route_id.empty())
        return RedesignModelRouteAction::Ignore;
    if (locked) {
        if (state.model_generation_session < route_session)
            return RedesignModelRouteAction::Ignore;
        if (state.model_generation_session == route_session &&
            (!state.model_generation_context ||
             (!route_id.empty() && !state_route_id.empty() && state_route_id != route_id)))
            return RedesignModelRouteAction::Ignore;
    }

    // A successful history restore owns a new session, even when both records
    // already have a model context. Polls only refresh the selected page.
    if (new_session)
        return state.model_generation_context ? RedesignModelRouteAction::Model : RedesignModelRouteAction::Image;
    if (!locked && state.model_generation_context && !current.model_generation_context)
        return RedesignModelRouteAction::Model;
    return RedesignModelRouteAction::Refresh;
}

} // namespace Slic3r::GUI
