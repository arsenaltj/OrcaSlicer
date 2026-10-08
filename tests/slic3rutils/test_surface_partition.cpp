#include "slic3r/GUI/AI/Model/SurfacePartition.hpp"
#include "slic3r/GUI/AI/Model/BeautySurfaceShapeLock.hpp"
#include "slic3r/GUI/AI/Model/BeautyCellRemap.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

namespace SP = Slic3r::AI::SurfacePartition;
using Json = nlohmann::json;
namespace {
Json polygon(std::initializer_list<std::array<double,3>> points) {
    return {{"polygon", points}, {"holes",Json::array()}};
}
Json fixture() {
    Json identity = {{"geometry_id",std::string(64,'a')},{"source_sha256",std::string(64,'b')},
        {"evidence_sha256",std::string(64,'c')},{"runtime_sha256",std::string(64,'d')},
        {"policy_sha256",std::string(64,'e')},{"baseline_sha256",std::string(64,'f')},
        {"boundary_policy_sha256",std::string(64,'1')},{"face_count",100000}};
    auto base = polygon({{1,0,0},{0,1,0},{0,0,1}});
    base.update({{"label","face"},{"parent_label","face"},{"subject_id","person"},{"kind","R6_PARENT"}});
    const auto narrow = polygon({{.65,.25,.10},{.64,.26,.10},{.14,.26,.60},{.15,.25,.60}});
    const Json views = Json::array({{{"family","front"},{"polygons",Json::array({narrow})}},
                                   {{"family","oblique"},{"polygons",Json::array({narrow})}}});
    return {{"schema","orca.surface-partition-request/v1"},{"identity",identity},
            {"triangle_budget",2000},{"existing_added_triangles",0},
            {"faces",Json::array({{{"source_face_id",1},{"baseline_triangle_count",1},
              {"base",Json::array({base})},{"layers",Json::array({{{"label","le"},{"parent_label","le"},
                  {"subject_id","person"},{"views",views}}})}}})}};
}
}

TEST_CASE("Continuous contour cuts a narrow strip within a source triangle", "[SurfacePartition]") {
    const auto request = fixture();
    const auto result = SP::build(request);
    SP::validate(result,request.at("identity"));
    double selected = 0.;
    for (const auto& cell : result.at("faces").front().at("cells")) if (cell.at("label") == "le")
        selected += SP::area(SP::polygons(Json::array({cell}),true));
    REQUIRE_THAT(selected,Catch::Matchers::WithinAbs(.005,1e-8));
    REQUIRE(result.at("faces").front().at("cells").size() > 1);
    REQUIRE(SP::build(request) == result);
    auto recolored = result;
    const auto id = result.at("faces").front().at("cells").front().at("id");
    recolored["unrelated_palette"] = "draft";
    REQUIRE(recolored.at("faces").front().at("cells").front().at("id") == id);
}

TEST_CASE("Iris intersection and eye white form disjoint cells including holes", "[SurfacePartition]") {
    auto request = fixture();
    auto& layers = request["faces"][0]["layers"];
    auto iris = layers[0]; iris["label"] = "iris-le";
    iris["envelope_views"] = layers[0]["views"];
    const auto center = polygon({{.45,.253,.297},{.448,.255,.297},{.446,.255,.299},{.448,.253,.299}});
    for (auto& view : iris["views"]) view["polygons"] = Json::array({center});
    layers.insert(layers.begin(),iris);
    const auto result = SP::build(request);
    bool iris_found=false, eye_found=false;
    for (const auto& cell : result.at("faces")[0].at("cells")) {
        iris_found |= cell.at("label") == "iris-le"; eye_found |= cell.at("label") == "le";
    }
    REQUIRE(iris_found); REQUIRE(eye_found);
    SP::validate(result,request.at("identity"));
}

