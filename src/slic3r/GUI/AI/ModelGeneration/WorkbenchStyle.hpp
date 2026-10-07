#pragma once

#include <wx/panel.h>
#include <wx/dcbuffer.h>
#include <wx/dcmemory.h>
#include <wx/dcgraph.h>
#include <wx/slider.h>
#include <wx/tglbtn.h>
#include <wx/scrolwin.h>
#include <wx/weakref.h>
#include <algorithm>
#include <cmath>
#include <vector>
#ifdef __WXMSW__
#include <wx/msw/dc.h>
#include <wx/msw/wrapcctl.h>
#endif
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/Widgets/ComboBox.hpp"

namespace Slic3r::GUI {

// Reused result-page controls retain their original appearance after a
// temporary workbench dialog closes. Color swatches keep their actual colors.
class WorkbenchAppearanceScope final {
public:
    explicit WorkbenchAppearanceScope(wxWindow* root) { apply(root); }
    ~WorkbenchAppearanceScope() {
        for (const auto& saved : m_saved) {
            if (!saved.window) continue;
            saved.window->SetName(saved.name);
            saved.window->SetBackgroundColour(saved.background);
            saved.window->SetForegroundColour(saved.foreground);
            saved.window->Refresh(false);
        }
    }
    WorkbenchAppearanceScope(const WorkbenchAppearanceScope&) = delete;
    WorkbenchAppearanceScope& operator=(const WorkbenchAppearanceScope&) = delete;
    void include(wxWindow* window) {
        const bool tracked = std::any_of(m_saved.begin(), m_saved.end(),
            [window](const auto& saved) { return saved.window == window; });
        if (tracked) {
            const wxColour background(32, 32, 35), foreground(235, 235, 235);
            if (window->GetBackgroundColour() != background || window->GetForegroundColour() != foreground) {
                window->SetBackgroundColour(background);
                window->SetForegroundColour(foreground);
                window->Refresh(false);
            }
            for (wxWindow* child : window->GetChildren()) include(child);
        } else if (window->GetName() != "ai_content_color") {
            apply(window);
            window->Refresh(false);
        }
    }
private:
    struct Appearance {
        wxWeakRef<wxWindow> window;
        wxString name;
        wxColour background;
        wxColour foreground;
    };
    void apply(wxWindow* window) {
        if (window->GetName() == "ai_content_color") return;
        m_saved.push_back({window, window->GetName(), window->GetBackgroundColour(), window->GetForegroundColour()});
        window->SetName("ai_content_color");
        window->SetBackgroundColour(wxColour(32, 32, 35));
        window->SetForegroundColour(wxColour(235, 235, 235));
        for (wxWindow* child : window->GetChildren()) apply(child);
    }
    std::vector<Appearance> m_saved;
};

class WorkbenchScrolledWindow final : public wxScrolledWindow {
public:
    WorkbenchScrolledWindow(wxWindow* parent, wxWindowID id, const wxPoint& position,
                            const wxSize& size, long style)
        : wxScrolledWindow(parent, id, position, size, style | wxCLIP_CHILDREN) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(GetBackgroundColour()));
            dc.Clear();
        });
        ShowScrollbars(wxSHOW_SB_NEVER, wxSHOW_SB_NEVER);
        m_bar = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        m_bar->SetName("ai_content_color");
        m_bar->SetBackgroundStyle(wxBG_STYLE_PAINT);
        m_bar->SetCursor(wxCURSOR_HAND);
        m_bar->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(m_bar);
            dc.SetBackground(wxBrush(GetBackgroundColour())); dc.Clear();
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(wxColour(105, 105, 112)));
            const auto thumb = thumb_rect();
            dc.DrawRoundedRectangle(thumb, FromDIP(2));
        });
        m_bar->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) {
            const auto thumb = thumb_rect();
            if (thumb.Contains(event.GetPosition())) {
                m_drag_offset = event.GetY() - thumb.y;
                m_bar->CaptureMouse();
            } else {
                int x, y, unit_x, unit_y;
                GetViewStart(&x, &y); GetScrollPixelsPerUnit(&unit_x, &unit_y);
                const int page = GetClientSize().y / std::max(1, unit_y);
                Scroll(-1, std::max(0, y + (event.GetY() < thumb.y ? -page : page)));
                update_bar();
            }
        });
        m_bar->Bind(wxEVT_MOTION, [this](wxMouseEvent& event) {
            if (!m_bar->HasCapture()) return;
            const int travel = m_bar->GetClientSize().y - thumb_rect().height;
            int unit_x, unit_y; GetScrollPixelsPerUnit(&unit_x, &unit_y);
            const int range = std::max(0, GetVirtualSize().y - GetClientSize().y);
            const int position = std::clamp(event.GetY() - m_drag_offset, 0, std::max(0, travel));
            Scroll(-1, travel > 0 ? int(double(position) * range / travel / std::max(1, unit_y) + .5) : 0);
            update_bar();
        });
        m_bar->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
            if (m_bar->HasCapture()) m_bar->ReleaseMouse();
        });
        m_bar->Bind(wxEVT_MOUSE_CAPTURE_LOST, [](wxMouseCaptureLostEvent&) {});
        m_bar->Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& event) {
            event.Skip(false);
            wxMouseEvent forwarded(event); forwarded.SetEventObject(this);
            GetEventHandler()->ProcessEvent(forwarded);
        });
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { update_bar(); update_shape(); event.Skip(); });
    }
    void set_rounded_corners(bool rounded) {
        m_rounded = rounded;
        update_shape();
    }
    bool Layout() override {
        if (m_laying_out) return true;
        if (!GetSizer()) return wxScrolledWindow::Layout();
        // Updating virtual size can synchronously request another layout.
        struct LayoutGuard {
            bool& active;
            explicit LayoutGuard(bool& value) : active(value) { active = true; }
            ~LayoutGuard() { active = false; }
        } guard(m_laying_out);
        const auto client = GetClientSize();
        const int height = std::max(client.y, GetSizer()->CalcMin().y);
        if (GetVirtualSize() != wxSize(client.x, height)) SetVirtualSize(client.x, height);
        int x, y, unit_x, unit_y;
        GetViewStart(&x, &y); GetScrollPixelsPerUnit(&unit_x, &unit_y);
        GetSizer()->SetDimension(-x * unit_x, -y * unit_y,
                                std::max(0, client.x - FromDIP(10)), height);
        update_bar();
        refresh_controls();
        return true;
    }
    void FitInside() override { Layout(); }
    void ScrollWindow(int dx, int dy, const wxRect* rect = nullptr) override {
        wxScrolledWindow::ScrollWindow(dx, dy, rect);
        // The indicator belongs to the viewport, not the scrolling content.
        update_bar();
        refresh_controls();
    }
