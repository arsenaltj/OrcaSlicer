#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "slic3r/GUI/AI/Model/LocalSemanticEvidence.hpp"

using namespace Slic3r;
namespace Semantic = GUI::LocalSemanticEvidence;
using Json = nlohmann::json;

namespace {
indexed_triangle_set mesh()
{
    indexed_triangle_set m;
    m.vertices={{1,1,1},{-1,-1,1},{-1,1,-1},{1,-1,-1}};
    m.indices={{0,2,1},{0,1,3},{0,3,2},{1,2,3}}; return m;
}
struct Fixture { Semantic::ExpectedIdentity expected; Semantic::VerifiedFaceBinding binding; Json payload; };
Fixture fixture()
{
    Fixture f; std::string error; const auto geometry=mesh();
    REQUIRE(Semantic::prove_ordered_faces(std::string(64,'a'),geometry,geometry,f.binding,error));
    f.expected.request_id="request-1"; f.expected.source_sha256=f.binding.source_sha256();
    f.expected.geometry_id=f.binding.geometry_id(); f.expected.face_count=f.binding.face_count();
    f.expected.weights_sha256=std::string(64,'b'); f.expected.runtime_sha256=std::string(64,'c'); f.expected.policy_sha256=std::string(64,'d');
    f.payload={{"schema",Semantic::schema},{"label_schema",Semantic::label_schema},{"request_id",f.expected.request_id},
        {"source_sha256",f.expected.source_sha256},{"geometry_id",f.expected.geometry_id},{"render_geometry_id",f.binding.render_geometry_id()},
        {"face_count",4},{"weights_sha256",f.expected.weights_sha256},{"runtime_sha256",f.expected.runtime_sha256},
        {"policy_sha256",f.expected.policy_sha256},{"subjects",Json::array({"person-a","person-b"})},
        {"regions",Json::array({{{"subject_id","person-a"},{"label","re"},{"samples",Json::array({Json::array({0,.99,.95,3,2})})}},
                               {{"subject_id","person-b"},{"label","face"},{"samples",Json::array({Json::array({1,.99,.95,3,2})})}}})}};
    return f;
}
void fails_without_replacement(const Fixture& f,const std::string& bytes)
{
    Semantic::Evidence previous; previous.identity.request_id="previous-confirmed-evidence"; previous.face_regions={42};
    std::string error;
    CHECK_FALSE(Semantic::decode(bytes,f.expected,f.binding,previous,error));
    CHECK_FALSE(error.empty());
    CHECK(previous.identity.request_id=="previous-confirmed-evidence");
    CHECK(previous.face_regions==std::vector<int32_t>{42});
}
}

TEST_CASE("semantic regions preserve separate subjects and never create user authority", "[LocalSemanticEvidence]")
{
    const auto f=fixture(); Semantic::Evidence out; std::string error;
    REQUIRE(Semantic::decode(f.payload.dump(),f.expected,f.binding,out,error));
    REQUIRE(out.regions.size()==2); CHECK(out.known_faces==2); CHECK(out.unknown_faces==2);
    CHECK(out.face_regions==std::vector<int32_t>{0,1,-1,-1});
    CHECK(out.regions[0].subject_id=="person-a"); CHECK(out.regions[1].subject_id=="person-b");
    CHECK(out.regions[0].protect_color); CHECK_FALSE(out.regions[1].protect_color);
    for(const auto& region:out.regions) {
        CHECK_FALSE(region.user_protected); CHECK_FALSE(region.locked_physical_slot.has_value());
        CHECK(region.id.find("auto-semantic:")==0);
    }
}

TEST_CASE("body clothing evidence remains automatic and cannot acquire user authority", "[LocalSemanticEvidence]")
{
    auto f=fixture(); f.payload["regions"][0]["label"]="cloth";
    Semantic::Evidence out; std::string error;
    REQUIRE(Semantic::decode(f.payload.dump(),f.expected,f.binding,out,error));
    CHECK(out.regions[0].label=="cloth");
    CHECK_FALSE(out.regions[0].user_protected);
    CHECK_FALSE(out.regions[0].locked_physical_slot.has_value());
}

