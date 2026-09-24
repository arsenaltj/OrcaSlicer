#pragma once

#include "IPrintablePaletteProvider.hpp"

namespace Slic3r::AI {
// Evidence of a candidate checked against supplied constraints. Distinct layer
// heights are not a sliced model's layer schedule or a physical measurement.
struct PrintColorRecipeProof {
    std::string schema {"orcaslicer.local-recipe-proof.v1"};
    std::vector<PrintSublayerMaterial> materials;
    PrintSublayerProcess process;
    std::vector<std::vector<double>> sublayer_heights_mm;
    std::string evidence_source, evidence_sha256;
    double uncertainty_delta_e {0};
    std::string checksum;
};
} // namespace Slic3r::AI