private:
    void update_shape() {
#ifdef __WXMSW__
        const wxSize size = GetClientSize();
        const int radius = m_rounded ? FromDIP(9) : 0;
        if (size == m_shape_size && radius == m_shape_radius) return;
        if (size.x <= 0 || size.y <= 0) return;
        m_shape_size = size; m_shape_radius = radius;
        HRGN region = radius ? ::CreateRoundRectRgn(0, 0, size.x + 1, size.y + 1, 2 * radius, 2 * radius) : nullptr;
        // Windows owns the region after a successful call, including child clipping.
        if (!::SetWindowRgn(GetHwnd(), region, TRUE)) {
            if (region) ::DeleteObject(region);
            m_shape_size = wxDefaultSize; m_shape_radius = -1;
        }
#endif
    }
    void refresh_controls() {
        // Native trackbars can retain stale pixels after scrolled layout moves.
        Refresh(false);
        refresh_descendants(this);
    }
    static void refresh_descendants(wxWindow* window) {
        for (wxWindow* child : window->GetChildren()) {
            child->Refresh(false);
            refresh_descendants(child);
        }
    }
    wxRect thumb_rect() const {
        const int height = m_bar->GetClientSize().y;
        const int virtual_height = std::max(1, GetVirtualSize().y);
        const int visible_height = GetClientSize().y;
        const int thumb_height = std::min(height, std::max(FromDIP(24), height * visible_height / virtual_height));
        int x, y, unit_x, unit_y;
        GetViewStart(&x, &y); GetScrollPixelsPerUnit(&unit_x, &unit_y);
        const int range = std::max(1, virtual_height - visible_height);
        const int top = std::clamp(int(double(y) * unit_y * (height - thumb_height) / range), 0, std::max(0, height - thumb_height));
        return wxRect(FromDIP(3), top, FromDIP(4), thumb_height);
    }
    void update_bar() {
        if (!m_bar) return;
        const auto size = GetClientSize();
        const int width = FromDIP(10);
        m_bar->SetSize(std::max(0, size.x - width), 0, width, std::max(0, size.y));
        m_bar->Show(GetVirtualSize().y > size.y);
        m_bar->Raise(); m_bar->Refresh(false);
    }
    wxPanel* m_bar {nullptr};
    int m_drag_offset {0};
    bool m_laying_out {false};
    bool m_rounded {false};
