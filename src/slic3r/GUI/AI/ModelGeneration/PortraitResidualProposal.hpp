#pragma once

#include "slic3r/AI/Contracts/LocalPrintColorResult.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <set>
#include <map>
#include <stdexcept>
#include <string>
#include <iterator>
#include <optional>
#include <vector>

namespace Slic3r::GUI::PortraitResidual {

inline constexpr const char* schema = "orca.portrait-residual-proposal/v1";
inline constexpr const char* policy_version = "portrait-residual-local-v2-verified-support";

struct Identity {
    std::string source_sha256, geometry_id;
    size_t face_count {0};
    std::string evidence_sha256, shape_lock_sha256, runtime_sha256, policy_sha256;

    bool valid() const {
        return face_count > 0 && face_count <= 2000000 &&
            AI::is_lowercase_sha256(source_sha256) && AI::is_lowercase_sha256(geometry_id) &&
            AI::is_lowercase_sha256(evidence_sha256) && AI::is_lowercase_sha256(shape_lock_sha256) &&
            AI::is_lowercase_sha256(runtime_sha256) && AI::is_lowercase_sha256(policy_sha256);
    }
    bool compatible(const Identity& other) const {
        return source_sha256 == other.source_sha256 && geometry_id == other.geometry_id &&
            face_count == other.face_count && evidence_sha256 == other.evidence_sha256 &&
            shape_lock_sha256 == other.shape_lock_sha256 && runtime_sha256 == other.runtime_sha256 &&
            policy_sha256 == other.policy_sha256;
    }
};

struct Proposal {
    std::string unit_id, kind, status;
    // These fields are copied from the offline audit so the host can repeat
    // parent/subject checks at apply time instead of trusting a color-only
    // proposal.
    std::string subject_id, parent_region;
    double confidence {0};
    size_t view_support {0};
    std::vector<size_t> source_faces, rejected_faces;
    std::vector<std::array<size_t, 3>> leaf_keys;
    std::vector<std::string> reasons;
    size_t additional_triangles {0};
    std::optional<std::array<float, 3>> target_rgb;
    std::optional<size_t> target_slot;

    bool actionable() const {
        return status == "PROPOSED" && (kind == "skin" || kind == "hair" || kind == "cloth") &&
            !source_faces.empty() && confidence >= .85 && view_support >= 2;
    }
    bool actionable_with_target() const { return actionable() && target_rgb.has_value(); }
};

struct Document {
    Identity identity;
    std::string request_sha256;
    std::string status;
    size_t remaining_triangle_budget {0};
    std::vector<Proposal> proposals, rejected;
    nlohmann::json statistics = nlohmann::json::object();

