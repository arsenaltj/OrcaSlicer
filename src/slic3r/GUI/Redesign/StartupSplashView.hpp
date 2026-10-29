#pragma once

#include "RedesignTheme.hpp"
#include "HonorBrandMark.hpp"
#include "libslic3r_version.h"

#include <algorithm>
#include <memory>
#include <wx/control.h>
#include <wx/dc.h>
#include <wx/graphics.h>

namespace Slic3r::GUI::StartupSplashView {

inline wxSize logical_size()
{
    return wxSize(590, 590);
}

inline wxFont font(wxWindow* window, int design_pixels)
{
    wxFont result(wxFontInfo(design_pixels * 0.75).Family(wxFONTFAMILY_SWISS));
    const wxString family = RedesignTheme::font_family();
    if (!family.empty())
        result.SetFaceName(family);
    result.SetPixelSize(wxSize(0, window->FromDIP(design_pixels)));
    return result;
}

inline void paint(wxWindow* window, wxDC& dc, const wxString& version,
                  const wxString& message, int progress, const wxBitmap& logo)
{
    const wxSize bounds = window->GetClientSize();
    dc.SetBackground(wxBrush(wxColour(32, 32, 34)));
    dc.Clear();
    if (bounds.x <= 0 || bounds.y <= 0)
        return;

    // Compress spacing on small high-DPI displays, keeping text at its native DPI size.
    const double spacing = std::min(1.0, double(bounds.y) / window->FromDIP(590));
    const auto y = [window, spacing](int value) { return int(window->FromDIP(value) * spacing); };
    const int icon_size = std::min(y(176), bounds.x - window->FromDIP(48));
    auto graphics = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::CreateFromUnknownDC(dc));
    if (graphics && icon_size > 0 && !logo.IsOk()) {
        graphics->SetPen(*wxTRANSPARENT_PEN);
        graphics->SetBrush(*wxWHITE_BRUSH);
        graphics->DrawRoundedRectangle((bounds.x - icon_size) / 2.0, y(98),
                                       icon_size, icon_size, y(24));
    }
    graphics.reset();
    if (logo.IsOk())
        dc.DrawBitmap(logo, (bounds.x - logo.GetWidth()) / 2, y(98), true);

    const auto draw_label = [&](const wxString& label, int pixels, int top, const wxColour& colour) {
        int fitted_pixels = pixels;
        dc.SetFont(font(window, fitted_pixels));
        while (dc.GetTextExtent(label).x > bounds.x - window->FromDIP(48) && fitted_pixels > 16)
            dc.SetFont(font(window, --fitted_pixels));
        dc.SetTextForeground(colour);
        const wxString fitted = wxControl::Ellipsize(label, dc, wxELLIPSIZE_END,
                                                     std::max(1, bounds.x - window->FromDIP(48)));
        const wxSize extent = dc.GetTextExtent(fitted);
        dc.DrawText(fitted, (bounds.x - extent.x) / 2, top);
        return extent.y;
    };
    // Keep the name and version together using the rendered logo and text heights.
    const int logo_bottom = y(98) + (logo.IsOk() ? logo.GetHeight() : std::max(0, icon_size));
    const int name_top = logo_bottom + y(40);
    const int name_height = draw_label(wxString::FromUTF8(SLIC3R_APP_NAME), 40, name_top,
                                      wxColour(255, 204, 48));
    draw_label(version, 24, name_top + name_height + y(18), wxColour(117, 117, 118));
    draw_label(message, 20, y(516) + window->FromDIP(2), wxColour(117, 117, 118));

    const int bar_height = window->FromDIP(8);
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(wxColour(29, 29, 31)));
    dc.DrawRectangle(0, bounds.y - bar_height, bounds.x, bar_height);
    const int filled = bounds.x * std::clamp(progress, 0, 100) / 100;
    if (filled > 0) {
        dc.SetBrush(wxBrush(wxColour(37, 111, 255)));
        dc.DrawRectangle(0, bounds.y - bar_height, filled, bar_height);
    }
}

}
