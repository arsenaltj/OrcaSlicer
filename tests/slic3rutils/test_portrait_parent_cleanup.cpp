#include "slic3r/GUI/AI/ModelGeneration/PortraitParentCleanup.hpp"
#include "slic3r/GUI/AI/ModelGeneration/PortraitParentCleanupLive.hpp"
#include "slic3r/GUI/AI/ModelGeneration/PortraitParentCoverage.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelSemanticColoring.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "slic3r/GUI/AI/Model/BeautyAppearance.hpp"
#include "slic3r/GUI/AI/Model/BeautyLeafEdits.hpp"
#include "libslic3r/Utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <chrono>
#include <thread>

namespace Cleanup = Slic3r::GUI::PortraitParentCleanup;
namespace AI = Slic3r::AI;
namespace SP = AI::SurfacePartition;
using Json = nlohmann::json;

namespace {
struct LiveFixture {
    AI::SemanticColoring::MeshSnapshot source;
    AI::SemanticColoring::Analysis analysis;
    Slic3r::GUI::PortraitShapeDetails details;
    AI::SemanticColoring::FaceColors inherited;
    std::vector<std::string> roles{"portrait-skin","portrait-dark","portrait-light","portrait-lips","portrait-cool","portrait-mid"};
    std::vector<Cleanup::Color> palette{{.92f,.76f,.70f},{.16f,.15f,.16f},{.965f,.969f,.976f},
        {.91f,.60f,.57f},{.40f,.55f,.71f},{.58f,.54f,.53f}};
    std::string skin;
    LiveFixture() {
        for(int y=0;y<=8;++y)for(int x=0;x<=8;++x)source.mesh.vertices.push_back({float(x),float(y),0});
        for(int y=0;y<8;++y)for(int x=0;x<8;++x) {
            const int a=y*9+x;source.mesh.indices.push_back({a,a+1,a+9});source.mesh.indices.push_back({a+1,a+10,a+9});
        }
        source.geometry_id=AI::SurfaceSelectionPersistence::geometry_fingerprint(source.mesh);
        analysis.geometry_id=source.geometry_id;analysis.face_labels.assign(128,AI::SemanticColoring::Label::Unknown);
        analysis.face_confidence.assign(128,0);
        const Json identity{{"geometry_id",source.geometry_id},{"source_sha256",std::string(64,'b')},
            {"evidence_sha256",std::string(64,'c')},{"runtime_sha256",std::string(64,'d')},
            {"policy_sha256",std::string(64,'e')},{"baseline_sha256",std::string(64,'f')},
            {"boundary_policy_sha256",std::string(64,'1')},{"face_count",128}};
        const Json base{{"polygon",{{1.,0.,0.},{0.,1.,0.},{0.,0.,1.}}},{"holes",Json::array()},
            {"label","face"},{"parent_label","face"},{"subject_id","person"},{"kind","SOURCE_PARENT"}};
        const Json corner{{"polygon",{{1.,0.,0.},{.5,.5,0.},{.5,0.,.5}}},{"holes",Json::array()}};
        const Json views=Json::array({{{"family","front"},{"polygons",Json::array({corner})}},
                                     {{"family","oblique"},{"polygons",Json::array({corner})}}});
        auto partition=SP::build({{"schema","orca.surface-partition-request/v1"},{"identity",identity},
            {"triangle_budget",2},{"existing_added_triangles",0},{"faces",Json::array({{
                {"source_face_id",size_t(0)},{"baseline_triangle_count",1},{"base",Json::array({base})},
                {"layers",Json::array({{{"label","le"},{"parent_label","le"},{"subject_id","person"},{"views",views}}})}}})}});
        Slic3r::GUI::LocalSemanticEvidence::ShapeDetail shape;shape.subject_id="person";shape.label="le";
        shape.status="PROTECTED_SHAPE_UNCERTAIN";shape.view_support=2;shape.accepted_faces={0};
        const auto hash=AI::beauty_leaf_digest(partition.dump());
        details.surface_partition=std::make_shared<Json>(partition);
        details.contour_locks=std::make_shared<AI::BeautySurfaceShapeLock>(
            AI::BeautySurfaceShapeLock::from_partition(partition,{shape},hash));
        details.locks.geometry_id=source.geometry_id;details.locks.source_sha256=identity.at("source_sha256");
        details.locks.face_count=128;details.locks.evidence_sha256=identity.at("evidence_sha256");
        details.locks.runtime_sha256=identity.at("runtime_sha256");details.locks.policy_sha256=identity.at("policy_sha256");
        details.subjects={"person"};details.base_colors.assign(128,{.8f,.65f,.58f,1});
        for(const auto& cell:partition.at("faces")[0].at("cells"))if(cell.at("label")=="face")skin=cell.at("id");
        REQUIRE_FALSE(skin.empty());
        details.parent_source_colors[skin]={.8f,.65f,.58f};
    }
    void sample(size_t face,const std::string& label="face",double confidence=.99,size_t views=2) {
        details.parent_samples.push_back(Json::array({face,"person",label,confidence,views,views}));
    }
    Cleanup::Result run(const AI::SemanticColoring::FaceColors& manual={},
                        const std::map<std::string,Cleanup::Color>& cells={}) {
        return Cleanup::Live::colors(details,source,analysis,roles,palette,inherited,manual,cells);
    }
};

struct Fixture {
    boost::filesystem::path root = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("parent-cleanup-%%%%-%%%%");
    Cleanup::Bundle bundle;
    std::string skin;
    Fixture() {
        const Json identity{{"geometry_id",std::string(64,'a')},{"source_sha256",std::string(64,'b')},
            {"evidence_sha256",std::string(64,'c')},{"runtime_sha256",std::string(64,'d')},
            {"policy_sha256",std::string(64,'e')},{"baseline_sha256",std::string(64,'f')},
            {"boundary_policy_sha256",std::string(64,'1')},{"face_count",10000}};
        const Json base{{"polygon",{{1,0,0},{0,1,0},{0,0,1}}},{"holes",Json::array()},
            {"label","skin"},{"parent_label","face"},{"subject_id","person"},{"kind","R6_PARENT"}};
        const Json strip{{"polygon",{{.65,.25,.10},{.64,.26,.10},{.14,.26,.60},{.15,.25,.60}}},{"holes",Json::array()}};
        const Json views=Json::array({{{"family","front"},{"polygons",Json::array({strip})}},
                                     {{"family","oblique"},{"polygons",Json::array({strip})}}});
        auto partition = SP::build({{"schema","orca.surface-partition-request/v1"},{"identity",identity},
            {"triangle_budget",200},{"existing_added_triangles",0},{"faces",Json::array({{
                {"source_face_id",size_t(1)},{"baseline_triangle_count",1},{"base",Json::array({base})},
                {"layers",Json::array({{{"label","le"},{"parent_label","le"},{"subject_id","person"},{"views",views}}})}}})}});
        const auto file_hash=write("partition.json",partition);
        Slic3r::GUI::LocalSemanticEvidence::ShapeDetail shape;
        shape.subject_id="person"; shape.label="le"; shape.status="PROTECTED_SHAPE_UNCERTAIN";
        shape.view_support=2; shape.accepted_faces={1};
        auto locks=AI::BeautySurfaceShapeLock::from_partition(partition,{shape},file_hash);
        bundle.partition=std::make_shared<Json>(partition);
        bundle.locks=std::make_shared<AI::BeautySurfaceShapeLock>(locks);
        for(const auto& cell:partition.at("faces")[0].at("cells")) if(cell.at("label")=="skin") skin=cell.at("id");
        REQUIRE_FALSE(skin.empty());
        bundle.candidate={{"schema","orca.portrait-parent-cleanup-candidate/v1"},
            {"source_sha256",identity.at("source_sha256")},{"geometry_id",identity.at("geometry_id")},{"face_count",10000},
            {"partition_sha256",partition.at("partition_sha256")},{"shape_lock_fingerprint",locks.fingerprint(partition)},
            {"detail_freeze_sha256",std::string(64,'2')},{"runtime_sha256",std::string(64,'3')},{"policy_sha256",std::string(64,'4')},
            {"boundary_runtime_sha256",identity.at("runtime_sha256")},{"boundary_policy_sha256",identity.at("boundary_policy_sha256")},
            {"partition_path","partition.json"},{"partition_file_sha256",file_hash},
            {"lock_path","locks.json"},{"lock_sha256",write("locks.json",locks.document)},
            {"frozen_boundary_sha256",Cleanup::frozen_boundary(partition,locks)},
            {"cells",Json::array({Json::array({skin,"portrait-skin","skin",size_t(1),false,"person"}),
                Json::array({Cleanup::root_cell_id(partition,2),"portrait-skin","skin",size_t(2),true,"person"}),
                Json::array({Cleanup::root_cell_id(partition,3),"portrait-mid","cloth",size_t(3),true,"person"})})}};
        publish();
    }
    ~Fixture() { boost::system::error_code ignored; boost::filesystem::remove_all(root,ignored); }
    std::string write(const std::string& name,const Json& value) {
        boost::filesystem::create_directories(root);
        boost::filesystem::ofstream stream(root/name,std::ios::binary); stream<<value.dump(); stream.close();
        return Cleanup::read_hash(root/name);
    }
    void publish() {
        auto entry=bundle.candidate; entry.erase("schema"); entry.erase("cells");
        entry["path"]="candidate.json"; entry["sha256"]=write("candidate.json",bundle.candidate);
        write("portrait_parent_cleanup_catalog.json",{{"schema","orca.portrait-parent-cleanup-catalog/v1"},{"candidates",Json::array({entry})}});
    }
    Cleanup::Bundle load() const { return Cleanup::load_bundle(root,std::string(64,'b'),std::string(64,'a'),10000); }
};
const std::vector<std::string> roles{"portrait-skin","portrait-dark","portrait-light"};
const std::vector<Cleanup::Color> palette{{.8f,.6f,.5f},{.1f,.1f,.1f},{.97f,.97f,.98f}};
}