TEST_CASE("Clipping refuses bad coverage duplicate cameras ownership and identity", "[SurfacePartition]") {
    auto request = fixture();
    SECTION("Same camera is not independent") {
        request["faces"][0]["layers"][0]["views"][1]["family"] = "front";
        REQUIRE_THROWS(SP::build(request));
    }
    SECTION("Missing or overlapping base siblings") {
        request["faces"][0]["base"].push_back(request["faces"][0]["base"][0]);
        REQUIRE_THROWS(SP::build(request));
    }
    SECTION("Cross-eye layer") {
        auto other = request["faces"][0]["layers"][0]; other["label"]="re"; other["parent_label"]="re";
        request["faces"][0]["layers"].push_back(other); REQUIRE_THROWS(SP::build(request));
    }
    SECTION("Another person owns the parent") {
        request["faces"][0]["base"][0]["subject_id"]="another person";
        REQUIRE_THROWS(SP::build(request));
    }
    SECTION("A preserved eye cannot be relabeled as the other eye") {
        request["faces"][0]["base"][0]["fallback_label"]="re";
        REQUIRE_THROWS(SP::build(request));
    }
    SECTION("Face mapping drift") { request["faces"][0]["source_face_id"]=100000; REQUIRE_THROWS(SP::build(request)); }
    SECTION("All identity hashes are checked") {
        const auto result=SP::build(request);
        for (const auto field : {"geometry_id","source_sha256","evidence_sha256","runtime_sha256","policy_sha256","baseline_sha256","boundary_policy_sha256"}) {
            auto drift=request.at("identity"); drift[field]=std::string(64,'2'); REQUIRE_THROWS(SP::validate(result,drift));
        }
    }
    SECTION("Output content drift") { auto result=SP::build(request); result["faces"][0]["cells"][0]["label"]="re"; REQUIRE_THROWS(SP::validate(result,request.at("identity"))); }
}

TEST_CASE("Exhausted contour budget retains only the affected R6 root", "[SurfacePartition]") {
    auto request=fixture(); request["triangle_budget"]=0;
    const auto result=SP::build(request);
    REQUIRE(result.at("faces")[0].at("status") == "R6_LOCAL_BUDGET_FALLBACK");
    REQUIRE(result.at("faces")[0].at("cells").size() == 1);
    REQUIRE(result.at("added_triangles") == 0);
    REQUIRE(result.at("faces")[0].at("cells")[0].at("label") == "face");
}

TEST_CASE("Surface lock v3 binds content-addressed cells and rejects unsafe references", "[SurfacePartition][BeautyShapeLock]") {
    using Lock=Slic3r::AI::BeautySurfaceShapeLock;
    const auto request=fixture(); const auto partition=SP::build(request);
    auto lock=request.at("identity");
    const std::string file_hash(64,'3');
    lock.update({{"schema","orca.beauty-shape-lock/v3"},{"partition_ref",{
        {"schema","orca.surface-partition-reference/v1"},{"path","surface-partitions/"+file_hash+".json"},{"sha256",file_hash}}}});
    std::vector<std::string> cells;
    for (const auto& cell : partition.at("faces")[0].at("cells")) if (cell.at("label") == "le") cells.push_back(cell.at("id"));
    std::sort(cells.begin(),cells.end());
    lock["locks"]=Json::array({{{"subject_id","person"},{"parent_label","le"},{"label","le"},
        {"status","PROTECTED_SHAPE_UNCERTAIN"},{"view_support",2},{"reasons",Json::array({"FIT_RISK"})},
        {"locked_cells",cells},{"nested_cells",Json::array()},{"periocular_cells",Json::array()}}});
    REQUIRE_NOTHROW(Lock::decode(lock,partition,request.at("identity"),file_hash));
    SECTION("Path traversal") { lock["partition_ref"]["path"]="../outside.json"; REQUIRE_THROWS(Lock::decode(lock,partition,request.at("identity"),file_hash)); }
    SECTION("Wrong eye") { lock["locks"][0]["label"]="re"; lock["locks"][0]["parent_label"]="re"; REQUIRE_THROWS(Lock::decode(lock,partition,request.at("identity"),file_hash)); }
    SECTION("Nested leaves eye") { lock["locks"][0]["nested_cells"]=Json::array({cells.front()}); REQUIRE_THROWS(Lock::decode(lock,partition,request.at("identity"),file_hash)); }
}

