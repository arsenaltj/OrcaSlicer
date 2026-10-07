#pragma once

#include "ModelFinishing.hpp"

namespace Slic3r::AI {
ModelFinishingResult finish_model_obj_baseline(const boost::filesystem::path& source,
    const boost::filesystem::path& destination, const ModelFinishingOptions& options,
    const std::function<bool()>& canceled);
ModelFinishingResult finish_model_artifact_baseline(const boost::filesystem::path& source,
    const boost::filesystem::path& destination, const ModelFinishingOptions& options,
    const std::function<bool()>& canceled);
ModelFinishingResult finish_beauty_artifact_baseline(const boost::filesystem::path& source,
    const boost::filesystem::path& destination, const ModelFinishingOptions& options,
    const std::function<bool()>& canceled);
}
