#pragma once

#include "slic3r/AI/ColorMatching/ColorMatchingEngine.hpp"
#include "slic3r/AI/ColorMatching/RegionColorMatching.hpp"
#include "ModelPreviewPalette.hpp"
#include "LocalPrintColorRecipes.hpp"
#include "LocalPrintColorLayeredSelection.hpp"

// Preserve the old include/namespace while desktop consumers migrate.
namespace Slic3r::GUI {
namespace LocalPrintColorMatching = AI::ColorMatching;
}
