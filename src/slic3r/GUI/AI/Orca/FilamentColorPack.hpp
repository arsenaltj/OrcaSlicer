#pragma once

#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <map>
#include <optional>

class wxWindow;
namespace Slic3r { class PresetBundle; }

namespace Slic3r::GUI {

// A physical-slot color card, not a temperature/flow preset or mixing recipe.
struct FilamentColorPack {
    std::string name;
    std::vector<std::string> colors;
    std::vector<std::string> labels;
    // v2 physical cards retain the native preset for each slot. Empty for
    // legacy trial-only cards and for generated suggestion/manual snapshots.
    std::vector<std::string> presets;
    // Optional native physical-spool appearance metadata for saved cards.
    std::vector<std::string> multi_colors, color_types;

    bool valid() const {
        if (name.empty() || colors.empty() || colors.size() > 6 || labels.size() != colors.size() ||
            (!multi_colors.empty() && multi_colors.size() != colors.size()) ||
            (!color_types.empty() && color_types.size() != colors.size())) return false;
        return std::all_of(colors.begin(), colors.end(), [](const std::string& color) {
            return color.size() == 7 && color[0] == '#' &&
                std::all_of(color.begin() + 1, color.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
        });
    }

    // Workbench previews may contain virtual mixed slots in addition to the
    // legacy six physical colors. Keep valid() unchanged for saved/generated
    // card compatibility and use this wider contract only for direct import.
    bool valid_for_workbench_import() const {
        if (name.empty() || colors.empty() || colors.size() > 32 || labels.size() != colors.size() ||
            (!presets.empty() && presets.size() != colors.size()) ||
            (!multi_colors.empty() && multi_colors.size() != colors.size()) ||
            (!color_types.empty() && color_types.size() != colors.size())) return false;
        return std::all_of(colors.begin(), colors.end(), [](const std::string& color) {
            return color.size() == 7 && color[0] == '#' &&
                std::all_of(color.begin() + 1, color.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
        });
    }

    bool valid_physical() const {
        if (name.empty() || colors.empty() || colors.size() > 32 || labels.size() != colors.size() ||
            presets.size() != colors.size() ||
            (!multi_colors.empty() && multi_colors.size() != colors.size()) ||
            (!color_types.empty() && color_types.size() != colors.size())) return false;
        return std::all_of(colors.begin(), colors.end(), [](const std::string& color) {
            return color.size() == 7 && color[0] == '#' &&
                std::all_of(color.begin() + 1, color.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
        }) && std::all_of(presets.begin(), presets.end(), [](const std::string& preset) { return !preset.empty(); });
    }
};

struct HonorPhysicalSlot {
    std::string id;
    std::string name;
    std::string color;
    std::string material_category;
};

struct HonorFilamentSuite {
    std::string id;
    std::string category;
    std::string name;
    std::string revision;
    std::vector<HonorPhysicalSlot> physical_slots;
    // Stable semantic role -> physical slot identity. Multiple roles may
    // intentionally share one physical color (for example hair and iris).
    std::map<std::string, std::string> semantic_role_slots;
    // Legacy suites color with all six slots. New cards reserve the last
    // physical spool for support, even when its RGB duplicates a model spool.
    size_t model_slot_count {6};
    std::string support_slot_id;

    bool semantic_coloring() const { return !semantic_role_slots.empty(); }
    std::optional<size_t> support_slot_index() const {
        for (size_t i = 0; i < physical_slots.size(); ++i)
            if (physical_slots[i].id == support_slot_id) return i;
        return {};
    }

    bool valid() const {
        if (id.empty() || category.empty() || name.empty() || revision.empty() || physical_slots.size() != 6)
            return false;
        std::vector<std::string> identities;
        identities.reserve(physical_slots.size());
        for (const auto& slot : physical_slots) {
            if (slot.id.empty() || slot.name.empty() || slot.material_category.empty() ||
                slot.color.size() != 7 || slot.color.front() != '#' ||
                !std::all_of(slot.color.begin() + 1, slot.color.end(), [](unsigned char c) {
                    return std::isxdigit(c) != 0;
                })) return false;
            identities.push_back(slot.id);
        }
        std::sort(identities.begin(), identities.end());
        if (std::adjacent_find(identities.begin(), identities.end()) != identities.end()) return false;
        if (model_slot_count == 0 || model_slot_count > physical_slots.size() ||
            (!support_slot_id.empty() && (!support_slot_index() || *support_slot_index() < model_slot_count)))
            return false;
        return std::all_of(semantic_role_slots.begin(), semantic_role_slots.end(),
            [&](const auto& binding) {
                const auto found = std::find_if(physical_slots.begin(), physical_slots.begin() + model_slot_count,
                    [&](const auto& slot) { return slot.id == binding.second; });
                return !binding.first.empty() && found != physical_slots.begin() + model_slot_count;
            });
    }

    FilamentColorPack color_pack() const {
        FilamentColorPack pack;
        pack.name = name;
        for (const auto& slot : physical_slots) {
            pack.colors.push_back(slot.color);
            pack.labels.push_back(slot.name);
        }
        return pack;
    }

    std::map<std::string, size_t> semantic_role_slot_indices() const {
        std::map<std::string, size_t> result;
        for (const auto& [role, identity] : semantic_role_slots) {
            const auto found = std::find_if(physical_slots.begin(), physical_slots.end(),
                [&](const HonorPhysicalSlot& slot) { return slot.id == identity; });
            if (found == physical_slots.end()) return {};
            result.emplace(role, size_t(found - physical_slots.begin()));
        }
        return result;
    }

    std::vector<std::string> portrait_palette_roles() const {
        static const std::map<std::string, std::string> roles {
            {"skin-light", "portrait-skin"}, {"charcoal", "portrait-dark"},
            {"milk-white", "portrait-light"}, {"peach", "portrait-lips"},
            {"denim-blue", "portrait-cool"}, {"gray-brown", "portrait-mid"},
        };
        std::vector<std::string> result;
        result.reserve(physical_slots.size());
        for (size_t i = 0; i < model_slot_count; ++i) {
            const auto& slot = physical_slots[i];
            const auto found = roles.find(slot.id);
            if (found == roles.end()) return {};
            result.push_back(found->second);
        }
        return result;
    }
};

inline const std::vector<HonorFilamentSuite>& honor_filament_suites() {
    static const std::vector<HonorFilamentSuite> suites {
        {"honor.people.six", u8"人物", u8"人物六色", "1", {
            {"charcoal", u8"曜石黑", "#000000", "PLA"},
            {"peach", u8"自然唇色", "#AF6E5E", "PLA"},
            {"skin-light", u8"轻肤色", "#ECC3B2", "PLA"},
            {"milk-white", u8"纯白", "#FFFFFF", "PLA"},
            {"gray-brown", u8"岩石灰", "#A7A5A4", "PLA"},
            {"support-white", u8"纯白 · 支撑材料", "#FFFFFF", "PLA"},
        }, {{"Skin", "skin-light"}, {"Hair", "charcoal"}, {"Iris", "charcoal"},
            {"Eyebrow", "charcoal"}, {"EyeSclera", "milk-white"}, {"Teeth", "milk-white"},
            {"Lips", "peach"}, {"MouthInterior", "peach"}}, 5, "support-white"},
        {"honor.pets.six", u8"萌宠", u8"萌宠六色", "1", {
            {"model-white", u8"纯白", "#FFFFFF", "PLA"},
            {"model-black", u8"曜石黑", "#000000", "PLA"},
            {"model-brown", u8"岩石棕", "#5E5145", "PLA"},
            {"model-orange", u8"虎橙", "#AC7443", "PLA"},
            {"model-yellow", u8"柠檬黄", "#D5C451", "PLA"},
            {"support-white", u8"纯白 · 支撑材料", "#FFFFFF", "PLA"},
        }, {}, 5, "support-white"},
    };
    return suites;
}

inline const HonorFilamentSuite* find_honor_filament_suite(const std::string& id,
                                                            const std::string& revision = {}) {
    const auto& suites = honor_filament_suites();
    const auto found = std::find_if(suites.begin(), suites.end(), [&](const HonorFilamentSuite& suite) {
        return suite.id == id && (revision.empty() || suite.revision == revision);
    });
    return found == suites.end() ? nullptr : &*found;
}

inline const HonorFilamentSuite& honor_people_suite() { return honor_filament_suites().front(); }
inline FilamentColorPack honor_people_color_pack() { return honor_people_suite().color_pack(); }
inline bool is_honor_filament_suite_name(const std::string& name) {
    const auto& suites = honor_filament_suites();
    return std::any_of(suites.begin(), suites.end(), [&](const auto& suite) { return suite.name == name; });
}

std::vector<FilamentColorPack> load_filament_color_packs();
FilamentColorPack capture_current_filament_color_pack(const Slic3r::PresetBundle& bundle,
    const std::string& name = u8"当前打印机耗材");
bool save_filament_color_pack(const FilamentColorPack& pack);
// Explicit project operation. Cancelling the picker leaves the project unchanged.
bool show_filament_color_packs(wxWindow* parent);

} // namespace Slic3r::GUI
