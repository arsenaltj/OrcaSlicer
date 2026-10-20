#pragma once

#include <wx/colour.h>

class wxScrolledWindow;

namespace Slic3r::GUI {
// Only the embedded native workspace opts into this appearance. The parameter
// page, its scroll units and its focus/search positioning remain unchanged.
void set_parameter_scrollbar_theme(wxScrolledWindow* page, bool enabled,
                                   const wxColour& track = wxColour(35, 35, 38));
}
