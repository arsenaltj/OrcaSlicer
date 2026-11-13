#pragma once

#include <algorithm>
#include <optional>
#include <wx/gdicmn.h>

namespace Slic3r::GUI {

inline wxSize workbench_history_handle_size() { return {18, 42}; }

enum class WorkbenchHistoryMountAction { Skip, Attach, Refresh };

inline WorkbenchHistoryMountAction workbench_history_mount_action(
    bool mounted, bool same_host, bool temporary_home)
{
    if (mounted) return same_host ? WorkbenchHistoryMountAction::Refresh : WorkbenchHistoryMountAction::Skip;
    return temporary_home ? WorkbenchHistoryMountAction::Skip : WorkbenchHistoryMountAction::Attach;
}

// Each retained page owns its drawer preference; temporary hiding is not a user action.
struct WorkbenchHistorySidebarState {
    bool workbench_open {true};
    bool creation_open {true};

    bool panel_visible(bool mounted, bool workbench, bool editing) const {
        return mounted ? creation_open : workbench && workbench_open && !editing;
    }
    void toggle(bool mounted, bool editing) {
        if (mounted) creation_open = !creation_open;
        else workbench_open = editing || !workbench_open;
    }
};

inline int workbench_history_scroll_target(int client_width, int history_left, int history_width,
    int current, int unit, int handle_width)
{
    if (client_width <= 0 || unit <= 0) return current;
    const int right = history_left + history_width;
    if (right > client_width) return current + (right - client_width + unit - 1) / unit;
    if (history_left < handle_width)
        return std::max(0, current + (history_left - handle_width) / unit);
    return current;
}

// Inputs are physical client coordinates of the common host, already DPI scaled.
inline std::optional<wxRect> workbench_history_toggle_bounds(
    const wxSize& client, const wxRect& viewport, const wxSize& size,
    std::optional<int> history_left)
{
    if (size.x <= 0 || size.y <= 0 || client.x < size.x || client.y < size.y ||
        viewport.height < size.y)
        return std::nullopt;
    const int width = size.x;
    const int height = size.y;
    const int top = std::clamp(viewport.y, 0, client.y);
    const int bottom = std::clamp(viewport.y + viewport.height, top, client.y);
    return wxRect(std::clamp(history_left.value_or(client.x) - width, 0, client.x - width),
        std::clamp(top + (bottom - top - height) / 2, 0, client.y - height), width, height);
}

} // namespace Slic3r::GUI
