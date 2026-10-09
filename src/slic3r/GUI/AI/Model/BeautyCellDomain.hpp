#pragma once

#include "SurfacePartition.hpp"
#include "SurfaceSelectionState.hpp"
#include <tuple>

namespace Slic3r::AI {

// Arbitrary clipped polygons retain cell IDs. They are never encoded as dyadic
// LeafKeys, and derived triangle indices never become source face IDs.
struct BeautyCellDomain {
    struct Triangle {
        size_t source_face_id;
        std::string cell_id;
        std::array<Vec3d,3> corners;
    };
    nlohmann::json partition;
    std::vector<Triangle> triangles;
    std::map<std::string,std::vector<size_t>> cell_triangles;
    std::vector<size_t> root_offsets;

    static std::shared_ptr<BeautyCellDomain> build(const nlohmann::json& document,
                                                  const nlohmann::json& identity) {
        SurfacePartition::validate(document,identity);
        auto result=std::make_shared<BeautyCellDomain>();
        result->partition=document;
        std::map<size_t,const nlohmann::json*> explicit_faces;
        for (const auto& face:document.at("faces"))
            explicit_faces.emplace(face.at("source_face_id"),&face);
        const size_t count=document.at("face_count");
        result->triangles.reserve(count+document.at("added_triangles").get<size_t>());
        const auto append=[&](size_t face,const std::string& id,const std::array<Vec3d,3>& corners) {
            if(!id.empty()) result->cell_triangles[id].push_back(result->triangles.size());
            result->triangles.push_back({face,id,corners});
        };
        for(size_t face=0;face<count;++face) {
            result->root_offsets.push_back(result->triangles.size());
            const auto found=explicit_faces.find(face);
            if(found==explicit_faces.end()) {
                append(face,{},
                    {Vec3d(1,0,0),Vec3d(0,1,0),Vec3d(0,0,1)});
                continue;
            }
            for(const auto& cell:found->second->at("cells")) for(const auto& triangle:cell.at("triangles")) {
                std::array<Vec3d,3> corners;
                for(size_t i=0;i<3;++i) for(size_t k=0;k<3;++k) corners[i][k]=triangle[i][k].get<double>();
                append(face,cell.at("id"),corners);
            }
        }
        result->root_offsets.push_back(result->triangles.size());
        return result;
    }
    size_t source_face_count() const { return partition.at("face_count"); }
    std::string fingerprint() const { return partition.at("partition_sha256"); }
    std::string cell_id(size_t triangle) const {
        const auto& entry=triangles.at(triangle);
        return entry.cell_id.empty() ? "source:"+std::to_string(entry.source_face_id) : entry.cell_id;
    }
    std::vector<size_t> indices(const std::string& id) const {
        const auto found=cell_triangles.find(id);
        if(found!=cell_triangles.end()) return found->second;
        if(id.compare(0,7,"source:")==0) {
            const auto value=id.substr(7);
            if(!value.empty() && value.size()<=20 && value.find_first_not_of("0123456789")==std::string::npos) {
                const size_t face=size_t(std::stoull(value));
                if(id=="source:"+std::to_string(face) && face<source_face_count() &&
                   root_offsets[face+1]==root_offsets[face]+1 && triangles[root_offsets[face]].cell_id.empty())
                    return {root_offsets[face]};
            }
        }
        throw std::invalid_argument("Cell is outside the editing mapping.");
    }
    indexed_triangle_set mesh(const indexed_triangle_set& source) const {
        if(source.indices.size()!=source_face_count() ||
            SurfaceSelectionPersistence::geometry_fingerprint(source)!=partition.at("geometry_id"))
            throw std::invalid_argument("Contour source mapping changed.");
        indexed_triangle_set result;
        result.vertices.reserve(triangles.size()*3); result.indices.reserve(triangles.size());
        for(const auto& triangle:triangles) {
            const auto& root=source.indices[triangle.source_face_id];
            const int first=int(result.vertices.size());
            for(const auto& bary:triangle.corners) {
                Vec3d point=Vec3d::Zero();
                for(size_t k=0;k<3;++k) point+=bary[k]*source.vertices[root[k]].cast<double>();
                result.vertices.push_back(point.cast<float>());
            }
            result.indices.emplace_back(first,first+1,first+2);
        }
        return result;
    }
    std::vector<std::vector<int32_t>> adjacency(const indexed_triangle_set& source) const {
        using Point=std::tuple<size_t,size_t,int64_t,int64_t>;
        std::map<std::tuple<float,float,float>,size_t> welded;
        std::vector<size_t> vertices; vertices.reserve(source.vertices.size());
        for(const auto& p:source.vertices) {
            auto entry=welded.emplace(std::make_tuple(p[0],p[1],p[2]),welded.size());
            vertices.push_back(entry.first->second);
        }
        const auto point=[&](const Triangle& t,const Vec3d& bary) -> Point {
            const auto& root=source.indices[t.source_face_id];
            for(size_t k=0;k<3;++k) if(bary[k]>=1.-2e-9) return {vertices[root[k]],size_t(-1),0,0};
            for(size_t zero=0;zero<3;++zero) if(std::abs(bary[zero])<=2e-9) {
                size_t a=(zero+1)%3,b=(zero+2)%3;
                if(vertices[root[a]]>vertices[root[b]]) std::swap(a,b);
                return {vertices[root[a]],vertices[root[b]],int64_t(std::llround(bary[b]*1e9)),0};
            }
            return {size_t(-1),t.source_face_id,int64_t(std::llround(bary[1]*1e9)),int64_t(std::llround(bary[2]*1e9))};
        };
        std::map<std::pair<Point,Point>,std::vector<size_t>> edges;
        std::vector<std::vector<int32_t>> result(triangles.size());
        for(size_t i=0;i<triangles.size();++i) for(size_t edge=0;edge<3;++edge) {
            auto a=point(triangles[i],triangles[i].corners[edge]);
            auto b=point(triangles[i],triangles[i].corners[(edge+1)%3]);
            if(b<a) std::swap(a,b);
            edges[{a,b}].push_back(i);
        }
        for(const auto& edge:edges) if(edge.second.size()==2) {
            const auto a=edge.second[0],b=edge.second[1];
            result[a].push_back(int32_t(b));result[b].push_back(int32_t(a));
        }
        return result;
    }
    size_t locate(size_t face,const Vec3d& bary) const {
        if(face>=source_face_count() || !bary.allFinite() || std::abs(bary.sum()-1.)>1e-7 || bary.minCoeff()<-2e-8)
            throw std::invalid_argument("Invalid contour surface hit.");
        const Vec2d p(bary[1],bary[2]);
        for(size_t i=root_offsets[face];i<root_offsets[face+1];++i) {
            const auto& corners=triangles[i].corners;
            Eigen::Matrix2d matrix;
            matrix.col(0)=Vec2d(corners[1][1]-corners[0][1],corners[1][2]-corners[0][2]);
            matrix.col(1)=Vec2d(corners[2][1]-corners[0][1],corners[2][2]-corners[0][2]);
            if(std::abs(matrix.determinant())<1e-20) continue;
            const auto local=(matrix.inverse()*(p-Vec2d(corners[0][1],corners[0][2]))).eval();
            if(local.minCoeff()>=-2e-8 && local.sum()<=1.+2e-8) return i;
        }
        throw std::invalid_argument("Surface hit is outside the contour partition.");
    }
};
} // namespace Slic3r::AI
