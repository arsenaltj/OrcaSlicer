#include <catch2/catch_all.hpp>
#include "slic3r/GUI/AI/Model/BeautySurface.hpp"
#include "slic3r/GUI/AI/Model/BeautyDocument.hpp"
#include "slic3r/GUI/AI/Model/BeautyPuzzle.hpp"
#include "slic3r/GUI/AI/Model/BeautyRecognition.hpp"
#include "slic3r/GUI/AI/Model/LocalSemanticGeometry.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "slic3r/GUI/AI/Model/ModelFinishing.hpp"
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <algorithm>
#include <cstring>
#include <chrono>
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
TEST_CASE("Historical model workbench preparation stages remain stable", "[.BeautyPreparationProbe]") {
    const auto env=[](const char* name) {
        const char* value=boost::nowide::getenv(name);
        return value?std::string(value):std::string{};
    };
    const auto source_text=env("ORCA_BEAUTY_PREP_SOURCE"),report_text=env("ORCA_BEAUTY_PREP_REPORT");
    if(source_text.empty() || report_text.empty())SKIP("Set a local model and a fresh report path.");
    const boost::filesystem::path source(source_text),report(report_text);
    REQUIRE(boost::filesystem::exists(source));
    REQUIRE_FALSE(boost::filesystem::exists(report));
    const bool appearance=env("ORCA_BEAUTY_SURFACE_MODE")=="appearance";
    const auto input_hash=model_artifact_sha256(source);
    REQUIRE_FALSE(input_hash.empty());
    nlohmann::json samples=nlohmann::json::array();
    std::vector<uint32_t> expected_patches;
    std::string expected_puzzle;
    for(int iteration=0;iteration<5;++iteration) {
        auto previous=std::chrono::steady_clock::now();
        const auto started=previous;
        nlohmann::json stages=nlohmann::json::object();
        const auto mark=[&](const char* name) {
            const auto now=std::chrono::steady_clock::now();
            stages[name]=std::chrono::duration<double,std::milli>(now-previous).count();
            previous=now;
        };
        TriangleMesh mesh;ObjInfo colors;std::string error;
        REQUIRE(load_model_artifact(source,mesh,colors,error));
        mark("load_decode");
        const auto surface=appearance?BeautySurface::build_for_appearance(mesh.its,colors.vertex_colors):BeautySurface::build(mesh.its,colors.vertex_colors);
        mark("surface_build");
        const auto face_colors=beauty_source_face_colors(mesh.its,colors.vertex_colors);
        mark("face_colors");
        auto puzzle=BeautyPuzzle::create_regions(*surface);
        mark("region_partition");
        puzzle.validate(*surface);
        mark("validate");
        if(colors.vertex_colors.empty())REQUIRE(face_colors.empty());
        else REQUIRE(face_colors.size()==mesh.its.indices.size());
        const auto encoded=puzzle.encode().dump();
        if(iteration==0) {expected_patches=surface->face_patch;expected_puzzle=encoded;}
        else {CHECK(surface->face_patch==expected_patches);CHECK(encoded==expected_puzzle);}
        samples.push_back({{"phase",iteration==0?"first":iteration==1?"warmup":"warm"},
            {"stages_ms",stages},
            {"operation_ms",std::chrono::duration<double,std::milli>(previous-started).count()},
            {"faces",mesh.its.indices.size()},{"patches",surface->patches.size()},
            {"pieces",puzzle.piece_count()}});
    }
    REQUIRE(model_artifact_sha256(source)==input_hash);
    const auto fnv=[](const void* data,size_t bytes) {
        const auto* value=static_cast<const unsigned char*>(data);
        uint64_t hash=14695981039346656037ull;
        for(size_t i=0;i<bytes;++i)hash=(hash^value[i])*1099511628211ull;
        return hash;
    };
    boost::filesystem::ofstream output(report);
    output<<nlohmann::json{{"source_sha256",input_hash},{"samples",samples},
        {"face_patch_fnv64",fnv(expected_patches.data(),expected_patches.size()*sizeof(uint32_t))},
        {"puzzle_fnv64",fnv(expected_puzzle.data(),expected_puzzle.size())},
        {"scope","offline no-recognition preparation; excludes metadata, UI, drafts and rendering"}}.dump(2);
    output.close();
    REQUIRE(output.good());
}
// Opt-in real asset probe: hash ordered topology and numeric fields outside timing.
// The fresh report permits cross-build comparisons without shipping private assets.
TEST_CASE("Historical surface topology and patch data stay stable across builds", "[.BeautySurfaceProbe]") {
    const auto env=[](const char* key){const auto value=boost::nowide::getenv(key);return value?std::string(value):std::string{};};
    const auto source=env("ORCA_BEAUTY_PREP_SOURCE"),report=env("ORCA_BEAUTY_SURFACE_REPORT");
    if(source.empty() || report.empty())SKIP("Set a local model and a fresh surface report path.");
    REQUIRE(boost::filesystem::exists(source));REQUIRE_FALSE(boost::filesystem::exists(report));
    const auto mode=env("ORCA_BEAUTY_SURFACE_MODE");
    const bool appearance=mode=="appearance";
    if(!appearance && !mode.empty())REQUIRE(mode=="full");
    const auto input_hash=model_artifact_sha256(source);
    TriangleMesh mesh;ObjInfo colors;std::string error;
    REQUIRE(load_model_artifact(source,mesh,colors,error));
    const auto digest=[](const void* bytes,size_t count) {
        const auto hash=Slic3r::GUI::LocalSemanticGeometry::detail::digest(static_cast<const char*>(bytes),count,nullptr);
        REQUIRE(hash.size()==32);
        std::string result;for(unsigned char byte:hash){result+="0123456789abcdef"[byte>>4];result+="0123456789abcdef"[byte&15];}
        return result;
    };
    const auto signature=[&](const BeautySurface& s) {
        nlohmann::json fields={{"geometry_id",s.geometry_id},{"boundary_edges",s.boundary_edges},{"nonmanifold_edges",s.nonmanifold_edges}};
        const auto array=[&](const char* name,const auto& values) {
            fields[name]={{"count",values.size()},{"sha256",digest(values.data(),values.size()*sizeof(values[0]))}};
        };
        array("vertex_class",s.vertex_class);array("class_representative",s.class_representative);
        array("face_neighbors",s.face_neighbors);array("areas",s.areas);array("face_patch",s.face_patch);
        array("neighbor_offsets",s.neighbor_offsets);array("vertex_neighbors",s.vertex_neighbors);array("edge_lengths",s.edge_lengths);
        // Keep the raw row-order hashes above. Canonical pairs separately prove
        // that an enumeration change retains every neighbor and exact length bit.
        std::vector<std::array<uint32_t,2>> canonical(s.vertex_neighbors.size());
        for(size_t v=0;!s.neighbor_offsets.empty() && v<s.class_representative.size();++v) {
            for(size_t at=s.neighbor_offsets[v];at<s.neighbor_offsets[v+1];++at) {
                canonical[at][0]=s.vertex_neighbors[at];
                std::memcpy(&canonical[at][1],&s.edge_lengths[at],sizeof(uint32_t));
            }
            std::sort(canonical.begin()+s.neighbor_offsets[v],canonical.begin()+s.neighbor_offsets[v+1]);
        }
        array("canonical_vertex_edges",canonical);
        const auto vectors=[&](const char* name,const auto& values) {
            std::vector<double> packed;packed.reserve(values.size()*3);
            for(const auto& v:values)for(int k=0;k<3;++k)packed.push_back(v[k]);
            array(name,packed);
        };
        vectors("centers",s.centers);vectors("normals",s.normals);
        std::string patches;
        for(const auto& patch:s.patches) {
            nlohmann::json entry={{"faces",digest(patch.faces.data(),patch.faces.size()*sizeof(size_t))},
                {"neighbors",digest(patch.neighbors.data(),patch.neighbors.size()*sizeof(uint32_t))}};
            std::array<double,10> numbers{};
            for(int k=0;k<3;++k){numbers[k]=patch.center[k];numbers[k+3]=patch.normal[k];numbers[k+6]=patch.mean_color[k];}
            numbers[9]=patch.area;entry["numbers"]=digest(numbers.data(),sizeof(numbers));
            patches+=entry.dump();
        }
        fields["patches"]={{"count",s.patches.size()},{"sha256",digest(patches.data(),patches.size())}};
        return fields;
    };
    nlohmann::json expected,saved_fields,consumer_fields,samples=nlohmann::json::array();
    for(int iteration=0;iteration<5;++iteration) {
        const auto started=std::chrono::steady_clock::now();
        auto surface=appearance?BeautySurface::build_for_appearance(mesh.its,colors.vertex_colors):BeautySurface::build(mesh.its,colors.vertex_colors);
        const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
        const auto fields=signature(*surface);
        if(iteration==0)expected=fields;else CHECK(fields==expected);
        samples.push_back({{"phase",iteration==0?"first":iteration==1?"warmup":"warm"},{"elapsed_ms",elapsed}});
        if(iteration==0) {
            const auto restored=appearance?BeautySurface::build_for_appearance(mesh.its,colors.vertex_colors,surface->face_patch):BeautySurface::build(mesh.its,colors.vertex_colors,surface->face_patch);
            CHECK(restored->face_patch==surface->face_patch);
            // Saved patches visit faces in index order, unlike fresh BFS patches.
            // Compare their numeric signature across builds, not against fresh sums.
            saved_fields=signature(*restored);
            std::vector<uint8_t> selected(mesh.its.indices.size()),protection(selected.size());
            double low=surface->centers.front().x(),high=low;
            for(const auto& center:surface->centers){low=std::min(low,center.x());high=std::max(high,center.x());}
            const double middle=low+(high-low)*.5;
            for(size_t f=0;f<selected.size();++f){selected[f]=surface->centers[f].x()<=middle;protection[f]=selected[f] && f%257==0;}
            if(std::find(selected.begin(),selected.end(),1)==selected.end())selected[0]=1;
            if(std::equal(selected.begin(),selected.end(),protection.begin()))protection[0]=0;
            const auto record=[&](const char* name,const auto& values) {
                consumer_fields[name]={{"count",values.size()},{"sha256",digest(values.data(),values.size()*sizeof(values[0]))}};
            };
            const auto face_weights=surface->face_weights(mesh.its,selected,protection,1.5);
            record("face_weights",face_weights);
            for(double radius:{.25,1.5,3.}) {
                const auto weights=surface->vertex_weights(mesh.its,selected,protection,radius);
                const auto name="vertex_weights_"+std::to_string(radius);record(name.c_str(),weights);
            }
            size_t moved=0;
            try {
                const auto deformed=surface->deform(mesh.its,selected,protection,.02,.25,moved);
                consumer_fields["deform_status"]="success";consumer_fields["moved"]=moved;
                std::vector<float> vertices;vertices.reserve(deformed.vertices.size()*3);
                for(const auto& v:deformed.vertices)for(int k=0;k<3;++k)vertices.push_back(v[k]);
                record("deformed_vertices",vertices);
            } catch(const std::runtime_error& failure) {
                consumer_fields["deform_status"]=failure.what();
            }
        }
    }
    size_t checkpoints=0;
    CHECK_THROWS_AS(BeautySurface::build(mesh.its,colors.vertex_colors,{},[&]{return ++checkpoints==3;}),std::runtime_error);
    CHECK(checkpoints==3);REQUIRE(model_artifact_sha256(source)==input_hash);
    boost::filesystem::ofstream output(report);
    output<<nlohmann::json{{"source_sha256",input_hash},{"surface_fields",expected},{"saved_surface_fields",saved_fields},{"consumer_fields",consumer_fields},{"samples",samples},
        {"scope","offline surface build only; loading, signature checks, UI, recognition and rendering excluded"}}.dump(2);
    output.close();REQUIRE(output.good());
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
    const auto appearance=GENERATE(false,true);
    auto surface=appearance?BeautySurface::build_for_appearance(mesh.its,colors.vertex_colors):BeautySurface::build(mesh.its,colors.vertex_colors);auto doc=document_for(surface);
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
    const auto appearance=GENERATE(false,true);
    const auto surface=appearance?BeautySurface::build_for_appearance(mesh.its,colors.vertex_colors):BeautySurface::build(mesh.its,colors.vertex_colors);auto doc=document_for(surface);
    ModelFinishingOptions options;options.smooth_surface=false;options.repair_mesh=false;options.beauty_appearance=true;
    options.beauty_surface=surface;options.selected_faces.resize(mesh.its.indices.size());std::iota(options.selected_faces.begin(),options.selected_faces.end(),0);
    const auto id=doc.add_group("protected",options.selected_faces);doc.group(id).locked=true;options.beauty_document=doc.encode();
    options.appearance.brightness=.1;options.appearance.face_weights.assign(mesh.its.indices.size(),1);
    CHECK_FALSE(finish_model_artifact(source,fixture.root/"locked.glb",options).success);CHECK_FALSE(boost::filesystem::exists(fixture.root/"locked.glb"));
    doc.group(id).locked=false;options.beauty_document=doc.encode();options.appearance.face_weights.clear();
    const auto result=finish_model_artifact(source,fixture.root/"appearance.glb",options);INFO(result.error);REQUIRE(result.success);CHECK(result.changed_texture_pixels>0);
    options.beauty_document["geometry_id"]="unrelated";CHECK_FALSE(finish_model_artifact(source,fixture.root/"wrong.glb",options).success);
}

