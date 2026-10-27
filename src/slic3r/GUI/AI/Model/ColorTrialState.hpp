#pragma once

#include "../ModelGeneration/ModelPreviewPalette.hpp"
#include "slic3r/AI/ModelGeneration/SemanticColoring/SemanticColoring.hpp"
#include "../ModelGeneration/PortraitPaletteRoles.hpp"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::AI::ColorTrialPersistence {

// Pure saved snapshot of the controls. Source/target pairs retain their order:
// target colors alone cannot reconstruct which original colors they replace.
// Dynamic pack indices remain manual snapshots. Project bindings use physical
// slot identities so equal colors and repeated mapped targets stay distinct.
struct State {
    std::vector<GUI::PreviewPalette::Color> colors, mapping_colors;
    std::array<bool, GUI::PreviewPalette::max_preview_colors> locks {};
    int source {0}, count {6};
    bool enabled {false}, fidelity {true}, lighting {false};
    bool semantic_optimization {true};
    std::vector<GUI::PreviewPalette::Color> semantic_palette, semantic_mapping_palette, semantic_portrait_card;
    std::vector<std::string> semantic_role_uids;
    SemanticColoring::SemanticRegionSlotBindings semantic_region_slots = SemanticColoring::default_semantic_region_slot_bindings;
    std::vector<size_t> project_color_slots, project_semantic_slots;
    std::string project_slot_identity;
    // Counts are kept per source.  The printer source may have a different
    // number of physical/mixed slots than the automatic or manual palette.
    int automatic_count {6}, manual_count {6}, project_count {0};
    // Optional native automatic-basis/color-card remap.  Old v1/v2 records
    // omit these fields and continue through the legacy paired-palette path.
    std::string automatic_basis_signature;
    std::vector<GUI::PreviewPalette::Color> color_card_target_palette;
    std::vector<size_t> color_card_target_slot_ids;
    std::vector<size_t> source_to_target_slot;
    double color_card_weighted_error {0.0};
    // Honor suites persist as a manual snapshot for old readers. New readers
    // use these optional stable identifiers to restore the suite UI without
    // changing the historical numeric source contract.
    std::string suite_id;
    std::string suite_revision;
};

// Saved color-card entries use the persisted manual source because dynamic
// UI source indices are not stable across installations. Keep the per-source
// count aligned with the exact palette being saved so restoring the snapshot
// cannot replace an N-color card with an older manual-count value.
inline void normalize_as_manual_snapshot(State& state)
{
    state.source = 2;
    state.manual_count = std::clamp(state.count, 1, int(GUI::PreviewPalette::max_preview_colors));
    state.project_color_slots.clear();
    state.project_semantic_slots.clear();
    state.project_slot_identity.clear();
}

inline void normalize_for_persistence(State& state)
{
    if (state.source != 1 || state.project_slot_identity.empty())
        normalize_as_manual_snapshot(state);
}

