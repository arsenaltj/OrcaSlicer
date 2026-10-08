#pragma once

#include <wx/window.h>

namespace Slic3r::GUI {

// Runs first-use setup inside the existing main window, then resumes startup.
bool run_startup_setup(wxWindow* parent);

}
