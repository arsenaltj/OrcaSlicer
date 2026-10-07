#pragma once

#include "slic3r/AI/ColorMatching/LocalPrintColorRecipes.hpp"
#include "slic3r/GUI/AI/Model/LocalPrintRecipeProofState.hpp"

// Compatibility entry for existing desktop consumers.
namespace Slic3r::GUI {
namespace LocalPrintColorRecipes = AI::ColorMatching::LocalPrintColorRecipes;
}
