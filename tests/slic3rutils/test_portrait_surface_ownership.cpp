#include <catch2/catch_test_macros.hpp>
#include "slic3r/GUI/AI/ModelGeneration/PortraitColorPlanBuild.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include <boost/nowide/cstdlib.hpp>
#include <cstring>
#include "../test_utils.hpp"

using namespace Slic3r;
namespace GUI = Slic3r::GUI;
namespace AI = Slic3r::AI;
namespace SC = AI::SemanticColoring;
using Json = nlohmann::json;

namespace {
Json read_json(const boost::filesystem::path& file) {
    boost::filesystem::ifstream stream(file,std::ios::binary); Json value; stream >> value; return value;
}
SC::FaceColors read_roots(const boost::filesystem::path& file) {
    boost::filesystem::ifstream stream(file,std::ios::binary);
    std::string bytes(std::istreambuf_iterator<char>(stream),{});
    if (!stream || bytes.size()%16) throw std::invalid_argument("Invalid preserved root file.");
    SC::FaceColors result;
    for (size_t i=0;i<bytes.size();i+=16) {
        uint32_t face; SC::Color rgb; std::memcpy(&face,bytes.data()+i,4); std::memcpy(rgb.data(),bytes.data()+i+4,12);
        result.push_back({face,rgb});
    }
    return result;
}
SC::SubfaceColors read_children(const boost::filesystem::path& file) {
    SC::SubfaceColors result;
    for (const auto& row:read_json(file)) result.push_back({row.at("face_id"),
        {row.at("path").at("depth"),row.at("path").at("value")},row.at("color"),row.at("confidence")});
    return result;
}
std::optional<SC::Color> effective(const AI::BeautyLeafKey& key,const SC::FaceColors& roots,const SC::SubfaceColors& children) {
    std::optional<SC::Color> color; uint8_t depth=0;
    const auto root=std::lower_bound(roots.begin(),roots.end(),key.source_face_id,[](const auto& a,size_t f){return a.first<f;});
    if(root!=roots.end() && root->first==key.source_face_id) color=root->second;
    for(const auto& child:children) if(child.face_id==key.source_face_id) {
        AI::BeautyLeafKey other{child.face_id,child.path.depth,child.path.value};
        if(other.contains(key) && child.path.depth>=depth) {color=child.color;depth=child.path.depth;}
    }
    return color;
}
}

