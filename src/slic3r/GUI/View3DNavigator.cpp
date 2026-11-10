#include <glad/gl.h>
#include "View3DNavigator.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "ImGuiWrapper.hpp"
#include "OpenGLManager.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Color.hpp"
#include <wx/event.h>
#include <wx/dcclient.h>
#include <wx/glcanvas.h>
#include <wx/weakref.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cfloat>

namespace Slic3r::GUI {

namespace {
struct ImGuiContextScope {
    ImGuiContext* saved {ImGui::GetCurrentContext()};
    explicit ImGuiContextScope(ImGuiContext* context) { ImGui::SetCurrentContext(context); }
    ~ImGuiContextScope() { ImGui::SetCurrentContext(saved); }
};
}

ImGuizmo::ViewManipulateResult draw_view_navigator(Transform3d& rotation,
    ImVec2 position, float size, bool dark, ImGuizmo::ViewManipulateState& state, bool accept_input)
{
    auto& io = ImGui::GetIO();
    const bool saved_down = io.MouseDown[0];
    if (!accept_input) { state.reset(); io.MouseDown[0] = false; }
    ImGuizmo::BeginFrame();
    auto& style = ImGuizmo::GetStyle();
    style.Colors[ImGuizmo::COLOR::DIRECTION_X] = ImGuiWrapper::to_ImVec4(ColorRGBA::Y());
    style.Colors[ImGuizmo::COLOR::DIRECTION_Y] = ImGuiWrapper::to_ImVec4(ColorRGBA::Z());
    style.Colors[ImGuizmo::COLOR::DIRECTION_Z] = ImGuiWrapper::to_ImVec4(ColorRGBA::X());
    style.Colors[ImGuizmo::COLOR::TEXT] = dark ? ImVec4(224 / 255.f, 224 / 255.f, 224 / 255.f, 1.f) : ImVec4(.2f, .2f, .2f, 1.f);
    style.Colors[ImGuizmo::COLOR::FACE] = dark ? ImVec4(.23f, .23f, .23f, 1.f) : ImVec4(.77f, .77f, .77f, 1.f);
    strcpy(style.AxisLabels[ImGuizmo::Axis::Axis_X], _u8L_CONTEXT("Y", "Axis").c_str());
    strcpy(style.AxisLabels[ImGuizmo::Axis::Axis_Y], _u8L_CONTEXT("Z", "Axis").c_str());
    strcpy(style.AxisLabels[ImGuizmo::Axis::Axis_Z], _u8L_CONTEXT("X", "Axis").c_str());
    strcpy(style.FaceLabels[ImGuizmo::FACES::FACE_FRONT], _u8L_CONTEXT("Front", "Camera View").c_str());
    strcpy(style.FaceLabels[ImGuizmo::FACES::FACE_BACK], _u8L_CONTEXT("Back", "Camera View").c_str());
    strcpy(style.FaceLabels[ImGuizmo::FACES::FACE_TOP], _u8L_CONTEXT("Top", "Camera View").c_str());
    strcpy(style.FaceLabels[ImGuizmo::FACES::FACE_BOTTOM], _u8L_CONTEXT("Bottom", "Camera View").c_str());
    strcpy(style.FaceLabels[ImGuizmo::FACES::FACE_LEFT], _u8L_CONTEXT("Left", "Camera View").c_str());
    strcpy(style.FaceLabels[ImGuizmo::FACES::FACE_RIGHT], _u8L_CONTEXT("Right", "Camera View").c_str());

    static const float projection[16] = {1.f,0.f,0.f,0.f, 0.f,1.f,0.f,0.f, 0.f,0.f,1.f,0.f, 0.f,0.f,0.f,1.f};
    const auto mapping = Geometry::rotation_transform(Vec3d(.5 * PI, 0., .5 * PI));
    Transform3d mapped = rotation * mapping;
    float view[16];
    for (unsigned c = 0; c < 4; ++c)
        for (unsigned r = 0; r < 4; ++r) view[c * 4 + r] = float(mapped(r, c));
    const auto result = ImGuizmo::ViewManipulate(view, projection, ImGuizmo::ROTATE,
        ImGuizmo::WORLD, nullptr, 8.f, position, ImVec2(size, size), 0x00101010, state);
    io.MouseDown[0] = saved_down;
    if (result.changed) {
        for (unsigned c = 0; c < 4; ++c)
            for (unsigned r = 0; r < 4; ++r) mapped(r, c) = view[c * 4 + r];
        rotation = Transform3d::Identity();
        rotation.linear() = (mapped * mapping.inverse()).linear();
    }
    return result;
}

void draw_view_navigator_backdrop(unsigned int texture, const wxRect& preview, const wxRect& surface)
{
    if (!texture || preview.IsEmpty() || surface.IsEmpty()) return;
    const auto overlap = preview.Intersect(surface);
    if (overlap.IsEmpty()) return;
    const auto offset = overlap.GetPosition() - surface.GetPosition();
    const float left = float(overlap.x - preview.x) / preview.width;
    const float right = float(overlap.x + overlap.width - preview.x) / preview.width;
    const float top = 1.f - float(overlap.y - preview.y) / preview.height;
    const float bottom = 1.f - float(overlap.y + overlap.height - preview.y) / preview.height;
    ImGui::GetBackgroundDrawList()->AddImage((ImTextureID)(intptr_t)texture,
        ImVec2(float(offset.x), float(offset.y)),
        ImVec2(float(offset.x + overlap.width), float(offset.y + overlap.height)),
        ImVec2(left, top), ImVec2(right, bottom));
}

struct WorkbenchViewNavigator::Impl {
    ImGuiContext* context {nullptr};
    std::unique_ptr<ImGuiWrapper> gui;
    ImGuizmo::ViewManipulateState state;
    ViewNavigatorMouseInput input;
    wxRect bounds;
    bool pressed {false};
    bool frame_open {false};
    std::string language;
    float font_size {0.f}, style_scale {0.f};
    wxWeakRef<wxGLCanvas> surface;
    wxWeakRef<wxWindow> bottom_anchor;
    wxWeakRef<wxWindow> preview;
    wxSize preview_size;
    unsigned int preview_texture {0};
    wxGLContext* gl_context {nullptr};
    std::function<Transform3d()> rotation;
    std::function<void(const Transform3d&)> changed;
    bool input_enabled {false}, visible {false};