TEST_CASE("Live parent rules use verified current-source observations instead of a reviewed catalog", "[PortraitParentCleanup]") {
    LiveFixture f;f.sample(0);f.sample(1,"body-skin");f.sample(2,"neck");
    f.details.base_colors[2]={.24f,.13f,.10f,1}; // Warm/dark skin is still confirmed skin.
    const auto partition=*f.details.surface_partition;
    const auto frozen=Cleanup::frozen_boundary(partition,*f.details.contour_locks);
    for(size_t count=3;count<=6;++count) {
        f.roles.resize(count);f.palette.resize(count);
        const auto result=f.run();INFO(result.reason);
        REQUIRE(result.applied==3);
        REQUIRE(result.cell_colors.at(f.skin)==f.palette[0]);
        REQUIRE(result.root_colors.at(1)==f.palette[0]);REQUIRE(result.root_colors.at(2)==f.palette[0]);
        REQUIRE(result.audit.at("rule_counts").at("UNIFIED_CONFIRMED_BODY_SKIN")==3);
        REQUIRE(Cleanup::frozen_boundary(*f.details.surface_partition,*f.details.contour_locks)==frozen);
        for(const auto& id:f.details.contour_locks->document["locks"][0]["locked_cells"])
            REQUIRE(result.cell_colors.count(id.get<std::string>())==0);
        // Restore the role vector for the next color count.
        f.roles={"portrait-skin","portrait-dark","portrait-light","portrait-lips","portrait-cool","portrait-mid"};
        f.palette={{.92f,.76f,.70f},{.16f,.15f,.16f},{.965f,.969f,.976f},
            {.91f,.60f,.57f},{.40f,.55f,.71f},{.58f,.54f,.53f}};
    }
}

TEST_CASE("Live parent composition repairs a mixed-face skin cell without painting its frozen eye", "[PortraitParentCleanup]") {
    LiveFixture f;f.sample(0);f.details.cell_colors[f.skin]=f.palette[3];
    const auto result=f.run();REQUIRE(result.root_colors.count(0)==0);
    REQUIRE(result.cell_colors.size()==1);REQUIRE(result.cell_colors.at(f.skin)==f.palette[0]);
    const auto manual=f.run({},{{f.skin,f.palette[4]}});
    REQUIRE(manual.cell_colors.empty());REQUIRE(manual.cell_labels.at(f.skin)=="skin");
}

TEST_CASE("Clipped parent colors use their own source samples and report actual inherited changes", "[PortraitParentCleanup]") {
    LiveFixture f;f.sample(0);f.sample(1);f.sample(2);
    f.details.cell_colors[f.skin]=f.palette[3];f.inherited={{1,f.palette[0]}};
    const auto result=f.run();REQUIRE(result.audit.at("known_assignment_changed_units")==1);
    REQUIRE(result.audit.at("already_same_assignment_units")==1);
    REQUIRE(result.audit.at("no_old_slot_units")==1);
    f.details.parent_source_colors.clear();
    const auto missing=f.run();REQUIRE(missing.cell_colors.empty());
    REQUIRE(missing.audit.at("preserved_reasons").at("CLIPPED_UNIT_SOURCE_COLOR_UNAVAILABLE")==1);
}

