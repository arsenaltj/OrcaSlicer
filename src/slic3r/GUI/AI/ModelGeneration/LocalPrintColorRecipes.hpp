#pragma once

#include "slic3r/AI/Contracts/LocalPrintColorResult.hpp"
#include "slic3r/AI/Contracts/IPrintablePaletteProvider.hpp"
#include "slic3r/GUI/AI/Model/LocalPrintRecipeProofState.hpp"
#include "libslic3r/FilamentMixer.hpp"
#include "libslic3r/TextureToColor/ColorUtils.hpp"
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <functional>
#include <map>
#include <numeric>
#include <stdexcept>

namespace Slic3r::GUI::LocalPrintColorRecipes {

// This catalog proves legality against supplied constraints, not against an
// unsliced model. The Orca adapter must supply/revalidate real process limits
// and the region dimensions before a candidate can become an applied recipe.
using Material = AI::PrintSublayerMaterial;
using Process = AI::PrintSublayerProcess;
struct Calibration {
    std::string recipe_key; // Complete materials + process + ordered thicknesses.
    std::string source_id, source_sha256;
    AI::ColorEvidence evidence {AI::ColorEvidence::Unknown};
    AI::PrintRgb color {};
    double uncertainty_delta_e {0}; // Declared bound; not manufactured by the solver.
};
struct Input {
    std::vector<Material> materials;
    Process process;
    std::vector<Calibration> calibration;
    int ratio_step_percent {5}, minimum_component_percent {20}; // Search grid, not machine limits.
    std::function<bool()> cancelled;
};
struct Candidate {
    std::string id;
    AI::MixedColorRecipe recipe;
    AI::PrintRgb color {};
    AI::ColorEvidence evidence {AI::ColorEvidence::Estimated};
    std::string evidence_source, evidence_sha256;
    double uncertainty_delta_e {0};
    std::vector<std::vector<double>> sublayer_heights_mm;
    std::optional<AI::PrintColorRecipeProof> proof;
};
struct Catalog {
    std::string material_fingerprint, process_fingerprint;
    std::vector<AI::PhysicalFilamentChannel> physical_channels;
    std::vector<Candidate> candidates;
    std::string error;
    bool cancelled {false};
    size_t rejected_by_process {0};
    bool ok() const { return error.empty() && !cancelled; }
};
struct Ranking {
    size_t candidate {0};
    double mean_delta_e00 {0}, worst_delta_e00 {0};
    bool within_tolerance {false};
};
struct SurfaceColor { AI::PrintRgb color; double area {0}; };

inline Input from_workspace(const AI::PrintablePaletteSnapshot& snapshot)
{
    Input input;
    input.materials = snapshot.sublayer_materials;
    input.process = snapshot.sublayer_process;
    // Stale/mismatched typed data must not borrow the newer flat identity.
    if (input.process.material_fingerprint != snapshot.material_fingerprint ||
        input.process.process_fingerprint != snapshot.process_fingerprint ||
        input.materials.size() != snapshot.physical_channels.size()) {
        input.materials.clear(); input.process = {};
        return input;
    }
    for (size_t i = 0; i < input.materials.size(); ++i) {
        auto& a = input.materials[i].channel;
        const auto& b = snapshot.physical_channels[i];
        if (a.slot != b.slot || a.display_color != b.display_color || a.material_type != b.material_type || a.compatible != b.compatible) {
            input.materials.clear(); input.process = {}; return input;
        }
        if (!snapshot.material_metadata_complete) a.compatible = false;
    }
    return input;
}

inline std::string digest(const nlohmann::json& value)
{
    return LocalPrintRecipeProofState::digest(value);
}
inline AI::PrintRgb rgb(const std::string& hex)
{
    AI::PrintRgb result {};
    for (size_t c = 0; c < 3; ++c) result[c] = float(std::stoul(hex.substr(1 + 2*c, 2), nullptr, 16)) / 255.f;
    return result;
}
inline std::string hex(const AI::PrintRgb& rgb)
{
    const char* digits = "0123456789ABCDEF";
    std::string value = "#";
    for (float c : rgb) { const auto b = unsigned(std::lround(c*255)); value += digits[b >> 4]; value += digits[b & 15]; }
    return value;
}
inline double delta_e(const AI::PrintRgb& a, const AI::PrintRgb& b)
{
    return tex2color::color_utils::calc_rgb_color_difference_by_ciede2000_srgb01(
        {double(a[0]), double(a[1]), double(a[2])}, {double(b[0]), double(b[1]), double(b[2])});
}

inline Catalog enumerate(const Input& input)
{
    Catalog output;
    auto reject = [&](const char* message) { output.error = message; return output; };
    auto cancelled = [&] { return input.cancelled && input.cancelled(); };
    if (cancelled()) { output.cancelled = true; return output; }
    const auto& p = input.process;
    if (p.material_fingerprint.empty() || p.process_fingerprint.empty() ||
        p.surface_condition.empty() || p.measurement_condition.empty())
        return reject("Recipe evaluation requires material, process and observation identities.");
    if (!p.sublayers_enabled || !p.first_layer_unsplit)
        return reject("This catalog requires the native sublayer mode with an unsplit first layer.");
    auto positive = [](double v) { return std::isfinite(v) && v > 0; };
    if (!positive(p.z_resolution_mm) || !positive(p.region_width_mm) || !positive(p.region_height_mm) ||
        p.layer_heights_mm.empty() || p.layer_heights_mm.size() > 4096)
        return reject("Recipe evaluation requires finite region dimensions and layer heights.");
    for (double h : p.layer_heights_mm)
        if (!positive(h)) return reject("Invalid layer height.");
    if (input.ratio_step_percent < 1 || input.ratio_step_percent > 50 || 100 % input.ratio_step_percent ||
        input.minimum_component_percent < input.ratio_step_percent || input.minimum_component_percent > 50 ||
        input.minimum_component_percent % input.ratio_step_percent)
        return reject("Invalid bounded recipe search grid.");
    std::vector<AI::PhysicalFilamentChannel> channels;
    for (const auto& m : input.materials) channels.push_back(m.channel);
    if (!AI::is_valid_physical_channel_set(channels)) return reject("Invalid physical material set.");
    output.material_fingerprint = p.material_fingerprint;
    output.process_fingerprint = p.process_fingerprint;
    output.physical_channels = channels;
    // Bound allocation/work before enumerating. Do not silently truncate the
    // catalog: a partial table would distort nearest-candidate claims.
    const size_t count = channels.size();
    const int units = 100 / input.ratio_step_percent;
    const int minimum_units = input.minimum_component_percent / input.ratio_step_percent;
    const size_t pair_ratios = size_t(std::max(0, units - 2*minimum_units + 1));
    const int triple_extra = units - 3*minimum_units;
    const size_t triple_ratios = triple_extra < 0 ? 0 : size_t((triple_extra+1)*(triple_extra+2)/2);
    const size_t pairs = count*(count-1)/2*pair_ratios;
    const size_t triples = count < 3 ? 0 : count*(count-1)*(count-2)/6*triple_ratios;
    if (pairs + triples > 20000 || (2*pairs + 3*triples)*p.layer_heights_mm.size() > 2000000)
        return reject("Recipe search exceeds the bounded catalog; reduce the grid or distinct layer-height set.");
    auto materials = input.materials;
    std::sort(materials.begin(), materials.end(), [](const auto& a, const auto& b) { return a.channel.slot < b.channel.slot; });
    nlohmann::json context = LocalPrintRecipeProofState::context(materials,p);
    for (const auto& m : materials) {
        if (m.identity.empty() || m.channel.material_type.empty() || !positive(m.min_layer_mm) ||
            !positive(m.max_layer_mm) || m.min_layer_mm > m.max_layer_mm || !positive(m.line_width_mm) ||
            !positive(m.temperature_c) || !positive(m.min_temperature_c) || !positive(m.max_temperature_c) ||
            m.min_temperature_c > m.max_temperature_c)
            return reject("Incomplete material identity or process limits.");
    }
    std::map<std::string, const Calibration*> calibration;
    for (const auto& c : input.calibration) {
        if (!AI::is_lowercase_sha256(c.recipe_key) || c.source_id.empty() || !AI::is_lowercase_sha256(c.source_sha256) ||
            (c.evidence != AI::ColorEvidence::Measured && c.evidence != AI::ColorEvidence::Interpolated) ||
            !AI::valid_print_rgb(c.color) || !std::isfinite(c.uncertainty_delta_e) || c.uncertainty_delta_e < 0 ||
            !calibration.emplace(c.recipe_key, &c).second)
            return reject("Invalid or ambiguous condition-bound calibration.");
    }
    auto evaluate = [&](const std::vector<size_t>& members, const std::vector<int>& weights) {
        Candidate candidate;
        nlohmann::json recipe_context = context;
        recipe_context["components"] = nlohmann::json::array();
        for (size_t i = 0; i < members.size(); ++i) {
            const auto& m = materials[members[i]];
            if (!m.channel.compatible || m.channel.material_type != materials[members.front()].channel.material_type ||
                m.temperature_c < m.min_temperature_c || m.temperature_c > m.max_temperature_c ||
                p.region_width_mm + 1e-9 < m.line_width_mm) { ++output.rejected_by_process; return; }
            candidate.recipe.components.push_back({m.channel.slot, double(weights[i])/100});
            recipe_context["components"].push_back({m.channel.slot, weights[i]});
        }
        for (double height : p.layer_heights_mm) {
            std::vector<double> row;
            if (p.region_height_mm + 1e-9 < height) { ++output.rejected_by_process; return; }
            for (size_t i = 0; i < members.size(); ++i) {
                const auto& m = materials[members[i]];
                const double sub = height * weights[i]/100;
                const double units = sub / p.z_resolution_mm;
                if (sub + 1e-9 < m.min_layer_mm || sub > m.max_layer_mm + 1e-9 ||
                    !std::isfinite(units) || std::abs(units - std::round(units)) > 1e-7) {
                    ++output.rejected_by_process; return;
                }
                row.push_back(sub);
            }
            candidate.sublayer_heights_mm.push_back(std::move(row));
        }
        candidate.id = digest(recipe_context);
        const auto sample = calibration.find(candidate.id);
        if (sample != calibration.end()) {
            const auto& c = *sample->second;
            candidate.color = c.color; candidate.evidence = c.evidence;
            candidate.evidence_source = c.source_id; candidate.evidence_sha256 = c.source_sha256;
            candidate.uncertainty_delta_e = c.uncertainty_delta_e;
        } else {
            // Reuse the polynomial primitive, deliberately bypassing the
            // unscoped HEX-only LUT in blend_color_multi. This is an estimate.
            candidate.color = LocalPrintRecipeProofState::estimate_color(materials,candidate.recipe);
            candidate.evidence_source = "local-polynomial-estimate-v1";
        }
        candidate.recipe.target_color = hex(candidate.color);
        AI::PrintColorTarget checked;
        checked.recipe=candidate.recipe;checked.output=candidate.color;checked.candidate_id=candidate.id;checked.evidence=candidate.evidence;
        checked.recipe_proof.emplace();auto& proof=*checked.recipe_proof;
        proof.materials=materials;proof.process=p;proof.sublayer_heights_mm=candidate.sublayer_heights_mm;
        proof.evidence_source=candidate.evidence_source;proof.evidence_sha256=candidate.evidence_sha256;
        proof.uncertainty_delta_e=candidate.uncertainty_delta_e;
        proof.checksum=LocalPrintRecipeProofState::digest(LocalPrintRecipeProofState::payload(checked));
        candidate.proof=std::move(checked.recipe_proof);
        output.candidates.push_back(std::move(candidate));
    };
    const int step = input.ratio_step_percent, minimum = input.minimum_component_percent;
    for (size_t a = 0; a < materials.size(); ++a) for (size_t b = a+1; b < materials.size(); ++b) {
        for (int wa = minimum; wa <= 100-minimum; wa += step) {
            if (cancelled()) { output.cancelled = true; output.candidates.clear(); return output; }
            evaluate({a,b}, {wa,100-wa});
        }
        for (size_t c = b+1; c < materials.size(); ++c)
            for (int wa = minimum; wa <= 100-2*minimum; wa += step)
                for (int wb = minimum; wb <= 100-wa-minimum; wb += step) {
                    if (cancelled()) { output.cancelled = true; output.candidates.clear(); return output; }
                    evaluate({a,b,c}, {wa,wb,100-wa-wb});
                }
    }
    return output;
}

// Score actual target-face samples, not only a quantization center. Keep the
// nearest legal candidate even outside tolerance. Ranking does not perform the
// final whole-model contrast/lock selection and cannot mark a result confirmed.
inline std::vector<Ranking> rank(const Catalog& catalog, const std::vector<SurfaceColor>& surface,
    double tolerance, const std::function<bool()>& cancelled = {})
{
    if (!catalog.ok() || surface.empty() || !std::isfinite(tolerance) || tolerance < 0)
        throw std::invalid_argument("Invalid recipe ranking input.");
    double area = 0;
    for (const auto& f : surface) {
        if (!AI::valid_print_rgb(f.color) || !std::isfinite(f.area) || f.area <= 0)
            throw std::invalid_argument("Invalid recipe surface sample.");
        area += f.area;
    }
    if (!std::isfinite(area)) throw std::invalid_argument("Recipe surface area overflow.");
    std::vector<Ranking> ranking;
    for (size_t i = 0; i < catalog.candidates.size(); ++i) {
        Ranking score; score.candidate = i;
        for (size_t f = 0; f < surface.size(); ++f) {
            if (f % 4096 == 0 && cancelled && cancelled()) return {};
            const double error = delta_e(surface[f].color, catalog.candidates[i].color);
            score.mean_delta_e00 += error * (surface[f].area / area);
            score.worst_delta_e00 = std::max(score.worst_delta_e00, error);
        }
        score.within_tolerance = catalog.candidates[i].evidence != AI::ColorEvidence::Unknown &&
            score.worst_delta_e00 + catalog.candidates[i].uncertainty_delta_e <= tolerance;
        ranking.push_back(score);
    }
    std::stable_sort(ranking.begin(), ranking.end(), [&](const auto& a, const auto& b) {
        if (a.within_tolerance != b.within_tolerance) return a.within_tolerance;
        if (a.mean_delta_e00 != b.mean_delta_e00) return a.mean_delta_e00 < b.mean_delta_e00;
        const auto& ca = catalog.candidates[a.candidate]; const auto& cb = catalog.candidates[b.candidate];
        if (ca.recipe.components.size() != cb.recipe.components.size()) return ca.recipe.components.size() < cb.recipe.components.size();
        return ca.id < cb.id;
    });
    return ranking;
}
} // namespace Slic3r::GUI::LocalPrintColorRecipes
