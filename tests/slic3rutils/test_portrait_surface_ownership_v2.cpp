#include <catch2/catch_test_macros.hpp>
#include "slic3r/GUI/AI/ModelGeneration/PortraitSurfaceOwnershipV2.hpp"

using Json=nlohmann::json;
namespace SP=Slic3r::AI::SurfacePartition;
using Ownership=Slic3r::GUI::PortraitSurfaceCellOwnership;

namespace {
struct Fixture {
    Json partition,locks,ownership;
    Fixture() {
        Json identity;
        for(const auto* key:{"source_sha256","geometry_id","evidence_sha256","runtime_sha256",
                            "policy_sha256","baseline_sha256","boundary_policy_sha256"}) identity[key]=std::string(64,'a');
        identity["face_count"]=1000;
        Json cell={{"polygon",{{1,0,0},{0,1,0},{0,0,1}}},{"holes",Json::array()},
            {"label","le"},{"parent_label","le"},{"subject_id","person"}};
        Json request={{"schema","orca.surface-partition-request/v1"},{"identity",identity},
            {"triangle_budget",20},{"existing_added_triangles",0},
            {"faces",Json::array({{{"source_face_id",0},{"base",Json::array({cell})},
                                  {"layers",Json::array()},{"baseline_triangle_count",1}}})}};
        partition=SP::build(request);
        locks={{"partition_ref",{{"sha256",std::string(64,'b')}}},{"locks",Json::array({{
            {"subject_id","person"},{"locked_cells",{partition["faces"][0]["cells"][0]["id"]}},
            {"periocular_cells",Json::array()}}})}};
        ownership={{"schema","orca.portrait-surface-ownership/v2"},
            {"partition_sha256",partition.at("partition_sha256")},{"partition_ref",locks.at("partition_ref")},
            {"detail_freeze_sha256",std::string(64,'c')},{"policy",Json::object()},
            {"policy_sha256",Slic3r::AI::beauty_leaf_digest("{}")},
            {"regions",Json::array({{{"id","body"},{"subject_id","person"},{"parent_label","skin"},
                {"status","CONFIRMED_PARENT"},{"view_ids",{"front","left"}},
                {"units",Json::array({{{"id",Ownership::root_id(partition,1)},{"source_face_id",1},
                                     {"implicit_root",true},{"view_ids",{"front","left"}}}})}}})}};
        for(const auto* key:{"source_sha256","geometry_id","evidence_sha256","face_count"}) ownership[key]=partition.at(key);
    }
};
}

TEST_CASE("Cell ownership represents body skin without changing a reviewed eye", "[PortraitSurfaceOwnershipV2]") {
    Fixture f;
    REQUIRE_NOTHROW(Ownership::decode(f.ownership,f.partition,f.locks,std::string(64,'c')));
    for(const auto* key:{"source_sha256","geometry_id","evidence_sha256","partition_sha256","detail_freeze_sha256"}) {
        auto bad=f.ownership;bad[key]=std::string(64,'d');
        REQUIRE_THROWS(Ownership::decode(bad,f.partition,f.locks,std::string(64,'c')));
    }
}

TEST_CASE("Parent cell ownership rejects frozen details mixed root aliases and correlated votes", "[PortraitSurfaceOwnershipV2]") {
    Fixture f;
    for(const auto mutation:{"feature","duplicate","person","camera","mixed_root","negative"}) {
        auto bad=f.ownership;auto& region=bad["regions"][0];auto& unit=region["units"][0];
        if(std::string(mutation)=="feature") {unit["id"]=f.partition["faces"][0]["cells"][0]["id"];unit["source_face_id"]=0;}
        if(std::string(mutation)=="duplicate") region["units"].push_back(unit);
        if(std::string(mutation)=="person") region["subject_id"]="other";
        if(std::string(mutation)=="camera") unit["view_ids"]={"front","front"};
        if(std::string(mutation)=="mixed_root") {unit["source_face_id"]=0;unit["id"]=Ownership::root_id(f.partition,0);}
        if(std::string(mutation)=="negative") unit["source_face_id"]=-1;
        REQUIRE_THROWS(Ownership::decode(bad,f.partition,f.locks,std::string(64,'c')));
    }
}

TEST_CASE("Cell ownership references stay inside the content addressed parent cache", "[PortraitSurfaceOwnershipV2]") {
    const auto hash=std::string(64,'a');
    Json ref={{"schema","orca.portrait-surface-ownership-reference/v2"},{"sha256",hash},
        {"path","portrait-ownership/"+hash+".json"}};
    REQUIRE(Ownership::safe_reference(ref));
    for(const auto path:{"../external.json","C:/external.json","/external.json"}) {
        ref["path"]=path;REQUIRE_FALSE(Ownership::safe_reference(ref));
    }
}
