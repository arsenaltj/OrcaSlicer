#pragma once

#include "BeautyPuzzle.hpp"
#include "BeautyRegionBoundary.hpp"
#include <cctype>
#include <set>

namespace Slic3r::AI {

// User-facing region identity is independent of native print-color pieces.
// The latter may split when a group crossing several pieces is painted.
// This is an internal draft representation; no existing document is migrated.
struct BeautyEditRegions {
    enum class BoundaryPaintMode { PreserveColors, ExtendAdjacentColors };
    static constexpr const char* schema = "orca.beauty-edit-regions/v1";
    std::string geometry_id;
    std::string source_sha256;
    std::vector<uint32_t> face_region;

    static BeautyEditRegions from_candidate(const BeautyPuzzle& baseline,
                                            const std::vector<int32_t>& candidate,
                                            const std::string& source_sha256) {
        check_source(source_sha256);
        if (baseline.geometry_id.empty() || baseline.face_piece.empty() ||
            baseline.face_piece.size() > BeautyPuzzle::max_faces ||
            candidate.size() != baseline.face_piece.size())
            throw std::invalid_argument("Edit regions do not match the source geometry.");
        BeautyEditRegions result{baseline.geometry_id, source_sha256, baseline.face_piece};
        const auto max_piece = *std::max_element(result.face_region.begin(), result.face_region.end());
        if (max_piece == 0 || max_piece > BeautyPuzzle::max_id)
            throw std::invalid_argument("Invalid original region identity.");
        std::map<int32_t, uint32_t> group_ids;
        uint32_t next = max_piece;
        for (size_t f = 0; f < candidate.size(); ++f) {
            const int32_t label = candidate[f];
            if (label == -1) continue;
            if (label < -1) throw std::invalid_argument("Invalid candidate region identity.");
            auto found = group_ids.find(label);
            if (found == group_ids.end()) {
                if (next >= BeautyPuzzle::max_id || group_ids.size() >= 64)
                    throw std::invalid_argument("Too many candidate edit regions.");
                found = group_ids.emplace(label, ++next).first;
            }
            result.face_region[f] = found->second;
        }
        result.validate(baseline.geometry_id, source_sha256, baseline.face_piece.size());
        return result;
    }

    void validate(const std::string& geometry, const std::string& source, size_t faces) const {
        check_source(source);
        if (geometry.empty() || geometry_id != geometry || source_sha256 != source ||
            faces == 0 || faces > BeautyPuzzle::max_faces || face_region.size() != faces ||
            std::any_of(face_region.begin(), face_region.end(), [](uint32_t id) {
                return id == 0 || id > BeautyPuzzle::max_id;
            })) throw std::invalid_argument("Saved edit regions belong to another model or are invalid.");
    }

    uint32_t at(size_t face) const { return face_region.at(face); }
    // A drawn semantic selection groups faces without merging print pieces.
    uint32_t assign_region(const BeautyPuzzle& printing, const std::string& source,
                           const std::vector<size_t>& selected) {
        validate(printing.geometry_id, source, printing.face_piece.size());
        if(selected.empty())throw std::invalid_argument("The new edit region is empty.");
        const uint32_t largest=*std::max_element(face_region.begin(),face_region.end());
        if(largest>=BeautyPuzzle::max_id)throw std::invalid_argument("Too many edit region identities.");
        auto next=*this;const uint32_t id=largest+1;
        for(size_t f:selected) {
            if(f>=face_region.size())throw std::invalid_argument("Edit selection is outside the model.");
            next.face_region[f]=id;
        }
        next.validate(printing.geometry_id,source,face_region.size());
        *this=std::move(next);return id;
    }
    size_t count() const { return std::set<uint32_t>(face_region.begin(), face_region.end()).size(); }
    std::vector<size_t> faces(uint32_t id) const {
        std::vector<size_t> result;
        for (size_t f = 0; f < face_region.size(); ++f)
            if (face_region[f] == id) result.push_back(f);
        if (result.empty()) throw std::invalid_argument("The selected edit region does not exist.");
        return result;
    }

