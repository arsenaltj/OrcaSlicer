#include <catch2/catch_test_macros.hpp>
#include "slic3r/AI/ColorMatching/PortraitColorConstraints.hpp"
#include "libslic3r/TextureToColor/ColorUtils.hpp"
#include <algorithm>
#include <limits>
#include <numeric>
#include <random>
#include <cmath>
#include <map>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace Slic3r::AI::ColorMatching;
namespace {
std::vector<RegionMaterial> colors() {
    return {{7,{.91f,.71f,.60f,1}},{41,{.73f,.32f,.29f,1}},{93,{.96f,.97f,.98f,1}},
        {105,{.44f,.33f,.24f,1}},{117,{.16f,.15f,.16f,1}}};
}
std::vector<std::array<int32_t,3>> strip(size_t count) {
    std::vector<std::array<int32_t,3>> neighbors;
    for(size_t f=0;f<count;++f)neighbors.push_back({f?int32_t(f-1):-1,f+1<count?int32_t(f+1):-1,-1});
    return neighbors;
}
}
TEST_CASE("Joint color correction removes an island that single face moves cannot improve", "[PortraitColorConstraints]") {
    auto palette=colors();palette[0].color={.46f,.35f,.26f,1};
    const std::vector<uint32_t> regions{1,2,2,3};const std::vector<size_t> slots{7,41,41,7};
    const std::vector<PortraitFaceRole> roles(4,PortraitFaceRole::Skin);
    const std::vector<uint8_t> selected{0,1,1,0},pinned(4,0);const auto neighbors=strip(4);
    const std::vector<double> areas(4,1);const std::vector<RegionRGB> source(4,palette[3].color);
    PortraitColorConstraintOptions policy;policy.lip_slots={41};policy.component_coherence=false;
    const auto single=constrain_selected_portrait_materials(regions,slots,roles,selected,pinned,neighbors,areas,source,palette,policy);
    REQUIRE(single.size()==1);CHECK(single.front().slot==105);
    policy.component_coherence=true;
    const auto joint=constrain_selected_portrait_materials(regions,slots,roles,selected,pinned,neighbors,areas,source,palette,policy);
    REQUIRE(joint.size()==1);CHECK(joint.front().slot==7);CHECK(joint.front().faces==std::vector<size_t>{1,2});
    // One infeasible face prevents a joint move even under a large edge cost.
    palette[1].color=palette[3].color;policy.maximum_source_error_increase=0;policy.boundary_edge_penalty=100;
    const auto bounded=constrain_selected_portrait_materials(regions,slots,roles,selected,pinned,neighbors,areas,source,palette,policy);
    REQUIRE(bounded.size()==1);CHECK(bounded.front().slot==105);
}

