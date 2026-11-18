#include "../model_internal.hpp"
#include "libslic3r/MeshRepair.hpp"
#include <chrono>
#include <ctime>
#include "libslic3r/AABBTreeIndirect.hpp"

namespace orca_model_detail {
J enhanced_glb_geometry(Context& c, std::vector<V>& vertices, std::vector<T>& faces, const std::function<void(int)>& report)
{
    const auto started = std::chrono::steady_clock::now();
    const auto cpu = std::clock();
    indexed_triangle_set original;
    original.vertices.reserve(vertices.size());
    original.indices.reserve(faces.size());
    for (const auto& v : vertices) {
        // GLTF Y-up metres -> desktop Z-up millimetres, once at the boundary.
        Slic3r::Vec3f point(float(v[0] * 1000), float(-v[2] * 1000), float(v[1] * 1000));
        if (!point.allFinite()) throw Error("INVALID_GLB", "Non-finite Core coordinate");
        original.vertices.push_back(point);
    }
    for (const auto& f : faces) original.indices.emplace_back(f[0], f[1], f[2]);
    const auto source_id = Slic3r::mesh_repair_geometry_id(original);
    Slic3r::TriangleMesh mesh(std::move(original));
    Slic3r::MeshRepairStats stats;
    std::string error;
    const bool success = Slic3r::repair_mesh(mesh, &stats, &error, [&](int progress) {
        check_cancel(c);
        report(progress);
        check_cancel(c);
    });
    check_cancel(c);
    if (!success) throw Error("REPAIR_FAILED", "Desktop enhanced repair did not produce a solid");
    const auto output_id = Slic3r::mesh_repair_geometry_id(mesh.its);
    if (output_id.empty() || mesh.its.indices.empty()) throw Error("REPAIR_FAILED", "Invalid enhanced output");
    vertices.clear(); faces.clear();
    vertices.reserve(mesh.its.vertices.size()); faces.reserve(mesh.its.indices.size());
    // Keep actual Core float values without another metre roundtrip. The carrier
    // must express the display unit/up-axis transform in a canonical root node.
    for (const auto& v : mesh.its.vertices) vertices.push_back({v.x(), v.y(), v.z()});
    for (const auto& f : mesh.its.indices) faces.push_back({uint32_t(f[0]), uint32_t(f[1]), uint32_t(f[2])});
    return {{"repairEntry", "repair_mesh"}, {"coreSourceCommit", "323c6c1c9ca7e5f94c7b7161af64584af0a69f3c"},
            {"sourceGeometryId", source_id}, {"outputGeometryId", output_id}, {"nativeRepairSucceeded", true},
            {"modified", stats.modified}, {"outputVertices", vertices.size()}, {"outputFaces", faces.size()},
            {"wallMs", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count()},
            {"cpuMs", 1000.0*(std::clock()-cpu)/CLOCKS_PER_SEC}, {"strictAiChainPerformed", false}};
}
std::string enhanced_core_id(const std::vector<V>& vertices, const std::vector<T>& faces)
{
    indexed_triangle_set mesh;
    for (const auto& v : vertices) mesh.vertices.emplace_back(float(v[0]),float(v[1]),float(v[2]));
    for (const auto& f : faces) mesh.indices.emplace_back(f[0],f[1],f[2]);
    return Slic3r::mesh_repair_geometry_id(mesh);
}
// Spatial projection uses the existing Core AABB tree, never an R x F scan.
void enhanced_corner_sources(Context& c, const std::vector<V>& input,
 const std::vector<T>& input_faces, const std::vector<V>& output, const std::vector<T>& output_faces, const std::function<void(size_t,const std::array<double,11>&)>& emit, const std::function<void(uint64_t)>& admit_workspace)
{
    const uint64_t base_workspace=input.size()*sizeof(Slic3r::Vec3f)+input_faces.size()*(sizeof(Slic3r::Vec3i32)+sizeof(std::pair<std::array<float,9>,size_t>));
    admit_workspace(base_workspace);
    std::vector<Slic3r::Vec3f> vertices; std::vector<Slic3r::Vec3i32> faces;
    vertices.reserve(input.size()); faces.reserve(input_faces.size());
    for (const auto& v : input) vertices.emplace_back(float(v[0]*1000),float(-v[2]*1000),float(v[1]*1000));
    for (const auto& f : input_faces) faces.emplace_back(f[0],f[1],f[2]);
    using Key=std::array<float,9>;
    auto key=[](const std::array<Slic3r::Vec3f,3>& points) { auto q=points;
        std::sort(q.begin(),q.end(),[](const auto& a,const auto& b){return std::array<float,3>{a.x(),a.y(),a.z()}<std::array<float,3>{b.x(),b.y(),b.z()};});
        Key k{};for(size_t i=0;i<3;++i)for(size_t j=0;j<3;++j)k[i*3+j]=q[i][j];return k; };
    std::vector<std::pair<Key,size_t>> exact;exact.reserve(faces.size());
    for(size_t i=0;i<faces.size();++i)exact.push_back({key({vertices[faces[i][0]],vertices[faces[i][1]],vertices[faces[i][2]]}),i});
    std::sort(exact.begin(),exact.end());
    Slic3r::AABBTreeIndirect::Tree<3,float> tree;
    for (size_t i=0;i<output_faces.size();++i) {
        if ((i&1023)==0) check_cancel(c);
        const auto& f=output_faces[i];
        Slic3r::Vec3d center=Slic3r::Vec3d::Zero();
        for (auto id:f) { const auto& v=output[id]; center+=Slic3r::Vec3d(v[0],v[1],v[2])/3.; }
        size_t hit=0; Slic3r::Vec3d point;
        std::array<Slic3r::Vec3f,3> output_points;
        for(size_t j=0;j<3;++j) { const auto& v=output[f[j]]; output_points[j]=Slic3r::Vec3f(float(v[0]),float(v[1]),float(v[2])); }
        const auto wanted=key(output_points);auto found=std::lower_bound(exact.begin(),exact.end(),std::make_pair(wanted,size_t(0)));
        double distance=0;
        if(found!=exact.end()&&found->first==wanted) hit=found->second;
        else { if(tree.empty()) {admit_workspace(base_workspace+uint64_t(faces.size())*96);tree=Slic3r::AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(vertices,faces);}
          distance=Slic3r::AABBTreeIndirect::squared_distance_to_indexed_triangle_set(vertices,faces,tree,center,hit,point); }
        if (!std::isfinite(distance)||distance<0) throw Error("APPEARANCE_UNSUPPORTED","Cannot project appearance");
        const auto& sf=faces.at(hit);
        auto a=vertices[sf[0]].cast<double>().eval(), b=vertices[sf[1]].cast<double>().eval(), d=vertices[sf[2]].cast<double>().eval();
        auto u=(b-a).eval(),v=(d-a).eval();
        double uu=u.dot(u),uv=u.dot(v),vv=v.dot(v),den=uu*vv-uv*uv;
        if (!(den>0)) throw Error("APPEARANCE_UNSUPPORTED","Degenerate appearance source");
        std::array<double,11> map{};map[0]=double(hit);
        for(size_t k=0;k<3;++k) {
            const auto& x=output[f[k]]; Slic3r::Vec3d q(x[0],x[1],x[2]);
            bool exact_corner=false;
            for(size_t n=0;n<3;++n)if(q==vertices[sf[int(n)]].cast<double>()) {
                map[1+k*3+n]=1.;exact_corner=true;break;
            }
            if(exact_corner)continue;
            const auto closest=Slic3r::AABBTreeIndirect::detail::closest_point_to_triangle<Slic3r::Vec3d>(q,a,b,d);
            map[10]=std::max(map[10],(q-closest).norm());
            auto w=(closest-a).eval();
            double y=(vv*w.dot(u)-uv*w.dot(v))/den,z=(uu*w.dot(v)-uv*w.dot(u))/den;
            map[1+k*3]=1-y-z;map[2+k*3]=y;map[3+k*3]=z;
        }
        emit(i,map);
    }
}

}
