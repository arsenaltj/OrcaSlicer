#pragma once

#include "BeautySurface.hpp"
#include "SurfaceSelectionState.hpp"
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace Slic3r::AI {

struct BeautyLeafKey {
    size_t source_face_id {0};
    uint8_t depth {0}, path {0};
    friend bool operator<(const BeautyLeafKey& a, const BeautyLeafKey& b) {
        return std::tie(a.source_face_id, a.depth, a.path) < std::tie(b.source_face_id, b.depth, b.path);
    }
    friend bool operator==(const BeautyLeafKey& a, const BeautyLeafKey& b) {
        return a.source_face_id == b.source_face_id && a.depth == b.depth && a.path == b.path;
    }
    bool valid(size_t count) const {
        return source_face_id < count && depth <= 4 && unsigned(path) < (1u << (2 * depth));
    }
    bool contains(const BeautyLeafKey& other) const {
        return source_face_id == other.source_face_id && depth <= other.depth &&
            (unsigned(other.path) >> (2 * (other.depth - depth))) == path;
    }
    std::array<Vec3d, 3> barycentric() const {
        if (!valid(source_face_id + 1)) throw std::invalid_argument("Invalid leaf path.");
        std::array<Vec3d, 3> vertices {Vec3d(1,0,0), Vec3d(0,1,0), Vec3d(0,0,1)};
        for (unsigned level = 0; level < depth; ++level) {
            const auto a = vertices[0], b = vertices[1], c = vertices[2];
            const Vec3d ab = (a+b)*.5, bc = (b+c)*.5, ca = (c+a)*.5;
            switch ((path >> (2 * (depth - level - 1))) & 3) {
            case 0: vertices = {a,ab,ca}; break;
            case 1: vertices = {ab,b,bc}; break;
            case 2: vertices = {bc,c,ca}; break;
            default: vertices = {ab,bc,ca}; break;
            }
        }
        return vertices;
    }
};

inline std::string beauty_leaf_digest(const std::string& bytes) {
    unsigned char output[EVP_MAX_MD_SIZE]; unsigned int size = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), output, &size, EVP_sha256(), nullptr) != 1 || size != 32)
        throw std::runtime_error("Cannot hash leaf mapping.");
    std::ostringstream stream;
    for (unsigned i = 0; i < size; ++i) stream << std::hex << std::setw(2) << std::setfill('0') << unsigned(output[i]);
    return stream.str();
}

struct BeautyLeafDomain {
    std::string canonical_geometry_id;
    size_t source_face_count {0};
    // Only split roots are serialized. Unsplit roots are implicit depth-zero leaves.
    std::vector<BeautyLeafKey> split_leaves;