    static void require(bool value, const char* message) {
        if (!value) throw std::invalid_argument(message);
    }
    static bool identifier(const std::string& value) {
        return !value.empty() && value.size()<=128 && std::all_of(value.begin(),value.end(),[](unsigned char c) {
            return std::isalnum(c) || c=='_' || c=='-' || c=='.' || c==':' ;
        });
    }
    static bool parent_matches(const Proposal& record) {
        if (record.kind=="skin") return record.parent_region=="skin" || record.parent_region=="face" ||
            record.parent_region=="neck" || record.parent_region=="body" || record.parent_region=="arm";
        if (record.kind=="hair") return record.parent_region=="hair";
        if (record.kind=="cloth") return record.parent_region=="cloth";
        return true;
    }
    static bool hard_reason(const std::string& reason) {
        static const std::set<std::string> reasons {"FROZEN_SHAPE_LOCK","CROSS_SUBJECT","CROSS_EYE_SIDE", "CROSS_PARENT",
            "UNBOUND_PARENT","MIXED_FACE_UNCONFIRMED","IDENTITY_DRIFT","SOURCE_MAPPING_INVALID","FACE_OUT_OF_RANGE"};
        return reasons.count(reason)!=0;
    }
    static void validate_faces(const std::vector<size_t>& faces, size_t count, const char* message) {
        require(std::is_sorted(faces.begin(), faces.end()) &&
            std::adjacent_find(faces.begin(), faces.end()) == faces.end(), message);
        for (const size_t face : faces) require(face < count, message);
    }
    static void validate_leaves(const std::vector<std::array<size_t, 3>>& leaves,
                                const std::vector<size_t>& source_faces, size_t count) {
        std::array<size_t, 3> previous{};
        bool has_previous = false;
        for (const auto& leaf : leaves) {
            const size_t face = leaf[0], depth = leaf[1], path = leaf[2];
            require(face < count && depth <= 4 && path < (size_t(1) << (2 * depth)),
                "Residual leaf key is out of range.");
            require(std::binary_search(source_faces.begin(), source_faces.end(), face),
                "Residual leaf lies outside its source faces.");
            require(!has_previous || previous < leaf, "Residual leaf keys are not ordered.");
            previous = leaf;
            has_previous = true;
        }
        for (const auto& leaf:leaves) for (size_t depth=0;depth<leaf[1];++depth) {
            const std::array<size_t,3> ancestor {leaf[0],depth,leaf[2]>>(2*(leaf[1]-depth))};
            require(!std::binary_search(leaves.begin(),leaves.end(),ancestor),"Residual leaf ancestry overlaps.");
        }
    }
    static std::vector<size_t> decode_faces(const nlohmann::json& value, size_t count, const char* message) {
        require(value.is_array(), message);
        std::vector<size_t> result;
        result.reserve(value.size());
        for (const auto& item : value) {
            require(item.is_number_unsigned() || (item.is_number_integer() && item.get<int64_t>() >= 0), message);
            result.push_back(item.get<size_t>());
        }
        validate_faces(result, count, message);
        return result;
    }
    static std::vector<std::array<size_t, 3>> decode_leaves(const nlohmann::json& value, size_t count) {
        require(value.is_array(), "Invalid residual leaf keys.");
        std::vector<std::array<size_t, 3>> result;
        for (const auto& item : value) {
            require(item.is_array() && item.size() == 3, "Invalid residual leaf key.");
            const auto face = item.at(0).get<size_t>(), depth = item.at(1).get<size_t>(), path = item.at(2).get<size_t>();
            require(face < count && depth <= 4 && path < (size_t(1) << (2 * depth)), "Residual leaf key is out of range.");
            result.push_back({face, depth, path});
        }
        require(std::is_sorted(result.begin(), result.end()) &&
            std::adjacent_find(result.begin(), result.end()) == result.end(), "Residual leaf keys are not ordered.");
        return result;
    }
    static Identity decode_identity(const nlohmann::json& value) {
        require(value.is_object(), "Missing residual identity.");
        Identity result;
        result.source_sha256 = value.at("source_sha256").get<std::string>();
        result.geometry_id = value.at("geometry_id").get<std::string>();
        result.face_count = value.at("face_count").get<size_t>();
        result.evidence_sha256 = value.at("evidence_sha256").get<std::string>();
        result.shape_lock_sha256 = value.at("shape_lock_sha256").get<std::string>();
        result.runtime_sha256 = value.at("runtime_sha256").get<std::string>();
        result.policy_sha256 = value.at("policy_sha256").get<std::string>();
        require(result.valid(), "Invalid residual identity.");
        return result;
    }
    static Proposal decode_proposal(const nlohmann::json& value, const Identity& identity) {
        require(value.is_object(), "Invalid residual proposal record.");
        Proposal result;
        result.unit_id = value.at("unit_id").get<std::string>();
        result.kind = value.at("proposal").get<std::string>();
        result.status = value.at("status").get<std::string>();
        result.subject_id = value.value("subject_id", std::string());
        result.parent_region = value.value("parent_region", std::string());
        result.confidence = value.at("confidence").get<double>();
        result.view_support = value.at("view_support").get<size_t>();
        result.source_faces = decode_faces(value.at("source_faces"), identity.face_count, "Invalid residual source faces.");
        result.rejected_faces = decode_faces(value.at("rejected_faces"), identity.face_count, "Invalid residual rejected faces.");
        result.leaf_keys = decode_leaves(value.value("leaf_keys", nlohmann::json::array()), identity.face_count);
        result.reasons = value.value("reasons", std::vector<std::string>{});
        result.additional_triangles = value.value("additional_triangles", size_t(0));
        if (value.contains("target_slot")) result.target_slot=value.at("target_slot").get<size_t>();
        if (value.contains("target_rgb")) {
            require(value.at("target_rgb").is_array() && value.at("target_rgb").size() == 3,
                "Invalid residual target color.");
            std::array<float, 3> color {};
            for (size_t index = 0; index < 3; ++index) {
                color[index] = value.at("target_rgb").at(index).get<float>();
                require(std::isfinite(color[index]) && color[index] >= 0.f && color[index] <= 1.f,
                    "Invalid residual target color.");
            }
            result.target_rgb = color;
        }
        require(!result.unit_id.empty() && result.unit_id.size() <= 128 &&
            (result.kind == "skin" || result.kind == "hair" || result.kind == "cloth" ||
             result.kind == "preserve" || result.kind == "subdivide"), "Invalid residual proposal kind.");
        require(result.status == "PROPOSED" || result.status == "REJECTED" || result.status == "UNAPPLIED_BUDGET",
            "Invalid residual proposal status.");
        require(std::isfinite(result.confidence) && result.confidence >= 0 && result.confidence <= 1 && result.view_support <= 16,
            "Invalid residual proposal confidence.");
        std::vector<size_t> overlap;
        std::set_intersection(result.source_faces.begin(), result.source_faces.end(),
            result.rejected_faces.begin(), result.rejected_faces.end(), std::back_inserter(overlap));
        require(overlap.empty(), "Residual accepted/rejected overlap.");
        for (const auto& leaf : result.leaf_keys)
            require(std::binary_search(result.source_faces.begin(), result.source_faces.end(), leaf[0]),
                "Residual leaf lies outside its source faces.");
        for (const auto& reason : result.reasons)
            require(!reason.empty() && reason.size() <= 96, "Invalid residual proposal reason.");
        if (result.kind == "subdivide") require(result.additional_triangles > 0, "Invalid residual subdivision budget.");
        return result;
    }
    void validate() const {
        require(identity.valid(), "Invalid residual document identity.");
        require(AI::is_lowercase_sha256(request_sha256),"Missing residual request identity.");
        require(status == "READY" || status == "PROTECTED_R9" || status == "UNAVAILABLE" ||
            status == "CANCELLED" || status == "APPLIED", "Invalid residual document status.");
        require(remaining_triangle_budget <= 20000, "Residual triangle budget exceeds policy.");
        std::set<std::string> ids;
        std::set<size_t> claimed;
        const auto validate_record=[](const Proposal& record) {
            require(identifier(record.unit_id),"Invalid residual unit identifier.");
            require(record.kind=="skin" || record.kind=="hair" || record.kind=="cloth" || record.kind=="preserve" || record.kind=="subdivide",
                "Invalid residual proposal kind.");
            require(record.status=="PROPOSED" || record.status=="REJECTED" || record.status=="UNAPPLIED_BUDGET","Invalid residual status.");
            require(std::isfinite(record.confidence) && record.confidence>=0 && record.confidence<=1 && record.view_support<=16,
                "Invalid residual support.");
            for (const auto& reason:record.reasons) require(identifier(reason),"Invalid residual reason.");
            if (record.status=="PROPOSED" && record.kind!="preserve" && record.kind!="subdivide")
                require(record.actionable() && identifier(record.subject_id) && parent_matches(record) &&
                    std::none_of(record.reasons.begin(),record.reasons.end(),hard_reason),"Unsafe residual proposal.");
            if (record.target_rgb) for (float channel:*record.target_rgb)
                require(std::isfinite(channel) && channel>=0 && channel<=1,"Invalid residual color.");
            if (record.target_slot) require(*record.target_slot<64,"Invalid residual palette slot.");
            if (record.kind=="subdivide") require(record.additional_triangles>0,"Invalid residual subdivision.");
        };
        for (const auto& record : proposals) {
            validate_record(record);
            require(ids.insert(record.unit_id).second, "Duplicate residual unit.");
            validate_faces(record.source_faces, identity.face_count, "Invalid residual source faces.");
            validate_faces(record.rejected_faces, identity.face_count, "Invalid residual rejected faces.");
            std::vector<size_t> overlap;
            std::set_intersection(record.source_faces.begin(), record.source_faces.end(),
                record.rejected_faces.begin(), record.rejected_faces.end(), std::back_inserter(overlap));
            require(overlap.empty(), "Residual accepted/rejected overlap.");
            validate_leaves(record.leaf_keys, record.source_faces, identity.face_count);
            if (record.kind != "preserve") for (const size_t face : record.source_faces)
                require(claimed.insert(face).second, "Overlapping residual proposals.");
        }
        for (const auto& record : rejected) {
            validate_record(record);
            require(ids.insert(record.unit_id).second, "Duplicate residual unit.");
            require(record.status == "REJECTED", "Rejected residual is not marked rejected.");
            validate_faces(record.source_faces, identity.face_count, "Invalid rejected residual faces.");
            validate_faces(record.rejected_faces, identity.face_count, "Invalid rejected residual rejected faces.");
            std::vector<size_t> overlap;
            std::set_intersection(record.source_faces.begin(), record.source_faces.end(),
                record.rejected_faces.begin(), record.rejected_faces.end(), std::back_inserter(overlap));
            require(overlap.empty(), "Residual accepted/rejected overlap.");
            validate_leaves(record.leaf_keys, record.source_faces, identity.face_count);
        }
    }
    static Document decode(const nlohmann::json& value, const Identity* expected = nullptr) {
        require(value.is_object() && value.at("schema") == schema && value.at("policy_version") == policy_version,
            "Unsupported residual proposal schema.");
        Document result;
        result.identity = decode_identity(value);
        result.request_sha256=value.at("request_sha256").get<std::string>();
        if (expected) require(result.identity.compatible(*expected), "Residual proposal identity drift.");
        result.status = value.at("status").get<std::string>();
        result.remaining_triangle_budget = value.value("remaining_triangle_budget", size_t(0));
        for (const auto& item : value.value("proposals", nlohmann::json::array()))
            result.proposals.push_back(decode_proposal(item, result.identity));
        for (const auto& item : value.value("rejected", nlohmann::json::array()))
            result.rejected.push_back(decode_proposal(item, result.identity));
        result.statistics = value.value("statistics", nlohmann::json::object());
        result.validate();
        return result;
    }
    nlohmann::json encode() const {
        validate();
        const auto encode_record = [](const Proposal& record) {
            nlohmann::json leaves = nlohmann::json::array();
            for (const auto& leaf : record.leaf_keys) leaves.push_back({leaf[0], leaf[1], leaf[2]});
            nlohmann::json value = { {"unit_id", record.unit_id}, {"proposal", record.kind}, {"confidence", record.confidence},
                {"view_support", record.view_support}, {"source_faces", record.source_faces},
                {"leaf_keys", leaves}, {"rejected_faces", record.rejected_faces}, {"reasons", record.reasons},
                {"applied", false}, {"status", record.status} };
            if (!record.subject_id.empty()) value["subject_id"] = record.subject_id;
            if (!record.parent_region.empty()) value["parent_region"] = record.parent_region;
            if (record.additional_triangles) value["additional_triangles"] = record.additional_triangles;
            if (record.target_rgb) value["target_rgb"] = *record.target_rgb;
            if (record.target_slot) value["target_slot"] = *record.target_slot;
            return value;
        };
        nlohmann::json result = { {"schema", schema}, {"policy_version", policy_version}, {"status", status}, {"request_sha256",request_sha256},
            {"source_sha256", identity.source_sha256}, {"geometry_id", identity.geometry_id},
            {"face_count", identity.face_count}, {"evidence_sha256", identity.evidence_sha256},
            {"shape_lock_sha256", identity.shape_lock_sha256}, {"runtime_sha256", identity.runtime_sha256},
            {"policy_sha256", identity.policy_sha256}, {"remaining_triangle_budget", remaining_triangle_budget},
            {"proposals", nlohmann::json::array()}, {"rejected", nlohmann::json::array()}, {"statistics", statistics} };
        for (const auto& record : proposals) result["proposals"].push_back(encode_record(record));
        for (const auto& record : rejected) result["rejected"].push_back(encode_record(record));
        return result;
    }
    std::vector<Proposal> actionable() const {
        validate();
        std::vector<Proposal> result;
        for (const auto& record : proposals) if (record.actionable()) result.push_back(record);
        return result;
    }
    void bind_to_request(const nlohmann::json& request,const std::string& hash) const {
        validate();
        require(request_sha256==hash && identity.compatible(decode_identity(request)),"Residual request or edit state changed.");
        std::map<std::string,const nlohmann::json*> units;
        for(const auto& unit:request.at("units")) units.emplace(unit.at("unit_id").get<std::string>(),&unit);
        for(const auto& record:proposals) if(record.actionable()) {
            const auto found=units.find(record.unit_id);
            require(found!=units.end(),"Residual proposal has no source unit.");
            const auto& unit=*found->second;
            require(record.subject_id==unit.at("subject_id") && record.parent_region==unit.at("parent_region") &&
                record.kind==unit.at("proposal") && record.source_faces==unit.at("face_ids").get<std::vector<size_t>>() &&
                record.leaf_keys==unit.value("leaf_keys",std::vector<std::array<size_t,3>>{}) &&
                record.target_rgb && *record.target_rgb==unit.at("target_rgb").get<std::array<float,3>>() &&
                record.target_slot && *record.target_slot==unit.at("target_slot").get<size_t>(),
                "Residual proposal changed its verified unit or palette.");
        }
    }
};

} // namespace Slic3r::GUI::PortraitResidual
