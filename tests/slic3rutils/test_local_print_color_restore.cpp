#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "slic3r/GUI/AI/Model/LocalPrintColorState.hpp"

using namespace Slic3r;
namespace State = GUI::LocalPrintColorState;

static AI::LocalPrintColorResult saved_confirmation()
{
    AI::LocalPrintColorResult value;
    value.algorithm_version="region-direct-v5-boundary-v1";
    value.source_sha256=std::string(64,'a'); value.geometry_id="verified-native-faces";
    value.parent_version="previous-confirmed-version";
    value.material_fingerprint="material-snapshot"; value.process_fingerprint="process-snapshot";
    value.face_count=4; value.requested_color_count=2;
    value.physical_channels={{0,"#000000","PLA",true},{3,"#FFFFFF","PLA",true}};
    for (size_t t=0;t<2;++t) {
        AI::PrintColorTarget target;
        const float channel=t==0?0.f:1.f;
        target.source=target.output={channel,channel,channel}; target.area=2;
        target.physical_slot=t==0?0:3; target.executable=target.within_tolerance=true;
        target.evidence=AI::ColorEvidence::Estimated; value.targets.push_back(target);
    }
    value.face_targets={1,0,1,0}; value.confirmed=true;
    value.regions={{"eye","person","eye",1,true,{0}},{"face","person","face",.8,false,{1,3}}};
    value.regions[0].locked_physical_slot=3;
    value.contrasts={{"eye","face",1,5,true}};
    value.user_overrides={{2,{1,1,1}}};
    return value;
}

TEST_CASE("unchanged confirmed history restores its exact partition without migrating algorithms", "[LocalPrintColorRestore]")
{
    auto saved=saved_confirmation();
    saved.algorithm_version=GENERATE(std::string("region-direct-v1"),std::string("region-direct-v5"),std::string("region-direct-v5-boundary-v1"));
    const auto serialized=State::encode(saved);
    AI::LocalPrintColorResult decoded; std::string reason;
    REQUIRE(State::decode(serialized,saved.source_sha256,saved.geometry_id,saved.material_fingerprint,
        saved.process_fingerprint,decoded,reason,saved.algorithm_version));
    auto current=decoded;
    current.algorithm_version="new-computation-version";
    current.parent_version="the-version-being-opened";
    // Only the input intent belongs to this draft; its target vectors are not
    // a recomputation and must not be used instead of the historical partition.
    current.targets.clear(); current.face_targets.clear(); current.confirmed=false;
    AI::LocalPrintColorResult displayed;
    REQUIRE(State::restore_confirmed(decoded,current,true,displayed,reason));
    CHECK(State::encode(displayed)==serialized);
    CHECK(displayed.face_targets==std::vector<size_t>{1,0,1,0});
    CHECK(displayed.algorithm_version==saved.algorithm_version);
    CHECK(displayed.parent_version==saved.parent_version);
    CHECK(current.parent_version=="the-version-being-opened");
    CHECK(reason.empty());
}

TEST_CASE("changed source intent or material settings never replace a confirmed version", "[LocalPrintColorRestore]")
{
    const auto saved=saved_confirmation(); auto current=saved;
    const auto change=GENERATE(0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19);
    if(change==0) current.source_sha256=std::string(64,'b');
    if(change==1) current.geometry_id="other-faces";
    if(change==2) current.face_count=5;
    if(change==3) current.material_fingerprint="new-materials";
    if(change==4) current.process_fingerprint="new-process";
    if(change==5) current.requested_color_count=3;
    if(change==6) current.mode=AI::PrintColorMode::Layered;
    if(change==7) current.color_tolerance=20;
    if(change==8) current.important_area_floor=.3;
    if(change==9) current.physical_channels[1].slot=4;
    if(change==10) current.physical_channels[1].display_color="#EEEEEE";
    if(change==11) current.physical_channels[1].material_type="PETG";
    if(change==12) current.physical_channels[1].compatible=false;
    if(change==13) current.user_overrides.clear();
    if(change==14) current.regions[0].faces={0,2};
    if(change==15) current.regions[0].user_protected=false;
    if(change==16) current.regions[1].protect_color=true;
    if(change==17) current.regions[0].locked_physical_slot=0;
    if(change==18) current.contrasts[0].minimum_output_delta_e=10;
    if(change==19) current.contrasts[0].hard=false;
    auto displayed=saved; displayed.parent_version="already-visible-confirmation";
    const auto before=State::encode(displayed); const auto saved_before=State::encode(saved);
    std::string reason;
    CHECK_FALSE(State::restore_confirmed(saved,current,true,displayed,reason));
    CHECK_FALSE(reason.empty());
    CHECK(State::encode(displayed)==before);
    CHECK(State::encode(saved)==saved_before);
}

TEST_CASE("unconfirmed malformed or manually repainted history is not directly restored", "[LocalPrintColorRestore]")
{
    auto saved=saved_confirmation(); const auto current=saved;
    const auto change=GENERATE(0,1,2); bool paint_matches=true;
    if(change==0) saved.confirmed=false;
    if(change==1) saved.face_targets[0]=99;
    if(change==2) paint_matches=false;
    auto displayed=current; const auto before=State::encode(displayed); std::string reason;
    CHECK_FALSE(State::restore_confirmed(saved,current,paint_matches,displayed,reason));
    CHECK(State::encode(displayed)==before);
}

TEST_CASE("previously accepted predicted error remains part of the restored confirmation", "[LocalPrintColorRestore]")
{
    auto saved=saved_confirmation();
    saved.targets[0].delta_e00=20; saved.targets[0].within_tolerance=false;
    saved.targets[0].unresolved_reason="Previously accepted uncalibrated display error.";
    AI::LocalPrintColorResult displayed; std::string reason;
    REQUIRE(State::restore_confirmed(saved,saved,true,displayed,reason));
    CHECK(State::encode(displayed)==State::encode(saved));
    CHECK(displayed.confirmed);
    CHECK(displayed.unresolved_count()==1);
}
