#pragma once

#include "RedesignTheme.hpp"
#include <chrono>
#include <cstdint>
#include <wx/panel.h>
#include <wx/utils.h>
#ifdef __WXMSW__
#include <glad/gl.h>
#include <boost/log/trivial.hpp>
#include <wx/glcanvas.h>
#include <wx/msw/wrapwin.h>
#include <wx/msw/wrapcctl.h>
#include <vector>
#endif

namespace Slic3r::GUI {

inline std::uint64_t& redesign_restore_snapshot_count()
{ static thread_local std::uint64_t count=0; return count; }

inline bool is_redesign_workbench_surface(const wxWindow* window)
{
    for (auto* ancestor = window; ancestor; ancestor = ancestor->GetParent())
        if (ancestor->GetName() == "ai_workbench_surface") return true;
    return false;
}

// Buffered controls still need an opaque surface while Windows restores their
// native windows, before the first WM_PAINT has populated the backing pixels.
class RedesignPaintPanel : public wxPanel {
public:
    RedesignPaintPanel(wxWindow* parent, wxWindowID id = wxID_ANY,
                       const wxPoint& position = wxDefaultPosition,
                       const wxSize& size = wxDefaultSize,
                       long style = wxTAB_TRAVERSAL)
        : wxPanel(parent, id, position, size, style)
    {
        SetBackgroundColour(RedesignTheme::panel_colour());
    }

    void preserve_restore_colour() { m_preserve_restore_colour = true; }
    bool preserves_restore_colour() const { return m_preserve_restore_colour; }
    void paint_owns_background(bool value=true) { m_paint_owns_background=value; }
    std::uint64_t background_erase_count() const { return m_background_erases; }

#ifdef __WXMSW__
    WXLRESULT MSWWindowProc(WXUINT message, WXWPARAM wparam, WXLPARAM lparam) override
    {
        if (message == WM_ERASEBKGND && wparam && GetBackgroundStyle() == wxBG_STYLE_PAINT) {
            ++m_background_erases;
            // This opt-in surface paints its complete background in one
            // buffered WM_PAINT; immediate erasing would expose a grey frame.
            if (m_paint_owns_background) return 1;
            const auto colour = GetBackgroundColour();
            const HBRUSH brush = ::CreateSolidBrush(RGB(colour.Red(), colour.Green(), colour.Blue()));
            RECT bounds;
            ::GetClientRect(GetHandle(), &bounds);
            ::FillRect(reinterpret_cast<HDC>(wparam), &bounds, brush);
            ::DeleteObject(brush);
            return 1;
        }
        return wxPanel::MSWWindowProc(message, wparam, lparam);
    }
#endif
private:
    bool m_preserve_restore_colour {false};
    bool m_paint_owns_background {false};
    std::uint64_t m_background_erases {0};
};

inline wxColour redesign_restore_background_colour(wxWindow* window)
{
    const auto colour = window->GetBackgroundColour();
    const auto* painted = dynamic_cast<RedesignPaintPanel*>(window);
    if (colour.IsOk() && (colour.GetLuminance() <= 0.5 ||
        (painted && painted->preserves_restore_colour()))) return colour;
    return RedesignTheme::background_colour();
}

// DWM requests client pixels before restoring native child windows. Keep the
// complete presented surface: wx 3.3 owner-painted controls create their own
// paint DC even when WM_PAINT receives an external DC. Forwarding that message
// therefore leaves the snapshot empty. Read back only for a window transition.
#ifdef __WXMSW__
struct RedesignRestoreSurface {
    wxWindow* window;
    HGLRC context {nullptr};
    wxSize frame_size;
    std::vector<unsigned char> pixels;

    bool is_viewport() const { return dynamic_cast<wxGLCanvas*>(window) != nullptr; }

