#pragma once
#include "BeautyLeafDomain.hpp"
#include "slic3r/AI/ModelGeneration/SemanticColoring/SemanticColoring.hpp"

namespace Slic3r::AI {
// Bind the exact rendered colors to the file that received them. Editing masks
// and selection are not appearance; changing either must not invalidate a bake.
inline nlohmann::json baked_portrait_appearance(const std::string& model_hash,
    const nlohmann::json& semantic, const std::map<std::string, SemanticColoring::Color>& cells,
    const std::string& partition_hash)
{
    return {{"schema", "orca.baked-portrait-appearance/v1"}, {"model_sha256", model_hash},
        {"appearance_sha256", beauty_leaf_digest(nlohmann::json{{"semantic",semantic},{"cells",cells},
            {"partition_sha256",partition_hash}}.dump())}};
}
inline bool same_baked_portrait_appearance(const nlohmann::json& saved,
    const nlohmann::json& current)
{
    return saved.is_object() && current.is_object() && saved == current &&
        current.value("schema",std::string()) == "orca.baked-portrait-appearance/v1" &&
        current.value("model_sha256",std::string()).size() == 64;
}
}
