#pragma once

#include "ModelPreviewPalette.hpp"
#include "LocalPrintColorRecipes.hpp"
#include "LocalPrintColorLayeredSelection.hpp"
#include "slic3r/AI/Contracts/LocalPrintColorResult.hpp"
#include "libslic3r/TextureToColor/ColorUtils.hpp"
#include <functional>
#include <numeric>
#include <map>
#include <memory>

namespace Slic3r::GUI::LocalPrintColorMatching {
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
};
struct Computation {
    AI::LocalPrintColorResult result;
    std::string error;
    bool cancelled {false};
    bool ok() const { return error.empty() && !cancelled; }
};

inline double delta_e(const PrintRgb& a, const PrintRgb& b)
{
    return Slic3r::tex2color::color_utils::calc_rgb_color_difference_by_ciede2000_srgb01(
        {double(a[0]), double(a[1]), double(a[2])}, {double(b[0]), double(b[1]), double(b[2])});
}
inline PrintRgb hex_rgb(const std::string& value)
{
    PrintRgb result {};
    for (size_t c = 0; c < 3; ++c) result[c] = float(std::stoul(value.substr(1 + 2*c, 2), nullptr, 16)) / 255.f;
    return result;
}
inline uint32_t packed_rgb(const PrintRgb& color)
{
    uint32_t packed = 0;
    for (float c : color) packed = (packed << 8) | uint32_t(std::lround(c * 255.f));
    return packed;
}

