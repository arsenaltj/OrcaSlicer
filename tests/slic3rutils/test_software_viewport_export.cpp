#include <catch2/catch_all.hpp>
#include "slic3r/GUI/AI/Orca/SoftwareViewportExport.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelLibraryModelThumbnail.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelLibraryThumbnail.hpp"
#include "../test_utils.hpp"

TEST_CASE("viewport export publishes complete bytes without replacing a screenshot", "[SoftwareViewportExport]")
{
    ScopedTemporaryDir folder("viewport-export");
    const auto target = folder.path() / "view.png";
    const std::vector<unsigned char> png {137,80,78,71,13,10,26,10,1,2,3};
    REQUIRE_NOTHROW(Slic3r::GUI::export_software_viewport_png(target, png));
    boost::filesystem::ifstream input(target, std::ios::binary);
    const std::vector<unsigned char> read((std::istreambuf_iterator<char>(input)), {});
    CHECK(read == png);
    const auto changed = std::vector<unsigned char> {137,80,78,71,13,10,26,10,9};
    CHECK_THROWS(Slic3r::GUI::export_software_viewport_png(target, changed));
    CHECK(boost::filesystem::file_size(target) == png.size());
    CHECK(std::distance(boost::filesystem::directory_iterator(folder.path()), boost::filesystem::directory_iterator()) == 1);
}

TEST_CASE("invalid screenshot encoding creates no destination", "[SoftwareViewportExport]")
{
    ScopedTemporaryDir folder("viewport-invalid");
    CHECK_THROWS(Slic3r::GUI::export_software_viewport_png(folder.path() / "view.png", {}));
    CHECK(boost::filesystem::is_empty(folder.path()));
}

TEST_CASE("failed viewport destination keeps the candidate available for retry", "[SoftwareViewportExport]")
{
    ScopedTemporaryDir folder("viewport-retry");
    const std::vector<unsigned char> png {137,80,78,71,13,10,26,10,1,2,3};
    const auto invalid = folder.path() / "missing" / "view.png";
    CHECK_THROWS(Slic3r::GUI::export_software_viewport_png(invalid, png));
    CHECK_FALSE(boost::filesystem::exists(invalid));
    REQUIRE_NOTHROW(Slic3r::GUI::export_software_viewport_png(folder.path() / "retry.png", png));
    CHECK(boost::filesystem::file_size(folder.path() / "retry.png") == png.size());
}

namespace {
Slic3r::GUI::LibraryModelThumbnailSource thumbnail_source(const boost::filesystem::path& model,
                                                         char hash = 'a')
{
    const auto path = std::filesystem::path(model.native());
    return {std::string(64, hash), std::filesystem::file_size(path),
        static_cast<int64_t>(std::filesystem::last_write_time(path).time_since_epoch().count())};
}
void write_thumbnail_model(const boost::filesystem::path& model)
{
    boost::filesystem::ofstream output(model, std::ios::binary);
    output << "private model source";
}
const std::vector<unsigned char> thumbnail_pixels {
    255,0,0, 0,255,0, // GL bottom row
    0,0,255, 255,255,0 // GL top row
};
}
TEST_CASE("A model thumbnail keeps its source and native top to bottom pixel order", "[ModelLibraryModelThumbnail]")
{
    using namespace Slic3r::GUI;
    ScopedTemporaryDir folder("model-thumbnail");
    const auto model = folder.path() / "model.glb";
    write_thumbnail_model(model);
    const auto bytes = boost::filesystem::file_size(model);
    const auto source = thumbnail_source(model);
    publish_library_model_thumbnail(folder.path(), model, source, 2, 2, thumbnail_pixels);
    const auto png = library_model_thumbnail_image(folder.path(), model);
    REQUIRE_FALSE(png.empty());
    std::atomic<bool> cancelled {false};
    const auto image = load_library_thumbnail(png, 2, cancelled);
    REQUIRE(image.IsOk());
    CHECK(image.GetRed(0,0) == 0);
    CHECK(image.GetBlue(0,0) == 255);
    CHECK(image.GetRed(0,1) == 255);
    CHECK(image.GetBlue(0,1) == 0);
    CHECK(boost::filesystem::file_size(model) == bytes);
}
TEST_CASE("Model thumbnail lookup rejects changed sources and unrelated model paths", "[ModelLibraryModelThumbnail]")
{
    using namespace Slic3r::GUI;
    ScopedTemporaryDir folder("model-thumbnail-stale");
    const auto model = folder.path() / "model.glb", other = folder.path() / "other.glb";
    write_thumbnail_model(model); write_thumbnail_model(other);
    const auto source = thumbnail_source(model);
    publish_library_model_thumbnail(folder.path(), model, source, 2, 2, thumbnail_pixels);
    CHECK(library_model_thumbnail_image(folder.path(), other).empty());
    std::filesystem::last_write_time(std::filesystem::path(model.native()),
        std::filesystem::last_write_time(std::filesystem::path(model.native())) + std::chrono::seconds(3));
    CHECK(library_model_thumbnail_image(folder.path(), model).empty());
    CHECK_THROWS(publish_library_model_thumbnail(folder.path(), model, source, 2, 2, thumbnail_pixels));
    // Refreshing a derived identity must work on Windows without rewriting history.
    publish_library_model_thumbnail(folder.path(), model, thumbnail_source(model, 'b'), 2, 2, thumbnail_pixels);
    CHECK_FALSE(library_model_thumbnail_image(folder.path(), model).empty());
}
TEST_CASE("Invalid thumbnail keys pixels and external paths never publish a model image", "[ModelLibraryModelThumbnail]")
{
    using namespace Slic3r::GUI;
    ScopedTemporaryDir folder("model-thumbnail-invalid"), outside("model-thumbnail-external");
    const auto model = folder.path() / "model.glb", external = outside.path() / "model.glb";
    write_thumbnail_model(model); write_thumbnail_model(external);
    auto source = thumbnail_source(model); source.sha256 = "../escape";
    CHECK_THROWS(publish_library_model_thumbnail(folder.path(), model, source, 2, 2, thumbnail_pixels));
    CHECK_THROWS(publish_library_model_thumbnail(folder.path(), external, thumbnail_source(external), 2, 2, thumbnail_pixels));
    CHECK_THROWS(publish_library_model_thumbnail(folder.path(), model, thumbnail_source(model), 2, 2, {}));
    CHECK_FALSE(boost::filesystem::exists(folder.path() / "model-thumbnails"));
}
TEST_CASE("A failed derived thumbnail destination can retry without changing the model", "[ModelLibraryModelThumbnail]")
{
    using namespace Slic3r::GUI;
    ScopedTemporaryDir folder("model-thumbnail-retry");
    const auto model = folder.path() / "model.glb", block = folder.path() / "model-thumbnails";
    write_thumbnail_model(model); write_thumbnail_model(block);
    const auto source = thumbnail_source(model);
    CHECK_THROWS(publish_library_model_thumbnail(folder.path(), model, source, 2, 2, thumbnail_pixels));
    CHECK(library_model_thumbnail_matches(model, source));
    REQUIRE(boost::filesystem::remove(block));
    REQUIRE_NOTHROW(publish_library_model_thumbnail(folder.path(), model, source, 2, 2, thumbnail_pixels));
    CHECK_FALSE(library_model_thumbnail_image(folder.path(), model).empty());
}