TEST_CASE("optional eye hints stay inside a host verified eye without changing semantic confidence", "[LocalSemanticEvidence]")
{
    auto f=fixture();
    f.payload["eye_details"]=Json::array({{{"subject_id","person-a"},{"label","re"},{"aperture_faces",{0}},{"iris_faces",{0}}}});
    Semantic::Evidence out;std::string error;
    REQUIRE(Semantic::decode(f.payload.dump(),f.expected,f.binding,out,error));
    REQUIRE(out.eye_details.size()==1);
    CHECK(out.eye_details[0].iris_faces==std::vector<size_t>{0});
    CHECK(out.known_faces==2);
    CHECK(out.face_regions==std::vector<int32_t>{0,1,-1,-1});
    const int change=GENERATE(0,1,2,3,4,5);
    if(change==0)f.payload["eye_details"][0]["iris_faces"]={1};
    if(change==1)f.payload["eye_details"][0]["aperture_faces"]={0,2};
    if(change==2)f.payload["eye_details"][0]["subject_id"]="person-b";
    if(change==3)f.payload["eye_details"][0]["label"]="pupil";
    if(change==4)f.payload["eye_details"][0]["confirmed"]=true;
    if(change==5)f.payload["eye_details"].push_back(f.payload["eye_details"][0]);
    fails_without_replacement(f,f.payload.dump());
}

TEST_CASE("unobserved low confidence and ambiguous semantic faces stay unknown", "[LocalSemanticEvidence]")
{
    auto f=fixture();
    f.payload["regions"][0]["samples"]=Json::array({Json::array({0,.99,.95,3,2}),Json::array({2,.89,.99,50,4}),Json::array({3,.99,.84,50,4})});
    f.payload["regions"][1]["samples"]=Json::array({Json::array({0,.99,.99,50,4}),Json::array({1,.9,.85,2,1})});
    Semantic::Evidence out; std::string error;
    REQUIRE(Semantic::decode(f.payload.dump(),f.expected,f.binding,out,error));
    CHECK(out.known_faces==1); CHECK(out.unknown_faces==3); CHECK(out.ambiguous_faces==1); CHECK(out.below_threshold_faces==2);
    REQUIRE(out.regions.size()==1); CHECK(out.regions[0].subject_id=="person-b"); CHECK(out.regions[0].faces==std::vector<size_t>{1});
}

TEST_CASE("Multiview facial shapes can recover a missed class without changing raw evidence", "[LocalSemanticEvidence]") {
    auto f=fixture();f.payload["regions"][0]["label"]="face";
    f.payload["feature_details"]=Json::array({{{"subject_id","person-a"},{"label","re"},
        {"faces",{0,2}},{"iris_faces",{2}},{"view_support",2}}});
    Semantic::Evidence out;std::string error;
    REQUIRE(Semantic::decode(f.payload.dump(),f.expected,f.binding,out,error));
    REQUIRE(out.feature_details.size()==1);
    CHECK(out.feature_details[0].faces==std::vector<size_t>{0,2});
    CHECK(out.face_regions==std::vector<int32_t>{0,1,-1,-1});
    CHECK(out.known_faces==2);
    CHECK(out.regions[0].label=="face");
    const int change=GENERATE(0,1,2,3,4,5,6,7,8);
    auto& hint=f.payload["feature_details"][0];
    if(change==0)hint["faces"]={0,1};
    if(change==1)hint["faces"]={2,3};
    if(change==2)hint["faces"]={0,4};
    if(change==3)hint["view_support"]=1;
    if(change==4)hint["iris_faces"]={3};
    if(change==5)hint["confirmed"]=true;
    if(change==6)hint["label"]="nose";
    if(change==7)f.payload["regions"][0]["label"]="hair";
    if(change==8)f.payload["feature_details"].push_back(hint);
    fails_without_replacement(f,f.payload.dump());
}