TEST_CASE("Parent repairs preserve feature leaves and unknown siblings without color-based exclusion", "[PortraitSurfaceOwnership][PortraitColorPlan]") {
    auto mesh=its_make_cube(10,10,10);
    // The actual domain budget is exercised with a realistic source count.
    const auto cube_faces=mesh.indices;
    while(mesh.indices.size()<1200) mesh.indices.insert(mesh.indices.end(),cube_faces.begin(),cube_faces.end());
    auto surface=AI::BeautySurface::build(mesh,{});
    GUI::PortraitShapeDetails details; auto& locks=details.locks;
    locks.geometry_id=surface->geometry_id; locks.face_count=mesh.indices.size(); locks.source_sha256=std::string(64,'a');
    locks.evidence_sha256=std::string(64,'b'); locks.runtime_sha256=std::string(64,'c'); locks.policy_sha256=std::string(64,'d');
    locks.baseline_sha256=locks.evidence_sha256; locks.boundary_policy_sha256=std::string(64,'e');
    AI::BeautyLeafDomain domain{surface->geometry_id,locks.face_count,{{0,1,0},{0,1,1},{0,1,2},{0,1,3}}};
    locks.leaf_domain=domain;
    AI::ShapeLock eye; eye.subject_id="person"; eye.label="le"; eye.parent_label="le"; eye.status="VALID_SHAPE";
    eye.view_support=2; eye.locked_faces={0}; eye.locked_leaves={{0,1,0}}; locks.locks={eye}; details.subjects={"person"};
    details.base_colors.assign(locks.face_count,RGBA{.2f,.08f,.04f,1});
    Json doc={{"schema","orca.portrait-surface-ownership/v1"},{"geometry_id",locks.geometry_id},
        {"source_sha256",locks.source_sha256},{"evidence_sha256",locks.evidence_sha256},{"face_count",locks.face_count},
        {"boundary_sha256",AI::beauty_leaf_digest(locks.encode().dump())},{"policy",Json::object()},
        {"policy_sha256",AI::beauty_leaf_digest("{}")},{"editing_domain",domain.encode()},
        {"editing_mapping_sha256",domain.fingerprint()}, {"regions",Json::array({{
            {"id","skin"},{"subject_id","person"},{"parent_label","face"},{"status","CONFIRMED_PARENT"},
            {"evidence_source","VERIFIED_FACE"},{"view_ids",{"front","left"}},{"risks",Json::array()},
            {"leaves",Json::array({{0,1,1},{1,0,0}})}}})}};
    details.surface_ownership=std::make_shared<GUI::PortraitSurfaceOwnership>(GUI::PortraitSurfaceOwnership::decode(doc,locks));
    const auto editable=domain.all_leaves();
    std::vector<int32_t> guidance(editable.size(),-1);
    details.surface_ownership->overlay_labels(editable,guidance);
    REQUIRE(guidance[1]==3); REQUIRE(guidance[0]==-1); REQUIRE(guidance[2]==-1);
    AI::SurfaceSelectionPersistence::SelectionState selection;
    selection.selected.assign(editable.size(),1);selection.protected_faces.assign(editable.size(),0);selection.protected_faces[4]=1;
    details.surface_ownership->overlay_selection("skin",editable,selection);
    REQUIRE(selection.selected[1]==1);REQUIRE(selection.selected[0]==0);REQUIRE(selection.selected[2]==0);
    std::vector<GUI::PortraitColorPlan::Slot> palette={{"portrait-skin",{.9f,.8f,.7f}},
        {"portrait-dark",{.1f,.1f,.1f}},{"portrait-light",{1,1,1}},{"portrait-lips",{.8f,.3f,.3f}}};
    SC::Analysis analysis; analysis.geometry_id=surface->geometry_id;
    analysis.face_labels.assign(locks.face_count,SC::Label::Unknown); analysis.face_confidence.assign(locks.face_count,0);
    SC::FaceColors roots={{0,palette[3].rgb}}; // Face 1 deliberately has no slot.
    SC::SubfaceColors children={{0,{1,0},palette[2].rgb,1}};
    const auto before=children;
    auto plan=GUI::build_portrait_color_plan(details,mesh,*surface,analysis,palette,roots,children);
    const auto encoded=plan.encode(); bool missing=false;
    for(const auto& assignment:encoded.at("assignments")) for(const auto& source:assignment.at("color_sources"))
        if(source.at("leaf")[0]==1) {REQUIRE(source.at("original_uid").is_null());missing=true;}
    REQUIRE(missing);
    GUI::apply_portrait_color_plan(plan,roots,children);
    REQUIRE(effective({0,1,0},roots,children)==palette[2].rgb);
    REQUIRE(effective({0,1,1},roots,children)==palette[0].rgb);
    REQUIRE(effective({0,1,2},roots,children)==palette[3].rgb);
    REQUIRE(effective({1,0,0},roots,children)==palette[0].rgb);
    SECTION("A feature, other person, drift or same camera cannot authorize parent color") {
        for(const auto field:{"source_sha256","geometry_id","boundary_sha256","evidence_sha256"}) {
            auto bad=doc; bad[field]=std::string(64,'f'); REQUIRE_THROWS(GUI::PortraitSurfaceOwnership::decode(bad,locks));
        }
        auto bad=doc; bad["regions"][0]["leaves"]={{0,1,0}}; REQUIRE_THROWS(GUI::PortraitSurfaceOwnership::decode(bad,locks));
        bad=doc; bad["regions"][0]["subject_id"]="other"; REQUIRE_THROWS(GUI::PortraitSurfaceOwnership::decode(bad,locks));
        bad=doc; bad["regions"][0]["view_ids"]={"front","front"}; REQUIRE_THROWS(GUI::PortraitSurfaceOwnership::decode(bad,locks));
    }
    SECTION("Optional addressed parent and color inheritance restore independently of the unchanged shape sidecar") {
        ScopedTemporaryDir directory("portrait-parent-context");
        const auto original=directory.path()/"source.glb"; std::string error;
        REQUIRE(AI::write_model_artifact(original,mesh,std::vector<RGBA>(mesh.vertices.size(),RGBA{1,1,1,1}),error));
        locks.source_sha256=AI::model_artifact_sha256(original);
        GUI::PortraitShapeCache::preserve_source(directory.path(),original,locks.source_sha256);
        doc["source_sha256"]=locks.source_sha256;doc["boundary_sha256"]=AI::beauty_leaf_digest(locks.encode().dump());
        details.surface_ownership=std::make_shared<GUI::PortraitSurfaceOwnership>(GUI::PortraitSurfaceOwnership::decode(doc,locks));
        Json inherited={{"schema","orca.portrait-color-inheritance/v1"},{"geometry_id",locks.geometry_id},
            {"source_sha256",locks.source_sha256},{"face_count",locks.face_count},
            {"boundary_sha256",doc.at("boundary_sha256")},{"palette",Json::array()},
            {"faces",Json::array({{0,palette[3].rgb}})},{"subfaces",Json::array()}};
        for(const auto& slot:palette) inherited["palette"].push_back(slot.rgb);
        details.color_inheritance=std::make_shared<GUI::PortraitColorInheritance>(GUI::PortraitColorInheritance::decode(inherited,locks));
        details.runtime_fingerprint=std::string(64,'f');
        const auto ref=GUI::PortraitShapeCache::save(details,directory.path());
        const auto loaded=GUI::PortraitShapeCache::load(ref,directory.path(),locks.geometry_id,locks.face_count,details.runtime_fingerprint);
        REQUIRE(loaded->surface_ownership);REQUIRE(loaded->color_inheritance);
        REQUIRE(loaded->locks.encode()==locks.encode());REQUIRE(loaded->color_inheritance->faces==details.color_inheritance->faces);
        auto bad=GUI::PortraitOwnershipCache::save(*details.surface_ownership,locks,directory.path());
        bad["path"]="../external.json";REQUIRE_THROWS(GUI::PortraitOwnershipCache::load(bad,directory.path(),locks));
        auto context=read_json(directory.path()/ref.at("path").get<std::string>());
        context["ownership_ref"]=bad;
        const auto bytes=context.dump(),hash=AI::beauty_leaf_digest(bytes);
        boost::filesystem::ofstream stream(directory.path()/"portrait-shapes"/(hash+".json"),std::ios::binary);stream<<bytes;stream.close();
        auto corrupt=ref;corrupt["sha256"]=hash;corrupt["path"]="portrait-shapes/"+hash+".json";
        const auto fallback=GUI::PortraitShapeCache::load(corrupt,directory.path(),locks.geometry_id,locks.face_count,details.runtime_fingerprint);
        REQUIRE_FALSE(fallback->surface_ownership);REQUIRE(fallback->locks.encode()==locks.encode());
        REQUIRE_FALSE(fallback->ownership_diagnostic.empty());
    }
}