TEST_CASE("Live parent rules preserve single-view conflicts missing roles and manual colors", "[PortraitParentCleanup]") {
    LiveFixture f;f.sample(1);
    SECTION("One view") {f.details.parent_samples[0][5]=1;}
    SECTION("Low confidence") {f.details.parent_samples[0][3]=.92;}
    SECTION("Another subject") {f.details.parent_samples[0][1]="another";}
    SECTION("Mixed hair child") {f.analysis.subface_labels.push_back({1,{1,0},AI::SemanticColoring::Label::Hair,.99f,2});}
    SECTION("Whole face accessory") {f.analysis.face_labels[1]=AI::SemanticColoring::Label::Accessories;f.analysis.face_confidence[1]=.99f;}
    SECTION("Frozen diagnostic face") {f.details.reserved_faces={1};}
    SECTION("Missing skin role") {f.roles[0]="";}
    SECTION("Manual face") {REQUIRE(f.run({{1,f.palette[4]}}).root_colors.empty());return;}
    REQUIRE(f.run().applied==0);
}

TEST_CASE("Live neutral clothing follows connected source shadows while preserving pigment and dark details", "[PortraitParentCleanup]") {
    LiveFixture f;
    for(size_t face:{size_t(1),size_t(2),size_t(3),size_t(4)})f.sample(face,"cloth");
    f.details.base_colors[1]={.7f,.7f,.7f,1};f.details.base_colors[2]={.56f,.56f,.56f,1};
    f.details.base_colors[3]={.9f,.1f,.2f,1};f.details.base_colors[4]={.01f,.01f,.01f,1};
    const auto result=f.run();INFO(result.reason);
    REQUIRE(result.root_colors.at(1)==f.palette[2]);REQUIRE(result.root_colors.at(2)==f.palette[2]);
    REQUIRE(result.root_colors.count(3)==0);REQUIRE(result.root_colors.count(4)==0);
    REQUIRE(result.audit.at("rule_counts").at("COL009_CONNECTED_NEUTRAL_CLOTH_SHADOW")==1);
}

TEST_CASE("Live cleanup uses an immutable donor set and never connects materials by a vertex", "[PortraitParentCleanup]") {
    LiveFixture f;
    for(size_t face:{size_t(1),size_t(2),size_t(3)})f.sample(face);
    f.inherited={{1,f.palette[0]},{2,f.palette[3]},{3,f.palette[3]}};
    const auto result=f.run();REQUIRE(result.audit.at("fixed_donors")==1);
    REQUIRE(result.audit.at("supported_island_units")==0);
    REQUIRE(result.audit.at("uniform_units")==3);
    std::vector<Cleanup::Live::Unit> units(2);
    for(auto& u:units) {u.subject="person";u.parent="skin";
        u.polygon={{"polygon",{{1.,0.,0.},{0.,1.,0.},{0.,0.,1.}}},{"holes",Json::array()}};}
    units[0].face=2;units[1].face=4; // Neighboring grid corners, no common edge.
    Cleanup::Live::connect(units,f.source.mesh);
    REQUIRE(units[0].neighbors.empty());
}

TEST_CASE("Live clothing uses body confidence without lowering facial confidence", "[PortraitParentCleanup]") {
    LiveFixture f;f.sample(1,"cloth",.93);f.sample(2,"face",.93);f.sample(3,"body-skin",.93);
    f.details.base_colors[1]={.7f,.7f,.7f,1};
    const auto result=f.run();INFO(result.reason);
    REQUIRE(result.root_colors.at(1)==f.palette[2]);
    REQUIRE(result.root_colors.count(2)==0);
    REQUIRE(result.root_colors.at(3)==f.palette[0]);
}

TEST_CASE("Connected clothing shadows are counted once per expansion ring", "[PortraitParentCleanup]") {
    LiveFixture f;
    for(size_t face:{size_t(16),size_t(17),size_t(18),size_t(70)})f.sample(face,"cloth",.93);
    f.details.base_colors[16]={.7f,.7f,.7f,1};f.details.base_colors[18]={.7f,.7f,.7f,1};
    f.details.base_colors[17]={.56f,.56f,.56f,1};f.details.base_colors[70]={.56f,.56f,.56f,1};
    const auto result=f.run();INFO(result.reason);
    REQUIRE(result.root_colors.size()==3);
    REQUIRE(result.root_colors.count(70)==0);
    REQUIRE(result.audit.at("rule_counts").at("COL009_CONNECTED_NEUTRAL_CLOTH_SHADOW")==1);
    REQUIRE(result.audit.at("preserved_reasons").at("NEUTRAL_SHADOW_REQUIRES_CONNECTED_CORE")==1);
}

TEST_CASE("Live parent rules reject identity drift and cancellation without publishing colors", "[PortraitParentCleanup]") {
    LiveFixture f;f.sample(1);
    SECTION("Geometry") {f.source.geometry_id=std::string(64,'0');REQUIRE(f.run().applied==0);}
    SECTION("Face count") {f.source.mesh.indices.pop_back();REQUIRE(f.run().applied==0);}
    SECTION("Source") {f.details.locks.source_sha256=std::string(64,'0');REQUIRE(f.run().applied==0);}
    SECTION("Evidence") {f.details.locks.evidence_sha256=std::string(64,'0');REQUIRE(f.run().applied==0);}
    SECTION("Frozen policy") {f.details.contour_locks=std::make_shared<AI::BeautySurfaceShapeLock>(*f.details.contour_locks);
        auto locks=*f.details.contour_locks;locks.document["policy_sha256"]=std::string(64,'0');
        f.details.contour_locks=std::make_shared<AI::BeautySurfaceShapeLock>(locks);REQUIRE(f.run().applied==0);}
    SECTION("Cancellation") {
        const auto result=Cleanup::Live::colors(f.details,f.source,f.analysis,f.roles,f.palette,{}, {}, {},[]{return true;});
        REQUIRE(result.reason=="parent_cleanup_cancelled");REQUIRE(result.root_colors.empty());
    }
}

namespace {
Json current_proposal(LiveFixture& f) {
    Json binding={{"source_sha256",f.details.locks.source_sha256},{"geometry_id",f.source.geometry_id},{"face_count",128},
        {"evidence_sha256",std::string(64,'2')},{"runtime_sha256",std::string(64,'3')},{"policy_sha256",std::string(64,'4')}};
    f.details.parent_evidence_identity=binding;f.details.parent_evidence_identity["host_runtime_fingerprint"]=std::string(64,'5');
    const Json policy={{"version","coverage-test"},{"root_coverage",1.},{"coverage_tolerance",1e-10}};
    return {{"schema","orca.portrait-parent-proposal/v1"},{"identity",binding},{"policy",policy},
        {"proposal_policy_sha256",AI::beauty_leaf_digest(policy.dump())},{"subject_id","person"},
        {"roots",Json::array({Json::array({1,3,Json::array({0,1}),Json::array({.24,.13,.1})})})},
        {"mixed",Json::array()},{"color_barycentric",AI::parent_visibility_samples},{"curve_library",Json::object()},
        {"cameras",Json::array({{{"family","front"},{"direction",{0.,0.,1.}},{"distance",40.}},
            {{"family","oblique"},{"direction",{.6,0.,.8}},{"distance",40.}}})},
        {"scope_faces",Json::array({0,1,16})},{"audit",Json::object()}};
}
}