    Impl() {
        ImGuiContextScope restore(ImGui::GetCurrentContext());
        // ImGui 1.83 selects a newly created context only when none is current.
        // ImGuiWrapper initializes that current context in its constructor.
        ImGui::SetCurrentContext(nullptr);
        gui = std::make_unique<ImGuiWrapper>();
        context = ImGui::GetCurrentContext();
    }
    ~Impl() {
        if (surface) surface->Destroy();
        ImGuiContextScope current(context);
        if (frame_open) gui->end_frame();
        gui.reset();
    }
};

WorkbenchViewNavigator::WorkbenchViewNavigator() : m_impl(std::make_unique<Impl>()) {}
WorkbenchViewNavigator::~WorkbenchViewNavigator() = default;

bool WorkbenchViewNavigator::mouse(wxMouseEvent& event)
{
    auto& self = *m_impl;
    const bool owns = self.pressed || self.bounds.Contains(event.GetPosition());
    self.input.push({ImVec2(float(event.GetX()), float(event.GetY())), event.LeftIsDown(),
        event.ControlDown(), event.ShiftDown(), event.AltDown(), event.MetaDown()});
    if (owns && event.LeftDown()) self.pressed = true;
    if (event.LeftUp()) self.pressed = false;
    return owns;
}

bool WorkbenchViewNavigator::begin(Transform3d& rotation, const wxSize& size, double dpi, bool dark, bool enabled)
{
    auto& self = *m_impl;
    ImGuiContextScope current(self.context);
    const auto language = wxGetApp().current_language_code().ToStdString();
    const auto* main_gui = wxGetApp().imgui();
    const float font = main_gui->get_font_size(), scale = main_gui->get_style_scaling();
    if (self.language != language) { self.gui->set_language(language); self.language = language; }
    if (self.font_size != font || self.style_scale != scale) {
        self.gui->set_scaling(font, scale, 1.f);
        self.font_size = font; self.style_scale = scale;
    }
    self.gui->set_display_size(float(size.x), float(size.y));
#if defined(__APPLE__)
    ImGui::GetIO().DisplayFramebufferScale = ImVec2(float(dpi), float(dpi));
    const double ui_scale = 1.;
#else
    const double ui_scale = dpi;
#endif
    const int side = int(std::lround(view_navigator_size * ui_scale));
    const int margin = int(std::lround(8. * ui_scale));
    self.bounds = wxRect(std::max(0, size.x - side - margin), std::max(0, size.y - side - margin), side, side);
    if (!enabled) reset();
    self.input.apply(ImGui::GetIO());
    self.gui->new_frame();
    self.frame_open = true;
    if (self.surface && self.preview)
        draw_view_navigator_backdrop(self.preview_texture, self.preview->GetScreenRect(), self.surface->GetScreenRect());
    const auto result = draw_view_navigator(rotation, ImVec2(float(self.bounds.x), float(self.bounds.y)),
        float(side), dark, self.state, enabled);
    return result.changed || self.state.interpolation_frames > 0 || self.input.pending();
}

void WorkbenchViewNavigator::render()
{
    auto& self = *m_impl;
    if (!self.frame_open) return;
    ImGuiContextScope current(self.context);
    self.gui->render(self.gui->end_frame());
    self.frame_open = false;
}

void WorkbenchViewNavigator::reset()
{
    auto& self = *m_impl;
    self.state.reset(); self.pressed = false;
    self.input.reset();
    if (self.surface && self.surface->HasCapture()) self.surface->ReleaseMouse();
    ImGuiContextScope current(self.context);
    auto& io = ImGui::GetIO();
    for (bool& down : io.MouseDown) down = false;
    io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
}

void WorkbenchViewNavigator::attach_surface(wxWindow* parent, wxWindow* bottom_anchor,
    std::function<Transform3d()> rotation, std::function<void(const Transform3d&)> changed)
{
    auto& self = *m_impl;
    if (self.surface || !parent || !bottom_anchor) return;
    self.bottom_anchor = bottom_anchor;
    self.rotation = std::move(rotation);
    self.changed = std::move(changed);
    self.surface = OpenGLManager::create_wxglcanvas(*parent);
    self.surface->SetName("workbench-view-navigator");
    self.gl_context = wxGetApp().init_glcontext(*self.surface);
    self.surface->Hide();
    self.surface->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        wxPaintDC dc(m_impl->surface);
        present_surface(m_impl->visible, m_impl->input_enabled);
    });
    auto input = [this](wxMouseEvent& event) {
        auto& self = *m_impl;
        if (!self.input_enabled) return;
        mouse(event);
        if (event.LeftDown() && !self.surface->HasCapture()) self.surface->CaptureMouse();
        if (event.LeftUp() && self.surface->HasCapture()) self.surface->ReleaseMouse();
        self.surface->Refresh(false);
    };
    self.surface->Bind(wxEVT_LEFT_DOWN, input);
    self.surface->Bind(wxEVT_LEFT_UP, input);
    self.surface->Bind(wxEVT_MOTION, input);
    self.surface->Bind(wxEVT_LEAVE_WINDOW, input);
    self.surface->Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { reset(); });
}

