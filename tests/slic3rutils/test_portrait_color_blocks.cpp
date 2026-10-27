#include "slic3r/GUI/AI/ModelGeneration/PortraitColorBlockPostprocess.hpp"
#include "slic3r/GUI/AI/ModelGeneration/PortraitColorBlockCache.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelPreviewPalette.hpp"
#include "slic3r/GUI/AI/Orca/FilamentColorPack.hpp"
#include <catch2/catch_all.hpp>
#include <chrono>
#include <iostream>

using namespace Slic3r;
namespace PC=GUI::PortraitColorBlocks;
namespace SC=AI::SemanticColoring;
namespace {
struct Sample {
    SC::MeshSnapshot source;
    SC::Analysis analysis;
    Sample() {
        source.geometry_id=std::string(64,'a');source.content_id=std::string(64,'b');
        source.mesh.vertices={{0,0,0},{1,0,0},{0,1,0},{1,1,0},{0,2,0},{1,2,0}};
        source.mesh.indices={{0,1,2},{1,3,2},{2,3,4},{3,5,4}};
        source.face_colors.assign(4,RGBA{.4f,.4f,.4f,1.f});
        analysis.geometry_id=source.geometry_id;analysis.content_id=source.content_id;
        analysis.signature=std::string(64,'c');analysis.face_labels.assign(4,SC::Label::Unknown);
        analysis.face_confidence.assign(4,.95f);
    }
    PC::Input input() const {return {source,analysis};}
};
const PC::Block& block(const PC::Result& r,size_t face,size_t track=1) {
    const auto unit=std::find_if(r.units.begin(),r.units.end(),[&](const auto& u){return u.face==face;});
    return r.tracks[track].blocks[r.tracks[track].unit_blocks[unit-r.units.begin()]];
}
std::vector<SC::Color> honor_palette() {
    std::vector<SC::Color> result;
    for (const auto& slot : GUI::young_portrait_honor_suite().physical_slots) {
        const auto channel = [&](size_t offset) {
            return float(std::stoul(slot.color.substr(offset, 2), nullptr, 16)) / 255.f;
        };
        result.push_back({channel(1), channel(3), channel(5)});
    }
    return result;
}
PC::Input honor_input(const Sample& sample) {
    auto input = sample.input();
    input.strategy = PC::Strategy::ColorRegionMajorityV2;
    input.intermediate_palette = honor_palette();
    input.mapping = input.intermediate_palette;
    input.targets = input.intermediate_palette;
    input.final_color_count = input.targets.size();
    input.fixed_role_slots = GUI::young_portrait_honor_suite().semantic_role_slot_indices();
    return input;
}
}

