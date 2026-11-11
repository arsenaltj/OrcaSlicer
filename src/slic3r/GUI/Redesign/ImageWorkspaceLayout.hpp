#pragma once

namespace Slic3r::GUI {

// Widths are in DIP. Automatic collapse must not change the user's preference.
class ImageWorkspaceLayout {
public:
    static constexpr int center_min_width = 420;
    static constexpr int history_width = 376;
    static constexpr int history_rail_width = 24;
    static constexpr int history_gap = 12;

    void set_width(int width) { if (width > 0) m_width = width; }
    bool can_expand() const { return m_width >= center_min_width + history_gap + history_width; }
    bool expanded() const { return m_prefer_expanded && can_expand(); }
    void toggle() { if (can_expand()) m_prefer_expanded = !m_prefer_expanded; }

private:
    int m_width {0};
    bool m_prefer_expanded {true};
};

}
