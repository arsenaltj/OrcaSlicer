#include "GenerationCompletionToast.hpp"
#include "RedesignTheme.hpp"
#include "libslic3r/Utils.hpp"
#include "SmartCreationControls.hpp"
#include "../wxExtensions.hpp"
#include <wx/dcbuffer.h>
#include <wx/dcclient.h>
#include <wx/display.h>
#include <wx/graphics.h>
#include <wx/toplevel.h>
#include <wx/weakref.h>
#include <algorithm>
#include <boost/log/trivial.hpp>
#ifdef __WXMSW__
#include <wx/msw/wrapwin.h>
#endif

namespace Slic3r::GUI {
GenerationCompletionToast::GenerationCompletionToast(wxWindow* owner)
    : wxPopupWindow(owner, wxBORDER_NONE), m_owner(owner), m_timer(this) {
    SetName("model-generation-completion-toast"); SetBackgroundStyle(wxBG_STYLE_PAINT);
    update_layout();
    Bind(wxEVT_TIMER,[this](wxTimerEvent&) {
        const int remaining = m_timing.remaining_ms(GenerationToastTiming::Clock::now());
        if (IsShown() && remaining > 0) m_timer.StartOnce(remaining);
        else dismiss();
    },m_timer.GetId());
    Bind(wxEVT_PAINT,[this](wxPaintEvent&) {
        if (m_layered) { wxPaintDC dc(this); return; }
        wxAutoBufferedPaintDC dc(this); dc.SetBackground(wxBrush(RedesignTheme::background_colour())); dc.Clear();
        auto gc=std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc)); if(!gc) return;
        draw_bubble(*gc);
        dc.DrawBitmap(m_icon,FromDIP(26),FromDIP(10)+(m_body.y-FromDIP(20))/2,true);
        dc.SetFont(smart_creation_font(14)); dc.SetTextForeground(*wxWHITE);
        for (std::size_t i=0; i<m_lines.size(); ++i)
            dc.DrawLabel(m_lines[i], wxRect(FromDIP(54),FromDIP(22)+static_cast<int>(i)*FromDIP(24),
                m_body.x-FromDIP(60),FromDIP(24)),wxALIGN_CENTER_VERTICAL);
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
        top->Bind(wxEVT_SHOW, [weak](wxShowEvent& event) {
            event.Skip(); if (weak && !event.IsShown()) weak->dismiss();
        });
        top->Bind(wxEVT_CLOSE_WINDOW, [weak](wxCloseEvent& event) {
            if (weak) weak->dismiss(); event.Skip();
        });
        top->Bind(wxEVT_DPI_CHANGED, [weak](wxDPIChangedEvent& event) {
            event.Skip();
            if (weak) weak->CallAfter([weak] { if (weak && weak->IsShown()) weak->position(); });
        });
    }
}
void GenerationCompletionToast::draw_bubble(wxGraphicsContext& gc) const {
    gc.SetPen(*wxTRANSPARENT_PEN);
    // Figma shadow: #00197E at 10%, offset(0,2), CSS blur10.
    for (int i=10; i>0; --i) {
        gc.SetBrush(wxBrush(wxColour(0,25,126,2)));
        gc.DrawRoundedRectangle(FromDIP(10-i),FromDIP(12-i),
            m_body.x+FromDIP(2*i),m_body.y+FromDIP(2*i),FromDIP(12+i));
    }
    gc.SetBrush(*wxBLACK_BRUSH);
    gc.DrawRoundedRectangle(FromDIP(10),FromDIP(10),m_body.x,m_body.y,FromDIP(12));
}
void GenerationCompletionToast::update_native_surface() {
#ifdef __WXMSW__
    const auto hwnd = reinterpret_cast<HWND>(GetHandle());
    const auto restore = [this, hwnd] {
        if (m_layered) SetWindowLongPtr(hwnd,GWL_EXSTYLE,m_plain_extended_style);
        m_layered = false;
    };
    if (!m_error) { restore(); return; }
    const auto size = GetSize();
    wxImage surface(size.x,size.y);
    surface.InitAlpha();
    std::fill_n(surface.GetAlpha(),static_cast<std::size_t>(size.x)*size.y,0);
    {
        auto gc = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(surface));
        if (!gc) { restore(); return; }
        draw_bubble(*gc);
        gc->DrawBitmap(m_icon,FromDIP(26),FromDIP(10)+(m_body.y-FromDIP(20))/2,FromDIP(20),FromDIP(20));
        const auto font = smart_creation_font(14);
        gc->SetFont(gc->CreateFont(font.GetPixelSize().y,font.GetFaceName(),wxFONTFLAG_DEFAULT,*wxWHITE));
        for (std::size_t i=0; i<m_lines.size(); ++i) {
            double width, height;
            gc->GetTextExtent(m_lines[i],&width,&height);
            gc->DrawText(m_lines[i],FromDIP(54),FromDIP(22)+i*FromDIP(24)+(FromDIP(24)-height)/2);
        }
    } // The image renderer writes RGBA back when the context is destroyed.
    BITMAPINFO info {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size.x;
    info.bmiHeader.biHeight = -size.y;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    const HDC dc = CreateCompatibleDC(nullptr);
    const HBITMAP bitmap = CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    if (!dc || !bitmap || !pixels) {
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        restore(); return;
    }
    auto* target = static_cast<unsigned char*>(pixels);
    const auto* rgb = surface.GetData();
    const auto* alpha = surface.GetAlpha();
    for (std::size_t i=0; i<static_cast<std::size_t>(size.x)*size.y; ++i) {
        target[4*i] = static_cast<unsigned char>((rgb[3*i+2]*alpha[i]+127)/255);
        target[4*i+1] = static_cast<unsigned char>((rgb[3*i+1]*alpha[i]+127)/255);
        target[4*i+2] = static_cast<unsigned char>((rgb[3*i]*alpha[i]+127)/255);
        target[4*i+3] = alpha[i];
    }
    const auto previous = SelectObject(dc,bitmap);
    if (!m_layered) {
        m_plain_extended_style = GetWindowLongPtr(hwnd,GWL_EXSTYLE);
        SetWindowLongPtr(hwnd,GWL_EXSTYLE,m_plain_extended_style | WS_EX_LAYERED | WS_EX_TRANSPARENT);
        m_layered = true;
    }
    const auto point = GetScreenPosition();
    POINT destination {point.x,point.y}, origin {0,0};
    SIZE extent {size.x,size.y};
    BLENDFUNCTION blend {AC_SRC_OVER,0,255,AC_SRC_ALPHA};
    const bool updated = UpdateLayeredWindow(hwnd,nullptr,&destination,&extent,dc,&origin,0,&blend,ULW_ALPHA) != FALSE;
    SelectObject(dc,previous); DeleteObject(bitmap); DeleteDC(dc);
    if (!updated) restore();
#endif
}
void GenerationCompletionToast::update_layout() {
    m_icon = create_scaled_bitmap(m_error ? "inline_generation_error" : "inline_generation_success", this, 20);
    m_lines.clear();
    if (!m_error) {
        m_body = FromDIP(wxSize(298,48));
        m_lines.push_back(wxString::FromUTF8("模型生成完成啦～快去工作台编辑吧！"));
    } else {
        wxClientDC dc(this); dc.SetFont(smart_creation_font(14));
        const int display_index = wxDisplay::GetFromWindow(m_owner);
        const int screen_width = wxDisplay(static_cast<unsigned>(display_index == wxNOT_FOUND ? 0 : display_index)).GetClientArea().width;
        const int limit = std::max(FromDIP(80), std::min({FromDIP(560),
            m_owner->GetClientSize().x-FromDIP(32), screen_width-FromDIP(32)}));
        const int width = std::min(limit, std::max(FromDIP(229),dc.GetTextExtent(m_message).x+FromDIP(60)));
        const int text_width = std::max(1,width-FromDIP(60));
        m_lines = generation_toast_lines(m_message, text_width,
            [&dc](const wxString& text) { return dc.GetTextExtent(text).x; });
        m_body = wxSize(width,FromDIP(24+24*static_cast<int>(m_lines.size())));
    }
    SetSize(m_body+FromDIP(wxSize(20,22)));
    Refresh(false);
}
void GenerationCompletionToast::position() {
    auto* top = wxDynamicCast(wxGetTopLevelParent(m_owner), wxTopLevelWindow);
    if (!m_owner->IsShownOnScreen() || (top && top->IsIconized())) { dismiss(); return; }
    update_layout();
    const auto origin=m_owner->ClientToScreen(wxPoint(0,0));
    wxPoint point(origin.x+(m_owner->GetClientSize().x-GetSize().x)/2,origin.y+FromDIP(m_error ? 64 : 6));
    if (m_error) {
        const int display_index = wxDisplay::GetFromWindow(m_owner);
        const auto area = wxDisplay(static_cast<unsigned>(display_index == wxNOT_FOUND ? 0 : display_index)).GetClientArea();
        point.x = std::clamp(point.x,area.x,std::max(area.x,area.GetRight()-GetSize().x+1));
        point.y = std::clamp(point.y,area.y,std::max(area.y,area.GetBottom()-GetSize().y+1));
    }
    Move(point);
    update_native_surface();
}
void GenerationCompletionToast::present() {
    m_error = false; m_message.clear(); show_for_interval();
}
void GenerationCompletionToast::present_error(const wxString& message) {
    m_error = true; m_message = message; show_for_interval();
}
void GenerationCompletionToast::show_for_interval() {
    if (!m_owner->IsShownOnScreen()) return;
    auto* top = wxDynamicCast(wxGetTopLevelParent(m_owner), wxTopLevelWindow);
    if (top && top->IsIconized()) return;
    m_timer.Stop(); position(); Show(); Raise(); Refresh(false); Update();
    m_timing.restart(GenerationToastTiming::Clock::now()); ++m_presentation;
    m_timer.StartOnce(3000); trace("present");
}
void GenerationCompletionToast::dismiss() {
    m_timer.Stop();
    if (IsShown()) trace("dismiss");
    m_timing.cancel(); Hide();
}
void GenerationCompletionToast::trace(const char* action) const {
    wxString enabled;
    if (wxGetEnv("ORCASLICER_UI_LATENCY_TRACE", &enabled) && enabled == "1")
        BOOST_LOG_TRIVIAL(info) << "[UiGenerationToast] action=" << action
            << " kind=" << (m_error ? "error" : "success") << " presentation=" << m_presentation
            << " elapsed_ms=" << m_timing.elapsed_ms(GenerationToastTiming::Clock::now())
            << " body_width_dip=" << ToDIP(m_body.x) << " body_height_dip=" << ToDIP(m_body.y);
}
}
