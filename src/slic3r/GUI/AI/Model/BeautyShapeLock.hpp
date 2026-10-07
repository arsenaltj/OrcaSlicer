#pragma once

#include "LocalSemanticEvidence.hpp"
#include "BeautyPuzzle.hpp"
#include "BeautyLeafDomain.hpp"
#include "ModelArtifact.hpp"
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace Slic3r::AI {

// Shape locks are the immutable ownership boundary between recognition and
// color editing. They deliberately live outside BeautyGuidance v1/v2 so old
// saved guidance remains readable and never acquires an implicit write scope.
struct ShapeLock {
    std::string subject_id;
    std::string parent_label;
    std::string label;
    std::string status;
    std::vector<size_t> locked_faces;
    std::vector<size_t> nested_faces;
    size_t view_support {0};
    std::vector<std::string> reasons;
    std::vector<BeautyLeafKey> locked_leaves, nested_leaves;
};

struct ShapeLockReference {
    static constexpr const char* schema = "orca.beauty-shape-lock-reference/v1";
    std::string path;
    std::string sha256;
    bool leaf_version {false};

    static bool valid_sha256(const std::string& value) {
        return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
    }

    nlohmann::json encode() const {
        return {{"schema", leaf_version ? "orca.beauty-shape-lock-reference/v2" : schema}, {"path", path}, {"sha256", sha256}};
    }

    static bool safe_path(const std::string& value) {
        if (value.empty() || value.size() > 192 || value.front() == '/' || value.find('\\') != std::string::npos ||
            value.find(':') != std::string::npos)
            return false;
        std::vector<std::string> parts;
        size_t start = 0;
        while (start <= value.size()) {
            const auto end = value.find('/', start);
            const auto part = value.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (part.empty() || part == "." || part == "..") return false;
            parts.push_back(part);
            if (end == std::string::npos) break;
            start = end + 1;
        }
        return parts.size() == 2 && parts.front() == "shape-locks" && valid_sha256(parts.back().substr(0, 64)) &&
            parts.back().size() == 69 && parts.back().substr(64) == ".json";
    }

    static ShapeLockReference decode(const nlohmann::json& value) {
        const auto version = value.value("schema", std::string());
        if (!value.is_object() || value.size() != 3 || (version != schema && version != "orca.beauty-shape-lock-reference/v2") ||
            !value.at("path").is_string() || !value.at("sha256").is_string())
            throw std::invalid_argument("Invalid shape lock sidecar reference.");
        ShapeLockReference result{value.at("path").get<std::string>(), value.at("sha256").get<std::string>()};
        result.leaf_version = version != schema;
        if (!safe_path(result.path) || !valid_sha256(result.sha256))
            throw std::invalid_argument("Unsafe shape lock sidecar reference.");
        if (result.path.substr(12, 64) != result.sha256.substr(0, 64))
            throw std::invalid_argument("Shape lock sidecar is not content addressed.");
        return result;
    }
};

struct ShapeLockSet {
    static constexpr const char* schema = "orca.beauty-shape-lock/v1";

    std::string geometry_id;
    std::string source_sha256;
    std::string evidence_sha256;
    std::string runtime_sha256;
    std::string policy_sha256;
    size_t face_count {0};
    std::vector<ShapeLock> locks;
    std::optional<BeautyLeafDomain> leaf_domain;
    std::string baseline_sha256, boundary_policy_sha256;

    bool empty() const { return locks.empty(); }

    static bool sha256(const std::string& value) {
        return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
    }
    static void require(bool condition, const char* message) {
        if (!condition) throw std::invalid_argument(message);
    }
    static std::vector<size_t> faces(const nlohmann::json& value, size_t count) {
        require(value.is_array() && value.size() <= count, "Invalid shape lock face list.");
        std::vector<size_t> result;
        result.reserve(value.size());
        for (const auto& item : value) {
            require(item.is_number_unsigned() || (item.is_number_integer() && item.get<int64_t>() >= 0),
                    "Invalid shape lock face index.");
            const auto face = item.get<uint64_t>();
            require(face < count, "Shape lock face exceeds the model.");
            result.push_back(size_t(face));
        }
        require(std::is_sorted(result.begin(), result.end()) &&
                    std::adjacent_find(result.begin(), result.end()) == result.end(),
                "Shape lock faces must be sorted and unique.");
        return result;
    }
    static nlohmann::json encode_faces(const std::vector<size_t>& value) {
        nlohmann::json result = nlohmann::json::array();
        for (const auto face : value) result.push_back(face);
        return result;
    }

    static bool allowed_parent(const std::string& label, const std::string& parent) {
        if (label == parent) return true;
        if ((label == "lb" || label == "rb" || label == "re" || label == "le" ||
             label == "ulip" || label == "llip" || label == "imouth") && parent == "face") return true;
        return label == "lip-line-corner" &&
            (parent == "face" || parent == "ulip" || parent == "llip" || parent == "imouth");
    }
    static bool supported_label(const std::string& label) {
        static const std::set<std::string> labels={"re","le","ulip","llip","imouth","lip-line-corner","lb","rb"};
        return labels.count(label)!=0;
    }
    static bool lockable_shape(const GUI::LocalSemanticEvidence::ShapeDetail& shape) {
        if (shape.accepted_faces.empty()) return false;
        if (shape.status != "VALID_SHAPE" && shape.status != "PROTECTED_SHAPE_UNCERTAIN") return false;
        return !hard_conflict(shape.reasons);
    }
    static bool hard_conflict(const std::vector<std::string>& reasons) {
        static const std::array<const char*, 4> hard = {
            "CROSS_SUBJECT", "CROSS_EYE", "PARENT_UNBOUND", "SOURCE_MAPPING"
        };
        return std::any_of(reasons.begin(), reasons.end(), [&](const std::string& reason) {
            return std::any_of(hard.begin(), hard.end(), [&](const char* marker) {
                return reason.find(marker) != std::string::npos;
            });
        });
    }

    static ShapeLockSet from_evidence(const GUI::LocalSemanticEvidence::Evidence& evidence,
                                      const std::string& evidence_hash) {
        ShapeLockSet result;
        result.geometry_id = evidence.identity.geometry_id;
        result.source_sha256 = evidence.identity.source_sha256;
        result.evidence_sha256 = evidence_hash;
        result.runtime_sha256 = evidence.identity.runtime_sha256;
        result.policy_sha256 = evidence.identity.policy_sha256;
        result.face_count = evidence.identity.face_count;
        require(sha256(result.geometry_id) && sha256(result.source_sha256) && sha256(result.evidence_sha256) &&
                    sha256(result.runtime_sha256) && sha256(result.policy_sha256),
                "Shape lock evidence identity is invalid.");
        std::set<size_t> claimed;
        for (const auto& shape : evidence.shape_details) {
            if (!lockable_shape(shape)) continue;
            require(supported_label(shape.label), "Shape lock label is not supported.");
            require(shape.view_support >= 2, "Shape lock requires multi-view support.");
            ShapeLock lock;
            lock.subject_id = shape.subject_id;
            lock.label = shape.label;
            lock.status = shape.status;
            lock.view_support = shape.view_support;
            lock.reasons = shape.reasons;
            lock.locked_faces = shape.accepted_faces;
            lock.nested_faces = shape.nested_faces;
            require(lock.nested_faces.empty() || lock.label == "re" || lock.label == "le",
                    "Nested iris lock requires an eye parent.");
            require(std::is_sorted(lock.locked_faces.begin(), lock.locked_faces.end()) &&
                        std::adjacent_find(lock.locked_faces.begin(), lock.locked_faces.end()) == lock.locked_faces.end(),
                    "Shape lock faces must be sorted and unique.");
            for (const auto face : shape.rejected_faces)
                require(!std::binary_search(lock.locked_faces.begin(), lock.locked_faces.end(), face),
                        "Shape lock accepted/rejected faces overlap.");
            for (const auto face : lock.locked_faces) {
                require(face < evidence.face_regions.size() && claimed.insert(face).second,
                        "Shape lock faces overlap another detail.");
                const auto owner = evidence.face_regions[face];
                require(owner >= 0 && size_t(owner) < evidence.regions.size(),
                        "Shape lock face has no semantic owner.");
                const auto& region = evidence.regions[size_t(owner)];
                require(region.subject_id == lock.subject_id && allowed_parent(lock.label, region.label),
                        "Shape lock crosses its semantic parent.");
                if (lock.parent_label.empty()) lock.parent_label = region.label;
                require(lock.parent_label == region.label, "Shape lock has mixed semantic parents.");
            }
            require(std::is_sorted(lock.nested_faces.begin(), lock.nested_faces.end()) &&
                        std::adjacent_find(lock.nested_faces.begin(), lock.nested_faces.end()) == lock.nested_faces.end(),
                    "Shape lock nested faces must be sorted and unique.");
            for (const auto face : lock.nested_faces)
                require(std::binary_search(lock.locked_faces.begin(), lock.locked_faces.end(), face),
                        "Shape lock nested face leaves its parent.");
            result.locks.push_back(std::move(lock));
        }
        return result;
    }

    nlohmann::json encode() const {
        require(sha256(geometry_id) && sha256(source_sha256) && sha256(evidence_sha256) &&
                    sha256(runtime_sha256) && sha256(policy_sha256) && face_count > 0,
                "Cannot encode an invalid shape lock set.");
        nlohmann::json values = nlohmann::json::array();
        for (const auto& lock : locks) {
            nlohmann::json item = {
                {"subject_id", lock.subject_id}, {"parent_label", lock.parent_label},
                {"label", lock.label}, {"status", lock.status},
                {"locked_faces", encode_faces(lock.locked_faces)},
                {"nested_faces", encode_faces(lock.nested_faces)}, {"view_support", lock.view_support}, {"reasons", lock.reasons}
            };
            if (leaf_domain) {
                item["locked_leaves"] = BeautyLeafDomain::encode_keys(lock.locked_leaves);
                item["nested_leaves"] = BeautyLeafDomain::encode_keys(lock.nested_leaves);
            }
            values.push_back(std::move(item));
        }
        nlohmann::json result = {{"schema", leaf_domain ? "orca.beauty-shape-lock/v2" : schema}, {"geometry_id", geometry_id}, {"source_sha256", source_sha256},
            {"evidence_sha256", evidence_sha256}, {"runtime_sha256", runtime_sha256},
            {"policy_sha256", policy_sha256}, {"face_count", face_count}, {"locks", std::move(values)}};
        if (leaf_domain) {
            validate_leaves();
            result["leaf_domain"] = leaf_domain->encode();
            result["leaf_mapping_sha256"] = leaf_domain->fingerprint();
            result["baseline_sha256"] = baseline_sha256;
            result["boundary_policy_sha256"] = boundary_policy_sha256;
        }
        return result;
    }

    void validate_leaves() const {
        if (!leaf_domain) return;
        require(sha256(baseline_sha256) && sha256(boundary_policy_sha256), "Leaf boundary identity is invalid.");
        require(leaf_domain->canonical_geometry_id == geometry_id && leaf_domain->source_face_count == face_count,
                "Leaf boundary belongs to another source.");
        leaf_domain->validate();
        std::set<BeautyLeafKey> available(leaf_domain->split_leaves.begin(), leaf_domain->split_leaves.end()), claimed;
        std::set<size_t> split_roots;
        for (const auto& leaf : leaf_domain->split_leaves) split_roots.insert(leaf.source_face_id);
        for (const auto& lock : locks) {
            BeautyLeafDomain::validate_keys(lock.locked_leaves, face_count);
            BeautyLeafDomain::validate_keys(lock.nested_leaves, face_count);
            require(!lock.locked_leaves.empty(), "Leaf lock has no accepted leaves.");
            std::set<size_t> roots;
            for (const auto& key : lock.locked_leaves) {
                require((split_roots.count(key.source_face_id) ? available.count(key) != 0 : key.depth == 0) &&
                        claimed.insert(key).second, "Leaf locks overlap or leave the mapping.");
                roots.insert(key.source_face_id);
            }
            require(std::vector<size_t>(roots.begin(), roots.end()) == lock.locked_faces, "Leaf root summary changed.");
            require(lock.nested_leaves.empty() || lock.label == "re" || lock.label == "le", "Nested leaf requires an eye.");
            std::set<size_t> nested_roots;
            for (const auto& key : lock.nested_leaves) {
                require(std::binary_search(lock.locked_leaves.begin(), lock.locked_leaves.end(), key), "Nested leaf leaves its parent.");
                nested_roots.insert(key.source_face_id);
            }
            require(std::vector<size_t>(nested_roots.begin(), nested_roots.end()) == lock.nested_faces,
                    "Nested leaf root summary changed.");
        }
    }

    static ShapeLockSet decode(const nlohmann::json& value, const std::string& geometry,
                               const std::string& source, size_t count,
                               const std::string& evidence = {}, const std::string& runtime = {},
                               const std::string& policy = {}, const std::string& baseline = {},
                               const std::string& boundary_policy = {}) {
        const bool leaf_version = value.value("schema", std::string()) == "orca.beauty-shape-lock/v2";
        require(leaf_version || (baseline.empty() && boundary_policy.empty()), "Leaf boundary identity requires v2 locks.");
        require(value.is_object() && (leaf_version || value.value("schema", std::string()) == schema),
                "Unsupported shape lock schema.");
        std::set<std::string> required = {"schema", "geometry_id", "source_sha256", "evidence_sha256",
            "runtime_sha256", "policy_sha256", "face_count", "locks"};
        if (leaf_version) required.insert({"leaf_domain", "leaf_mapping_sha256", "baseline_sha256", "boundary_policy_sha256"});
        require(value.size() == required.size(), "Unexpected shape lock fields.");
        for (auto it = value.begin(); it != value.end(); ++it) require(required.count(it.key()) != 0, "Unknown shape lock field.");
        ShapeLockSet result;
        result.geometry_id = value.at("geometry_id").get<std::string>();
        result.source_sha256 = value.at("source_sha256").get<std::string>();
        result.evidence_sha256 = value.at("evidence_sha256").get<std::string>();
        result.runtime_sha256 = value.at("runtime_sha256").get<std::string>();
        result.policy_sha256 = value.at("policy_sha256").get<std::string>();
        require(sha256(result.geometry_id) && sha256(result.source_sha256) && sha256(result.evidence_sha256) &&
                    sha256(result.runtime_sha256) && sha256(result.policy_sha256), "Invalid shape lock identity.");
        require(result.geometry_id == geometry && result.source_sha256 == source,
                "Shape lock belongs to another model.");
        if (!evidence.empty()) require(result.evidence_sha256 == evidence, "Shape lock evidence changed.");
        if (!runtime.empty()) require(result.runtime_sha256 == runtime, "Shape lock runtime changed.");
        if (!policy.empty()) require(result.policy_sha256 == policy, "Shape lock policy changed.");
        require(value.at("face_count").is_number_unsigned() && value.at("face_count").get<size_t>() == count,
                "Shape lock face count changed.");
        result.face_count = count;
        if (leaf_version) {
            result.leaf_domain = BeautyLeafDomain::decode(value.at("leaf_domain"), geometry, count,
                value.at("leaf_mapping_sha256").get<std::string>());
            result.baseline_sha256 = value.at("baseline_sha256").get<std::string>();
            result.boundary_policy_sha256 = value.at("boundary_policy_sha256").get<std::string>();
            if (!baseline.empty()) require(result.baseline_sha256 == baseline, "Leaf baseline changed.");
            if (!boundary_policy.empty()) require(result.boundary_policy_sha256 == boundary_policy, "Leaf boundary policy changed.");
        }
        const auto& locks = value.at("locks");
        require(locks.is_array() && locks.size() <= 128, "Too many shape locks.");
        std::set<size_t> claimed;
        std::set<std::pair<std::string, std::string>> identities;
        for (const auto& item : locks) {
            require(item.is_object() && (leaf_version ? item.size() == 10 :
                (item.size() == 7 || (item.size() == 8 && item.contains("reasons")))), "Invalid shape lock record.");
            ShapeLock lock;
            lock.subject_id = item.at("subject_id").get<std::string>();
            lock.parent_label = item.at("parent_label").get<std::string>();
            lock.label = item.at("label").get<std::string>();
            lock.status = item.at("status").get<std::string>();
            require(!lock.subject_id.empty() && !lock.parent_label.empty() && !lock.label.empty() &&
                        (lock.status == "VALID_SHAPE" || lock.status == "PROTECTED_SHAPE_UNCERTAIN") &&
                        supported_label(lock.label) &&
                        allowed_parent(lock.label,lock.parent_label) && identities.emplace(lock.subject_id, lock.label).second,
                    "Invalid or duplicate shape lock identity.");
            lock.locked_faces = faces(item.at("locked_faces"), count);
            lock.nested_faces = faces(item.at("nested_faces"), count);
            require(lock.nested_faces.empty() || lock.label == "re" || lock.label == "le",
                    "Nested iris lock requires an eye parent.");
            require(!lock.locked_faces.empty(), "Shape lock must contain accepted faces.");
            require(item.at("view_support").is_number_unsigned() && item.at("view_support").get<size_t>() >= 2,
                    "Shape lock lacks multi-view support.");
            lock.view_support = item.at("view_support").get<size_t>();
            if (leaf_version) {
                lock.locked_leaves = BeautyLeafDomain::decode_keys(item.at("locked_leaves"), count);
                lock.nested_leaves = BeautyLeafDomain::decode_keys(item.at("nested_leaves"), count);
            }
            if (item.contains("reasons")) {
                require(item.at("reasons").is_array() && item.at("reasons").size() <= 16, "Invalid shape lock reasons.");
                for (const auto& reason : item.at("reasons")) {
                    const auto text = reason.get<std::string>();
                    require(GUI::LocalSemanticEvidence::detail::identifier(text), "Invalid shape lock reason.");
                    lock.reasons.push_back(text);
                }
            }
            require(!hard_conflict(lock.reasons), "Shape lock contains an ownership conflict.");
            if (!leaf_version) for (const auto face : lock.locked_faces)
                require(claimed.insert(face).second, "Shape lock faces overlap.");
            for (const auto face : lock.nested_faces)
                require(std::binary_search(lock.locked_faces.begin(), lock.locked_faces.end(), face),
                        "Shape lock nested face leaves its parent.");
            result.locks.push_back(std::move(lock));
        }
        result.validate_leaves();
        return result;
    }

    bool compatible(const std::string& geometry, const std::string& source, size_t count,
                    const std::string& evidence = {}, const std::string& runtime = {},
                    const std::string& policy = {}) const {
        try {
            if (geometry_id != geometry || source_sha256 != source || face_count != count ||
                !sha256(geometry_id) || !sha256(source_sha256) || !sha256(evidence_sha256) ||
                !sha256(runtime_sha256) || !sha256(policy_sha256)) return false;
            if (!evidence.empty() && evidence_sha256 != evidence) return false;
            if (!runtime.empty() && runtime_sha256 != runtime) return false;
            if (!policy.empty() && policy_sha256 != policy) return false;
            validate_leaves();
            return true;
        } catch (...) { return false; }
    }
    bool boundary_compatible(const std::string& baseline, const std::string& boundary_policy,
                             const std::string& mapping) const {
        try {
            return leaf_domain && sha256(baseline) && sha256(boundary_policy) &&
                baseline_sha256 == baseline && boundary_policy_sha256 == boundary_policy &&
                compatible(geometry_id, source_sha256, face_count) && leaf_domain->fingerprint() == mapping;
        } catch (...) { return false; }
    }

    std::optional<size_t> lock_for_face(size_t face) const {
        for (size_t index = 0; index < locks.size(); ++index)
            if (std::binary_search(locks[index].locked_faces.begin(), locks[index].locked_faces.end(), face)) return index;
        return std::nullopt;
    }
    bool face_locked(size_t face) const { return lock_for_face(face).has_value(); }

    // Re-cut verified faces into standalone editable pieces. Nested faces are
    // cut after their parent so a child detail cannot be recolored by a parent
    // operation. No unverified face is added to either piece.
    void isolate(BeautyPuzzle& puzzle, const BeautySurface& surface) const {
        if (leaf_domain) throw std::invalid_argument("Leaf locks require the derived editing domain.");
        if (puzzle.face_piece.size() != face_count) throw std::invalid_argument("Shape lock geometry does not match the puzzle.");
        for (const auto& lock : locks) {
            std::set<size_t> nested(lock.nested_faces.begin(), lock.nested_faces.end());
            std::vector<size_t> outer;
            outer.reserve(lock.locked_faces.size());
            for (const auto face : lock.locked_faces) if (!nested.count(face)) outer.push_back(face);
            if (!outer.empty()) puzzle.assign_region(outer, surface, false);
            if (!lock.nested_faces.empty()) puzzle.assign_region(lock.nested_faces, surface, false);
        }
    }

    // A locked face must retain its piece identity and may not become part of
    // any piece containing an unlocked face or a different lock.
    bool preserves(const BeautyPuzzle& before, const BeautyPuzzle& after) const {
        if (before.face_piece.size() != face_count || after.face_piece.size() != face_count) return false;
        std::vector<int32_t> owners(face_count, -1);
        for (size_t index = 0; index < locks.size(); ++index)
            for (const auto face : locks[index].locked_faces) {
                if (face >= face_count || owners[face] >= 0) return false;
                owners[face] = int32_t(index * 2 + (std::binary_search(locks[index].nested_faces.begin(),
                    locks[index].nested_faces.end(), face) ? 1 : 0));
            }
        std::map<uint32_t, int32_t> piece_owner;
        std::map<uint32_t, uint32_t> forward, reverse;
        for (size_t face = 0; face < face_count; ++face) {
            const auto next = after.face_piece[face];
            const auto inserted = piece_owner.emplace(next, owners[face]);
            if (!inserted.second && inserted.first->second != owners[face]) return false;
            if (owners[face] < 0) continue;
            const auto old = before.face_piece[face];
            const auto to = forward.emplace(old, next);
            const auto from = reverse.emplace(next, old);
            if ((!to.second && to.first->second != next) || (!from.second && from.first->second != old)) return false;
        }
        return true;
    }
};

// Shape locks are stored in an application-owned content-addressed directory.
// The model metadata contains only ShapeLockReference, so a model cannot point
// the workbench at an arbitrary external file.
inline ShapeLockReference write_shape_lock_sidecar(const boost::filesystem::path& root,
                                                   const ShapeLockSet& locks) {
    if (locks.empty()) throw std::invalid_argument("Cannot persist an empty shape lock set.");
    const auto payload = locks.encode().dump();
    const auto directory = root / "shape-locks";
    if (boost::filesystem::is_symlink(directory)) throw std::invalid_argument("Unsafe shape lock sidecar directory.");
    boost::system::error_code ec;
    boost::filesystem::create_directories(directory, ec);
    if (ec) throw std::runtime_error("Cannot create the shape lock sidecar directory.");
    const auto temporary = directory / boost::filesystem::unique_path("shape-lock-%%%%-%%%%.tmp");
    try {
        boost::filesystem::ofstream output(temporary, std::ios::binary);
        output << payload;
        output.close();
        if (!output) throw std::runtime_error("Cannot write the shape lock sidecar.");
        const auto digest = AI::model_artifact_sha256(temporary);
        if (!ShapeLockSet::sha256(digest)) throw std::runtime_error("Cannot hash the shape lock sidecar.");
        const auto filename = digest + ".json";
        const auto relative = std::string("shape-locks/") + filename;
        const auto target = directory / filename;
        if (boost::filesystem::is_symlink(target)) throw std::invalid_argument("Unsafe shape lock sidecar file.");
        ShapeLockReference reference{relative, digest, bool(locks.leaf_domain)};
        if (boost::filesystem::is_regular_file(target, ec) && !ec) {
            if (AI::model_artifact_sha256(target) != digest) throw std::runtime_error("Shape lock sidecar changed.");
            boost::filesystem::remove(temporary, ec);
            return reference;
        }
        boost::filesystem::rename(temporary, target, ec);
        if (ec) throw std::runtime_error("Cannot publish the shape lock sidecar.");
        return reference;
    } catch (...) {
        boost::filesystem::remove(temporary, ec);
        throw;
    }
}

inline ShapeLockSet read_shape_lock_sidecar(const boost::filesystem::path& root,
                                            const ShapeLockReference& reference,
                                            const std::string& geometry,
                                            const std::string& source,
                                            size_t face_count,
                                            const std::string& evidence = {},
                                            const std::string& runtime = {},
                                            const std::string& policy = {}) {
    ShapeLockReference::decode(reference.encode());
    if (boost::filesystem::is_symlink(root / "shape-locks")) throw std::invalid_argument("Unsafe shape lock sidecar directory.");
    const auto expected = root / "shape-locks" / (reference.path.substr(12, 64) + ".json");
    if (reference.path != expected.lexically_relative(root).generic_string() || boost::filesystem::is_symlink(expected))
        throw std::invalid_argument("Shape lock sidecar path escapes its owner.");
    boost::system::error_code ec;
    if (!boost::filesystem::is_regular_file(expected, ec) || ec || AI::model_artifact_sha256(expected) != reference.sha256)
        throw std::invalid_argument("Shape lock sidecar hash changed.");
    if (boost::filesystem::file_size(expected, ec) > 8 * 1024 * 1024 || ec)
        throw std::invalid_argument("Shape lock sidecar is too large.");
    boost::filesystem::ifstream input(expected, std::ios::binary);
    if (!input) throw std::invalid_argument("Cannot read shape lock sidecar.");
    nlohmann::json value;
    input >> value;
    if ((value.value("schema", std::string()) == "orca.beauty-shape-lock/v2") != reference.leaf_version)
        throw std::invalid_argument("Shape lock reference version changed.");
    return ShapeLockSet::decode(value, geometry, source, face_count, evidence, runtime, policy);
}

} // namespace Slic3r::AI
