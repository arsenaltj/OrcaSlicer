#include <catch2/catch_all.hpp>
#include "slic3r/AI/AppearanceEditing/BeautyRegionPrecision.hpp"
#include "slic3r/AI/AppearanceEditing/BeautyDocument.hpp"
#include "slic3r/AI/AppearanceEditing/ModelFinishing.hpp"
#include "slic3r/AI/ModelArtifacts/ModelArtifact.hpp"
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <chrono>
#include <numeric>

using namespace Slic3r;
using namespace Slic3r::AI;
namespace {
indexed_triangle_set precision_grid(float scale=1,bool seams=false) {
    indexed_triangle_set mesh;
    for(int y=0;y<=28;++y)for(int x=0;x<=40;++x)mesh.vertices.emplace_back(x*scale,y*scale,0);
    for(int y=0;y<28;++y)for(int x=0;x<40;++x) {
        const int a=y*41+x;mesh.indices.emplace_back(a,a+1,a+42);mesh.indices.emplace_back(a,a+42,a+41);
    }
    if(seams) {
        indexed_triangle_set copies;
        for(const auto& t:mesh.indices){const int a=int(copies.vertices.size());for(int v:t)copies.vertices.push_back(mesh.vertices[size_t(v)]);copies.indices.emplace_back(a,a+1,a+2);}
        return copies;
    }
    return mesh;
}
std::vector<uint32_t> precision_regions(const BeautySurface& surface,float scale=1) {
    std::vector<uint32_t> result;
    for(const auto& c:surface.centers){const double x=c.x()/scale-20,y=c.y()/scale-14;result.push_back(x*x/100+y*y/16<1?2:1);}
    return result;
}
BeautyPuzzle precision_print(const BeautySurface& surface,const std::vector<uint32_t>& regions) {
    BeautyPuzzle result;result.geometry_id=surface.geometry_id;result.face_piece=regions;result.next_id=4;
    result.palette={{0,"#E8B49A","PLA",true},{4,"#B9514A","PLA",true}};result.paint_filament(1,0);result.paint_filament(2,4);
    if(std::find(regions.begin(),regions.end(),3)!=regions.end())result.paint_filament(3,0);
    result.validate(surface);return result;
}
double ellipse_border_error(const indexed_triangle_set& mesh,const BeautySurface& surface,const std::vector<uint32_t>& owners,float scale=1) {
    double error=0,length=0;
    for(size_t f=0;f<owners.size();++f)for(int e=0;e<3;++e) {
        const int32_t n=surface.face_neighbors[f][size_t(e)];if(n<0 || size_t(n)<=f || owners[f]==owners[size_t(n)])continue;
        const auto& t=mesh.indices[f];const Vec3d a=mesh.vertices[size_t(t[e])].cast<double>()/scale,b=mesh.vertices[size_t(t[(e+1)%3])].cast<double>()/scale;
        const auto c=(a+b)*.5;const double w=(b-a).norm(),r=std::sqrt((c.x()-20)*(c.x()-20)/100+(c.y()-14)*(c.y()-14)/16);
        error+=w*(r-1)*(r-1);length+=w;
    }
    REQUIRE(length>0);return std::sqrt(error/length);
}
template<class T> void raw_file(const boost::filesystem::path& path,const std::vector<T>& data) {
    boost::filesystem::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(data.data()),std::streamsize(data.size()*sizeof(T)));out.close();REQUIRE(bool(out));
}
}

