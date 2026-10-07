#pragma once

#include <wx/colour.h>
#include <wx/font.h>
#include <wx/fontenum.h>
#include <wx/window.h>

namespace Slic3r::GUI::RedesignTheme {

inline wxColour background_colour()
{
    return wxColour(49, 49, 53);
}

inline wxColour flow_background_colour()
{
    return wxColour(49, 49, 54);
}

inline wxColour panel_colour()
{
    return wxColour(35, 35, 38);
}

inline wxColour control_colour()
{
    return wxColour(25, 25, 27);
}

inline wxColour primary_text_colour()
{
    return wxColour(255, 255, 255, 220);
}

inline wxColour secondary_text_colour()
{
    return wxColour(255, 255, 255, 150);
}

inline wxColour accent_colour()
{
    return wxColour(255, 194, 39);
}

inline wxColour divider_colour()
{
    return wxColour(255, 255, 255, 28);
}

inline wxString font_family()
{
    return wxFontEnumerator::IsValidFacename("HONOR Sans Design") ? "HONOR Sans Design" :
           wxFontEnumerator::IsValidFacename("HarmonyOS Sans SC") ? "HarmonyOS Sans SC" :
           wxFontEnumerator::IsValidFacename("Microsoft YaHei UI") ? "Microsoft YaHei UI" :
           wxString();
}

inline void style_text(wxWindow* window, const wxColour& colour, int point_size, bool bold = false)
{
    window->SetForegroundColour(colour);
    wxFontInfo font(point_size);
    font.Family(wxFONTFAMILY_SWISS).Bold(bold);
    const wxString family = font_family();
    if (!family.empty())
        font.FaceName(family);
    window->SetFont(wxFont(font));
}

inline void style_medium_text(wxWindow* window, const wxColour& colour, int point_size)
{
    style_text(window, colour, point_size);
    wxFont font = window->GetFont();
    font.SetWeight(wxFONTWEIGHT_MEDIUM);
    window->SetFont(font);
}

}
