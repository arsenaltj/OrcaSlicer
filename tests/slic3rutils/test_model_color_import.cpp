#include "slic3r/GUI/TextureImportDialog.hpp"
#include "slic3r/GUI/AI/Orca/OrcaWorkspaceAdapter.hpp"
#include "slic3r/GUI/AI/Orca/AIImportSeamRepair.hpp"
#include "libslic3r/TexturePainting.hpp"
#include "slic3r/GUI/ModelColorImportResult.hpp"
#include "slic3r/GUI/AI/ModelGeneration/BeautyWorkbenchControls.hpp"
#include "slic3r/GUI/TextureImportModel.hpp"
#include <catch2/catch_test_macros.hpp>
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Semver.hpp"
#include "test_utils.hpp"
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <boost/filesystem/fstream.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdlib>

using namespace Slic3r;
using namespace Slic3r::GUI;

// Explicit saved-asset consumer check; does not open a window or slice a model.
TEST_CASE("Refined saved assets hand exact per-child slots to native facet painting", "[.][BeautyRegionPrecisionImportProbe]") {
    const char* value=std::getenv("ORCA_PRECISION_IMPORT_SOURCE");if(!value || !*value)SKIP("Explicit saved precision GLB required.");
    const boost::filesystem::path path(value);const auto hash=AI::model_artifact_sha256(path);
    AI::ModelImportRequest request;REQUIRE_NOTHROW(BeautyWorkbenchControls::prepare_import(path,request));REQUIRE(request.matched_colors.has_value());REQUIRE(request.matched_colors->valid());CHECK(request.matched_colors->source_sha256==hash);
    TriangleMesh mesh;ObjInfo info;std::string error;REQUIRE(AI::load_model_artifact(path,mesh,info,error));CHECK(request.matched_colors->geometry_id==AI::SurfaceSelectionPersistence::geometry_fingerprint(mesh.its));
    std::vector<uint32_t> slots(mesh.its.indices.size());boost::filesystem::ifstream input(path.parent_path()/"slots.u32",std::ios::binary);input.read(reinterpret_cast<char*>(slots.data()),slots.size()*sizeof(uint32_t));REQUIRE(bool(input));REQUIRE(request.matched_colors->face_slots.size()==slots.size());
    for(size_t f=0;f<slots.size();++f)CHECK(request.matched_colors->face_slots[f]==slots[f]);
    Model model;auto* volume=model.add_object()->add_volume(mesh);model.texture_mesh=std::make_shared<TexturedMesh>();
    for(const auto& v:mesh.its.vertices)model.texture_mesh->vertices.push_back({v.x(),v.y(),v.z()});for(const auto& t:mesh.its.indices)model.texture_mesh->indices.push_back({t.x(),t.y(),t.z()});
    auto options=model_import_color_options(request);options.matched_source=std::make_shared<indexed_triangle_set>(mesh.its);
    REQUIRE_NOTHROW(apply_matched_texture_colors(model,options,options.matched_filaments));TriangleSelector expected(mesh);
    for(size_t f=0;f<slots.size();++f)expected.set_facet(int(f),EnforcerBlockerType(int(EnforcerBlockerType::Extruder1)+int(slots[f])));
    CHECK(volume->mmu_segmentation_facets.get_data()==expected.serialize());CHECK(AI::model_artifact_sha256(path)==hash);
    nlohmann::json receipt{{"source_sha256",hash},{"faces",slots.size()},{"geometry_id",request.matched_colors->geometry_id},{"exact_facet_slots",true},{"gui",false},{"slicing",false},{"three_mf_roundtrip",false}};
    if(const char* roundtrip=std::getenv("ORCA_PRECISION_IMPORT_3MF");roundtrip && std::string(roundtrip)=="1") {
        const auto project=path.parent_path()/"refined-materials.3mf";REQUIRE_FALSE(boost::filesystem::exists(project));
        const auto facets=volume->mmu_segmentation_facets.get_data();const auto geometry=volume->mesh().its;
        ScopedTemporaryDir backup("precision_3mf");model.set_backup_path(backup.string());model.add_default_instances();
        DynamicPrintConfig config=DynamicPrintConfig::full_print_config();std::vector<std::string> palette(6);
        for(const auto& channel:request.matched_colors->palette){REQUIRE(channel.slot<palette.size());palette[channel.slot]=channel.display_color;}
        config.set_key_value("filament_colour",new ConfigOptionStrings(palette));
        PlateData plate;plate.plate_index=0;StoreParams params;params.path=project.string();params.model=&model;params.config=&config;
        params.plate_data_list.push_back(&plate);params.strategy=SaveStrategy::Zip64|SaveStrategy::Silence;REQUIRE(store_bbs_3mf(params));
        Model restored;ScopedTemporaryDir restore_backup("precision_3mf_restore");restored.set_backup_path(restore_backup.string());
        boost::filesystem::create_directories(restore_backup.path()/"Metadata");DynamicPrintConfig restored_config;
        ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Enable};PlateDataPtrs plates;std::vector<Preset*> presets;
        bool bbl=false,orca=false;Semver version;
        const bool loaded=load_bbs_3mf(project.string().c_str(),&restored_config,&substitutions,&restored,&plates,&presets,&bbl,&orca,&version,nullptr,
            LoadStrategy::LoadModel|LoadStrategy::LoadConfig);
        release_PlateData_list(plates);for(auto* preset:presets)delete preset;REQUIRE(loaded);
        REQUIRE(restored.objects.size()==1);REQUIRE(restored.objects[0]->volumes.size()==1);const auto* reopened=restored.objects[0]->volumes[0];
        REQUIRE(reopened->mesh().its.indices==geometry.indices);REQUIRE(reopened->mesh().its.vertices.size()==geometry.vertices.size());
        for(size_t v=0;v<geometry.vertices.size();++v)CHECK_THAT((reopened->mesh().its.vertices[v]-geometry.vertices[v]).norm(),Catch::Matchers::WithinAbs(0,1e-3));
        CHECK(reopened->mmu_segmentation_facets.get_data()==facets);CHECK(volume->mmu_segmentation_facets.get_data()==facets);
        REQUIRE(restored_config.option<ConfigOptionStrings>("filament_colour"));CHECK(restored_config.option<ConfigOptionStrings>("filament_colour")->values==palette);
        REQUIRE(AI::model_artifact_sha256(path)==hash);
        receipt["three_mf_roundtrip"]=true;receipt["three_mf_sha256"]=AI::model_artifact_sha256(project);receipt["three_mf_file"]=project.filename().string();
        receipt["exact_three_mf_facets"]=true;receipt["exact_three_mf_palette"]=true;receipt["three_mf_vertex_tolerance_mm"]=1e-3;
    }
    const auto proof=path.parent_path()/"import-proof.json";REQUIRE_FALSE(boost::filesystem::exists(proof));boost::filesystem::ofstream report(proof);report<<receipt.dump(2);report.close();REQUIRE(bool(report));
}

