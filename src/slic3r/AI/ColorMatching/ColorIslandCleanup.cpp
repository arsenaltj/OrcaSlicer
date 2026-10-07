#include "ColorIslandCleanup.hpp"
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace Slic3r::AI::ColorMatching {
std::vector<ColorIslandReplacement> find_enclosed_color_islands(
    const std::vector<size_t>& slots, const std::vector<uint8_t>& selected,
    const std::vector<uint8_t>& pinned, const std::vector<std::array<int32_t, 3>>& neighbors,
    const std::vector<double>& areas, const std::vector<std::array<uint8_t, 3>>& smooth_edges,
    const ColorIslandCleanupOptions& options, const std::function<bool()>& canceled) {
    const size_t count = slots.size();
    const auto require = [](bool ok) { if (!ok) throw std::invalid_argument("Invalid color island input."); };
    const auto checkpoint = [&] { if (canceled && canceled()) throw std::runtime_error("Color cleanup cancelled."); };
    require(count > 0 && selected.size() == count && pinned.size() == count && neighbors.size() == count &&
            areas.size() == count && smooth_edges.size() == count);
    require(std::isfinite(options.maximum_island_fraction) && options.maximum_island_fraction >= 0. &&
            options.maximum_island_fraction <= .05 && std::isfinite(options.minimum_surrounding_area_ratio) &&
            options.minimum_surrounding_area_ratio >= 2.);
    checkpoint();
    for (size_t f = 0; f < count; ++f) {
        if ((f & 4095) == 0) checkpoint();
        require(slots[f] != std::numeric_limits<size_t>::max() && selected[f] <= 1 && pinned[f] <= 1 &&
                std::isfinite(areas[f]) && areas[f] >= 0.);
        for (size_t e = 0; e < 3; ++e) {
            const int32_t n = neighbors[f][e];
            require(n >= -1 && (n < 0 || (size_t(n) < count && size_t(n) != f)) && smooth_edges[f][e] <= 1);
            if (n >= 0) {
                size_t reverse = 0;
                while (reverse < 3 && neighbors[size_t(n)][reverse] != int32_t(f)) ++reverse;
                require(reverse < 3 && smooth_edges[size_t(n)][reverse] == smooth_edges[f][e]);
            }
        }
    }
    const size_t none = count;
    std::vector<size_t> patch(count, none), component(count, none), queue;
    std::vector<double> patch_area, component_area;
    std::vector<std::vector<size_t>> members;
    std::vector<uint8_t> protected_component;
    // Each disconnected selected surface has its own area budget.
    for (size_t seed = 0; seed < count; ++seed) if (selected[seed] && patch[seed] == none) {
        checkpoint();
        const size_t id = patch_area.size(); patch_area.push_back(0.);
        queue.assign(1, seed); patch[seed] = id;
        for (size_t at = 0; at < queue.size(); ++at) {
            if ((at & 4095) == 0) checkpoint();
            const size_t f = queue[at]; patch_area[id] += areas[f];
            require(std::isfinite(patch_area[id]));
            for (int32_t n : neighbors[f]) if (n >= 0 && selected[size_t(n)] && patch[size_t(n)] == none) {
                patch[size_t(n)] = id; queue.push_back(size_t(n));
            }
        }
    }
    for (size_t seed = 0; seed < count; ++seed) if (selected[seed] && component[seed] == none) {
        checkpoint();
        const size_t id = members.size(); members.emplace_back(); component_area.push_back(0.);
        protected_component.push_back(0); queue.assign(1, seed); component[seed] = id;
        for (size_t at = 0; at < queue.size(); ++at) {
            if ((at & 4095) == 0) checkpoint();
            const size_t f = queue[at]; members[id].push_back(f); component_area[id] += areas[f];
            protected_component[id] |= pinned[f];
            for (int32_t n : neighbors[f]) if (n >= 0 && selected[size_t(n)] &&
                slots[size_t(n)] == slots[seed] && component[size_t(n)] == none) {
                component[size_t(n)] = id; queue.push_back(size_t(n));
            }
        }
    }
    std::vector<ColorIslandReplacement> result;
    for (size_t id = 0; id < members.size(); ++id) {
        checkpoint();
        const auto& faces = members[id];
        const double area = component_area[id];
        if (protected_component[id] || area <= 0. || area > patch_area[patch[faces.front()]] * options.maximum_island_fraction) continue;
        bool enclosed = true; size_t target = std::numeric_limits<size_t>::max();
        std::set<size_t> surrounding;
        for (size_t f : faces) {
            if ((f & 4095) == 0) checkpoint();
            for (size_t e = 0; e < 3; ++e) {
                const int32_t n = neighbors[f][e];
                if (n < 0 || !selected[size_t(n)] || !smooth_edges[f][e]) { enclosed = false; continue; }
                if (component[size_t(n)] == id) continue;
                if (target == std::numeric_limits<size_t>::max()) target = slots[size_t(n)];
                else if (target != slots[size_t(n)]) enclosed = false;
                surrounding.insert(component[size_t(n)]);
            }
        }
        double surrounding_area = 0.;
        for (size_t other : surrounding) surrounding_area += component_area[other];
        if (enclosed && !surrounding.empty() && surrounding_area >= area * options.minimum_surrounding_area_ratio)
            result.push_back({target, faces});
    }
    checkpoint();
    return result;
}
}
