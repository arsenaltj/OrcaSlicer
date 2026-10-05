#pragma once

#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "slic3r/GUI/AI/Model/SurfaceSelectionState.hpp"
#include "SemanticRegionEvidence.hpp"
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <memory>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace Slic3r::GUI {

// Read-only, content-addressed detail evidence. It is deliberately separate
// from SemanticRegionEvidence so an invalid experimental detail cannot disable
// the established primary semantic cache.
struct SecondaryRegion {
    std::string detail_id;
    std::string parent_region;
    std::string source_module;
    std::string subject_id;
    std::vector<size_t> accepted_faces;
    std::vector<size_t> protected_faces;
    std::vector<size_t> low_confidence_faces;
    std::vector<std::string> view_support;
    float confidence {0.f};
    std::string status;
    std::string evidence_sha256;

    static bool supported_id(const std::string& id) {
        static const std::set<std::string> ids = {
            "hair", "cloth", "face", "neck", "lr", "rr", "lb", "rb", "le", "re", "iris",
            "ulip", "llip", "imouth", "nose", "teeth", "lip-line-corner", "ear", "accessories"
        };
        return ids.find(id) != ids.end();
    }
    bool valid(size_t face_count) const {
        if (!supported_id(detail_id) || parent_region.empty() || source_module.empty() ||
            !std::isfinite(confidence) || confidence < 0.f || confidence > 1.f ||
            (status != "PASS" && status != "PROTECTED_CONTINUE" && status != "FAIL_STOP") ||
            !AI::SurfaceSelectionPersistence::detail::valid_fingerprint(evidence_sha256)) return false;
        auto sorted_unique_in_range = [face_count](const std::vector<size_t>& faces) {
            return std::all_of(faces.begin(), faces.end(), [face_count](size_t face) { return face < face_count; }) &&
                std::is_sorted(faces.begin(), faces.end()) &&
                std::adjacent_find(faces.begin(), faces.end()) == faces.end();
        };
        if (!sorted_unique_in_range(accepted_faces) || !sorted_unique_in_range(protected_faces) ||
            !sorted_unique_in_range(low_confidence_faces)) return false;
        std::vector<size_t> overlap;
        std::set_intersection(accepted_faces.begin(), accepted_faces.end(), protected_faces.begin(), protected_faces.end(),
            std::back_inserter(overlap));
        if (!overlap.empty()) return false;
        overlap.clear();
        std::set_intersection(accepted_faces.begin(), accepted_faces.end(), low_confidence_faces.begin(), low_confidence_faces.end(),
            std::back_inserter(overlap));
        return overlap.empty();
    }
    bool selectable() const { return !accepted_faces.empty() && status == "PASS"; }
};

struct SecondaryRegionEvidence {
    std::string geometry_id;
    std::string source_sha256;
    std::string runtime_identity;
    size_t face_count {0};
    std::vector<SecondaryRegion> regions;

    // Native v17 labels have no left/right eye or upper/lower lip identity.
    // Export only details that the cached face labels can distinguish.
    static std::shared_ptr<const SecondaryRegionEvidence> from_primary(
        const SemanticRegionEvidence& primary, const std::string& source_sha256,
        const std::string& source_content_id, std::string& error,
        bool verified_model_reference = false) {
        namespace SC = AI::SemanticColoring;
        if (!primary.valid() || (!verified_model_reference && primary.source_content_id != source_content_id) ||
            !AI::SurfaceSelectionPersistence::detail::valid_fingerprint(source_sha256) ||
            !AI::SurfaceSelectionPersistence::detail::valid_fingerprint(primary.analysis_signature)) {
            error = "Primary semantic evidence does not match the current source";
            return {};
        }
        auto result = std::make_shared<SecondaryRegionEvidence>();
        result->geometry_id = primary.geometry_id;
        result->source_sha256 = source_sha256;
        result->runtime_identity = primary.runtime_identity;
        result->face_count = primary.labels.size();
        struct Mapping { SC::Label label; const char* id; const char* parent; const char* module; };
        static constexpr Mapping mappings[] = {
            {SC::Label::Hair, "hair", "hair", "hair-ear"},
            {SC::Label::FaceSkin, "face", "skin", "face"},
            {SC::Label::Clothes, "cloth", "clothes", "body"},
            {SC::Label::Accessories, "accessories", "clothes", "accessories"},
            {SC::Label::MouthInterior, "imouth", "lips", "teeth-oral"},
            {SC::Label::Iris, "iris", "eyes", "eyes"}
        };
        for (const auto& mapping : mappings) {
            SecondaryRegion detail;
            detail.detail_id = mapping.id;
            detail.parent_region = mapping.parent;
            detail.source_module = mapping.module;
            detail.evidence_sha256 = primary.analysis_signature;
            double confidence_sum = 0.0;
            for (size_t face = 0; face < primary.labels.size(); ++face) {
                if (primary.labels[face] != mapping.label) continue;
                if (primary.confidence[face] >= SC::minimum_confidence) {
                    detail.accepted_faces.push_back(face);
                    confidence_sum += primary.confidence[face];
                } else detail.low_confidence_faces.push_back(face);
            }
            if (detail.accepted_faces.empty() && detail.low_confidence_faces.empty()) continue;
            detail.confidence = detail.accepted_faces.empty() ? 0.f :
                static_cast<float>(confidence_sum / detail.accepted_faces.size());
            detail.status = detail.accepted_faces.empty() ? "PROTECTED_CONTINUE" : "PASS";
            result->regions.push_back(std::move(detail));
        }
        if (!result->valid()) {
            error = "Primary semantic cache contains no usable secondary details";
            return {};
        }
        error.clear();
        return result;
    }

    static std::shared_ptr<const SecondaryRegionEvidence> for_derived_model(
        const SecondaryRegionEvidence& original, const std::string& original_source_sha256,
        const std::string& geometry, size_t faces, const std::string& runtime,
        const std::string& derived_source_sha256, std::string& error) {
        if (!original.valid() ||
            !original.compatible(geometry, original_source_sha256, faces, runtime) ||
            !AI::SurfaceSelectionPersistence::detail::valid_fingerprint(derived_source_sha256)) {
            error = "Secondary evidence cannot be transferred to a changed model";
            return {};
        }
        auto result = std::make_shared<SecondaryRegionEvidence>(original);
        result->source_sha256 = derived_source_sha256;
        error.clear();
        return result;
    }

    bool compatible(const std::string& geometry, const std::string& source, size_t faces,
                    const std::string& runtime) const {
        return !geometry.empty() && geometry_id == geometry &&
            !source.empty() && source_sha256 == source && face_count == faces &&
            !runtime.empty() && runtime_identity == runtime;
    }
    bool valid() const {
        if (!AI::SurfaceSelectionPersistence::detail::valid_fingerprint(geometry_id) ||
            !AI::SurfaceSelectionPersistence::detail::valid_fingerprint(source_sha256) ||
            runtime_identity.empty() || face_count == 0 || regions.empty()) return false;
        std::set<std::string> ids;
        for (const auto& region : regions)
            if (!region.valid(face_count) || !ids.insert(region.detail_id).second) return false;
        return true;
    }
    struct DetailCatalogEntry {
        std::string detail_id;
        std::string parent_region;
        std::string status;
        size_t accepted_count {0};
        size_t protected_count {0};
        size_t low_confidence_count {0};
    };
    using DetailCatalog = std::vector<DetailCatalogEntry>;
    DetailCatalog catalog() const {
        DetailCatalog result;
        result.reserve(regions.size());
        for (const auto& region : regions)
            result.push_back({region.detail_id, region.parent_region, region.status,
                region.accepted_faces.size(), region.protected_faces.size(), region.low_confidence_faces.size()});
        return result;
    }
    std::vector<std::string> detail_ids(const std::string& parent = {}) const {
        std::vector<std::string> result;
        for (const auto& region : regions)
            if (parent.empty() || region.parent_region == parent) result.push_back(region.detail_id);
        return result;
    }
    struct Match {
        AI::SurfaceSelectionPersistence::SelectionState selection;
        size_t selected {0};
        size_t protected_count {0};
        size_t low_confidence {0};
    };
    Match select(const std::string& detail_id,
                 const AI::SurfaceSelectionPersistence::SelectionState& state,
                 bool include_protected = false) const {
        Match result;
        if (state.selected.size() != face_count) return result;
        result.selection = state;
        const auto it = std::find_if(regions.begin(), regions.end(), [&](const SecondaryRegion& region) {
            return region.detail_id == detail_id;
        });
        if (it == regions.end()) return result;
        std::set<size_t> protected_set(it->protected_faces.begin(), it->protected_faces.end());
        std::set<size_t> low_set(it->low_confidence_faces.begin(), it->low_confidence_faces.end());
        std::vector<size_t> candidate_faces = it->accepted_faces;
        if (include_protected) {
            candidate_faces.insert(candidate_faces.end(), it->protected_faces.begin(), it->protected_faces.end());
            candidate_faces.insert(candidate_faces.end(), it->low_confidence_faces.begin(), it->low_confidence_faces.end());
            std::sort(candidate_faces.begin(), candidate_faces.end());
            candidate_faces.erase(std::unique(candidate_faces.begin(), candidate_faces.end()), candidate_faces.end());
        }
        if (candidate_faces.empty()) {
            result.protected_count = it->protected_faces.size();
            result.low_confidence = it->low_confidence_faces.size();
            return result;
        }
        auto& next = result.selection;
        next.selected.assign(face_count, 0); next.foreground.assign(face_count, 0); next.domain.assign(face_count, 0);
        for (size_t face : candidate_faces) {
            // The explicit preview mode is a diagnostic view: it may display
            // protected/low-confidence evidence without changing the stored
            // protection mask or granting material-write permission.
            if (!include_protected &&
                ((face < state.protected_faces.size() && state.protected_faces[face]) ||
                 protected_set.count(face))) {
                ++result.protected_count; continue;
            }
            if (low_set.count(face) && !include_protected) { ++result.low_confidence; continue; }
            next.selected[face] = next.foreground[face] = next.domain[face] = 1;
            ++result.selected;
        }
        if (!result.selected) result.selection = state;
        return result;
    }
    nlohmann::json encode() const {
        nlohmann::json details = nlohmann::json::array();
        for (const auto& region : regions) details.push_back({
            {"detail_id", region.detail_id}, {"parent_region", region.parent_region},
            {"source_module", region.source_module}, {"subject_id", region.subject_id},
            {"accepted_faces", region.accepted_faces}, {"protected_faces", region.protected_faces},
            {"low_confidence_faces", region.low_confidence_faces}, {"view_support", region.view_support},
            {"confidence", region.confidence}, {"status", region.status}, {"evidence_sha256", region.evidence_sha256}});
        return {{"schema", "orca.secondary-region-evidence/v1"}, {"geometry_id", geometry_id},
            {"source_sha256", source_sha256}, {"runtime_identity", runtime_identity},
            {"face_count", face_count}, {"regions", std::move(details)}};
    }
    static std::shared_ptr<const SecondaryRegionEvidence> decode(const nlohmann::json& doc,
        const std::string& geometry, const std::string& source, size_t faces, const std::string& runtime,
        std::string& error) {
        try {
            if (doc.is_object() && doc.value("schema", std::string {}) == "orca.region-proposal-evidence/v1") {
                if (doc.value("source", nlohmann::json::object()).value("glb_sha256", std::string {}) != source ||
                    doc.value("source", nlohmann::json::object()).value("geometry_sha256", std::string {}) != geometry ||
                    doc.value("authorization", nlohmann::json::object()).value("candidate_slot", nlohmann::json()) != nullptr ||
                    doc.value("authorization", nlohmann::json::object()).value("material_write_authorized", false) ||
                    doc.value("authorization", nlohmann::json::object()).value("material_tree_changed", false))
                    throw std::runtime_error("Provider evidence is not read-only or has incompatible source identity");
                if (!doc.at("records").is_array()) throw std::runtime_error("Provider evidence records are missing");
                auto result = std::make_shared<SecondaryRegionEvidence>();
                result->geometry_id = geometry; result->source_sha256 = source; result->runtime_identity = runtime; result->face_count = faces;
                std::map<std::string, SecondaryRegion> merged;
                auto detail_for = [](const nlohmann::json& row) {
                    const std::string module = row.value("module", std::string {});
                    const std::string region = row.value("region", std::string {});
                    const std::string eye = row.value("eye_side", std::string {});
                    const bool left_eye = eye == "left" || eye == "l" || eye.find("left") != std::string::npos;
                    if (module == "eyebrow") return left_eye ? std::string("lb") : std::string("rb");
                    if (module == "eyes") {
                        if (region == "iris" || row.value("semantic_label", std::string {}) == "iris") return std::string("iris");
                        return left_eye ? std::string("le") : std::string("re");
                    }
                    if (module == "upper-lower-lip") return region.find("upper") != std::string::npos ? std::string("ulip") : std::string("llip");
                    if (module == "lip-line-corner") return std::string("lip-line-corner");
                    if (module == "teeth-oral") return region == "teeth" || row.value("semantic_label", std::string {}) == "teeth" ? std::string("teeth") : std::string("imouth");
                    if (module == "nose") return std::string("nose");
                    if (module == "hair-ear") return region == "ear" || row.value("semantic_label", std::string {}) == "ear" ? std::string("ear") : std::string("hair");
                    if (module == "accessories") return std::string("accessories");
                    throw std::runtime_error("Provider module is unsupported");
                };
                auto parent_for = [](const std::string& id) {
                    if (id == "hair" || id == "ear") return std::string("hair");
                    if (id == "face" || id == "neck" || id == "nose") return std::string("skin");
                    if (id == "le" || id == "re" || id == "iris" || id == "lb" || id == "rb") return std::string("eyes");
                    if (id == "cloth" || id == "accessories") return std::string("clothes");
                    return std::string("lips");
                };
                for (const auto& row : doc.at("records")) {
                    if (!row.is_object()) throw std::runtime_error("Malformed provider evidence record");
                    const auto face_value = row.value("source_face_id", nlohmann::json());
                    if (!face_value.is_number_integer() || face_value.get<int64_t>() < 0) throw std::runtime_error("Provider evidence face id is invalid");
                    const size_t face = face_value.get<size_t>();
                    if (face >= faces) throw std::runtime_error("Provider evidence face id is out of range");
                    const auto module = row.value("module", std::string {});
                    if ((module == "eyebrow" || module == "eyes") && row.value("eye_side", std::string {}).empty())
                        throw std::runtime_error("Provider eye-side identity is missing");
                    const std::string id = detail_for(row);
                    if (!SecondaryRegion::supported_id(id)) throw std::runtime_error("Provider detail mapping is unsupported");
                    auto& detail = merged[id];
                    detail.detail_id = id; detail.parent_region = parent_for(id);
                    detail.source_module = row.value("module", std::string {});
                    detail.subject_id = row.value("person_instance", std::string {});
                    detail.confidence = std::max(detail.confidence, row.value("confidence", 0.f));
                    detail.status = row.value("offline_candidate_authorized", false) ? "PASS" : "PROTECTED_CONTINUE";
                    const auto evidence_hash = row.value("evidence_sha256", std::string(64, '0'));
                    detail.evidence_sha256 = AI::SurfaceSelectionPersistence::detail::valid_fingerprint(evidence_hash) ? evidence_hash : std::string(64, '0');
                    const auto views = row.value("view_ids", nlohmann::json::array());
                    if (views.is_array()) for (const auto& view : views) detail.view_support.push_back(view.get<std::string>());
                    if (row.value("offline_candidate_authorized", false)) detail.accepted_faces.push_back(face);
                    else detail.protected_faces.push_back(face);
                }
                for (auto& item : merged) {
                    auto& detail = item.second;
                    auto normalize = [](std::vector<size_t>& values) { std::sort(values.begin(), values.end()); values.erase(std::unique(values.begin(), values.end()), values.end()); };
                    normalize(detail.accepted_faces); normalize(detail.protected_faces);
                    std::vector<size_t> overlap;
                    std::set_intersection(detail.accepted_faces.begin(), detail.accepted_faces.end(), detail.protected_faces.begin(), detail.protected_faces.end(), std::back_inserter(overlap));
                    if (!overlap.empty()) throw std::runtime_error("Provider evidence maps one face to accepted and protected detail");
                    if (!detail.protected_faces.empty()) detail.status = "PROTECTED_CONTINUE";
                    std::sort(detail.view_support.begin(), detail.view_support.end()); detail.view_support.erase(std::unique(detail.view_support.begin(), detail.view_support.end()), detail.view_support.end());
                    result->regions.push_back(std::move(detail));
                }
                if (!result->valid()) throw std::runtime_error("Malformed provider evidence mapping");
                error.clear(); return result;
            }
            if (!doc.is_object()) throw std::runtime_error("Secondary evidence JSON must be an object");
            const auto schema = doc.value("schema", std::string {});
            if (schema == "orca.readonly-render-package/v1")
                throw std::runtime_error("六视角渲染包尚未包含二级语义区域，不能直接导入；请先生成与当前模型匹配的 Provider 二级证据");
            if (schema != "orca.secondary-region-evidence/v1")
                throw std::runtime_error("不支持的二级证据 schema");
            if (doc.value("geometry_id", std::string {}) != geometry)
                throw std::runtime_error("Secondary evidence geometry identity mismatch");
            if (doc.value("source_sha256", std::string {}) != source)
                throw std::runtime_error("Secondary evidence source hash mismatch");
            if (doc.value("runtime_identity", std::string {}) != runtime)
                throw std::runtime_error("Secondary evidence runtime identity mismatch");
            if (doc.value("face_count", size_t(0)) != faces)
                throw std::runtime_error("Secondary evidence face count mismatch");
            if (!doc.contains("regions") || !doc.at("regions").is_array() || doc.at("regions").size() > 128)
                throw std::runtime_error("Secondary evidence regions are missing or invalid");
            auto result = std::make_shared<SecondaryRegionEvidence>();
            result->geometry_id = geometry; result->source_sha256 = source; result->runtime_identity = runtime; result->face_count = faces;
            for (const auto& value : doc.at("regions")) {
                SecondaryRegion region;
                region.detail_id = value.at("detail_id").get<std::string>();
                region.parent_region = value.at("parent_region").get<std::string>();
                region.source_module = value.at("source_module").get<std::string>();
                region.subject_id = value.value("subject_id", std::string {});
                region.accepted_faces = value.at("accepted_faces").get<std::vector<size_t>>();
                region.protected_faces = value.at("protected_faces").get<std::vector<size_t>>();
                region.low_confidence_faces = value.at("low_confidence_faces").get<std::vector<size_t>>();
                region.view_support = value.at("view_support").get<std::vector<std::string>>();
                region.confidence = value.at("confidence").get<float>();
                region.status = value.at("status").get<std::string>();
                region.evidence_sha256 = value.at("evidence_sha256").get<std::string>();
                result->regions.push_back(std::move(region));
            }
            if (!result->valid() || !result->compatible(geometry, source, faces, runtime)) throw std::runtime_error("Malformed secondary evidence");
            error.clear(); return result;
        } catch (const std::exception& e) { error = e.what(); return {}; }
    }
};

namespace SecondaryRegionEvidenceCache {
// The v6 provider replay contains one complete record per projected sample;
// the current seven-module evidence package is about 143 MB. Keep a bounded
// limit while allowing that valid package to be imported read-only.
inline constexpr uintmax_t maximum_bytes = 256ULL * 1024 * 1024;
inline nlohmann::json save(const SecondaryRegionEvidence& evidence, const boost::filesystem::path& cache,
                           std::string& error) {
    boost::filesystem::path temporary;
    try {
        if (!evidence.valid()) throw std::runtime_error("Invalid secondary evidence");
        boost::filesystem::create_directories(cache);
        temporary = cache / boost::filesystem::unique_path("secondary-%%%%-%%%%-%%%%.tmp");
        { boost::filesystem::ofstream output(temporary, std::ios::binary); output << evidence.encode(); output.close();
          if (!output.good()) throw std::runtime_error("Could not write secondary evidence"); }
        const auto hash = AI::model_artifact_sha256(temporary);
        if (!AI::SurfaceSelectionPersistence::detail::valid_fingerprint(hash)) throw std::runtime_error("Could not hash secondary evidence");
        const auto destination = cache / (hash + ".json");
        if (boost::filesystem::exists(destination) && AI::model_artifact_sha256(destination) == hash)
            boost::filesystem::remove(temporary);
        else {
            if (boost::filesystem::exists(destination)) boost::filesystem::remove(destination);
            boost::filesystem::rename(temporary, destination);
        }
        error.clear();
        return {{"schema", "orca.secondary-region-reference/v1"}, {"sha256", hash},
            {"geometry_id", evidence.geometry_id}, {"source_sha256", evidence.source_sha256},
            {"face_count", evidence.face_count}, {"runtime_identity", evidence.runtime_identity}};
    } catch (const std::exception& e) {
        error = e.what();
        if (!temporary.empty()) { boost::system::error_code ignored; boost::filesystem::remove(temporary, ignored); }
        return nlohmann::json::object();
    }
}
inline std::shared_ptr<const SecondaryRegionEvidence> load(const nlohmann::json& reference,
    const boost::filesystem::path& cache, const std::string& geometry, const std::string& source,
    size_t faces, const std::string& runtime, std::string& error) {
    try {
        if (!reference.is_object() || reference.value("schema", std::string {}) != "orca.secondary-region-reference/v1")
            throw std::runtime_error("Secondary evidence reference schema is invalid");
        const auto hash = reference.at("sha256").get<std::string>();
        if (!AI::SurfaceSelectionPersistence::detail::valid_fingerprint(hash) ||
            reference.value("geometry_id", std::string {}) != geometry || reference.value("source_sha256", std::string {}) != source ||
            reference.value("face_count", size_t(0)) != faces || reference.value("runtime_identity", std::string {}) != runtime)
            throw std::runtime_error("Secondary evidence reference is incompatible");
        const auto file = cache / (hash + ".json");
        if (!boost::filesystem::is_regular_file(file) || boost::filesystem::file_size(file) > maximum_bytes ||
            AI::model_artifact_sha256(file) != hash) throw std::runtime_error("Secondary evidence cache hash is invalid");
        boost::filesystem::ifstream input(file, std::ios::binary);
        return SecondaryRegionEvidence::decode(nlohmann::json::parse(input, nullptr, false), geometry, source, faces, runtime, error);
    } catch (const std::exception& e) { error = e.what(); return {}; }
}
} // namespace SecondaryRegionEvidenceCache
} // namespace Slic3r::GUI
