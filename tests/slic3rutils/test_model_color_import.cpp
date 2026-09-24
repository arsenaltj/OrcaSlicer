#include "slic3r/GUI/TextureImportDialog.hpp"
#include "slic3r/GUI/AI/Orca/OrcaWorkspaceAdapter.hpp"
#include "slic3r/GUI/AI/Orca/AIImportSeamRepair.hpp"
#include "libslic3r/TexturePainting.hpp"
#include "slic3r/GUI/ModelColorImportResult.hpp"
#include "slic3r/GUI/TextureImportModel.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace Slic3r;
using namespace Slic3r::GUI;

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
    REQUIRE(stitch_ai_import_seams(*object)==1);
    CHECK(its_num_open_edges(volume->mesh().its)==0);
    CHECK(volume->mesh().its.indices.size()==seams.indices.size());
    CHECK(volume->mmu_segmentation_facets.get_data()==colors);
    for(size_t f=0;f<seams.indices.size();++f)for(int corner=0;corner<3;++corner)
        CHECK(volume->mesh().its.vertices[volume->mesh().its.indices[f][corner]]==before.vertices[before.indices[f][corner]]);
    CHECK(stitch_ai_import_seams(*object)==0);
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
    REQUIRE(stitch_ai_import_seams(*object)==0);
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