TEST_CASE("Unknown facial parts can use separate short native surface paths to accepted face context", "[LocalSemanticEvidence]") {
    auto f=fixture();f.payload["regions"][0]["label"]="face";
    f.payload["regions"][1]["subject_id"]="person-a";
    f.payload["regions"][1]["label"]="nose";
    f.payload["feature_details"]=Json::array({{{"subject_id","person-a"},{"label","re"},
        {"faces",{2,3}},{"iris_faces",{2}},{"view_support",2},{"anchor_paths",{{2,0},{3,1}}}}});
    Semantic::Evidence out;std::string error;
    const bool decoded=Semantic::decode(f.payload.dump(),f.expected,f.binding,out,error);CAPTURE(error);
    REQUIRE(decoded);
    CHECK(out.feature_details[0].faces==std::vector<size_t>{2,3});
    CHECK(out.face_regions==std::vector<int32_t>{0,1,-1,-1});
    const int change=GENERATE(0,1,2,3,4,5,6,7,8,9);
    auto& hint=f.payload["feature_details"][0];
    if(change==0)hint["anchor_paths"]={{2,0}};
    if(change==1)hint["anchor_paths"]={{2,0},{3,0}};
    if(change==2)hint["anchor_paths"]={{0,1},{3,0}};
    if(change==3)hint["anchor_paths"]={{2,0,2,1},{3,0}};
    if(change==4)hint["anchor_paths"]={{2,4},{3,0}};
    if(change==5)f.payload["regions"][1]["subject_id"]="person-b";
    if(change==6)f.payload["regions"][1]["label"]="hair";
    if(change==7)hint["anchor_paths"]={{2,0},{3,2}};
    if(change==8)hint["anchor_paths"]={{2,0},{3,1},{2,0},{3,1},{2,0}};
    if(change==9) {
        auto disconnected=mesh();
        for(int c=0;c<3;++c) {disconnected.vertices.push_back((disconnected.vertices[disconnected.indices[0][c]]+Vec3f(.01f,0,0)).eval());
            disconnected.indices[0][c]=int(disconnected.vertices.size()-1);}
        REQUIRE(Semantic::prove_ordered_faces(f.expected.source_sha256,disconnected,disconnected,f.binding,error));
        f.expected.geometry_id=f.binding.geometry_id();f.payload["geometry_id"]=f.expected.geometry_id;
        f.payload["render_geometry_id"]=f.binding.render_geometry_id();
    }
    fails_without_replacement(f,f.payload.dump());
}

TEST_CASE("no detections are a valid wholly unknown semantic observation", "[LocalSemanticEvidence]")
{
    auto f=fixture(); f.payload["subjects"]=Json::array(); f.payload["regions"]=Json::array();
    Semantic::Evidence out; std::string error;
    REQUIRE(Semantic::decode(f.payload.dump(),f.expected,f.binding,out,error));
    CHECK(out.regions.empty()); CHECK(out.known_faces==0); CHECK(out.unknown_faces==4);
}

TEST_CASE("echoed identity alone cannot establish native semantic face correspondence", "[LocalSemanticEvidence]")
{
    auto f=fixture(); f.binding=Semantic::VerifiedFaceBinding{};
    fails_without_replacement(f,f.payload.dump());
}

TEST_CASE("semantic evidence rejects stale runtime weights policies requests and meshes", "[LocalSemanticEvidence]")
{
    auto f=fixture(); const std::string key=GENERATE("request_id","source_sha256","geometry_id","render_geometry_id","weights_sha256","runtime_sha256","policy_sha256");
    f.payload[key]="stale"; fails_without_replacement(f,f.payload.dump());
}

TEST_CASE("semantic payloads reject authority injection and unsupported label claims", "[LocalSemanticEvidence]")
{
    auto f=fixture(); const int change=GENERATE(0,1,2,3,4,5,6,7);
    if(change==0) f.payload["confirmed"]=true;
    if(change==1) f.payload["physical_channels"]=Json::array();
    if(change==2) f.payload["user_overrides"]=Json::array();
    if(change==3) f.payload["regions"][0]["user_protected"]=true;
    if(change==4) f.payload["regions"][0]["locked_physical_slot"]=3;
    if(change==5) f.payload["regions"][0]["protect_color"]=true;
    if(change==6) f.payload["regions"][0]["label"]="teeth";
    if(change==7) f.payload["label_schema"]="arbitrary-label-model";
    fails_without_replacement(f,f.payload.dump());
}