#ifdef __WXMSW__
    wxSize m_shape_size {wxDefaultSize};
    int m_shape_radius {-1};
#endif
};

class WorkbenchSwitch final : public wxBitmapToggleButton {
public:
    WorkbenchSwitch(wxWindow* parent, const wxString& name)
        : wxBitmapToggleButton(parent, wxID_ANY, wxNullBitmap, wxDefaultPosition,
                               wxDefaultSize, wxBORDER_NONE | wxBU_EXACTFIT) {
        SetName(name);
        SetLabel(name);
        SetToolTip(name);
        SetBackgroundColour(parent->GetBackgroundColour());
        Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& event) { rescale_workbench(); event.Skip(); });
        Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& event) { rescale_workbench(); event.Skip(); });
        rescale_workbench();
    }
    void SetValue(bool value) override {
        wxBitmapToggleButton::SetValue(value);
        rescale_workbench();
    }
    bool Enable(bool enabled = true) override {
        const bool changed = wxBitmapToggleButton::Enable(enabled);
        if (changed) rescale_workbench();
        return changed;
    }
    void rescale_workbench() {
        const wxSize size = FromDIP(wxSize(36, 24));
        wxBitmap bitmap(size.x, size.y);
        wxMemoryDC memory(bitmap);
        memory.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
        memory.Clear();
        {
            wxGCDC dc(memory);
            const wxRect track(FromDIP(2), FromDIP(3), FromDIP(32), FromDIP(18));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(!IsEnabled() ? wxColour(64, 64, 69)
                : GetValue() ? wxColour(61, 127, 255) : wxColour(98, 98, 104)));
            dc.DrawRoundedRectangle(track, track.height / 2.);
            dc.SetBrush(wxBrush(IsEnabled() ? wxColour(255, 255, 255) : wxColour(135, 135, 141)));
            const int radius = FromDIP(7);
            dc.DrawCircle(wxPoint(GetValue() ? track.GetRight() - FromDIP(9) + 1
                                            : track.x + FromDIP(9),
                                  track.y + track.height / 2), radius);
            if (HasFocus()) {
                dc.SetPen(wxPen(wxColour(235, 235, 235), FromDIP(1)));
                dc.SetBrush(*wxTRANSPARENT_BRUSH);
                dc.DrawRoundedRectangle(0, 0, size.x - 1, size.y - 1, FromDIP(4));
            }
        }
        memory.SelectObject(wxNullBitmap);
#ifdef __WXMSW__
        bitmap.SetScaleFactor(GetDPIScaleFactor());
#endif
        SetBitmap(bitmap);
        SetBitmapPressed(bitmap);
        SetBitmapCurrent(bitmap);
        SetBitmapFocus(bitmap);
        SetBitmapDisabled(bitmap);
        SetMinSize(size);
        SetMaxSize(size);
        Refresh(false);
    }
};

