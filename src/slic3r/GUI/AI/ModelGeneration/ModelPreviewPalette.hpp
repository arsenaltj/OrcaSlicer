#pragma once

#include "slic3r/AI/ColorMatching/ModelPreviewPalette.hpp"

// Compatibility entry for existing desktop consumers.
namespace Slic3r::GUI::PreviewPalette {
// Desktop-only portrait mapping extends this namespace in another header.
using namespace AI::ColorMatching::PreviewPalette;
}