TEST_CASE("Portrait correction preserves its mask and loss bound across tones palettes and mesh sizes", "[PortraitColorConstraints][PortraitColorGeneralization]") {
    // Synthetic contract coverage, not a human skin/recognition accuracy dataset.
    const std::vector<RegionRGB> tones{{.94f,.78f,.67f,1},{.68f,.44f,.30f,1},{.27f,.14f,.10f,1},
        {.12f,.10f,.12f,1},{.04f,.65f,.85f,1},{.8f,.04f,.55f,1}};
    const auto error=[](const RegionRGB& a,const RegionRGB& b){
        return Slic3r::tex2color::color_utils::calc_rgb_color_difference_by_ciede2000_srgb01({a[0],a[1],a[2]},{b[0],b[1],b[2]});
    };
    for(size_t count:{size_t(8),size_t(48),size_t(256)})for(double scale:{.001,1.,100.})for(bool missing_skin:{false,true}) {
        INFO("faces="<<count<<" area scale="<<scale<<" missing skin="<<missing_skin);
        auto palette=colors();if(missing_skin)palette.erase(palette.begin());
        std::vector<uint32_t> regions(count);std::iota(regions.begin(),regions.end(),1);
        std::vector<size_t> slots(count,41);std::vector<PortraitFaceRole> roles(count);
        std::vector<uint8_t> selected(count),pinned(count);std::vector<double> areas(count);std::vector<RegionRGB> source(count);
        for(size_t f=0;f<count;++f) {
            roles[f]=PortraitFaceRole(f%7);selected[f]=f%5!=0;pinned[f]=f%11==0;
            areas[f]=scale*(1.+f%4);source[f]=tones[f%tones.size()];
        }
        const auto neighbors=strip(count);PortraitColorConstraintOptions policy;policy.lip_slots={41};policy.lip_boundary_guard_rings=0;
        const auto apply=[&](const std::vector<ColorRegionSplit>& result) {
            auto values=slots;std::vector<uint8_t> written(count,0);
            for(const auto& split:result)for(size_t f:split.faces){REQUIRE_FALSE(written[f]);written[f]=1;values[f]=split.slot;}
            return values;
        };
        policy.boundary_edge_penalty=0;
        const auto nearest=apply(constrain_selected_portrait_materials(regions,slots,roles,selected,pinned,neighbors,areas,source,palette,policy));
        policy.boundary_edge_penalty=4;
        const auto corrected=apply(constrain_selected_portrait_materials(regions,slots,roles,selected,pinned,neighbors,areas,source,palette,policy));
        CHECK(corrected!=slots);
        const auto rgb=[&](size_t slot)->const RegionRGB& {return std::find_if(palette.begin(),palette.end(),[&](const auto& m){return m.slot==slot;})->color;};
        double initial_energy=0,final_energy=0;
        for(size_t f=0;f<count;++f) {
            CHECK((corrected[f]!=slots[f])==(nearest[f]!=slots[f]));
            if(corrected[f]!=slots[f]) {
                CHECK(selected[f]==1);CHECK(pinned[f]==0);CHECK(corrected[f]!=41);
                CHECK(error(source[f],rgb(corrected[f]))<=error(source[f],rgb(41))+8.+1e-9);
                initial_energy+=error(source[f],rgb(nearest[f]));final_energy+=error(source[f],rgb(corrected[f]));
            }
            for(int32_t n:neighbors[f])if(n>=0 && size_t(n)>f){initial_energy+=4*(nearest[f]!=nearest[size_t(n)]);final_energy+=4*(corrected[f]!=corrected[size_t(n)]);}
        }
        CHECK(final_energy<=initial_energy+1e-8);
        std::reverse(palette.begin(),palette.end());
        CHECK(apply(constrain_selected_portrait_materials(regions,slots,roles,selected,pinned,neighbors,areas,source,palette,policy))==corrected);
        std::fill(roles.begin(),roles.end(),PortraitFaceRole::Unknown);
        CHECK(constrain_selected_portrait_materials(regions,slots,roles,selected,pinned,neighbors,areas,source,palette,policy).empty());
    }
}