TEST_CASE("Adjacent source faces share all contour intersections across UV seams", "[SurfacePartition]") {
    auto request=fixture();
    request["faces"][0]["source_vertices"]={0,1,2};
    const auto strip=polygon({{.65,.25,.10},{.64,.26,.10},{-.16,.26,.90},{-.15,.25,.90}});
    for (auto& view : request["faces"][0]["layers"][0]["views"]) view["polygons"]=Json::array({strip});
    auto neighbor=request["faces"][0]; neighbor["source_face_id"]=2; neighbor["source_vertices"]={2,1,3};
    neighbor["layers"]=Json::array(); request["faces"].push_back(neighbor);
    const auto result=SP::build(request);
    SP::validate(result,request.at("identity"));
    REQUIRE(result.at("seam_inserted_vertices").get<size_t>()>=2);
    REQUIRE(result.at("faces")[1].at("triangle_count").get<size_t>()>1);
    std::set<int64_t> first,second;
    for (const auto& c : result.at("faces")[0].at("cells")) for (const auto& v : c.at("polygon"))
        if (std::abs(v[0].get<double>())<1e-9) first.insert(int64_t(std::llround(v[1].get<double>()*SP::scale)));
    for (const auto& c : result.at("faces")[1].at("cells")) for (const auto& v : c.at("polygon"))
        if (std::abs(v[2].get<double>())<1e-9) second.insert(int64_t(std::llround(v[1].get<double>()*SP::scale)));
    REQUIRE(first==second);
}

TEST_CASE("New detail clips to confirmed parent leaves and preserves mixed siblings", "[SurfacePartition]") {
    auto request=fixture();
    auto& layer=request["faces"][0]["layers"][0];
    layer["parent_polygons"]=Json::array({polygon({{1,0,0},{.5,.5,0},{.5,0,.5}})});
    const auto result=SP::build(request);
    const auto parent=SP::polygons(layer.at("parent_polygons"),true);
    double selected=0.;
    for (const auto& cell : result.at("faces")[0].at("cells")) if (cell.at("label")=="le") {
        const auto region=SP::polygons(Json::array({cell}),true);
        selected+=SP::area(region);
        REQUIRE_THAT(SP::area(diff_ex(region,parent)),Catch::Matchers::WithinAbs(0.,1e-9));
    }
    REQUIRE(selected>0.);
    REQUIRE(selected<.005);
    SP::validate(result,request.at("identity"));
}

TEST_CASE("Source eye line holes do not paint the eye white", "[SurfacePartition]") {
    auto request=fixture();
    const Json outside=Json::array({{.7,.15,.15},{.4,.45,.15},{.1,.45,.45},{.4,.15,.45}});
    const Json hole=Json::array({{.6,.2,.2},{.4,.4,.2},{.2,.4,.4},{.4,.2,.4}});
    auto& layer=request["faces"][0]["layers"][0];
    layer["label"]="periocular-le";
    request["curve_library"]={{"line",Json::array()}, {"white",Json::array()}};
    for (const auto& p : outside) request["curve_library"]["line"].push_back({p[1],p[2]});
    for (const auto& p : hole) request["curve_library"]["white"].push_back({p[1],p[2]});
    for (auto& view : layer["views"]) {
        view.erase("polygons");
        view["matrix"]={{-1,1,0},{-1,0,1},{1,0,0}};
        view["contour_ids"]={"line"}; view["exclude_contour_ids"]={"white"};
    }
    const auto result=SP::build(request);
    const auto white=SP::polygons(Json::array({{{"polygon",hole},{"holes",Json::array()}}}),true);
    bool found=false;
    for (const auto& cell : result.at("faces")[0].at("cells")) if (cell.at("label")=="periocular-le") {
        found=true;
        REQUIRE_THAT(SP::area(intersection_ex(SP::polygons(Json::array({cell}),true),white)),Catch::Matchers::WithinAbs(0.,1e-9));
    }
    REQUIRE(found);
    SP::validate(result,request.at("identity"));
}