TEST_CASE("Face interior contours retain surface coverage and reduce staircase error on an ellipse", "[BeautyRegionPrecision]") {
    const auto mesh=precision_grid();const auto surface=BeautySurface::build_for_appearance(mesh,{});const auto regions=precision_regions(*surface);
    const auto result=refine_selected_region_mesh(mesh,*surface,regions,2);REQUIRE(result.split_faces>0);REQUIRE(result.mesh.indices.size()>mesh.indices.size());
    const auto next=BeautySurface::build_for_appearance(result.mesh,{});
    CHECK(next->boundary_edges==surface->boundary_edges);CHECK(next->nonmanifold_edges==0);
    std::vector<double> covered(mesh.indices.size(),0);size_t inside_cuts=0;
    for(size_t f=0;f<result.parent_faces.size();++f){covered[result.parent_faces[f]]+=next->areas[f];if(result.face_region[f]!=regions[result.parent_faces[f]])++inside_cuts;}
    CHECK(inside_cuts>0);for(size_t f=0;f<covered.size();++f)CHECK_THAT(covered[f],Catch::Matchers::WithinAbs(surface->areas[f],1e-5));
    CHECK(ellipse_border_error(result.mesh,*next,result.face_region)<ellipse_border_error(mesh,*surface,regions));
    for(size_t v=0;v<result.vertices.size();++v) {
        const auto& ancestry=result.vertices[v];const Vec3d expected=(1-ancestry.fraction)*mesh.vertices[ancestry.a].cast<double>()+ancestry.fraction*mesh.vertices[ancestry.b].cast<double>();
        CHECK_THAT((expected-result.mesh.vertices[v].cast<double>()).norm(),Catch::Matchers::WithinAbs(0,3e-6));
    }
}

TEST_CASE("A fitted backtracking corner retains the original chain side despite reversed local tangents", "[BeautyRegionPrecision][BeautyRegionCurves]") {
    indexed_triangle_set plane;plane.vertices={Vec3f(0,0,0),Vec3f(40,0,0),Vec3f(40,40,0),Vec3f(0,40,0)};plane.indices={Vec3i32(0,1,2),Vec3i32(0,2,3)};
    const BeautyBoundaryContours::Neighbors neighbors{{{-1,-1,1}},{{0,-1,-1}}};
    const std::vector<Vec3f> points{Vec3f(5,10,0),Vec3f(20,11,0),Vec3f(15,11,0),Vec3f(35,10,0)};
    std::vector<BeautyBoundaryContours::Edge> edges;for(size_t i=0;i+1<points.size();++i)edges.push_back({points[i],points[i+1],0,1,1,2});
    const auto curves=BeautyBoundaryContours::build(edges,plane,neighbors);REQUIRE_FALSE(curves.empty());size_t tangent_reversals=0;
    for(const auto& curve:curves) {
        CHECK(curve.source_orientation==1);
        tangent_reversals+=(curve.b-curve.a).dot(curve.source_tangent)<0;
    }
    CHECK(tangent_reversals>0);
}

TEST_CASE("Refined children inherit exact material until explicit semantic paint and restore against new geometry", "[BeautyRegionPrecision]") {
    const auto mesh=precision_grid();const auto surface=BeautySurface::build_for_appearance(mesh,{});const auto regions=precision_regions(*surface);
    const auto original=precision_print(*surface,regions);const auto saved=original.encode();const auto refined=refine_selected_region_mesh(mesh,*surface,regions,2);
    const auto next=BeautySurface::build_for_appearance(refined.mesh,{});auto layers=remap_beauty_precision(refined,refined.mesh,*next,original,std::string(64,'a'));
    for(size_t f=0;f<refined.parent_faces.size();++f)CHECK(layers.printing.filament_slots.at(layers.printing.face_piece[f])==original.filament_slots.at(original.face_piece[refined.parent_faces[f]]));
    layers.editing.paint_filament(layers.printing,*next,layers.editing.source_sha256,2,4);layers.editing.paint_filament(layers.printing,*next,layers.editing.source_sha256,1,0);
    for(size_t f=0;f<refined.face_region.size();++f)CHECK(layers.printing.filament_slots.at(layers.printing.face_piece[f])==(refined.face_region[f]==2?4:0));
    CHECK(BeautyPuzzle::decode(layers.printing.encode(),next->geometry_id,refined.face_region.size()).same_edit(layers.printing));
    CHECK(BeautyEditRegions::decode(layers.editing.encode(),next->geometry_id,layers.editing.source_sha256,refined.face_region.size()).face_region==refined.face_region);
    CHECK(original.encode()==saved);CHECK_THROWS(remap_beauty_precision(refined,mesh,*surface,original,std::string(64,'a')));
}