class WorkbenchSlider final : public wxSlider {
public:
    WorkbenchSlider(wxWindow* parent, wxWindowID id, int value, int minimum, int maximum,
                    const wxColour& accent = wxColour(255, 194, 39),
                    std::string thumb_asset = {})
        : wxSlider(parent, id, value, minimum, maximum), m_accent(accent), m_thumb_asset(std::move(thumb_asset)) {
        SetBackgroundColour(parent->GetBackgroundColour());
    }
    bool HasTransparentBackground() override { return false; }
    void SetValue(int value) override {
        wxSlider::SetValue(value);
        Refresh(false);
    }
#ifdef __WXMSW__
protected:
    bool MSWOnScroll(int orientation, WXWORD parameter, WXWORD position, WXHWND control) override {
        const bool handled = wxSlider::MSWOnScroll(orientation, parameter, position, control);
        // Native trackbars invalidate only the thumb; our filled track changes too.
        Refresh(false);
        return handled;
    }
    bool MSWOnNotify(int id, WXLPARAM parameter, WXLPARAM* result) override {
        auto* draw = reinterpret_cast<NMCUSTOMDRAW*>(parameter);
        if (draw->hdr.code != NM_CUSTOMDRAW)
            return wxSlider::MSWOnNotify(id, parameter, result);
        if (draw->dwDrawStage == CDDS_PREPAINT) {
            wxDCTemp dc(draw->hdc, GetClientSize());
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(GetBackgroundColour()));
            dc.DrawRectangle(GetClientRect());
            // Paint the complete track in one stage: partial native item
            // notifications can omit the channel after parent scrolling.
            RECT channel {}, thumb {};
            ::SendMessage(GetHwnd(), TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channel));
            ::SendMessage(GetHwnd(), TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
            const wxColour accent = IsEnabled() ? m_accent : wxColour(105, 105, 110);
            {
                const auto& rect = channel;
                const int height = std::max(1, int(std::lround(FromDIP(9) / 2.)));
                const int y = (rect.top + rect.bottom - height) / 2;
                dc.SetBrush(wxBrush(wxColour(92, 92, 98)));
                dc.DrawRoundedRectangle(rect.left, y, rect.right - rect.left, height, height / 2.);
                dc.SetBrush(wxBrush(accent));
                const int width = (rect.right - rect.left) * (GetValue() - GetMin()) / (GetMax() - GetMin());
                if (width > 0) dc.DrawRoundedRectangle(rect.left, y, width, height, height / 2.);
            }
            {
                const auto& rect = thumb;
                dc.SetBrush(wxBrush(IsEnabled() ? wxColour(255, 255, 255) : wxColour(135, 135, 141)));
                const int radius = FromDIP(7);
                const wxPoint center((rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2);
                if (m_thumb_asset.empty()) dc.DrawCircle(center, radius);
                else {
                    const int edge = FromDIP(20);
                    if (!m_thumb_bitmap.IsOk() || m_thumb_edge != edge) {
                        m_thumb_bitmap = ScalableBitmap(this, m_thumb_asset, 20).bmp();
                        m_thumb_edge = edge;
                    }
                    dc.DrawBitmap(m_thumb_bitmap, center.x - edge / 2, center.y - edge / 2, true);
                }
                if (HasFocus()) {
                    dc.SetBrush(*wxTRANSPARENT_BRUSH);
                    dc.SetPen(wxPen(wxColour(235, 235, 235), FromDIP(1)));
                    dc.DrawCircle(center, radius + FromDIP(2));
                }
            }
            *result = CDRF_SKIPDEFAULT;
            return true;
        }
        return wxSlider::MSWOnNotify(id, parameter, result);
    }
    WXHBRUSH DoMSWControlColor(WXHDC dc, wxColour, WXHWND window) override {
        // wxSlider caches a parent-themed background across reparenting.
        // Use this control's brush while preserving native input/accessibility.
        return wxControl::DoMSWControlColor(dc, GetBackgroundColour(), window);
    }
#endif
private:
    wxColour m_accent;
    std::string m_thumb_asset;
    wxBitmap m_thumb_bitmap;
    int m_thumb_edge {0};
};

inline void bind_workbench_choice_escape(ComboBox* choice)
{
    const auto dismiss = [choice](wxKeyEvent& event) {
        if (event.GetKeyCode() == WXK_ESCAPE && choice->is_drop_down()) {
            choice->DismissDropdown();
            choice->SetFocus();
        } else {
            event.Skip();
        }
    };
    // Mouse and keyboard opening can focus different windows.
    choice->Bind(wxEVT_KEY_DOWN, dismiss);
    choice->GetDropDown().Bind(wxEVT_KEY_DOWN, dismiss);
}

