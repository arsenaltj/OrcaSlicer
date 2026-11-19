#pragma once

#include "RedesignTheme.hpp"
#include "RedesignRestorePixels.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <wx/panel.h>
#include <wx/utils.h>
#ifdef __WXMSW__
#include <glad/gl.h>
#include <boost/log/trivial.hpp>
#include <wx/glcanvas.h>
#include <wx/weakref.h>
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
inline bool redesign_restore_trace_enabled()
{
    wxString value;
    return wxGetEnv("ORCASLICER_UI_LATENCY_TRACE", &value) && value == "1";
}

struct RedesignRestoreCaptureMetrics {
    unsigned native_controls {0}, screen_copies {0}, capture_flushes {0}, gl_reads {0}, stale_controls {0};
    const char* skip_reason {"none"};
    std::size_t output_bytes {0}, bitmap_bytes {0};
    double native_ms {0}, prepare_ms {0}, capture_ms {0}, flush_ms {0}, copy_ms {0}, gl_ms {0};
};

struct RedesignRestoreStageTimer {
    double* elapsed;
    std::chrono::steady_clock::time_point started {std::chrono::steady_clock::now()};
    ~RedesignRestoreStageTimer() {
        if (elapsed) *elapsed += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
    }
};

struct RedesignRestoreGdiApi {
    using DC = HDC;
    using Bitmap = HBITMAP;
    using Object = HGDIOBJ;
    DC screen() const { return ::GetDC(nullptr); }
    DC memory(DC source) const { return ::CreateCompatibleDC(source); }
    Bitmap bitmap(DC source, int width, int height, void** bits) const {
        BITMAPINFO info {};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        return ::CreateDIBSection(source, &info, DIB_RGB_COLORS, bits, nullptr, 0);
    }
    Object select(DC dc, Object bitmap) const { return ::SelectObject(dc, bitmap); }
    bool valid_selection(Object value) const { return value && value != HGDI_ERROR; }
    bool copy(DC target, DC source, const RestoreCapture::Bounds& bounds) const {
        return ::BitBlt(target, 0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top,
                        source, bounds.left, bounds.top, SRCCOPY) != FALSE;
    }
    bool flush() const { return ::GdiFlush() != FALSE; }
    void delete_bitmap(Bitmap value) const { ::DeleteObject(value); }
    void delete_dc(DC value) const { ::DeleteDC(value); }
    void release_screen(DC value) const { ::ReleaseDC(nullptr, value); }
};

inline RestoreCapture::FrameIdentity redesign_native_frame_identity(wxWindow* window)
{
    if (!window || window->IsBeingDeleted()) return {};
    const HWND hwnd = window->GetHandle();
    RECT client {};
    if (!hwnd || !::IsWindow(hwnd) || !::GetClientRect(hwnd, &client)) return {};
    const auto dpi = window->GetDPI();
    return {reinterpret_cast<std::uintptr_t>(hwnd), reinterpret_cast<std::uintptr_t>(::GetParent(hwnd)),
        reinterpret_cast<std::uintptr_t>(::GetAncestor(hwnd, GA_ROOT)), client.right, client.bottom, dpi.x, dpi.y};
}

inline bool redesign_screen_client(HWND hwnd, RestoreCapture::Bounds& bounds)
{
    RECT client {};
    POINT origin {};
    if (!::GetClientRect(hwnd, &client) || !::ClientToScreen(hwnd, &origin)) return false;
    if (!RestoreCapture::make_bounds(origin.x, origin.y, client.right, client.bottom, bounds)) return false;
    std::size_t bytes = 0;
    return RestoreCapture::rgba_bytes(bounds, bytes);
}

struct RedesignRestoreSurface {
    wxWindow* window;
    HGLRC context {nullptr};
    wxSize frame_size;
    RestoreCapture::FrameIdentity native_identity;
    std::vector<unsigned char> pixels;

    bool is_viewport() const { return dynamic_cast<wxGLCanvas*>(window) != nullptr; }

    void retain_frame(RedesignRestoreCaptureMetrics* metrics = nullptr)
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
        if (!is_viewport()) return;
        RedesignRestoreStageTimer timer {metrics ? &metrics->gl_ms : nullptr};
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
        if (metrics) ++metrics->gl_reads;
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