namespace detail {
inline bool valid_fingerprint(const std::string& value)
{
    if (value.size() != 64) return false;
    for (char c : value) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

inline bool valid_basis_signature(const std::string& value)
{
    if (value.empty()) return true;
    if (value.size() != 16 && value.size() != 64) return false;
    return std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

inline bool valid_suite_token(const std::string& value)
{
    return !value.empty() && value.size() <= 128 &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '.' || c == '-' || c == '_';
        });
}

inline bool valid_state(const State& state)
{
    const bool has_suite_id = !state.suite_id.empty();
    const bool has_suite_revision = !state.suite_revision.empty();
    if (has_suite_id != has_suite_revision ||
        (has_suite_id && (state.source != 2 || !valid_suite_token(state.suite_id) ||
                          !valid_suite_token(state.suite_revision))))
        return false;
    if (!valid_basis_signature(state.automatic_basis_signature) ||
        !std::isfinite(state.color_card_weighted_error) || state.color_card_weighted_error < 0.0)
        return false;
    const bool has_card = !state.color_card_target_palette.empty() ||
        !state.color_card_target_slot_ids.empty() || !state.source_to_target_slot.empty();
    if (has_card) {
        if (state.automatic_basis_signature.empty() ||
            state.color_card_target_palette.size() != state.color_card_target_slot_ids.size() ||
            state.colors.size() != state.mapping_colors.size() ||
            state.source_to_target_slot.size() != state.mapping_colors.size() ||
            state.mapping_colors.size() > state.color_card_target_palette.size()) return false;
        std::vector<uint8_t> used(state.color_card_target_palette.size(), 0);
        std::set<size_t> slot_ids;
        for (size_t id : state.color_card_target_slot_ids)
            if (id > 65535 || !slot_ids.insert(id).second) return false;
        for (size_t i = 0; i < state.source_to_target_slot.size(); ++i) {
            const size_t slot = state.source_to_target_slot[i];
            if (slot >= used.size() || used[slot] || state.colors[i] != state.color_card_target_palette[slot]) return false;
            used[slot] = 1;
        }
    } else if (state.color_card_weighted_error != 0.0) return false;
    if (!state.project_slot_identity.empty() || !state.project_color_slots.empty() || !state.project_semantic_slots.empty()) {
        if (state.source != 1 || state.project_slot_identity.empty() || state.project_slot_identity.size() > 65536 ||
            state.project_color_slots.size() != state.colors.size() ||
            state.project_semantic_slots.size() != state.semantic_palette.size()) return false;
        for (const auto* slots : {&state.project_color_slots, &state.project_semantic_slots})
            for (size_t slot : *slots) if (slot > 65535) return false;
    }
    if(!GUI::valid_portrait_roles(state.semantic_role_uids,state.colors.size())) return false;
    if (state.semantic_palette.size() > 6 ||
        (!state.semantic_mapping_palette.empty() && state.semantic_mapping_palette.size() != state.semantic_palette.size()) ||
        (!state.semantic_portrait_card.empty() && state.semantic_portrait_card.size() != 6)) return false;
    if (state.source < 0 || state.source > 2 || state.count < 1 ||
        state.count > int(GUI::PreviewPalette::max_preview_colors) ||
        state.automatic_count < 1 || state.automatic_count > int(GUI::PreviewPalette::max_preview_colors) ||
        state.manual_count < 1 || state.manual_count > int(GUI::PreviewPalette::max_preview_colors) ||
        state.project_count < 0 || state.project_count > int(GUI::PreviewPalette::max_preview_colors) ||
        state.colors.size() > size_t(state.count) || state.colors.size() != state.mapping_colors.size() ||
        (state.enabled && state.colors.empty())) return false;
    for (const int slot : state.semantic_region_slots)
        if (slot < -1 || slot >= int(GUI::PreviewPalette::max_preview_colors) ||
            (slot >= 0 && size_t(slot) >= state.colors.size())) return false;
    for (const auto* palette : {&state.colors, &state.mapping_colors, &state.semantic_palette, &state.semantic_mapping_palette,
                                &state.semantic_portrait_card, &state.color_card_target_palette})
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

inline bool read_palette(const nlohmann::json& value, std::vector<GUI::PreviewPalette::Color>& palette,
                         size_t maximum = GUI::PreviewPalette::max_preview_colors)
{
    if (!value.is_array() || value.size() > maximum) return false;
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

inline bool read_region_slots(const nlohmann::json& value,
                              SemanticColoring::SemanticRegionSlotBindings& slots)
{
    if (!value.is_object()) return false;
    const std::array<const char*, SemanticColoring::semantic_region_slot_count> names {{
        "eye_sclera", "iris", "eyebrow", "lips"
    }};
    SemanticColoring::SemanticRegionSlotBindings parsed = SemanticColoring::default_semantic_region_slot_bindings;
    for (size_t i = 0; i < names.size(); ++i) {
        if (!value.contains(names[i])) return false;
        const auto& entry = value[names[i]];
        if (!entry.is_number_integer()) return false;
        const int64_t slot = entry.get<int64_t>();
        if (slot < -1 || slot >= int64_t(GUI::PreviewPalette::max_preview_colors)) return false;
        parsed[i] = int(slot);
    }
    slots = parsed;
    return true;
}
} // namespace detail

inline nlohmann::json encode(const State& state, size_t actual_face_count, const std::string& fingerprint)
{
    if (!detail::valid_fingerprint(fingerprint)) throw std::invalid_argument("Invalid trial color geometry fingerprint.");
    if (!detail::valid_state(state)) throw std::invalid_argument("Invalid trial color state.");
    nlohmann::json doc {{"schema", "orca.color-trial/v2"}, {"geometry_sha256", fingerprint}, {"face_count", actual_face_count},
            {"colors", state.colors}, {"mapping_colors", state.mapping_colors}, {"locks", state.locks},
            {"source", state.source}, {"count", state.count}, {"enabled", state.enabled},
            {"automatic_count", state.automatic_count}, {"manual_count", state.manual_count},
            {"project_count", state.project_count},
            {"fidelity", state.fidelity}, {"lighting", state.lighting},
            {"semantic_optimization", state.semantic_optimization},
            {"semantic_palette", state.semantic_palette}, {"semantic_mapping_palette", state.semantic_mapping_palette},
            {"semantic_portrait_card", state.semantic_portrait_card},
            {"semantic_role_uids", state.semantic_role_uids},
            {"semantic_region_slots", {
                {"eye_sclera", state.semantic_region_slots[0]},
                {"iris", state.semantic_region_slots[1]},
                {"eyebrow", state.semantic_region_slots[2]},
                {"lips", state.semantic_region_slots[3]}
            }}};
    if (!state.project_slot_identity.empty()) {
        doc["project_slot_identity"] = state.project_slot_identity;
        doc["project_color_slots"] = state.project_color_slots;
        doc["project_semantic_slots"] = state.project_semantic_slots;
    }
    if (!state.automatic_basis_signature.empty()) doc["automatic_basis_signature"] = state.automatic_basis_signature;
    if (!state.color_card_target_palette.empty()) {
        doc["color_card_target_palette"] = state.color_card_target_palette;
        doc["color_card_target_slot_ids"] = state.color_card_target_slot_ids;
        doc["source_to_target_slot"] = state.source_to_target_slot;
        doc["color_card_weighted_error"] = state.color_card_weighted_error;
    }
    if (!state.suite_id.empty()) {
        doc["suite_id"] = state.suite_id;
        doc["suite_revision"] = state.suite_revision;
    }
    return doc;
}

// Reject mismatched geometry or malformed fields before returning any state.
// No palette is re-derived, reordered, or fitted to edited vertex colors.
// A disabled snapshot still retains its paired centers for later toggling.
inline bool decode(const nlohmann::json& doc, size_t actual_face_count, const std::string& fingerprint,
                   State& output, std::string& error)
{
    error.clear();
    auto fail = [&](const char* message) { error = message; return false; };
    if (!doc.is_object() || !doc.contains("schema") ||
        (doc["schema"] != "orca.color-trial/v1" && doc["schema"] != "orca.color-trial/v2"))
        return fail("Unsupported trial color schema.");
    if (!detail::valid_fingerprint(fingerprint) || !doc.contains("geometry_sha256") ||
        !doc["geometry_sha256"].is_string() || doc["geometry_sha256"].get_ref<const std::string&>() != fingerprint)
        return fail("The saved trial colors belong to different geometry.");
    uint64_t face_count, source, count;
    if (!doc.contains("face_count") || !detail::read_unsigned(doc["face_count"], face_count) || face_count != actual_face_count)
        return fail("The saved trial color face count does not match the loaded mesh.");
    if (!doc.contains("source") || !detail::read_unsigned(doc["source"], source) || source > 2 ||
        !doc.contains("count") || !detail::read_unsigned(doc["count"], count) || count < 1 ||
        count > GUI::PreviewPalette::max_preview_colors)
        return fail("Invalid trial color source or color count.");
    for (const char* field : {"enabled", "fidelity", "lighting"})
        if (!doc.contains(field) || !doc[field].is_boolean()) return fail("Trial color options must be explicit booleans.");
    if (!doc.contains("locks") || !doc["locks"].is_array() ||
        (doc["locks"].size() != 6 && doc["locks"].size() != GUI::PreviewPalette::max_preview_colors))
        return fail("Trial color lock flags do not match a supported preview version.");
    State restored;
    for (size_t i = 0; i < doc["locks"].size(); ++i) {
        if (!doc["locks"][i].is_boolean()) return fail("Trial color locks must be booleans.");
        restored.locks[i] = doc["locks"][i].get<bool>();
    }
    if (!doc.contains("colors") || !doc.contains("mapping_colors") ||
        !detail::read_palette(doc["colors"], restored.colors) ||
        !detail::read_palette(doc["mapping_colors"], restored.mapping_colors))
        return fail("Trial colors require normalized RGB palettes within the preview limit.");
    restored.source = int(source);
    restored.count = int(count);
    const bool has_suite_id = doc.contains("suite_id");
    const bool has_suite_revision = doc.contains("suite_revision");
    if (has_suite_id != has_suite_revision)
        return fail("Honor suite identity must include both id and revision.");
    if (has_suite_id) {
        if (!doc["suite_id"].is_string() || !doc["suite_revision"].is_string())
            return fail("Invalid Honor suite identity.");
        restored.suite_id = doc["suite_id"].get<std::string>();
        restored.suite_revision = doc["suite_revision"].get<std::string>();
    }
    auto read_count = [&](const char* name, int fallback, int& target) {
        if (!doc.contains(name)) { target = fallback; return true; }
        uint64_t value;
        if (!detail::read_unsigned(doc[name], value) || value < 1 || value > GUI::PreviewPalette::max_preview_colors)
            return false;
        target = int(value); return true;
    };
    if (!read_count("automatic_count", restored.source == 0 ? restored.count : 6, restored.automatic_count) ||
        !read_count("manual_count", restored.source == 2 ? restored.count : 6, restored.manual_count))
        return fail("Invalid source color counts.");
    if (doc.contains("project_count")) {
        uint64_t value;
        if (!detail::read_unsigned(doc["project_count"], value) || value > GUI::PreviewPalette::max_preview_colors)
            return fail("Invalid printer color count.");
        restored.project_count = int(value);
    } else restored.project_count = restored.source == 1 ? restored.count : 0;
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
    if (doc.contains("semantic_palette") && !detail::read_palette(doc["semantic_palette"], restored.semantic_palette, 6))
        return fail("Invalid semantic palette.");
    if (doc.contains("semantic_mapping_palette") && !detail::read_palette(doc["semantic_mapping_palette"], restored.semantic_mapping_palette, 6))
        return fail("Invalid semantic candidate assignments.");
    if (doc.contains("semantic_portrait_card") && !detail::read_palette(doc["semantic_portrait_card"], restored.semantic_portrait_card, 6))
        return fail("Invalid semantic portrait card.");
    if (!restored.semantic_portrait_card.empty() && restored.semantic_portrait_card.size() != 6)
        return fail("A semantic portrait card requires six roles.");
    if(doc.contains("semantic_role_uids")) {
        if(!doc["semantic_role_uids"].is_array() || doc["semantic_role_uids"].size()>6) return fail("Invalid portrait role bindings.");
        for(const auto& role:doc["semantic_role_uids"]) {
            if(!role.is_string()) return fail("Invalid portrait role binding.");
            restored.semantic_role_uids.push_back(role.get<std::string>());
        }
    } else restored.semantic_role_uids=GUI::portrait_roles_for_card(
        restored.semantic_palette.empty()?restored.colors:restored.semantic_palette,restored.semantic_portrait_card);
    if (doc.contains("semantic_region_slots") &&
        !detail::read_region_slots(doc["semantic_region_slots"], restored.semantic_region_slots))
        return fail("Invalid semantic region slot bindings.");
    if (doc.contains("project_slot_identity") || doc.contains("project_color_slots") || doc.contains("project_semantic_slots")) {
        if (!doc.contains("project_slot_identity") || !doc["project_slot_identity"].is_string())
            return fail("Invalid project slot identity.");
        restored.project_slot_identity = doc["project_slot_identity"].get<std::string>();
        if (restored.project_slot_identity.empty()) return fail("Missing project slot identity.");
        for (const auto& item : {std::make_pair("project_color_slots", &restored.project_color_slots),
                                 std::make_pair("project_semantic_slots", &restored.project_semantic_slots)}) {
            if (!doc.contains(item.first) || !doc[item.first].is_array() ||
                doc[item.first].size() > GUI::PreviewPalette::max_preview_colors) return fail("Invalid project slot bindings.");
            for (const auto& entry : doc[item.first]) {
                uint64_t slot;
                if (!detail::read_unsigned(entry, slot) || slot > 65535) return fail("Invalid physical project slot.");
                item.second->push_back(size_t(slot));
            }
        }
    }
    if (doc.contains("automatic_basis_signature")) {
        if (!doc["automatic_basis_signature"].is_string()) return fail("Invalid automatic color basis signature.");
        restored.automatic_basis_signature = doc["automatic_basis_signature"].get<std::string>();
        if (!detail::valid_basis_signature(restored.automatic_basis_signature))
            return fail("Invalid automatic color basis signature.");
    }
    const bool has_card = doc.contains("color_card_target_palette") ||
        doc.contains("color_card_target_slot_ids") || doc.contains("source_to_target_slot") ||
        doc.contains("color_card_weighted_error");
    if (has_card) {
        if (!doc.contains("color_card_target_palette") ||
            !detail::read_palette(doc["color_card_target_palette"], restored.color_card_target_palette) ||
            !doc.contains("color_card_target_slot_ids") || !doc["color_card_target_slot_ids"].is_array() ||
            !doc.contains("source_to_target_slot") || !doc["source_to_target_slot"].is_array() ||
            !doc.contains("color_card_weighted_error") || !doc["color_card_weighted_error"].is_number())
            return fail("Incomplete color-card remap.");
        for (const auto& list : {std::make_pair(&doc["color_card_target_slot_ids"], &restored.color_card_target_slot_ids),
                                 std::make_pair(&doc["source_to_target_slot"], &restored.source_to_target_slot)}) {
            if (list.first->size() > GUI::PreviewPalette::max_preview_colors)
                return fail("Color-card remap exceeds the preview limit.");
            for (const auto& entry : *list.first) {
                uint64_t value;
                if (!detail::read_unsigned(entry, value) || value > 65535)
                    return fail("Invalid color-card slot identity.");
                list.second->push_back(size_t(value));
            }
        }
        restored.color_card_weighted_error = doc["color_card_weighted_error"].get<double>();
    }
    if (!detail::valid_state(restored)) return fail("Trial color centers and targets must form valid pairs.");
    output = std::move(restored);
    return true;
}

namespace detail {

struct ProjectSlotIdentityEntry {
    size_t slot {0};
    bool mixed {false};
    std::string descriptor;
};

struct ProjectSlotIdentity {
    std::string printer;
    std::string nozzles;
    std::vector<ProjectSlotIdentityEntry> slots;
};

inline std::optional<ProjectSlotIdentity> decode_project_slot_identity(const std::string& value)
{
    const auto document = nlohmann::json::parse(value, nullptr, false);
    if (document.is_discarded() || !document.is_object() ||
        !document.contains("printer") || !document["printer"].is_string() ||
        !document.contains("nozzles") || !document["nozzles"].is_string() ||
        !document.contains("slots") || !document["slots"].is_array()) return {};

    ProjectSlotIdentity result;
    result.printer = document["printer"].get<std::string>();
    result.nozzles = document["nozzles"].get<std::string>();
    std::set<size_t> seen;
    for (const auto& item : document["slots"]) {
        if (!item.is_array() || item.size() < 2 ||
            !(item[0].is_number_unsigned() || item[0].is_number_integer()) || !item[1].is_boolean()) return {};
        const auto signed_slot = item[0].get<std::int64_t>();
        if (signed_slot < 0 || signed_slot > 65535 || !seen.insert(size_t(signed_slot)).second) return {};
        nlohmann::json descriptor = item;
        for (const char* key : {"loaded", "recipes"}) {
            if (!document.contains(key) || !document[key].is_array()) continue;
            for (const auto& candidate : document[key]) {
                if (!candidate.is_array() || candidate.empty() ||
                    !(candidate[0].is_number_unsigned() || candidate[0].is_number_integer())) continue;
                if (candidate[0].get<std::int64_t>() == signed_slot) descriptor.push_back(candidate);
            }
        }
        result.slots.push_back({size_t(signed_slot), item[1].get<bool>(), descriptor.dump()});
    }
    return result;
}

inline bool project_slot_identity_rebindable(const std::string& before, const std::string& after,
    const std::vector<size_t>& bindings)
{
    if (before == after) return true;
    const auto previous = decode_project_slot_identity(before);
    const auto current = decode_project_slot_identity(after);
    if (!previous || !current || previous->printer != current->printer || previous->nozzles != current->nozzles)
        return false;

    size_t stable_prefix = 0;
    if (previous->slots.size() == current->slots.size()) {
        while (stable_prefix < previous->slots.size() &&
               previous->slots[stable_prefix].slot == current->slots[stable_prefix].slot &&
               previous->slots[stable_prefix].mixed == current->slots[stable_prefix].mixed)
            ++stable_prefix;
    } else {
        const size_t shared = std::min(previous->slots.size(), current->slots.size());
        while (stable_prefix < shared &&
               previous->slots[stable_prefix].descriptor == current->slots[stable_prefix].descriptor)
            ++stable_prefix;
    }
    for (const size_t binding : bindings) {
        const auto found = std::find_if(previous->slots.begin(), previous->slots.end(),
            [binding](const auto& item) { return item.slot == binding; });
        if (found == previous->slots.end() || size_t(found - previous->slots.begin()) >= stable_prefix ||
            size_t(found - previous->slots.begin()) >= current->slots.size() ||
            current->slots[size_t(found - previous->slots.begin())].slot != binding ||
            current->slots[size_t(found - previous->slots.begin())].mixed != found->mixed) return false;
    }
    return true;
}

} // namespace detail

inline bool synchronize_project_targets(State& state, const std::vector<size_t>& slots,
    const std::vector<GUI::PreviewPalette::Color>& colors, const std::string& identity)
{
    if (state.source != 1) return true;
    if (!detail::valid_state(state) || state.project_slot_identity.empty() ||
        identity.empty() || slots.size() != colors.size()) return false;
    std::vector<size_t> bindings = state.project_color_slots;
    bindings.insert(bindings.end(), state.project_semantic_slots.begin(), state.project_semantic_slots.end());
    if (!detail::project_slot_identity_rebindable(state.project_slot_identity, identity, bindings)) return false;
    auto updated = state;
    for (size_t i = 0; i < updated.color_card_target_slot_ids.size(); ++i) {
        const auto found = std::find(slots.begin(), slots.end(), updated.color_card_target_slot_ids[i]);
        if (found == slots.end()) return false;
        updated.color_card_target_palette[i] = colors[size_t(found - slots.begin())];
    }
    for (const auto& item : {std::make_pair(&updated.colors, &state.project_color_slots),
                             std::make_pair(&updated.semantic_palette, &state.project_semantic_slots)}) {
        for (size_t i = 0; i < item.second->size(); ++i) {
            const auto found = std::find(slots.begin(), slots.end(), (*item.second)[i]);
            if (found == slots.end()) return false;
            (*item.first)[i] = colors[size_t(found - slots.begin())];
        }
    }
    updated.project_slot_identity = identity;
    if (!detail::valid_state(updated)) return false;
    state = std::move(updated);
    return true;
}

inline std::optional<GUI::PreviewPalette::Color> project_target_replacement(const State& before,
    const State& after, const GUI::PreviewPalette::Color& color)
{
    if (before.source != 1 || after.source != 1 || before.project_slot_identity.empty() ||
        before.project_slot_identity != after.project_slot_identity ||
        before.project_semantic_slots != after.project_semantic_slots ||
        before.semantic_palette.size() != after.semantic_palette.size()) return {};
    std::optional<GUI::PreviewPalette::Color> replacement;
    for (size_t i = 0; i < before.semantic_palette.size(); ++i) {
        if (before.semantic_palette[i] != color) continue;
        if (replacement && *replacement != after.semantic_palette[i]) return {};
        replacement = after.semantic_palette[i];
    }
    return replacement;
}
} // namespace Slic3r::AI::ColorTrialPersistence
