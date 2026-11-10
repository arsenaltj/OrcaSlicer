#pragma once

#include <cstdint>
#include <algorithm>
#include <array>
#include <optional>
#include <wx/gdicmn.h>

namespace Slic3r::GUI {

// Normalized half extents for presenting a completed frame without stretching.
inline std::optional<std::array<float, 2>> preview_frame_extents(
    const wxSize& source, const wxSize& target)
{
    if (source.x <= 0 || source.y <= 0 || target.x <= 0 || target.y <= 0)
        return std::nullopt;
    const double aspect_ratio = double(source.x) * target.y / (double(source.y) * target.x);
    return std::array<float, 2> {float(std::min(1., aspect_ratio)),
                                float(std::min(1., 1. / aspect_ratio))};
}

// A frame and its deferred completion belong to one model/layout generation.
// Size equality alone cannot recognize a resize away from and back to it.
class ModelPreviewFrameState {
public:
    struct Frame {
        wxSize size;
        double dpi_scale;
        std::uint64_t generation;
    };

    void invalidate() { ++m_generation; }
    Frame begin(const wxSize& size, double dpi_scale) const {
        return {size, dpi_scale, m_generation};
    }
    bool current(const Frame& frame, const wxSize& size, double dpi_scale, bool shown) const {
        return shown && size.x > 0 && size.y > 0 && frame.generation == m_generation &&
            frame.size == size && frame.dpi_scale == dpi_scale;
    }

private:
    std::uint64_t m_generation {0};
};

} // namespace Slic3r::GUI
