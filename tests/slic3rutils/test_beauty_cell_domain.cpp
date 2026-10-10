#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "slic3r/GUI/AI/Model/BeautyCellEdits.hpp"
#include "slic3r/GUI/AI/ModelGeneration/PortraitShapeDetails.hpp"
#include "slic3r/GUI/AI/ModelGeneration/PortraitContourProposal.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelSemanticColoring.hpp"
#include "../test_utils.hpp"

using namespace Slic3r;
using namespace Slic3r::AI;
using Json=nlohmann::json;
namespace SP=SurfacePartition;
namespace {
struct Cells {
    indexed_triangle_set source;
    Json partition;
    Json request;
    std::vector<GUI::LocalSemanticEvidence::ShapeDetail> shapes;
    BeautySurfaceShapeLock lock;
    std::shared_ptr<BeautyLeafEditing> editing;
    Cells() {
        for(int i=0;i<1000;++i) {
            source.vertices.emplace_back(float(i*2),0,0);
            source.vertices.emplace_back(float(i*2+1),0,0);
            source.vertices.emplace_back(float(i*2),1,0);
            source.indices.emplace_back(i*3,i*3+1,i*3+2);
        }
        const auto geometry=SurfaceSelectionPersistence::geometry_fingerprint(source);
        Json identity={{"geometry_id",geometry},{"source_sha256",std::string(64,'b')},
            {"evidence_sha256",std::string(64,'c')},{"runtime_sha256",std::string(64,'d')},
            {"policy_sha256",std::string(64,'e')},{"baseline_sha256",std::string(64,'f')},
            {"boundary_policy_sha256",std::string(64,'1')},{"face_count",source.indices.size()}};
        const auto polygon=[](Json points){return Json{{"polygon",points},{"holes",Json::array()}};};
        auto base=polygon({{1,0,0},{0,1,0},{0,0,1}});
        base.update({{"label","face"},{"parent_label","face"},{"subject_id","person"}});
        const auto witnesses=[](const Json& p){return Json::array({{{"family","front"},{"polygons",Json::array({p})}},
            {{"family","oblique"},{"polygons",Json::array({p})}}});};
        const auto opening=witnesses(polygon({{.8,.1,.1},{.2,.7,.1},{.2,.1,.7}}));
        const auto iris=witnesses(polygon({{.6,.2,.2},{.5,.3,.2},{.4,.3,.3},{.5,.2,.3}}));
        request={{"schema","orca.surface-partition-request/v1"},{"identity",identity},
            {"triangle_budget",20},{"existing_added_triangles",0},{"faces",Json::array({{
                {"source_face_id",0},{"baseline_triangle_count",1},{"base",Json::array({base})},
                {"layers",Json::array({{{"label","iris-le"},{"parent_label","le"},{"subject_id","person"},
                    {"views",iris},{"envelope_views",opening}},{{"label","le"},{"parent_label","le"},
                    {"subject_id","person"},{"views",opening}}})}}})}};
        for(const auto& brow:std::vector<std::pair<size_t,std::string>>{{1,"lb"},{2,"rb"}}) {
            auto b=base;b["label"]=brow.second;b["parent_label"]=brow.second;
            request["faces"].push_back({{"source_face_id",brow.first},{"baseline_triangle_count",1},
                {"base",Json::array({b})},{"layers",Json::array()}});
        }
        partition=SP::build(request);
        for(const auto* label:{"le","lb","rb"}) {
            GUI::LocalSemanticEvidence::ShapeDetail shape;
            shape.subject_id="person";shape.label=label;shape.status="PROTECTED_SHAPE_UNCERTAIN";
            shape.accepted_faces={label==std::string("le") ? 0u : label==std::string("lb") ? 1u : 2u};
            shape.view_support=2;shape.reasons={"FIT_RISK"};shapes.push_back(shape);
        }
        lock=BeautySurfaceShapeLock::from_partition(partition,shapes,beauty_leaf_digest(partition.dump()));
        auto canonical=std::make_shared<VertexColorRegionEditor>();std::string error;
        std::vector<RGBA> colors(source.vertices.size(),RGBA{1,1,1,1});
        if(!canonical->initialize(source,colors,error)) throw std::runtime_error(error);
        editing=BeautyLeafEditing::build_cells(lock,partition,canonical,std::vector<RGBA>(1000,RGBA{1,1,1,1}));
    }
};
}

