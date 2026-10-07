#pragma once

#include "slic3r/AI/ModelGeneration/SemanticColoring/SemanticColoring.hpp"
#include "slic3r/GUI/AI/Model/SurfaceSelectionState.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem.hpp>
#include <filesystem>
#include <fstream>
#include <memory>

namespace Slic3r::GUI {

// Selection evidence is independent of the display palette and automatic paint.
struct SemanticRegionEvidence {
    std::string geometry_id, source_content_id, analysis_signature;
    std::string body_identity, face_identity, runtime_identity;
    std::vector<AI::SemanticColoring::Label> labels;
    std::vector<float> confidence;

    bool compatible(const std::string& geometry, size_t faces, const std::string& runtime) const {
        return !geometry.empty() && geometry_id == geometry && labels.size() == faces &&
            confidence.size() == faces && !runtime.empty() && runtime_identity == runtime;
    }
    bool valid() const {
        namespace SC = AI::SemanticColoring;
        if (!AI::SurfaceSelectionPersistence::detail::valid_fingerprint(geometry_id) ||
            !AI::SurfaceSelectionPersistence::detail::valid_fingerprint(source_content_id) ||
            analysis_signature.empty() || body_identity.empty() || face_identity.empty() ||
            runtime_identity.empty() || labels.empty() || confidence.size() != labels.size()) return false;
        bool reliable = false;
        for (size_t face = 0; face < labels.size(); ++face) {
            if (size_t(labels[face]) >= SC::label_count || !std::isfinite(confidence[face]) ||
                confidence[face] < 0.f || confidence[face] > 1.f) return false;
            reliable |= labels[face] != SC::Label::Unknown && labels[face] != SC::Label::Background &&
                confidence[face] >= SC::minimum_confidence;
        }
        return reliable;
    }
    static std::shared_ptr<const SemanticRegionEvidence> from_analysis(
        const AI::SemanticColoring::Analysis& analysis, const std::string& runtime) {
        if (analysis.canceled || !analysis.error.empty() || !analysis.person_detected) return {};
        auto evidence = std::make_shared<SemanticRegionEvidence>();
        evidence->geometry_id = analysis.geometry_id;
        evidence->source_content_id = analysis.content_id;
        evidence->analysis_signature = analysis.signature;
        evidence->body_identity = analysis.body_identity;
        evidence->face_identity = analysis.face_identity;
        evidence->runtime_identity = runtime;
        evidence->labels = analysis.face_labels;
        evidence->confidence = analysis.face_confidence;
        return evidence->valid() ? evidence : nullptr;
    }
    nlohmann::json encode() const {
        std::vector<unsigned int> values;
        values.reserve(labels.size());
        for (auto label : labels) values.push_back(unsigned(label));
        return {{"schema", "orca.semantic-region-evidence/v1"},
            {"pipeline", AI::SemanticColoring::pipeline_version}, {"geometry_id", geometry_id},
            {"face_count", labels.size()}, {"source_content_id", source_content_id},
            {"analysis_signature", analysis_signature}, {"body_identity", body_identity},
            {"face_identity", face_identity}, {"runtime_identity", runtime_identity},
            {"labels", values}, {"confidence", confidence}};
    }
    static std::shared_ptr<const SemanticRegionEvidence> decode(const nlohmann::json& doc,
        const std::string& geometry, size_t faces, const std::string& runtime, std::string& error) {
        try {
            if (!doc.is_object() || doc.at("schema") != "orca.semantic-region-evidence/v1" ||
                doc.at("pipeline") != AI::SemanticColoring::pipeline_version || doc.at("geometry_id") != geometry ||
                doc.at("face_count").get<size_t>() != faces || doc.at("runtime_identity") != runtime ||
                !doc.at("labels").is_array() || doc.at("labels").size() != faces ||
                !doc.at("confidence").is_array() || doc.at("confidence").size() != faces)
                throw std::runtime_error("Region evidence identity or array length mismatch");
            auto evidence = std::make_shared<SemanticRegionEvidence>();
            evidence->geometry_id = geometry;
            evidence->runtime_identity = runtime;
            evidence->source_content_id = doc.at("source_content_id").get<std::string>();
            evidence->analysis_signature = doc.at("analysis_signature").get<std::string>();
            evidence->body_identity = doc.at("body_identity").get<std::string>();
            evidence->face_identity = doc.at("face_identity").get<std::string>();
            for (const auto& value : doc.at("labels")) {
                size_t label = 0;
                if (!AI::SurfaceSelectionPersistence::detail::read_size(value, label) ||
                    label >= AI::SemanticColoring::label_count) throw std::runtime_error("Invalid region label");
                evidence->labels.push_back(AI::SemanticColoring::Label(label));
            }
            evidence->confidence = doc.at("confidence").get<std::vector<float>>();
            if (!evidence->valid()) throw std::runtime_error("Region evidence has no reliable labels or is malformed");
            error.clear();
            return evidence;
        } catch (const std::exception& e) { error = e.what(); return {}; }
    }

