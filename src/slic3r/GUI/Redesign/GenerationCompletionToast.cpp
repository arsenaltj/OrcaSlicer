#include "GenerationCompletionToast.hpp"
#include "RedesignTheme.hpp"
#include "libslic3r/Utils.hpp"
#include "SmartCreationControls.hpp"
#include "../wxExtensions.hpp"
#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/toplevel.h>
#include <wx/weakref.h>

namespace Slic3r::GUI {
GenerationCompletionToast::GenerationCompletionToast(wxWindow* owner)
    : wxPopupWindow(owner, wxBORDER_NONE), m_owner(owner), m_timer(this) {
    SetName("model-generation-completion-toast"); SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetSize(FromDIP(wxSize(318,70)));
    m_icon = create_scaled_bitmap("inline_generation_success", this, 20);
    Bind(wxEVT_TIMER,[this](wxTimerEvent&) { dismiss(); },m_timer.GetId());
    Bind(wxEVT_PAINT,[this](wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(this); dc.SetBackground(wxBrush(RedesignTheme::background_colour())); dc.Clear();
        auto gc=std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc)); if(!gc) return;
        gc->SetPen(*wxTRANSPARENT_PEN);
        // Figma shadow: #00197E at 10%, offset(0,2), CSS blur10.
        for(int i=10;i>0;--i) {
            gc->SetBrush(wxBrush(wxColour(0,25,126,2)));
            gc->DrawRoundedRectangle(FromDIP(10-i),FromDIP(12-i),FromDIP(298+2*i),FromDIP(48+2*i),FromDIP(12+i));
        }
        gc->SetBrush(*wxBLACK_BRUSH);
        gc->DrawRoundedRectangle(FromDIP(10),FromDIP(10),FromDIP(298),FromDIP(48),FromDIP(12));
        dc.DrawBitmap(m_icon,FromDIP(26),FromDIP(24),true);
        dc.SetFont(smart_creation_font(14)); dc.SetTextForeground(*wxWHITE);
        dc.DrawLabel(wxString::FromUTF8("模型生成完成啦～快去工作台编辑吧！"),
            wxRect(FromDIP(54),FromDIP(22),FromDIP(238),FromDIP(24)),wxALIGN_CENTER_VERTICAL);
    });
    wxWeakRef<GenerationCompletionToast> weak(this);
    owner->Bind(wxEVT_SIZE, [weak](wxSizeEvent& event) {
        event.Skip(); if (weak && weak->IsShown()) weak->position();
    });
    owner->Bind(wxEVT_SHOW, [weak](wxShowEvent& event) {
        event.Skip(); if (weak && !event.IsShown()) weak->dismiss();
    });
    if (auto* top = wxDynamicCast(wxGetTopLevelParent(owner), wxTopLevelWindow)) {
        top->Bind(wxEVT_MOVE, [weak](wxMoveEvent& event) {
            event.Skip(); if (weak && weak->IsShown()) weak->position();
        });
        top->Bind(wxEVT_ICONIZE, [weak](wxIconizeEvent& event) {
            event.Skip(); if (weak && event.IsIconized()) weak->dismiss();
        });
        top->Bind(wxEVT_DPI_CHANGED, [weak](wxDPIChangedEvent& event) {
            event.Skip();
            if (weak) weak->CallAfter([weak] { if (weak && weak->IsShown()) weak->position(); });
        });
    }
}
void GenerationCompletionToast::position() {
    auto* top = wxDynamicCast(wxGetTopLevelParent(m_owner), wxTopLevelWindow);
    if (!m_owner->IsShownOnScreen() || (top && top->IsIconized())) { dismiss(); return; }
    const auto size = FromDIP(wxSize(318,70));
    if (GetSize() != size) {
        SetSize(size); m_icon = create_scaled_bitmap("inline_generation_success", this, 20);
        Refresh(false);
    }
    const auto origin=m_owner->ClientToScreen(wxPoint(0,0));
    Move(origin.x+(m_owner->GetClientSize().x-GetSize().x)/2,origin.y+FromDIP(6));
}
void GenerationCompletionToast::present() {
    if (!m_owner->IsShownOnScreen()) return;
    auto* top = wxDynamicCast(wxGetTopLevelParent(m_owner), wxTopLevelWindow);
    if (top && top->IsIconized()) return;
    position(); Show(); Raise(); m_timer.StartOnce(3000);
}
void GenerationCompletionToast::dismiss() { m_timer.Stop(); Hide(); }
}