TEST_CASE("Contour editing uses exact cell IDs and separates eye white from iris", "[BeautyCellDomain][BeautyShapeLock]") {
    Cells f;
    REQUIRE(f.editing->contour());
    REQUIRE(f.editing->keys.empty());
    const auto iris=f.editing->cells->locate(0,Vec3d(.5,.25,.25));
    const auto white=f.editing->cells->locate(0,Vec3d(.4,.15,.45));
    REQUIRE(f.editing->owners[iris]!=f.editing->owners[white]);
    REQUIRE(f.editing->owners[iris]>=0);
    REQUIRE(f.editing->cells->indices(f.editing->cells->cell_id(iris)).size()>0);
    REQUIRE_THROWS(f.editing->cells->locate(1000,Vec3d(1,0,0)));
    REQUIRE_THROWS(f.editing->cells->mesh(its_make_cube(1,1,1)));
    SurfaceSelectionPersistence::SelectionState state;
    state.selected.assign(f.editing->size(),1);f.editing->constrain(state,true);
    REQUIRE(state.selected[iris]==0);
    REQUIRE(state.selected[white]==0);
}

TEST_CASE("Three to six portrait colors keep both brows dark and eye white light", "[BeautyCellDomain][PortraitShapeDetails]") {
    Cells f;
    const size_t count=GENERATE(3u,4u,5u,6u);
    std::vector<SemanticColoring::Color> palette{{.8f,.6f,.5f},{40.f/255,38.f/255,41.f/255},
        {246.f/255,247.f/255,249.f/255},{.8f,.2f,.4f},{.2f,.3f,.5f},{.5f,.5f,.5f}};
    palette.resize(count);
    std::vector<std::string> roles(GUI::portrait_role_names.begin(),GUI::portrait_role_names.begin()+count);
    const auto colors=GUI::portrait_contour_colors(f.partition,roles,palette);
    for(const auto& face:f.partition.at("faces")) for(const auto& cell:face.at("cells")) {
        const auto label=cell.at("label").get<std::string>();
        if(label=="le") REQUIRE(colors.at(cell.at("id"))==palette[2]);
        if(label=="lb" || label=="rb" || label=="iris-le") REQUIRE(colors.at(cell.at("id"))==palette[1]);
        if(label=="face") REQUIRE(colors.count(cell.at("id"))==0);
    }
    REQUIRE(GUI::portrait_contour_colors(f.partition,{},palette).empty());
    roles[1]="";
    std::set<std::string> missing_roles;
    const auto missing=GUI::portrait_contour_colors(f.partition,roles,palette,&missing_roles);
    REQUIRE(missing_roles==std::set<std::string>{"portrait-dark"});
    for(const auto& face:f.partition.at("faces")) for(const auto& cell:face.at("cells"))
        if(cell.at("label")=="lb" || cell.at("label")=="rb") REQUIRE(missing.count(cell.at("id"))==0);
}

