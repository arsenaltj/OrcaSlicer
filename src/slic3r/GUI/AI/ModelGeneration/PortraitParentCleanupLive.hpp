#pragma once

#include "PortraitParentCleanup.hpp"
#include "PortraitShapeDetails.hpp"
#include "PortraitColorPlan.hpp"
#include <numeric>
#include <optional>

namespace Slic3r::GUI::PortraitParentCleanup {
namespace Live {
namespace SC = AI::SemanticColoring;
namespace SP = AI::SurfacePartition;

struct Unit {
    std::string id, subject, parent;
    size_t face = 0;
    Json polygon;
    Color source {}, lab {};
    std::optional<Color> previous;
    std::string inheritance;
    bool implicit = false, manual = false;
    std::set<size_t> neighbors;
    std::string target, rule;
};

inline std::string parent(const std::string& label) {
    if (label == "face" || label == "nose" || label == "lr" || label == "rr" ||
        label == "neck" || label == "body-skin" || label == "skin") return "skin";
    return label == "cloth" ? "cloth" : label == "hair" ? "hair" : std::string();
}
inline bool conflict(SC::Label label, const std::string& owner) {
    if (label == SC::Label::Unknown || label == SC::Label::Background) return false;
    return owner == "skin" ? label != SC::Label::FaceSkin && label != SC::Label::BodySkin :
        label != SC::Label::Clothes;
}
inline bool continuous(const Color& a, const Color& b, const std::string& owner) {
    const double ax = a[1]/std::max(a[0], .1f), ay = a[2]/std::max(a[0], .1f);
    const double bx = b[1]/std::max(b[0], .1f), by = b[2]/std::max(b[0], .1f);
    return std::hypot(ax-bx, ay-by) <= (owner == "skin" ? .07 : .02) &&
        (owner == "skin" || std::abs(a[0]-b[0]) <= .12);
}
inline double chroma(const Color& lab) { return std::hypot(lab[1],lab[2]); }

// Positive-length source edges and cell edges, including holes. A shared root
// or vertex alone cannot connect materials or grant cleanup support.
inline void connect(std::vector<Unit>& units, const indexed_triangle_set& mesh) {
    std::map<std::array<float,3>,size_t> weld;
    std::vector<size_t> vertices(mesh.vertices.size());
    for (size_t i=0;i<mesh.vertices.size();++i) {
        const auto& p=mesh.vertices[i];
        vertices[i]=weld.emplace(std::array<float,3>{p.x(),p.y(),p.z()},weld.size()).first->second;
    }
    using Line=std::array<int64_t,5>;
    struct Edge { int64_t start,end;size_t unit; };
    std::map<Line,std::vector<Edge>> edges;
    constexpr int64_t scale=1000000000;
    for (size_t i=0;i<units.size();++i) {
        const auto& u=units[i];
        std::vector<Json> rings{u.polygon.at("polygon")};
        for (const auto& hole:u.polygon.at("holes")) rings.push_back(hole);
        for (const auto& ring:rings) for (size_t point=0;point<ring.size();++point) {
            std::array<int64_t,3> a{},b{};
            for (size_t k=0;k<3;++k) {
                a[k]=int64_t(std::floor(ring[point][k].get<double>()*scale+.5));
                b[k]=int64_t(std::floor(ring[(point+1)%ring.size()][k].get<double>()*scale+.5));
            }
            Line line{};int64_t lo=0,hi=0;bool seam=false;
            for (size_t k=0;k<3;++k) if (a[k]==0 && b[k]==0) {
                const size_t j=(k+1)%3,l=(k+2)%3;
                const auto vj=vertices[mesh.indices[u.face][j]],vl=vertices[mesh.indices[u.face][l]];
                line={0,int64_t(std::min(vj,vl)),int64_t(std::max(vj,vl)),0,0};
                lo=vj<vl ? a[l] : a[j];hi=vj<vl ? b[l] : b[j];seam=true;break;
            }
            if (!seam) {
                int64_t dx=b[1]-a[1],dy=b[2]-a[2];
                const auto divisor=std::gcd(std::abs(dx),std::abs(dy));
                if (!divisor) continue;
                dx/=divisor;dy/=divisor;
                if (dx<0 || (dx==0 && dy<0)) {dx=-dx;dy=-dy;}
                line={1,int64_t(u.face),dx,dy,dx*a[2]-dy*a[1]};
                lo=dx*a[1]+dy*a[2];hi=dx*b[1]+dy*b[2];
            }
            if (lo!=hi) edges[line].push_back({std::min(lo,hi),std::max(lo,hi),i});
        }
    }
    for (auto& entry:edges) {
        auto& segments=entry.second;
        std::sort(segments.begin(),segments.end(),[](const Edge& a,const Edge& b){return a.start<b.start;});
        std::vector<Edge> active;
        for (const auto& edge:segments) {
            active.erase(std::remove_if(active.begin(),active.end(),[&](const Edge& a){return a.end<=edge.start;}),active.end());
            for (const auto& previous:active) if (edge.unit!=previous.unit &&
                units[edge.unit].subject==units[previous.unit].subject && units[edge.unit].parent==units[previous.unit].parent) {
                units[edge.unit].neighbors.insert(previous.unit);units[previous.unit].neighbors.insert(edge.unit);
            }
            active.push_back(edge);
        }
    }
}

// This is the installed, evidence-driven counterpart of material_targets() and
// supported_cleanup() in portrait_parent_cleanup.py. No reviewed model catalog
// is needed. It changes colors and parent selection metadata, never polygons.
inline Result colors(const PortraitShapeDetails& details, const SC::MeshSnapshot& source,
    const SC::Analysis& analysis, const std::vector<std::string>& roles, const std::vector<Color>& palette,
    const SC::FaceColors& inherited, const SC::FaceColors& manual,
    const std::map<std::string,Color>& manual_cells, const SC::Cancel& cancelled={}) {
    Result result;
    try {
        if (!details.compatible(source.geometry_id,source.mesh.indices.size()) ||
            analysis.geometry_id!=source.geometry_id || analysis.face_labels.size()!=source.mesh.indices.size() ||
            analysis.face_confidence.size()!=source.mesh.indices.size())
            throw std::invalid_argument("live_parent_source_identity_drift");
        if (!details.surface_partition || !details.contour_locks) {
            result.reason="live_parent_contour_unavailable";return result;
        }
        if (roles.empty() || roles.size()!=palette.size() || !valid_portrait_roles(roles,palette.size()))
            throw std::invalid_argument("palette_roles_unavailable");
        std::map<std::string,Color> slots;
        for (size_t i=0;i<roles.size();++i) {
            for (auto c:palette[i]) if (!std::isfinite(c) || c<0 || c>1) throw std::invalid_argument("invalid_parent_palette");
            if (!roles[i].empty()) slots[roles[i]]=palette[i];
        }
        const auto stop=[&]{if(cancelled && cancelled()) throw std::invalid_argument("parent_cleanup_cancelled");};
        const auto& partition=*details.surface_partition;
        details.validate_parent_identity();
        if(partition.at("evidence_sha256")!=details.locks.evidence_sha256 ||
            partition.at("runtime_sha256")!=details.locks.runtime_sha256 ||
            partition.at("policy_sha256")!=details.locks.policy_sha256)
            throw std::invalid_argument("live_parent_evidence_identity_drift");
        const auto boundary_identity=AI::BeautySurfaceShapeLock::identity(partition);
        for(const auto& row:boundary_identity.items())
            if(details.contour_locks->document.at(row.key())!=row.value())
                throw std::invalid_argument("live_parent_frozen_identity_drift");
        std::map<size_t,std::vector<Json>> explicit_cells;
        for (const auto& row:partition.at("faces")) for (const auto& cell:row.at("cells"))
            explicit_cells[row.at("source_face_id")].push_back(cell);
        std::set<std::string> frozen;
        for (const auto& lock:details.contour_locks->document.at("locks"))
            for (const auto* key:{"locked_cells","nested_cells","periocular_cells"})
                for (const auto& id:lock.value(key,Json::array())) frozen.insert(id.get<std::string>());
        std::set<size_t> reserved(details.reserved_faces.begin(),details.reserved_faces.end());
        std::map<size_t,Color> previous(inherited.begin(),inherited.end()),manual_roots(manual.begin(),manual.end());
        std::map<size_t,std::vector<const SC::SubfaceLabelEvidence*>> children;
        for (const auto& child:analysis.subface_labels) if(child.confidence>=SC::minimum_confidence)
            children[child.face_id].push_back(&child);
        std::map<std::string,size_t> retained;
        std::set<size_t> seen;
        std::vector<Unit> units;
        const Json root={{"polygon",{{1.,0.,0.},{0.,1.,0.},{0.,0.,1.}}},{"holes",Json::array()}};
        std::map<size_t,Json> observations;
        for(const auto& sample:details.parent_samples)observations[sample.at(0)]=sample;
        for(const auto& [face,label]:details.parent_verified_roots)
            observations[face]=Json::array({face,details.subjects.at(0),label,.99,size_t(2),size_t(2)});
        // A clipped parent cell is independently proved by coverage and native
        // visibility; a root observation need not contain a raster pixel.
        for(const auto& [face,cells]:explicit_cells)for(const auto& c:cells)
            if((c.at("label")=="skin" || c.at("label")=="cloth") && details.parent_source_colors.count(c.at("id")))
                observations.try_emplace(face,Json::array({face,c.at("subject_id"),c.at("label"),.99,size_t(2),size_t(2)}));
        for (const auto& [observed_face,sample]:observations) {
            stop();
            if (!sample.is_array() || sample.size()!=6) throw std::invalid_argument("invalid_parent_observation");
            const size_t face=sample.at(0);const auto sid=sample.at(1).get<std::string>(),label=sample.at(2).get<std::string>();
            if (face>=details.base_colors.size() || !seen.insert(face).second ||
                std::find(details.subjects.begin(),details.subjects.end(),sid)==details.subjects.end())
                throw std::invalid_argument("live_parent_observation_identity_drift");
            const auto observed_owner=parent(label);const double confidence=sample.at(3);
            const size_t pixels=sample.at(4),views=sample.at(5);
            if (!std::isfinite(confidence) || confidence>1 || pixels<views || views>16)
                throw std::invalid_argument("invalid_parent_observation_support");
            if (observed_owner=="hair") {++retained["HAIR_BOUNDARY_PROTECTED"];continue;}
            if (observed_owner.empty() || confidence<((label=="body-skin" || label=="cloth") ? .9 : .95) || views<2) {
                ++retained["PARENT_INDEPENDENT_SUPPORT_INSUFFICIENT"];continue;
            }
            const bool explicit_root=explicit_cells.count(face)!=0;
            if(!explicit_root && details.parent_proposal && !details.parent_verified_roots.count(face)) {
                ++retained["UNCONFIRMED_WHOLE_ROOT_PRESERVED"];continue;
            }
            if (!explicit_root && (reserved.count(face) ||
                (analysis.face_confidence[face]>=SC::minimum_confidence && conflict(analysis.face_labels[face],observed_owner)))) {
                ++retained["FROZEN_OR_PARENT_CONFLICT"];continue;
            }
            const std::vector<Json> candidates=explicit_root ? explicit_cells.at(face) : std::vector<Json>{root};
            for (const auto& cell:candidates) {
                const auto id=explicit_root ? cell.at("id").get<std::string>() : "source:"+std::to_string(face);
                const bool clipped_parent=explicit_root && (cell.at("label")=="skin" || cell.at("label")=="cloth") &&
                    details.parent_source_colors.count(id);
                if(explicit_root && details.parent_proposal && !clipped_parent && !details.parent_verified_roots.count(face)) {
                    ++retained["UNCONFIRMED_MIXED_SIBLING_PRESERVED"];continue;
                }
                const auto owner=clipped_parent ? cell.at("label").get<std::string>() : observed_owner;
                if (explicit_root && (frozen.count(id) || frozen_label(cell.at("label")) || cell.at("subject_id")!=sid ||
                    (cell.at("label")!="R6" && parent(cell.at("label"))!=owner))) {
                    ++retained["FROZEN_OR_CELL_PARENT_CONFLICT"];continue;
                }
                bool blocked=false;
                for (const auto* child:children[face]) if (conflict(child->label,owner)) {
                    if (!explicit_root) {blocked=true;break;}
                    std::array<SC::Barycentric,3> corners;
                    if (!SC::subface_vertices(child->path,corners)) throw std::invalid_argument("invalid_native_parent_child");
                    const auto mask=SP::polygons(Json::array({{{"polygon",corners},{"holes",Json::array()}}}),true);
                    if (SP::area(intersection_ex(SP::polygons(Json::array({cell}),true),mask))>SP::area_tolerance) {
                        blocked=true;break;
                    }
                }
                if (blocked) {++retained["MIXED_PARENT_CHILD_PRESERVED"];continue;}
                Unit unit;unit.id=id;unit.face=face;unit.subject=sid;unit.parent=owner;unit.polygon=cell;unit.implicit=!explicit_root;
                unit.manual=manual_roots.count(face) || manual_cells.count(id);
                const auto sampled=details.parent_source_colors.find(id);
                const auto root_sample=details.parent_source_colors.find("source:"+std::to_string(face));
                if(explicit_root && sampled==details.parent_source_colors.end() && root_sample==details.parent_source_colors.end()) {
                    ++retained["CLIPPED_UNIT_SOURCE_COLOR_UNAVAILABLE"];continue;
                }
                for (size_t k=0;k<3;++k) {
                    unit.source[k]=sampled!=details.parent_source_colors.end() ? sampled->second[k] :
                        root_sample!=details.parent_source_colors.end() ? root_sample->second[k] : details.base_colors[face][k];
                    if (!std::isfinite(unit.source[k]) || unit.source[k]<0 || unit.source[k]>1)
                        throw std::invalid_argument("invalid_verified_parent_source_color");
                }
                unit.lab=PortraitColorPlan::oklab(unit.source);
                if (explicit_root && details.cell_colors.count(id)) {unit.previous=details.cell_colors.at(id);unit.inheritance="SAVED_CELL_COLOR";}
                else if(previous.count(face)) {unit.previous=previous.at(face);unit.inheritance="NATIVE_ROOT_COLOR";}
                else unit.inheritance="SOURCE_UV_NO_OLD_SLOT";
                units.push_back(std::move(unit));
            }
        }
        stop();connect(units,source.mesh);stop();
        std::set<size_t> frontier;
        for (size_t i=0;i<units.size();++i) {
            auto& u=units[i];
            if (u.manual) {++retained["MANUAL_COLOR_PRIORITY"];continue;}
            if (u.parent=="skin") {u.target="portrait-skin";u.rule="UNIFIED_CONFIRMED_BODY_SKIN";}
            else if(chroma(u.lab)>.018) {++retained["LOCAL_SOURCE_PIGMENT_PATTERN_PRESERVED"];continue;}
            else if(u.lab[0]>=.65) {u.target="portrait-light";u.rule="COL009_NEUTRAL_CLOTH_CORE";frontier.insert(i);}
            else ++retained["NEUTRAL_SHADOW_REQUIRES_CONNECTED_CORE"];
            if (!u.target.empty() && !slots.count(u.target)) {++retained["MISSING_"+u.target];u.target.clear();frontier.erase(i);}
        }
        auto visited=frontier;
        for (size_t hop=0;hop<3 && !frontier.empty();++hop) {
            std::set<size_t> next;
            for (const auto i:frontier) for (const auto neighbor:units[i].neighbors) {
                auto& u=units[neighbor];
                if (visited.count(neighbor) || next.count(neighbor) || u.manual || u.parent!="cloth" || u.lab[0]<.42 || chroma(u.lab)>.018 ||
                    !continuous(units[i].lab,u.lab,"cloth")) continue;
                u.target="portrait-light";u.rule="COL009_CONNECTED_NEUTRAL_CLOTH_SHADOW";next.insert(neighbor);
                if(retained["NEUTRAL_SHADOW_REQUIRES_CONNECTED_CORE"])--retained["NEUTRAL_SHADOW_REQUIRES_CONNECTED_CORE"];
            }
            visited.insert(next.begin(),next.end());frontier=std::move(next);
        }
        // Donors are frozen before the single cleanup pass. A newly corrected
        // island cannot become a donor for another island or cross a boundary.
        std::set<size_t> donors;
        for(size_t i=0;i<units.size();++i) if(!units[i].target.empty() && units[i].previous &&
            *units[i].previous==slots.at(units[i].target)) donors.insert(i);
        std::set<size_t> candidates;
        for(size_t i=0;i<units.size();++i) if(!units[i].target.empty() && !donors.count(i))candidates.insert(i);
        size_t supported_islands=0;
        while(!candidates.empty()) {
            stop();std::vector<size_t> group{*candidates.begin()};candidates.erase(group.front());
            for(size_t k=0;k<group.size();++k) for(const auto n:units[group[k]].neighbors) {
                if(!candidates.count(n) || units[n].target!=units[group[k]].target || units[n].previous!=units[group[k]].previous)continue;
                candidates.erase(n);group.push_back(n);
            }
            if(group.size()>64)continue;
            for(const auto i:group) {
                size_t support=0;
                for(const auto n:units[i].neighbors) if(donors.count(n) && units[n].target==units[i].target &&
                    continuous(units[i].lab,units[n].lab,units[i].parent))++support;
                if(support>=2 && double(support)/std::max<size_t>(1,units[i].neighbors.size())>=2./3.) {
                    units[i].rule="FIXED_SUPPORT_SINGLE_PASS_ISLAND";++supported_islands;
                }
            }
        }
        Json rules=Json::object(),histories=Json::object();size_t changed_known=0,already_same=0,no_old_slot=0;
        for(const auto& u:units) {
            if(u.implicit)result.root_labels[u.face]=u.parent;else result.cell_labels[u.id]=u.parent;
            if(u.target.empty())continue;
            histories[u.inheritance]=histories.value(u.inheritance,size_t(0))+1;
            if(!u.previous)++no_old_slot;else if(*u.previous==slots.at(u.target))++already_same;else ++changed_known;
            if(u.implicit)result.root_colors[u.face]=slots.at(u.target);else result.cell_colors[u.id]=slots.at(u.target);
            rules[u.rule]=rules.value(u.rule,size_t(0))+1;
        }
        result.applied=result.root_colors.size()+result.cell_colors.size();
        result.reason=result.applied ? "live_parent_rules_applied" : "live_parent_no_safe_targets";
        size_t components=0;std::set<size_t> component_seen;
        for(size_t i=0;i<units.size();++i)if(component_seen.insert(i).second) {
            ++components;std::vector<size_t> group{i};
            for(size_t k=0;k<group.size();++k)for(const auto n:units[group[k]].neighbors)if(component_seen.insert(n).second)group.push_back(n);
        }
        result.audit={{"algorithm","frozen-detail-parent-cleanup/live-v2"},{"source_sha256",details.locks.source_sha256},
            {"geometry_id",source.geometry_id},{"evidence_sha256",details.parent_evidence_identity.empty() ?
                Json(details.locks.evidence_sha256) : details.parent_evidence_identity.at("evidence_sha256")},
            {"boundary_evidence_sha256",details.locks.evidence_sha256},
            {"frozen_boundary_sha256",frozen_boundary(partition,*details.contour_locks)},
            {"source_face_denominator",source.mesh.indices.size()},{"parent_observations",details.parent_samples.size()},
            {"reliable_units",units.size()},{"unclaimed_source_faces",source.mesh.indices.size()-seen.size()},
            {"uniform_units",result.applied},{"fixed_donors",donors.size()},{"supported_island_units",supported_islands},
            {"known_assignment_changed_units",changed_known},{"already_same_assignment_units",already_same},
            {"no_old_slot_units",no_old_slot},{"inheritance_counts",histories},{"continuous_parent_components",components},
            {"coverage",details.parent_coverage_audit},
            {"donor_assignment_source","CURRENT_VERIFIED_CANDIDATE_OR_SAVED_CELL_COLOR"},
            {"island_cleanup_is_subset_of_uniform_repair",true},{"island_cleanup_additional_targets",0},
            {"rule_counts",rules},{"preserved_reasons",retained},{"added_triangles",0},
            {"material_tree_changed",false},{"production_material_write",false}};
    } catch(const std::exception& error) {result={};result.reason=error.what();}
    return result;
}
} // namespace Live
} // namespace Slic3r::GUI::PortraitParentCleanup
