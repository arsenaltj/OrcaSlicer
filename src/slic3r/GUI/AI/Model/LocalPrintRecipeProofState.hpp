#pragma once

#include "slic3r/AI/ColorMatching/LocalPrintRecipeProofState.hpp"

// Compatibility entry for existing desktop consumers.
namespace Slic3r::GUI {
namespace LocalPrintRecipeProofState = AI::ColorMatching::LocalPrintRecipeProofState;
}
