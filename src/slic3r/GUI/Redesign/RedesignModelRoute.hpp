#pragma once

#include "../AI/ModelGeneration/ModelGenerationHost.hpp"
#include "../AI/ModelGeneration/PostGenerationUiState.hpp"

namespace Slic3r::GUI {

enum class RedesignModelRouteAction { Ignore, Refresh, Model, Image };

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

inline const std::string& model_generation_route_id(const ModelGenerationUIState& state)
{
    return state.model_generation_context && !state.model_asset_id.empty() ? state.model_asset_id : state.job_id;
}

inline bool model_generation_preserves_workbench(const ModelGenerationUIState& state,
    RedesignModelRouteAction action, bool workbench_visible)
{
    return workbench_visible && action == RedesignModelRouteAction::Model &&
        state.stage == ModelGenerationUIStage::ModelReady &&
        !state.model_asset_id.empty();
}

inline RedesignModelRouteAction model_generation_route_action(
    const ModelGenerationUIState& state, const ModelGenerationUIState& current,
    bool locked, std::uint64_t route_session, const std::string& route_id)
{
    if (state.revision < current.revision ||
        state.model_generation_session < current.model_generation_session)
        return RedesignModelRouteAction::Ignore;

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
