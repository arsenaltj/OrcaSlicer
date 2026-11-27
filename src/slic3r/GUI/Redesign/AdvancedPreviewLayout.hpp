#pragma once

#include <wx/gdicmn.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace Slic3r::GUI {

inline wxSize fit_design_preview(wxSize space, wxSize preferred)
{
    if (space.x <= 0 || space.y <= 0 || preferred.x <= 0 || preferred.y <= 0) return {0, 0};
    const double scale = std::min({1.0, double(space.x) / preferred.x, double(space.y) / preferred.y});
    return {std::max(1, int(std::lround(preferred.x * scale))), std::max(1, int(std::lround(preferred.y * scale)))};
}

inline wxRect center_design_preview(wxSize viewport, wxSize content)
{
    return {(viewport.x - content.x) / 2, (viewport.y - content.y) / 2, content.x, content.y};
}

inline std::array<wxRect, 4> design_preview_grid(wxSize viewport, int fit_height, wxSize preferred, int gap)
{
    gap = std::clamp(gap, 0, std::max(0, std::min(viewport.x, fit_height) - 2));
    const auto card = fit_design_preview({(viewport.x - gap) / 2, (fit_height - gap) / 2}, preferred);
    const auto block = center_design_preview(viewport, {2 * card.x + gap, 2 * card.y + gap});
    std::array<wxRect, 4> result;
    for (int i = 0; i < 4; ++i)
        result[i] = {block.x + i % 2 * (card.x + gap), block.y + i / 2 * (card.y + gap), card.x, card.y};
    return result;
}

struct DesignPreviewControls {
    struct ThumbnailHit { std::size_t index; bool selection; };
    wxRect title, regenerate;
    std::array<wxRect, 4> thumbnails {}, thumbnail_checks {};

    std::optional<ThumbnailHit> thumbnail_at(wxPoint point, bool show_checks) const
    {
        for (std::size_t i = 0; i < thumbnails.size(); ++i) {
            if (!thumbnails[i].Contains(point)) continue;
            // A visible checkbox owns its corner even when it is disabled.
            return ThumbnailHit {i, show_checks && thumbnail_checks[i].Contains(point)};
        }
        return std::nullopt;
    }
};

template<class Dip>
DesignPreviewControls design_preview_controls(wxSize card, bool single, Dip dip)
{
    DesignPreviewControls result;
    if (card.x <= 0 || card.y <= 0) return result;
    const int inset = dip(12);
    const int title_width = std::min(card.x, dip(52));
    const int title_height = std::min(card.y, dip(24));
    result.title = {(card.x - title_width) / 2, std::min(inset, card.y - title_height), title_width, title_height};
    const int icon = std::min({card.x, card.y, dip(24)});
    result.regenerate = {std::min(inset, card.x - icon), std::max(0, card.y - inset - icon), icon, icon};
    if (!single) return result;

    const int gap = std::min(dip(4), std::max(0, (card.x - 4) / 3));
    const int bottom = std::min(inset, card.y / 4);
    const int side = std::min({dip(36), std::max(0, (card.x - 3 * gap) / 4), card.y - bottom});
    const int strip_width = 4 * side + 3 * gap;
    const int x = (card.x - strip_width) / 2;
    const int y = card.y - bottom - side;
    const int check_side = std::min(dip(18), side / 2);
    for (int i = 0; i < 4; ++i) {
        const int left = x + i * (side + gap);
        result.thumbnails[i] = {left, y, side, side};
        result.thumbnail_checks[i] = {left + side - check_side, y, check_side, check_side};
    }
    if (side > 0 && result.regenerate.Intersects(wxRect(x, y, strip_width, side)))
        result.regenerate.y = std::max(0, y - dip(4) - icon);
    return result;
}

} // namespace Slic3r::GUI