// Deterministic area-weighted quantization followed by bounded whole-palette
// assignment. It consumes supplied region evidence; it never invents anatomy.
inline Computation compute(const Input& input)
{
    Computation computation;
    auto& result = computation.result;
    result = input.identity;
    result.algorithm_version = "region-direct-v5";
    result.mode = AI::print_color_mode(result.requested_color_count);
    result.color_tolerance = input.tolerance;
    result.important_area_floor = input.important_area_floor;
    if (!input.contrasts.empty()) result.contrasts = input.contrasts;
    result.confirmed = false;
    result.targets.clear(); result.face_targets.clear(); result.notices.clear();
    auto fail = [&](std::string message) { computation.error = std::move(message); return computation; };
    auto cancelled = [&] { return input.cancelled && input.cancelled(); };
    if (result.requested_color_count < 1 || result.requested_color_count > AI::kMaxLocalPrintTargetColors)
        return fail("Target color count must be between 1 and 32.");
    if (input.faces.empty() || input.faces.size() != result.face_count)
        return fail("Face samples do not match the source geometry.");
    if (!std::isfinite(input.tolerance) || input.tolerance < 0 || !std::isfinite(input.important_area_floor) ||
        input.important_area_floor < 0 || input.important_area_floor > 1) return fail("Invalid color evaluation settings.");
    if (!result.physical_channels.empty() && !AI::is_valid_physical_channel_set(result.physical_channels))
        return fail("Invalid physical material snapshot.");
    auto faces = input.faces;
    std::set<size_t> overridden;
    for (const auto& item : result.user_overrides) {
        if (item.first >= faces.size() || !AI::valid_print_rgb(item.second) || !overridden.insert(item.first).second)
            return fail("User color overrides no longer match this model.");
        faces[item.first].color = item.second;
    }
    double total_area = 0;
    std::set<uint32_t> source_colors;
    PreviewPalette::Histogram histogram;
    for (size_t i = 0; i < faces.size(); ++i) {
        if ((i % 4096) == 0 && cancelled()) { computation.cancelled = true; return computation; }
        const auto& face = faces[i];
        if (!AI::valid_print_rgb(face.color) || !std::isfinite(face.area) || face.area <= 0)
            return fail("Invalid surface color or degenerate face; repair geometry before matching.");
        total_area += face.area;
        if (!std::isfinite(total_area)) return fail("Surface area overflow.");
        if (!AI::valid_print_rgb(input.faces[i].color)) return fail("Invalid original surface color.");
        source_colors.insert(packed_rgb(input.faces[i].color));
    }
    if (total_area <= 0) return fail("Model has no positive surface area.");
    result.sampled_source_color_count = source_colors.size();

    std::vector<PrintRgb> locked;
    auto lock = [&](PrintRgb color) {
        const auto existing = std::find(locked.begin(), locked.end(), color);
        if (existing != locked.end()) return size_t(existing - locked.begin());
        locked.push_back(color); return locked.size() - 1;
    };
    // Explicit material locks participate before quantization. Equal RGB values
    // can still require separate targets when the user chooses different slots.
    std::map<size_t, size_t> face_slots;
    std::map<size_t, std::vector<size_t>> slot_faces;
    std::set<std::string> region_ids;
    for (const auto& region : result.regions) {
        if (region.id.empty() || !region_ids.insert(region.id).second || !std::isfinite(region.confidence) ||
            region.confidence < 0 || region.confidence > 1) return fail("Invalid region identity or confidence.");
        std::set<size_t> unique_faces;
        for (size_t f : region.faces) {
            if (f >= faces.size() || !unique_faces.insert(f).second) return fail("Invalid or duplicate region face.");
            if (!region.locked_physical_slot) continue;
            const size_t slot = *region.locked_physical_slot;
            if (std::none_of(result.physical_channels.begin(), result.physical_channels.end(),
                [&](const auto& channel) { return channel.slot == slot && channel.compatible; }))
                return fail("A user material lock is unavailable in the current snapshot.");
            const auto inserted = face_slots.emplace(f, slot);
            if (!inserted.second && inserted.first->second != slot) return fail("Overlapping regions have conflicting material locks.");
            if (inserted.second) slot_faces[slot].push_back(f);
        }
    }
    auto representative = [&](const std::vector<size_t>& group) {
        PreviewPalette::Histogram region_histogram;
        for (size_t f : group) region_histogram.add(packed_rgb(faces[f].color), faces[f].area);
        const auto palette = region_histogram.print_palette(1);
        const auto center = PreviewPalette::to_lab(palette.empty() ? faces[group.front()].color : palette.front());
        const size_t medoid = *std::min_element(group.begin(), group.end(), [&](size_t a, size_t b) {
            return PreviewPalette::distance(PreviewPalette::to_lab(faces[a].color), center) <
                   PreviewPalette::distance(PreviewPalette::to_lab(faces[b].color), center);
        });
        return faces[medoid].color;
    };
    std::map<size_t, size_t> slot_centers;
    std::set<size_t> occupied_centers;
    std::vector<std::pair<size_t, size_t>> duplicate_slot_centers;
    for (const auto& group : slot_faces) {
        const auto channel = std::find_if(result.physical_channels.begin(), result.physical_channels.end(),
            [&](const auto& item) { return item.slot == group.first; });
        const size_t center = lock(hex_rgb(channel->display_color));
        slot_centers[group.first] = center;
        if (!occupied_centers.insert(center).second) duplicate_slot_centers.emplace_back(group.first, center);
    }
    // AI protection is a finite preference over the observed color distribution,
    // not an instruction to reserve one exact target per texture variation.
    // Each region contributes at most confidence * important_area_floor of the
    // model area. Square-root bin mass keeps minority colors represented without
    // a face-area cutoff or collapsing a multicolor eye to one representative.
    // Overlap takes the maximum contribution; total bonus is capped at one model.
    std::vector<double> semantic_weight(faces.size(), 0);
    for (const auto& region : result.regions) {
        if (region.user_protected || !region.protect_color || region.confidence < .5 ||
            region.locked_physical_slot || region.faces.empty()) continue;
        auto bin = [&](size_t f) {
            const uint32_t rgb = packed_rgb(faces[f].color);
            return ((rgb >> 19) << 10) | (((rgb >> 11) & 31) << 5) | ((rgb >> 3) & 31);
        };
        std::map<uint32_t, double> mass;
        double region_area = 0;
        for (size_t f : region.faces) {
            if (face_slots.count(f) || overridden.count(f)) continue;
            mass[bin(f)] += faces[f].area; region_area += faces[f].area;
        }
        if (region_area <= 0) continue;
        double root_mass = 0;
        for (const auto& item : mass) root_mass += std::sqrt(item.second / region_area);
        for (size_t f : region.faces) {
            if (face_slots.count(f) || overridden.count(f)) continue;
            const double bin_area = mass.at(bin(f));
            const double bonus = input.important_area_floor * region.confidence *
                (std::sqrt(bin_area / region_area) / root_mass) * (faces[f].area / bin_area);
            semantic_weight[f] = std::max(semantic_weight[f], bonus);
        }
        if (cancelled()) { computation.cancelled = true; return computation; }
    }
    const double semantic_total = std::accumulate(semantic_weight.begin(), semantic_weight.end(), 0.0);
    if (semantic_total > 1) for (double& weight : semantic_weight) weight /= semantic_total;
    // A local slot override must not pull unrelated same-source-color faces
    // into that output. Its actual output is a fixed classification center;
    // only unlocked surfaces contribute to the remaining histogram.
    for (size_t f = 0; f < faces.size(); ++f) {
        if (face_slots.count(f)) continue;
        const double weighted_area = faces[f].area + semantic_weight[f] * total_area;
        if (!std::isfinite(weighted_area)) return fail("Surface importance overflow.");
        histogram.add(packed_rgb(faces[f].color), weighted_area);
    }
    // Explicit recolors reserve exact target colors. Material locks describe the
    // chosen output instead; their source recolors remain in user_overrides.
    std::map<size_t, size_t> fixed_faces;
    for (const auto& item : result.user_overrides)
        if (!face_slots.count(item.first)) fixed_faces[item.first] = lock(item.second);
    for (const auto& region : result.regions) {
        const bool protected_color = region.user_protected;
        if (!protected_color || region.faces.empty() || region.locked_physical_slot) continue;
        std::vector<size_t> eligible;
        for (size_t f : region.faces) if (!face_slots.count(f) && faces[f].area > 0) eligible.push_back(f);
        if (eligible.empty()) continue;
        lock(representative(eligible));
        // A region can contain more than one meaningful color (eye white and
        // pupil, or a small shirt emblem). Protect its supported color range,
        // not just its majority representative. No area cutoff is applied.
        // Explicit protections that cannot fit n produce a budget conflict.
        std::map<uint32_t, PrintRgb> candidates;
        for (size_t f : eligible) candidates.emplace(packed_rgb(faces[f].color), faces[f].color);
        std::vector<std::pair<PrintRgb, double>> distances;
        for (const auto& candidate : candidates) {
            double nearest = std::numeric_limits<double>::infinity();
            for (const auto& center : locked) nearest = std::min(nearest, delta_e(candidate.second, center));
            distances.push_back({candidate.second, nearest});
        }
        while (!distances.empty()) {
            if (cancelled()) { computation.cancelled = true; return computation; }
            const auto farthest = std::max_element(distances.begin(), distances.end(),
                [](const auto& a, const auto& b) { return a.second < b.second; });
            if (farthest->second <= input.tolerance + 1e-6) break;
            const auto color = farthest->first;
            lock(color);
            if (locked.size() + duplicate_slot_centers.size() > result.requested_color_count)
                return fail("Protected colors exceed the requested budget; adjust protections or the requested count.");
            for (auto& candidate : distances) candidate.second = std::min(candidate.second, delta_e(candidate.first, color));
        }
    }
    if (locked.size() + duplicate_slot_centers.size() > result.requested_color_count)
        return fail("Protected colors exceed the requested budget; adjust protections or the requested count.");
    auto palette = histogram.print_palette(result.requested_color_count - duplicate_slot_centers.size(), locked);
    if (palette.empty()) return fail("Unable to form a target palette.");
    for (const auto& duplicate : duplicate_slot_centers) {
        slot_centers[duplicate.first] = palette.size();
        const auto color = palette[duplicate.second];
        palette.push_back(color);
    }
    for (const auto& item : face_slots) fixed_faces[item.first] = slot_centers.at(item.second);
    std::vector<PrintRgb> labs;
    for (auto color : palette) labs.push_back(PreviewPalette::to_lab(color));
    result.face_targets.reserve(faces.size());
    std::vector<double> area(palette.size(), 0);
    std::vector<bool> used(palette.size(), false);
    for (size_t face = 0; face < faces.size(); ++face) {
        const auto fixed = fixed_faces.find(face);
        size_t target = fixed != fixed_faces.end() ? fixed->second :
            PreviewPalette::nearest_lab_index(PreviewPalette::to_lab(faces[face].color), labs);
        result.face_targets.push_back(target); area[target] += faces[face].area; used[target] = true;
    }
    // Empty clustering centers do not create fictitious colors just to reach n.
    std::vector<size_t> remap(palette.size(), size_t(-1));
    for (size_t t = 0; t < palette.size(); ++t) {
        if (!used[t]) continue;
        remap[t] = result.targets.size();
        AI::PrintColorTarget target;
        target.source = target.output = palette[t]; target.area = area[t];
        target.unresolved_reason = "No compatible physical assignment.";
        result.targets.push_back(target);
    }
    for (auto& t : result.face_targets) t = remap[t];
    // A hard contrast can require two separately assignable targets even when
    // a material lock places both regions at the same quantization center.
    // Split only exclusive faces of that shared group, within the user's n;
    // never duplicate geometry or reinterpret an overlapping face twice.
    for (const auto& constraint : result.contrasts) {
        if (result.mode != AI::PrintColorMode::Layered || !constraint.hard || constraint.minimum_output_delta_e <= 0 ||
            result.targets.size() >= result.requested_color_count) continue;
        auto region = [&](const std::string& id) -> const AI::PrintColorRegion* {
            const auto found = std::find_if(result.regions.begin(), result.regions.end(), [&](const auto& r) { return r.id == id; });
            return found == result.regions.end() ? nullptr : &*found;
        };
        const auto* a = region(constraint.first_region); const auto* b = region(constraint.second_region);
        if (!a || !b || a == b || a->faces.empty() || b->faces.empty()) continue;
        auto dominant = [&](const AI::PrintColorRegion& r) {
            std::vector<double> mass(result.targets.size(), 0);
            for (size_t f : r.faces) mass[result.face_targets[f]] += faces[f].area;
            return size_t(std::max_element(mass.begin(), mass.end()) - mass.begin());
        };
        const size_t shared = dominant(*a);
        if (shared != dominant(*b)) continue;
        const std::set<size_t> first_faces(a->faces.begin(), a->faces.end());
        std::vector<size_t> moved;
        double moved_area = 0;
        for (size_t f : b->faces) if (result.face_targets[f] == shared && !first_faces.count(f)) {
            moved.push_back(f); moved_area += faces[f].area;
        }
        if (moved.empty() || moved_area >= result.targets[shared].area) continue;
        auto target = result.targets[shared]; target.area = moved_area;
        result.targets[shared].area -= moved_area;
        for (size_t f : moved) result.face_targets[f] = result.targets.size();
        result.targets.push_back(std::move(target));
        if (cancelled()) { computation.cancelled = true; return computation; }
    }
    std::vector<std::optional<size_t>> target_locks(result.targets.size());
    for (const auto& item : face_slots) target_locks[result.face_targets[item.first]] = item.second;
    std::vector<std::vector<size_t>> locked_target_faces(result.targets.size());
    for (size_t f = 0; f < faces.size(); ++f)
        if (target_locks[result.face_targets[f]]) locked_target_faces[result.face_targets[f]].push_back(f);
    for (size_t t = 0; t < result.targets.size(); ++t)
        if (!locked_target_faces[t].empty())
            result.targets[t].output = result.targets[t].source = representative(locked_target_faces[t]);
    std::vector<double> weights(result.targets.size(), 0);
    for (size_t t = 0; t < weights.size(); ++t) weights[t] = result.targets[t].area / total_area;
    for (size_t f = 0; f < faces.size(); ++f) weights[result.face_targets[f]] += semantic_weight[f];
    const auto distribution_weights = weights;
    std::map<std::string, size_t> region_target;
    for (const auto& region : result.regions) {
        if (region.faces.empty()) continue;
        std::vector<double> contributions(result.targets.size(), 0);
        for (size_t f : region.faces) contributions[result.face_targets[f]] += faces[f].area;
        const auto t = size_t(std::max_element(contributions.begin(), contributions.end()) - contributions.begin());
        region_target[region.id] = t;
        if (region.user_protected)
            for (size_t part = 0; part < contributions.size(); ++part)
                if (contributions[part] > 0) weights[part] = std::max(weights[part], input.important_area_floor);
    }
    for (const auto& c : result.contrasts)
        if (!region_target.count(c.first_region) || !region_target.count(c.second_region) ||
            !std::isfinite(c.weight) || c.weight < 0 || !std::isfinite(c.minimum_output_delta_e) || c.minimum_output_delta_e < 0)
            return fail("Invalid region contrast constraint.");

    std::vector<size_t> physical;
    std::vector<PrintRgb> material_colors;
    for (size_t p = 0; p < result.physical_channels.size(); ++p) {
        if (!result.physical_channels[p].compatible) continue;
        physical.push_back(p); material_colors.push_back(hex_rgb(result.physical_channels[p].display_color));
    }
    if (result.mode == AI::PrintColorMode::Layered) {
        result.notices.push_back("Layered mode requires process-validated candidates; unassigned targets remain unresolved.");
        if (input.recipe_catalog) {
            if (input.recipe_catalog->cancelled) { computation.cancelled = true; return computation; }
            if (!input.recipe_catalog->ok())
                result.notices.push_back("Workspace recipe evaluation: " + input.recipe_catalog->error);
            else result.notices.push_back("Workspace recipe candidates: " + std::to_string(input.recipe_catalog->candidates.size()));
        }
    }
    const size_t k = result.targets.size(), p = physical.size();
    // Distinct direct groups require distinct physical assignments. Missing
    // channels remain unresolved; do not silently fold several groups to one.
    if (result.mode == AI::PrintColorMode::Layered) {
        const auto selection = LocalPrintColorLayeredSelection::select(result, faces, semantic_weight, weights,
            distribution_weights, target_locks, region_target, input.recipe_catalog.get(), input.cancelled);
        if (selection.cancelled) { computation.cancelled = true; return computation; }
        result.algorithm_version = "region-layered-v2";
    } else if (k <= 6 && p > 0) {
        // CIEDE2000 is nonlinear: the distance to an Oklab centroid can rank
        // spools differently from the error on the actual surface. Accumulate
        // each target/slot cost once, then reuse it for all <= 720 assignments.
        std::vector<std::vector<double>> surface_cost(k, std::vector<double>(p, 0));
        std::vector<std::vector<double>> preference_cost(k, std::vector<double>(p, 0));
        for (size_t f = 0; f < faces.size(); ++f) {
            if (f % 4096 == 0 && cancelled()) { computation.cancelled = true; return computation; }
            const size_t t = result.face_targets[f];
            for (size_t s = 0; s < p; ++s) {
                const double error = delta_e(faces[f].color, material_colors[s]);
                surface_cost[t][s] += (faces[f].area / result.targets[t].area) * error;
                preference_cost[t][s] += (faces[f].area / total_area + semantic_weight[f]) * error;
            }
        }
        // Dummy entries represent unresolved groups when the printer has fewer
        // channels. Still at most 6! permutations; no fictitious material slots.
        std::vector<size_t> order(std::max(k, p)); std::iota(order.begin(), order.end(), 0);
        std::vector<size_t> best;
        double best_cost = std::numeric_limits<double>::infinity();
        double best_coverage = -1;
        do {
            if (cancelled()) { computation.cancelled = true; return computation; }
            double cost = 0, coverage = 0; bool feasible = true;
            for (size_t t = 0; t < k; ++t) {
                if (target_locks[t] && (order[t] >= p || result.physical_channels[physical[order[t]]].slot != *target_locks[t])) {
                    feasible = false; break;
                }
                if (order[t] >= p) continue;
                coverage += weights[t];
                cost += semantic_total > 0 ?
                    (weights[t] / distribution_weights[t]) * preference_cost[t][order[t]] :
                    weights[t] * surface_cost[t][order[t]];
            }
            for (const auto& constraint : result.contrasts) {
                const auto a = region_target.at(constraint.first_region), b = region_target.at(constraint.second_region);
                if (order[a] >= p || order[b] >= p) continue;
                const double contrast = delta_e(material_colors[order[a]], material_colors[order[b]]);
                if (constraint.hard && contrast < constraint.minimum_output_delta_e) { feasible = false; break; }
                const double original = delta_e(result.targets[a].source, result.targets[b].source);
                cost += constraint.weight * std::max(0.0, original - contrast);
            }
            if (feasible && (coverage > best_coverage + 1e-12 ||
                (std::abs(coverage - best_coverage) <= 1e-12 && cost < best_cost))) {
                best_coverage = coverage; best_cost = cost; best = order;
            }
        } while (std::next_permutation(order.begin(), order.end()));
        if (!best.empty()) for (size_t t = 0; t < k; ++t) {
            if (best[t] >= p) continue;
            auto& target = result.targets[t];
            target.physical_slot = result.physical_channels[physical[best[t]]].slot;
            target.output = material_colors[best[t]];
            target.delta_e00 = surface_cost[t][best[t]];
            target.evidence = AI::ColorEvidence::Estimated;
            target.executable = true; target.within_tolerance = target.delta_e00 <= input.tolerance;
            target.unresolved_reason = target.within_tolerance ? "" : "Display-color estimate exceeds the requested tolerance.";
        }
        else result.notices.push_back("No assignment satisfies the protected contrast constraints.");
        if (p < k) result.notices.push_back("Only part of the target palette fits the current compatible physical channels.");
    } else {
        result.notices.push_back("The current compatible physical channels cannot directly cover all target groups.");
    }
    // One bounded comparison with ordinary matching prevents accepting a soft
    // candidate dominated on actual surface quality. The baseline disables only
    // automatic flags, so this call cannot recurse and all user constraints stay.
    if (semantic_total > 0) {
        auto baseline_input = input;
        for (auto& region : baseline_input.identity.regions)
            if (!region.user_protected) region.protect_color = false;
        auto baseline = compute(baseline_input);
        if (baseline.cancelled) { computation.cancelled = true; return computation; }
        if (baseline.ok()) {
            struct ActualQuality { double covered = 0, error = 0, worst = 0; };
            auto quality = [&](const AI::LocalPrintColorResult& candidate, const std::vector<size_t>* group) {
                ActualQuality q;
                double area = 0;
                const size_t count = group ? group->size() : faces.size();
                for (size_t i = 0; i < count; ++i) {
                    if (i % 4096 == 0 && cancelled()) break;
                    const size_t f = group ? (*group)[i] : i;
                    const double fraction = faces[f].area / total_area;
                    area += fraction;
                    const auto& target = candidate.targets[candidate.face_targets[f]];
                    if (!target.executable) continue;
                    const double error = delta_e(faces[f].color, target.output);
                    q.covered += fraction; q.error += fraction * error; q.worst = std::max(q.worst, error);
                }
                if (q.covered > 0) q.error /= q.covered;
                if (area > 0) q.covered /= area;
                return q;
            };
            auto no_worse = [](const ActualQuality& a, const ActualQuality& b) {
                if (a.covered + 1e-12 < b.covered) return false;
                if (b.covered == 0) return true; // No covered error to compare.
                return a.covered > 0 && a.error <= b.error + 1e-9 && a.worst <= b.worst + 1e-9;
            };
            bool keep_baseline = no_worse(quality(baseline.result, nullptr), quality(result, nullptr));
            for (const auto& region : result.regions) {
                if (!keep_baseline) break;
                if (region.user_protected || !region.protect_color || region.confidence < .5 || region.faces.empty()) continue;
                keep_baseline = no_worse(quality(baseline.result, &region.faces), quality(result, &region.faces));
            }
            if (cancelled()) { computation.cancelled = true; return computation; }
            if (keep_baseline) {
                result = std::move(baseline.result);
                result.regions = input.identity.regions;
                result.notices.push_back("The AI preference candidate gave no further improvement; retained ordinary matching with no worse overall or protected-region coverage, mean and worst color error.");
            }
        }
    }
    if (result.regions.empty()) result.notices.push_back("No semantic regions supplied; this is local statistical matching only.");
    for (const auto& region : result.regions) {
        if (region.user_protected || !region.protect_color || region.confidence < .5 || region.faces.empty()) continue;
        double covered = 0, uncovered = 0, error_sum = 0, worst = 0;
        for (size_t f : region.faces) {
            const auto& target = result.targets[result.face_targets[f]];
            if (!target.executable) { uncovered += faces[f].area; continue; }
            const double error = delta_e(faces[f].color, target.output);
            covered += faces[f].area; error_sum += faces[f].area * error; worst = std::max(worst, error);
        }
        result.notices.push_back("AI color preference [" + region.id + "] is approximate: " +
            (covered > 0 ? "covered mean DeltaE00=" + std::to_string(error_sum / covered) +
                ", worst DeltaE00=" + std::to_string(worst) : "no covered surface") +
            "; unassigned area fraction=" + std::to_string(uncovered / (covered + uncovered)) +
            ((uncovered > 0 || worst > input.tolerance) ? "; requested color tolerance was not preserved." : "; estimated colors meet the requested tolerance."));
    }
    if (!result.valid(computation.error)) return computation;
    return computation;
}

} // namespace Slic3r::GUI::LocalPrintColorMatching