    std::array<float, 4> representative_color(const BeautyPuzzle& printing,
                                               const BeautySurface& surface, uint32_t id) const {
        if (printing.geometry_id != geometry_id || printing.face_piece.size() != face_region.size() ||
            surface.areas.size() != face_region.size())
            throw std::invalid_argument("Edit colors belong to another model.");
        std::array<double, 3> sum{};
        double total = 0.;
        for (size_t f = 0; f < face_region.size(); ++f) if (face_region[f] == id) {
            const auto painted = printing.colors.find(printing.face_piece[f]);
            const double weight = std::max(surface.areas[f], 1e-15);
            total += weight;
            for (size_t channel = 0; channel < 3; ++channel)
                sum[channel] += weight * (painted == printing.colors.end() ?
                    surface.patches[surface.face_patch[f]].mean_color[channel] : painted->second[channel]);
        }
        if (total == 0.) throw std::invalid_argument("The selected edit region does not exist.");
        return {float(sum[0] / total), float(sum[1] / total), float(sum[2] / total), 1.f};
    }

    std::array<float, 4> source_region_color(const std::vector<RGBA>& source,
                                              const BeautySurface& surface, uint32_t id) const {
        if (source.size() != face_region.size() || surface.areas.size() != face_region.size())
            throw std::invalid_argument("Source colors belong to another model.");
        std::array<double, 3> sum{};
        double total = 0.;
        for (size_t f = 0; f < face_region.size(); ++f) if (face_region[f] == id) {
            const double weight = std::max(surface.areas[f], 1e-15);
            total += weight;
            for (size_t channel = 0; channel < 3; ++channel) sum[channel] += weight * source[f][channel];
        }
        if (total == 0.) throw std::invalid_argument("The selected edit region does not exist.");
        return {float(sum[0] / total), float(sum[1] / total), float(sum[2] / total), 1.f};
    }

    void paint_filament(BeautyPuzzle& printing, const BeautySurface& surface,
                        const std::string& source, uint32_t id, size_t slot) const {
        validate(printing.geometry_id, source, printing.face_piece.size());
        printing.paint_faces_filament(faces(id), surface, slot);
    }