TEST_CASE("Face interior cuts remain conforming across UV seams and uniform scales", "[BeautyRegionPrecision]") {
    size_t splits=0;double error=0;
    for(float scale:{.1f,1.f,10.f})for(bool seams:{false,true}) {
        const auto mesh=precision_grid(scale,seams);const auto surface=BeautySurface::build_for_appearance(mesh,{});const auto regions=precision_regions(*surface,scale);
        const auto result=refine_selected_region_mesh(mesh,*surface,regions,2);const auto next=BeautySurface::build_for_appearance(result.mesh,{});
        REQUIRE(result.split_faces>0);CHECK(next->boundary_edges==surface->boundary_edges);CHECK(next->nonmanifold_edges==0);
        const double actual=ellipse_border_error(result.mesh,*next,result.face_region,scale);
        if(!splits){splits=result.split_faces;error=actual;}else{CHECK(result.split_faces==splits);CHECK_THAT(actual,Catch::Matchers::WithinAbs(error,1e-5));}
    }
}

TEST_CASE("Precision fitting preserves protected detail holes and does not partially commit on cancellation", "[BeautyRegionPrecision]") {
    const auto mesh=precision_grid();const auto surface=BeautySurface::build_for_appearance(mesh,{});auto regions=precision_regions(*surface);std::vector<uint8_t> anchors(regions.size(),0);
    for(size_t f=0;f<regions.size();++f)if(std::abs(surface->centers[f].x()-20)<2 && std::abs(surface->centers[f].y()-14)<1){regions[f]=3;anchors[f]=1;}
    const auto result=refine_selected_region_mesh(mesh,*surface,regions,2,anchors);REQUIRE(result.split_faces>0);
    std::vector<size_t> children(regions.size(),0);
    for(size_t f=0;f<result.parent_faces.size();++f){++children[result.parent_faces[f]];if(anchors[result.parent_faces[f]])CHECK(result.face_region[f]==3);}
    for(size_t f=0;f<children.size();++f)if(anchors[f])CHECK(children[f]==1);
    const auto frozen=refine_selected_region_mesh(mesh,*surface,regions,2,std::vector<uint8_t>(regions.size(),1));CHECK(frozen.split_faces==0);CHECK(frozen.face_region==regions);CHECK(frozen.mesh.indices==mesh.indices);
    size_t checks=0;CHECK_THROWS(refine_selected_region_mesh(mesh,*surface,regions,2,{},[&]{return ++checks>8;}));
    auto stale=*surface;stale.geometry_id="stale";CHECK_THROWS(refine_selected_region_mesh(mesh,stale,regions,2));CHECK_THROWS(refine_selected_region_mesh(mesh,*surface,regions,2,{1}));
}

TEST_CASE("New orphan semantic islands are rejected before material layers are rebound", "[BeautyRegionPrecision]") {
    const auto mesh=precision_grid();const auto surface=BeautySurface::build_for_appearance(mesh,{});const auto regions=precision_regions(*surface);const auto original=precision_print(*surface,regions);const auto saved=original.encode();
    auto refined=refine_selected_region_mesh(mesh,*surface,regions,2);const auto next=BeautySurface::build_for_appearance(refined.mesh,{});CHECK(assess_beauty_precision(refined,*next).applicable());
    size_t face=0;while(face<refined.face_region.size() && (next->centers[face]-Vec3d(20,14,0)).norm()>1)++face;REQUIRE(face<refined.face_region.size());REQUIRE(refined.face_region[face]==2);refined.face_region[face]=1;
    const auto assessment=assess_beauty_precision(refined,*next);CHECK_FALSE(assessment.applicable());CHECK(assessment.unanchored_components==1);
    CHECK_THROWS(remap_beauty_precision(refined,refined.mesh,*next,original,std::string(64,'a')));CHECK(original.encode()==saved);
    // Explicit review can inspect the rejected candidate without accepting it.
    CHECK_NOTHROW(remap_beauty_precision(refined,refined.mesh,*next,original,std::string(64,'a'),true));
}

