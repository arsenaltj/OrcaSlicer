#pragma once

#include "libslic3r/Model.hpp"

#include <cmath>
#include <cstddef>

namespace Slic3r::GUI {

inline bool orca_placement_target_is_eligible(const ModelObject* object, const ModelInstance* instance)
{
    return object != nullptr && instance != nullptr && object->printable && instance->printable;
}

inline bool orca_placement_respects_plate_lock(bool plate_locked, std::size_t transform_count)
{
    return !plate_locked || transform_count == 0;
}

inline bool orca_placement_transform_is_valid(const Transform3d& matrix)
{
    return matrix.matrix().allFinite() && std::abs(matrix.linear().determinant()) > 1e-12 &&
           matrix.matrix().row(3).isApprox(Eigen::RowVector4d(0.0, 0.0, 0.0, 1.0));
}

inline bool orca_placement_transform_preserves_geometry(const Transform3d& current,
                                                        const Transform3d& requested)
{
    if (!current.matrix().allFinite() || !requested.matrix().allFinite())
        return false;
    const double current_determinant = current.linear().determinant();
    const double requested_determinant = requested.linear().determinant();
    if (std::abs(current_determinant) <= 1e-12 || std::abs(requested_determinant) <= 1e-12 ||
        std::signbit(current_determinant) != std::signbit(requested_determinant))
        return false;

    const Matrix3d current_gram = current.linear().transpose() * current.linear();
    const Matrix3d requested_gram = requested.linear().transpose() * requested.linear();
    return current_gram.isApprox(requested_gram, 1e-8);
}

} // namespace Slic3r::GUI
