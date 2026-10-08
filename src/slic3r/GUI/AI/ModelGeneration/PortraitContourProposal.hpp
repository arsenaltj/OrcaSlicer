#pragma once

#include "slic3r/GUI/AI/Model/BeautySurfaceShapeLock.hpp"
#include "slic3r/GUI/AI/Model/SurfaceSelectionState.hpp"
#include <tuple>

namespace Slic3r::GUI {
inline nlohmann::json build_verified_contour_partition(nlohmann::json request,
    const LocalSemanticEvidence::Evidence& evidence,const indexed_triangle_set& mesh,const std::string& evidence_hash) {
    namespace SP=AI::SurfacePartition;
    const auto& identity=request.at("identity");
    SP::require(identity.at("source_sha256")==evidence.identity.source_sha256 && identity.at("geometry_id")==evidence.identity.geometry_id &&
        identity.at("face_count")==mesh.indices.size() && identity.at("evidence_sha256")==evidence_hash &&
        identity.at("runtime_sha256")==evidence.identity.runtime_sha256 && identity.at("policy_sha256")==evidence.identity.policy_sha256 &&
        AI::SurfaceSelectionPersistence::geometry_fingerprint(mesh)==evidence.identity.geometry_id,"Contour source identity changed.");
    using Owner=std::pair<std::string,std::string>;
    std::map<size_t,Owner> parents,seeds;
    std::map<Owner,std::set<size_t>> accepted,rejected,nested;
    for(const auto& region:evidence.regions) for(auto face:region.faces)
        SP::require(parents.emplace(face,Owner{region.subject_id,region.label}).second,"Contour parent overlap.");
    for(const auto& shape:evidence.shape_details) {
        const Owner owner{shape.subject_id,shape.label};
        rejected[owner].insert(shape.rejected_faces.begin(),shape.rejected_faces.end());
        if(!AI::ShapeLockSet::lockable_shape(shape)) continue;
        accepted[owner].insert(shape.accepted_faces.begin(),shape.accepted_faces.end());
        nested[owner].insert(shape.nested_faces.begin(),shape.nested_faces.end());
        for(auto face:shape.accepted_faces) SP::require(seeds.emplace(face,owner).second,"Contour seed ownership overlap.");
    }
    SP::require(request.at("faces").is_array() && request.at("faces").size()<=mesh.indices.size(),"Contour root count changed.");
    std::map<std::tuple<float,float,float>,size_t> welded;
    std::map<std::pair<size_t,size_t>,std::vector<size_t>> edges;
    std::set<size_t> roots;
    for(auto& row:request["faces"]) {
        const size_t face=LocalSemanticEvidence::detail::index(row.at("source_face_id"),mesh.indices.size()-1);
        SP::require(roots.insert(face).second,"Duplicate contour root.");
        std::array<size_t,3> vertices;
        for(size_t c=0;c<3;++c) {
            const auto& point=mesh.vertices.at(mesh.indices[face][c]);
            vertices[c]=welded.emplace(std::make_tuple(point[0],point[1],point[2]),welded.size()).first->second;
        }
        // Edge identities are reconstructed from the proved native mesh.
        row["source_vertices"]=vertices;
        for(size_t c=0;c<3;++c) edges[std::minmax(vertices[c],vertices[(c+1)%3])].push_back(face);
    }
    std::map<size_t,std::set<size_t>> neighbors;
    for(const auto& edge:edges) if(edge.second.size()==2) {
        neighbors[edge.second[0]].insert(edge.second[1]);neighbors[edge.second[1]].insert(edge.second[0]);
    }
    std::map<Owner,std::set<size_t>> scopes;
    for(const auto& entry:accepted) {
        const auto& owner=entry.first;auto& scope=scopes[owner];scope=entry.second;
        const size_t rings=owner.second=="lb" || owner.second=="rb" ? 2 : 1;
        const auto legal=[&](size_t face) {
            if(rejected[owner].count(face)) return false;
            const auto seed=seeds.find(face);
            if(seed!=seeds.end()) return seed->second==owner;
            const auto parent=parents.find(face);
            return parent!=parents.end() && parent->second.first==owner.first &&
                (parent->second.second=="face" || parent->second.second=="nose" || parent->second.second==owner.second);
        };
        for(size_t ring=0;ring<rings;++ring) {
            auto expanded=scope;
            for(auto face:scope) for(auto neighbor:neighbors[face]) if(legal(neighbor)) expanded.insert(neighbor);
            scope=std::move(expanded);
        }
    }
    for(const auto& row:request.at("faces")) {
        const size_t face=row.at("source_face_id");
        for(const auto& base:row.at("base")) {
            auto label=base.at("label").get<std::string>();
            const bool iris=label.compare(0,5,"iris-")==0;
            if(iris) label=label.substr(5);
            if(AI::ShapeLockSet::supported_label(label)) {
                const Owner owner{base.at("subject_id").get<std::string>(),label};
                SP::require(accepted[owner].count(face) && (!iris || nested[owner].count(face)),
                    "Contour fallback grants an unproved detail or nested iris.");
            } else if(label!="R6") {
                const auto parent=parents.find(face);
                SP::require(parent!=parents.end() && parent->second.first==base.at("subject_id") &&
                    (parent->second.second==label || (label=="face" && seeds.count(face))),
                    "Contour fallback leaves its proved source parent.");
            }
        }
        for(const auto& layer:row.at("layers")) {
        auto label=layer.at("label").get<std::string>();
        if(label.compare(0,5,"iris-")==0) label=label.substr(5);
        if(label.compare(0,11,"periocular-")==0) label=label.substr(11);
        const Owner owner{layer.at("subject_id").get<std::string>(),label};
        SP::require(scopes.count(owner) && scopes.at(owner).count(row.at("source_face_id").get<size_t>()),
                    "Contour proposal leaves its proved parent or bounded topology scope.");
        }
    }
    return SP::build(request);
}
} // namespace Slic3r::GUI
