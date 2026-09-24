#pragma once

#include "ColorIntent.hpp"
#include "PrintColorRecipeProof.hpp"
#include <algorithm>
#include <limits>
#include <set>
#include <utility>

namespace Slic3r::AI {

// Local manufacturing intent. Provider ColorIntent v1 remains unchanged.
inline constexpr size_t kMaxLocalPrintTargetColors = 32;
inline constexpr const char* kLocalPrintColorSchema = "orcaslicer.local-print-color.v1";
using PrintRgb = std::array<float, 3>; // normalized, nonlinear sRGB
enum class PrintColorMode { Direct, Layered };
enum class ColorEvidence { Measured, Interpolated, Estimated, Unknown };
inline PrintColorMode print_color_mode(size_t requested) { return requested <= 6 ? PrintColorMode::Direct : PrintColorMode::Layered; }
inline bool valid_print_rgb(const PrintRgb& rgb)
{
    return std::all_of(rgb.begin(), rgb.end(), [](float c) { return std::isfinite(c) && c >= 0.f && c <= 1.f; });
}

struct PrintColorRegion {
    std::string id;
    std::string subject_id;
    std::string label;
    double confidence {0};
    bool user_protected {false};
    std::vector<size_t> faces;
    bool protect_color {false};
    std::optional<size_t> locked_physical_slot;
};

struct PrintColorTarget {
    PrintRgb source {};
    PrintRgb output {};
    double area {0};
    // region-direct-v2 and later: area-weighted error over source faces in this target.
    double delta_e00 {0};
    std::optional<size_t> physical_slot;
    std::optional<MixedColorRecipe> recipe;
    std::optional<PrintColorRecipeProof> recipe_proof;
    std::string candidate_id;
    ColorEvidence evidence {ColorEvidence::Unknown};
    bool executable {false};
    bool within_tolerance {false};
    std::string unresolved_reason;
};

struct PrintColorContrast {
    std::string first_region;
    std::string second_region;
    double weight {1};
    double minimum_output_delta_e {0};
    bool hard {false};
};

struct LocalPrintColorResult {
    std::string schema {kLocalPrintColorSchema};
    std::string algorithm_version;
    std::string source_sha256;
    std::string geometry_id;
    std::string parent_version;
    std::string material_fingerprint;
    std::string process_fingerprint;
    size_t requested_color_count {6};
    size_t source_color_count {0};
    size_t sampled_source_color_count {0}; // Original face samples, before user edits; not UV texture cardinality.
    size_t face_count {0};
    PrintColorMode mode {PrintColorMode::Direct};
    double color_tolerance {5};
    double important_area_floor {0.02};
    std::vector<PhysicalFilamentChannel> physical_channels;
    std::vector<PrintColorRegion> regions;
    std::vector<PrintColorContrast> contrasts;
    std::vector<PrintColorTarget> targets;
    // One target index per source face. Consumers apply this partition exactly,
    // never re-cluster the palette or classify interpolated shader pixels.
    std::vector<size_t> face_targets;
    std::vector<std::pair<size_t, PrintRgb>> user_overrides;
    std::vector<std::string> notices;
    bool confirmed {false};

    size_t unresolved_count() const {
        return std::count_if(targets.begin(), targets.end(), [](const auto& t) {
            return !t.executable || !t.within_tolerance;
        });
    }

