#pragma once

#include "BeautyShapeLock.hpp"
#include "VertexColorRegionEditor.hpp"

namespace Slic3r::AI {

// Derived face indices never leave this adapter as canonical source face IDs.
struct BeautyLeafEditing {
    BeautyLeafDomain domain;
    std::vector<BeautyLeafKey> keys;
    std::vector<int32_t> owners;
    ShapeLockSet editing_locks;
    std::shared_ptr<BeautySurface> surface;
    std::shared_ptr<VertexColorRegionEditor> editor;
    std::shared_ptr<VertexColorRegionEditor> canonical_editor;
    std::string shape_boundary_sha256;

    static std::shared_ptr<BeautyLeafEditing> build(const ShapeLockSet& locks,
        std::shared_ptr<VertexColorRegionEditor> canonical, const std::vector<RGBA>& base_colors,
        const BeautyLeafDomain* parent_domain = nullptr) {
        if (!locks.leaf_domain || !canonical || !canonical->ready() ||
            base_colors.size() != locks.face_count ||
            !locks.compatible(SurfaceSelectionPersistence::geometry_fingerprint(canonical->mesh()),locks.source_sha256,base_colors.size()))
            throw std::invalid_argument("Derived editor source changed.");
        auto result = std::make_shared<BeautyLeafEditing>();
        result->shape_boundary_sha256 = beauty_leaf_digest(locks.encode().dump());
        result->domain = parent_domain ? *parent_domain : *locks.leaf_domain;
        if (result->domain.canonical_geometry_id != locks.geometry_id || result->domain.source_face_count != locks.face_count)
            throw std::invalid_argument("Parent editing domain changed its source.");
        result->keys = result->domain.all_leaves();
        for (const auto& lock : locks.locks) for (const auto& key : lock.locked_leaves)
            if (!std::binary_search(result->keys.begin(), result->keys.end(), key))
                throw std::invalid_argument("Parent editing domain changes a locked leaf.");
        result->canonical_editor = std::move(canonical);
        const auto original = BeautySurface::build(result->canonical_editor->mesh(),result->canonical_editor->vertex_colors());
        auto mesh = result->domain.mesh(result->canonical_editor->mesh());
        std::vector<RGBA> colors; colors.reserve(mesh.vertices.size());
        for (const auto& key : result->keys) for (size_t corner = 0; corner < 3; ++corner)
            colors.push_back(base_colors[key.source_face_id]);
        result->surface = BeautySurface::build(mesh,colors);
        const auto links = result->domain.adjacency(result->canonical_editor->mesh(),*original);
        for (size_t i = 0; i < links.size(); ++i)
            result->surface->face_neighbors[i].assign(links[i].begin(),links[i].end());
        for (auto& patch : result->surface->patches) {
            std::set<uint32_t> neighbors;
            for (const auto f : patch.faces) for (const auto n : result->surface->face_neighbors[f])
                if (result->surface->face_patch[n] != result->surface->face_patch[f]) neighbors.insert(result->surface->face_patch[n]);
            patch.neighbors.assign(neighbors.begin(),neighbors.end());
        }
        result->editor = std::make_shared<VertexColorRegionEditor>();
        std::string error;
        if (!result->editor->initialize(std::move(mesh),std::move(colors),error)) throw std::invalid_argument(error);
        result->editor->set_face_adjacency(result->surface->face_neighbors);
        result->editing_locks = locks;
        result->editing_locks.leaf_domain.reset();
        result->editing_locks.geometry_id = result->surface->geometry_id;
        result->editing_locks.face_count = result->keys.size();
        result->owners.assign(result->keys.size(),-1);
        for (size_t index = 0; index < locks.locks.size(); ++index) {
            const auto& source = locks.locks[index]; auto& mapped = result->editing_locks.locks[index];
            mapped.locked_faces.clear(); mapped.nested_faces.clear();
            mapped.locked_leaves.clear(); mapped.nested_leaves.clear();
            for (const auto& key : source.locked_leaves) {
                const auto i = result->index(key); mapped.locked_faces.push_back(i);
                const bool nested = std::binary_search(source.nested_leaves.begin(),source.nested_leaves.end(),key);
                if (nested) mapped.nested_faces.push_back(i);
                result->owners[i] = int32_t(index*2 + (nested ? 1 : 0));
            }
        }
        return result;
    }
    size_t index(const BeautyLeafKey& key) const {
        const auto found = std::lower_bound(keys.begin(),keys.end(),key);
        if (found == keys.end() || !(*found == key)) throw std::invalid_argument("Leaf is outside the editing mapping.");
        return size_t(found-keys.begin());
    }
    template<class T> std::vector<T> expand(const std::vector<T>& canonical, T fallback = {}) const {
        if (canonical.empty()) return std::vector<T>(keys.size(),fallback);
        if (canonical.size() != domain.source_face_count) throw std::invalid_argument("Canonical mask changed.");
        std::vector<T> result; result.reserve(keys.size());
        for (const auto& key : keys) result.push_back(canonical[key.source_face_id]);
        return result;
    }
    SurfaceSelectionPersistence::SelectionState expand(const SurfaceSelectionPersistence::SelectionState& canonical) const {
        return {expand(canonical.selected),expand(canonical.protected_faces),expand(canonical.foreground),expand(canonical.domain)};
    }
    SurfaceSelectionPersistence::SelectionState collapse(const SurfaceSelectionPersistence::SelectionState& state) const {
        if (state.selected.size() != keys.size()) throw std::invalid_argument("Leaf selection changed.");
        SurfaceSelectionPersistence::SelectionState result;
        result.selected.assign(domain.source_face_count,1);
        result.foreground = result.domain = result.selected;
        result.protected_faces.assign(domain.source_face_count,0);
        for (size_t i = 0; i < keys.size(); ++i) {
            const auto f = keys[i].source_face_id;
            result.selected[f] &= state.selected[i];
            result.foreground[f] &= i < state.foreground.size() ? state.foreground[i] : 0;
            result.domain[f] &= i < state.domain.size() ? state.domain[i] : 0;
            result.protected_faces[f] |= i < state.protected_faces.size() ? state.protected_faces[i] : 0;
        }
        return result;
    }
    void constrain(SurfaceSelectionPersistence::SelectionState& state, bool parent = false) const {
        if (state.selected.size() != keys.size()) throw std::invalid_argument("Leaf selection changed.");
        std::set<int32_t> selected_owners;
        if (!parent) for (size_t i = 0; i < keys.size(); ++i) if (state.selected[i] && owners[i] >= 0) selected_owners.insert(owners[i]);
        for (size_t i = 0; i < keys.size(); ++i)
            if ((parent && owners[i] >= 0) || (!selected_owners.empty() && !selected_owners.count(owners[i]))) {
                state.selected[i] = 0;
                if (i < state.foreground.size()) state.foreground[i] = 0;
                if (i < state.domain.size()) state.domain[i] = 0;
            }
    }
    BeautyLeafKey hit_key(const VertexColorRegionEditor::SurfaceHit& hit) const {
        const auto& key = keys.at(hit.face); Vec3d bary = Vec3d::Zero();
        const auto corners = key.barycentric();
        for (size_t i = 0; i < 3; ++i) bary += hit.barycentric[i]*corners[i];
        return domain.locate(key.source_face_id,bary);
    }
    bool colorable(const BeautyPuzzle& puzzle, uint32_t piece, bool locked_only = true) const {
        if (puzzle.geometry_id != surface->geometry_id || puzzle.face_piece.size() != keys.size()) return false;
        std::optional<int32_t> owner;
        bool found = false;
        for (size_t i = 0; i < keys.size(); ++i) if (puzzle.face_piece[i] == piece) {
            found = true;
            if (locked_only && owners[i] < 0) return false;
            if (owner && *owner != owners[i]) return false;
            owner = owners[i];
        }
        return found;
    }
    struct BoundarySegment { Vec3f a,b,normal; };
    std::vector<BoundarySegment> boundary_segments(const std::vector<uint32_t>& pieces) const {
        if (pieces.size() != keys.size()) throw std::invalid_argument("Leaf partition changed.");
        std::vector<BoundarySegment> result;
        const auto& mesh = canonical_editor->mesh();
        const auto corners = [&](size_t i) {
            std::array<Vec3d,3> result;
            const auto weights=keys[i].barycentric(); const auto& root=mesh.indices[keys[i].source_face_id];
            for (size_t c=0;c<3;++c) {
                result[c]=Vec3d::Zero();
                for (size_t k=0;k<3;++k) result[c]+=weights[c][k]*mesh.vertices[root[k]].cast<double>();
            }
            return result;
        };
        for (size_t f = 0; f < pieces.size(); ++f) for (const auto n : surface->face_neighbors[f]) {
            if (n < 0 || size_t(n) <= f || pieces[f] == pieces[n]) continue;
            const auto a=corners(f),b=corners(size_t(n));
            for (int e = 0; e < 3; ++e) {
                const Vec3d start = a[e];
                const Vec3d direction = (a[(e+1)%3]-start).eval();
                const double length2 = direction.squaredNorm();
                if (length2 < 1e-24) continue;
                for (int j = 0; j < 3; ++j) {
                    const Vec3d x = (b[j]-start).eval();
                    const Vec3d y = (b[(j+1)%3]-start).eval();
                    if (x.cross(direction).squaredNorm() > length2*1e-14 || y.cross(direction).squaredNorm() > length2*1e-14) continue;
                    const double tx = x.dot(direction)/length2, ty = y.dot(direction)/length2;
                    const double low = std::max(0.,std::min(tx,ty)), high = std::min(1.,std::max(tx,ty));
                    if (high-low > 1e-7) result.push_back({(start+low*direction).cast<float>(),
                        (start+high*direction).cast<float>(),surface->normals[f].cast<float>()});
                }
            }
        }
        return result;
    }
};
} // namespace Slic3r::AI
