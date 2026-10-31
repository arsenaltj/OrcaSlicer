#pragma once

#include <wx/gdicmn.h>

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace Slic3r::GUI {

// Geometry from the last notification render, in the canvas's own pixel space.
class CanvasNotificationLayout
{
public:
    bool update(const void* canvas, wxSize size, std::vector<wxRect> bounds)
    {
        if (m_canvas == canvas && m_size == size && m_bounds == bounds) return false;
        m_canvas = canvas;
        m_size = size;
        m_bounds = std::move(bounds);
        return true;
    }

    const std::vector<wxRect>& bounds(const void* canvas, wxSize size) const
    {
        static const std::vector<wxRect> empty;
        return m_canvas == canvas && m_size == size ? m_bounds : empty;
    }

private:
    const void* m_canvas {nullptr};
    wxSize m_size;
    std::vector<wxRect> m_bounds;
};

// Keep the established position unless the notification column needs more room.
inline std::optional<wxRect> progress_above_notifications(wxRect preferred, wxSize canvas_size,
    const std::vector<wxRect>& notifications, int gap, int margin)
{
    const wxRect viewport(wxPoint(0, 0), canvas_size);
    for (const auto& rect : notifications) {
        if (!rect.IsEmpty() && rect.Intersects(viewport) &&
            rect.GetLeft() < preferred.GetRight() + 1 && rect.GetRight() >= preferred.GetLeft())
            preferred.y = std::min(preferred.y, rect.y - gap - preferred.height);
    }
    // Leave errors accessible when the notification column fills the viewport.
    // The slicing sidebar still owns its normal progress and cancel controls.
    if (preferred.y < margin) return std::nullopt;
    return preferred;
}

} // namespace Slic3r::GUI
