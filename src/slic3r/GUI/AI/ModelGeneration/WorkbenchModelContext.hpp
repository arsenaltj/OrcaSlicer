#pragma once

#include "slic3r/AI/Contracts/GeneratedModelArtifact.hpp"
#include <cstdint>
#include <map>

namespace Slic3r::GUI {

// Value-only asset projection retained for contract consumers. The application
// owns the editable session exclusively through Role::WorkbenchAssets; this
// record does not create or own another editor or generation session.
struct WorkbenchModelContext {
    boost::filesystem::path path, reference_image_path, design_image_path;
    std::string asset_id, sha256, format, color_encoding;
    std::vector<std::string> palette;
    std::map<std::string, std::string> palette_roles;
    boost::filesystem::path color_intent_path;
    std::string color_intent_schema, color_intent_sha256;
    bool use_printable_colors {false};
    bool ready {false};
    std::uint64_t revision {0};

    AI::GeneratedModelArtifact artifact() const {
        AI::GeneratedModelArtifact result;
        result.local_path = path; result.job_id = asset_id;
        result.format = format; result.color_encoding = color_encoding;
        result.generation_palette = palette; result.used_printable_colors = use_printable_colors;
        if (!color_intent_path.empty())
            result.color_intent_manifest = AI::ColorIntentManifestRef{
                color_intent_path.string(), color_intent_schema, color_intent_sha256};
        return result;
    }
};

} // namespace Slic3r::GUI
