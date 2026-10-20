#pragma once

#include "RedesignTheme.hpp"
#include "../AI/ModelGeneration/WorkbenchStyle.hpp"
#include <wx/stattext.h>

namespace Slic3r::GUI {
inline wxColour slicing_control_colour() { return wxColour(21, 21, 23); }

class SlicingModeButton final : public Button {
public:
    SlicingModeButton(wxWindow* parent, const wxString& label) : Button(parent, label) {
        SetMinSize(FromDIP(wxSize(0, 40)));
        SetCursor(wxCursor(wxCURSOR_HAND));
        RedesignTheme::style_medium_text(this, RedesignTheme::primary_text_colour(), 12);
        // Register all painted states with Button's existing input handler.
        SetBackgroundColor(StateColor(
            std::make_pair(wxColour(21, 21, 23), int(StateColor::Disabled)),
            std::make_pair(wxColour(58, 58, 62), int(StateColor::Pressed)),
            std::make_pair(wxColour(48, 48, 52), int(StateColor::Hovered)),
            std::make_pair(wxColour(38, 38, 42), int(StateColor::Checked)),
            std::make_pair(wxColour(21, 21, 23), int(StateColor::Normal))));
        SetBorderColor(StateColor(
            std::make_pair(RedesignTheme::accent_colour(), int(StateColor::Focused)),
            std::make_pair(wxColour(21, 21, 23), int(StateColor::Normal))));
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            const int states = state_handler.states();
            const auto size = GetClientSize();
            dc.SetBackground(wxBrush(background_color.colorForStatesNoDark(states)));
            dc.Clear();
            dc.SetFont(GetFont());
            dc.SetTextForeground(!IsEnabled() ? wxColour(105, 105, 112) :
                GetValue() ? RedesignTheme::primary_text_colour() : wxColour(184, 184, 192));
            dc.DrawLabel(GetLabel(), wxRect(size), wxALIGN_CENTER);
            if (GetValue()) {
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.SetBrush(wxBrush(IsEnabled() ? RedesignTheme::accent_colour() : wxColour(105, 105, 112)));
                const int width = std::min(FromDIP(40), size.x);
                dc.DrawRectangle((size.x - width) / 2, size.y - FromDIP(3), width, FromDIP(3));
            }
            if (IsEnabled() && (states & StateColor::Focused)) {
                dc.SetPen(wxPen(RedesignTheme::accent_colour(), FromDIP(1), wxPENSTYLE_DOT));
                dc.SetBrush(*wxTRANSPARENT_BRUSH);
                dc.DrawRectangle(GetClientRect().Deflate(FromDIP(3)));
            }
        });
        Bind(wxEVT_DPI_CHANGED, [this](wxDPIChangedEvent& event) {
            SetMinSize(FromDIP(wxSize(0, 40)));
            event.Skip();
        });
    }
};

class SlicingCheckBadge final : public wxStaticText {
public:
    explicit SlicingCheckBadge(wxWindow* parent) : wxStaticText(parent, wxID_ANY, wxEmptyString) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(FromDIP(wxSize(60, 36)));
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC buffer(this);
            buffer.SetBackground(wxBrush(GetParent()->GetBackgroundColour())); buffer.Clear();
            wxGCDC dc(buffer);
            const auto size = GetClientSize();
            dc.SetPen(*wxTRANSPARENT_PEN); dc.SetBrush(wxBrush(GetBackgroundColour()));
            dc.DrawRoundedRectangle(0, 0, size.x, size.y, FromDIP(12));
            dc.SetFont(GetFont()); dc.SetTextForeground(GetForegroundColour());
            dc.DrawLabel(GetLabel(), wxRect(size), wxALIGN_CENTER);
        });
    }
};