TEST_CASE("Workbench mixed colors import as native virtual slots and reject altered mixtures before painting", "[ModelColorImport][BeautyWorkbench]") {
    const auto mesh=its_make_cube(10,10,10);Model model;
    auto* volume=model.add_object()->add_volume(TriangleMesh(mesh));
    model.texture_mesh=std::make_shared<TexturedMesh>();
    AI::ModelImportRequest request;AI::ModelMatchedColors matched;
    matched.source_sha256=std::string(64,'a');matched.geometry_id=AI::SurfaceSelectionPersistence::geometry_fingerprint(mesh);
    matched.palette={{0,"#FFFFFF","PLA",true},{1,"#000000","PLA",true}};
    matched.mixed_recipes={{"#808080",{{0,.4},{1,.6}},2,std::string(64,'b')}};
    matched.face_slots.assign(mesh.indices.size(),2);matched.face_slots[0]=0;
    REQUIRE(matched.valid());request.matched_colors=matched;
    auto options=model_import_color_options(request);options.matched_source=std::make_shared<indexed_triangle_set>(mesh);
    REQUIRE(options.matched_filaments.back().kind==TextureFilamentKind::ExistingMixed);
    const auto blank=volume->mmu_segmentation_facets.get_data();
    auto altered=options.matched_filaments;altered.back().mixed_ratios={60,40};
    CHECK_THROWS(apply_matched_texture_colors(model,options,altered));CHECK(volume->mmu_segmentation_facets.get_data()==blank);
    auto incompatible = options.matched_filaments;
    incompatible.front().compatible = false;
    CHECK_THROWS(apply_matched_texture_colors(model,options,incompatible));
    CHECK(volume->mmu_segmentation_facets.get_data()==blank);
    apply_matched_texture_colors(model,options,options.matched_filaments);
    TriangleSelector expected(volume->mesh());
    for(size_t f=0;f<mesh.indices.size();++f)expected.set_facet(int(f),EnforcerBlockerType(int(EnforcerBlockerType::Extruder1)+int(matched.face_slots[f])));
    CHECK(volume->mmu_segmentation_facets.get_data()==expected.serialize());
    matched.mixed_recipes[0].components[0].slot=2;CHECK_FALSE(matched.valid());
}
TEST_CASE("Saved workbench assignments import exact slots even when filaments share RGB", "[ModelColorImport][BeautyWorkbench]") {
    const auto mesh=its_make_cube(10,10,10);
    Model model;auto* object=model.add_object();auto* volume=object->add_volume(TriangleMesh(mesh));
    object->center_around_origin(false);
    model.texture_mesh=std::make_shared<TexturedMesh>();
    for(const auto& v:mesh.vertices)model.texture_mesh->vertices.push_back({v.x(),v.y(),v.z()});
    for(const auto& f:mesh.indices)model.texture_mesh->indices.push_back({f.x(),f.y(),f.z()});
    model.texture_mesh->precomputed_face_colors.assign(mesh.indices.size(),{34,34,34});
    AI::ModelImportRequest request;AI::ModelMatchedColors matched;
    matched.source_sha256=std::string(64,'a');matched.geometry_id=AI::SurfaceSelectionPersistence::geometry_fingerprint(mesh);
    matched.palette={{1,"#222222","PLA",true},{3,"#222222","PLA",true}};
    for(size_t f=0;f<mesh.indices.size();++f)matched.face_slots.push_back(f%2?1:3);
    REQUIRE(matched.valid());request.matched_colors=matched;
    auto options=model_import_color_options(request);
    // Texture seams duplicate vertices in the workbench, while the native
    // importer welds them. The physical triangles keep the same order.
    auto seams=std::make_shared<indexed_triangle_set>();
    for(const auto& face:mesh.indices) {
        const int first=int(seams->vertices.size());
        for(int corner=0;corner<3;++corner)seams->vertices.push_back(mesh.vertices[face[corner]]);
        seams->indices.emplace_back(first,first+1,first+2);
    }
    options.matched_source=seams;
    const auto before=volume->mesh().its;
    apply_matched_texture_colors(model,options,options.matched_filaments);
    CHECK(volume->mesh().its.vertices==before.vertices);
    CHECK(volume->mesh().its.indices==before.indices);
    TriangleSelector expected(volume->mesh());
    for(size_t f=0;f<mesh.indices.size();++f)expected.set_facet(int(f),EnforcerBlockerType(int(EnforcerBlockerType::Extruder1)+int(matched.face_slots[f])));
    CHECK(volume->mmu_segmentation_facets.get_data()==expected.serialize());
    const auto painting=volume->mmu_segmentation_facets.get_data();
    std::swap(seams->indices[0],seams->indices[1]);
    CHECK_THROWS(apply_matched_texture_colors(model,options,options.matched_filaments));
    CHECK(volume->mmu_segmentation_facets.get_data()==painting);
    std::swap(seams->indices[0],seams->indices[1]);
    auto changed=options.matched_filaments;changed.front().color_hex="#FFFFFF";
    CHECK_THROWS(apply_matched_texture_colors(model,options,changed));
    CHECK(volume->mmu_segmentation_facets.get_data()==painting);
    options.matched_face_slots.pop_back();
    CHECK_THROWS(apply_matched_texture_colors(model,options,options.matched_filaments));
    CHECK(volume->mmu_segmentation_facets.get_data()==painting);
    options.matched_face_slots=matched.face_slots;options.matched_face_slots[0]=99;
    CHECK_THROWS(apply_matched_texture_colors(model,options,options.matched_filaments));
    CHECK(volume->mmu_segmentation_facets.get_data()==painting);
}

