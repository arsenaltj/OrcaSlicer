#pragma once

#include "slic3r/AI/Contracts/GeneratedModelArtifact.hpp"
#include "slic3r/AI/Contracts/ColorIntent.hpp"

#include <cstddef>
#include <cstdint>
#include <array>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include <set>
#include <tuple>

namespace Slic3r::AI {

enum class ImportColorMode
{
    ManualMatch,
    AutoMap,
    SingleColor,
    NativeMatch
};

// An explicit editing choice for this import, separate from the immutable
// generated artifact and provider color-intent manifest. Normalized sRGB.
struct ModelColorTrial
{
    std::vector<std::array<float, 3>> mapping_colors;
    std::vector<std::array<float, 3>> target_colors;

    bool valid() const
    {
        if (mapping_colors.empty() || mapping_colors.size() > 6 ||
            mapping_colors.size() != target_colors.size()) return false;
        for (const auto* palette : {&mapping_colors, &target_colors})
            for (const auto& color : *palette)
                for (float channel : color)
                    if (!std::isfinite(channel) || channel < 0.f || channel > 1.f) return false;
        return true;
    }
};

struct ModelSubfaceColorOverride
{
    size_t face_id {0};
    uint8_t depth {0};
    uint8_t path {0};
    std::array<float, 3> color {};
};

// Explicit per-face native assignments from an accepted workbench version.
// RGB cannot identify a slot when two loaded materials have the same color.
struct ModelSubfaceSlot {
    size_t face_id {0};
    uint8_t depth {0}, path {0};
    size_t slot {0};
};
struct ModelMatchedColors {
    std::string source_sha256, geometry_id;
    std::vector<PhysicalFilamentChannel> palette;
    std::vector<size_t> face_slots;
    // Optional midpoint leaves on the same immutable source geometry. Slot
    // identity remains authoritative even when different slots share RGB.
    std::vector<ModelSubfaceSlot> subface_slots;
    std::vector<MixedColorRecipe> mixed_recipes;
    // Optional native configuration fingerprints. Empty values preserve older
    // beauty-puzzle records; new workbench snapshots populate both.
    std::string material_fingerprint, process_fingerprint;
    bool valid() const {
        if (!is_lowercase_sha256(source_sha256) || !is_lowercase_sha256(geometry_id) ||
            face_slots.empty() || face_slots.size() > 2000000 ||
            !is_valid_physical_channel_set(palette) ||
            !valid_native_mixed_palette(palette, mixed_recipes)) return false;
        for (size_t slot : face_slots)
            if (!native_palette_has_slot(palette, mixed_recipes, slot)) return false;
        if (subface_slots.size() > 2000000) return false;
        std::set<std::tuple<size_t, uint8_t, uint8_t>> keys;
        for (const auto& leaf : subface_slots) {
            if (leaf.face_id >= face_slots.size() || leaf.depth == 0 || leaf.depth > 4 ||
                unsigned(leaf.path) >= (1u << (2u * leaf.depth)) ||
                !native_palette_has_slot(palette, mixed_recipes, leaf.slot) ||
                !keys.emplace(leaf.face_id, leaf.depth, leaf.path).second) return false;
        }
        return true;
    }
};

// Internal persistence for an accepted workbench printer-filament match.
// This is deliberately optional metadata; older model records simply omit it.
inline nlohmann::json encode_model_matched_colors(const ModelMatchedColors& value)
{
    if (!value.valid()) throw std::invalid_argument("Invalid matched printer-filament snapshot.");
    nlohmann::json palette = nlohmann::json::array();
    for (const auto& channel : value.palette)
        palette.push_back({{"slot", channel.slot}, {"color", channel.display_color},
                           {"material", channel.material_type}, {"compatible", channel.compatible}});
    nlohmann::json recipes = nlohmann::json::array();
    for (const auto& recipe : value.mixed_recipes) {
        nlohmann::json components = nlohmann::json::array();
        for (const auto& component : recipe.components)
            components.push_back({component.slot, component.ratio});
        recipes.push_back({{"slot", recipe.existing_virtual_slot ? *recipe.existing_virtual_slot : 255},
                           {"color", recipe.target_color}, {"components", components},
                           {"settings", recipe.native_settings_fingerprint}, {"uniform", recipe.uniform_color}});
    }
    nlohmann::json leaves = nlohmann::json::array();
    for (const auto& leaf : value.subface_slots)
        leaves.push_back({leaf.face_id, leaf.depth, leaf.path, leaf.slot});
    return {{"schema", "orca.applied-printer-filament-match/v1"},
            {"source_sha256", value.source_sha256}, {"geometry_id", value.geometry_id},
            {"palette", palette}, {"mixed_recipes", recipes}, {"face_slots", value.face_slots}, {"subface_slots", leaves},
            {"material_fingerprint", value.material_fingerprint}, {"process_fingerprint", value.process_fingerprint}};
}

inline bool decode_model_matched_colors(const nlohmann::json& doc, ModelMatchedColors& output,
                                       std::string& error)
{
    error.clear();
    auto fail = [&](const char* message) { error = message; return false; };
    if (!doc.is_object() || doc.value("schema", std::string{}) != "orca.applied-printer-filament-match/v1")
        return fail("Unsupported applied printer-filament snapshot.");
    if (!doc.contains("source_sha256") || !doc["source_sha256"].is_string() ||
        !doc.contains("geometry_id") || !doc["geometry_id"].is_string() ||
        !doc.contains("palette") || !doc["palette"].is_array() ||
        !doc.contains("mixed_recipes") || !doc["mixed_recipes"].is_array() ||
        !doc.contains("face_slots") || !doc["face_slots"].is_array())
        return fail("Incomplete applied printer-filament snapshot.");
    ModelMatchedColors parsed;
    parsed.source_sha256 = doc["source_sha256"].get<std::string>();
    parsed.geometry_id = doc["geometry_id"].get<std::string>();
    if (doc.contains("material_fingerprint") && !doc["material_fingerprint"].is_string())
        return fail("Invalid material fingerprint in applied snapshot.");
    if (doc.contains("process_fingerprint") && !doc["process_fingerprint"].is_string())
        return fail("Invalid process fingerprint in applied snapshot.");
    parsed.material_fingerprint = doc.value("material_fingerprint", std::string{});
    parsed.process_fingerprint = doc.value("process_fingerprint", std::string{});
    for (const auto& item : doc["palette"]) {
        if (!item.is_object() || !item.contains("slot") || !item.contains("color") ||
            !item.contains("material") || !item.contains("compatible") || !item["slot"].is_number_unsigned() ||
            !item["color"].is_string() || !item["material"].is_string() || !item["compatible"].is_boolean())
            return fail("Invalid physical channel in applied snapshot.");
        parsed.palette.push_back({item["slot"].get<size_t>(), item["color"].get<std::string>(),
                                  item["material"].get<std::string>(), item["compatible"].get<bool>()});
    }
    for (const auto& item : doc["mixed_recipes"]) {
        if (!item.is_object() || !item.contains("slot") || !item.contains("color") ||
            !item.contains("components") || !item["slot"].is_number_unsigned() ||
            !item["color"].is_string() || !item["components"].is_array())
            return fail("Invalid mixed recipe in applied snapshot.");
        const size_t slot = item["slot"].get<size_t>();
        if (slot >= 255) return fail("Invalid mixed recipe slot.");
        MixedColorRecipe recipe;
        recipe.existing_virtual_slot = slot;
        recipe.target_color = item["color"].get<std::string>();
        if (item.contains("settings") && !item["settings"].is_string()) return fail("Invalid mixed recipe settings.");
        if (item.contains("settings")) recipe.native_settings_fingerprint = item["settings"].get<std::string>();
        if (item.contains("uniform") && !item["uniform"].is_boolean()) return fail("Invalid mixed recipe state.");
        if (item.contains("uniform")) recipe.uniform_color = item["uniform"].get<bool>();
        for (const auto& component : item["components"]) {
            if (!component.is_array() || component.size() != 2 || !component[0].is_number_unsigned() ||
                !component[1].is_number()) return fail("Invalid mixed recipe component.");
            recipe.components.push_back({component[0].get<size_t>(), component[1].get<double>()});
        }
        parsed.mixed_recipes.push_back(std::move(recipe));
    }
    for (const auto& item : doc["face_slots"])
        if (!item.is_number_unsigned()) return fail("Invalid face slot in applied snapshot.");
        else parsed.face_slots.push_back(item.get<size_t>());
    if (doc.contains("subface_slots")) {
        const auto& leaves = doc["subface_slots"];
        if (!leaves.is_array() || leaves.size() > 2000000) return fail("Invalid applied subface slots.");
        for (const auto& leaf : leaves) {
            if (!leaf.is_array() || leaf.size() != 4 ||
                !std::all_of(leaf.begin(), leaf.end(), [](const auto& v) { return v.is_number_unsigned(); }) ||
                leaf[1].get<size_t>() > 4 || leaf[2].get<size_t>() > 255)
                return fail("Invalid applied subface identity.");
            parsed.subface_slots.push_back({leaf[0].get<size_t>(), leaf[1].get<uint8_t>(),
                leaf[2].get<uint8_t>(), leaf[3].get<size_t>()});
        }
    }
    if (!parsed.valid()) return fail("Applied printer-filament snapshot failed validation.");
    output = std::move(parsed);
    return true;
}

struct ModelImportRequest
{
    GeneratedModelArtifact artifact;
    ImportColorMode         color_mode { ImportColorMode::NativeMatch };
    std::optional<ModelColorTrial> color_trial;
    // Source triangle ordinals and explicit normalized sRGB targets. The desktop
    // adapter validates the geometry identity before forwarding native matching.
    // Later entries replace earlier entries for the same face.
    std::vector<std::pair<size_t, std::array<float, 3>>> face_color_overrides;
    // Sparse midpoint leaves on the unchanged source topology. Unspecified
    // leaves inherit the corresponding whole-face assignment above.
    std::vector<ModelSubfaceColorOverride> subface_color_overrides;
    std::string face_color_geometry_id;
    std::optional<ModelMatchedColors> matched_colors;
    // Only the live, validated printer-filament source may reuse the accepted
    // on-disk face-to-slot snapshot. Other color sources must rematch.
    bool use_saved_printer_match {false};
    // Desktop-only optional caches, archived through existing 3MF auxiliaries.
    std::vector<nlohmann::json> portrait_color_block_references;
    // Desktop preparation intent only. Never serialized into a provider job or
    // applied to geometry by importing; the user must confirm in preparation.
    bool suggest_base_preparation {false};
    // Explicit exception path for an unchecked/open model. This metadata is
    // retained by the native preparation flow and never means slicing passed.
    bool topology_risk_confirmed {false};
    std::string topology_risk_reason;
    std::string topology_check_sha256;
};

enum class ModelImportOutcome
{
    Imported,
    InvalidArtifact,
    ImportFailed,
    RepairFailed,
    Cancelled
};

struct ModelImportResult
{
    ModelImportOutcome outcome { ModelImportOutcome::ImportFailed };
    ImportColorMode    color_mode { ImportColorMode::NativeMatch };
    bool               colors_applied { false };
    bool               color_mapping_collapsed { false };
    bool               manual_coloring_required { false };
    bool               manual_repair_required { false };
    bool               topology_risk { false };
    std::string        topology_risk_reason;
    size_t             source_color_count { 0 };
    size_t             mapped_color_count { 0 };
    size_t             subface_color_count { 0 };
    bool               subface_colors_applied { false };
    std::string        error;

    bool imported() const { return outcome == ModelImportOutcome::Imported; }
};

class IModelArtifactConsumer
{
public:
    virtual ~IModelArtifactConsumer() = default;

    virtual ModelImportResult import_artifact(const ModelImportRequest& request) = 0;
};

} // namespace Slic3r::AI
