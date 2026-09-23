#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "slic3r/GUI/AI/Model/LocalSemanticDraft.hpp"

using namespace Slic3r;
namespace Draft = GUI::LocalSemanticDraft;
namespace Semantic = GUI::LocalSemanticEvidence;
using Catch::Matchers::WithinAbs;

namespace {
AI::PrintColorRegion automatic(const std::string& label,std::vector<size_t> faces)
{
    AI::PrintColorRegion region;
    region.subject_id="subject-a";region.label=label;region.id="auto-semantic:"+region.subject_id+":"+label;
    region.confidence=.97;region.protect_color=label=="re";region.faces=std::move(faces);
    return region;
}
AI::PrintColorRegion manual(const std::string& name,std::vector<size_t> faces)
{
    AI::PrintColorRegion region;region.id=name;region.label="user region";
    region.confidence=1;region.user_protected=true;region.faces=std::move(faces);return region;
}
struct Fixture {
    AI::LocalPrintColorResult draft;
    Semantic::Evidence evidence;
    Fixture()
    {
        draft.source_sha256=std::string(64,'a');draft.geometry_id=std::string(64,'b');draft.face_count=8;
        draft.confirmed=true;draft.parent_version="confirmed-parent";
        evidence.identity.source_sha256=draft.source_sha256;evidence.identity.geometry_id=draft.geometry_id;
        evidence.identity.face_count=8;evidence.subjects={"subject-a"};
        evidence.regions={automatic("re",{0,1,2,3,4,5,6})};index();
    }
    void index()
    {
        evidence.face_regions.assign(8,-1);evidence.known_faces=0;
        for(size_t r=0;r<evidence.regions.size();++r)
            for(size_t face:evidence.regions[r].faces) {evidence.face_regions.at(face)=int32_t(r);++evidence.known_faces;}
        evidence.unknown_faces=8-evidence.known_faces;
    }
};
void unchanged(const AI::PrintColorRegion& actual,const AI::PrintColorRegion& expected)
{
    CHECK(actual.id==expected.id);CHECK(actual.subject_id==expected.subject_id);CHECK(actual.label==expected.label);
    CHECK_THAT(actual.confidence,WithinAbs(expected.confidence,1e-12));
    CHECK(actual.faces==expected.faces);CHECK(actual.user_protected==expected.user_protected);
    CHECK(actual.protect_color==expected.protect_color);CHECK(actual.locked_physical_slot==expected.locked_physical_slot);
}
void fails_unchanged(const Fixture& f)
{
    const auto prior=manual("existing destination",{7});std::vector<AI::PrintColorRegion> output{prior};std::string error;
    CHECK_FALSE(Draft::merge_regions(f.draft,f.evidence,output,error));CHECK_FALSE(error.empty());
    REQUIRE(output.size()==1);unchanged(output.front(),prior);
}
}

TEST_CASE("semantic merging replaces old automatic regions without labeling unknown faces", "[LocalSemanticDraft]")
{
    Fixture f;f.draft.regions={automatic("hair",{0,7})};
    std::vector<AI::PrintColorRegion> output;std::string error;
    REQUIRE(Draft::merge_regions(f.draft,f.evidence,output,error));
    REQUIRE(output.size()==1);CHECK(output.front().id==f.evidence.regions.front().id);
    CHECK(output.front().faces==std::vector<size_t>{0,1,2,3,4,5,6});
    CHECK_FALSE(output.front().user_protected);CHECK_FALSE(output.front().locked_physical_slot.has_value());
    REQUIRE(f.draft.regions.size()==1);CHECK(f.draft.regions.front().label=="hair");
    CHECK(f.draft.confirmed);CHECK(f.draft.parent_version=="confirmed-parent");
}

TEST_CASE("semantic merging preserves manual constraints and either contrast endpoint exactly", "[LocalSemanticDraft]")
{
    Fixture f;
    auto locked=manual("slot-lock",{1});locked.locked_physical_slot=2;
    auto protected_region=manual("protected",{2});
    auto referenced_auto=automatic("hair",{4});
    auto other_endpoint=manual("contrast-other",{5});other_endpoint.user_protected=false;
    f.draft.regions={locked,protected_region,referenced_auto,other_endpoint};
    f.draft.user_overrides={{3,{.1f,.2f,.3f}}};
    AI::PrintColorContrast contrast;contrast.first_region=referenced_auto.id;contrast.second_region=other_endpoint.id;
    contrast.weight=2;contrast.minimum_output_delta_e=7;contrast.hard=GENERATE(false,true);f.draft.contrasts={contrast};
    const auto saved_regions=f.draft.regions;
    std::vector<AI::PrintColorRegion> output;std::string error;
    REQUIRE(Draft::merge_regions(f.draft,f.evidence,output,error));
    REQUIRE(output.size()==5);
    for(size_t i=0;i<saved_regions.size();++i) unchanged(output[i],saved_regions[i]);
    CHECK(output.back().faces==std::vector<size_t>{0,6});
    REQUIRE(f.draft.user_overrides.size()==1);CHECK(f.draft.user_overrides.front().first==3);
    for(size_t c=0;c<3;++c) CHECK_THAT(f.draft.user_overrides.front().second[c],WithinAbs((c+1)*.1,1e-7));
    REQUIRE(f.draft.contrasts.size()==1);CHECK(f.draft.contrasts.front().first_region==contrast.first_region);
    CHECK(f.draft.contrasts.front().second_region==contrast.second_region);CHECK(f.draft.contrasts.front().hard==contrast.hard);
    CHECK_THAT(f.draft.contrasts.front().weight,WithinAbs(2.,1e-12));
    CHECK_THAT(f.draft.contrasts.front().minimum_output_delta_e,WithinAbs(7.,1e-12));
}