TEST_CASE("Beauty document encoding keeps historical runs and editing metadata", "[BeautySurface]") {
    BeautyDocument doc;doc.geometry_id="encoding-fixture";doc.face_count=6;
    doc.face_patch={0,0,1,2,2,0};doc.next_group_id=4;
    doc.groups.push_back({3,"protected",{0,5},true,false});
    doc.edits=nlohmann::json::array({{{"operation","preserve-unknown-history"},{"value",7}}});
    const auto encoded=doc.encode();
    CHECK(encoded.at("patch_runs")==nlohmann::json::array({{0,2},{1,1},{2,2},{0,1}}));
    CHECK(encoded.at("groups")==nlohmann::json::array({{{"id",3},{"name","protected"},{"faces",{0,5}},
        {"locked",true},{"preserve_color",false}}}));
    CHECK(encoded.at("edits")==doc.edits);
    CHECK(BeautyDocument::decode(encoded,doc.geometry_id,6).encode()==encoded);
    CHECK(doc.face_patch==std::vector<uint32_t>{0,0,1,2,2,0});
    doc.face_patch.clear();CHECK(doc.encode().at("patch_runs").is_array());CHECK(doc.encode().at("patch_runs").empty());
    doc.face_patch.assign(6,0);CHECK(doc.encode().at("patch_runs")==nlohmann::json::array({{0,6}}));
}


