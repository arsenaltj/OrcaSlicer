#pragma once
#include "slic3r/GUI/AI/Model/ColorTrialState.hpp"
#include <wx/string.h>

namespace Slic3r::GUI {
// Serialized color state plus display diagnostics; no controls or application.
struct ModelPreviewColorState : AI::ColorTrialPersistence::State {
    wxString project_signature, notice;
    // Transient source snapshot used by the printer-filament configuration
    // importer. It is intentionally separate from the serialized trial state.
    std::vector<GUI::PreviewPalette::Color> manual_colors;
};
}
