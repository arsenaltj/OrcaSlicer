#pragma once

#include "BeautyLeafDomain.hpp"
#include "SurfaceTriangulation.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Tesselate.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>

namespace Slic3r::AI::SurfacePartition {
using Json = nlohmann::json;
constexpr double scale = 1000000000.;
constexpr double area_tolerance = 2e-8;

inline void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}

inline bool hash(const std::string& value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

inline ExPolygons root() {
    return {ExPolygon(Polygon({Point(0,0), Point(coord_t(scale),0), Point(0,coord_t(scale))}))};
}

inline double area(const ExPolygons& polygons) {
    boost::multiprecision::int128_t sum=0;
    for (const auto& polygon : polygons) {
        sum+=SurfaceTriangulation::signed_twice_area(polygon.contour);
        for(const auto& hole:polygon.holes) sum+=SurfaceTriangulation::signed_twice_area(hole);
    }
    return sum.convert_to<double>()/(2.*scale*scale);
}

inline Polygon contour(const Json& vertices, bool bounded) {
    require(vertices.is_array() && vertices.size() >= 3 && vertices.size() <= 8192, "Invalid partition contour.");
    Points points;
    for (const auto& vertex : vertices) {
        require(vertex.is_array() && vertex.size() == 3, "Invalid barycentric vertex.");
        const auto b = vertex.get<std::array<double,3>>();
        require(std::all_of(b.begin(), b.end(), [](double x) { return std::isfinite(x) && std::abs(x) < 1e5; }) &&
                std::abs(b[0] + b[1] + b[2] - 1.) < 1e-7, "Invalid barycentric coordinates.");
        if (bounded) require(*std::min_element(b.begin(), b.end()) >= -2e-8 &&
                             *std::max_element(b.begin(), b.end()) <= 1. + 2e-8, "Cell leaves the source face.");
        const Point p(coord_t(std::llround(b[1] * scale)), coord_t(std::llround(b[2] * scale)));
        if (points.empty() || points.back() != p) points.push_back(p);
    }
    if (points.size() > 1 && points.front() == points.back()) points.pop_back();
    require(points.size() >= 3, "Collapsed partition contour.");
    Polygon result(std::move(points));
    require(SurfaceTriangulation::signed_twice_area(result)!=0, "Zero area partition contour.");
    return result;
}

inline ExPolygons polygons(const Json& value, bool bounded = false) {
    require(value.is_array() && value.size() <= 256, "Invalid polygon set.");
    ExPolygons result;
    for (const auto& row : value) {
        ExPolygon p;
        p.contour = contour(row.at("polygon"), bounded);
        SurfaceTriangulation::orient(p.contour,true);
        for (const auto& hole : row.value("holes", Json::array())) {
            auto h = contour(hole, bounded); SurfaceTriangulation::orient(h,false); p.holes.push_back(std::move(h));
        }
        result.push_back(std::move(p));
    }
    return bounded && result.size() == 1 ? result : union_ex(result);
}

inline Json encode_contour(Polygon polygon, bool outer) {
    SurfaceTriangulation::orient(polygon,outer);
    auto& points = polygon.points;
    const auto first = std::min_element(points.begin(), points.end(), [](const Point& a, const Point& b) {
        return a.x() < b.x() || (a.x() == b.x() && a.y() < b.y());
    });
    std::rotate(points.begin(), first, points.end());
    Json result = Json::array();
    for (const auto& p : points) result.push_back({1. - double(p.x() + p.y()) / scale, double(p.x()) / scale, double(p.y()) / scale});
    return result;
}

inline Json encode_polygon(const ExPolygon& polygon) {
    Json holes = Json::array();
    for (const auto& h : polygon.holes) holes.push_back(encode_contour(h, false));
    std::sort(holes.begin(), holes.end(), [](const Json& a, const Json& b) { return a.dump() < b.dump(); });
    return {{"polygon", encode_contour(polygon.contour, true)}, {"holes", holes}};
}

inline Json legacy_triangles(const ExPolygon& polygon) {
    const auto points = triangulate_expolygon_2d(polygon);
    require(!points.empty() && points.size() % 3 == 0, "Partition tessellation failed.");
    Json result = Json::array();
    double total = 0.;
    const double divisor = scale * SCALING_FACTOR;
    for (size_t i = 0; i < points.size(); i += 3) {
        Json triangle = Json::array();
        for (size_t j = 0; j < 3; ++j) {
            const auto p = points[i+j] / divisor;
            triangle.push_back({1. - p.x() - p.y(), p.x(), p.y()});
        }
        const Vec2d a = points[i] / divisor, b = points[i+1] / divisor, c = points[i+2] / divisor;
        total += std::abs((b.x()-a.x())*(c.y()-a.y())-(b.y()-a.y())*(c.x()-a.x())) * .5;
        result.push_back(std::move(triangle));
    }
    require(std::abs(total - polygon.area() / (scale * scale)) < area_tolerance, "Tessellation does not cover its cell.");
    return result;
}

inline Json triangles(const ExPolygon& polygon) {
    const auto points=to_points(ExPolygons{polygon});
    const auto indices=SurfaceTriangulation::triangulate(polygon);
    Json result=Json::array();
    ExPolygons covered;
    for(const auto& t:indices) {
        Polygon triangle({points[t[0]],points[t[1]],points[t[2]]});
        const ExPolygons region{ExPolygon(triangle)};
        require(SurfacePartition::area(diff_ex(region,ExPolygons{polygon}))<area_tolerance,"Triangle leaves its cell or enters a hole.");
        require(SurfacePartition::area(intersection_ex(covered,region))<area_tolerance,"Cell triangles overlap.");
        covered=union_ex(covered,region);
        result.push_back(encode_contour(std::move(triangle),true));
    }
    require(SurfacePartition::area(diff_ex(ExPolygons{polygon},covered))<area_tolerance,"Tessellation does not cover its cell.");
    return result;
}

inline void normalize_tessellation(Json& document) {
    int64_t added=document.at("added_triangles");
    for(auto& face:document["faces"]) {
        size_t count=0;
        for(auto& cell:face["cells"]) {
            const auto old_count=cell.at("triangles").size();
            cell["triangles"]=triangles(polygons(Json::array({cell}),true).front());
            count+=cell["triangles"].size();
            added+=int64_t(cell["triangles"].size())-int64_t(old_count);
        }
        face["triangle_count"]=count;
    }
    require(added>=0 && size_t(added)<=document.at("triangle_budget").get<size_t>(),"Rebuilt contour exceeds its cumulative budget.");
    document["added_triangles"]=added;
    document["tessellation_version"]=1;
}

inline ExPolygons view_polygons(const Json& view, const Json& library) {
    if (view.contains("polygons")) return polygons(view.at("polygons"));
    require(view.at("matrix").is_array() && view.at("matrix").size() == 3, "Invalid face projection matrix.");
    const auto matrix = view.at("matrix").get<std::array<std::array<double,3>,3>>();
    for (const auto& row : matrix) for (double x : row) require(std::isfinite(x) && std::abs(x)<1e5, "Invalid source projection.");
    const auto projected_contours = [&](const Json& ids) {
      Polygons contours;
      for (const auto& id : ids) {
        const auto& curve = library.at(id.get<std::string>());
        require(curve.size() >= 3 && curve.size() <= 8192, "Invalid shared fitted curve.");
        Points points;
        for (const auto& vertex : curve) {
            const auto p = vertex.get<std::array<double,2>>();
            require(std::isfinite(p[0]) && std::isfinite(p[1]), "Invalid fitted curve point.");
            const double b1 = p[0]*matrix[0][1] + p[1]*matrix[1][1] + matrix[2][1];
            const double b2 = p[0]*matrix[0][2] + p[1]*matrix[1][2] + matrix[2][2];
            require(std::isfinite(b1) && std::isfinite(b2) && std::abs(b1)<1e5 && std::abs(b2)<1e5, "Projected curve exceeds numerical bounds.");
            const Point q(coord_t(std::llround(b1*scale)),coord_t(std::llround(b2*scale)));
            if (points.empty() || points.back()!=q) points.push_back(q);
        }
        if (points.size()>1 && points.front()==points.back()) points.pop_back();
        if (points.size()<3) continue;
        Polygon polygon(std::move(points)); polygon.make_counter_clockwise(); contours.push_back(std::move(polygon));
      }
      return union_ex(contours);
    };
    const auto outer = projected_contours(view.at("contour_ids"));
    return view.contains("exclude_contour_ids") ? diff_ex(outer,projected_contours(view.at("exclude_contour_ids"))) : outer;
}

inline ExPolygons majority(const Json& views, const Json& library = Json::object()) {
    require(views.is_array() && views.size() <= 16, "Invalid independent view list.");
    std::set<std::string> families;
    std::vector<ExPolygons> sets;
    for (const auto& view : views) {
        const auto family = view.at("family").get<std::string>();
        require(!family.empty() && families.insert(family).second, "Duplicate independent camera.");
        sets.push_back(intersection_ex(root(), view_polygons(view,library)));
    }
    if (sets.size() < 2) return {};
    // Dynamic coverage levels compute strict majority, without treating a
    // zoomed crop as an independent witness or raster-voting entire faces.
    const size_t threshold = std::max(size_t(2), sets.size()/2 + 1);
    std::vector<ExPolygons> levels(sets.size()+1);
    size_t visited = 0;
    for (const auto& mask : sets) {
        for (size_t k = ++visited; k > 1; --k)
            levels[k] = union_ex(levels[k], intersection_ex(levels[k-1], mask));
        levels[1] = union_ex(levels[1], mask);
    }
    return levels[threshold];
}

inline void validate_cells(const Json& cells) {
    require(cells.is_array() && !cells.empty(), "Empty source face partition.");
    ExPolygons covered;
    double total = 0.;
    for (const auto& cell : cells) {
        const auto part = polygons(Json::array({cell}), true);
        require(SurfacePartition::area(diff_ex(part, root())) < area_tolerance, "Partition leaves its root.");
        require(SurfacePartition::area(intersection_ex(covered, part)) < area_tolerance, "Partition cells overlap.");
        covered = union_ex(covered, part);
        total += SurfacePartition::area(part);
    }
    require(std::abs(total - .5) < area_tolerance && SurfacePartition::area(diff_ex(root(), covered)) < area_tolerance,
            "Partition does not completely cover its root.");
}

inline Json partition_face(const Json& face, const Json& library = Json::object()) {
    const auto& base = face.at("base");
    validate_cells(base);
    ExPolygons remaining = root();
    Json cells = Json::array();
    std::set<std::string> sides, subjects, labels;
    const auto register_side = [&](const std::string& label) {
        if (label == "le" || label == "lb" || label.find("-le") != std::string::npos) sides.insert("left");
        if (label == "re" || label == "rb" || label.find("-re") != std::string::npos) sides.insert("right");
    };
    for (const auto& original : base) {
        const auto subject=original.at("subject_id").get<std::string>();
        require(!subject.empty(),"Unbound source parent cell.");
        subjects.insert(subject);
        register_side(original.value("fallback_label",original.at("label").get<std::string>()));
    }
    for (const auto& layer : face.at("layers")) {
        const auto label = layer.at("label").get<std::string>();
        const auto subject = layer.at("subject_id").get<std::string>();
        const auto parent = layer.at("parent_label").get<std::string>();
        require(labels.insert(label).second && !subject.empty(), "Duplicate or unbound detail.");
        const bool parent_repair=face.value("parent_repair",false);
        require((parent_repair && (label=="skin" || label=="hair" || label=="cloth") && label==parent) ||
                label == "le" || label == "re" || label == "lb" || label == "rb" || label == "ulip" ||
                label == "llip" || label == "imouth" || label == "iris-le" || label == "iris-re" ||
                label == "periocular-le" || label == "periocular-re", "Unsupported clipped detail.");
        subjects.insert(subject);
        register_side(label);
        require(subjects.size() == 1 && sides.size() <= 1, "Cross-subject or cross-eye partition.");
        auto mask = majority(layer.at("views"),library);
        if (layer.contains("parent_polygons")) mask = intersection_ex(mask,polygons(layer.at("parent_polygons"),true));
        if (layer.contains("envelope_views")) mask = intersection_ex(mask, majority(layer.at("envelope_views"),library));
        if (label.find("iris-") == 0) require(parent == label.substr(5), "Iris belongs to another eye.");
        if (parent_repair) for (const auto& other : face.at("layers")) {
            if (other.at("label")==label) continue;
            auto conflict=majority(other.at("views"),library);
            if (other.contains("parent_polygons"))
                conflict=intersection_ex(conflict,polygons(other.at("parent_polygons"),true));
            mask=diff_ex(mask,conflict);
        }
        auto selected = intersection_ex(remaining, mask);
        const auto append_selected = [&](const ExPolygons& parts, const Json* original) {
            for (const auto& polygon : parts) {
                auto cell = encode_polygon(polygon);
                cell.update({{"label", label}, {"parent_label", parent}, {"subject_id", subject},
                             {"kind", layer.value("kind", "CLIPPED_FEATURE")}, {"view_support", layer.at("views").size()}});
                if (original && original->contains("id")) cell["origin_cell_id"]=original->at("id");
                if (original && original->contains("source_leaf")) cell["source_leaf"]=original->at("source_leaf");
                cells.push_back(std::move(cell));
            }
        };
        if (parent_repair) {
            for (const auto& original : base)
                append_selected(intersection_ex(selected,polygons(Json::array({original}),true)),&original);
        } else append_selected(selected,nullptr);
        remaining = diff_ex(remaining, selected);
    }
    for (const auto& original : base) {
        const auto part = intersection_ex(remaining, polygons(Json::array({original}), true));
        for (const auto& polygon : part) {
            auto cell = original;
            cell.update(encode_polygon(polygon));
            if (face.value("preserve_color_provenance",false) && original.contains("id"))
                cell["origin_cell_id"]=original.at("id");
            if (face.value("redefine_rb",false) && original.at("label")=="rb") {
                cell["label"]="face"; cell["parent_label"]="face";
                cell["kind"]="R8_SOURCE_SKIN_RECLAIM";
            }
            cells.push_back(std::move(cell));
        }
    }
    validate_cells(cells);
    return cells;
}

inline size_t finish_cells(Json& cells, size_t id, const Json& identity) {
    size_t count = 0;
    for (auto& cell : cells) {
        cell["triangles"] = triangles(polygons(Json::array({cell}), true).front());
        count += cell["triangles"].size();
        auto stable = cell;
        for (const auto* field : {"id", "triangles", "view_support", "source_leaf", "kind"}) stable.erase(field);
        stable["geometry_id"] = identity.at("geometry_id"); stable["source_face_id"] = id;
        cell["id"] = beauty_leaf_digest(stable.dump());
    }
    std::sort(cells.begin(), cells.end(), [](const Json& a, const Json& b) { return a.at("id") < b.at("id"); });
    return count;
}

inline void conform_edges(Json& result, const Json& request) {
    using Edge = std::pair<size_t,size_t>;
    // Partition coordinates are serialized at `scale` precision. A contour
    // that mathematically lies on a source edge can therefore decode as one
    // integer unit away from zero. Treat that representation as the same edge
    // during seam propagation; otherwise only one side receives the split and
    // the final mesh contains a T-junction.
    constexpr coord_t edge_tolerance = 1;
    const auto on_edge = [edge_tolerance](coord_t value) { return std::abs(value) <= edge_tolerance; };
    std::map<size_t,std::array<size_t,3>> vertices;
    for (const auto& face : request.at("faces")) if (face.contains("source_vertices"))
        vertices.emplace(face.at("source_face_id"),face.at("source_vertices").get<std::array<size_t,3>>());
    if (vertices.empty()) return;
    std::map<Edge,std::set<coord_t>> cuts;
    const auto edge_info = [](const Point& p, size_t zero, const std::array<size_t,3>& ids) {
        const std::array<coord_t,3> bary {coord_t(scale)-p.x()-p.y(),p.x(),p.y()};
        const size_t a=(zero+1)%3,b=(zero+2)%3;
        return std::make_pair(Edge(std::min(ids[a],ids[b]),std::max(ids[a],ids[b])),
                              ids[a]<ids[b] ? bary[b] : bary[a]);
    };
    for (const auto& face : result.at("faces")) {
        const auto found=vertices.find(face.at("source_face_id"));
        require(found!=vertices.end(),"Missing source edge mapping.");
        for (const auto& cell : face.at("cells")) {
            const auto polygon=polygons(Json::array({cell}),true).front();
            auto collect=[&](const Polygon& contour) {
                for (const auto& p : contour.points) {
                    const std::array<coord_t,3> b {coord_t(scale)-p.x()-p.y(),p.x(),p.y()};
                    for (size_t i=0;i<3;++i) if (on_edge(b[i])) {
                        const auto e=edge_info(p,i,found->second); cuts[e.first].insert(e.second);
                    }
                }
            };
            collect(polygon.contour); for (const auto& h : polygon.holes) collect(h);
        }
    }
    size_t inserted=0;
    int64_t added=result.at("existing_added_triangles").get<int64_t>();
    for (auto& face : result["faces"]) {
        const auto& ids=vertices.at(face.at("source_face_id"));
        for (auto& cell : face["cells"]) {
            auto polygon=polygons(Json::array({cell}),true).front();
            auto expand=[&](Polygon& contour) {
                Points points;
                for (size_t index=0;index<contour.points.size();++index) {
                    const auto a=contour.points[index],b=contour.points[(index+1)%contour.points.size()];
                    points.push_back(a);
                    const std::array<coord_t,3> ba {coord_t(scale)-a.x()-a.y(),a.x(),a.y()};
                    const std::array<coord_t,3> bb {coord_t(scale)-b.x()-b.y(),b.x(),b.y()};
                    for (size_t zero=0;zero<3;++zero) if (on_edge(ba[zero]) && on_edge(bb[zero])) {
                        const auto ea=edge_info(a,zero,ids),eb=edge_info(b,zero,ids);
                        std::vector<coord_t> positions;
                        for (const auto t : cuts.at(ea.first)) if (t>std::min(ea.second,eb.second) && t<std::max(ea.second,eb.second)) positions.push_back(t);
                        if (ea.second>eb.second) std::reverse(positions.begin(),positions.end());
                        for (const auto t : positions) {
                            std::array<coord_t,3> weights {0,0,0};
                            const size_t j=(zero+1)%3,k=(zero+2)%3;
                            weights[j]=ids[j]<ids[k] ? coord_t(scale)-t : t;
                            weights[k]=coord_t(scale)-weights[j];
                            points.emplace_back(weights[1],weights[2]); ++inserted;
                        }
                    }
                }
                contour.points=std::move(points);
            };
            expand(polygon.contour); for (auto& h : polygon.holes) expand(h);
            if (request.contains("incremental_baseline") && !cell.contains("origin_cell_id") && cell.contains("id"))
                cell["origin_cell_id"]=cell.at("id");
            cell.update(encode_polygon(polygon));
        }
        face["triangle_count"]=finish_cells(face["cells"],face.at("source_face_id"),request.at("identity"));
        added+=face.at("triangle_count").get<int64_t>()-face.at("baseline_triangle_count").get<int64_t>();
    }
    require(added>=0,"Invalid cumulative seam budget.");
    result["added_triangles"]=added;
    result["seam_inserted_vertices"]=inserted;
}

inline Json cell_boundary(const Json& cell) {
    auto result = encode_polygon(polygons(Json::array({cell}),true).front());
    for (const auto* field : {"label","parent_label","subject_id"}) result[field]=cell.at(field);
    return result;
}

inline void restore_unchanged(Json& cells, const Json& base) {
    for (auto& cell : cells) {
        const auto geometry=cell_boundary(cell);
        for (const auto& old : base) if (old.contains("id") && cell_boundary(old)==geometry) {
            cell=old;
            break;
        }
    }
    std::sort(cells.begin(),cells.end(),[](const Json& a,const Json& b) { return a.at("id")<b.at("id"); });
}

inline bool preserves_frozen_cells(const Json& before, const Json& after, const Json& ids) {
    for (const auto* key : {"geometry_id","source_sha256","face_count"})
        if (before.at(key)!=after.at(key)) return false;
    std::map<std::string,std::pair<size_t,Json>> old_cells,new_cells;
    for (const auto& row : before.at("faces")) for (const auto& cell : row.at("cells"))
        old_cells.emplace(cell.at("id"),std::make_pair(row.at("source_face_id").get<size_t>(),cell_boundary(cell)));
    for (const auto& row : after.at("faces")) for (const auto& cell : row.at("cells"))
        new_cells.emplace(cell.at("id"),std::make_pair(row.at("source_face_id").get<size_t>(),cell_boundary(cell)));
    std::set<std::string> seen;
    for (const auto& id : ids) {
        const auto key=id.get<std::string>();
        if (!seen.insert(key).second || !old_cells.count(key) || !new_cells.count(key) ||
            old_cells.at(key)!=new_cells.at(key)) return false;
    }
    return true;
}

inline void validate(const Json& document, const Json& expected);

inline bool sample_in_contour(const Json& polygon, const Vec2d& point) {
    bool inside=false;
    constexpr double tolerance=2e-9;
    for (size_t i=0;i<polygon.size();++i) {
        const auto& va=polygon[i];
        const auto& vb=polygon[(i+1)%polygon.size()];
        const Vec2d a(va[1].get<double>(),va[2].get<double>());
        const Vec2d b(vb[1].get<double>(),vb[2].get<double>());
        const Vec2d edge=b-a, offset=point-a;
        const double length=edge.norm();
        if (length<1e-15) continue;
        const double cross=edge.x()*offset.y()-edge.y()*offset.x();
        const double dot=offset.dot(edge);
        if (std::abs(cross)<=tolerance*length && dot>=-tolerance*length &&
            dot<=length*length+tolerance*length) return true;
        if (std::abs(edge.y())>1e-15 && (a.y()>point.y())!=(b.y()>point.y()) &&
            point.x()<edge.x()*offset.y()/edge.y()+a.x()) inside=!inside;
    }
    return inside;
}

inline void validate_cell_samples(const Json& cells) {
    const std::array<std::array<double,3>,7> weights {{{1./3,1./3,1./3},
        {.98,.01,.01},{.01,.98,.01},{.01,.01,.98},{.49,.49,.02},{.02,.49,.49},{.49,.02,.49}}};
    // An absolute area check alone can miss an invalid, near-zero-width remainder.
    for (const auto& cell:cells) for (const auto& triangle:cell.at("triangles")) {
        for (const auto& w:weights) {
            Vec2d point=Vec2d::Zero();
            for (size_t i=0;i<3;++i)
                point+=w[i]*Vec2d(triangle[i][1].get<double>(),triangle[i][2].get<double>());
            require(sample_in_contour(cell.at("polygon"),point),"Tessellation samples leave their cell.");
            for (const auto& hole:cell.at("holes"))
                require(!sample_in_contour(hole,point),"Tessellation samples enter a cell hole.");
        }
    }
}

inline Json incremental_cut_cost(const Json& face, const Json& identity) {
    auto cells=partition_face(face,Json::object());
    finish_cells(cells,face.at("source_face_id"),identity);
    validate_cell_samples(cells);
    size_t triangles=0;
    for (const auto& cell:cells) triangles+=cell.at("triangles").size();
    const auto edge_points=[](const Json& rows) {
        std::set<std::pair<size_t,int64_t>> points;
        for (const auto& cell:rows) for (const auto& point:cell.at("polygon"))
            for (size_t edge=0;edge<3;++edge) if (std::abs(point[edge].get<double>())<1e-8)
                points.emplace(edge,int64_t(std::llround(point[(edge+1)%3].get<double>()*1000000000.)));
        return points;
    };
    const auto before=edge_points(face.at("base")),after=edge_points(cells);
    size_t splits=0;
    for (const auto& point:after) if (!before.count(point)) ++splits;
    const size_t baseline=face.at("baseline_triangle_count");
    const size_t local=triangles>baseline ? triangles-baseline : 0;
    // Neighbour conformity is reserved here and still checked by the final build.
    return {{"source_face_id",face.at("source_face_id")},{"local_added_triangles",local},
            {"source_edge_splits",splits},{"estimated_added_triangles",local+2*splits}};
}

inline Json build_incremental(const Json& request) {
    const auto& baseline=request.at("incremental_baseline");
    const auto& identity=request.at("identity");
    Json compatible=identity; compatible.erase("boundary_policy_sha256");
    validate(baseline,compatible);
    require(request.at("baseline_partition_sha256")==baseline.at("partition_sha256"),"Incremental baseline drift.");
    require(request.at("existing_added_triangles")==baseline.at("added_triangles"),"Incremental triangle baseline drift.");
    const size_t budget=request.at("triangle_budget").get<size_t>();
    const size_t face_count=identity.at("face_count").get<size_t>();
    const bool parent_repair=request.value("repair_scope",std::string())=="parent";
    require(budget==baseline.at("triangle_budget").get<size_t>(),"Incremental budget drift.");
    require(hash(identity.at("boundary_policy_sha256").get<std::string>()),"Invalid incremental policy identity.");
    std::map<size_t,Json> originals,requests;
    for (const auto& face : baseline.at("faces")) originals.emplace(face.at("source_face_id"),face);
    Json result=identity;
    result.update({{"schema","orca.surface-partition/v1"},{"faces",Json::array()},
        {"triangle_budget",budget},{"existing_added_triangles",baseline.at("added_triangles")},
        {"implicit_roots",baseline.at("implicit_roots")}});
    std::set<size_t> retained;
    std::set<size_t> frozen_seam_retained;
    for (const auto& face : request.at("faces")) {
        const size_t id=face.at("source_face_id").get<size_t>();
        require(id<identity.at("face_count").get<size_t>() && requests.emplace(id,face).second,"Invalid incremental root.");
        require(face.contains("source_vertices") && face.at("source_vertices").size()==3,"Missing incremental edge mapping.");
        require(!face.value("redefine_rb",false) || !face.at("layers").empty(),"Brow redefinition has no source witnesses.");
        if (originals.count(id)) {
            require(face.at("base")==originals.at(id).at("cells"),"Incremental base cells changed.");
            require(face.at("baseline_triangle_count")==originals.at(id).at("triangle_count"),"Incremental base triangles changed.");
        } else {
            require(face.at("baseline_triangle_count")==face.at("base").size(),"Invalid implicit root partition.");
            for (const auto& cell : face.at("base")) require(cell.at("label")=="R6","Implicit root is not passthrough.");
        }
        for (const auto& layer : face.at("layers")) {
            const auto label=layer.at("label").get<std::string>();
            require(parent_repair ? (face.value("parent_repair",false) &&
                (label=="skin" || label=="hair" || label=="cloth") && layer.at("parent_label")==label) :
                (label=="rb" && layer.at("parent_label")=="rb"),"Incremental repair crosses its detail scope.");
            Json allowed=Json::array();
            for (const auto& cell : face.at("base")) if ((!parent_repair && cell.at("label")=="rb") ||
                cell.at("label")=="face" || cell.at("label")=="R6" ||
                (parent_repair && (cell.at("label")=="skin" || cell.at("label")=="hair" || cell.at("label")=="cloth")))
                allowed.push_back(cell);
            require(layer.contains("parent_polygons") &&
                SurfacePartition::area(diff_ex(polygons(layer.at("parent_polygons"),true),polygons(allowed,true)))<area_tolerance,
                "Incremental repair overwrites a protected sibling.");
        }
        auto cells=face.at("layers").empty() ? face.at("base") : partition_face(face,request.value("curve_library",Json::object()));
        finish_cells(cells,id,identity);
        if (parent_repair && !face.at("layers").empty()) validate_cell_samples(cells);
        restore_unchanged(cells,face.at("base"));
        const auto original=originals.find(id);
        const std::string prefix=parent_repair ? "R9" : "R8";
        const std::string status=face.at("layers").empty() && original!=originals.end() ?
            original->second.at("status").get<std::string>() : face.at("layers").empty() ? prefix+"_SEAM_ONLY" : prefix+"_CONTOUR_CLIPPED";
        result["faces"].push_back({{"source_face_id",id},{"cells",cells},{"status",status},
            {"baseline_triangle_count",face.at("baseline_triangle_count")},{"reasons",face.value("reasons",Json::array())}});
    }
    for (const auto& [id,row] : originals) require(requests.count(id),"Incremental partition drops a baseline root.");
    const auto unconformed=result;
    while (true) {
        result=unconformed;
        for (auto& face : result["faces"]) if (retained.count(face.at("source_face_id"))) {
            face["cells"]=requests.at(face.at("source_face_id")).at("base");
            if(frozen_seam_retained.count(face.at("source_face_id"))) {
                face["status"]="R9_LOCAL_FROZEN_SEAM_FALLBACK";
                face["reasons"].push_back("R9_FROZEN_SEAM_RETAIN_BASELINE");
            } else {
                face["status"]=parent_repair ? "R9_LOCAL_BUDGET_FALLBACK" : "R8_LOCAL_BUDGET_FALLBACK";
                face["reasons"].push_back(parent_repair ? "R9_BUDGET_RETAIN_R8" : "R8_BUDGET_RETAIN_R7");
            }
        }
        conform_edges(result,request);
        for (auto& face : result["faces"]) {
            restore_unchanged(face["cells"],requests.at(face.at("source_face_id")).at("base"));
            size_t triangles=0;
            for (const auto& cell : face.at("cells")) triangles+=cell.at("triangles").size();
            face["triangle_count"]=triangles;
        }
        if(parent_repair && !preserves_frozen_cells(baseline,result,request.at("frozen_cell_ids"))) {
            // Only cuts which touch a changed frozen source seam are retained.
            // A conflict beside an eye must not discard safe arm/cloth repairs.
            std::map<std::string,std::pair<size_t,Json>> old_cells,new_cells;
            for(const auto& f:baseline.at("faces"))for(const auto& c:f.at("cells"))old_cells[c.at("id")]={f.at("source_face_id"),cell_boundary(c)};
            for(const auto& f:result.at("faces"))for(const auto& c:f.at("cells"))new_cells[c.at("id")]={f.at("source_face_id"),cell_boundary(c)};
            std::set<size_t> affected;
            for(const auto& id:request.at("frozen_cell_ids"))if(!new_cells.count(id) || old_cells.at(id)!=new_cells.at(id))affected.insert(old_cells.at(id).first);
            size_t added=0;
            for(const auto root:affected) {
                const auto& vertices=requests.at(root).at("source_vertices");
                for(const auto& [candidate,row]:requests)if(!row.at("layers").empty() && !retained.count(candidate)) {
                    size_t common=0;for(const auto& v:row.at("source_vertices"))if(std::find(vertices.begin(),vertices.end(),v)!=vertices.end())++common;
                    if(candidate==root || common>=2){retained.insert(candidate);frozen_seam_retained.insert(candidate);++added;}
                }
            }
            require(added>0,"Frozen detail changed without a local cut to retain.");
            continue;
        }
        require(preserves_frozen_cells(baseline,result,request.at("frozen_cell_ids")),"Frozen detail changed by incremental clipping.");
        if (result.at("added_triangles").get<size_t>()<=budget) break;
        size_t cost=0,selected=face_count;
        for (const auto& face : result.at("faces")) if (face.at("status")==
                (parent_repair ? "R9_CONTOUR_CLIPPED" : "R8_CONTOUR_CLIPPED")) {
            const auto current=face.at("triangle_count").get<size_t>();
            if (selected==face_count || current>cost) { cost=current; selected=face.at("source_face_id"); }
        }
        require(selected<face_count,"Incremental retained topology exceeds budget.");
        retained.insert(selected);
    }
    normalize_tessellation(result);
    result["partition_sha256"]=beauty_leaf_digest(result.dump());
    return result;
}

inline Json build(const Json& request) {
    require(request.at("schema") == "orca.surface-partition-request/v1", "Invalid clipping request.");
    if (request.contains("incremental_baseline")) return build_incremental(request);
    const auto& identity = request.at("identity");
    for (const auto* field : {"geometry_id", "source_sha256", "evidence_sha256", "runtime_sha256", "policy_sha256", "baseline_sha256", "boundary_policy_sha256"})
        require(hash(identity.at(field).get<std::string>()), "Invalid surface identity.");
    const size_t count = identity.at("face_count").get<size_t>();
    require(count > 0 && count <= 2000000, "Invalid source face count.");
    const size_t budget = request.at("triangle_budget").get<size_t>();
    size_t added = request.at("existing_added_triangles").get<size_t>();
    require(budget <= std::min(size_t(20000), count*2/100) && added <= budget, "Invalid partition budget.");
    Json result = identity;
    result.update({{"schema", "orca.surface-partition/v1"}, {"faces", Json::array()},
                   {"triangle_budget", budget}, {"existing_added_triangles", added}, {"implicit_roots", "R6_PASSTHROUGH"}});
    std::set<size_t> seen;
    for (const auto& face : request.at("faces")) {
        const size_t id = face.at("source_face_id").get<size_t>();
        require(id < count && seen.insert(id).second, "Duplicate or out of range source face.");
        auto cells = partition_face(face,request.value("curve_library",Json::object()));
        const size_t old_triangles = face.at("baseline_triangle_count").get<size_t>();
        size_t new_triangles = finish_cells(cells,id,identity);
        const size_t increase = new_triangles > old_triangles ? new_triangles - old_triangles : 0;
        std::string status = face.at("layers").empty() ? "R6_LOCAL_EVIDENCE_FALLBACK" : "CONTOUR_CLIPPED";
        if (increase > budget - added) {
            cells = face.at("base");
            for (auto& cell : cells) {
                if (cell.contains("fallback_label")) { cell["label"] = cell["fallback_label"]; cell.erase("fallback_label"); }
                cell["kind"] = "R6_RETAINED";
            }
            new_triangles = finish_cells(cells,id,identity); status = "R6_LOCAL_BUDGET_FALLBACK";
        } else added += increase;
        result["faces"].push_back({{"source_face_id", id}, {"status", status}, {"cells", cells},
                                   {"baseline_triangle_count", old_triangles}, {"triangle_count", new_triangles},
                                   {"reasons", face.value("reasons", Json::array())}});
    }
    result["added_triangles"] = added;
    // Include conformity triangles in the same budget. If sharing source-edge
    // cuts exceeds it, retain the most costly local proposal and retry from
    // the unconformed draft, rather than retaining cuts from a rejected one.
    const auto unconformed=result;
    std::set<size_t> seam_fallbacks;
    while (true) {
        result=unconformed;
        for (auto& face : result["faces"]) if (seam_fallbacks.count(face.at("source_face_id"))) {
            const auto original=std::find_if(request.at("faces").begin(),request.at("faces").end(),[&](const Json& row) {
                return row.at("source_face_id")==face.at("source_face_id");
            });
            face["cells"]=original->at("base");
            for (auto& cell : face["cells"]) {
                if (cell.contains("fallback_label")) { cell["label"]=cell["fallback_label"]; cell.erase("fallback_label"); }
                cell["kind"]="R6_RETAINED";
            }
            face["triangle_count"]=finish_cells(face["cells"],face.at("source_face_id"),identity);
            face["status"]="R6_LOCAL_SEAM_BUDGET_FALLBACK";
            face["reasons"].push_back("SEAM_CONFORMITY_BUDGET_EXHAUSTED");
        }
        conform_edges(result,request);
        if (result.at("added_triangles").get<size_t>()<=budget) break;
        size_t most_costly=0,selected=count;
        for (const auto& face : result.at("faces")) if (face.at("status")=="CONTOUR_CLIPPED") {
            const size_t cost=face.at("triangle_count").get<size_t>();
            if (selected==count || cost>most_costly) { most_costly=cost; selected=face.at("source_face_id"); }
        }
        require(selected<count,"Retained baseline seam topology exceeds the budget.");
        seam_fallbacks.insert(selected);
    }
    normalize_tessellation(result);
    result["partition_sha256"] = beauty_leaf_digest(result.dump());
    return result;
}

inline void validate(const Json& document, const Json& expected) {
    require(document.at("schema") == "orca.surface-partition/v1", "Unsupported surface partition.");
    for (auto it = expected.begin(); it != expected.end(); ++it)
        require(document.contains(it.key()) && document.at(it.key()) == it.value(), "Surface partition identity drift.");
    auto unhashed = document; unhashed.erase("partition_sha256");
    require(document.at("partition_sha256") == beauty_leaf_digest(unhashed.dump()), "Surface partition content drift.");
    require(document.at("added_triangles").get<size_t>() <= document.at("triangle_budget").get<size_t>(), "Surface budget drift.");
    const int version=document.value("tessellation_version",0);
    require(version==0 || version==1,"Unsupported surface tessellation version.");
    std::set<size_t> faces;
    std::set<std::string> ids;
    int64_t computed_added=document.at("existing_added_triangles").get<int64_t>();
    for (const auto& face : document.at("faces")) {
        const size_t id = face.at("source_face_id").get<size_t>();
        require(id < document.at("face_count").get<size_t>() && faces.insert(id).second, "Invalid source face mapping.");
        validate_cells(face.at("cells"));
        size_t triangle_count=0;
        for (const auto& cell : face.at("cells")) {
            require(ids.insert(cell.at("id").get<std::string>()).second, "Duplicate stable cell ID.");
            const auto polygon=polygons(Json::array({cell}), true).front();
            require(cell.at("triangles") == (version ? triangles(polygon) : legacy_triangles(polygon)), "Cell triangles drift.");
            triangle_count+=cell.at("triangles").size();
        }
        require(face.at("triangle_count")==triangle_count,"Stored face triangle count drift.");
        computed_added+=int64_t(triangle_count)-face.at("baseline_triangle_count").get<int64_t>();
    }
    require(computed_added>=0 && document.at("added_triangles")==computed_added,"Cumulative surface triangle budget drift.");
}

// Compatibility is checked before rebuilding. Cell identity describes the
// contour, never its disposable triangulation; old addressed data stays intact.
inline Json rebuild_tessellation(const Json& document,const std::function<bool()>& canceled={},
                                 const std::function<void(size_t,size_t)>& progress={}) {
    validate(document,Json::object());
    if(document.value("tessellation_version",0)==1) return document;
    Json result=document;
    int64_t added=document.at("added_triangles");
    size_t done=0;
    for(auto& face:result["faces"]) {
        if(canceled && canceled()) throw std::runtime_error("Contour mesh rebuild cancelled.");
        size_t count=0;
        for(auto& cell:face["cells"]) {
            try {
                const auto old_count=cell.at("triangles").size();
                cell["triangles"]=triangles(polygons(Json::array({cell}),true).front());
                count+=cell["triangles"].size();
                added+=int64_t(cell["triangles"].size())-int64_t(old_count);
            } catch(const std::exception& e) {
                throw std::invalid_argument("Contour rebuild face="+face.at("source_face_id").dump()+
                    " cell="+cell.at("id").get<std::string>()+": "+e.what());
            }
        }
        face["triangle_count"]=count;
        if(progress) progress(++done,result["faces"].size());
    }
    require(added>=0 && size_t(added)<=result.at("triangle_budget").get<size_t>(),"Rebuilt contour exceeds its cumulative budget.");
    result["added_triangles"]=added;
    result["tessellation_version"]=1;
    result.erase("partition_sha256");result["partition_sha256"]=beauty_leaf_digest(result.dump());
    validate(result,Json::object());
    return result;
}
} // namespace Slic3r::AI::SurfacePartition
