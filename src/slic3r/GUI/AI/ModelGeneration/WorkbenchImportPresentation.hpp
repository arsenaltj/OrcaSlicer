#pragma once
#include <wx/gdicmn.h>
#include <algorithm>

namespace Slic3r::GUI {
// All coordinates belong to the content host. Restrict feedback to the actual
// viewport, including a horizontally scrolled or partially clipped workbench.
inline wxRect workbench_import_feedback_bounds(wxRect viewport, const wxRect& content)
{
    viewport.Intersect(content);
    return viewport;
}

inline wxRect workbench_import_card_bounds(wxRect viewport, const wxRect& content,
                                           const wxSize& requested_size)
{
    viewport = workbench_import_feedback_bounds(viewport, content);
    if (viewport.IsEmpty() || requested_size.x <= 0 || requested_size.y <= 0) return {};
    const int width = std::min(viewport.width, requested_size.x);
    const int height = std::min(viewport.height, requested_size.y);
    return {viewport.x + (viewport.width - width) / 2,
            viewport.y + (viewport.height - height) / 2, width, height};
}
} // namespace Slic3r::GUI
