#pragma once

#include "slic3r/AI/Contracts/ColorIntent.hpp"

#include <boost/filesystem/path.hpp>

#include <optional>
#include <string>
#include <vector>

namespace Slic3r::AI {

// Immutable hand-off from model generation to downstream consumers. This type
// deliberately contains no wx, Orca workspace, or provider-specific objects.
struct GeneratedModelArtifact
{
    boost::filesystem::path local_path;
    std::string             job_id;
    std::string             format;
    std::string             color_encoding;
    std::vector<std::string> generation_palette;
    bool                    used_printable_colors { false };
    std::optional<ColorIntentManifestRef> color_intent_manifest;
};

} // namespace Slic3r::AI