TEST_CASE("Reviewed R5 colors receive only confirmed parent repairs in all four budgets", "[.][PortraitR6Replay]") {
    const auto environment=[](const char* key){const auto v=boost::nowide::getenv(key);return v?std::string(v):std::string();};
    const boost::filesystem::path run(environment("ORCA_R6_RUN")),output(environment("ORCA_R6_COLOR_OUTPUT"));
    if(run.empty() || output.empty()) SKIP("Explicit preserved R6 run and new output are required.");
    REQUIRE_FALSE(boost::filesystem::exists(output));
    const auto manifest=read_json(run/"stage-manifest.json");
    for(const auto& entry:manifest.at("files")) REQUIRE(AI::model_artifact_sha256(run/entry.at("path").get<std::string>())==entry.at("sha256"));
    const auto identity=read_json(run/"baseline/shape-locks.json");
    TriangleMesh mesh; ObjInfo colors; std::string error;
    REQUIRE(AI::load_model_artifact(run/"baseline/source.glb",mesh,colors,error));
    auto surface=AI::BeautySurface::build(mesh.its,colors.vertex_colors);
    GUI::PortraitShapeDetails details;
    details.locks=AI::ShapeLockSet::decode(identity,surface->geometry_id,manifest.at("source_sha256"),mesh.its.indices.size(),
        identity.at("evidence_sha256"),identity.at("runtime_sha256"),identity.at("policy_sha256"),
        identity.at("baseline_sha256"),identity.at("boundary_policy_sha256"));
    details.base_colors=colors.face_colors.size()==mesh.its.indices.size()?colors.face_colors:AI::beauty_source_face_colors(mesh.its,colors.vertex_colors);
    details.subjects=read_json(run/"baseline/evidence.json").at("subjects").get<std::vector<std::string>>();
    const auto ownership_file=run/environment("ORCA_R6_OWNERSHIP");
    details.surface_ownership=std::make_shared<GUI::PortraitSurfaceOwnership>(GUI::PortraitSurfaceOwnership::decode(read_json(ownership_file),details.locks));
    SC::Analysis analysis; analysis.geometry_id=surface->geometry_id;
    analysis.face_labels.assign(details.locks.face_count,SC::Label::Unknown); analysis.face_confidence.assign(details.locks.face_count,0);
    boost::filesystem::create_directories(output); Json reports=Json::array();
    // Five colors first, followed by regression budgets.
    for(const auto count:{5,3,4,6}) {
        const auto folder=run/"baseline"/("colors-"+std::to_string(count));
        const auto old_plan=read_json(folder/"portrait-color-plan.json");
        std::vector<GUI::PortraitColorPlan::Slot> palette;
        for(const auto& row:old_plan.at("palette")) palette.push_back({row.at("uid"),row.at("rgb")});
        auto roots=read_roots(folder/"face-colors.bin"); auto children=read_children(folder/"subface-colors.json");
        const auto old_roots=roots; const auto old_children=children;
        const auto plan=GUI::build_portrait_color_plan(details,mesh.its,*surface,analysis,palette,roots,children);
        GUI::apply_portrait_color_plan(plan,roots,children);
        std::set<AI::BeautyLeafKey> targeted;
        for(const auto& c:plan.components) targeted.insert(c.leaves.begin(),c.leaves.end());
        size_t changed=0; const auto all=details.surface_ownership->editing_domain.all_leaves();
        // Index each root's effective leaf color, including texture-only inherited siblings.
        std::map<size_t,SC::SubfaceColors> old_groups,new_groups;
        for(const auto& c:old_children) old_groups[c.face_id].push_back(c);
        for(const auto& c:children) new_groups[c.face_id].push_back(c);
        for(const auto& key:all) {
            const auto a=effective(key,old_roots,old_groups[key.source_face_id]);
            const auto b=effective(key,roots,new_groups[key.source_face_id]);
            if(!targeted.count(key)) {if(a!=b) FAIL("A non-target sibling or feature changed");}
            else if(a!=b) ++changed;
        }
        const auto directory=output/("colors-"+std::to_string(count)); boost::filesystem::create_directories(directory);
        const auto write=[&](const char* name,const Json& value){boost::filesystem::ofstream stream(directory/name,std::ios::binary);stream<<value.dump();stream.close();REQUIRE(bool(stream));};
        write("portrait-color-plan.json",plan.encode());
        boost::filesystem::ofstream stream(directory/"face-colors.bin",std::ios::binary);
        for(const auto& row:roots){const uint32_t f=uint32_t(row.first);stream.write(reinterpret_cast<const char*>(&f),4);stream.write(reinterpret_cast<const char*>(row.second.data()),12);} stream.close();REQUIRE(bool(stream));
        Json subs=Json::array();for(const auto& child:children)subs.push_back({{"face_id",child.face_id},{"path",{{"depth",child.path.depth},{"value",child.path.value}}},{"color",child.color},{"confidence",child.confidence}});
        write("subface-colors.json",subs);
        reports.push_back({{"color_count",count},{"changed_leaves",changed},{"components",plan.components.size()},
            {"plan_sha256",AI::model_artifact_sha256(directory/"portrait-color-plan.json")},
            {"face_colors_sha256",AI::model_artifact_sha256(directory/"face-colors.bin")},
            {"subface_colors_sha256",AI::model_artifact_sha256(directory/"subface-colors.json")},
            {"non_target_and_locked_colors_unchanged",true}});
    }
    boost::filesystem::ofstream stream(output/"stage-report.json");stream<<Json{{"variants",reports},
        {"ownership_sha256",AI::model_artifact_sha256(ownership_file)},{"visual_status","PENDING_USER"},
        {"production_enabled",false},{"new_recognition_performed",false},{"original_material_tree_changed",false}}.dump();
    stream.close();REQUIRE(bool(stream));
}
