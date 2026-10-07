#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "slic3r/GUI/AI/ModelGeneration/PortraitColorPlanBuild.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include <boost/nowide/cstdlib.hpp>
#include <boost/filesystem/fstream.hpp>
#include <fstream>
#include <cstring>

using Slic3r::GUI::PortraitColorPlan;
using Plan = PortraitColorPlan;

namespace {
Plan fixture(size_t count = 6) {
    Plan p;
    p.geometry_id = std::string(64,'a'); p.source_sha256 = std::string(64,'b');
    p.boundary_sha256 = std::string(64,'c'); p.face_count = 10000;
    p.palette = {{"portrait-skin",{.97f,.886f,.855f}},{"portrait-dark",{.157f,.149f,.161f}},
        {"portrait-light",{.965f,.969f,.976f}},{"portrait-lips",{.918f,.604f,.573f}},
        {"portrait-cool",{.4f,.549f,.714f}},{"portrait-mid",{.584f,.545f,.525f}}};
    p.palette.resize(count);
    return p;
}
Plan::Component component(size_t face, std::string label, size_t old = 0) {
    Plan::Component c;
    c.id = std::to_string(face); c.region = "person:"+label; c.label = label;
    c.leaves = {{face,0,0}}; c.area = 1.; c.source = {.78f,.65f,.55f}; c.original_slot = old;
    return c;
}
}

TEST_CASE("Fixed portrait roles preserve brows eyes lips and skin at every color budget", "[PortraitColorPlan]") {
    const auto count = GENERATE(3u,4u,5u,6u);
    auto p = fixture(count);
    for (const auto& label : {"lb","rb","iris","le","re","ulip","llip","teeth","face","neck"})
        p.components.push_back(component(p.components.size(),label));
    const auto before = p.palette;
    p.optimize();
    for (size_t i = 0; i < p.components.size(); ++i) REQUIRE(p.decisions[i].slot == p.preferred(p.components[i]));
    REQUIRE(p.palette.size() == count);
    for (size_t i = 0; i < count; ++i) {
        REQUIRE(p.palette[i].uid == before[i].uid);
        REQUIRE(p.palette[i].rgb == before[i].rgb);
    }
    REQUIRE(p.decisions[0].slot == p.slot("portrait-dark"));
    REQUIRE(p.decisions[5].slot == (count == 3 ? p.slot("portrait-dark") : p.slot("portrait-lips")));
}

TEST_CASE("Oral and explicit conflict components retain their previous assignments", "[PortraitColorPlan]") {
    auto p = fixture();
    p.components = {component(0,"imouth",2),component(1,"face",3)};
    p.components[1].conflict = true;
    p.optimize();
    REQUIRE(p.decisions[0].slot == 2);
    REQUIRE(p.decisions[1].slot == 3);
}

TEST_CASE("Extra slots require distinct visible camera families and perceptual benefit", "[PortraitColorPlan]") {
    auto p = fixture();
    auto c = component(0,"cloth",2); c.source = p.palette[4].rgb;
    REQUIRE_FALSE(p.extra_supported(c,4));
    c.visible_pixels = {{"front",100},{"oblique",31}};
    REQUIRE_FALSE(p.extra_supported(c,4));
    c.visible_pixels["oblique"] = 32;
    REQUIRE(p.extra_supported(c,4));
    c.source = p.palette[2].rgb;
    REQUIRE_FALSE(p.extra_supported(c,4));
    c.source = p.palette[4].rgb; p.palette[4].rgb = p.palette[2].rgb;
    REQUIRE_FALSE(p.extra_supported(c,4));
}

TEST_CASE("An unsupported cold fragment joins its adjacent garment without penalizing small features", "[PortraitColorPlan]") {
    auto p = fixture();
    auto large = component(0,"cloth",2), small = component(1,"cloth",4), brow = component(2,"lb",2);
    large.area = 100.; large.source = p.palette[2].rgb; large.neighbors = {1};
    small.area = .01; small.source = p.palette[4].rgb; small.neighbors = {0}; brow.area = .001;
    p.components = {large,small,brow}; p.optimize();
    REQUIRE(p.decisions[0].slot == 2);
    REQUIRE(p.decisions[1].slot == 2);
    REQUIRE(p.decisions[2].slot == 1);
    const auto encoded = p.encode();
    REQUIRE(encoded.at("schema") == "orca.portrait-color-plan/v1");
    REQUIRE(encoded.at("unused_slots").size() == 4);
    const std::vector<size_t> original {2,4,2};
    REQUIRE_THAT(encoded.at("initial_cost").get<double>(),Catch::Matchers::WithinAbs(p.cost(original),1e-10));
    REQUIRE_THAT(encoded.at("total_cost_delta").get<double>(),Catch::Matchers::WithinAbs(
        encoded.at("cost").get<double>()-p.cost(original),1e-10));
    for (const auto term : encoded.at("terms")) {
        REQUIRE(term.get<double>() >= 0.);
        REQUIRE(term.get<double>() <= 1.);
    }
}

