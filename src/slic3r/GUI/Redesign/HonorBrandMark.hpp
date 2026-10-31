#pragma once

#include "../BitmapCache.hpp"
#include "libslic3r_version.h"

#include <wx/control.h>
#include <wx/dcbuffer.h>

namespace Slic3r::GUI {

// Shared brand mark for the Honor titlebar, startup buffer and dialog headers.
class HonorBrandMark final : public wxControl
{
public:
    explicit HonorBrandMark(wxWindow* parent, int size = 24)
        : wxControl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE), m_size(size)
    {
        SetName(SLIC3R_APP_NAME);
        SetToolTip(SLIC3R_APP_NAME);
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Rescale();
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
            dc.Clear();
            if (m_bitmap.IsOk())
                dc.DrawBitmap(m_bitmap, (GetClientSize().x - m_bitmap.GetWidth()) / 2,
                              (GetClientSize().y - m_bitmap.GetHeight()) / 2, true);
        });
        const auto forward = [this](wxMouseEvent& event) {
            event.SetPosition(GetParent()->ScreenToClient(ClientToScreen(event.GetPosition())));
            GetParent()->GetEventHandler()->ProcessEvent(event);
        };
        Bind(wxEVT_LEFT_DOWN, forward);
        Bind(wxEVT_LEFT_DCLICK, forward);
#ifdef __WXMSW__
        Bind(wxEVT_DPI_CHANGED, [this](wxDPIChangedEvent& event) { Rescale(); event.Skip(); });
#endif
    }

    void Rescale()
    {
        const int size = FromDIP(m_size);
        BitmapCache cache;
        if (const auto* bitmap = cache.load_png("honor_titlebar", size, size)) m_bitmap = *bitmap;
        SetMinSize(wxSize(size, size));
        SetSize(wxSize(size, size));
        Refresh();
    }

private:
    int m_size;
    wxBitmap m_bitmap;
};

}
