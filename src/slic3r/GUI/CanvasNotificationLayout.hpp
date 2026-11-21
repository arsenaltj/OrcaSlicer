#pragma once

#include <wx/gdicmn.h>

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace Slic3r::GUI {

struct CanvasNotificationBounds
{
    std::vector<wxRect> visible;
    wxRect object_info;
};

// Geometry from the last notification render, in wx canvas client coordinates.
class CanvasNotificationLayout
{
public:
    bool update(const void* canvas, wxSize size, CanvasNotificationBounds bounds)
    {
        if (m_canvas == canvas && m_size == size && m_bounds.visible == bounds.visible &&
            m_bounds.object_info == bounds.object_info) return false;
        m_canvas = canvas;
        m_size = size;
        m_bounds = std::move(bounds);
        return true;
    }

    const CanvasNotificationBounds& bounds(const void* canvas, wxSize size) const
    {
        static const CanvasNotificationBounds empty;
        return m_canvas == canvas && m_size == size ? m_bounds : empty;
    }

private:
    const void* m_canvas {nullptr};
    wxSize m_size;
    CanvasNotificationBounds m_bounds;
};

// Share the object card's horizontal bounds and clear the whole notification stack.
inline std::optional<wxRect> progress_above_notifications(wxRect preferred, wxSize canvas_size,
    const CanvasNotificationBounds& notifications, int gap, int margin)
{
    const wxRect viewport(wxPoint(0, 0), canvas_size);
    wxRect object_info = notifications.object_info;
    if (!object_info.Intersect(viewport).IsEmpty()) {
        preferred.x = object_info.x;
        preferred.width = object_info.width;
    } else {
        const int available_width = canvas_size.x - 2 * margin;
        if (available_width <= 0) return std::nullopt;
        preferred.width = std::min(preferred.width, available_width);
        preferred.x = std::clamp(preferred.x, margin, canvas_size.x - margin - preferred.width);
    }
    if (preferred.IsEmpty()) return std::nullopt;

    for (const auto& rect : notifications.visible) {
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
