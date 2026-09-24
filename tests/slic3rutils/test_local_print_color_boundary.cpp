#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "slic3r/GUI/AI/ModelGeneration/LocalPrintColorBoundaryRefinement.hpp"

using namespace Slic3r;
namespace Matching = GUI::LocalPrintColorMatching;
namespace Boundary = GUI::LocalPrintColorBoundaryRefinement;

namespace {
indexed_triangle_set tetrahedron()
{
    indexed_triangle_set mesh;
    mesh.vertices = {{1,1,1},{-1,-1,1},{-1,1,-1},{1,-1,-1}};
    mesh.indices = {{0,2,1},{0,1,3},{0,3,2},{1,2,3}};
    return mesh;
}
indexed_triangle_set octahedron()
{
    indexed_triangle_set mesh;
    mesh.vertices = {{1,0,0},{0,1,0},{-1,0,0},{0,-1,0},{0,0,1},{0,0,-1}};
    for (int i=0; i<4; ++i) mesh.indices.emplace_back(4,i,(i+1)%4);
    for (int i=0; i<4; ++i) mesh.indices.emplace_back(5,(i+1)%4,i);
    return mesh;
}
struct Example {
    indexed_triangle_set mesh;
    Matching::Input input;
    Matching::Computation baseline;
};
Example example(indexed_triangle_set mesh, std::vector<size_t> labels, const std::vector<size_t>& desired,
                const std::vector<std::string>& colors = {"#000000", "#FFFFFF"})
{
    Example e; e.mesh=std::move(mesh);
    auto& id=e.input.identity;
    id.algorithm_version="boundary-test-baseline";
    id.source_sha256=std::string(64,'a');
    id.geometry_id=AI::SurfaceSelectionPersistence::geometry_fingerprint(e.mesh);
    id.material_fingerprint="test-materials"; id.process_fingerprint="test-process";
    id.face_count=labels.size(); id.requested_color_count=colors.size();
    id.color_tolerance=e.input.tolerance; id.important_area_floor=e.input.important_area_floor;
    id.face_targets=std::move(labels);
    for (size_t t=0; t<colors.size(); ++t) {
        id.physical_channels.push_back({t,colors[t],"PLA",true});
        AI::PrintColorTarget target;
        target.source=target.output=Matching::hex_rgb(colors[t]);
        target.physical_slot=t; target.executable=true; target.evidence=AI::ColorEvidence::Estimated;
        id.targets.push_back(target);
    }
    for (size_t f=0; f<desired.size(); ++f) {
        const auto& tri=e.mesh.indices[f];
        const double area=.5*(e.mesh.vertices[tri[1]].cast<double>()-e.mesh.vertices[tri[0]].cast<double>()).cross(
            e.mesh.vertices[tri[2]].cast<double>()-e.mesh.vertices[tri[0]].cast<double>()).norm();
        e.input.faces.push_back({Matching::hex_rgb(colors[desired[f]]),area});
        id.targets[id.face_targets[f]].area+=area;
    }
    for (size_t f=0; f<desired.size(); ++f) {
        auto& t=id.targets[id.face_targets[f]];
        t.delta_e00+=e.input.faces[f].area/t.area*Matching::delta_e(e.input.faces[f].color,t.output);
    }
    for (auto& t:id.targets) {
        t.within_tolerance=t.delta_e00<=id.color_tolerance;
        if (!t.within_tolerance) t.unresolved_reason="Test baseline display residual.";
    }
    e.baseline.result=id;
    std::string error;
    REQUIRE(e.baseline.result.valid(error));
    return e;
}
bool notice_contains(const Matching::Computation& result, const std::string& text)
{
    return std::any_of(result.result.notices.begin(),result.result.notices.end(),[&](const auto& n) {return n.find(text)!=std::string::npos;});
}
Example movable() { return example(tetrahedron(),{0,0,1,1},{1,0,1,1}); }
}

