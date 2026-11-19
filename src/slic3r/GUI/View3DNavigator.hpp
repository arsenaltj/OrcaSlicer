#pragma once

#include "libslic3r/Point.hpp"
#include <imgui.h>
#include <ImGuizmo.h>
#include <deque>
#include <memory>
#include <functional>
#include <wx/gdicmn.h>
#include <optional>
#include <algorithm>
#include <vector>

class wxMouseEvent;
class wxWindow;

namespace Slic3r::GUI {

inline constexpr float view_navigator_size = 128.f;

// All rectangles use the surface parent's client coordinates. Keep the right
// edge on the actual model canvas, even when the footer spans a beauty panel.
inline std::optional<wxRect> workbench_navigator_bounds(const wxRect& viewport,
    const wxRect& footer, const wxRect& host, int side, const std::vector<wxRect>& tools = {})
{
    if (side <= 0 || viewport.IsEmpty() || host.IsEmpty()) return {};
    const auto usable = viewport.Intersect(host);
    if (usable.width < side || usable.height < side) return {};
    const int right = usable.x + usable.width;
    const int baseline = footer.IsEmpty() ? usable.y + usable.height : footer.y + footer.height;
    wxRect candidate(right - side, baseline - side, side, side);
    // The part below the canvas may use only the footer area on its model
    // side. If the toolbar cannot accommodate it, keep the entire control GL-local.
    const wxRect strip(usable.x, usable.y, usable.width,
        std::max(usable.height, baseline - usable.y));
    if (!host.Contains(candidate) || !strip.Contains(candidate) ||
        (!footer.IsEmpty() && candidate.y + side > usable.y + usable.height &&
         (footer.x > candidate.x || footer.x + footer.width < right)) ||
        std::any_of(tools.begin(), tools.end(), [&](const auto& tool) { return tool.Intersects(candidate); }))
        candidate = wxRect(right - side, usable.y + usable.height - side, side, side);
    return host.Contains(candidate) ? std::optional<wxRect>(candidate) : std::nullopt;
}

// ImGui 1.83 polls button state once per frame. Preserve transitions when wx
// delivers a complete click before the next paint, while coalescing motion.
class ViewNavigatorMouseInput {
public:
    struct Sample {
        ImVec2 position;
        bool down, ctrl, shift, alt, super;
    };
    void push(Sample sample) {
        const bool transition = sample.down != m_last_down;
        m_last_down = sample.down;
        if (!m_samples.empty() && !m_samples.back().transition && !transition)
            m_samples.back().sample = sample;
        else
            m_samples.push_back({sample, transition});
    }
    void apply(ImGuiIO& io) {
        if (m_samples.empty()) return;
        const auto sample = m_samples.front().sample;
        m_samples.pop_front();
        // Moving to a face before pressing is not a drag of that face.
        if (sample.down && !io.MouseDown[0]) io.MousePosPrev = sample.position;
        io.MousePos = sample.position;
        io.MouseDown[0] = sample.down;
        io.KeyCtrl = sample.ctrl; io.KeyShift = sample.shift;
        io.KeyAlt = sample.alt; io.KeySuper = sample.super;
    }
    bool pending() const { return !m_samples.empty(); }
    void reset() { m_samples.clear(); m_last_down = false; }
private:
    struct PendingSample { Sample sample; bool transition; };
    std::deque<PendingSample> m_samples;
    bool m_last_down {false};
};

// Rotation is always in Orca's Z-up coordinates. The helper owns no camera.
ImGuizmo::ViewManipulateResult draw_view_navigator(Transform3d& rotation,
    ImVec2 position, float size, bool dark, ImGuizmo::ViewManipulateState& state,
    bool accept_input = true);

// Composite only the model pixels covered by the navigation surface. The
// framebuffer texture has bottom-up rows; window coordinates run top-down.
void draw_view_navigator_backdrop(unsigned int texture, const wxRect& preview, const wxRect& surface);

// A private ImGui context keeps the workbench's input, font atlas and draw lists
// separate from the native slicer's globally owned ImGui frame.
class WorkbenchViewNavigator {
public:
    WorkbenchViewNavigator();
    ~WorkbenchViewNavigator();
    bool mouse(wxMouseEvent& event);
    bool begin(Transform3d& rotation, const wxSize& size, double dpi, bool dark, bool enabled);
    void render();
    void reset();
    void attach_surface(wxWindow* parent, wxWindow* bottom_anchor,
        std::function<Transform3d()> rotation, std::function<void(const Transform3d&)> changed);
    bool has_surface() const;
    void set_preview_frame(unsigned int texture, wxWindow* preview);
    void show_surface(bool visible);
    bool present_surface(bool visible, bool enabled);
    bool presentation_failed() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace Slic3r::GUI