TEST_CASE("Texture import accepts vertex aliases but rejects changed ordered triangle geometry", "[ModelColorImport][WorkbenchTextureImport]") {
    auto source=std::make_shared<indexed_triangle_set>(its_make_cube(10,10,10));
    for(auto& v:source->vertices)v+=Vec3f(20,30,40);
    auto native=*source;
    native.vertices.push_back(native.vertices[native.indices[0][0]]+Vec3f(.000002f,0,0));
    native.indices[0][0]=int(native.vertices.size()-1);
    CHECK_FALSE(LocalPrintColorApplication::same_surface_partition(*source,native));
    Model model;auto* volume=model.add_object()->add_volume(TriangleMesh(native));
    model.objects.front()->center_around_origin(false);
    model.texture_mesh=std::make_shared<TexturedMesh>();
    AI::ModelImportRequest request;AI::ModelMatchedColors matched;
    matched.source_sha256=std::string(64,'a');matched.geometry_id=AI::SurfaceSelectionPersistence::geometry_fingerprint(*source);
    matched.palette={{0,"#FFFFFF","PLA",true},{1,"#000000","PLA",true}};
    for(size_t f=0;f<source->indices.size();++f)matched.face_slots.push_back(f%2);
    request.matched_colors=matched;auto options=model_import_color_options(request);options.matched_source=source;
    bool rejected=false;
    SECTION("same physical corners receive every original face assignment") {}
    SECTION("reordered faces reject before any paint is published") {std::swap(source->indices[0],source->indices[1]);rejected=true;}
    SECTION("reversed winding rejects before any paint is published") {std::swap(source->indices[0][1],source->indices[0][2]);rejected=true;}
    SECTION("changed positions reject before any paint is published") {source->vertices[0].x()+=.01f;rejected=true;}
    SECTION("dropped faces reject before any paint is published") {source->indices.pop_back();rejected=true;}
    const auto before=volume->mmu_segmentation_facets.get_data();
    if(rejected) {
        CHECK_THROWS(apply_matched_texture_colors(model,options,options.matched_filaments));
        CHECK(volume->mmu_segmentation_facets.get_data()==before);
    } else {
        REQUIRE_NOTHROW(apply_matched_texture_colors(model,options,options.matched_filaments));
        TriangleSelector expected(volume->mesh());
        for(size_t f=0;f<matched.face_slots.size();++f)
            expected.set_facet(int(f),EnforcerBlockerType(int(EnforcerBlockerType::Extruder1)+int(matched.face_slots[f])));
        CHECK(volume->mmu_segmentation_facets.get_data()==expected.serialize());
    }
}

