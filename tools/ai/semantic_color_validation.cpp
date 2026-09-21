// Offline developer harness. Uses the same loaders, recognizers, mapper and
// barycentric subface tree as the application; no sidecar or service is used.
#include "slic3r/AI/ModelGeneration/SemanticColoring/SemanticColoring.hpp"
#include "slic3r/AI/ModelGeneration/SemanticColoring/SemanticPaletteMapping.hpp"
#include "slic3r/AI/ModelGeneration/SemanticColoring/MediaPipeRegionRecognizers.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "slic3r/GUI/AI/Model/SurfaceSelectionState.hpp"
#include "slic3r/GUI/AI/ModelGeneration/PortraitColorPackMapping.hpp"
#include "semantic_validation_persistence.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include "semantic_validation_recorder.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <limits>
#include <set>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace SC = Slic3r::AI::SemanticColoring;
namespace AI = Slic3r::AI;
namespace PP = Slic3r::GUI::PreviewPalette;
namespace validation_fs = std::filesystem;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using Color = SC::Color;
double seconds(Clock::time_point start) { return std::chrono::duration<double>(Clock::now() - start).count(); }
size_t peak_memory()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters {};
    return GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) ? counters.PeakWorkingSetSize : 0;
#else
    return 0;
#endif
}
void write_json(const validation_fs::path& path, const Json& value)
{
    std::ofstream out(path, std::ios::binary);
    out << value.dump(2);
    if (!out) throw std::runtime_error("Cannot write diagnostic JSON: " + path.string());
}
uint32_t packed(Color color)
{
    uint32_t result = 0;
    for (float value : color) result = (result << 8) | uint32_t(std::lround(std::clamp(value, 0.f, 1.f) * 255.f));
    return result;
}
SC::MeshSnapshot read_mesh(const validation_fs::path& path)
{
    Slic3r::TriangleMesh mesh;
    Slic3r::ObjInfo info;
    std::string error;
    if (!AI::load_model_artifact(path.string(), mesh, info, error)) throw std::runtime_error(error);
    SC::MeshSnapshot result;
    result.mesh = std::move(mesh.its);
    result.vertex_colors = std::move(info.vertex_colors);
    result.face_colors = std::move(info.face_colors);
    result.geometry_id = AI::SurfaceSelectionPersistence::geometry_fingerprint(result.mesh);
    result.content_id = SC::content_fingerprint(result);
    return result;
}
Color original_face(const SC::MeshSnapshot& mesh, size_t id)
{
    if (mesh.vertex_colors.size() != mesh.mesh.vertices.size()) {
        const auto& rgba = mesh.face_colors.at(id); return {rgba[0], rgba[1], rgba[2]};
    }
    Color result {};
    for (int corner = 0; corner < 3; ++corner) for (int c = 0; c < 3; ++c)
        result[c] += mesh.vertex_colors[mesh.mesh.indices[id][corner]][c] / 3.f;
    return result;
}
PP::Histogram histogram(const SC::MeshSnapshot& mesh)
{
    PP::Histogram result;
    for (size_t id = 0; id < mesh.mesh.indices.size(); ++id) {
        const auto& f = mesh.mesh.indices[id];
        const float area = (mesh.mesh.vertices[f[1]] - mesh.mesh.vertices[f[0]]).cross(
            mesh.mesh.vertices[f[2]] - mesh.mesh.vertices[f[0]]).norm();
        if (mesh.vertex_colors.size() == mesh.mesh.vertices.size()) {
            for (int corner = 0; corner < 3; ++corner) {
                const int v = f[corner];
                const auto& rgba = mesh.vertex_colors[v]; result.add(packed({rgba[0], rgba[1], rgba[2]}), area / 3.f);
            }
        } else result.add(packed(original_face(mesh, id)), area);
    }
    return result;
}
std::vector<SC::PaletteSlot> slots(const std::vector<Color>& colors, const std::string& prefix)
{
    std::vector<SC::PaletteSlot> result;
    for (size_t i = 0; i < colors.size(); ++i) result.push_back({prefix + std::to_string(i), colors[i], true});
    return result;
}
const char* label_name(SC::Label label)
{
    switch (label) {
    case SC::Label::Unknown: return "unknown";
    case SC::Label::Background: return "background";
    case SC::Label::Hair: return "hair";
    case SC::Label::FaceSkin: return "face-skin";
    case SC::Label::BodySkin: return "body-skin";
    case SC::Label::Clothes: return "clothes";
    case SC::Label::Accessories: return "accessories";
    case SC::Label::Lips: return "lips";
    case SC::Label::MouthInterior: return "mouth-interior";
    case SC::Label::EyeSclera: return "eye-sclera";
    case SC::Label::Iris: return "iris";
    case SC::Label::Eyebrow: return "eyebrow";
    }
    return "unknown";
}
Color oklab(Color rgb)
{
    for (float& value : rgb)
        value = value <= .04045f ? value / 12.92f : std::pow((value + .055f) / 1.055f, 2.4f);
    const float l = std::cbrt(.4122214708f*rgb[0] + .5363325363f*rgb[1] + .0514459929f*rgb[2]);
    const float m = std::cbrt(.2119034982f*rgb[0] + .6806995451f*rgb[1] + .1073969566f*rgb[2]);
    const float s = std::cbrt(.0883024619f*rgb[0] + .2817188376f*rgb[1] + .6299787005f*rgb[2]);
    return {.2104542553f*l + .793617785f*m - .0040720468f*s,
        1.9779984951f*l - 2.428592205f*m + .4505937099f*s,
        .0259040371f*l + .7827717662f*m - .808675766f*s};
}
double squared_distance(const Color& lhs, const Color& rhs)
{
    double result = 0.0;
    for (size_t channel = 0; channel < 3; ++channel) {
        const double delta = double(lhs[channel]) - double(rhs[channel]);
        result += delta * delta;
    }
    return result;
}
struct RegionDiagnosticMatch {
    std::vector<size_t> assignment_counts;
    std::vector<size_t> subface_counts;
    double confidence_sum {0.0};
    size_t confidence_count {0};
};
Json region_diagnostics(const SC::MeshSnapshot& mesh, const SC::Analysis& analysis,
                        const SC::SlotMappingResult& mapping, const std::vector<SC::PaletteSlot>& palette)
{
    Json regions = Json::array();
    if (mapping.material_centers.empty()) return regions;
    std::vector<RegionDiagnosticMatch> matches(mapping.material_centers.size());
    for (auto& match : matches) {
        match.assignment_counts.assign(palette.size(), 0);
        match.subface_counts.assign(palette.size(), 0);
    }
    const auto center_for_face = [&](size_t face_id) -> size_t {
        if (face_id >= mesh.mesh.indices.size() || face_id >= analysis.face_labels.size()) return mapping.material_centers.size();
        const Color source = oklab(original_face(mesh, face_id));
        const SC::Label label = analysis.face_labels[face_id];
        size_t winner = mapping.material_centers.size();
        double best = std::numeric_limits<double>::infinity();
        for (size_t index = 0; index < mapping.material_centers.size(); ++index) {
            const auto& center = mapping.material_centers[index];
            if (center.label != label) continue;
            const double distance = squared_distance(source, center.original_oklab);
            if (distance < best) { best = distance; winner = index; }
        }
        // Boundary protection can leave a face with a baseline label while its
        // material center is represented by the related skin label. Keep the
        // diagnostic useful without changing the production mapping.
        if (winner == mapping.material_centers.size() &&
            (label == SC::Label::FaceSkin || label == SC::Label::BodySkin)) {
            for (size_t index = 0; index < mapping.material_centers.size(); ++index) {
                const auto& center = mapping.material_centers[index];
                if (center.label != SC::Label::FaceSkin && center.label != SC::Label::BodySkin) continue;
                const double distance = squared_distance(source, center.original_oklab);
                if (distance < best) { best = distance; winner = index; }
            }
        }
        return winner;
    };
    std::map<std::string, size_t> slot_index;
    for (size_t index = 0; index < palette.size(); ++index) slot_index[palette[index].id] = index;
    for (const auto& assignment : mapping.face_slots) {
        const size_t center = center_for_face(assignment.face_id);
        const auto found = slot_index.find(assignment.slot_id);
        if (center == mapping.material_centers.size() || found == slot_index.end()) continue;
        ++matches[center].assignment_counts[found->second];
        if (assignment.face_id < analysis.face_confidence.size()) {
            matches[center].confidence_sum += analysis.face_confidence[assignment.face_id];
            ++matches[center].confidence_count;
        }
    }
    for (const auto& assignment : mapping.subface_slots) {
        const size_t center = center_for_face(assignment.face_id);
        const auto found = slot_index.find(assignment.slot_id);
        if (center == mapping.material_centers.size() || found == slot_index.end()) continue;
        ++matches[center].subface_counts[found->second];
        if (assignment.face_id < analysis.face_confidence.size()) {
            matches[center].confidence_sum += analysis.face_confidence[assignment.face_id] * .5;
            matches[center].confidence_count += 1;
        }
    }
    const auto recommendations = SC::recommend_region_slots(mapping, palette);
    std::map<size_t, const SC::RegionColorRecommendation*> recommendation_by_center;
    for (const auto& recommendation : recommendations)
        recommendation_by_center.emplace(recommendation.material_center_id, &recommendation);
    for (size_t center_index = 0; center_index < mapping.material_centers.size(); ++center_index) {
        const auto& center = mapping.material_centers[center_index];
        const auto& match = matches[center_index];
        size_t selected = palette.size(), selected_count = 0;
        for (size_t slot = 0; slot < palette.size(); ++slot) {
            if (match.assignment_counts[slot] + match.subface_counts[slot] > selected_count) {
                selected = slot; selected_count = match.assignment_counts[slot] + match.subface_counts[slot];
            }
        }
        const bool accepted = selected < palette.size();
        Json candidates = Json::array();
        const auto recommendation = recommendation_by_center.find(center.id);
        if (recommendation != recommendation_by_center.end()) for (const auto& candidate : recommendation->second->candidates) {
            const auto slot = std::find_if(palette.begin(),palette.end(),[&](const auto& value){return value.id==candidate.slot_id;});
            const size_t slot_id = slot == palette.end() ? palette.size() : size_t(slot-palette.begin());
            candidates.push_back({{"slot_id",candidate.slot_id},{"rgb",candidate.color},
                {"enabled",slot!=palette.end()&&slot->enabled},{"score",candidate.score},
                {"score_components",{{"source",candidate.source_cost},{"semantic",candidate.semantic_cost},
                    {"role_bonus",candidate.role_bonus},{"multi_view",nullptr},{"geometry",nullptr},
                    {"continuity",nullptr}}},{"evidence_available",{{"source",true},{"semantic",true},
                    {"multi_view",false},{"geometry",false},{"continuity",false}}},
                {"face_assignment_count",slot_id<palette.size()?match.assignment_counts[slot_id]:0},
                {"subface_assignment_count",slot_id<palette.size()?match.subface_counts[slot_id]:0},
                {"accepted",candidate.accepted},{"reason",candidate.rejection_reason}});
        }
        std::vector<size_t> region_faces;
        for (const auto& face : mapping.face_slots) if (face.region_id == center.region_id) region_faces.push_back(face.face_id);
        std::sort(region_faces.begin(),region_faces.end()); region_faces.erase(std::unique(region_faces.begin(),region_faces.end()),region_faces.end());
        Json ranges=Json::array();
        for(size_t i=0;i<region_faces.size();) {size_t j=i+1;while(j<region_faces.size()&&region_faces[j]==region_faces[j-1]+1)++j;
            ranges.push_back({region_faces[i],region_faces[j-1]});i=j;}
        std::string status_reason;
        if (accepted) status_reason = "region_has_face_or_subface_assignment";
        else if (mapping.boundary_budget_fallback) status_reason = "boundary_budget_fallback_without_new_assignment";
        else status_reason = "no_region_assignment_evidence";
        regions.push_back({
            {"id", center.id}, {"label", unsigned(center.label)}, {"label_name", label_name(center.label)},
            {"confidence", match.confidence_count == 0 ? 0.0 : match.confidence_sum / double(match.confidence_count)},
            {"original_rgb", center.original_rgb}, {"original_oklab", center.original_oklab},
            {"region_id",center.region_id},{"palette_signature",mapping.palette_signature},
            {"distribution",{{"q05",center.q05_oklab},{"q50",center.q50_oklab},{"q95",center.q95_oklab},
                {"medoid",center.medoid_oklab},{"dispersion",center.color_dispersion},
                {"dominant_ratio",center.dominant_ratio},{"gradient_strength",center.gradient_strength}}},
            {"face_count", center.face_count}, {"surface_area", center.surface_area},
            {"selected_slot_id", accepted ? palette[selected].id : ""}, {"accepted", accepted},
            {"top_score",recommendation==recommendation_by_center.end()?nullptr:Json(recommendation->second->top_score)},
            {"second_score",recommendation==recommendation_by_center.end()||!std::isfinite(recommendation->second->second_score)?nullptr:Json(recommendation->second->second_score)},
            {"score_margin",recommendation==recommendation_by_center.end()||!std::isfinite(recommendation->second->score_margin)?nullptr:Json(recommendation->second->score_margin)},
            {"ambiguous",recommendation!=recommendation_by_center.end()&&recommendation->second->ambiguous},
            {"face_id_ranges",std::move(ranges)},{"uv_bbox",nullptr},{"reason", status_reason},
            {"candidate_slots", std::move(candidates)}
        });
    }
    return regions;
}
Json mapping_summary(const SC::MeshSnapshot& mesh, const SC::Analysis& analysis,
                     const SC::SlotMappingResult& mapping, const std::vector<SC::PaletteSlot>& palette,
                     bool include_resolved_regions = false)
{
    Json labels = Json::object();
    std::map<std::string, size_t> counts;
    for (const auto& assignment : mapping.face_slots) {
        ++counts[assignment.slot_id];
        const auto label = std::to_string(unsigned(analysis.face_labels.at(assignment.face_id)));
        if (!labels.contains(label)) labels[label] = Json::object();
        labels[label][assignment.slot_id] = labels[label].value(assignment.slot_id, size_t(0)) + 1;
    }
    Json skin_lip_sharing = Json::array();
    const std::string skin = std::to_string(unsigned(SC::Label::FaceSkin));
    const std::string lips = std::to_string(unsigned(SC::Label::Lips));
    if (labels.contains(skin) && labels.contains(lips))
        for (auto at = labels[skin].begin(); at != labels[skin].end(); ++at)
            if (labels[lips].contains(at.key())) skin_lip_sharing.push_back(at.key());
    Json entries = Json::array();
    for (const auto& slot : palette) entries.push_back({{"id", slot.id}, {"rgb", slot.color}, {"enabled", slot.enabled}});
    Json resolved = Json::array();
    if (include_resolved_regions)
        for (const auto& region : mapping.resolved_regions) resolved.push_back({
            {"region_id",region.region_id},{"intended_slot_id",region.intended_slot_id},
            {"actual_slot_id",region.actual_slot_id},{"actual_color",region.actual_color},
            {"status",SC::region_resolution_status_name(region.status)}});
    return {{"palette", entries}, {"automatic_face_count", mapping.faces.size()},
        {"subface_count", mapping.subfaces.size()}, {"slot_face_counts", counts},
        {"label_slot_face_counts", labels}, {"face_skin_lips_shared_material_ids",skin_lip_sharing}, {"added_triangles", mapping.added_triangles},
        {"rejected_candidates", mapping.rejected_candidates}, {"material_center_count", mapping.material_centers.size()},
        {"substituted_assignments", mapping.substituted_assignments}, {"boundary_budget_fallback", mapping.boundary_budget_fallback},
        {"source_face_count", mesh.mesh.indices.size()}, {"resolved_regions",std::move(resolved)}};
}
SC::SlotMappingResult map(const SC::MeshSnapshot& mesh, const SC::Analysis& analysis,
                          const std::vector<SC::PaletteSlot>& palette, const std::vector<Color>& card,
                          const std::vector<SC::RegionColorOverride>& overrides = {})
{
    SC::SlotMappingResult result;
    std::string error;
    if (!SC::map_palette_slots(mesh, analysis, palette, card, {}, overrides, result, error)) throw std::runtime_error(error);
    return result;
}
void complete_display_faces(const SC::MeshSnapshot& mesh, SC::SlotMappingResult& mapping,
                            const PP::ColorTrialMapping& fallback, const SC::Analysis& analysis,
                            const std::vector<Color>& portrait_card)
{
    std::vector<Color> centers;
    for (const auto& color : fallback.mapping_colors) centers.push_back(PP::to_lab(color));
    if (centers.empty()) throw std::runtime_error("A display baseline is missing");
    SC::FaceColors complete;
    complete.reserve(mesh.mesh.indices.size());
    size_t lip_slot = fallback.target_colors.size(), skin_slot = fallback.target_colors.size();
    if (portrait_card.size() == 6) {
        for (size_t index = 0; index < fallback.target_colors.size(); ++index) {
            if (fallback.target_colors[index] == portrait_card[3]) lip_slot = index;
            if (fallback.target_colors[index] == portrait_card[0]) skin_slot = index;
        }
    }
    for (size_t id = 0; id < mesh.mesh.indices.size(); ++id) {
        size_t target = PP::nearest_lab_index(PP::to_lab(original_face(mesh, id)), centers);
        if (id < analysis.face_labels.size() && lip_slot < fallback.target_colors.size() && target == lip_slot &&
            (analysis.face_labels[id] == SC::Label::FaceSkin || analysis.face_labels[id] == SC::Label::BodySkin ||
             analysis.face_labels[id] == SC::Label::Unknown) && skin_slot < fallback.target_colors.size())
            target = skin_slot;
        complete.emplace_back(id, fallback.target_colors[target]);
    }
    for (const auto& face : mapping.faces) complete.at(face.first).second = face.second;
    mapping.faces = std::move(complete);
}
void write_ppm(const validation_fs::path& path, const SC::RGBImage& image)
{
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << image.width << ' ' << image.height << "\n255\n";
    out.write(reinterpret_cast<const char*>(image.pixels.data()), std::streamsize(image.pixels.size()));
    if (!out) throw std::runtime_error("Cannot write view: " + path.string());
}
struct Camera { const char* name; Slic3r::Vec3f direction, right; };
const std::array<Camera, 6> cameras {{
    {"front", {1,0,0}, {0,1,0}}, {"back", {-1,0,0}, {0,-1,0}},
    {"left", {0,1,0}, {-1,0,0}}, {"right", {0,-1,0}, {1,0,0}},
    {"top", {0,0,1}, {1,0,0}}, {"bottom", {0,0,-1}, {1,0,0}}
}};
SC::RGBImage paint_view(const SC::RenderedView& view, const SC::SlotMappingResult& mapping,
                        const PP::ColorTrialMapping& baseline)
{
    SC::RGBImage image = view.image;
    std::map<size_t, Color> roots(mapping.faces.begin(), mapping.faces.end());
    std::map<size_t, std::vector<SC::SubfaceColor>> leaves;
    for (const auto& item : mapping.subfaces) leaves[item.face_id].push_back(item);
    std::vector<Color> centers;
    for (const auto& color : baseline.mapping_colors) centers.push_back(PP::to_lab(color));
    for (size_t pixel = 0; pixel < view.face_ids.size(); ++pixel) {
        const size_t face = view.face_ids[pixel];
        if (face == UINT32_MAX) continue;
        Color color {image.pixels[pixel*3] / 255.f, image.pixels[pixel*3+1] / 255.f, image.pixels[pixel*3+2] / 255.f};
        const auto root = roots.find(face);
        if (root != roots.end()) color = root->second;
        else if (!centers.empty()) color = baseline.target_colors[PP::nearest_lab_index(PP::to_lab(color), centers)];
        const auto group = leaves.find(face);
        uint8_t chosen_depth = 0;
        if (group != leaves.end()) for (const auto& leaf : group->second) {
            SC::SubfacePath at;
            if (leaf.path.depth >= chosen_depth && SC::locate_subface(view.barycentric[pixel], leaf.path.depth, at) && at == leaf.path) {
                color = leaf.color; chosen_depth = leaf.path.depth;
            }
        }
        for (int c = 0; c < 3; ++c) image.pixels[pixel*3+c] = uint8_t(std::lround(std::clamp(color[c], 0.f, 1.f) * 255.f));
    }
    return image;
}
SC::RGBImage macro_view(const SC::RenderedView& view, const SC::Analysis& analysis)
{
    SC::RGBImage image = view.image;
    const std::array<std::array<uint8_t, 3>, 8> colors {{{96,96,96}, {227,158,132}, {234,210,95},
        {94,174,135}, {68,147,116}, {84,145,205}, {112,83,133}, {195,113,55}}};
    for (size_t pixel = 0; pixel < view.face_ids.size(); ++pixel) {
        const size_t id = view.face_ids[pixel];
        if (id == UINT32_MAX) continue;
        const bool owned = id < analysis.face_person_instances.size() &&
            analysis.face_person_instances[id] != UINT32_MAX;
        const size_t region = owned && id < analysis.face_macro_regions.size() ?
            size_t(analysis.face_macro_regions[id]) : 0;
        const auto color = colors[std::min(region, colors.size() - 1)];
        for (int channel = 0; channel < 3; ++channel)
            image.pixels[pixel * 3 + channel] = color[channel];
    }
    return image;
}
void views(const validation_fs::path& output, const SC::MeshSnapshot& mesh, const SC::SlotMappingResult& baseline,
           const SC::SlotMappingResult& candidate, const PP::ColorTrialMapping& fallback, int size,
           const SC::Analysis& baseline_analysis, const SC::Analysis& candidate_analysis)
{
    SC::MeshSnapshot camera_mesh = mesh;
    const float ct = 1.f / std::sqrt(1.01f), st = .1f * ct;
    Json camera_record = Json::array();
    for (const auto& camera : cameras) {
        const auto up = camera.direction.cross(camera.right);
        for (size_t i = 0; i < mesh.mesh.vertices.size(); ++i) {
            const auto& vertex = mesh.mesh.vertices[i];
            const float d = vertex.dot(camera.direction), u = vertex.dot(up);
            // Undo the production renderer's fixed .1 elevation. This camera
            // transform preserves all face IDs and gives true orthogonal views.
            camera_mesh.mesh.vertices[i] = {vertex.dot(camera.right), -ct*d + st*u, st*d + ct*u};
        }
        const auto view = SC::render_view(camera_mesh, 0.f, size);
        if (!view.error.empty()) throw std::runtime_error(view.error);
        write_ppm(output / (std::string(camera.name) + "-original.ppm"), view.image);
        write_ppm(output / (std::string(camera.name) + "-baseline.ppm"), paint_view(view, baseline, fallback));
        write_ppm(output / (std::string(camera.name) + "-candidate.ppm"), paint_view(view, candidate, fallback));
        write_ppm(output / (std::string(camera.name) + "-macro.ppm"), macro_view(view, candidate_analysis));
        // Diagnostic face/depth maps are lossless and independent of lighting.
        std::ofstream ids(output / (std::string(camera.name) + "-faces.u32"), std::ios::binary);
        ids.write(reinterpret_cast<const char*>(view.face_ids.data()), std::streamsize(view.face_ids.size()*sizeof(uint32_t)));
        std::ofstream depth(output / (std::string(camera.name) + "-depth.f32"), std::ios::binary);
        depth.write(reinterpret_cast<const char*>(view.depth.data()), std::streamsize(view.depth.size()*sizeof(float)));
        camera_record.push_back({{"name", camera.name}, {"direction", {camera.direction.x(),camera.direction.y(),camera.direction.z()}},
            {"right", {camera.right.x(),camera.right.y(),camera.right.z()}}, {"size", size}});
        Json details = Json::array();
        for (const std::string part : {"eyes-brows", "skin-boundaries", "lips"}) {
            const auto relevant = [&](SC::Label label) {
                if (part == "eyes-brows") return label == SC::Label::EyeSclera || label == SC::Label::Iris || label == SC::Label::Eyebrow;
                if (part == "lips") return label == SC::Label::Lips || label == SC::Label::MouthInterior;
                return label == SC::Label::FaceSkin;
            };
            int left = size, top = size, right = -1, bottom = -1;
            for (size_t pixel = 0; pixel < view.face_ids.size(); ++pixel) {
                const size_t face = view.face_ids[pixel];
                if (face >= candidate_analysis.face_labels.size()) continue;
                if (!relevant(candidate_analysis.face_labels[face]) && !relevant(baseline_analysis.face_labels[face])) continue;
                const int x = int(pixel % size), y = int(pixel / size);
                left = std::min(left,x); right = std::max(right,x); top = std::min(top,y); bottom = std::max(bottom,y);
            }
            if (right < left || bottom < top) continue;
            const int padding = std::max(4, std::max(right-left+1, bottom-top+1) / 8);
            left = std::max(0,left-padding); top = std::max(0,top-padding);
            right = std::min(size-1,right+padding); bottom = std::min(size-1,bottom+padding);
            const SC::ViewRegion region {float(left)/size, float(top)/size,
                float(right-left+1)/size, float(bottom-top+1)/size};
            const int detail_size = std::clamp(std::max(right-left+1,bottom-top+1)*4, 64, 2048);
            const auto detail = SC::render_region(camera_mesh, 0.f, region, detail_size);
            if (!detail.error.empty()) throw std::runtime_error(detail.error);
            const auto name = std::string(camera.name) + "-" + part + "-4x";
            write_ppm(output / (name + "-original.ppm"), detail.image);
            write_ppm(output / (name + "-baseline.ppm"), paint_view(detail, baseline, fallback));
            write_ppm(output / (name + "-candidate.ppm"), paint_view(detail, candidate, fallback));
            std::ofstream faces(output / (name + "-faces.u32"), std::ios::binary);
            faces.write(reinterpret_cast<const char*>(detail.face_ids.data()), std::streamsize(detail.face_ids.size()*sizeof(uint32_t)));
            details.push_back({{"name", name}, {"full_view_box", {left,top,right+1,bottom+1}},
                {"render_size", detail_size}, {"source", "original-mesh-rerasterized"},
                {"selection", "union-of-baseline-and-candidate-labels-with-fixed-padding"}});
        }
        write_json(output / (std::string(camera.name) + "-details.json"), details);
    }
    write_json(output / "cameras.json", camera_record);
}
SC::Analysis analyze(const SC::MeshSnapshot& mesh, SC::RegionRecognizers& providers,
                     const validation_fs::path& cache, bool boundary, bool reuse, double& elapsed)
{
    SC::Analysis result;
    const std::string boundary_id = boundary && providers.boundary ? providers.boundary->identity() : "none";
    const std::string pose_id = boundary && providers.pose && providers.pose_error.empty() ?
        providers.pose->identity() : "none";
    if (reuse && validation_fs::is_regular_file(cache)) {
        std::ifstream input(cache);
        std::string error;
        if (SC::decode_analysis(Json::parse(input), mesh, providers.body->identity(), providers.face->identity(),
                boundary_id, pose_id, result, error)) { elapsed = 0; return result; }
    }
    const auto start = Clock::now();
    const auto progress = [](int progress, const std::string& stage) { std::cerr << progress << "% " << stage << '\n'; };
    if (boundary) {
        auto recorder = std::make_shared<SC::SemanticValidationRecorder>(cache.parent_path() / "recognizer-evidence");
        SC::RecordingBodyRecognizer body(*providers.body, recorder);
        SC::RecordingFaceRecognizer face(*providers.face, recorder);
        std::unique_ptr<SC::RecordingBoundaryRefiner> refiner;
        if (providers.boundary) refiner = std::make_unique<SC::RecordingBoundaryRefiner>(*providers.boundary, recorder);
        const SC::RenderObserver observer = [recorder](const SC::RenderedView& view, int index,
            const SC::ViewRegion& region, bool face_crop) { recorder->rendered(view, index, region, face_crop); };
        result = SC::analyze(mesh, body, face, refiner.get(), providers.pose_error.empty() ? providers.pose.get() : nullptr,
                             {}, progress, observer);
    } else result = SC::analyze(mesh, *providers.body, *providers.face, nullptr, {}, progress);
    elapsed = seconds(start);
    if (!result.error.empty() || result.canceled) throw std::runtime_error(result.error.empty() ? "Analysis canceled" : result.error);
    write_json(cache, SC::encode_analysis(result));
    return result;
}
int main(int argc, char** argv)
{
    try {
        validation_fs::path runtime, fixtures, output;
        std::string selected;
        int size = 512;
        size_t image_palette_count = 0;
        std::vector<std::pair<std::string,std::string>> requested_region_slots;
        bool images = true, reuse = false, images_only = false, skip_palette_matrix = false;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            auto value = [&]() { if (++i >= argc) throw std::runtime_error("Missing value for " + arg); return std::string(argv[i]); };
            if (arg == "--runtime") runtime = value();
            else if (arg == "--fixtures") fixtures = value();
            else if (arg == "--output") output = value();
            else if (arg == "--model") selected = value();
            else if (arg == "--size") size = std::stoi(value());
            else if (arg == "--image-palette-count") image_palette_count = size_t(std::stoul(value()));
            else if (arg == "--region-slot") {
                const std::string binding = value();
                const size_t separator = binding.find('=');
                if (separator == std::string::npos || separator == 0 || separator + 1 == binding.size())
                    throw std::runtime_error("Region binding must be REGION_ID=SLOT_ID");
                requested_region_slots.emplace_back(binding.substr(0, separator), binding.substr(separator + 1));
            }
            else if (arg == "--skip-images") images = false;
            else if (arg == "--images-only") images_only = true;
            else if (arg == "--skip-palette-matrix") skip_palette_matrix = true;
            else if (arg == "--reuse-analysis") reuse = true;
            else throw std::runtime_error("Unknown option: " + arg);
        }
        if (runtime.empty() || fixtures.empty() || output.empty() || size < 64 || size > 2048)
            throw std::runtime_error("Usage: semantic_color_validation --runtime DIR --fixtures DIR --output DIR [--model left|right|couple|animal] [--region-slot REGION_ID=SLOT_ID] [--reuse-analysis] [--skip-images] [--images-only] [--skip-palette-matrix] [--size 512] [--image-palette-count 1..6]");
        runtime = validation_fs::absolute(runtime);
        if (validation_fs::exists(runtime / "ai" / "portrait_semantics" / "libmediapipe.dll"))
            runtime /= "ai/portrait_semantics";
        validation_fs::create_directories(output);
        const auto load_start = Clock::now();
        auto providers = SC::create_region_recognizers("mediapipe.cpu.v1", "mediapipe.cpu.v1", "mobilesam.cpu.v1", runtime);
        const double load_seconds = seconds(load_start);
        if (!providers.error.empty()) throw std::runtime_error(providers.error);
        // Frozen young-portrait card from local-validation-baseline/inputs.json.
        const std::vector<Color> card {{247.f/255,226.f/255,218.f/255}, {40.f/255,38.f/255,41.f/255},
            {246.f/255,247.f/255,249.f/255}, {234.f/255,154.f/255,146.f/255},
            {102.f/255,140.f/255,182.f/255}, {149.f/255,139.f/255,134.f/255}};
        Json summary {{"schema", "orca.semantic-local-validation/v1"}, {"pipeline", SC::pipeline_version},
            {"requested_boundary", "mobilesam.cpu.v1"}, {"actual_boundary", providers.boundary ? providers.boundary->identity() : "none"},
            {"boundary_fallback_reason", providers.boundary_error}, {"provider_load_seconds", load_seconds},
            {"limits", "CPU unlit diagnostics; GUI/MSAA, physical color accuracy and user visual acceptance are separate."}, {"models", Json::array()}};
        bool failed = false, matched = false;
        for (const std::string name : {"left", "right", "couple", "animal"}) {
            if (!selected.empty() && selected != name) continue;
            matched = true;
            Json record {{"model", name}};
            try {
                const auto model_start = Clock::now();
                const auto source = fixtures / (name + ".obj");
                const auto destination = output / name;
                validation_fs::create_directories(destination);
                auto mesh = read_mesh(source);
                record["source"] = source.string();
                record["sha256"] = AI::model_artifact_sha256(source.string());
                record["geometry_id"] = mesh.geometry_id;
                record["content_id"] = mesh.content_id;
                const auto colors = histogram(mesh);
                double baseline_time = 0, candidate_time = 0;
                const auto baseline = analyze(mesh, providers, destination / "analysis-baseline.json", false, reuse, baseline_time);
                const auto candidate = analyze(mesh, providers, destination / "analysis-candidate.json", true, reuse, candidate_time);
                record["baseline_seconds"] = baseline_time;
                record["candidate_seconds"] = candidate_time;
                record["person_detected"] = candidate.person_detected;
                record["reliable_faces"] = candidate.reliable_faces;
                record["face_views"] = candidate.face_views;
                record["pose_identity"] = candidate.pose_identity;
                record["pose_error"] = candidate.pose_error;
                record["macro_region_faces"] = Json::array();
                for (size_t region = 0; region < 8; ++region) {
                    size_t faces = 0, owned = 0;
                    for (size_t id = 0; id < candidate.face_macro_regions.size(); ++id) {
                        if (size_t(candidate.face_macro_regions[id]) != region) continue;
                        ++faces;
                        if (candidate.face_person_instances[id] != UINT32_MAX) ++owned;
                    }
                    record["macro_region_faces"].push_back({{"region", region}, {"faces", faces}, {"owned", owned}});
                }
                record["roi_runs"] = Json::array();
                for (const auto& roi : candidate.boundary_runs) record["roi_runs"].push_back({
                    {"person", roi.person_id}, {"part", unsigned(roi.part)}, {"side", unsigned(roi.side)},
                    {"box", {roi.region.left,roi.region.top,roi.region.right,roi.region.bottom}},
                    {"status", roi.status}, {"reason", roi.reason}, {"model_score", roi.model_score},
                    {"loading_ms", roi.loading_ms}, {"encoding_ms", roi.encoding_ms}, {"decoding_ms", roi.decoding_ms}, {"changed_pixels", roi.changed_pixels}});
                if (images_only) {
                    const size_t count = image_palette_count == 0 ? size_t(6) : image_palette_count;
                    if (count == 0 || count > card.size()) throw std::runtime_error("Image palette count must be between 1 and 6.");
                    const std::vector<Color> image_card(card.begin(), card.begin() + count);
                    const auto image_palette = slots(image_card, "fixed-");
                    auto image_baseline = map(mesh, baseline, image_palette, card);
                    auto image_candidate = map(mesh, candidate, image_palette, card);
                    const PP::ColorTrialMapping image_fallback {true, image_card, image_card};
                    complete_display_faces(mesh, image_baseline, image_fallback, baseline, card);
                    complete_display_faces(mesh, image_candidate, image_fallback, candidate, card);
                    views(destination, mesh, image_baseline, image_candidate, image_fallback, size, baseline, candidate);
                    record["image_only"] = true;
                    record["render_palette_count"] = count;
                    record["status"] = "completed-awaiting-visual-review";
                    write_json(destination / "result.json", record);
                    summary["models"].push_back(record);
                    write_json(output / "summary.json", summary);
                    std::cout << name << ": " << record.value("status", "unknown") << std::endl;
                    continue;
                }
                const auto palette = slots(card, "fixed-");
                auto before = map(mesh, baseline, palette, card), after = map(mesh, candidate, palette, card);
                if (!requested_region_slots.empty()) {
                    std::vector<SC::RegionColorOverride> intents;
                    intents.reserve(requested_region_slots.size());
                    for (const auto& binding : requested_region_slots) {
                        SC::RegionColorOverride intent;
                        intent.analysis_signature = candidate.signature;
                        intent.region_id = binding.first;
                        intent.slot_id = binding.second;
                        intent.locked = true;
                        intents.push_back(std::move(intent));
                    }
                    after = map(mesh, candidate, palette, card, intents);
                    record["requested_region_slots"] = Json::array();
                    for (const auto& binding : requested_region_slots)
                        record["requested_region_slots"].push_back({{"region_id",binding.first},{"slot_id",binding.second}});
                }
                record["six_color_candidate"] = mapping_summary(mesh, candidate, after, palette, true);
                // R0 diagnostic export: keep region evidence separate from the
                // legacy material-targets file so existing consumers remain
                // compatible while every center has its source, confidence,
                // candidate slots and an explicit selection/rejection reason.
                record["region_diagnostics"] = region_diagnostics(mesh, candidate, after, palette);
                write_json(destination / "region-diagnostics.json", record["region_diagnostics"]);
                record["palette_cases"] = Json::array();
                record["technical_failures"] = Json::array();
                const auto technical = [&](bool condition, const std::string& detail) {
                    if (!condition) record["technical_failures"].push_back(detail);
                    return condition;
                };
                if (name == "animal") technical(after.face_slots.empty() && after.subface_slots.empty(),
                    "Animal fixture acquired automatic portrait assignments");
                const auto centers = [](const SC::SlotMappingResult& mapped) {
                    Json values = Json::array();
                    for (const auto& center : mapped.material_centers) values.push_back({center.id,
                        unsigned(center.label), center.original_rgb, center.original_oklab,
                        center.face_count, center.surface_area});
                    return values;
                };
                const auto frozen_centers = centers(after);
                // Deterministic in-process replay fingerprint, not an artifact SHA256.
                const auto signature = [](const SC::SlotMappingResult& mapped) {
                    uint64_t value = 14695981039346656037ull;
                    const auto append = [&](const void* data, size_t length) {
                        const auto* bytes = static_cast<const unsigned char*>(data);
                        for (size_t i = 0; i < length; ++i) { value ^= bytes[i]; value *= 1099511628211ull; }
                    };
                    const auto string = [&](const std::string& id) { append(id.data(), id.size()); const char zero = 0; append(&zero, 1); };
                    for (const auto& face : mapped.face_slots) {
                        append(&face.face_id, sizeof(face.face_id)); string(face.slot_id); string(face.intended_slot_id);
                        append(face.intended_color.data(), sizeof(float) * 3);
                    }
                    for (const auto& leaf : mapped.subface_slots) {
                        append(&leaf.face_id, sizeof(leaf.face_id)); append(&leaf.path.depth, sizeof(leaf.path.depth));
                        append(&leaf.path.value, sizeof(leaf.path.value)); string(leaf.slot_id); string(leaf.intended_slot_id);
                        append(leaf.intended_color.data(), sizeof(float) * 3);
                    }
                    for (const auto& face : mapped.faces) append(face.second.data(), sizeof(float) * 3);
                    for (const auto& leaf : mapped.subfaces) append(leaf.color.data(), sizeof(float) * 3);
                    return std::to_string(value);
                };
                std::map<std::pair<bool,size_t>, std::string> first_mapping;
                const auto suggestion = [&](size_t count, std::string& fallback_reason) {
                    std::vector<SC::PaletteSlot> suggested;
                    if (!SC::suggest_material_slots(after.material_centers, count, {}, suggested, fallback_reason))
                        suggested = slots(colors.palette(count, {}, true), "auto-fallback-");
                    return suggested;
                };
                const auto save_case = [&](const std::string& name, const std::vector<SC::PaletteSlot>& candidates,
                                           const SC::SlotMappingResult& mapped, double elapsed) {
                    auto status = mapping_summary(mesh, candidate, mapped, candidates);
                    status["case"] = name; status["mapping_seconds"] = elapsed;
                    status["recognition_reused"] = true;
                    status["material_centers_unchanged"] = technical(centers(mapped) == frozen_centers,
                        name + ": source material centers changed with candidate palette");
                    status["assignment_fingerprint_fnv1a64"] = signature(mapped);
                    status["enabled_candidates"] = std::count_if(candidates.begin(), candidates.end(), [](const auto& slot) { return slot.enabled; });
                    status["semantic_labels_source"] = "Frozen analysis; FaceSkin and Lips mappings are reported separately even when sharing a material.";
                    record["palette_cases"].push_back(std::move(status));
                };
                if (!skip_palette_matrix) for (size_t count = 1; count <= 6; ++count) for (const bool automatic : {false, true}) {
                    std::string fallback_reason;
                    const auto candidates = automatic ? suggestion(count, fallback_reason)
                        : std::vector<SC::PaletteSlot>(palette.begin(), palette.begin() + count);
                    const auto start = Clock::now();
                    const auto mapped = map(mesh, candidate, candidates, automatic ? std::vector<Color>{} : card);
                    save_case(std::string(automatic ? "automatic-" : "fixed-") + std::to_string(count), candidates, mapped, seconds(start));
                    record["palette_cases"].back()["requested_count"] = count;
                    record["palette_cases"].back()["suggestion_fallback_reason"] = fallback_reason;
                    first_mapping[{automatic,count}] = signature(mapped);
                }
                if (!skip_palette_matrix) for (size_t removed = 0; removed < card.size(); ++removed) {
                    auto candidates = palette; candidates[removed].enabled = false;
                    const auto start = Clock::now();
                    const auto mapped = map(mesh, candidate, candidates, card);
                    save_case("remove-role-" + std::to_string(removed), candidates, mapped, seconds(start));
                }
                if (!skip_palette_matrix) for (const auto enabled : {std::vector<size_t>{0,2,4}, std::vector<size_t>{0,1,2,3,5}}) {
                    auto candidates = palette;
                    for (size_t i = 0; i < candidates.size(); ++i)
                        candidates[i].enabled = std::find(enabled.begin(), enabled.end(), i) != enabled.end();
                    const auto start = Clock::now();
                    const auto mapped = map(mesh, candidate, candidates, card);
                    save_case("nonprefix-enabled-" + std::to_string(enabled.size()), candidates, mapped, seconds(start));
                }
                if (!skip_palette_matrix) for (const bool nearby : {true,false}) {
                    auto candidates = std::vector<SC::PaletteSlot>(palette.begin(), palette.begin() + 5);
                    const auto original_five = map(mesh, candidate, candidates, {});
                    candidates.push_back({nearby ? "added-near-skin" : "added-unrelated-green",
                        nearby ? Color{.94f,.86f,.82f} : Color{.05f,.98f,.05f}, true});
                    const auto start = Clock::now();
                    const auto mapped = map(mesh, candidate, candidates, {});
                    save_case(nearby ? "five-add-nearby-skin" : "five-add-unrelated-green", candidates, mapped, seconds(start));
                    auto& status = record["palette_cases"].back();
                    status["before_addition"] = mapping_summary(mesh, candidate, original_five,
                        std::vector<SC::PaletteSlot>(palette.begin(), palette.begin() + 5));
                    status["expected_behavior"] = "Extra candidates compete against fixed regional material centers; no requirement to use every candidate.";
                }
                record["palette_replay_sequences"] = Json::array();
                if (!skip_palette_matrix) for (const bool automatic : {false,true}) for (const bool increasing : {true,false}) {
                    Json sequence {{"kind", automatic ? "automatic" : "fixed"},
                        {"order", increasing ? "1-2-3-4-5-6-5-4-3-2-1" : "6-5-4-3-2-1-2-3-4-5-6"}, {"steps", Json::array()}};
                    for (size_t step = 0; step < 11; ++step) {
                        const size_t base = step <= 5 ? step + 1 : 11 - step;
                        const size_t count = increasing ? base : 7 - base;
                        std::string reason;
                        const auto candidates = automatic ? suggestion(count, reason)
                            : std::vector<SC::PaletteSlot>(palette.begin(), palette.begin() + count);
                        const auto start = Clock::now();
                        const auto mapped = map(mesh, candidate, candidates, automatic ? std::vector<Color>{} : card);
                        sequence["steps"].push_back({{"requested_count",count}, {"actual_count", candidates.size()},
                            {"same_count_assignments_restored",technical(signature(mapped) == first_mapping.at({automatic,count}),
                                "Palette sequence changed same-count assignments at count " + std::to_string(count))},
                            {"material_centers_unchanged",technical(centers(mapped) == frozen_centers,
                                "Palette sequence changed source material centers")},
                            {"mapping_seconds",seconds(start)}, {"recognition_reused",true}});
                    }
                    record["palette_replay_sequences"].push_back(std::move(sequence));
                }
                for (const size_t count : {size_t(1),size_t(2)}) {
                    auto reduced_palette = palette;
                    for (size_t i = count; i < reduced_palette.size(); ++i) reduced_palette[i].enabled = false;
                    SC::SlotMappingResult reduced, restored;
                    std::string error;
                    if (!SC::remap_palette_slots(after, reduced_palette, reduced, error)) throw std::runtime_error(error);
                    if (!SC::remap_palette_slots(reduced, palette, restored, error)) throw std::runtime_error(error);
                    record["six_" + std::to_string(count) + "_six_stable_slot_sequence"] = {
                        {"restored_faces_equal", technical(after.faces == restored.faces,"Stable-slot sequence failed whole-face restoration")},
                        {"restored_subfaces_equal", technical(after.subfaces == restored.subfaces,"Stable-slot sequence failed subface restoration")},
                        {"restored_slot_identity_equal", technical(signature(after) == signature(restored),"Stable-slot sequence failed slot identity restoration")},
                        {"reduced_automatic_faces", reduced.faces.size()}, {"temporary_substitutions", reduced.substituted_assignments},
                        {"recognition_reused",true}, {"material_centers_unchanged",technical(centers(reduced) == frozen_centers,
                            "Stable-slot sequence changed source material centers")}};
                }
                Json material_targets = Json::array();
                for (const auto& center : after.material_centers)
                    material_targets.push_back({{"id", std::to_string(center.id)}, {"label", unsigned(center.label)},
                        {"rgb", center.original_rgb}, {"face_count", center.face_count}});
                write_json(destination / "material-targets.json", material_targets);
                size_t changed_labels = 0;
                for (size_t id = 0; id < baseline.face_labels.size(); ++id)
                    if (baseline.face_labels[id] != candidate.face_labels[id]) ++changed_labels;
                record["boundary_changed_face_labels"] = changed_labels;
                record["baseline_subface_evidence"] = baseline.subface_labels.size();
                record["candidate_subface_evidence"] = candidate.subface_labels.size();
                write_json(destination / "slot-mapping-candidate.json", SC::encode_slot_mapping(after));
                // Unrecognized surfaces use the same direct physical-candidate
                // baseline as the local validation page. Semantic regions keep
                // the independent, region-aware mapping above.
                const PP::ColorTrialMapping fallback {true, card, card};
                record["persistence"] = Orca::SemanticValidation::verify_persistence(mesh, after, palette, fallback, destination);
                if (images) {
                    // The diagnostic record always keeps the full six-slot mapping. For
                    // visual acceptance we can render a selected palette count with the
                    // same frozen analysis, so a missing 4-color flat set is generated
                    // without changing the recognizer, camera or geometry inputs.
                    SC::SlotMappingResult image_baseline = before;
                    SC::SlotMappingResult image_candidate = after;
                    PP::ColorTrialMapping image_fallback = fallback;
                    if (image_palette_count != 0 && image_palette_count < palette.size()) {
                        const auto image_palette = std::vector<SC::PaletteSlot>(palette.begin(), palette.begin() + image_palette_count);
                        image_baseline = map(mesh, baseline, image_palette, card);
                        image_candidate = map(mesh, candidate, image_palette, card);
                        image_fallback = PP::ColorTrialMapping{
                            true,
                            std::vector<Color>(card.begin(), card.begin() + image_palette_count),
                            std::vector<Color>(card.begin(), card.begin() + image_palette_count)};
                    }
                    complete_display_faces(mesh, image_baseline, image_fallback, baseline, card);
                    complete_display_faces(mesh, image_candidate, image_fallback, candidate, card);
                    views(destination, mesh, image_baseline, image_candidate, image_fallback, size, baseline, candidate);
                    record["render_palette_count"] = image_palette_count == 0 ? palette.size() : image_palette_count;
                }
                record["palette_matrix"] = skip_palette_matrix ? "covered-by-automated-tests-or-separate-real-model-evidence" : "completed";
                record["total_seconds"] = seconds(model_start);
                record["peak_process_bytes"] = peak_memory();
                if (record["technical_failures"].empty()) record["status"] = "completed-awaiting-visual-review";
                else { record["status"] = "completed-with-technical-failures"; failed = true; }
                write_json(destination / "result.json", record);
            } catch (const std::exception& error) { record["status"] = "failed"; record["error"] = error.what(); failed = true; }
            summary["models"].push_back(record);
            write_json(output / "summary.json", summary);
            std::cout << name << ": " << record.value("status", "unknown");
            if (record.contains("error")) std::cout << " (" << record["error"].get<std::string>() << ')';
            std::cout << std::endl;
        }
        if (!matched) throw std::runtime_error("Unknown fixture name: " + selected);
        return failed ? 2 : 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
