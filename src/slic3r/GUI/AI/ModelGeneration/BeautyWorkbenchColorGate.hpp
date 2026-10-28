#pragma once

#include "PrinterFilamentPreview.hpp"
#include "slic3r/AI/Contracts/IModelArtifactConsumer.hpp"

#include <atomic>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>

namespace Slic3r::GUI {

enum class BeautyColorGateState { Unchecked, Checking, Compatible, Blocked };
enum class BeautyWorkbenchTask { Recolor, Soften };
enum class BeautyWorkbenchLeaveState { Ready, ConfirmChanges, Processing };

struct BeautyWorkbenchSidebarInput {
    BeautyWorkbenchTask task {BeautyWorkbenchTask::Recolor};
    BeautyColorGateState color_gate {BeautyColorGateState::Unchecked};
    bool model_editable {false};
    bool processing {false};
    bool candidate_ready {false};
    bool dirty {false};
    bool selection_present {false};
    bool can_undo {false};
    bool can_redo {false};
    bool more_tools {false};
    bool more_repairs {false};
};

struct BeautyWorkbenchSidebarState {
    bool can_start_edit {false};
    bool can_save {false};
    bool can_leave {false};
    bool can_undo {false};
    bool can_redo {false};
    bool can_accept_candidate {false};
    bool can_compare {false};
    bool show_recolor {false};
    bool show_soften {false};
    bool show_more_tools {false};
    bool show_region_shortcuts {false};
    bool show_more_repairs {false};
    bool show_repair_actions {false};
    bool show_progress {false};
    bool show_candidate_actions {false};
};

inline BeautyWorkbenchSidebarState derive_beauty_workbench_sidebar_state(
    const BeautyWorkbenchSidebarInput& input)
{
    BeautyWorkbenchSidebarState state;
    const bool compatible = input.color_gate == BeautyColorGateState::Compatible;
    const bool busy = input.processing || input.color_gate == BeautyColorGateState::Checking;
    state.can_start_edit = input.model_editable && compatible && !busy &&
        !input.candidate_ready;
    state.can_save = input.model_editable && compatible && !busy;
    state.can_leave = !busy;
    // A changed filament configuration freezes new edits and saving, but the
    // user must still be able to unwind or discard the existing draft.
    state.can_undo = !busy && input.can_undo;
    state.can_redo = !busy && compatible && input.can_redo;
    state.can_accept_candidate = input.model_editable && compatible && !busy &&
        input.candidate_ready;
    state.can_compare = compatible && !busy && (input.candidate_ready || input.dirty);
    state.show_recolor = input.task == BeautyWorkbenchTask::Recolor;
    state.show_soften = input.task == BeautyWorkbenchTask::Soften;
    state.show_more_tools = input.more_tools;
    state.show_region_shortcuts = state.show_recolor;
    state.show_more_repairs = input.more_tools;
    state.show_repair_actions = input.more_tools && input.more_repairs;
    state.show_progress = busy;
    state.show_candidate_actions = input.candidate_ready;
    return state;
}

inline BeautyWorkbenchLeaveState derive_beauty_workbench_leave_state(bool processing,
                                                                      bool has_changes)
{
    if (processing) return BeautyWorkbenchLeaveState::Processing;
    return has_changes ? BeautyWorkbenchLeaveState::ConfirmChanges
                       : BeautyWorkbenchLeaveState::Ready;
}

template <class Refresh, class StillCurrent, class Resume>
inline bool continue_beauty_partition_after_semantics(bool request_current,
                                                       Refresh&& refresh,
                                                       StillCurrent&& still_current,
                                                       Resume&& resume)
{
    if (!request_current) return false;
    std::forward<Refresh>(refresh)();
    if (!std::forward<StillCurrent>(still_current)()) return false;
    std::forward<Resume>(resume)();
    return true;
}

inline bool beauty_workbench_has_active_processing(bool finishing,
                                                    bool semantic,
                                                    bool portrait,
                                                    bool transaction)
{
    return finishing || semantic || portrait || transaction;
}

inline bool beauty_workbench_has_unsaved_changes(bool candidate_ready,
                                                  bool manual_color_dirty,
                                                  bool controls_dirty,
                                                  bool comparison_draft)
{
    return candidate_ready || manual_color_dirty || controls_dirty || comparison_draft;
}

inline bool beauty_workbench_compare_available(bool model_ready,
                                                bool processing,
                                                BeautyColorGateState color_gate,
                                                bool candidate_ready,
                                                bool has_changes)
{
    return model_ready && !processing && color_gate == BeautyColorGateState::Compatible &&
        (candidate_ready || has_changes);
}

inline std::vector<size_t> beauty_unprotected_selected_faces(
    const std::vector<size_t>& selected_faces,
    const std::vector<uint8_t>& protected_faces)
{
    std::vector<size_t> editable;
    editable.reserve(selected_faces.size());
    for (size_t face : selected_faces) {
        if (face < protected_faces.size() && protected_faces[face]) continue;
        editable.push_back(face);
    }
    return editable;
}

inline bool beauty_candidate_requires_filament_rematch(bool topology_candidate,
                                                        size_t faces_before,
                                                        size_t faces_after,
                                                        size_t removed_degenerate_faces,
                                                        size_t removed_duplicate_faces)
{
    return topology_candidate && (faces_before != faces_after ||
        removed_degenerate_faces != 0 || removed_duplicate_faces != 0);
}

inline bool beauty_preserve_color_block_artifact(bool has_color_block_contract,
                                                  bool printer_filament_application)
{
    return has_color_block_contract && !printer_filament_application;
}

struct BeautyColorGateSurfaceColor {
    PrinterFilamentPreview::Color color {};
    std::optional<size_t> preferred_slot;
};

struct BeautyColorGateSubface : BeautyColorGateSurfaceColor {
    size_t face_id {0};
    uint8_t depth {0};
    uint8_t path {0};
};

struct BeautyColorGateRequest {
    std::string source_sha256;
    std::string geometry_id;
    std::string material_fingerprint;
    std::string process_fingerprint;
    std::string configuration_signature;
    std::vector<AI::PhysicalFilamentChannel> palette;
    std::vector<AI::MixedColorRecipe> mixed_recipes;
    std::vector<BeautyColorGateSurfaceColor> faces;
    std::vector<BeautyColorGateSubface> subfaces;
    std::string unavailable_reason;

