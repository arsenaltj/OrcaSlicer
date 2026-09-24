#pragma once

#include "LocalPrintColorRecipes.hpp"

namespace Slic3r::GUI::LocalPrintColorLayeredSelection {
struct Outcome { bool cancelled {false}; };

// All target groups participate. A bounded centroid shortlist is followed by
// exact source-face costs and joint contrast-aware beam search. This is a
// reproducible heuristic, not a proof of global optimality over all recipes.
template<class Face>
Outcome select(AI::LocalPrintColorResult& result, const std::vector<Face>& faces,
    const std::vector<double>& semantic_weight, const std::vector<double>& weights,
    const std::vector<double>& distribution_weights, const std::vector<std::optional<size_t>>& locks,
    const std::map<std::string, size_t>& region_target, const LocalPrintColorRecipes::Catalog* catalog,
    const std::function<bool()>& cancelled)
{
    namespace Recipes = LocalPrintColorRecipes;
    auto stopped = [&] { return cancelled && cancelled(); };
    struct Option {
        AI::PrintRgb color;
        std::optional<size_t> slot;
        const Recipes::Candidate* recipe {nullptr};
        double mean {0}, worst {0}, cost {0};
        size_t complexity() const { return recipe ? recipe->recipe.components.size() : 1; }
    };
    std::vector<Option> direct;
    for (const auto& channel : result.physical_channels)
        if (channel.compatible) direct.push_back({Recipes::rgb(channel.display_color), channel.slot});
    bool matching_catalog = catalog && catalog->ok() &&
        catalog->material_fingerprint == result.material_fingerprint && catalog->process_fingerprint == result.process_fingerprint &&
        catalog->physical_channels.size() == result.physical_channels.size();
    if (matching_catalog) for (const auto& expected : result.physical_channels) {
        const auto found = std::find_if(catalog->physical_channels.begin(), catalog->physical_channels.end(),
            [&](const auto& actual) { return expected.slot == actual.slot && expected.display_color == actual.display_color &&
                expected.material_type == actual.material_type && expected.compatible == actual.compatible; });
        if (found == catalog->physical_channels.end()) { matching_catalog = false; break; }
    }
    if (catalog && catalog->ok() && !matching_catalog)
        result.notices.push_back("Layered selection ignored a stale material/process catalog.");
    std::vector<uint8_t> verified(catalog ? catalog->candidates.size() : 0,0);
    if(matching_catalog) for(size_t i=0;i<catalog->candidates.size();++i) {
        if(i%256==0 && stopped()) return {true};
        const auto& c=catalog->candidates[i];
        AI::PrintColorTarget checked;checked.recipe=c.recipe;checked.recipe_proof=c.proof;
        checked.output=c.color;checked.candidate_id=c.id;checked.evidence=c.evidence;
        std::string reason;
        verified[i]=LocalPrintRecipeProofState::valid(checked,result,reason) && c.proof &&
            c.sublayer_heights_mm==c.proof->sublayer_heights_mm && c.evidence_source==c.proof->evidence_source &&
            c.evidence_sha256==c.proof->evidence_sha256 && c.uncertainty_delta_e==c.proof->uncertainty_delta_e;
    }
    std::vector<std::vector<Option>> domains(result.targets.size());
    for (size_t t = 0; t < domains.size(); ++t) {
        if (stopped()) return {true};
        for (const auto& option : direct)
            if (!locks[t] || option.slot == locks[t]) domains[t].push_back(option);
        if (!locks[t] && matching_catalog) {
            std::vector<std::pair<double, size_t>> shortlist;
            for (size_t i = 0; i < catalog->candidates.size(); ++i) {
                if (i % 256 == 0 && stopped()) return {true};
                const auto& c = catalog->candidates[i];
                if(!verified[i]) continue;
                if (!AI::is_lowercase_sha256(c.id) || !AI::valid_print_rgb(c.color) ||
                    !AI::is_valid_mixed_color_recipe(c.recipe) || c.recipe.components.size() < 2 ||
                    c.recipe.existing_virtual_slot || c.sublayer_heights_mm.empty() ||
                    !std::isfinite(c.uncertainty_delta_e) || c.uncertainty_delta_e < 0 ||
                    c.evidence == AI::ColorEvidence::Unknown) continue;
                if (std::any_of(c.recipe.components.begin(), c.recipe.components.end(), [&](const auto& part) {
                    return std::none_of(direct.begin(), direct.end(), [&](const auto& slot) { return slot.slot == part.slot; });
                })) continue;
                shortlist.push_back({Recipes::delta_e(result.targets[t].source, c.color), i});
            }
            std::stable_sort(shortlist.begin(), shortlist.end(), [&](const auto& a, const auto& b) {
                if (a.first != b.first) return a.first < b.first;
                return catalog->candidates[a.second].id < catalog->candidates[b.second].id;
            });
            if (shortlist.size() > 16) shortlist.resize(16);
            for (const auto& ranked : shortlist) {
                const auto& c = catalog->candidates[ranked.second];
                domains[t].push_back({c.color, {}, &c});
            }
        }
        if (domains[t].empty()) {
            result.notices.push_back("Layered selection found a target without a compatible assignment.");
            return {};
        }
    }
    double total_area = 0;
    for (const auto& face : faces) total_area += face.area;
    struct Contrast { size_t a, b; double source, weight, minimum; bool hard; };
    std::vector<Contrast> contrasts;
    for (const auto& c : result.contrasts) {
        const size_t a = region_target.at(c.first_region), b = region_target.at(c.second_region);
        contrasts.push_back({a, b, Recipes::delta_e(result.targets[a].source, result.targets[b].source),
            c.weight, c.minimum_output_delta_e, c.hard});
    }
    // A small, bounded separation preference for supported skin/lip and
    // skin/hair pairs. Eye and inner-mouth labels contain multiple colors;
    // their mean is not evidence for a separate pupil or tooth output. Do not
    // use those means to force an unnatural skin color. The 10 DeltaE cap is
    // an experimental preference, not a perceptual or print acceptance limit.
    auto skin = [](const std::string& label) { return label == "face" || label == "nose" || label == "neck"; };
    auto detail = [](const std::string& label) {
        return label == "ulip" || label == "llip" || label == "hair";
    };
    auto reliable = [](const AI::PrintColorRegion& r) {
        return r.id.compare(0, 14, "auto-semantic:") == 0 && !r.subject_id.empty() &&
            r.confidence >= .5 && !r.faces.empty();
    };
    auto source_color = [&](const AI::PrintColorRegion& r) {
        double area = 0; std::array<double, 3> sum {};
        for (size_t f : r.faces) { area += faces[f].area; for (size_t c = 0; c < 3; ++c) sum[c] += faces[f].area * faces[f].color[c]; }
        return AI::PrintRgb {float(sum[0]/area), float(sum[1]/area), float(sum[2]/area)};
    };
    std::map<std::pair<size_t, size_t>, Contrast> automatic;
    for (const auto& a : result.regions) if (reliable(a) && skin(a.label))
        for (const auto& b : result.regions) if (reliable(b) && detail(b.label) && a.subject_id == b.subject_id) {
            if (stopped()) return {true};
            if (std::any_of(result.contrasts.begin(), result.contrasts.end(), [&](const auto& c) {
                return (c.first_region == a.id && c.second_region == b.id) || (c.first_region == b.id && c.second_region == a.id);
            })) continue;
            const double distance = Recipes::delta_e(source_color(a), source_color(b));
            if (distance <= result.color_tolerance) continue;
            const size_t first = region_target.at(a.id), second = region_target.at(b.id);
            if (first == second) continue; // This partition cannot separate them.
            const auto key = std::minmax(first, second);
            const Contrast candidate {first, second, std::min(10., distance),
                .5 * std::min(a.confidence, b.confidence), 0, false};
            const auto found = automatic.find(key);
            // Face/nose/neck and upper/lower lip may share target groups. One
            // target pair receives one preference, not a label-count multiplier.
            if (found == automatic.end()) automatic.emplace(key, candidate);
            else {
                found->second.source = std::max(found->second.source, candidate.source);
                found->second.weight = std::max(found->second.weight, candidate.weight);
            }
        }
    for (const auto& pair : automatic) contrasts.push_back(pair.second);
    for (size_t f = 0; f < faces.size(); ++f) {
        if (f % 4096 == 0 && stopped()) return {true};
        const size_t t = result.face_targets[f];
        for (auto& option : domains[t]) {
            const double error = Recipes::delta_e(faces[f].color, option.color);
            option.mean += faces[f].area / result.targets[t].area * error;
            option.worst = std::max(option.worst, error);
            option.cost += (faces[f].area / total_area + semantic_weight[f]) *
                (weights[t] / distribution_weights[t]) * error;
        }
    }
    std::vector<size_t> order(domains.size()); std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (bool(locks[a]) != bool(locks[b])) return bool(locks[a]);
        return weights[a] > weights[b];
    });
    struct State { std::vector<int> choices; double cost {0}; size_t complexity {0}; };
    auto better = [](const State& a, const State& b) {
        if (a.cost != b.cost) return a.cost < b.cost;
        if (a.complexity != b.complexity) return a.complexity < b.complexity;
        return a.choices < b.choices;
    };
    std::vector<State> beam {{std::vector<int>(domains.size(), -1)}};
    for (size_t t : order) {
        if (stopped()) return {true};
        std::vector<State> next;
        for (const auto& state : beam) for (size_t c = 0; c < domains[t].size(); ++c) {
            auto candidate = state; candidate.choices[t] = int(c);
            candidate.cost += domains[t][c].cost; candidate.complexity += domains[t][c].complexity();
            bool feasible = true;
            for (const auto& contrast : contrasts) {
                const size_t a = contrast.a, b = contrast.b;
                if ((a != t && b != t) || candidate.choices[a] < 0 || candidate.choices[b] < 0) continue;
                const double distance = Recipes::delta_e(domains[a][candidate.choices[a]].color, domains[b][candidate.choices[b]].color);
                if (contrast.hard && distance + 1e-9 < contrast.minimum) { feasible = false; break; }
                candidate.cost += contrast.weight * std::max(0., contrast.source - distance);
            }
            if (feasible) next.push_back(std::move(candidate));
        }
        if (next.empty()) {
            result.notices.push_back("Layered bounded search found no assignment satisfying all material locks and hard contrasts.");
            return {};
        }
        std::stable_sort(next.begin(), next.end(), better);
        if (next.size() > 128) next.resize(128);
        beam = std::move(next);
    }
    if (stopped()) return {true};
    const auto& best = beam.front();
    for (size_t t = 0; t < domains.size(); ++t) {
        auto& target = result.targets[t]; const auto& option = domains[t][best.choices[t]];
        target.output = option.color; target.physical_slot = option.slot;
        target.delta_e00 = option.mean; target.executable = true;
        target.evidence = option.recipe ? option.recipe->evidence : AI::ColorEvidence::Estimated;
        target.within_tolerance = option.worst + (option.recipe ? option.recipe->uncertainty_delta_e : 0) <= result.color_tolerance;
        target.unresolved_reason = target.within_tolerance ? "" : "The closest selected legal approximation exceeds the requested surface tolerance.";
        target.recipe.reset();target.recipe_proof.reset();target.candidate_id.clear();
        if (option.recipe) { target.recipe = option.recipe->recipe; target.candidate_id = option.recipe->id; target.recipe_proof=option.recipe->proof; }
    }
    result.notices.push_back("Layered selection evaluated every target; shared physical outputs are approximations, not additional loaded materials.");
    result.notices.push_back("Layered search uses all direct colors, up to 16 predicted recipe candidates per target and a 128-state joint beam; it does not prove a global optimum.");
    return {};
}
} // namespace Slic3r::GUI::LocalPrintColorLayeredSelection
