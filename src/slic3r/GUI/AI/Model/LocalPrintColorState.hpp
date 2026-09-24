#pragma once

#include "slic3r/AI/Contracts/LocalPrintColorResult.hpp"
#include "LocalPrintRecipeProofState.hpp"
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace Slic3r::GUI::LocalPrintColorState {
using Json = nlohmann::json;

// Restore an already confirmed partition, not a cache of a current algorithm's
// computation. Callers first verify the source bytes/native face correspondence
// and supply a fresh material/process snapshot and native painting comparison.
// Failure never replaces the destination or mutates the saved version.
inline bool restore_confirmed(const AI::LocalPrintColorResult& saved, const AI::LocalPrintColorResult& current,
    bool native_paint_matches, AI::LocalPrintColorResult& destination, std::string& reason)
{
    reason.clear();
    if (!saved.valid(reason)) return false;
    for(const auto& target:saved.targets) if(target.recipe) {
        if(!LocalPrintRecipeProofState::valid(target,saved,reason)) return false;
        const auto fresh=std::find_if(current.targets.begin(),current.targets.end(),[&](const auto& candidate) {
            return candidate.recipe && candidate.recipe_proof && candidate.candidate_id==target.candidate_id &&
                candidate.recipe_proof->checksum==target.recipe_proof->checksum;
        });
        if(fresh==current.targets.end() || !LocalPrintRecipeProofState::valid(*fresh,current,reason)) {
            reason="A saved recipe confirmation requires fresh matching constraint and color evidence.";return false;
        }
    }
    auto changed = [&] { reason = "The saved confirmation no longer matches the current source, intent or materials."; return false; };
    if (!saved.confirmed || !native_paint_matches) return changed();
    if (saved.source_sha256 != current.source_sha256 || saved.geometry_id != current.geometry_id ||
        saved.face_count != current.face_count || saved.material_fingerprint != current.material_fingerprint ||
        saved.process_fingerprint != current.process_fingerprint || saved.requested_color_count != current.requested_color_count ||
        saved.mode != current.mode || saved.color_tolerance != current.color_tolerance ||
        saved.important_area_floor != current.important_area_floor || saved.user_overrides != current.user_overrides ||
        saved.physical_channels.size() != current.physical_channels.size() || saved.regions.size() != current.regions.size() ||
        saved.contrasts.size() != current.contrasts.size()) return changed();
    for (size_t i=0; i<saved.physical_channels.size(); ++i) {
        const auto& a=saved.physical_channels[i]; const auto& b=current.physical_channels[i];
        if (a.slot!=b.slot || a.display_color!=b.display_color || a.material_type!=b.material_type || a.compatible!=b.compatible)
            return changed();
    }
    for (size_t i=0; i<saved.regions.size(); ++i) {
        const auto& a=saved.regions[i]; const auto& b=current.regions[i];
        if (a.id!=b.id || a.subject_id!=b.subject_id || a.label!=b.label || a.confidence!=b.confidence ||
            a.faces!=b.faces || a.user_protected!=b.user_protected || a.protect_color!=b.protect_color ||
            a.locked_physical_slot!=b.locked_physical_slot) return changed();
    }
    for (size_t i=0; i<saved.contrasts.size(); ++i) {
        const auto& a=saved.contrasts[i]; const auto& b=current.contrasts[i];
        if (a.first_region!=b.first_region || a.second_region!=b.second_region || a.weight!=b.weight ||
            a.minimum_output_delta_e!=b.minimum_output_delta_e || a.hard!=b.hard) return changed();
    }
    // Algorithm and parent lineage may differ in the draft identity. Neither
    // invalidates historical output; preserve both fields exactly in the result.
    auto restored=saved;
    destination=std::move(restored);
    return true;
}

// A version record stores the exact source-face partition. Filesystem writes
// and project transactions remain the responsibility of the shared page host.
inline Json encode(const AI::LocalPrintColorResult& value)
{
    std::string error;
    if (!value.valid(error)) throw std::invalid_argument(error);
    Json j = {{"schema", value.schema}, {"algorithm_version", value.algorithm_version},
        {"source_sha256", value.source_sha256}, {"geometry_id", value.geometry_id},
        {"parent_version", value.parent_version}, {"material_fingerprint", value.material_fingerprint},
        {"process_fingerprint", value.process_fingerprint}, {"requested_color_count", value.requested_color_count},
        {"source_color_count", value.source_color_count}, {"sampled_source_color_count", value.sampled_source_color_count},
        {"face_count", value.face_count}, {"mode", value.mode == AI::PrintColorMode::Direct ? "direct" : "layered"},
        {"color_tolerance", value.color_tolerance}, {"important_area_floor", value.important_area_floor},
        {"face_targets", value.face_targets}, {"user_overrides", value.user_overrides},
        {"notices", value.notices}, {"confirmed", value.confirmed},
        {"physical_channels", Json::array()}, {"regions", Json::array()}, {"targets", Json::array()}, {"contrasts", Json::array()}};
    for (const auto& c : value.contrasts) j["contrasts"].push_back({{"first_region", c.first_region},
        {"second_region", c.second_region}, {"weight", c.weight}, {"minimum_output_delta_e", c.minimum_output_delta_e}, {"hard", c.hard}});
    for (const auto& p : value.physical_channels)
        j["physical_channels"].push_back({{"slot", p.slot}, {"display_color", p.display_color},
            {"material_type", p.material_type}, {"compatible", p.compatible}});
    for (const auto& r : value.regions) {
        Json region = {{"id", r.id}, {"subject_id", r.subject_id}, {"label", r.label}, {"confidence", r.confidence},
            {"user_protected", r.user_protected}, {"protect_color", r.protect_color}, {"faces", r.faces},
            {"locked_physical_slot", nullptr}};
        if (r.locked_physical_slot) region["locked_physical_slot"] = *r.locked_physical_slot;
        j["regions"].push_back(std::move(region));
    }
    for (const auto& t : value.targets) {
        const char* evidence = t.evidence == AI::ColorEvidence::Measured ? "measured" :
            t.evidence == AI::ColorEvidence::Interpolated ? "interpolated" :
            t.evidence == AI::ColorEvidence::Estimated ? "estimated" : "unknown";
        Json target = {{"source", t.source}, {"output", t.output}, {"area", t.area}, {"delta_e00", t.delta_e00},
            {"candidate_id", t.candidate_id}, {"evidence", evidence}, {"executable", t.executable},
            {"within_tolerance", t.within_tolerance}, {"unresolved_reason", t.unresolved_reason},
            {"physical_slot", nullptr}, {"recipe", nullptr}};
        if (t.physical_slot) target["physical_slot"] = *t.physical_slot;
        if (t.recipe) {
            Json recipe = {{"target_color", t.recipe->target_color}, {"components", Json::array()}, {"existing_virtual_slot", nullptr}};
            for (const auto& c : t.recipe->components) recipe["components"].push_back({{"slot", c.slot}, {"ratio", c.ratio}});
            if (t.recipe->existing_virtual_slot) recipe["existing_virtual_slot"] = *t.recipe->existing_virtual_slot;
            target["recipe"] = std::move(recipe);
            if(t.recipe_proof) {
                if(!LocalPrintRecipeProofState::valid(t,value,error)) throw std::invalid_argument(error);
                target["recipe_proof"]=LocalPrintRecipeProofState::encode(t);
            }
        }
        j["targets"].push_back(std::move(target));
    }
    return j;
}

inline size_t index(const Json& j)
{
    if (!j.is_number_integer() || (j.is_number_integer() && !j.is_number_unsigned() && j.get<int64_t>() < 0))
        throw std::invalid_argument("A color-state index must be a nonnegative integer.");
    const auto n = j.get<uint64_t>();
    if (n > std::numeric_limits<size_t>::max()) throw std::invalid_argument("Color-state index overflow.");
    return size_t(n);
}
inline std::vector<size_t> indices(const Json& j)
{
    if (!j.is_array()) throw std::invalid_argument("Color-state indices must be an array.");
    std::vector<size_t> result;
    result.reserve(j.size());
    for (const auto& item : j) result.push_back(index(item));
    return result;
}
inline AI::PrintRgb rgb(const Json& j)
{
    if (!j.is_array() || j.size() != 3) throw std::invalid_argument("RGB requires exactly three channels.");
    const auto value = j.get<AI::PrintRgb>();
    if (!AI::valid_print_rgb(value)) throw std::invalid_argument("Invalid RGB channel value.");
    return value;
}

// Strong failure guarantee: invalid/stale records never replace the last
// confirmed state. Callers supply current identities before applying a cache.
inline bool decode(const Json& j, const std::string& source_sha256, const std::string& geometry_id,
    const std::string& material_fingerprint, const std::string& process_fingerprint,
    AI::LocalPrintColorResult& destination, std::string& error, const std::string& algorithm_version = "region-direct-v5")
{
    try {
        AI::LocalPrintColorResult value;
        value.schema = j.at("schema").get<std::string>();
        value.algorithm_version = j.at("algorithm_version").get<std::string>();
        if (value.algorithm_version != algorithm_version) throw std::invalid_argument("Color result uses a stale matching algorithm.");
        value.source_sha256 = j.at("source_sha256").get<std::string>();
        value.geometry_id = j.at("geometry_id").get<std::string>();
        value.parent_version = j.at("parent_version").get<std::string>();
        value.material_fingerprint = j.at("material_fingerprint").get<std::string>();
        value.process_fingerprint = j.at("process_fingerprint").get<std::string>();
        if (value.source_sha256 != source_sha256 || value.geometry_id != geometry_id ||
            value.material_fingerprint != material_fingerprint || value.process_fingerprint != process_fingerprint)
            throw std::invalid_argument("Color result is stale for the current model, materials or process.");
        value.requested_color_count = index(j.at("requested_color_count"));
        value.source_color_count = index(j.at("source_color_count"));
        value.sampled_source_color_count = index(j.at("sampled_source_color_count"));
        value.face_count = index(j.at("face_count"));
        value.color_tolerance = j.at("color_tolerance").get<double>();
        value.important_area_floor = j.at("important_area_floor").get<double>();
        const auto mode = j.at("mode").get<std::string>();
        if (mode != "direct" && mode != "layered") throw std::invalid_argument("Unknown local printing mode.");
        value.mode = mode == "direct" ? AI::PrintColorMode::Direct : AI::PrintColorMode::Layered;
        value.face_targets = indices(j.at("face_targets"));
        value.notices = j.at("notices").get<std::vector<std::string>>();
        value.confirmed = j.at("confirmed").get<bool>();
        for (const char* key : {"physical_channels", "regions", "targets", "user_overrides", "contrasts"})
            if (!j.at(key).is_array()) throw std::invalid_argument("Color-state collections must be arrays.");
        for (const auto& c : j.at("contrasts")) value.contrasts.push_back({c.at("first_region").get<std::string>(),
            c.at("second_region").get<std::string>(), c.at("weight").get<double>(),
            c.at("minimum_output_delta_e").get<double>(), c.at("hard").get<bool>()});
        for (const auto& p : j.at("physical_channels")) value.physical_channels.push_back({index(p.at("slot")),
            p.at("display_color").get<std::string>(), p.at("material_type").get<std::string>(), p.at("compatible").get<bool>()});
        for (const auto& r : j.at("regions")) {
            AI::PrintColorRegion region;
            region.id = r.at("id").get<std::string>(); region.subject_id = r.at("subject_id").get<std::string>();
            region.label = r.at("label").get<std::string>(); region.confidence = r.at("confidence").get<double>();
            region.user_protected = r.at("user_protected").get<bool>(); region.protect_color = r.at("protect_color").get<bool>();
            region.faces = indices(r.at("faces"));
            if (!r.at("locked_physical_slot").is_null()) region.locked_physical_slot = index(r.at("locked_physical_slot"));
            value.regions.push_back(std::move(region));
        }
        for (const auto& t : j.at("targets")) {
            AI::PrintColorTarget target;
            target.source = rgb(t.at("source")); target.output = rgb(t.at("output"));
            target.area = t.at("area").get<double>(); target.delta_e00 = t.at("delta_e00").get<double>();
            target.candidate_id = t.at("candidate_id").get<std::string>();
            const auto evidence = t.at("evidence").get<std::string>();
            if (evidence == "measured") target.evidence = AI::ColorEvidence::Measured;
            else if (evidence == "interpolated") target.evidence = AI::ColorEvidence::Interpolated;
            else if (evidence == "estimated") target.evidence = AI::ColorEvidence::Estimated;
            else if (evidence != "unknown") throw std::invalid_argument("Unknown color evidence level.");
            target.executable = t.at("executable").get<bool>(); target.within_tolerance = t.at("within_tolerance").get<bool>();
            target.unresolved_reason = t.at("unresolved_reason").get<std::string>();
            if (!t.at("physical_slot").is_null()) target.physical_slot = index(t.at("physical_slot"));
            if (!t.at("recipe").is_null()) {
                const auto& r = t.at("recipe");
                AI::MixedColorRecipe recipe;
                recipe.target_color = r.at("target_color").get<std::string>();
                if (!r.at("components").is_array()) throw std::invalid_argument("Recipe components must be an array.");
                for (const auto& c : r.at("components")) recipe.components.push_back({index(c.at("slot")), c.at("ratio").get<double>()});
                if (!r.at("existing_virtual_slot").is_null()) recipe.existing_virtual_slot = index(r.at("existing_virtual_slot"));
                target.recipe = std::move(recipe);
                const auto proof=t.find("recipe_proof");
                if(proof!=t.end() && !proof->is_null()) {
                    target.recipe_proof=LocalPrintRecipeProofState::decode(*proof);
                    if(!LocalPrintRecipeProofState::valid(target,value,error)) return false;
                    if(LocalPrintRecipeProofState::encode(target)!=*proof) {
                        error="Serialized recipe evidence disagrees with its target.";return false;
                    }
                } else {
                    if(value.confirmed) {error="The saved recipe confirmation has no constraint evidence.";return false;}
                    target.executable=false;target.within_tolerance=false;
                    target.unresolved_reason="This legacy recipe preview must be recomputed with complete constraint evidence.";
                }
            } else if(t.contains("recipe_proof") && !t.at("recipe_proof").is_null()) {
                error="A physical target cannot carry recipe evidence.";return false;
            }
            value.targets.push_back(std::move(target));
        }
        for (const auto& item : j.at("user_overrides")) {
            if (!item.is_array() || item.size() != 2) throw std::invalid_argument("Invalid user color override record.");
            value.user_overrides.emplace_back(index(item.at(0)), rgb(item.at(1)));
        }
        if (!value.valid(error)) return false;
        destination = std::move(value);
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
} // namespace Slic3r::GUI::LocalPrintColorState

