#pragma once
#include "libslic3r/Point.hpp"
#include <algorithm>
#include <cmath>

namespace Slic3r::AI {
// Compact, invertible screen-space deformation shared by live boundary feedback
// and surface classification on release. Its maximum displacement gradient is
// below one, so large drags stay continuous instead of folding the region.
struct BeautyBoundaryDrag {
    Vec2d anchor, delta;
    double radius;
    int handle {0}; // 1: move; 2/3: width; 4/5: height.
    Vec2d center {Vec2d::Zero()}, half {Vec2d::Ones()}, scale {Vec2d::Ones()};
    BeautyBoundaryDrag(Vec2d from, Vec2d to, double minimum_radius)
        : anchor(from), delta(to-anchor),
          radius(std::max({1.0,minimum_radius,2.5*delta.norm()})) {}
    static BeautyBoundaryDrag shape(const Vec2d& low,const Vec2d& high,int control,const Vec2d& movement) {
        BeautyBoundaryDrag drag(Vec2d::Zero(),movement,1.);
        drag.handle=control;drag.center=(low+high)*.5;drag.half=(high-low).cwiseMax(2.)*.5;
        if(control==1) {
            // Keep overlap with the original piece, so visibility sampling and
            // the surface transaction cannot teleport it through another part.
            const double extent=drag.delta.cwiseQuotient(drag.half).norm();
            if(extent>1.)drag.delta/=extent;
        } else {
            const int axis=control<=3?0:1;
            const double sign=(control==2 || control==4)?-1.:1.;
            drag.scale[axis]=std::clamp(1.+sign*movement[axis]/drag.half[axis],.25,3.);
            drag.delta.setZero();
        }
        return drag;
    }
    double weight(const Vec2d& point) const {
        const double t=std::max(0.0,1.0-(point-anchor).squaredNorm()/(radius*radius));
        return t*t;
    }
    Vec2d forward(const Vec2d& point) const {
        if(handle)return center+(point-center).cwiseProduct(scale)+delta;
        return point+delta*weight(point);
    }
    Vec2d inverse(const Vec2d& point) const {
        if(handle)return center+(point-center-delta).cwiseQuotient(scale);
        Vec2d source=point;
        for(int i=0;i<40;++i) {
            const Vec2d next=point-delta*weight(source);
            if((next-source).squaredNorm()<1e-10)return next;
            source=next;
        }
        return source;
    }
    bool affects(const Vec2d& point) const {
        if(handle)return ((point-center).cwiseAbs().array()<=(half.array()+2.)).all() ||
            ((point-center-delta).cwiseAbs().array()<=(half.cwiseProduct(scale).array()+2.)).all();
        return (point-anchor).norm()<=radius+delta.norm();
    }
    bool ellipse_contains(const Vec2d& point) const {
        return handle && (inverse(point)-center).cwiseQuotient(half).squaredNorm()<=1.;
    }
};
}
