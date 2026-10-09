#pragma once

#include "PortraitShapeDetails.hpp"
#include "PortraitParentCleanup.hpp"
#include "slic3r/GUI/AI/Model/ParentSurfaceVisibility.hpp"

namespace Slic3r::GUI::PortraitParentCoverage {
using Json=nlohmann::json;
namespace SP=AI::SurfacePartition;
namespace SC=AI::SemanticColoring;

inline bool editable(const Json& cell) {
    const auto label=cell.at("label").get<std::string>();
    return label=="face" || label=="R6" || label=="skin" || label=="cloth";
}
inline bool contains(const Json& cell,const Vec2d& point) {
    if(!SP::sample_in_contour(cell.at("polygon"),point))return false;
    for(const auto& hole:cell.value("holes",Json::array()))if(SP::sample_in_contour(hole,point))return false;
    return true;
}

inline void validate(const Json& proposal,const PortraitShapeDetails& details) {
    SP::require(proposal.at("schema")=="orca.portrait-parent-proposal/v1","Invalid parent proposal.");
    details.validate_parent_identity();
    SP::require(!details.parent_evidence_identity.empty(),"Parent proposal has no current evidence binding.");
    for(const auto* key:{"geometry_id","source_sha256","face_count","evidence_sha256","runtime_sha256","policy_sha256"})
        SP::require(proposal.at("identity").at(key)==details.parent_evidence_identity.at(key),"Parent proposal identity drift.");
    SP::require(AI::beauty_leaf_digest(proposal.at("policy").dump())==proposal.at("proposal_policy_sha256"),"Parent proposal policy drift.");
    SP::require(proposal.at("policy").at("root_coverage").get<double>()==1.,"Partial coverage cannot grant whole-root authority.");
    const auto coverage_tolerance=proposal.at("policy").at("coverage_tolerance").get<double>();
    SP::require(std::isfinite(coverage_tolerance) && coverage_tolerance>=0 && coverage_tolerance<=1e-10,
        "Invalid whole-root coverage tolerance.");
    const auto subject=proposal.at("subject_id").get<std::string>();
    SP::require(std::find(details.subjects.begin(),details.subjects.end(),subject)!=details.subjects.end(),"Parent proposal subject drift.");
    const auto count=details.locks.face_count;
    SP::require(proposal.at("cameras").size()<=16 && proposal.at("roots").size()<=count &&
        proposal.at("mixed").size()<=count,"Parent proposal exceeds its budget.");
    SP::require(!proposal.at("cameras").empty(),"No parent cameras.");
    std::set<size_t> faces;
    for(const auto& row:proposal.at("roots")) {
        SP::require(row.is_array() && (row.size()==4 || row.size()==5),"Invalid parent root observation.");
        const auto face=LocalSemanticEvidence::detail::index(row[0],count-1);
        SP::require(faces.insert(face).second && (row[1]==3 || row[1]==4),"Conflicting parent root.");
        std::set<std::string> families;std::set<size_t> views;
        for(const auto& view:row[2]) {
            const auto index=LocalSemanticEvidence::detail::index(view,proposal.at("cameras").size()-1);
            SP::require(views.insert(index).second,"Duplicate parent camera.");
            families.insert(proposal.at("cameras")[index].at("family").get<std::string>());
        }
        SP::require(families.size()>=2,"Parent root has correlated or insufficient support.");
        SP::require(row[3].is_array() && row[3].size()==3,"Missing source parent RGB.");
        for(const auto& channel:row[3]) LocalSemanticEvidence::detail::probability(channel);
        if(row.size()==5) {
            std::set<std::string> vote_families;
            for(const auto& vote:row[4]) {
                SP::require(vote.is_array() && vote.size()==2 && (vote[1]==1 || vote[1]==3 || vote[1]==4),"Invalid projected parent vote.");
                const auto camera=LocalSemanticEvidence::detail::index(vote[0],proposal.at("cameras").size()-1);
                SP::require(vote_families.insert(proposal.at("cameras")[camera].at("family").get<std::string>()).second,"Correlated parent vote.");
            }
        }
    }
    for(const auto& row:proposal.at("mixed")) {
        const auto face=LocalSemanticEvidence::detail::index(row.at("source_face_id"),count-1);
        SP::require(faces.insert(face).second && row.at("layers").size()<=2,"Conflicting mixed parent root.");
        SP::require(row.at("source_samples").size()==7,"Mixed parent source samples changed.");
        for(const auto& rgb:row.at("source_samples")) {
            SP::require(rgb.is_array() && rgb.size()==3,"Invalid mixed parent source RGB.");
            for(const auto& c:rgb)LocalSemanticEvidence::detail::probability(c);
        }
        std::set<std::string> labels;
        for(const auto& layer:row.at("layers")) {
            const auto label=layer.at("label").get<std::string>();
            SP::require((label=="skin" || label=="cloth") && labels.insert(label).second &&
                layer.at("subject_id")==subject && layer.at("parent_label")==label,"Cross-parent or cross-subject mixed proposal.");
            SP::require(layer.at("views").is_array() && layer.at("views").size()<=16,"Mixed camera budget exceeded.");
            std::set<std::string> families;
            for(const auto& v:layer.at("views")) {
                const auto camera=LocalSemanticEvidence::detail::index(v.at("camera"),proposal.at("cameras").size()-1);
                SP::require(v.at("family")==proposal.at("cameras")[camera].at("family"),"Parent camera family drift.");
                SP::require(families.insert(v.at("family").get<std::string>()).second,"Correlated mixed parent crop.");
            }
            SP::require(families.size()>=2,"Mixed proposal has insufficient independent support.");
        }
        SP::require(row.at("color_samples").size()==proposal.at("color_barycentric").size(),"Mixed source sampling drift.");
        for(const auto& rgb:row.at("color_samples")) {
            SP::require(rgb.is_array() && rgb.size()==3,"Invalid mixed source color.");
            for(const auto& c:rgb)LocalSemanticEvidence::detail::probability(c);
        }
    }
    SP::require(proposal.at("color_barycentric").size()<=512,"Parent source sampler exceeds budget.");
    for(const auto& b:proposal.at("color_barycentric")) {
        const auto weights=b.get<std::array<double,3>>();double total=0;
        for(double value:weights){SP::require(std::isfinite(value) && value>=0 && value<=1,"Invalid source sample barycentric.");total+=value;}
        SP::require(std::abs(total-1)<1e-7,"Source sample leaves its triangle.");
    }
}

inline void apply(PortraitShapeDetails& details,const SC::MeshSnapshot& source,const SC::Analysis& analysis,
    const SC::FaceColors& manual,const std::map<std::string,SC::Color>& manual_cells,const SC::Cancel& cancelled) {
    if(!details.parent_proposal || !details.surface_partition || !details.contour_locks)return;
    const auto& proposal=*details.parent_proposal;
    validate(proposal,details);
    SP::require(source.geometry_id==details.locks.geometry_id && source.mesh.indices.size()==details.locks.face_count &&
        analysis.geometry_id==source.geometry_id && analysis.face_labels.size()==source.mesh.indices.size() &&
        analysis.face_confidence.size()==source.mesh.indices.size(),"Native parent source geometry drift.");
    const auto stop=[&]{if(cancelled && cancelled())throw std::invalid_argument("parent_coverage_cancelled");};
    const auto subject=proposal.at("subject_id").get<std::string>();
    const auto baseline=*details.surface_partition;
    std::set<size_t> reserved(details.reserved_faces.begin(),details.reserved_faces.end()),manual_roots;
    for(const auto& row:manual)manual_roots.insert(row.first);
    Json frozen=Json::array();std::set<std::string> protected_cells;
    std::set<size_t> frozen_roots;
    std::map<size_t,Json> originals;
    for(const auto& lock:details.contour_locks->document.at("locks"))
        for(const auto* field:{"locked_cells","nested_cells","periocular_cells"})
            for(const auto& id:lock.value(field,Json::array()))protected_cells.insert(id.get<std::string>());
    for(const auto& face:baseline.at("faces")) {
        const size_t root=face.at("source_face_id");originals[root]=face;
        for(const auto& cell:face.at("cells"))if(!editable(cell) || protected_cells.count(cell.at("id")) || manual_cells.count(cell.at("id"))) {
            protected_cells.insert(cell.at("id").get<std::string>());
            frozen_roots.insert(root);
        }
    }
    for(const auto& id:protected_cells)frozen.push_back(id);
    AI::ParentSurfaceVisibility visibility(source.mesh);
    const auto direction=[&](size_t view){
        const auto& camera=proposal.at("cameras").at(view);
        const auto a=camera.at("direction").get<std::array<double,3>>();
        const Vec3d d(a[0],a[1],a[2]);const double distance=camera.at("distance");
        SP::require(d.allFinite() && std::abs(d.norm()-1)<1e-5 && std::isfinite(distance) && distance>0,"Invalid parent camera.");
        return std::make_pair(d,distance);
    };
    const auto visible_views=[&](size_t face,const Json& views,const std::array<std::array<double,3>,7>& samples=AI::parent_visibility_samples) {
        std::set<std::string> families;std::vector<Vec3d> directions;
        for(const auto& v:views) {
            stop();const size_t index=v.is_object() ? v.at("camera").get<size_t>() : v.get<size_t>();
            const auto camera=direction(index);
            if(AI::parent_visible_samples(visibility,face,camera.first,camera.second,samples)==127 &&
                std::all_of(directions.begin(),directions.end(),[&](const auto& d){return d.dot(camera.first)<.996194698;})) {
                if(families.insert(proposal.at("cameras").at(index).at("family").get<std::string>()).second)directions.push_back(camera.first);
            }
        }
        return families.size();
    };
    std::map<std::string,size_t> retained;size_t roots=0;
    details.parent_verified_roots.clear();details.parent_source_colors.clear();
    for(const auto& row:proposal.at("roots")) {
        stop();const size_t face=row[0];std::string parent=row[1]==3 ? "skin" : "cloth";
        if(manual_roots.count(face) || manual_cells.count("source:"+std::to_string(face))){++retained["MANUAL_ROOT_PRESERVED"];continue;}
        if(!originals.count(face) && reserved.count(face)){++retained["DETAIL_SOURCE_ROOT_PRESERVED"];continue;}
        if(row.size()==5) {
            std::map<size_t,Json> votes;
            for(const auto& vote:row[4])votes[vote[1].get<size_t>()].push_back(vote[0]);
            std::set<size_t> visible_labels;size_t selected=0;
            for(const auto& [label,views]:votes) {
                const auto amount=visible_views(face,views);
                if(amount)visible_labels.insert(label);
                if(amount>=2)selected=label;
            }
            if(visible_labels.size()>1){++retained["NATIVE_VISIBLE_PARENT_CONFLICT"];continue;}
            if(selected!=3 && selected!=4){++retained["NATIVE_INDEPENDENT_VISIBILITY_INSUFFICIENT"];continue;}
            parent=selected==3 ? "skin" : "cloth";
        } else if(visible_views(face,row[2])<2){++retained["NATIVE_INDEPENDENT_VISIBILITY_INSUFFICIENT"];continue;}
        const auto label=analysis.face_labels.at(face);
        if(!originals.count(face) && analysis.face_confidence.at(face)>=SC::minimum_confidence &&
           label!=SC::Label::Unknown && label!=SC::Label::Background &&
           label!=(parent=="skin" ? SC::Label::FaceSkin : SC::Label::Clothes) &&
           !(parent=="skin" && label==SC::Label::BodySkin)) {++retained["NATIVE_PARENT_CONFLICT"];continue;}
        details.parent_verified_roots[face]=parent;
        details.parent_source_colors["source:"+std::to_string(face)]=row[3].get<SC::Color>();++roots;
    }
    // Mixed roots are cut only inside editable siblings. Saved detail geometry
    // and manual cells cannot be changed even by source-consistent proposals.
    std::map<size_t,Json> changes;std::map<size_t,Json> mixed_samples;
    for(const auto& row:proposal.at("mixed")) {
        stop();const size_t face=row.at("source_face_id");
        if(manual_roots.count(face) || manual_cells.count("source:"+std::to_string(face)) || (!originals.count(face) && reserved.count(face))) {++retained["MIXED_PROTECTED_ROOT"];continue;}
        Json base=originals.count(face) ? originals.at(face).at("cells") : Json::array({{{"polygon",{{1.,0.,0.},{0.,1.,0.},{0.,0.,1.}}},
            {"holes",Json::array()},{"label","R6"},{"parent_label","face"},{"subject_id",subject}}});
        Json allowed=Json::array();
        for(const auto& cell:base)if(editable(cell) && !protected_cells.count(cell.value("id",std::string())) &&
            !manual_cells.count(cell.value("id",std::string())) && cell.at("subject_id")==subject)allowed.push_back(cell);
        if(allowed.empty()){++retained["NO_EDITABLE_MIXED_SIBLING"];continue;}
        Json layers=Json::array();
        for(auto layer:row.at("layers")) {
            ExPolygons mask;
            try {mask=intersection_ex(SP::majority(layer.at("views"),proposal.at("curve_library")),SP::polygons(allowed,true));}
            catch(const std::exception&){++retained["MIXED_LOCAL_CURVE_NUMERICAL_FALLBACK"];continue;}
            if(mask.empty())continue;
            // Native ray proof samples the proposed portion, rather than all
            // of a mixed source root or a neighbouring raster pixel.
            Json witnessed=Json::array();
            for(const auto& component:mask) {
                bool supported=true;
                for(const auto& triangle:SP::triangles(component)) {
                    std::array<std::array<double,3>,7> samples{};
                    for(size_t i=0;i<7;++i)for(size_t k=0;k<3;++k)
                        for(size_t corner=0;corner<3;++corner)samples[i][k]+=AI::parent_visibility_samples[i][corner]*triangle[corner][k].get<double>();
                    Json witnesses=Json::array();
                    for(const auto& v:layer.at("views")) {
                        const auto observed=SP::view_polygons(v,proposal.at("curve_library"));
                        const auto portion=SP::polygons(Json::array({{{"polygon",triangle},{"holes",Json::array()}}}),true);
                        if(SP::area(diff_ex(portion,observed))<SP::area_tolerance)witnesses.push_back(v);
                    }
                    if(visible_views(face,witnesses,samples)<2){supported=false;break;}
                }
                if(supported)witnessed.push_back(SP::encode_polygon(component));
                else ++retained["MIXED_COMPONENT_VISIBILITY_INSUFFICIENT"];
            }
            if(witnessed.empty())continue;
            layer["parent_polygons"]=witnessed;layers.push_back(std::move(layer));
        }
        if(layers.empty()){++retained["MIXED_NO_TWO_VISIBLE_WITNESSES"];continue;}
        changes[face]={{"source_face_id",face},{"base",base},{"baseline_triangle_count",originals.count(face) ? originals.at(face).at("triangle_count") : Json(1)},
            {"layers",layers},{"parent_repair",true},{"preserve_color_provenance",true}};
        mixed_samples[face]=row.at("color_samples");
    }
    if(!changes.empty()) {
        // Reconstruct source seams from the current native mesh, including
        // terminal neighbours, without granting them any painting authority.
        std::map<std::array<float,3>,size_t>weld;std::vector<size_t> vertex_ids(source.mesh.vertices.size());
        for(size_t i=0;i<vertex_ids.size();++i){const auto&p=source.mesh.vertices[i];vertex_ids[i]=weld.emplace(std::array<float,3>{p.x(),p.y(),p.z()},weld.size()).first->second;}
        std::map<std::pair<size_t,size_t>,std::vector<size_t>>edges;
        for(size_t f=0;f<source.mesh.indices.size();++f)for(size_t k=0;k<3;++k)
            edges[std::minmax(vertex_ids[source.mesh.indices[f][k]],vertex_ids[source.mesh.indices[f][(k+1)%3]])].push_back(f);
        std::set<size_t> included;for(const auto&row:originals)included.insert(row.first);
        for(const auto&row:changes) {
            included.insert(row.first);
            for(size_t k=0;k<3;++k)for(auto n:edges[std::minmax(vertex_ids[source.mesh.indices[row.first][k]],vertex_ids[source.mesh.indices[row.first][(k+1)%3]])])included.insert(n);
        }
        Json requests=Json::array();
        for(auto face:included) {
            auto row=changes.count(face) ? changes.at(face) : originals.count(face) ? Json{{"source_face_id",face},{"base",originals.at(face).at("cells")},
                {"baseline_triangle_count",originals.at(face).at("triangle_count")},{"layers",Json::array()}} : Json{{"source_face_id",face},
                {"base",Json::array({{{"polygon",{{1.,0.,0.},{0.,1.,0.},{0.,0.,1.}}},{"holes",Json::array()},
                    {"label","R6"},{"parent_label","face"},{"subject_id",subject}}})},{"baseline_triangle_count",1},{"layers",Json::array()}};
            row["source_vertices"]=Json::array();for(size_t k=0;k<3;++k)row["source_vertices"].push_back(vertex_ids[source.mesh.indices[face][k]]);
            requests.push_back(std::move(row));
        }
        auto identity=AI::BeautySurfaceShapeLock::identity(baseline);
        identity["boundary_policy_sha256"]=AI::beauty_leaf_digest(Json{{"approved_boundary_policy",baseline.at("boundary_policy_sha256")},
            {"parent_proposal_policy",proposal.at("proposal_policy_sha256")}}.dump());
        Json request={{"schema","orca.surface-partition-request/v1"},{"repair_scope","parent"},{"identity",identity},
            {"incremental_baseline",baseline},{"baseline_partition_sha256",baseline.at("partition_sha256")},
            {"existing_added_triangles",baseline.at("added_triangles")},{"triangle_budget",baseline.at("triangle_budget")},
            {"frozen_cell_ids",frozen},{"faces",requests},{"curve_library",proposal.at("curve_library")}};
        try {
            auto partition=SP::build(request);
            auto locks=details.contour_locks->document;
            const auto partition_identity=AI::BeautySurfaceShapeLock::identity(partition);
            for(const auto&field:partition_identity.items())locks[field.key()]=field.value();
            const auto hash=AI::beauty_leaf_digest(partition.dump());
            locks["partition_ref"]={{"schema","orca.surface-partition-reference/v1"},{"path","surface-partitions/"+hash+".json"},{"sha256",hash}};
            details.contour_locks=std::make_shared<AI::BeautySurfaceShapeLock>(AI::BeautySurfaceShapeLock::decode(locks,partition,AI::BeautySurfaceShapeLock::identity(partition),hash));
            // Preserve each untouched or split sibling's actual saved color.
            auto old_colors=details.cell_colors;details.cell_colors.clear();
            details.confirmed_parent_roots.clear();details.confirmed_parent_cell_labels.clear();
            for(const auto& face:partition.at("faces"))for(const auto& cell:face.at("cells")) {
                const auto id=cell.at("id").get<std::string>(),origin=cell.value("origin_cell_id",id);
                if(old_colors.count(origin))details.cell_colors[id]=old_colors.at(origin);
                if(mixed_samples.count(face.at("source_face_id"))) {
                    const auto samples=mixed_samples.at(face.at("source_face_id"));SC::Color rgb{};size_t n=0;
                    for(size_t i=0;i<samples.size();++i)if(contains(cell,Vec2d(proposal.at("color_barycentric")[i][1].get<double>(),proposal.at("color_barycentric")[i][2].get<double>()))) {
                        for(size_t k=0;k<3;++k)rgb[k]+=samples[i][k].get<float>();++n;
                    }
                    if(n){for(auto&c:rgb)c/=float(n);details.parent_source_colors[id]=rgb;}
                }
            }
            details.surface_partition=std::make_shared<Json>(std::move(partition));
        } catch(const std::exception& error) {retained["MIXED_LOCAL_PARTITION_FALLBACK"]=changes.size();details.parent_coverage_audit["partition_diagnostic"]=error.what();}
    }
    size_t mixed_applied=0,mixed_cells=0,mixed_retained=0;
    Json mixed_fallbacks=Json::object();
    for(const auto& face:details.surface_partition->at("faces"))if(changes.count(face.at("source_face_id"))) {
        if(face.at("status")=="R9_CONTOUR_CLIPPED") {
            ++mixed_applied;
            for(const auto& cell:face.at("cells"))if(cell.at("label")=="skin" || cell.at("label")=="cloth")++mixed_cells;
        } else {
            ++mixed_retained;
            const auto status=face.at("status").get<std::string>();
            mixed_fallbacks[status]=mixed_fallbacks.value(status,size_t(0))+1;
        }
    }
    details.parent_coverage_audit.update({{"verified_root_proposals",roots},{"mixed_requested_roots",changes.size()},
        {"mixed_applied_roots",mixed_applied},{"mixed_parent_cells",mixed_cells},{"mixed_retained_roots",mixed_retained},
        {"mixed_fallback_statuses",mixed_fallbacks},
        {"baseline_added_triangles",baseline.at("added_triangles")},{"final_added_triangles",details.surface_partition->at("added_triangles")},
        {"retained_reasons",retained},{"scope_source_faces",proposal.at("scope_faces").size()},{"proposal_audit",proposal.at("audit")},
        {"frozen_cells_preserved",SP::preserves_frozen_cells(baseline,*details.surface_partition,frozen)}});
}
} // namespace Slic3r::GUI::PortraitParentCoverage
