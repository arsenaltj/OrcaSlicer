#pragma once

#include "../Model/ColorTrialState.hpp"
#include <limits>

namespace Slic3r::GUI::PrinterFilamentPreview {
using Color = PreviewPalette::Color;

inline size_t nearest(const Color& color, const std::vector<Color>& colors)
{
    size_t result = colors.size();
    float best = std::numeric_limits<float>::max();
    const auto lab = PreviewPalette::to_lab(color);
    for (size_t i = 0; i < colors.size(); ++i) {
        const float distance = PreviewPalette::distance(lab, PreviewPalette::to_lab(colors[i]));
        if (distance < best) { best = distance; result = i; }
    }
    return result;
}

// Explicit native identities chosen by the compatible recipe planner.
inline bool apply_bound(AI::ColorTrialPersistence::State& state, const std::vector<size_t>& bindings,
    const std::vector<Color>& matched, const std::string& identity)
{
    const size_t count = state.colors.size() + state.semantic_palette.size();
    if (identity.empty() || bindings.size() != count || matched.size() != count || state.colors.empty()) return false;
    auto updated = state;
    updated.colors.assign(matched.begin(), matched.begin() + state.colors.size());
    updated.semantic_palette.assign(matched.begin() + state.colors.size(), matched.end());
    updated.project_color_slots.assign(bindings.begin(), bindings.begin() + state.colors.size());
    updated.project_semantic_slots.assign(bindings.begin() + state.colors.size(), bindings.end());
    updated.source = 1; updated.enabled = true; updated.count = int(updated.colors.size());
    updated.project_slot_identity = identity;
    if (!AI::ColorTrialPersistence::detail::valid_state(updated)) return false;
    state = std::move(updated); return true;
}

// Keep source centers and region indices: matching changes only target colors.
// Prefer an existing bound slot on equal RGB, rather than merging slot identity.
inline bool apply(AI::ColorTrialPersistence::State& state, const std::vector<size_t>& slots,
    const std::vector<Color>& colors, const std::string& identity)
{
    if (colors.empty() || colors.size() > PreviewPalette::max_preview_colors ||
        colors.size() != slots.size() || identity.empty() || state.colors.empty()) return false;
    auto updated = state;
    auto match = [&](std::vector<Color>& palette, std::vector<size_t>& bindings) {
        const auto previous = bindings;
        bindings.clear();
        for (size_t i = 0; i < palette.size(); ++i) {
            size_t target = nearest(palette[i], colors);
            if (state.project_slot_identity == identity && i < previous.size()) {
                const auto found = std::find(slots.begin(), slots.end(), previous[i]);
                if (found != slots.end() && colors[size_t(found - slots.begin())] == palette[i])
                    target = size_t(found - slots.begin());
            }
            palette[i] = colors[target]; bindings.push_back(slots[target]);
        }
    };
    match(updated.colors, updated.project_color_slots);
    match(updated.semantic_palette, updated.project_semantic_slots);
    updated.source = 1; updated.enabled = true; updated.count = int(updated.colors.size());
    updated.project_slot_identity = identity;
    if (!AI::ColorTrialPersistence::detail::valid_state(updated)) return false;
    state = std::move(updated);
    return true;
}
} // namespace Slic3r::GUI::PrinterFilamentPreview