class WorkbenchChoice final : public ComboBox {
public:
    explicit WorkbenchChoice(wxWindow* parent)
        : ComboBox(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                   0, nullptr, wxCB_READONLY) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(wxColour(22, 22, 25));
        auto& dropdown = GetDropDown();
        dropdown.SetBackgroundColour(wxColour(32, 32, 35));
        dropdown.SetTextColor(StateColor(wxColour(235, 235, 235)));
        dropdown.SetSelectorBackgroundColor(StateColor(wxColour(49, 49, 54)));
        dropdown.SetUseContentWidth(true, true);
        bind_workbench_choice_escape(this);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC buffer(this);
            buffer.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
            buffer.Clear();
            wxGCDC dc(buffer);
            const auto size = GetClientSize();
            const int inset = FromDIP(10), edge = FromDIP(12);
            dc.SetPen(HasFocus() ? wxPen(wxColour(105, 105, 112), FromDIP(1)) : *wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(wxColour(22, 22, 25)));
            dc.DrawRoundedRectangle(0, 0, size.x - 1, size.y - 1, FromDIP(8));
            int text_x = inset;
            const int selected = GetSelection();
            const wxBitmap swatch = selected >= 0 && unsigned(selected) < GetCount()
                ? GetItemBitmap(unsigned(selected)) : wxNullBitmap;
            if (swatch.IsOk()) {
                const auto swatch_size = swatch.GetLogicalSize();
                dc.DrawBitmap(swatch, inset, (size.y - swatch_size.y) / 2, true);
                text_x += swatch_size.x + FromDIP(6);
            }
            dc.SetFont(GetFont());
            dc.SetTextForeground(IsEnabled() ? wxColour(235, 235, 235) : wxColour(105, 105, 110));
            const auto text = wxControl::Ellipsize(GetLabel(), dc, wxELLIPSIZE_END,
                std::max(0, size.x - text_x - inset - edge - FromDIP(6)));
            dc.DrawText(text, text_x, (size.y - dc.GetTextExtent(text).y) / 2);
            if (m_arrow.IsOk())
                dc.DrawBitmap(IsEnabled() ? m_arrow : m_disabled_arrow,
                              size.x - inset - edge, (size.y - edge) / 2, true);
        });
        Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& event) { Refresh(false); event.Skip(); });
        Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& event) { Refresh(false); event.Skip(); });
        Rescale();
    }
    void SetMinSize(const wxSize&) override {
        ComboBox::SetMinSize(FromDIP(wxSize(1, 32)));
    }
    void Rescale() override {
        ComboBox::Rescale();
        auto arrow = ScalableBitmap(this, "workbench_palette_arrow", 12).bmp().ConvertToImage().Rotate90();
        m_arrow = wxBitmap(arrow);
        m_disabled_arrow = wxBitmap(arrow.ConvertToGreyscale());
#ifdef __WXMSW__
        m_arrow.SetScaleFactor(GetDPIScaleFactor());
        m_disabled_arrow.SetScaleFactor(GetDPIScaleFactor());
#endif
        SetMinSize(wxDefaultSize);
        Refresh(false);
    }
private:
    wxBitmap m_arrow, m_disabled_arrow;
};

inline ComboBox* workbench_choice(wxWindow* parent)
{
    return new WorkbenchChoice(parent);
}

