#pragma once

#include <boost/filesystem/path.hpp>

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
};

} // namespace Slic3r::AI