TEST_CASE("Seam budget exhaustion retains a local proposal without losing complete coverage", "[SurfacePartition]") {
    auto request=fixture();
    const auto strip=polygon({{.65,.25,.10},{.64,.26,.10},{-.16,.26,.90},{-.15,.25,.90}});
    for (auto& view : request["faces"][0]["layers"][0]["views"]) view["polygons"]=Json::array({strip});
    request["triangle_budget"]=SP::build(request).at("added_triangles");
    request["faces"][0]["source_vertices"]={0,1,2};
    auto neighbor=request["faces"][0]; neighbor["source_face_id"]=2; neighbor["source_vertices"]={2,1,3};
    neighbor["layers"]=Json::array(); request["faces"].push_back(neighbor);
    const auto result=SP::build(request);
    REQUIRE(result.at("faces")[0].at("status")=="R6_LOCAL_SEAM_BUDGET_FALLBACK");
    REQUIRE(result.at("added_triangles").get<size_t>()<=request.at("triangle_budget").get<size_t>());
    REQUIRE(result.at("faces")[1].at("cells").front().at("label")=="face");
    SP::validate(result,request.at("identity"));
}

namespace {
Json incremental_fixture() {
    auto request=fixture();
    request["faces"][0]["layers"][0]["label"]="rb";
    request["faces"][0]["layers"][0]["parent_label"]="rb";
    request["faces"][0]["source_vertices"]={0,1,2};
    auto eye=fixture().at("faces")[0]; eye["source_face_id"]=2; eye["source_vertices"]={3,4,5};
    request["faces"].push_back(eye);
    const auto original=SP::build(request);
    request["incremental_baseline"]=original;
    request["baseline_partition_sha256"]=original.at("partition_sha256");
    request["existing_added_triangles"]=original.at("added_triangles");
    request["identity"]["boundary_policy_sha256"]=std::string(64,'2');
    request["frozen_cell_ids"]=Json::array();
    for (size_t i=0;i<2;++i) {
        auto& face=request["faces"][i];
        face["preserve_color_provenance"]=true;
        face["base"]=original.at("faces")[i].at("cells");
        face["baseline_triangle_count"]=original.at("faces")[i].at("triangle_count");
        if (i==1) {
            face["layers"]=Json::array();
            for (const auto& cell : face.at("base")) request["frozen_cell_ids"].push_back(cell.at("id"));
        } else {
            auto& layer=face["layers"][0]; layer["parent_polygons"]=face.at("base");
            const auto wider=polygon({{.66,.24,.10},{.62,.28,.10},{.12,.28,.60},{.16,.24,.60}});
            for (auto& view : layer["views"]) view["polygons"]=Json::array({wider});
        }
    }
    return request;
}
}

TEST_CASE("Local brow repair rejudges skin cells and preserves frozen eyes", "[SurfacePartition][BeautyShapeLock]") {
    const auto request=incremental_fixture();
    const auto result=SP::build(request);
    SP::validate(result,request.at("identity"));
    REQUIRE(SP::preserves_frozen_cells(request.at("incremental_baseline"),result,request.at("frozen_cell_ids")));
    REQUIRE(result.at("faces")[1].at("cells")==request.at("incremental_baseline").at("faces")[1].at("cells"));
    double selected=0.;
    for (const auto& cell : result.at("faces")[0].at("cells")) if (cell.at("label")=="rb")
        selected+=SP::area(SP::polygons(Json::array({cell}),true));
    REQUIRE_THAT(selected,Catch::Matchers::WithinAbs(.02,1e-8));
    REQUIRE(result.at("added_triangles").get<size_t>()>=request.at("existing_added_triangles").get<size_t>());
    for (const auto& cell : result.at("faces")[0].at("cells")) if (cell.at("label")=="face" && cell.contains("origin_cell_id")) {
        const auto& origin=cell.at("origin_cell_id");
        const auto& old=request.at("incremental_baseline").at("faces")[0].at("cells");
        const auto found=std::find_if(old.begin(),old.end(),[&](const Json& c) { return c.at("id")==origin; });
        REQUIRE(found!=old.end());
        REQUIRE(found->at("label")=="face");
    }
}

