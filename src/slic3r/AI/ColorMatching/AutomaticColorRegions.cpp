#include "AutomaticColorRegions.hpp"
#include "libslic3r/TextureToColor/ColorUtils.hpp"
#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace Slic3r::AI::ColorMatching {
namespace {
using RGB = tex2color::color_utils::ColorDouble;
RGB rgb(const RegionRGB& c) { return {c[0], c[1], c[2]}; }
double distance(const RGB& a, const RGB& b) {
    return tex2color::color_utils::calc_rgb_color_difference_by_ciede2000_srgb01(a, b);
}
void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}
}

std::vector<ColorRegionSplit> refine_automatic_color_regions(
    const std::vector<uint32_t>& face_regions, const std::vector<uint32_t>& face_patches,
    const std::vector<std::array<int32_t, 3>>& neighbors, const std::vector<double>& areas,
    const std::vector<RegionRGB>& source, const std::vector<AutomaticRegion>& eligible,
    const std::vector<RegionMaterial>& materials, const ColorRegionOptions& options,
    const std::function<bool()>& canceled)
{
    const auto checkpoint = [&] {
        if (canceled && canceled()) throw std::runtime_error("Colour region refinement cancelled.");
    };
    checkpoint();
    const size_t count = face_regions.size(), missing = std::numeric_limits<size_t>::max();
    require(count > 0 && face_patches.size() == count && neighbors.size() == count &&
        areas.size() == count && source.size() == count, "Colour region arrays belong to different geometry.");
    require(std::isfinite(options.minimum_patch_gain) && options.minimum_patch_gain >= 0 &&
        std::isfinite(options.minimum_surface_fraction) && options.minimum_surface_fraction >= 0 && options.minimum_surface_fraction <= 1 &&
        std::isfinite(options.minimum_region_fraction) && options.minimum_region_fraction >= 0 && options.minimum_region_fraction <= 1,
        "Invalid colour region thresholds.");
    std::unordered_map<size_t, size_t> material_index;
    std::vector<RGB> palette;
    for (const auto& m : materials) {
        require(material_index.emplace(m.slot, palette.size()).second, "Repeated material slot.");
        for (float c : m.color) require(std::isfinite(c) && c >= 0 && c <= 1, "Invalid material colour.");
        palette.push_back(rgb(m.color));
    }
    struct Owner { size_t baseline; double area = 0; };
    std::unordered_map<uint32_t, Owner> owners;
    for (const auto& r : eligible) {
        require(material_index.count(r.baseline_slot) != 0, "Automatic region uses an unavailable material.");
        require(owners.emplace(r.id, Owner{material_index.at(r.baseline_slot)}).second, "Repeated automatic region.");
    }
    struct Patch { RGB sum{}; double area = 0; size_t best = std::numeric_limits<size_t>::max(); };
    // A micro-patch may cross an editing boundary; never borrow its colour from
    // another region or a manually painted part.
    std::unordered_map<uint64_t, size_t> patch_index;
    std::vector<Patch> patches;
    std::vector<size_t> patch_of(count, missing);
    double total_area = 0;
    for (size_t f = 0; f < count; ++f) {
        if ((f & 4095) == 0) checkpoint();
        require(std::isfinite(areas[f]) && areas[f] >= 0, "Invalid face area.");
        for (float c : source[f]) require(std::isfinite(c) && c >= 0 && c <= 1, "Invalid source colour.");
        for (int32_t n : neighbors[f]) require(n == -1 || (n >= 0 && size_t(n) < count && size_t(n) != f), "Invalid face adjacency.");
        total_area += areas[f];
        auto owner = owners.find(face_regions[f]);
        if (owner == owners.end()) continue;
        owner->second.area += areas[f];
        const uint64_t key = (uint64_t(face_regions[f]) << 32) | face_patches[f];
        const auto added = patch_index.emplace(key, patches.size());
        if (added.second) patches.emplace_back();
        patch_of[f] = added.first->second;
        auto& patch = patches[patch_of[f]];
        patch.area += areas[f];
        for (size_t c = 0; c < 3; ++c) patch.sum[c] += areas[f] * source[f][c];
    }
    require(std::isfinite(total_area), "Invalid total surface area.");
    if (palette.size() < 2 || total_area == 0 || owners.empty()) return {};
    size_t inspected = 0;
    for (const auto& entry : patch_index) {
        if ((inspected++ & 255) == 0) checkpoint();
        auto& p = patches[entry.second];
        if (p.area <= 0) continue;
        for (double& c : p.sum) c /= p.area;
        const auto baseline = owners.at(uint32_t(entry.first >> 32)).baseline;
        const double old_error = distance(p.sum, palette[baseline]);
        double best_error = old_error;
        size_t best = baseline;
        for (size_t m = 0; m < palette.size(); ++m) {
            const double error = distance(p.sum, palette[m]);
            if (error < best_error) { best = m; best_error = error; }
        }
        if (best != baseline && old_error - best_error >= options.minimum_patch_gain) p.best = best;
    }
    std::vector<size_t> proposal(count, missing);
    for (size_t f = 0; f < count; ++f) {
        if ((f & 4095) == 0) checkpoint();
        if (patch_of[f] == missing || areas[f] <= 0) continue;
        const auto best = patches[patch_of[f]].best;
        if (best == missing) continue;
        const auto baseline = owners.at(face_regions[f]).baseline;
        // A patch average is only a proposal. Verify the actual face before it
        // can change; a darker/lighter neighbour cannot overrule its own colour.
        if (distance(rgb(source[f]), palette[best]) <= distance(rgb(source[f]), palette[baseline])) proposal[f] = best;
    }
    const auto minimum_area = [&](uint32_t id) {
        return std::max(total_area * options.minimum_surface_fraction, owners.at(id).area * options.minimum_region_fraction);
    };
    std::vector<uint8_t> visited(count, 0);
    std::vector<ColorRegionSplit> result;
    std::vector<size_t> queue;
    const auto collect = [&] {
        result.clear();
        std::fill(visited.begin(), visited.end(), 0);
        for (size_t seed = 0; seed < count; ++seed) {
            if ((seed & 4095) == 0) checkpoint();
            if (visited[seed] || proposal[seed] == missing) continue;
            queue.assign(1, seed); visited[seed] = 1;
            RGB sum{}; double area = 0;
            for (size_t at = 0; at < queue.size(); ++at) {
                if ((at & 4095) == 0) checkpoint();
                const size_t f = queue[at];
                area += areas[f];
                for (size_t c = 0; c < 3; ++c) sum[c] += areas[f] * source[f][c];
                for (int32_t n : neighbors[f]) if (n >= 0 && !visited[size_t(n)] &&
                    face_regions[size_t(n)] == face_regions[seed] && proposal[size_t(n)] == proposal[seed]) {
                    visited[size_t(n)] = 1; queue.push_back(size_t(n));
                }
            }
            if (area <= 0 || area < minimum_area(face_regions[seed])) continue;
            RegionRGB target{float(sum[0] / area), float(sum[1] / area), float(sum[2] / area), 1};
            // Keep rematching the stored target deterministic, including equal-RGB
            // slots. A component requiring a contradictory target is not accepted.
            size_t nearest = 0;
            double best_error = std::numeric_limits<double>::infinity();
            for (size_t m = 0; m < palette.size(); ++m) {
                const double error = distance(rgb(target), palette[m]);
                if (error < best_error) { nearest = m; best_error = error; }
            }
            if (nearest != proposal[seed]) continue;
            result.push_back({face_regions[seed], materials[nearest].slot, target, queue});
        }
    };
    collect();
    if (result.empty()) return result;
    // Area-filtering the new colours alone can strand hundreds of one-face
    // remnants of the original colour. Reconnect those remnants by withdrawing
    // short paths of proposed faces, never by painting over their source colour.
    // This is local to each automatic owner and cannot affect manual neighbours.
    std::fill(proposal.begin(), proposal.end(), missing);
    for (const auto& split : result) for (size_t f : split.faces) proposal[f] = material_index.at(split.slot);
    struct Remainder { uint32_t owner; double area = 0; std::vector<size_t> faces; };
    std::vector<Remainder> remnants;
    std::unordered_map<uint32_t, size_t> largest;
    std::fill(visited.begin(), visited.end(), 0);
    for (size_t seed = 0; seed < count; ++seed) {
        if ((seed & 4095) == 0) checkpoint();
        if (visited[seed] || proposal[seed] != missing || !owners.count(face_regions[seed])) continue;
        queue.assign(1, seed); visited[seed] = 1;
        double area = 0;
        for (size_t at = 0; at < queue.size(); ++at) {
            if ((at & 4095) == 0) checkpoint();
            const size_t f = queue[at]; area += areas[f];
            for (int32_t n : neighbors[f]) if (n >= 0 && !visited[size_t(n)] &&
                face_regions[size_t(n)] == face_regions[seed] && proposal[size_t(n)] == missing) {
                visited[size_t(n)] = 1; queue.push_back(size_t(n));
            }
        }
        const auto inserted = largest.emplace(face_regions[seed], remnants.size());
        if (!inserted.second && area > remnants[inserted.first->second].area) inserted.first->second = remnants.size();
        remnants.push_back({face_regions[seed], area, queue});
    }
    std::vector<uint8_t> anchor(remnants.size(), 0);
    std::unordered_set<uint32_t> reconnect;
    for (size_t i = 0; i < remnants.size(); ++i) {
        const auto& r = remnants[i];
        anchor[i] = r.area >= minimum_area(r.owner) || largest.at(r.owner) == i;
        if (!anchor[i]) reconnect.insert(r.owner);
    }
    if (!reconnect.empty()) {
        // 0/1 breadth-first search: existing colour is free; undoing a proposed
        // face costs one. A deterministic shortest path limits lost detail.
        std::vector<size_t> steps(count, missing), parent(count, missing);
        std::vector<uint8_t> connected(count, 0);
        std::deque<size_t> frontier;
        for (size_t i = 0; i < remnants.size(); ++i) if (anchor[i] && reconnect.count(remnants[i].owner))
            for (size_t f : remnants[i].faces) { steps[f] = 0; parent[f] = f; connected[f] = 1; frontier.push_back(f); }
        size_t scanned = 0;
        while (!frontier.empty()) {
            if ((scanned++ & 4095) == 0) checkpoint();
            const size_t f = frontier.front(); frontier.pop_front();
            for (int32_t neighbor : neighbors[f]) if (neighbor >= 0) {
                const size_t n = size_t(neighbor);
                if (face_regions[n] != face_regions[f]) continue;
                const size_t cost = proposal[n] != missing;
                if (steps[f] + cost >= steps[n]) continue;
                steps[n] = steps[f] + cost; parent[n] = f;
                if (cost) frontier.push_back(n); else frontier.push_front(n);
            }
        }
        for (size_t i = 0; i < remnants.size(); ++i) if (!anchor[i]) {
            size_t f = remnants[i].faces.front();
            while (!connected[f] && parent[f] != missing) {
                if ((scanned++ & 4095) == 0) checkpoint();
                connected[f] = 1; proposal[f] = missing; f = parent[f];
            }
        }
        // Retained islands must still meet the area and saved-target rules.
        // Any discarded fragment touches a withdrawn path, so discarding it
        // reconnects to retained colour rather than creating a new speck.
        collect();
    }
    checkpoint();
    return result;
}
} // namespace Slic3r::AI::ColorMatching
