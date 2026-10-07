#pragma once

#include "ColorMatchingTypes.hpp"

namespace Slic3r::AI::ColorMatching {

// A strategy computes a candidate only. It never confirms or applies a project.
class IColorMatchingEngine {
public:
    virtual ~IColorMatchingEngine() = default;
    virtual const char* algorithm_id() const noexcept = 0;
    virtual const char* algorithm_version(const Input& input) const noexcept = 0;
    virtual Computation compute(const Input& input) const = 0;
};

std::shared_ptr<const IColorMatchingEngine> baseline_engine();
Computation compute(const Input& input);

} // namespace Slic3r::AI::ColorMatching
