#pragma once
#include <wx/gdicmn.h>
#include <algorithm>
#include <cstdint>

namespace Slic3r::GUI {
// The AI settings are a sibling of the content host. Retain their union,
// including the gap, while leaving the fixed navigation outside the cover.
inline wxRect workbench_retained_page_bounds(wxRect content, const wxRect& settings)
{
    if (!settings.IsEmpty()) content.Union(settings);
    return content;
}

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

// Keep the visible source viewport as the reference while the destination is
// being laid out. Owner movement and resizing still move the card with it.
class WorkbenchLoadingCardAnchor {
public:
    WorkbenchLoadingCardAnchor() = default;
    WorkbenchLoadingCardAnchor(const wxRect& viewport, const wxRect& content, const wxRect& owner)
        : m_viewport(workbench_import_feedback_bounds(viewport, content)), m_owner(owner)
    {
        if (m_viewport.IsEmpty()) m_viewport = content;
        m_viewport.Intersect(owner);
    }

    wxRect bounds(const wxRect& owner, const wxSize& card_size) const
    {
        if (m_viewport.IsEmpty() || owner.IsEmpty())
            return workbench_import_card_bounds(owner, owner, card_size);
        const auto resize = [](int value, int original, int current) {
            return int(std::int64_t(value) * current / original);
        };
        const wxRect viewport(
            owner.x + resize(m_viewport.x - m_owner.x, m_owner.width, owner.width),
            owner.y + resize(m_viewport.y - m_owner.y, m_owner.height, owner.height),
            resize(m_viewport.width, m_owner.width, owner.width),
            resize(m_viewport.height, m_owner.height, owner.height));
        return workbench_import_card_bounds(viewport, owner, card_size);
    }

private:
    wxRect m_viewport;
    wxRect m_owner;
};
} // namespace Slic3r::GUI