// Keep ComboBox input and accessibility, while placing the reference arrows
// at the right edge instead of TextInput's leading-icon slot.
class WorkbenchPaletteChoice final : public ComboBox {
public:
    explicit WorkbenchPaletteChoice(wxWindow* parent)
        : ComboBox(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                   0, nullptr, wxCB_READONLY) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(wxColour(22, 22, 25));
        auto& dropdown = GetDropDown();
        dropdown.SetBackgroundColour(wxColour(32, 32, 35));
        dropdown.SetTextColor(StateColor(wxColour(235, 235, 235)));
        dropdown.SetSelectorBackgroundColor(StateColor(wxColour(49, 49, 54)));
        dropdown.SetUseContentWidth(true, true);
        bind_workbench_choice_escape(this);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC buffer(this);
            buffer.SetBackground(wxBrush(GetParent()->GetBackgroundColour())); buffer.Clear();
            wxGCDC dc(buffer);
            const auto size = GetClientSize();
            dc.SetPen(HasFocus() ? wxPen(wxColour(105, 105, 112), FromDIP(1)) : *wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(GetBackgroundColour()));
            dc.DrawRoundedRectangle(0, 0, size.x - 1, size.y - 1, FromDIP(9));
            dc.SetFont(GetFont());
            dc.SetTextForeground(IsEnabled() ? wxColour(235, 235, 235) : wxColour(105, 105, 110));
            const int inset = FromDIP(12);
            const wxString label = wxControl::Ellipsize(GetLabel(), dc, wxELLIPSIZE_END,
                std::max(0, size.x - 2 * inset - FromDIP(16)));
            dc.DrawText(label, inset, (size.y - dc.GetTextExtent(label).y) / 2);
            if (m_down.IsOk()) {
                const int edge = FromDIP(10);
                const int x = size.x - inset - edge;
                dc.DrawBitmap(m_up, x, size.y / 2 - FromDIP(10), true);
                dc.DrawBitmap(m_down, x, size.y / 2 + FromDIP(1), true);
            }
        });
        Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& event) { Refresh(false); event.Skip(); });
        Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& event) { Refresh(false); event.Skip(); });
        Rescale();
    }
    void SetMinSize(const wxSize&) override {
        ComboBox::SetMinSize(FromDIP(wxSize(1, 42)));
    }
    void Rescale() override {
        ComboBox::Rescale();
        const auto arrow = ScalableBitmap(this, "workbench_palette_arrow", 10).bmp().ConvertToImage();
        m_down = wxBitmap(arrow.Rotate90());
        m_up = wxBitmap(arrow.Rotate90().Mirror(false));
#ifdef __WXMSW__
        m_down.SetScaleFactor(GetDPIScaleFactor());
        m_up.SetScaleFactor(GetDPIScaleFactor());
#endif
        SetMinSize(wxDefaultSize);
        Refresh(false);
    }
private:
    wxBitmap m_up, m_down;
};