    // A logical boundary can move without recoloring. Explicit color extension
    // propagates the nearest receiving boundary's slot, never the whole group's
    // majority color (a logical eye may contain white, iris and pupil slots).
    bool reshape(BeautyPuzzle& printing, const BeautySurface& surface,
                           const std::string& source, uint32_t id,
                           const std::vector<size_t>& added,
                           const std::vector<size_t>& removed,
                           BoundaryPaintMode mode = BoundaryPaintMode::PreserveColors) {
        validate(printing.geometry_id, source, printing.face_piece.size());
        if (surface.face_neighbors.size() != face_region.size() ||
            surface.areas.size() != face_region.size() || surface.centers.size() != face_region.size())
            throw std::invalid_argument("Edit boundary needs the original surface.");
        if (mode == BoundaryPaintMode::ExtendAdjacentColors && printing.palette.empty())
            throw std::invalid_argument("Color extension needs matched filaments.");
        faces(id);
        const size_t count = face_region.size();
        std::vector<uint8_t> add_mask(count, 0), remove_mask(count, 0), changed(count, 0);
        auto mark = [&](const std::vector<size_t>& samples, std::vector<uint8_t>& mask, bool outward) {
            for (size_t f : samples) {
                if (f >= count) throw std::invalid_argument("Edit boundary is outside the model.");
                if ((face_region[f] != id) == outward) mask[f] = 1;
            }
        };
        mark(added, add_mask, true);
        mark(removed, remove_mask, false);
        BeautyEditRegions next = *this;
        std::vector<size_t> queue;
        for (size_t f = 0; f < count; ++f) if (add_mask[f])
            for (int32_t neighbor : surface.face_neighbors[f])
                if (neighbor >= 0 && face_region[size_t(neighbor)] == id) {
                    changed[f] = 1; queue.push_back(f); break;
                }
        for (size_t at = 0; at < queue.size(); ++at)
            for (int32_t neighbor : surface.face_neighbors[queue[at]])
                if (neighbor >= 0 && add_mask[size_t(neighbor)] && !changed[size_t(neighbor)]) {
                    changed[size_t(neighbor)] = 1; queue.push_back(size_t(neighbor));
                }
        for (size_t f : queue) next.face_region[f] = id;

        using Pending = std::pair<double, size_t>;
        std::priority_queue<Pending, std::vector<Pending>, std::greater<Pending>> pending;
        std::vector<double> distance(count, std::numeric_limits<double>::infinity());
        std::vector<uint32_t> recipient(count, 0);
        auto offer = [&](size_t f, double d, uint32_t owner) {
            if (d < distance[f] || (d == distance[f] && owner < recipient[f])) {
                distance[f] = d; recipient[f] = owner; pending.emplace(d, f);
            }
        };
        for (size_t f = 0; f < count; ++f) if (remove_mask[f])
            for (int32_t neighbor : surface.face_neighbors[f])
                if (neighbor >= 0 && next.face_region[size_t(neighbor)] != id)
                    offer(f, 1., next.face_region[size_t(neighbor)]);
        while (!pending.empty()) {
            const auto [distance_here, f] = pending.top(); pending.pop();
            if (distance_here != distance[f]) continue;
            for (int32_t neighbor : surface.face_neighbors[f])
                if (neighbor >= 0 && remove_mask[size_t(neighbor)])
                    offer(size_t(neighbor), distance_here + 1., recipient[f]);
        }
        for (size_t f = 0; f < count; ++f) if (remove_mask[f]) {
            if (!recipient[f]) continue;
            next.face_region[f] = recipient[f]; changed[f] = 1;
        }
        if (std::none_of(changed.begin(), changed.end(), [](uint8_t bit) { return bit != 0; })) return false;
        if (std::none_of(next.face_region.begin(), next.face_region.end(),
            [&](uint32_t owner) { return owner == id; }))
            throw std::invalid_argument("An edit cannot remove the entire selected region.");
        next.validate(printing.geometry_id, source, count);
        if (mode == BoundaryPaintMode::PreserveColors) {
            printing.validate(surface);
            *this = std::move(next);
            return true;
        }
        std::vector<double> color_distance(count, std::numeric_limits<double>::infinity());
        std::vector<size_t> recipient_slot(count, SIZE_MAX);
        std::priority_queue<Pending, std::vector<Pending>, std::greater<Pending>> color_pending;
        auto offer_color = [&](size_t f, double distance, size_t slot) {
            if (distance < color_distance[f] || (distance == color_distance[f] && slot < recipient_slot[f])) {
                color_distance[f] = distance; recipient_slot[f] = slot; color_pending.emplace(distance, f);
            }
        };
        auto step = [&](size_t f, size_t n) { return std::max(1e-12, (surface.centers[f] - surface.centers[n]).norm()); };
        for (size_t f = 0; f < count; ++f) if (changed[f])
            for (int32_t neighbor : surface.face_neighbors[f]) if (neighbor >= 0) {
                const size_t n = size_t(neighbor);
                if (changed[n] || next.face_region[n] != next.face_region[f]) continue;
                const auto slot = printing.filament_slots.find(printing.face_piece[n]);
                if (slot != printing.filament_slots.end()) offer_color(f, step(f, n), slot->second);
            }
        while (!color_pending.empty()) {
            const auto [distance, f] = color_pending.top(); color_pending.pop();
            if (distance != color_distance[f]) continue;
            for (int32_t neighbor : surface.face_neighbors[f]) if (neighbor >= 0) {
                const size_t n = size_t(neighbor);
                if (changed[n] && next.face_region[n] == next.face_region[f])
                    offer_color(n, distance + step(f, n), recipient_slot[f]);
            }
        }
        std::map<size_t, std::vector<size_t>> recolor;
        for (size_t f = 0; f < count; ++f) if (changed[f]) {
            if (recipient_slot[f] == SIZE_MAX) throw std::invalid_argument("The receiving boundary has no printable color.");
            const auto old = printing.filament_slots.find(printing.face_piece[f]);
            if (old == printing.filament_slots.end() || old->second != recipient_slot[f]) recolor[recipient_slot[f]].push_back(f);
        }
        BeautyPuzzle printed = printing;
        for (const auto& entry : recolor) printed.paint_faces_filament(entry.second, surface, entry.first);
        printed.validate(surface);
        *this = std::move(next);
        printing = std::move(printed);
        return true;
    }