TEST_CASE("Incremental brow repair retains exact old cells when the budget is exhausted", "[SurfacePartition]") {
    auto request=incremental_fixture();
    const auto separate=polygon({{.8,.1,.1},{.75,.15,.1},{.7,.15,.15},{.75,.1,.15}});
    for (auto& view : request["faces"][0]["layers"][0]["views"]) view["polygons"]=Json::array({separate});
    auto& original=request["incremental_baseline"];
    request["triangle_budget"]=original.at("added_triangles");
    original["triangle_budget"]=request.at("triangle_budget");
    original.erase("partition_sha256"); original["partition_sha256"]=Slic3r::AI::beauty_leaf_digest(original.dump());
    request["baseline_partition_sha256"]=original.at("partition_sha256");
    const auto result=SP::build(request);
    REQUIRE(result.at("faces")[0].at("status")=="R8_LOCAL_BUDGET_FALLBACK");
    REQUIRE(result.at("faces")[0].at("cells")==original.at("faces")[0].at("cells"));
    REQUIRE(result.at("added_triangles")==original.at("added_triangles"));
    SP::validate(result,request.at("identity"));
}

TEST_CASE("Incremental repair blocks frozen ownership and source identity drift", "[SurfacePartition][BeautyShapeLock]") {
    auto request=incremental_fixture();
    SECTION("Original base cells cannot change") {
        request["faces"][0]["base"][0]["label"]="hair";
        REQUIRE_THROWS(SP::build(request));
    }
    SECTION("Other details cannot be repaired implicitly") {
        request["faces"][0]["layers"][0]["label"]="re";
        REQUIRE_THROWS(SP::build(request));
    }
    SECTION("Missing root cannot silently disappear") {
        request["faces"].erase(request["faces"].begin()+1);
        REQUIRE_THROWS(SP::build(request));
    }
    SECTION("Frozen cell cannot be cut") {
        for (const auto& c : request.at("incremental_baseline").at("faces")[0].at("cells"))
            request["frozen_cell_ids"].push_back(c.at("id"));
        REQUIRE_THROWS(SP::build(request));
    }
    SECTION("Source identity must remain bound") {
        request["identity"]["source_sha256"]=std::string(64,'3');
        REQUIRE_THROWS(SP::build(request));
    }
    SECTION("Baseline hash cannot be swapped") {
        request["baseline_partition_sha256"]=std::string(64,'4');
        REQUIRE_THROWS(SP::build(request));
    }
}

TEST_CASE("Source supported brow trim reclaims skin without modifying other details", "[SurfacePartition]") {
    auto request=incremental_fixture();
    request["faces"][0]["redefine_rb"]=true;
    const auto narrower=polygon({{.647,.253,.10},{.643,.257,.10},{.143,.257,.60},{.147,.253,.60}});
    for (auto& view : request["faces"][0]["layers"][0]["views"]) view["polygons"]=Json::array({narrower});
    const auto result=SP::build(request);
    double selected=0.; bool reclaimed=false;
    for (const auto& c : result.at("faces")[0].at("cells")) {
        if (c.at("label")=="rb") selected+=SP::area(SP::polygons(Json::array({c}),true));
        if (c.value("kind","")=="R8_SOURCE_SKIN_RECLAIM") {
            reclaimed=true; REQUIRE(c.at("label")=="face"); REQUIRE(c.contains("origin_cell_id"));
        }
    }
    REQUIRE(reclaimed);
    REQUIRE_THAT(selected,Catch::Matchers::WithinAbs(.002,1e-8));
    REQUIRE(SP::preserves_frozen_cells(request.at("incremental_baseline"),result,request.at("frozen_cell_ids")));
    SP::validate(result,request.at("identity"));
}

namespace {
Json parent_repair_fixture() {
    auto request=incremental_fixture();
    request["repair_scope"]="parent";
    auto& face=request["faces"][0];
    face["parent_repair"]=true;
    auto& layer=face["layers"][0];
    layer["label"]="skin"; layer["parent_label"]="skin";
    layer["parent_polygons"]=Json::array();
    for (const auto& cell : face.at("base")) {
        if (cell.at("label")=="face") layer["parent_polygons"].push_back(cell);
        else request["frozen_cell_ids"].push_back(cell.at("id"));
    }
    return request;
}
}