TEST_CASE("a boundary move reduces actual face error and preserves physical outputs", "[LocalPrintColorBoundary]")
{
    auto e=movable();
    const auto result=Boundary::compute_refined(e.input,e.mesh,e.baseline);
    REQUIRE(result.ok());
    CHECK(result.result.face_targets==std::vector<size_t>{1,0,1,1});
    CHECK(result.result.algorithm_version==e.baseline.result.algorithm_version);
    CHECK(result.result.source_sha256==e.input.identity.source_sha256);
    CHECK(result.result.geometry_id==e.input.identity.geometry_id);
    CHECK(notice_contains(result,"not a printability"));
    std::string error; CHECK(result.result.valid(error));
    for (size_t t=0;t<2;++t) {
        CHECK(result.result.targets[t].physical_slot==e.baseline.result.targets[t].physical_slot);
        for(size_t c=0;c<3;++c) {
            CHECK_THAT(result.result.targets[t].output[c],Catch::Matchers::WithinAbs(e.baseline.result.targets[t].output[c],1e-7));
            CHECK_THAT(result.result.targets[t].source[c],Catch::Matchers::WithinAbs(result.result.targets[t].output[c],1e-7));
        }
        CHECK_THAT(result.result.targets[t].delta_e00,Catch::Matchers::WithinAbs(0,1e-10));
        CHECK(result.result.targets[t].within_tolerance);
        CHECK_THAT(result.result.targets[t].area,Catch::Matchers::WithinAbs(e.input.faces[0].area*(t==0?1:3),1e-10));
    }
}

TEST_CASE("explicit edits protected regions locks and both contrast endpoints remain fixed", "[LocalPrintColorBoundary]")
{
    auto e=movable();
    const auto constraint=GENERATE(0,1,2,3,4);
    auto& id=e.input.identity;
    if (constraint==0) id.user_overrides={{0,{1,1,1}}};
    else {
        id.regions={{"detail","person","detail",1,false,{0}}};
        if (constraint==1) id.regions[0].user_protected=true;
        if (constraint==2) id.regions[0].locked_physical_slot=0;
        if (constraint>=3) {
            id.regions.push_back({"other","person","other",1,false,{2}});
            id.contrasts={{"other","detail",1,0,constraint==4}};
        }
    }
    e.baseline.result=id;
    const auto result=Boundary::compute_refined(e.input,e.mesh,e.baseline);
    REQUIRE(result.ok());
    CHECK(result.result.face_targets==e.baseline.result.face_targets);
    CHECK(notice_contains(result,"no eligible move"));
}

TEST_CASE("refined residuals use actual source faces and explicit edits rather than the old seed", "[LocalPrintColorBoundary]")
{
    auto e=movable();
    e.input.faces[0].color={.8f,.8f,.8f};
    e.input.faces[2].color={0,0,0};
    e.input.identity.user_overrides={{2,{.9f,.9f,.9f}}};
    e.baseline.result.user_overrides=e.input.identity.user_overrides;
    const auto result=Boundary::compute_refined(e.input,e.mesh,e.baseline);
    REQUIRE(result.ok());
    CHECK(result.result.face_targets==std::vector<size_t>{1,0,1,1});
    const auto& target=result.result.targets[1];
    const double expected=(Matching::delta_e({.8f,.8f,.8f},{1,1,1})+
                           Matching::delta_e({.9f,.9f,.9f},{1,1,1}))/3;
    CHECK_THAT(target.delta_e00,Catch::Matchers::WithinAbs(expected,1e-8));
    for(size_t c=0;c<3;++c) CHECK_THAT(target.source[c],Catch::Matchers::WithinAbs(.9,1e-7));
    CHECK(target.within_tolerance==(expected<=e.input.tolerance));
    CHECK_THAT(e.input.faces[2].color[0],Catch::Matchers::WithinAbs(0,1e-10)); // Original sample is retained.
}

TEST_CASE("duplicate vertices across texture seams use exact complete edges", "[LocalPrintColorBoundary]")
{
    auto e=movable(); indexed_triangle_set seams;
    for (const auto& face:e.mesh.indices) {
        const int start=int(seams.vertices.size());
        for (int c=0;c<3;++c) seams.vertices.push_back(e.mesh.vertices[face[c]]);
        seams.indices.emplace_back(start,start+1,start+2);
    }
    REQUIRE(AI::SurfaceSelectionPersistence::geometry_fingerprint(seams)==e.input.identity.geometry_id);
    const auto result=Boundary::compute_refined(e.input,seams,e.baseline);
    REQUIRE(result.ok());
    CHECK(result.result.face_targets==std::vector<size_t>{1,0,1,1});
}