TEST_CASE("Cell colors and exact selections restore without changing boundary identity", "[BeautyCellDomain][BeautyShapeLock]") {
    Cells f;SurfaceSelectionPersistence::SelectionState state;
    state.selected.assign(f.editing->size(),0);
    const auto triangle=f.editing->cells->locate(0,Vec3d(.5,.25,.25));
    state.selected[triangle]=1;f.editing->constrain(state);
    const auto id=f.editing->cells->cell_id(triangle);
    std::map<std::string,SemanticColoring::Color> colors{{id,{1,0,0}},{"source:999",{0,1,0}}};
    const auto saved=BeautyCellEdits::encode(*f.editing,colors,state);
    const auto restored=BeautyCellEdits::decode(saved,*f.editing);
    REQUIRE(restored.colors==colors);
    REQUIRE(restored.selection.selected==state.selected);
    auto bad=saved;bad["mapping_sha256"]=std::string(64,'a');
    REQUIRE_THROWS(BeautyCellEdits::decode(bad,*f.editing));
    const auto before=f.lock.fingerprint(f.partition);
    auto relocated=f.lock;relocated.document["partition_ref"]["sha256"]=std::string(64,'9');
    REQUIRE(relocated.fingerprint(f.partition)==before);
    auto changed=f.partition;changed["geometry_id"]=std::string(64,'9');
    REQUIRE_THROWS(BeautyCellDomain::build(changed,BeautySurfaceShapeLock::identity(f.partition)));
}

TEST_CASE("Unlocked contour selection and painting share complete polygon cells", "[BeautyCellDomain][BeautyLeafEditing]") {
    Cells f;
    const auto& triangles=f.editing->cells->triangles;
    const auto found=std::find_if(triangles.begin(),triangles.end(),[&](const auto& triangle) {
        return !triangle.cell_id.empty() && f.editing->cells->indices(triangle.cell_id).size()>1;
    });
    REQUIRE(found!=triangles.end());
    const auto cell=f.editing->cells->indices(found->cell_id);
    SurfaceSelectionPersistence::SelectionState state;
    state.selected.assign(f.editing->size(),0);
    state.foreground=state.domain=state.protected_faces=state.selected;
    state.selected[cell.front()]=1;
    f.editing->normalize_cells(state);
    REQUIRE(std::count(state.selected.begin(),state.selected.end(),1)==cell.size());
    for(auto i:cell) {
        REQUIRE(state.selected[i]==1);
        REQUIRE(state.foreground[i]==1);
        REQUIRE(state.domain[i]==1);
    }
    state.protected_faces[cell.back()]=1;
    f.editing->normalize_cells(state);
    REQUIRE(std::count(state.selected.begin(),state.selected.end(),1)==0);
}

TEST_CASE("Contour generation cannot grant a detail through an unproved fallback", "[BeautyCellDomain][LocalSemanticEvidence]") {
    Cells f;GUI::LocalSemanticEvidence::Evidence evidence;
    const auto& identity=f.request.at("identity");
    evidence.identity.source_sha256=identity.at("source_sha256");evidence.identity.geometry_id=identity.at("geometry_id");
    evidence.identity.runtime_sha256=identity.at("runtime_sha256");evidence.identity.policy_sha256=identity.at("policy_sha256");
    evidence.identity.face_count=f.source.indices.size();evidence.shape_details=f.shapes;
    for(const auto& shape:f.shapes) {
        PrintColorRegion region;
        region.subject_id=shape.subject_id;region.label=shape.label;region.faces=shape.accepted_faces;
        evidence.regions.push_back(region);
    }
    const auto hash=identity.at("evidence_sha256").get<std::string>();
    REQUIRE_NOTHROW(GUI::build_verified_contour_partition(f.request,evidence,f.source,hash));
    auto bad=f.request;bad["faces"][1]["base"][0]["label"]="rb";
    REQUIRE_THROWS(GUI::build_verified_contour_partition(bad,evidence,f.source,hash));
    bad=f.request;bad["faces"][1]["base"][0]["subject_id"]="other-person";
    REQUIRE_THROWS(GUI::build_verified_contour_partition(bad,evidence,f.source,hash));
    bad=f.request;bad["faces"][0]["base"][0]["label"]="iris-le";
    REQUIRE_THROWS(GUI::build_verified_contour_partition(bad,evidence,f.source,hash));
}