TEST_CASE("AI import stitches exact texture seams after painting without losing any face assignment", "[ModelColorImport][AIImportSeam]") {
    const auto cube=its_make_cube(10,10,10);
    indexed_triangle_set seams;
    for(const auto& face:cube.indices) {
        const int start=int(seams.vertices.size());
        for(int corner=0;corner<3;++corner)seams.vertices.push_back(cube.vertices[face[corner]]);
        seams.indices.emplace_back(start,start+1,start+2);
    }
    Model model;auto* object=model.add_object();auto* volume=object->add_volume(TriangleMesh(seams));
    TriangleSelector painting(volume->mesh());
    for(size_t face=0;face<seams.indices.size();++face)painting.set_facet(int(face),
        face%2?EnforcerBlockerType::Extruder2:EnforcerBlockerType::Extruder1);
    volume->mmu_segmentation_facets.set(painting);
    const auto colors=volume->mmu_segmentation_facets.get_data();
    const auto before=volume->mesh().its;
    CHECK(its_num_open_edges(volume->mesh().its)>0);
    const auto result = stitch_ai_import_seams(*object);
    REQUIRE(result.stitched_volumes == 1);
    CHECK_FALSE(result.has_open_edges);
    CHECK(its_num_open_edges(volume->mesh().its)==0);
    CHECK(volume->mesh().its.indices.size()==seams.indices.size());
    CHECK(volume->mmu_segmentation_facets.get_data()==colors);
    for(size_t f=0;f<seams.indices.size();++f)for(int corner=0;corner<3;++corner)
        CHECK(volume->mesh().its.vertices[volume->mesh().its.indices[f][corner]]==before.vertices[before.indices[f][corner]]);
    const auto repeated = stitch_ai_import_seams(*object);
    CHECK(repeated.stitched_volumes == 0);
    CHECK_FALSE(repeated.has_open_edges);
}