TEST_CASE("Current parent coverage proves independent source visibility and preserves frozen detail cells", "[PortraitParentCleanup][ParentSurfaceVisibility]") {
    LiveFixture f;const auto before=*f.details.surface_partition;
    auto proposal=current_proposal(f);
    SECTION("Subpixel root needs no prior raster sample") {
        f.details.parent_proposal=std::make_shared<Json>(proposal);
        Slic3r::GUI::PortraitParentCoverage::apply(f.details,f.source,f.analysis,{}, {},{});
        REQUIRE(f.details.parent_verified_roots.at(1)=="skin");
        REQUIRE(f.run().root_colors.at(1)==f.palette[0]);
        REQUIRE(*f.details.surface_partition==before);
    }
    SECTION("Older parent summaries cannot bypass incomplete root coverage") {
        f.sample(2);
        f.details.parent_proposal=std::make_shared<Json>(proposal);
        Slic3r::GUI::PortraitParentCoverage::apply(f.details,f.source,f.analysis,{}, {},{});
        const auto result=f.run();
        REQUIRE(result.root_colors.count(1)==1);
        REQUIRE(result.root_colors.count(2)==0);
        REQUIRE(result.audit.at("preserved_reasons").at("UNCONFIRMED_WHOLE_ROOT_PRESERVED")==1);
    }
    SECTION("Correlated camera directions are not independent") {
        proposal["cameras"][1]["direction"]=proposal["cameras"][0]["direction"];
        f.details.parent_proposal=std::make_shared<Json>(proposal);
        Slic3r::GUI::PortraitParentCoverage::apply(f.details,f.source,f.analysis,{}, {},{});
        REQUIRE(f.details.parent_verified_roots.empty());
    }
    SECTION("Source drift") {proposal["identity"]["source_sha256"]=std::string(64,'0');
        REQUIRE_THROWS(Slic3r::GUI::PortraitParentCoverage::validate(proposal,f.details));}
    SECTION("Another subject") {proposal["subject_id"]="other";
        REQUIRE_THROWS(Slic3r::GUI::PortraitParentCoverage::validate(proposal,f.details));}
    SECTION("Partial root policy cannot grant whole-face authority") {
        proposal["policy"]["root_coverage"]=.98;
        proposal["proposal_policy_sha256"]=AI::beauty_leaf_digest(proposal.at("policy").dump());
        REQUIRE_THROWS(Slic3r::GUI::PortraitParentCoverage::validate(proposal,f.details));
    }
    SECTION("Actual mesh identity drift") {
        for(size_t i=0;i<3;++i)f.source.mesh.vertices.push_back(f.source.mesh.vertices[f.source.mesh.indices[1][i]]+Slic3r::Vec3f(0,0,.001f));
        const auto start=int(f.source.mesh.vertices.size())-3;f.source.mesh.indices.push_back({start,start+1,start+2});
        // Visibility is tested against the actual mesh, not echoed packet fields.
        f.details.parent_proposal=std::make_shared<Json>(proposal);
        REQUIRE_THROWS(Slic3r::GUI::PortraitParentCoverage::apply(f.details,f.source,f.analysis,{}, {},{}));
    }
    SECTION("Manual root") {
        f.details.parent_proposal=std::make_shared<Json>(proposal);
        Slic3r::GUI::PortraitParentCoverage::apply(f.details,f.source,f.analysis,{{1,f.palette[4]}}, {},{});
        REQUIRE(f.details.parent_verified_roots.empty());
    }
}

TEST_CASE("Mixed current parent proposals clip siblings locally without granting whole-root coloring", "[PortraitParentCleanup][SurfacePartition]") {
    LiveFixture f;auto proposal=current_proposal(f);proposal["roots"]=Json::array();
    const Json half{{"polygon",{{1.,0.,0.},{0.,.5,.5},{0.,0.,1.}}},{"holes",Json::array()}};
    Json views=Json::array({{{"family","front"},{"camera",0},{"polygons",Json::array({half})}},
        {{"family","oblique"},{"camera",1},{"polygons",Json::array({half})}}});
    proposal["mixed"]=Json::array({{{"source_face_id",16},{"source_samples",Json::array()},
        {"color_samples",Json::array()},{"layers",Json::array({{{"label","skin"},{"parent_label","skin"},
            {"subject_id","person"},{"views",views}}})}}});
    for(size_t i=0;i<7;++i){proposal["mixed"][0]["source_samples"].push_back({.8,.65,.58});proposal["mixed"][0]["color_samples"].push_back({.8,.65,.58});}
    SECTION("Budget exhausted retains only the mixed root") {
        f.details.parent_proposal=std::make_shared<Json>(proposal);
        const auto before=*f.details.surface_partition;
        Slic3r::GUI::PortraitParentCoverage::apply(f.details,f.source,f.analysis,{}, {},{});
        REQUIRE(f.details.parent_verified_roots.empty());
        REQUIRE(f.details.parent_coverage_audit.at("mixed_applied_roots")==0);
        REQUIRE(f.details.surface_partition->at("added_triangles")==before.at("added_triangles"));
    }
    SECTION("Independent supported portion can be cut with available budget") {
        auto identity=AI::BeautySurfaceShapeLock::identity(*f.details.surface_partition);
        Json base{{"polygon",{{1.,0.,0.},{0.,1.,0.},{0.,0.,1.}}},{"holes",Json::array()},
            {"label","le"},{"parent_label","le"},{"subject_id","person"}};
        auto partition=SP::build({{"schema","orca.surface-partition-request/v1"},{"identity",identity},{"triangle_budget",2},
            {"existing_added_triangles",0},{"faces",Json::array({{{"source_face_id",0},{"baseline_triangle_count",1},
                {"base",Json::array({base})},{"layers",Json::array()}}})}});
        Slic3r::GUI::LocalSemanticEvidence::ShapeDetail shape;shape.label="le";shape.subject_id="person";
        shape.status="VALID_SHAPE";shape.accepted_faces={0};shape.view_support=2;
        f.details.surface_partition=std::make_shared<Json>(partition);
        f.details.contour_locks=std::make_shared<AI::BeautySurfaceShapeLock>(AI::BeautySurfaceShapeLock::from_partition(partition,{shape},AI::beauty_leaf_digest(partition.dump())));
        const auto frozen=f.details.contour_locks->document.at("locks")[0].at("locked_cells");
        f.details.parent_proposal=std::make_shared<Json>(proposal);
        Slic3r::GUI::PortraitParentCoverage::apply(f.details,f.source,f.analysis,{}, {},{});
        INFO(f.details.parent_coverage_audit.dump());
        REQUIRE(f.details.parent_coverage_audit.at("mixed_applied_roots")==1);
        REQUIRE(SP::preserves_frozen_cells(partition,*f.details.surface_partition,frozen));
        const auto result=f.run();REQUIRE(result.root_colors.count(16)==0);REQUIRE_FALSE(result.cell_colors.empty());
        for(const auto& row:f.details.surface_partition->at("faces"))if(row.at("source_face_id")==16)
            for(const auto& cell:row.at("cells"))if(cell.at("label")=="R6")REQUIRE(result.cell_colors.count(cell.at("id"))==0);
    }
}