TEST_CASE("Local objective differences equal the fully normalized global objective", "[PortraitColorPlan]") {
    auto p = fixture();
    p.components = {component(0,"cloth",2),component(1,"cloth",1),component(2,"face",0),component(3,"lb",1)};
    p.components[0].neighbors = {1,2}; p.components[1].neighbors = {0};
    p.components[2].neighbors = {0,3}; p.components[3].neighbors = {2};
    p.components[1].area = .01;
    std::vector<size_t> choices {2,1,0,1};
    const auto norm = p.normalization();
    for (size_t i = 0; i < choices.size(); ++i) for (size_t target = 0; target < p.palette.size(); ++target) {
        auto changed = choices; changed[i] = target;
        REQUIRE_THAT(p.delta(choices,i,target,norm),Catch::Matchers::WithinAbs(p.cost(changed)-p.cost(choices),1e-10));
    }
}

TEST_CASE("Color planning refuses ambiguous leaf ownership invalid identities and asymmetric topology", "[PortraitColorPlan]") {
    auto p = fixture(); p.components = {component(0,"face"),component(1,"lb")};
    SECTION("Ancestor overlap") {
        p.components[1].leaves = {{0,1,0}};
        REQUIRE_THROWS(p.optimize());
    }
    SECTION("Invalid hash") { p.boundary_sha256 = "wrong"; REQUIRE_THROWS(p.optimize()); }
    SECTION("Invalid RGB") { p.palette[0].rgb[0] = 2; REQUIRE_THROWS(p.optimize()); }
    SECTION("Unknown extra role") { p.palette[4].uid = "custom-extra"; REQUIRE_THROWS(p.optimize()); }
    SECTION("Missing inverse edge") { p.components[0].neighbors = {1}; REQUIRE_THROWS(p.optimize()); }
}

