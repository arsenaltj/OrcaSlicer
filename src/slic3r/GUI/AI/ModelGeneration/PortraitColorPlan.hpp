#pragma once

#include "slic3r/GUI/AI/Model/BeautyLeafDomain.hpp"
#include <array>
#include <limits>
#include <numeric>
#include <optional>

namespace Slic3r::GUI {

// A color decision has its own identity; it cannot redefine a shape boundary.
struct PortraitColorPlan {
    using RGB = std::array<float, 3>;
    struct Slot { std::string uid; RGB rgb; };
    struct Component {
        std::string id, region, label;
        std::vector<AI::BeautyLeafKey> leaves;
        RGB source {};
        double area {0.};
        size_t original_slot {0};
        std::vector<size_t> neighbors;
        // Keys must identify independent source camera families, not crops.
        std::map<std::string, size_t> visible_pixels;
        bool conflict {false};
        std::vector<std::string> risks;
        // R6 records actual inheritance separately from the optimizer's initial slot.
        nlohmann::json color_sources;
    };
    struct Decision {
        size_t slot {0};
        double cost_before {0.}, cost_after {0.};
        std::vector<std::string> reasons;
    };
    static constexpr std::array<double, 5> weights {1., 2., 3., 4., 3.};
    std::string geometry_id, source_sha256, boundary_sha256;
    size_t face_count {0};
    std::vector<Slot> palette;
    std::vector<Component> components;
    std::vector<Decision> decisions;
    nlohmann::json ownership_reference;
    std::optional<AI::BeautyLeafDomain> repair_domain;