    static void validate_keys(const std::vector<BeautyLeafKey>& keys, size_t count) {
        if (!std::is_sorted(keys.begin(), keys.end())) throw std::invalid_argument("Leaf keys are not ordered.");
        std::set<BeautyLeafKey> seen;
        for (const auto& key : keys) {
            if (!key.valid(count) || !seen.insert(key).second) throw std::invalid_argument("Invalid or duplicate leaf key.");
            for (uint8_t depth = 0; depth < key.depth; ++depth)
                if (seen.count({key.source_face_id, depth, uint8_t(key.path >> (2 * (key.depth-depth)))}))
                    throw std::invalid_argument("Leaf keys overlap an ancestor.");
        }
    }
    static std::vector<BeautyLeafKey> decode_keys(const nlohmann::json& value, size_t count) {
        if (!value.is_array() || value.size() > 2000000) throw std::invalid_argument("Invalid leaf list.");
        std::vector<BeautyLeafKey> result;
        for (const auto& item : value) {
            if (!item.is_array() || item.size() != 3) throw std::invalid_argument("Invalid leaf record.");
            for (const auto& field : item) if (!field.is_number_integer() || field.get<int64_t>() < 0)
                throw std::invalid_argument("Invalid leaf integer.");
            const auto depth = item[1].get<uint64_t>(), path = item[2].get<uint64_t>();
            if (depth > 4 || path > 255) throw std::invalid_argument("Leaf path exceeds depth four.");
            result.push_back({item[0].get<size_t>(), uint8_t(depth), uint8_t(path)});
        }
        validate_keys(result, count);
        return result;
    }
    static nlohmann::json encode_keys(const std::vector<BeautyLeafKey>& keys) {
        auto result = nlohmann::json::array();
        for (const auto& key : keys) result.push_back({key.source_face_id, key.depth, key.path});
        return result;
    }
    void validate() const {
        if (canonical_geometry_id.size() != 64 || !source_face_count || source_face_count > 2000000)
            throw std::invalid_argument("Invalid canonical leaf geometry.");
        validate_keys(split_leaves, source_face_count);
        std::map<size_t, double> coverage;
        for (const auto& key : split_leaves) {
            if (!key.depth) throw std::invalid_argument("Unsplit roots must be implicit.");
            coverage[key.source_face_id] += std::ldexp(1., -2 * key.depth);
        }
        for (const auto& root : coverage) if (root.second != 1.)
            throw std::invalid_argument("Split leaves do not cover their root.");
        const auto added = split_leaves.size() - coverage.size();
        if (added > std::min<size_t>(20000, size_t(source_face_count * .02)))
            throw std::invalid_argument("Leaf triangle budget exceeded.");
    }
    std::vector<BeautyLeafKey> all_leaves() const {
        validate();
        std::vector<BeautyLeafKey> result;
        auto next = split_leaves.begin();
        for (size_t face = 0; face < source_face_count; ++face) {
            if (next == split_leaves.end() || next->source_face_id != face) result.push_back({face,0,0});
            else while (next != split_leaves.end() && next->source_face_id == face) result.push_back(*next++);
        }
        return result;
    }
    nlohmann::json encode() const {
        validate();
        return {{"schema", "orca.beauty-leaf-domain/v1"}, {"geometry_id", canonical_geometry_id},
            {"face_count", source_face_count}, {"split_leaves", encode_keys(split_leaves)}};
    }
    std::string fingerprint() const { return beauty_leaf_digest(encode().dump()); }
    BeautyLeafKey locate(size_t face, const Vec3d& bary) const {
        if (face >= source_face_count || !bary.allFinite() || bary.minCoeff() < -1e-6 || std::abs(bary.sum()-1.) > 1e-5)
            throw std::invalid_argument("Invalid canonical hit.");
        const auto begin = std::lower_bound(split_leaves.begin(), split_leaves.end(), BeautyLeafKey{face,0,0});
        if (begin == split_leaves.end() || begin->source_face_id != face) return {face,0,0};
        for (auto next = begin; next != split_leaves.end() && next->source_face_id == face; ++next) {
            const auto corners = next->barycentric();
            Eigen::Matrix3d transform;
            for (int i = 0; i < 3; ++i) transform.col(i) = corners[i];
            if ((transform.inverse()*bary).minCoeff() >= -1e-6) return *next;
        }
        throw std::invalid_argument("Hit leaves its canonical partition.");
    }
    static BeautyLeafDomain decode(const nlohmann::json& value, const std::string& geometry,
                                  size_t count, const std::string& hash) {
        if (!value.is_object() || value.size() != 4 || value.at("schema") != "orca.beauty-leaf-domain/v1" ||
            value.at("geometry_id") != geometry || value.at("face_count") != count ||
            beauty_leaf_digest(value.dump()) != hash) throw std::invalid_argument("Leaf mapping identity changed.");
        BeautyLeafDomain result{geometry, count, decode_keys(value.at("split_leaves"), count)};
        result.validate();
        return result;
    }
    indexed_triangle_set mesh(const indexed_triangle_set& original) const {
        if (original.indices.size() != source_face_count ||
            SurfaceSelectionPersistence::geometry_fingerprint(original) != canonical_geometry_id)
            throw std::invalid_argument("Leaf source geometry changed.");
        indexed_triangle_set result;
        for (const auto& key : all_leaves()) {
            const auto& face = original.indices[key.source_face_id];
            const auto base = int(result.vertices.size());
            for (const auto& bary : key.barycentric()) {
                Vec3d p = Vec3d::Zero();
                for (int corner = 0; corner < 3; ++corner) p += bary[corner] * original.vertices[face[corner]].cast<double>();
                result.vertices.push_back(p.cast<float>());
            }
            result.indices.emplace_back(base, base+1, base+2);
        }
        return result;
    }
    std::vector<std::vector<size_t>> adjacency(const indexed_triangle_set& original, const BeautySurface& canonical) const {
        if (canonical.geometry_id != canonical_geometry_id || original.indices.size() != source_face_count ||
            canonical.face_neighbors.size() != source_face_count || canonical.vertex_class.size() != original.vertices.size() ||
            SurfaceSelectionPersistence::geometry_fingerprint(original) != canonical_geometry_id)
            throw std::invalid_argument("Leaf adjacency source changed.");
        const auto keys = all_leaves();
        std::vector<std::vector<size_t>> result(keys.size());
        std::vector<size_t> first(source_face_count+1,keys.size());
        std::vector<uint8_t> split(source_face_count,0), affected(source_face_count,0);
        for (size_t i = 0; i < keys.size(); ++i) {
            first[keys[i].source_face_id] = std::min(first[keys[i].source_face_id],i);
            if (keys[i].depth) split[keys[i].source_face_id] = affected[keys[i].source_face_id] = 1;
        }
        for (size_t f = 0; f < source_face_count; ++f) if (split[f])
            for (const auto n : canonical.face_neighbors[f]) if (n >= 0) affected[size_t(n)] = 1;
        for (size_t f = 0; f < source_face_count; ++f) if (!split[f])
            for (const auto n : canonical.face_neighbors[f]) if (n >= 0 && !split[size_t(n)])
                result[first[f]].push_back(first[size_t(n)]);
        struct Segment { size_t leaf, root; int low, high; };
        using Line = std::tuple<size_t,int,int,int>;
        std::map<Line,std::vector<Segment>> internal;
        std::map<std::pair<uint32_t,uint32_t>,std::vector<Segment>> external;
        for (size_t i = 0; i < keys.size(); ++i) {
            const auto f = keys[i].source_face_id;
            if (!affected[f]) continue;
            const auto corners = keys[i].barycentric();
            for (size_t edge = 0; edge < 3; ++edge) {
                const auto& a = corners[edge]; const auto& b = corners[(edge+1)%3];
                int boundary = -1;
                for (int corner = 0; corner < 3; ++corner) if (a[corner] == 0. && b[corner] == 0.) boundary = corner;
                if (boundary >= 0) {
                    const auto& face = original.indices[f];
                    const int ca = (boundary+1)%3, cb = (boundary+2)%3;
                    const auto va = canonical.vertex_class[size_t(face[ca])], vb = canonical.vertex_class[size_t(face[cb])];
                    const int high = va < vb ? cb : ca;
                    const int t0 = int(std::lround(a[high]*16)), t1 = int(std::lround(b[high]*16));
                    external[std::minmax(va,vb)].push_back({i,f,std::min(t0,t1),std::max(t0,t1)});
                } else {
                    const int ax = int(std::lround(a[0]*16)), ay = int(std::lround(a[1]*16));
                    const int bx = int(std::lround(b[0]*16)), by = int(std::lround(b[1]*16));
                    int nx = by-ay, ny = ax-bx, offset = -(nx*ax+ny*ay);
                    const auto divisor = std::gcd(std::abs(nx),std::abs(ny));
                    if (!divisor) throw std::invalid_argument("Collapsed leaf edge.");
                    nx /= divisor; ny /= divisor; offset /= divisor;
                    if (nx < 0 || (nx == 0 && ny < 0)) { nx = -nx; ny = -ny; offset = -offset; }
                    const int t0 = -ny*ax+nx*ay, t1 = -ny*bx+nx*by;
                    internal[{f,nx,ny,offset}].push_back({i,f,std::min(t0,t1),std::max(t0,t1)});
                }
            }
        }
        const auto connect = [&](std::vector<Segment>& segments, bool cross_root) {
            std::sort(segments.begin(),segments.end(),[](const auto& a,const auto& b) { return a.low < b.low; });
            for (size_t i = 0; i < segments.size(); ++i) for (size_t j = i+1; j < segments.size() && segments[j].low < segments[i].high; ++j) {
                const auto& a = segments[i]; const auto& b = segments[j];
                if (a.leaf == b.leaf || std::min(a.high,b.high) <= std::max(a.low,b.low)) continue;
                if (cross_root && (a.root == b.root || std::find(canonical.face_neighbors[a.root].begin(),
                    canonical.face_neighbors[a.root].end(),int32_t(b.root)) == canonical.face_neighbors[a.root].end())) continue;
                result[a.leaf].push_back(b.leaf); result[b.leaf].push_back(a.leaf);
            }
        };
        for (auto& item : internal) connect(item.second,false);
        for (auto& item : external) connect(item.second,true);
        for (auto& row : result) {
            std::sort(row.begin(),row.end()); row.erase(std::unique(row.begin(),row.end()),row.end());
        }
        return result;
    }
};
} // namespace Slic3r::AI