TEST_CASE("semantic indices scores support and canonical collections are strictly checked", "[LocalSemanticEvidence]")
{
    auto f=fixture(); const int change=GENERATE(0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15);
    auto& samples=f.payload["regions"][0]["samples"];
    if(change==0) samples[0][0]=-1;
    if(change==1) samples[0][0]=4;
    if(change==2) samples[0][0]=true;
    if(change==3) samples[0][0]=0.0;
    if(change==4) samples[0][1]=1.1;
    if(change==5) samples[0][1]=nullptr;
    if(change==6) samples[0][2]=-.1;
    if(change==7) samples[0][3]=0;
    if(change==8) samples[0][4]=17;
    if(change==9) samples.push_back(samples[0]);
    if(change==10) f.payload["subjects"].push_back("person-a");
    if(change==11) f.payload["regions"][0]["subject_id"]="absent-person";
    if(change==12) f.payload["regions"].push_back(f.payload["regions"][0]);
    if(change==13) f.payload["subjects"][0]="../person";
    if(change==14) f.payload["face_count"]=5;
    if(change==15) samples[0][4]=4; // More views than observed pixels.
    fails_without_replacement(f,f.payload.dump());
}

TEST_CASE("semantic parser rejects duplicate JSON keys and excessive nesting or subjects", "[LocalSemanticEvidence]")
{
    auto f=fixture(); const int change=GENERATE(0,1,2);
    if(change==0) { auto bytes=f.payload.dump(); bytes.insert(1,"\"schema\":\"duplicate\","); fails_without_replacement(f,bytes); }
    if(change==1) fails_without_replacement(f,std::string(20,'[')+"0"+std::string(20,']'));
    if(change==2) { for(int i=0;i<33;++i) f.payload["subjects"].push_back("p"+std::to_string(i)); fails_without_replacement(f,f.payload.dump()); }
}

TEST_CASE("host semantic thresholds cannot be weakened through invalid policy values", "[LocalSemanticEvidence]")
{
    auto f=fixture(); const int change=GENERATE(0,1,2,3);
    if(change==0) f.expected.minimum_confidence=std::numeric_limits<double>::quiet_NaN();
    if(change==1) f.expected.minimum_dominance=.1;
    if(change==2) f.expected.maximum_views=65;
    if(change==3) f.expected.protected_labels.insert("pupil");
    fails_without_replacement(f,f.payload.dump());
}

TEST_CASE("ordered geometry proof permits vertex renumbering but rejects changed face meaning", "[LocalSemanticEvidence]")
{
    const auto original=mesh(); auto rendered=original;
    const int change=GENERATE(0,1,2,3,4,5);
    if(change==0) { std::reverse(rendered.vertices.begin(),rendered.vertices.end()); for(auto& f:rendered.indices) for(int c=0;c<3;++c) f[c]=3-f[c]; }
    if(change==1) std::swap(rendered.indices[0],rendered.indices[1]);
    if(change==2) std::swap(rendered.indices[0][0],rendered.indices[0][1]);
    if(change==3) rendered.vertices[0].x()+=.01f;
    if(change==4) rendered.vertices[0].x()=std::numeric_limits<float>::infinity();
    if(change==5) rendered.indices[0][0]=999;
    Semantic::VerifiedFaceBinding binding; std::string error;
    if(change==0) {
        REQUIRE(Semantic::prove_ordered_faces(std::string(64,'a'),original,rendered,binding,error));
        CHECK(binding.valid()); CHECK(binding.face_count()==4);
    } else {
        CHECK_FALSE(Semantic::prove_ordered_faces(std::string(64,'a'),original,rendered,binding,error));
        CHECK_FALSE(binding.valid());
    }
}

TEST_CASE("coincident coordinates do not justify a changed vertex sharing graph", "[LocalSemanticEvidence]")
{
    auto original=mesh(); original.vertices.push_back(original.vertices[0]); original.indices[0][0]=4;
    auto rendered=original; rendered.indices[1][0]=4;
    Semantic::VerifiedFaceBinding binding; std::string error;
    CHECK_FALSE(Semantic::prove_ordered_faces(std::string(64,'a'),original,rendered,binding,error));
    CHECK_FALSE(binding.valid());
}