    // Compute first, so zero matches never destroy a user's selection/history.
    struct Match {
        AI::SurfaceSelectionPersistence::SelectionState selection;
        size_t selected {0}, protected_count {0}, low_confidence {0};
    };
    Match select(const std::string& region, const AI::SurfaceSelectionPersistence::SelectionState& state) const {
        namespace SC = AI::SemanticColoring;
        Match result;
        if (state.selected.size() != labels.size()) return result;
        result.selection = state;
        auto& next = result.selection;
        next.selected.assign(labels.size(), 0);
        next.foreground.assign(labels.size(), 0);
        next.domain.assign(labels.size(), 0);
        for (size_t face = 0; face < labels.size(); ++face) {
            const auto label = labels[face];
            const bool matches = region == "hair" ? label == SC::Label::Hair :
                region == "skin" ? label == SC::Label::FaceSkin || label == SC::Label::BodySkin :
                region == "eyes" ? label == SC::Label::EyeSclera || label == SC::Label::Iris || label == SC::Label::Eyebrow :
                region == "lips" ? label == SC::Label::Lips : region == "clothes" && label == SC::Label::Clothes;
            if (!matches) continue;
            if (face < state.protected_faces.size() && state.protected_faces[face]) { ++result.protected_count; continue; }
            if (confidence[face] < SC::minimum_confidence) { ++result.low_confidence; continue; }
            next.selected[face] = next.foreground[face] = next.domain[face] = 1;
            ++result.selected;
        }
        if (!result.selected) result.selection = state;
        return result;
    }
};

namespace SemanticRegionEvidenceCache {
inline constexpr uintmax_t maximum_bytes = 128ULL * 1024 * 1024;

inline nlohmann::json save(const SemanticRegionEvidence& evidence, const boost::filesystem::path& cache,
                           std::string& error) {
    boost::filesystem::path temporary;
    try {
        if (!evidence.valid()) throw std::runtime_error("Invalid region evidence");
        boost::filesystem::create_directories(cache);
        temporary = cache / boost::filesystem::unique_path("region-%%%%-%%%%-%%%%.tmp");
        { boost::filesystem::ofstream output(temporary, std::ios::binary);
          output << evidence.encode(); output.close();
          if (!output.good()) throw std::runtime_error("Could not write region evidence"); }
        const auto hash = AI::model_artifact_sha256(temporary);
        if (hash.empty()) throw std::runtime_error("Could not hash region evidence");
        const auto destination = cache / (hash + ".json");
        if (boost::filesystem::exists(destination) && AI::model_artifact_sha256(destination) == hash)
            boost::filesystem::remove(temporary);
        else {
            // A corrupt content-addressed entry may be replaced, never another model file.
            if (boost::filesystem::exists(destination)) boost::filesystem::remove(destination);
            boost::filesystem::rename(temporary, destination);
        }
        error.clear();
        return {{"schema", "orca.semantic-region-reference/v1"}, {"sha256", hash},
            {"geometry_id", evidence.geometry_id}, {"face_count", evidence.labels.size()},
            {"runtime_identity", evidence.runtime_identity}};
    } catch (const std::exception& e) {
        error = e.what();
        if (!temporary.empty()) { boost::system::error_code ignored; boost::filesystem::remove(temporary, ignored); }
        return nlohmann::json::object();
    }
}
inline std::shared_ptr<const SemanticRegionEvidence> load(const nlohmann::json& reference,
    const boost::filesystem::path& cache, const std::string& geometry, size_t faces,
    const std::string& runtime, std::string& error) {
    try {
        const auto hash = reference.at("sha256").get<std::string>();
        if (reference.at("schema") != "orca.semantic-region-reference/v1" ||
            !AI::SurfaceSelectionPersistence::detail::valid_fingerprint(hash) ||
            reference.at("geometry_id") != geometry || reference.at("face_count").get<size_t>() != faces ||
            reference.at("runtime_identity") != runtime)
            throw std::runtime_error("Region cache reference is incompatible");
        const auto file = cache / (hash + ".json");
        if (boost::filesystem::file_size(file) > maximum_bytes || AI::model_artifact_sha256(file) != hash)
            throw std::runtime_error("Region cache file hash is invalid");
        boost::filesystem::ifstream input(file, std::ios::binary);
        return SemanticRegionEvidence::decode(nlohmann::json::parse(input, nullptr, false), geometry, faces, runtime, error);
    } catch (const std::exception& e) { error = e.what(); return {}; }
}
} // namespace SemanticRegionEvidenceCache
} // namespace Slic3r::GUI