TEST_CASE("Joint correction respects face permutations and explicit roles with ambiguous duplicate materials", "[PortraitColorConstraints][PortraitColorGeneralization]") {
    auto palette=colors();palette[0].color={.46f,.35f,.26f,1};palette.push_back({801,palette[1].color});
    const std::vector<size_t> base_slots{7,41,41,7};
    std::vector<size_t> order{0,1,2,3};std::mt19937 random(4831);
    for(unsigned trial=0;trial<24;++trial) {
        std::shuffle(order.begin(),order.end(),random);std::vector<size_t> inverse(4);
        for(size_t f=0;f<4;++f)inverse[order[f]]=f;
        auto neighbors=strip(4);std::vector<std::array<int32_t,3>> shuffled(4);std::vector<size_t> slots(4);
        std::vector<uint32_t> regions(4);std::vector<uint8_t> selected(4),pinned(4,0);
        for(size_t f=0;f<4;++f) {
            const size_t old=order[f];slots[f]=base_slots[old];regions[f]=uint32_t(old==0?1:old==3?3:2);selected[f]=old==1 || old==2;
            for(size_t edge=0;edge<3;++edge)shuffled[f][edge]=neighbors[old][edge]<0?-1:int32_t(inverse[size_t(neighbors[old][edge])]);
        }
        const std::vector<PortraitFaceRole> roles(4,PortraitFaceRole::Skin);const std::vector<double> areas(4,1);
        const std::vector<RegionRGB> source(4,palette[3].color);PortraitColorConstraintOptions policy;policy.lip_slots={41,801};
        const auto result=constrain_selected_portrait_materials(regions,slots,roles,selected,pinned,shuffled,areas,source,palette,policy);
        REQUIRE(result.size()==1);CHECK(result.front().slot==7);CHECK(result.front().faces.size()==2);
        for(size_t face:result.front().faces)CHECK(selected[face]==1);
        std::reverse(palette.begin(),palette.end());
        // Explicit empty lip roles means no prohibition, regardless of RGB.
        policy.lip_slots.clear();CHECK(constrain_selected_portrait_materials(regions,slots,roles,selected,pinned,shuffled,areas,source,palette,policy).empty());
        // Reverse back so the source fixture keeps referring to the same color.
        std::reverse(palette.begin(),palette.end());
    }
}
TEST_CASE("Portrait material constraints retain lips their rim unknown clothing manual paint and outside faces", "[PortraitColorConstraints]") {
    const auto palette=colors();const std::vector<uint32_t> regions{1,1,1,1,1,1,1,1,2,2,3,3};const std::vector<size_t> slots(12,41);
    std::vector<PortraitFaceRole> roles(12,PortraitFaceRole::Eye);
    roles[4]=PortraitFaceRole::Lips;roles[6]=PortraitFaceRole::Unknown;roles[7]=PortraitFaceRole::Other;
    roles[8]=PortraitFaceRole::Skin;roles[9]=PortraitFaceRole::Hair;roles[11]=PortraitFaceRole::Mouth;
    std::vector<uint8_t> selected(12,1),pinned(12,0);selected[1]=0;pinned[2]=1;
    const auto neighbors=strip(12);const std::vector<double> areas(12,1);const std::vector<RegionRGB> source(12,palette[3].color);
    PortraitColorConstraintOptions policy;policy.lip_slots={41};policy.lip_boundary_guard_rings=1;
    const auto result=constrain_selected_portrait_materials(regions,slots,roles,selected,pinned,neighbors,areas,source,palette,policy);
    REQUIRE(result.size()==2);CHECK(result[0].faces==std::vector<size_t>{0});CHECK(result[1].faces==std::vector<size_t>{8,9});
    for(const auto& item:result)CHECK(item.slot==105);
    CHECK(result[0].source_region==1);CHECK(result[1].source_region==2);
    auto reversed=palette;std::reverse(reversed.begin(),reversed.end());
    const auto reordered=constrain_selected_portrait_materials(regions,slots,roles,selected,pinned,neighbors,areas,source,reversed,policy);
    REQUIRE(reordered.size()==result.size());for(size_t i=0;i<result.size();++i){CHECK(reordered[i].faces==result[i].faces);CHECK(reordered[i].slot==result[i].slot);}
    CHECK(slots==std::vector<size_t>(12,41));CHECK(source==std::vector<RegionRGB>(12,palette[3].color));
    auto opaque=slots;opaque[1]=900; // Unselected/unsupported native recipe stays opaque.
    CHECK_NOTHROW(constrain_selected_portrait_materials(regions,opaque,roles,selected,pinned,neighbors,areas,source,palette,policy));
    CHECK_THROWS(constrain_selected_portrait_materials(regions,slots,roles,selected,pinned,neighbors,areas,source,palette,policy,[]{return true;}));
}
TEST_CASE("Portrait constraints withdraw articulation corrections without repainting unknown donor fragments", "[PortraitColorConstraints]") {
    const auto palette=colors();const std::vector<uint32_t> regions(5,1);const std::vector<size_t> slots(5,41);
    std::vector<PortraitFaceRole> roles(5,PortraitFaceRole::Eye);roles.front()=roles.back()=PortraitFaceRole::Unknown;
    const std::vector<uint8_t> selected(5,1),pinned(5,0);const auto neighbors=strip(5);
    const std::vector<double> areas(5,1);const std::vector<RegionRGB> source(5,palette[3].color);
    PortraitColorConstraintOptions policy;policy.lip_slots={41};
    CHECK(constrain_selected_portrait_materials(regions,slots,roles,selected,pinned,neighbors,areas,source,palette,policy).empty());
    roles.front()=PortraitFaceRole::Eye;
    const auto leaves=constrain_selected_portrait_materials(regions,slots,roles,selected,pinned,neighbors,areas,source,palette,policy);
    REQUIRE(leaves.size()==1);CHECK(leaves[0].faces==std::vector<size_t>{0,1,2,3});
    auto disconnected=neighbors;disconnected[1][1]=-1;disconnected[2][0]=-1;
    CHECK_THROWS(constrain_selected_portrait_materials(regions,slots,roles,selected,pinned,disconnected,areas,source,palette,policy));
}
TEST_CASE("Portrait constraints bound source loss and preserve ambiguous or unavailable material choices", "[PortraitColorConstraints]") {
    const auto palette=colors();const std::vector<uint32_t> regions(4,1);const std::vector<size_t> slots(4,41);
    const std::vector<PortraitFaceRole> roles(4,PortraitFaceRole::Skin);const std::vector<uint8_t> selected(4,1),pinned(4,0);
    const auto neighbors=strip(4);const std::vector<double> areas(4,1);const std::vector<RegionRGB> source(4,palette[1].color);
    PortraitColorConstraintOptions policy;policy.lip_slots={41};policy.maximum_source_error_increase=0;
    CHECK(constrain_selected_portrait_materials(regions,slots,roles,selected,pinned,neighbors,areas,source,palette,policy).empty());
    policy.maximum_source_error_increase=100;
    CHECK_FALSE(constrain_selected_portrait_materials(regions,slots,roles,selected,pinned,neighbors,areas,source,palette,policy).empty());
    policy.lip_slots={7,41,93,105,117};
    CHECK(constrain_selected_portrait_materials(regions,slots,roles,selected,pinned,neighbors,areas,source,palette,policy).empty());
    policy.lip_slots={999};
    CHECK_THROWS(constrain_selected_portrait_materials(regions,slots,roles,selected,pinned,neighbors,areas,source,palette,policy));
    policy.lip_slots={41};auto bad_mask=selected;bad_mask[0]=2;
    CHECK_THROWS(constrain_selected_portrait_materials(regions,slots,roles,bad_mask,pinned,neighbors,areas,source,palette,policy));
    CHECK(portrait_face_role("rr")==PortraitFaceRole::Skin);CHECK(portrait_face_role("ulip")==PortraitFaceRole::Lips);
    CHECK(portrait_face_role("unrecognized")==PortraitFaceRole::Unknown);
}
TEST_CASE("Suggested lip materials distinguish the accepted portrait red from its skin brown black and white slots", "[PortraitColorConstraints]") {
    const std::vector<RegionMaterial> palette{{0,{247/255.f,226/255.f,218/255.f,1}},{1,{40/255.f,38/255.f,41/255.f,1}},
        {2,{246/255.f,247/255.f,249/255.f,1}},{3,{232/255.f,180/255.f,154/255.f,1}},
        {4,{185/255.f,81/255.f,74/255.f,1}},{5,{112/255.f,83/255.f,62/255.f,1}},
        {91,{234/255.f,154/255.f,146/255.f,1}}};
    CHECK(suggest_lip_material_slots(palette)==std::vector<size_t>{4});
    auto equal_rgb=palette;equal_rgb.push_back({97,palette[4].color});
    CHECK(suggest_lip_material_slots(equal_rgb)==std::vector<size_t>{4,97});
}
TEST_CASE("Portrait corrections join a nearby material without expanding the accepted mask or increasing boundary energy", "[PortraitColorConstraints]") {
    auto palette=colors();palette[0].color={.46f,.35f,.26f,1};
    const std::vector<uint32_t> regions{1,2,3};const std::vector<size_t> slots{7,41,7};
    const std::vector<PortraitFaceRole> roles{PortraitFaceRole::Unknown,PortraitFaceRole::Eye,PortraitFaceRole::Unknown};
    const std::vector<uint8_t> selected{0,1,0},pinned{1,0,1};const auto neighbors=strip(3);
    const std::vector<double> areas(3,1);const std::vector<RegionRGB> source(3,palette[3].color);
    PortraitColorConstraintOptions policy;policy.lip_slots={41};policy.boundary_edge_penalty=0;
    const auto nearest=constrain_selected_portrait_materials(regions,slots,roles,selected,pinned,neighbors,areas,source,palette,policy);
    REQUIRE(nearest.size()==1);CHECK(nearest.front().slot==105);
    policy.boundary_edge_penalty=4;
    const auto coherent=constrain_selected_portrait_materials(regions,slots,roles,selected,pinned,neighbors,areas,source,palette,policy);
    REQUIRE(coherent.size()==1);CHECK(coherent.front().slot==7);CHECK(coherent.front().faces==std::vector<size_t>{1});
    auto reversed=palette;std::reverse(reversed.begin(),reversed.end());
    const auto reordered=constrain_selected_portrait_materials(regions,slots,roles,selected,pinned,neighbors,areas,source,reversed,policy);
    REQUIRE(reordered.size()==1);CHECK(reordered.front().slot==7);
    policy.coherence_passes=0;
    const auto disabled=constrain_selected_portrait_materials(regions,slots,roles,selected,pinned,neighbors,areas,source,palette,policy);
    REQUIRE(disabled.size()==1);CHECK(disabled.front().slot==105);
    CHECK(slots==std::vector<size_t>{7,41,7});
}
TEST_CASE("Spatial color coherence never overrides the source loss bound or accepts invalid budgets", "[PortraitColorConstraints]") {
    auto palette=colors();palette[0].color={.46f,.35f,.26f,1};palette[1].color=palette[3].color;
    const std::vector<uint32_t> regions{1,2,3};const std::vector<size_t> slots{7,41,7};
    const std::vector<PortraitFaceRole> roles(3,PortraitFaceRole::Eye);
    const std::vector<uint8_t> selected{0,1,0},pinned(3,0);const auto neighbors=strip(3);
    const std::vector<double> areas(3,1);const std::vector<RegionRGB> source(3,palette[3].color);
    PortraitColorConstraintOptions policy;policy.lip_slots={41};policy.maximum_source_error_increase=0;policy.boundary_edge_penalty=100;
    const auto bounded=constrain_selected_portrait_materials(regions,slots,roles,selected,pinned,neighbors,areas,source,palette,policy);
    REQUIRE(bounded.size()==1);CHECK(bounded.front().slot==105);
    policy.boundary_edge_penalty=-1;
    CHECK_THROWS(constrain_selected_portrait_materials(regions,slots,roles,selected,pinned,neighbors,areas,source,palette,policy));
    policy.boundary_edge_penalty=std::numeric_limits<double>::infinity();
    CHECK_THROWS(constrain_selected_portrait_materials(regions,slots,roles,selected,pinned,neighbors,areas,source,palette,policy));
    policy.boundary_edge_penalty=4;policy.coherence_passes=33;
    CHECK_THROWS(constrain_selected_portrait_materials(regions,slots,roles,selected,pinned,neighbors,areas,source,palette,policy));
}