TEST_CASE("Surface distance and deformation retain seam protection under neighbor row permutations", "[BeautySurface][BeautyWorkbench]") {
    const auto mesh=grid(true);
    const auto surface=BeautySurface::build(mesh,{});
    auto permuted=*surface;
    for(size_t v=0;v<permuted.class_representative.size();++v) {
        const auto first=permuted.neighbor_offsets[v],last=permuted.neighbor_offsets[v+1];
        std::reverse(permuted.vertex_neighbors.begin()+first,permuted.vertex_neighbors.begin()+last);
        std::reverse(permuted.edge_lengths.begin()+first,permuted.edge_lengths.begin()+last);
    }
    const auto selected=middle(mesh);
    std::vector<uint8_t> protection(selected.size());protection[42]=1;
    for(double radius:{.25,1.5,3.}) {
        // Bit equality here protects a representation-only performance change.
        CHECK(surface->vertex_weights(mesh,selected,protection,radius)==permuted.vertex_weights(mesh,selected,protection,radius));
        CHECK(surface->face_weights(mesh,selected,protection,radius)==permuted.face_weights(mesh,selected,protection,radius));
    }
    size_t moved=0,reordered_moved=0;
    const auto original=surface->deform(mesh,selected,protection,.02,.25,moved);
    const auto reordered=permuted.deform(mesh,selected,protection,.02,.25,reordered_moved);
    CHECK(moved==reordered_moved);
    CHECK(original.vertices==reordered.vertices);
    CHECK(original.indices==reordered.indices);
    CHECK(original.vertices[24]==original.vertices.back());
    for(int k=0;k<3;++k)CHECK(original.vertices[mesh.indices[42][k]]==mesh.vertices[mesh.indices[42][k]]);
}