    bool valid(std::string& error) const {
        auto fail = [&](const char* message) { error = message; return false; };
        error.clear();
        if (schema != kLocalPrintColorSchema || algorithm_version.empty() ||
            !is_lowercase_sha256(source_sha256) || geometry_id.empty() ||
            material_fingerprint.empty() || process_fingerprint.empty())
            return fail("Missing or unsupported color result identity.");
        if (requested_color_count < 1 || requested_color_count > kMaxLocalPrintTargetColors ||
            mode != print_color_mode(requested_color_count)) return fail("Invalid requested color budget or mode.");
        if (!std::isfinite(color_tolerance) || color_tolerance < 0 || !std::isfinite(important_area_floor) ||
            important_area_floor < 0 || important_area_floor > 1) return fail("Invalid color evaluation settings.");
        if (face_count == 0 || face_targets.size() != face_count || targets.empty() ||
            targets.size() > requested_color_count) return fail("Incomplete surface partition or color budget exceeded.");
        if (!physical_channels.empty() && !is_valid_physical_channel_set(physical_channels))
            return fail("Invalid physical channel snapshot.");
        auto available = [&](size_t slot) {
            return std::any_of(physical_channels.begin(), physical_channels.end(), [&](const auto& p) {
                return p.slot == slot && p.compatible;
            });
        };
        std::vector<bool> used(targets.size(), false);
        for (size_t target : face_targets) {
            if (target >= targets.size()) return fail("Surface partition references an absent target.");
            used[target] = true;
        }
        if (std::find(used.begin(), used.end(), false) != used.end()) return fail("A target has no surface region.");
        for (const auto& t : targets) {
            if (!valid_print_rgb(t.source) || !valid_print_rgb(t.output) || !std::isfinite(t.area) || t.area < 0 ||
                !std::isfinite(t.delta_e00) || t.delta_e00 < 0) return fail("Invalid target color or residual.");
            if (t.physical_slot && t.recipe) return fail("A target cannot be both physical and virtual.");
            if (t.recipe_proof && !t.recipe) return fail("Recipe evidence has no recipe.");
            if (t.executable && !t.physical_slot && !t.recipe) return fail("Executable target has no assignment.");
            if (t.physical_slot && !available(*t.physical_slot)) return fail("Target references an unavailable physical slot.");
            if (t.evidence != ColorEvidence::Measured && t.evidence != ColorEvidence::Interpolated &&
                t.evidence != ColorEvidence::Estimated && t.evidence != ColorEvidence::Unknown)
                return fail("Invalid color evidence level.");
            if (t.physical_slot) {
                const auto p = std::find_if(physical_channels.begin(), physical_channels.end(),
                    [&](const auto& channel) { return channel.slot == *t.physical_slot; });
                for (size_t c = 0; c < 3; ++c) {
                    const float expected = float(std::stoul(p->display_color.substr(1 + 2*c, 2), nullptr, 16)) / 255.f;
                    if (std::abs(t.output[c] - expected) > 1e-6f)
                        return fail("Direct output disagrees with its physical material color.");
                }
            }
            if (t.recipe) {
                if (mode != PrintColorMode::Layered || !is_valid_mixed_color_recipe(*t.recipe) ||
                    t.recipe->components.size() < 2 || t.candidate_id.empty()) return fail("Invalid layered candidate.");
                for (const auto& c : t.recipe->components)
                    if (!available(c.slot)) return fail("Recipe references an unavailable or virtual slot.");
                if (confirmed && !t.recipe_proof) return fail("A recipe without its constraint evidence cannot be confirmed.");
                if (t.recipe_proof && (t.recipe_proof->schema != "orcaslicer.local-recipe-proof.v1" ||
                    t.recipe_proof->process.material_fingerprint != material_fingerprint ||
                    t.recipe_proof->process.process_fingerprint != process_fingerprint ||
                    !is_lowercase_sha256(t.recipe_proof->checksum) ||
                    !std::isfinite(t.recipe_proof->uncertainty_delta_e) || t.recipe_proof->uncertainty_delta_e < 0))
                    return fail("Recipe evidence does not match the result identity.");
                if (t.recipe_proof && t.within_tolerance &&
                    t.delta_e00 + t.recipe_proof->uncertainty_delta_e > color_tolerance + 1e-9)
                    return fail("Recipe uncertainty exceeds the declared tolerance.");
            }
            if ((!t.executable || !t.within_tolerance) && t.unresolved_reason.empty())
                return fail("Unresolved target requires an explanation.");
            if (t.evidence == ColorEvidence::Unknown && t.within_tolerance)
                return fail("Unknown evidence cannot establish color tolerance.");
            if (t.within_tolerance && t.delta_e00 > color_tolerance)
                return fail("Color residual exceeds its declared tolerance.");
            if (confirmed && !t.executable) return fail("An invalid assignment cannot be confirmed.");
        }
        std::set<std::string> ids;
        for (const auto& r : regions) {
            if (r.id.empty() || !ids.insert(r.id).second || !std::isfinite(r.confidence) ||
                r.confidence < 0 || r.confidence > 1) return fail("Invalid region identity or confidence.");
            std::set<size_t> region_faces;
            for (size_t face : r.faces) {
                if (face >= face_count || !region_faces.insert(face).second) return fail("Region references an absent or duplicate face.");
                const auto& target = targets[face_targets[face]];
                if (r.locked_physical_slot && target.executable && target.physical_slot != r.locked_physical_slot)
                    return fail("Material assignment violates an explicit region lock.");
            }
            if (r.locked_physical_slot && !available(*r.locked_physical_slot))
                return fail("A user material lock is unavailable in the current snapshot.");
        }
        for (const auto& c : contrasts)
            if (!ids.count(c.first_region) || !ids.count(c.second_region) || !std::isfinite(c.weight) || c.weight < 0 ||
                !std::isfinite(c.minimum_output_delta_e) || c.minimum_output_delta_e < 0)
                return fail("Invalid region contrast constraint.");
        std::set<size_t> overrides;
        for (const auto& item : user_overrides)
            if (item.first >= face_count || !valid_print_rgb(item.second) || !overrides.insert(item.first).second)
                return fail("Invalid user color override.");
        return true;
    }
};

} // namespace Slic3r::AI