TEST_CASE("Semantic component validation detects splitting merging and deletion even with unchanged totals", "[BeautyRegionPrecision]") {
    const auto mesh=precision_grid();const auto surface=BeautySurface::build_for_appearance(mesh,{});
    auto regions=precision_regions(*surface);
    enum class Change {split,merge,remove,equal_total};
    for(const auto change:{Change::split,Change::merge,Change::remove,Change::equal_total}) {
        DYNAMIC_SECTION("component change " << int(change)) {
            if(change==Change::split)std::fill(regions.begin(),regions.end(),1);
            else if(change!=Change::remove)for(size_t f=0;f<regions.size();++f)
                regions[f]=surface->centers[f].x()>=18 && surface->centers[f].x()<22?2:1;
            // Printing pieces are connected independently of semantic regions.
            const auto original=precision_print(*surface,precision_regions(*surface));const auto saved=original.encode();
            auto refined=refine_selected_region_mesh(mesh,*surface,regions,1,std::vector<uint8_t>(regions.size(),1));
            REQUIRE(assess_beauty_precision(refined,*surface).applicable());
            for(size_t f=0;f<regions.size();++f) {
                const double x=surface->centers[f].x();
                if(change==Change::split)refined.face_region[f]=x>=19 && x<21?2:1;
                else if(change==Change::equal_total)refined.face_region[f]=x>=10 && x<14?2:1;
                else refined.face_region[f]=1;
            }
            const auto result=assess_beauty_precision(refined,*surface);CHECK_FALSE(result.applicable());
            if(change==Change::split){CHECK(result.split_components==1);CHECK(result.unanchored_components==1);CHECK(result.merged_components==0);CHECK(result.removed_components==0);}
            if(change==Change::merge){CHECK(result.merged_components==1);CHECK(result.removed_components==1);CHECK(result.split_components==0);}
            if(change==Change::remove){CHECK(result.removed_components==1);CHECK(result.merged_components==0);CHECK(result.split_components==0);}
            if(change==Change::equal_total){CHECK(result.source_components==result.refined_components);CHECK(result.split_components==1);CHECK(result.merged_components==1);}
            CHECK_THROWS(remap_beauty_precision(refined,mesh,*surface,original,std::string(64,'a')));
            CHECK(original.encode()==saved);
        }
        regions=precision_regions(*surface);
    }
}

