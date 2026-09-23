#include <catch2/catch_all.hpp>
#include "libslic3r/AABBTreeIndirect.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Preset.hpp"
#include "slic3r/GUI/AI/Orca/OrcaPrintColorLayers.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <array>

using namespace Slic3r;
namespace {
using Json=nlohmann::json;
using Groups=std::vector<ExPolygons>;
Groups surface_groups(const std::filesystem::path& file) {
    std::ifstream input(file);if(!input)throw std::runtime_error("Missing projection trace.");
    Groups groups;std::string text;size_t group=0;
    while(std::getline(input,text)) {
        std::istringstream line(text);char kind;line>>kind;
        if(kind=='G'){line>>group;groups.resize(std::max(groups.size(),group+1));}
        else if(kind=='C' || kind=='H') {
            Points points;coord_t x,y;while(line>>x>>y)points.emplace_back(x,y);
            if(kind=='C')groups[group].emplace_back(Polygon(std::move(points)));
            else groups[group].back().holes.emplace_back(std::move(points));
        }
    }
    return groups;
}
}

// The optional real fixture is intentionally hidden from ordinary CI. It reads
// existing local G-code evidence and never submits generation or printer jobs.
TEST_CASE("Emitted wall samples bind to saved source faces with explicit ambiguity", "[ColorSurfaceBindingEvidence][.]")
{
    const char* project=std::getenv("ORCA_PROJECTION_PROJECT");
    const char* version=std::getenv("ORCA_PROJECTION_VERSION");
    const char* path_evidence=std::getenv("ORCA_SURFACE_GCODE_AUDIT");
    const char* trace_path=std::getenv("ORCA_TOOLPATH_PROJECTION_TRACES");
    const char* destination=std::getenv("ORCA_SURFACE_EVIDENCE");
    if(!project || !version || !path_evidence || !trace_path || !destination)SKIP("Explicit local model, saved targets, G-code audit, traces and empty destination required.");
    const std::filesystem::path root(destination),traces(trace_path);
    REQUIRE(std::filesystem::is_directory(root));REQUIRE(std::filesystem::is_empty(root));
    std::filesystem::create_directories(root/"model-backup"/"Metadata");
    Model model;model.set_backup_path((root/"model-backup").string());DynamicPrintConfig config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Enable};
    PlateDataPtrs plates;std::vector<Preset*> presets;bool bbl=false,orca=false;Semver number;
    REQUIRE(load_bbs_3mf(project,&config,&substitutions,&model,&plates,&presets,&bbl,&orca,&number,nullptr,LoadStrategy::LoadModel|LoadStrategy::LoadConfig));
    release_PlateData_list(plates);for(auto* preset:presets)delete preset;
    auto complete=DynamicPrintConfig::full_print_config();complete.apply(config);config=std::move(complete);
    REQUIRE(model.objects.size()==1);REQUIRE(model.objects.front()->volumes.size()==1);
    const auto* volume=model.objects.front()->volumes.front();
    const auto identity=GUI::OrcaPrintColorLayers::identity(*model.objects.front(),config);
    Json saved,audit;{std::ifstream input(version);input>>saved;}{std::ifstream input(path_evidence);input>>audit;}
    saved=saved.at("result");const auto labels=saved.at("face_targets").get<std::vector<size_t>>();
    std::vector<size_t> slots;for(const auto& target:saved.at("targets"))slots.push_back(target.at("physical_slot").get<size_t>());
    REQUIRE(slots.size()==12);REQUIRE(labels.size()==volume->mesh().its.indices.size());
    TriangleSelector expected(volume->mesh()),actual(volume->mesh());
    for(size_t face=0;face<labels.size();++face) {
        if(labels[face]>=slots.size() || slots[labels[face]]>=4)throw std::runtime_error("Invalid saved face assignment.");
        expected.set_facet(int(face),EnforcerBlockerType(int(EnforcerBlockerType::Extruder1)+int(slots[labels[face]])));
    }
    actual.deserialize(volume->mmu_segmentation_facets.get_data());
    REQUIRE(expected.serialize()==actual.serialize());
    Print print;print.set_status_silent();print.set_plate_index(0);print.set_plate_origin(Vec3d::Zero());print.apply(model,config);
    REQUIRE(print.objects().size()==1);auto* object=print.get_object(0);object->slice();
    REQUIRE(object->layers().size()==500);REQUIRE(object->instances().size()==1);
    REQUIRE(object->instances().front().shift==Point(150000000,135000000));
    TriangleMesh mesh=volume->mesh();mesh.transform(object->trafo_centered()*volume->get_matrix());
    REQUIRE(mesh.its.indices==volume->mesh().its.indices);
    const auto tree=AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(mesh.its.vertices,mesh.its.indices);
    using Faces=decltype(mesh.its.indices);
    std::array<Faces,4> by_slot;
    for(size_t face=0;face<labels.size();++face)by_slot[slots[labels[face]]].push_back(mesh.its.indices[face]);
    std::array<AABBTreeIndirect::Tree<3,float>,4> trees;
    for(size_t slot=0;slot<4;++slot){REQUIRE_FALSE(by_slot[slot].empty());trees[slot]=AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(mesh.its.vertices,by_slot[slot]);}
    std::map<size_t,Groups> projected;
    for(const auto& segment:audit.at("focus_segments")) {
        const size_t id=segment.at("layer_id").get<size_t>();
        if(projected.count(id))continue;
        const auto groups=surface_groups(traces/("mm-exact-1-merged-"+std::to_string(id)+".txt"));
        REQUIRE(groups.size()==slots.size());Groups physical(4);
        for(size_t target=0;target<slots.size();++target)append(physical[slots[target]],groups[target]);
        for(auto& polygons:physical)polygons=union_ex(polygons);
        projected.emplace(id,std::move(physical));
    }
    Json samples=Json::array();double represented=0.;
    constexpr double step=0.1,boundary_margin=0.02;
    for(const auto& segment:audit.at("focus_segments")) {
        if(segment.at("role")!="Outer wall")continue;
        const size_t id=segment.at("layer_id").get<size_t>(),tool=segment.at("tool").get<size_t>();
        const auto& layer=*object->layers().at(id);
        const Vec2d a(segment.at("start").at(0).get<double>(),segment.at("start").at(1).get<double>());
        const Vec2d b(segment.at("end").at(0).get<double>(),segment.at("end").at(1).get<double>());
        const double length=(b-a).norm();const size_t count=std::max<size_t>(1,size_t(std::ceil(length/step)));
        const double width=segment.at("width_mm").get<double>(),limit=width+layer.height;
        for(size_t index=0;index<count;++index) {
            const Vec2d xy=a+(b-a)*((double(index)+0.5)/double(count));
            const Vec3d query(xy.x(),xy.y(),layer.slice_z);size_t face=0;Vec3d nearest;
            const double squared=AABBTreeIndirect::squared_distance_to_indexed_triangle_set(mesh.its.vertices,mesh.its.indices,tree,query,face,nearest);
            if(!std::isfinite(squared) || squared<0. || face>=labels.size())throw std::runtime_error("Invalid source surface query.");
            const size_t source_slot=slots[labels[face]];const double distance=std::sqrt(squared);
            double competing=std::numeric_limits<double>::infinity();
            for(size_t slot=0;slot<4;++slot)if(slot!=source_slot) {
                size_t other_face=0;Vec3d other;
                const double d=AABBTreeIndirect::squared_distance_to_indexed_triangle_set(mesh.its.vertices,by_slot[slot],trees[slot],query,other_face,other);
                if(!std::isfinite(d) || d<0.)throw std::runtime_error("Invalid competing surface query.");
                competing=std::min(competing,std::sqrt(d));
            }
            const auto& triangle=mesh.its.indices[face];
            const Vec3d edge1=mesh.its.vertices[triangle[1]].cast<double>()-mesh.its.vertices[triangle[0]].cast<double>();
            const Vec3d edge2=mesh.its.vertices[triangle[2]].cast<double>()-mesh.its.vertices[triangle[0]].cast<double>();
            const Vec3d normal=edge1.cross(edge2);const double norm=normal.norm();
            Json covers=Json::array();const Point point(scale_(xy.x()),scale_(xy.y()));
            for(size_t slot=0;slot<4;++slot)if(std::any_of(projected.at(id)[slot].begin(),projected.at(id)[slot].end(),[&](const ExPolygon& polygon){return polygon.contains(point);}))covers.push_back(slot);
            const bool nearby=distance<=limit,ambiguous=competing-distance<=boundary_margin;
            samples.push_back({{"layer_id",id},{"gcode_line",segment.at("line")},{"tool",tool},{"source_face",face},
                {"source_target",labels[face]},{"source_slot",source_slot},{"xy",{xy.x(),xy.y()}},{"slice_z_mm",layer.slice_z},
                {"nearest_surface_point",{nearest.x(),nearest.y(),nearest.z()}},{"surface_distance_mm",distance},
                {"competing_slot_distance_mm",competing},{"normal_z",norm>1e-12?Json(normal.z()/norm):Json(nullptr)},
                {"nearby_under_geometric_rule",nearby},{"ambiguous_slot_under_geometric_rule",ambiguous},
                {"source_slot_matches_tool",source_slot==tool},{"projected_slots",std::move(covers)},
                {"represented_length_mm",length/double(count)},{"proximity_limit_mm",limit}});
            represented+=length/double(count);
        }
    }
    Json result={{"scope","sampled emitted outer walls compared with saved source face assignments"},
        {"step_mm",step},{"competing_slot_margin_mm",boundary_margin},{"source_face_count",labels.size()},
        {"canonical_source_paint_matches_saved",true},{"represented_length_mm",represented},{"samples",std::move(samples)},
        {"color_fidelity_verified",false},{"physical_print_verified",false}};
    {std::ofstream output(root/"source-wall-samples.json");output<<result.dump(2);REQUIRE(bool(output));}
    REQUIRE(represented>0.);REQUIRE_FALSE(result.at("samples").empty());
    CHECK(GUI::OrcaPrintColorLayers::identity(*model.objects.front(),config)==identity);
}
