#include <catch2/catch_test_macros.hpp>
#include "slic3r/AI/AppearanceEditing/BeautyPuzzle.hpp"
#include "slic3r/AI/AppearanceEditing/BeautyEditRegions.hpp"
#include "slic3r/AI/AppearanceEditing/SurfaceSelectionState.hpp"
#include "slic3r/AI/AppearanceEditing/ModelFinishing.hpp"
#include "slic3r/AI/ColorMatching/AutomaticColorRegions.hpp"
#include "slic3r/AI/ModelArtifacts/ModelArtifact.hpp"
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <openssl/sha.h>
#include <chrono>
#include <iomanip>
#include <sstream>

using namespace Slic3r;
using namespace Slic3r::AI;
namespace {
std::string signature(const std::string& value) {
    unsigned char bytes[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), bytes);
    std::ostringstream out;
    for(unsigned char b:bytes)out<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(b);
    return out.str();
}
BeautySurface strip_surface() {
    BeautySurface s;s.geometry_id="colour-strip";s.face_patch={0,0,0,0,1,1,1,1};s.patches.resize(2);
    s.patches[0].mean_color={0,0,0};s.patches[1].mean_color={1,1,1};s.areas.assign(8,1.);
    for(int i=0;i<8;++i){s.centers.emplace_back(i,0,0);s.normals.emplace_back(0,0,1);
        s.face_neighbors.push_back({i>0?i-1:-1,i<7?i+1:-1,-1});}
    return s;
}
}

TEST_CASE("Beauty automatic colour refinement survives save and rematch without touching painted regions", "[BeautyColorRegions]") {
    const auto surface=strip_surface();
    BeautyPuzzle puzzle;puzzle.geometry_id=surface.geometry_id;puzzle.face_piece.assign(8,1);puzzle.next_id=2;
    std::vector<RGBA> source(8,{1,1,1,1});for(size_t f=0;f<4;++f)source[f]={0,0,0,1};
    const std::vector<PhysicalFilamentChannel> palette{{3,"#FFFFFF","PLA",true},{11,"#000000","PLA",true}};
    SECTION("Fresh automatic matching retains both colors") {
        puzzle.match_filaments(surface,palette,{},source);
        CHECK(puzzle.piece_count()==2);
        CHECK(puzzle.filament_slots.at(puzzle.face_piece[0])==11);
        CHECK(puzzle.filament_slots.at(puzzle.face_piece[7])==3);
        REQUIRE_NOTHROW(puzzle.validate(surface));
        auto restored=BeautyPuzzle::decode(puzzle.encode(),surface.geometry_id,8);
        restored.match_filaments(surface,palette,{},source,true);
        CHECK(restored.same_edit(puzzle));
        auto reordered=palette;std::reverse(reordered.begin(),reordered.end());
        restored.match_filaments(surface,reordered,{},source,true);
        CHECK(restored.filament_slots.at(restored.face_piece[0])==11);
        CHECK(restored.filament_slots.at(restored.face_piece[7])==3);
        // Adding grey must not quantize the remaining white faces from their
        // old mixed black/white parent target.
        auto extended=palette;extended.push_back({17,"#808080","PLA",true});
        restored.match_filaments(surface,extended,{},source);
        CHECK(restored.filament_slots.at(restored.face_piece[0])==11);
        CHECK(restored.filament_slots.at(restored.face_piece[7])==3);
    }
    SECTION("Manual color before matching is preserved") {
        puzzle.paint(1,{1,1,1,1});puzzle.match_filaments(surface,palette,{},source,true);
        CHECK(puzzle.piece_count()==1);CHECK(puzzle.filament_slots.at(1)==3);
    }
    SECTION("Already matched legacy records are never resegmented") {
        puzzle.match_filaments(surface,palette,{},source,false);const auto saved=puzzle;
        puzzle.match_filaments(surface,palette,{},source,true);CHECK(puzzle.same_edit(saved));
    }
}

