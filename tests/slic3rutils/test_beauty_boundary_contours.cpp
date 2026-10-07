#include <catch2/catch_all.hpp>
#include "slic3r/GUI/AI/Model/BeautyBoundaryContours.hpp"
#include <atomic>
#include <cstring>
#include <thread>
using namespace Slic3r;
using namespace Slic3r::AI;

TEST_CASE("Boundary contours reduce staircase turning while keeping junctions and bounded displacement", "[BeautyWorkbench][BeautyBoundaryContours]") {
    std::vector<Vec3f> points;
    for(int i=0;i<30;++i)points.emplace_back(float(i),float(i%2)*.7f,0.f);
    auto roughness=[](const auto& path){double sum=0;for(size_t i=1;i+1<path.size();++i)
        sum+=(path[i-1]-2*path[i]+path[i+1]).norm();return sum;};
    const auto smooth=BeautyBoundaryContours::fair(points,false);
    CHECK((smooth.front()-points.front()).norm()<1e-6);
    CHECK((smooth.back()-points.back()).norm()<1e-6);
    CHECK(roughness(smooth)<roughness(points)*.25);
    for(size_t i=0;i<points.size();++i){CHECK((smooth[i]-points[i]).norm()<1.f);CHECK(std::abs(smooth[i].z())<1e-6);}
}

TEST_CASE("Contour construction keeps shared junctions fixed and projects curves onto the surface", "[BeautyWorkbench][BeautyBoundaryContours]") {
    indexed_triangle_set mesh;mesh.vertices={{-10,-10,0},{10,-10,0},{0,10,0}};mesh.indices={{0,1,2}};
    BeautyBoundaryContours::Neighbors neighbors(1,{-1,-1,-1});
    std::vector<BeautyBoundaryContours::Edge> edges={
        {{-3,0,0},{-2,.5f,0},0,0,1,2},{{-2,.5f,0},{-1,-.4f,0},0,0,1,2},{{-1,-.4f,0},{0,0,0},0,0,1,2},
        {{0,0,0},{2,1,0},0,0,1,3},{{0,0,0},{1,-2,0},0,0,2,3}};
    const auto saved=mesh.vertices;
    const auto curves=BeautyBoundaryContours::build(edges,mesh,neighbors);
    // Derived beauty surfaces can have more than three adjacent faces per row.
    // The native canonical topology and the adaptive topology use the same
    // projection and preserve junctions without copying either adjacency table.
    const std::vector<std::vector<int32_t>> adaptive_neighbors{{-1,-1,-1,-1}};
    const auto adaptive=BeautyBoundaryContours::build(edges,mesh,adaptive_neighbors);
    REQUIRE(adaptive.size()==curves.size());
    for(size_t i=0;i<curves.size();++i) {
        CHECK((adaptive[i].a-curves[i].a).norm()<1e-6);
        CHECK((adaptive[i].b-curves[i].b).norm()<1e-6);
    }
    REQUIRE(curves.size()>edges.size());
    size_t junctions=0;
    for(const auto& edge:curves){
        CHECK(std::abs(edge.a.z())<1e-6);CHECK(std::abs(edge.b.z())<1e-6);
        if(edge.a.norm()<1e-6)++junctions;if(edge.b.norm()<1e-6)++junctions;
        CHECK(edge.left<edge.right);
    }
    CHECK(junctions==3);
    for(size_t i=0;i<saved.size();++i)CHECK((saved[i]-mesh.vertices[i]).norm()<1e-6);
}

TEST_CASE("Closed boundary curves have no gaps or artificial open endpoints", "[BeautyWorkbench][BeautyBoundaryContours]") {
    indexed_triangle_set mesh;mesh.vertices={{-10,-10,0},{10,-10,0},{0,10,0}};mesh.indices={{0,1,2}};
    BeautyBoundaryContours::Neighbors neighbors(1,{-1,-1,-1});
    std::vector<BeautyBoundaryContours::Edge> edges={
        {{-1,-1,0},{1,-1,0},0,0,1,2},{{1,-1,0},{1,1,0},0,0,1,2},
        {{1,1,0},{-1,1,0},0,0,1,2},{{-1,1,0},{-1,-1,0},0,0,1,2}};
    const auto curves=BeautyBoundaryContours::build(edges,mesh,neighbors);REQUIRE(curves.size()>4);
    for(size_t i=0;i<curves.size();++i)CHECK((curves[i].b-curves[(i+1)%curves.size()].a).norm()<1e-5);
    CHECK((curves.front().a-curves.front().b).norm()>1e-4);
    double area=0;for(const auto& edge:curves)area+=edge.a.x()*edge.b.y()-edge.b.x()*edge.a.y();
    CHECK(std::abs(area)*.5>2.7);
}

