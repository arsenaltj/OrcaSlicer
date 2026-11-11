#pragma once

#include "ModelPreviewPalette.hpp"
#include "SemanticRegionEvidence.hpp"
#include <numeric>
#include <set>

namespace Slic3r::GUI::PortraitSuite {

// Separate from ColorCardRemap: that public contract remains one-to-one.
struct Binding {
    std::string schema {"orca.portrait-suite-binding/v1"};
    std::string basis_signature, suite_id, suite_revision;
    std::vector<size_t> source_to_model_slot;
    bool near_color_fallback {false};
    double weighted_error {0.0};
    bool valid(size_t count) const {
        if (schema != "orca.portrait-suite-binding/v1" || basis_signature.empty() || suite_id.empty() ||
            suite_revision.empty() || count < 4 || count > 6 || source_to_model_slot.size() != count ||
            !std::isfinite(weighted_error) || weighted_error < 0) return false;
        std::set<size_t> used;
        for (size_t slot : source_to_model_slot) { if (slot >= 5) return false; used.insert(slot); }
        return used.size() == std::min(count, size_t(5));
    }
};
inline void to_json(nlohmann::json& j, const Binding& b) {
    j = {{"schema", b.schema}, {"basis_signature", b.basis_signature}, {"suite_id", b.suite_id},
         {"suite_revision", b.suite_revision}, {"source_to_model_slot", b.source_to_model_slot},
         {"near_color_fallback", b.near_color_fallback}, {"weighted_error", b.weighted_error}};
}
inline void from_json(const nlohmann::json& j, Binding& b) {
    b.schema = j.at("schema"); b.basis_signature = j.at("basis_signature");
    b.suite_id = j.at("suite_id"); b.suite_revision = j.at("suite_revision");
    b.source_to_model_slot = j.at("source_to_model_slot").get<std::vector<size_t>>();
    b.near_color_fallback = j.at("near_color_fallback"); b.weighted_error = j.at("weighted_error");
    if (!b.valid(b.source_to_model_slot.size())) throw std::invalid_argument("Invalid portrait suite binding");
}

struct Constraints {
    bool reliable {false};
    bool person_detected {false}, partial {false};
    double known_area_ratio {0.0};
    std::vector<std::set<AI::SemanticColoring::Label>> labels;
    bool known_boundary_allows_share(size_t a, size_t b) const {
        namespace SC = AI::SemanticColoring;
        // Even an incomplete analysis can contain reliable protected faces.
        // Explicit fallback cannot discard those observations.
        if (a >= labels.size() || b >= labels.size()) return !reliable;
        // Never erase eyes, brows or lips, or a skin/garment/hair boundary.
        for (auto slot : {a, b}) for (auto label : labels[slot])
            if (label == SC::Label::EyeSclera || label == SC::Label::Iris ||
                label == SC::Label::Eyebrow || label == SC::Label::Lips ||
                label == SC::Label::MouthInterior) return false;
        if (labels[a].size() > 1 || labels[b].size() > 1) return false;
        if (labels[a].empty() || labels[b].empty()) return !reliable;
        return labels[a] == labels[b];
    }
};

inline Constraints constraints(const PreviewPalette::AutomaticColorBasis& basis,
                               const SemanticRegionEvidence* evidence, const std::string& source_content = {}) {
    Constraints c; c.labels.resize(basis.source_palette.size());
    if (!basis.valid() || !evidence || !evidence->valid() || evidence->geometry_id != basis.geometry_id ||
        basis.unit_source_slots.size() != evidence->labels.size() ||
        (!source_content.empty() && source_content != evidence->source_content_id)) return c;
    c.person_detected = evidence->person_detected;
    double total = 0, known = 0;
    for (size_t face = 0; face < evidence->labels.size(); ++face) {
        const size_t slot = basis.unit_source_slots[face];
        if (slot >= c.labels.size()) return c;
        const double area = basis.unit_area[face]; total += area;
        auto label = evidence->labels[face];
        if (evidence->confidence[face] < AI::SemanticColoring::minimum_confidence ||
            label == AI::SemanticColoring::Label::Unknown || label == AI::SemanticColoring::Label::Background) continue;
        known += area; c.labels[slot].insert(label);
    }
    // A back/bottom surface without a camera sample is not evidence that the
    // detected person vanished. Keep trustworthy face and garment observations,
    // including small eyes/brows which only survived in midpoint leaves.
    for (const auto& leaf : evidence->subfaces)
        if (leaf.label != AI::SemanticColoring::Label::Unknown && leaf.label != AI::SemanticColoring::Label::Background)
            c.labels[basis.unit_source_slots[leaf.face_id]].insert(leaf.label);
    bool face = false, body = false;
    for (const auto& labels : c.labels) for (auto label : labels) {
        using L = AI::SemanticColoring::Label;
        face |= label == L::FaceSkin || label == L::EyeSclera || label == L::Iris ||
            label == L::Eyebrow || label == L::Lips;
        body |= label == L::Hair || label == L::BodySkin || label == L::Clothes;
    }
    c.known_area_ratio = total > 0 ? known / total : 0;
    c.reliable = c.person_detected && face && body;
    c.partial = c.person_detected && c.known_area_ratio < .7;
    return c;
}

struct Result { Binding binding; std::string reason; std::vector<std::pair<size_t, size_t>> conflicts; };
inline Result match(const PreviewPalette::AutomaticColorBasis& basis,
                    const std::vector<PreviewPalette::Color>& five_colors, const Constraints& c,
                    const std::string& suite, const std::string& revision, bool fallback_confirmed = false) {
    Result r;
    const size_t count = basis.source_palette.size();
    if (!basis.valid() || count != basis.requested_count || count < 4 || count > 6 || five_colors.size() != 5) {
        r.reason = "原始外观无法建立与 2D 最终色数一致的 4/5/6 个稳定源区域。"; return r;
    }
    if (!c.reliable && !fallback_confirmed) {
        r.reason = "人像语义证据不可靠，需明确确认后才可保留源区域作近色回退。"; return r;
    }
    Binding best; best.basis_signature = basis.basis_signature; best.suite_id = suite;
    best.suite_revision = revision; best.near_color_fallback = !c.reliable;
    best.weighted_error = std::numeric_limits<double>::infinity();
    if (count <= 5) {
        const auto remap = PreviewPalette::minimum_cost_color_card_remap(basis, five_colors);
        if (remap.valid(count)) { best.source_to_model_slot = remap.source_to_target_slot; best.weighted_error = remap.weighted_error; }
    } else {
        for (size_t a = 0; a < count; ++a) for (size_t b = a + 1; b < count; ++b) {
            // Explicit fallback may use colour evidence when semantics is
            // unavailable, but never merge very different source colours.
            const bool safe = (c.reliable || fallback_confirmed) && c.known_boundary_allows_share(a, b) &&
                PreviewPalette::distance(PreviewPalette::to_lab(basis.source_palette[a]),
                    PreviewPalette::to_lab(basis.source_palette[b])) <= .0225f;
            if (!safe) { r.conflicts.emplace_back(a, b); continue; }
            std::vector<size_t> order {0, 1, 2, 3, 4};
            do {
                std::vector<size_t> slots(count); size_t group = 0;
                for (size_t i = 0; i < count; ++i) if (i != b) slots[i] = order[group++];
                slots[b] = slots[a]; double cost = 0;
                for (size_t i = 0; i < count; ++i)
                    cost += basis.source_area[i] * double(PreviewPalette::distance(
                        PreviewPalette::to_lab(basis.source_palette[i]), PreviewPalette::to_lab(five_colors[slots[i]])));
                if (cost < best.weighted_error || (cost == best.weighted_error && slots < best.source_to_model_slot)) {
                    best.source_to_model_slot = std::move(slots); best.weighted_error = cost;
                }
            } while (std::next_permutation(order.begin(), order.end()));
        }
    }
    if (!best.valid(count)) {
        r.reason = "六个源区域没有安全的共享颜色组合；请在工作台处理冲突区域，源区域和支撑槽均已保留。";
        return r;
    }
    r.binding = std::move(best); return r;
}
} // namespace Slic3r::GUI::PortraitSuite
