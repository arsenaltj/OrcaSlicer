#pragma once

#include "ModelGenerationStarfield.hpp"
#include <wx/brush.h>
#include <wx/graphics.h>
#include <wx/pen.h>

namespace Slic3r::GUI {

inline void draw_model_generation_starfield(wxGraphicsContext& gc, const ModelGenerationStarfield& stars,
    double seconds, const wxSize& size, const wxRect& quiet_area, double dip_scale)
{
    if (size.x <= 0 || size.y <= 0)
        return;
    gc.PushState();
    gc.Clip(2.0 * dip_scale, 2.0 * dip_scale,
            std::max(0.0, size.x - 4.0 * dip_scale), std::max(0.0, size.y - 4.0 * dip_scale));
    gc.SetPen(*wxTRANSPARENT_PEN);
    for (std::size_t i = 0; i < ModelGenerationStarfield::particle_count; ++i) {
        const auto star = stars.sample(i, seconds);
        const double offset_x = star.x - 0.5, offset_y = star.y - 0.5;
        const double distance = std::hypot(offset_x, offset_y);
        const double direction_x = offset_x / distance, direction_y = offset_y / distance;
        // Gather around the text rather than losing every star behind its
        // quiet area when the viewport is small. The outer edge stays fixed.
        const double halo_x = (quiet_area.width / 2.0 + 30.0 * dip_scale) / size.x;
        const double halo_y = (quiet_area.height / 2.0 + 30.0 * dip_scale) / size.y;
        const double inner_radius = std::min(0.46 * 0.72, 1.0 / std::hypot(direction_x / halo_x, direction_y / halo_y));
        const double radius_from_center = inner_radius + (0.46 - inner_radius) * distance / 0.46;
        const double x = (0.5 + direction_x * radius_from_center) * size.x;
        const double y = (0.5 + direction_y * radius_from_center) * size.y;
        const double dx = std::max({double(quiet_area.GetLeft()) - x, 0.0, x - quiet_area.GetRight()});
        const double dy = std::max({double(quiet_area.GetTop()) - y, 0.0, y - quiet_area.GetBottom()});
        const double fade = std::clamp(std::hypot(dx, dy) / (36.0 * dip_scale), 0.0, 1.0);
        const int alpha = int(215.0 * star.opacity * fade);
        if (alpha < 3)
            continue;
        const double radius = star.radius * dip_scale;
        const int red = star.warm ? 255 : 226, green = star.warm ? 209 : 232, blue = star.warm ? 126 : 246;
        gc.SetBrush(wxBrush(wxColour(red, green, blue, alpha / 7)));
        gc.DrawEllipse(x - radius * 3.0, y - radius * 3.0, radius * 6.0, radius * 6.0);
        gc.SetBrush(wxBrush(wxColour(red, green, blue, alpha)));
        gc.DrawEllipse(x - radius, y - radius, radius * 2.0, radius * 2.0);
    }
    gc.PopState();
}

} // namespace Slic3r::GUI
