#pragma once

#include "libslic3r/Geometry.hpp"
#include <algorithm>
#include <cmath>
#include <optional>

namespace Slic3r::GUI {

// Ordinary previews retain their established yaw/pitch orbit. A navigator can
// supply a complete orientation (including the bottom view) without losing it
// when the user next orbits, compares appearances, or restores an edit snapshot.
struct ModelPreviewCameraRotation {
    double yaw {-0.65};
    double pitch {-1.05};
    std::optional<Eigen::Quaterniond> navigator;

    Transform3d transform() const {
        Transform3d result = Transform3d::Identity();
        result.linear() = navigator ? navigator->toRotationMatrix() :
            (Geometry::rotation_transform(pitch * Vec3d::UnitX()) *
             Geometry::rotation_transform(yaw * Vec3d::UnitZ())).linear().eval();
        return result;
    }

    void reset(double new_yaw, double new_pitch) {
        yaw = new_yaw; pitch = new_pitch; navigator.reset();
    }

    void set(const Transform3d& rotation) {
        navigator = Eigen::Quaterniond(rotation.linear()).normalized();
    }

    void orbit(double horizontal, double vertical) {
        if (navigator) {
            const auto rotation = Geometry::rotation_transform(vertical * Vec3d::UnitX()) *
                transform() * Geometry::rotation_transform(horizontal * Vec3d::UnitZ());
            set(rotation);
        } else {
            yaw += horizontal;
            pitch = std::clamp(pitch + vertical, -1.5707963267948966, -0.15);
        }
    }

    void turn(double angle) {
        if (navigator) set(transform() * Geometry::rotation_transform(angle * Vec3d::UnitZ()));
        else yaw = std::remainder(yaw + angle, 6.283185307179586);
    }
};

} // namespace Slic3r::GUI