TEST_CASE("Parent clipping preserves frozen details and inherited siblings", "[SurfacePartition][PortraitSurfaceOwnership]") {
    const auto request=parent_repair_fixture();
    const auto result=SP::build(request);
    SP::validate(result,request.at("identity"));
    REQUIRE(SP::preserves_frozen_cells(request.at("incremental_baseline"),result,request.at("frozen_cell_ids")));
    bool selected=false,retained=false;
    const auto allowed=SP::polygons(request.at("faces")[0].at("layers")[0].at("parent_polygons"),true);
    for (const auto& cell : result.at("faces")[0].at("cells")) {
        if (cell.at("label")=="skin") {
            selected=true;
            REQUIRE(cell.contains("origin_cell_id"));
            REQUIRE_THAT(SP::area(diff_ex(SP::polygons(Json::array({cell}),true),allowed)),Catch::Matchers::WithinAbs(0.,1e-9));
        }
        if (cell.at("label")=="face") {
            retained=true;
            REQUIRE(cell.contains("origin_cell_id"));
        }
    }
    REQUIRE(selected); REQUIRE(retained);
}

TEST_CASE("Parent repair refuses detail overreach and cross subject support", "[SurfacePartition][PortraitSurfaceOwnership]") {
    auto request=parent_repair_fixture();
    SECTION("Frozen sibling is outside allowed parent scope") {
        request["faces"][0]["layers"][0]["parent_polygons"]=request.at("faces")[0].at("base");
        REQUIRE_THROWS(SP::build(request));
    }
    SECTION("Another person's evidence cannot cut the parent") {
        request["faces"][0]["layers"][0]["subject_id"]="other-person";
        REQUIRE_THROWS(SP::build(request));
    }
    SECTION("Parent repair is an explicit request scope") {
        request.erase("repair_scope"); REQUIRE_THROWS(SP::build(request));
    }
    SECTION("Correlated crops cannot provide two independent votes") {
        request["faces"][0]["layers"][0]["views"][1]["family"]="front";
        REQUIRE_THROWS(SP::build(request));
    }
}

TEST_CASE("Contradictory parent cuts retain the original appearance", "[SurfacePartition][PortraitSurfaceOwnership]") {
    auto request=parent_repair_fixture();
    auto conflict=request["faces"][0]["layers"][0];
    conflict["label"]="hair"; conflict["parent_label"]="hair";
    request["faces"][0]["layers"].push_back(conflict);
    const auto result=SP::build(request);
    REQUIRE(result.at("faces")[0].at("cells")==request.at("incremental_baseline").at("faces")[0].at("cells"));
    SP::validate(result,request.at("identity"));
}

TEST_CASE("Parent clipping budget exhaustion retains the exact local baseline", "[SurfacePartition][PortraitSurfaceOwnership]") {
    auto request=parent_repair_fixture();
    auto& baseline=request["incremental_baseline"];
    request["triangle_budget"]=baseline.at("added_triangles");
    baseline["triangle_budget"]=request.at("triangle_budget");
    baseline.erase("partition_sha256"); baseline["partition_sha256"]=Slic3r::AI::beauty_leaf_digest(baseline.dump());
    request["baseline_partition_sha256"]=baseline.at("partition_sha256");
    const auto result=SP::build(request);
    REQUIRE(result.at("faces")[0].at("status")=="R9_LOCAL_BUDGET_FALLBACK");
    REQUIRE(result.at("faces")[0].at("cells")==baseline.at("faces")[0].at("cells"));
    REQUIRE(result.at("added_triangles")==baseline.at("added_triangles"));
    SP::validate(result,request.at("identity"));
}

TEST_CASE("Parent cut budgeting measures local geometry instead of charging every root equally", "[SurfacePartition][PortraitSurfaceOwnership]") {
    auto request=parent_repair_fixture();
    auto& face=request["faces"][0];
    const auto cost=SP::incremental_cut_cost(face,request.at("identity"));
    auto cells=SP::partition_face(face,Json::object());
    SP::finish_cells(cells,face.at("source_face_id"),request.at("identity"));
    size_t triangles=0;
    for (const auto& cell:cells) triangles+=cell.at("triangles").size();
    const auto baseline=face.at("baseline_triangle_count").get<size_t>();
    REQUIRE(cost.at("local_added_triangles").get<size_t>()==(triangles>baseline ? triangles-baseline : 0));
    REQUIRE(cost.at("estimated_added_triangles").get<size_t>()>=cost.at("local_added_triangles").get<size_t>());
    face["layers"]=Json::array();
    const auto unchanged=SP::incremental_cut_cost(face,request.at("identity"));
    REQUIRE(unchanged.at("estimated_added_triangles")==0);
}