class SlicingGoalCard final : public Button {
public:
    SlicingGoalCard(wxWindow* parent, const wxString& title) : Button(parent, title), m_title(title) {
        SetMinSize(FromDIP(wxSize(148, 63)));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC buffer(this);
            buffer.SetBackground(wxBrush(GetParent()->GetBackgroundColour())); buffer.Clear();
            wxGCDC dc(buffer);
            const auto size = GetClientSize();
            const int border = std::max(1, FromDIP(1));
            const int radius = FromDIP(12);
            const wxColour card_colour(68, 68, 69);
            dc.SetPen(*wxTRANSPARENT_PEN);
            if (m_selected) {
                // Fill an outer frame and an inset card instead of centering a
                // pen on the control edge. This keeps the visible ring exactly
                // one DIP wide on every side and at high-DPI rounded corners.
                dc.SetBrush(wxBrush(RedesignTheme::accent_colour()));
                dc.DrawRoundedRectangle(0, 0, size.x, size.y, radius);
                dc.SetBrush(wxBrush(card_colour));
                dc.DrawRoundedRectangle(border, border,
                    std::max(0, size.x - border * 2), std::max(0, size.y - border * 2),
                    std::max(0, radius - border));
            } else {
                dc.SetBrush(wxBrush(card_colour));
                dc.DrawRoundedRectangle(0, 0, size.x, size.y, radius);
            }
            if (m_recently_sliced) {
                const int x = size.x - FromDIP(15), y = FromDIP(14);
                dc.SetPen(wxPen(RedesignTheme::accent_colour(), FromDIP(2)));
                dc.DrawLine(x - FromDIP(5), y, x - FromDIP(2), y + FromDIP(3));
                dc.DrawLine(x - FromDIP(2), y + FromDIP(3), x + FromDIP(4), y - FromDIP(4));
            }
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(wxColour(229, 229, 230)));
            const int x = FromDIP(10), y = (size.y-FromDIP(24))/2;
            wxPoint bolt[]{{x+FromDIP(9),y}, {x,y+FromDIP(14)}, {x+FromDIP(6),y+FromDIP(14)},
                {x+FromDIP(3),y+FromDIP(24)}, {x+FromDIP(14),y+FromDIP(9)}, {x+FromDIP(7),y+FromDIP(9)}};
            dc.DrawPolygon(6, bolt);
            const int left = FromDIP(30);
            dc.SetFont(GetFont());
            dc.SetTextForeground(wxColour(229,229,230));
            dc.DrawText(m_title, left, FromDIP(12));
            auto font = GetFont(); font.SetFractionalPointSize(10.5); dc.SetFont(font);
            dc.SetTextForeground(wxColour(180,180,181));
            dc.DrawText(wxControl::Ellipsize(m_layer, dc, wxELLIPSIZE_END, std::max(1,size.x-left-FromDIP(8))),
                left, FromDIP(35));
        });
        RedesignTheme::style_text(this, RedesignTheme::primary_text_colour(), 12);
    }
    void update(const wxString& layer, bool selected, bool recently_sliced) {
        if (m_layer == layer && m_selected == selected && m_recently_sliced == recently_sliced) return;
        m_layer = layer; m_selected = selected; m_recently_sliced = recently_sliced;
        wxWindow::SetLabel(m_title+"\n"+layer); Refresh(false);
    }
private:
    wxString m_title, m_layer;
    bool m_selected{false};
    bool m_recently_sliced{false};
};

inline WorkbenchButton* slicing_link(wxWindow* parent, const wxString& label) {
    auto* button = workbench_button(parent, label);
    button->SetMinSize(parent->FromDIP(wxSize(-1, 24)));
    button->SetBorderWidth(0);
    button->SetBackgroundColor(StateColor(parent->GetBackgroundColour()));
    button->SetTextColor(StateColor(wxColour(190,190,196)));
    RedesignTheme::style_text(button, wxColour(190,190,196), 9);
    auto font=button->GetFont(); font.SetFractionalPointSize(10.5); button->SetFont(font);
    return button;
}

class SlicingCanvasOverlay final : public wxPanel {
public:
    explicit SlicingCanvasOverlay(wxWindow* parent) : wxPanel(parent) {
        SetName("slicing-canvas-overlay");
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC buffer(this);
            buffer.SetBackground(wxBrush(RedesignTheme::flow_background_colour())); buffer.Clear();
            wxGCDC dc(buffer);
            dc.SetPen(*wxTRANSPARENT_PEN);
            const auto size = GetClientSize();
            dc.SetFont(GetFont());
            if (m_progress >= 0) {
                auto font=GetFont(); font.SetFractionalPointSize(10.5); dc.SetFont(font);
                dc.SetTextForeground(wxColour(190,190,196));
                const auto percent=wxString::Format("%d%%",m_progress);
                dc.DrawText(wxControl::Ellipsize(m_text,dc,wxELLIPSIZE_END,size.x-FromDIP(48)),0,0);
                dc.DrawText(percent,size.x-dc.GetTextExtent(percent).x,0);
                dc.SetBrush(wxBrush(wxColour(62,62,66))); dc.DrawRoundedRectangle(0,FromDIP(30),size.x,FromDIP(8),FromDIP(2));
                dc.SetBrush(wxBrush(wxColour(37,111,255)));
                dc.DrawRoundedRectangle(0,FromDIP(30),size.x*m_progress/100,FromDIP(8),FromDIP(2));
            } else {
                dc.SetBrush(wxBrush(RedesignTheme::panel_colour()));
                dc.DrawRoundedRectangle(0,0,size.x,size.y,FromDIP(12));
                auto font=GetFont(); font.SetPointSize(12); dc.SetFont(font);
                const auto rows=wxSplit(m_text,'\n');
                for (size_t i=0;i<rows.size();++i) {
                    const auto columns=wxSplit(rows[i],'\t');
                    const int y=FromDIP(20+int(i)*41);
                    dc.SetTextForeground(wxColour(172,172,175)); dc.DrawText(columns[0],FromDIP(16),y);
                    if (columns.size()>1) {
                        dc.SetTextForeground(wxColour(223,223,226));
                        dc.DrawText(columns[1],size.x-FromDIP(16)-dc.GetTextExtent(columns[1]).x,y);
                    }
                }
            }
        });
        RedesignTheme::style_text(this, RedesignTheme::secondary_text_colour(), 10);
    }
    void update(const wxString& text, int progress = -1) {
        if (m_text == text && m_progress == progress) return;
        m_text=text; m_progress=progress; SetLabel(text); Refresh(false);
    }
private:
    wxString m_text;
    int m_progress{-1};
};
} // namespace Slic3r::GUI
