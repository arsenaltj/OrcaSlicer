#pragma once

#include "RedesignTheme.hpp"
#include "../AI/ModelGeneration/WorkbenchStyle.hpp"
#include <wx/stattext.h>

namespace Slic3r::GUI {
inline wxColour slicing_control_colour() { return wxColour(21, 21, 23); }

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
            dc.SetPen(m_selected ? wxPen(RedesignTheme::accent_colour(), FromDIP(1)) : *wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(wxColour(68, 68, 69)));
            dc.DrawRoundedRectangle(0, 0, size.x-1, size.y-1, FromDIP(12));
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
    void update(const wxString& layer, bool selected) {
        if (m_layer == layer && m_selected == selected) return;
        m_layer = layer; m_selected = selected;
        wxWindow::SetLabel(m_title+"\n"+layer); Refresh(false);
    }
private:
    wxString m_title, m_layer;
    bool m_selected{false};
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
