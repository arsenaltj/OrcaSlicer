#include "BeautyRegionBoundary.hpp"
#include "BeautyBoundaryContours.hpp"
#include "SurfaceSelectionState.hpp"
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>

namespace Slic3r::AI {
BeautyRegionBoundaryInput selected_region_boundary_input(
    const indexed_triangle_set& mesh,const BeautySurface& surface,
    const std::vector<uint32_t>& regions,uint32_t selected,
    const std::vector<uint8_t>& protected_faces,const std::function<bool()>& canceled) {
    const auto checkpoint=[&]{if(canceled && canceled())throw std::runtime_error("Region curve fitting cancelled.");};
    const auto require=[](bool ok){if(!ok)throw std::invalid_argument("Region boundary belongs to invalid or different geometry.");};
    checkpoint();const size_t count=mesh.indices.size();
    require(count>0 && regions.size()==count && surface.face_neighbors.size()==count &&
        surface.centers.size()==count && surface.normals.size()==count && surface.areas.size()==count &&
        (protected_faces.empty() || protected_faces.size()==count));
    require(selected>0 && std::find(regions.begin(),regions.end(),selected)!=regions.end());
    require(surface.geometry_id==SurfaceSelectionPersistence::geometry_fingerprint(mesh));
    std::vector<uint8_t> pinned(count,0);
    for(size_t f=0;f<count;++f) {
        if((f&4095)==0)checkpoint();
        require(regions[f]>0 && surface.centers[f].allFinite() && surface.normals[f].allFinite() &&
            std::isfinite(surface.areas[f]) && surface.areas[f]>=0);
        for(int c=0;c<3;++c){const int v=mesh.indices[f][c];require(v>=0 && size_t(v)<mesh.vertices.size() && mesh.vertices[size_t(v)].allFinite());}
        if(!protected_faces.empty()){require(protected_faces[f]<=1);pinned[f]=protected_faces[f];}
        if(surface.areas[f]==0)pinned[f]=1;
        std::set<uint32_t> owners{regions[f]};
        for(int32_t n:surface.face_neighbors[f]) {
            require(n>=-1 && (n<0 || (size_t(n)<count && size_t(n)!=f)));
            if(n<0){pinned[f]=1;continue;}
            require(std::find(surface.face_neighbors[size_t(n)].begin(),surface.face_neighbors[size_t(n)].end(),int32_t(f))!=surface.face_neighbors[size_t(n)].end());
            owners.insert(regions[size_t(n)]);
            if(surface.normals[f].dot(surface.normals[size_t(n)])<.8)pinned[f]=1;
        }
        if(owners.size()>2)pinned[f]=1;
    }
    std::vector<BeautyBoundaryContours::Edge> edges;
    for(size_t f=0;f<count;++f) {
        if((f&4095)==0)checkpoint();
        if(regions[f]!=selected)continue;
        for(size_t e=0;e<3;++e) {
            const int32_t n=surface.face_neighbors[f][e];
            if(n<0 || regions[size_t(n)]==selected)continue;
            const auto& t=mesh.indices[f];
            edges.push_back({mesh.vertices[size_t(t[int(e)])],mesh.vertices[size_t(t[int((e+1)%3)])],
                f,n,std::min(selected,regions[size_t(n)]),std::max(selected,regions[size_t(n)])});
        }
    }
    return {BeautyBoundaryContours::build(edges,mesh,surface.face_neighbors,canceled),std::move(pinned)};
}

BeautyRegionBoundaryPlan plan_selected_region_boundary(
    const indexed_triangle_set& mesh,const BeautySurface& surface,
    const std::vector<uint32_t>& regions,uint32_t selected,
    const std::vector<uint8_t>& protected_faces,const std::function<bool()>& canceled) {
    const auto checkpoint=[&]{if(canceled && canceled())throw std::runtime_error("Region curve fitting cancelled.");};
    const auto input=selected_region_boundary_input(mesh,surface,regions,selected,protected_faces,canceled);
    const auto& curves=input.curves;const auto& pinned=input.pinned_faces;
    const size_t count=mesh.indices.size();
    struct Vote {double distance;bool inside;};
    std::map<size_t,Vote> votes;
    size_t at=0;
    for(const auto& curve:curves) {
        if((at++&255)==0)checkpoint();
        if(curve.neighbor<0)continue;
        const size_t other=size_t(curve.neighbor),source=curve.face;
        if(pinned[source] || pinned[other])continue;
        const Vec3d tangent=(curve.b-curve.a).cast<double>();
        const double direction=tangent.dot(curve.source_tangent.cast<double>());
        const double spacing=curve.source_tangent.cast<double>().norm(),length2=tangent.squaredNorm();
        if(length2<=0 || spacing<=0 || (!curve.source_orientation && std::abs(direction)<.25*std::sqrt(length2)*spacing))continue;
        const double orientation=curve.source_orientation?curve.source_orientation:(direction<0?-1.:1.);
        const Vec3d normal=(surface.normals[source]+surface.normals[other]).normalized();
        std::vector<size_t> band{source,other};
        std::set<size_t> seen{source,other};
        size_t begin=0;
        for(unsigned ring=0;ring<3;++ring) {
            const size_t end=band.size();
            for(size_t i=begin;i<end;++i)for(int32_t raw:surface.face_neighbors[band[i]]) {
                if(raw<0)continue;const size_t n=size_t(raw);
                // Walk only the two local owners, avoiding nearby back surfaces
                // and third features even when their 3D positions are close.
                if((regions[n]==selected || regions[n]==regions[other]) && !pinned[n] && seen.insert(n).second)band.push_back(n);
            }
            begin=end;
        }
        for(size_t f:band) {
            if(pinned[f] || surface.normals[f].dot(normal)<.8)continue;
            const Vec3d a=curve.a.cast<double>(),delta=surface.centers[f]-a;
            const Vec3d offset=delta-tangent*std::clamp(delta.dot(tangent)/length2,0.,1.);
            const double distance=offset.squaredNorm();
            if(distance>4*spacing*spacing)continue;
            const double side=tangent.cross(offset).dot(normal)*orientation;
            const double tolerance=1e-9*std::sqrt(length2*std::max(surface.areas[f],1e-30));
            if(std::abs(side)<=tolerance)continue;
            const auto old=votes.find(f);
            if(old==votes.end() || distance<old->second.distance)votes[f]={distance,side>0};
        }
    }
    std::vector<uint8_t> added(count,0),removed(count,0);
    for(const auto& entry:votes) {
        if(entry.second.inside && regions[entry.first]!=selected)added[entry.first]=1;
        else if(!entry.second.inside && regions[entry.first]==selected)removed[entry.first]=1;
    }
    // Simultaneous inward/outward moves can strand an added patch after its
    // old attachment is removed. Every receiving component must still touch
    // unchanged ownership, providing a real local paint donor for extension.
    // Withdraw orphan proposals; never invent a color or repaint a remote face.
    bool progress=true;
    while(progress) {
        checkpoint();progress=false;
        const auto retain_attached=[&](auto& transfer,const auto& opposite,bool inward) {
            std::vector<uint8_t> reached(count,0);std::vector<size_t> queue;
            for(size_t f=0;f<count;++f)if(transfer[f])
                for(int32_t n:surface.face_neighbors[f])if(n>=0 && !opposite[size_t(n)] &&
                    ((regions[size_t(n)]!=selected)==inward) && !transfer[size_t(n)]) {
                    reached[f]=1;queue.push_back(f);break;
                }
            for(size_t at=0;at<queue.size();++at) {
                if((at&4095)==0)checkpoint();
                for(int32_t n:surface.face_neighbors[queue[at]])if(n>=0 && transfer[size_t(n)] && !reached[size_t(n)]) {
                    reached[size_t(n)]=1;queue.push_back(size_t(n));
                }
            }
            for(size_t f=0;f<count;++f)if(transfer[f] && !reached[f]){transfer[f]=0;progress=true;}
        };
        retain_attached(added,removed,false);retain_attached(removed,added,true);
    }
    BeautyRegionBoundaryPlan result;result.curve_segments=curves.size();
    for(size_t f=0;f<count;++f){if(added[f])result.added.push_back(f);if(removed[f])result.removed.push_back(f);}
    checkpoint();return result;
}
}