TEST_CASE("AI import leaves a genuine opening and its painting unchanged for manual repair", "[ModelColorImport][AIImportSeam]") {
    const auto cube=its_make_cube(10,10,10);
    indexed_triangle_set open;
    for(size_t f=1;f<cube.indices.size();++f) {
        const auto& face=cube.indices[f];const int start=int(open.vertices.size());
        for(int corner=0;corner<3;++corner)open.vertices.push_back(cube.vertices[face[corner]]);
        open.indices.emplace_back(start,start+1,start+2);
    }
    Model model;auto* object=model.add_object();auto* volume=object->add_volume(TriangleMesh(open));
    TriangleSelector painting(volume->mesh());painting.set_facet(0,EnforcerBlockerType::Extruder1);
    volume->mmu_segmentation_facets.set(painting);
    const auto colors=volume->mmu_segmentation_facets.get_data();
    const auto result = stitch_ai_import_seams(*object);
    REQUIRE(result.stitched_volumes == 0);
    CHECK(result.has_open_edges);
    CHECK(volume->mesh().its.indices==open.indices);
    CHECK(volume->mmu_segmentation_facets.get_data()==colors);
    CHECK(its_num_open_edges(volume->mesh().its)>0);
}

TEST_CASE("AI original color imports keep target count editable independently of physical feeds", "[ModelColorImport]")
{
    AI::ModelImportRequest request;
    const auto options = model_import_color_options(request);
    CHECK(options.initial_target_colors == 12);
    CHECK(options.initial_target_colors > options.physical_filament_limit);
    CHECK(options.fixed_palette.empty());
    CHECK(options.fixed_mapping_palette.empty());
    CHECK(options.initial_color_smoothing == 0);
    CHECK(options.preserve_existing_filaments);
    CHECK(options.z_up);
    CHECK_FALSE(options.source_units_in_meters);
    const auto cube = its_make_cube(10, 10, 10);
    TexturedMesh source;
    for (const auto& v : cube.vertices) source.vertices.push_back({v.x(), v.y(), v.z()});
    for (const auto& f : cube.indices) source.indices.push_back({f.x(), f.y(), f.z()});
    source.precomputed_face_colors = {{255,0,0}, {0,255,0}, {0,0,255}, {255,255,0},
        {255,0,255}, {0,255,255}, {0,0,0}, {255,255,255}, {128,0,0}, {0,128,0}, {0,0,128}, {128,128,128}};
    REQUIRE(source.indices.size() == source.precomputed_face_colors.size());
    TexturePaintingSettings settings;
    settings.target_colors_num = options.initial_target_colors;
    settings.smooth_weight = options.initial_color_smoothing / 10.;
    settings.fixed_palette = options.fixed_palette;
    settings.fixed_mapping_palette = options.fixed_mapping_palette;
    PaintedMesh painted;
    REQUIRE(face_colors_to_painting(source, painted, settings));
    CHECK(painted.cluster_colors.size() > options.physical_filament_limit);
    CHECK(painted.indices == source.indices);
    // The AI handoff must not change defaults for ordinary file imports.
    const TextureImportOptions ordinary;
    CHECK(ordinary.initial_target_colors == 0);
    CHECK(ordinary.initial_color_smoothing == -1);
    CHECK(ordinary.physical_filament_limit == 0);
}

