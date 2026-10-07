#include "BeautyRegionPrecision.hpp"
#include "SurfaceSelectionState.hpp"
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace Slic3r::AI {
BeautyRegionPrecision refine_selected_region_mesh(
    const indexed_triangle_set& mesh,const BeautySurface& surface,const std::vector<uint32_t>& regions,
    uint32_t selected,const std::vector<uint8_t>& protected_faces,const std::function<bool()>& canceled) {
    const auto check=[&]{if(canceled && canceled())throw std::runtime_error("Precise region edit cancelled.");};
    const auto require=[](bool ok){if(!ok)throw std::invalid_argument("Invalid precise region surface.");};
    const auto input=selected_region_boundary_input(mesh,surface,regions,selected,protected_faces,canceled);
    require(surface.vertex_class.size()==mesh.vertices.size() && !surface.class_representative.empty());
    const size_t classes=surface.class_representative.size(),faces=mesh.indices.size();
    std::vector<uint8_t> inside(classes,0),outside(classes,0),fixed(classes,0);
    std::vector<double> spacing(classes,0),values(classes,0);
    std::vector<std::set<uint32_t>> owners(classes);
    for(size_t f=0;f<faces;++f) {
        if((f&4095)==0)check();
        for(int c=0;c<3;++c) {
            const size_t v=size_t(mesh.indices[f][c]),group=surface.vertex_class[v];require(group<classes);
            (regions[f]==selected?inside:outside)[group]=1;
            fixed[group]|=input.pinned_faces[f];owners[group].insert(regions[f]);
            spacing[group]=std::max(spacing[group],(mesh.vertices[v]-mesh.vertices[size_t(mesh.indices[f][(c+1)%3])]).cast<double>().norm());
        }
    }
    for(size_t v=0;v<classes;++v) {
        require(surface.class_representative[v]<mesh.vertices.size());
        if(owners[v].size()>2)fixed[v]=1;
        values[v]=inside[v] && outside[v]?0.:inside[v]?spacing[v]:-spacing[v];
    }
    struct Vote {double distance,value;uint32_t other;};
    std::map<size_t,Vote> votes;std::vector<double> face_distance(faces,std::numeric_limits<double>::infinity());
    std::vector<uint32_t> other_owner(faces,0);
    size_t at=0;
    for(const auto& curve:input.curves) {
        if((at++&255)==0)check();
        if(curve.neighbor<0)continue;
        const size_t source=curve.face,other=size_t(curve.neighbor);
        if(input.pinned_faces[source] || input.pinned_faces[other])continue;
        const Vec3d a=curve.a.cast<double>(),tangent=(curve.b-curve.a).cast<double>();
        const double length2=tangent.squaredNorm(),edge=curve.source_tangent.cast<double>().norm();
        const double orientation=tangent.dot(curve.source_tangent.cast<double>());
        if(length2<=0 || edge<=0 || (!curve.source_orientation && std::abs(orientation)<.25*std::sqrt(length2)*edge))continue;
        const double side_orientation=curve.source_orientation?curve.source_orientation:(orientation<0?-1.:1.);
        const Vec3d normal=(surface.normals[source]+surface.normals[other]).normalized();
        std::vector<size_t> band{source,other};std::set<size_t> seen{source,other};size_t begin=0;
        for(unsigned ring=0;ring<3;++ring) {
            const size_t end=band.size();
            for(size_t i=begin;i<end;++i)for(int32_t raw:surface.face_neighbors[band[i]]) {
                if(raw<0)continue;const size_t f=size_t(raw);
                if(!input.pinned_faces[f] && (regions[f]==selected || regions[f]==regions[other]) && seen.insert(f).second)band.push_back(f);
            }
            begin=end;
        }
        std::set<size_t> visited_vertices;
        const auto sample=[&](const Vec3d& p) {
            const Vec3d d=p-a,offset=d-tangent*std::clamp(d.dot(tangent)/length2,0.,1.);
            return std::pair<double,double>{offset.squaredNorm(),tangent.cross(offset).dot(normal)*side_orientation/std::sqrt(length2)};
        };
        for(size_t f:band) {
            if(input.pinned_faces[f] || surface.normals[f].dot(normal)<.8)continue;
            const double fd=sample(surface.centers[f]).first;
            if(fd<face_distance[f]){face_distance[f]=fd;other_owner[f]=regions[other];}
            for(int raw:mesh.indices[f]) {
                const size_t group=surface.vertex_class[size_t(raw)];
                if(fixed[group] || !visited_vertices.insert(group).second)continue;
                const auto s=sample(mesh.vertices[size_t(raw)].cast<double>());
                if(s.first>4*edge*edge)continue;
                const auto old=votes.find(group);
                if(old==votes.end() || s.first<old->second.distance)votes[group]={s.first,s.second,regions[other]};
            }
        }
    }
    for(const auto& vote:votes)values[vote.first]=std::abs(vote.second.value)<=spacing[vote.first]*1e-7?0.:vote.second.value;
    // An unvisited face must not be reclassified through a nearby back surface
    // or another owner. Pin its incident field to the original border.
    for(size_t f=0;f<faces;++f)if(!other_owner[f])for(int raw:mesh.indices[f]) {
        const size_t group=surface.vertex_class[size_t(raw)];
        if(regions[f]!=selected && values[group]>0)values[group]=0;
        if(regions[f]==selected && values[group]<0)values[group]=0;
    }
    BeautyRegionPrecision result;result.source_geometry_id=surface.geometry_id;result.curve_segments=input.curves.size();
    result.source_face_components.assign(faces,UINT32_MAX);
    for(size_t seed=0;seed<faces;++seed)if(result.source_face_components[seed]==UINT32_MAX) {
        check();const uint32_t component=uint32_t(result.source_component_regions.size());result.source_component_regions.push_back(regions[seed]);
        std::vector<size_t> queue{seed};result.source_face_components[seed]=component;
        for(size_t i=0;i<queue.size();++i) {
            if((i&4095)==0)check();for(int32_t raw:surface.face_neighbors[queue[i]])if(raw>=0) {
                const size_t f=size_t(raw);if(regions[f]==regions[seed] && result.source_face_components[f]==UINT32_MAX){result.source_face_components[f]=component;queue.push_back(f);}
            }
        }
    }
    result.mesh.vertices=mesh.vertices;result.vertices.reserve(mesh.vertices.size()+votes.size());
    for(size_t v=0;v<mesh.vertices.size();++v)result.vertices.push_back({uint32_t(v),uint32_t(v),0.});
    using Edge=std::pair<uint32_t,uint32_t>;
    std::map<Edge,int> cuts;
    // Position-welded cuts share the exact float point; source indices still
    // get separate interpolated UVs/normals on a seam.
    std::map<Edge,Vec3f> welded_positions;
    const auto cut=[&](int raw_a,int raw_b) {
        const uint32_t va=uint32_t(std::min(raw_a,raw_b)),vb=uint32_t(std::max(raw_a,raw_b));
        const auto found=cuts.find({va,vb});if(found!=cuts.end())return found->second;
        const uint32_t ca=surface.vertex_class[va],cb=surface.vertex_class[vb];
        double t=values[ca]/(values[ca]-values[cb]);require(std::isfinite(t) && t>0 && t<1);
        const Edge welded{std::min(ca,cb),std::max(ca,cb)};
        auto point=welded_positions.find(welded);
        if(point==welded_positions.end()) {
            const uint32_t x=surface.class_representative[welded.first],y=surface.class_representative[welded.second];
            const double alpha=values[welded.first]/(values[welded.first]-values[welded.second]);
            point=welded_positions.emplace(welded,((1-alpha)*mesh.vertices[x].cast<double>()+alpha*mesh.vertices[y].cast<double>()).cast<float>()).first;
        }
        require(result.mesh.vertices.size()<6000000);
        const int id=int(result.mesh.vertices.size());result.mesh.vertices.push_back(point->second);result.vertices.push_back({va,vb,t});
        cuts.emplace(Edge{va,vb},id);return id;
    };
    const auto emit=[&](const std::vector<int>& polygon,size_t parent,uint32_t owner) {
        require(owner>0);
        for(size_t i=1;i+1<polygon.size();++i) {
            const Vec3i32 tri(polygon[0],polygon[i],polygon[i+1]);
            if(tri[0]==tri[1] || tri[1]==tri[2] || tri[2]==tri[0])continue;
            const Vec3d a=result.mesh.vertices[size_t(tri[0])].cast<double>(),b=result.mesh.vertices[size_t(tri[1])].cast<double>(),c=result.mesh.vertices[size_t(tri[2])].cast<double>();
            require((b-a).cross(c-a).dot(surface.normals[parent])>0);
            require(result.mesh.indices.size()<BeautyPuzzle::max_faces);
            result.mesh.indices.push_back(tri);result.parent_faces.push_back(parent);result.face_region.push_back(owner);
        }
    };
    for(size_t f=0;f<faces;++f) {
        if((f&4095)==0)check();const auto& tri=mesh.indices[f];
        std::array<double,3> s;bool positive=false,negative=false;
        for(int c=0;c<3;++c){s[c]=values[surface.vertex_class[size_t(tri[c])]];positive|=s[c]>0;negative|=s[c]<0;}
        if(input.pinned_faces[f] || surface.areas[f]==0 || (!positive && !negative)) {
            result.mesh.indices.push_back(tri);result.parent_faces.push_back(f);result.face_region.push_back(regions[f]);continue;
        }
        const uint32_t outside_id=regions[f]==selected?other_owner[f]:regions[f];
        if(!positive || !negative) {emit({tri[0],tri[1],tri[2]},f,positive?selected:outside_id);continue;}
        std::vector<int> plus,minus;
        for(int c=0;c<3;++c) {
            const int next=(c+1)%3;
            if(s[c]>=0)plus.push_back(tri[c]);if(s[c]<=0)minus.push_back(tri[c]);
            if((s[c]>0 && s[next]<0) || (s[c]<0 && s[next]>0)) {
                const int v=cut(tri[c],tri[next]);plus.push_back(v);minus.push_back(v);
            }
        }
        emit(plus,f,selected);emit(minus,f,outside_id);++result.split_faces;
    }
    if(result.split_faces==0 && result.face_region==regions) {
        result.mesh=mesh;result.vertices.resize(mesh.vertices.size());check();return result;
    }
    // Match the importer's first-use vertex order, dropping unused vertices.
    std::vector<int> compact(result.mesh.vertices.size(),-1);indexed_triangle_set reordered;
    std::vector<SurfaceVertexBlend> blends;
    for(auto& tri:result.mesh.indices)for(int c=0;c<3;++c) {
        const size_t v=size_t(tri[c]);
        if(compact[v]<0){compact[v]=int(reordered.vertices.size());reordered.vertices.push_back(result.mesh.vertices[v]);blends.push_back(result.vertices[v]);}
        tri[c]=compact[v];
    }
    result.mesh.vertices=std::move(reordered.vertices);result.vertices=std::move(blends);check();return result;
}

BeautyPrecisionValidity assess_beauty_precision(const BeautyRegionPrecision& refined,const BeautySurface& surface) {
    const size_t count=refined.face_region.size();
    if(!count || refined.parent_faces.size()!=count || surface.face_neighbors.size()!=count || refined.source_component_regions.empty())
        throw std::invalid_argument("Invalid refined region connectivity.");
    BeautyPrecisionValidity result;result.source_components=refined.source_component_regions.size();
    std::vector<size_t> occurrences(result.source_components,0);std::vector<uint8_t> visited(count,0);
    for(size_t seed=0;seed<count;++seed)if(!visited[seed]) {
        ++result.refined_components;std::vector<size_t> queue{seed};visited[seed]=1;std::set<uint32_t> ancestry;
        for(size_t i=0;i<queue.size();++i) {
            const size_t f=queue[i],parent=refined.parent_faces[f];
            if(parent>=refined.source_face_components.size())throw std::invalid_argument("Invalid refined component ancestry.");
            const uint32_t id=refined.source_face_components[parent];
            if(id>=result.source_components)throw std::invalid_argument("Invalid source semantic component.");
            if(refined.source_component_regions[id]==refined.face_region[f])ancestry.insert(id);
            for(int32_t raw:surface.face_neighbors[f]) {
                if(raw < -1 || (raw>=0 && (size_t(raw)>=count || size_t(raw)==f)))throw std::invalid_argument("Invalid refined component adjacency.");
                if(raw>=0 && !visited[size_t(raw)] && refined.face_region[size_t(raw)]==refined.face_region[seed]){visited[size_t(raw)]=1;queue.push_back(size_t(raw));}
            }
        }
        if(ancestry.empty())++result.unanchored_components;
        if(ancestry.size()>1)++result.merged_components;
        for(uint32_t id:ancestry)++occurrences[id];
    }
    for(size_t count:occurrences){if(!count)++result.removed_components;else if(count>1)result.split_components+=count-1;}
    return result;
}

BeautyPrecisionLayers remap_beauty_precision(const BeautyRegionPrecision& refined,
    const indexed_triangle_set& loaded,const BeautySurface& surface,const BeautyPuzzle& original,const std::string& hash,
    bool allow_component_changes_for_review) {
    if(original.geometry_id!=refined.source_geometry_id || loaded.indices!=refined.mesh.indices ||
       loaded.vertices.size()!=refined.mesh.vertices.size() || refined.parent_faces.size()!=loaded.indices.size() ||
       refined.face_region.size()!=loaded.indices.size() || surface.geometry_id!=SurfaceSelectionPersistence::geometry_fingerprint(loaded))
        throw std::invalid_argument("Refined beauty layers do not match the actual model.");
    for(size_t v=0;v<loaded.vertices.size();++v) {
        const double tolerance=std::max(1e-5,refined.mesh.vertices[v].cast<double>().cwiseAbs().maxCoeff()*3e-6);
        if(!loaded.vertices[v].allFinite() || (loaded.vertices[v]-refined.mesh.vertices[v]).cast<double>().norm()>tolerance)
            throw std::invalid_argument("Refined beauty surface moved during saving.");
    }
    if(!allow_component_changes_for_review && !assess_beauty_precision(refined,surface).applicable())
        throw std::invalid_argument("Precision fitting changed semantic connectivity; correct the region before applying it.");
    BeautyPrecisionLayers result;result.printing=original;result.printing.geometry_id=surface.geometry_id;result.printing.face_piece.clear();
    for(size_t parent:refined.parent_faces)result.printing.face_piece.push_back(original.face_piece.at(parent));
    result.printing.validate(surface);
    result.editing={surface.geometry_id,hash,refined.face_region};result.editing.validate(surface.geometry_id,hash,loaded.indices.size());
    return result;
}
}
