#pragma once

#include "BeautyLeafEditing.hpp"
#include "BeautyAppearance.hpp"
#include "slic3r/AI/ModelGeneration/SemanticColoring/SemanticColoring.hpp"

namespace Slic3r::AI {

// Exact draft state is independent of the legacy, conservatively collapsed masks.
struct BeautyLeafEdits {
    std::string geometry_id, source_sha256, mapping_sha256, boundary_sha256;
    std::map<BeautyLeafKey, SemanticColoring::Color> colors;
    SurfaceSelectionPersistence::SelectionState selection;

    static BeautyLeafEdits capture(const BeautyLeafEditing& editing, const ShapeLockSet& locks,
        const std::map<BeautyLeafKey, SemanticColoring::Color>& colors,
        const SurfaceSelectionPersistence::SelectionState& selection) {
        BeautyLeafEdits result{locks.geometry_id, locks.source_sha256, editing.domain.fingerprint(),
            beauty_leaf_digest(locks.encode().dump()), colors, selection};
        for (auto* mask : {&result.selection.protected_faces,&result.selection.foreground,&result.selection.domain})
            if (mask->empty()) mask->assign(editing.keys.size(),0);
        result.validate(editing, locks);
        return result;
    }
    void validate(const BeautyLeafEditing& editing, const ShapeLockSet& locks) const {
        if (!locks.leaf_domain || editing.domain.canonical_geometry_id!=locks.geometry_id ||
            editing.domain.source_face_count!=locks.face_count ||
            editing.shape_boundary_sha256!=beauty_leaf_digest(locks.encode().dump()) ||
            geometry_id != locks.geometry_id || source_sha256 != locks.source_sha256 ||
            mapping_sha256 != editing.domain.fingerprint() || boundary_sha256 != beauty_leaf_digest(locks.encode().dump()))
            throw std::invalid_argument("Leaf draft identity changed.");
        for (const auto& entry : colors) {
            editing.index(entry.first);
            for (const auto c : entry.second) if (!std::isfinite(c) || c < 0 || c > 1)
                throw std::invalid_argument("Invalid leaf draft color.");
        }
        for (const auto* mask : {&selection.selected, &selection.protected_faces, &selection.foreground, &selection.domain}) {
            if (mask->size() != editing.keys.size()) throw std::invalid_argument("Leaf draft mask changed.");
            for (auto value : *mask) if (value > 1) throw std::invalid_argument("Invalid leaf draft mask.");
        }
    }
    nlohmann::json encode(const BeautyLeafEditing& editing, const ShapeLockSet& locks) const {
        validate(editing, locks);
        nlohmann::json result{{"schema", "orca.beauty-leaf-edit/v1"}, {"geometry_id",geometry_id},
            {"source_sha256",source_sha256}, {"mapping_sha256",mapping_sha256}, {"boundary_sha256",boundary_sha256},
            {"face_count",editing.domain.source_face_count}, {"colors",nlohmann::json::array()}};
        for (const auto& entry : colors)
            result["colors"].push_back({entry.first.source_face_id,entry.first.depth,entry.first.path,entry.second});
        const std::array<const char*,4> names{"selected","protected","foreground","domain"};
        const std::array<const std::vector<uint8_t>*,4> masks{&selection.selected,&selection.protected_faces,&selection.foreground,&selection.domain};
        for (size_t m=0;m<names.size();++m) {
            std::vector<BeautyLeafKey> keys;
            for (size_t i=0;i<editing.keys.size();++i) if ((*masks[m])[i]) keys.push_back(editing.keys[i]);
            result[names[m]] = BeautyLeafDomain::encode_keys(keys);
        }
        return result;
    }
    static BeautyLeafEdits decode(const nlohmann::json& value, const BeautyLeafEditing& editing, const ShapeLockSet& locks) {
        if (!value.is_object() || value.size()!=11 || value.at("schema")!="orca.beauty-leaf-edit/v1" ||
            value.at("face_count")!=editing.domain.source_face_count || !value.at("colors").is_array() ||
            value.at("colors").size()>editing.keys.size()) throw std::invalid_argument("Invalid leaf draft record.");
        BeautyLeafEdits result;
        result.geometry_id=value.at("geometry_id"); result.source_sha256=value.at("source_sha256");
        result.mapping_sha256=value.at("mapping_sha256"); result.boundary_sha256=value.at("boundary_sha256");
        std::vector<BeautyLeafKey> keys;
        for (const auto& entry : value.at("colors")) {
            if (!entry.is_array() || entry.size()!=4) throw std::invalid_argument("Invalid leaf color record.");
            const auto decoded=BeautyLeafDomain::decode_keys(nlohmann::json::array({{entry[0],entry[1],entry[2]}}),locks.face_count);
            keys.push_back(decoded.front());
            result.colors.emplace(decoded.front(),entry[3].get<SemanticColoring::Color>());
        }
        BeautyLeafDomain::validate_keys(keys,locks.face_count);
        const std::array<const char*,4> names{"selected","protected","foreground","domain"};
        const std::array<std::vector<uint8_t>*,4> masks{&result.selection.selected,&result.selection.protected_faces,&result.selection.foreground,&result.selection.domain};
        for (size_t m=0;m<names.size();++m) {
            masks[m]->assign(editing.keys.size(),0);
            for (const auto& key : BeautyLeafDomain::decode_keys(value.at(names[m]),locks.face_count)) (*masks[m])[editing.index(key)]=1;
        }
        result.validate(editing,locks);
        return result;
    }
};

inline void compose_leaf_colors(SemanticColoring::FaceColors& faces, SemanticColoring::SubfaceColors& children,
    const std::map<BeautyLeafKey, SemanticColoring::Color>& colors) {
    children.erase(std::remove_if(children.begin(),children.end(),[&](const auto& child) {
        BeautyLeafKey key{child.face_id,child.path.depth,child.path.value};
        for (uint8_t depth=0;depth<=key.depth;++depth)
            if (colors.count({key.source_face_id,depth,uint8_t(key.path>>(2*(key.depth-depth)))})) return true;
        return false;
    }),children.end());
    std::map<size_t,SemanticColoring::Color> roots(faces.begin(),faces.end());
    for (const auto& entry : colors) {
        if (!entry.first.depth) roots[entry.first.source_face_id]=entry.second;
        else children.push_back({entry.first.source_face_id,{entry.first.depth,entry.first.path},entry.second,1.f});
    }
    faces.assign(roots.begin(),roots.end());
    std::sort(children.begin(),children.end(),[](const auto& a,const auto& b){return std::tie(a.face_id,a.path)<std::tie(b.face_id,b.path);});
}

inline void appearance_subface_colors(BeautyAppearanceOptions& options,const SemanticColoring::SubfaceColors& children,
    const std::string& geometry) {
    const size_t count=options.face_weights.size();
    if (options.face_target_colors.size()!=count) throw std::invalid_argument("Missing appearance root colors.");
    std::map<size_t,std::map<BeautyLeafKey,SemanticColoring::Color>> colors;
    for (const auto& item : children) {
        const BeautyLeafKey key{item.face_id,item.path.depth,item.path.value};
        if (!key.valid(count) || !key.depth || !colors[key.source_face_id].emplace(key,item.color).second)
            throw std::invalid_argument("Invalid appearance child colors.");
    }
    options.leaves.clear(); options.canonical_geometry_id=geometry;
    for (const auto& root : colors) {
        std::set<BeautyLeafKey> branches;
        for (const auto& color : root.second) for (uint8_t d=0;d<color.first.depth;++d)
            branches.insert({root.first,d,uint8_t(color.first.path>>(2*(color.first.depth-d)))});
        const float inherited_weight=options.face_weights[root.first];
        const auto visit=[&](const auto& self,BeautyLeafKey key)->void {
            if (branches.count(key)) {
                for (uint8_t c=0;c<4;++c) self(self,{key.source_face_id,uint8_t(key.depth+1),uint8_t((key.path<<2)|c)});
                return;
            }
            auto rgb=options.face_target_colors[root.first]; float weight=inherited_weight;
            for (uint8_t d=key.depth;d>0;--d) {
                const auto found=root.second.find({root.first,d,uint8_t(key.path>>(2*(key.depth-d)))});
                if (found!=root.second.end()) { rgb=found->second; weight=1.f; break; }
            }
            options.leaves.push_back({key,weight,rgb});
        };
        visit(visit,{root.first,0,0}); options.face_weights[root.first]=0;
    }
    std::sort(options.leaves.begin(),options.leaves.end(),[](const auto& a,const auto& b){return a.key<b.key;});
}

inline void preserve_locked_leaf_colors(const ShapeLockSet& locks,const SemanticColoring::FaceColors& before,
    const SemanticColoring::SubfaceColors& children_before,SemanticColoring::FaceColors& after,
    SemanticColoring::SubfaceColors& children_after) {
    if (!locks.leaf_domain) return;
    const std::map<size_t,SemanticColoring::Color> roots(before.begin(),before.end());
    std::map<BeautyLeafKey,SemanticColoring::Color> children,protected_colors;
    std::set<BeautyLeafKey> protected_keys,source_only_keys;
    for (const auto& child:children_before) children[{child.face_id,child.path.depth,child.path.value}]=child.color;
    for (const auto& lock:locks.locks) for (const auto& key:lock.locked_leaves) {
        protected_keys.insert(key);
        bool found=false;
        for (uint8_t d=key.depth;d>0;--d) {
            const auto child=children.find({key.source_face_id,d,uint8_t(key.path>>(2*(key.depth-d)))});
            if (child!=children.end()) { protected_colors[key]=child->second; found=true; break; }
        }
        if (!found) {
            const auto root=roots.find(key.source_face_id);
            if (root!=roots.end()) protected_colors[key]=root->second;
            else {
                // Without an automatic underlayer, retain the source appearance.
                source_only_keys.insert(key);
                after.erase(std::remove_if(after.begin(),after.end(),[&](const auto& item){return item.first==key.source_face_id;}),after.end());
            }
        }
    }
    children_after.erase(std::remove_if(children_after.begin(),children_after.end(),[&](const auto& child) {
        const BeautyLeafKey key{child.face_id,child.path.depth,child.path.value};
        for (const auto& locked:protected_keys) if (locked.contains(key)) return true;
        for (const auto& locked:source_only_keys) if (key.contains(locked)) return true;
        return false;
    }),children_after.end());
    compose_leaf_colors(after,children_after,protected_colors);
    for (const auto& child:children_before) {
        const BeautyLeafKey key{child.face_id,child.path.depth,child.path.value}; bool restore=false;
        for (uint8_t d=0;d<key.depth;++d)
            if (protected_keys.count({key.source_face_id,d,uint8_t(key.path>>(2*(key.depth-d)))})) { restore=true; break; }
        if (restore) children_after.push_back(child);
    }
    std::sort(children_after.begin(),children_after.end(),[](const auto& a,const auto& b){return std::tie(a.face_id,a.path)<std::tie(b.face_id,b.path);});
}
} // namespace Slic3r::AI
