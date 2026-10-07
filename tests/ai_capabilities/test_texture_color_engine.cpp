#include <catch2/catch_test_macros.hpp>
#include "slic3r/AI/ColorMatching/TextureColorMatchingEngine.hpp"

using namespace Slic3r;
namespace Colors = AI::ColorMatching;
namespace {
class RecordingTextureEngine final : public Colors::ITextureColorMatchingEngine {
public:
    mutable size_t computed {0}, matched {0};
    const char* algorithm_id() const noexcept override { return "recording-texture"; }
    const char* algorithm_version() const noexcept override { return "recording-texture-v2"; }
    Colors::TextureColorResult compute(const Colors::TextureColorRequest& request) const override
    { ++computed; return Colors::baseline_texture_engine()->compute(request); }
    std::vector<FilamentMatch> match(const std::vector<std::array<size_t, 3>>& clusters,
        const std::vector<std::array<float, 4>>& colors, const std::vector<std::string>& names) const override
    { ++matched; return Colors::baseline_texture_engine()->match(clusters, colors, names); }
};
}
TEST_CASE("Texture color consumers can replace computation and matching while keeping source face colors", "[TextureColorEngine]")
{
    TexturedMesh mesh;
    mesh.vertices = {{0,0,0}, {1,0,0}, {0,1,0}};
    mesh.indices = {{0,1,2}};
    mesh.precomputed_face_colors = {{255,0,0}};
    TexturePaintingSettings settings;
    settings.target_colors_num = 1; settings.smooth_weight = 0;
    settings.fixed_palette = {{255,0,0}}; settings.fixed_mapping_palette = {{255,0,0}};
    const auto engine = std::make_shared<RecordingTextureEngine>();
    const auto result = Colors::compute_texture_colors({mesh, settings, {}, {}}, engine);
    REQUIRE(result.success);
    REQUIRE(result.painted.face_colors.size() == 1);
    CHECK(result.painted.face_colors.front() == mesh.precomputed_face_colors.front());
    CHECK(result.painted.indices == mesh.indices);
    CHECK(result.algorithm_version == "recording-texture-v2");
    const auto matches = Colors::match_texture_colors(result.painted.cluster_colors, {{1,0,0,1}}, {"red"}, engine);
    REQUIRE(matches.size() == 1);
    CHECK(matches.front().filament_index == 0);
    CHECK(engine->computed == 1);
    CHECK(engine->matched == 1);
    const auto canceled = Colors::compute_texture_colors({mesh, settings, {}, [] { return true; }}, engine);
    CHECK(canceled.canceled);
    CHECK_FALSE(canceled.success);
    CHECK(engine->computed == 1);
}
