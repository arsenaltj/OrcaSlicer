#pragma once

#include "slic3r/GUI/AI/Model/BeautyShapeLock.hpp"
#include "slic3r/GUI/AI/Model/BeautyLeafEditing.hpp"
#include "slic3r/GUI/AI/Model/BeautyRecognition.hpp"
#include "PortraitSurfaceOwnership.hpp"
#include "PortraitColorInheritance.hpp"
#include "slic3r/AI/ModelGeneration/SemanticColoring/SemanticColoring.hpp"
#include <iomanip>
#include <sstream>

namespace Slic3r::GUI {

// One source-bound view supplies coloring, picking and puzzle boundaries.
struct PortraitShapeDetails {
    struct Detail {
        std::string key, subject, label, parent, status;
        std::vector<size_t> faces;
        std::vector<std::string> reasons;
    };
    AI::ShapeLockSet locks;
    std::vector<RGBA> base_colors;
    std::vector<size_t> reserved_faces;
    // Complete validated evidence context, including people without shape locks.
    std::vector<std::string> subjects;
    std::string runtime_fingerprint;
    std::shared_ptr<const PortraitSurfaceOwnership> surface_ownership;
    std::string ownership_diagnostic;
    std::shared_ptr<const PortraitColorInheritance> color_inheritance;

