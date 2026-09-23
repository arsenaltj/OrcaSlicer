#include "slic3r/GUI/AI/Model/LocalSemanticGeometry.hpp"
#include "slic3r/GUI/AI/Model/LocalSemanticEvidence.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/cstdlib.hpp>

using namespace Slic3r;
namespace Transfer = GUI::LocalSemanticGeometry;
namespace {
indexed_triangle_set triangle()
{ indexed_triangle_set m;m.vertices={{-0.f,0,0},{1,0,0},{0,1,0}};m.indices={{0,1,2}};return m; }
std::string packet()
{ std::string data,error;REQUIRE(Transfer::encode(triangle(),std::string(64,'a'),data,error));return data; }
void resign(std::string& data)
{ const auto digest=Transfer::detail::digest(data.data(),data.size()-32,nullptr);data.replace(data.size()-32,32,digest); }
}

TEST_CASE("geometry transfer preserves vertex sharing ordered corners and signed zero", "[LocalSemanticGeometry]")
{
    const auto bytes=packet();Transfer::Packet result;std::string error;
    REQUIRE(Transfer::decode(bytes,std::string(64,'a'),result,error));
    CHECK(result.source_sha256==std::string(64,'a'));
    CHECK(result.geometry_id==AI::SurfaceSelectionPersistence::geometry_fingerprint(triangle()));
    REQUIRE(result.mesh.vertices.size()==3);CHECK(std::signbit(result.mesh.vertices[0][0]));
    for(int corner=0;corner<3;++corner) CHECK(result.mesh.indices[0][corner]==corner);
    std::string encoded;REQUIRE(Transfer::encode(result.mesh,result.source_sha256,encoded,error));CHECK(encoded==bytes);
    auto positive=triangle();positive.vertices[0][0]=0.f;
    CHECK(AI::SurfaceSelectionPersistence::geometry_fingerprint(positive)==result.geometry_id);
    REQUIRE(Transfer::encode(positive,result.source_sha256,encoded,error));CHECK(encoded!=bytes);
}

TEST_CASE("geometry transfer rejects corrupt stale nonfinite and invalid indexed payloads atomically", "[LocalSemanticGeometry]")
{
    auto data=packet();const int change=GENERATE(0,1,2,3,4,5,6,7,8,9,10);
    if(change==0) data[0]='X';
    if(change==1) data.pop_back();
    if(change==2) data+='x';
    if(change==3) data[24]='b';
    if(change==4) data[8]=char(255);
    if(change==5) data[16]=0;
    if(change==6) data[160]^=1; // altered arrays, original checksum
    if(change==7) {data[154]=char(0x80);data[155]=char(0x7f);resign(data);} // infinity, valid checksum
    if(change==8) {data[188]=3;resign(data);} // index outside array
    if(change==9) {data[88]=data[88]=='0'?'1':'0';resign(data);} // valid SHA text, wrong geometry
    if(change==10) {data[188]=1;resign(data);} // changed corner order with stale identity
    Transfer::Packet old;old.source_sha256="previous";old.mesh=triangle();std::string error;
    CHECK_FALSE(Transfer::decode(data,std::string(64,'a'),old,error));
    CHECK_FALSE(error.empty());CHECK(old.source_sha256=="previous");
    for(int corner=0;corner<3;++corner) CHECK(old.mesh.indices[0][corner]==corner);
}

TEST_CASE("geometry transfer cancels and rejects invalid source meshes without replacing output", "[LocalSemanticGeometry]")
{
    auto mesh=triangle();std::string output="keep",error;const int change=GENERATE(0,1,2,3,4);
    std::atomic<bool> cancel{change==0};
    if(change==1) mesh.vertices[0][0]=std::numeric_limits<float>::quiet_NaN();
    if(change==2) mesh.indices[0][0]=-1;
    if(change==3) mesh.indices.clear();
    if(change==4) mesh.vertices.push_back(Vec3f(std::numeric_limits<float>::infinity(),0,0));
    CHECK_FALSE(Transfer::encode(mesh,std::string(64,'a'),output,error,&cancel));CHECK(output=="keep");
    cancel=true;Transfer::Packet old;old.source_sha256="keep";
    CHECK_FALSE(Transfer::decode(packet(),std::string(64,'a'),old,error,&cancel));CHECK(old.source_sha256=="keep");
}