    std::string color_revision() const
    {
        // This is a cache revision, not an external identity. FNV-1a keeps the
        // main thread's snapshot pass cheap while still covering RGB8, native
        // slot preference, and every subface key.
        uint64_t hash = 1469598103934665603ULL;
        const auto mix = [&hash](uint64_t value) {
            for (size_t byte = 0; byte < sizeof(value); ++byte) {
                hash ^= (value >> (byte * 8)) & 0xff;
                hash *= 1099511628211ULL;
            }
        };
        for (const auto& face : faces) {
            mix(PrinterFilamentPreview::rgb8(face.color));
            mix(face.preferred_slot.value_or(size_t(-1)));
        }
        for (const auto& child : subfaces) {
            mix(child.face_id); mix(child.depth); mix(child.path);
            mix(PrinterFilamentPreview::rgb8(child.color));
            mix(child.preferred_slot.value_or(size_t(-1)));
        }
        std::ostringstream output;
        output << std::hex << std::setw(16) << std::setfill('0') << hash;
        return output.str();
    }

    std::string cache_key() const
    {
        return source_sha256 + ":" + geometry_id + ":" + color_revision() + ":" +
            material_fingerprint + ":" + process_fingerprint + ":" + configuration_signature;
    }
};

struct BeautyColorGateIdentity {
    std::string source_sha256;
    std::string geometry_id;
    std::string color_revision;
    std::string material_fingerprint;
    std::string process_fingerprint;
    std::string configuration_signature;

    std::string cache_key() const
    {
        if (source_sha256.empty() && geometry_id.empty() && color_revision.empty() &&
            material_fingerprint.empty() && process_fingerprint.empty() &&
            configuration_signature.empty()) return {};
        return source_sha256 + ":" + geometry_id + ":" + color_revision + ":" +
            material_fingerprint + ":" + process_fingerprint + ":" + configuration_signature;
    }
};

inline BeautyColorGateIdentity beauty_color_gate_identity(const BeautyColorGateRequest& request)
{
    return {request.source_sha256, request.geometry_id, request.color_revision(),
            request.material_fingerprint, request.process_fingerprint,
            request.configuration_signature};
}

struct BeautyColorGateResult {
    std::optional<AI::ModelMatchedColors> matched;
    std::vector<std::string> missing_colors;
    std::vector<std::string> ambiguous_colors;
    std::string error;
    bool canceled {false};