TEST_CASE("missing open invalid and nonmanifold topology retain the complete baseline", "[LocalPrintColorBoundary]")
{
    auto e=movable(); const auto damage=GENERATE(0,1,2,3,4,5);
    if (damage==0) e.mesh={};
    if (damage==1) e.mesh.vertices[0].x()=std::numeric_limits<float>::infinity();
    if (damage==2) e.mesh.indices[0][0]=999;
    if (damage==3) e.mesh.vertices[0]=e.mesh.vertices[1];
    if (damage==4) {
        e.mesh.indices[0]=e.mesh.indices[1]; // Duplicate face creates one- and three-face edges.
        e.input.identity.geometry_id=AI::SurfaceSelectionPersistence::geometry_fingerprint(e.mesh);
        e.baseline.result.geometry_id=e.input.identity.geometry_id;
    }
    if (damage==5) {
        e.mesh.vertices.push_back(e.mesh.vertices[0]+Vec3f(.001f,0,0));
        e.mesh.indices[0][0]=4; // Almost coincident is deliberately not welded.
        e.input.identity.geometry_id=AI::SurfaceSelectionPersistence::geometry_fingerprint(e.mesh);
        e.baseline.result.geometry_id=e.input.identity.geometry_id;
    }
    const auto result=Boundary::compute_refined(e.input,e.mesh,e.baseline);
    REQUIRE(result.ok());
    CHECK(result.result.face_targets==e.baseline.result.face_targets);
    CHECK(notice_contains(result,"kept the baseline"));
}

TEST_CASE("a better remote color cannot seed a new island", "[LocalPrintColorBoundary]")
{
    auto mesh=tetrahedron(); const auto second=tetrahedron();
    for (const auto& v:second.vertices) mesh.vertices.push_back(v+Vec3f(10,0,0));
    for (const auto& f:second.indices) mesh.indices.emplace_back(f[0]+4,f[1]+4,f[2]+4);
    auto e=example(std::move(mesh),{0,0,1,1,2,2,2,2},{2,0,1,1,2,2,2,2},{"#000000","#FFFFFF","#FF0000"});
    const auto result=Boundary::compute_refined(e.input,e.mesh,e.baseline);
    REQUIRE(result.ok());
    for(size_t f=0;f<4;++f) CHECK(result.result.face_targets[f]!=2);
}

TEST_CASE("a simultaneous round cannot remove an existing output group", "[LocalPrintColorBoundary]")
{
    auto e=example(tetrahedron(),{0,1,1,1},{1,1,1,1});
    const auto result=Boundary::compute_refined(e.input,e.mesh,e.baseline);
    REQUIRE(result.ok());
    CHECK(result.result.face_targets==e.baseline.result.face_targets);
    CHECK(notice_contains(result,"empty an existing target"));
}

TEST_CASE("simultaneous locally valid moves roll back when total boundary grows", "[LocalPrintColorBoundary]")
{
    // Octahedron dual graph: the three individually valid flips grow the final
    // boundary from five equal edges to six. Checking local moves is insufficient.
    auto e=example(octahedron(),{0,0,0,0,0,1,1,1},{0,0,0,0,1,0,1,0});
    const auto result=Boundary::compute_refined(e.input,e.mesh,e.baseline);
    REQUIRE(result.ok());
    CHECK(result.result.face_targets==e.baseline.result.face_targets);
    CHECK(notice_contains(result,"increase total color-boundary"));
}

TEST_CASE("a target cannot fragment even when the total boundary stays equal", "[LocalPrintColorBoundary]")
{
    // Nine edges both before and after; black components rise two -> three,
    // while white decreases three -> two. Total component count would hide it.
    auto e=example(octahedron(),{0,0,0,1,1,0,1,0},{1,0,1,1,0,1,0,1});
    const auto result=Boundary::compute_refined(e.input,e.mesh,e.baseline);
    REQUIRE(result.ok());
    CHECK(result.result.face_targets==e.baseline.result.face_targets);
    CHECK(notice_contains(result,"increase a target's connected-component"));
}

TEST_CASE("changed identity evaluation settings and native geometry cannot refine a stale result", "[LocalPrintColorBoundary]")
{
    auto e=movable(); const auto drift=GENERATE(0,1,2,3,4,5,6,7);
    if (drift==0) e.input.identity.source_sha256=std::string(64,'b');
    if (drift==1) e.input.identity.geometry_id="new-geometry";
    if (drift==2) e.input.identity.face_count++;
    if (drift==3) e.input.identity.material_fingerprint="new-materials";
    if (drift==4) e.input.identity.process_fingerprint="new-process";
    if (drift==5) e.input.important_area_floor=.5;
    if (drift==6) e.input.tolerance=20;
    if (drift==7) e.mesh.vertices[0].x()+=.1f;
    const auto result=Boundary::compute_refined(e.input,e.mesh,e.baseline);
    REQUIRE(result.ok());
    CHECK(result.result.face_targets==e.baseline.result.face_targets);
    CHECK(notice_contains(result,"kept the baseline"));
}

