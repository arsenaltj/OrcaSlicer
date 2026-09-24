#include <catch2/catch_all.hpp>
#include "slic3r/GUI/AI/Model/BeautyDocument.hpp"
#include "slic3r/GUI/AI/Model/BeautyMetadata.hpp"

using namespace Slic3r::AI;

TEST_CASE("Beauty documents bind regions to geometry and source identity", "[BeautyWorkbench]")
{
    BeautyDocument document;
    document.geometry_id = "geometry-a";
    document.source_sha256 = std::string(64, 'a');
    document.face_count = 4;
    document.face_patch = {0, 0, 1, 1};
    document.add_group("eyes", {0, 1});

    const auto encoded = document.encode();
    const auto reopened = BeautyDocument::decode(encoded, "geometry-a", 4, std::string(64, 'a'));
    CHECK(reopened.geometry_id == document.geometry_id);
    CHECK(reopened.source_sha256 == document.source_sha256);
    CHECK(reopened.groups.size() == 1);
    CHECK(reopened.groups.front().faces == std::vector<size_t>{0, 1});
    CHECK_THROWS(BeautyDocument::decode(encoded, "geometry-b", 4, std::string(64, 'a')));
    CHECK_THROWS(BeautyDocument::decode(encoded, "geometry-a", 4, std::string(64, 'b')));
}

TEST_CASE("Legacy Beauty documents remain readable without source identity", "[BeautyWorkbench]")
{
    BeautyDocument document;
    document.geometry_id = "geometry-a";
    document.face_count = 1;
    document.face_patch = {0};
    auto encoded = document.encode();
    encoded.erase("source_sha256");
    CHECK_NOTHROW(BeautyDocument::decode(encoded, "geometry-a", 1));
    CHECK_THROWS(BeautyDocument::decode(encoded, "geometry-a", 1, std::string(64, 'a')));
}

TEST_CASE("Beauty runtime and cache namespaces stay separate from portrait semantics", "[BeautyWorkbench]")
{
    CHECK(beauty_runtime_path("/app").generic_string() == "/app/ai/beauty_semantics");
    CHECK(beauty_cache_path("/data").generic_string() == "/data/cache/beauty_semantics");
    CHECK(beauty_runtime_path("/app") != boost::filesystem::path("/app/ai/portrait_semantics"));
    CHECK(beauty_cache_path("/data") != boost::filesystem::path("/data/cache/portrait_semantics"));
}
