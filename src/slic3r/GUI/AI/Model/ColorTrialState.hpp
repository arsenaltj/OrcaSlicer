#pragma once

#include "../ModelGeneration/ModelPreviewPalette.hpp"
#include <nlohmann/json.hpp>

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <sstream>
#include <iomanip>
#include <utility>
#include <vector>

namespace Slic3r::AI::ColorTrialPersistence {

// Pure saved snapshot of the controls. Source/target pairs retain their order:
// target colors alone cannot reconstruct which original colors they replace.
// A saved project/pack choice should be converted to source=2 (manual snapshot)
// by the host; dynamic pack indices and project signatures are not persisted.
struct State {
    struct StoredSlot {
        std::string id;
        GUI::PreviewPalette::Color color {}, mapping_color {};
        bool enabled {true};
    };
    std::vector<GUI::PreviewPalette::Color> colors, mapping_colors;
    std::array<bool, 6> locks {};
    int source {0}, count {6};
    bool enabled {false}, fidelity {true}, lighting {false};
    bool semantic_optimization {true};
    std::vector<GUI::PreviewPalette::Color> semantic_palette, semantic_mapping_palette, semantic_portrait_card;
    std::vector<std::string> slot_ids;
    std::vector<bool> slot_enabled;
    std::vector<std::string> legacy_slot_ids;
    std::vector<StoredSlot> dormant_slots;
};

inline std::string stable_slot_uid(const std::string& scope, const std::string& source_identity,
                                   size_t occurrence = 0)
{
    uint64_t hash = 1469598103934665603ull;
    const auto append = [&hash](const void* bytes, size_t count) {
        const auto* data = static_cast<const unsigned char*>(bytes);
        for (size_t index = 0; index < count; ++index) {
            hash ^= data[index];
            hash *= 1099511628211ull;
        }
    };
    append(scope.data(), scope.size());
    const unsigned char separator = 0;
    append(&separator, 1);
    append(source_identity.data(), source_identity.size());
    append(&occurrence, sizeof(occurrence));
    std::ostringstream output;
    output << "slot-" << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

inline std::string new_slot_uid()
{
    static std::atomic<uint64_t> counter {0};
    const uint64_t serial = counter.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint64_t stamp = uint64_t(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    return stable_slot_uid("created", std::to_string(stamp), size_t(serial));
}

// Preserve hidden manual slots as data rather than reconstructing their colors
// from a new histogram when the count increases again.
inline void remember_manual_slots(State& state)
{
    const auto& targets = state.semantic_palette.empty() ? state.colors : state.semantic_palette;
    const auto& mapping = state.semantic_mapping_palette.empty() ?
        (state.semantic_palette.empty() ? state.mapping_colors : targets) : state.semantic_mapping_palette;
    state.dormant_slots.resize(std::max(state.dormant_slots.size(), targets.size()));
    for (size_t i = 0; i < targets.size(); ++i)
        state.dormant_slots[i] = {i < state.slot_ids.size() ? state.slot_ids[i] : new_slot_uid(),
            targets[i], mapping[i], i >= state.slot_enabled.size() || state.slot_enabled[i]};
}

inline bool resize_manual_slots(State& state, size_t count, const std::vector<GUI::PreviewPalette::Color>& suggestions)
{
    if (count < 1 || count > 6) return false;
    remember_manual_slots(state);
    while (state.dormant_slots.size() < count) {
        const size_t index = state.dormant_slots.size();
        const auto color = suggestions.empty() ? GUI::PreviewPalette::Color{.5f,.5f,.5f} : suggestions[index % suggestions.size()];
        std::string id = new_slot_uid();
        while (std::any_of(state.dormant_slots.begin(), state.dormant_slots.end(), [&](const auto& slot) { return slot.id == id; }))
            id = new_slot_uid();
        state.dormant_slots.push_back({id, color, color, true});
    }
    state.colors.clear(); state.mapping_colors.clear(); state.slot_ids.clear(); state.slot_enabled.clear();
    for (size_t index = 0; index < count; ++index) {
        const auto& slot = state.dormant_slots[index];
        state.colors.push_back(slot.color); state.mapping_colors.push_back(slot.mapping_color);
        state.slot_ids.push_back(slot.id); state.slot_enabled.push_back(slot.enabled);
    }
    state.semantic_palette = state.colors; state.semantic_mapping_palette = state.mapping_colors;
    state.legacy_slot_ids = state.slot_ids;
    state.source = 2; state.count = int(count);
    return true;
}

inline bool edit_slot_color(State& state, const std::string& id, const GUI::PreviewPalette::Color& replacement)
{
    for (float channel : replacement) if (!std::isfinite(channel) || channel < 0.f || channel > 1.f) return false;
    const auto found = std::find(state.slot_ids.begin(), state.slot_ids.end(), id);
    if (found == state.slot_ids.end()) return false;
    const size_t slot = size_t(found - state.slot_ids.begin());
    if (state.semantic_palette.empty()) state.semantic_palette = state.colors;
    if (state.semantic_mapping_palette.empty()) state.semantic_mapping_palette = state.semantic_palette;
    if (slot >= state.semantic_palette.size()) return false;
    if (state.legacy_slot_ids.size() != state.colors.size()) {
        state.legacy_slot_ids.clear();
        if (state.colors == state.semantic_palette) state.legacy_slot_ids = state.slot_ids;
        else for (const auto& color : state.colors) {
            const auto match = std::find(state.semantic_palette.begin(), state.semantic_palette.end(), color);
            state.legacy_slot_ids.push_back(match == state.semantic_palette.end() ? std::string() :
                state.slot_ids[size_t(match - state.semantic_palette.begin())]);
        }
    }
    state.semantic_palette[slot] = replacement;
    for (size_t group = 0; group < state.colors.size(); ++group)
        if (state.legacy_slot_ids[group] == id) state.colors[group] = replacement;
    for (auto& dormant : state.dormant_slots) if (dormant.id == id) dormant.color = replacement;
    state.source = 2; state.enabled = true;
    return true;
}

namespace detail {
inline bool valid_fingerprint(const std::string& value)
{
    if (value.size() != 64) return false;
    for (char c : value) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

inline bool valid_state(const State& state)
{
    const size_t slots = state.semantic_palette.empty() ? state.colors.size() : state.semantic_palette.size();
    if ((!state.legacy_slot_ids.empty() && state.legacy_slot_ids.size() != state.colors.size()) || state.dormant_slots.size() > 6) return false;
    for (size_t i = 0; i < state.dormant_slots.size(); ++i) {
        const auto& slot = state.dormant_slots[i];
        if (slot.id.empty() || slot.id.size() > 256) return false;
        for (size_t j = 0; j < i; ++j) if (state.dormant_slots[j].id == slot.id) return false;
        for (const auto& color : {slot.color, slot.mapping_color}) for (float channel : color)
            if (!std::isfinite(channel) || channel < 0.f || channel > 1.f) return false;
    }
    if ((!state.slot_ids.empty() && state.slot_ids.size() != slots) ||
        (!state.slot_enabled.empty() && state.slot_enabled.size() != slots)) return false;
    for (size_t i = 0; i < state.slot_ids.size(); ++i) {
        if (state.slot_ids[i].empty()) return false;
        for (size_t j = 0; j < i; ++j) if (state.slot_ids[i] == state.slot_ids[j]) return false;
    }
    if (state.semantic_palette.size() > 6 ||
        (!state.semantic_mapping_palette.empty() && state.semantic_mapping_palette.size() != state.semantic_palette.size()) ||
        (!state.semantic_portrait_card.empty() && state.semantic_portrait_card.size() != 6)) return false;
    if (state.source < 0 || state.source > 2 || state.count < 1 || state.count > 6 ||
        state.colors.size() > size_t(state.count) || state.colors.size() != state.mapping_colors.size() ||
        (state.enabled && state.colors.empty())) return false;
    for (const auto* palette : {&state.colors, &state.mapping_colors, &state.semantic_palette, &state.semantic_mapping_palette, &state.semantic_portrait_card})
        for (const auto& color : *palette)
            for (float channel : color)
                if (!std::isfinite(channel) || channel < 0.0f || channel > 1.0f) return false;
    return true;
}

inline bool read_unsigned(const nlohmann::json& value, uint64_t& result)
{
    if (value.is_number_unsigned()) { result = value.get<uint64_t>(); return true; }
    if (!value.is_number_integer() || value.get<int64_t>() < 0) return false;
    result = uint64_t(value.get<int64_t>());
    return true;
}

inline bool read_palette(const nlohmann::json& value, std::vector<GUI::PreviewPalette::Color>& palette)
{
    if (!value.is_array() || value.size() > 6) return false;
    for (const auto& entry : value) {
        if (!entry.is_array() || entry.size() != 3) return false;
        GUI::PreviewPalette::Color color;
        for (size_t channel = 0; channel < color.size(); ++channel) {
            if (!entry[channel].is_number()) return false;
            const double number = entry[channel].get<double>();
            if (!std::isfinite(number) || number < 0.0 || number > 1.0) return false;
            color[channel] = float(number);
        }
        palette.push_back(color);
    }
    return true;
}
} // namespace detail

inline nlohmann::json encode(const State& state, size_t actual_face_count, const std::string& fingerprint)
{
    if (!detail::valid_fingerprint(fingerprint)) throw std::invalid_argument("Invalid trial color geometry fingerprint.");
    if (!detail::valid_state(state)) throw std::invalid_argument("Invalid trial color state.");
    nlohmann::json dormant = nlohmann::json::array();
    for (const auto& slot : state.dormant_slots) dormant.push_back({slot.id, slot.color, slot.mapping_color, slot.enabled});
    return {{"schema", "orca.color-trial/v1"}, {"geometry_sha256", fingerprint}, {"face_count", actual_face_count},
            {"colors", state.colors}, {"mapping_colors", state.mapping_colors}, {"locks", state.locks},
            {"source", state.source}, {"count", state.count}, {"enabled", state.enabled},
            {"fidelity", state.fidelity}, {"lighting", state.lighting},
            {"semantic_optimization", state.semantic_optimization},
            {"semantic_palette", state.semantic_palette}, {"semantic_mapping_palette", state.semantic_mapping_palette},
            {"semantic_portrait_card", state.semantic_portrait_card},
            {"slot_ids", state.slot_ids}, {"slot_enabled", state.slot_enabled},
            {"legacy_slot_ids", state.legacy_slot_ids}, {"dormant_slots", std::move(dormant)}};
}

// Reject mismatched geometry or malformed fields before returning any state.
// No palette is re-derived, reordered, or fitted to edited vertex colors.
// A disabled snapshot still retains its paired centers for later toggling.
inline bool decode(const nlohmann::json& doc, size_t actual_face_count, const std::string& fingerprint,
                   State& output, std::string& error)
{
    error.clear();
    auto fail = [&](const char* message) { error = message; return false; };
    if (!doc.is_object() || !doc.contains("schema") || doc["schema"] != "orca.color-trial/v1")
        return fail("Unsupported trial color schema.");
    if (!detail::valid_fingerprint(fingerprint) || !doc.contains("geometry_sha256") ||
        !doc["geometry_sha256"].is_string() || doc["geometry_sha256"].get_ref<const std::string&>() != fingerprint)
        return fail("The saved trial colors belong to different geometry.");
    uint64_t face_count, source, count;
    if (!doc.contains("face_count") || !detail::read_unsigned(doc["face_count"], face_count) || face_count != actual_face_count)
        return fail("The saved trial color face count does not match the loaded mesh.");
    if (!doc.contains("source") || !detail::read_unsigned(doc["source"], source) || source > 2 ||
        !doc.contains("count") || !detail::read_unsigned(doc["count"], count) || count < 1 || count > 6)
        return fail("Invalid trial color source or color count.");
    for (const char* field : {"enabled", "fidelity", "lighting"})
        if (!doc.contains(field) || !doc[field].is_boolean()) return fail("Trial color options must be explicit booleans.");
    if (!doc.contains("locks") || !doc["locks"].is_array() || doc["locks"].size() != 6)
        return fail("Trial colors require six lock flags.");
    State restored;
    for (size_t i = 0; i < restored.locks.size(); ++i) {
        if (!doc["locks"][i].is_boolean()) return fail("Trial color locks must be booleans.");
        restored.locks[i] = doc["locks"][i].get<bool>();
    }
    if (!doc.contains("colors") || !doc.contains("mapping_colors") ||
        !detail::read_palette(doc["colors"], restored.colors) ||
        !detail::read_palette(doc["mapping_colors"], restored.mapping_colors))
        return fail("Trial colors require normalized RGB palettes with at most six colors.");
    restored.source = int(source);
    restored.count = int(count);
    restored.enabled = doc["enabled"].get<bool>();
    restored.fidelity = doc["fidelity"].get<bool>();
    restored.lighting = doc["lighting"].get<bool>();
    // Existing saved manual trials predate semantic suggestions. Restoring
    // them must not silently reinterpret the user's original group assignments.
    restored.semantic_optimization = false;
    if (doc.contains("semantic_optimization")) {
        if (!doc["semantic_optimization"].is_boolean()) return fail("Invalid semantic color option.");
        restored.semantic_optimization = doc["semantic_optimization"].get<bool>();
    }
    if (doc.contains("semantic_palette") && !detail::read_palette(doc["semantic_palette"], restored.semantic_palette))
        return fail("Invalid semantic palette.");
    if (doc.contains("semantic_mapping_palette") && !detail::read_palette(doc["semantic_mapping_palette"], restored.semantic_mapping_palette))
        return fail("Invalid semantic candidate assignments.");
    if (doc.contains("semantic_portrait_card") && !detail::read_palette(doc["semantic_portrait_card"], restored.semantic_portrait_card))
        return fail("Invalid semantic portrait card.");
    if (!restored.semantic_portrait_card.empty() && restored.semantic_portrait_card.size() != 6)
        return fail("A semantic portrait card requires six roles.");
    if (doc.contains("slot_ids")) {
        if (!doc["slot_ids"].is_array() || doc["slot_ids"].size() > 6) return fail("Invalid slot identities.");
        for (const auto& id : doc["slot_ids"]) {
            if (!id.is_string()) return fail("Invalid slot identity.");
            restored.slot_ids.push_back(id.get<std::string>());
        }
    }
    if (doc.contains("slot_enabled")) {
        if (!doc["slot_enabled"].is_array() || doc["slot_enabled"].size() > 6) return fail("Invalid active slots.");
        for (const auto& active : doc["slot_enabled"]) {
            if (!active.is_boolean()) return fail("Invalid active slot flag.");
            restored.slot_enabled.push_back(active.get<bool>());
        }
    }
    if (doc.contains("legacy_slot_ids")) {
        if (!doc["legacy_slot_ids"].is_array() || doc["legacy_slot_ids"].size() > 6) return fail("Invalid legacy slot identities.");
        for (const auto& id : doc["legacy_slot_ids"]) {
            if (!id.is_string()) return fail("Invalid legacy slot identity.");
            restored.legacy_slot_ids.push_back(id.get<std::string>());
        }
    }
    if (doc.contains("dormant_slots")) {
        if (!doc["dormant_slots"].is_array() || doc["dormant_slots"].size() > 6) return fail("Invalid dormant slots.");
        for (const auto& entry : doc["dormant_slots"]) {
            if (!entry.is_array() || entry.size() != 4 || !entry[0].is_string() || !entry[3].is_boolean())
                return fail("Invalid dormant slot entry.");
            std::vector<GUI::PreviewPalette::Color> pair;
            if (!detail::read_palette(nlohmann::json::array({entry[1],entry[2]}), pair)) return fail("Invalid dormant slot colors.");
            restored.dormant_slots.push_back({entry[0].get<std::string>(),pair[0],pair[1],entry[3].get<bool>()});
        }
    }
    if (!detail::valid_state(restored)) return fail("Trial color centers and targets must form valid pairs.");
    output = std::move(restored);
    return true;
}
} // namespace Slic3r::AI::ColorTrialPersistence