TEST_CASE("cancellation before and during refinement cannot return successful computed changes", "[LocalPrintColorBoundary]")
{
    auto e=movable(); const int phase=GENERATE(0,1,2); int checkpoints=0;
    if(GENERATE(false,true)) {
        e.input.identity.mode=e.baseline.result.mode=AI::PrintColorMode::Layered;
        e.input.identity.requested_color_count=e.baseline.result.requested_color_count=8;
    }
    e.input.cancelled=[&] {++checkpoints; return false;};
    REQUIRE(Boundary::compute_refined(e.input,e.mesh,e.baseline).ok());
    REQUIRE(checkpoints>2);
    const int threshold=phase==0?1:phase==1?checkpoints/2:checkpoints; int polls=0;
    e.input.cancelled=[&] {return ++polls>=threshold;};
    const auto result=Boundary::compute_refined(e.input,e.mesh,e.baseline);
    CHECK(result.cancelled);
    CHECK_FALSE(result.ok());
    CHECK(result.result.face_targets==e.baseline.result.face_targets);
}

TEST_CASE("layered shared physical groups refine without creating tools or removing source groups", "[LocalPrintColorBoundary]")
{
    indexed_triangle_set mesh;
    std::vector<size_t> labels, desired;
    for (int part=0; part<4; ++part) {
        const auto shape=tetrahedron(); const int offset=int(mesh.vertices.size());
        for (const auto& p:shape.vertices) mesh.vertices.push_back(p+Vec3f(float(part*5),0,0));
        for (const auto& f:shape.indices) mesh.indices.emplace_back(f[0]+offset,f[1]+offset,f[2]+offset);
        labels.insert(labels.end(),{0,0,1,1}); desired.insert(desired.end(),{1,0,1,1});
    }
    auto e=example(mesh,labels,desired);
    auto& id=e.input.identity;
    id.requested_color_count=8; id.mode=AI::PrintColorMode::Layered;
    const auto two=id.targets; id.targets.clear();
    for (size_t part=0; part<4; ++part) {
        for (auto target:two) { target.area/=4; id.targets.push_back(target); }
        for (size_t f=0; f<4; ++f) id.face_targets[part*4+f]=part*2+labels[part*4+f];
    }
    e.baseline.result=id;
    const auto result=Boundary::compute_refined(e.input,e.mesh,e.baseline);
    REQUIRE(result.ok());
    CHECK(result.result.targets.size()==8);
    CHECK(result.result.requested_color_count==8);
    CHECK(result.result.physical_channels.size()==2);
    for (size_t f=0; f<desired.size(); ++f) {
        CHECK(result.result.targets[result.result.face_targets[f]].physical_slot==std::optional<size_t>(desired[f]));
        CHECK_THAT(Matching::delta_e(e.input.faces[f].color,result.result.targets[result.result.face_targets[f]].output),
            Catch::Matchers::WithinAbs(0,1e-10));
    }
    std::string error; CHECK(result.result.valid(error));
}

TEST_CASE("layered boundary moves preserve reliable regional dominant outputs", "[LocalPrintColorBoundary]")
{
    auto e=movable();
    auto& id=e.input.identity;
    id.requested_color_count=8; id.mode=AI::PrintColorMode::Layered;
    id.regions={{"auto-semantic:p:llip","p","llip",.9,false,{0}}};
    e.baseline.result=id;
    const auto result=Boundary::compute_refined(e.input,e.mesh,e.baseline);
    REQUIRE(result.ok());
    CHECK(result.result.face_targets==e.baseline.result.face_targets);
    CHECK(notice_contains(result,"dominant physical output"));
}