TEST_CASE("Cell samples reject thin tessellation excursions and hole intrusion", "[SurfacePartition]") {
    auto cell=polygon({{.055403020,.5,.444596980},{.115466548,.514755495,.369777957},
        {.081638678,.668361322,.25},{.081638677,.668361323,.25},
        {.115466548,.514755496,.369777956},{.055403016,.5,.444596984}});
    cell["triangles"]=SP::legacy_triangles(SP::polygons(Json::array({cell}),true).front());
    REQUIRE_THROWS(SP::validate_cell_samples(Json::array({cell})));
    cell=polygon({{1,0,0},{0,1,0},{0,0,1}});
    cell["triangles"]=Json::array({cell.at("polygon")});
    REQUIRE_NOTHROW(SP::validate_cell_samples(Json::array({cell})));
    cell["holes"]=Json::array({{{.4,.3,.3},{.3,.4,.3},{.3,.3,.4}}});
    REQUIRE_THROWS(SP::validate_cell_samples(Json::array({cell})));
}

TEST_CASE("Constrained contour triangulation retains collinear seam vertices", "[SurfacePartition]") {
    // Actual failing face 182089: GLU emitted a collinear ear containing the
    // shared edge split, plus an unsplit source triangle.
    const auto cell=polygon({{1,0,0},{.697257154,.302742846,0},{0,1,0},{0,0,1}});
    const auto shape=SP::polygons(Json::array({cell}),true).front();
    const auto result=SP::triangles(shape);
    REQUIRE(result.size()==2);
    REQUIRE(result==SP::triangles(shape));
    size_t uses=0;
    for(const auto& t:result) {
        const auto contour=SP::contour(t,true);
        REQUIRE(Slic3r::AI::SurfaceTriangulation::orientation(contour.points[0],contour.points[1],contour.points[2])>0);
        for(const auto& p:contour.points) if(p==Slic3r::Point(302742846,0)) ++uses;
    }
    REQUIRE(uses==2);
}

TEST_CASE("Legacy contour drafts rebuild without changing cells or paint", "[SurfacePartition]") {
    auto request=fixture();request["faces"][0]["layers"]=Json::array();
    request["faces"][0]["base"][0]["polygon"]={{1,0,0},{.697257154,.302742846,0},{0,1,0},{0,0,1}};
    auto old=SP::build(request);old.erase("tessellation_version");
    old["faces"][0]["cells"][0]["polygon"]={{1,0,0},{.697257154,.302742846,0},{0,1,0},{0,0,1}};
    old["faces"][0]["triangle_count"]=2;old["added_triangles"]=1;
    for(auto& f:old["faces"]) for(auto& c:f["cells"])
        c["triangles"]=SP::legacy_triangles(SP::polygons(Json::array({c}),true).front());
    old.erase("partition_sha256");old["partition_sha256"]=Slic3r::AI::beauty_leaf_digest(old.dump());
    REQUIRE_NOTHROW(SP::validate(old,request.at("identity")));
    auto rebuilt=SP::rebuild_tessellation(old);
    REQUIRE(rebuilt.at("tessellation_version")==1);
    REQUIRE(old.at("faces")[0].at("cells")[0].at("id")==rebuilt.at("faces")[0].at("cells")[0].at("id"));
    REQUIRE(SP::cell_boundary(old.at("faces")[0].at("cells")[0])==SP::cell_boundary(rebuilt.at("faces")[0].at("cells")[0]));
    REQUIRE(SP::rebuild_tessellation(rebuilt)==rebuilt);
    for(const bool face_count : {false,true}) {
        auto tampered=rebuilt;
        if(face_count) tampered["faces"][0]["triangle_count"]=0;
        else tampered["added_triangles"]=0;
        tampered.erase("partition_sha256");tampered["partition_sha256"]=Slic3r::AI::beauty_leaf_digest(tampered.dump());
        REQUIRE_THROWS(SP::validate(tampered,request.at("identity")));
    }
    REQUIRE_THROWS(SP::rebuild_tessellation(old,[]{return true;}));
    auto tampered=old;tampered["source_sha256"]=std::string(64,'0');
    REQUIRE_THROWS(SP::rebuild_tessellation(tampered));
    Json edits={{"schema","orca.beauty-cell-edit/v1"},{"geometry_id",old.at("geometry_id")},
        {"source_sha256",old.at("source_sha256")},{"mapping_sha256",old.at("partition_sha256")},
        {"boundary_sha256","old"},{"triangle_count",100001},
        {"colors",Json::array({{old.at("faces")[0].at("cells")[0].at("id"),{1.,0.,0.}}})},
        {"selected",{1,2}},{"protected",{1,2}},{"foreground",Json::array()},{"domain",{0,1,2,99999,100000}}};
    const auto mapped=Slic3r::AI::remap_cell_edits(edits,old,rebuilt,"old","new");
    REQUIRE(mapped.at("colors")==edits.at("colors"));
    REQUIRE(mapped.at("selected")==edits.at("selected"));
    REQUIRE(mapped.at("protected")==edits.at("protected"));
    REQUIRE(mapped.at("domain")==edits.at("domain"));
    REQUIRE(mapped.at("mapping_sha256")==rebuilt.at("partition_sha256"));
    REQUIRE_THROWS(Slic3r::AI::remap_cell_edits(edits,old,rebuilt,"wrong","new"));
}

