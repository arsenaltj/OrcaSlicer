#pragma once

#include "RedesignWidgets.hpp"
#include <wx/stattext.h>
#include <wx/sizer.h>
#include <array>
#include <cmath>

namespace Slic3r::GUI {
inline constexpr std::array<const char*, 9> smart_creation_styles {
    "sculpture", "realistic", "portrait_sketch", "relief", "ink_relief", "diorama", "cartoon", "low_poly", "custom"};

// Figma uses logical pixels at 96 DPI. Point sizes already scale with wx's DPI.
inline wxFont smart_creation_font(int pixels, wxFontWeight weight = wxFONTWEIGHT_NORMAL)
{
    wxFontInfo info(pixels * 72.0 / 96.0);
    info.Family(wxFONTFAMILY_SWISS).Weight(weight);
    const auto family = RedesignTheme::font_family();
    if (!family.empty()) info.FaceName(family);
    return wxFont(info);
}

inline wxColour smart_creation_opaque_colour(const wxColour& colour, const wxColour& background)
{
    const int alpha = colour.Alpha();
    const auto blend = [alpha](int foreground, int behind) {
        return (foreground * alpha + behind * (255 - alpha) + 127) / 255;
    };
    return wxColour(blend(colour.Red(), background.Red()), blend(colour.Green(), background.Green()),
                    blend(colour.Blue(), background.Blue()));
}

inline void style_smart_creation_text(wxWindow* window, int pixels,
    const wxColour& colour, wxFontWeight weight = wxFONTWEIGHT_NORMAL)
{
    window->SetFont(smart_creation_font(pixels, weight));
    // Native MSW text and wxDC ignore alpha; composite against the actual surface.
    const auto surface = window->GetParent() ? window->GetParent()->GetBackgroundColour() : window->GetBackgroundColour();
    window->SetForegroundColour(smart_creation_opaque_colour(colour, surface));
}

inline wxString smart_creation_style_label(const std::string& style)
{
    if (style == "portrait_sketch") return wxString::FromUTF8("肖像速写");
    if (style == "sculpture") return wxString::FromUTF8("雕塑");
    if (style == "cartoon") return wxString::FromUTF8("手办");
    if (style == "realistic") return wxString::FromUTF8("多色写实");
    if (style == "low_poly") return wxString::FromUTF8("低多边形");
    if (style == "relief") return wxString::FromUTF8("浮雕");
    if (style == "ink_relief") return wxString::FromUTF8("水墨版画浮雕");
    if (style == "diorama") return wxString::FromUTF8("微缩场景");
    if (style == "custom") return wxString::FromUTF8("自定义风格");
    return wxString::FromUTF8(style);
}

class SmartUploadTile final : public wxPanel {
public:
    SmartUploadTile(wxWindow* parent, const wxBitmap& icon)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, parent->FromDIP(wxSize(40, 40))), m_icon(icon)
    {
        SetMinSize(parent->FromDIP(wxSize(40, 40)));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(parent->GetBackgroundColour());
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(GetBackgroundColour())); dc.Clear();
            auto gc = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc));
            if (!gc) return;
            const auto size = GetClientSize();
            gc->SetBrush(wxBrush(wxColour(56, 56, 58)));
            gc->SetPen(wxPen(wxColour(91, 91, 93), std::max(1, FromDIP(1)), wxPENSTYLE_SHORT_DASH));
            gc->DrawRoundedRectangle(0, 0, size.x - 1, size.y - 1, FromDIP(7));
            if (m_icon.IsOk())
                dc.DrawBitmap(m_icon, (size.x - m_icon.GetWidth()) / 2, (size.y - m_icon.GetHeight()) / 2, true);
        });
    }
private:
    wxBitmap m_icon;
};

