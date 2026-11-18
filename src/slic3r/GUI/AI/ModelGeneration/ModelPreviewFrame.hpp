#pragma once

#include <cstdint>
#include <algorithm>
#include <array>
#include <optional>
#include <functional>
#include <string>
#include <utility>
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

// A leaf overlay waits independently from model-install and portrait observers.
// Replacing or canceling it also rejects completions already in the GUI queue.
class ModelPreviewOverlayCompletion {
public:
    using Token = std::uint64_t;
    void request(std::function<void()> callback) { ++m_token; m_callback = std::move(callback); }
    Token token() const { return m_token; }
    bool pending() const { return bool(m_callback); }
    bool complete(Token token) {
        if (token != m_token || !m_callback) return false;
        auto callback = std::exchange(m_callback, {});
        ++m_token;
        callback();
        return true;
    }
private:
    Token m_token {0};
    std::function<void()> m_callback;
};

// Model installation owns a separate waiter. A CPU payload or an old visible
// frame cannot release its business gate, and replacing it invalidates GUI
// completions which have already been queued.
class ModelPreviewInstallationCompletion {
public:
    using Token = std::uint64_t;
    void request(std::string identity, std::function<void(bool)> callback) {
        ++m_token; m_identity = std::move(identity); m_callback = std::move(callback);
    }
    void cancel() { ++m_token; m_callback = {}; m_identity.clear(); }
    Token token() const { return m_token; }
    bool pending() const { return bool(m_callback); }
    bool complete(Token token, const std::string& identity, bool success,
                  bool current_frame = true, bool uploaded = true) {
        if (token != m_token || !m_callback || identity != m_identity ||
            (success && (!current_frame || !uploaded))) return false;
        auto callback = std::exchange(m_callback, {});
        ++m_token; m_identity.clear();
        callback(success);
        return true;
    }
private:
    Token m_token {0};
    std::string m_identity;
    std::function<void(bool)> m_callback;
};

} // namespace Slic3r::GUI