TEST_CASE("Merged correction components never increase energy on branched mesh adjacency", "[PortraitColorConstraints][PortraitColorGeneralization]") {
    const auto palette=colors();std::mt19937 random(8159);
    const auto error=[](const RegionRGB& a,const RegionRGB& b){
        return Slic3r::tex2color::color_utils::calc_rgb_color_difference_by_ciede2000_srgb01({a[0],a[1],a[2]},{b[0],b[1],b[2]});
    };
    const auto rgb=[&](size_t slot)->const RegionRGB& {return std::find_if(palette.begin(),palette.end(),[&](const auto& m){return m.slot==slot;})->color;};
    for(size_t count:{size_t(20),size_t(80),size_t(160)})for(unsigned trial=0;trial<12;++trial) {
        INFO("faces="<<count<<" trial="<<trial);
        auto neighbors=strip(count);std::vector<uint32_t> regions(count);std::iota(regions.begin(),regions.end(),1);
        std::vector<size_t> slots(count,41);std::vector<RegionRGB> source(count);std::vector<double> areas(count,1);
        std::vector<uint8_t> selected(count,1),pinned(count,0);const std::vector<PortraitFaceRole> roles(count,PortraitFaceRole::Skin);
        for(size_t f=0;f<count;++f) {
            neighbors[f][2]=int32_t(f<count/2?f+count/2:f-count/2);
            if(f%13==0)slots[f]=7;if(f%17==0)pinned[f]=1;
            source[f]=palette[random()%palette.size()].color;
            for(size_t c=0;c<3;++c)source[f][c]=std::clamp(source[f][c]+float(int(random()%11)-5)/255.f,0.f,1.f);
        }
        PortraitColorConstraintOptions policy;policy.lip_slots={41};policy.boundary_edge_penalty=0;
        const auto apply=[&](const auto& result){auto values=slots;for(const auto& split:result)for(size_t f:split.faces)values[f]=split.slot;return values;};
        const auto nearest=apply(constrain_selected_portrait_materials(regions,slots,roles,selected,pinned,neighbors,areas,source,palette,policy));
        policy.boundary_edge_penalty=4;
        const auto corrected=apply(constrain_selected_portrait_materials(regions,slots,roles,selected,pinned,neighbors,areas,source,palette,policy));
        double initial=0,final=0;
        for(size_t f=0;f<count;++f) {
            CHECK((corrected[f]!=slots[f])==(nearest[f]!=slots[f]));
            if(corrected[f]!=slots[f]) {
                CHECK(pinned[f]==0);CHECK(corrected[f]!=41);
                CHECK(error(source[f],rgb(corrected[f]))<=error(source[f],rgb(41))+8.+1e-9);
                initial+=error(source[f],rgb(nearest[f]));final+=error(source[f],rgb(corrected[f]));
            }
            for(int32_t n:neighbors[f])if(n>=0 && size_t(n)>f){initial+=4*(nearest[f]!=nearest[size_t(n)]);final+=4*(corrected[f]!=corrected[size_t(n)]);}
        }
        CHECK(final<=initial+1e-8);
    }
}

