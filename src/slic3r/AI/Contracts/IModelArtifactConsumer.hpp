#pragma once

#include "slic3r/AI/Contracts/GeneratedModelArtifact.hpp"
#include "slic3r/AI/Contracts/ColorIntent.hpp"

#include <cstddef>
#include <cstdint>
#include <array>
#include <cmath>
#include <string>
#include <utility>

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
struct ModelMatchedColors {
    std::string source_sha256, geometry_id;
    std::vector<PhysicalFilamentChannel> palette;
    std::vector<size_t> face_slots;
    std::vector<MixedColorRecipe> mixed_recipes;
    bool valid() const {
        if (!is_lowercase_sha256(source_sha256) || !is_lowercase_sha256(geometry_id) ||
            face_slots.empty() || face_slots.size() > 2000000 ||
            !is_valid_physical_channel_set(palette) ||
            !valid_native_mixed_palette(palette, mixed_recipes)) return false;
        for (size_t slot : face_slots)
            if (!native_palette_has_slot(palette, mixed_recipes, slot)) return false;
        return true;
    }
};

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
