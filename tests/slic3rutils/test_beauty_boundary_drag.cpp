#include <catch2/catch_all.hpp>
#include "slic3r/GUI/AI/Model/BeautyBoundaryDrag.hpp"
using namespace Slic3r;
using namespace Slic3r::AI;

TEST_CASE("Eye handles transform a bounded ellipse instead of retaining contour corners", "[BeautyBoundaryDrag]") {
    const auto wide=BeautyBoundaryDrag::shape({20,30},{40,50},3,{5,0});
    CHECK(wide.ellipse_contains({30,40}));
    CHECK(wide.ellipse_contains({44,40}));
    CHECK_FALSE(wide.ellipse_contains({44,49}));
    CHECK_FALSE(wide.ellipse_contains({30,51}));
    const auto moved=BeautyBoundaryDrag::shape({20,30},{40,50},1,{3,4});
    CHECK(moved.ellipse_contains({33,44}));
    CHECK_FALSE(moved.ellipse_contains({20,30}));
}

TEST_CASE("Small region handles resize one axis and keep the opposite axis and center stable", "[BeautyWorkbench][BeautyBoundaryDrag]") {
    const Vec2d low(20,30),high(40,42),center=(low+high)*.5;
    for(int handle=2;handle<=5;++handle) {
        const auto drag=BeautyBoundaryDrag::shape(low,high,handle,{5,4});
        CHECK((drag.forward(center)-center).norm()<1e-9);
        const int stable=handle<=3?1:0;
        for(const Vec2d point:{low,high,center,Vec2d(24,38)}) {
            CHECK(drag.forward(point)[stable]==point[stable]);
            CHECK((drag.inverse(drag.forward(point))-point).norm()<1e-9);
            CHECK(drag.affects(drag.forward(point)));
        }
        const auto collapsed=BeautyBoundaryDrag::shape(low,high,handle,{-1000,1000});
        CHECK(collapsed.scale.minCoeff()>=.25);
        CHECK(collapsed.scale.maxCoeff()<=3.);
    }
    const auto moved=BeautyBoundaryDrag::shape(low,high,1,{3,2});
    CHECK((moved.forward(low)-low-Vec2d(3,2)).norm()<1e-9);
    CHECK((moved.forward(high)-high-Vec2d(3,2)).norm()<1e-9);
    const auto far=BeautyBoundaryDrag::shape(low,high,1,{1000,1000});
    CHECK(far.delta.cwiseQuotient(far.half).norm()<=1.+1e-9);
    CHECK_FALSE(far.affects({10000,10000}));
}

TEST_CASE("Boundary dragging follows the pointer continuously without folding or changing distant points", "[BeautyWorkbench][BeautyBoundaryDrag]") {
    const double distance=GENERATE(-240.,-20.,0.,20.,240.);
    const BeautyBoundaryDrag drag({100.,100.},{100.+distance,100.},44.);
    CHECK((drag.forward(drag.anchor)-(drag.anchor+drag.delta)).norm()<1e-9);
    const Vec2d far=drag.anchor+Vec2d(drag.radius*2,0);
    CHECK((drag.forward(far)-far).norm()<1e-9);
    double previous=-1e20;
    for(int x=-800;x<=1000;x+=3) {
        const Vec2d point(double(x),110.);
        const Vec2d moved=drag.forward(point);
        CHECK(moved.x()>previous);
        CHECK((drag.inverse(moved)-point).norm()<1e-4);
        previous=moved.x();
    }
}

TEST_CASE("Boundary dragging preserves a smooth area instead of leaving a narrow painted trail", "[BeautyWorkbench][BeautyBoundaryDrag]") {
    const BeautyBoundaryDrag drag({0.,0.},{24.,0.},50.);
    for(int y=-30;y<=30;++y) {
        const Vec2d edge=drag.forward({0.,double(y)});
        CHECK(edge.x()>0);
        CHECK(edge.x()<=24.);
        CHECK(drag.inverse(edge-Vec2d(.25,0)).x()<0.);
        CHECK(drag.inverse(edge+Vec2d(.25,0)).x()>0.);
    }
    CHECK(std::abs(drag.forward({0.,-20.}).x()-drag.forward({0.,20.}).x())<1e-9);
}
