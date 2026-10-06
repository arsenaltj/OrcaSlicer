#pragma once

#include <wx/window.h>

namespace Slic3r::GUI {

// Runs the complete native first-use session against the existing application.
bool run_startup_setup(wxWindow* parent);

}