TEST_CASE("AI recoloring preserves accepted trial and local face overrides across a fresh match", "[ModelColorImport]")
{
    AI::ModelImportRequest request;
    request.color_trial = AI::ModelColorTrial {{{.8f, .6f, .5f}, {.7f, .2f, .2f}},
                                               {{.9f, .7f, .6f}, {.8f, .3f, .4f}}};
    request.face_color_overrides = {{7, {.2f, .7f, .3f}}};
    const auto accepted = model_import_color_options(request);
    CHECK(accepted.fixed_mapping_palette == std::vector<std::array<size_t, 3>> {{204, 153, 128}, {179, 51, 51}});
    CHECK(accepted.fixed_palette == std::vector<std::array<size_t, 3>> {{230, 179, 153}, {204, 77, 102}});
    CHECK(accepted.face_color_overrides == std::vector<std::pair<size_t, std::array<size_t, 3>>> {{7, {51, 179, 77}}});
    request.color_trial.reset();
    const auto fresh = model_import_color_options(request);
    CHECK(fresh.fixed_palette.empty());
    CHECK(fresh.fixed_mapping_palette.empty());
    CHECK(fresh.face_color_overrides == accepted.face_color_overrides);
    CHECK(fresh.initial_color_smoothing == 0);
}

TEST_CASE("Native color import feedback counts physical and mixed filament assignments", "[ModelColorImport]")
{
    Model model;
    auto* object = model.add_object();
    auto* volume = object->add_volume(TriangleMesh(its_make_cube(10, 10, 10)), ModelVolumeType::MODEL_PART, false);
    const auto& mesh = volume->mesh().its;
    PaintedMesh painted;
    for (const auto& vertex : mesh.vertices)
        painted.vertices.push_back({vertex.x(), vertex.y(), vertex.z()});
    for (const auto& face : mesh.indices)
        painted.indices.push_back({face[0], face[1], face[2]});
    painted.cluster_colors = {{255, 0, 0}, {128, 0, 128}};
    for (size_t face = 0; face < mesh.indices.size(); ++face)
        painted.face_colors.push_back(painted.cluster_colors[face % 2]);
    // The native matcher remaps newly created mixtures into project IDs; these
    // must not be restricted to the 1-6 physical feed slots on the AI side.
    std::vector<FilamentMatch> matches(2);
    matches[0].cluster_index = 0;
    matches[0].filament_index = 0;
    matches[1].cluster_index = 1;
    matches[1].filament_index = 7;
    REQUIRE(apply_painted_mesh_to_volume(painted, matches, *volume));

    ModelColorImportResult result;
    collect_model_color_import_result(&result, model, painted.cluster_colors.size());
    CHECK(result.colors_applied);
    CHECK_FALSE(result.cancelled);
    CHECK(result.source_color_count == 2);
    CHECK(result.mapped_color_count == 2);
    CHECK(volume->mmu_segmentation_facets.has_facets(*volume, EnforcerBlockerType::Extruder8));
}

TEST_CASE("Geometry-only imports do not report successful color matching", "[ModelColorImport]")
{
    Model model;
    model.add_object()->add_volume(TriangleMesh(its_make_cube(10, 10, 10)), ModelVolumeType::MODEL_PART, false);
    ModelColorImportResult result;
    collect_model_color_import_result(&result, model, 0);
    CHECK_FALSE(result.colors_applied);
    CHECK(result.mapped_color_count == 0);
}

TEST_CASE("Color matching closes exact texture seams before prompting for repair", "[ModelColorImport][AIImportSeam]") {
    const auto cube = its_make_cube(10, 10, 10);
    TexturedMesh source;
    for (size_t face = 0; face < cube.indices.size(); ++face) {
        const int first = int(source.vertices.size());
        for (int corner = 0; corner < 3; ++corner) {
            const auto& vertex = cube.vertices[cube.indices[face][corner]];
            source.vertices.push_back({vertex.x(), vertex.y(), vertex.z()});
        }
        source.indices.push_back({first, first + 1, first + 2});
        source.precomputed_face_colors.push_back(face % 2 ?
            std::array<size_t, 3>{0, 0, 255} : std::array<size_t, 3>{255, 0, 0});
    }
    bool repair_prompt = false;
    TexturePaintingSettings settings;
    settings.target_colors_num = 2;
    settings.smooth_weight = 0;
    settings.fixed_palette = {{255, 0, 0}, {0, 0, 255}};
    settings.mesh_repair_decision = TexturePaintingSettings::MeshRepairDecision::Ask;
    settings.mesh_repair_decision_required = &repair_prompt;
    PaintedMesh painted;
    REQUIRE(face_colors_to_painting(source, painted, settings));
    CHECK_FALSE(repair_prompt);
    CHECK(painted.face_colors == source.precomputed_face_colors);
    indexed_triangle_set closed;
    for (const auto& vertex : painted.vertices)
        closed.vertices.emplace_back(vertex[0], vertex[1], vertex[2]);
    for (const auto& face : painted.indices)
        closed.indices.emplace_back(face[0], face[1], face[2]);
    CHECK(its_num_open_edges(closed) == 0);
}

