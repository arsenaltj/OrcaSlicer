#pragma once

#include "LocalPrintColorCommit.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelPreviewPalette.hpp"

namespace Slic3r::GUI::SlicingModelPalette {

// Work with the current formal model, including painted subfaces. Geometry,
// physical spool colors and unrelated volumes are never changed by this tool.
class Selector : public TriangleSelector {
public:
    using TriangleSelector::TriangleSelector;
    std::vector<double> areas(int base) const {
        std::vector<double> weights(32, 0.);
        for (const auto& triangle : m_triangles) {
            if (!triangle.valid() || triangle.is_split()) continue;
            const int state = int(triangle.get_state());
            const int slot = (state == 0 ? base : state) - 1;
            if (slot < 0 || slot >= 32) continue;
            const auto& a = m_vertices[triangle.verts_idxs[0]].v;
            const auto& b = m_vertices[triangle.verts_idxs[1]].v;
            const auto& c = m_vertices[triangle.verts_idxs[2]].v;
            weights[slot] += .5 * double((b-a).cross(c-a).norm());
        }
        return weights;
    }
    void remap(const EnforcerBlockerStateMap& map) {
        for (auto& triangle : m_triangles)
            if (triangle.valid() && !triangle.is_split())
                triangle.set_state(map[size_t(triangle.get_state())]);
    }
};

inline int base_extruder(const ModelVolume& volume) {
    if (const auto* value = dynamic_cast<const ConfigOptionInt*>(volume.config.option("extruder")); value && value->value > 0) return value->value;
    if (const auto* object = volume.get_object())
        if (const auto* value = dynamic_cast<const ConfigOptionInt*>(object->config.option("extruder")); value && value->value > 0) return value->value;
    return 1;
}

inline std::vector<PreviewPalette::Color> select_targets(const PreviewPalette::Histogram& histogram,
    const std::vector<PreviewPalette::Color>& physical, const std::vector<bool>& compatible,
    size_t count, int source, const std::vector<PreviewPalette::Color>& manual = {},
    const std::vector<PreviewPalette::Color>& preferred = {})
{
    count = std::clamp(count, size_t(1), size_t(32));
    std::vector<PreviewPalette::Color> targets;
    if (source == 2) {
        targets = manual;
    } else if (source == 1) {
        for (size_t i = 0; i < physical.size() && i < compatible.size(); ++i)
            if (compatible[i]) targets.push_back(physical[i]);
    } else if (!preferred.empty()) {
        targets = preferred;
    } else {
        targets = histogram.trial_palette(count, {}, true);
    }
    if (targets.size() > count) targets.resize(count);
    return targets;
}

inline bool prepare(const TriangleMesh& mesh, const TriangleSelector::TriangleSplittingData& source, int base,
    const std::vector<PreviewPalette::Color>& colors, const std::vector<bool>& compatible,
    const std::vector<PreviewPalette::Color>& targets, LocalPrintColorApplication::PreparedPainting& prepared,
    std::vector<size_t>& used_slots, std::string& error)
{
    error.clear();
    if (colors.empty() || colors.size() > 32 || compatible.size() != colors.size() || targets.empty() ||
        targets.size() > 32 || base < 1 || size_t(base) > colors.size()) {
        error = "模型色卡或耗材槽无效。"; return false;
    }
    auto valid = [](const auto& color) {
        return std::all_of(color.begin(), color.end(), [](float c) { return std::isfinite(c) && c >= 0.f && c <= 1.f; });
    };
    if (!std::all_of(colors.begin(), colors.end(), valid) || !std::all_of(targets.begin(), targets.end(), valid)) {
        error = "色卡含无效颜色。"; return false;
    }
    std::vector<PreviewPalette::Color> physical_lab, target_lab;
    std::vector<size_t> slots;
    for (size_t i = 0; i < colors.size(); ++i) if (compatible[i]) {
        physical_lab.push_back(PreviewPalette::to_lab(colors[i])); slots.push_back(i);
    }
    if (slots.empty()) { error = "当前工程没有兼容的耗材槽。"; return false; }
    for (auto color : targets) target_lab.push_back(PreviewPalette::to_lab(color));
    EnforcerBlockerStateMap map;
    for (size_t i = 0; i < map.size(); ++i) map[i] = static_cast<EnforcerBlockerType>(i);
    for (size_t i = 0; i < colors.size(); ++i) {
        const auto target = PreviewPalette::nearest_lab_index(PreviewPalette::to_lab(colors[i]), target_lab);
        const auto physical = PreviewPalette::nearest_lab_index(target_lab[target], physical_lab);
        map[i+1] = static_cast<EnforcerBlockerType>(slots[physical]+1);
    }
    map[0] = map[base];
    Selector selector(mesh);
    selector.deserialize(source);
    const auto weights = selector.areas(base);
    for (size_t i = colors.size(); i < weights.size(); ++i)
        if (weights[i] > 0) { error = "模型引用了当前工程不存在的耗材槽。"; return false; }
    std::vector<size_t> used;
    for (size_t i = 0; i < colors.size(); ++i) if (weights[i] > 0) {
        const auto slot = size_t(map[i+1])-1;
        if (std::find(used.begin(), used.end(), slot) == used.end()) used.push_back(slot);
    }
    selector.remap(map);
    LocalPrintColorApplication::PreparedPainting next;
    next.data = selector.serialize();
    next.base_extruder = int(map[base]);
    next.geometry_id = AI::SurfaceSelectionPersistence::geometry_fingerprint(mesh.its);
    prepared = std::move(next); used_slots = std::move(used);
    return true;
}
} // namespace Slic3r::GUI::SlicingModelPalette
