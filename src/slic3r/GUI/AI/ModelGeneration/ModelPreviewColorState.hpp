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

struct ModelPreviewInitialColorState {
    std::string geometry_id;
    ModelPreviewColorState trial;
    PreviewPalette::AutomaticColorBasis basis;
    std::vector<PreviewPalette::Color> suggestions;
    bool default_ai_palette {false};

    bool matches_default_workbench_palette(const std::string& current_geometry,
        const ModelPreviewColorState& current) const
    {
        return default_ai_palette && current_geometry == geometry_id && current.enabled && current.source == 0 &&
            current.suite_id.empty() && current.count == trial.count && current.colors == trial.colors &&
            current.mapping_colors == trial.mapping_colors && current.locks == trial.locks &&
            current.fidelity == trial.fidelity && current.automatic_basis_signature == basis.basis_signature;
    }
};
}