TEST_CASE("automatic-looking IDs cannot override retained user authority", "[LocalSemanticDraft]")
{
    Fixture f;auto retained=f.evidence.regions.front();retained.faces={7};retained.user_protected=GENERATE(true,false);
    if(!retained.user_protected) retained.locked_physical_slot=2;
    f.draft.regions={retained};std::vector<AI::PrintColorRegion> output;std::string error;
    REQUIRE(Draft::merge_regions(f.draft,f.evidence,output,error));
    REQUIRE(output.size()==1);unchanged(output.front(),retained);
    // Even disjoint incoming faces cannot reuse the retained region's ID.
    CHECK(output.front().faces==std::vector<size_t>{7});
}

TEST_CASE("semantic merging drops regions left empty after explicit face exclusions", "[LocalSemanticDraft]")
{
    Fixture f;f.draft.regions={manual("whole observed selection",{0,1,2,3,4,5,6})};
    std::vector<AI::PrintColorRegion> output;std::string error;
    REQUIRE(Draft::merge_regions(f.draft,f.evidence,output,error));
    REQUIRE(output.size()==1);unchanged(output.front(),f.draft.regions.front());
}

TEST_CASE("no reliable semantic observations remove replaceable hints without inventing regions", "[LocalSemanticDraft]")
{
    Fixture f;f.draft.regions={automatic("hair",{0,7}),manual("keep",{1})};
    f.evidence.subjects.clear();f.evidence.regions.clear();f.index();
    std::vector<AI::PrintColorRegion> output;std::string error;
    REQUIRE(Draft::merge_regions(f.draft,f.evidence,output,error));
    REQUIRE(output.size()==1);CHECK(output.front().id=="keep");
}

TEST_CASE("stale semantic surfaces fail without replacing the destination", "[LocalSemanticDraft]")
{
    Fixture f;const int mismatch=GENERATE(0,1,2,3);
    if(mismatch==0) f.evidence.identity.source_sha256=std::string(64,'c');
    if(mismatch==1) f.evidence.identity.geometry_id=std::string(64,'c');
    if(mismatch==2) f.evidence.identity.face_count=7;
    if(mismatch==3) {f.draft.source_sha256="invalid";f.evidence.identity.source_sha256="invalid";}
    fails_unchanged(f);
}

TEST_CASE("malformed automatic regions cannot inject authority or ambiguous assignments", "[LocalSemanticDraft]")
{
    Fixture f;const int invalid=GENERATE(0,1,2,3,4,5,6,7,8,9,10,11);
    auto& region=f.evidence.regions.front();
    if(invalid==0) region.user_protected=true;
    if(invalid==1) region.locked_physical_slot=2;
    if(invalid==2) region.faces.push_back(8);
    if(invalid==3) region.faces.push_back(0);
    if(invalid==4) region.confidence=std::numeric_limits<double>::quiet_NaN();
    if(invalid==5) region.label="teeth";
    if(invalid==6) region.id="manual-injected";
    if(invalid==7) region.subject_id="unknown-subject";
    if(invalid==8) f.evidence.face_regions[0]=-1;
    if(invalid==9) f.evidence.known_faces=0;
    if(invalid==10) f.evidence.face_regions[7]=0;
    if(invalid==11) f.evidence.regions.push_back(region);
    fails_unchanged(f);
}

TEST_CASE("invalid draft references fail instead of silently dropping user constraints", "[LocalSemanticDraft]")
{
    Fixture f;f.draft.regions={manual("keep",{0})};const int invalid=GENERATE(0,1,2,3);
    if(invalid==0) f.draft.regions.front().faces={0,0};
    if(invalid==1) f.draft.regions.push_back(f.draft.regions.front());
    if(invalid==2) {AI::PrintColorContrast c;c.first_region="keep";c.second_region="missing";f.draft.contrasts={c};}
    if(invalid==3) f.draft.user_overrides={{8,{1,0,0}}};
    fails_unchanged(f);
}