TEST_CASE("Adjacent equal colors form one unresolved region across a fold", "[PortraitColorBlocks]") {
    Sample s;s.source.mesh.vertices[4].z()=2;s.source.mesh.vertices[5].z()=2;
    auto r=PC::build(s.input());
    REQUIRE(r->current().blocks.size()==1);
    REQUIRE(r->current().blocks.front().members.size()==4);
    REQUIRE(r->unresolved_count()==1);
}
TEST_CASE("ColorRegionMajorityV2 partitions by color before semantic voting", "[PortraitColorBlocks]") {
    Sample s;
    s.analysis.face_labels={SC::Label::FaceSkin,SC::Label::Hair,SC::Label::Unknown,SC::Label::Eyebrow};
    auto in=s.input(); in.strategy=PC::Strategy::ColorRegionMajorityV2;
    in.intermediate_palette={{.4f,.4f,.4f},{.8f,.8f,.8f}}; in.final_color_count=4;
    auto r=PC::build(in);
    REQUIRE(r->strategy==PC::Strategy::ColorRegionMajorityV2);
    REQUIRE(r->current().blocks.size()==1);
    REQUIRE(r->final_palette.size()==4);
    REQUIRE_FALSE(r->intermediate_palette.empty());
}
TEST_CASE("ColorRegionMajorityV2 cache round trips role locks and palettes", "[PortraitColorBlocks]") {
    Sample s; auto in=s.input(); in.strategy=PC::Strategy::ColorRegionMajorityV2;
    in.intermediate_palette={{.4f,.4f,.4f}}; in.final_color_count=4;
    auto r=PC::build(in); const auto doc=PC::encode(*r);
    REQUIRE(doc.at("schema")=="orca.portrait-color-blocks/v2");
    std::string error; auto restored=PC::decode(doc,s.source.geometry_id,4,s.source.content_id,s.analysis.signature,r->palette_signature,error);
    REQUIRE(restored); REQUIRE(restored->strategy==PC::Strategy::ColorRegionMajorityV2);
    REQUIRE(restored->final_palette==r->final_palette); REQUIRE(restored->role_locks==r->role_locks);
    REQUIRE(restored->final_slot_indices==r->final_slot_indices);
    REQUIRE(restored->role_slot_locks==r->role_slot_locks);
    REQUIRE(restored->role_regions==r->role_regions);
}
TEST_CASE("Honor portrait V2 binds confirmed anatomy to its shared physical slots", "[PortraitColorBlocks][HonorFilamentSuite]") {
    Sample sample;
    const auto palette = honor_palette();
    sample.analysis.face_labels = {SC::Label::FaceSkin, SC::Label::Hair,
        SC::Label::EyeSclera, SC::Label::MouthInterior};
    sample.analysis.face_confidence.assign(4, .95f);
    sample.source.face_colors = {{palette[0][0], palette[0][1], palette[0][2], 1.f},
        {palette[1][0], palette[1][1], palette[1][2], 1.f},
        {palette[2][0], palette[2][1], palette[2][2], 1.f},
        {palette[3][0], palette[3][1], palette[3][2], 1.f}};

    const auto result = PC::build(honor_input(sample));
    REQUIRE(block(*result, 0).display_slot == 0);
    REQUIRE(block(*result, 1).display_slot == 1);
    REQUIRE(block(*result, 2).display_slot == 2);
    REQUIRE(block(*result, 3).display_slot == 3);
    REQUIRE(PC::reliable_fixed_role_blocks(*result,
        GUI::young_portrait_honor_suite().semantic_role_slot_indices()) == 4);
    REQUIRE(PC::used_display_slots(*result) == std::vector<size_t>{0, 1, 2, 3});

    const auto document = PC::encode(*result);
    std::string error;
    const auto restored = PC::decode(document, sample.source.geometry_id, 4,
        sample.source.content_id, sample.analysis.signature, result->palette_signature, error);
    REQUIRE(restored);
    REQUIRE(restored->role_slot_locks == result->role_slot_locks);
    REQUIRE(PC::used_display_slots(*restored) == std::vector<size_t>{0, 1, 2, 3});
}
TEST_CASE("Honor portrait V2 lets hair iris and eyebrows share charcoal", "[PortraitColorBlocks][HonorFilamentSuite]") {
    Sample sample;
    const auto palette = honor_palette();
    sample.analysis.face_labels = {SC::Label::Hair, SC::Label::Iris,
        SC::Label::Eyebrow, SC::Label::EyeSclera};
    sample.analysis.face_confidence.assign(4, .95f);
    sample.source.face_colors = {{.15f, .12f, .10f, 1.f}, {.08f, .06f, .04f, 1.f},
        {.22f, .16f, .12f, 1.f}, {.92f, .92f, .90f, 1.f}};

    const auto result = PC::build(honor_input(sample));
    REQUIRE(block(*result, 0).display_slot == 1);
    REQUIRE(block(*result, 1).display_slot == 1);
    REQUIRE(block(*result, 2).display_slot == 1);
    REQUIRE(block(*result, 3).display_slot == 2);
    REQUIRE(block(*result, 0).display == palette[1]);
    REQUIRE(block(*result, 3).display == palette[2]);
}
TEST_CASE("Honor portrait V2 leaves low confidence and non anatomy regions on nearest colors", "[PortraitColorBlocks][HonorFilamentSuite]") {
    Sample sample;
    const auto palette = honor_palette();
    sample.analysis.face_labels = {SC::Label::FaceSkin, SC::Label::Clothes,
        SC::Label::Accessories, SC::Label::Unknown};
    sample.analysis.face_confidence = {.69f, .95f, .95f, .95f};
    sample.source.face_colors = {{palette[4][0], palette[4][1], palette[4][2], 1.f},
        {palette[5][0], palette[5][1], palette[5][2], 1.f},
        {palette[3][0], palette[3][1], palette[3][2], 1.f},
        {palette[2][0], palette[2][1], palette[2][2], 1.f}};

    const auto result = PC::build(honor_input(sample));
    REQUIRE(block(*result, 0).status == PC::Status::Unresolved);
    REQUIRE(block(*result, 0).display_slot == 4);
    REQUIRE(block(*result, 1).display_slot == 5);
    REQUIRE(block(*result, 2).display_slot == 3);
    REQUIRE(block(*result, 3).display_slot == 2);
    REQUIRE(PC::reliable_fixed_role_blocks(*result,
        GUI::young_portrait_honor_suite().semantic_role_slot_indices()) == 0);
}
TEST_CASE("Stale V2 algorithm caches are rejected", "[PortraitColorBlocks]") {
    Sample s; auto in = s.input(); in.strategy = PC::Strategy::ColorRegionMajorityV2;
    in.intermediate_palette = {{.4f, .4f, .4f}}; in.final_color_count = 4;
    const auto result = PC::build(in);
    auto doc = PC::encode(*result);
    doc["algorithm_version"] = PC::portrait_postprocess_algorithm_version - 1;
    std::string error;
    REQUIRE_FALSE(PC::decode(doc, s.source.geometry_id, 4, s.source.content_id,
        s.analysis.signature, result->palette_signature, error));
    REQUIRE_FALSE(error.empty());
}
TEST_CASE("ColorRegionMajorityV2 keeps non protected semantic ownership on color blocks", "[PortraitColorBlocks]") {
    Sample s;
    s.analysis.face_labels = {SC::Label::Clothes, SC::Label::Accessories,
        SC::Label::Clothes, SC::Label::Unknown};
    auto in = s.input(); in.strategy = PC::Strategy::ColorRegionMajorityV2;
    in.intermediate_palette = {{.4f, .4f, .4f}}; in.final_color_count = 4;
    const auto r = PC::build(in);
    REQUIRE(r->current().blocks.size() == 1);
    REQUIRE(r->current().blocks.front().semantic == SC::Label::Clothes);
}
TEST_CASE("ColorRegionMajorityV2 reserves independent core anatomy colors", "[PortraitColorBlocks]") {
    Sample s;
    s.analysis.face_labels = {SC::Label::FaceSkin, SC::Label::Lips, SC::Label::EyeSclera, SC::Label::Iris};
    s.source.face_colors = {{.72f,.42f,.30f,1.f}, {.75f,.08f,.12f,1.f},
        {.92f,.92f,.92f,1.f}, {.06f,.04f,.03f,1.f}};
    auto in = s.input(); in.strategy = PC::Strategy::ColorRegionMajorityV2;
    in.intermediate_palette = {{.72f,.42f,.30f},{.75f,.08f,.12f},{.92f,.92f,.92f},{.06f,.04f,.03f}};
    in.final_color_count = 4;
    const auto result = PC::build(in);
    REQUIRE(result->role_locks.size() == 4);
    REQUIRE(result->role_regions.size() == 4);
    const auto skin = result->role_locks.at("Skin");
    const auto lips = result->role_locks.at("Lips");
    const auto sclera = result->role_locks.at("EyeSclera");
    const auto iris = result->role_locks.at("Iris");
    REQUIRE(GUI::PreviewPalette::distance(GUI::PreviewPalette::to_lab(skin), GUI::PreviewPalette::to_lab(lips)) > 1e-8f);
    REQUIRE(GUI::PreviewPalette::distance(GUI::PreviewPalette::to_lab(sclera), GUI::PreviewPalette::to_lab(iris)) > 1e-8f);
}
TEST_CASE("Portrait color postprocess is restricted to four through six colors", "[PortraitColorBlocks]") {
    REQUIRE_FALSE(PC::portrait_color_postprocess_supported(3));
    REQUIRE(PC::portrait_color_postprocess_supported(4));
    REQUIRE(PC::portrait_color_postprocess_supported(5));
    REQUIRE(PC::portrait_color_postprocess_supported(6));
    REQUIRE_FALSE(PC::portrait_color_postprocess_supported(7));

    Sample s;
    auto in = s.input();
    in.strategy = PC::Strategy::ColorRegionMajorityV2;
    in.intermediate_palette = {{.1f, .1f, .1f}, {.9f, .9f, .9f}};
    in.final_color_count = 7;
    REQUIRE_THROWS(PC::build(in));
}
TEST_CASE("Portrait optimization preserves automatic suggestion slot identity at four five and six colors", "[PortraitColorBlocks]") {
    const size_t count = GENERATE(size_t(4), size_t(5), size_t(6));
    Sample s;
    s.analysis.face_labels = {SC::Label::FaceSkin, SC::Label::Lips, SC::Label::EyeSclera, SC::Label::Iris};
    s.source.face_colors = {{.72f,.42f,.30f,1.f}, {.75f,.08f,.12f,1.f},
        {.92f,.92f,.92f,1.f}, {.06f,.04f,.03f,1.f}};
    auto in = s.input();
    in.strategy = PC::Strategy::ColorRegionMajorityV2;
    in.intermediate_palette = {{.72f,.42f,.30f}, {.75f,.08f,.12f}, {.92f,.92f,.92f},
        {.06f,.04f,.03f}, {.05f,.55f,.75f}, {.20f,.65f,.25f}};
    in.targets = {{.05f,.55f,.75f}, {.92f,.92f,.92f}, {.06f,.04f,.03f},
        {.75f,.08f,.12f}, {.72f,.42f,.30f}, {.20f,.65f,.25f}};
    in.targets.resize(count);
    in.mapping = in.targets;
    in.final_color_count = count;
    const auto result = PC::build(in);

    REQUIRE(result->final_palette == in.targets);
    REQUIRE(result->final_slot_indices.size() == count);
    for (size_t slot = 0; slot < count; ++slot)
        REQUIRE(result->final_slot_indices[slot] == slot);
    for (const auto& [role, slot] : result->role_slot_locks) {
        REQUIRE(slot < count);
        REQUIRE(result->role_locks.at(role) == in.targets[slot]);
    }
    for (const auto& block : result->current().blocks) {
        REQUIRE(block.display_slot >= 0);
        REQUIRE(size_t(block.display_slot) < count);
        REQUIRE(block.display == in.targets[size_t(block.display_slot)]);
    }
}
TEST_CASE("Dark portrait roles prefer the darkest legal card slot", "[PortraitColorBlocks]") {
    Sample s;
    s.analysis.face_labels = {SC::Label::FaceSkin, SC::Label::Iris, SC::Label::Hair, SC::Label::Eyebrow};
    s.source.face_colors = {{.72f, .42f, .30f, 1.f}, {.28f, .12f, .05f, 1.f},
        {.40f, .22f, .12f, 1.f}, {.52f, .30f, .16f, 1.f}};
    auto in = s.input();
    in.strategy = PC::Strategy::ColorRegionMajorityV2;
    in.intermediate_palette = {{.72f, .42f, .30f}, {.28f, .12f, .05f}, {.40f, .22f, .12f}, {.52f, .30f, .16f}};
    in.targets = {{.72f, .42f, .30f}, {.55f, .25f, .12f}, {.02f, .02f, .02f}, {.95f, .95f, .95f}};
    in.final_color_count = 4;
    const auto result = PC::build(in);
    REQUIRE(result->role_locks.at("Iris") == SC::Color{.02f, .02f, .02f});
    REQUIRE(result->role_locks.at("Hair") == SC::Color{.02f, .02f, .02f});
    REQUIRE(result->role_locks.at("Eyebrow") == SC::Color{.02f, .02f, .02f});
}
TEST_CASE("Clothes compete for a remaining slot using the original source color", "[PortraitColorBlocks]") {
    Sample s;
    s.source.mesh.vertices.push_back({10, 0, 0});
    s.source.mesh.vertices.push_back({11, 0, 0});
    s.source.mesh.vertices.push_back({10, 1, 0});
    s.source.mesh.indices.push_back({6, 7, 8});
    s.source.face_colors.push_back({.05f, .55f, .75f, 1.f});
    s.analysis.face_labels = {SC::Label::FaceSkin, SC::Label::Lips, SC::Label::EyeSclera,
        SC::Label::Iris, SC::Label::Clothes};
    s.analysis.face_confidence.assign(5, .95f);
    s.source.face_colors[0] = {.72f, .42f, .30f, 1.f};
    s.source.face_colors[1] = {.75f, .08f, .12f, 1.f};
    s.source.face_colors[2] = {.92f, .92f, .92f, 1.f};
    s.source.face_colors[3] = {.06f, .04f, .03f, 1.f};
    auto in = s.input();
    in.strategy = PC::Strategy::ColorRegionMajorityV2;
    in.intermediate_palette = {{.72f, .42f, .30f}, {.75f, .08f, .12f}, {.92f, .92f, .92f},
        {.06f, .04f, .03f}, {.05f, .55f, .75f}};
    in.targets = in.intermediate_palette;
    in.final_color_count = 5;
    const auto result = PC::build(in);
    REQUIRE(result->final_palette.size() == 5);
    const auto clothes = std::find_if(result->current().blocks.begin(), result->current().blocks.end(),
        [](const auto &item) { return item.semantic == SC::Label::Clothes; });
    REQUIRE(clothes != result->current().blocks.end());
    REQUIRE(clothes->display == SC::Color{.05f, .55f, .75f});
}
TEST_CASE("Color card remapping preserves portrait regions and core slot separation", "[PortraitColorBlocks]") {
    Sample s;
    s.analysis.face_labels = {SC::Label::FaceSkin, SC::Label::Lips, SC::Label::EyeSclera, SC::Label::Iris};
    s.source.face_colors = {{.72f,.42f,.30f,1.f}, {.75f,.08f,.12f,1.f},
        {.92f,.92f,.92f,1.f}, {.06f,.04f,.03f,1.f}};
    auto in = s.input(); in.strategy = PC::Strategy::ColorRegionMajorityV2;
    in.intermediate_palette = {{.72f,.42f,.30f},{.75f,.08f,.12f},{.92f,.92f,.92f},{.06f,.04f,.03f}};
    in.final_color_count = 4;
    auto result = PC::build(in);
    const auto regions = result->role_regions;
    const std::vector<SC::Color> card = {{.60f,.30f,.20f},{.90f,.10f,.20f},{.98f,.98f,.98f},{.01f,.01f,.01f}};
    REQUIRE(PC::remap_to_palette(*result, card));
    REQUIRE(result->role_regions == regions);
    REQUIRE(result->final_palette == card);
    REQUIRE(result->role_locks.at("EyeSclera") != result->role_locks.at("Iris"));
    REQUIRE_FALSE(PC::remap_to_palette(*result, {{.5f,.5f,.5f},{.5f,.5f,.5f},{.7f,.7f,.7f},{.8f,.8f,.8f}}));
}
TEST_CASE("Gradual garment shading compares neighboring faces instead of one seed", "[PortraitColorBlocks]") {
    Sample s;
    s.source.face_colors[0]={.30f,.30f,.30f,1};
    s.source.face_colors[1]={.34f,.34f,.34f,1};
    s.source.face_colors[2]={.38f,.38f,.38f,1};
    s.source.face_colors[3]={.42f,.42f,.42f,1};
    const auto r=PC::build(s.input());
    REQUIRE(r->current().blocks.size()==1);
}
TEST_CASE("ColorRegionMajorityV2 does not chain a gradient through one semantic region", "[PortraitColorBlocks]") {
    Sample s;
    s.source.face_colors[0]={.30f,.30f,.30f,1};
    s.source.face_colors[1]={.34f,.34f,.34f,1};
    s.source.face_colors[2]={.38f,.38f,.38f,1};
    s.source.face_colors[3]={.42f,.42f,.42f,1};
    auto in=s.input();
    in.strategy=PC::Strategy::ColorRegionMajorityV2;
    in.intermediate_palette={{.30f,.30f,.30f},{.42f,.42f,.42f}};
    in.targets={{.30f,.30f,.30f},{.42f,.42f,.42f},{.15f,.15f,.15f},{.75f,.75f,.75f}};
    in.final_color_count=4;
    const auto r=PC::build(in);
    REQUIRE(r->current().blocks.size()>1);
}
TEST_CASE("Diagnostics attribute a color split to the color threshold", "[PortraitColorBlocks]") {
    Sample s;
    s.source.face_colors[0]={.1f,.1f,.1f,1};
    s.source.face_colors[1]={.9f,.9f,.9f,1};
    const auto r=PC::build(s.input());
    REQUIRE(r->current().blocks.size()>=2);
    REQUIRE(r->diagnostic_summary().color_blocked_edges>0);
    REQUIRE(std::any_of(r->current().blocks.begin(),r->current().blocks.end(),[](const auto &b) {
        return b.diagnostic.primary==PC::BlockCause::ColorThreshold;
    }));
}
TEST_CASE("Diagnostics keep semantic boundaries separate from color thresholds", "[PortraitColorBlocks]") {
    Sample s;
    s.analysis.face_labels[0]=SC::Label::FaceSkin;
    s.analysis.face_labels[1]=SC::Label::BodySkin;
    const auto r=PC::build(s.input());
    REQUIRE(r->current().blocks.size()>=2);
    REQUIRE(r->diagnostic_summary().semantic_blocked_edges>0);
    REQUIRE(std::any_of(r->current().blocks.begin(),r->current().blocks.end(),[](const auto &b) {
        return b.diagnostic.primary==PC::BlockCause::SemanticBoundary;
    }));
}
TEST_CASE("Uncertain skin boundaries do not split one same color region", "[PortraitColorBlocks]") {
    Sample s;
    s.analysis.face_labels[0]=SC::Label::FaceSkin;
    const auto r=PC::build(s.input());
    REQUIRE(r->current().blocks.size()==1);
    REQUIRE(r->diagnostic_summary().semantic_blocked_edges==0);
}
TEST_CASE("Confirmed eye feature boundaries remain protected", "[PortraitColorBlocks]") {
    Sample s;
    s.analysis.face_labels[0]=SC::Label::Iris;
    const auto r=PC::build(s.input());
    REQUIRE(r->current().blocks.size()>=2);
    REQUIRE(r->diagnostic_summary().semantic_blocked_edges>0);
    REQUIRE(std::any_of(r->current().blocks.begin(),r->current().blocks.end(),[](const auto &b) {
        return b.diagnostic.primary==PC::BlockCause::SemanticBoundary;
    }));
}
TEST_CASE("Diagnostics expose open and non manifold topology", "[PortraitColorBlocks]") {
    Sample open;
    const auto open_result=PC::build(open.input());
    REQUIRE(open_result->diagnostic_summary().topology_disconnected>0);

    Sample non_manifold;
    non_manifold.source.mesh.vertices.push_back({0,-1,0});
    non_manifold.source.mesh.vertices.push_back({0,0,-1});
    non_manifold.source.mesh.indices.push_back({0,1,6});
    non_manifold.source.mesh.indices.push_back({0,1,7});
    non_manifold.source.face_colors.push_back({.4f,.4f,.4f,1});
    non_manifold.source.face_colors.push_back({.4f,.4f,.4f,1});
    non_manifold.analysis.face_labels.push_back(SC::Label::Unknown);
    non_manifold.analysis.face_labels.push_back(SC::Label::Unknown);
    non_manifold.analysis.face_confidence.push_back(.95f);
    non_manifold.analysis.face_confidence.push_back(.95f);
    const auto non_manifold_result=PC::build(non_manifold.input());
    REQUIRE(non_manifold_result->diagnostic_summary().nonmanifold_edges>0);
}
TEST_CASE("Diagnostic fields are optional for old color block caches", "[PortraitColorBlocks]") {
    Sample s; const auto r=PC::build(s.input()); auto doc=PC::encode(*r);
    for(auto &track:doc["tracks"]) for(auto &block:track["blocks"]) block.erase("diagnostic");
    std::string error;
    const auto restored=PC::decode(doc,s.source.geometry_id,4,s.source.content_id,s.analysis.signature,r->palette_signature,error);
    REQUIRE(restored); REQUIRE(error.empty());
    for(const auto &b:restored->current().blocks)
        REQUIRE(b.diagnostic.primary==PC::BlockCause::None);
}
TEST_CASE("Equal colors on disconnected surfaces remain separate", "[PortraitColorBlocks]") {
    Sample s;s.source.mesh.vertices.push_back({10,0,0});s.source.mesh.vertices.push_back({11,0,0});s.source.mesh.vertices.push_back({10,1,0});
    s.source.mesh.indices[3]={6,7,8};
    auto r=PC::build(s.input());REQUIRE(r->current().blocks.size()==2);
}
TEST_CASE("Non anatomy labels do not fragment one continuous color region", "[PortraitColorBlocks]") {
    Sample s;
    s.analysis.face_labels={SC::Label::Clothes,SC::Label::Accessories,SC::Label::Unknown,SC::Label::Clothes};
    const auto r=PC::build(s.input());
    REQUIRE(r->current().blocks.size()==1);
}
TEST_CASE("Face partition follows connected color block identity", "[PortraitColorBlocks]") {
    Sample s;
    s.source.face_colors[0] = {.9f,.1f,.1f,1.f};
    s.source.face_colors[1] = {.9f,.1f,.1f,1.f};
    s.source.face_colors[2] = {.1f,.1f,.9f,1.f};
    s.source.face_colors[3] = {.1f,.1f,.9f,1.f};
    const auto r = PC::build(s.input());
    const auto partition = r->face_partition();
    REQUIRE(partition.size() == s.source.mesh.indices.size());
    REQUIRE(partition[0] == partition[1]);
    REQUIRE(partition[2] == partition[3]);
    REQUIRE(partition[0] != partition[2]);
    REQUIRE(std::all_of(partition.begin(), partition.end(), [](uint32_t id) { return id != 0; }));
}
TEST_CASE("Face partition splits disconnected islands after unit collapse", "[PortraitColorBlocks]") {
    PC::Result result;
    result.face_count = 4;
    result.units = {{0,{0,0}},{1,{0,0}},{2,{0,0}},{3,{0,0}}};
    result.tracks[1].unit_blocks = {0,0,0,0};
    result.tracks[1].blocks.push_back({0,{0,1,2,3},{.4f,.4f,.4f},{.4f,.4f,.4f},SC::Label::Unknown,PC::Status::Unresolved,0.,{}, {}, -1});
    const auto partition = result.face_partition({{1},{0},{3},{2}});
    REQUIRE(partition == std::vector<uint32_t>{1,1,2,2});
}
TEST_CASE("Minor source texture errors share a stable color block", "[PortraitColorBlocks]") {
    Sample s;s.source.face_colors[1]={.401f,.402f,.4f,1};
    auto r=PC::build(s.input());REQUIRE(r->current().blocks.size()==1);
    REQUIRE(PC::encode(*r)==PC::encode(*PC::build(s.input())));
}
TEST_CASE("High confidence anatomy seeds label only their same color neighbors", "[PortraitColorBlocks]") {
    const auto label=GENERATE(SC::Label::Iris,SC::Label::EyeSclera,SC::Label::Lips,SC::Label::FaceSkin,SC::Label::BodySkin);
    Sample s;s.analysis.face_labels[0]=label;
    auto r=PC::build(s.input());REQUIRE(block(*r,1).semantic==label);
    REQUIRE(block(*r,1).status==PC::Status::Confirmed);
    s.analysis.face_confidence[0]=.1f;r=PC::build(s.input());
    REQUIRE(block(*r,1).status==PC::Status::Unresolved);
}
TEST_CASE("Conflicting anatomy seeds do not claim the unknown color region", "[PortraitColorBlocks]") {
    Sample s;s.analysis.face_labels[0]=SC::Label::Iris;s.analysis.face_labels[3]=SC::Label::Lips;
    auto r=PC::build(s.input());
    REQUIRE(block(*r,1).semantic==SC::Label::Unknown);
    REQUIRE(block(*r,1).candidate==-1);
    REQUIRE(block(*r,1).reason=="conflicting_semantic_seeds");
}
TEST_CASE("Person identity prevents color propagation and temporary replacement", "[PortraitColorBlocks]") {
    Sample s;s.analysis.face_labels[0]=SC::Label::FaceSkin;
    auto in=s.input();in.person_ids={1,2,2,2};auto r=PC::build(in);
    REQUIRE(block(*r,1).status==PC::Status::Unresolved);
    REQUIRE(block(*r,1).candidate==-1);
}
TEST_CASE("An unresolved region previews a confirmed lighter neighbor without gaining its label", "[PortraitColorBlocks]") {
    Sample s;s.source.face_colors[0]={.9f,.9f,.9f,1};s.analysis.face_labels[0]=SC::Label::FaceSkin;
    auto r=PC::build(s.input());const auto& b=block(*r,1);
    REQUIRE(b.members.size()==3);
    REQUIRE(b.status==PC::Status::Unresolved);REQUIRE(b.semantic==SC::Label::Unknown);
    REQUIRE(b.candidate==block(*r,0).id);REQUIRE(b.display==block(*r,0).original);
}
TEST_CASE("Temporary neighbors never cascade their candidate through another region", "[PortraitColorBlocks]") {
    Sample s;s.source.face_colors[0]={.9f,.9f,.9f,1};s.source.face_colors[3]={.1f,.1f,.1f,1};
    s.analysis.face_labels[0]=SC::Label::FaceSkin;auto r=PC::build(s.input());
    REQUIRE(block(*r,1).candidate>=0);REQUIRE(block(*r,3).candidate==-1);
    REQUIRE(block(*r,3).display==block(*r,3).original);
}
TEST_CASE("A darker confirmed neighbor leaves unresolved source colors unchanged", "[PortraitColorBlocks]") {
    Sample s;s.source.face_colors[0]={.1f,.1f,.1f,1};s.analysis.face_labels[0]=SC::Label::FaceSkin;
    auto r=PC::build(s.input());REQUIRE(block(*r,1).candidate==-1);
    REQUIRE(block(*r,1).display==block(*r,1).original);
}
TEST_CASE("Source and ordinary color tracks are independent and reversible", "[PortraitColorBlocks]") {
    Sample s;auto in=s.input();in.mapping={{.4f,.4f,.4f}};in.targets={{.9f,.1f,.1f}};
    auto r=PC::build(in);REQUIRE(r->active==PC::Track::ProcessedFaceColor);
    REQUIRE(r->tracks[0].blocks[0].display!=r->tracks[1].blocks[0].display);
    const auto before=PC::encode(*r);auto edited=*r;
    REQUIRE(edited.recolor(0,{.2f,.3f,.4f}));REQUIRE(edited.unresolved_count()==0);
    REQUIRE(edited.current().blocks[0].status==PC::Status::ManualConfirmed);
    REQUIRE(PC::encode(*r)==before);REQUIRE(edited.tracks[0].blocks[0].status==PC::Status::Unresolved);
}
TEST_CASE("Existing subfaces retain complete coverage and exact member identity", "[PortraitColorBlocks]") {
    Sample s;auto in=s.input();in.processed_subfaces={{1,{1,0},{.95f,.1f,.1f},1}};
    auto r=PC::build(in);REQUIRE(r->units.size()==7);
    SC::FaceColors faces;SC::SubfaceColors children;r->appearance(faces,children);
    REQUIRE(faces.size()==3);REQUIRE(children.size()==4);
    REQUIRE(std::any_of(children.begin(),children.end(),[](const auto& c){return c.face_id==1 && c.path.value==0 && c.color[0]==.95f;}));
    std::string error;auto decoded=PC::decode(PC::encode(*r),s.source.geometry_id,4,s.source.content_id,s.analysis.signature,r->palette_signature,error);
    REQUIRE(decoded);REQUIRE(PC::encode(*decoded)==PC::encode(*r));
}
TEST_CASE("Corrupt or stale block caches are rejected without changing ordinary appearance", "[PortraitColorBlocks]") {
    Sample s;auto r=PC::build(s.input());const auto original=PC::encode(*r);auto doc=original;
    const int failure=GENERATE(0,1,2,3,4,5);
    switch(failure) {
    case 0:doc["geometry_id"]="changed";break;case 1:doc["source_signature"]="changed";break;
    case 2:doc["semantic_signature"]="changed";break;case 3:doc["palette_signature"]="changed";break;
    case 4:doc["tracks"][1]["blocks"][0]["members"].push_back(0);break;
    default:doc["units"][0][1]=4;break;
    }
    std::string error;REQUIRE_FALSE(PC::decode(doc,s.source.geometry_id,4,s.source.content_id,s.analysis.signature,r->palette_signature,error));
    REQUIRE_FALSE(error.empty());REQUIRE(PC::encode(*r)==original);
}
TEST_CASE("Addressed block cache preserves both tracks and rejects missing files", "[PortraitColorBlocks]") {
    Sample s;auto r=PC::build(s.input());
    const auto root=boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("orca-color-blocks-%%%%-%%%%");
    const auto ref=PC::save(*r,root);std::string error;
    auto restored=PC::load(ref,root,s.source.geometry_id,4,s.source.content_id,error);
    REQUIRE(restored);REQUIRE(PC::encode(*restored)==PC::encode(*r));
    boost::filesystem::remove(root/(ref.at("content_id").get<std::string>()+".json"));
    REQUIRE_FALSE(PC::load(ref,root,s.source.geometry_id,4,s.source.content_id,error));
    boost::filesystem::remove(root);
}
TEST_CASE("Canceled block generation never returns a partially colored candidate", "[PortraitColorBlocks]") {
    Sample s;REQUIRE_THROWS(PC::build(s.input(),[]{return true;}));
}
TEST_CASE("Known non anatomy colors remain unresolved semantic barriers", "[PortraitColorBlocks]") {
    Sample s;s.analysis.face_labels[0]=SC::Label::FaceSkin;s.analysis.face_labels[1]=SC::Label::Hair;
    s.source.face_colors[0]={.95f,.95f,.95f,1};
    const auto r=PC::build(s.input());
    REQUIRE(block(*r,1).status==PC::Status::Unresolved);
    REQUIRE(block(*r,1).semantic==SC::Label::Hair);
    REQUIRE(block(*r,1).candidate==-1);
    REQUIRE(block(*r,2).candidate==-1);
}
TEST_CASE("Explicit reruns preserve manual confirmations on both tracks and child boundaries", "[PortraitColorBlocks]") {
    Sample s;auto in=s.input();in.processed_subfaces={{1,{2,15},{.95f,.1f,.1f},1}};
    auto previous=PC::build(in);
    const auto unit=std::find_if(previous->units.begin(),previous->units.end(),[](const auto& u){return u.face==1 && u.path.depth==2 && u.path.value==15;});
    REQUIRE(unit!=previous->units.end());const auto index=size_t(unit-previous->units.begin());
    REQUIRE(previous->recolor(previous->tracks[1].unit_blocks[index],{.1f,.2f,.9f}));
    previous->active=PC::Track::SourceAppearance;
    REQUIRE(previous->recolor(previous->tracks[0].unit_blocks[0],{.2f,.8f,.1f}));
    auto rerun=s.input();rerun.previous=previous;
    const auto result=PC::build(rerun);
    const auto found=std::find_if(result->units.begin(),result->units.end(),[](const auto& u){return u.face==1 && u.path.depth==2 && u.path.value==15;});
    REQUIRE(found!=result->units.end());
    const auto& child=result->tracks[1].blocks[result->tracks[1].unit_blocks[found-result->units.begin()]];
    REQUIRE(child.status==PC::Status::ManualConfirmed);
    REQUIRE(child.display==(SC::Color{.1f,.2f,.9f}));
    REQUIRE(block(*result,0,0).status==PC::Status::ManualConfirmed);
    REQUIRE(block(*result,0,0).display==(SC::Color{.2f,.8f,.1f}));
    REQUIRE(result->active==PC::Track::ProcessedFaceColor);
    rerun.previous=previous;s.source.content_id=std::string(64,'f');
    REQUIRE_THROWS(PC::build(rerun));
}
TEST_CASE("Equal boundary candidates use perceptual distance and stable block identity", "[PortraitColorBlocks]") {
    Sample s;s.source.mesh.vertices={{0,0,0},{1,0,0},{0,1,0},{1,1,0},{0,2,0},{1,2,0}};
    s.analysis.face_labels[0]=SC::Label::FaceSkin;s.analysis.face_labels[3]=SC::Label::BodySkin;
    s.source.face_colors[0]={.95f,.95f,.95f,1};s.source.face_colors[3]={.65f,.65f,.65f,1};
    auto r=PC::build(s.input());REQUIRE(block(*r,1).candidate==block(*r,3).id);
    s.source.face_colors[3]=s.source.face_colors[0];
    r=PC::build(s.input());REQUIRE(block(*r,1).candidate==block(*r,0).id);
}
TEST_CASE("Oversized cache indices never wrap to valid block members", "[PortraitColorBlocks]") {
    Sample s;auto r=PC::build(s.input());auto doc=PC::encode(*r);
    const int part=GENERATE(0,1,2,3,4);
    const uint64_t huge=uint64_t(1)<<32;
    if(part==0)doc["units"][0][1]=huge;
    if(part==1)doc["tracks"][0]["unit_blocks"][0]=huge;
    if(part==2)doc["tracks"][0]["blocks"][0]["members"][0]=huge;
    if(part==3)doc["active_track"]=huge;
    if(part==4)doc["tracks"][0]["blocks"][0]["semantic"]=huge;
    std::string error;REQUIRE_FALSE(PC::decode(doc,s.source.geometry_id,4,s.source.content_id,s.analysis.signature,r->palette_signature,error));
}
TEST_CASE("High confidence midpoint evidence keeps small anatomy seeds as separate units", "[PortraitColorBlocks]") {
    Sample s;s.analysis.subface_labels.push_back({1,{4,255},SC::Label::Iris,.95f});
    auto r=PC::build(s.input());
    const auto found=std::find_if(r->units.begin(),r->units.end(),[](const auto& u){return u.face==1 && u.path.depth==4 && u.path.value==255;});
    REQUIRE(found!=r->units.end());
    REQUIRE(r->current().blocks[r->current().unit_blocks[found-r->units.begin()]].semantic==SC::Label::Iris);
    std::string error;REQUIRE(PC::decode(PC::encode(*r),s.source.geometry_id,4,s.source.content_id,s.analysis.signature,r->palette_signature,error));
}
TEST_CASE("A longer shared boundary wins over a closer brighter color", "[PortraitColorBlocks]") {
    Sample s;s.source.mesh.vertices[4]={-3,2,0};
    s.source.face_colors[0]={.6f,.6f,.6f,1};s.source.face_colors[3]={.95f,.95f,.95f,1};
    s.analysis.face_labels[0]=SC::Label::FaceSkin;s.analysis.face_labels[3]=SC::Label::BodySkin;
    auto r=PC::build(s.input());
    REQUIRE(block(*r,1).candidate==block(*r,3).id);
    REQUIRE(block(*r,1).semantic==SC::Label::Unknown);
}
TEST_CASE("Confirmed anatomy boundaries never merge equal colors", "[PortraitColorBlocks]") {
    Sample s;s.analysis.face_labels={SC::Label::Iris,SC::Label::EyeSclera,SC::Label::Lips,SC::Label::FaceSkin};
    auto r=PC::build(s.input());REQUIRE(r->current().blocks.size()==4);
    for(size_t i=0;i<4;++i)REQUIRE(block(*r,i).semantic==s.analysis.face_labels[i]);
}
TEST_CASE("Fractional cache identities never become triangle or block indices", "[PortraitColorBlocks]") {
    Sample s;auto r=PC::build(s.input());auto doc=PC::encode(*r);
    const int part=GENERATE(0,1,2);
    if(part==0)doc["units"][0][0]=.5;
    if(part==1)doc["tracks"][0]["unit_blocks"][0]=.5;
    if(part==2)doc["tracks"][0]["blocks"][0]["members"][0]=.5;
    std::string error;REQUIRE_FALSE(PC::decode(doc,s.source.geometry_id,4,s.source.content_id,s.analysis.signature,r->palette_signature,error));
}
TEST_CASE("One million connected triangles stay within the retained color block budget", "[PortraitColorBlocksPerformance]") {
    Sample s;constexpr int w=1000,h=500;
    s.source.mesh.vertices.clear();s.source.mesh.indices.clear();
    for(int y=0;y<=h;++y)for(int x=0;x<=w;++x)s.source.mesh.vertices.emplace_back(float(x),float(y),0.f);
    for(int y=0;y<h;++y)for(int x=0;x<w;++x) {
        const int a=y*(w+1)+x;s.source.mesh.indices.emplace_back(a,a+1,a+w+1);s.source.mesh.indices.emplace_back(a+1,a+w+2,a+w+1);
    }
    const auto n=s.source.mesh.indices.size();s.source.face_colors.assign(n,{.4f,.4f,.4f,1});
    s.analysis.face_labels.assign(n,SC::Label::Unknown);s.analysis.face_confidence.assign(n,.95f);
    const auto start=std::chrono::steady_clock::now();const auto r=PC::build(s.input());
    const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    std::cout<<"PortraitColorBlocks faces="<<n<<" elapsed_ms="<<ms<<" retained_bytes="<<r->memory_bytes()<<std::endl;
    REQUIRE(r->units.size()==n);REQUIRE(r->current().blocks.size()==1);
    REQUIRE(r->memory_bytes()<256ULL*1024*1024);
}