    bool compatible(const std::string& geometry, size_t faces) const {
        return locks.geometry_id == geometry && locks.face_count == faces && base_colors.size() == faces &&
            locks.compatible(geometry, locks.source_sha256, faces) &&
            (!surface_ownership || (surface_ownership->editing_domain.canonical_geometry_id == geometry &&
                surface_ownership->editing_domain.source_face_count == faces));
    }
    static std::string parent(const std::string& label) {
        return label == "lb" || label == "rb" || label == "re" || label == "le" || label == "iris" ? "eyes" : "lips";
    }
    std::vector<Detail> catalog(const std::string& group = {}, const AI::BeautyLeafEditing* editing = nullptr) const {
        std::vector<Detail> result;
        for (const auto& lock : (editing ? editing->editing_locks.locks : locks.locks)) {
            if (!lock.nested_faces.empty() && lock.label != "re" && lock.label != "le")
                throw std::invalid_argument("Nested iris detail requires an eye parent.");
            if (!group.empty() && parent(lock.label) != group) continue;
            std::vector<size_t> outer;
            std::set_difference(lock.locked_faces.begin(), lock.locked_faces.end(),
                lock.nested_faces.begin(), lock.nested_faces.end(), std::back_inserter(outer));
            if (!outer.empty()) result.push_back({lock.subject_id + ":" + lock.label, lock.subject_id, lock.label,
                parent(lock.label), lock.status, std::move(outer), lock.reasons});
            if (!lock.nested_faces.empty()) result.push_back({lock.subject_id + ":" + lock.label + ":iris", lock.subject_id,
                "iris", "eyes", lock.status, lock.nested_faces, lock.reasons});
        }
        return result;
    }
    std::vector<uint8_t> ownership() const {
        std::vector<uint8_t> result(locks.face_count, 0);
        for (const auto& lock : locks.locks) for (const auto face : lock.locked_faces) result.at(face) = 1;
        return result;
    }
    AI::BeautyGuidance guidance() const {
        AI::BeautyGuidance result;
        result.labels.assign(locks.face_count, -1);
        for (const auto& detail : catalog()) {
            const auto id = int32_t(result.names.size());
            result.names.push_back(detail.label);
            for (const auto face : detail.faces) result.labels.at(face) = id;
        }
        return result;
    }
};

inline bool native_facial_detail(AI::SemanticColoring::Label label) {
    using Label = AI::SemanticColoring::Label;
    return label == Label::EyeSclera || label == Label::Iris || label == Label::Eyebrow ||
        label == Label::Lips || label == Label::MouthInterior;
}

// Remove the native fine-region lane even when local landmarks are unavailable.
inline void compose_portrait_shapes(const AI::SemanticColoring::Analysis& analysis,
    const PortraitShapeDetails* details, AI::SemanticColoring::FaceColors& faces,
    AI::SemanticColoring::SubfaceColors& subfaces, const AI::SemanticColoring::FaceColors& locked_colors = {}) {
    std::vector<uint8_t> reserved(analysis.face_labels.size(), 0);
    for (size_t f = 0; f < reserved.size(); ++f) reserved[f] = native_facial_detail(analysis.face_labels[f]);
    for (const auto& child : analysis.subface_labels)
        if (child.face_id < reserved.size() && native_facial_detail(child.label)) reserved[child.face_id] = 1;
    if (details) {
        if (!details->compatible(analysis.geometry_id, reserved.size())) throw std::invalid_argument("Portrait shape identity changed.");
        for (const auto f : details->reserved_faces) reserved.at(f) = 1;
        for (const auto& lock : details->locks.locks) for (const auto f : lock.locked_faces) reserved.at(f) = 1;
    }
    faces.erase(std::remove_if(faces.begin(), faces.end(), [&](const auto& item) {
        return item.first >= reserved.size() || reserved[item.first];
    }), faces.end());
    subfaces.erase(std::remove_if(subfaces.begin(), subfaces.end(), [&](const auto& item) {
        return item.face_id >= reserved.size() || reserved[item.face_id];
    }), subfaces.end());
    if (!details) return;
    const auto owned = details->ownership();
    std::vector<uint8_t> oral(owned.size(), 0);
    for (const auto& lock : details->locks.locks) if (lock.label == "imouth")
        for (const auto f : lock.locked_faces) oral[f] = 1;
    for (const auto& item : locked_colors) {
        if (item.first >= owned.size() || !owned[item.first] || oral[item.first])
            throw std::invalid_argument("Automatic detail color leaves its shape lock.");
        faces.push_back(item);
    }
    std::sort(faces.begin(), faces.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
}

inline AI::SemanticColoring::FaceColors match_portrait_shape_colors(const PortraitShapeDetails& details,
    const AI::BeautySurface& surface, const std::vector<AI::PhysicalFilamentChannel>& palette,
    const AI::SemanticColoring::FaceColors& parent_colors = {}) {
    if (!details.compatible(surface.geometry_id, surface.face_patch.size()))
        throw std::invalid_argument("Shape source colors belong to another surface.");
    if (!AI::is_valid_physical_channel_set(palette)) return {};
    auto puzzle = AI::BeautyPuzzle::create(surface);
    details.locks.isolate(puzzle, surface);
    const auto catalog = details.catalog();
    std::vector<uint8_t> allowed(details.locks.face_count, 0);
    for (const auto& detail : catalog) if (detail.label != "imouth") for (const auto f : detail.faces) allowed[f] = 1;
    std::set<uint32_t> blocked;
    for (size_t f = 0; f < allowed.size(); ++f) if (!allowed[f]) blocked.insert(puzzle.face_piece[f]);
    std::map<size_t, AI::SemanticColoring::Color> parents;
    for (const auto& entry : parent_colors) parents.emplace(entry);
    std::map<uint32_t, std::map<AI::SemanticColoring::Color, double>> parent_votes;
    for (const auto& entry : parents) if (entry.first < allowed.size() && allowed[entry.first] && !blocked.count(puzzle.face_piece[entry.first]))
        parent_votes[puzzle.face_piece[entry.first]][entry.second] += std::max(surface.areas[entry.first], 1e-15);
    const auto lab = [](const std::array<float, 4>& rgb) {
        std::array<float, 3> linear {rgb[0], rgb[1], rgb[2]};
        for (auto& c : linear) c = c <= .04045f ? c / 12.92f : std::pow((c + .055f) / 1.055f, 2.4f);
        const float l = std::cbrt(.4122214708f*linear[0] + .5363325363f*linear[1] + .0514459929f*linear[2]);
        const float m = std::cbrt(.2119034982f*linear[0] + .6806995451f*linear[1] + .1073969566f*linear[2]);
        const float s = std::cbrt(.0883024619f*linear[0] + .2817188376f*linear[1] + .6299787005f*linear[2]);
        return std::array<float, 3> {.2104542553f*l + .793617785f*m - .0040720468f*s,
            1.9779984951f*l - 2.428592205f*m + .4505937099f*s,
            .0259040371f*l + .7827717662f*m - .808675766f*s};
    };
    // Keep the target workbench's facial palette protections when matching
    // isolated locks: a small baked warm shadow is not a lip-colored material.
    for (const auto& detail : catalog) {
        if (detail.label == "imouth") continue;
        struct Mean { std::array<double, 3> rgb {}; double area = 0.; };
        std::map<uint32_t, Mean> means;
        for (const auto f : detail.faces) {
            const auto id = puzzle.face_piece[f];
            if (blocked.count(id)) continue;
            auto& mean = means[id];
            const double area = std::max(surface.areas[f], 1e-15);
            mean.area += area;
            for (size_t c = 0; c < 3; ++c) mean.rgb[c] += area * details.base_colors[f][c];
        }
        for (const auto& entry : means) {
            const auto& mean = entry.second;
            const std::array<float, 4> source {float(mean.rgb[0]/mean.area), float(mean.rgb[1]/mean.area), float(mean.rgb[2]/mean.area), 1};
            const auto center = lab(source);
            const bool brow = detail.label == "lb" || detail.label == "rb";
            const bool eye = detail.label == "le" || detail.label == "re" || detail.label == "iris";
            const bool natural_dark = (brow || detail.label == "iris") && center[0] < .67f &&
                std::hypot(center[1], center[2]) < .12f && center[1] >= -.012f && center[2] >= -.012f;
            // A risky landmark band may land on skin below the pigment. Keep
            // its appearance until it is reviewed; the selection remains editable.
            if (brow && detail.status == "PROTECTED_SHAPE_UNCERTAIN" && !natural_dark &&
                center[0] >= .67f && center[1] >= 0.f && center[2] >= 0.f && std::hypot(center[1], center[2]) < .12f) {
                const auto votes = parent_votes.find(entry.first);
                if (votes != parent_votes.end() && !votes->second.empty()) {
                    const auto chosen = std::max_element(votes->second.begin(), votes->second.end(),
                        [](const auto& a, const auto& b) { return a.second < b.second; });
                    puzzle.colors[entry.first] = {chosen->first[0], chosen->first[1], chosen->first[2], 1};
                }
                continue;
            }
            auto channels = palette;
            for (auto& channel : channels) {
                const auto target = lab(AI::BeautyPuzzle::filament_color(channel));
                const float chroma = std::hypot(target[1], target[2]);
                const bool red = chroma >= .055f && target[1] >= .045f && target[1] > target[2] * 1.25f + .01f;
                if ((brow || eye) && red) channel.compatible = false;
                if (natural_dark && (target[0] > .72f || chroma >= .12f || target[1] < -.012f || target[2] < -.012f))
                    channel.compatible = false;
            }
            if (std::none_of(channels.begin(), channels.end(), [](const auto& c) { return c.compatible; })) continue;
            AI::BeautyPuzzle matcher;
            matcher.palette = std::move(channels);
            const auto slot = matcher.nearest_filament(source);
            const auto found = std::find_if(palette.begin(), palette.end(), [slot](const auto& c) { return c.slot == slot; });
            puzzle.colors[entry.first] = AI::BeautyPuzzle::filament_color(*found);
        }
    }
    AI::SemanticColoring::FaceColors result;
    for (size_t f = 0; f < allowed.size(); ++f) if (allowed[f] && !blocked.count(puzzle.face_piece[f])) {
        const auto color = puzzle.colors.find(puzzle.face_piece[f]);
        if (color != puzzle.colors.end()) result.push_back({f, {color->second[0], color->second[1], color->second[2]}});
    }
    return result;
}

namespace PortraitShapeCache {
inline constexpr size_t maximum_bytes = 128ULL * 1024 * 1024;
inline boost::filesystem::path source_path(const boost::filesystem::path& root, const std::string& hash) {
    if (!AI::ShapeLockSet::sha256(hash)) throw std::invalid_argument("Invalid portrait source identity.");
    return root / "portrait-sources" / (hash + ".glb");
}
inline boost::filesystem::path verified_source(const boost::filesystem::path& root, const std::string& hash) {
    const auto file = source_path(root, hash);
    if (boost::filesystem::is_symlink(root / "portrait-sources") || boost::filesystem::is_symlink(file) ||
        !boost::filesystem::is_regular_file(file) || AI::model_artifact_sha256(file) != hash)
        throw std::invalid_argument("Original portrait source is missing or changed.");
    return file;
}
inline boost::filesystem::path preserve_source(const boost::filesystem::path& root,
    const boost::filesystem::path& source, const std::string& hash) {
    if (!boost::filesystem::is_regular_file(source) || AI::model_artifact_sha256(source) != hash)
        throw std::invalid_argument("Portrait source changed before preservation.");
    const auto target = source_path(root, hash);
    if (boost::filesystem::is_symlink(target.parent_path())) throw std::invalid_argument("Unsafe portrait source cache.");
    boost::filesystem::create_directories(target.parent_path());
    if (!boost::filesystem::exists(target)) {
        const auto temporary = target.parent_path() / boost::filesystem::unique_path("source-%%%%-%%%%.tmp");
        try {
            boost::filesystem::copy_file(source, temporary);
            if (AI::model_artifact_sha256(temporary) != hash) throw std::runtime_error("Portrait source changed during preservation.");
            boost::filesystem::rename(temporary, target);
        } catch (...) { boost::system::error_code ignored; boost::filesystem::remove(temporary, ignored); throw; }
    }
    return verified_source(root, hash);
}
inline std::string digest(const std::string& bytes) {
    unsigned char output[EVP_MAX_MD_SIZE]; unsigned int size = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), output, &size, EVP_sha256(), nullptr) != 1 || size != 32) return {};
    std::ostringstream stream;
    for (unsigned int i = 0; i < size; ++i) stream << std::hex << std::setw(2) << std::setfill('0') << unsigned(output[i]);
    return stream.str();
}
inline nlohmann::json save(const PortraitShapeDetails& details, const boost::filesystem::path& root) {
    if (!details.compatible(details.locks.geometry_id, details.locks.face_count)) throw std::invalid_argument("Invalid portrait shape context.");
    verified_source(root, details.locks.source_sha256);
    auto lock_reference = details.locks.empty() ? nlohmann::json() : AI::write_shape_lock_sidecar(root, details.locks).encode();
    nlohmann::json document = {{"schema", "orca.portrait-shape-context/v1"}, {"identity", details.locks.encode()},
        {"shape_lock_ref", lock_reference}, {"base_colors", details.base_colors}, {"reserved_faces", details.reserved_faces},
        {"runtime_fingerprint", details.runtime_fingerprint}, {"subjects", details.subjects}};
    if (details.surface_ownership)
        document["ownership_ref"] = PortraitOwnershipCache::save(*details.surface_ownership, details.locks, root);
    if (details.color_inheritance)
        document["color_inheritance_ref"] = PortraitColorInheritanceCache::save(*details.color_inheritance,details.locks,root);
    const auto bytes = document.dump();
    if (bytes.size() > maximum_bytes) throw std::invalid_argument("Portrait shape context exceeds its size limit.");
    const auto hash = digest(bytes);
    const auto directory = root / "portrait-shapes";
    if (boost::filesystem::is_symlink(directory)) throw std::invalid_argument("Unsafe portrait shape cache.");
    boost::filesystem::create_directories(directory);
    const auto file = directory / (hash + ".json");
    if (boost::filesystem::exists(file)) {
        if (AI::model_artifact_sha256(file) != hash) throw std::invalid_argument("Portrait shape cache changed.");
    } else {
        const auto temporary = directory / boost::filesystem::unique_path("shape-%%%%-%%%%.tmp");
        boost::filesystem::ofstream stream(temporary, std::ios::binary); stream << bytes; stream.close();
        if (!stream) throw std::runtime_error("Cannot write portrait shape cache.");
        boost::filesystem::rename(temporary, file);
    }
    return {{"schema", "orca.portrait-shape-reference/v1"}, {"path", "portrait-shapes/" + hash + ".json"}, {"sha256", hash}};
}
inline std::shared_ptr<const PortraitShapeDetails> load(const nlohmann::json& reference,
    const boost::filesystem::path& root, const std::string& geometry, size_t count,
    const std::string& runtime_fingerprint, bool preserve_old_source = false) {
    if (reference.value("schema", std::string()) != "orca.portrait-shape-reference/v1")
        throw std::invalid_argument("Invalid portrait shape reference.");
    const auto hash = reference.at("sha256").get<std::string>();
    if (!AI::ShapeLockSet::sha256(hash) || reference.at("path") != "portrait-shapes/" + hash + ".json")
        throw std::invalid_argument("Unsafe portrait shape cache path.");
    const auto file = root / "portrait-shapes" / (hash + ".json");
    if (boost::filesystem::is_symlink(root / "portrait-shapes") || boost::filesystem::is_symlink(file) ||
        !boost::filesystem::is_regular_file(file) || boost::filesystem::file_size(file) > maximum_bytes ||
        AI::model_artifact_sha256(file) != hash) throw std::invalid_argument("Portrait shape cache hash changed.");
    boost::filesystem::ifstream stream(file, std::ios::binary); nlohmann::json document; stream >> document;
    if (document.at("schema") != "orca.portrait-shape-context/v1") throw std::invalid_argument("Invalid portrait shape context.");
    const auto saved_runtime = document.at("runtime_fingerprint").get<std::string>();
    const bool stale_runtime = saved_runtime != runtime_fingerprint;
    if (stale_runtime && !preserve_old_source) throw std::invalid_argument("Portrait shape runtime changed.");
    auto result = std::make_shared<PortraitShapeDetails>();
    const auto& identity = document.at("identity");
    result->locks = AI::ShapeLockSet::decode(identity, geometry, identity.at("source_sha256").get<std::string>(), count,
        identity.at("evidence_sha256").get<std::string>(), identity.at("runtime_sha256").get<std::string>(), identity.at("policy_sha256").get<std::string>());
    if (!result->locks.empty()) {
        const auto sidecar = AI::read_shape_lock_sidecar(root, AI::ShapeLockReference::decode(document.at("shape_lock_ref")),
            geometry, result->locks.source_sha256, count, result->locks.evidence_sha256, result->locks.runtime_sha256, result->locks.policy_sha256);
        if (sidecar.encode() != result->locks.encode()) throw std::invalid_argument("Portrait shape sidecar changed.");
    }
    result->base_colors = document.at("base_colors").get<std::vector<RGBA>>();
    if (result->base_colors.size() != count) throw std::invalid_argument("Portrait source colors changed.");
    for (const auto& color : result->base_colors) for (const auto channel : color)
        if (!std::isfinite(channel) || channel < 0.f || channel > 1.f) throw std::invalid_argument("Invalid portrait source color.");
    result->reserved_faces = AI::ShapeLockSet::faces(document.at("reserved_faces"), count);
    if (document.contains("subjects")) {
        result->subjects = document.at("subjects").get<std::vector<std::string>>();
        std::set<std::string> seen;
        for (const auto& subject : result->subjects)
            if (!LocalSemanticEvidence::detail::identifier(subject) || !seen.insert(subject).second)
                throw std::invalid_argument("Invalid portrait subject context.");
    }
    result->runtime_fingerprint = saved_runtime;
    if (!stale_runtime && document.contains("ownership_ref")) {
        try { result->surface_ownership = PortraitOwnershipCache::load(document.at("ownership_ref"), root, result->locks); }
        catch (const std::exception& e) { result->ownership_diagnostic = e.what(); }
    }
    if (!stale_runtime && document.contains("color_inheritance_ref")) {
        try { result->color_inheritance = PortraitColorInheritanceCache::load(document.at("color_inheritance_ref"),root,result->locks); }
        catch (const std::exception& e) {
            result->surface_ownership.reset(); result->ownership_diagnostic = e.what();
        }
    }
    verified_source(root, result->locks.source_sha256);
    // An old runtime may supply verified source colors, never old boundaries.
    if (stale_runtime) result->locks.locks.clear();
    return result;
}
} // namespace PortraitShapeCache
} // namespace Slic3r::GUI