TEST_CASE("Contour caches reopen exact boundaries colors and risk status with safe references", "[BeautyCellDomain][PortraitShapeDetails]") {
    Cells f;ScopedTemporaryDir directory("portrait-contour-cache");
    const auto source=directory.path()/"source.glb";std::string error;
    REQUIRE(write_model_artifact(source,f.source,std::vector<RGBA>(f.source.vertices.size(),RGBA{1,1,1,1}),error));
    const auto source_hash=model_artifact_sha256(source);
    namespace Cache=GUI::PortraitShapeCache;
    REQUIRE_FALSE(source_hash.empty());
    Cache::preserve_source(directory.path(),source,source_hash);
    f.request["identity"]["source_sha256"]=source_hash;
    f.partition=SP::build(f.request);
    f.lock=BeautySurfaceShapeLock::from_partition(f.partition,f.shapes,beauty_leaf_digest(f.partition.dump()));
    GUI::PortraitShapeDetails details;
    const auto& identity=f.request.at("identity");
    details.locks.geometry_id=identity.at("geometry_id");details.locks.source_sha256=source_hash;
    details.locks.evidence_sha256=identity.at("evidence_sha256");details.locks.runtime_sha256=identity.at("runtime_sha256");
    details.locks.policy_sha256=identity.at("policy_sha256");details.locks.face_count=f.source.indices.size();
    details.base_colors.assign(f.source.indices.size(),RGBA{1,1,1,1});
    details.runtime_fingerprint=std::string(64,'d');details.subjects={"person"};
    details.surface_partition=std::make_shared<Json>(f.partition);
    details.contour_locks=std::make_shared<BeautySurfaceShapeLock>(f.lock);
    details.cell_colors=GUI::portrait_contour_colors(f.partition,{"portrait-skin","portrait-dark","portrait-light"},
        {{.8f,.6f,.5f},{.1f,.1f,.1f},{.97f,.97f,.98f}});
    const auto reference=Cache::save(details,directory.path());
    const auto reopened=Cache::load(reference,directory.path(),details.locks.geometry_id,details.locks.face_count,details.runtime_fingerprint);
    REQUIRE(reopened->boundary_fingerprint()==details.boundary_fingerprint());
    REQUIRE(reopened->cell_colors==details.cell_colors);
    REQUIRE(reopened->contour_locks->document.at("locks")[0].at("status")=="PROTECTED_SHAPE_UNCERTAIN");
    REQUIRE(Cache::save(*reopened,directory.path())==reference);
    auto document=Cache::read_addressed(directory.path(),"portrait-shapes","orca.portrait-shape-reference/v1",reference);
    document["surface_partition_ref"]["path"]="../outside.json";
    const auto invalid=Cache::write_addressed(directory.path(),"portrait-shapes","orca.portrait-shape-reference/v1",document);
    REQUIRE_THROWS(Cache::load(invalid,directory.path(),details.locks.geometry_id,details.locks.face_count,details.runtime_fingerprint));
}

TEST_CASE("Contour rendering remains inside the common cumulative budget", "[BeautyCellDomain][ModelSemanticColoring]") {
    Cells f;SemanticColoring::MeshSnapshot source;
    source.mesh=f.source;source.geometry_id=SurfaceSelectionPersistence::geometry_fingerprint(f.source);
    source.vertex_colors.assign(f.source.vertices.size(),RGBA{1,1,1,1});
    const auto colors=GUI::portrait_contour_colors(f.partition,{"portrait-skin","portrait-dark","portrait-light"},
        {{.8f,.6f,.5f},{.1f,.1f,.1f},{.97f,.97f,.98f}});
    const auto geometry=GUI::build_semantic_colored_geometry(source,{}, {}, {}, &f.partition,&colors);
    REQUIRE_FALSE(geometry.is_empty());
    REQUIRE(geometry.indices_count()/3<=f.source.indices.size()+20);
    REQUIRE(geometry.indices_count()/3==f.editing->size());
}
