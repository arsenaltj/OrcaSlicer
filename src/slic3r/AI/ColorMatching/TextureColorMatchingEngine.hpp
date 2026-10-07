#pragma once
#include "libslic3r/TexturePainting.hpp"
#include <memory>
#include <string>

namespace Slic3r::AI::ColorMatching {
struct TextureColorRequest {
    const TexturedMesh& mesh;
    const TexturePaintingSettings& settings;
    PaintProgressCallback progress;
    PaintCancelCallback canceled;
};
struct TextureColorResult {
    PaintedMesh painted;
    bool success {false}, canceled {false};
    std::string algorithm_id, algorithm_version;
};
class ITextureColorMatchingEngine {
public:
    virtual ~ITextureColorMatchingEngine() = default;
    virtual const char* algorithm_id() const noexcept = 0;
    virtual const char* algorithm_version() const noexcept = 0;
    virtual double new_filament_delta_e_threshold() const noexcept { return 5.0; }
    virtual TextureColorResult compute(const TextureColorRequest& request) const = 0;
    virtual std::vector<FilamentMatch> match(const std::vector<std::array<size_t, 3>>& clusters,
        const std::vector<std::array<float, 4>>& colors, const std::vector<std::string>& names) const = 0;
};
std::shared_ptr<const ITextureColorMatchingEngine> baseline_texture_engine();
TextureColorResult compute_texture_colors(const TextureColorRequest& request,
    std::shared_ptr<const ITextureColorMatchingEngine> engine = {});
std::vector<FilamentMatch> match_texture_colors(const std::vector<std::array<size_t, 3>>& clusters,
    const std::vector<std::array<float, 4>>& colors, const std::vector<std::string>& names,
    std::shared_ptr<const ITextureColorMatchingEngine> engine = {});
} // namespace Slic3r::AI::ColorMatching