    bool compatible() const { return matched.has_value() && !canceled && error.empty(); }
};

inline BeautyColorGateResult match_beauty_workbench_colors(
    const BeautyColorGateRequest& request, const std::atomic<bool>* canceled = nullptr)
{
    BeautyColorGateResult result;
    const auto stopped = [&] { return canceled && canceled->load(); };
    if (stopped()) { result.canceled = true; return result; }
    if (!request.unavailable_reason.empty()) {
        result.error = request.unavailable_reason;
        return result;
    }
    if (!AI::is_lowercase_sha256(request.source_sha256) ||
        !AI::is_lowercase_sha256(request.geometry_id) || request.faces.empty()) {
        result.error = "当前模型身份或完整表面配色无效，请重新载入模型。";
        return result;
    }

    AI::ModelMatchedColors matched;
    matched.source_sha256 = request.source_sha256;
    matched.geometry_id = request.geometry_id;
    matched.material_fingerprint = request.material_fingerprint;
    matched.process_fingerprint = request.process_fingerprint;
    matched.palette = request.palette;
    matched.mixed_recipes = request.mixed_recipes;
    if (!AI::valid_native_mixed_palette(matched.palette, matched.mixed_recipes)) {
        result.error = "当前打印机耗材或叠色配方无效，请先完成配置。";
        return result;
    }

    std::vector<PrinterFilamentPreview::NativeColorSlot> candidates;
    auto append = [&candidates](size_t slot, const std::string& color, bool mixed) {
        const auto key = PrinterFilamentPreview::rgb8(color);
        if (key) candidates.push_back({slot, PrinterFilamentPreview::rgb8_color(*key), mixed});
    };
    auto physical = matched.palette;
    std::sort(physical.begin(), physical.end(), [](const auto& first, const auto& second) {
        return first.slot < second.slot;
    });
    for (const auto& slot : physical)
        if (slot.compatible) append(slot.slot, slot.display_color, false);
    auto recipes = matched.mixed_recipes;
    std::sort(recipes.begin(), recipes.end(), [](const auto& first, const auto& second) {
        return first.existing_virtual_slot.value_or(size_t(-1)) <
               second.existing_virtual_slot.value_or(size_t(-1));
    });
    for (const auto& recipe : recipes)
        if (recipe.existing_virtual_slot) append(*recipe.existing_virtual_slot, recipe.target_color, true);
    if (candidates.empty()) {
        result.error = "当前没有可用于模型配色的兼容耗材槽。";
        return result;
    }

    struct StrictBinding {
        std::vector<size_t> slots;
        std::set<uint32_t> missing;
        std::set<uint32_t> ambiguous;
        bool canceled {false};
    };
    const auto bind = [&](const auto& surfaces) {
        StrictBinding binding;
        binding.slots.reserve(surfaces.size());
        for (const auto& surface : surfaces) {
            if (stopped()) {
                binding.canceled = true;
                return binding;
            }
            const uint32_t key = PrinterFilamentPreview::rgb8(surface.color);
            auto chosen = candidates.end();
            if (surface.preferred_slot) {
                chosen = std::find_if(candidates.begin(), candidates.end(), [&](const auto& candidate) {
                    return candidate.slot == *surface.preferred_slot &&
                           PrinterFilamentPreview::rgb8(candidate.color) == key;
                });
            }
            if (chosen == candidates.end()) {
                size_t matches = 0;
                for (auto candidate = candidates.begin(); candidate != candidates.end(); ++candidate) {
                    if (PrinterFilamentPreview::rgb8(candidate->color) != key) continue;
                    chosen = candidate;
                    ++matches;
                }
                if (matches > 1) {
                    binding.ambiguous.insert(key);
                    binding.slots.push_back(size_t(-1));
                    continue;
                }
            }
            if (chosen == candidates.end()) {
                binding.missing.insert(key);
                binding.slots.push_back(size_t(-1));
            } else {
                binding.slots.push_back(chosen->slot);
            }
        }
        return binding;
    };

    const auto faces = bind(request.faces);
    if (faces.canceled) { result.canceled = true; return result; }
    const auto subfaces = bind(request.subfaces);
    if (subfaces.canceled) { result.canceled = true; return result; }
    for (uint32_t key : faces.missing) result.missing_colors.push_back(PrinterFilamentPreview::rgb8_hex(key));
    for (uint32_t key : subfaces.missing) result.missing_colors.push_back(PrinterFilamentPreview::rgb8_hex(key));
    std::sort(result.missing_colors.begin(), result.missing_colors.end());
    result.missing_colors.erase(std::unique(result.missing_colors.begin(), result.missing_colors.end()),
                                result.missing_colors.end());
    for (uint32_t key : faces.ambiguous) result.ambiguous_colors.push_back(PrinterFilamentPreview::rgb8_hex(key));
    for (uint32_t key : subfaces.ambiguous) result.ambiguous_colors.push_back(PrinterFilamentPreview::rgb8_hex(key));
    std::sort(result.ambiguous_colors.begin(), result.ambiguous_colors.end());
    result.ambiguous_colors.erase(std::unique(result.ambiguous_colors.begin(), result.ambiguous_colors.end()),
                                  result.ambiguous_colors.end());
    if (!result.ambiguous_colors.empty()) {
        result.error = "模型中存在相同 RGB 但槽位身份不明确的区域，请先用当前耗材适配模型或恢复带槽位记录的版本。";
        return result;
    }
    if (!result.missing_colors.empty()) return result;

    matched.face_slots = faces.slots;
    for (size_t index = 0; index < request.subfaces.size(); ++index) {
        const auto& child = request.subfaces[index];
        matched.subface_slots.push_back({child.face_id, child.depth, child.path, subfaces.slots[index]});
    }
    if (!matched.valid()) {
        result.error = "当前耗材映射没有生成完整的面级和子面级绑定。";
        return result;
    }
    result.matched = std::move(matched);
    return result;
}

struct BeautyPaintSlotChanges {
    std::vector<size_t> faces;
    std::vector<AI::ModelSubfaceSlot> subfaces;
};

inline bool apply_beauty_paint_slot(AI::ModelMatchedColors& matched,
                                    const BeautyPaintSlotChanges& changes,
                                    size_t native_slot)
{
    if (!matched.valid() || !AI::native_palette_has_slot(
            matched.palette, matched.mixed_recipes, native_slot)) return false;
    auto updated = matched;
    for (size_t face : changes.faces) {
        if (face >= updated.face_slots.size()) return false;
        updated.face_slots[face] = native_slot;
    }
    std::map<std::tuple<size_t, uint8_t, uint8_t>, size_t> leaves;
    for (const auto& child : updated.subface_slots)
        leaves[{child.face_id, child.depth, child.path}] = child.slot;
    for (const auto& child : changes.subfaces) {
        if (child.face_id >= updated.face_slots.size() || child.depth == 0 || child.depth > 4 ||
            unsigned(child.path) >= (1u << (2u * child.depth))) return false;
        leaves[{child.face_id, child.depth, child.path}] = native_slot;
    }
    updated.subface_slots.clear();
    updated.subface_slots.reserve(leaves.size());
    for (const auto& item : leaves)
        updated.subface_slots.push_back({std::get<0>(item.first), std::get<1>(item.first),
                                         std::get<2>(item.first), item.second});
    if (!updated.valid()) return false;
    matched = std::move(updated);
    return true;
}

inline bool restore_beauty_paint_slots(AI::ModelMatchedColors& current,
                                       const AI::ModelMatchedColors& baseline,
                                       const BeautyPaintSlotChanges& changes)
{
    if (!current.valid() || !baseline.valid() ||
        current.face_slots.size() != baseline.face_slots.size() ||
        current.geometry_id != baseline.geometry_id) return false;
    auto updated = current;
    for (size_t face : changes.faces) {
        if (face >= updated.face_slots.size()) return false;
        updated.face_slots[face] = baseline.face_slots[face];
    }
    std::map<std::tuple<size_t, uint8_t, uint8_t>, size_t> leaves, original;
    for (const auto& child : updated.subface_slots)
        leaves[{child.face_id, child.depth, child.path}] = child.slot;
    for (const auto& child : baseline.subface_slots)
        original[{child.face_id, child.depth, child.path}] = child.slot;
    for (const auto& child : changes.subfaces) {
        const auto key = std::make_tuple(child.face_id, child.depth, child.path);
        const auto found = original.find(key);
        if (found == original.end()) leaves.erase(key);
        else leaves[key] = found->second;
    }
    updated.subface_slots.clear();
    for (const auto& item : leaves)
        updated.subface_slots.push_back({std::get<0>(item.first), std::get<1>(item.first),
                                         std::get<2>(item.first), item.second});
    if (!updated.valid()) return false;
    current = std::move(updated);
    return true;
}

} // namespace Slic3r::GUI