TEST_CASE("Applying a leaf color preserves the sibling and every non-target face", "[PortraitColorPlan]") {
    namespace SC = Slic3r::AI::SemanticColoring;
    auto p = fixture(); p.components = {component(0,"lb",2)};
    p.components.front().leaves = {{0,2,0}}; p.optimize();
    SC::FaceColors roots {{0,p.palette[0].rgb},{1,p.palette[2].rgb}};
    SC::SubfaceColors children {{0,{1,0},p.palette[0].rgb,1.f},{0,{2,1},p.palette[2].rgb,1.f},
        {1,{2,2},p.palette[1].rgb,1.f}};
    Slic3r::GUI::apply_portrait_color_plan(p,roots,children);
    REQUIRE(roots.front().second == p.palette[0].rgb);
    REQUIRE(roots.back().second == p.palette[2].rgb);
    REQUIRE(children.size() == 4);
    REQUIRE(children[1].path.value == 0);
    REQUIRE(children[1].color == p.palette[1].rgb);
    REQUIRE(children[2].path.value == 1);
    REQUIRE(children[2].color == p.palette[2].rgb);
}
TEST_CASE("A no-op root color decision preserves existing mixed clothing skin children", "[PortraitColorPlan]") {
    namespace SC = Slic3r::AI::SemanticColoring;
    auto p = fixture(); p.components = {component(0,"cloth",2)};
    p.components.front().source = p.palette[2].rgb; p.optimize();
    SC::FaceColors roots {{0,p.palette[2].rgb}};
    SC::SubfaceColors children {{0,{1,0},p.palette[0].rgb,1.f}};
    const auto before = children;
    Slic3r::GUI::apply_portrait_color_plan(p,roots,children);
    REQUIRE(children == before);
}
TEST_CASE("Native mixed roots never enter whole-face color optimization", "[PortraitColorPlan]") {
    using namespace Slic3r;
    using namespace Slic3r::AI;
    namespace SC = SemanticColoring;
    const auto mesh = its_make_cube(10,10,10);
    auto surface = BeautySurface::build(mesh,{});
    Slic3r::GUI::PortraitShapeDetails details;
    details.subjects = {"person"};
    auto& locks = details.locks;
    locks.geometry_id = surface->geometry_id; locks.face_count = mesh.indices.size();
    locks.source_sha256 = std::string(64,'a'); locks.evidence_sha256 = std::string(64,'b');
    locks.runtime_sha256 = std::string(64,'c'); locks.policy_sha256 = std::string(64,'d');
    ShapeLock brow; brow.subject_id = "person"; brow.parent_label = "lb"; brow.label = "lb";
    brow.status = "VALID_SHAPE"; brow.view_support = 2; brow.locked_faces = {0}; locks.locks = {brow};
    auto p = fixture();
    details.base_colors.assign(locks.face_count,RGBA{1,1,1,1});
    SC::Analysis analysis; analysis.geometry_id = locks.geometry_id;
    analysis.face_labels.assign(locks.face_count,SC::Label::Clothes);
    analysis.face_confidence.assign(locks.face_count,.95f);
    analysis.subface_labels = {{1,{1,0},SC::Label::BodySkin,.95f,4}};
    SC::FaceColors previous;
    for (size_t f = 0; f < locks.face_count; ++f) previous.push_back({f,p.palette[2].rgb});
    SC::SubfaceColors children {{2,{1,0},p.palette[0].rgb,1.f}};
    const auto before = children;
    auto plan = Slic3r::GUI::build_portrait_color_plan(details,mesh,*surface,analysis,p.palette,previous,children);
    for (const auto& c : plan.components) for (const auto& key : c.leaves) {
        REQUIRE(key.source_face_id != 1);
        REQUIRE(key.source_face_id != 2);
    }
    Slic3r::GUI::apply_portrait_color_plan(plan,previous,children);
    REQUIRE(children == before);
    SECTION("Already evidenced skin children repair their color without expanding into the root") {
        analysis.subface_labels.front().label = SC::Label::FaceSkin;
        children = {{1,{1,0},p.palette[4].rgb,.88f},{1,{1,1},p.palette[1].rgb,1.f}};
        const auto original_roots = previous;
        const auto revised = Slic3r::GUI::build_portrait_color_plan(details,mesh,*surface,analysis,p.palette,previous,children);
        Slic3r::GUI::apply_portrait_color_plan(revised,previous,children);
        REQUIRE(previous[1] == original_roots[1]);
        REQUIRE(children.front().color == p.palette[0].rgb);
        REQUIRE_THAT(children.front().confidence,Catch::Matchers::WithinAbs(.88,1e-6));
        REQUIRE(children.back().color == p.palette[1].rgb);
    }
    SECTION("Out of palette child colors are rejected") {
        children.front().color = {.3f,.8f,.2f};
        REQUIRE_THROWS(Slic3r::GUI::build_portrait_color_plan(details,mesh,*surface,analysis,p.palette,previous,children));
    }
    SECTION("A native eye child inside a skin child blocks that skin repair") {
        analysis.subface_labels = {{1,{1,0},SC::Label::FaceSkin,.95f,4},{1,{2,0},SC::Label::EyeSclera,.95f,4}};
        children = {{1,{1,0},p.palette[4].rgb,1.f}};
        const auto unchanged = children;
        const auto conflict = Slic3r::GUI::build_portrait_color_plan(details,mesh,*surface,analysis,p.palette,previous,children);
        Slic3r::GUI::apply_portrait_color_plan(conflict,previous,children);
        REQUIRE(children == unchanged);
    }
    SECTION("A second person without shape locks still prevents coarse native attribution") {
        details.subjects.push_back("other-person");
        const auto multi = Slic3r::GUI::build_portrait_color_plan(details,mesh,*surface,analysis,p.palette,previous,before);
        for (const auto& c : multi.components) for (const auto& key : c.leaves) REQUIRE(key.source_face_id == 0);
    }
    SECTION("Historical contexts without complete subjects cannot attribute coarse native masks") {
        details.subjects.clear();
        const auto old = Slic3r::GUI::build_portrait_color_plan(details,mesh,*surface,analysis,p.palette,previous,before);
        for (const auto& c : old.components) for (const auto& key : c.leaves) REQUIRE(key.source_face_id == 0);
    }
}

