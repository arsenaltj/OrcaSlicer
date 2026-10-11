#pragma once

#include "libslic3r/Model.hpp"
#include "libslic3r/ParameterUtils.hpp"

namespace Slic3r::GUI::PrinterFilamentSlotRemap {
struct Change {
    std::vector<size_t> inserted;
    int removed {-1}, replacement {-1}; // zero-based, replacement BEFORE deletion
};

inline int remap(int value, const Change& change)
{
    if (value <= 0) return value;
    for (auto at : change.inserted) if (value > int(at)) ++value;
    auto apply_removal = [&value](int removed, int replacement) {
        if (removed < 0) return;
        if (value == removed + 1) value = replacement + 1;
        if (value > removed + 1) --value;
    };
    apply_removal(change.removed, change.replacement);
    return value;
}
inline void config(ModelConfig& config, const Change& change)
{
    for (const auto* key : {"extruder", "support_filament", "support_interface_filament"})
        if (config.has(key)) config.set(key, remap(config.opt_int(key), change));
}

inline void plate_config(DynamicPrintConfig& config, const Change& change)
{
    for (const auto* key : {"filament_map", "filament_nozzle_map", "filament_volume_map"}) {
        auto* option = config.option<ConfigOptionInts>(key);
        if (!option) continue;
        for (const auto at : change.inserted)
            if (at <= option->values.size()) option->values.insert(option->values.begin() + at, key == std::string("filament_map") ? 1 : 0);
        auto erase_slot = [&option](int removed) {
            if (removed >= 0 && size_t(removed) < option->values.size())
                option->values.erase(option->values.begin() + removed);
        };
        erase_slot(change.removed);
    }
    auto sequence = [&change](std::vector<int>& values) {
        if (values.empty() || values.front() == 0) return;
        std::vector<int> updated;
        for (const int value : values) {
            const int target = remap(value, change);
            if (std::find(updated.begin(), updated.end(), target) == updated.end()) updated.push_back(target);
        }
        for (size_t i = 0; i < change.inserted.size(); ++i) {
            int added = int(change.inserted[i] + 1);
            for (size_t j = i + 1; j < change.inserted.size(); ++j)
                if (added > int(change.inserted[j])) ++added;
            updated.push_back(added);
        }
        values = std::move(updated);
    };
    if (auto* first = config.option<ConfigOptionInts>("first_layer_print_sequence")) sequence(first->values);
    const auto* count = config.option<ConfigOptionInt>("other_layers_print_sequence_nums");
    auto* other = config.option<ConfigOptionInts>("other_layers_print_sequence");
    if (count && other && count->value > 0 && other->values.size() % size_t(count->value) == 0 &&
        other->values.size() / size_t(count->value) > 2) {
        auto groups = get_other_layers_print_sequence(count->value, other->values);
        for (auto& group : groups) sequence(group.second);
        int new_count = 0;
        get_other_layers_print_sequence(groups, new_count, other->values);
    }
}

// Prepare on a COPY before recording native history. Reuse the native facet
// remapping and preserve geometry, boundaries and subface painting.
inline void prepare(Model& model, const Change& change, size_t final_count,
    const std::vector<unsigned char>& old_mixed)
{
    for (auto* object : model.objects) {
        config(object->config, change);
        for (auto& range : object->layer_config_ranges) config(range.second, change);
        for (auto* volume : object->volumes) {
            for (auto at : change.inserted)
                volume->mmu_segmentation_facets.shift_states_above(*volume, EnforcerBlockerType(at + 1), 1);
            auto delete_slot = [&](int removed, int replacement) {
                if (removed < 0) return;
                const int target = replacement > removed ? replacement - 1 : replacement;
                volume->update_extruder_count_when_delete_filament(final_count, size_t(removed + 1), target + 1, old_mixed);
            };
            delete_slot(change.removed, change.replacement);
            config(volume->config, change);
        }
    }
    for (auto& plate : model.plates_custom_gcodes)
        for (auto& code : plate.second.gcodes)
            if (code.type == CustomGCode::Type::ToolChange || code.type == CustomGCode::Type::ColorChange)
                code.extruder = remap(code.extruder, change);
}
// Dialogs may import several cards before submitting. Replay the operations
// in their original order, including transient slots inserted then removed.
inline bool validate(const std::vector<Change>& changes, size_t initial_count, size_t final_count)
{
    size_t count = initial_count;
    for (const auto& change : changes) {
        for (size_t at : change.inserted) {
            if (at > count || count >= size_t(EnforcerBlockerType::ExtruderMax)) return false;
            ++count;
        }
        if (change.removed >= 0) {
            if (count <= 1 || size_t(change.removed) >= count || change.replacement < 0 ||
                size_t(change.replacement) >= count || change.removed == change.replacement) return false;
            --count;
        }
    }
    return count == final_count;
}

inline void prepare(Model& model, const std::vector<Change>& changes, size_t initial_count,
    std::vector<unsigned char> mixed)
{
    mixed.resize(initial_count, false);
    size_t count = initial_count;
    for (const auto& change : changes) {
        for (size_t at : change.inserted) {
            mixed.insert(mixed.begin() + at, false);
            ++count;
        }
        const size_t next_count = count - (change.removed >= 0 ? 1 : 0);
        prepare(model, change, next_count, mixed);
        if (change.removed >= 0) mixed.erase(mixed.begin() + change.removed);
        count = next_count;
    }
}
inline void commit(Model& live, Model& prepared)
{
    for (size_t i = 0; i < live.objects.size(); ++i) {
        auto* object = live.objects[i]; auto* source = prepared.objects[i];
        object->config.assign_config(std::move(source->config));
        object->layer_config_ranges.swap(source->layer_config_ranges);
        for (size_t j = 0; j < object->volumes.size(); ++j) {
            object->volumes[j]->config.assign_config(std::move(source->volumes[j]->config));
            object->volumes[j]->mmu_segmentation_facets.assign(std::move(source->volumes[j]->mmu_segmentation_facets));
        }
    }
    live.plates_custom_gcodes.swap(prepared.plates_custom_gcodes);
}
} // namespace Slic3r::GUI::PrinterFilamentSlotRemap
