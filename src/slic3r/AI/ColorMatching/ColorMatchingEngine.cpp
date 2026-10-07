#include "ColorMatchingEngine.hpp"
#include "RegionColorMatching.hpp"

namespace Slic3r::AI::ColorMatching {
namespace {
class RegionColorMatchingEngine final : public IColorMatchingEngine {
public:
    const char* algorithm_id() const noexcept override { return "region-matching"; }
    const char* algorithm_version(const Input& input) const noexcept override
    {
        return print_color_mode(input.identity.requested_color_count) == PrintColorMode::Layered
            ? "region-layered-v2" : "region-direct-v5";
    }
    Computation compute(const Input& input) const override { return compute_baseline(input); }
};
}

std::shared_ptr<const IColorMatchingEngine> baseline_engine()
{
    static const auto engine = std::make_shared<const RegionColorMatchingEngine>();
    return engine;
}

Computation compute(const Input& input)
{
    // Retain the strategy for the complete call, including recursive baseline
    // comparisons. A later configuration change cannot change this operation.
    const auto engine = input.engine ? input.engine : baseline_engine();
    auto result = engine->compute(input);
    result.algorithm_id = engine->algorithm_id();
    result.result.algorithm_version = engine->algorithm_version(input);
    result.result.confirmed = false;
    return result;
}
} // namespace Slic3r::AI::ColorMatching
