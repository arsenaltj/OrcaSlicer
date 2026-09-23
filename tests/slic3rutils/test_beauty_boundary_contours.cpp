#include <catch2/catch_all.hpp>
#include "slic3r/GUI/AI/Model/BeautyBoundaryContours.hpp"
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