TEST_CASE("layered refinement propagates across neighbors exposed by earlier rounds", "[LocalPrintColorBoundary]")
{
    auto e=example(octahedron(),{0,0,0,1,1,1,0,1},{1,1,0,1,1,1,0,1});
    const auto direct=Boundary::compute_refined(e.input,e.mesh,e.baseline);
    REQUIRE(direct.ok());
    CHECK(direct.result.face_targets==std::vector<size_t>{1,0,0,1,1,1,0,1});
    e.input.identity.mode=e.baseline.result.mode=AI::PrintColorMode::Layered;
    e.input.identity.requested_color_count=e.baseline.result.requested_color_count=8;
    const auto result=Boundary::compute_refined(e.input,e.mesh,e.baseline);
    INFO(result.error);
    REQUIRE(result.ok());
    CHECK(result.result.face_targets==std::vector<size_t>{1,1,0,1,1,1,0,1});
    CHECK(notice_contains(result,"2 bounded display-color boundary rounds"));
    CHECK(notice_contains(result,"no eligible move"));
    CHECK(result.result.targets.size()==2);
    for (const auto& t:result.result.targets) {
        CHECK(t.area>0);
        CHECK_THAT(t.delta_e00,Catch::Matchers::WithinAbs(0,1e-10));
    }
    const auto repeated=Boundary::compute_refined(e.input,e.mesh,result);
    REQUIRE(repeated.ok());
    CHECK(repeated.result.face_targets==result.result.face_targets);
}

TEST_CASE("a rejected later boundary round retains earlier valid progress and all groups", "[LocalPrintColorBoundary]")
{
    auto e=example(octahedron(),{0,0,0,1,1,1,0,1},{1,1,1,1,1,1,1,1});
    e.input.identity.mode=e.baseline.result.mode=AI::PrintColorMode::Layered;
    e.input.identity.requested_color_count=e.baseline.result.requested_color_count=8;
    const auto result=Boundary::compute_refined(e.input,e.mesh,e.baseline);
    INFO(result.error);
    REQUIRE(result.ok());
    // The first round removes two leaves of the black patch. The next round
    // would erase its last two faces, so only that later proposal is rejected.
    CHECK(result.result.face_targets==std::vector<size_t>{1,0,0,1,1,1,1,1});
    CHECK(notice_contains(result,"empty an existing target"));
    CHECK(result.result.targets.size()==2);
    CHECK(result.result.targets[0].area>0);
    CHECK(result.result.targets[1].area>0);
    for (size_t f=0;f<e.input.faces.size();++f)
        CHECK(Matching::delta_e(e.input.faces[f].color,result.result.targets[result.result.face_targets[f]].output)<=
            Matching::delta_e(e.input.faces[f].color,e.baseline.result.targets[e.baseline.result.face_targets[f]].output));
}

TEST_CASE("recipes missing physical assignments and confirmed results are not revised", "[LocalPrintColorBoundary]")
{
    auto e=movable(); const int unsupported=GENERATE(0,1,2);
    if(unsupported==0) {
        e.input.identity.requested_color_count=e.baseline.result.requested_color_count=7;
        e.input.identity.mode=e.baseline.result.mode=AI::PrintColorMode::Layered;
        auto& target=e.baseline.result.targets[0];
        target.physical_slot.reset();
        AI::MixedColorRecipe recipe; recipe.components={{0,.5},{1,.5}};
        recipe.target_color="#000000";
        target.recipe=recipe; target.candidate_id="synthetic-recipe";
    }
    if(unsupported==1) {
        auto& target=e.baseline.result.targets[0];
        target.physical_slot.reset(); target.executable=false; target.within_tolerance=false;
        target.unresolved_reason="No physical assignment.";
    }
    if(unsupported==2) e.baseline.result.confirmed=true;
    const auto result=Boundary::compute_refined(e.input,e.mesh,e.baseline);
    REQUIRE(result.ok());
    CHECK(result.result.face_targets==e.baseline.result.face_targets);
    CHECK(notice_contains(result,"kept the baseline"));
}

TEST_CASE("whole wrong-color islands can improve where individual boundary moves cannot", "[LocalPrintColorBoundary]")
{
    auto mesh=octahedron(); const auto extra=tetrahedron();
    const int offset=int(mesh.vertices.size());
    for(const auto& p:extra.vertices) mesh.vertices.push_back(p+Vec3f(5,0,0));
    for(const auto& f:extra.indices) mesh.indices.emplace_back(f[0]+offset,f[1]+offset,f[2]+offset);
    auto e=example(mesh,{0,0,0,0,1,1,1,1,0,0,0,0},{1,1,1,1,1,1,1,1,0,0,0,0});
    const auto ordinary=Boundary::compute_refined(e.input,e.mesh,e.baseline);
    REQUIRE(ordinary.ok());
    CHECK(ordinary.result.face_targets==e.baseline.result.face_targets);
    e.input.identity.mode=e.baseline.result.mode=AI::PrintColorMode::Layered;
    e.input.identity.requested_color_count=e.baseline.result.requested_color_count=8;
    const bool protected_face=GENERATE(false,true);
    if(protected_face) {
        e.input.identity.regions={{"manual","person","selection",1,true,{0}}};
        e.baseline.result.regions=e.input.identity.regions;
    }
    const auto result=Boundary::compute_refined(e.input,e.mesh,e.baseline);
    INFO(result.error);REQUIRE(result.ok());
    if(protected_face) CHECK(result.result.face_targets==e.baseline.result.face_targets);
    else {
        CHECK(result.result.face_targets==std::vector<size_t>{1,1,1,1,1,1,1,1,0,0,0,0});
        CHECK(notice_contains(result,"1 whole physical islands"));
        CHECK(result.result.targets.size()==2);
    }
}

