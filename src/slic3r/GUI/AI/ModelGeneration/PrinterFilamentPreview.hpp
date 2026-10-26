#pragma once

#include "../Model/ColorTrialState.hpp"
#include "../Orca/FilamentColorPack.hpp"
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <type_traits>

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

struct NativeColorSlot {
    size_t slot {0};
    Color color {};
    bool mixed {false};
};

inline uint32_t rgb8(const Color& color)
{
    auto channel = [](float value) {
        return uint32_t(std::lround(std::clamp(value, 0.f, 1.f) * 255.f));
    };
    return (channel(color[0]) << 16) | (channel(color[1]) << 8) | channel(color[2]);
}

inline std::optional<uint32_t> rgb8(const std::string& color)
{
    if (!AI::is_rgb_hex_color(color)) return {};
    uint32_t value = 0;
    for (size_t index = 1; index < color.size(); ++index) {
        const char ch = color[index];
        const uint32_t digit = ch >= '0' && ch <= '9' ? uint32_t(ch - '0') :
            ch >= 'A' && ch <= 'F' ? uint32_t(ch - 'A' + 10) : uint32_t(ch - 'a' + 10);
        value = (value << 4) | digit;
    }
    return value;
}

inline Color rgb8_color(uint32_t value)
{
    return {float((value >> 16) & 0xff) / 255.f,
            float((value >> 8) & 0xff) / 255.f,
            float(value & 0xff) / 255.f};
}

// Color controls expose the ordinary palette as the semantic fallback while
// no distinct semantic palette exists. Printer-backed states must bind that
// fallback to the same native slots or the persisted state becomes invalid as
// soon as it is read back from the controls.
inline void bind_implicit_semantic_palette(AI::ColorTrialPersistence::State& state)
{
    if (state.colors.empty() || state.colors.size() > 6 ||
        state.project_color_slots.size() != state.colors.size())
        return;
    if (state.semantic_palette.empty()) {
        state.semantic_palette = state.colors;
        state.semantic_mapping_palette = state.mapping_colors;
    }
    // Older printer-backed records may already expose the ordinary palette as
    // their semantic fallback while lacking the corresponding slot bindings.
    // Complete that implicit representation instead of disabling the preview.
    if (state.project_semantic_slots.empty() && state.semantic_palette == state.colors)
        state.project_semantic_slots = state.project_color_slots;
}

inline std::string rgb8_hex(uint32_t value)
{
    std::ostringstream out;
    out << '#' << std::uppercase << std::hex << std::setw(6) << std::setfill('0') << (value & 0xffffff);
    return out.str();
}

// Build the color card represented by an actual model appearance. Printer
// filament state is deliberately rejected: callers must use the captured
// pre-printer state so a round trip never imports the printer palette back
// into itself.
inline std::optional<FilamentColorPack> model_color_pack(
    const AI::ColorTrialPersistence::State& state,
    const std::string& name = u8"模型配色")
{
    if (state.source == 1 || state.colors.empty() ||
        state.colors.size() > PreviewPalette::max_preview_colors ||
        state.colors.size() != state.mapping_colors.size())
        return std::nullopt;

    FilamentColorPack pack;
    pack.name = name;
    pack.colors.reserve(state.colors.size());
    pack.labels.reserve(state.colors.size());
    for (size_t index = 0; index < state.colors.size(); ++index) {
        for (float channel : state.colors[index])
            if (!std::isfinite(channel) || channel < 0.f || channel > 1.f)
                return std::nullopt;
        pack.colors.push_back(rgb8_hex(rgb8(state.colors[index])));
        pack.labels.push_back("槽位 " + std::to_string(index + 1));
    }
    return pack.valid_for_workbench_import()
        ? std::optional<FilamentColorPack>(std::move(pack)) : std::nullopt;
}

struct ExactColorBinding {
    std::vector<size_t> slots;
    std::vector<std::string> missing_colors;

    bool complete(size_t color_count) const
    {
        return missing_colors.empty() && slots.size() == color_count;
    }
};

// Coverage is an RGB8 set comparison. Candidate order is authoritative for
// ties: callers provide physical slots first, then mixed slots, each in native
// slot order. A still-valid preferred slot preserves an existing binding.
inline ExactColorBinding bind_exact_colors(const std::vector<Color>& colors,
    const std::vector<NativeColorSlot>& candidates,
    const std::vector<std::optional<size_t>>& preferred = {})
{
    ExactColorBinding result;
    result.slots.reserve(colors.size());
    std::set<uint32_t> missing;
    for (size_t index = 0; index < colors.size(); ++index) {
        const uint32_t key = rgb8(colors[index]);
        auto chosen = candidates.end();
        if (index < preferred.size() && preferred[index]) {
            chosen = std::find_if(candidates.begin(), candidates.end(), [&](const NativeColorSlot& candidate) {
                return candidate.slot == *preferred[index] && rgb8(candidate.color) == key;
            });
        }
        if (chosen == candidates.end()) {
            chosen = std::find_if(candidates.begin(), candidates.end(), [&](const NativeColorSlot& candidate) {
                return rgb8(candidate.color) == key;
            });
        }
        if (chosen == candidates.end()) {
            missing.insert(key);
            result.slots.push_back(size_t(-1));
        } else {
            result.slots.push_back(chosen->slot);
        }
    }
    for (uint32_t key : missing) result.missing_colors.push_back(rgb8_hex(key));
    return result;
}