TEST_CASE("Appearance preparation retains partitioning and geometry operations without eager vertex rows", "[BeautySurface]") {
    const auto seam=GENERATE(false,true);
    auto mesh=grid(seam);
    const std::vector<RGBA> colors(mesh.vertices.size(),RGBA{.6f,.4f,.3f,1.f});
    // Exact bytes are the compatibility property under this same toolchain.
    const auto bytes_equal=[](const auto& a,const auto& b){return a.size()==b.size() &&
        (a.empty() || std::memcmp(a.data(),b.data(),a.size()*sizeof(a[0]))==0);};
    const auto full=BeautySurface::build(mesh,colors),appearance=BeautySurface::build_for_appearance(mesh,colors);
    REQUIRE(appearance->neighbor_offsets.empty());REQUIRE(appearance->vertex_neighbors.empty());REQUIRE(appearance->edge_lengths.empty());
    CHECK(full->geometry_id==appearance->geometry_id);CHECK(full->vertex_class==appearance->vertex_class);
    CHECK(full->class_representative==appearance->class_representative);CHECK(full->face_patch==appearance->face_patch);
    CHECK(full->face_neighbors==appearance->face_neighbors);CHECK(bytes_equal(full->centers,appearance->centers));
    CHECK(bytes_equal(full->normals,appearance->normals));CHECK(bytes_equal(full->areas,appearance->areas));
    CHECK(bytes_equal(full->face_edge_lengths,appearance->face_edge_lengths));
    CHECK(std::memcmp(&full->geometry_extent,&appearance->geometry_extent,sizeof(double))==0);
    REQUIRE(appearance->face_edge_lengths.size()==mesh.indices.size());
    for(size_t f=0;f<mesh.indices.size();++f)for(size_t e=0;e<3;++e) {
        const auto& triangle=mesh.indices[f];
        const double expected=(mesh.vertices[triangle[e]].cast<double>()-mesh.vertices[triangle[(e+1)%3]].cast<double>()).norm();
        CHECK_THAT(double(appearance->face_edge_lengths[f][e]),Catch::Matchers::WithinAbs(expected,1e-6));
    }
    CHECK(full->boundary_edges==appearance->boundary_edges);CHECK(full->nonmanifold_edges==appearance->nonmanifold_edges);
    REQUIRE(full->patches.size()==appearance->patches.size());
    for(size_t p=0;p<full->patches.size();++p) {
        CHECK(full->patches[p].faces==appearance->patches[p].faces);CHECK(full->patches[p].neighbors==appearance->patches[p].neighbors);
        CHECK(std::memcmp(full->patches[p].mean_color.data(),appearance->patches[p].mean_color.data(),3*sizeof(double))==0);
        CHECK(std::memcmp(&full->patches[p].area,&appearance->patches[p].area,sizeof(double))==0);
    }
    const auto puzzle=BeautyPuzzle::create_regions(*full),other=BeautyPuzzle::create_regions(*appearance);
    CHECK(puzzle.encode()==other.encode());CHECK(document_for(full).encode()==document_for(appearance).encode());
    const auto restored=BeautySurface::build_for_appearance(mesh,colors,full->face_patch);
    CHECK(restored->face_patch==full->face_patch);CHECK(restored->face_neighbors==full->face_neighbors);
    const auto selected=middle(mesh);std::vector<uint8_t> protected_faces(mesh.indices.size(),0);protected_faces[28]=1;
    for(double radius:{.25,1.,3.}) {
        CHECK(bytes_equal(appearance->face_weights(mesh,selected,protected_faces,radius),full->face_weights(mesh,selected,protected_faces,radius)));
        CHECK(bytes_equal(appearance->vertex_weights(mesh,selected,protected_faces,radius),full->vertex_weights(mesh,selected,protected_faces,radius)));
    }
    for(double displacement:{-.1,.1}) {
        size_t moved=0,other_moved=0;
        const auto expected=full->deform(mesh,selected,protected_faces,displacement,.5,moved);
        const auto actual=appearance->deform(mesh,selected,protected_faces,displacement,.5,other_moved);
        CHECK(bytes_equal(actual.vertices,expected.vertices));CHECK(actual.indices==expected.indices);CHECK(other_moved==moved);
    }
    REQUIRE(appearance->neighbor_offsets.empty());REQUIRE(appearance->vertex_neighbors.empty());REQUIRE(appearance->edge_lengths.empty());
    REQUIRE_THROWS(appearance->vertex_weights(mesh,{},protected_faces,1));
    REQUIRE_THROWS(appearance->vertex_weights(mesh,selected,protected_faces,0));
    size_t checkpoints=0;
    REQUIRE_THROWS_AS(BeautySurface::build_for_appearance(mesh,colors,{},[&]{return ++checkpoints==3;}),std::runtime_error);
    CHECK(checkpoints==3);CHECK(BeautySurface::build_for_appearance(mesh,colors)->face_patch==full->face_patch);
}