TEST_CASE("Reviewed parent candidate colors explicit cells and implicit roots independently", "[PortraitParentCleanup]") {
    Fixture f; const auto bundle=f.load(); REQUIRE(bundle.partition); REQUIRE(bundle.locks);
    const auto result=Cleanup::colors(bundle,roles,palette);
    REQUIRE(result.reason=="candidate_applied"); REQUIRE(result.applied==2);
    REQUIRE(result.cell_colors.size()==1); REQUIRE(result.root_colors.size()==1);
    REQUIRE(result.cell_colors.at(f.skin)==palette[0]); REQUIRE(result.root_colors.at(2)==palette[0]);
    auto six_roles=roles; six_roles.insert(six_roles.end(),{"portrait-lips","portrait-cool","portrait-mid"});
    auto six=palette; six.insert(six.end(),{{.9f,.5f,.6f},{.3f,.4f,.5f},{.5f,.5f,.5f}});
    REQUIRE(Cleanup::colors(bundle,six_roles,six).applied==3);
}

TEST_CASE("Parent repair removes inherited subface colors only on confirmed pure roots", "[PortraitParentCleanup]") {
    Fixture f; const auto repair=Cleanup::colors(f.load(),roles,palette);
    AI::SemanticColoring::FaceColors faces{{2,{1,0,0}},{4,{1,0,0}}};
    AI::SemanticColoring::SubfaceColors children{{2,{0}, {1,0,0},1.f},{4,{0},{1,0,0},1.f}};
    Cleanup::apply_roots(repair,faces,children);
    REQUIRE(faces[0].second==palette[0]); REQUIRE(faces[1].second==Cleanup::Color{1,0,0});
    REQUIRE(children.size()==1); REQUIRE(children[0].face_id==4);
    const auto composed=AI::SemanticColoring::compose(faces,{{2,{0,1,0}}},true);
    REQUIRE(composed[0].second==Cleanup::Color{0,1,0});
}

TEST_CASE("Reviewed parent ownership binds historical labels without changing the partition", "[PortraitParentCleanup]") {
    Fixture f;
    const auto original=*f.bundle.partition;
    auto relabeled=original;
    for (auto& cell:relabeled["faces"][0]["cells"]) if(cell.at("id")==f.skin) {
        cell["label"]="face";
        cell["parent_label"]="face";
    }
    f.bundle.partition=std::make_shared<Json>(relabeled);
    REQUIRE(Cleanup::colors(f.bundle,roles,palette).applied==0);
    f.bundle.candidate["ownership_relabels"]={{f.skin,{{"source_label","face"},
        {"source_parent_label","face"},{"target_label","skin"}}}};
    const auto repair=Cleanup::colors(f.bundle,roles,palette);
    REQUIRE(repair.applied==2);
    REQUIRE(repair.cell_labels.at(f.skin)=="skin");
    REQUIRE(*f.bundle.partition==relabeled);
    Slic3r::GUI::PortraitShapeDetails details;
    details.surface_partition=f.bundle.partition;
    details.confirmed_parent_cell_labels=repair.cell_labels;
    REQUIRE(details.confirmed_parent_cells().at(f.skin)=="skin");
    f.bundle.candidate["ownership_relabels"][f.skin]["source_parent_label"]="hair";
    REQUIRE(Cleanup::colors(f.bundle,roles,palette).applied==0);
}

TEST_CASE("Unrepaired stable parent cells retain their colors when the partition is adopted", "[PortraitParentCleanup]") {
    Fixture f;
    const Cleanup::Color prior{.7f,.4f,.3f};
    auto inherited = Cleanup::inherited_cell_colors(f.bundle, {{f.skin,prior},{std::string(64,'0'),{1,0,0}}});
    REQUIRE(inherited.size()==1);
    REQUIRE(inherited.at(f.skin)==prior);
    f.bundle.candidate["cells"].erase(f.bundle.candidate["cells"].begin());
    const auto repair = Cleanup::colors(f.bundle,roles,palette);
    REQUIRE(repair.cell_colors.empty());
    REQUIRE(inherited.at(f.skin)==prior);
    f.bundle.candidate["cells"].push_back(Json::array({f.skin,"portrait-skin","skin",size_t(1),false,"person"}));
    for (const auto& row : Cleanup::colors(f.bundle,roles,palette).cell_colors) inherited[row.first]=row.second;
    REQUIRE(inherited.at(f.skin)==palette[0]);
}

TEST_CASE("Parent proposal refuses frozen mixed cross-person and corrupt cell mappings", "[PortraitParentCleanup]") {
    Fixture f;
    SECTION("Frozen eye") { f.bundle.candidate["cells"][0][0]=f.bundle.locks->document["locks"][0]["locked_cells"][0]; }
    SECTION("Implicit root inside a mixed face") {
        f.bundle.candidate["cells"][1][3]=size_t(1);
        f.bundle.candidate["cells"][1][0]=Cleanup::root_cell_id(*f.bundle.partition,1);
    }
    SECTION("Another person") { f.bundle.candidate["cells"][1][5]="another"; }
    SECTION("Duplicate cell") { f.bundle.candidate["cells"].push_back(f.bundle.candidate["cells"][0]); }
    SECTION("Unknown role") { f.bundle.candidate["cells"][1][1]="portrait-lips"; }
    SECTION("Out of range") { f.bundle.candidate["cells"][1][3]=size_t(10000); }
    SECTION("Unbound ownership relabel") { f.bundle.candidate["ownership_relabels"]={{std::string(64,'0'),
        {{"source_label","face"},{"source_parent_label","face"},{"target_label","skin"}}}}; }
    const auto result=Cleanup::colors(f.bundle,roles,palette);
    REQUIRE(result.applied==0); REQUIRE(result.cell_colors.empty()); REQUIRE(result.root_colors.empty());
}