// Explicit local-asset probe. No generation, no GUI and no automatic anatomy.
TEST_CASE("Native precision probe saves actual refined material geometry with parent mapping", "[.][BeautyRegionPrecisionProbe]") {
    const auto env=[](const char* name){const char* value=boost::nowide::getenv(name);return value?std::string(value):std::string();};
    const auto source=boost::filesystem::path(env("ORCA_PRECISION_SOURCE")),input=boost::filesystem::path(env("ORCA_PRECISION_INPUT")),output=boost::filesystem::path(env("ORCA_PRECISION_OUTPUT"));
    if(source.empty() || input.empty() || output.empty())SKIP("Explicit immutable model, old baseline records and a new output directory required.");
    REQUIRE_FALSE(boost::filesystem::exists(output));REQUIRE(boost::filesystem::create_directories(output));
    const auto hash=model_artifact_sha256(source);TriangleMesh original;ObjInfo colors;std::string error;REQUIRE(load_model_artifact(source,original,colors,error));
    const auto surface=BeautySurface::build_for_appearance(original.its,colors.vertex_colors);
    boost::filesystem::ifstream record_file(input/"edit-record.json");const auto record=nlohmann::json::parse(record_file);
    const auto puzzle=BeautyPuzzle::decode(record.at("puzzle"),surface->geometry_id,original.its.indices.size());
    const auto editing=BeautyEditRegions::decode(record.at("edit_regions"),surface->geometry_id,hash,original.its.indices.size());
    boost::filesystem::ifstream mask(input/"anchors.u8",std::ios::binary);std::vector<uint8_t> anchors(original.its.indices.size());mask.read(reinterpret_cast<char*>(anchors.data()),anchors.size());REQUIRE(bool(mask));
    // Same contour/protection and same explicit material intent on the previous
    // face-center representation. Save it too, so precision is the variable.
    auto face_editing=editing;auto face_printing=puzzle;
    face_editing.smooth_curve_boundary(face_printing,original.its,*surface,hash,2,BeautyEditRegions::BoundaryPaintMode::PreserveColors,anchors);
    face_editing.paint_filament(face_printing,*surface,hash,2,4);std::vector<size_t> face_released;
    for(size_t f=0;f<editing.face_region.size();++f)if(editing.face_region[f]==2 && face_editing.face_region[f]!=2)face_released.push_back(f);
    if(!face_released.empty())face_printing.paint_faces_filament(face_released,*surface,3);
    const auto face_base=output/"face-base.glb";boost::filesystem::copy_file(source,face_base);CHECK(model_artifact_sha256(face_base)==hash);
    BeautyDocument face_document;face_document.geometry_id=surface->geometry_id;face_document.face_count=original.its.indices.size();face_document.face_patch=surface->face_patch;
    auto face_record=face_document.encode();face_record["puzzle"]=face_printing.encode();face_record["edit_regions"]=face_editing.encode();face_record["puzzle_base_file"]=face_base.filename().string();face_record["puzzle_base_sha256"]=hash;
    ModelFinishingOptions face_options;face_options.smooth_surface=false;face_options.repair_mesh=false;face_options.beauty_puzzle=true;face_options.beauty_surface=surface;face_options.beauty_document=face_record;
    const auto face_saved=output/"face-painted.glb";const auto face_finished=finish_model_artifact(face_base,face_saved,face_options);INFO(face_finished.error);REQUIRE(face_finished.success);
    std::vector<RGBA> face_colors_before;std::vector<uint32_t> face_slots_before;
    for(uint32_t id:face_printing.face_piece){face_colors_before.push_back(face_printing.colors.at(id));face_slots_before.push_back(uint32_t(face_printing.filament_slots.at(id)));}
    raw_file(output/"face-colors.f32",face_colors_before);raw_file(output/"face-slots.u32",face_slots_before);raw_file(output/"face-regions.u32",face_editing.face_region);
    const auto start=std::chrono::steady_clock::now();const auto refined=refine_selected_region_mesh(original.its,*surface,editing.face_region,2,anchors);
    REQUIRE(refined.split_faces>0);const double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    const auto geometry_source=read_glb_geometry_source(source,original.its);const auto base=output/"refined-base.glb";write_glb_surface_refinement(*geometry_source,base,refined.mesh,refined.vertices,refined.parent_faces);
    TriangleMesh loaded;ObjInfo loaded_colors;REQUIRE(load_model_artifact(base,loaded,loaded_colors,error));const auto next=BeautySurface::build_for_appearance(loaded.its,loaded_colors.vertex_colors);
    CHECK(next->boundary_edges==surface->boundary_edges);CHECK(next->nonmanifold_edges==surface->nonmanifold_edges);
    const auto validity=assess_beauty_precision(refined,*next);
    if(!validity.applicable())CHECK_THROWS(remap_beauty_precision(refined,loaded.its,*next,puzzle,model_artifact_sha256(base)));
    auto layers=remap_beauty_precision(refined,loaded.its,*next,puzzle,model_artifact_sha256(base),true);
    std::vector<size_t> released;for(size_t f=0;f<refined.parent_faces.size();++f) {
        const size_t p=refined.parent_faces[f];if(editing.face_region[p]==2 && refined.face_region[f]!=2)released.push_back(f);
        if(anchors[p]){CHECK(refined.face_region[f]==editing.face_region[p]);CHECK(layers.printing.filament_slots.at(layers.printing.face_piece[f])==puzzle.filament_slots.at(puzzle.face_piece[p]));}
    }
    // Explicit diagnostic user intent: selected lip=slot4; released rim=slot3.
    // Slot3 is supplied for this comparison, never inferred by the algorithm.
    layers.editing.paint_filament(layers.printing,*next,layers.editing.source_sha256,2,4);
    if(!released.empty())layers.printing.paint_faces_filament(released,*next,3);
    std::vector<uint32_t> slots;std::vector<RGBA> face_colors;
    for(size_t f=0;f<refined.parent_faces.size();++f) {
        const size_t p=refined.parent_faces[f];const auto slot=layers.printing.filament_slots.at(layers.printing.face_piece[f]);slots.push_back(uint32_t(slot));face_colors.push_back(layers.printing.colors.at(layers.printing.face_piece[f]));
        if(refined.face_region[f]==editing.face_region[p])CHECK(slot==puzzle.filament_slots.at(puzzle.face_piece[p]));
        if(anchors[p])CHECK(slot==puzzle.filament_slots.at(puzzle.face_piece[p]));
    }
    BeautyDocument document;document.geometry_id=next->geometry_id;document.face_count=loaded.its.indices.size();document.face_patch=next->face_patch;
    auto accepted=document.encode();accepted["puzzle"]=layers.printing.encode();accepted["edit_regions"]=layers.editing.encode();accepted["puzzle_base_file"]=base.filename().string();accepted["puzzle_base_sha256"]=model_artifact_sha256(base);
    const nlohmann::json connectivity{{"applicable",validity.applicable()},{"source_components",validity.source_components},{"refined_components",validity.refined_components},{"unanchored_components",validity.unanchored_components},{"split_components",validity.split_components},{"merged_components",validity.merged_components},{"removed_components",validity.removed_components}};
    accepted["precision_review"]={{"algorithm",BeautyRegionPrecision::algorithm_version},{"connectivity",connectivity},{"diagnostic_only",true}};
    ModelFinishingOptions options;options.smooth_surface=false;options.repair_mesh=false;options.beauty_puzzle=true;options.beauty_surface=next;options.beauty_document=accepted;
    const auto saved=output/"refined-painted.glb";const auto finished=finish_model_artifact(base,saved,options);INFO(finished.error);REQUIRE(finished.success);
    TriangleMesh reopened;ObjInfo saved_colors;REQUIRE(load_model_artifact(saved,reopened,saved_colors,error));CHECK(reopened.its.indices==loaded.its.indices);CHECK(reopened.its.vertices==loaded.its.vertices);
    auto sidecar=saved;sidecar.replace_extension(".json");boost::filesystem::ofstream metadata(sidecar);metadata<<nlohmann::json{{"model_sha256",finished.output_sha256},{"beauty_workbench",accepted}}.dump();metadata.close();REQUIRE(bool(metadata));
    boost::filesystem::ifstream restored_file(sidecar);const auto restored=nlohmann::json::parse(restored_file).at("beauty_workbench");
    CHECK(BeautyPuzzle::decode(restored.at("puzzle"),next->geometry_id,slots.size()).same_edit(layers.printing));CHECK(BeautyEditRegions::decode(restored.at("edit_regions"),next->geometry_id,layers.editing.source_sha256,slots.size()).face_region==refined.face_region);
    raw_file(output/"vertices.f32",loaded.its.vertices);raw_file(output/"triangles.i32",loaded.its.indices);raw_file(output/"parents.u64",refined.parent_faces);raw_file(output/"regions.u32",refined.face_region);raw_file(output/"slots.u32",slots);raw_file(output/"colors.f32",face_colors);raw_file(output/"neighbors.i32",next->face_neighbors);
    std::vector<RGBA> sampled;for(const auto& t:reopened.its.indices){RGBA c{};for(int v:t)for(size_t ch=0;ch<4;++ch)c[ch]+=saved_colors.vertex_colors[size_t(v)][ch]/3;sampled.push_back(c);}raw_file(output/"saved_face_colors.f32",sampled);
    boost::filesystem::ofstream report(output/"probe.json");report<<nlohmann::json{{"algorithm",BeautyRegionPrecision::algorithm_version},{"connectivity",connectivity},{"diagnostic_only",true},{"source_sha256",hash},{"base_sha256",model_artifact_sha256(base)},{"saved_sha256",finished.output_sha256},{"face_saved_sha256",face_finished.output_sha256},{"geometry_id",next->geometry_id},{"faces_before",original.its.indices.size()},{"faces_after",slots.size()},{"split_faces",refined.split_faces},{"vertices_before",original.its.vertices.size()},{"vertices_after",loaded.its.vertices.size()},{"curve_seconds",elapsed},{"released_children",released.size()},{"palette",layers.printing.encode().at("palette")},{"manual_selected_slot",4},{"manual_released_slot",3},{"gui_validated",false},{"print_validated",false}}.dump(2);report.close();REQUIRE(bool(report));CHECK(model_artifact_sha256(source)==hash);
}
