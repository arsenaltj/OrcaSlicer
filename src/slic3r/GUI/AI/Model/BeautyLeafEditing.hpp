#pragma once

#include "BeautyShapeLock.hpp"
#include "VertexColorRegionEditor.hpp"
#include "BeautyCellDomain.hpp"
#include "BeautySurfaceShapeLock.hpp"

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
    std::shared_ptr<const BeautyCellDomain> cells;

    bool contour() const { return bool(cells); }
    size_t size() const { return cells ? cells->triangles.size() : keys.size(); }
    size_t source_count() const { return cells ? cells->source_face_count() : domain.source_face_count; }
    size_t source_face(size_t i) const { return cells ? cells->triangles.at(i).source_face_id : keys.at(i).source_face_id; }
    std::string mapping_fingerprint() const { return cells ? cells->fingerprint() : domain.fingerprint(); }
    std::array<Vec3d,3> barycentric(size_t i) const { return cells ? cells->triangles.at(i).corners : keys.at(i).barycentric(); }

    static std::shared_ptr<BeautyLeafEditing> build_cells(const BeautySurfaceShapeLock& locks,
        const nlohmann::json& partition,std::shared_ptr<VertexColorRegionEditor> canonical,
        const std::vector<RGBA>& base_colors) {
        if(!canonical || !canonical->ready() || base_colors.size()!=partition.at("face_count"))
            throw std::invalid_argument("Contour editor source changed.");
        BeautySurfaceShapeLock::decode(locks.document,partition,BeautySurfaceShapeLock::identity(partition),
                                      locks.document.at("partition_ref").at("sha256"));
        auto result=std::make_shared<BeautyLeafEditing>();
        result->cells=BeautyCellDomain::build(partition,BeautySurfaceShapeLock::identity(partition));
        result->canonical_editor=std::move(canonical);
        result->shape_boundary_sha256=locks.fingerprint(partition);
        auto mesh=result->cells->mesh(result->canonical_editor->mesh());
        std::vector<RGBA> colors;colors.reserve(mesh.vertices.size());
        for(const auto& triangle:result->cells->triangles) for(size_t c=0;c<3;++c) colors.push_back(base_colors[triangle.source_face_id]);
        result->surface=BeautySurface::build(mesh,colors);
        result->surface->face_neighbors=result->cells->adjacency(result->canonical_editor->mesh());
        for(auto& patch:result->surface->patches) {
            std::set<uint32_t> neighbors;
            for(auto f:patch.faces) for(auto n:result->surface->face_neighbors[f])
                if(result->surface->face_patch[n]!=result->surface->face_patch[f]) neighbors.insert(result->surface->face_patch[n]);
            patch.neighbors.assign(neighbors.begin(),neighbors.end());
        }
        result->editor=std::make_shared<VertexColorRegionEditor>();std::string error;
        if(!result->editor->initialize(std::move(mesh),std::move(colors),error)) throw std::invalid_argument(error);
        result->editor->set_face_adjacency(result->surface->face_neighbors);
        auto& mapped=result->editing_locks;
        mapped.geometry_id=result->surface->geometry_id;mapped.face_count=result->size();
        for(const auto* field:{"source_sha256","evidence_sha256","runtime_sha256","policy_sha256"}) {
            if(std::string(field)=="source_sha256") mapped.source_sha256=partition.at(field);
            if(std::string(field)=="evidence_sha256") mapped.evidence_sha256=partition.at(field);
            if(std::string(field)=="runtime_sha256") mapped.runtime_sha256=partition.at(field);
            if(std::string(field)=="policy_sha256") mapped.policy_sha256=partition.at(field);
        }
        result->owners.assign(result->size(),-1);
        const auto faces_for=[&](const nlohmann::json& ids) {
            std::vector<size_t> indices;
            for(const auto& id:ids) {
                const auto found=result->cells->indices(id.get<std::string>());
                indices.insert(indices.end(),found.begin(),found.end());
            }
            std::sort(indices.begin(),indices.end());return indices;
        };
        for(const auto& lock:locks.document.at("locks")) {
            ShapeLock region;
            region.subject_id=lock.at("subject_id");region.label=lock.at("label");region.parent_label=lock.at("parent_label");
            region.status=lock.at("status");region.view_support=lock.at("view_support");region.reasons=lock.at("reasons").get<std::vector<std::string>>();
            region.locked_faces=faces_for(lock.at("locked_cells"));region.nested_faces=faces_for(lock.at("nested_cells"));
            const auto index=mapped.locks.size();
            for(auto f:region.locked_faces) result->owners[f]=int32_t(index*2+(std::binary_search(region.nested_faces.begin(),region.nested_faces.end(),f)?1:0));
            mapped.locks.push_back(region);
            if(lock.contains("periocular_cells") && !lock.at("periocular_cells").empty()) {
                region.label="periocular-"+region.label;region.parent_label=region.label;region.nested_faces.clear();
                region.locked_faces=faces_for(lock.at("periocular_cells"));
                for(auto f:region.locked_faces) result->owners[f]=int32_t(mapped.locks.size()*2);
                mapped.locks.push_back(std::move(region));
            }
        }
        return result;
    }

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
        if (canonical.empty()) return std::vector<T>(size(),fallback);
        if (canonical.size() != source_count()) throw std::invalid_argument("Canonical mask changed.");
        std::vector<T> result; result.reserve(size());
        for (size_t i=0;i<size();++i) result.push_back(canonical[source_face(i)]);
        return result;
    }
    SurfaceSelectionPersistence::SelectionState expand(const SurfaceSelectionPersistence::SelectionState& canonical) const {
        return {expand(canonical.selected),expand(canonical.protected_faces),expand(canonical.foreground),expand(canonical.domain)};
    }
    SurfaceSelectionPersistence::SelectionState collapse(const SurfaceSelectionPersistence::SelectionState& state) const {
        if (state.selected.size() != size()) throw std::invalid_argument("Surface selection changed.");
        SurfaceSelectionPersistence::SelectionState result;
        result.selected.assign(source_count(),1);
        result.foreground = result.domain = result.selected;
        result.protected_faces.assign(source_count(),0);
        for (size_t i = 0; i < size(); ++i) {
            const auto f = source_face(i);
            result.selected[f] &= state.selected[i];
            result.foreground[f] &= i < state.foreground.size() ? state.foreground[i] : 0;
            result.domain[f] &= i < state.domain.size() ? state.domain[i] : 0;
            result.protected_faces[f] |= i < state.protected_faces.size() ? state.protected_faces[i] : 0;
        }
        return result;
    }
    void normalize_cells(SurfaceSelectionPersistence::SelectionState& state) const {
        if (state.selected.size() != size()) throw std::invalid_argument("Surface selection changed.");
        if(cells) {
            std::set<std::string> selected;
            for(size_t i=0;i<size();++i) if(state.selected[i]) selected.insert(cells->cell_id(i));
            for(const auto& id:selected) {
                const auto triangles=cells->indices(id);
                const bool protected_cell=std::any_of(triangles.begin(),triangles.end(),
                    [&](size_t i){return i<state.protected_faces.size() && state.protected_faces[i];});
                for(auto i:triangles) {
                    state.selected[i]=protected_cell ? 0 : 1;
                    if(i<state.foreground.size()) state.foreground[i]=state.selected[i];
                    if(i<state.domain.size()) state.domain[i]=state.selected[i];
                }
            }
        }
    }
    void constrain(SurfaceSelectionPersistence::SelectionState& state, bool parent = false) const {
        normalize_cells(state);
        std::set<int32_t> selected_owners;
        if (!parent) for (size_t i = 0; i < size(); ++i) if (state.selected[i] && owners[i] >= 0) selected_owners.insert(owners[i]);
        for (size_t i = 0; i < size(); ++i)
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
        if (puzzle.geometry_id != surface->geometry_id || puzzle.face_piece.size() != size()) return false;
        std::optional<int32_t> owner;
        bool found = false;
        for (size_t i = 0; i < size(); ++i) if (puzzle.face_piece[i] == piece) {
            found = true;
            if (locked_only && owners[i] < 0) return false;
            if (owner && *owner != owners[i]) return false;
            owner = owners[i];
        }
        return found;
    }
    struct BoundarySegment { Vec3f a,b,normal; };
    std::vector<BoundarySegment> boundary_segments(const std::vector<uint32_t>& pieces) const {
        if (pieces.size() != size()) throw std::invalid_argument("Surface partition changed.");
        std::vector<BoundarySegment> result;
        const auto& mesh = canonical_editor->mesh();
        const auto corners = [&](size_t i) {
            std::array<Vec3d,3> result;
            const auto weights=barycentric(i); const auto& root=mesh.indices[source_face(i)];
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