namespace {
struct MetricStrip {
    std::vector<uint32_t> regions;
    std::vector<size_t> slots;
    std::vector<uint8_t> selected,pinned;
    std::vector<PortraitFaceRole> roles;
    std::vector<std::array<int32_t,3>> neighbors;
    std::vector<std::array<float,3>> lengths;
    std::vector<double> areas;
    std::vector<RegionRGB> source;
};
MetricStrip metric_strip(unsigned n,double scale) {
    // Same 3 x 1 rectangle; its middle unit square is the editable brown
    // candidate between two immutable warm strips, at every resolution.
    std::vector<std::array<double,2>> vertices;
    for(unsigned y=0;y<=n;++y)for(unsigned x=0;x<=3*n;++x)vertices.push_back({scale*x/n,scale*y/n});
    std::vector<std::array<int32_t,3>> triangles;std::vector<uint8_t> chosen;
    for(unsigned y=0;y<n;++y)for(unsigned x=0;x<3*n;++x) {
        const int32_t a=int32_t(y*(3*n+1)+x),b=a+1,c=a+int32_t(3*n+1),d=c+1;
        triangles.push_back({a,b,d});triangles.push_back({a,d,c});
        chosen.push_back(x>=n && x<2*n);chosen.push_back(chosen.back());
    }
    MetricStrip out;const size_t count=triangles.size();out.selected=chosen;out.pinned.assign(count,0);
    out.regions.resize(count);std::iota(out.regions.begin(),out.regions.end(),1);
    out.roles.assign(count,PortraitFaceRole::Skin);out.slots.resize(count);out.source.assign(count,colors()[3].color);
    out.neighbors.assign(count,{-1,-1,-1});out.lengths.resize(count);out.areas.assign(count,scale*scale/(2.*n*n));
    std::map<std::pair<int32_t,int32_t>,std::pair<size_t,size_t>> edges;
    for(size_t f=0;f<count;++f) {
        out.slots[f]=chosen[f]?41:7;
        for(size_t e=0;e<3;++e) {
            const auto a=triangles[f][e],b=triangles[f][(e+1)%3];
            out.lengths[f][e]=float(std::hypot(vertices[a][0]-vertices[b][0],vertices[a][1]-vertices[b][1]));
            const auto found=edges.emplace(std::minmax(a,b),std::make_pair(f,e));
            if(!found.second) {
                const auto other=found.first->second;out.neighbors[f][e]=int32_t(other.first);
                out.neighbors[other.first][other.second]=int32_t(f);
            }
        }
    }
    return out;
}
}
TEST_CASE("Geometric color costs correct the same physical region across subdivision and uniform scaling", "[PortraitColorConstraints][PortraitColorGeneralization]") {
    auto palette=colors();palette[0].color={.46f,.35f,.26f,1};
    PortraitColorConstraintOptions policy;policy.lip_slots={41};
    for(unsigned resolution:{1u,2u,8u})for(double scale:{.01,1.,100.}) {
        INFO("resolution="<<resolution<<" scale="<<scale);
        const auto mesh=metric_strip(resolution,scale);const PortraitColorGeometry geometry{mesh.lengths,.5*scale};
        const auto result=constrain_selected_portrait_materials(mesh.regions,mesh.slots,mesh.roles,mesh.selected,mesh.pinned,
            mesh.neighbors,mesh.areas,mesh.source,palette,policy,{},&geometry);
        size_t changed=0;double corrected_area=0;
        for(const auto& split:result) {
            CHECK(split.slot==7);
            for(size_t f:split.faces){CHECK(mesh.selected[f]==1);++changed;corrected_area+=mesh.areas[f];}
        }
        CHECK(changed==size_t(2*resolution*resolution));
        CHECK_THAT(corrected_area/(scale*scale),Catch::Matchers::WithinAbs(1.,1e-9));
        if(resolution==8 && scale==1.) {
            policy.geometric_coherence=false;
            const auto old=constrain_selected_portrait_materials(mesh.regions,mesh.slots,mesh.roles,mesh.selected,mesh.pinned,
                mesh.neighbors,mesh.areas,mesh.source,palette,policy,{},&geometry);
            CHECK(std::any_of(old.begin(),old.end(),[](const auto& split){return split.slot==105;}));
            policy.geometric_coherence=true;
        }
    }
}
TEST_CASE("Geometric correction rejects inconsistent metrics and retains source and manual protection", "[PortraitColorConstraints][PortraitColorGeneralization]") {
    auto palette=colors();palette[0].color={.46f,.35f,.26f,1};auto mesh=metric_strip(2,1.);
    PortraitColorConstraintOptions policy;policy.lip_slots={41};const auto lengths=mesh.lengths;
    PortraitColorGeometry geometry{mesh.lengths,.5};
    const auto solve=[&]{return constrain_selected_portrait_materials(mesh.regions,mesh.slots,mesh.roles,mesh.selected,mesh.pinned,
        mesh.neighbors,mesh.areas,mesh.source,palette,policy,{},&geometry);};
    geometry.reference_length=0;CHECK_THROWS(solve());geometry.reference_length=.5;
    const size_t face=size_t(std::find(mesh.selected.begin(),mesh.selected.end(),1)-mesh.selected.begin());
    const size_t side=size_t(std::find_if(mesh.neighbors[face].begin(),mesh.neighbors[face].end(),[](int32_t n){return n>=0;})-mesh.neighbors[face].begin());
    mesh.lengths[face][side]*=2;CHECK_THROWS(solve());mesh.lengths=lengths;
    mesh.lengths[0][0]=std::numeric_limits<float>::quiet_NaN();CHECK_THROWS(solve());mesh.lengths=lengths;
    mesh.lengths.pop_back();CHECK_THROWS(solve());mesh.lengths=lengths;
    policy.boundary_scale_fraction=0;CHECK_THROWS(solve());policy.boundary_scale_fraction=.001;
    mesh.pinned[face]=1;
    for(const auto& split:solve())CHECK(std::find(split.faces.begin(),split.faces.end(),face)==split.faces.end());
    mesh.pinned[face]=0;palette[1].color=palette[3].color;policy.maximum_source_error_increase=0;
    geometry.reference_length=100;
    for(const auto& split:solve())CHECK(split.slot==105);
    CHECK_THROWS(constrain_selected_portrait_materials(mesh.regions,mesh.slots,mesh.roles,mesh.selected,mesh.pinned,
        mesh.neighbors,mesh.areas,mesh.source,palette,policy,[]{return true;},&geometry));
}

TEST_CASE("Geometric correction matches parallel shared edges by length without changing slot identities", "[PortraitColorConstraints][PortraitColorGeneralization]") {
    const auto palette=colors();const std::vector<uint32_t> regions{1,2};const std::vector<size_t> slots(2,41);
    const std::vector<PortraitFaceRole> roles(2,PortraitFaceRole::Skin);const std::vector<uint8_t> selected(2,1),pinned(2,0);
    const std::vector<std::array<int32_t,3>> neighbors{{1,1,1},{0,0,0}};
    const std::vector<std::array<float,3>> lengths{{1,2,3},{3,1,2}};
    const std::vector<double> areas(2,1);const std::vector<RegionRGB> source(2,palette[3].color);
    PortraitColorConstraintOptions policy;policy.lip_slots={41};const PortraitColorGeometry geometry{lengths,1};
    const auto result=constrain_selected_portrait_materials(regions,slots,roles,selected,pinned,neighbors,areas,source,palette,policy,{},&geometry);
    REQUIRE(result.size()==2);for(const auto& split:result)CHECK(split.slot==105);
}
