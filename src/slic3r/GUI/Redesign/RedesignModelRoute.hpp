#pragma once

#include "../AI/ModelGeneration/ModelGenerationHost.hpp"

namespace Slic3r::GUI {

enum class RedesignModelRouteAction { Ignore, Refresh, Model, Image };

inline RedesignModelRouteAction model_generation_route_action(
    const ModelGenerationUIState& state, const ModelGenerationUIState& current,
    bool locked, std::uint64_t route_session, const std::string& route_job_id)
{
    if (state.revision < current.revision ||
        state.model_generation_session < current.model_generation_session)
        return RedesignModelRouteAction::Ignore;

    const bool new_session = state.model_generation_session > current.model_generation_session;
    if (new_session && state.job_id.empty())
        return RedesignModelRouteAction::Ignore;
    if (locked) {
        if (state.model_generation_session < route_session)
            return RedesignModelRouteAction::Ignore;
        if (state.model_generation_session == route_session &&
            (!state.model_generation_context ||
             (!route_job_id.empty() && !state.job_id.empty() && state.job_id != route_job_id)))
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
