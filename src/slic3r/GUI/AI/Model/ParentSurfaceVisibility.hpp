#pragma once

#include "libslic3r/AABBTreeIndirect.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include <array>

namespace Slic3r::AI {

inline constexpr std::array<std::array<double,3>,7> parent_visibility_samples {{
    {{1./3,1./3,1./3}},{{.98,.01,.01}},{{.01,.98,.01}},{{.01,.01,.98}},
    {{.49,.49,.02}},{{.02,.49,.49}},{{.49,.02,.49}}
}};

struct ParentSurfaceVisibility {
    const indexed_triangle_set& mesh;
    AABBTreeIndirect::Tree3f tree;
    explicit ParentSurfaceVisibility(const indexed_triangle_set& source) : mesh(source),
        tree(AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(source.vertices,source.indices)) {}
};

inline uint8_t parent_visible_samples(const ParentSurfaceVisibility& visibility, size_t face,
                                     const Vec3d& direction_to_camera, double distance,
                                     const std::array<std::array<double,3>,7>& samples=parent_visibility_samples) {
    const auto& mesh=visibility.mesh;
    const auto& indices=mesh.indices.at(face);
    const Vec3d a=mesh.vertices[indices[0]].cast<double>(),b=mesh.vertices[indices[1]].cast<double>(),
                c=mesh.vertices[indices[2]].cast<double>();
    const double epsilon=std::max(1e-20,(b-a).cross(c-a).norm()*1e-8);
    uint8_t mask=0;
    for (size_t sample=0;sample<samples.size();++sample) {
        const auto& weights=samples[sample];
        const Vec3d target=weights[0]*a+weights[1]*b+weights[2]*c;
        igl::Hit<float> hit;
        const Vec3d origin=target+distance*direction_to_camera,ray=-direction_to_camera;
        const bool found=AABBTreeIndirect::intersect_ray_first_hit(mesh.vertices,mesh.indices,
            visibility.tree,origin,ray,hit,epsilon);
        if (found && hit.id==int(face))
            mask|=uint8_t(1u<<sample);
    }
    return mask;
}

} // namespace Slic3r::AI