TEST_CASE("Distinct saved model assets retain their own rendered thumbnail even with identical model bytes", "[ModelLibraryModelThumbnail][ModelLibraryModelThumbnailAssetIdentity]")
{
    using namespace Slic3r::GUI;
    ScopedTemporaryDir folder("model-thumbnail-asset-identity");
    const auto a = folder.path() / "version-a.glb", b = folder.path() / "version-b.glb";
    write_thumbnail_model(a); write_thumbnail_model(b);
    // Saved display metadata may differ while model bytes stay identical.
    const auto sha = std::string(64, 'a');
    auto source_a = thumbnail_source(a), source_b = thumbnail_source(b);
    source_a.sha256 = source_b.sha256 = sha;
    const std::vector<unsigned char> red {255,0,0}, blue {0,0,255};
    publish_library_model_thumbnail(folder.path(), a, source_a, 1, 1, red);
    publish_library_model_thumbnail(folder.path(), b, source_b, 1, 1, blue);
    const auto png_a = library_model_thumbnail_image(folder.path(), a);
    const auto png_b = library_model_thumbnail_image(folder.path(), b);
    REQUIRE_FALSE(png_a.empty()); REQUIRE_FALSE(png_b.empty());
    CHECK_FALSE(png_a == png_b);
    std::atomic<bool> cancelled {false};
    CHECK(load_library_thumbnail(png_a, 1, cancelled).GetRed(0,0) == 255);
    CHECK(load_library_thumbnail(png_b, 1, cancelled).GetBlue(0,0) == 255);
}

TEST_CASE("Damaged published model thumbnails are rebuilt without changing their source", "[ModelLibraryModelThumbnail][ModelLibraryModelThumbnailRecovery]")
{
    using namespace Slic3r::GUI;
    ScopedTemporaryDir folder("model-thumbnail-repair");
    const auto model = folder.path() / "model.glb";
    write_thumbnail_model(model);
    const auto source = thumbnail_source(model);
    const std::vector<unsigned char> red {255,0,0}, blue {0,0,255};
    publish_library_model_thumbnail(folder.path(), model, source, 1, 1, red);
    const auto png = library_model_thumbnail_image(folder.path(), model);
    REQUIRE_FALSE(png.empty());
    const bool truncate = GENERATE(true, false);
    // Both truncation and a same-size changed file must invalidate only the
    // derived image. Do not hash or decode every model while listing cards.
    {
        const auto bytes = boost::filesystem::file_size(png);
        boost::filesystem::ofstream damaged(png, std::ios::binary);
        damaged << std::string(truncate ? 3 : size_t(bytes), 'x');
    }
    std::filesystem::last_write_time(std::filesystem::path(png.native()),
        std::filesystem::last_write_time(std::filesystem::path(png.native())) + std::chrono::seconds(3));
    CHECK(library_model_thumbnail_image(folder.path(), model).empty());
    CHECK(library_model_thumbnail_matches(model, source));
    REQUIRE_NOTHROW(publish_library_model_thumbnail(folder.path(), model, source, 1, 1, blue));
    const auto restored = library_model_thumbnail_image(folder.path(), model);
    REQUIRE_FALSE(restored.empty());
    std::atomic<bool> cancelled {false};
    const auto image = load_library_thumbnail(restored, 1, cancelled);
    REQUIRE(image.IsOk());
    CHECK(image.GetBlue(0,0) == 255);
    CHECK(library_model_thumbnail_matches(model, source));
}
