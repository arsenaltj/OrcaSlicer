#pragma once

#include "RedesignPageTransition.hpp"
#include "RedesignPresentationDeadline.hpp"
#include <wx/gdicmn.h>

#include <chrono>
#include <cstddef>
#include <optional>

namespace Slic3r::GUI {

inline std::optional<wxRect> preview_cover_bounds(const wxRect& canvas, const wxRect& owner)
{
    if (canvas.IsEmpty() || owner.IsEmpty()) return {};
    auto bounds = canvas.Intersect(owner);
    return bounds.IsEmpty() ? std::nullopt : std::optional<wxRect>(bounds);
}

// Plater can announce completion before dropping its running flag, and can
// publish the same completed result through more than one listener.
class SliceCompletionPresentation
{
public:
    bool consume(bool completed, bool running, bool valid, unsigned int result_id)
    {
        if (!completed || running || !valid || (m_last_result && *m_last_result == result_id))
            return false;
        m_last_result = result_id;
        return true;
    }

private:
    std::optional<unsigned int> m_last_result;
};

class RedesignPreviewFrameGate
{
public:
    using Token = RedesignPageTransition::Token;
    using Clock = std::chrono::steady_clock;
    enum class Status { Stale, Waiting, Ready, TimedOut };

    void begin(Token token, unsigned int result_id, Clock::time_point now)
    {
        m_token = token;
        m_result = result_id;
        m_deadline.begin(now); m_paused = false;
        m_frame.reset();
    }

    void invalidate_frame() { m_frame.reset(); }
    void finish() { m_token.reset(); m_frame.reset(); }
    void set_paused(bool paused, Clock::time_point now) {
        m_deadline.observe(now, paused);
        if (paused || m_paused != paused) m_frame.reset();
        m_paused = paused;
    }

    Status observe(Token token, unsigned int loaded_result, bool content_ready, bool viewport_ready,
                   int width, int height, std::size_t presented_frames, Clock::time_point now)
    {
        if (!m_token || token != *m_token) return Status::Stale;
        if (m_deadline.observe(now, m_paused)) return Status::TimedOut;
        if (m_paused) return Status::Waiting;
        if (loaded_result != m_result || !content_ready || !viewport_ready || width <= 0 || height <= 0) {
            m_frame.reset();
            return Status::Waiting;
        }
        // First observe the final loaded/fitted layout, then demand a later
        // SwapBuffers. Old frames and frames at a previous size cannot reveal it.
        if (!m_frame || m_frame->width != width || m_frame->height != height) {
            m_frame = Frame {width, height, presented_frames};
            return Status::Waiting;
        }
        return presented_frames > m_frame->count ? Status::Ready : Status::Waiting;
    }

private:
    struct Frame { int width; int height; std::size_t count; };
    std::optional<Token> m_token;
    unsigned int m_result {0};
    RedesignPresentationDeadline m_deadline;
    bool m_paused {false};
    std::optional<Frame> m_frame;
};

} // namespace Slic3r::GUI
