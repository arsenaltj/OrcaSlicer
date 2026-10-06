#pragma once

#include "RedesignTheme.hpp"
#include <algorithm>
#include <memory>
#include <wx/panel.h>
#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/control.h>

namespace Slic3r::GUI {

class RoundedPanel final : public wxPanel {
public:
    RoundedPanel(wxWindow* parent, const wxSize& size, const wxColour& face,
                 const wxColour& surrounding, int radius)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, size)
        , m_face(face), m_surrounding(surrounding), m_radius(radius)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(surrounding);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(m_surrounding));
            dc.Clear();
            const wxSize size = GetClientSize();
            if (size.x <= 0 || size.y <= 0) return;
            auto gc = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc));
            if (!gc) return;
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->SetBrush(wxBrush(m_face));
            gc->DrawRoundedRectangle(0, 0, size.x, size.y, FromDIP(m_radius));
        });
    }
    const wxColour& face_colour() const { return m_face; }
private:
    wxColour m_face, m_surrounding;
    int m_radius;
};

class RoundedActionButton final : public wxPanel {
public:
    RoundedActionButton(wxWindow* parent, const wxString& caption, bool primary, int height)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, parent->FromDIP(height)))
        , m_primary(primary)
    {
        SetLabel(caption);
        SetMinSize(wxSize(-1, FromDIP(height)));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(RedesignTheme::panel_colour());
        SetCanFocus(true);
        RedesignTheme::style_text(this, primary ? wxColour(20, 20, 20) : RedesignTheme::primary_text_colour(), 11, primary);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& event) {
            m_hovered = true;
            Refresh();
            event.Skip();
        });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& event) {
            m_hovered = false;
            m_pressed = false;
            Refresh();
            event.Skip();
        });
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) {
            if (!IsEnabled())
                return;
            m_pressed = true;
            SetFocus();
            Refresh();
            event.Skip();
        });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
            if (!IsEnabled() || !m_pressed)
                return;
            m_pressed = false;
            Refresh();
            wxCommandEvent command(wxEVT_BUTTON, GetId());
            command.SetEventObject(this);
            ProcessWindowEvent(command);
        });
        Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) {
            if (IsEnabled() && (event.GetKeyCode() == WXK_RETURN || event.GetKeyCode() == WXK_SPACE)) {
                wxCommandEvent command(wxEVT_BUTTON, GetId());
                command.SetEventObject(this);
                ProcessWindowEvent(command);
                return;
            }
            event.Skip();
        });
        Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& event) { Refresh(); event.Skip(); });
        Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& event) { Refresh(); event.Skip(); });
    }

    void set_secondary_face(const wxColour& colour) { m_secondary_face = colour; Refresh(); }
    void set_alignment(int alignment) { m_alignment = alignment; Refresh(); }
    void set_icon(const wxBitmap& icon) { m_icon = icon; Refresh(); }
    void set_text_colour(const wxColour& colour) { m_text_colour = colour; Refresh(); }

private:
    void paint()
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetBackgroundColour()));
        dc.Clear();
        const wxSize size = GetClientSize();
        if (size.x <= 0 || size.y <= 0)
            return;
        auto gc = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc));
        if (!gc)
            return;

        wxColour face;
        wxColour foreground;
        if (!IsEnabled()) {
            face = m_primary ? wxColour(91, 80, 43) : wxColour(43, 43, 47);
            foreground = m_primary ? wxColour(25, 25, 25, 150) : wxColour(255, 255, 255, 78);
        } else if (m_primary) {
            face = m_pressed ? wxColour(231, 168, 20) : m_hovered ? wxColour(255, 207, 76) : RedesignTheme::accent_colour();
            foreground = wxColour(20, 20, 20);
        } else {
            face = m_pressed ? wxColour(52, 52, 56) : m_hovered ? wxColour(47, 47, 51) : m_secondary_face;
            foreground = RedesignTheme::primary_text_colour();
        }

        gc->SetBrush(wxBrush(face));
        gc->SetPen(FindFocus() == this ? wxPen(RedesignTheme::accent_colour(), std::max(1, FromDIP(1))) : *wxTRANSPARENT_PEN);
        gc->DrawRoundedRectangle(FromDIP(1), FromDIP(1), size.x - FromDIP(2), size.y - FromDIP(2), FromDIP(10));
        dc.SetFont(GetFont());
        if (IsEnabled() && m_text_colour.IsOk()) foreground = m_text_colour;
        dc.SetTextForeground(foreground);
        const int icon_width = m_icon.IsOk() ? m_icon.GetLogicalWidth() + FromDIP(12) : 0;
        const auto caption = wxControl::Ellipsize(GetLabel(), dc, wxELLIPSIZE_END,
            std::max(0, size.x - FromDIP(32) - icon_width));
        const wxSize extent = dc.GetTextExtent(caption);
        dc.DrawText(caption, m_alignment == wxALIGN_LEFT ? FromDIP(16) : (size.x - extent.x) / 2,
                    (size.y - extent.y) / 2);
        if (m_icon.IsOk()) {
            const int x = GetLabel().empty() ? (size.x - m_icon.GetLogicalWidth()) / 2 :
                size.x - m_icon.GetLogicalWidth() - FromDIP(12);
            dc.DrawBitmap(m_icon, x, (size.y - m_icon.GetLogicalHeight()) / 2, true);
        }
    }

    wxColour m_secondary_face { RedesignTheme::control_colour() };
    wxColour m_text_colour;
    wxBitmap m_icon;
    int m_alignment { wxALIGN_CENTER };
    bool m_primary { false };
    bool m_hovered { false };
    bool m_pressed { false };
};

} // namespace Slic3r::GUI