bool WorkbenchViewNavigator::has_surface() const { return bool(m_impl->surface); }

void WorkbenchViewNavigator::set_preview_frame(unsigned int texture, wxWindow* preview)
{
    auto& self = *m_impl;
    self.preview_texture = texture;
    self.preview = preview;
    self.preview_size = preview ? preview->GetClientSize() : wxSize();
    if (!texture && self.surface) self.surface->Hide();
}

void WorkbenchViewNavigator::show_surface(bool visible)
{
    auto& self = *m_impl;
    if (!self.surface || !self.bottom_anchor) return;
    self.visible = visible;
    if (!visible || !self.preview_texture || !self.preview || self.preview_size != self.preview->GetClientSize()) {
        if (!visible) reset();
        self.surface->Hide();
        return;
    }
    const int side = self.surface->FromDIP(int(view_navigator_size));
    const auto anchor_size = self.bottom_anchor->GetClientSize();
    const auto anchor = self.surface->GetParent()->ScreenToClient(
        self.bottom_anchor->ClientToScreen(wxPoint(anchor_size.x, anchor_size.y)));
    const wxRect bounds(anchor - wxPoint(side, side), wxSize(side, side));
    if (self.surface->GetRect() != bounds) self.surface->SetSize(bounds);
    self.surface->Show();
    self.surface->Raise();
}

bool WorkbenchViewNavigator::present_surface(bool visible, bool enabled)
{
    auto& self = *m_impl;
    if (!self.surface) return true;
    self.input_enabled = enabled;
    show_surface(visible);
    if (!visible) return true;
    if (!self.surface->IsShownOnScreen() && self.preview) self.preview->Refresh(false);
    if (!self.surface->IsShownOnScreen() || !self.gl_context || !self.rotation) return false;
    const auto size = self.surface->GetClientSize();
    if (size.x <= 0 || size.y <= 0 || !self.surface->SetCurrent(*self.gl_context) || !wxGetApp().init_opengl())
        return false;
    int width = size.x, height = size.y;
#if defined(__APPLE__)
    width = int(std::lround(width * self.surface->GetDPIScaleFactor()));
    height = int(std::lround(height * self.surface->GetDPIScaleFactor()));
#endif
    ::glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ::glViewport(0, 0, width, height);
    ::glDisable(GL_SCISSOR_TEST);
    ::glDisable(GL_DEPTH_TEST);
    ::glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    ::glClearColor(49.f/255, 49.f/255, 54.f/255, 1.f);
    ::glClear(GL_COLOR_BUFFER_BIT);
    auto rotation = self.rotation();
    const bool active = begin(rotation, size, self.surface->GetDPIScaleFactor(), true, enabled);
    render();
    const bool submitted = self.surface->SwapBuffers();
    if (active) {
        if (self.changed) self.changed(rotation);
        self.surface->Refresh(false);
    }
    return submitted;
}

} // namespace Slic3r::GUI