TEST_CASE("whole-model simplification cannot authorize a new color island inside an eye", "[LocalPrintColorBoundary]")
{
    auto e=example(tetrahedron(),{0,0,0,1},{1,0,0,1});
    e.input.identity.mode=e.baseline.result.mode=AI::PrintColorMode::Layered;
    e.input.identity.requested_color_count=e.baseline.result.requested_color_count=8;
    AI::PrintColorRegion eye{"auto-semantic:eye","person","le",.99,false,{0,1}};
    eye.protect_color=true;
    e.input.identity.regions=e.baseline.result.regions={eye};
    const auto result=Boundary::compute_refined(e.input,e.mesh,e.baseline);
    INFO(result.error);REQUIRE(result.ok());
    CHECK(result.result.face_targets==e.baseline.result.face_targets);
}

TEST_CASE("coherent critical detail corrections preserve regional topology within the total boundary budget", "[LocalPrintColorBoundary]")
{
    auto mesh=octahedron();const auto extra=tetrahedron();
    for(auto& vertex:mesh.vertices) vertex*=2.f; // Keep the white majority after excluding the adjacent tetrahedron face.
    for(int part=1;part<=2;++part) {
        const int offset=int(mesh.vertices.size());
        for(const auto& p:extra.vertices) mesh.vertices.push_back(p+Vec3f(float(part*5),0,0));
        for(const auto& f:extra.indices) mesh.indices.emplace_back(f[0]+offset,f[1]+offset,f[2]+offset);
    }
    auto e=example(mesh,{0,0,0,0,1,1,1,1,0,0,0,0,0,0,0,1},{1,1,1,1,1,1,1,1,0,0,0,0,1,0,0,1});
    e.input.identity.mode=e.baseline.result.mode=AI::PrintColorMode::Layered;
    e.input.identity.requested_color_count=e.baseline.result.requested_color_count=8;
    AI::PrintColorRegion lip{"auto-semantic:lip","person","ulip",.99,false,{4,5,6,7,12,15}};
    const int region_edge=GENERATE(0,1,2);
    if(region_edge==1) lip.faces={4,5,6,7,12}; // Whole wrong-color block; correct neighbor is outside the mask.
    if(region_edge==2) lip.faces={0,4,5,6,7,12,13}; // Earlier island removal must not pay for splitting 12/13.
    INFO(region_edge);
    std::array<double,2> regional_area{};
    for(size_t f:lip.faces) regional_area[e.baseline.result.face_targets[f]]+=e.input.faces[f].area;
    REQUIRE(regional_area[1]>regional_area[0]);
    lip.protect_color=true;e.input.identity.regions=e.baseline.result.regions={lip};
    const auto result=Boundary::compute_refined(e.input,e.mesh,e.baseline);
    INFO(result.error);REQUIRE(result.ok());
    auto expected=std::vector<size_t>{1,1,1,1,1,1,1,1,0,0,0,0,1,0,0,1};
    if(region_edge==2) expected[12]=0;
    CHECK(result.result.face_targets==expected);
    if(region_edge!=2) CHECK(notice_contains(result,"1 protected-region patches"));
    CHECK(result.result.regions[0].protect_color);
    CHECK(result.result.targets.size()==2);
    for(size_t f=0;f<e.input.faces.size();++f) {
        if(region_edge==2 && f==12) continue;
        CHECK_THAT(Matching::delta_e(e.input.faces[f].color,result.result.targets[result.result.face_targets[f]].output),Catch::Matchers::WithinAbs(0,1e-10));
    }
    // A cancellation signal may be observed only once. In particular, regional
    // or global rejection must not consume it and publish earlier patch moves.
    int checkpoints=0;
    e.input.cancelled=[&] {++checkpoints;return false;};
    REQUIRE(Boundary::compute_refined(e.input,e.mesh,e.baseline).ok());
    for(int phase=1;phase<=checkpoints;++phase) {
        int polls=0;
        e.input.cancelled=[&] {return ++polls==phase;};
        const auto stopped=Boundary::compute_refined(e.input,e.mesh,e.baseline);
        INFO(phase);
        CHECK(stopped.cancelled);
        CHECK_FALSE(stopped.ok());
        CHECK(stopped.result.face_targets==e.baseline.result.face_targets);
    }
}