TEST_CASE("Reviewed bundle fails closed on paths hashes and source drift", "[PortraitParentCleanup]") {
    Fixture f;
    SECTION("Unsafe path") { f.bundle.candidate["partition_path"]="../partition.json"; f.publish(); REQUIRE_FALSE(f.load().partition); }
    SECTION("Windows absolute path") { REQUIRE_FALSE(Cleanup::safe_filename("C:partition.json")); }
    SECTION("Partition hash") { f.bundle.candidate["partition_file_sha256"]=std::string(64,'0'); f.publish(); REQUIRE_FALSE(f.load().partition); }
    SECTION("Lock hash") { f.bundle.candidate["lock_sha256"]=std::string(64,'0'); f.publish(); REQUIRE_FALSE(f.load().partition); }
    SECTION("Frozen fingerprint") { f.bundle.candidate["frozen_boundary_sha256"]=std::string(64,'0'); f.publish(); REQUIRE_FALSE(f.load().partition); }
    SECTION("Runtime") { f.bundle.candidate["boundary_runtime_sha256"]=std::string(64,'0'); f.publish(); REQUIRE_FALSE(f.load().partition); }
    SECTION("Policy") { f.bundle.candidate["boundary_policy_sha256"]=std::string(64,'0'); f.publish(); REQUIRE_FALSE(f.load().partition); }
    SECTION("Source") { REQUIRE_FALSE(Cleanup::load_bundle(f.root,std::string(64,'0'),std::string(64,'a'),10000).partition); }
}

TEST_CASE("Edited boundaries block upgrades and stable frozen cells allow parent changes", "[PortraitParentCleanup]") {
    Fixture f; const auto loaded=f.load();
    REQUIRE_FALSE(Cleanup::can_adopt(loaded,nullptr,nullptr,false));
    REQUIRE(Cleanup::can_adopt(loaded,nullptr,nullptr,true));
    auto previous=*loaded.partition; previous["partition_sha256"]=std::string(64,'5');
    REQUIRE(Cleanup::can_adopt(loaded,&previous,loaded.locks.get(),true));
    REQUIRE_FALSE(Cleanup::can_adopt(loaded,&previous,loaded.locks.get(),false));
    for(auto& c:previous["faces"][0]["cells"]) if(c["label"]=="le") c["polygon"][0][0]=.5;
    REQUIRE_FALSE(Cleanup::can_adopt(loaded,&previous,loaded.locks.get(),true));
}

TEST_CASE("Fangfei reviewed bundle replays all color counts through the production loader", "[.][PortraitParentCleanupReplay]") {
    const auto* root=boost::nowide::getenv("ORCA_PARENT_CLEANUP_REPLAY_ROOT"); REQUIRE(root);
    const auto bundle=Cleanup::load_bundle(root,"793c69491bc1039da75aca8eda4b5dd4307335fe0f02b84696562a86f722c066",
        "ed727689bd1658ac0c59a5726214d851fdaad3945b4475bb53129b85bb3f7026",976825);
    INFO(bundle.reason); REQUIRE(bundle.partition); REQUIRE(bundle.locks);
    const std::vector<std::string> all{"portrait-skin","portrait-dark","portrait-light","portrait-lips","portrait-cool","portrait-mid"};
    for(size_t count=3;count<=6;++count) {
        const std::vector<std::string> card(all.begin(),all.begin()+count);
        const auto result=Cleanup::colors(bundle,card,std::vector<Cleanup::Color>(count,{.5f,.5f,.5f}));
        INFO(count); INFO(result.reason);
        REQUIRE(result.applied==(count==6 ? 152238 : 152076));
        REQUIRE_FALSE(result.root_colors.empty()); REQUIRE_FALSE(result.cell_colors.empty());
    }
}