    void retain_control_frame()
    {
        // A container DC may include child pixels. Replaying it after layout
        // would leave old labels outside their new bounds; children retain
        // their own frames and the container keeps its opaque background.
        if (!window->GetChildren().empty()) {
            pixels.clear();
            return;
        }
        const HWND hwnd = window->GetHandle();
        const auto size = window->GetClientSize();
        if (size.x <= 0 || size.y <= 0 || size_t(size.x) * size_t(size.y) > 64u * 1024u * 1024u)
            return;
        // A composited control's own DC can still contain the system's empty
        // white surface. Capture its presented pixels while our root owns the
        // foreground, clipping to every ancestor so hidden/offscreen areas
        // never borrow pixels from another application.
        if (::GetForegroundWindow() != ::GetAncestor(hwnd, GA_ROOT)) {
            pixels.clear();
            return;
        }
        POINT origin {0, 0};
        ::ClientToScreen(hwnd, &origin);
        RECT visible {origin.x, origin.y, origin.x + size.x, origin.y + size.y};
        for (HWND ancestor = ::GetParent(hwnd); ancestor; ancestor = ::GetParent(ancestor)) {
            RECT client;
            ::GetClientRect(ancestor, &client);
            ::MapWindowPoints(ancestor, nullptr, reinterpret_cast<POINT*>(&client), 2);
            if (!::IntersectRect(&visible, &visible, &client)) {
                pixels.clear();
                return;
            }
        }
        const HDC source = ::GetDC(nullptr);
        if (!source) return;
        const HDC target = ::CreateCompatibleDC(source);
        BITMAPINFO info {};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = size.x;
        info.bmiHeader.biHeight = size.y;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        const HBITMAP bitmap = target ? ::CreateDIBSection(source, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
        if (bitmap) {
            const auto previous = ::SelectObject(target, bitmap);
            const auto background = redesign_restore_background_colour(window);
            const HBRUSH brush = ::CreateSolidBrush(RGB(background.Red(), background.Green(), background.Blue()));
            RECT bounds {0, 0, size.x, size.y};
            ::FillRect(target, &bounds, brush);
            ::DeleteObject(brush);
            if (::BitBlt(target, visible.left - origin.x, visible.top - origin.y,
                visible.right - visible.left, visible.bottom - visible.top,
                source, visible.left, visible.top, SRCCOPY)) {
                ::GdiFlush();
                const auto* first = static_cast<const unsigned char*>(bits);
                pixels.assign(first, first + size_t(size.x) * size_t(size.y) * 4);
                frame_size = size;
            }
            ::SelectObject(target, previous);
            ::DeleteObject(bitmap);
        }
        if (target) ::DeleteDC(target);
        ::ReleaseDC(nullptr, source);
    }

    void retain_frame()
    {
        // The workbench presents its native controls in one final layout pass
        // and retains its GL frame on the GPU. Screen copies can contain a
        // clipped handle and must never become a second presentation owner.
        if (is_redesign_workbench_surface(window)) {
            pixels.clear();
            return;
        }
        const HWND hwnd = window->GetHandle();
        if (!::IsWindowVisible(hwnd) || ::IsIconic(::GetAncestor(hwnd, GA_ROOT))) return;
        if (!is_viewport()) {
            retain_control_frame();
            return;
        }
        if (!context && ::WindowFromDC(::wglGetCurrentDC()) == hwnd)
            context = ::wglGetCurrentContext();
        if (!context ||
            !glad_glReadPixels || !glad_glBindBuffer) return;
        RECT bounds;
        ::GetClientRect(hwnd, &bounds);
        const int width = bounds.right, height = bounds.bottom;
        if (width <= 0 || height <= 0 || size_t(width) * size_t(height) > 64u * 1024u * 1024u) return;
        const HGLRC previous_context = ::wglGetCurrentContext();
        const HDC previous_dc = ::wglGetCurrentDC();
        const HDC dc = ::GetDC(hwnd);
        if (!dc) return;
        if (!::wglMakeCurrent(dc, context)) {
            ::ReleaseDC(hwnd, dc);
            return;
        }

        const bool arb = GLAD_GL_VERSION_3_0 || GLAD_GL_ARB_framebuffer_object;
        const bool ext = !arb && GLAD_GL_EXT_framebuffer_object;
        GLint framebuffer = 0;
        if (arb) {
            ::glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &framebuffer);
            ::glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        } else if (ext) {
            ::glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &framebuffer);
            ::glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
        }
        GLint alignment, row_length, skip_rows, skip_pixels, pack_buffer, read_buffer;
        ::glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
        ::glGetIntegerv(GL_PACK_ROW_LENGTH, &row_length);
        ::glGetIntegerv(GL_PACK_SKIP_ROWS, &skip_rows);
        ::glGetIntegerv(GL_PACK_SKIP_PIXELS, &skip_pixels);
        ::glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
        ::glGetIntegerv(GL_READ_BUFFER, &read_buffer);
        ::glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        ::glPixelStorei(GL_PACK_ALIGNMENT, 1);
        ::glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        ::glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        ::glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        for (int count = 0; count < 16; ++count) {
            const auto error = ::glGetError();
            if (error == GL_NO_ERROR) break;
            BOOST_LOG_TRIVIAL(warning) << "restore snapshot prior GL error: " << error;
        }
        ::glReadBuffer(GL_FRONT);
        std::vector<unsigned char> frame(size_t(width) * size_t(height) * 4);
        ::glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, frame.data());
        const auto error = ::glGetError();
        ::glReadBuffer(read_buffer);
        ::glPixelStorei(GL_PACK_ALIGNMENT, alignment);
        ::glPixelStorei(GL_PACK_ROW_LENGTH, row_length);
        ::glPixelStorei(GL_PACK_SKIP_ROWS, skip_rows);
        ::glPixelStorei(GL_PACK_SKIP_PIXELS, skip_pixels);
        ::glBindBuffer(GL_PIXEL_PACK_BUFFER, pack_buffer);
        if (arb) ::glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
        else if (ext) ::glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, framebuffer);
        ::wglMakeCurrent(previous_dc, previous_context);
        ::ReleaseDC(hwnd, dc);
        if (error != GL_NO_ERROR) {
            BOOST_LOG_TRIVIAL(warning) << "restore snapshot readback failed: " << error;
            return;
        }
        frame_size = wxSize(width, height);
        pixels = std::move(frame);
    }

    bool draw_frame(HDC dc) const
    {
        if (pixels.empty()) return false;
        const auto size = window->GetClientSize();
        if (size.x <= 0 || size.y <= 0) return false;
        if (!is_viewport() && size != frame_size) return false;
        BITMAPINFO bitmap {};
        bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmap.bmiHeader.biWidth = frame_size.x;
        bitmap.bmiHeader.biHeight = frame_size.y; // GL and DIB rows both start at the bottom.
        bitmap.bmiHeader.biPlanes = 1;
        bitmap.bmiHeader.biBitCount = 32;
        bitmap.bmiHeader.biCompression = BI_RGB;
        return ::StretchDIBits(dc, 0, 0, size.x, size.y, 0, 0, frame_size.x, frame_size.y,
            pixels.data(), &bitmap, DIB_RGB_COLORS, SRCCOPY) != GDI_ERROR;
    }
};