TEST_CASE("A preserved R4 portrait creates four source-bound R5 color decisions without recognition", "[.][PortraitR5Replay]") {
    using namespace Slic3r;
    using namespace Slic3r::AI;
    namespace SC = SemanticColoring;
    namespace GUI = Slic3r::GUI;
    using Json = nlohmann::json;
    const auto env = [](const char* key) {
        const auto value = boost::nowide::getenv(key); return value ? std::string(value) : std::string();
    };
    const boost::filesystem::path run(env("ORCA_R5_RUN")), output(env("ORCA_R5_COLOR_OUTPUT"));
    const boost::filesystem::path native_cache(env("ORCA_R5_NATIVE_CACHE"));
    if (run.empty() || output.empty() || native_cache.empty()) SKIP("Explicit preserved run, analysis cache and new output are required.");
    REQUIRE_FALSE(boost::filesystem::exists(output));
    const auto read = [](const boost::filesystem::path& path) {
        boost::filesystem::ifstream stream(path,std::ios::binary);
        if (!stream) throw std::runtime_error("Missing R5 input.");
        return std::string(std::istreambuf_iterator<char>(stream),{});
    };
    const auto manifest = Json::parse(read(run/"stage-manifest.json"));
    const auto baseline = Json::parse(read(run/"baseline/export.json"));
    const auto variant_manifest = Json::parse(read(run/"stage-baseline-variants.json"));
    for (const auto& entry : variant_manifest.at("files"))
        REQUIRE(model_artifact_sha256(run/entry.at("path").get<std::string>()) == entry.at("sha256"));
    const auto source_hash = manifest.at("source_sha256").get<std::string>();
    REQUIRE(model_artifact_sha256(run/"baseline/source.glb") == source_hash);
    for (const auto& entry : manifest.at("files"))
        REQUIRE(model_artifact_sha256(run/entry.at("path").get<std::string>()) == entry.at("sha256"));
    TriangleMesh mesh; ObjInfo colors; std::string error;
    REQUIRE(load_model_artifact(run/"baseline/source.glb",mesh,colors,error));
    auto surface = BeautySurface::build(mesh.its,colors.vertex_colors);
    REQUIRE(surface->geometry_id == manifest.at("geometry_id"));
    GUI::PortraitShapeDetails details;
    const auto boundary_file = run/"brows/shape-locks.json";
    const auto identity = Json::parse(read(boundary_file));
    const auto boundary_report = Json::parse(read(run/"brows/stage-report.json"));
    REQUIRE(model_artifact_sha256(boundary_file) == boundary_report.at("shape_lock_sha256"));
    REQUIRE(AI::beauty_leaf_digest(boundary_report.at("boundary_policy").dump()) == identity.at("boundary_policy_sha256"));
    details.locks = ShapeLockSet::decode(identity,surface->geometry_id,source_hash,mesh.its.indices.size(),
        manifest.at("evidence_sha256"),identity.at("runtime_sha256"),identity.at("policy_sha256"),
        manifest.at("evidence_sha256"),identity.at("boundary_policy_sha256"));
    details.base_colors = colors.face_colors.size() == mesh.its.indices.size() ? colors.face_colors :
        beauty_source_face_colors(mesh.its,colors.vertex_colors);
    const auto evidence = Json::parse(read(run/"baseline/replay/evidence.json"));
    details.subjects = evidence.at("subjects").get<std::vector<std::string>>();
    std::set<size_t> reserved;
    for (const auto& item : evidence.at("shape_details")) {
        for (const auto face : item.at("accepted_faces")) reserved.insert(face.get<size_t>());
        for (const auto face : item.at("rejected_faces")) reserved.insert(face.get<size_t>());
    }
    details.reserved_faces.assign(reserved.begin(),reserved.end());
    SC::MeshSnapshot source; source.mesh = mesh.its; source.vertex_colors = colors.vertex_colors;
    source.face_colors = colors.face_colors; source.geometry_id = surface->geometry_id;
    source.content_id = SC::content_fingerprint(source);
    const auto native_hash = model_artifact_sha256(native_cache);
    REQUIRE(native_hash == env("ORCA_R5_NATIVE_CACHE_SHA256"));
    const auto native = Json::parse(read(native_cache)); SC::Analysis analysis;
    REQUIRE(SC::decode_analysis(native,source,native.at("body_identity"),native.at("face_identity"),analysis,error));
    boost::filesystem::create_directories(output);
    boost::filesystem::copy_file(native_cache,output/"native-analysis.json");
    const auto binary_colors = [](const std::string& bytes) {
        SC::FaceColors result;
        if (bytes.size()%16) throw std::invalid_argument("Invalid preserved face colors.");
        for (size_t offset = 0; offset < bytes.size(); offset += 16) {
            uint32_t face; SC::Color rgb;
            std::memcpy(&face,bytes.data()+offset,4); std::memcpy(rgb.data(),bytes.data()+offset+4,12);
            result.push_back({face,rgb});
        }
        return result;
    };
    Json reports = Json::array();
    for (const auto& card : baseline.at("palettes")) {
        const auto count = card.at("color_count").get<size_t>();
        std::vector<Plan::Slot> palette;
        for (const auto& item : card.at("palette")) palette.push_back({item.at("uid"),item.at("rgb").get<Plan::RGB>()});
        const auto name = "colors-"+std::to_string(count)+"-merged";
        const auto face_file = run/"baseline/variants"/(name+".overrides.bin");
        auto roots = binary_colors(read(face_file)); SC::SubfaceColors children;
        for (const auto& item : Json::parse(read(run/"baseline/variants"/(name+".subfaces.json"))))
            children.push_back({item.at("face_id"),{item.at("path").at("depth"),item.at("path").at("value")},item.at("color"),item.at("confidence")});
        auto plan = GUI::build_portrait_color_plan(details,mesh.its,*surface,analysis,palette,roots,children);
        const auto decisions = plan.encode();
        const auto root_before = std::map<size_t,SC::Color>(roots.begin(),roots.end());
        GUI::apply_portrait_color_plan(plan,roots,children);
        std::set<size_t> targeted;
        for (const auto& c : plan.components) if (!c.conflict && c.label != "imouth")
            for (const auto& leaf : c.leaves) targeted.insert(leaf.source_face_id);
        size_t changed_roots = 0;
        for (const auto& root : roots) {
            const auto before = root_before.find(root.first);
            if (!targeted.count(root.first)) { REQUIRE(before != root_before.end()); REQUIRE(root.second == before->second); }
            else if (before == root_before.end() || root.second != before->second) ++changed_roots;
        }
        const auto directory = output/("colors-"+std::to_string(count)); boost::filesystem::create_directories(directory);
        const auto write = [&](const char* name,const Json& doc) {
            boost::filesystem::ofstream stream(directory/name,std::ios::binary); stream << doc.dump(); stream.close(); REQUIRE(bool(stream));
        };
        write("portrait-color-plan.json",decisions);
        boost::filesystem::ofstream stream(directory/"face-colors.bin",std::ios::binary);
        for (const auto& item : roots) {
            const uint32_t face = uint32_t(item.first); stream.write(reinterpret_cast<const char*>(&face),4);
            stream.write(reinterpret_cast<const char*>(item.second.data()),12);
        }
        stream.close(); REQUIRE(bool(stream));
        auto subfaces = Json::array();
        for (const auto& child : children) subfaces.push_back({{"face_id",child.face_id},
            {"path",{{"depth",child.path.depth},{"value",child.path.value}}},{"color",child.color},{"confidence",child.confidence}});
        write("subface-colors.json",subfaces);
        reports.push_back({{"color_count",count},{"component_count",plan.components.size()},{"changed_roots",changed_roots},
            {"plan_sha256",model_artifact_sha256(directory/"portrait-color-plan.json")},
            {"face_colors_sha256",model_artifact_sha256(directory/"face-colors.bin")},
            {"subface_colors_sha256",model_artifact_sha256(directory/"subface-colors.json")},{"unused_decision_slots",decisions.at("unused_slots")}});
    }
    REQUIRE(model_artifact_sha256(native_cache) == native_hash);
    REQUIRE(model_artifact_sha256(run/"baseline/source.glb") == source_hash);
    boost::filesystem::ofstream report(output/"stage-report.json");
    report << Json{{"schema","orca.r5-color-report/v1"},{"code_status","PASS"},{"visual_status","PENDING_USER"},
        {"source_sha256",source_hash},{"geometry_id",surface->geometry_id},{"boundary_sha256",AI::beauty_leaf_digest(identity.dump())},
        {"native_analysis_sha256",native_hash},{"new_recognition_performed",false},{"material_tree_changed",false},
        {"extra_visibility","No validated full-body camera pixels supplied; extra slot allocation fails closed."},
        {"historical_material_tree_hashes_verified",false},{"variants",reports},{"production_enabled",false}}.dump(2);
    report.close(); REQUIRE(bool(report));
}