TEST_CASE("Color matching still warns for a real missing face", "[ModelColorImport][AIImportSeam]") {
    const auto cube = its_make_cube(10, 10, 10);
    TexturedMesh source;
    for (size_t face = 1; face < cube.indices.size(); ++face) {
        const int first = int(source.vertices.size());
        for (int corner = 0; corner < 3; ++corner) {
            const auto& vertex = cube.vertices[cube.indices[face][corner]];
            source.vertices.push_back({vertex.x(), vertex.y(), vertex.z()});
        }
        source.indices.push_back({first, first + 1, first + 2});
        source.precomputed_face_colors.push_back({255, 0, 0});
    }
    bool repair_prompt = false;
    TexturePaintingSettings settings;
    settings.mesh_repair_decision = TexturePaintingSettings::MeshRepairDecision::Ask;
    settings.mesh_repair_decision_required = &repair_prompt;
    PaintedMesh painted;
    CHECK_FALSE(face_colors_to_painting(source, painted, settings));
    CHECK(repair_prompt);
}
TEST_CASE("AI seam status includes untouched modifier openings and rechecks changed geometry", "[ModelColorImport][AIImportSeam]") {
    Model model;
    auto* object = model.add_object();
    const auto cube = its_make_cube(10,10,10);
    auto* solid = object->add_volume(TriangleMesh(cube));
    auto open = cube;
    open.indices.pop_back();
    auto* modifier = object->add_volume(TriangleMesh(open), ModelVolumeType::PARAMETER_MODIFIER);
    const auto id = modifier->id();
    const auto result = stitch_ai_import_seams(*object);
    CHECK(result.stitched_volumes == 0);
    CHECK(result.has_open_edges);
    CHECK(modifier->id() == id);
    CHECK(modifier->mesh().its.indices == open.indices);
    modifier->set_mesh(TriangleMesh(cube));
    CHECK_FALSE(stitch_ai_import_seams(*object).has_open_edges);
    solid->set_mesh(TriangleMesh(open));
    CHECK(stitch_ai_import_seams(*object).has_open_edges);
}

TEST_CASE("Local import seam checks compare transaction reuse with repeated adjacency checks", "[.AIImportSeamProbe]") {
    const char* source = std::getenv("ORCASLICER_MODEL_ARTIFACT_FIXTURE");
    const char* report = std::getenv("ORCASLICER_SEAM_PROBE_REPORT");
    if (!source || !report) SKIP("Set a local model and a new seam probe report path.");
    REQUIRE_FALSE(boost::filesystem::exists(report));
    const auto source_hash = AI::model_artifact_sha256(source);
    TriangleMesh mesh; ObjInfo colors; std::string error;
    REQUIRE(AI::load_model_artifact(source, mesh, colors, error));
    auto baseline = [](ModelObject& object) {
        AIImportSeamResult result;
        for (ModelVolume* volume : object.volumes) {
            if (!volume || !volume->is_model_part() || its_num_open_edges(volume->mesh().its) == 0) continue;
            TriangleMesh candidate = volume->mesh();
            if (!stitch_exact_mesh_seams(candidate)) continue;
            volume->set_mesh(std::move(candidate));
            volume->set_new_unique_id();
            ++result.stitched_volumes;
        }
        result.has_open_edges = std::any_of(object.volumes.begin(), object.volumes.end(), [](const ModelVolume* volume) {
            return volume != nullptr && its_num_open_edges(volume->mesh().its) != 0;
        });
        return result;
    };
    nlohmann::json runs = nlohmann::json::array();
    for (int run = 0; run < 6; ++run) {
        Model models[2];
        for (auto& model : models) model.add_object()->add_volume(mesh);
        AIImportSeamResult result[2];
        double ms[2];
        for (int order = 0; order < 2; ++order) {
            const int kind = (run + order) % 2;
            const auto start = std::chrono::steady_clock::now();
            result[kind] = kind ? stitch_ai_import_seams(*models[kind].objects[0]) : baseline(*models[kind].objects[0]);
            ms[kind] = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now() - start).count();
        }
        REQUIRE(result[0].stitched_volumes == result[1].stitched_volumes);
        REQUIRE(result[0].has_open_edges == result[1].has_open_edges);
        const auto& a = models[0].objects[0]->volumes[0]->mesh().its;
        const auto& b = models[1].objects[0]->volumes[0]->mesh().its;
        REQUIRE(a.vertices == b.vertices);
        REQUIRE(a.indices == b.indices);
        runs.push_back({{"run",run},{"baseline_ms",ms[0]},{"candidate_ms",ms[1]},
                        {"stitched",result[1].stitched_volumes},{"open",result[1].has_open_edges}});
    }
    REQUIRE(AI::model_artifact_sha256(source) == source_hash);
    boost::filesystem::ofstream output(report, std::ios::binary);
    output << nlohmann::json({{"source_sha256",source_hash},{"faces",mesh.its.indices.size()},{"runs",runs}}).dump(2);
    output.close();
    REQUIRE(output.good());

}