// Explicit local fixture manifest; no providers, shells, networking or rendering
// mocks. Exports native arrays and proves the independently rendered packet.
TEST_CASE("semantic render packets prove native Assimp face correspondence for supplied assets", "[.][LocalSemanticGeometryNative]")
{
    const char* raw=boost::nowide::getenv("ORCA_SEMANTIC_GEOMETRY_FIXTURES");
    REQUIRE(raw!=nullptr);const boost::filesystem::path manifest(raw);
    boost::filesystem::ifstream input(manifest);REQUIRE(input.good());nlohmann::json spec;input>>spec;
    REQUIRE(spec.is_array());REQUIRE_FALSE(spec.empty());
    auto report=nlohmann::json::array();
    for(const auto& item:spec) {
        const auto name=item.at("name").get<std::string>();CAPTURE(name);
        const boost::filesystem::path source(item.at("source").get<std::string>());
        TriangleMesh native;ObjInfo colors;std::string error;
        REQUIRE(AI::load_model_artifact(source,native,colors,error));
        const auto source_sha=AI::model_artifact_sha256(source);REQUIRE(source_sha.size()==64);
        std::string encoded;REQUIRE(Transfer::encode(native.its,source_sha,encoded,error));
        const boost::filesystem::path export_path(item.at("native_packet").get<std::string>());
        REQUIRE_FALSE(boost::filesystem::exists(export_path));
        {boost::filesystem::ofstream output(export_path,std::ios::binary);output.write(encoded.data(),encoded.size());REQUIRE(output.good());}
        boost::filesystem::ifstream packet_file(boost::filesystem::path(item.at("render_packet").get<std::string>()),std::ios::binary);
        REQUIRE(packet_file.good());std::string bytes((std::istreambuf_iterator<char>(packet_file)),{});
        Transfer::Packet rendered;REQUIRE(Transfer::decode(bytes,source_sha,rendered,error));
        GUI::LocalSemanticEvidence::VerifiedFaceBinding binding;
        const bool proven=GUI::LocalSemanticEvidence::prove_ordered_faces(source_sha,native.its,rendered.mesh,binding,error);
        double max_error=0;size_t different_components=0;
        if(native.its.indices.size()==rendered.mesh.indices.size())
            for(size_t f=0;f<native.its.indices.size();++f) for(int c=0;c<3;++c) for(int a=0;a<3;++a) {
                const double delta=std::abs(double(native.its.vertices[native.its.indices[f][c]][a])-double(rendered.mesh.vertices[rendered.mesh.indices[f][c]][a]));
                max_error=std::max(max_error,delta);different_components+=delta!=0;
            }
        const bool expected_proven=item.value("expected_proven",true);
        report.push_back({{"name",name},{"source_sha256",source_sha},{"vertices",native.its.vertices.size()},
            {"faces",native.its.indices.size()},{"native_geometry",AI::SurfaceSelectionPersistence::geometry_fingerprint(native.its)},
            {"render_geometry",rendered.geometry_id},{"proven",proven},{"expected_proven",expected_proven},
            {"max_corner_error_mm",max_error},{"different_components",different_components}});
        CHECK(proven==expected_proven);
        if(!expected_proven) CHECK_FALSE(binding.valid());
    }
    const auto output_path=manifest.parent_path()/"native-proof-report.json";
    REQUIRE_FALSE(boost::filesystem::exists(output_path));boost::filesystem::ofstream output(output_path);output<<report.dump(2);REQUIRE(output.good());
}