class SmartStyleCard final : public wxPanel {
public:
    SmartStyleCard(wxWindow* parent, const char* id, const wxString& caption, std::function<void()> select)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, parent->FromDIP(wxSize(96, 106))), m_caption(caption)
    {
        SetMinSize(parent->FromDIP(wxSize(96, 106)));
        SetBackgroundStyle(wxBG_STYLE_PAINT); SetCanFocus(true);
        SetFont(smart_creation_font(13));
        SetName("smart-style-" + wxString::FromUTF8(id)); SetToolTip(caption);
        wxImage image(wxString::FromUTF8(Slic3r::resources_dir() + "/images/smart_creation/" + id + ".png"));
        if (image.IsOk()) m_image = image;
        Bind(wxEVT_LEFT_UP, [this, select](wxMouseEvent&) { if (IsEnabled()) { SetFocus(); select(); } });
        Bind(wxEVT_KEY_DOWN, [this, select](wxKeyEvent& e) {
            if (IsEnabled() && (e.GetKeyCode() == WXK_SPACE || e.GetKeyCode() == WXK_RETURN)) select(); else e.Skip();
        });
        Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& e) { Refresh(); e.Skip(); });
        Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& e) { Refresh(); e.Skip(); });
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this); dc.SetBackground(wxBrush(RedesignTheme::panel_colour())); dc.Clear();
            const auto sz = GetClientSize(); const int h = sz.y - FromDIP(26);
            auto gc = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc));
            if (gc) {
                gc->SetPen(*wxTRANSPARENT_PEN); gc->SetBrush(wxBrush(wxColour(46, 46, 49)));
                gc->DrawRoundedRectangle(0, 0, sz.x, sz.y, FromDIP(12));
            }
            if (m_image.IsOk()) {
                const wxSize target(std::max(1, sz.x), std::max(1, h));
                if (!m_thumbnail.IsOk() || m_thumbnail_size != target) {
                    const double scale = std::max(double(target.x) / m_image.GetWidth(),
                                                  double(target.y) / m_image.GetHeight());
                    const int scaled_width = std::max(target.x, int(std::ceil(m_image.GetWidth() * scale)));
                    const int scaled_height = std::max(target.y, int(std::ceil(m_image.GetHeight() * scale)));
                    wxImage thumbnail = m_image.Scale(scaled_width, scaled_height, wxIMAGE_QUALITY_HIGH)
                        .GetSubImage(wxRect((scaled_width - target.x) / 2, (scaled_height - target.y) / 2,
                                            target.x, target.y));
                    if (!thumbnail.HasAlpha()) thumbnail.InitAlpha();
                    unsigned char* alpha = thumbnail.GetAlpha();
                    const int radius = std::min({FromDIP(12), target.x / 2, target.y});
                    const double centre = radius - 0.5;
                    const double radius_squared = double(radius) * radius;
                    for (int y = 0; y < radius; ++y) {
                        for (int x = 0; x < radius; ++x) {
                            const double dx = centre - x;
                            const double dy = centre - y;
                            if (dx * dx + dy * dy > radius_squared) {
                                alpha[y * target.x + x] = 0;
                                alpha[y * target.x + target.x - 1 - x] = 0;
                            }
                        }
                    }
                    m_thumbnail = wxBitmap(thumbnail);
                    m_thumbnail_size = target;
                }
                dc.DrawBitmap(m_thumbnail, 0, 0, true);
            }
            dc.SetFont(GetFont());
            dc.SetTextForeground(!IsEnabled() ? RedesignTheme::secondary_text_colour() :
                smart_creation_opaque_colour(m_selected ? wxColour(255, 255, 255, 217) :
                    wxColour(255, 255, 255, 179), wxColour(46, 46, 49)));
            const auto label = wxControl::Ellipsize(m_caption, dc, wxELLIPSIZE_END, sz.x - FromDIP(8));
            dc.DrawText(label, (sz.x - dc.GetTextExtent(label).x) / 2, h + FromDIP(4));
            if (gc && (m_selected || HasFocus())) {
                gc->SetBrush(*wxTRANSPARENT_BRUSH);
                gc->SetPen(wxPen(wxColour(37, 111, 255), std::max(1, FromDIP(1))));
                gc->DrawRoundedRectangle(0, 0, sz.x - 1, sz.y - 1, FromDIP(12));
            }
        });
    }
    void set_selected(bool selected) {
        if (m_selected == selected) return;
        m_selected = selected;
        SetFont(smart_creation_font(13, selected ? wxFONTWEIGHT_MEDIUM : wxFONTWEIGHT_NORMAL));
        Refresh();
    }
private:
    wxImage m_image;
    wxBitmap m_thumbnail;
    wxSize m_thumbnail_size;
    wxString m_caption;
    bool m_selected {false};
};

} // namespace Slic3r::GUI
