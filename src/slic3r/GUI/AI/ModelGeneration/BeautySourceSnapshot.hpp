#pragma once

#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Color.hpp"
#include <cstring>
#include <memory>
#include <vector>

namespace Slic3r::GUI {

// Aliases immutable source arrays while their original task/editor stays alive.
// Producers must not move or mutate these arrays after sharing them.
class BeautySourceSnapshot {
public:
    BeautySourceSnapshot() = default;
    template<class Owner>
    static BeautySourceSnapshot share(const std::shared_ptr<Owner>& owner,
        const indexed_triangle_set& mesh, const std::vector<RGBA>& colors) {
        if (!owner || mesh.indices.empty() || mesh.vertices.empty() || colors.size() != mesh.vertices.size())
            return {};
        return {std::shared_ptr<const indexed_triangle_set>(owner, &mesh),
                std::shared_ptr<const std::vector<RGBA>>(owner, &colors)};
    }

    static bool identical_mesh(const indexed_triangle_set& source, const indexed_triangle_set& target) {
        if(&source==&target)return true;
        if(source.vertices.size()!=target.vertices.size() || source.indices!=target.indices)return false;
        for(size_t v=0;v<source.vertices.size();++v)
            if(std::memcmp(source.vertices[v].data(),target.vertices[v].data(),3*sizeof(float))!=0)return false;
        return true;
    }
    bool matches_geometry(const indexed_triangle_set& target) const {
        return bool(*this) && identical_mesh(mesh(),target);
    }

    explicit operator bool() const { return bool(m_mesh) && bool(m_colors); }
    const indexed_triangle_set& mesh() const { return *m_mesh; }
    const std::vector<RGBA>& colors() const { return *m_colors; }

private:
    BeautySourceSnapshot(std::shared_ptr<const indexed_triangle_set> mesh,
                         std::shared_ptr<const std::vector<RGBA>> colors)
        : m_mesh(std::move(mesh)), m_colors(std::move(colors)) {}
    std::shared_ptr<const indexed_triangle_set> m_mesh;
    std::shared_ptr<const std::vector<RGBA>> m_colors;
};

} // namespace Slic3r::GUI
