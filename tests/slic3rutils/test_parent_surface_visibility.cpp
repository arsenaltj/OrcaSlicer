#include "slic3r/GUI/AI/Model/ParentSurfaceVisibility.hpp"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("Subpixel parent visibility tests the bound surface against real occluders", "[ParentSurfaceVisibility][PortraitSurfaceOwnership]") {
    indexed_triangle_set mesh;
    mesh.vertices={{0,0,0},{.0001f,0,0},{0,.0001f,0},
                   {-1,-1,1},{1,-1,1},{0,1,1}};
    mesh.indices={{0,1,2},{3,4,5}};
    const Slic3r::AI::ParentSurfaceVisibility indexed(mesh);
    REQUIRE(Slic3r::AI::parent_visible_samples(indexed,0,Slic3r::Vec3d(0,0,1),10.)==0);
    REQUIRE(Slic3r::AI::parent_visible_samples(indexed,0,Slic3r::Vec3d(0,0,-1),10.)==127);
    REQUIRE(Slic3r::AI::parent_visible_samples(indexed,1,Slic3r::Vec3d(0,0,1),10.)==127);
}

TEST_CASE("A visible parent centroid does not grant visibility to a hidden child", "[ParentSurfaceVisibility][PortraitSurfaceOwnership]") {
    indexed_triangle_set mesh;
    mesh.vertices={{0,0,0},{3,0,0},{0,3,0},{-.1f,-.1f,1},{1,-.1f,1},{-.1f,1,1}};
    mesh.indices={{0,1,2},{3,4,5}};
    const Slic3r::AI::ParentSurfaceVisibility indexed(mesh);
    const auto root=Slic3r::AI::parent_visible_samples(indexed,0,Slic3r::Vec3d(0,0,1),10.);
    REQUIRE((root & 1u)==1u);
    auto child=Slic3r::AI::parent_visibility_samples;
    for (auto& weights:child) weights={{.95,.025,.025}};
    REQUIRE(Slic3r::AI::parent_visible_samples(indexed,0,Slic3r::Vec3d(0,0,1),10.,child)==0);
}
