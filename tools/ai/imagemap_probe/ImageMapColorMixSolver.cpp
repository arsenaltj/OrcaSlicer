#include "IColorMixSolver.hpp"
#include "ColorSolver.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

namespace Orca::ImageMapProbe {
namespace {
using Clock = std::chrono::steady_clock;
double ms(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
bool valid(const RGB& color) {
    return std::all_of(color.begin(), color.end(), [](float c) { return std::isfinite(c) && c >= 0.f && c <= 1.f; });
}
class Solver final : public IColorMixSolver {
    std::map<std::string, Slic3r::ColorSolverCandidateSet> m_cache;
public:
    MixResult solve(const RGB& target, const std::vector<Filament>& filaments) override {
        MixResult result;
        result.solver_identity = "OrcaSlicer-ImageMap/92548381056dbf72836b0a1bdc455f238218dbfb/PigmentPainter/ClosestMix/Oklab/20units";
        if (!valid(target) || filaments.empty() || filaments.size() > 6) {
            result.error = "The probe requires normalized sRGB and one to six filaments."; return result;
        }
        std::set<std::string> ids;
        std::vector<RGB> colors;
        std::ostringstream key;
        key << std::hexfloat;
        for (const auto& filament : filaments) {
            if (filament.slot_id.empty() || !ids.insert(filament.slot_id).second || !valid(filament.rgb)) {
                result.error = "Filament IDs must be unique and RGB values finite and normalized."; return result;
            }
            key << filament.slot_id.size() << ':' << filament.slot_id;
            for (const auto channel : filament.rgb) key << ':' << channel;
            colors.push_back(filament.rgb);
        }
        auto entry = m_cache.find(key.str());
        result.cache_hit = entry != m_cache.end();
        if (entry == m_cache.end()) {
            const auto start = Clock::now();
            auto candidates = Slic3r::build_color_solver_candidates(colors, Slic3r::ColorSolverMixModel::PigmentPainter, 20);
            result.candidate_build_ms = ms(start);
            if (candidates.empty()) { result.error = "The upstream candidate builder returned no candidates."; return result; }
            entry = m_cache.emplace(key.str(), std::move(candidates)).first;
        }
        result.candidate_count = entry->second.rgbs.size() / 3;
        const auto start = Clock::now();
        const auto weights = Slic3r::solve_color_solver_weights_for_target(entry->second, target,
            Slic3r::ColorSolverLookupMode::ClosestMix, Slic3r::ColorSolverMode::Oklab);
        if (weights.size() != filaments.size()) { result.error = "The upstream solver returned an invalid weight vector."; return result; }
        float total = 0.f;
        for (size_t i = 0; i < weights.size(); ++i) {
            if (!std::isfinite(weights[i]) || weights[i] < 0.f) { result.error = "Invalid predicted weight."; return result; }
            total += weights[i];
            result.weights.push_back({filaments[i].slot_id, weights[i]});
        }
        if (std::abs(total - 1.f) > .0001f) { result.error = "Mixture weights do not sum to one."; return result; }
        result.predicted_rgb = Slic3r::mix_color_solver_components(colors, weights, Slic3r::ColorSolverMixModel::PigmentPainter);
        if (!valid(result.predicted_rgb)) { result.error = "Invalid predicted RGB."; return result; }
        const auto a = Slic3r::color_solver_oklab_from_srgb(target), b = Slic3r::color_solver_oklab_from_srgb(result.predicted_rgb);
        for (int channel = 0; channel < 3; ++channel) result.oklab_error += (a[channel] - b[channel]) * (a[channel] - b[channel]);
        result.oklab_error = std::sqrt(result.oklab_error);
        result.solve_ms = ms(start);
        result.ok = true;
        return result;
    }
};
}
std::unique_ptr<IColorMixSolver> make_imagemap_solver() { return std::make_unique<Solver>(); }
}
