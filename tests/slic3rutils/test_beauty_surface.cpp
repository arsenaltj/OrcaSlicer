#include <catch2/catch_all.hpp>
#include "slic3r/GUI/AI/Model/BeautySurface.hpp"
#include "slic3r/GUI/AI/Model/BeautyDocument.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "slic3r/GUI/AI/Model/ModelFinishing.hpp"
#include <boost/filesystem.hpp>
#include <numeric>

using namespace Slic3r;
using namespace Slic3r::AI;
namespace {
indexed_triangle_set grid(bool seam=false) {
    indexed_triangle_set mesh;
    for(int y=0;y<7;++y)for(int x=0;x<7;++x)mesh.vertices.emplace_back(float(x),float(y),0.f);
    for(int y=0;y<6;++y)for(int x=0;x<6;++x) {
        const int a=y*7+x;mesh.indices.emplace_back(a,a+1,a+8);mesh.indices.emplace_back(a,a+8,a+7);
    }
    if(seam) {
        const int duplicate=int(mesh.vertices.size());mesh.vertices.push_back(mesh.vertices[24]);
        for(size_t f=36;f<mesh.indices.size();++f)for(int k=0;k<3;++k)if(mesh.indices[f][k]==24)mesh.indices[f][k]=duplicate;
    }
    return mesh;
}
std::vector<uint8_t> middle(const indexed_triangle_set& mesh) {
    std::vector<uint8_t> mask(mesh.indices.size());
    for(size_t f=0;f<mask.size();++f) {
        Vec3f c=Vec3f::Zero();for(int k=0;k<3;++k)c+=mesh.vertices[mesh.indices[f][k]]/3.f;
        mask[f]=c.x()>1 && c.x()<5 && c.y()>1 && c.y()<5;
    }
    return mask;
}
BeautyDocument document_for(const std::shared_ptr<BeautySurface>& s) {
    BeautyDocument d;d.geometry_id=s->geometry_id;d.face_count=s->face_patch.size();d.face_patch=s->face_patch;return d;
}
struct Fixture {
    boost::filesystem::path root=boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("beauty-surface-%%%%-%%%%");
    Fixture(){boost::filesystem::create_directory(root);}
    ~Fixture(){boost::system::error_code error;boost::filesystem::remove_all(root,error);}
};
}
TEST_CASE("Beauty patches label the original surface deterministically and reload their saved partition","[BeautyWorkbench][BeautySurface]") {
    const auto mesh=grid(true);const auto before=mesh;
    auto a=BeautySurface::build(mesh,{}),b=BeautySurface::build(mesh,{});
    REQUIRE(a->face_patch==b->face_patch);REQUIRE(mesh.indices==before.indices);REQUIRE(mesh.vertices==before.vertices);
    REQUIRE(a->vertex_class[24]==a->vertex_class.back());
    CHECK(a->boundary_edges==24); // perimeter only; UV seam is not an opening
    CHECK(a->nonmanifold_edges==0);
    size_t covered=0;for(const auto& p:a->patches) {
        covered+=p.faces.size();
        for(auto n:p.neighbors)REQUIRE(n<a->patches.size());
    }
    REQUIRE(covered==mesh.indices.size());
    const auto decoded=BeautyDocument::decode(document_for(a).encode(),a->geometry_id,mesh.indices.size());
    REQUIRE(BeautySurface::build(mesh,{},decoded.face_patch)->face_patch==a->face_patch);
    REQUIRE_THROWS(BeautySurface::build(mesh,{},std::vector<uint32_t>(mesh.indices.size(),uint32_t(mesh.indices.size()))));
    REQUIRE_THROWS(BeautySurface::build(mesh,{}, {},[]{return true;}));
}
TEST_CASE("Topology preflight distinguishes open and nonmanifold edges before painting","[BeautyWorkbench][BeautySurface]") {
    indexed_triangle_set open;
    open.vertices={Vec3f(0,0,0),Vec3f(1,0,0),Vec3f(0,1,0),Vec3f(0,-1,0),Vec3f(0,0,1)};
    open.indices.emplace_back(0,1,2);
    auto surface=BeautySurface::build(open,{});
    CHECK(surface->boundary_edges==3);
    CHECK(surface->nonmanifold_edges==0);
    open.indices.emplace_back(1,0,3);
    open.indices.emplace_back(0,1,4);
    surface=BeautySurface::build(open,{});
    CHECK(surface->boundary_edges==6);
    CHECK(surface->nonmanifold_edges==1);
}
TEST_CASE("Appearance feathering stays within the selected and unprotected faces","[BeautyWorkbench][BeautySurface]") {
    const auto mesh=grid();const auto surface=BeautySurface::build(mesh,{});auto selected=middle(mesh);
    std::vector<uint8_t> protection(selected.size());protection[42]=1;
    const auto crisp=surface->face_weights(mesh,selected,protection,0),soft=surface->face_weights(mesh,selected,protection,1.5);
    bool transition=false;
    for(size_t f=0;f<selected.size();++f) {
        if(!selected[f] || protection[f]) {CHECK(crisp[f]==0);CHECK(soft[f]==0);}
        else {CHECK(crisp[f]==1);CHECK(soft[f]>=0);CHECK(soft[f]<=1);transition|=soft[f]>0 && soft[f]<1;}
    }
    CHECK(transition);REQUIRE_THROWS(surface->face_weights(mesh,{},protection,1));
}
TEST_CASE("Geometry weights keep unselected seams fixed and move the selected interior continuously","[BeautyWorkbench][BeautySurface]") {
    const auto mesh=grid(true);const auto surface=BeautySurface::build(mesh,{});auto selected=middle(mesh);
    const auto weights=surface->vertex_weights(mesh,selected,{},3);
    CHECK_THAT(weights[24],Catch::Matchers::WithinAbs(weights.back(),1e-6));
    CHECK(weights[24]>weights[23]);CHECK(weights[23]>0);
    size_t moved=0;auto edited=surface->deform(mesh,selected,{},.2,3,moved);
    REQUIRE(moved>0);REQUIRE(edited.indices==mesh.indices);
    for(size_t f=0;f<selected.size();++f)if(!selected[f])for(int k=0;k<3;++k) {
        const auto v=mesh.indices[f][k];CHECK(edited.vertices[v]==mesh.vertices[v]);
    }
    REQUIRE(edited.vertices[24]==edited.vertices.back());
    std::vector<uint8_t> protected_faces(selected.size());protected_faces[42]=1;
    edited=surface->deform(mesh,selected,protected_faces,.2,3,moved);
    for(int k=0;k<3;++k)CHECK(edited.vertices[mesh.indices[42][k]]==mesh.vertices[mesh.indices[42][k]]);
    auto stale=mesh;stale.vertices[0].z()+=.01f;
    REQUIRE_THROWS(surface->deform(stale,selected,{},.2,3,moved));
    REQUIRE_THROWS(surface->deform(mesh,selected,{},3,3,moved));
}
TEST_CASE("Saved beauty regions preserve partial patches protection and reversible group edits","[BeautyWorkbench][BeautySurface]") {
    const auto surface=BeautySurface::build(grid(),{});auto d=document_for(surface);BeautyGroupHistory history;
    history.record(d);const auto face=d.add_group("face",{10,11,12,13});
    history.record(d);const auto detail=d.split_group(face,{11,12},"detail");
    d.group(detail).locked=true;d.group(detail).preserve_color=true;
    CHECK(d.protection()[11]==1);CHECK(d.protection()[10]==0);
    REQUIRE_THROWS(d.merge_selection(detail,{14}));
    auto saved=BeautyDocument::decode(d.encode(),d.geometry_id,d.face_count);
    CHECK(saved.group(detail).faces==std::vector<size_t>{11,12});CHECK(saved.group(detail).locked);
    REQUIRE(history.undo(d));CHECK(d.groups.size()==1);CHECK(d.group(face).faces.size()==4);
    REQUIRE(history.redo(d));CHECK(d.groups.size()==2);CHECK(d.group(detail).locked);
    REQUIRE_THROWS(BeautyDocument::decode(d.encode(),"different",d.face_count));
    auto corrupt=d.encode();corrupt["groups"][0]["faces"]={d.face_count};
    REQUIRE_THROWS(BeautyDocument::decode(corrupt,d.geometry_id,d.face_count));
    corrupt=d.encode();corrupt["patch_runs"]={{0,d.face_count+1}};
    REQUIRE_THROWS(BeautyDocument::decode(corrupt,d.geometry_id,d.face_count));
}
TEST_CASE("Beauty geometry versions round trip without overwriting the source or losing face correspondence","[BeautyWorkbench][BeautySurface]") {
    Fixture fixture;const auto source=fixture.root/"source.glb",target=fixture.root/"edited.glb";
    const auto initial=grid(true);std::string error;
    REQUIRE(write_model_artifact(source,initial,std::vector<RGBA>(initial.vertices.size(),RGBA{.6f,.4f,.3f,1}),error));
    TriangleMesh mesh;ObjInfo colors;REQUIRE(load_model_artifact(source,mesh,colors,error));
    auto surface=BeautySurface::build(mesh.its,colors.vertex_colors);auto doc=document_for(surface);
    ModelFinishingOptions options;options.smooth_surface=false;options.repair_mesh=false;options.beauty_deform=true;
    options.beauty_surface=surface;options.beauty_document=doc.encode();options.beauty_displacement_mm=.1;options.beauty_falloff_mm=1;
    const auto mask=middle(mesh.its);for(size_t f=0;f<mask.size();++f)if(mask[f])options.selected_faces.push_back(f);
    const auto hash=model_artifact_sha256(source);
    const auto result=finish_model_artifact(source,target,options);INFO(result.error);REQUIRE(result.success);REQUIRE(result.moved_vertices>0);
    CHECK(model_artifact_sha256(source)==hash);TriangleMesh edited;ObjInfo after;REQUIRE(load_model_artifact(target,edited,after,error));
    CHECK(edited.its.indices==mesh.its.indices);CHECK(after.vertex_colors==colors.vertex_colors);
    CHECK_FALSE(finish_model_artifact(source,target,options).success);
    const auto canceled=finish_model_artifact(source,fixture.root/"canceled.glb",options,[]{return true;});CHECK(canceled.canceled);
    CHECK_FALSE(boost::filesystem::exists(fixture.root/"canceled.glb"));
}
TEST_CASE("Beauty appearance uses persisted protection and rejects an unrelated document","[BeautyWorkbench][BeautySurface]") {
    Fixture fixture;const auto source=boost::filesystem::path(std::string(TEST_DATA_DIR))/"model_artifact"/"jpeg-textured.glb";
    TriangleMesh mesh;ObjInfo colors;std::string error;REQUIRE(load_model_artifact(source,mesh,colors,error));
    const auto surface=BeautySurface::build(mesh.its,colors.vertex_colors);auto doc=document_for(surface);
    ModelFinishingOptions options;options.smooth_surface=false;options.repair_mesh=false;options.beauty_appearance=true;
    options.beauty_surface=surface;options.selected_faces.resize(mesh.its.indices.size());std::iota(options.selected_faces.begin(),options.selected_faces.end(),0);
    const auto id=doc.add_group("protected",options.selected_faces);doc.group(id).locked=true;options.beauty_document=doc.encode();
    options.appearance.brightness=.1;options.appearance.face_weights.assign(mesh.its.indices.size(),1);
    CHECK_FALSE(finish_model_artifact(source,fixture.root/"locked.glb",options).success);CHECK_FALSE(boost::filesystem::exists(fixture.root/"locked.glb"));
    doc.group(id).locked=false;options.beauty_document=doc.encode();options.appearance.face_weights.clear();
    const auto result=finish_model_artifact(source,fixture.root/"appearance.glb",options);INFO(result.error);REQUIRE(result.success);CHECK(result.changed_texture_pixels>0);
    options.beauty_document["geometry_id"]="unrelated";CHECK_FALSE(finish_model_artifact(source,fixture.root/"wrong.glb",options).success);
}
