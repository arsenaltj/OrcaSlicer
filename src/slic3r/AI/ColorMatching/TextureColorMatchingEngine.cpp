#include "TextureColorMatchingEngine.hpp"

namespace Slic3r::AI::ColorMatching {
namespace {
class NativeTextureColorEngine final : public ITextureColorMatchingEngine {
public:
    const char* algorithm_id() const noexcept override { return "native-texture-color"; }
    const char* algorithm_version() const noexcept override { return "native-texture-color-v1"; }
    TextureColorResult compute(const TextureColorRequest& request) const override
    {
        TextureColorResult result;
        if (!request.mesh.precomputed_face_colors.empty())
            result.success = face_colors_to_painting(request.mesh, result.painted,
                request.settings, request.progress, request.canceled);
        else
            result.success = texture_to_painting(request.mesh, result.painted,
                request.settings, request.progress, request.canceled);
        return result;
    }
    std::vector<FilamentMatch> match(const std::vector<std::array<size_t, 3>>& clusters,
        const std::vector<std::array<float, 4>>& colors, const std::vector<std::string>& names) const override
    { return match_clusters_to_filaments(clusters, colors, names); }
};
}
std::shared_ptr<const ITextureColorMatchingEngine> baseline_texture_engine()
{
    static const auto engine = std::make_shared<const NativeTextureColorEngine>();
    return engine;
}
TextureColorResult compute_texture_colors(const TextureColorRequest& request,
    std::shared_ptr<const ITextureColorMatchingEngine> engine)
{
    if (!engine) engine = baseline_texture_engine();
    TextureColorResult result;
    if (request.canceled && request.canceled()) result.canceled = true;
    else result = engine->compute(request);
    result.canceled = result.canceled || (request.canceled && request.canceled());
    if (result.canceled) result.success = false;
    result.algorithm_id = engine->algorithm_id(); result.algorithm_version = engine->algorithm_version();
    return result;
}
std::vector<FilamentMatch> match_texture_colors(const std::vector<std::array<size_t, 3>>& clusters,
    const std::vector<std::array<float, 4>>& colors, const std::vector<std::string>& names,
    std::shared_ptr<const ITextureColorMatchingEngine> engine)
{
    if (!engine) engine = baseline_texture_engine();
    return engine->match(clusters, colors, names);
}
} // namespace Slic3r::AI::ColorMatching
