#pragma once

#include "ModelGenerationHost.hpp"
#include "ModelGenerationPresentation.hpp"
#include "PostGenerationUiState.hpp"

namespace Slic3r::GUI {

// A retained local candidate is not an active operation. The Shell asks for
// confirmation before replacing it; cancelling leaves the workbench intact.
inline bool design_generation_available(const ModelGenerationUIInput& input,
    bool service_available, bool busy, const PostGenerationUiState& workbench,
    bool palette_ready)
{
    return service_available && !busy && palette_ready &&
        post_generation_return_to_design_allowed(workbench) &&
        (!input.image_path.empty() || !input.prompt.empty()) &&
        ModelGenerationPresentation::is_supported_style(input.style) &&
        (input.style != "custom" || !input.custom_style.empty());
}

} // namespace Slic3r::GUI