TEST_CASE("invalid and cancelled baselines cannot become successful refinements", "[LocalPrintColorBoundary]")
{
    auto e=movable(); const int failure=GENERATE(0,1,2);
    if(failure==0) e.baseline.result.face_targets[0]=99;
    if(failure==1) e.baseline.error="Prior matching failed.";
    if(failure==2) e.baseline.cancelled=true;
    const auto result=Boundary::compute_refined(e.input,e.mesh,e.baseline);
    CHECK_FALSE(result.ok());
    CHECK(result.result.face_targets==e.baseline.result.face_targets);
    if(failure==2) CHECK(result.cancelled);
    else CHECK_FALSE(result.error.empty());
}

TEST_CASE("a lower overall mean cannot compensate for damage to a protected lip", "[LocalPrintColorBoundary]")
{
    auto e=example(tetrahedron(),{0,0,0,1},{1,0,0,1});
    e.input.faces[0].area=10;
    AI::PrintColorRegion lip{"auto-semantic:lip","subject","llip",.99,false,{2}};
    lip.protect_color=true;
    auto baseline=e.baseline.result;
    baseline.regions={lip};
    auto candidate=baseline;
    candidate.face_targets={1,0,1,1};
    const auto a=GUI::LocalPrintColorQuality::evaluate(e.input.faces,candidate);
    const auto b=GUI::LocalPrintColorQuality::evaluate(e.input.faces,baseline);
    REQUIRE(a.error.empty()); REQUIRE(b.error.empty());
    CHECK(a.mean_delta_e<b.mean_delta_e);
    CHECK(a.regions[0].mean_delta_e>b.regions[0].mean_delta_e);
    CHECK_FALSE(Boundary::preserves_quality(a,b,{lip}));
    CHECK(Boundary::preserves_quality(a,b,{})); // Unprotected overall improvement is real.
}

TEST_CASE("a better mean and maximum cannot hide a worse surface error percentile", "[LocalPrintColorBoundary]")
{
    auto e=example(octahedron(),{0,1,4,4,4,1,2,3},{0,0,0,0,4,1,2,3},
        {"#000000","#1B1B1B","#333333","#808080","#FFFFFF"});
    const std::vector<double> areas={94,3,2,1,.001,.001,.001,.001};
    for(size_t f=0;f<areas.size();++f) e.input.faces[f].area=areas[f];
    auto candidate=e.baseline.result;
    candidate.face_targets[1]=2;
    candidate.face_targets[2]=candidate.face_targets[3]=3;
    const auto a=GUI::LocalPrintColorQuality::evaluate(e.input.faces,candidate);
    const auto b=GUI::LocalPrintColorQuality::evaluate(e.input.faces,e.baseline.result);
    REQUIRE(a.error.empty()); REQUIRE(b.error.empty());
    CHECK(a.mean_delta_e<b.mean_delta_e);
    CHECK(a.worst_delta_e<b.worst_delta_e);
    CHECK(a.p95_delta_e>b.p95_delta_e);
    CHECK_FALSE(Boundary::preserves_quality(a,b,{}));
}

TEST_CASE("the final quality gate keeps evidence and explicit locks on tied output", "[LocalPrintColorBoundary]")
{
    auto e=example(tetrahedron(),{0,0,1,1},{0,0,1,1});
    AI::PrintColorRegion lip{"auto-semantic:lip","subject","llip",.99,false,{2,3}};
    lip.protect_color=true;
    AI::PrintColorRegion manual{"manual","subject","selection",1,true,{0}};
    manual.locked_physical_slot=0;
    e.input.identity.regions={lip,manual};
    e.input.identity.user_overrides={{0,{0,0,0}}};
    const auto result=Boundary::compute_guarded(e.input,e.mesh);
    REQUIRE(result.ok());
    CHECK(notice_contains(result,"Final semantic quality gate retained"));
    REQUIRE(result.result.regions.size()==2);
    CHECK(result.result.regions[0].protect_color);
    CHECK(result.result.regions[1].locked_physical_slot==0);
    CHECK(result.result.user_overrides==e.input.identity.user_overrides);
    const auto& target=result.result.targets[result.result.face_targets[0]];
    CHECK(target.physical_slot==0);
    CHECK_FALSE(result.result.confirmed);
    CHECK(result.result.algorithm_version=="region-direct-v5-boundary-v1-quality-v1-locks-v1");
    e.input.cancelled=[] {return true;};
    const auto stopped=Boundary::compute_guarded(e.input,e.mesh);
    CHECK(stopped.cancelled); CHECK_FALSE(stopped.ok());
}

