#pragma once

#include <array>

namespace Slic3r::GUI {

enum class RedesignWorkspacePage { Assets, Image, Model, Print, Account };
enum class RedesignModelView { Result, Workbench, Slicing, Preview };

struct RedesignWorkbenchRoute
{
    RedesignWorkspacePage page;
    RedesignModelView view;
};

struct RedesignNavigationItem
{
    RedesignWorkspacePage page;
    const char* label;
    const char* icon;
};

// Visual navigation is independent of the retained pages and legacy tab IDs.
inline constexpr std::array<RedesignNavigationItem, 3> redesign_navigation_items {{
    {RedesignWorkspacePage::Assets, "资产", "redesign_nav_assets.png"},
    {RedesignWorkspacePage::Image, "图像", "redesign_nav_image.png"},
    {RedesignWorkspacePage::Model, "工作台", "redesign_nav_model.png"}
}};

inline RedesignWorkspacePage redesign_navigation_page(RedesignWorkspacePage page)
{
    return page == RedesignWorkspacePage::Print ? RedesignWorkspacePage::Model : page;
}

inline bool workbench_print_preparation_allowed(bool gcode_ready, bool busy)
{
    return gcode_ready && !busy;
}

// Monitor and media routes remain accessible without a printable project.
inline RedesignModelView workbench_return_view(bool gcode_ready)
{
    return gcode_ready ? RedesignModelView::Preview : RedesignModelView::Slicing;
}

inline RedesignWorkbenchRoute workbench_resume_route(RedesignWorkbenchRoute retained,
                                                    bool monitoring, bool gcode_ready)
{
    if (!gcode_ready &&
        ((retained.page == RedesignWorkspacePage::Print && !monitoring) ||
         (retained.page == RedesignWorkspacePage::Model && retained.view == RedesignModelView::Preview)))
        return {RedesignWorkspacePage::Model, RedesignModelView::Slicing};
    return retained;
}

} // namespace Slic3r::GUI
