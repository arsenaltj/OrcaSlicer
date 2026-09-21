#pragma once
#include "slic3r/AI/ModelGeneration/SemanticColoring/SemanticPaletteMapping.hpp"
#include "slic3r/GUI/AI/Orca/ModelColorUpdate.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelPreviewPalette.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Semver.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace Orca::SemanticValidation {
namespace SC = Slic3r::AI::SemanticColoring;
namespace AI = Slic3r::AI;
namespace GUI = Slic3r::GUI;
namespace PP = Slic3r::GUI::PreviewPalette;
using Json = nlohmann::json;

inline std::string color_hex(const SC::Color& color)
{
    std::ostringstream out;
    out << '#' << std::hex << std::setfill('0');
    for (float channel : color) out << std::setw(2) << int(std::lround(std::clamp(channel, 0.f, 1.f) * 255.f));
    return out.str();
}
inline void require_persistence(bool condition, const char* error)
{
    if (!condition) throw std::runtime_error(error);
}
inline Json persist_and_reload(Slic3r::Model& model, Slic3r::DynamicPrintConfig& config,
                               const std::filesystem::path& path, const std::filesystem::path& backup)
{
    using namespace Slic3r;
    // The 3MF writer still opens a narrow path on Windows. Exercise its real
    // writer/reader on an ASCII path, then publish the verified file at the
    // requested (possibly Unicode) evidence location.
    const auto staging = std::filesystem::temp_directory_path() / "orca-semantic-validation" /
        std::to_string(std::hash<std::string>{}(path.u8string()));
    std::filesystem::create_directories(staging / "backup");
    model.set_backup_path((staging / "backup").string());
    const auto& expected_volume = *model.objects.front()->volumes.front();
    const auto expected = expected_volume.mmu_segmentation_facets.get_data();
    const auto& indices = expected_volume.mesh().its.indices;
    PlateData plate;
    plate.plate_index = 0;
    const auto staged_path = staging / path.filename();
    const auto path_text = staged_path.string();
    StoreParams params;
    params.path = path_text.c_str();
    params.model = &model;
    params.config = &config;
    params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
    params.plate_data_list.push_back(&plate);
    require_persistence(store_bbs_3mf(params), "Unable to save the real semantic 3MF project");

    DynamicPrintConfig restored_config;
    ConfigSubstitutionContext substitutions {ForwardCompatibilitySubstitutionRule::Enable};
    PlateDataPtrs plates;
    std::vector<Preset*> presets;
    struct LoadedResources {
        PlateDataPtrs& plates;
        std::vector<Preset*>& presets;
        ~LoadedResources() { release_PlateData_list(plates); for (auto* preset : presets) delete preset; }
    } loaded_resources {plates, presets};
    Model restored = Model::read_from_file(path_text, &restored_config, &substitutions,
        LoadStrategy::LoadModel | LoadStrategy::LoadConfig, &plates, &presets);
    require_persistence(restored.objects.size() == 1 && restored.objects.front()->volumes.size() == 1,
        "3MF changed the semantic model/volume count");
    const auto* volume = restored.objects.front()->volumes.front();
    require_persistence(volume->mesh().its.indices == indices, "3MF changed original face ordinals");
    require_persistence(volume->mmu_segmentation_facets.get_data() == expected, "3MF changed the semantic midpoint material tree");
    const auto* expected_colors = config.option<ConfigOptionStrings>("filament_colour");
    const auto* restored_colors = restored_config.option<ConfigOptionStrings>("filament_colour");
    require_persistence(expected_colors && restored_colors && expected_colors->values == restored_colors->values,
        "3MF changed the project filament slots");
    std::filesystem::create_directories(path.parent_path());
    std::filesystem::copy_file(staged_path, path, std::filesystem::copy_options::overwrite_existing);
    Json states = Json::array();
    size_t used_state_count = 0;
    TriangleSelector selector(volume->mesh());
    selector.deserialize(volume->mmu_segmentation_facets.get_data());
    for (size_t slot = 0; slot < restored_colors->values.size(); ++slot) {
        const auto count = selector.num_facets(EnforcerBlockerType(slot + 1));
        used_state_count += count > 0 ? 1 : 0;
        states.push_back({{"project_slot", slot}, {"painted_leaf_count", count}});
    }
    return {{"file", path.u8string()}, {"bytes", std::filesystem::file_size(path)},
        {"original_face_count", indices.size()}, {"material_tree_identical", true},
        {"face_order_identical", true}, {"project_slots_identical", true}, {"material_states", states},
        {"configured_project_material_count", restored_colors->values.size()},
        {"actual_used_material_count", used_state_count}};
}
inline Json verify_persistence(const SC::MeshSnapshot& source, const SC::SlotMappingResult& mapping,
                               const std::vector<SC::PaletteSlot>& palette, const PP::ColorTrialMapping& fallback,
                               const std::filesystem::path& output)
{
    using namespace Slic3r;
    std::vector<SC::Color> centers;
    for (const auto& color : fallback.mapping_colors) centers.push_back(PP::to_lab(color));
    require_persistence(!centers.empty() && fallback.target_colors.size() == centers.size(), "Persistence requires a complete safe baseline");
    std::map<std::string, size_t> slot_index;
    std::vector<AI::ModelPaletteSlot> slots;
    std::vector<std::string> config_colors;
    for (size_t i = 0; i < palette.size(); ++i) {
        require_persistence(slot_index.emplace(palette[i].id, i).second, "Duplicate material slot ID");
        slots.push_back({palette[i].id, palette[i].color, i});
        config_colors.push_back(color_hex(palette[i].color));
    }
    std::vector<AI::ModelFaceSlotOverride> faces;
    faces.reserve(source.mesh.indices.size());
    for (size_t id = 0; id < source.mesh.indices.size(); ++id) {
        SC::Color color {};
        if (source.vertex_colors.size() == source.mesh.vertices.size()) {
            for (int corner = 0; corner < 3; ++corner) for (int c = 0; c < 3; ++c)
                color[c] += source.vertex_colors[source.mesh.indices[id][corner]][c] / 3.f;
        } else {
            const auto& rgba = source.face_colors.at(id); color = {rgba[0],rgba[1],rgba[2]};
        }
        const auto target = fallback.target_colors[PP::nearest_lab_index(PP::to_lab(color), centers)];
        const auto chosen = std::find_if(palette.begin(), palette.end(), [&](const auto& slot) { return slot.color == target; });
        require_persistence(chosen != palette.end(), "Safe baseline references a missing slot");
        faces.push_back({id, chosen->id});
    }
    for (const auto& assignment : mapping.face_slots) {
        require_persistence(assignment.face_id < faces.size(), "Automatic face slot exceeds source topology");
        faces[assignment.face_id].slot_id = assignment.slot_id;
    }
    std::vector<AI::ModelSubfaceColorOverride> leaves;
    for (const auto& leaf : mapping.subface_slots) {
        const auto slot = slot_index.find(leaf.slot_id);
        require_persistence(slot != slot_index.end(), "Subface uses a missing slot");
        leaves.push_back({leaf.face_id, leaf.path.depth, leaf.path.value, palette[slot->second].color, leaf.slot_id});
    }
    TriangleSelector::TriangleSplittingData painting;
    std::string error;
    if (!GUI::apply_subface_color_overrides(source.mesh, source.mesh, painting, {}, leaves,
        error, slots, faces)) throw std::runtime_error(error);
    Model model;
    auto* object = model.add_object("Local semantic validation", "local-source.obj", TriangleMesh(source.mesh));
    object->add_instance();
    auto* volume = object->volumes.front();
    require_persistence(GUI::matches_source_topology(source.mesh, *volume), "Import failed strict recorded-translation topology validation");
    const auto automatic = painting;
    volume->mmu_segmentation_facets.set_data(std::move(painting));
    object->config.set("extruder", 1);
    volume->config.set("extruder", 1);
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_filaments(palette.size());
    config.set_key_value("filament_colour", new ConfigOptionStrings(config_colors));
    Json record;
    std::map<std::string, size_t> whole_counts, child_counts;
    for (const auto& face : faces) ++whole_counts[face.slot_id];
    size_t depth_three = 0;
    for (const auto& leaf : leaves) { ++child_counts[leaf.slot_id]; depth_three += leaf.depth == 3 ? 1 : 0; }
    Json only_subfaces = Json::array();
    for (const auto& entry : child_counts) if (whole_counts[entry.first] == 0)
        only_subfaces.push_back({{"slot_id", entry.first}, {"project_slot", slot_index.at(entry.first)},
            {"subface_assignments", entry.second}});
    record["whole_face_slot_counts"] = whole_counts;
    record["subface_slot_counts"] = child_counts;
    record["slots_used_only_by_subfaces"] = only_subfaces;
    record["third_level_subface_assignments"] = depth_three;
    record["automatic"] = persist_and_reload(model, config, output / "automatic.3mf", output / "backup-automatic");

    // A deliberate technical edit exercises whole-face override precedence on
    // a face carrying child materials, then restores the automatic snapshot.
    // This manual fixture is not presented as a visual improvement.
    const size_t edited_face = leaves.empty() ? 0 : leaves.front().face_id;
    const auto before_id = faces[edited_face].slot_id;
    const auto replacement = palette.size() > 1 ? (slot_index.at(before_id) + 1) % palette.size() : 0;
    faces[edited_face].slot_id = palette[replacement].id;
    leaves.erase(std::remove_if(leaves.begin(), leaves.end(), [edited_face](const auto& leaf) {
        return leaf.face_id == edited_face;
    }), leaves.end());
    TriangleSelector::TriangleSplittingData manual;
    if (!GUI::apply_subface_color_overrides(source.mesh, source.mesh, manual, {}, leaves,
        error, slots, faces)) throw std::runtime_error(error);
    TriangleSelector manual_selector(volume->mesh());
    manual_selector.deserialize(manual);
    EnforcerBlockerType state;
    require_persistence(manual_selector.facet_state(int(edited_face), state) && int(state) == int(replacement + 1),
        "A manual whole-face choice did not override all automatic child materials");
    volume->mmu_segmentation_facets.set_data(std::move(manual));
    record["manual"] = persist_and_reload(model, config, output / "manual-override.3mf", output / "backup-manual");
    auto restored_automatic = automatic;
    volume->mmu_segmentation_facets.set_data(std::move(restored_automatic));
    require_persistence(volume->mmu_segmentation_facets.get_data() == automatic, "Automatic material snapshot did not restore after the edit");
    record["manual_face_id"] = edited_face;
    record["manual_overrides_all_children"] = true;
    record["automatic_snapshot_restored"] = true;
    record["limits"] = "Uses the actual importer helper and 3MF writer/reader. Main-window interactions and slicing material preview remain separate checks.";
    return record;
}
}
