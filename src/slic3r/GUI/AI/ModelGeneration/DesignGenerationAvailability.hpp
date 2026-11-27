#pragma once

#include "ModelGenerationHost.hpp"
#include "ModelGenerationPresentation.hpp"
#include "PostGenerationUiState.hpp"

namespace Slic3r::GUI {

// A retained local candidate is not an active operation. The Shell asks for
// confirmation before replacing it; cancelling leaves the workbench intact.
inline bool design_generation_clickable(bool busy, const PostGenerationUiState& workbench)
{
    return !busy && post_generation_return_to_design_allowed(workbench);
}

inline std::string design_generation_rejection(const ModelGenerationUIInput& input,
    bool service_available, bool busy, const PostGenerationUiState& workbench,
    bool palette_ready)
{
    if (!design_generation_clickable(busy, workbench)) return "workbench_guard";
    if (!service_available) return "service_unavailable";
    if (!palette_ready) return "palette_unavailable";
    if (input.image_path.empty() && input.prompt.empty()) return "empty_input";
    if (input.prompt.size() > ModelGenerationPresentation::MAX_MODEL_INPUT_BYTES) return "prompt_too_long";
    if (!ModelGenerationPresentation::is_supported_style(input.style)) return "unsupported_style";
    if (input.style == "custom" && input.custom_style.empty()) return "custom_style_empty";
    return {};
}

inline bool design_generation_available(const ModelGenerationUIInput& input,
    bool service_available, bool busy, const PostGenerationUiState& workbench, bool palette_ready)
{
    return design_generation_rejection(input, service_available, busy, workbench, palette_ready).empty();
}

} // namespace Slic3r::GUI