inline LRESULT CALLBACK redesign_restore_background_proc(HWND hwnd, UINT message, WPARAM parameter,
                                                         LPARAM data, UINT_PTR id, DWORD_PTR context)
{
    auto* state = reinterpret_cast<RedesignRestoreSurface*>(context);
    if (message == WM_NCDESTROY) {
        ::RemoveWindowSubclass(hwnd, redesign_restore_background_proc, id);
        delete state;
        return ::DefSubclassProc(hwnd, message, parameter, data);
    }
    auto* surface = state->window;
    if (message == WM_PAINT) {
        const auto result = ::DefSubclassProc(hwnd, message, parameter, data);
        // A restore snapshot is only a bridge to this first real control paint.
        // Replaying it on later erases can resurrect a clipped handle or text.
        state->pixels.clear();
        if (state->is_viewport() && ::WindowFromDC(::wglGetCurrentDC()) == hwnd)
            state->context = ::wglGetCurrentContext();
        return result;
    }
    if ((message == WM_ERASEBKGND || message == WM_PRINTCLIENT) && parameter &&
        !surface->IsBeingDeleted()) {
        if (state->draw_frame(reinterpret_cast<HDC>(parameter))) return 1;
        // A self-painted surface owns its background in WM_PAINT. In
        // particular, the startup buffer has no retained transition frame:
        // filling it here removes the logo until the next buffered paint.
        if (message == WM_ERASEBKGND && surface->GetBackgroundStyle() == wxBG_STYLE_PAINT)
            return 1;
        // Some owner-painted controls retain a system colour even though
        // their WM_PAINT draws a dark face. Explicit colour surfaces opt out.
        const auto colour = redesign_restore_background_colour(surface);
        const HBRUSH brush = ::CreateSolidBrush(RGB(colour.Red(), colour.Green(), colour.Blue()));
        RECT bounds;
        ::GetClientRect(hwnd, &bounds);
        ::FillRect(reinterpret_cast<HDC>(parameter), &bounds, brush);
        ::DeleteObject(brush);
        return 1;
    }
    return ::DefSubclassProc(hwnd, message, parameter, data);
}
#endif