TEST_CASE("Refined automatic regions reuse only unchanged native mixture definitions", "[BeautyColorRegions]") {
    const auto surface=strip_surface();
    BeautyPuzzle puzzle;puzzle.geometry_id=surface.geometry_id;puzzle.face_piece.assign(8,1);puzzle.next_id=2;
    const std::vector<PhysicalFilamentChannel> palette{{3,"#FFFFFF","PLA",true},{11,"#000000","PLA",true}};
    const MixedColorRecipe mix{"#808080",{{3,.5},{11,.5}},17,std::string(64,'a'),true};
    std::vector<RGBA> source(8,{1,1,1,1});
    for(size_t f=0;f<4;++f)source[f]=BeautyPuzzle::filament_color({17,mix.target_color,{},true});
    puzzle.match_filaments(surface,palette,{mix},source);
    REQUIRE(puzzle.piece_count()==2);
    CHECK(puzzle.filament_slots.at(puzzle.face_piece[0])==17);
    CHECK(puzzle.filament_slots.at(puzzle.face_piece[7])==3);
    REQUIRE(puzzle.mixed_recipes.size()==1);
    CHECK(same_native_mixed_recipe(puzzle.mixed_recipes.front(),mix));
    auto reordered=palette;std::reverse(reordered.begin(),reordered.end());
    puzzle.match_filaments(surface,reordered,{mix},source);
    CHECK(puzzle.filament_slots.at(puzzle.face_piece[0])==17);
    const auto partition=puzzle.face_piece;
    auto changed=mix;changed.native_settings_fingerprint=std::string(64,'b');
    puzzle.match_filaments(surface,palette,{changed},source);
    CHECK(puzzle.mixed_recipes.empty());
    CHECK(puzzle.filament_slots.at(puzzle.face_piece[0])!=17);
    CHECK(puzzle.face_piece==partition);
}

