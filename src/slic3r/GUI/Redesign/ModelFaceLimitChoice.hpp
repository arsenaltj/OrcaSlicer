#pragma once

#include "../AI/ModelGeneration/ModelGenerationHost.hpp"

namespace Slic3r::GUI {

inline std::vector<int> model_face_limit_choices(const ModelGenerationUIOptions& options)
{
    std::vector<int> choices {1000000};
    if (options.provider != "hunyuan")
        choices.push_back(2000000);
    // Keep legacy records truthful without offering 300k for new generations.
    if (options.face_limit == 300000)
        choices.push_back(300000);
    return choices;
}

inline ModelGenerationUIOptions model_generation_options_for_face_limit(ModelGenerationUIOptions options,
                                                                       int face_limit)
{
    options.face_limit = options.provider == "hunyuan" && face_limit > 1000000 ? 1000000 : face_limit;
    options.geometry_quality = options.face_limit == 2000000 ? "detailed" : "standard";
    if (options.provider == "hunyuan")
        options.texture_quality = "standard";
    return options;
}

} // namespace Slic3r::GUI