inline void prepare_redesign_restore_background(wxWindow* window)
{
#ifdef __WXMSW__
    // Native containers and text controls also erase before their first paint.
    // Retain their presented content without changing normal WM_PAINT handling.
    if (!window || window->IsTopLevel()) return;
    const auto id = reinterpret_cast<UINT_PTR>(redesign_restore_background_proc);
    DWORD_PTR context;
    const HWND hwnd = window->GetHandle();
    if (!::GetWindowSubclass(hwnd, redesign_restore_background_proc, id, &context)) {
        auto* state = new RedesignRestoreSurface {window};
        if (!::SetWindowSubclass(hwnd, redesign_restore_background_proc, id, reinterpret_cast<DWORD_PTR>(state)))
            delete state;
    }
#endif
}

inline void retain_redesign_window_frames(wxWindow* root)
{
#ifdef __WXMSW__
    if (!root || !root->IsShownOnScreen()) return;
    // Workbench cards and their controls can be created after the last resize.
    // Register them before Windows starts assembling a transition snapshot.
    prepare_redesign_restore_background(root);
    DWORD_PTR context;
    const auto id = reinterpret_cast<UINT_PTR>(redesign_restore_background_proc);
    if (::GetWindowSubclass(root->GetHandle(), redesign_restore_background_proc, id, &context)) {
        auto* state = reinterpret_cast<RedesignRestoreSurface*>(context);
        state->retain_frame();
    }
    for (auto* child : root->GetChildren()) retain_redesign_window_frames(child);
#endif
}

// Both custom caption actions and native system commands can announce the
// same transition. Only its first entry may retain the native surfaces.
inline void retain_redesign_restore_frames_once(wxWindow* root)
{
#ifdef __WXMSW__
    constexpr auto property=L"HonorForge.RestoreFramesRetained";
    if (!root || ::GetPropW(root->GetHandle(),property)) return;
    if (!::SetPropW(root->GetHandle(),property,reinterpret_cast<HANDLE>(1))) return;
    ++redesign_restore_snapshot_count();
    const auto started=std::chrono::steady_clock::now();
    retain_redesign_window_frames(root);
    wxString trace;
    if (wxGetEnv("ORCASLICER_UI_LATENCY_TRACE",&trace) && trace=="1")
        BOOST_LOG_TRIVIAL(info) << "[UiRestoreSnapshot] captured=1 elapsed_ms="
            << std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
#endif
}

inline void release_redesign_restore_frames(wxWindow* root)
{
#ifdef __WXMSW__
    if (root) ::RemovePropW(root->GetHandle(),L"HonorForge.RestoreFramesRetained");
#endif
}

} // namespace Slic3r::GUI
