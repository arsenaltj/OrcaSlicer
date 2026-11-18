#pragma once
#include "ColorUtils.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <numeric>
#include <vector>

namespace Slic3r::tex2color {

// A bounded, call-local diagnostic cache. Exact source/target RGB words are
// the identity, and collisions only evict entries. The scalar calculation and
// the caller's face-order accumulation remain unchanged.
class FallbackColorDifferences {
    struct Entry { uint64_t key = UINT64_MAX; double value = 0.; };
    std::vector<Entry> m_entries;
public:
    // One MiB bounds the diagnostic workspace while retaining repeated RGB
    // pairs from detailed textures. Collisions still only evict exact values.
    explicit FallbackColorDifferences(bool enabled = true) : m_entries(enabled ? 65536 : 0) {}
    double operator()(const std::array<size_t, 3>& source, const std::array<size_t, 3>& target) {
        uint64_t key = 0;
        for (const auto& color : {source, target}) for (size_t channel : color) {
            if (channel > 255)
                return color_utils::calc_rgb_color_difference_by_ciede2000(source, target);
            key = (key << 8) | channel;
        }
        if (m_entries.empty()) return color_utils::calc_rgb_color_difference_by_ciede2000(source, target);
        const auto slot = ((key ^ (key >> 17) ^ (key >> 33)) * 11400714819323198485ULL) >> 48;
        auto& entry = m_entries[size_t(slot)];
        if (entry.key != key) {
            entry.value = color_utils::calc_rgb_color_difference_by_ciede2000(source, target);
            entry.key = key;
        }
        return entry.value;
    }
};

// Bounded, deterministic weighted median cut. Only used after the existing
// palette/label path fails; it never edits coordinates or repairs geometry.
inline bool fallback_color_labels(const std::vector<std::array<size_t, 3>>& colors,
    std::vector<std::array<size_t, 3>>& palette, std::vector<size_t>& labels,
    const std::function<bool()>& canceled)
{
    using RGB = std::array<size_t, 3>;
    if (colors.empty()) return false;
    std::map<RGB, uint64_t> exact;
    for (const auto& color : colors) {
        if (color[0] > 255 || color[1] > 255 || color[2] > 255) return false;
        ++exact[color];
        if (exact.size() > 32) break;
    }
    palette.clear();
    if (exact.size() <= 32) {
        for (const auto& row : exact) palette.push_back(row.first);
    } else {
        // 5-bit bins bound the histogram to 1 MiB; original channel sums
        // retain the actual weighted mean instead of inventing bin colors.
        struct Bin { uint64_t weight = 0; std::array<uint64_t, 3> sum {}; };
        std::vector<Bin> histogram(32768);
        for (size_t i = 0; i < colors.size(); ++i) {
            if ((i & 4095) == 0 && canceled && canceled()) return false;
            const auto& c = colors[i];
            if (c[0] > 255 || c[1] > 255 || c[2] > 255) return false;
            auto& bin = histogram[((c[0] >> 3) << 10) | ((c[1] >> 3) << 5) | (c[2] >> 3)];
            ++bin.weight;
            for (int axis = 0; axis < 3; ++axis) bin.sum[axis] += c[axis];
        }
        struct Sample { RGB color; uint64_t weight; };
        std::vector<Sample> samples;
        for (const auto& bin : histogram) if (bin.weight) {
            RGB c;
            for (int axis = 0; axis < 3; ++axis) c[axis] = size_t((bin.sum[axis] + bin.weight / 2) / bin.weight);
            samples.push_back({c, bin.weight});
        }
        std::vector<std::vector<size_t>> boxes(1);
        boxes.front().resize(samples.size());
        std::iota(boxes.front().begin(), boxes.front().end(), size_t(0));
        while (boxes.size() < 32) {
            if (canceled && canceled()) return false;
            size_t selected = boxes.size(), axis = 0;
            uint64_t best = 0;
            for (size_t b = 0; b < boxes.size(); ++b) {
                if (boxes[b].size() < 2) continue;
                RGB low {255,255,255}, high {0,0,0}; uint64_t weight = 0;
                for (size_t id : boxes[b]) {
                    weight += samples[id].weight;
                    for (int a = 0; a < 3; ++a) {
                        low[a] = std::min(low[a], samples[id].color[a]);
                        high[a] = std::max(high[a], samples[id].color[a]);
                    }
                }
                for (size_t a = 0; a < 3; ++a) {
                    const uint64_t score = weight * (high[a] - low[a]);
                    if (score > best) { best = score; selected = b; axis = a; }
                }
            }
            if (selected == boxes.size()) break;
            auto& box = boxes[selected];
            std::sort(box.begin(), box.end(), [&](size_t a, size_t b) {
                if (samples[a].color[axis] != samples[b].color[axis])
                    return samples[a].color[axis] < samples[b].color[axis];
                return samples[a].color < samples[b].color;
            });
            uint64_t total = 0, left = 0;
            for (size_t id : box) total += samples[id].weight;
            size_t middle = 0;
            do { left += samples[box[middle++]].weight; } while (middle < box.size() - 1 && left < (total + 1) / 2);
            std::vector<size_t> right(box.begin() + middle, box.end());
            box.resize(middle); boxes.push_back(std::move(right));
        }
        for (const auto& box : boxes) {
            std::array<uint64_t, 3> sum {}; uint64_t weight = 0;
            for (size_t id : box) {
                weight += samples[id].weight;
                for (int a = 0; a < 3; ++a) sum[a] += samples[id].color[a] * samples[id].weight;
            }
            RGB c;
            for (int a = 0; a < 3; ++a) c[a] = size_t((sum[a] + weight / 2) / weight);
            palette.push_back(c);
        }
        std::sort(palette.begin(), palette.end());
        palette.erase(std::unique(palette.begin(), palette.end()), palette.end());
    }
    if (palette.empty()) return false;
    const color_utils::ColorDistancePalette nearest(palette);
    labels.resize(colors.size());
    for (size_t i = 0; i < colors.size(); ++i) {
        if ((i & 4095) == 0 && canceled && canceled()) return false;
        nearest.nearest(colors[i], labels[i]);
    }
    return true;
}
}