TEST_CASE("Canceled late geometry preparation preserves a reusable appearance surface", "[BeautySurface]") {
    const auto stop=GENERATE(size_t(1),size_t(3),size_t(5));
    const auto mesh=grid(true);const auto original=mesh.vertices;
    const auto surface=BeautySurface::build_for_appearance(mesh,{});
    const auto before=document_for(surface).encode();
    const auto selected=middle(mesh);
    size_t checks=0,moved=0;
    REQUIRE_THROWS_AS(surface->deform(mesh,selected,{},.1,.5,moved,[&]{return ++checks==stop;}),std::runtime_error);
    CHECK(checks==stop);CHECK(mesh.vertices==original);CHECK(document_for(surface).encode()==before);
    CHECK(surface->neighbor_offsets.empty());CHECK(surface->vertex_neighbors.empty());CHECK(surface->edge_lengths.empty());
    const auto retried=surface->deform(mesh,selected,{},.1,.5,moved);
    size_t expected_moved=0;
    const auto full=BeautySurface::build(mesh,{})->deform(mesh,selected,{},.1,.5,expected_moved);
    CHECK(moved==expected_moved);CHECK(retried.indices==full.indices);
    CHECK(std::memcmp(retried.vertices.data(),full.vertices.data(),full.vertices.size()*sizeof(Vec3f))==0);
}