TEST_CASE("failed geometry proofs retain the last verified binding", "[LocalSemanticEvidence]")
{
    const auto original=mesh(); auto rendered=original;
    Semantic::VerifiedFaceBinding binding; std::string error;
    REQUIRE(Semantic::prove_ordered_faces(std::string(64,'a'),original,original,binding,error));
    const auto native_id=binding.geometry_id(),render_id=binding.render_geometry_id();
    const int change=GENERATE(0,1,2,3);
    if(change==0) rendered.vertices.push_back({0,0,0});
    if(change==1) rendered.indices[0][1]=rendered.indices[0][0];
    if(change==2) rendered.vertices[0].x()+=.00001f; // Near the origin this exceeds the explicit relative bound.
    if(change==3) rendered.vertices[0].x()=std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(Semantic::prove_ordered_faces(std::string(64,'b'),original,rendered,binding,error));
    CHECK(binding.valid()); CHECK(binding.source_sha256()==std::string(64,'a'));
    CHECK(binding.geometry_id()==native_id); CHECK(binding.render_geometry_id()==render_id);
}

TEST_CASE("one pixel does not establish semantic coverage and oversized support is rejected", "[LocalSemanticEvidence]")
{
    auto f=fixture(); auto& sample=f.payload["regions"][0]["samples"][0];
    sample[3]=1; sample[4]=1;
    Semantic::Evidence out; std::string error;
    REQUIRE(Semantic::decode(f.payload.dump(),f.expected,f.binding,out,error));
    CHECK(out.face_regions[0]==-1); CHECK(out.below_threshold_faces==1);
    sample[3]=4096ULL*4096+1;
    fails_without_replacement(f,f.payload.dump());
}

TEST_CASE("semantic overlap stays unknown independently of region order", "[LocalSemanticEvidence]")
{
    auto f=fixture();
    f.payload["regions"][1]["samples"]=Json::array({Json::array({0,.6,.6,2,1})});
    Semantic::Evidence first,second; std::string error;
    REQUIRE(Semantic::decode(f.payload.dump(),f.expected,f.binding,first,error));
    std::reverse(f.payload["regions"].begin(),f.payload["regions"].end());
    REQUIRE(Semantic::decode(f.payload.dump(),f.expected,f.binding,second,error));
    CHECK(first.face_regions==second.face_regions); CHECK(first.known_faces==0); CHECK(second.ambiguous_faces==1);
}

TEST_CASE("coincident triangles cannot prove texture face identity through geometry alone", "[LocalSemanticEvidence]")
{
    auto original=mesh(); const auto triangle=original.indices[0];
    const int change=GENERATE(0,1,2,3);
    if(change==0) original.indices.push_back(triangle);
    if(change==1) original.indices.push_back({triangle[1],triangle[2],triangle[0]});
    if(change==2) original.indices.push_back({triangle[2],triangle[1],triangle[0]});
    if(change==3) {
        for(int c=0;c<3;++c) original.vertices.push_back(original.vertices[triangle[c]]);
        original.indices.push_back({4,5,6}); // Distinct seam indices, identical geometric triangle.
    }
    Semantic::VerifiedFaceBinding binding; std::string error;
    REQUIRE(Semantic::prove_ordered_faces(std::string(64,'a'),original,original,binding,error));
    CHECK_FALSE(binding.usable_face(0));CHECK_FALSE(binding.usable_face(4));
    CHECK(binding.usable_face(1));
    auto f=fixture();f.binding=binding;
    f.expected.geometry_id=binding.geometry_id();f.expected.face_count=binding.face_count();
    f.payload["geometry_id"]=binding.geometry_id();f.payload["render_geometry_id"]=binding.render_geometry_id();
    f.payload["face_count"]=binding.face_count();
    Semantic::Evidence output;
    // Even perfectly confident evidence cannot authorize either ambiguous face.
    CHECK_FALSE(Semantic::decode(f.payload.dump(),f.expected,binding,output,error));
    f.payload["regions"]=Json::array({{{"subject_id","person-a"},{"label","face"},{"samples",{{1,.99,.99,12,2}}}}});
    REQUIRE(Semantic::decode(f.payload.dump(),f.expected,binding,output,error));
    CHECK(output.known_faces==1);CHECK(output.face_regions[0]==-1);CHECK(output.face_regions[4]==-1);
}
