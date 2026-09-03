#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r::AI {

inline constexpr size_t kMinPhysicalColorChannels = 1;
inline constexpr size_t kMaxPhysicalColorChannels = 6;
inline constexpr size_t kMaxMixedColorComponents  = 3;
inline constexpr size_t kMinTargetPaletteColors = 1;
inline constexpr size_t kMaxTargetPaletteColors = 6;
inline constexpr size_t kLegacyDefaultTargetPaletteColors = 4;
inline constexpr const char* kColorIntentSchemaV1 = "orcaslicer.color-intent.v1";
inline constexpr std::array<const char*, kMaxTargetPaletteColors> kPaletteRoleIds {
    "primary", "structure", "light", "accent", "secondary", "detail"
};

enum class ColorOutputMode
{
    DiscreteFilament,
    ProcessMix,
};

// A zero-based Orca project slot backed by one physical filament input. Virtual
// mixed-filament slots are represented by MixedColorRecipe instead.
struct PhysicalFilamentChannel
{
    size_t      slot { 0 };
    std::string display_color;
    std::string material_type;
    bool        compatible { false };
};

struct MixedColorComponent
{
    size_t slot { 0 };
    double ratio { 0.0 };
};

struct MixedColorRecipe
{
    std::string                      target_color;
    std::vector<MixedColorComponent> components;
    std::optional<size_t>            existing_virtual_slot;
};

struct ColorIntentManifestRef
{
    std::string local_path;
    std::string schema;
    std::string sha256;
};

inline bool is_lowercase_sha256(const std::string& value) noexcept
{
    if (value.size() != 64)
        return false;
    for (const char ch : value)
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
            return false;
    return true;
}

inline bool is_valid_color_intent_manifest_ref(const ColorIntentManifestRef& reference) noexcept
{
    return !reference.local_path.empty() && reference.schema == kColorIntentSchemaV1 &&
           is_lowercase_sha256(reference.sha256);
}

constexpr bool is_supported_physical_channel_count(size_t count) noexcept
{
    return count >= kMinPhysicalColorChannels && count <= kMaxPhysicalColorChannels;
}

constexpr bool is_supported_target_palette_color_count(size_t count) noexcept
{
    return count >= kMinTargetPaletteColors && count <= kMaxTargetPaletteColors;
}

inline bool is_active_palette_role(const std::string& role, size_t color_count) noexcept
{
    if (!is_supported_target_palette_color_count(color_count))
        return false;
    for (size_t index = 0; index < color_count; ++index) {
        if (role == kPaletteRoleIds[index])
            return true;
    }
    return false;
}

inline bool is_rgb_hex_color(const std::string& color) noexcept
{
    if (color.size() != 7 || color.front() != '#')
        return false;
    for (size_t index = 1; index < color.size(); ++index) {
        const char ch = color[index];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F') || (ch >= 'a' && ch <= 'f')))
            return false;
    }
    return true;
}

inline bool is_valid_physical_channel_set(const std::vector<PhysicalFilamentChannel>& channels) noexcept
{
    if (!is_supported_physical_channel_count(channels.size()))
        return false;
    for (size_t index = 0; index < channels.size(); ++index) {
        if (!is_rgb_hex_color(channels[index].display_color))
            return false;
        for (size_t other = index + 1; other < channels.size(); ++other) {
            if (channels[index].slot == channels[other].slot)
                return false;
        }
    }
    return true;
}

inline bool has_valid_mixed_components(const MixedColorRecipe& recipe, double tolerance = 1e-6) noexcept
{
    if (recipe.components.empty() || recipe.components.size() > kMaxMixedColorComponents ||
        !std::isfinite(tolerance) || tolerance < 0.0)
        return false;

    double total = 0.0;
    for (size_t index = 0; index < recipe.components.size(); ++index) {
        const MixedColorComponent& component = recipe.components[index];
        if (!std::isfinite(component.ratio) || component.ratio <= 0.0)
            return false;
        for (size_t other = index + 1; other < recipe.components.size(); ++other) {
            if (component.slot == recipe.components[other].slot)
                return false;
        }
        total += component.ratio;
    }
    return std::isfinite(total) && std::abs(total - 1.0) <= tolerance;
}

inline bool is_valid_mixed_color_recipe(const MixedColorRecipe& recipe, double tolerance = 1e-6) noexcept
{
    return is_rgb_hex_color(recipe.target_color) && has_valid_mixed_components(recipe, tolerance);
}

} // namespace Slic3r::AI
