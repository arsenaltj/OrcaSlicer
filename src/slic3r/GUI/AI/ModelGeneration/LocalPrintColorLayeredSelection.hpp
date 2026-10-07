#pragma once

#include "slic3r/AI/ColorMatching/LocalPrintColorLayeredSelection.hpp"
#include "LocalPrintColorRecipes.hpp"

// Compatibility entry for existing desktop consumers.
namespace Slic3r::GUI {
namespace LocalPrintColorLayeredSelection = AI::ColorMatching::LocalPrintColorLayeredSelection;
}