TEST_CASE("Portrait optimization adopts the reviewed parent partition and preserves facial role colors", "[.][PortraitParentCleanupIntegrationReplay]") {
    const auto env = [](const char* name) {
        const auto* value = boost::nowide::getenv(name);
        REQUIRE(value);
        return std::string(value);
    };
    const boost::filesystem::path model(env("ORCA_PARENT_CLEANUP_REPLAY_MODEL"));
    const auto installation = boost::filesystem::path(env("ORCA_PARENT_CLEANUP_REPLAY_INSTALL"));
    const auto data = boost::filesystem::path(env("ORCA_PARENT_CLEANUP_REPLAY_DATA"));
    struct Directories {
        std::string resources = Slic3r::resources_dir(), data = Slic3r::data_dir();
        ~Directories() { Slic3r::set_resources_dir(resources); Slic3r::set_data_dir(data); }
    } directories;
    Slic3r::set_resources_dir((installation / "resources").generic_string());
    Slic3r::set_data_dir(data.generic_string());
    Slic3r::TriangleMesh mesh;
    Slic3r::ObjInfo source_colors;
    std::string error;
    REQUIRE(AI::load_model_artifact(model, mesh, source_colors, error));
    auto source = std::make_shared<AI::SemanticColoring::MeshSnapshot>();
    source->mesh = mesh.its;
    source->vertex_colors = source_colors.vertex_colors;
    source->face_colors = source_colors.face_colors;
    source->geometry_id = AI::SurfaceSelectionPersistence::geometry_fingerprint(source->mesh);
    REQUIRE(source->geometry_id == "ed727689bd1658ac0c59a5726214d851fdaad3945b4475bb53129b85bb3f7026");
    source->content_id = AI::SemanticColoring::content_fingerprint(*source);
    const auto source_hash = AI::model_artifact_sha256(model);
    const auto bundle = Cleanup::load_bundle(installation / "resources/beauty-runtime/modules", source_hash,
        source->geometry_id, source->mesh.indices.size());
    INFO(bundle.reason);
    REQUIRE(bundle.partition);
    std::shared_ptr<const Slic3r::GUI::PortraitShapeDetails> previous;
    auto metadata = model;
    metadata.replace_extension(".json");
    if (boost::filesystem::is_regular_file(metadata)) {
        boost::filesystem::ifstream stream(metadata, std::ios::binary);
        const auto saved = Json::parse(stream);
        if (saved.contains("portrait_shape_reference")) {
            const auto& reference = saved.at("portrait_shape_reference");
            REQUIRE(reference.at("model_sha256") == source_hash);
            previous = Slic3r::GUI::PortraitShapeCache::load(reference, data / "cache",
                source->geometry_id, source->mesh.indices.size(),
                Slic3r::GUI::portrait_shape_runtime_fingerprint(), true);
            REQUIRE(previous->surface_partition);
            REQUIRE(Cleanup::can_adopt(bundle, previous->surface_partition.get(), previous->contour_locks.get(), true));
        }
    }
    std::vector<std::string> card{"portrait-skin","portrait-dark","portrait-light","portrait-lips","portrait-cool"};
    std::vector<Cleanup::Color> targets{{.9411765f,.8235294f,.7411765f},
        {.15686275f,.14901961f,.16078431f},{.96470588f,.96862745f,.97647059f},
        {.8f,.3f,.4f},{.25f,.35f,.45f}};
    const bool full_card = boost::nowide::getenv("ORCA_PARENT_CLEANUP_REPLAY_SIX") != nullptr;
    std::vector<Cleanup::Color> portrait;
    if (full_card) {
        portrait = {{236.f/255,195.f/255,178.f/255}, {40.f/255,38.f/255,41.f/255},
            {246.f/255,247.f/255,249.f/255}, {234.f/255,154.f/255,146.f/255},
            {102.f/255,140.f/255,182.f/255}, {149.f/255,139.f/255,134.f/255}};
        targets = portrait;
        std::reverse(targets.begin(), targets.end());
        const auto binding = Slic3r::GUI::portrait_card_slot_mapping(targets, portrait);
        REQUIRE(binding.enabled);
        REQUIRE(binding.target_colors == targets);
        card = Slic3r::GUI::portrait_roles_for_card(binding.target_colors, portrait);
    }
    Slic3r::GUI::ModelSemanticColoring coordinator(
        std::filesystem::u8path((installation / "ai/portrait_semantics").generic_string()),
        std::filesystem::u8path((data / "native-cache").generic_string()));
    REQUIRE(coordinator.request(source, targets, targets, portrait, {}, std::filesystem::u8path(model.generic_string()), previous, card));
    std::unique_ptr<Slic3r::GUI::ModelSemanticColoring::Result> result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    while (!result && std::chrono::steady_clock::now() < deadline) {
        result = coordinator.poll();
        if (!result && !coordinator.busy()) break;
        if (!result) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!result) coordinator.cancel();
    REQUIRE(result);
    INFO(result->error);
    INFO(result->shape_error);
    INFO(result->parent_repair_status);
    REQUIRE(result->error.empty());
    REQUIRE(result->parent_repair_cells == (full_card ? 152238 : 152076));
    REQUIRE(result->shape_details);
    REQUIRE_FALSE(result->geometry.is_empty());
    REQUIRE(result->shape_details->mapping_fingerprint() == bundle.partition->at("partition_sha256"));
    REQUIRE(Cleanup::frozen_boundary(*result->shape_details->surface_partition, *result->shape_details->contour_locks) ==
        bundle.candidate.at("frozen_boundary_sha256"));
    const auto expected = Slic3r::GUI::portrait_contour_colors(*bundle.partition, card, targets);
    if (full_card) {
        const auto repaired = Cleanup::colors(bundle, card, targets);
        const auto skin = std::find(card.begin(), card.end(), "portrait-skin");
        const auto light = std::find(card.begin(), card.end(), "portrait-light");
        REQUIRE(skin != card.end());
        REQUIRE(light != card.end());
        const auto skin_color = targets.at(size_t(skin-card.begin()));
        REQUIRE(skin_color != targets.at(size_t(light-card.begin())));
        size_t repaired_skin_cells = 0;
        for (const auto& entry : repaired.cell_labels) if (entry.second == "skin") {
            REQUIRE(result->shape_details->cell_colors.at(entry.first) == skin_color);
            ++repaired_skin_cells;
        }
        std::map<size_t, Cleanup::Color> automatic;
        for (const auto& entry : result->automatic) automatic[entry.first] = entry.second;
        for (const auto& entry : repaired.root_labels) if (entry.second == "skin") {
            REQUIRE(automatic.at(entry.first) == skin_color);
            ++repaired_skin_cells;
        }
        REQUIRE(repaired_skin_cells == 50322);
    }
    if (previous) {
        const auto inherited = Cleanup::inherited_cell_colors(bundle, previous->cell_colors);
        const auto repaired = Cleanup::colors(bundle, card, targets);
        for (const auto& entry : inherited) if (!expected.count(entry.first) && !repaired.cell_colors.count(entry.first)) {
            REQUIRE(result->shape_details->cell_colors.count(entry.first)==1);
            REQUIRE(result->shape_details->cell_colors.at(entry.first)==entry.second);
        }
    }
    for (const auto& face : bundle.partition->at("faces")) for (const auto& cell : face.at("cells")) {
        const auto id = cell.at("id").get<std::string>();
        if (Cleanup::frozen_label(cell.at("label")) && expected.count(id))
            REQUIRE(result->shape_details->cell_colors.at(id) == expected.at(id));
    }
    const auto reference = Slic3r::GUI::PortraitShapeCache::save(*result->shape_details, data / "cache");
    const auto reopened = Slic3r::GUI::PortraitShapeCache::load(reference, data / "cache",
        source->geometry_id, source->mesh.indices.size(),
        Slic3r::GUI::portrait_shape_runtime_fingerprint(), true);
    REQUIRE(reopened->mapping_fingerprint() == result->shape_details->mapping_fingerprint());
    REQUIRE(reopened->boundary_fingerprint() == result->shape_details->boundary_fingerprint());
    REQUIRE(reopened->cell_colors == result->shape_details->cell_colors);
    REQUIRE(reopened->confirmed_parent_roots == result->shape_details->confirmed_parent_roots);
    REQUIRE(reopened->confirmed_parent_cell_labels == result->shape_details->confirmed_parent_cell_labels);
    REQUIRE_FALSE(reopened->confirmed_parent_cell_labels.empty());
    REQUIRE(AI::model_artifact_sha256(model) == source_hash);
}

