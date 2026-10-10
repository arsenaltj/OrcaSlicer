#pragma once

#include "slic3r/GUI/AI/Model/BeautyShapeLock.hpp"
#include "slic3r/GUI/AI/Model/BeautyLeafEditing.hpp"
#include "slic3r/GUI/AI/Model/BeautyRecognition.hpp"
#include "PortraitSurfaceOwnership.hpp"
#include "PortraitColorInheritance.hpp"
#include "PortraitPaletteRoles.hpp"
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
        size_t unit_count=0;
    };
    AI::ShapeLockSet locks;
    std::vector<RGBA> base_colors;
    std::vector<size_t> reserved_faces;
    // Complete validated evidence context, including people without shape locks.
    std::vector<std::string> subjects;
    std::string runtime_fingerprint;
    nlohmann::json parent_samples = nlohmann::json::array();
    // Parent evidence may be refreshed without changing an accepted contour's identity.
    nlohmann::json parent_evidence_identity = nlohmann::json::object();
    // Optional current-source proposals live in their own content-addressed
    // sidecar. They do not change the accepted facial evidence identity.
    std::shared_ptr<const nlohmann::json> parent_proposal;
    std::map<size_t,std::string> parent_verified_roots;
    std::map<std::string,AI::SemanticColoring::Color> parent_source_colors;
    nlohmann::json parent_coverage_audit = nlohmann::json::object();
    std::shared_ptr<const PortraitSurfaceOwnership> surface_ownership;
    std::string ownership_diagnostic;
    std::shared_ptr<const PortraitColorInheritance> color_inheritance;
    std::shared_ptr<const AI::BeautySurfaceShapeLock> contour_locks;
    std::shared_ptr<const nlohmann::json> surface_partition;
    std::map<std::string,AI::SemanticColoring::Color> cell_colors;
    std::map<size_t,std::string> confirmed_parent_roots;
    std::map<std::string,std::string> confirmed_parent_cell_labels;
    std::string color_diagnostic;
    nlohmann::json parent_cleanup_audit = nlohmann::json::object();

    void validate_parent_identity() const {
        if (parent_evidence_identity.empty()) return; // Old source-bound contexts remain readable.
        const auto& identity=parent_evidence_identity;
        LocalSemanticEvidence::detail::keys(identity,{"source_sha256","geometry_id","face_count",
            "evidence_sha256","runtime_sha256","policy_sha256","host_runtime_fingerprint"});
        if(identity.at("source_sha256")!=locks.source_sha256 || identity.at("geometry_id")!=locks.geometry_id ||
            identity.at("face_count")!=locks.face_count) throw std::invalid_argument("Parent source identity changed.");
        for(const auto* key:{"evidence_sha256","runtime_sha256","policy_sha256","host_runtime_fingerprint"})
            if(!AI::ShapeLockSet::sha256(identity.at(key).get<std::string>()))
                throw std::invalid_argument("Invalid parent evidence identity.");
    }
    bool current_parent_evidence(const std::string& fingerprint) const {
        if(fingerprint.empty()) return false;
        validate_parent_identity();
        return parent_evidence_identity.empty() ? runtime_fingerprint==fingerprint && !parent_samples.empty() :
            parent_evidence_identity.at("host_runtime_fingerprint")==fingerprint;
    }
    void bind_parent_evidence(const LocalSemanticEvidence::Evidence& evidence,
                             const std::string& hash, const std::string& fingerprint) {
        const auto& identity=evidence.identity;
        if(identity.source_sha256!=locks.source_sha256 || identity.geometry_id!=locks.geometry_id ||
            identity.face_count!=locks.face_count || evidence.subjects!=subjects)
            throw std::invalid_argument("Fresh parent evidence belongs to a different source or subject.");
        const nlohmann::json binding={{"source_sha256",identity.source_sha256},{"geometry_id",identity.geometry_id},
            {"face_count",identity.face_count},{"evidence_sha256",hash},{"runtime_sha256",identity.runtime_sha256},
            {"policy_sha256",identity.policy_sha256},{"host_runtime_fingerprint",fingerprint}};
        PortraitShapeDetails candidate;candidate.locks.geometry_id=locks.geometry_id;
        candidate.locks.source_sha256=locks.source_sha256;candidate.locks.face_count=locks.face_count;
        candidate.parent_evidence_identity=binding;candidate.validate_parent_identity();
        parent_evidence_identity=binding;parent_samples=evidence.parent_samples;
        parent_proposal.reset();parent_verified_roots.clear();parent_source_colors.clear();
        parent_coverage_audit=nlohmann::json::object();
    }

    std::map<std::string,std::string> confirmed_parent_cells() const {
        auto result = confirmed_parent_cell_labels;
        for (const auto& root : confirmed_parent_roots) result.emplace("source:" + std::to_string(root.first), root.second);
        if (surface_partition) for (const auto& face : surface_partition->at("faces")) for (const auto& cell : face.at("cells")) {
            const auto label = cell.at("label").get<std::string>();
            if (label == "skin" || label == "cloth") result.emplace(cell.at("id").get<std::string>(), label);
        }
        return result;
    }
    void overlay_parent_labels(const AI::BeautyLeafEditing& editing, std::vector<int32_t>& labels) const {
        if (!editing.cells || labels.size() != editing.size()) return;
        const auto parents = confirmed_parent_cells();
        for (size_t i=0; i<labels.size(); ++i) {
            const auto found = parents.find(editing.cells->cell_id(i));
            if (found != parents.end()) labels[i] = int32_t(found->second == "skin" ?
                AI::SemanticColoring::Label::BodySkin : AI::SemanticColoring::Label::Clothes);
        }
    }
    void overlay_parent_selection(const std::string& region, const AI::BeautyLeafEditing& editing,
                                   AI::SurfaceSelectionPersistence::SelectionState& state) const {
        if (!editing.cells || state.selected.size() != editing.size()) return;
        const auto parents = confirmed_parent_cells();
        for (size_t i=0; i<state.selected.size(); ++i) {
            const auto found = parents.find(editing.cells->cell_id(i));
            if (found == parents.end()) continue;
            const bool matches = region == (found->second == "skin" ? "skin" : "clothes");
            const bool protected_cell = i < state.protected_faces.size() && state.protected_faces[i];
            state.selected[i] = matches && !protected_cell;
            if (i < state.foreground.size()) state.foreground[i] = state.selected[i];
            if (i < state.domain.size()) state.domain[i] = matches;
        }
    }

    bool derived_boundary() const { return bool(surface_partition) || bool(locks.leaf_domain); }
    bool has_locks() const { return contour_locks ? !contour_locks->document.at("locks").empty() : !locks.empty(); }
    std::string boundary_fingerprint() const {
        return contour_locks ? contour_locks->fingerprint(*surface_partition) : AI::beauty_leaf_digest(locks.encode().dump());
    }
    std::string mapping_fingerprint() const {
        return surface_partition ? surface_partition->at("partition_sha256").get<std::string>() :
            locks.leaf_domain ? locks.leaf_domain->fingerprint() : std::string();
    }

    bool compatible(const std::string& geometry, size_t faces) const {
        return locks.geometry_id == geometry && locks.face_count == faces && base_colors.size() == faces &&
            locks.compatible(geometry, locks.source_sha256, faces) &&
            (!surface_partition || (contour_locks && surface_partition->at("geometry_id")==geometry &&
                surface_partition->at("source_sha256")==locks.source_sha256 && surface_partition->at("face_count")==faces)) &&
            (!surface_ownership || (surface_ownership->editing_domain.canonical_geometry_id == geometry &&
                surface_ownership->editing_domain.source_face_count == faces));
    }
    static std::string parent(const std::string& label) {
        return label == "lb" || label == "rb" || label == "re" || label == "le" || label == "iris" ||
            label.compare(0,11,"periocular-")==0 ? "eyes" : "lips";
    }
    std::vector<Detail> catalog(const std::string& group = {}, const AI::BeautyLeafEditing* editing = nullptr) const {
        std::vector<Detail> result;
        if(contour_locks && !editing) return result;
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
        if(editing && editing->cells) for(auto& detail:result) {
            std::set<std::string> ids;
            for(auto face:detail.faces) ids.insert(editing->cells->cell_id(face));
            detail.unit_count=ids.size();
        }
        return result;
    }
    std::vector<uint8_t> ownership() const {
        std::vector<uint8_t> result(locks.face_count, 0);
        if(surface_partition) {
            for(const auto& face:surface_partition->at("faces")) for(const auto& cell:face.at("cells"))
                if(AI::ShapeLockSet::supported_label(cell.at("label")) ||
                   cell.at("label").get<std::string>().compare(0,5,"iris-")==0 ||
                   cell.at("label").get<std::string>().compare(0,11,"periocular-")==0)
                    result.at(face.at("source_face_id"))=1;
            return result;
        }
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

inline AI::SemanticColoring::FaceColors match_portrait_shape_role_colors(const PortraitShapeDetails& details,
    const std::vector<AI::SemanticColoring::Color>& palette,const std::vector<std::string>& roles) {
    if(!valid_portrait_roles(roles,palette.size())) throw std::invalid_argument("Invalid portrait color roles.");
    std::map<size_t,AI::SemanticColoring::Color> result;
    for(const auto& detail:details.catalog()) {
        const auto role=portrait_detail_role(detail.label,palette.size());
        if(role.empty()) continue;
        const auto slot=std::find(roles.begin(),roles.end(),role);
        if(slot==roles.end()) continue;
        for(auto face:detail.faces) result.emplace(face,palette.at(size_t(slot-roles.begin())));
    }
    return {result.begin(),result.end()};
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
inline nlohmann::json write_addressed(const boost::filesystem::path& root,const std::string& directory,
    const std::string& schema,const nlohmann::json& value) {
    const auto bytes=value.dump();
    if(bytes.size()>maximum_bytes) throw std::invalid_argument("Contour sidecar exceeds its size limit.");
    const auto hash=digest(bytes);const auto owner=root/directory;const auto file=owner/(hash+".json");
    if(boost::filesystem::is_symlink(owner) || boost::filesystem::is_symlink(file)) throw std::invalid_argument("Unsafe contour sidecar.");
    boost::filesystem::create_directories(owner);
    if(boost::filesystem::exists(file)) {
        if(AI::model_artifact_sha256(file)!=hash) throw std::invalid_argument("Contour sidecar changed.");
    } else {
        const auto temporary=owner/boost::filesystem::unique_path("contour-%%%%-%%%%.tmp");
        try {
            boost::filesystem::ofstream output(temporary,std::ios::binary);output<<bytes;output.close();
            if(!output) throw std::runtime_error("Cannot write contour sidecar.");
            boost::filesystem::rename(temporary,file);
        } catch(...) {boost::system::error_code error;boost::filesystem::remove(temporary,error);throw;}
    }
    return {{"schema",schema},{"path",directory+"/"+hash+".json"},{"sha256",hash}};
}
inline nlohmann::json read_addressed(const boost::filesystem::path& root,const std::string& directory,
    const std::string& schema,const nlohmann::json& reference) {
    const auto hash=reference.at("sha256").get<std::string>();
    if(reference.size()!=3 || reference.at("schema")!=schema || !AI::ShapeLockSet::sha256(hash) ||
       reference.at("path")!=directory+"/"+hash+".json") throw std::invalid_argument("Unsafe contour sidecar reference.");
    const auto file=root/directory/(hash+".json");
    if(boost::filesystem::is_symlink(root/directory) || boost::filesystem::is_symlink(file) ||
       !boost::filesystem::is_regular_file(file) || boost::filesystem::file_size(file)>maximum_bytes ||
       AI::model_artifact_sha256(file)!=hash) throw std::invalid_argument("Contour sidecar identity changed.");
    boost::filesystem::ifstream input(file,std::ios::binary);nlohmann::json value;input>>value;return value;
}
inline nlohmann::json save(const PortraitShapeDetails& details, const boost::filesystem::path& root) {
    if (!details.compatible(details.locks.geometry_id, details.locks.face_count)) throw std::invalid_argument("Invalid portrait shape context.");
    details.validate_parent_identity();
    verified_source(root, details.locks.source_sha256);
    auto lock_reference = details.locks.empty() ? nlohmann::json() : AI::write_shape_lock_sidecar(root, details.locks).encode();
    nlohmann::json document = {{"schema", "orca.portrait-shape-context/v1"}, {"identity", details.locks.encode()},
        {"shape_lock_ref", lock_reference}, {"base_colors", details.base_colors}, {"reserved_faces", details.reserved_faces},
        {"runtime_fingerprint", details.runtime_fingerprint}, {"subjects", details.subjects},
        {"parent_samples", details.parent_samples}};
    if(!details.parent_evidence_identity.empty())document["parent_evidence_identity"]=details.parent_evidence_identity;
    if(details.surface_partition) {
        document["schema"]="orca.portrait-shape-context/v2";
        const auto partition_ref=write_addressed(root,"surface-partitions","orca.surface-partition-reference/v1",*details.surface_partition);
        auto locks=details.contour_locks->document;locks["partition_ref"]=partition_ref;
        AI::BeautySurfaceShapeLock::decode(locks,*details.surface_partition,AI::BeautySurfaceShapeLock::identity(*details.surface_partition),partition_ref.at("sha256"));
        document["surface_partition_ref"]=partition_ref;
        document["surface_lock_ref"]=write_addressed(root,"shape-locks","orca.beauty-shape-lock-reference/v3",locks);
        document["color_diagnostic"]=details.color_diagnostic;
        document["cell_colors"]=details.cell_colors;
        document["confirmed_parent_roots"]=details.confirmed_parent_roots;
        document["confirmed_parent_cell_labels"]=details.confirmed_parent_cell_labels;
        document["parent_cleanup_audit"]=details.parent_cleanup_audit;
        document["parent_coverage_audit"]=details.parent_coverage_audit;
        if(details.parent_proposal)document["parent_proposal_ref"]=write_addressed(root,"parent-proposals",
            "orca.portrait-parent-proposal-reference/v1",*details.parent_proposal);
    }
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
    if (document.at("schema") != "orca.portrait-shape-context/v1" && document.at("schema")!="orca.portrait-shape-context/v2")
        throw std::invalid_argument("Invalid portrait shape context.");
    const auto saved_runtime = document.at("runtime_fingerprint").get<std::string>();
    const bool stale_runtime = saved_runtime != runtime_fingerprint;
    if (stale_runtime && !preserve_old_source && document.at("schema")!="orca.portrait-shape-context/v2")
        throw std::invalid_argument("Portrait shape runtime changed.");
    auto result = std::make_shared<PortraitShapeDetails>();
    const auto& identity = document.at("identity");
    result->locks = AI::ShapeLockSet::decode(identity, geometry, identity.at("source_sha256").get<std::string>(), count,
        identity.at("evidence_sha256").get<std::string>(), identity.at("runtime_sha256").get<std::string>(), identity.at("policy_sha256").get<std::string>());
    if (!result->locks.empty()) {
        const auto sidecar = AI::read_shape_lock_sidecar(root, AI::ShapeLockReference::decode(document.at("shape_lock_ref")),
            geometry, result->locks.source_sha256, count, result->locks.evidence_sha256, result->locks.runtime_sha256, result->locks.policy_sha256);
        if (sidecar.encode() != result->locks.encode()) throw std::invalid_argument("Portrait shape sidecar changed.");
    }
    if(document.at("schema")=="orca.portrait-shape-context/v2") {
        auto partition=read_addressed(root,"surface-partitions","orca.surface-partition-reference/v1",document.at("surface_partition_ref"));
        const auto lock_doc=read_addressed(root,"shape-locks","orca.beauty-shape-lock-reference/v3",document.at("surface_lock_ref"));
        const auto expected=AI::BeautySurfaceShapeLock::identity(partition);
        if(expected.at("geometry_id")!=geometry || expected.at("source_sha256")!=result->locks.source_sha256 ||
           expected.at("face_count")!=count || expected.at("evidence_sha256")!=result->locks.evidence_sha256 ||
           expected.at("runtime_sha256")!=result->locks.runtime_sha256 || expected.at("policy_sha256")!=result->locks.policy_sha256)
            throw std::invalid_argument("Contour context source changed.");
        result->contour_locks=std::make_shared<AI::BeautySurfaceShapeLock>(AI::BeautySurfaceShapeLock::decode(
            lock_doc,partition,expected,document.at("surface_partition_ref").at("sha256")));
        result->surface_partition=std::make_shared<nlohmann::json>(std::move(partition));
        result->cell_colors=document.value("cell_colors",std::map<std::string,AI::SemanticColoring::Color>{});
        result->confirmed_parent_roots=document.value("confirmed_parent_roots",std::map<size_t,std::string>{});
        result->confirmed_parent_cell_labels=document.value("confirmed_parent_cell_labels",std::map<std::string,std::string>{});
        result->color_diagnostic=document.value("color_diagnostic",std::string());
        result->parent_cleanup_audit=document.value("parent_cleanup_audit",nlohmann::json::object());
        result->parent_coverage_audit=document.value("parent_coverage_audit",nlohmann::json::object());
        if(!result->parent_coverage_audit.is_object() || result->parent_coverage_audit.dump().size()>16384)
            throw std::invalid_argument("Invalid parent coverage audit.");
        if(!result->parent_cleanup_audit.is_object() || result->parent_cleanup_audit.dump().size()>16384)
            throw std::invalid_argument("Invalid parent cleanup audit.");
        if(result->color_diagnostic.size()>1024) throw std::invalid_argument("Invalid contour color diagnostic.");
        std::set<std::string> cell_ids;
        std::set<size_t> explicit_roots;
        for (const auto& face : result->surface_partition->at("faces")) explicit_roots.insert(face.at("source_face_id").get<size_t>());
        for (const auto& root : result->confirmed_parent_roots)
            if (root.first >= count || explicit_roots.count(root.first) || (root.second != "skin" && root.second != "cloth"))
                throw std::invalid_argument("Confirmed parent root leaves its partition.");
        std::set<std::string> frozen_cells;
        for (const auto& lock : result->contour_locks->document.at("locks"))
            for (const auto* field : {"locked_cells", "nested_cells", "periocular_cells"})
                for (const auto& id : lock.value(field,nlohmann::json::array())) frozen_cells.insert(id.get<std::string>());
        for(const auto& face:result->surface_partition->at("faces")) for(const auto& cell:face.at("cells")) {
            const auto id=cell.at("id").get<std::string>(), label=cell.at("label").get<std::string>();
            cell_ids.insert(id);
            if (AI::ShapeLockSet::supported_label(label) || label=="teeth" ||
                label.compare(0,5,"iris-")==0 || label.compare(0,11,"periocular-")==0) frozen_cells.insert(id);
        }
        for(const auto& entry:result->confirmed_parent_cell_labels)
            if(!cell_ids.count(entry.first) || frozen_cells.count(entry.first) ||
               (entry.second!="skin" && entry.second!="cloth"))
                throw std::invalid_argument("Confirmed parent cell leaves its editable domain.");
        for(const auto& entry:result->cell_colors) {
            if(!cell_ids.count(entry.first)) throw std::invalid_argument("Contour color leaves its partition.");
            for(float c:entry.second) if(!std::isfinite(c) || c<0 || c>1) throw std::invalid_argument("Invalid contour color.");
        }
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
    if(document.contains("parent_evidence_identity")) {
        result->parent_evidence_identity=document.at("parent_evidence_identity");
        result->validate_parent_identity();
    }
    const bool stale_parent=result->parent_evidence_identity.empty() ? stale_runtime :
        result->parent_evidence_identity.at("host_runtime_fingerprint")!=runtime_fingerprint;
    if(!stale_parent && document.contains("parent_proposal_ref")) {
        auto proposal=read_addressed(root,"parent-proposals","orca.portrait-parent-proposal-reference/v1",document.at("parent_proposal_ref"));
        if(proposal.at("schema")!="orca.portrait-parent-proposal/v1" || result->parent_evidence_identity.empty())
            throw std::invalid_argument("Unbound parent proposal.");
        for(const auto* key:{"source_sha256","geometry_id","face_count","evidence_sha256","runtime_sha256","policy_sha256"})
            if(proposal.at("identity").at(key)!=result->parent_evidence_identity.at(key))throw std::invalid_argument("Saved parent proposal identity drift.");
        result->parent_proposal=std::make_shared<nlohmann::json>(std::move(proposal));
    }
    if (!stale_parent && document.contains("parent_samples")) {
        const auto& samples=document.at("parent_samples");
        if (!samples.is_array() || samples.size()>count) throw std::invalid_argument("Invalid parent observations.");
        size_t previous=0; bool first=true;
        for (const auto& sample:samples) {
            if (!sample.is_array() || sample.size()!=6) throw std::invalid_argument("Invalid parent observation.");
            const auto f=LocalSemanticEvidence::detail::index(sample.at(0),count-1);
            const auto subject=sample.at(1).get<std::string>(), label=sample.at(2).get<std::string>();
            const auto confidence=LocalSemanticEvidence::detail::probability(sample.at(3));
            const auto views=LocalSemanticEvidence::detail::index(sample.at(5),16);
            const auto pixels=LocalSemanticEvidence::detail::index(sample.at(4),16*4096*4096);
            if ((!first && f<=previous) || std::find(result->subjects.begin(),result->subjects.end(),subject)==result->subjects.end() ||
                (label!="face" && label!="nose" && label!="lr" && label!="rr" && label!="neck" &&
                 label!="hair" && label!="cloth" && label!="body-skin") || confidence<.85 || !views || pixels<views ||
                (label=="body-skin" && (confidence<.9 || views<2)))
                throw std::invalid_argument("Invalid parent observation support.");
            previous=f;first=false;
        }
        result->parent_samples=samples;
    }
    if(stale_parent)result->parent_evidence_identity=nlohmann::json::object();
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
    if (stale_runtime && !result->surface_partition) result->locks.locks.clear();
    return result;
}
} // namespace PortraitShapeCache
} // namespace Slic3r::GUI