// Custom Button caches pixel metrics and bitmap-only icons. Keep the workbench's
// source dimensions so those caches can be rebuilt after a monitor DPI change.
class WorkbenchButton final : public Button {
public:
    WorkbenchButton(wxWindow* parent, const wxString& label) : Button(parent, label) {
        SetPaddingSize(FromDIP(wxSize(8, 6)));
    }
    void SetLabel(const wxString& label) override {
        if (!m_multiline) { Button::SetLabel(label); return; }
        wxWindow::SetLabel(label);
        Refresh(false);
    }
    void set_multiline_label() {
        m_multiline = true;
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxPaintDC dc(this);
            StaticBox::render(dc);
            dc.SetFont(GetFont());
            dc.SetTextForeground(IsEnabled() ? wxColour(235, 235, 235) : wxColour(125, 125, 130));
            wxRect bounds(GetClientSize());
            bounds.Deflate(FromDIP(8), FromDIP(6));
            wxString label;
            for (const auto& line : wxSplit(GetLabel(), '\n')) {
                if (!label.empty()) label += "\n";
                label += wxControl::Ellipsize(line, dc, wxELLIPSIZE_END, std::max(1, bounds.width));
            }
            dc.SetClippingRegion(bounds);
            dc.DrawLabel(label, bounds, wxALIGN_CENTER);
        });
    }
    void SetMinSize(const wxSize& size) override {
        m_minimum = ToDIP(size);
        Button::SetMinSize(size);
    }
    void SetMaxSize(const wxSize& size) override {
        m_maximum = ToDIP(size);
        Button::SetMaxSize(size);
    }
    void SetPaddingSize(const wxSize& size) {
        m_padding = ToDIP(size);
        Button::SetPaddingSize(size);
    }
    void SetCornerRadius(double radius) {
        m_radius = ToDIP(int(radius));
        Button::SetCornerRadius(radius);
    }
    void set_scaled_icon(const std::string& asset, int edge, bool mirror = false, bool lighten = false) {
        m_asset = asset; m_edge = edge; m_mirror = mirror; m_lighten = lighten;
        refresh_icon();
    }
    void set_navigation_assets(const std::string& leading, const std::string& trailing) {
        m_navigation_leading = leading;
        m_navigation_trailing = trailing;
        refresh_navigation_assets();
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxPaintDC dc(this);
            StaticBox::render(dc);
            dc.SetFont(GetFont());
            const auto size = GetClientSize();
            const int inset = FromDIP(12);
            const int edge = FromDIP(18);
            const int arrow_x = size.x - FromDIP(9) - edge;
            const auto text = wxControl::Ellipsize(GetLabel(), dc, wxELLIPSIZE_END,
                std::max(1, arrow_x - inset - FromDIP(6)));
            dc.SetTextForeground(IsEnabled() ? wxColour(235, 235, 235) : wxColour(105, 105, 110));
            dc.DrawText(text, inset, (size.y - dc.GetTextExtent(text).y) / 2);
            // The reference's transparent leading layer shares the text inset.
            if (m_navigation_leading_bitmap.IsOk())
                dc.DrawBitmap(m_navigation_leading_bitmap, inset, (size.y - edge) / 2, true);
            if (m_navigation_trailing_bitmap.IsOk())
                dc.DrawBitmap(m_navigation_trailing_bitmap, arrow_x, (size.y - edge) / 2, true);
        });
    }
    void set_drawer_assets() {
        m_drawer = true;
        refresh_drawer_assets();
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxPaintDC dc(this);
            dc.SetBackground(wxBrush(GetBackgroundColour()));
            dc.Clear();
            const auto size = GetClientSize();
            if (m_drawer_background.IsOk())
                dc.DrawBitmap(m_drawer_background, 0, (size.y - FromDIP(98)) / 2, true);
            if (m_drawer_arrow.IsOk())
                dc.DrawBitmap(m_drawer_arrow, FromDIP(3), (size.y - FromDIP(12)) / 2, true);
            if (HasFocus()) {
                dc.SetBrush(*wxTRANSPARENT_BRUSH);
                dc.SetPen(wxPen(wxColour(255, 194, 39)));
                dc.DrawRectangle(FromDIP(2), (size.y - FromDIP(16)) / 2,
                                 FromDIP(15), FromDIP(16));
            }
        });
    }
    void set_drawer_open(bool open) {
        if (m_drawer_open == open) return;
        m_drawer_open = open;
        refresh_drawer_assets();
        Refresh(false);
    }
    void rescale_workbench() {
        Button::SetMaxSize(FromDIP(m_maximum));
        Button::SetPaddingSize(FromDIP(m_padding));
        Button::SetCornerRadius(FromDIP(m_radius));
        Button::SetMinSize(FromDIP(m_minimum));
        if (!m_asset.empty()) refresh_icon();
        if (!m_navigation_trailing.empty()) refresh_navigation_assets();
        if (m_drawer) refresh_drawer_assets();
        Button::Rescale();
    }
#ifdef __WXMSW__
protected:
    WXLRESULT MSWWindowProc(WXUINT message, WXWPARAM parameter, WXLPARAM data) override {
        // Alt+Space belongs to the native system menu, not Button's Space activation.
        if ((message == WM_SYSKEYDOWN || message == WM_SYSKEYUP) && parameter == WXK_SPACE)
            return ::DefWindowProc(GetHwnd(), message, parameter, data);
        return Button::MSWWindowProc(message, parameter, data);
    }
