#include <catch2/catch_all.hpp>
#include "slic3r/AI/ColorMatching/ColorIslandCleanup.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include <limits>

using namespace Slic3r;
using namespace Slic3r::AI;
using namespace Slic3r::AI::ColorMatching;

namespace {
struct Fixture {
    std::vector<size_t> slots;
    std::vector<uint8_t> selected, pinned;
    std::vector<double> areas;
    std::vector<std::array<int32_t,3>> neighbors;
    std::vector<std::array<uint8_t,3>> edges;
    size_t center;
    Fixture() {
        // A triangulated planar disk supplies a genuinely enclosed face.
        indexed_triangle_set mesh;
        for(int y=0;y<=30;++y)for(int x=0;x<=30;++x)mesh.vertices.emplace_back(float(x),float(y),0);
        for(int y=0;y<30;++y)for(int x=0;x<30;++x) {
            const int a=y*31+x;mesh.indices.emplace_back(a,a+1,a+32);mesh.indices.emplace_back(a,a+32,a+31);
        }
        // ColorMatching tests link libslic3r, not the appearance library.
        const auto raw=its_face_neighbors(mesh);
        for(const auto& n:raw)neighbors.push_back({n[0],n[1],n[2]});
        const size_t count=mesh.indices.size();slots.assign(count,7);selected.assign(count,1);pinned.assign(count,0);
        areas.assign(count,.5);edges.assign(count,{1,1,1});center=size_t((15*30+15)*2);slots[center]=41;
    }
    auto run(const std::function<bool()>& canceled={}) const {
        return find_enclosed_color_islands(slots,selected,pinned,neighbors,areas,edges,{},canceled);
    }
};
}

TEST_CASE("Local color cleanup proposes only an enclosed small island and preserves its immutable inputs", "[ColorIslandCleanup]") {
    Fixture f;const auto before=f.slots;
    const auto result=f.run();REQUIRE(result.size()==1);
    CHECK(result.front().slot==7);CHECK(result.front().faces==std::vector<size_t>{f.center});CHECK(f.slots==before);
    CHECK(f.run().front().faces==result.front().faces);
}

TEST_CASE("Color cleanup retains protected features selection borders mixed borders and geometric creases", "[ColorIslandCleanup]") {
    Fixture f;
    SECTION("A protected pupil survives") { f.pinned[f.center]=1;CHECK(f.run().empty()); }
    SECTION("An island touching the selection edge survives") { f.selected[size_t(f.neighbors[f.center][0])]=0;CHECK(f.run().empty()); }
    SECTION("Two surrounding colors do not vote away a detail") { f.slots[size_t(f.neighbors[f.center][0])]=99;CHECK(f.run().empty()); }
    SECTION("A sharp crease blocks cleanup") {
        const auto n=f.neighbors[f.center][0];f.edges[f.center][0]=0;
        for(size_t e=0;e<3;++e)if(f.neighbors[size_t(n)][e]==int32_t(f.center))f.edges[size_t(n)][e]=0;
        CHECK(f.run().empty());
    }
    SECTION("Disconnected selection budgets cannot be pooled") {
        f.selected.assign(f.selected.size(),0);f.selected[f.center]=1;
        for(int32_t n:f.neighbors[f.center])f.selected[size_t(n)]=1;
        for(size_t i=0;i<60;++i)f.selected[i]=1;
        CHECK(f.run().empty());
    }
}

TEST_CASE("Color island cleanup rejects invalid adjacency areas and cancellation without mutation", "[ColorIslandCleanup]") {
    Fixture f;const auto before=f.slots;
    SECTION("Nonfinite area") { f.areas[f.center]=std::numeric_limits<double>::quiet_NaN();CHECK_THROWS(f.run()); }
    SECTION("Out of range adjacency") { f.neighbors[f.center][0]=int32_t(f.slots.size());CHECK_THROWS(f.run()); }
    SECTION("Missing reverse edge") { f.neighbors[size_t(f.neighbors[f.center][0])]={-1,-1,-1};CHECK_THROWS(f.run()); }
    SECTION("Mismatched protection") { f.pinned.pop_back();CHECK_THROWS(f.run()); }
    SECTION("Cancellation") { CHECK_THROWS(f.run([]{return true;})); }
    CHECK(f.slots==before);
}
