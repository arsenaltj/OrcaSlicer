#include "SemanticPaletteMapping.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <nlohmann/json.hpp>

namespace Slic3r::AI::SemanticColoring {
namespace {
std::string stable_hash(const std::string& value)
{
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char byte : value) { hash ^= byte; hash *= 1099511628211ULL; }
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

std::string whole_region_id(const std::string& signature, Label label, const std::vector<size_t>& faces)
{
    nlohmann::json identity = {signature, "whole", size_t(label), faces};
    return "region:" + stable_hash(identity.dump());
}

std::string child_region_id(const std::string& signature, Label label, size_t face_id, const SubfacePath& path)
{
    return "region:" + stable_hash(nlohmann::json::array(
        {signature, "child", size_t(label), face_id, path.depth, path.value}).dump());
}

bool valid(const Color& color)
{
    return std::all_of(color.begin(), color.end(), [](float v) { return std::isfinite(v) && v >= 0.f && v <= 1.f; });
}
Color oklab(Color rgb)
{
    for (float& value : rgb) value = value <= .04045f ? value / 12.92f : std::pow((value + .055f) / 1.055f, 2.4f);
    const float l = std::cbrt(.4122214708f*rgb[0] + .5363325363f*rgb[1] + .0514459929f*rgb[2]);
    const float m = std::cbrt(.2119034982f*rgb[0] + .6806995451f*rgb[1] + .1073969566f*rgb[2]);
    const float s = std::cbrt(.0883024619f*rgb[0] + .2817188376f*rgb[1] + .6299787005f*rgb[2]);
    return {.2104542553f*l + .793617785f*m - .0040720468f*s,
        1.9779984951f*l - 2.428592205f*m + .4505937099f*s,
        .0259040371f*l + .7827717662f*m - .808675766f*s};
}
bool enabled_slots(const std::vector<PaletteSlot>& slots, std::vector<PaletteSlot>& active, std::string& error)
{
    std::set<std::string> ids;
    for (const auto& slot : slots) {
        if (slot.id.empty() || slot.id.size() > 256 || !ids.insert(slot.id).second || !valid(slot.color)) {
            error = "Palette slots require unique nonempty IDs and normalized RGB colors."; return false;
        }
        if (slot.enabled) active.push_back(slot);
    }
    if (active.empty() || active.size() > 6) {
        error = "Semantic coloring requires between one and six enabled filament slots."; return false;
    }
    std::sort(active.begin(), active.end(), [](const PaletteSlot& a, const PaletteSlot& b) { return a.id < b.id; });
    return true;
}
const PaletteSlot& temporary_slot(const std::vector<PaletteSlot>& active, const Color& original)
{
    const Color source = oklab(original);
    const float source_chroma = std::hypot(source[1], source[2]);
    size_t winner = 0;
    float best = std::numeric_limits<float>::max();
    const bool neutral_available = std::any_of(active.begin(), active.end(), [](const PaletteSlot& slot) {
        const auto color = oklab(slot.color); return std::hypot(color[1], color[2]) < .035f;
    });
    for (size_t index = 0; index < active.size(); ++index) {
        const Color target = oklab(active[index].color);
        const float chroma = std::hypot(target[1], target[2]);
        if (source_chroma < .015f && neutral_available && chroma >= .035f) continue;
        float score = 0.f;
        for (size_t channel = 0; channel < 3; ++channel) {
            const float delta = source[channel] - target[channel]; score += delta * delta;
        }
        const float excess = std::max(0.f, chroma - source_chroma - .025f);
        score += 2.f * excess * excess;
        if (score < best) { best = score; winner = index; }
    }
    return active[winner];
}
} // namespace

const char* region_resolution_status_name(RegionResolutionStatus status)
{
    switch (status) {
    case RegionResolutionStatus::Automatic: return "AUTOMATIC";
    case RegionResolutionStatus::Locked: return "LOCKED";
    case RegionResolutionStatus::TemporarySubstitute: return "TEMPORARY_SUBSTITUTE";
    case RegionResolutionStatus::Ambiguous: return "PALETTE_AMBIGUOUS";
    case RegionResolutionStatus::StaleIntent: return "STALE_REGION_INTENT";
    case RegionResolutionStatus::MissingSlot: return "PALETTE_SLOT_DISABLED";
    case RegionResolutionStatus::NoFeasibleCandidate: return "NO_FEASIBLE_CANDIDATE";
    }
    return "AUTOMATIC";
}

std::string semantic_palette_signature(const std::vector<PaletteSlot>& slots)
{
    std::vector<PaletteSlot> ordered = slots;
    std::sort(ordered.begin(), ordered.end(), [](const auto& lhs, const auto& rhs) { return lhs.id < rhs.id; });
    nlohmann::json document = nlohmann::json::array();
    for (const auto& slot : ordered) document.push_back({slot.id, slot.color, slot.enabled});
    return "palette:" + stable_hash(document.dump());
}

bool map_palette_slots(const MeshSnapshot& source, const Analysis& analysis, const std::vector<PaletteSlot>& slots,
                       const std::vector<Color>& card, const SubfaceBudget& budget,
                       SlotMappingResult& output, std::string& error, const Cancel& cancel)
{
    return map_palette_slots(source, analysis, slots, card, budget, {}, output, error, cancel);
}

Color face_oklab(const MeshSnapshot& source, size_t face_id)
{
    Color rgb {};
    if (source.face_colors.size() == source.mesh.indices.size()) {
        for (size_t channel = 0; channel < 3; ++channel) rgb[channel] = source.face_colors[face_id][channel];
    } else {
        for (int corner = 0; corner < 3; ++corner)
            for (size_t channel = 0; channel < 3; ++channel)
                rgb[channel] += source.vertex_colors[source.mesh.indices[face_id][corner]][channel] / 3.f;
    }
    return oklab(rgb);
}

double face_area(const MeshSnapshot& source, size_t face_id)
{
    const auto& face = source.mesh.indices[face_id];
    return .5 * double((source.mesh.vertices[face[1]] - source.mesh.vertices[face[0]])
        .cross(source.mesh.vertices[face[2]] - source.mesh.vertices[face[0]]).norm());
}

void populate_region_evidence(const MeshSnapshot& source, const std::string& signature,
                              const std::vector<size_t>& face_material_ids,
                              std::vector<MaterialCenter>& centers)
{
    struct Sample { Color color; double weight; size_t face_id; };
    std::vector<std::vector<Sample>> samples(centers.size());
    std::vector<std::vector<size_t>> faces(centers.size());
    std::vector<Color> face_colors(source.mesh.indices.size());
    for (size_t face_id = 0; face_id < face_material_ids.size(); ++face_id) {
        face_colors[face_id] = face_oklab(source, face_id);
        const size_t material = face_material_ids[face_id];
        if (material >= centers.size()) continue;
        const double weight = std::max(1e-12, face_area(source, face_id));
        samples[material].push_back({face_colors[face_id], weight, face_id});
        faces[material].push_back(face_id);
    }
    for (size_t material = 0; material < centers.size(); ++material) {
        auto& center = centers[material];
        center.region_id = whole_region_id(signature, center.label, faces[material]);
        const auto& values = samples[material];
        if (values.empty()) continue;
        double total = 0.0;
        for (const auto& sample : values) total += sample.weight;
        const auto quantile = [&](size_t channel, double q) {
            std::vector<std::pair<float,double>> ordered;
            for (const auto& sample : values) ordered.emplace_back(sample.color[channel], sample.weight);
            std::sort(ordered.begin(), ordered.end());
            double cumulative = 0.0;
            for (const auto& item : ordered) {
                cumulative += item.second;
                if (cumulative >= total * q) return item.first;
            }
            return ordered.back().first;
        };
        for (size_t channel = 0; channel < 3; ++channel) {
            center.q05_oklab[channel] = quantile(channel, .05);
            center.q50_oklab[channel] = quantile(channel, .50);
            center.q95_oklab[channel] = quantile(channel, .95);
        }
        const Sample* medoid = &values.front();
        float nearest = std::numeric_limits<float>::max();
        double dispersion = 0.0, dominant = 0.0;
        for (const auto& sample : values) {
            float squared = 0.f;
            for (size_t channel = 0; channel < 3; ++channel) {
                const float delta = sample.color[channel] - center.q50_oklab[channel];
                squared += delta * delta;
            }
            dispersion += sample.weight * std::sqrt(squared);
            if (squared < nearest || (squared == nearest && sample.face_id < medoid->face_id)) {
                nearest = squared; medoid = &sample;
            }
        }
        center.medoid_oklab = medoid->color;
        for (const auto& sample : values) {
            float squared = 0.f;
            for (size_t channel = 0; channel < 3; ++channel) {
                const float delta = sample.color[channel] - center.medoid_oklab[channel];
                squared += delta * delta;
            }
            if (squared <= .02f * .02f) dominant += sample.weight;
        }
        center.color_dispersion = float(dispersion / total);
        center.dominant_ratio = float(dominant / total);
    }

    std::map<std::pair<int,int>, size_t> edge_owner;
    std::vector<double> gradient_sum(centers.size()), gradient_weight(centers.size());
    for (size_t face_id = 0; face_id < source.mesh.indices.size(); ++face_id) {
        const auto& face = source.mesh.indices[face_id];
        for (int edge = 0; edge < 3; ++edge) {
            int a = face[edge], b = face[(edge + 1) % 3];
            if (a > b) std::swap(a, b);
            const auto inserted = edge_owner.emplace(std::make_pair(a,b), face_id);
            if (inserted.second) continue;
            const size_t other = inserted.first->second;
            const size_t material = face_id < face_material_ids.size() ? face_material_ids[face_id] : centers.size();
            if (material >= centers.size() || other >= face_material_ids.size() || face_material_ids[other] != material) continue;
            float squared = 0.f;
            for (size_t channel = 0; channel < 3; ++channel) {
                const float delta = face_colors[face_id][channel] - face_colors[other][channel];
                squared += delta * delta;
            }
            gradient_sum[material] += std::sqrt(squared);
            gradient_weight[material] += 1.0;
        }
    }
    for (size_t material = 0; material < centers.size(); ++material)
        if (gradient_weight[material] > 0.0)
            centers[material].gradient_strength = float(gradient_sum[material] / gradient_weight[material]);
}

Label leaf_label(const Analysis& analysis, size_t face_id, const SubfacePath& path)
{
    const SubfaceLabelEvidence* best = nullptr;
    for (const auto& evidence : analysis.subface_labels) {
        if (evidence.face_id != face_id || evidence.path.depth > path.depth) continue;
        const unsigned shift = 2u * unsigned(path.depth - evidence.path.depth);
        if (uint8_t(path.value >> shift) != evidence.path.value) continue;
        if (!best || evidence.path.depth > best->path.depth) best = &evidence;
    }
    return best ? best->label : (face_id < analysis.face_labels.size() ? analysis.face_labels[face_id] : Label::Unknown);
}

bool map_palette_slots(const MeshSnapshot& source, const Analysis& analysis, const std::vector<PaletteSlot>& slots,
                       const std::vector<Color>& card, const SubfaceBudget& budget,
                       const std::vector<RegionColorOverride>& overrides,
                       SlotMappingResult& output, std::string& error, const Cancel& cancel)
{
    output = {}; error.clear();
    std::vector<PaletteSlot> active;
    if (!enabled_slots(slots, active, error)) return false;
    if (analysis.canceled || !analysis.error.empty() || analysis.geometry_id != source.geometry_id ||
        analysis.content_id != source.content_id || analysis.signature != analysis_cache_key(source,
            analysis.body_identity, analysis.face_identity,
            analysis.boundary_identity.empty() ? "none" : analysis.boundary_identity,
            analysis.pose_identity.empty() ? "none" : analysis.pose_identity) ||
        analysis.face_labels.size() != source.mesh.indices.size() ||
        analysis.face_confidence.size() != source.mesh.indices.size()) {
        error = "Palette mapping requires current recognition evidence."; return false;
    }
    if (cancel && cancel()) { error="Material mapping canceled."; return false; }
    // Non-human fallback has deliberately empty semantic overlays.
    if (!analysis.person_detected) return true;
    std::vector<Color> palette;
    for (const auto& slot : active) palette.push_back(slot.color);
    SlotMappingResult mapped;
    mapped.analysis_signature = analysis.signature;
    mapped.palette_signature = semantic_palette_signature(slots);
    mapped.portrait_card = card;
    std::vector<size_t> face_material_ids;
    mapped.faces = map_palette_materials(source, analysis, palette, card, mapped.material_centers,
                                         face_material_ids, cancel);
    populate_region_evidence(source, analysis.signature, face_material_ids, mapped.material_centers);
    std::vector<std::string> face_region_ids(face_material_ids.size());
    for (size_t face_id = 0; face_id < face_material_ids.size(); ++face_id)
        if (face_material_ids[face_id] < mapped.material_centers.size())
            face_region_ids[face_id] = mapped.material_centers[face_material_ids[face_id]].region_id;

    std::map<std::string, const RegionColorOverride*> region_overrides;
    std::map<std::string, PaletteSlot> resolved_override_slots;
    std::map<std::string, size_t> resolution_by_region;
    std::map<std::string, PaletteSlot> known_slots;
    for (const auto& slot : slots) known_slots.emplace(slot.id, slot);
    for (const auto& override : overrides) {
        mapped.region_overrides.push_back(override);
        std::string region_id = override.region_id;
        if (override.analysis_signature != analysis.signature) {
            mapped.resolved_regions.push_back({region_id, override.slot_id, {}, {}, RegionResolutionStatus::StaleIntent});
            continue;
        }
        if (region_id.empty() && override.material_center_id < mapped.material_centers.size())
            region_id = mapped.material_centers[override.material_center_id].region_id;
        const auto known = known_slots.find(override.slot_id);
        if (region_id.empty()) {
            mapped.resolved_regions.push_back({{}, override.slot_id, {}, {}, RegionResolutionStatus::StaleIntent});
            continue;
        }
        if (known == known_slots.end()) {
            mapped.resolved_regions.push_back({region_id, override.slot_id, {}, {}, RegionResolutionStatus::MissingSlot});
            continue;
        }
        if (!region_overrides.emplace(region_id, &override).second) {
            mapped.resolved_regions.push_back({region_id, override.slot_id, {}, {}, RegionResolutionStatus::Ambiguous});
            continue;
        }
        const auto active_found = std::find_if(active.begin(), active.end(), [&](const auto& slot) {
            return slot.id == override.slot_id;
        });
        const Color intended = override.has_target_color ? override.target_color : known->second.color;
        const PaletteSlot actual = active_found == active.end() ? temporary_slot(active, intended) : *active_found;
        resolved_override_slots.emplace(region_id, actual);
        resolution_by_region[region_id] = mapped.resolved_regions.size();
        mapped.resolved_regions.push_back({region_id, override.slot_id, actual.id, actual.color,
            active_found == active.end() ? RegionResolutionStatus::TemporarySubstitute : RegionResolutionStatus::Locked});
    }
    const auto apply_region_override = [&](FaceColors& colors) {
        for (auto& face : colors) {
            if (face.first >= face_region_ids.size()) continue;
            const auto found = resolved_override_slots.find(face_region_ids[face.first]);
            if (found != resolved_override_slots.end()) face.second = found->second.color;
        }
    };
    apply_region_override(mapped.faces);
    // A boundary candidate can legitimately change a coarse face root from
    // skin to hair, but it must never erase a stable baseline eye or brow
    // root. The subface mapper preserves safe children; restore their parent
    // material here so preview and import use the same protected tree.
    if (!analysis.baseline_face_labels.empty() &&
        analysis.baseline_face_labels.size() == source.mesh.indices.size()) {
        Analysis baseline = analysis;
        baseline.face_labels = analysis.baseline_face_labels;
        baseline.face_confidence = analysis.baseline_face_confidence;
        baseline.subface_labels = analysis.baseline_subface_labels;
        baseline.baseline_face_labels.clear(); baseline.baseline_face_confidence.clear();
        baseline.baseline_subface_labels.clear();
        const auto protected_detail = [](Label label) {
            return label == Label::EyeSclera || label == Label::Iris || label == Label::Eyebrow ||
                   label == Label::Lips || label == Label::MouthInterior;
        };
        FaceColors missing_roots;
        for (size_t face_id = 0; face_id < baseline.face_labels.size(); ++face_id) {
            const Label label = baseline.face_labels[face_id];
            if (!protected_detail(label)) continue;
            const auto decision = decide_region_palette(face_oklab(source, face_id), label, palette, card);
            const size_t selected = decision.selected_index;
            if (selected == palette.size()) continue;
            const auto current = std::lower_bound(mapped.faces.begin(), mapped.faces.end(), face_id,
                [](const auto& item, size_t id) { return item.first < id; });
            const std::pair<size_t, Color> safe {face_id, palette[selected]};
            if (current == mapped.faces.end() || current->first != face_id) missing_roots.push_back(safe);
            else current->second = safe.second;
        }
        mapped.faces.insert(mapped.faces.end(), missing_roots.begin(), missing_roots.end());
        std::sort(mapped.faces.begin(), mapped.faces.end(),
                  [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
    }
    // Explicit region edits have higher priority than the protected-detail
    // baseline restoration above. They still remain restricted to their
    // discovered original material faces.
    apply_region_override(mapped.faces);
    SubfaceBudgetResult leaves;
    if (!map_subface_palette(source, analysis, mapped.faces, palette, card, budget, leaves, error, cancel)) return false;
    mapped.subfaces = std::move(leaves.accepted);
    std::vector<std::string> subface_region_ids;
    subface_region_ids.reserve(mapped.subfaces.size());
    for (auto& leaf : mapped.subfaces) {
        const Label label = leaf_label(analysis, leaf.face_id, leaf.path);
        const std::string region_id = child_region_id(analysis.signature, label, leaf.face_id, leaf.path);
        subface_region_ids.push_back(region_id);
        const auto found = resolved_override_slots.find(region_id);
        if (found != resolved_override_slots.end()) leaf.color = found->second.color;
    }
    mapped.added_triangles = leaves.added_triangles; mapped.rejected_candidates = leaves.rejected_candidates;
    if (mapped.rejected_candidates > 0 && !analysis.baseline_face_labels.empty()) {
        // The subface budgeter retains the known-safe depth-2 tree on every
        // rejected face. Restore only those roots; accepted upgrades on other
        // faces retain their own material and geometry.
        std::set<size_t> rejected_upgrade_faces;
        for (const auto& evidence : analysis.subface_labels)
            if (evidence.path.depth == 3) rejected_upgrade_faces.insert(evidence.face_id);
        for (const auto& leaf : mapped.subfaces)
            if (leaf.path.depth == 3) rejected_upgrade_faces.erase(leaf.face_id);
        if (!rejected_upgrade_faces.empty()) {
            Analysis baseline = analysis;
            baseline.face_labels = analysis.baseline_face_labels;
            baseline.face_confidence = analysis.baseline_face_confidence;
            baseline.subface_labels = analysis.baseline_subface_labels;
            baseline.baseline_face_labels.clear(); baseline.baseline_face_confidence.clear();
            baseline.baseline_subface_labels.clear();
            const FaceColors safe_roots = map_palette(source, baseline, palette, card);
            for (auto& face : mapped.faces) {
                if (!rejected_upgrade_faces.count(face.first)) continue;
                const auto safe = std::lower_bound(safe_roots.begin(), safe_roots.end(), face.first,
                    [](const auto& item, size_t id) { return item.first < id; });
                if (safe != safe_roots.end() && safe->first == face.first) face.second = safe->second;
            }
        }
        mapped.boundary_budget_fallback = true;
    }
    const auto slot_for = [&](const Color& color, const std::string& region_id,
                              const Color& original, Label label) -> const PaletteSlot* {
        const auto override = resolved_override_slots.find(region_id);
        if (override != resolved_override_slots.end()) {
            const auto found = std::find_if(active.begin(), active.end(), [&](const auto& slot) {
                return slot.id == override->second.id;
            });
            if (found != active.end()) return &*found;
        }
        const auto decision = decide_region_palette(original, label, palette, card);
        if (decision.selected_index < active.size() && active[decision.selected_index].color == color)
            return &active[decision.selected_index];
        const auto found = std::find_if(active.begin(), active.end(), [&](const PaletteSlot& slot) { return slot.color == color; });
        return found == active.end() ? nullptr : &*found;
    };
    for (const auto& face : mapped.faces) {
        const size_t material_id = face.first < face_material_ids.size() ? face_material_ids[face.first] : 0;
        const MaterialCenter* center = material_id < mapped.material_centers.size() ? &mapped.material_centers[material_id] : nullptr;
        const std::string region_id = center ? center->region_id : std::string();
        const Color original = center ? center->original_oklab : face_oklab(source, face.first);
        const Label label = center ? center->label : analysis.face_labels[face.first];
        const auto* slot = slot_for(face.second, region_id, original, label);
        if (!slot) { error = "Semantic face target is absent from enabled slots."; return false; }
        const auto region = region_overrides.find(region_id);
        const std::string intended = region != region_overrides.end() ? region->second->slot_id : slot->id;
        mapped.face_slots.push_back({face.first, slot->id, intended, slot->color, material_id, region_id});
    }
    for (size_t index = 0; index < mapped.subfaces.size(); ++index) {
        const auto& leaf = mapped.subfaces[index];
        const std::string& region_id = subface_region_ids[index];
        const Label label = leaf_label(analysis, leaf.face_id, leaf.path);
        const Color original = face_oklab(source, leaf.face_id);
        const auto* slot = slot_for(leaf.color, region_id, original, label);
        if (!slot) { error = "Semantic child target is absent from enabled slots."; return false; }
        const size_t material_id = leaf.face_id < face_material_ids.size() ? face_material_ids[leaf.face_id] : 0;
        const auto region = region_overrides.find(region_id);
        const std::string intended = region != region_overrides.end() ? region->second->slot_id : slot->id;
        mapped.subface_slots.push_back({leaf.face_id, leaf.path, slot->id, leaf.confidence,
                                        intended, slot->color, material_id, region_id});
    }
    std::set<std::string> described_regions;
    for (const auto& resolved : mapped.resolved_regions) described_regions.insert(resolved.region_id);
    std::map<std::string, size_t> first_face_by_region;
    for (size_t index = 0; index < mapped.face_slots.size(); ++index)
        first_face_by_region.emplace(mapped.face_slots[index].region_id, index);
    for (const auto& center : mapped.material_centers) {
        if (center.region_id.empty() || described_regions.count(center.region_id)) continue;
        const auto assignment = first_face_by_region.find(center.region_id);
        if (assignment == first_face_by_region.end()) continue;
        const auto& face = mapped.face_slots[assignment->second];
        const auto decision = decide_region_palette(center.original_oklab, center.label, palette, card);
        mapped.resolved_regions.push_back({center.region_id, face.intended_slot_id, face.slot_id,
            face.intended_color, decision.ambiguous ? RegionResolutionStatus::Ambiguous :
                                                            RegionResolutionStatus::Automatic});
        described_regions.insert(center.region_id);
    }
    for (const auto& leaf : mapped.subface_slots) {
        if (leaf.region_id.empty() || described_regions.count(leaf.region_id)) continue;
        mapped.resolved_regions.push_back({leaf.region_id,leaf.intended_slot_id,leaf.slot_id,leaf.intended_color,
                                           RegionResolutionStatus::Automatic});
        described_regions.insert(leaf.region_id);
    }
    std::set<std::string> realized_regions;
    for (const auto& face : mapped.face_slots) realized_regions.insert(face.region_id);
    for (const auto& leaf : mapped.subface_slots) realized_regions.insert(leaf.region_id);
    for (const auto& item : resolution_by_region)
        if (!realized_regions.count(item.first)) mapped.resolved_regions[item.second].status = RegionResolutionStatus::StaleIntent;
    output = std::move(mapped); return true;
}

bool map_palette_slots(const MeshSnapshot& source, const Analysis& analysis, const std::vector<PaletteSlot>& slots,
                       const std::vector<Color>& card, const std::vector<RegionColorOverride>& overrides,
                       const SubfaceBudget& budget, SlotMappingResult& output, std::string& error,
                       const Cancel& cancel)
{
    return map_palette_slots(source, analysis, slots, card, budget, overrides, output, error, cancel);
}

std::vector<RegionColorRecommendation> recommend_region_slots(const SlotMappingResult& mapping,
                                                              const std::vector<PaletteSlot>& slots)
{
    std::vector<RegionColorRecommendation> result;
    std::vector<PaletteSlot> candidates;
    for (const auto& slot : slots)
        if (slot.enabled && !slot.id.empty() && valid(slot.color)) candidates.push_back(slot);
    if (candidates.empty()) return result;
    std::sort(candidates.begin(), candidates.end(), [](const PaletteSlot& lhs, const PaletteSlot& rhs) {
        return lhs.id < rhs.id;
    });
    std::vector<Color> palette;
    for (const auto& slot : candidates) palette.push_back(slot.color);
    for (const auto& center : mapping.material_centers) {
        RegionColorRecommendation recommendation;
        recommendation.analysis_signature = mapping.analysis_signature;
        recommendation.material_center_id = center.id;
        recommendation.region_id = center.region_id;
        recommendation.label = center.label;
        recommendation.original_rgb = center.original_rgb;
        recommendation.original_oklab = center.original_oklab;
        recommendation.face_count = center.face_count;
        recommendation.surface_area = center.surface_area;
        const auto decision = decide_region_palette(center.original_oklab, center.label, palette,
                                                    mapping.portrait_card);
        recommendation.top_score = decision.best_cost;
        recommendation.second_score = decision.second_cost;
        recommendation.score_margin = decision.score_margin;
        recommendation.ambiguous = decision.ambiguous;
        for (const auto& item : decision.candidates) {
            const auto& slot = candidates[item.palette_index];
            RegionColorCandidate candidate;
            candidate.slot_id = slot.id;
            candidate.color = slot.color;
            candidate.score = item.total_cost;
            candidate.source_cost = item.source_cost;
            candidate.semantic_cost = item.semantic_cost;
            candidate.role_bonus = item.role_bonus;
            candidate.accepted = item.accepted;
            candidate.rejection_reason = palette_decision_reason_name(item.reason);
            recommendation.candidates.push_back(std::move(candidate));
        }
        std::stable_sort(recommendation.candidates.begin(), recommendation.candidates.end(),
                         [](const RegionColorCandidate& lhs, const RegionColorCandidate& rhs) {
                             if (lhs.accepted != rhs.accepted) return lhs.accepted > rhs.accepted;
                             if (lhs.score != rhs.score) return lhs.score < rhs.score;
                             return lhs.slot_id < rhs.slot_id;
                         });
        const auto assignment = std::find_if(mapping.face_slots.begin(), mapping.face_slots.end(),
                                             [&](const FaceSlotAssignment& value) {
                                                 return !center.region_id.empty() ? value.region_id == center.region_id :
                                                     value.material_center_id == center.id;
                                             });
        if (assignment != mapping.face_slots.end()) recommendation.recommended_slot_id = assignment->slot_id;
        else {
            const auto accepted = std::find_if(recommendation.candidates.begin(), recommendation.candidates.end(),
                                               [](const RegionColorCandidate& value) { return value.accepted; });
            if (accepted != recommendation.candidates.end()) recommendation.recommended_slot_id = accepted->slot_id;
        }
        result.push_back(std::move(recommendation));
    }
    return result;
}

bool remap_palette_slots(const SlotMappingResult& original, const std::vector<PaletteSlot>& slots,
                         SlotMappingResult& output, std::string& error)
{
    error.clear();
    std::vector<PaletteSlot> active;
    if (!enabled_slots(slots, active, error)) { output = {}; return false; }
    if (original.faces.size() != original.face_slots.size() || original.subfaces.size() != original.subface_slots.size()) {
        error = "Stable slot assignments are incomplete."; output = {}; return false;
    }
    SlotMappingResult mapped = original;
    mapped.faces.clear(); mapped.subfaces.clear(); mapped.substituted_assignments = 0;
    mapped.palette_signature = semantic_palette_signature(slots);
    const auto resolve = [&](auto& assignment) -> const PaletteSlot& {
        if (assignment.intended_slot_id.empty()) assignment.intended_slot_id = assignment.slot_id;
        const auto found = std::find_if(active.begin(), active.end(), [&](const PaletteSlot& slot) {
            return slot.id == assignment.intended_slot_id;
        });
        const auto& target = found != active.end() ? *found : temporary_slot(active, assignment.intended_color);
        if (found == active.end()) ++mapped.substituted_assignments;
        assignment.slot_id = target.id;
        // Explicit edits update the intent color while its intended slot exists.
        if (found != active.end()) assignment.intended_color = target.color;
        return target;
    };
    for (auto& face : mapped.face_slots) {
        if (face.slot_id.empty() || !valid(face.intended_color)) { error = "Invalid stable face assignment."; output = {}; return false; }
        const auto& slot = resolve(face); mapped.faces.emplace_back(face.face_id, slot.color);
    }
    for (auto& leaf : mapped.subface_slots) {
        if (leaf.slot_id.empty() || !valid(leaf.intended_color)) { error = "Invalid stable child assignment."; output = {}; return false; }
        const auto& slot = resolve(leaf); mapped.subfaces.push_back({leaf.face_id, leaf.path, slot.color, leaf.confidence});
    }
    output = std::move(mapped); return true;
}

bool suggest_material_slots(const std::vector<MaterialCenter>& centers, size_t requested_count,
                            const std::vector<PaletteSlot>& locked_slots,
                            std::vector<PaletteSlot>& output, std::string& error)
{
    output.clear(); error.clear();
    if (requested_count == 0 || requested_count > 6 || locked_slots.size() > 6) {
        error = "Automatic material suggestions require one through six colors."; return false;
    }
    std::set<std::string> selected_ids;
    std::vector<PaletteSlot> selected = locked_slots;
    for (const auto& slot : selected)
        if (!slot.enabled || slot.id.empty() || !valid(slot.color) || !selected_ids.insert(slot.id).second) {
            error = "Locked suggestions require enabled unique slots and normalized colors."; return false;
        }
    std::sort(selected.begin(), selected.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    const size_t target_count = std::max(requested_count, selected.size());
    const auto semantic_group = [](Label label) { return label == Label::BodySkin ? Label::FaceSkin : label; };
    const auto salience_floor = [](Label label) {
        switch (label) {
        case Label::FaceSkin: case Label::Hair: return .12;
        case Label::Lips: case Label::EyeSclera: return .08;
        case Label::Iris: return .05;
        case Label::Eyebrow: return .04;
        default: return 0.0;
        }
    };
    std::array<size_t, label_count> label_centers {};
    double total_area = 0.0;
    std::set<size_t> material_ids;
    for (const auto& center : centers) {
        if (size_t(center.label) >= label_count || !valid(center.original_rgb) ||
            !std::isfinite(center.surface_area) || center.surface_area < 0.0 ||
            !material_ids.insert(center.id).second ||
            !std::all_of(center.original_oklab.begin(), center.original_oklab.end(), [](float v) { return std::isfinite(v); })) {
            error = "Invalid source material evidence for suggestions."; return false;
        }
        if (center.surface_area == 0.0 || center.face_count == 0) continue;
        total_area += center.surface_area;
        ++label_centers[size_t(semantic_group(center.label))];
    }
    if (total_area <= 0.0) {
        error = "Automatic material suggestions require source surface-area evidence."; return false;
    }
    struct SupportedColor { const MaterialCenter* representative; double weight; };
    std::map<std::array<int,3>, SupportedColor> bins;
    // This bounds medoid scoring only. Source-material discovery remains the
    // frozen semantic stage; we do not recluster pixels as N changes.
    for (const auto& center : centers) {
        if (center.surface_area == 0.0 || center.face_count == 0) continue;
        const auto group = semantic_group(center.label);
        const double weight = std::max(center.surface_area / total_area,
                                      salience_floor(group) / double(label_centers[size_t(group)]));
        std::array<int,3> key {};
        for (size_t channel = 0; channel < 3; ++channel)
            key[channel] = int(std::floor(center.original_oklab[channel] / .02f));
        auto found = bins.emplace(key, SupportedColor{&center, 0.0}).first;
        found->second.weight += weight;
        if (center.surface_area > found->second.representative->surface_area ||
            (center.surface_area == found->second.representative->surface_area && center.id < found->second.representative->id))
            found->second.representative = &center;
    }
    std::vector<SupportedColor> candidates;
    for (const auto& bin : bins) candidates.push_back(bin.second);
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        return a.weight != b.weight ? a.weight > b.weight : a.representative->id < b.representative->id;
    });
    if (candidates.size() > 256) candidates.resize(256);
    const auto distance_squared = [](const Color& a, const Color& b) {
        double score = 0.0;
        for (size_t channel = 0; channel < 3; ++channel) {
            const double delta = double(a[channel]) - b[channel]; score += delta * delta;
        }
        return score;
    };
    std::vector<Color> selected_labs;
    for (const auto& slot : selected) selected_labs.push_back(oklab(slot.color));
    std::vector<double> nearest;
    nearest.reserve(bins.size());
    for (const auto& bin : bins) {
        double best = std::numeric_limits<double>::infinity();
        for (const auto& color : selected_labs)
            best = std::min(best, distance_squared(bin.second.representative->original_oklab, color));
        nearest.push_back(best);
    }
    while (selected.size() < target_count) {
        const MaterialCenter* winner = nullptr;
        double best_cost = std::numeric_limits<double>::infinity();
        for (const auto& candidate : candidates) {
            const auto& center = *candidate.representative;
            if (selected_ids.count("material-" + std::to_string(center.id)) != 0) continue;
            if (std::any_of(selected_labs.begin(), selected_labs.end(), [&](const auto& color) {
                    return distance_squared(center.original_oklab, color) < .035 * .035;
                })) continue;
            double total_cost = 0.0;
            size_t index = 0;
            for (const auto& bin : bins) {
                total_cost += bin.second.weight * std::min(nearest[index++],
                    distance_squared(bin.second.representative->original_oklab, center.original_oklab));
            }
            if (total_cost < best_cost || (total_cost == best_cost && winner && center.id < winner->id)) {
                winner = &center; best_cost = total_cost;
            }
        }
        if (!winner) break;
        const std::string id = "material-" + std::to_string(winner->id);
        selected.push_back({id, winner->original_rgb, true}); selected_ids.insert(id);
        selected_labs.push_back(winner->original_oklab);
        size_t index = 0;
        for (const auto& bin : bins) {
            nearest[index] = std::min(nearest[index],
                distance_squared(bin.second.representative->original_oklab, winner->original_oklab));
            ++index;
        }
    }
    output = std::move(selected); return true;
}

nlohmann::json encode_slot_mapping(const SlotMappingResult& mapping)
{
    nlohmann::json faces = nlohmann::json::array(), leaves = nlohmann::json::array(), centers = nlohmann::json::array();
    for (const auto& face : mapping.face_slots)
        faces.push_back({face.face_id, face.slot_id, face.intended_slot_id, face.intended_color,
                         face.material_center_id, face.region_id});
    for (const auto& leaf : mapping.subface_slots)
        leaves.push_back({leaf.face_id, leaf.path.depth, leaf.path.value, leaf.slot_id, leaf.confidence,
                          leaf.intended_slot_id, leaf.intended_color, leaf.material_center_id, leaf.region_id});
    for (const auto& center : mapping.material_centers)
        centers.push_back({center.id, size_t(center.label), center.original_rgb, center.original_oklab,
            center.face_count, center.surface_area, center.region_id, center.q05_oklab, center.q50_oklab,
            center.q95_oklab, center.medoid_oklab, center.color_dispersion, center.dominant_ratio,
            center.gradient_strength});
    nlohmann::json overrides = nlohmann::json::array();
    for (const auto& item : mapping.region_overrides)
        overrides.push_back({{"analysis_signature",item.analysis_signature},{"material_center_id",item.material_center_id},
            {"slot_uid",item.slot_id},{"locked",item.locked},{"region_id",item.region_id},
            {"target_color",item.target_color},{"has_target_color",item.has_target_color},
            {"semantic_role",size_t(item.semantic_role)}});
    nlohmann::json resolved = nlohmann::json::array();
    for (const auto& item : mapping.resolved_regions)
        resolved.push_back({item.region_id,item.intended_slot_id,item.actual_slot_id,item.actual_color,size_t(item.status)});
    return {{"schema", material_mapping_version}, {"analysis_signature", mapping.analysis_signature},
            {"faces", std::move(faces)}, {"subfaces", std::move(leaves)}, {"centers", std::move(centers)},
            {"region_overrides", std::move(overrides)}, {"resolved_regions",std::move(resolved)},
            {"palette_signature",mapping.palette_signature},{"portrait_card",mapping.portrait_card},
            {"boundary_budget_fallback", mapping.boundary_budget_fallback}};
}

bool decode_slot_mapping(const nlohmann::json& doc, size_t face_count, const std::vector<PaletteSlot>& slots,
                         SlotMappingResult& output, std::string& error)
{
    output = {}; error.clear();
    try {
        if ((doc.at("schema") != material_mapping_version && doc.at("schema") != "orca.semantic-material-mapping/v5" &&
             doc.at("schema") != "orca.semantic-material-mapping/v4" &&
             doc.at("schema") != "orca.semantic-material-mapping/v3" && doc.at("schema") != "orca.semantic-material-mapping/v2") ||
            face_count == 0 || face_count > 2000000)
            throw std::invalid_argument("Invalid material mapping version or geometry size.");
        SlotMappingResult restored;
        if (doc.contains("analysis_signature")) {
            if (!doc["analysis_signature"].is_string()) throw std::invalid_argument("Invalid analysis signature.");
            restored.analysis_signature = doc["analysis_signature"].get<std::string>();
        }
        if (doc.contains("palette_signature")) restored.palette_signature = doc.at("palette_signature").get<std::string>();
        if (doc.contains("portrait_card")) restored.portrait_card = doc.at("portrait_card").get<std::vector<Color>>();
        if (doc.contains("boundary_budget_fallback")) {
            if (!doc["boundary_budget_fallback"].is_boolean()) throw std::invalid_argument("Invalid boundary fallback flag.");
            restored.boundary_budget_fallback = doc["boundary_budget_fallback"].get<bool>();
        }
        const auto& faces = doc.at("faces"); const auto& leaves = doc.at("subfaces");
        if (!faces.is_array() || faces.size() > face_count || !leaves.is_array() || leaves.size() > 1000000)
            throw std::invalid_argument("Invalid material assignment count.");
        std::set<size_t> seen_faces;
        std::set<std::pair<size_t, SubfacePath>> seen_leaves;
        std::set<std::tuple<size_t, uint8_t, uint8_t>> split_nodes;
        for (const auto& entry : faces) {
            if (!entry.is_array() || (entry.size() != 4 && entry.size() != 5 && entry.size() != 6))
                throw std::invalid_argument("Invalid face assignment.");
            FaceSlotAssignment face {entry[0].get<size_t>(), entry[1].get<std::string>(),
                                     entry[2].get<std::string>(), entry[3].get<Color>(),
                                     entry.size() >= 5 ? entry[4].get<size_t>() : 0,
                                     entry.size() == 6 ? entry[5].get<std::string>() : std::string()};
            if (face.face_id >= face_count || !seen_faces.insert(face.face_id).second)
                throw std::invalid_argument("Invalid or duplicate face identity.");
            restored.face_slots.push_back(face); restored.faces.emplace_back(face.face_id, face.intended_color);
        }
        for (const auto& entry : leaves) {
            if (!entry.is_array() || (entry.size() != 7 && entry.size() != 8 && entry.size() != 9))
                throw std::invalid_argument("Invalid child assignment.");
            const unsigned depth = entry[1].get<unsigned>(), value = entry[2].get<unsigned>();
            if (depth == 0 || depth > 3 || value >= (1u << (depth * 2u)))
                throw std::invalid_argument("Invalid child path.");
            SubfaceSlotAssignment leaf {entry[0].get<size_t>(), {uint8_t(depth), uint8_t(value)},
                entry[3].get<std::string>(), entry[4].get<float>(), entry[5].get<std::string>(), entry[6].get<Color>(),
                entry.size() >= 8 ? entry[7].get<size_t>() : 0,
                entry.size() == 9 ? entry[8].get<std::string>() : std::string()};
            if (leaf.face_id >= face_count || !std::isfinite(leaf.confidence) || leaf.confidence < 0.f ||
                leaf.confidence > 1.f || !seen_leaves.emplace(leaf.face_id, leaf.path).second)
                throw std::invalid_argument("Invalid or duplicate child assignment.");
            restored.subface_slots.push_back(leaf);
            restored.subfaces.push_back({leaf.face_id, leaf.path, leaf.intended_color, leaf.confidence});
            for (uint8_t ancestor = 0; ancestor < leaf.path.depth; ++ancestor)
                split_nodes.emplace(leaf.face_id, ancestor, ancestor == 0 ? 0 :
                    uint8_t(leaf.path.value >> (2u * (leaf.path.depth - ancestor))));
        }
        for (const auto& item : seen_leaves)
            for (uint8_t depth = 1; depth < item.second.depth; ++depth)
                if (seen_leaves.count({item.first, {depth, uint8_t(item.second.value >> (2u * (item.second.depth - depth)))}}))
                    throw std::invalid_argument("Material child paths overlap an ancestor.");
        restored.added_triangles = split_nodes.size() * 3;
        const auto& centers = doc.at("centers");
        if (!centers.is_array() || centers.size() > face_count * 6)
            throw std::invalid_argument("Invalid material center count.");
        for (const auto& entry : centers) {
            if (!entry.is_array() || (entry.size() != 5 && entry.size() != 6 && entry.size() != 14) ||
                entry[1].get<size_t>() >= label_count)
                throw std::invalid_argument("Invalid material center.");
            MaterialCenter center {entry[0].get<size_t>(), Label(entry[1].get<size_t>()), entry[2].get<Color>(),
                                    entry[3].get<Color>(), entry[4].get<size_t>(), entry.size() >= 6 ? entry[5].get<double>() : 0.0};
            if (entry.size() == 14) {
                center.region_id=entry[6].get<std::string>(); center.q05_oklab=entry[7].get<Color>();
                center.q50_oklab=entry[8].get<Color>(); center.q95_oklab=entry[9].get<Color>();
                center.medoid_oklab=entry[10].get<Color>(); center.color_dispersion=entry[11].get<float>();
                center.dominant_ratio=entry[12].get<float>(); center.gradient_strength=entry[13].get<float>();
            }
            if (!valid(center.original_rgb) || center.face_count > face_count ||
                !std::isfinite(center.surface_area) || center.surface_area < 0.0 ||
                !std::all_of(center.original_oklab.begin(), center.original_oklab.end(), [](float v) { return std::isfinite(v); }))
                throw std::invalid_argument("Invalid material center appearance.");
            restored.material_centers.push_back(center);
        }
        if (doc.contains("region_overrides")) {
            const auto& overrides = doc["region_overrides"];
            if (!overrides.is_array() || overrides.size() >
                restored.material_centers.size() + restored.subface_slots.size())
                throw std::invalid_argument("Invalid region override count.");
            std::set<std::string> seen_regions;
            for (const auto& entry : overrides) {
                RegionColorOverride item;
                if (entry.is_array() && entry.size() == 4 && entry[0].is_string()) {
                    item={entry[0].get<std::string>(),entry[1].get<size_t>(),entry[2].get<std::string>(),entry[3].get<bool>()};
                    if (item.material_center_id < restored.material_centers.size())
                        item.region_id=restored.material_centers[item.material_center_id].region_id;
                } else if (entry.is_object()) {
                    item.analysis_signature=entry.at("analysis_signature").get<std::string>();
                    item.material_center_id=entry.value("material_center_id",size_t(0));
                    item.slot_id=entry.at("slot_uid").get<std::string>(); item.locked=entry.value("locked",true);
                    item.region_id=entry.value("region_id",std::string());
                    item.target_color=entry.value("target_color",Color{});
                    item.has_target_color=entry.value("has_target_color",false);
                    item.semantic_role=Label(entry.value("semantic_role",size_t(Label::Unknown)));
                } else throw std::invalid_argument("Invalid region color override.");
                const std::string identity=item.region_id.empty()?"legacy:"+std::to_string(item.material_center_id):item.region_id;
                if (item.analysis_signature.empty() || item.slot_id.empty() || !seen_regions.insert(identity).second)
                    throw std::invalid_argument("Invalid or duplicate region color override.");
                restored.region_overrides.push_back(std::move(item));
            }
        }
        if (doc.contains("resolved_regions")) for (const auto& entry : doc.at("resolved_regions")) {
            if (!entry.is_array() || entry.size()!=5 || entry[4].get<size_t>()>size_t(RegionResolutionStatus::NoFeasibleCandidate))
                throw std::invalid_argument("Invalid resolved region color.");
            restored.resolved_regions.push_back({entry[0].get<std::string>(),entry[1].get<std::string>(),
                entry[2].get<std::string>(),entry[3].get<Color>(),RegionResolutionStatus(entry[4].get<size_t>())});
        }
        return remap_palette_slots(restored, slots, output, error);
    } catch (const std::exception& exception) { error = exception.what(); output = {}; return false; }
}

} // namespace Slic3r::AI::SemanticColoring