namespace {
std::string contour_bytes(const std::vector<BeautyBoundaryContours::Segment>& segments) {
    std::string bytes;
    const auto append=[&](const auto& value) {
        bytes.append(reinterpret_cast<const char*>(&value),sizeof(value));
    };
    for(const auto& segment:segments) {
        for(int axis=0;axis<3;++axis) {append(segment.a[axis]);append(segment.b[axis]);}
        append(segment.face);append(segment.neighbor);append(segment.left);append(segment.right);append(segment.tolerance);
    }
    return bytes;
}
}

TEST_CASE("Pre-cancelled contour work stops before accessing empty geometry", "[BeautyWorkbench][BeautyBoundaryContours]") {
    const indexed_triangle_set mesh;
    const BeautyBoundaryContours::Neighbors neighbors;
    const std::vector<BeautyBoundaryContours::Edge> edges;
    const std::vector<Vec3f> points;
    const std::function<bool()> canceled=[] {return true;};
    REQUIRE_THROWS_AS(BeautyBoundaryContours::build(edges,mesh,neighbors,canceled),std::runtime_error);
    REQUIRE_THROWS_AS(BeautyBoundaryContours::fair(points,false,canceled),std::runtime_error);
    CHECK(BeautyBoundaryContours::build(edges,mesh,neighbors).empty());
    CHECK(BeautyBoundaryContours::fair(points,false).empty());
}

TEST_CASE("A long contour cancels on a worker and can be retried without changing its input", "[BeautyWorkbench][BeautyBoundaryContours]") {
    indexed_triangle_set mesh;
    mesh.vertices={{-10000,-10000,0},{30000,-10000,0},{-10000,30000,0}};
    mesh.indices={{0,1,2}};
    const BeautyBoundaryContours::Neighbors neighbors(1,{-1,-1,-1});
    std::vector<BeautyBoundaryContours::Edge> edges;
    for(size_t i=0;i<10000;++i) {
        edges.push_back({{float(i),float(i%2)*.2f,0},{float(i+1),float((i+1)%2)*.2f,0},0,-1,1,2});
    }
    const auto original=edges;
    const auto original_vertices=mesh.vertices;
    std::atomic<unsigned> checks{0};
    bool completed=false;
    std::exception_ptr failure;
    std::thread worker([&] {
        try {BeautyBoundaryContours::build(edges,mesh,neighbors,[&] {return ++checks>=3;});completed=true;}
        catch(...) {failure=std::current_exception();}
    });
    worker.join();
    REQUIRE(failure);
    REQUIRE_THROWS_AS(std::rethrow_exception(failure),std::runtime_error);
    CHECK_FALSE(completed);
    CHECK(checks.load()>1);
    REQUIRE(edges.size()==original.size());
    bool unchanged=true;
    for(size_t i=0;i<edges.size();++i) {
        if(std::memcmp(edges[i].a.data(),original[i].a.data(),3*sizeof(float))!=0)unchanged=false;
        if(std::memcmp(edges[i].b.data(),original[i].b.data(),3*sizeof(float))!=0)unchanged=false;
        if(edges[i].face!=original[i].face || edges[i].neighbor!=original[i].neighbor ||
           edges[i].left!=original[i].left || edges[i].right!=original[i].right)unchanged=false;
    }
    for(size_t i=0;i<mesh.vertices.size();++i)
        if(std::memcmp(mesh.vertices[i].data(),original_vertices[i].data(),3*sizeof(float))!=0)unchanged=false;
    CHECK(unchanged);
    const auto baseline=BeautyBoundaryContours::build(edges,mesh,neighbors);
    const auto retry=BeautyBoundaryContours::build(edges,mesh,neighbors,[] {return false;});
    REQUIRE_FALSE(baseline.empty());
    CHECK(contour_bytes(retry)==contour_bytes(baseline));
}

TEST_CASE("Long boundary smoothing cancels on a worker without changing its source points", "[BeautyWorkbench][BeautyBoundaryContours]") {
    std::vector<Vec3f> points;
    for(size_t i=0;i<10000;++i)points.emplace_back(float(i),float(i%2)*.2f,0);
    const auto original=points;
    std::atomic<unsigned> checks{0};
    std::exception_ptr failure;
    std::thread worker([&] {
        try {BeautyBoundaryContours::fair(points,false,[&] {return ++checks>=4;});}
        catch(...) {failure=std::current_exception();}
    });
    worker.join();
    REQUIRE(failure);
    REQUIRE_THROWS_AS(std::rethrow_exception(failure),std::runtime_error);
    CHECK(checks.load()>1);
    bool unchanged=true;
    for(size_t i=0;i<points.size();++i)
        if(std::memcmp(points[i].data(),original[i].data(),3*sizeof(float))!=0)unchanged=false;
    CHECK(unchanged);
    const auto baseline=BeautyBoundaryContours::fair(points,false);
    const auto retry=BeautyBoundaryContours::fair(points,false,[] {return false;});
    bool identical=baseline.size()==retry.size();
    for(size_t i=0;i<baseline.size() && identical;++i)
        if(std::memcmp(baseline[i].data(),retry[i].data(),3*sizeof(float))!=0)identical=false;
    CHECK(identical);
}
