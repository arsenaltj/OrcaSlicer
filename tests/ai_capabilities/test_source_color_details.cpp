#include <catch2/catch_all.hpp>
#include "slic3r/AI/ColorMatching/SourceColorDetails.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include <numeric>
#include <set>

using namespace Slic3r;
using namespace Slic3r::AI::ColorMatching;
namespace {
struct DetailFixture {
    std::vector<uint32_t> regions;
    std::vector<size_t> slots;
    std::vector<uint8_t> selected, pinned;
    std::vector<std::array<int32_t,3>> neighbors;
    std::vector<double> areas;
    std::vector<RegionRGB> source;
    std::vector<RegionMaterial> materials{{7,{.9f,.55f,.5f,1}},{41,{.05f,.05f,.05f,1}},{93,{1,1,1,1}}};
    DetailFixture() {
        indexed_triangle_set mesh;
        for(int y=0;y<=20;++y)for(int x=0;x<=30;++x)mesh.vertices.emplace_back(float(x),float(y),0);
        for(int y=0;y<20;++y)for(int x=0;x<30;++x) {
            const int a=y*31+x;mesh.indices.emplace_back(a,a+1,a+32);mesh.indices.emplace_back(a,a+32,a+31);
        }
        for(const auto& n:its_face_neighbors(mesh))neighbors.push_back({n[0],n[1],n[2]});
        const size_t count=mesh.indices.size();regions.assign(count,1);slots.assign(count,7);
        selected.assign(count,0);pinned.assign(count,0);areas.assign(count,.5);source.assign(count,materials[0].color);
        for(size_t f=0;f<count;++f) {
            const int x=int(f/2)%30,y=int(f/2)/30;
            if(x>=5 && x<20 && y>=5 && y<15)selected[f]=1;
            if(x>=10 && x<14 && y>=8 && y<10)source[f]=materials[2].color;
            if(x>=12 && x<14 && y>=10 && y<12){regions[f]=2;slots[f]=41;source[f]=materials[1].color;}
            // A distant white source feature must never be edited.
            if(x==25 && y==17)source[f]=materials[2].color;
        }
    }
    auto run(const SourceColorDetailOptions& o={},const std::function<bool()>& cancel={})const {
        return recover_selected_source_color_details(regions,slots,selected,pinned,neighbors,areas,source,materials,o,cancel);
    }
};
}
TEST_CASE("Source detail recovery restores coherent missing colors inside a multi-color selection without changing the input", "[SourceColorDetails]") {
    const DetailFixture f;const auto before=f.source;const auto result=f.run();REQUIRE_FALSE(result.empty());
    std::set<size_t> recovered;
    for(const auto& proposal:result) {
        CHECK(proposal.slot==93);
        for(size_t face:proposal.faces){CHECK(f.selected[face]==1);CHECK(f.slots[face]==7);recovered.insert(face);}
    }
    CHECK(recovered.size()==16);CHECK(f.source==before);
    CHECK(f.run()[0].faces==result[0].faces);
}
TEST_CASE("Source detail recovery respects manual locks local area budgets cancellation and material identity", "[SourceColorDetails]") {
    DetailFixture f;
    SECTION("Pinned details stay exact") {
        for(size_t i=0;i<f.source.size();++i)if(f.source[i]==f.materials[2].color)f.pinned[i]=1;
        CHECK(f.run().empty());
    }
    SECTION("Unselected geometry does not inflate the area threshold") {
        for(size_t i=0;i<f.areas.size();++i)if(!f.selected[i])f.areas[i]=1e12;
        CHECK_FALSE(f.run().empty());
    }
    SECTION("Slots with equal RGB are not arbitrarily swapped") {
        f.materials.push_back({103,f.materials[0].color});CHECK_FALSE(f.run().empty());
        for(const auto& p:f.run())CHECK(p.slot==93);
    }
    SECTION("Invalid or cancelled calls leave the inputs intact") {
        const auto source=f.source;CHECK_THROWS(f.run({},[]{return true;}));CHECK(f.source==source);
        f.selected[0]=2;CHECK_THROWS(f.run());
    }
}
