#pragma once

#include <cstdint>
#include <optional>

namespace Slic3r::GUI {

// Grid and Single place the navigation controls differently. Treat rapid
// releases at the same screen position as one gesture even if relayout puts a
// different child control under the pointer. The first release stays immediate.
class AdvancedPreviewClickSequence {
public:
    bool accept(std::uint64_t time_ms, int x, int y,
                std::uint64_t double_click_ms, int tolerance)
    {
        if (m_last && time_ms >= m_last->time_ms &&
            time_ms - m_last->time_ms <= double_click_ms &&
            distance(x, m_last->x) <= tolerance && distance(y, m_last->y) <= tolerance)
            return false;

        m_last = Click {time_ms, x, y};
        return true;
    }

    void reset() { m_last.reset(); }

private:
    static std::int64_t distance(int a, int b)
    {
        const auto delta = static_cast<std::int64_t>(a) - b;
        return delta < 0 ? -delta : delta;
    }
    struct Click { std::uint64_t time_ms; int x, y; };
    std::optional<Click> m_last;
};

} // namespace Slic3r::GUI