TEST_CASE("Colour model probe compares immutable native assets in the actual no-recognition matching path", "[.][AutomaticColorModelProbe]") {
    const auto env=[](const char* name){const char* value=boost::nowide::getenv(name);return value?std::string(value):std::string();};
    const auto input=env("ORCA_COLOR_SOURCE"),output=env("ORCA_COLOR_OUTPUT");
    if(input.empty() || output.empty())SKIP("Explicit local model and new output directory required.");
    const boost::filesystem::path source_path(input),root(output);
    REQUIRE_FALSE(boost::filesystem::exists(root));REQUIRE(boost::filesystem::create_directories(root));
    const auto source_hash=model_artifact_sha256(source_path);
    REQUIRE(source_hash.size()==64);
    const bool check_save=env("ORCA_COLOR_SAVE")=="1";
    const bool boundary_probe=env("ORCA_COLOR_REPAIR")=="1";
    const bool detail_probe=env("ORCA_COLOR_DETAILS")=="1";
    const bool constraint_probe=env("ORCA_COLOR_CONSTRAINTS")=="1";
    const auto curve_role=env("ORCA_COLOR_CURVE_REGION");const bool curve_probe=!curve_role.empty();
    if(curve_probe){REQUIRE_FALSE(boundary_probe);REQUIRE_FALSE(constraint_probe);REQUIRE(detail_probe);}
    if(constraint_probe){REQUIRE_FALSE(boundary_probe);REQUIRE(detail_probe);}
    const std::string selection_fraction=env("ORCA_COLOR_SELECTION_TOP_FRACTION");
    const double top_fraction=selection_fraction.empty()?1.:std::stod(selection_fraction);
    REQUIRE(top_fraction>0.);REQUIRE(top_fraction<=1.);
    const auto save_base=root/"original.glb";
    if(check_save) {
        REQUIRE(model_artifact_format(source_path)=="glb");
        boost::filesystem::copy_file(source_path,save_base);
        REQUIRE(model_artifact_sha256(save_base)==source_hash);
    }
    TriangleMesh mesh;ObjInfo colors;std::string error;
    const bool loaded_model=load_model_artifact(source_path,mesh,colors,error);INFO(error);REQUIRE(loaded_model);
    REQUIRE(colors.vertex_colors.size()==mesh.its.vertices.size());
    const auto surface=BeautySurface::build_for_appearance(mesh.its,colors.vertex_colors);
    std::vector<RGBA> faces(mesh.its.indices.size(),{0,0,0,1});
    for(size_t f=0;f<faces.size();++f)for(int v:mesh.its.indices[f])for(size_t c=0;c<3;++c)
        faces[f][c]+=colors.vertex_colors.at(size_t(v))[c]/3.f;
    const auto initial=BeautyPuzzle::create_regions(*surface);
    double zmin=mesh.its.vertices.front().z(),zmax=zmin;
    for(const auto& v:mesh.its.vertices){zmin=std::min(zmin,double(v.z()));zmax=std::max(zmax,double(v.z()));}
    const double selection_z=zmax-(zmax-zmin)*top_fraction;
    std::vector<size_t> detail_selection;
    for(size_t f=0;f<surface->centers.size();++f)if(surface->centers[f].z()>=selection_z)detail_selection.push_back(f);
    std::vector<PhysicalFilamentChannel> palette{{0,"#F7E2DA","PLA",true},{1,"#282629","PLA",true},
        {2,"#F6F7F9","PLA",true},{3,"#EA9A92","PLA",true},{4,"#668CB6","PLA",true},{5,"#958B86","PLA",true}};
    const auto palette_file=env("ORCA_COLOR_PALETTE_FILE");
    std::string palette_hash;
    if(!palette_file.empty()) {
        boost::filesystem::ifstream in{boost::filesystem::path(palette_file)};REQUIRE(in.good());
        nlohmann::json values;in>>values;
        REQUIRE(values.is_array());REQUIRE(is_supported_physical_channel_count(values.size()));
        palette.clear();
        for(const auto& value:values) {
            REQUIRE(value.is_string());
            palette.push_back({palette.size(),value.get<std::string>(),"PLA",true});
        }
        REQUIRE(is_valid_physical_channel_set(palette));
        palette_hash=model_artifact_sha256(boost::filesystem::path(palette_file));REQUIRE(palette_hash.size()==64);
    }
    const auto guidance_file=env("ORCA_COLOR_GUIDANCE_FILE");std::string guidance_hash,guidance_kind;
    std::vector<int32_t> constraint_labels;std::vector<std::string> constraint_names;
    if(constraint_probe || curve_probe) {
        REQUIRE_FALSE(guidance_file.empty());
        boost::filesystem::ifstream in{boost::filesystem::path(guidance_file)};REQUIRE(in.good());nlohmann::json annotation;in>>annotation;
        REQUIRE(annotation.at("schema")=="orca.portrait-constraint-labels/v1");
        REQUIRE(annotation.at("source_sha256")==source_hash);REQUIRE(annotation.at("geometry_id")==surface->geometry_id);
        REQUIRE(annotation.at("face_count")==faces.size());constraint_names=annotation.at("names").get<std::vector<std::string>>();
        guidance_kind=annotation.at("kind").get<std::string>();
        for(const auto& run:annotation.at("runs")) {
            REQUIRE(run.is_array());REQUIRE(run.size()==2);
            const int32_t label=run[0].get<int32_t>();const size_t length=run[1].get<size_t>();
            REQUIRE(label>=-1);if(label>=0)REQUIRE(size_t(label)<constraint_names.size());REQUIRE(length>0);
            REQUIRE(length<=faces.size()-constraint_labels.size());constraint_labels.insert(constraint_labels.end(),length,label);
        }
        REQUIRE(constraint_labels.size()==faces.size());guidance_hash=model_artifact_sha256(boost::filesystem::path(guidance_file));
    }
    nlohmann::json timings,saves=nlohmann::json::object();bool baseline_can_save=false;
    for(bool refined:{false,true}) {
        auto puzzle=initial;const auto started=std::chrono::steady_clock::now();
        puzzle.match_filaments(*surface,palette,{},faces,boundary_probe || detail_probe || refined);
        size_t repaired_faces=0,constrained_faces=0;
        if(detail_probe && (refined || constraint_probe || curve_probe)) {
            const auto before=puzzle;
            repaired_faces=puzzle.recover_source_color_details(*surface,detail_selection,faces);
            std::vector<uint8_t> selected(faces.size(),0);for(size_t f:detail_selection)selected[f]=1;
            size_t outside_changes=0;
            for(size_t f=0;f<faces.size();++f)if(!selected[f])
                outside_changes+=puzzle.filament_slots.at(puzzle.face_piece[f])!=before.filament_slots.at(before.face_piece[f]);
            CHECK(outside_changes==0);
        }
        std::optional<BeautyEditRegions> curve_regions;std::vector<uint8_t> curve_anchors;
        size_t curved_faces=0,curve_material_changes=0,curve_initial_faces=0;
        if(curve_probe) {
            const auto found=std::find(constraint_names.begin(),constraint_names.end(),curve_role);
            REQUIRE(found!=constraint_names.end());const int32_t label=int32_t(found-constraint_names.begin());
            const size_t slot=std::stoul(env("ORCA_COLOR_CURVE_SLOT"));
            REQUIRE(native_palette_has_slot(puzzle.palette,puzzle.mixed_recipes,slot));
            curve_regions=BeautyEditRegions{surface->geometry_id,source_hash,std::vector<uint32_t>(faces.size(),1)};
            curve_anchors.assign(faces.size(),0);
            for(size_t f=0;f<faces.size();++f) {
                const auto material=puzzle.filament_slots.at(puzzle.face_piece[f]);
                if(constraint_labels[f]==label && material==slot){curve_regions->face_region[f]=2;++curve_initial_faces;}
                // Existing white eye/mouth detail and other known features are
                // explicitly anchored in this offline manual selection example.
                if((constraint_labels[f]>=0 && constraint_labels[f]!=label) ||
                    (constraint_labels[f]==label && material==2))curve_anchors[f]=1;
            }
            REQUIRE(curve_initial_faces>0);
            const auto before=puzzle;const auto original_regions=*curve_regions;
            if(refined)curved_faces=curve_regions->smooth_curve_boundary(puzzle,mesh.its,*surface,source_hash,2,
                BeautyEditRegions::BoundaryPaintMode::ExtendAdjacentColors,curve_anchors);
            const auto restored_regions=BeautyEditRegions::decode(curve_regions->encode(),surface->geometry_id,source_hash,faces.size());
            CHECK(restored_regions.face_region==curve_regions->face_region);
            for(size_t f=0;f<faces.size();++f) {
                const bool changed=puzzle.filament_slots.at(puzzle.face_piece[f])!=before.filament_slots.at(before.face_piece[f]);
                curve_material_changes+=changed;
                if(original_regions.face_region[f]==curve_regions->face_region[f])CHECK_FALSE(changed);
                if(curve_anchors[f]){CHECK_FALSE(changed);CHECK(original_regions.face_region[f]==curve_regions->face_region[f]);}
            }
        }
        if(constraint_probe && refined) {
            const auto before=puzzle;
            ColorMatching::PortraitColorConstraintOptions policy;
            policy.geometric_coherence=env("ORCA_COLOR_GEOMETRIC_COHERENCE")!="0";
            constrained_faces=puzzle.constrain_selected_colors(*surface,detail_selection,faces,constraint_labels,constraint_names,policy);
            std::vector<uint8_t> selected(faces.size(),0);for(size_t f:detail_selection)selected[f]=1;
            size_t outside=0;
            for(size_t f=0;f<faces.size();++f)if(!selected[f])outside+=puzzle.filament_slots.at(puzzle.face_piece[f])!=before.filament_slots.at(before.face_piece[f]);
            CHECK(outside==0);
            CHECK(puzzle.constrain_selected_colors(*surface,detail_selection,faces,constraint_labels,constraint_names,policy)==0);
        }
        if(boundary_probe && refined) {
            std::vector<size_t> selected(faces.size());std::iota(selected.begin(),selected.end(),0);
            // Explicit offline selection, no inferred facial annotations. This
            // measures real face-slot repair, not semantic quality or GUI use.
            const auto protection=puzzle.protect_color_intent(*surface,faces,std::vector<uint8_t>(faces.size(),0));
            repaired_faces=puzzle.smooth_selected_boundaries(*surface,selected,protection);
        }
        const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
        REQUIRE_NOTHROW(puzzle.validate(*surface));
        const auto encoded=puzzle.encode();auto restored=BeautyPuzzle::decode(encoded,surface->geometry_id,faces.size());
        restored.match_filaments(*surface,palette,{},faces,refined);CHECK(restored.same_edit(puzzle));
        auto reordered=palette;std::reverse(reordered.begin(),reordered.end());
        restored.match_filaments(*surface,reordered,{},faces,refined);
        CHECK(restored.face_piece==puzzle.face_piece);CHECK(restored.colors==puzzle.colors);
        CHECK(restored.target_colors==puzzle.target_colors);CHECK(restored.filament_slots==puzzle.filament_slots);
        if(refined && !boundary_probe && !constraint_probe && !curve_probe) {
            struct Mean {std::array<double,3> rgb{};double area=0;};
            std::map<uint32_t,Mean> means;
            for(size_t f=0;f<faces.size();++f) {
                auto& mean=means[puzzle.face_piece[f]];const double area=std::max(surface->areas[f],1e-15);
                mean.area+=area;for(size_t c=0;c<3;++c)mean.rgb[c]+=area*faces[f][c];
            }
            double largest_error=0;
            for(const auto& entry:means)for(size_t c=0;c<3;++c)
                largest_error=std::max(largest_error,std::abs(entry.second.rgb[c]/entry.second.area-puzzle.target_colors.at(entry.first)[c]));
            CHECK(largest_error<1e-6);
        }
        const std::string mode=refined?"candidate":"baseline";const auto directory=root/mode;
        REQUIRE(boost::filesystem::create_directory(directory));
        nlohmann::json files=nlohmann::json::object();
        const auto write=[&](const char* name,const auto& values,const char* dtype,size_t columns) {
            const auto path=directory/name;boost::filesystem::ofstream out(path,std::ios::binary);
            const size_t bytes=values.size()*sizeof(values[0]);
            out.write(reinterpret_cast<const char*>(values.data()),std::streamsize(bytes));out.close();REQUIRE(out.good());
            files[name]={{"sha256",model_artifact_sha256(path)},{"bytes",bytes},{"dtype",dtype},{"shape",{values.size()/columns,columns}}};
        };
        std::vector<float> vertices,source_rgb,automatic_rgb,edge_lengths;std::vector<int32_t> triangles,neighbors;
        for(const auto& v:mesh.its.vertices)for(int c=0;c<3;++c)vertices.push_back(v[c]);
        for(const auto& f:mesh.its.indices)for(int c=0;c<3;++c)triangles.push_back(f[c]);
        for(const auto& f:surface->face_neighbors)for(int32_t n:f)neighbors.push_back(n);
        for(const auto& f:surface->face_edge_lengths)for(float length:f)edge_lengths.push_back(length);
        for(size_t f=0;f<faces.size();++f)for(int c=0;c<3;++c){source_rgb.push_back(faces[f][c]);automatic_rgb.push_back(puzzle.colors.at(puzzle.face_piece[f])[c]);}
        const uint32_t endian=1;REQUIRE(*reinterpret_cast<const unsigned char*>(&endian)==1);
        write("vertices.f32",vertices,"<f4",3);write("triangles.i32",triangles,"<i4",3);
        write("source.f32",source_rgb,"<f4",3);write("automatic.f32",automatic_rgb,"<f4",3);
        write("areas.f64",surface->areas,"<f8",1);write("pieces.u32",puzzle.face_piece,"<u4",1);
        write("neighbors.i32",neighbors,"<i4",3);
        const auto adjacency_diagnostic=files.at("neighbors.i32");files.erase("neighbors.i32");
        write("edge_lengths.f32",edge_lengths,"<f4",3);
        const auto geometric_diagnostic=nlohmann::json{{"edge_lengths",files.at("edge_lengths.f32")},{"model_extent",surface->geometry_extent},
            {"reference_length",surface->geometry_extent*ColorMatching::PortraitColorConstraintOptions{}.boundary_scale_fraction},
            {"geometric_coherence",constraint_probe && refined && env("ORCA_COLOR_GEOMETRIC_COHERENCE")!="0"}};
        files.erase("edge_lengths.f32");
        nlohmann::json curve_diagnostic;
        if(curve_regions) {
            write("editing.u32",curve_regions->face_region,"<u4",1);write("anchors.u8",curve_anchors,"|u1",1);
            curve_diagnostic={{"editing",files.at("editing.u32")},{"anchors",files.at("anchors.u8")},
                {"selected_region",2},{"role",curve_role},{"initial_faces",curve_initial_faces},
                {"changed_region_faces",curved_faces},{"changed_material_faces",curve_material_changes},
                {"method","Explicit feature ROI plus existing material selection; white and other known features anchored. Not expert anatomical ground truth."}};
            files.erase("editing.u32");files.erase("anchors.u8");
            boost::filesystem::ofstream record(directory/"edit-regions.json");record<<curve_regions->encode().dump(2);
            record.close();REQUIRE(record.good());
        }
        const auto loaded=signature(files["vertices.f32"]["sha256"].get<std::string>()+files["triangles.i32"]["sha256"].get<std::string>()+files["source.f32"]["sha256"].get<std::string>());
        boost::filesystem::ofstream metadata(directory/"manifest.json");
        metadata<<nlohmann::json({{"schema",1},{"source_sha256",source_hash},{"geometry_id",surface->geometry_id},
            {"loaded_mesh_colors_sha256",loaded},{"output_sha256",signature(encoded.dump())},{"palette",encoded.at("palette")},
            {"files",files},{"adjacency_diagnostic",adjacency_diagnostic},{"geometric_diagnostic",geometric_diagnostic},{"curve_diagnostic",curve_diagnostic},
            {"source_representation","native loader face-mean sRGB; not intrinsic albedo"},
            {"automatic_method",curve_probe?(refined?std::string(BeautyRegionBoundaryPlan::algorithm_version)+"; explicit local curve edit + adjacent material extension":"automatic-color-regions-v1 + selected-source-color-details-v1; same local selection, before explicit curve edit"):
                constraint_probe?(refined?std::string(ColorMatching::PortraitColorConstraintOptions::algorithm_version)+"; supplied "+guidance_kind:"current automatic-color-regions-v1 + selected-source-color-details-v1; same palette, source and selection"):
                detail_probe?(refined?"selected-source-color-details-v1; explicit geometric selection, no recognition":"current automatic-color-regions-v1; no recognition"):boundary_probe?(refined?"explicit face-slot boundary repair; no recognition":"current automatic-color-regions-v1; no recognition"):
                refined?std::string(ColorMatching::ColorRegionOptions::algorithm_version)+"; no recognition":"workbench baseline; no recognition"}}).dump(2);
        metadata.close();REQUIRE(metadata.good());
        timings[mode]={{"matching_seconds",seconds},{"pieces",puzzle.piece_count()},{"repaired_faces",repaired_faces},
            {"constrained_faces",constrained_faces},
            {"curve_region_faces",curved_faces},{"curve_material_faces",curve_material_changes},
            {"selected_faces",detail_selection.size()},{"selection_min_z",selection_z},{"selection_top_fraction",top_fraction}};
        if(check_save) {
            ModelFinishingOptions options;options.smooth_surface=false;options.repair_mesh=false;
            options.beauty_puzzle=true;options.beauty_surface=surface;
            options.beauty_document={{"puzzle",encoded},{"puzzle_base_file",save_base.filename().string()},{"puzzle_base_sha256",source_hash}};
            if(curve_regions)options.beauty_document["edit_regions"]=curve_regions->encode();
            const auto destination=root/(mode+"-saved.glb");
            const auto finished=finish_model_artifact(save_base,destination,options);
            saves[mode]={{"success",finished.success},{"error",finished.error},{"output_sha256",finished.output_sha256},
                         {"changed_texture_pixels",finished.changed_texture_pixels},{"faces_before",finished.faces_before},{"faces_after",finished.faces_after}};
            if(!refined)baseline_can_save=finished.success;
            else if(baseline_can_save){INFO(finished.error);CHECK(finished.success);}
            if(finished.success) {
                CHECK(finished.faces_before==faces.size());CHECK(finished.faces_after==faces.size());
                CHECK(model_artifact_sha256(destination)==finished.output_sha256);
                if(curve_regions) {
                    TriangleMesh saved_mesh;ObjInfo saved_colors;std::string saved_error;
                    REQUIRE(load_model_artifact(destination,saved_mesh,saved_colors,saved_error));
                    REQUIRE(saved_mesh.its.indices==mesh.its.indices);REQUIRE(saved_mesh.its.vertices==mesh.its.vertices);
                    REQUIRE(saved_colors.vertex_colors.size()==saved_mesh.its.vertices.size());
                    std::vector<float> saved_rgb(faces.size()*3,0);
                    for(size_t f=0;f<faces.size();++f)for(int v:saved_mesh.its.indices[f])for(size_t c=0;c<3;++c)
                        saved_rgb[f*3+c]+=saved_colors.vertex_colors[size_t(v)][c]/3.f;
                    const auto saved_path=directory/"saved_face_colors.f32";
                    boost::filesystem::ofstream out(saved_path,std::ios::binary);
                    out.write(reinterpret_cast<const char*>(saved_rgb.data()),std::streamsize(saved_rgb.size()*sizeof(float)));
                    out.close();REQUIRE(out.good());
                    saves[mode]["reloaded_colors"]={{"sha256",model_artifact_sha256(saved_path)},{"dtype","<f4"},{"shape",{faces.size(),3}},
                        {"representation","Native loader vertex samples of saved texture, averaged per face; not per-pixel texture interior."}};
                    const auto record_path=directory/"edit-record.json";
                    boost::filesystem::ofstream record(record_path);record<<options.beauty_document.dump(2);record.close();REQUIRE(record.good());
                    boost::filesystem::ifstream reopened(record_path);nlohmann::json restored_record;reopened>>restored_record;
                    const auto saved_geometry=SurfaceSelectionPersistence::geometry_fingerprint(saved_mesh.its);
                    CHECK(BeautyPuzzle::decode(restored_record.at("puzzle"),saved_geometry,faces.size()).same_edit(puzzle));
                    CHECK(BeautyEditRegions::decode(restored_record.at("edit_regions"),saved_geometry,source_hash,faces.size()).face_region==curve_regions->face_region);
                }
                // The production finisher reloads the output and verifies exact
                // vertices/triangles before returning success.
            } else CHECK_FALSE(boost::filesystem::exists(destination));
            CHECK(model_artifact_sha256(save_base)==source_hash);
        }
    }
    CHECK(model_artifact_sha256(source_path)==source_hash);
    if(!palette_file.empty())CHECK(model_artifact_sha256(boost::filesystem::path(palette_file))==palette_hash);
    if(constraint_probe || curve_probe)CHECK(model_artifact_sha256(boost::filesystem::path(guidance_file))==guidance_hash);
    boost::filesystem::ofstream report(root/"probe.json");report<<nlohmann::json({{"source_sha256",source_hash},{"source_unchanged",true},
        {"palette_input_sha256",palette_hash},{"palette_input_kind",palette_file.empty()?"fixed software baseline":"supplied software HEX list; physical materials unverified"},
        {"guidance_input_sha256",guidance_hash},{"guidance_kind",guidance_kind},
        {"faces",faces.size()},{"timings",timings},{"save",saves}}).dump(2);
    report.close();REQUIRE(report.good());
}