TEST_CASE("a small jacket material lock can share black without displacing unrelated hair", "[LocalPrintColorBoundary]")
{
    auto e=example(octahedron(),{0,0,1,1,2,2,3,3},{0,0,1,1,2,2,3,3},
        {"#000000","#FFFFFF","#E53935","#26A69A"});
    for(size_t f : {size_t(0),size_t(1)}) e.input.faces[f].color=Matching::hex_rgb("#71594C");
    for(size_t f : {size_t(2),size_t(3)}) e.input.faces[f].color=Matching::hex_rgb("#DDA17C");
    e.input.faces[4].area*=.01;
    const auto unlocked=Boundary::compute_guarded(e.input,e.mesh);
    REQUIRE(unlocked.ok());
    AI::PrintColorRegion manual{"jacket-patch","subject","selection",1,true,{4}};
    manual.locked_physical_slot=0;
    e.input.identity.regions={manual};
    const auto result=Boundary::compute_guarded(e.input,e.mesh);
    REQUIRE(result.ok());
    const auto& r=result.result;
    CHECK(r.targets[r.face_targets[4]].physical_slot==0);
    for(size_t f=0;f<e.input.faces.size();++f) {
        if(f==4) continue;
        CHECK(r.targets[r.face_targets[f]].physical_slot==unlocked.result.targets[unlocked.result.face_targets[f]].physical_slot);
    }
    std::set<size_t> slots;
    for(const auto& t:r.targets) { REQUIRE(t.physical_slot); CHECK(slots.insert(*t.physical_slot).second); }
    CHECK(r.targets.size()<=e.input.identity.requested_color_count);
    CHECK(r.regions[0].locked_physical_slot==0);
    std::string error; CHECK(r.valid(error));
}

TEST_CASE("local lock reuse never bypasses unavailable or conflicting material constraints", "[LocalPrintColorBoundary]")
{
    auto e=example(octahedron(),{0,0,1,1,2,2,3,3},{0,0,1,1,2,2,3,3},
        {"#000000","#FFFFFF","#E53935","#26A69A"});
    AI::PrintColorRegion manual{"patch","subject","selection",1,true,{4}};
    manual.locked_physical_slot=0;
    e.input.identity.regions={manual};
    const bool conflict=GENERATE(false,true);
    if(conflict) {
        manual.id="conflict"; manual.locked_physical_slot=1;
        e.input.identity.regions.push_back(manual);
    } else e.input.identity.physical_channels[0].compatible=false;
    const auto result=Boundary::compute_guarded(e.input,e.mesh);
    CHECK_FALSE(result.ok()); CHECK_FALSE(result.error.empty());
}

TEST_CASE("sharing a material group cannot erase a required region contrast", "[LocalPrintColorBoundary]")
{
    auto e=example(octahedron(),{0,0,1,1,2,2,3,3},{0,0,1,1,2,2,3,3},
        {"#000000","#FFFFFF","#E53935","#26A69A"});
    for(size_t f : {size_t(0),size_t(1)}) e.input.faces[f].color=Matching::hex_rgb("#71594C");
    for(size_t f : {size_t(2),size_t(3)}) e.input.faces[f].color=Matching::hex_rgb("#DDA17C");
    e.input.faces[4].area*=.01;
    AI::PrintColorRegion manual{"patch","subject","selection",1,true,{4}};
    manual.locked_physical_slot=0;
    e.input.identity.regions={manual,{"hair","subject","hair",1,false,{0,1}}};
    e.input.identity.contrasts={{"patch","hair",1,10,true}};
    const auto result=Boundary::compute_guarded(e.input,e.mesh);
    REQUIRE(result.ok());
    const auto& r=result.result;
    REQUIRE(std::all_of(r.targets.begin(),r.targets.end(),[](const auto& t) {return t.executable;}));
    CHECK(r.targets[r.face_targets[4]].physical_slot==0);
    CHECK(Matching::delta_e(r.targets[r.face_targets[4]].output,r.targets[r.face_targets[0]].output)>=10);
}
