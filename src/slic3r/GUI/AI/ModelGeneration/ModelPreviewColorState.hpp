#pragma once
#include "slic3r/GUI/AI/Model/ColorTrialState.hpp"
#include <wx/string.h>

namespace Slic3r::GUI {
// Serialized color state plus display diagnostics; no controls or application.
struct ModelPreviewColorState : AI::ColorTrialPersistence::State {
    wxString project_signature, notice;
    // Transient diagnostic shown by the 3D workbench when an Honor suite
    // request has to use global nearest-colour matching.
    wxString honor_suite_fallback_reason;
    // Transient source snapshot used by the printer-filament configuration
    // importer. It is intentionally separate from the serialized trial state.
    std::vector<GUI::PreviewPalette::Color> manual_colors;
};
}
