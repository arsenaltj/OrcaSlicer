#pragma once

#include <chrono>

namespace Slic3r::GUI {

// Only time during which the owning window can present a frame counts toward
// the preparation deadline. Both edges of an iconize/restore interval are
// observed, so an idle/minimized window cannot consume its presentation budget.
class RedesignPresentationDeadline {
public:
    using Clock = std::chrono::steady_clock;
    void begin(Clock::time_point now) { m_last = now; m_elapsed = {}; m_paused = false; }
    bool observe(Clock::time_point now, bool paused) {
        if (!m_paused && !paused) m_elapsed += now - m_last;
        m_last = now;
        m_paused = paused;
        return m_elapsed >= std::chrono::seconds(15);
    }
private:
    Clock::time_point m_last;
    Clock::duration m_elapsed {};
    bool m_paused {false};
};

} // namespace Slic3r::GUI
