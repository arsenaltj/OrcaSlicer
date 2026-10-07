#include <catch2/catch_test_macros.hpp>
#include "slic3r/AI/ColorMatching/AutomaticColorRegions.hpp"
#include <limits>

using namespace Slic3r::AI::ColorMatching;
namespace {
struct Strip {
    std::vector<uint32_t> owners = {1,1,1,1,1,1,1,1};
    std::vector<uint32_t> patches = {0,0,0,0,1,1,1,1};
    std::vector<std::array<int32_t,3>> neighbors;
    std::vector<double> areas = std::vector<double>(8,1.);
    std::vector<RegionRGB> colors = {{0,0,0,1},{0,0,0,1},{0,0,0,1},{0,0,0,1},
                                    {1,1,1,1},{1,1,1,1},{1,1,1,1},{1,1,1,1}};
    std::vector<AutomaticRegion> eligible = {{1,7}};
    std::vector<RegionMaterial> materials = {{7,{1,1,1,1}},{11,{0,0,0,1}}};
    Strip() {for(int32_t i=0;i<8;++i)neighbors.push_back({i>0?i-1:-1,i<7?i+1:-1,-1});}
    auto refine(const ColorRegionOptions& options={},const std::function<bool()>& canceled={}) const {
        return refine_automatic_color_regions(owners,patches,neighbors,areas,colors,eligible,materials,options,canceled);
    }
};
}

TEST_CASE("Automatic colour refinement retains a coherent secondary colour in real sparse slots", "[AutomaticColorRegions]") {
    Strip s;const auto original=s.colors;const auto ids=s.owners;
    const auto splits=s.refine();
    REQUIRE(splits.size()==1);
    CHECK(splits[0].source_region==1);CHECK(splits[0].slot==11);
    CHECK(splits[0].faces==std::vector<size_t>{0,1,2,3});
    CHECK(s.colors==original);CHECK(s.owners==ids);
}

TEST_CASE("Automatic colour refinement excludes manual regions and isolated tiny texture noise", "[AutomaticColorRegions]") {
    Strip s;
    SECTION("A different owner is never borrowed into an automatic region") {
        for(size_t i=0;i<4;++i)s.owners[i]=2;
        CHECK(s.refine().empty());
    }
    SECTION("Strong but microscopic noise does not create a print region") {
        for(size_t i=0;i<4;++i)s.areas[i]=1e-7;
        CHECK(s.refine().empty());
    }
    SECTION("Disconnected colour islands cannot pool their area to pass the threshold") {
        s.neighbors.assign(8,{-1,-1,-1});
        ColorRegionOptions options;options.minimum_region_fraction=.2;
        CHECK(s.refine(options).empty());
    }
}

TEST_CASE("Patch averages cannot recolour a face that already matches its baseline", "[AutomaticColorRegions]") {
    Strip s;s.colors[2]={1,1,1,1};
    const auto splits=s.refine();
    REQUIRE_FALSE(splits.empty());
    for(const auto& split:splits)for(size_t f:split.faces)CHECK(f!=2);
}

TEST_CASE("Automatic colour refinement reconnects tiny retained islands without repainting them", "[AutomaticColorRegions]") {
    Strip s;
    s.colors[2]={1,1,1,1};s.areas[2]=1e-5;
    const auto splits=s.refine();
    REQUIRE(splits.size()==1);
    // The isolated white face joins the main white region by withdrawing only
    // the black bridge face, leaving the substantial black detail in place.
    CHECK(splits.front().faces==std::vector<size_t>{0,1});
    CHECK(splits.front().slot==11);
    const auto again=s.refine();
    REQUIRE(again.size()==splits.size());
    CHECK(again.front().faces==splits.front().faces);
}

TEST_CASE("Automatic colour refinement rejects invalid geometry and cancellation without modifying inputs", "[AutomaticColorRegions]") {
    Strip s;const auto original=s.owners;
    SECTION("mismatched arrays") {s.colors.pop_back();CHECK_THROWS(s.refine());}
    SECTION("invalid source") {s.colors[0][0]=std::numeric_limits<float>::quiet_NaN();CHECK_THROWS(s.refine());}
    SECTION("invalid neighbors") {s.neighbors[0][1]=100;CHECK_THROWS(s.refine());}
    SECTION("unknown baseline material") {s.eligible[0].baseline_slot=17;CHECK_THROWS(s.refine());}
    SECTION("cancel during scanning") {size_t calls=0;CHECK_THROWS(s.refine({},[&]{return ++calls==2;}));}
    CHECK(s.owners==original);
}
