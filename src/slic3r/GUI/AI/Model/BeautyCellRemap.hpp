#pragma once
#include "SurfacePartition.hpp"
#include <set>

namespace Slic3r::AI {
// Migrate triangle-index masks only when they describe exactly the same area.
// A partly selected cell must never silently become an entirely selected cell.
inline nlohmann::json remap_cell_edits(const nlohmann::json& value,
    const nlohmann::json& before,const nlohmann::json& after,
    const std::string& old_boundary,const std::string& new_boundary) {
    namespace SP=SurfacePartition;using Json=nlohmann::json;
    if(value.is_null() || value.empty()) return value;
    SP::require(value.at("schema")=="orca.beauty-cell-edit/v1" &&
        value.at("geometry_id")==before.at("geometry_id") && value.at("source_sha256")==before.at("source_sha256") &&
        value.at("mapping_sha256")==before.at("partition_sha256") && value.at("boundary_sha256")==old_boundary,
        "Old contour selection identity changed.");
    std::map<size_t,const Json*> old_faces,new_faces;
    for(const auto& f:before.at("faces")) old_faces.emplace(f.at("source_face_id"),&f);
    for(const auto& f:after.at("faces")) new_faces.emplace(f.at("source_face_id"),&f);
    SP::require(before.at("face_count")==after.at("face_count") && old_faces.size()==new_faces.size(),"Contour roots changed during rebuild.");
    Json result=value;
    size_t old_total=before.at("face_count"),new_total=after.at("face_count");
    for(const auto& f:before.at("faces")) {--old_total;for(const auto& c:f.at("cells")) old_total+=c.at("triangles").size();}
    for(const auto& f:after.at("faces")) {--new_total;for(const auto& c:f.at("cells")) new_total+=c.at("triangles").size();}
    SP::require(value.at("triangle_count")==old_total,"Old contour selection size changed.");
    for(const auto* name:{"selected","protected","foreground","domain"}) {
        const auto indices=value.at(name).get<std::vector<size_t>>();
        SP::require(std::is_sorted(indices.begin(),indices.end()) &&
            std::adjacent_find(indices.begin(),indices.end())==indices.end() &&
            (indices.empty() || indices.back()<old_total),"Invalid old contour selection mask.");
        Json output=Json::array();size_t oi=0,ni=0;
        for(size_t root=0;root<before.at("face_count").get<size_t>();++root) {
            const auto found=old_faces.find(root);
            if(found==old_faces.end()) {if(std::binary_search(indices.begin(),indices.end(),oi)) output.push_back(ni);++oi;++ni;continue;}
            SP::require(new_faces.count(root),"Rebuilt root disappeared.");
            const auto& old_cells=found->second->at("cells");const auto& new_cells=new_faces.at(root)->at("cells");
            SP::require(old_cells.size()==new_cells.size(),"Rebuilt cells changed.");
            for(size_t c=0;c<old_cells.size();++c) {
                const auto& a=old_cells[c];const auto& b=new_cells[c];
                SP::require(a.at("id")==b.at("id") && SP::cell_boundary(a)==SP::cell_boundary(b),"Rebuild changed a protected cell boundary.");
                const size_t ac=a.at("triangles").size(),bc=b.at("triangles").size();
                const auto begin=std::lower_bound(indices.begin(),indices.end(),oi);
                const auto end=std::lower_bound(begin,indices.end(),oi+ac);
                if(size_t(end-begin)==ac) for(size_t k=0;k<bc;++k) output.push_back(ni+k);
                else if(begin!=end) {
                    ExPolygons selected;
                    for(auto index=begin;index!=end;++index) {
                        const auto& t=a.at("triangles")[*index-oi];
                        Points p;for(const auto& v:t) p.emplace_back(coord_t(std::llround(v[1].get<double>()*SP::scale)),coord_t(std::llround(v[2].get<double>()*SP::scale)));
                        if(SurfaceTriangulation::orientation(p[0],p[1],p[2])==0) continue;
                        Polygon triangle(p);SurfaceTriangulation::orient(triangle,true);selected=union_ex(selected,ExPolygons{ExPolygon(triangle)});
                    }
                    for(size_t k=0;k<bc;++k) {
                        const auto region=SP::polygons(Json::array({{{"polygon",b.at("triangles")[k]},{"holes",Json::array()}}}),true);
                        if(diff_ex(region,selected).empty()) output.push_back(ni+k);
                        else SP::require(intersection_ex(region,selected).empty(),"Partial triangle selection crosses the rebuilt mesh; retain draft and clear that selection before retrying.");
                    }
                }
                oi+=ac;ni+=bc;
            }
        }
        result[name]=std::move(output);
    }
    result["mapping_sha256"]=after.at("partition_sha256");result["boundary_sha256"]=new_boundary;result["triangle_count"]=new_total;
    return result;
}
} // namespace Slic3r::AI
