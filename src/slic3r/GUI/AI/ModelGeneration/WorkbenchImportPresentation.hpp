#pragma once
#include <wx/gdicmn.h>

namespace Slic3r::GUI {
// All coordinates belong to the content host. Restrict feedback to the actual
// viewport, including a horizontally scrolled or partially clipped workbench.
inline wxRect workbench_import_feedback_bounds(wxRect viewport, const wxRect& content)
{
    viewport.Intersect(content);
    return viewport;
}
} // namespace Slic3r::GUI