TEST_CASE("explicit single-color import preserves geometry and uses the chosen native slot", "[ModelColorImport][SingleColorImport]") {
    Model model;
    auto* object = model.add_object();
    auto* volume = object->add_volume(TriangleMesh(its_make_cube(10, 10, 10)));
    const auto before = volume->mesh().its;
    TriangleSelector painting(volume->mesh());
    painting.set_facet(0, EnforcerBlockerType::Extruder2);
    volume->mmu_segmentation_facets.set(painting);
    REQUIRE(apply_single_color_import(model, 4));
    CHECK(object->config.opt_int("extruder") == 5);
    CHECK(volume->config.opt_int("extruder") == 5);
    CHECK(volume->mmu_segmentation_facets.empty());
    CHECK(volume->mesh().its.vertices == before.vertices);
    CHECK(volume->mesh().its.indices == before.indices);
}
TEST_CASE("missing single-color choice leaves the private model untouched", "[ModelColorImport][SingleColorImport]") {
    Model model;
    auto* object = model.add_object();
    auto* volume = object->add_volume(TriangleMesh(its_make_cube(10, 10, 10)));
    const auto config = object->config.get();
    CHECK_FALSE(apply_single_color_import(model, size_t(-1)));
    CHECK(object->config.get() == config);
    CHECK(volume->mmu_segmentation_facets.empty());
}


TEST_CASE("Workbench texture matching filters incompatible slots without merging identical colors", "[ModelColorImport][TextureCompatibility]")
{
    std::vector<TextureFilamentEntry> entries(4);
    for (size_t i = 0; i < entries.size(); ++i) {
        entries[i].dialog_index = int(i);
        entries[i].project_config_index = i + 1;
        entries[i].color_hex = "#668CB6";
        entries[i].type = "PLA";
        entries[i].preset_name = "PLA fixture";
    }
    entries[1].compatible = false;
    entries[3].kind = TextureFilamentKind::ExistingMixed;
    TextureImportOptions options;
    options.workspace_presentation = true;
    const auto usable = workspace_texture_filaments(entries, &options);
    REQUIRE(usable.size() == 2);
    CHECK(usable[0].project_config_index == 1);
    CHECK(usable[1].project_config_index == 3);
    CHECK(usable[0].dialog_index == 0);
    CHECK(usable[1].dialog_index == 1);
    CHECK(usable[0].color_hex == usable[1].color_hex);
    options.workspace_presentation = false;
    CHECK(workspace_texture_filaments(entries, &options).size() == entries.size());
    CHECK(workspace_texture_filaments(entries, nullptr).size() == entries.size());
}

TEST_CASE("Workbench texture matching returns no usable slots when material identity is missing", "[ModelColorImport][TextureCompatibility]")
{
    std::vector<TextureFilamentEntry> entries(3);
    entries[0].compatible = false;
    entries[1].type = "PLA";
    entries[2].preset_name = "PLA fixture";
    TextureImportOptions options;
    options.workspace_presentation = true;
    CHECK(workspace_texture_filaments(entries, &options).empty());
}