    // Curve fitting and explicit paint extension share one undo transaction.
    // Default fitting changes only editing ownership; subsequent region paint
    // uses those exact faces. Existing v1 records already serialize the result.
    size_t smooth_curve_boundary(BeautyPuzzle& printing,const indexed_triangle_set& mesh,
        const BeautySurface& surface,const std::string& source,uint32_t id,
        BoundaryPaintMode mode=BoundaryPaintMode::PreserveColors,
        const std::vector<uint8_t>& protected_faces={},const std::function<bool()>& canceled={}) {
        validate(printing.geometry_id,source,printing.face_piece.size());
        const auto plan=plan_selected_region_boundary(mesh,surface,face_region,id,protected_faces,canceled);
        auto next=*this;auto printed=printing;
        if(!next.reshape(printed,surface,source,id,plan.added,plan.removed,mode))return 0;
        if(canceled && canceled())throw std::runtime_error("Region curve fitting cancelled.");
        size_t changed=0;for(size_t f=0;f<face_region.size();++f)changed+=next.face_region[f]!=face_region[f];
        *this=std::move(next);printing=std::move(printed);return changed;
    }

    // Compatibility entry for callers explicitly requesting paint extension.
    bool reshape_and_paint(BeautyPuzzle& printing, const BeautySurface& surface,
                           const std::string& source, uint32_t id,
                           const std::vector<size_t>& added, const std::vector<size_t>& removed) {
        return reshape(printing, surface, source, id, added, removed, BoundaryPaintMode::ExtendAdjacentColors);
    }

    nlohmann::json encode() const {
        validate(geometry_id, source_sha256, face_region.size());
        nlohmann::json runs = nlohmann::json::array();
        for (size_t start = 0; start < face_region.size();) {
            size_t end = start + 1;
            while (end < face_region.size() && face_region[end] == face_region[start]) ++end;
            runs.push_back({face_region[start], end - start});
            start = end;
        }
        return {{"schema", schema}, {"geometry_id", geometry_id},
                {"source_sha256", source_sha256}, {"face_count", face_region.size()},
                {"runs", std::move(runs)}};
    }

    static BeautyEditRegions decode(const nlohmann::json& saved, const std::string& geometry,
                                     const std::string& source, size_t faces) {
        if (!saved.is_object() || saved.at("schema") != schema ||
            saved.at("geometry_id") != geometry || saved.at("source_sha256") != source ||
            saved.at("face_count") != faces || !saved.at("runs").is_array())
            throw std::invalid_argument("Invalid edit region record.");
        BeautyEditRegions result{geometry, source, {}};
        if (faces == 0 || faces > BeautyPuzzle::max_faces)
            throw std::invalid_argument("Invalid edit region face count.");
        result.face_region.reserve(faces);
        for (const auto& run : saved.at("runs")) {
            if (!run.is_array() || run.size() != 2 || !run[0].is_number_unsigned() ||
                !run[1].is_number_unsigned()) throw std::invalid_argument("Invalid edit region run.");
            const uint64_t id = run[0].get<uint64_t>();
            const uint64_t length = run[1].get<uint64_t>();
            if (id == 0 || id > BeautyPuzzle::max_id || length == 0 ||
                length > faces - result.face_region.size())
                throw std::invalid_argument("Invalid edit region run.");
            result.face_region.insert(result.face_region.end(), size_t(length), uint32_t(id));
        }
        result.validate(geometry, source, faces);
        return result;
    }

private:
    static void check_source(const std::string& sha) {
        if (sha.size() != 64 || !std::all_of(sha.begin(), sha.end(), [](unsigned char ch) {
            return std::isxdigit(ch) != 0;
        })) throw std::invalid_argument("Invalid edit region source hash.");
    }
};

} // namespace Slic3r::AI