TEST_CASE("Constrained triangulation rejects intersecting rings without discarding thin regions", "[SurfacePartition]") {
    auto thin=polygon({{1,0,0},{.5,.5,0},{.499999999,.5,.000000001}});
    REQUIRE_NOTHROW(SP::triangles(SP::polygons(Json::array({thin}),true).front()));
    auto crossing=polygon({{.9,0,.1},{.3,.6,.1},{.9,.1,0},{.3,.1,.6}});
    REQUIRE_THROWS(SP::triangles(SP::polygons(Json::array({crossing}),true).front()));
}

TEST_CASE("Point touching contour lobes preserve their boundary without a zero area bridge", "[SurfacePartition]") {
    auto pinched=polygon({{1,0,0},{.5,.5,0},{.6,.2,.2},{.4,.4,.2},{.2,.4,.4},
        {.6,.2,.2},{.5,0,.5}});
    const auto shape=SP::polygons(Json::array({pinched}),true).front();
    const auto result=SP::triangles(shape);
    REQUIRE_FALSE(result.empty());
    for(const auto& t:result) {
        const auto p=SP::contour(t,true);
        REQUIRE(Slic3r::AI::SurfaceTriangulation::orientation(p.points[0],p.points[1],p.points[2])>0);
    }
}

TEST_CASE("Translated thin triangles retain exact area throughout contour export", "[SurfacePartition]") {
    // Actual face 522922: its valid half-grid-square triangle was rejected by
    // floating shoelace cancellation after the old degenerate ears were fixed.
    const Slic3r::Polygon thin({Slic3r::Point(387203201,205286082),
        Slic3r::Point(387203202,205286080),Slic3r::Point(387203203,205286079)});
    REQUIRE(Slic3r::AI::SurfaceTriangulation::signed_twice_area(thin)==1);
    const auto encoded=SP::encode_contour(thin,true);
    const auto decoded=SP::polygons(Json::array({{{"polygon",encoded},{"holes",Json::array()}}}),true);
    REQUIRE(Slic3r::AI::SurfaceTriangulation::signed_twice_area(decoded.front().contour)==1);
    REQUIRE(SP::area(decoded)>0.);
    REQUIRE(SP::triangles(decoded.front()).size()==1);
    auto reversed=thin;reversed.reverse();
    REQUIRE(SP::encode_contour(reversed,true)==encoded);
    auto collapsed=encoded;collapsed[2]={.407510719,.387203203,.205286078};
    REQUIRE_THROWS(SP::contour(collapsed,true));
}
