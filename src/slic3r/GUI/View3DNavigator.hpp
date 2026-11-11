#pragma once

#include "libslic3r/Point.hpp"
#include <imgui.h>
#include <ImGuizmo.h>
#include <deque>
#include <memory>
#include <functional>
#include <wx/gdicmn.h>

class wxMouseEvent;
class wxWindow;

namespace Slic3r::GUI {

inline constexpr float view_navigator_size = 128.f;

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
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace Slic3r::GUI