// Local opt-in evidence replay. Private models and outputs stay outside Git.
TEST_CASE("Current portrait evidence refresh preserves saved details across actual palette drafts", "[.PortraitParentSaveReplay]") {
    namespace GUI=Slic3r::GUI;
    namespace fs=boost::filesystem;
    const auto env=[](const char* key) {const auto* p=boost::nowide::getenv(key);REQUIRE(p);return fs::path(p);};
    const auto baseline=env("ORCA_PORTRAIT_PARENT_BASELINE"), stage1=env("ORCA_PORTRAIT_PARENT_STAGE1");
    const auto runtime=env("ORCA_PORTRAIT_PARENT_RUNTIME"), data=env("ORCA_PORTRAIT_PARENT_DATA");
    const auto output=env("ORCA_PORTRAIT_PARENT_OUTPUT");
    REQUIRE_FALSE(fs::exists(output));fs::create_directories(output);
    const auto read=[](const fs::path& p){fs::ifstream f(p,std::ios::binary);REQUIRE(bool(f));return Json::parse(f);};
    struct Restore {std::string resources=Slic3r::resources_dir(),data=Slic3r::data_dir();
        ~Restore(){Slic3r::set_resources_dir(resources);Slic3r::set_data_dir(data);}} restore;
    Slic3r::set_resources_dir((runtime/"resources").generic_string());Slic3r::set_data_dir(data.generic_string());
    const auto model=baseline/"gui-data/generated_models/downloads/orcaslicer-ai-8f6f1850-c076-4264-b26b-ced09f0397ff.glb";
    Slic3r::TriangleMesh mesh;Slic3r::ObjInfo colors;std::string error;
    REQUIRE(AI::load_model_artifact(model,mesh,colors,error));
    auto source=std::make_shared<GUI::ModelSemanticColoring::Snapshot>();
    source->mesh=mesh.its;source->vertex_colors=colors.vertex_colors;source->face_colors=colors.face_colors;
    source->geometry_id=AI::SurfaceSelectionPersistence::geometry_fingerprint(source->mesh);
    source->content_id=AI::SemanticColoring::content_fingerprint(*source);
    const auto source_hash=AI::model_artifact_sha256(model);
    GUI::PortraitShapeCache::preserve_source(output/"cache",model,source_hash);
    const auto old_results=read(stage1/"results.json");Json report=Json::array();
    const auto* selected_palette=boost::nowide::getenv("ORCA_PORTRAIT_PARENT_PALETTE");
    for(const auto& old:old_results) {
        const auto started=std::chrono::steady_clock::now();const auto name=old.at("palette").get<std::string>();
        if(selected_palette && name!=selected_palette) continue;
        const auto draft_ref=read(baseline/(name+"-draft-reference.json"));
        const auto draft=read(baseline/"gui-data/cache"/draft_ref.at("path").get<std::string>());
        REQUIRE(draft.at("source_sha256")==source_hash);
        const auto doc=read(stage1/"cache"/old.at("shape_reference").at("path").get<std::string>());
        auto shapes=GUI::PortraitShapeCache::load(old.at("shape_reference"),stage1/"cache",source->geometry_id,
            source->mesh.indices.size(),doc.at("runtime_fingerprint"),true);
        const auto before_boundary=Cleanup::frozen_boundary(*shapes->surface_partition,*shapes->contour_locks);
        const auto& card=draft.at("colors");
        REQUIRE(draft.at("manual").at("colors").empty());
        if(!draft.at("leaf_edits").is_null()) REQUIRE(draft.at("leaf_edits").at("colors").empty());
        GUI::ModelSemanticColoring coordinator(std::filesystem::u8path((runtime/"ai/portrait_semantics").generic_string()),
            std::filesystem::u8path((data/"cache/portrait_semantics").generic_string()));
        auto progress=std::make_shared<GUI::PortraitOptimizationTask>("replay-"+name,model.generic_string(),1);
        auto saved_appearance=std::make_shared<GUI::ModelSemanticColoring::SavedAppearance>();
        saved_appearance->geometry_id=source->geometry_id;saved_appearance->face_count=source->mesh.indices.size();
        for(const auto& row:draft.at("semantic").at("faces"))
            saved_appearance->faces.emplace_back(row[0].get<size_t>(),row[1].get<Cleanup::Color>());
        for(const auto& row:draft.at("semantic").at("subfaces"))
            saved_appearance->subfaces.push_back({row[0].get<size_t>(),{row[1].get<uint8_t>(),row[2].get<uint8_t>()},row[3].get<Cleanup::Color>(),1.f});
        REQUIRE(coordinator.request(source,card.at("semantic_mapping_palette").get<std::vector<Cleanup::Color>>(),
            card.at("semantic_palette").get<std::vector<Cleanup::Color>>(),
            card.at("semantic_portrait_card").get<std::vector<Cleanup::Color>>(),{},
            std::filesystem::u8path(model.generic_string()),shapes,card.at("semantic_role_uids").get<std::vector<std::string>>(),true,{},progress,saved_appearance));
        std::unique_ptr<GUI::ModelSemanticColoring::Result> result;
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::minutes(12);
        while(!result && std::chrono::steady_clock::now()<deadline) {
            result=coordinator.poll();if(!result && !coordinator.busy())break;
            if(!result)std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if(!result)coordinator.cancel();REQUIRE(result);INFO(name);INFO(result->error);INFO(result->shape_error);
        REQUIRE(result->error.empty());REQUIRE(result->shape_details);
        auto next=result->shape_details;
        REQUIRE(Cleanup::frozen_boundary(*next->surface_partition,*next->contour_locks)==before_boundary);
        REQUIRE(next->contour_locks->document.at(std::string("locks"))==shapes->contour_locks->document.at(std::string("locks")));
        std::set<size_t> new_explicit;
        for(const auto& face:next->surface_partition->at(std::string("faces")))
            new_explicit.insert(face.at(std::string("source_face_id")).get<size_t>());
        const std::map<size_t,Cleanup::Color> final_faces(result->automatic.begin(),result->automatic.end());
        for(const auto& row:saved_appearance->faces)
            if(!next->confirmed_parent_roots.count(row.first) && !new_explicit.count(row.first)) {
                REQUIRE(final_faces.count(row.first));REQUIRE(final_faces.at(row.first)==row.second);
            }
        const auto shape_ref=GUI::PortraitShapeCache::save(*next,output/"cache");
        AI::BeautyAppearanceOptions options;options.face_weights.assign(source->mesh.indices.size(),0.f);
        options.face_target_colors.resize(options.face_weights.size());
        Json semantic={{"faces",Json::array()},{"subfaces",Json::array()}};
        for(const auto& row:result->automatic) {options.face_weights.at(row.first)=1.f;options.face_target_colors.at(row.first)=row.second;
            semantic["faces"].push_back({row.first,row.second});}
        for(const auto& row:result->automatic_subfaces)semantic["subfaces"].push_back({row.face_id,row.path.depth,row.path.value,row.color});
        AI::appearance_subface_colors(options,result->automatic_subfaces,source->geometry_id);
        AI::appearance_cell_colors(options,*next->surface_partition,next->cell_colors,source->geometry_id);
        const auto path=output/(name+".glb");const auto saved=AI::edit_glb_appearance(model,path,options);
        INFO(saved.error);REQUIRE(saved.success);
        Slic3r::TriangleMesh reopened;Slic3r::ObjInfo reopened_colors;
        REQUIRE(AI::load_model_artifact(path,reopened,reopened_colors,error));
        REQUIRE(AI::SurfaceSelectionPersistence::geometry_fingerprint(reopened.its)==source->geometry_id);
        REQUIRE(AI::model_artifact_sha256(model)==source_hash);
        fs::ofstream(output/(name+"-appearance.json"))<<Json({{"semantic",semantic},{"colors",card},{"shapes",shape_ref},
            {"source_sha256",source_hash},{"geometry",source->geometry_id},{"face_count",source->mesh.indices.size()}}).dump();
        report.push_back({{"palette",name},{"source_sha256",source_hash},{"output_sha256",AI::model_artifact_sha256(path)},
            {"parent_status",result->parent_repair_status},{"shape_error",result->shape_error},
            {"parent_audit",result->parent_repair_audit},{"frozen_boundaries_preserved",true},
            {"seconds",std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count()}});
        fs::ofstream(output/"results.json")<<report.dump(2);
    }
    REQUIRE_FALSE(report.empty());
}