// Rebuild the model's automatic color basis for every currently available
// native slot, then bind only the non-empty source regions one-to-one. The
// requested/project count retains unused native slots without inventing tiny
// regions on the model.
inline bool apply_automatic_basis(AI::ColorTrialPersistence::State& state,
    const PreviewPalette::AutomaticColorBasis& basis,
    const std::vector<size_t>& slots, const std::vector<Color>& colors,
    const std::string& identity)
{
    if (!basis.valid() || identity.empty() || colors.empty() ||
        colors.size() > PreviewPalette::max_preview_colors || colors.size() != slots.size()) return false;
    const auto remap = PreviewPalette::minimum_cost_color_card_remap(basis, colors, slots);
    if (!remap.valid(basis.source_palette.size())) return false;

    auto updated = state;
    updated.colors = remap.source_order_targets();
    updated.mapping_colors = basis.source_palette;
    updated.semantic_palette.clear();
    updated.semantic_mapping_palette.clear();
    updated.semantic_portrait_card.clear();
    updated.semantic_role_uids.clear();
    updated.semantic_region_slots.fill(-1);
    updated.semantic_optimization = false;
    updated.project_color_slots.clear();
    for (size_t target : remap.source_to_target_slot)
        updated.project_color_slots.push_back(remap.target_slot_ids[target]);
    updated.project_semantic_slots.clear();
    bind_implicit_semantic_palette(updated);
    updated.source = 1;
    updated.suite_id.clear();
    updated.suite_revision.clear();
    updated.enabled = true;
    updated.count = int(colors.size());
    updated.project_count = int(colors.size());
    updated.project_slot_identity = identity;
    updated.automatic_basis_signature = basis.basis_signature;
    updated.color_card_target_palette = remap.target_palette;
    updated.color_card_target_slot_ids = remap.target_slot_ids;
    updated.source_to_target_slot = remap.source_to_target_slot;
    updated.color_card_weighted_error = remap.weighted_error;
    updated.locks.fill(false);
    if (!AI::ColorTrialPersistence::detail::valid_state(updated)) return false;
    state = std::move(updated);
    return true;
}

// A saved candidate already contains the mapped target colors in its material
// appearance. Reusing the pre-save source centers would apply the printer
// mapping a second time after reload. Preserve every native slot identity and
// unused logical slot, but make the baked targets their own display centers.
template<class State>
inline State normalize_baked_candidate_state(State state)
{
    static_assert(std::is_base_of_v<AI::ColorTrialPersistence::State, State>);
    if (state.source != 1 || !state.enabled || state.colors.empty() ||
        state.colors.size() != state.mapping_colors.size())
        return state;
    state.mapping_colors = state.colors;
    if (!state.semantic_palette.empty())
        state.semantic_mapping_palette = state.semantic_palette;
    return state;
}

// Explicit native identities chosen by the compatible recipe planner.
inline bool apply_bound(AI::ColorTrialPersistence::State& state, const std::vector<size_t>& bindings,
    const std::vector<Color>& matched, const std::string& identity)
{
    const size_t color_count = state.colors.size();
    const size_t semantic_count = state.semantic_palette.size();
    const size_t count = color_count + semantic_count;
    // A portrait/semantic palette describes the same logical color slots as
    // the regular palette.  It must share the slot binding; concatenating the
    // two palettes would make a six-color card look like twelve requested
    // consumables and create extra mix slots.  Keep accepting the historical
    // combined form for snapshots written before this shared-slot rule.
    const bool shared_palette = color_count && semantic_count == color_count &&
        bindings.size() == color_count && matched.size() == color_count;
    if (identity.empty() || (!shared_palette && (bindings.size() != count || matched.size() != count)) ||
        state.colors.empty()) return false;
    auto updated = state;
    updated.colors.assign(matched.begin(), matched.begin() + color_count);
    updated.project_color_slots.assign(bindings.begin(), bindings.begin() + color_count);
    if (shared_palette) {
        updated.semantic_palette = updated.colors;
        updated.project_semantic_slots = updated.project_color_slots;
    } else {
        updated.semantic_palette.assign(matched.begin() + color_count, matched.end());
        updated.project_semantic_slots.assign(bindings.begin() + color_count, bindings.end());
    }
    updated.source = 1; updated.enabled = true; updated.count = int(updated.colors.size());
    updated.suite_id.clear();
    updated.suite_revision.clear();
    updated.project_slot_identity = identity;
    updated.color_card_target_palette.clear();
    updated.color_card_target_slot_ids.clear();
    updated.source_to_target_slot.clear();
    updated.color_card_weighted_error = 0.0;
    bind_implicit_semantic_palette(updated);
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
    updated.suite_id.clear();
    updated.suite_revision.clear();
    updated.project_slot_identity = identity;
    updated.color_card_target_palette.clear();
    updated.color_card_target_slot_ids.clear();
    updated.source_to_target_slot.clear();
    updated.color_card_weighted_error = 0.0;
    bind_implicit_semantic_palette(updated);
    if (!AI::ColorTrialPersistence::detail::valid_state(updated)) return false;
    state = std::move(updated);
    return true;
}
} // namespace Slic3r::GUI::PrinterFilamentPreview
