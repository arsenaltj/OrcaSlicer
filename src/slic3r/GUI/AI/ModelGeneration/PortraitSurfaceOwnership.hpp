#pragma once

#include "slic3r/GUI/AI/Model/BeautyShapeLock.hpp"

namespace Slic3r::GUI {

// Parent attribution is independent of the palette and of the frozen shape sidecar.
struct PortraitSurfaceOwnership {
    struct Region {
        std::string id, subject, parent, status, evidence_source;
        std::vector<std::string> views, risks;
        std::vector<AI::BeautyLeafKey> leaves;
    };
    nlohmann::json document;
    AI::BeautyLeafDomain editing_domain;
    std::vector<Region> regions;

    static PortraitSurfaceOwnership decode(const nlohmann::json& value, const AI::ShapeLockSet& locks) {
        using Domain = AI::BeautyLeafDomain;
        if (!locks.leaf_domain || value.at("schema") != "orca.portrait-surface-ownership/v1" ||
            value.at("geometry_id") != locks.geometry_id || value.at("source_sha256") != locks.source_sha256 ||
            value.at("evidence_sha256") != locks.evidence_sha256 || value.at("face_count") != locks.face_count ||
            value.at("boundary_sha256") != AI::beauty_leaf_digest(locks.encode().dump()) ||
            value.at("policy_sha256") != AI::beauty_leaf_digest(value.at("policy").dump()))
            throw std::invalid_argument("Portrait parent ownership identity changed.");
        PortraitSurfaceOwnership result; result.document = value;
        result.editing_domain = Domain::decode(value.at("editing_domain"), locks.geometry_id,
            locks.face_count, value.at("editing_mapping_sha256"));
        const auto mapping = result.editing_domain.all_leaves();
        std::set<AI::BeautyLeafKey> frozen;
        std::set<std::string> subjects, ids;
        for (const auto& lock : locks.locks) {
            subjects.insert(lock.subject_id);
            frozen.insert(lock.locked_leaves.begin(), lock.locked_leaves.end());
        }
        for (const auto& key : frozen) if (!std::binary_search(mapping.begin(), mapping.end(), key))
            throw std::invalid_argument("Parent partition changes a reviewed feature leaf.");
        std::vector<AI::BeautyLeafKey> claimed;
        for (const auto& item : value.at("regions")) {
            Region region;
            region.id = item.at("id"); region.subject = item.at("subject_id");
            region.parent = item.at("parent_label"); region.status = item.at("status");
            region.evidence_source = item.at("evidence_source");
            region.views = item.at("view_ids").get<std::vector<std::string>>();
            region.risks = item.at("risks").get<std::vector<std::string>>();
            const std::set<std::string> views(region.views.begin(), region.views.end());
            if (region.id.empty() || !ids.insert(region.id).second || !subjects.count(region.subject) ||
                (region.parent != "face" && region.parent != "cloth") ||
                (region.status != "CONFIRMED_PARENT" && region.status != "SUPPORTED_PARENT_PROPOSAL") ||
                views.size() < 2 || views.count("") || region.evidence_source.empty())
                throw std::invalid_argument("Invalid or conflicting portrait parent attribution.");
            region.leaves = Domain::decode_keys(item.at("leaves"), locks.face_count);
            if (region.leaves.empty()) throw std::invalid_argument("Empty portrait parent region.");
            for (const auto& key : region.leaves) if (frozen.count(key) ||
                !std::binary_search(mapping.begin(), mapping.end(), key))
                throw std::invalid_argument("Parent attribution crosses a frozen feature or its editing partition.");
            claimed.insert(claimed.end(), region.leaves.begin(), region.leaves.end());
            result.regions.push_back(std::move(region));
        }
        std::sort(claimed.begin(), claimed.end()); Domain::validate_keys(claimed, locks.face_count);
        return result;
    }
    void validate(const AI::ShapeLockSet& locks) const {
        const auto checked = decode(document, locks);
        if (checked.editing_domain.encode() != editing_domain.encode() || checked.regions.size() != regions.size())
            throw std::invalid_argument("Portrait parent editing mapping changed.");
        for (size_t i = 0; i < regions.size(); ++i) {
            const auto& a = checked.regions[i]; const auto& b = regions[i];
            if (a.id != b.id || a.subject != b.subject || a.parent != b.parent || a.status != b.status ||
                a.evidence_source != b.evidence_source || a.views != b.views || a.risks != b.risks || a.leaves != b.leaves)
                throw std::invalid_argument("Portrait parent region changed after validation.");
        }
    }
    std::string fingerprint() const { return AI::beauty_leaf_digest(document.dump()); }
    void overlay_labels(const std::vector<AI::BeautyLeafKey>& keys, std::vector<int32_t>& labels) const {
        if (keys.size()!=labels.size()) throw std::invalid_argument("Parent guidance editing mapping changed.");
        for (const auto& region:regions) for (const auto& key:region.leaves) {
            const auto found=std::lower_bound(keys.begin(),keys.end(),key);
            if(found==keys.end() || !(*found==key)) throw std::invalid_argument("Parent guidance leaves its partition.");
            labels[size_t(found-keys.begin())]=region.parent=="face"?3:5;
        }
    }
    void overlay_selection(const std::string& group,const std::vector<AI::BeautyLeafKey>& keys,
                           AI::SurfaceSelectionPersistence::SelectionState& state) const {
        if(group!="skin" && group!="clothes") return;
        if(state.selected.size()!=keys.size()) throw std::invalid_argument("Parent selection editing mapping changed.");
        std::set<size_t> roots;
        for(const auto& region:regions) for(const auto& key:region.leaves) roots.insert(key.source_face_id);
        for(size_t i=0;i<keys.size();++i) if(roots.count(keys[i].source_face_id)) {
            state.selected[i]=0;
            if(i<state.foreground.size()) state.foreground[i]=0;
            if(i<state.domain.size()) state.domain[i]=0;
        }
        for(const auto& region:regions) if((region.parent=="face" && group=="skin") || (region.parent=="cloth" && group=="clothes"))
            for(const auto& key:region.leaves) {
                const auto found=std::lower_bound(keys.begin(),keys.end(),key);
                if(found==keys.end() || !(*found==key)) throw std::invalid_argument("Parent selection leaves its partition.");
                const size_t i=size_t(found-keys.begin());
                if(i<state.protected_faces.size() && state.protected_faces[i]) continue;
                state.selected[i]=1;
                if(i<state.foreground.size()) state.foreground[i]=1;
                if(i<state.domain.size()) state.domain[i]=1;
            }
    }
};

namespace PortraitOwnershipCache {
inline nlohmann::json save(const PortraitSurfaceOwnership& ownership, const AI::ShapeLockSet& locks,
                          const boost::filesystem::path& root) {
    ownership.validate(locks);
    const auto hash = ownership.fingerprint(), bytes = ownership.document.dump();
    const auto directory = root / "portrait-ownership";
    if (boost::filesystem::is_symlink(directory)) throw std::invalid_argument("Unsafe portrait ownership cache.");
    boost::filesystem::create_directories(directory);
    const auto file = directory / (hash + ".json");
    if (boost::filesystem::exists(file)) {
        if (AI::model_artifact_sha256(file) != hash) throw std::invalid_argument("Portrait ownership cache changed.");
    } else {
        const auto temporary = directory / boost::filesystem::unique_path("parent-%%%%-%%%%.tmp");
        boost::filesystem::ofstream stream(temporary, std::ios::binary); stream << bytes; stream.close();
        if (!stream) throw std::runtime_error("Cannot save portrait ownership cache.");
        boost::filesystem::rename(temporary, file);
    }
    return {{"schema", "orca.portrait-surface-ownership-reference/v1"},
        {"path", "portrait-ownership/"+hash+".json"}, {"sha256", hash}};
}
inline std::shared_ptr<const PortraitSurfaceOwnership> load(const nlohmann::json& ref,
    const boost::filesystem::path& root, const AI::ShapeLockSet& locks) {
    const auto hash = ref.at("sha256").get<std::string>();
    if (ref.at("schema") != "orca.portrait-surface-ownership-reference/v1" || !AI::ShapeLockSet::sha256(hash) ||
        ref.at("path") != "portrait-ownership/"+hash+".json")
        throw std::invalid_argument("Unsafe portrait ownership reference.");
    const auto file = root / "portrait-ownership" / (hash+".json");
    if (boost::filesystem::is_symlink(file.parent_path()) || boost::filesystem::is_symlink(file) ||
        !boost::filesystem::is_regular_file(file) || boost::filesystem::file_size(file) > 128ULL*1024*1024 ||
        AI::model_artifact_sha256(file) != hash) throw std::invalid_argument("Portrait ownership sidecar changed.");
    boost::filesystem::ifstream stream(file, std::ios::binary); nlohmann::json value; stream >> value;
    return std::make_shared<PortraitSurfaceOwnership>(PortraitSurfaceOwnership::decode(value, locks));
}
}
} // namespace Slic3r::GUI