#endif
private:
    void refresh_drawer_assets() {
        // Figma's rotated/flipped handle is a horizontal mirror of the source.
        auto background = ScalableBitmap(this, "workbench_drawer_handle", 98).bmp().ConvertToImage().Mirror();
        auto arrow = ScalableBitmap(this, "workbench_page_edge", 12).bmp().ConvertToImage();
        if (!m_drawer_open) arrow = arrow.Mirror();
        m_drawer_background = wxBitmap(background);
        m_drawer_arrow = wxBitmap(arrow);
#ifdef __WXMSW__
        m_drawer_background.SetScaleFactor(GetDPIScaleFactor());
        m_drawer_arrow.SetScaleFactor(GetDPIScaleFactor());
#endif
    }
    void refresh_navigation_assets() {
        m_navigation_leading_bitmap = m_navigation_leading.empty() ? wxNullBitmap
            : ScalableBitmap(this, m_navigation_leading, 18).bmp();
        m_navigation_trailing_bitmap = ScalableBitmap(this, m_navigation_trailing, 18).bmp();
    }
    void refresh_icon() {
        auto icon = ScalableBitmap(this, m_asset, m_edge).bmp().ConvertToImage();
        if (m_lighten) icon.Replace(0, 0, 0, 255, 255, 255);
        if (m_mirror) icon = icon.Mirror();
        Button::SetIcon(wxBitmap(icon));
    }
    wxSize m_minimum {wxDefaultSize};
    wxSize m_maximum {wxDefaultSize};
    wxSize m_padding {8, 6};
    int m_radius {8};
    std::string m_asset;
    int m_edge {16};
    bool m_mirror {false};
    bool m_lighten {false};
    std::string m_navigation_leading, m_navigation_trailing;
    wxBitmap m_navigation_leading_bitmap, m_navigation_trailing_bitmap;
    bool m_drawer {false};
    bool m_multiline {false};
    bool m_drawer_open {true};
    wxBitmap m_drawer_background, m_drawer_arrow;
};

inline WorkbenchButton* workbench_button(wxWindow* parent, const wxString& label)
{
    auto* button = new WorkbenchButton(parent, label);
    button->SetCornerRadius(parent->FromDIP(8));
    button->SetBorderWidth(0);
    button->SetBackgroundColor(StateColor(
        std::pair<wxColour, int>(wxColour(48, 48, 52), StateColor::Hovered),
        std::pair<wxColour, int>(wxColour(22, 22, 25), StateColor::Normal)));
    button->SetTextColor(StateColor(
        std::pair<wxColour, int>(wxColour(105, 105, 110), StateColor::Disabled),
        std::pair<wxColour, int>(wxColour(235, 235, 235), StateColor::Normal)));
    button->SetPaddingSize(parent->FromDIP(wxSize(8, 6)));
    button->SetMinSize(parent->FromDIP(wxSize(-1, 32)));
    return button;
}

class WorkbenchPanel final : public wxPanel {
public:
    explicit WorkbenchPanel(wxWindow* parent, bool clip_children = false)
        : wxPanel(parent), m_clip_children(clip_children) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(wxColour(32, 32, 35));
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC buffer(this);
            buffer.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
            buffer.Clear();
            wxGCDC dc(buffer);
            dc.SetBrush(wxBrush(GetBackgroundColour()));
            dc.SetPen(m_selected ? wxPen(wxColour(255, 194, 39), FromDIP(1)) : *wxTRANSPARENT_PEN);
            const auto size = GetClientSize();
            dc.DrawRoundedRectangle(0, 0, size.x - 1, size.y - 1, FromDIP(9));
        });
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { update_shape(); event.Skip(); });
    }
    void set_selected(bool selected) { m_selected = selected; Refresh(false); }
    void SetMinSize(const wxSize& size) override {
        m_minimum = ToDIP(size);
        wxPanel::SetMinSize(size);
    }
    void rescale_workbench() {
        wxPanel::SetMinSize(FromDIP(m_minimum));
        update_shape();
    }
private:
    void update_shape() {
#ifdef __WXMSW__
        if (!m_clip_children) return;
        const wxSize size = GetClientSize();
        const int radius = FromDIP(9);
        if (size.x <= 0 || size.y <= 0 || (size == m_shape_size && radius == m_shape_radius)) return;
        HRGN region = ::CreateRoundRectRgn(0, 0, size.x + 1, size.y + 1, 2 * radius, 2 * radius);
        if (!region) return;
        // The region clips opaque child backgrounds as well as the panel paint.
        if (::SetWindowRgn(GetHwnd(), region, TRUE)) {
            m_shape_size = size;
            m_shape_radius = radius;
        } else {
            ::DeleteObject(region);
        }
#endif
    }
    bool m_selected {false};
    bool m_clip_children {false};
    wxSize m_minimum {wxDefaultSize};
#ifdef __WXMSW__
    wxSize m_shape_size {wxDefaultSize};
    int m_shape_radius {-1};
#endif
};

} // namespace Slic3r::GUI
