#pragma once

#include "LocalPrintColorRecipes.hpp"
#include <functional>
#include <memory>

namespace Slic3r::AI::ColorMatching {
class IColorMatchingEngine;

using AI::PrintRgb;
struct FaceSample { PrintRgb color; double area; };
using ContrastConstraint = AI::PrintColorContrast;
struct Input {
    AI::LocalPrintColorResult identity;
    std::vector<FaceSample> faces;
    std::vector<ContrastConstraint> contrasts;
    double tolerance {5}; // Experiment setting, not a universal pass threshold.
    double important_area_floor {0.02};
    std::function<bool()> cancelled;
    // Frozen once by the shared page worker; baseline/refinement comparisons
    // reuse the same forward predictions. Selection/application is separate.
    std::shared_ptr<const LocalPrintColorRecipes::Catalog> recipe_catalog;
    // Optional immutable strategy captured with this operation's input.
    std::shared_ptr<const IColorMatchingEngine> engine;
};
struct Computation {
    AI::LocalPrintColorResult result;
    std::string error;
    bool cancelled {false};
    std::string algorithm_id;
    bool ok() const { return error.empty() && !cancelled; }
};

} // namespace Slic3r::AI::ColorMatching