    static RGB oklab(RGB rgb) {
        for (auto& c : rgb) c = c <= .04045f ? c/12.92f : std::pow((c+.055f)/1.055f, 2.4f);
        const float l = std::cbrt(.4122214708f*rgb[0]+.5363325363f*rgb[1]+.0514459929f*rgb[2]);
        const float m = std::cbrt(.2119034982f*rgb[0]+.6806995451f*rgb[1]+.1073969566f*rgb[2]);
        const float s = std::cbrt(.0883024619f*rgb[0]+.2817188376f*rgb[1]+.6299787005f*rgb[2]);
        return {.2104542553f*l+.793617785f*m-.0040720468f*s,
            1.9779984951f*l-2.428592205f*m+.4505937099f*s,
            .0259040371f*l+.7827717662f*m-.808675766f*s};
    }
    static double distance(const RGB& a, const RGB& b) {
        double result = 0.;
        for (size_t i = 0; i < 3; ++i) result += std::pow(double(a[i])-b[i], 2);
        return std::sqrt(result);
    }
    static bool feature(const std::string& label) {
        return label == "lb" || label == "rb" || label == "le" || label == "re" || label == "iris" ||
            label == "ulip" || label == "llip" || label == "teeth" || label == "lip-line-corner";
    }
    size_t slot(const std::string& uid) const {
        const auto found = std::find_if(palette.begin(), palette.end(), [&](const auto& s) { return s.uid == uid; });
        return size_t(found-palette.begin());
    }
    size_t preferred(const Component& c) const {
        if (c.label == "lb" || c.label == "rb" || c.label == "iris") return slot("portrait-dark");
        if (c.label == "le" || c.label == "re" || c.label == "teeth") return slot("portrait-light");
        if (c.label == "ulip" || c.label == "llip" || c.label == "lip-line-corner")
            return palette.size() == 3 ? slot("portrait-dark") : slot("portrait-lips");
        if (c.label == "face" || c.label == "neck" || c.label == "skin") return slot("portrait-skin");
        return palette.size();
    }
    bool extra_supported(const Component& c, size_t target) const {
        if (target >= palette.size()) return false;
        if (palette[target].uid != "portrait-cool" && palette[target].uid != "portrait-mid") return true;
        size_t views = 0;
        for (const auto& view : c.visible_pixels) if (view.second >= 32) ++views;
        if (views < 2) return false;
        const auto proposed = oklab(palette[target].rgb), source = oklab(c.source);
        size_t reference = palette.size();
        double best = std::numeric_limits<double>::max();
        for (size_t i = 0; i < palette.size(); ++i) {
            if (palette[i].uid != "portrait-dark" && palette[i].uid != "portrait-light") continue;
            const auto error = distance(source, oklab(palette[i].rgb));
            if (error < best) { best = error; reference = i; }
        }
        return reference < palette.size() && distance(proposed, oklab(palette[reference].rgb)) >= .03 &&
            best-distance(source, proposed) >= .03;
    }
    std::vector<size_t> allowed(const Component& c) const {
        if (c.conflict || c.label == "imouth") return {c.original_slot};
        const auto anchor = preferred(c);
        if (anchor < palette.size()) return {anchor};
        // The R5 pass operates only on confirmed region components.
        if (c.label != "hair" && c.label != "cloth" && c.label != "base" && c.label != "accessories")
            return {c.original_slot};
        std::vector<size_t> result;
        for (size_t target = 0; target < palette.size(); ++target) {
            const auto& uid = palette[target].uid;
            if (uid == "portrait-skin" || uid == "portrait-lips") continue;
            if (extra_supported(c, target)) result.push_back(target);
        }
        return result.empty() ? std::vector<size_t>{c.original_slot} : result;
    }
    void validate() const {
        const auto hash = [](const std::string& v) {
            return v.size() == 64 && std::all_of(v.begin(), v.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
        };
        if (!hash(geometry_id) || !hash(source_sha256) || !hash(boundary_sha256) || !face_count ||
            palette.size() < 3 || palette.size() > 6) throw std::invalid_argument("Invalid portrait color identity.");
        if (repair_domain) {
            repair_domain->validate();
            if (repair_domain->canonical_geometry_id != geometry_id || repair_domain->source_face_count != face_count ||
                ownership_reference.value("schema",std::string()) != "orca.portrait-surface-ownership-reference/v1")
                throw std::invalid_argument("Parent repair mapping changed its source.");
        }
        std::set<std::string> uids, ids;
        for (const auto& s : palette) {
            if (s.uid.empty() || !uids.insert(s.uid).second) throw std::invalid_argument("Duplicate color slot UID.");
            for (const auto v : s.rgb) if (!std::isfinite(v) || v < 0 || v > 1) throw std::invalid_argument("Invalid color slot.");
        }
        for (const auto& uid : {"portrait-skin", "portrait-dark", "portrait-light"})
            if (!uids.count(uid)) throw std::invalid_argument("Portrait base roles are missing.");
        if (palette.size() > 3 && !uids.count("portrait-lips")) throw std::invalid_argument("Portrait lip role is missing.");
        const std::vector<std::string> roles {"portrait-skin","portrait-dark","portrait-light","portrait-lips","portrait-cool","portrait-mid"};
        const std::set<std::string> expected(roles.begin(),roles.begin()+palette.size());
        if (uids != expected) throw std::invalid_argument("Portrait role UIDs changed.");
        std::vector<AI::BeautyLeafKey> claimed;
        for (size_t i = 0; i < components.size(); ++i) {
            const auto& c = components[i];
            if (c.id.empty() || c.region.empty() || !ids.insert(c.id).second || c.leaves.empty() ||
                c.original_slot >= palette.size() || !std::isfinite(c.area) || c.area <= 0)
                throw std::invalid_argument("Invalid portrait component.");
            for (const auto value : c.source) if (!std::isfinite(value) || value < 0 || value > 1)
                throw std::invalid_argument("Invalid verified component color.");
            AI::BeautyLeafDomain::validate_keys(c.leaves, face_count);
            claimed.insert(claimed.end(), c.leaves.begin(), c.leaves.end());
            if (!std::is_sorted(c.neighbors.begin(), c.neighbors.end()) ||
                std::adjacent_find(c.neighbors.begin(), c.neighbors.end()) != c.neighbors.end())
                throw std::invalid_argument("Component neighbors must be unique and ordered.");
            for (const auto neighbor : c.neighbors)
                if (neighbor >= components.size() || neighbor == i ||
                    !std::binary_search(components[neighbor].neighbors.begin(), components[neighbor].neighbors.end(), i))
                    throw std::invalid_argument("Component adjacency is invalid.");
            for (const auto& view : c.visible_pixels) if (view.first.empty()) throw std::invalid_argument("Missing camera identity.");
        }
        std::sort(claimed.begin(), claimed.end());
        AI::BeautyLeafDomain::validate_keys(claimed, face_count);
    }
    std::array<double, 5> terms(const std::vector<size_t>& choices) const {
        if (choices.size() != components.size()) throw std::invalid_argument("Invalid color choices.");
        std::map<std::string, double> region_area, error;
        std::array<double, 5> result {};
        double edges = 0., fragments = 0., features = 0., contrasts = 0.;
        for (size_t i = 0; i < components.size(); ++i) {
            const auto& c = components[i];
            if (choices[i] >= palette.size()) throw std::invalid_argument("Color choice leaves the palette.");
            region_area[c.region] += c.area;
            error[c.region] += c.area*std::min(1., distance(oklab(c.source),oklab(palette[choices[i]].rgb))/1.4);
            if (feature(c.label)) {
                ++features;
                result[3] += choices[i] == preferred(c) ? 0. : 1.;
            } else {
                ++fragments;
                const bool isolated = c.area < .5 && std::none_of(c.neighbors.begin(), c.neighbors.end(),
                    [&](size_t n) { return choices[n] == choices[i]; });
                result[2] += isolated ? 1. : 0.;
            }
            for (size_t n : c.neighbors) if (n > i) {
                if (components[n].region == c.region) {
                    ++edges;
                    result[1] += choices[n] == choices[i] ? 0. : 1.;
                } else if (feature(c.label) || feature(components[n].label)) {
                    ++contrasts;
                    const auto a = oklab(palette[choices[i]].rgb), b = oklab(palette[choices[n]].rgb);
                    result[4] += std::clamp((.08-distance(a,b))/.08, 0., 1.);
                }
            }
        }
        for (const auto& r : error) result[0] += r.second/region_area.at(r.first);
        if (!error.empty()) result[0] /= error.size();
        if (edges) result[1] /= edges;
        if (fragments) result[2] /= fragments;
        if (features) result[3] /= features;
        if (contrasts) result[4] /= contrasts;
        return result;
    }
    double cost(const std::vector<size_t>& choices) const {
        const auto values = terms(choices);
        return std::inner_product(values.begin(), values.end(), weights.begin(), 0.);
    }
    struct Normalization {
        std::map<std::string, double> region_area;
        double edges {0.}, fragments {0.}, features {0.}, contrasts {0.};
    };
    Normalization normalization() const {
        Normalization result;
        for (size_t i = 0; i < components.size(); ++i) {
            const auto& c = components[i];
            result.region_area[c.region] += c.area;
            if (feature(c.label)) ++result.features; else ++result.fragments;
            for (const auto n : c.neighbors) if (n > i) {
                if (components[n].region == c.region) ++result.edges;
                else if (feature(c.label) || feature(components[n].label)) ++result.contrasts;
            }
        }
        return result;
    }
    double delta(std::vector<size_t>& choices, size_t i, size_t target, const Normalization& norm) const {
        const auto& c = components[i];
        const auto old = choices[i];
        if (old == target) return 0.;
        const auto source = oklab(c.source);
        double result = weights[0]*c.area/norm.region_area.at(c.region)/norm.region_area.size()*
            (std::min(1.,distance(source,oklab(palette[target].rgb))/1.4)-
             std::min(1.,distance(source,oklab(palette[old].rgb))/1.4));
        if (feature(c.label) && norm.features)
            result += weights[3]*((target == preferred(c) ? 0. : 1.)-(old == preferred(c) ? 0. : 1.))/norm.features;
        for (const auto n : c.neighbors) {
            if (components[n].region == c.region && norm.edges)
                result += weights[1]*((choices[n] == target ? 0. : 1.)-(choices[n] == old ? 0. : 1.))/norm.edges;
            else if ((feature(c.label) || feature(components[n].label)) && norm.contrasts) {
                const auto neighbor = oklab(palette[choices[n]].rgb);
                result += weights[4]/norm.contrasts*(
                    std::clamp((.08-distance(oklab(palette[target].rgb),neighbor))/.08,0.,1.)-
                    std::clamp((.08-distance(oklab(palette[old].rgb),neighbor))/.08,0.,1.));
            }
        }
        const auto isolated = [&](size_t index) {
            const auto& item = components[index];
            return !feature(item.label) && item.area < .5 && std::none_of(item.neighbors.begin(),item.neighbors.end(),
                [&](size_t n) { return choices[n] == choices[index]; }) ? 1. : 0.;
        };
        double before = isolated(i);
        for (const auto n : c.neighbors) before += isolated(n);
        choices[i] = target;
        double after = isolated(i);
        for (const auto n : c.neighbors) after += isolated(n);
        choices[i] = old;
        if (norm.fragments) result += weights[2]*(after-before)/norm.fragments;
        return result;
    }
    void optimize() {
        validate();
        std::vector<size_t> choices;
        for (const auto& c : components) choices.push_back(c.original_slot);
        decisions.assign(components.size(), {});
        for (size_t i = 0; i < components.size(); ++i) {
            const auto legal = allowed(components[i]);
            if (std::find(legal.begin(), legal.end(), choices[i]) == legal.end()) choices[i] = legal.front();
        }
        const auto norm = normalization();
        std::vector<std::vector<size_t>> groups;
        std::vector<uint8_t> visited(components.size(),0);
        for (size_t first = 0; first < components.size(); ++first) {
            if (visited[first]) continue;
            std::vector<size_t> group {first}; visited[first] = 1;
            for (size_t next = 0; next < group.size(); ++next) for (const auto n : components[group[next]].neighbors)
                if (!visited[n] && components[n].region == components[first].region) {
                    visited[n] = 1; group.push_back(n);
                }
            groups.push_back(std::move(group));
        }
        // Fixed deterministic coordinate descent; no palette-specific tuning.
        for (unsigned sweep = 0; sweep < 8; ++sweep) {
            bool changed = false;
            // Joint moves prevent a forced tiny-fragment fallback from dragging
            // its much larger neighbor into the wrong material.
            for (const auto& group : groups) {
                std::vector<size_t> original;
                for (const auto i : group) original.push_back(choices[i]);
                size_t selected = palette.size(); double best = 0.;
                for (size_t candidate = 0; candidate < palette.size(); ++candidate) {
                    bool legal = true; double proposed = 0.;
                    for (const auto i : group) {
                        const auto options = allowed(components[i]);
                        if (std::find(options.begin(),options.end(),candidate) == options.end()) { legal = false; break; }
                        proposed += delta(choices,i,candidate,norm); choices[i] = candidate;
                    }
                    for (size_t j = 0; j < group.size(); ++j) choices[group[j]] = original[j];
                    if (legal && proposed < best-1e-12) { best = proposed; selected = candidate; }
                }
                if (selected < palette.size()) for (const auto i : group) {
                    changed |= choices[i] != selected; choices[i] = selected;
                }
            }
            for (size_t i = 0; i < components.size(); ++i) {
                auto selected = choices[i];
                double best = 0.;
                for (const auto candidate : allowed(components[i])) {
                    const double proposed = delta(choices,i,candidate,norm);
                    if (proposed < best-1e-12) { best = proposed; selected = candidate; }
                }
                changed |= selected != choices[i];
                choices[i] = selected;
            }
            if (!changed) break;
        }
        const auto final_cost = cost(choices);
        for (size_t i = 0; i < components.size(); ++i) {
            auto& d = decisions[i];
            const auto& c = components[i];
            d.slot = choices[i];
            d.cost_before = final_cost+delta(choices,i,c.original_slot,norm);
            d.cost_after = final_cost;
            d.reasons = c.conflict ? std::vector<std::string>{"EXPLICIT_CONFLICT_PRESERVED"} :
                c.label == "imouth" ? std::vector<std::string>{"ORAL_AUTOMATIC_COLOR_DISABLED"} :
                preferred(c) < palette.size() ? std::vector<std::string>{"SEMANTIC_VISUAL_ROLE"} :
                std::vector<std::string>{"NORMALIZED_GLOBAL_OBJECTIVE"};
            if (palette.size() == 3 && (c.label == "ulip" || c.label == "llip"))
                d.reasons.push_back("THREE_COLOR_CONTINUOUS_DARK_LIPS");
            d.reasons.insert(d.reasons.end(), c.risks.begin(), c.risks.end());
        }
    }
    void validate_solution() const {
        validate();
        if (decisions.size() != components.size()) throw std::invalid_argument("Portrait color plan is not solved.");
        for (size_t i = 0; i < components.size(); ++i) {
            const auto legal = allowed(components[i]);
            if (std::find(legal.begin(),legal.end(),decisions[i].slot) == legal.end() ||
                !std::isfinite(decisions[i].cost_before) || !std::isfinite(decisions[i].cost_after))
                throw std::invalid_argument("Portrait decision violates its visual role or visibility gate.");
        }
    }
    nlohmann::json encode() const {
        validate_solution();
        auto slots = nlohmann::json::array(), assignments = nlohmann::json::array();
        std::set<size_t> used;
        std::vector<size_t> choices;
        std::vector<size_t> initial;
        for (const auto& c : components) initial.push_back(c.original_slot);
        for (const auto& s : palette) slots.push_back({{"uid",s.uid},{"rgb",s.rgb}});
        for (size_t i = 0; i < components.size(); ++i) {
            const auto& c = components[i]; const auto& d = decisions[i];
            if (d.slot >= palette.size()) throw std::invalid_argument("Invalid plan assignment.");
            used.insert(d.slot); choices.push_back(d.slot);
            assignments.push_back({{"component_id",c.id},{"region",c.region},{"label",c.label},
                {"leaves",AI::BeautyLeafDomain::encode_keys(c.leaves)},{"original_uid",palette[c.original_slot].uid},
                {"applied_uid",palette[d.slot].uid},{"source_rgb",c.source},{"area",c.area},
                {"visible_pixels",c.visible_pixels},{"cost_before",d.cost_before},{"cost_after",d.cost_after},{"reasons",d.reasons}});
            if (!c.color_sources.is_null()) {
                assignments.back()["original_uid"] = nullptr;
                assignments.back()["initial_uid"] = palette[c.original_slot].uid;
                assignments.back()["color_sources"] = c.color_sources;
            }
        }
        std::vector<std::string> unused;
        for (size_t i = 0; i < palette.size(); ++i) if (!used.count(i)) unused.push_back(palette[i].uid);
        const nlohmann::json policy = {{"algorithm",repair_domain ? "r6-parent-component/v1" : "r5-region-component/v2"},{"weights",weights},
            {"normalization","region-balanced-0-1"},{"extra_min_views",2},{"extra_min_pixels",32},
            {"extra_min_delta_e",.03},{"ordinary_fragment_area_mm2",.5},{"max_sweeps",8},{"oral_automatic",false}};
        nlohmann::json result = {{"schema","orca.portrait-color-plan/v1"},{"geometry_id",geometry_id},{"source_sha256",source_sha256},
            {"face_count",face_count},{"boundary_sha256",boundary_sha256},{"palette",slots},
            {"palette_sha256",AI::beauty_leaf_digest(slots.dump())},{"policy",policy},
            {"policy_sha256",AI::beauty_leaf_digest(policy.dump())},{"assignments",assignments},
            {"initial_terms",terms(initial)},{"initial_cost",cost(initial)},
            {"terms",terms(choices)},{"cost",cost(choices)},{"total_cost_delta",cost(choices)-cost(initial)},
            {"decision_cost_before_definition","Final assignment with only this component restored to its original slot."},
            {"unused_slots",unused},
            {"visual_status","PENDING_USER"},{"production_enabled",false},{"material_write_authorized",false}};
        if (repair_domain) {
            result["ownership_ref"] = ownership_reference;
            result["editing_mapping_sha256"] = repair_domain->fingerprint();
            result["composition_priority"] = {"MANUAL", "R5_SHAPE_LOCK", "CONFIRMED_PARENT", "UNRESOLVED_R5"};
            result["preserve_reason"] = "OUTSIDE_CONFIRMED_PARENT_OWNERSHIP";
        }
        return result;
    }
};
} // namespace Slic3r::GUI
