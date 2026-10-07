#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace Slic3r::AI {

struct BeautyDetailChoice {
    std::string name;
    uint32_t region_id;
    size_t seed_faces;
};

// Recognition supplies seeds; the existing puzzle owns the editable boundary.
// A mixed region is not advertised as a named facial detail.
inline std::vector<BeautyDetailChoice> beauty_detail_choices(
    const std::vector<int32_t>& labels, const std::vector<std::string>& names,
    const std::vector<uint32_t>& regions, const std::vector<double>& areas)
{
    if (labels.size() != regions.size() || labels.size() != areas.size())
        throw std::invalid_argument("Beauty detail identity does not match the surface.");
    const auto is_detail = [](const std::string& name) {
        return name == "lb" || name == "rb" || name == "le" || name == "re" ||
            name == "iris" || name == "ulip" || name == "llip" || name == "imouth" ||
            name == "lip-line-corner" || name == "nose" || name == "lr" || name == "rr";
    };
    struct Support { double area = 0.; size_t faces = 0; };
    std::map<uint32_t, std::map<int32_t, Support>> by_region;
    std::map<uint32_t, double> region_area;
    for (size_t face = 0; face < labels.size(); ++face) {
        const double area = std::max(areas[face], 1e-15);
        region_area[regions[face]] += area;
        const int32_t label = labels[face];
        if (label < 0) continue;
        if (size_t(label) >= names.size() || regions[face] == 0)
            throw std::invalid_argument("Invalid beauty detail label or region.");
        auto& support = by_region[regions[face]][label];
        support.area += area;
        ++support.faces;
    }
    std::vector<BeautyDetailChoice> result;
    for (const auto& region : by_region) {
        double known_area = 0.;
        int32_t best_label = -1;
        double best_area = 0.;
        for (const auto& item : region.second) {
            known_area += item.second.area;
            if (item.second.area > best_area) {
                best_label = item.first;
                best_area = item.second.area;
            }
        }
        if (best_label >= 0 && best_area >= known_area * .8 &&
            best_area >= region_area.at(region.first) * .1 &&
            is_detail(names[size_t(best_label)]))
            result.push_back({names[size_t(best_label)], region.first,
                region.second.at(best_label).faces});
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.name == b.name ? a.region_id < b.region_id : a.name < b.name;
    });
    return result;
}

} // namespace Slic3r::AI
