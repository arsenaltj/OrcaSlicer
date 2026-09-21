#pragma once
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace Orca::ImageMapProbe {
using RGB = std::array<float, 3>;
struct Filament { std::string slot_id; RGB rgb {}; };
struct SlotWeight { std::string slot_id; float weight {0}; };
struct MixResult {
    bool ok {false};
    std::string error, solver_identity;
    std::vector<SlotWeight> weights;
    RGB predicted_rgb {};
    float oklab_error {0};
    double candidate_build_ms {0}, solve_ms {0};
    size_t candidate_count {0};
    bool cache_hit {false};
};
// Generic mixture prediction only. Weights are not an ordered layer stack or
// an executable printing recipe, and the model is not filament-calibrated.
class IColorMixSolver {
public:
    virtual ~IColorMixSolver() = default;
    virtual MixResult solve(const RGB& target, const std::vector<Filament>&) = 0;
};
std::unique_ptr<IColorMixSolver> make_imagemap_solver();
}