    bool draw_frame(HDC dc)
    {
        if (pixels.empty()) return false;
        const auto size = window->GetClientSize();
        if (size.x <= 0 || size.y <= 0) return false;
        if (!is_viewport() && (!::IsWindowVisible(window->GetHandle()) ||
            !native_identity.matches(redesign_native_frame_identity(window)))) {
            pixels.clear();
            return false;
        }
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

inline void discard_redesign_native_frames(wxWindow* root);

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
    // Hiding a page invalidates its native pixels, including descendants.
    // Iconizing the main window keeps its last complete frame for restoration.
    if (message == WM_SHOWWINDOW && !parameter && !::IsIconic(::GetAncestor(hwnd, GA_ROOT)) &&
        !::GetPropW(::GetAncestor(hwnd, GA_ROOT), L"HonorForge.Iconized"))
        discard_redesign_native_frames(surface);
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

#ifdef __WXMSW__
struct RedesignNativeRestoreTarget {
    wxWeakRef<wxWindow> window;
    DWORD_PTR state;
    RestoreCapture::FrameIdentity identity;
    RestoreCapture::Region region;
    std::uint32_t background;
};

inline RedesignRestoreSurface* redesign_current_restore_surface(const RedesignNativeRestoreTarget& target)
{
    auto* window = target.window.get();
    if (!window || !target.identity.matches(redesign_native_frame_identity(window)) || !window->IsShownOnScreen())
        return nullptr;
    DWORD_PTR context = 0;
    const auto id = reinterpret_cast<UINT_PTR>(redesign_restore_background_proc);
    if (!::GetWindowSubclass(window->GetHandle(), redesign_restore_background_proc, id, &context) || context != target.state)
        return nullptr;
    return reinterpret_cast<RedesignRestoreSurface*>(context);
}

inline void collect_redesign_restore_targets(wxWindow* root, std::vector<RedesignNativeRestoreTarget>& native,
    std::vector<wxWeakRef<wxWindow>>& viewports, RestoreCapture::Plan& plan,
    RedesignRestoreCaptureMetrics* metrics)
{
    if (!root || !root->IsShownOnScreen()) return;
    prepare_redesign_restore_background(root);
    DWORD_PTR context = 0;
    const auto id = reinterpret_cast<UINT_PTR>(redesign_restore_background_proc);
    if (::GetWindowSubclass(root->GetHandle(), redesign_restore_background_proc, id, &context)) {
        auto* state = reinterpret_cast<RedesignRestoreSurface*>(context);
        if (is_redesign_workbench_surface(root)) state->pixels.clear();
        else if (state->is_viewport()) viewports.emplace_back(root);
        else if (!root->GetChildren().empty()) state->pixels.clear();
        else {
            RedesignNativeRestoreTarget target {root, context, redesign_native_frame_identity(root), {}, 0};
            const HWND hwnd = root->GetHandle();
            bool eligible = target.identity.matches(target.identity) &&
                ::GetForegroundWindow() == ::GetAncestor(hwnd, GA_ROOT) &&
                redesign_screen_client(hwnd, target.region.client);
            target.region.visible = target.region.client;
            for (HWND ancestor = ::GetParent(hwnd); eligible && ancestor; ancestor = ::GetParent(ancestor)) {
                RestoreCapture::Bounds client;
                eligible = redesign_screen_client(ancestor, client);
                if (eligible) target.region.visible = RestoreCapture::intersect(target.region.visible, client);
            }
            const auto screen_left = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
            const auto screen_top = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
            RestoreCapture::Bounds screen;
            eligible = eligible && RestoreCapture::make_bounds(screen_left, screen_top,
                ::GetSystemMetrics(SM_CXVIRTUALSCREEN), ::GetSystemMetrics(SM_CYVIRTUALSCREEN), screen);
            target.region.visible = RestoreCapture::intersect(target.region.visible, screen);
            if (eligible && RestoreCapture::contains(target.region.client, target.region.visible)) {
                const auto colour = redesign_restore_background_colour(root);
                target.background = std::uint32_t(colour.Blue()) | (std::uint32_t(colour.Green()) << 8) |
                    (std::uint32_t(colour.Red()) << 16);
                plan.include(target.region, state->pixels.capacity());
                native.push_back(std::move(target));
                if (metrics) ++metrics->native_controls;
            } else state->pixels.clear();
        }
    }
    for (auto* child : root->GetChildren()) collect_redesign_restore_targets(child, native, viewports, plan, metrics);
}

inline void discard_redesign_native_frames(wxWindow* root)
{
    if (!root || !root->GetHandle() || ::IsIconic(::GetAncestor(root->GetHandle(), GA_ROOT)) ||
        ::GetPropW(::GetAncestor(root->GetHandle(), GA_ROOT), L"HonorForge.Iconized")) return;
    DWORD_PTR context = 0;
    const auto id = reinterpret_cast<UINT_PTR>(redesign_restore_background_proc);
    if (::GetWindowSubclass(root->GetHandle(), redesign_restore_background_proc, id, &context)) {
        auto* state = reinterpret_cast<RedesignRestoreSurface*>(context);
        if (!state->is_viewport()) state->pixels.clear();
    }
    for (auto* child : root->GetChildren()) discard_redesign_native_frames(child);
}

inline bool capture_redesign_native_batch(wxWindow* root, const std::vector<RedesignNativeRestoreTarget>& targets,
    const RestoreCapture::Plan& plan, RedesignRestoreCaptureMetrics* metrics)
{
    const auto fail = [metrics](const char* reason) { if (metrics) metrics->skip_reason = reason; return false; };
    if (metrics) {
        metrics->bitmap_bytes = plan.bitmap_bytes;
        metrics->output_bytes = plan.output_bytes;
    }
    if (plan.error != RestoreCapture::PlanError::None) return fail("budget_or_region");
    if (targets.empty()) return fail("no_native_controls");
    if (::GetForegroundWindow() != root->GetHandle()) return fail("not_foreground");
    RedesignRestoreGdiApi api;
    RestoreCapture::ScreenBitmap<RedesignRestoreGdiApi> bitmap(api);
    {
        RedesignRestoreStageTimer timer {metrics ? &metrics->prepare_ms : nullptr};
        if (!bitmap.prepare(plan.capture)) return fail("gdi_prepare");
    }
    if (metrics) {
        ++metrics->screen_copies;
    }
    {
        RedesignRestoreStageTimer timer {metrics ? &metrics->capture_ms : nullptr};
        if (!bitmap.copy()) return fail("bitblt");
    }
    if (metrics) ++metrics->capture_flushes;
    {
        RedesignRestoreStageTimer timer {metrics ? &metrics->flush_ms : nullptr};
        if (!bitmap.flush()) return fail("gdi_flush");
    }
    RedesignRestoreStageTimer timer {metrics ? &metrics->copy_ms : nullptr};
    for (const auto& target : targets) {
        auto* state = redesign_current_restore_surface(target);
        RestoreCapture::Bounds client;
        if (!state || ::GetForegroundWindow() != root->GetHandle() ||
            !redesign_screen_client(target.window->GetHandle(), client) || !(client == target.region.client)) {
            if (target.window) discard_redesign_native_frames(target.window.get());
            if (metrics) ++metrics->stale_controls;
            continue;
        }
        if (!RestoreCapture::copy_region(bitmap.data(), plan.bitmap_bytes, plan.capture,
                                         target.region, target.background, state->pixels)) return fail("pixel_copy");
        state->frame_size = {target.identity.width, target.identity.height};
        state->native_identity = target.identity;
    }
    return true;
}
#endif

inline void retain_redesign_window_frames(wxWindow* root
#ifdef __WXMSW__
    , RedesignRestoreCaptureMetrics* metrics = nullptr
#endif
)
{
#ifdef __WXMSW__
    // Restoring an iconic window must not collect an invisible desktop or
    // clear its valid pre-minimize pixels. The existing presenter repaints it.
    if (!root || !root->GetHandle()) return;
    if (::IsIconic(::GetAncestor(root->GetHandle(), GA_ROOT)) ||
        ::GetPropW(::GetAncestor(root->GetHandle(), GA_ROOT), L"HonorForge.Iconized")) {
        if (metrics) metrics->skip_reason = "iconic";
        return;
    }
    if (!root->IsShownOnScreen()) {
        discard_redesign_native_frames(root);
        if (metrics) metrics->skip_reason = "hidden";
        return;
    }
    std::vector<RedesignNativeRestoreTarget> native;
    std::vector<wxWeakRef<wxWindow>> viewports;
    RestoreCapture::Plan plan;
    try {
        collect_redesign_restore_targets(root, native, viewports, plan, metrics);
        RedesignRestoreStageTimer timer {metrics ? &metrics->native_ms : nullptr};
        if (!capture_redesign_native_batch(root, native, plan, metrics))
            discard_redesign_native_frames(root);
    } catch (const std::exception&) {
        discard_redesign_native_frames(root);
        if (metrics) metrics->skip_reason = "native_exception";
    }
    // GL keeps its original context/readback/restore ownership. It does not
    // borrow the batch DIB or participate in native pixel allocation.
    for (const auto& weak : viewports) {
        if (auto* window = weak.get()) {
            DWORD_PTR context = 0;
            const auto id = reinterpret_cast<UINT_PTR>(redesign_restore_background_proc);
            if (::GetWindowSubclass(window->GetHandle(), redesign_restore_background_proc, id, &context))
                reinterpret_cast<RedesignRestoreSurface*>(context)->retain_frame(metrics);
        }
    }
#endif
}

// Both custom caption actions and native system commands can announce the
// same transition. Only its first entry may retain the native surfaces.
inline void retain_redesign_restore_frames_once(wxWindow* root, const char* action = "unspecified", const char* source = "unspecified")
{
#ifdef __WXMSW__
    constexpr auto property=L"HonorForge.RestoreFramesRetained";
    if (!root) return;
    const bool trace = redesign_restore_trace_enabled();
    if (::GetPropW(root->GetHandle(),property)) {
        if (trace) BOOST_LOG_TRIVIAL(info) << "[UiRestoreSnapshot] id=" << redesign_restore_snapshot_count()
            << " action=" << action << " source=" << source << " duplicate=1 captured=0";
        return;
    }
    if (!::SetPropW(root->GetHandle(),property,reinterpret_cast<HANDLE>(1))) return;
    ++redesign_restore_snapshot_count();
    const auto started=std::chrono::steady_clock::now();
    RedesignRestoreCaptureMetrics metrics;
    retain_redesign_window_frames(root, trace ? &metrics : nullptr);
    if (trace) {
        const auto elapsed = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
        BOOST_LOG_TRIVIAL(info) << "[UiRestoreSnapshot] id=" << redesign_restore_snapshot_count()
            << " action=" << action << " source=" << source << " mode=batch duplicate=0"
            << " native_controls=" << metrics.native_controls << " screen_copies=" << metrics.screen_copies
            << " capture_flushes=" << metrics.capture_flushes << " gl_reads=" << metrics.gl_reads
            << " stale_controls=" << metrics.stale_controls << " skip_reason=" << metrics.skip_reason
            << " prepare_ms=" << metrics.prepare_ms << " capture_ms=" << metrics.capture_ms
            << " capture_flush_ms=" << metrics.flush_ms << " copy_ms=" << metrics.copy_ms
            << " gl_ms=" << metrics.gl_ms << " native_ms=" << metrics.native_ms
            << " collect_ms=" << std::max(0., elapsed - metrics.native_ms - metrics.gl_ms)
            << " output_bytes=" << metrics.output_bytes << " bitmap_bytes=" << metrics.bitmap_bytes
            << " peak_bytes=" << metrics.output_bytes + metrics.bitmap_bytes
            << " elapsed_ms=" << elapsed;
    }
#endif
}

inline void release_redesign_restore_frames(wxWindow* root)
{
#ifdef __WXMSW__
    if (root) ::RemovePropW(root->GetHandle(),L"HonorForge.RestoreFramesRetained");
#endif
}

} // namespace Slic3r::GUI
