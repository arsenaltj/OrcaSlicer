#include "slic3r/GUI/AI/ModelGeneration/ModelPreview3D.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelGenerationPresentation.hpp"
#include "slic3r/GUI/AI/ModelGeneration/PortraitNativePartition.hpp"
#include "slic3r/GUI/AI/ModelGeneration/PortraitNativePartitionCache.hpp"
#include "slic3r/GUI/AI/Orca/WorkbenchModelDecode.hpp"
#include "slic3r/GUI/AI/Model/ModelFinishing.hpp"
#include "slic3r/GUI/AI/Model/BeautyDocument.hpp"
#include "../test_utils.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <cmath>
#include <cstring>
#include <future>
#include <string_view>

using Slic3r::GUI::ModelPreview3D;
using Slic3r::GUI::ModelPreviewFrameState;
using Slic3r::Vec3d;
namespace selection = Slic3r::AI::SurfaceSelectionPersistence;
namespace trial = Slic3r::AI::ColorTrialPersistence;

namespace {
indexed_triangle_set original_appearance_fixture_mesh()
{
    indexed_triangle_set mesh;
    mesh.vertices.push_back(Slic3r::Vec3f(0.f, 0.f, 0.f));
    constexpr float tau = 6.2831853071795864769f;
    for (size_t index = 0; index < 7; ++index) {
        const float angle = tau * float(index) / 7.f;
        mesh.vertices.emplace_back(std::cos(angle), std::sin(angle), 0.f);
    }
    for (int index = 1; index <= 7; ++index)
        mesh.indices.emplace_back(0, index, index == 7 ? 1 : index + 1);
    return mesh;
}

std::vector<Slic3r::RGBA> original_appearance_fixture_colors(size_t count)
{
    const std::array<Slic3r::RGBA, 8> colors {{
        Slic3r::RGBA {1.f, 0.f, 0.f, 1.f}, Slic3r::RGBA {0.f, 1.f, 0.f, 1.f},
        Slic3r::RGBA {0.f, 0.f, 1.f, 1.f}, Slic3r::RGBA {1.f, 1.f, 0.f, 1.f},
        Slic3r::RGBA {1.f, 0.f, 1.f, 1.f}, Slic3r::RGBA {0.f, 1.f, 1.f, 1.f},
        Slic3r::RGBA {.25f, .5f, .75f, 1.f}, Slic3r::RGBA {.8f, .35f, .1f, 1.f}
    }};
    std::vector<Slic3r::RGBA> result;
    result.reserve(count);
    for (size_t index = 0; index < count; ++index) result.push_back(colors[index % colors.size()]);
    return result;
}

void write_original_appearance_json(const boost::filesystem::path& path, const nlohmann::json& value)
{
    boost::filesystem::create_directories(path.parent_path());
    boost::filesystem::ofstream output(path, std::ios::binary);
    output << value.dump();
    REQUIRE(output.good());
}
}

TEST_CASE("Preview frame completions require the current visible viewport and DPI",
          "[ModelPreviewState][ModelPreviewFrame]")
{
    ModelPreviewFrameState state;
    const auto frame = state.begin({800, 600}, 1.25);
    CHECK(state.current(frame, {800, 600}, 1.25, true));
    CHECK_FALSE(state.current(frame, {600, 600}, 1.25, true));
    CHECK_FALSE(state.current(frame, {800, 600}, 1.5, true));
    CHECK_FALSE(state.current(frame, {800, 600}, 1.25, false));
    CHECK_FALSE(state.current(frame, {0, 600}, 1.25, true));
    CHECK_FALSE(state.current(frame, {800, 0}, 1.25, true));
}

TEST_CASE("Resizing back to the same size cannot accept an older preview frame",
          "[ModelPreviewState][ModelPreviewFrame]")
{
    ModelPreviewFrameState state;
    const auto before = state.begin({800, 600}, 2.);
    state.invalidate();
    const auto narrow = state.begin({600, 600}, 2.);
    state.invalidate();
    CHECK_FALSE(state.current(before, {800, 600}, 2., true));
    CHECK_FALSE(state.current(narrow, {800, 600}, 2., true));
    CHECK(state.current(state.begin({800, 600}, 2.), {800, 600}, 2., true));
}

TEST_CASE("Replacing a model invalidates its frame even when viewport dimensions stay unchanged",
          "[ModelPreviewState][ModelPreviewFrame]")
{
    ModelPreviewFrameState state;
    const auto previous_model = state.begin({800, 600}, 1.);
    state.invalidate();
    CHECK_FALSE(state.current(previous_model, {800, 600}, 1., true));
    CHECK(state.current(state.begin({800, 600}, 1.), {800, 600}, 1., true));
}

TEST_CASE("Resize transitions preserve the completed image aspect inside the new viewport",
          "[ModelPreviewState][ModelPreviewFrame]")
{
    const wxSize source = GENERATE(wxSize(2232, 1647), wxSize(1688, 1647), wxSize(600, 1000));
    const wxSize target = GENERATE(wxSize(1688, 1647), wxSize(2232, 1647), wxSize(1200, 800));
    const auto extents = Slic3r::GUI::preview_frame_extents(source, target);
    REQUIRE(extents);
    CHECK((*extents)[0] > 0.f);
    CHECK((*extents)[0] <= 1.f);
    CHECK((*extents)[1] > 0.f);
    CHECK((*extents)[1] <= 1.f);
    const double image_aspect = double((*extents)[0]) * target.x / (double((*extents)[1]) * target.y);
    CHECK_THAT(image_aspect, Catch::Matchers::WithinAbs(double(source.x) / source.y, 0.000001));
    const auto double_dpi = Slic3r::GUI::preview_frame_extents(source * 2, target * 2);
    REQUIRE(double_dpi);
    CHECK(*double_dpi == *extents);
    CHECK_FALSE(Slic3r::GUI::preview_frame_extents({0, source.y}, target));
    CHECK_FALSE(Slic3r::GUI::preview_frame_extents(source, {target.x, 0}));
}

TEST_CASE("Saved low-color versions retain the immutable original palette source",
          "[ModelPreviewState][ModelPreviewPalette][OriginalAppearance][Regression]")
{
    ScopedTemporaryDir temporary("orca-original-appearance-palette");
    const auto root = temporary.path();
    const auto models = root / "models";
    const auto downloads = root / "downloads";
    boost::filesystem::create_directories(models);
    boost::filesystem::create_directories(downloads);
    const auto original = models / "original.obj";
    const auto saved = models / "saved-four.obj";
    const auto record = downloads / "saved-four.json";
    const auto mesh = original_appearance_fixture_mesh();
    const auto original_colors = original_appearance_fixture_colors(mesh.vertices.size());
    auto saved_colors = original_colors;
    for (size_t index = 4; index < saved_colors.size(); ++index) saved_colors[index] = saved_colors[index - 4];
    std::string error;
    REQUIRE(Slic3r::AI::write_model_artifact(original, mesh, original_colors, error));
    REQUIRE(Slic3r::AI::write_model_artifact(saved, mesh, saved_colors, error));
    write_original_appearance_json(record, {
        {"source_model", "models/original.obj"},
        {"source_sha256", Slic3r::AI::model_artifact_sha256(original)}
    });

    ModelPreview3D::PreparedModel prepared;
    const bool loaded = ModelPreview3D::prepare_model(saved, prepared, error, {}, record);
    INFO(error);
    REQUIRE(loaded);
    REQUIRE(prepared.has_source_appearance);
    REQUIRE(prepared.current_appearance_distinct);
    REQUIRE(prepared.trial_histogram);
    CHECK(prepared.original_appearance.carrier_model == boost::filesystem::canonical(original));
    CHECK(prepared.original_appearance.origin_model == boost::filesystem::canonical(original));
    CHECK(prepared.colors == 8);
    CHECK(prepared.trial_histogram->trial_palette(8, {}, true).size() == 8);
}

TEST_CASE("Original surface sampling survives face reorder winding and repaired triangles",
          "[ModelPreviewState][OriginalAppearance]")
{
    const auto source=boost::filesystem::path(std::string(TEST_DATA_DIR))/"model_artifact"/"textured.glb";
    Slic3r::TriangleMesh mesh;Slic3r::ObjInfo colors;std::string error;
    REQUIRE(Slic3r::AI::load_model_artifact(source,mesh,colors,error));
    const auto hash=Slic3r::AI::model_artifact_sha256(source);
    const auto original=Slic3r::GUI::ModelOriginalSurfaceSampler::load(source,hash,mesh.its,error);
    INFO(error);REQUIRE(original);
    auto repaired=mesh.its;
    std::reverse(repaired.indices.begin(),repaired.indices.end());
    for (auto& face:repaired.indices) std::swap(face[0],face[2]);
    const auto remapped=Slic3r::GUI::ModelOriginalSurfaceSampler::load(source,hash,repaired,error);
    REQUIRE(remapped);
    for (size_t face=0;face<repaired.indices.size();++face) {
        const auto actual=remapped->sample(face,{.2f,.3f,.5f});
        const auto expected=original->sample(repaired.indices.size()-1-face,{.5f,.3f,.2f});
        for (size_t c=0;c<3;++c) CHECK_THAT(actual[c],Catch::Matchers::WithinAbs(expected[c],1e-5));
    }
    const auto face=mesh.its.indices.front();
    const auto midpoint=(mesh.its.vertices[face[0]]+mesh.its.vertices[face[1]])*.5f;
    repaired=mesh.its;const int mid=int(repaired.vertices.size());repaired.vertices.push_back(midpoint);
    repaired.indices.front()={face[0],mid,face[2]};
    const auto changed=Slic3r::GUI::ModelOriginalSurfaceSampler::load(source,hash,repaired,error);
    INFO(error);REQUIRE(changed);
    const auto expected=original->sample(0,{.35f,.15f,.5f});
    const auto actual=changed->sample(0,{.2f,.3f,.5f});
    for(size_t c=0;c<3;++c) CHECK_THAT(actual[c],Catch::Matchers::WithinAbs(expected[c],1e-4));
    CHECK_FALSE(Slic3r::GUI::ModelOriginalSurfaceSampler::load(source,std::string(64,'0'),repaired,error));
    CHECK_FALSE(Slic3r::GUI::ModelOriginalSurfaceSampler::load(source,hash,repaired,error,[]{return true;}));
}

TEST_CASE("AI source counts follow only hash verified model history", "[ModelPreviewState][OriginalAppearance]")
{
    ScopedTemporaryDir temporary("orca-source-color-history");const auto root=temporary.path();
    boost::filesystem::create_directories(root/"models");
    const auto original=root/"models"/"original.obj",saved=root/"models"/"saved.obj";
    const auto mesh=original_appearance_fixture_mesh();std::string error;
    REQUIRE(Slic3r::AI::write_model_artifact(original,mesh,original_appearance_fixture_colors(mesh.vertices.size()),error));
    boost::filesystem::copy_file(original,saved);
    const auto original_record=Slic3r::AI::beauty_metadata_path(original,root);
    // Explicit finishing records use downloads so prepare_model can recover the
    // same library root used to validate relative source-model references.
    const auto record=root/"downloads"/"saved.json";
    const auto hash=Slic3r::AI::model_artifact_sha256(original);
    write_original_appearance_json(original_record,{{"model_sha256",hash},{"palette",{"#000000","#ffffff","#ff0000","#0000ff"}}});
    write_original_appearance_json(record,{{"source_model","models/original.obj"},{"source_sha256",hash}});
    using Slic3r::GUI::original_appearance_detail::ai_source_color_count;
    CHECK(ai_source_color_count(saved,root,record)==4);
    ModelPreview3D::PreparedModel prepared;
    REQUIRE(ModelPreview3D::prepare_model(saved,prepared,error,{},record));
    CHECK(prepared.ai_source_color_count==4);
    write_original_appearance_json(record,{{"source_model","models/original.obj"},{"source_sha256","wrong"}});
    CHECK(ai_source_color_count(saved,root,record)==0);
    write_original_appearance_json(record,{{"source_model","../outside.obj"},{"source_sha256",hash}});
    CHECK(ai_source_color_count(saved,root,record)==0);
}

TEST_CASE("Original texture sampling preserves UV wrapping and linear material factors",
          "[ModelPreviewState][OriginalAppearance]")
{
    const auto path=boost::filesystem::path(std::string(TEST_DATA_DIR))/"model_artifact"/"textured.glb";
    Slic3r::TriangleMesh mesh;Slic3r::ObjInfo colors;Slic3r::AI::ModelArtifactTextureSurface texture;std::string error;
    REQUIRE(Slic3r::AI::load_model_artifact(path,mesh,colors,error,{},&texture));
    REQUIRE_FALSE(texture.images.empty());REQUIRE(texture.faces.size()==mesh.its.indices.size());
    auto& image=texture.images.front();image.width=2;image.height=2;
    image.rgba={255,0,0,255,0,255,0,255,0,0,255,255,255,255,255,255};
    auto& face=texture.faces.front();face.image=0;
    for(auto& corner:face.corners){corner.uv={1.25f,.25f};corner.multiplier={.5f,.5f,.5f};}
    const auto hash=Slic3r::AI::model_artifact_sha256(path);
    for(const auto wrap:std::array<int,3>{10497,33648,33071}) {
        face.wrap_s=wrap;
        const auto sampler=Slic3r::GUI::ModelOriginalSurfaceSampler::load(path,hash,mesh.its,error,{},&mesh,&colors,&texture);
        INFO(error);REQUIRE(sampler);
        const auto sampled=sampler->sample(0,{.2f,.3f,.5f});
        const size_t channel=wrap==10497?0:1;
        for(size_t c=0;c<3;++c) CHECK_THAT(sampled[c],Catch::Matchers::WithinAbs(c==channel?.73535698f:0.f,1e-5));
    }
    namespace SC=Slic3r::AI::SemanticColoring;
    SC::MeshSnapshot snapshot;snapshot.mesh=mesh.its;snapshot.vertex_colors=colors.vertex_colors;
    snapshot.geometry_id=selection::geometry_fingerprint(mesh.its);snapshot.content_id=SC::content_fingerprint(snapshot);
    const auto old_content=snapshot.content_id,old_key=SC::analysis_cache_key(snapshot,"body","face");
    snapshot.sampling_signature="original-surface/v1:"+hash;
    CHECK(SC::content_fingerprint(snapshot)==old_content);
    CHECK(SC::analysis_cache_key(snapshot,"body","face")!=old_key);
}

TEST_CASE("Explicit original appearance references validate identity and topology",
          "[ModelPreviewState][OriginalAppearance]")
{
    ScopedTemporaryDir temporary("orca-original-appearance-explicit");
    const auto root = temporary.path();
    boost::filesystem::create_directories(root / "models");
    const auto carrier = root / "models" / "carrier.obj";
    const auto mesh = original_appearance_fixture_mesh();
    std::string error;
    REQUIRE(Slic3r::AI::write_model_artifact(carrier, mesh,
        original_appearance_fixture_colors(mesh.vertices.size()), error));
    const auto hash = Slic3r::AI::model_artifact_sha256(carrier);
    const auto topology = selection::topology_fingerprint(mesh);
    Slic3r::GUI::OriginalAppearanceReference reference {
        boost::filesystem::canonical(carrier), hash, topology,
        boost::filesystem::canonical(carrier), hash
    };
    Slic3r::GUI::ResolvedOriginalAppearance resolved;
    REQUIRE(Slic3r::GUI::resolve_original_appearance_reference(reference, mesh, root, resolved, error));
    CHECK(resolved.reference.carrier_sha256 == hash);
    CHECK(resolved.carrier_appearance.vertex_colors.size() == mesh.vertices.size());

    auto wrong_topology = mesh;
    std::swap(wrong_topology.indices.front(), wrong_topology.indices.back());
    CHECK_FALSE(Slic3r::GUI::resolve_original_appearance_reference(
        reference, wrong_topology, root, resolved, error));
    CHECK(error.find("拓扑") != std::string::npos);

    auto bad_hash = reference;
    bad_hash.carrier_sha256.assign(64, '0');
    CHECK_FALSE(Slic3r::GUI::resolve_original_appearance_reference(
        bad_hash, mesh, root, resolved, error));
    CHECK(error.find("哈希") != std::string::npos);
}

TEST_CASE("Repaired carriers recover original UV evidence without mixing averaged vertex colors", "[ModelPreviewState][OriginalAppearance]")
{
    ScopedTemporaryDir temporary("orca-repaired-texture-evidence");
    const auto root = temporary.path();
    boost::filesystem::create_directories(root / "models");
    const auto original = root / "models" / "original.glb";
    const auto repaired = root / "models" / "repaired.obj";
    const auto record = root / "downloads" / "repaired.json";
    boost::filesystem::copy_file(boost::filesystem::path(std::string(TEST_DATA_DIR)) /
        "model_artifact" / "textured.glb", original);
    Slic3r::TriangleMesh mesh;
    Slic3r::ObjInfo colors;
    std::string error;
    REQUIRE(Slic3r::AI::load_model_artifact(original, mesh, colors, error));
    REQUIRE(Slic3r::AI::write_model_artifact(repaired, mesh.its,
        std::vector<Slic3r::RGBA>(mesh.its.vertices.size(), {1.f, 0.f, 1.f, 1.f}), error));
    REQUIRE(Slic3r::AI::load_model_artifact(repaired, mesh, colors, error));
    const Slic3r::GUI::OriginalAppearanceReference reference {
        boost::filesystem::canonical(repaired), Slic3r::AI::model_artifact_sha256(repaired),
        selection::topology_fingerprint(mesh.its), boost::filesystem::canonical(original),
        Slic3r::AI::model_artifact_sha256(original)
    };
    write_original_appearance_json(record, {{"original_appearance", reference.encode(root)}});
    ModelPreview3D::PreparedModel prepared;
    const bool loaded = ModelPreview3D::prepare_model(repaired, prepared, error, {}, record);
    INFO(error);
    REQUIRE(loaded);
    REQUIRE(prepared.has_source_appearance);
    REQUIRE_FALSE(prepared.texture_surface.images.empty());
    REQUIRE(prepared.semantic_source);
    REQUIRE(prepared.semantic_source->sample_original);
    REQUIRE(prepared.trial_histogram);
    Slic3r::GUI::PreviewPalette::Histogram expected;
    for (size_t face = 0; face < mesh.its.indices.size(); ++face) {
        const auto& triangle = mesh.its.indices[face];
        const double area = (mesh.its.vertices[triangle[1]] - mesh.its.vertices[triangle[0]]).cross(
            mesh.its.vertices[triangle[2]] - mesh.its.vertices[triangle[0]]).norm();
        const auto sampled = prepared.semantic_source->sample_original(face, {1.f/3, 1.f/3, 1.f/3});
        expected.add(Slic3r::GUI::preview_rgb8(sampled[0], sampled[1], sampled[2]), area * 3);
    }
    CHECK(prepared.trial_histogram->signature() == expected.signature());
}

TEST_CASE("Filament baking retains original evidence across split vertices and reopening",
          "[ModelPreviewState][OriginalAppearance][Regression]")
{
    ScopedTemporaryDir temporary("orca-filament-original-appearance");
    struct DataScope {
        std::string previous = Slic3r::data_dir();
        ~DataScope() { Slic3r::set_data_dir(previous); }
    } scope;
    Slic3r::set_data_dir(temporary.path().string());
    const auto root = temporary.path() / "models";
    const auto models = root / "job";
    const auto downloads = root / "downloads";
    boost::filesystem::create_directories(models);
    boost::filesystem::create_directories(downloads);
    const auto original = models / "original.obj";
    const auto parent = models / "orcaslicer-ai-finish-repaired.obj";
    const auto carrier = models / "orcaslicer-ai-finish-repaired.source-appearance.obj";
    const auto mapped = downloads / "orcaslicer-ai-finish-semantic-mapped.obj";
    const auto mesh = original_appearance_fixture_mesh();
    auto original_mesh = mesh;
    original_mesh.indices.pop_back();
    const auto colors = original_appearance_fixture_colors(mesh.vertices.size());
    std::string error;
    REQUIRE(Slic3r::AI::write_model_artifact(original, original_mesh, colors, error));
    REQUIRE(Slic3r::AI::write_model_artifact(carrier, mesh, colors, error));
    REQUIRE(Slic3r::AI::write_model_artifact(parent, mesh,
        std::vector<Slic3r::RGBA>(mesh.vertices.size(), {1.f, 0.f, 0.f, 1.f}), error));
    const Slic3r::GUI::OriginalAppearanceReference reference {
        boost::filesystem::canonical(carrier), Slic3r::AI::model_artifact_sha256(carrier),
        selection::topology_fingerprint(mesh), boost::filesystem::canonical(original),
        Slic3r::AI::model_artifact_sha256(original)
    };
    write_original_appearance_json(downloads / "orcaslicer-ai-finish-repaired.json", {
        {"source_model", original.lexically_relative(root).generic_string()},
        {"source_sha256", reference.origin_sha256},
        {"original_appearance", reference.encode(root)}
    });
    ModelPreview3D::PreparedModel parent_preview;
    REQUIRE(ModelPreview3D::prepare_model(parent, parent_preview, error));
    REQUIRE(parent_preview.has_source_appearance);
    REQUIRE(parent_preview.original_appearance.carrier_model == reference.carrier_model);
    Slic3r::AI::ModelFinishingOptions options;
    options.smooth_surface = options.repair_mesh = false;
    options.beauty_appearance = true;
    Slic3r::AI::BeautyDocument document;
    document.geometry_id = selection::geometry_fingerprint(mesh);
    document.face_count = mesh.indices.size();
    document.face_patch.assign(document.face_count, 0);
    options.beauty_document = document.encode();
    options.appearance.face_weights.assign(document.face_count, 1.f);
    for (size_t face = 0; face < document.face_count; ++face) {
        options.selected_faces.push_back(face);
        options.appearance.face_target_colors.push_back(face % 2
            ? std::array<float, 3>{1.f, 0.f, 0.f} : std::array<float, 3>{0.f, 1.f, 0.f});
    }
    const auto baked = Slic3r::AI::finish_model_artifact(parent, mapped, options);
    INFO(baked.error);
    REQUIRE(baked.success);
    REQUIRE(baked.vertices > mesh.vertices.size());
    ModelPreview3D::PreparedModel incompatible;
    REQUIRE(ModelPreview3D::prepare_model(mapped, incompatible, error, {}, {}, true, {}, nullptr, reference));
    CHECK_FALSE(incompatible.has_source_appearance);
    Slic3r::GUI::OriginalAppearanceReference mapped_reference;
    const auto mapped_carrier = downloads / "orcaslicer-ai-finish-semantic-mapped.source-appearance.obj";
    REQUIRE(Slic3r::GUI::prepare_original_appearance_for_candidate(
        parent, mapped, baked.source_sha256, parent_preview.original_appearance,
        mapped_carrier, root, mapped_reference, error));
    REQUIRE(mapped_reference.carrier_model != reference.carrier_model);
    CHECK(mapped_reference.origin_model == reference.origin_model);
    ModelPreview3D::PreparedModel candidate;
    REQUIRE(ModelPreview3D::prepare_model(mapped, candidate, error, {}, {}, true, {}, nullptr, mapped_reference));
    REQUIRE(candidate.has_source_appearance);
    write_original_appearance_json(downloads / "orcaslicer-ai-finish-semantic-mapped.json", {
        {"source_model", parent.lexically_relative(root).generic_string()},
        {"source_sha256", baked.source_sha256},
        {"original_appearance", mapped_reference.encode(root)}
    });
    ModelPreview3D::PreparedModel reopened;
    REQUIRE(ModelPreview3D::prepare_model(mapped, reopened, error));
    INFO(reopened.original_appearance_error);
    REQUIRE(reopened.has_source_appearance);
    CHECK(reopened.original_appearance.carrier_model == mapped_reference.carrier_model);
    CHECK(reopened.original_appearance.origin_model == reference.origin_model);
    REQUIRE(reopened.trial_histogram);
    CHECK(reopened.trial_histogram->trial_palette(8, {}, true).size() == 8);
    {
        boost::filesystem::ofstream modified(mapped_carrier, std::ios::app);
        modified << "\n# modified original evidence\n";
        modified.close();
        REQUIRE(modified.good());
    }
    ModelPreview3D::PreparedModel rejected;
    REQUIRE(ModelPreview3D::prepare_model(mapped, rejected, error));
    CHECK_FALSE(rejected.has_source_appearance);
    CHECK(rejected.original_appearance_error.find("哈希") != std::string::npos);
}

TEST_CASE("Original appearance mapping inherits coordinate-only changes and publishes topology carriers",
          "[ModelPreviewState][OriginalAppearance]")
{
    ScopedTemporaryDir temporary("orca-original-appearance-remap");
    const auto root = temporary.path();
    const auto models = root / "models";
    boost::filesystem::create_directories(models);
    indexed_triangle_set source_mesh;
    source_mesh.vertices = {
        Slic3r::Vec3f(0.f, 0.f, 0.f),
        Slic3r::Vec3f(1.f, 0.f, 0.f),
        Slic3r::Vec3f(0.f, 1.f, 0.f)};
    source_mesh.indices.emplace_back(0, 1, 2);
    const std::vector<Slic3r::RGBA> source_colors {
        {1.f, 0.f, 0.f, 1.f}, {0.f, 1.f, 0.f, 1.f}, {0.f, 0.f, 1.f, 1.f}};
    const auto source = models / "source.obj";
    std::string error;
    REQUIRE(Slic3r::AI::write_model_artifact(source, source_mesh, source_colors, error));
    const auto source_hash = Slic3r::AI::model_artifact_sha256(source);
    const auto canonical_source = boost::filesystem::canonical(source);
    const Slic3r::GUI::OriginalAppearanceReference inherited {
        canonical_source, source_hash, selection::topology_fingerprint(source_mesh),
        canonical_source, source_hash
    };

    auto deformed_mesh = source_mesh;
    deformed_mesh.vertices[1].x() = 1.25f;
    const auto deformed = models / "deformed.obj";
    REQUIRE(Slic3r::AI::write_model_artifact(deformed, deformed_mesh, source_colors, error));
    const auto unused_carrier = models / "deformed.source-appearance.obj";
    Slic3r::GUI::OriginalAppearanceReference result;
    REQUIRE(Slic3r::GUI::prepare_original_appearance_for_candidate(
        source, deformed, source_hash, inherited, unused_carrier, root, result, error));
    CHECK(result.carrier_model == canonical_source);
    CHECK(result.topology_sha256 == inherited.topology_sha256);
    CHECK_FALSE(boost::filesystem::exists(unused_carrier));

    auto repaired_mesh = source_mesh;
    repaired_mesh.vertices.emplace_back(1.f / 3.f, 1.f / 3.f, 0.f);
    repaired_mesh.indices.clear();
    repaired_mesh.indices.emplace_back(0, 1, 3);
    repaired_mesh.indices.emplace_back(1, 2, 3);
    repaired_mesh.indices.emplace_back(2, 0, 3);
    const auto repaired = models / "repaired.obj";
    REQUIRE(Slic3r::AI::write_model_artifact(repaired, repaired_mesh,
        std::vector<Slic3r::RGBA>(repaired_mesh.vertices.size(), Slic3r::RGBA {0.f, 0.f, 0.f, 1.f}), error));
    const auto carrier = models / "repaired.source-appearance.obj";
    REQUIRE(Slic3r::GUI::prepare_original_appearance_for_candidate(
        source, repaired, source_hash, inherited, carrier, root, result, error));
    REQUIRE(boost::filesystem::is_regular_file(carrier));
    CHECK(result.carrier_sha256 == Slic3r::AI::model_artifact_sha256(carrier));
    CHECK(result.topology_sha256 == selection::topology_fingerprint(repaired_mesh));
    CHECK(result.origin_model == canonical_source);
    CHECK(result.origin_sha256 == source_hash);

    // A later shared partition still decodes the immutable origin, but must
    // map geometry from the carrier matching the current evidence topology.
    // Passing the origin as the mapping mesh reproduces the production
    // topology-mismatch regression after a previous repair.
    auto repartitioned_mesh = repaired_mesh;
    repartitioned_mesh.vertices.emplace_back(.5f, 0.f, 0.f);
    repartitioned_mesh.indices.front() = {0, 4, 3};
    repartitioned_mesh.indices.emplace_back(4, 1, 3);
    const auto repartitioned = models / "repartitioned.obj";
    REQUIRE(Slic3r::AI::write_model_artifact(repartitioned, repartitioned_mesh,
        std::vector<Slic3r::RGBA>(repartitioned_mesh.vertices.size(),
            Slic3r::RGBA {0.f, 0.f, 0.f, 1.f}), error));
    const auto repartitioned_carrier = models / "repartitioned.source-appearance.obj";
    Slic3r::GUI::OriginalAppearanceReference remapped;
    CHECK_FALSE(Slic3r::GUI::prepare_original_appearance_for_candidate(
        result.origin_model, repartitioned, result.origin_sha256, result,
        repartitioned_carrier, root, remapped, error));
    CHECK(error.find("拓扑") != std::string::npos);
    REQUIRE(Slic3r::GUI::prepare_original_appearance_for_candidate(
        result.carrier_model, repartitioned, result.carrier_sha256, result,
        repartitioned_carrier, root, remapped, error));
    CHECK(remapped.topology_sha256 == selection::topology_fingerprint(repartitioned_mesh));
    CHECK(remapped.origin_model == canonical_source);
    CHECK(remapped.origin_sha256 == source_hash);

    Slic3r::TriangleMesh mapped_mesh;
    Slic3r::ObjInfo mapped_appearance;
    REQUIRE(Slic3r::AI::load_model_artifact(carrier, mapped_mesh, mapped_appearance, error));
    REQUIRE(mapped_appearance.vertex_colors.size() == repaired_mesh.vertices.size());
    CHECK_THAT(mapped_appearance.vertex_colors[3][0], Catch::Matchers::WithinAbs(1.f / 3.f, 0.02f));
    CHECK_THAT(mapped_appearance.vertex_colors[3][1], Catch::Matchers::WithinAbs(1.f / 3.f, 0.02f));
    CHECK_THAT(mapped_appearance.vertex_colors[3][2], Catch::Matchers::WithinAbs(1.f / 3.f, 0.02f));

    const auto rejected_carrier = models / "rejected.source-appearance.obj";
    CHECK_FALSE(Slic3r::GUI::prepare_original_appearance_for_candidate(
        source, repaired, std::string(64, '0'), inherited, rejected_carrier, root, result, error));
    CHECK(error.find("发生变化") != std::string::npos);
    CHECK_FALSE(boost::filesystem::exists(rejected_carrier));
}

TEST_CASE("Original appearance topology mapping follows preview color precedence",
          "[ModelPreviewState][OriginalAppearance]")
{
    indexed_triangle_set source;
    source.vertices = {
        Slic3r::Vec3f(0.f, 0.f, 0.f),
        Slic3r::Vec3f(1.f, 0.f, 0.f),
        Slic3r::Vec3f(0.f, 1.f, 0.f)};
    source.indices.emplace_back(0, 1, 2);
    indexed_triangle_set target = source;
    target.vertices.emplace_back(1.f / 3.f, 1.f / 3.f, 0.f);
    target.indices.clear();
    target.indices.emplace_back(0, 1, 3);
    target.indices.emplace_back(1, 2, 3);
    target.indices.emplace_back(2, 0, 3);
    Slic3r::ObjInfo appearance;
    appearance.vertex_colors = {
        {1.f, 0.f, 0.f, 1.f}, {0.f, 1.f, 0.f, 1.f}, {0.f, 0.f, 1.f, 1.f}};
    appearance.face_colors = {{0.f, 0.f, 0.f, 1.f}};
    std::vector<Slic3r::RGBA> mapped;
    std::string error;
    REQUIRE(Slic3r::GUI::original_appearance_detail::map_appearance_to_vertices(
        source, appearance, target, mapped, error));
    REQUIRE(mapped.size() == target.vertices.size());
    CHECK_THAT(mapped[3][0], Catch::Matchers::WithinAbs(1.f / 3.f, 0.0001f));
    CHECK_THAT(mapped[3][1], Catch::Matchers::WithinAbs(1.f / 3.f, 0.0001f));
    CHECK_THAT(mapped[3][2], Catch::Matchers::WithinAbs(1.f / 3.f, 0.0001f));

    appearance.vertex_colors.clear();
    REQUIRE(Slic3r::GUI::original_appearance_detail::map_appearance_to_vertices(
        source, appearance, target, mapped, error));
    REQUIRE(mapped.size() == target.vertices.size());
    CHECK_THAT(mapped[3][0], Catch::Matchers::WithinAbs(0.f, 0.0001f));
    CHECK_THAT(mapped[3][1], Catch::Matchers::WithinAbs(0.f, 0.0001f));
    CHECK_THAT(mapped[3][2], Catch::Matchers::WithinAbs(0.f, 0.0001f));
}

TEST_CASE("Repaired appearance ignores zero area source faces and keeps original face colors",
          "[ModelPreviewState][OriginalAppearance][Regression]")
{
    indexed_triangle_set source;
    source.vertices = {{0.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}};
    source.indices = {{0, 0, 1}, {0, 1, 2}, {0, 2, 2}};
    indexed_triangle_set target = source;
    target.vertices.emplace_back(0.25f, 0.25f, 0.f);
    target.indices = {{0, 1, 3}, {1, 2, 3}, {2, 0, 3}};
    Slic3r::ObjInfo appearance;
    const bool vertex_colors = GENERATE(false, true);
    appearance.face_colors = {{1.f, 0.f, 0.f, 1.f}, {0.f, 1.f, 0.f, 1.f}, {0.f, 0.f, 1.f, 1.f}};
    if (vertex_colors)
        appearance.vertex_colors = {{1.f, 0.f, 0.f, 1.f}, {0.f, 1.f, 0.f, 1.f}, {0.f, 0.f, 1.f, 1.f}};
    std::vector<Slic3r::RGBA> mapped;
    std::string error;
    REQUIRE(Slic3r::GUI::original_appearance_detail::map_appearance_to_vertices(
        source, appearance, target, mapped, error));
    REQUIRE(mapped.size() == target.vertices.size());
    if (vertex_colors) {
        CHECK(mapped[0] == appearance.vertex_colors[0]);
        CHECK_THAT(mapped[3][0], Catch::Matchers::WithinAbs(0.5f, 0.0001f));
        CHECK_THAT(mapped[3][1], Catch::Matchers::WithinAbs(0.25f, 0.0001f));
        CHECK_THAT(mapped[3][2], Catch::Matchers::WithinAbs(0.25f, 0.0001f));
    } else {
        for (const auto& color : mapped) CHECK(color == appearance.face_colors[1]);
    }
    CHECK_FALSE(Slic3r::GUI::original_appearance_detail::map_appearance_to_vertices(
        source, appearance, target, mapped, error, [] { return true; }));
    CHECK(mapped.empty());
    CHECK_FALSE(error.empty());
}

TEST_CASE("Original appearance rejects a source without any valid surface",
          "[ModelPreviewState][OriginalAppearance][Regression]")
{
    indexed_triangle_set source;
    source.vertices = {{0.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}};
    source.indices = {{0, 0, 1}};
    const bool invalid_corner = GENERATE(false, true);
    if (invalid_corner) source.indices[0][2] = 3;
    indexed_triangle_set target;
    target.vertices = source.vertices;
    target.indices = {{0, 1, 2}};
    Slic3r::ObjInfo appearance;
    appearance.face_colors = {{1.f, 0.f, 0.f, 1.f}};
    std::vector<Slic3r::RGBA> mapped;
    std::string error;
    CHECK_FALSE(Slic3r::GUI::original_appearance_detail::map_appearance_to_vertices(
        source, appearance, target, mapped, error));
    CHECK(mapped.empty());
    CHECK_FALSE(error.empty());
}

TEST_CASE("Whole-face colour exports preserve original corners after saving and reopening",
          "[ModelPreviewState][OriginalAppearance][Regression]")
{
    ScopedTemporaryDir temporary("orca-original-appearance-colour-export");
    const auto root = temporary.path();
    const auto models = root / "models";
    boost::filesystem::create_directories(models);
    const auto mesh = original_appearance_fixture_mesh();
    const auto colors = original_appearance_fixture_colors(mesh.vertices.size());
    const auto original = models / "original.obj";
    const auto source = models / "source.obj";
    const auto candidate = models / "candidate.obj";
    std::string error;
    REQUIRE(Slic3r::AI::write_model_artifact(original, mesh, colors, error));
    REQUIRE(Slic3r::AI::write_model_artifact(source, mesh,
        std::vector<Slic3r::RGBA>(mesh.vertices.size(), {0.f, 0.f, 0.f, 1.f}), error));
    const auto origin_hash = Slic3r::AI::model_artifact_sha256(original);
    const Slic3r::GUI::OriginalAppearanceReference inherited {
        boost::filesystem::canonical(original), origin_hash, selection::topology_fingerprint(mesh),
        boost::filesystem::canonical(original), origin_hash
    };
    Slic3r::AI::ModelFinishingOptions options;
    options.beauty_appearance = true;
    options.smooth_surface = options.repair_mesh = false;
    options.appearance.face_weights.assign(mesh.indices.size(), 1.f);
    options.appearance.face_target_colors.assign(mesh.indices.size(), {.2f, .2f, .2f});
    Slic3r::AI::BeautyDocument document;
    document.geometry_id = selection::geometry_fingerprint(mesh);
    document.face_count = mesh.indices.size();
    document.face_patch.assign(mesh.indices.size(), 0);
    options.beauty_document = document.encode();
    for (size_t face = 0; face < mesh.indices.size(); ++face) options.selected_faces.push_back(face);
    const auto finished = Slic3r::AI::finish_model_artifact(source, candidate, options);
    INFO(finished.error);
    REQUIRE(finished.success);
    Slic3r::TriangleMesh saved;
    Slic3r::ObjInfo saved_colors;
    REQUIRE(Slic3r::AI::load_model_artifact(candidate, saved, saved_colors, error));
    REQUIRE(saved.its.vertices.size() == mesh.indices.size() * 3);
    CHECK(selection::topology_fingerprint(saved.its) != inherited.topology_sha256);
    CHECK(selection::geometry_fingerprint(saved.its) == document.geometry_id);

    const auto source_record = models / "source.json";
    write_original_appearance_json(source_record, {{"original_appearance", inherited.encode(root)}});
    const auto record = root / "downloads" / "candidate.json";
    nlohmann::json metadata {{"source_model", "models/source.obj"},
        {"source_sha256", finished.source_sha256}};
    SECTION("new saves publish and reuse an immutable companion") {
        auto carrier = candidate;
        carrier.replace_extension(".source-appearance.obj");
        Slic3r::GUI::OriginalAppearanceReference mapped;
        REQUIRE(Slic3r::GUI::prepare_original_appearance_for_candidate(source, candidate,
            finished.source_sha256, inherited, carrier, root, mapped, error));
        REQUIRE(mapped.valid());
        CHECK(mapped.topology_sha256 == selection::topology_fingerprint(saved.its));
        CHECK(mapped.origin_sha256 == origin_hash);
        const auto carrier_hash = Slic3r::AI::model_artifact_sha256(carrier);
        REQUIRE(Slic3r::GUI::prepare_original_appearance_for_candidate(source, candidate,
            finished.source_sha256, inherited, carrier, root, mapped, error));
        CHECK(Slic3r::AI::model_artifact_sha256(carrier) == carrier_hash);
        metadata["original_appearance"] = mapped.encode(root);
    }
    SECTION("legacy histories recover the ancestor's explicit original evidence") {
        // The saved parent colours are black. Its explicit carrier, rather than
        // those edited colours, must supply the recovered original palette.
        CHECK_FALSE(metadata.contains("original_appearance"));
    }
    write_original_appearance_json(record, metadata);
    const auto record_hash = Slic3r::AI::model_artifact_sha256(record);
    for (int reopen = 0; reopen < 2; ++reopen) {
        ModelPreview3D::PreparedModel prepared;
        REQUIRE(ModelPreview3D::prepare_model(candidate, prepared, error, {}, record));
        INFO(prepared.original_appearance_error);
        REQUIRE(prepared.has_source_appearance);
        CHECK(prepared.original_appearance.origin_sha256 == origin_hash);
        Slic3r::TriangleMesh evidence;
        Slic3r::ObjInfo evidence_colors;
        REQUIRE(Slic3r::AI::load_model_artifact(prepared.original_appearance.carrier_model,
            evidence, evidence_colors, error));
        REQUIRE(evidence_colors.vertex_colors.size() == saved.its.vertices.size());
        for (size_t face = 0; face < mesh.indices.size(); ++face)
            for (int corner = 0; corner < 3; ++corner)
                for (int channel = 0; channel < 4; ++channel)
                    CHECK_THAT(evidence_colors.vertex_colors[evidence.its.indices[face][corner]][channel],
                        Catch::Matchers::WithinAbs(colors[mesh.indices[face][corner]][channel], 0.00001f));
    }
    CHECK(Slic3r::AI::model_artifact_sha256(record) == record_hash);
    CHECK(Slic3r::AI::model_artifact_sha256(original) == origin_hash);
    CHECK(Slic3r::AI::model_artifact_sha256(source) == finished.source_sha256);
}

TEST_CASE("Original colour recovery rejects changed geometry and preserves conflicting companions",
          "[ModelPreviewState][OriginalAppearance][Regression]")
{
    ScopedTemporaryDir temporary("orca-original-appearance-colour-failure");
    const auto root = temporary.path();
    const auto models = root / "models";
    boost::filesystem::create_directories(models);
    const auto mesh = original_appearance_fixture_mesh();
    const auto colors = original_appearance_fixture_colors(mesh.vertices.size());
    const auto source = models / "source.obj";
    const auto target = models / "target.obj";
    const auto companion = models / "target.source-appearance.obj";
    std::string error;
    REQUIRE(Slic3r::AI::write_model_artifact(source, mesh, colors, error));
    const auto hash = Slic3r::AI::model_artifact_sha256(source);
    const Slic3r::GUI::OriginalAppearanceReference reference {
        boost::filesystem::canonical(source), hash, selection::topology_fingerprint(mesh),
        boost::filesystem::canonical(source), hash};
    write_original_appearance_json(models / "source.json", {{"original_appearance", reference.encode(root)}});
    indexed_triangle_set split;
    std::vector<Slic3r::RGBA> split_colors;
    for (const auto& face : mesh.indices) {
        const int offset = int(split.vertices.size());
        for (int vertex : face) { split.vertices.push_back(mesh.vertices[vertex]); split_colors.push_back(colors[vertex]); }
        split.indices.emplace_back(offset, offset + 1, offset + 2);
    }
    SECTION("changed corner positions cannot borrow original evidence") {
        split.vertices.front().x() += 0.1f;
        REQUIRE(Slic3r::AI::write_model_artifact(target, split, split_colors, error));
        Slic3r::GUI::ResolvedOriginalAppearance resolved;
        CHECK_FALSE(Slic3r::GUI::resolve_original_appearance(target, split,
            {{"source_model", "models/source.obj"}, {"source_sha256", hash}}, root, resolved, error));
        CHECK_FALSE(boost::filesystem::exists(companion));
    }
    SECTION("cancellation leaves no companion") {
        REQUIRE(Slic3r::AI::write_model_artifact(target, split, split_colors, error));
        Slic3r::GUI::OriginalAppearanceReference mapped;
        CHECK_FALSE(Slic3r::GUI::prepare_original_appearance_for_candidate(source, target,
            hash, reference, companion, root, mapped, error, [] { return true; }));
        CHECK_FALSE(boost::filesystem::exists(companion));
    }
    SECTION("different companion bytes remain untouched") {
        REQUIRE(Slic3r::AI::write_model_artifact(target, split, split_colors, error));
        REQUIRE(Slic3r::AI::write_model_artifact(companion, mesh, colors, error));
        const auto before = Slic3r::AI::model_artifact_sha256(companion);
        Slic3r::GUI::OriginalAppearanceReference mapped;
        CHECK_FALSE(Slic3r::GUI::prepare_original_appearance_for_candidate(source, target,
            hash, reference, companion, root, mapped, error));
        CHECK_FALSE(mapped.valid());
        CHECK(Slic3r::AI::model_artifact_sha256(companion) == before);
    }
}

TEST_CASE("Legacy appearance recovery rejects cycles traversal and excessive ancestry",
          "[ModelPreviewState][OriginalAppearance]")
{
    ScopedTemporaryDir temporary("orca-original-appearance-chain");
    const auto root = temporary.path();
    const auto models = root / "models";
    boost::filesystem::create_directories(models);
    const auto mesh = original_appearance_fixture_mesh();
    const auto colors = original_appearance_fixture_colors(mesh.vertices.size());
    std::string error;
    const auto write_model = [&](const std::string& name) {
        const auto path = models / name;
        REQUIRE(Slic3r::AI::write_model_artifact(path, mesh, colors, error));
        return path;
    };
    const auto current = write_model("current.obj");
    const auto parent = write_model("parent.obj");
    const auto current_metadata = nlohmann::json {
        {"source_model", "models/parent.obj"},
        {"source_sha256", Slic3r::AI::model_artifact_sha256(parent)}
    };
    write_original_appearance_json(parent.parent_path() / "parent.json", {
        {"source_model", "models/current.obj"},
        {"source_sha256", Slic3r::AI::model_artifact_sha256(current)}
    });
    Slic3r::GUI::ResolvedOriginalAppearance resolved;
    CHECK_FALSE(Slic3r::GUI::resolve_original_appearance(
        current, mesh, current_metadata, root, resolved, error));
    CHECK(error.find("循环") != std::string::npos);

    const nlohmann::json traversal {
        {"source_model", "../outside.obj"}, {"source_sha256", std::string(64, '0')}
    };
    CHECK_FALSE(Slic3r::GUI::resolve_original_appearance(
        current, mesh, traversal, root, resolved, error));
    CHECK(error.find("路径") != std::string::npos);

    std::vector<boost::filesystem::path> versions;
    versions.reserve(65);
    for (size_t index = 0; index < 65; ++index)
        versions.push_back(write_model("chain-" + std::to_string(index) + ".obj"));
    for (size_t index = 1; index < versions.size(); ++index) {
        write_original_appearance_json(versions[index].parent_path() /
            (versions[index].stem().string() + ".json"), {
                {"source_model", "models/" + versions[index - 1].filename().string()},
                {"source_sha256", Slic3r::AI::model_artifact_sha256(versions[index - 1])}
            });
    }
    const nlohmann::json long_chain {
        {"source_model", "models/" + versions.back().filename().string()},
        {"source_sha256", Slic3r::AI::model_artifact_sha256(versions.back())}
    };
    CHECK_FALSE(Slic3r::GUI::resolve_original_appearance(
        current, mesh, long_chain, root, resolved, error));
    CHECK(error.find("安全上限") != std::string::npos);
}

TEST_CASE("Original appearance rejects a symlink in any path component",
          "[ModelPreviewState][OriginalAppearance]")
{
    ScopedTemporaryDir temporary("orca-original-appearance-symlink");
    const auto root = temporary.path();
    const auto models = root / "models";
    boost::filesystem::create_directories(models);
    const auto carrier = models / "carrier.obj";
    const auto mesh = original_appearance_fixture_mesh();
    std::string error;
    REQUIRE(Slic3r::AI::write_model_artifact(carrier, mesh,
        original_appearance_fixture_colors(mesh.vertices.size()), error));
    boost::system::error_code link_error;
    boost::filesystem::create_directory_symlink(models, root / "linked-models", link_error);
    if (link_error) SKIP("symlinks unavailable on this filesystem: " + link_error.message());
    const nlohmann::json metadata {
        {"source_model", "linked-models/carrier.obj"},
        {"source_sha256", Slic3r::AI::model_artifact_sha256(carrier)}
    };
    Slic3r::GUI::ResolvedOriginalAppearance resolved;
    CHECK_FALSE(Slic3r::GUI::resolve_original_appearance(
        carrier, mesh, metadata, root, resolved, error));
    CHECK(error.find("路径") != std::string::npos);
}

TEST_CASE("Legacy semantic colors do not replace the displayed manual import palette", "[ModelPreviewState][ManualTrialImport][Regression]")
{
    using Controls = Slic3r::GUI::ModelPreviewColorControls;
    Controls::State saved;
    saved.source = 2;
    saved.colors = {{.1f, .2f, .3f}, {.4f, .5f, .6f}};
    saved.semantic_palette = {{.8f, .2f, .3f}, {.8f, .2f, .3f}};
    saved.semantic_optimization = true;
    CHECK(Controls::manual_import_palette(saved) == saved.colors);
    saved.semantic_optimization = false;
    CHECK(Controls::manual_import_palette(saved) == saved.colors);
    saved.semantic_optimization = true;
    saved.colors.resize(8, saved.colors.front());
    CHECK(Controls::manual_import_palette(saved) == saved.colors);
}

TEST_CASE("Restored manual history ignores legacy semantic recoloring", "[ModelPreviewState][ManualTrialImport][Regression]")
{
    using Controls = Slic3r::GUI::ModelPreviewColorControls;
    const std::string fingerprint(64, 'a');
    trial::State original;
    original.source = 2; original.count = 2; original.enabled = false;
    original.colors = {{.7f, .2f, .3f}, {.4f, .5f, .6f}};
    original.mapping_colors = {{.1f, .2f, .3f}, {.4f, .5f, .6f}};
    original.semantic_palette = {{.9f, .2f, .3f}, {.4f, .5f, .6f}};
    original.semantic_mapping_palette = original.mapping_colors;
    Controls::State restored;
    std::string error;
    REQUIRE(trial::decode(trial::encode(original, 20, fingerprint), 20, fingerprint, restored, error));
    REQUIRE(restored.manual_colors.empty());
    CHECK(Controls::manual_import_palette(restored) == original.colors);
    CHECK(restored.mapping_colors == original.mapping_colors);
}

TEST_CASE("Manual trial import colors survive alternate sources and same-workpiece snapshots", "[ModelPreviewState][ManualTrialImport][Regression]")
{
    using Controls = Slic3r::GUI::ModelPreviewColorControls;
    Controls::State saved;
    saved.source = 2; saved.semantic_optimization = false;
    saved.colors = {{.9f, .2f, .3f}, {.9f, .2f, .3f}, {.1f, .5f, .6f}};
    const auto manual = Controls::manual_import_palette(saved);
    saved.manual_colors = manual;
    saved.source = GENERATE(0, 1, 3);
    saved.colors = {{.1f, .2f, .3f}};
    const Controls::State reloaded = saved;
    CHECK(Controls::manual_import_palette(reloaded) == manual);
    CHECK(Controls::manual_import_palette(reloaded).size() == 3);
}

TEST_CASE("Texture upload rejects an uninitialized OpenGL loader without calling it", "[ModelPreviewState]")
{
    // A cold start has a canvas/context before GLAD has loaded its entry points.
    // Keep this independent of other tests that may have initialized OpenGL.
    struct RestoreEntryPoint {
        PFNGLGETINTEGERVPROC saved {glad_glGetIntegerv};
        ~RestoreEntryPoint() { glad_glGetIntegerv = saved; }
    } restore;
    glad_glGetIntegerv = nullptr;
    Slic3r::AI::ModelArtifactTextureSurface surface;
    surface.faces.resize(1);
    indexed_triangle_set mesh;
    mesh.vertices = {{0.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}};
    mesh.indices.emplace_back(0, 1, 2);
    const std::vector<Slic3r::Vec3f> normals(3, Slic3r::Vec3f(0.f, 0.f, 1.f));
    REQUIRE_THROWS_WITH(Slic3r::GUI::ModelPreviewTexture(surface, mesh, normals),
                        "OpenGL texture preview is not initialized.");
}

namespace {
struct SavedPreview {
    ScopedTemporaryDir directory {"orca-preview-state"};
    boost::filesystem::path model = directory.path() / "models" / "saved.glb";
    boost::filesystem::path record = directory.path() / "downloads" / "saved.json";
    boost::filesystem::path adjacent = directory.path() / "models" / "saved.json";
    ModelPreview3D::PreparedModel original;
    selection::SelectionState masks;
    selection::FaceColorOverrides intent {{0, {0.2f, 0.7f, 0.1f}}};
    trial::State colors;

    SavedPreview()
    {
        boost::filesystem::create_directories(model.parent_path());
        boost::filesystem::create_directories(record.parent_path());
        boost::filesystem::copy_file(boost::filesystem::path(std::string(TEST_DATA_DIR)) /
            "model_artifact" / "baseline.glb", model);
        std::string error;
        REQUIRE(ModelPreview3D::prepare_model(model, original, error));
        REQUIRE(original.triangles >= 2);
        masks.selected.assign(original.triangles, 0);
        masks.protected_faces.assign(original.triangles, 0);
        masks.foreground.assign(original.triangles, 0);
        masks.domain.assign(original.triangles, 0);
        masks.selected[0] = masks.foreground[0] = masks.domain[0] = 1;
        masks.protected_faces[1] = 1;
        colors.mapping_colors = {{0.1f, 0.2f, 0.3f}};
        colors.colors = {{0.8f, 0.6f, 0.4f}};
        colors.source = 2;
        colors.count = 1;
        colors.enabled = true;
        colors.locks[0] = true;
    }

    nlohmann::json metadata(const std::string& geometry) const
    {
        return {
            {"local_selection", selection::encode(masks, original.triangles, geometry)},
            {"face_color_intent", selection::encode_colors(intent, original.triangles, geometry)},
            {"color_trial", trial::encode(colors, original.triangles, geometry)}
        };
    }

    void write(const boost::filesystem::path& path, const nlohmann::json& value) const
    {
        boost::filesystem::ofstream output(path);
        REQUIRE(output.good());
        output << value.dump();
        output.close();
        REQUIRE(output.good());
    }

    void check_restored(const ModelPreview3D::PreparedModel& prepared) const
    {
        REQUIRE(prepared.selection.has_value());
        CHECK(prepared.selection->selected == masks.selected);
        CHECK(prepared.selection->protected_faces == masks.protected_faces);
        CHECK(prepared.selection->foreground == masks.foreground);
        CHECK(prepared.selection->domain == masks.domain);
        CHECK(prepared.face_color_overrides == intent);
        REQUIRE(prepared.color_trial.has_value());
        CHECK(prepared.color_trial->mapping_colors == colors.mapping_colors);
        CHECK(prepared.color_trial->colors == colors.colors);
        CHECK(prepared.color_trial->locks == colors.locks);
        CHECK(prepared.color_trial->enabled == colors.enabled);
        CHECK(prepared.color_trial->source == 2);
    }
};
}

TEST_CASE("History preview restores editing state stored outside the model directory", "[ModelPreviewState]")
{
    SavedPreview saved;
    saved.write(saved.record, saved.metadata(saved.original.geometry_id));
    REQUIRE_FALSE(boost::filesystem::exists(saved.adjacent));
    ModelPreview3D::PreparedModel prepared;
    std::string error;
    REQUIRE(ModelPreview3D::prepare_model(saved.model, prepared, error, {}, saved.record));
    saved.check_restored(prepared);
}

TEST_CASE("Repeated preview RGB keeps each vertex alpha and local face overrides", "[ModelPreviewState]")
{
    ScopedTemporaryDir directory {"orca-preview-rgb-alpha"};
    const auto path = directory.path() / "colors.obj";
    {
        boost::filesystem::ofstream output(path);
        output << "v 0 0 0 1 0 0 .25\nv 1 0 0 1 0 0 .5\nv 0 1 0 1 0 0 .75\n"
               << "v 2 0 0 0 1 0 .2\nv 3 0 0 0 1 0 .4\nv 2 1 0 0 1 0 .8\n"
               << "v 4 0 0 1 0 0 .1\nv 5 0 0 1 0 0 .3\nv 4 1 0 1 0 0 .6\n"
               << "f 1 2 3\nf 4 5 6\nf 7 8 9\n";
        REQUIRE(output.good());
    }
    for (bool override_face : {false, true}) {
        ModelPreview3D::PreparedModel prepared;
        std::string error;
        ModelPreview3D::FaceColorOverrides overrides;
        if (override_face) overrides = {{0, {0.f, 0.f, 1.f}}};
        REQUIRE(ModelPreview3D::prepare_model(path, prepared, error, overrides));
        REQUIRE(prepared.geometry.vertices.size() == 9 * 8);
        REQUIRE(prepared.vertex_colors.size() == 9);
        CHECK(prepared.colors == 2);
        bool colors_match = true, alpha_matches = true;
        for (size_t vertex = 0; vertex < 9; ++vertex) {
            const uint32_t expected = vertex >= 3 && vertex < 6 ? 65280u : 16711680u;
            const float expected_rgb = float(expected);
            const float expected_alpha = override_face && vertex < 3 ? -1.f : prepared.vertex_colors[vertex][3];
            // Packed shader attributes must retain their exact bytes, including alpha.
            colors_match &= std::memcmp(&prepared.geometry.vertices[vertex * 8 + 6], &expected_rgb, sizeof(float)) == 0;
            alpha_matches &= std::memcmp(&prepared.geometry.vertices[vertex * 8 + 7], &expected_alpha, sizeof(float)) == 0;
        }
        CHECK(colors_match);
        CHECK(alpha_matches);
        CHECK(prepared.face_color_overrides == overrides);
    }
}

TEST_CASE("The chosen history record takes precedence over adjacent preview state", "[ModelPreviewState]")
{
    SavedPreview saved;
    saved.write(saved.record, saved.metadata(saved.original.geometry_id));
    saved.write(saved.adjacent, nlohmann::json::object());
    ModelPreview3D::PreparedModel prepared;
    std::string error;
    REQUIRE(ModelPreview3D::prepare_model(saved.model, prepared, error, {}, saved.record));
    saved.check_restored(prepared);
}

TEST_CASE("Saved preview state is rejected when geometry differs despite matching face counts", "[ModelPreviewState]")
{
    SavedPreview saved;
    const std::string other_geometry(64, '0');
    REQUIRE(other_geometry != saved.original.geometry_id);
    saved.write(saved.record, saved.metadata(other_geometry));
    ModelPreview3D::PreparedModel prepared;
    std::string error;
    REQUIRE(ModelPreview3D::prepare_model(saved.model, prepared, error, {}, saved.record));
    CHECK_FALSE(prepared.selection.has_value());
    CHECK_FALSE(prepared.color_trial.has_value());
    CHECK(prepared.face_color_overrides.empty());
    CHECK(prepared.triangles == saved.original.triangles);
}

TEST_CASE("Preview state beside a model remains available without an explicit history record", "[ModelPreviewState]")
{
    SavedPreview saved;
    saved.write(saved.adjacent, saved.metadata(saved.original.geometry_id));
    ModelPreview3D::PreparedModel prepared;
    std::string error;
    REQUIRE(ModelPreview3D::prepare_model(saved.model, prepared, error));
    saved.check_restored(prepared);
}

TEST_CASE("A missing chosen history record does not borrow another adjacent snapshot", "[ModelPreviewState]")
{
    SavedPreview saved;
    saved.write(saved.adjacent, saved.metadata(saved.original.geometry_id));
    REQUIRE_FALSE(boost::filesystem::exists(saved.record));
    ModelPreview3D::PreparedModel prepared;
    std::string error;
    REQUIRE(ModelPreview3D::prepare_model(saved.model, prepared, error, {}, saved.record));
    CHECK_FALSE(prepared.selection.has_value());
    CHECK_FALSE(prepared.color_trial.has_value());
    CHECK(prepared.face_color_overrides.empty());
}

TEST_CASE("original color matching ignores saved trial edits and preserves native mesh connectivity", "[ModelPreviewState]")
{
    SavedPreview saved;
    saved.write(saved.adjacent, saved.metadata(saved.original.geometry_id));
    ModelPreview3D::PreparedModel prepared;
    std::string error;
    REQUIRE(ModelPreview3D::prepare_model(saved.model, prepared, error, {}, {}, false));
    CHECK_FALSE(prepared.selection.has_value());
    CHECK_FALSE(prepared.color_trial.has_value());
    CHECK(prepared.face_color_overrides.empty());
    CHECK(prepared.geometry.vertices == saved.original.geometry.vertices);
    CHECK(prepared.mesh.vertices == saved.original.mesh.vertices);
    CHECK(prepared.mesh.indices == saved.original.mesh.indices);
}

TEST_CASE("original preparation retains indexed geometry even without vertex colors", "[ModelPreviewState]")
{
    ScopedTemporaryDir directory {"orca-color-native-mesh"};
    const auto path = directory.path() / "plain.obj";
    {
        boost::filesystem::ofstream file(path);
        file << "v 0 0 0\nv 20 0 0\nv 0 20 0\nv 0 0 20\n"
                "f 1 3 2\nf 1 2 4\nf 1 4 3\nf 2 3 4\n";
    }
    ModelPreview3D::PreparedModel prepared;
    std::string error;
    REQUIRE(ModelPreview3D::prepare_model(path, prepared, error, {}, {}, false));
    CHECK(prepared.mesh.vertices.size() == 4);
    CHECK(prepared.mesh.indices.size() == 4);
    CHECK(prepared.geometry.vertices_count() == 12);
    CHECK(selection::geometry_fingerprint(prepared.mesh) == prepared.geometry_id);
}

TEST_CASE("Canceled preview preparation never reads a model or retains an earlier result", "[ModelPreviewState]")
{
    SavedPreview saved;
    auto prepared=std::move(saved.original);
    prepared.prepare_render_geometry();
    REQUIRE(prepared.render_geometry.has_value());
    std::string error;
    REQUIRE_FALSE(ModelPreview3D::prepare_model(saved.directory.path()/"missing.glb",prepared,error,
        {},{},true,[]{return true;}));
    CHECK(error=="Model preview loading canceled.");
    CHECK_FALSE(prepared.render_geometry.has_value());
    CHECK(prepared.geometry.is_empty());
    CHECK(prepared.mesh.indices.empty());
    CHECK(prepared.path.empty());
    CHECK(prepared.geometry_id.empty());
    CHECK(prepared.triangles==0);
    CHECK_FALSE(prepared.semantic_source);
}

TEST_CASE("Interrupted preview preparation discards partial state and can restore a fresh task", "[ModelPreviewState]")
{
    SavedPreview saved;
    saved.write(saved.record,saved.metadata(saved.original.geometry_id));
    for(const int checkpoint:{2,4,8}) {
        INFO("Cancel checkpoint "<<checkpoint);
        int visits=0;
        ModelPreview3D::PreparedModel prepared;
        std::string error;
        REQUIRE_FALSE(ModelPreview3D::prepare_model(saved.model,prepared,error,{},saved.record,true,
            [&]{return ++visits>=checkpoint;}));
        CHECK(error=="Model preview loading canceled.");
        CHECK(prepared.geometry.is_empty());
        CHECK(prepared.mesh.indices.empty());
        CHECK(prepared.geometry_id.empty());
        CHECK_FALSE(prepared.semantic_source);
        CHECK_FALSE(prepared.trial_histogram);
        CHECK_FALSE(prepared.selection);
        CHECK_FALSE(prepared.color_trial);
        CHECK(prepared.face_color_overrides.empty());
        CHECK(prepared.triangles==0);
        REQUIRE(ModelPreview3D::prepare_model(saved.model,prepared,error,{},saved.record,true,[]{return false;}));
        CHECK(error.empty());
        saved.check_restored(prepared);
        REQUIRE(prepared.semantic_source);
        CHECK(prepared.semantic_source->content_id==saved.original.semantic_source->content_id);
        CHECK(prepared.mesh.indices==saved.original.mesh.indices);
        CHECK(prepared.geometry.indices==saved.original.geometry.indices);
    }
}

TEST_CASE("A worker observes an externally canceled preview before publishing its CPU result", "[ModelPreviewState]")
{
    SavedPreview saved;
    std::atomic<bool> canceled{false};
    std::promise<void> reached,release;
    auto arrived=reached.get_future();auto released=release.get_future();
    ModelPreview3D::PreparedModel prepared;
    std::string error;
    auto worker=std::async(std::launch::async,[&]{
        int visits=0;
        return ModelPreview3D::prepare_model(saved.model,prepared,error,{}, {},true,[&]{
            if(++visits==2){reached.set_value();released.wait();}
            return canceled.load();
        });
    });
    const auto status=arrived.wait_for(std::chrono::seconds(5));
    canceled=true;release.set_value();
    const bool completed=worker.get();
    REQUIRE(status==std::future_status::ready);
    CHECK_FALSE(completed);
    CHECK(error=="Model preview loading canceled.");
    CHECK(prepared.geometry.is_empty());
    CHECK(prepared.triangles==0);
    CHECK_FALSE(prepared.semantic_source);
}

// Opt-in CPU measurement using an existing local model. Measures preparation
// and cancellation checkpoints; it never creates a window or uploads to GL.
TEST_CASE("Historical preview cancellation measures avoided preparation work", "[.ModelPreviewCancelProbe]")
{
    struct RestoreLogging {
        unsigned level {Slic3r::get_logging_level()};
        ~RestoreLogging() { Slic3r::set_logging_level(level); }
    } restore_logging;
    const auto env=[](const char* key){const auto value=boost::nowide::getenv(key);return value?std::string(value):std::string{};};
    if (env("ORCASLICER_ASSIMP_TIMING") == "1" || env("ORCASLICER_MODEL_PREPARE_TIMING") == "1")
        Slic3r::set_logging_level(3);
    const auto source=env("ORCA_PREVIEW_SOURCE"),report=env("ORCA_PREVIEW_REPORT");
    if(source.empty() || report.empty())SKIP("Set a historical model and a fresh CPU report path.");
    REQUIRE_FALSE(boost::filesystem::exists(report));
    const auto source_hash=Slic3r::AI::model_artifact_sha256(source);
    REQUIRE_FALSE(source_hash.empty());
    nlohmann::json samples=nlohmann::json::array();
    std::string geometry_id,content_id;
    uint64_t render_digest=0;
    for(int round=0;round<4;++round)for(int index=0;index<3;++index) {
        const int mode=(index+round)%3;
        ModelPreview3D::PreparedModel prepared;
        std::string error;
        const auto start=std::chrono::steady_clock::now();
        const auto requested=start+std::chrono::milliseconds(mode==1?50:250);
        std::function<bool()> cancellation;
        if(mode!=0)cancellation=[&]{
            return std::chrono::steady_clock::now()>=requested;
        };
        const bool ok=ModelPreview3D::prepare_model(source,prepared,error,{}, {},true,cancellation);
        const auto finished=std::chrono::steady_clock::now();
        uint64_t digest=14695981039346656037ull;
        if(mode==0) {
            REQUIRE(ok);REQUIRE(prepared.semantic_source);
            for(const unsigned char byte:std::string_view(reinterpret_cast<const char*>(prepared.geometry.vertices.data()),prepared.geometry.vertices.size()*sizeof(float)))
                digest=(digest^byte)*1099511628211ull;
            if(round==0){geometry_id=prepared.geometry_id;content_id=prepared.semantic_source->content_id;render_digest=digest;}
            CHECK(prepared.geometry_id==geometry_id);
            CHECK(prepared.semantic_source->content_id==content_id);
            CHECK(digest==render_digest);
        } else {
            REQUIRE_FALSE(ok);CHECK(error=="Model preview loading canceled.");
            CHECK(prepared.geometry.is_empty());CHECK(prepared.triangles==0);CHECK_FALSE(prepared.semantic_source);
        }
        samples.push_back({{"round",round},{"mode",mode==0?"full":mode==1?"cancel_after_50ms":"cancel_after_250ms"},
            {"elapsed_ms",std::chrono::duration<double,std::milli>(finished-start).count()},
            {"return_after_request_ms",mode==0?nlohmann::json{}:nlohmann::json(std::chrono::duration<double,std::milli>(finished-requested).count())}});
    }
    CHECK(Slic3r::AI::model_artifact_sha256(source)==source_hash);
    boost::filesystem::ofstream output(report);
    output<<nlohmann::json{{"source_sha256",source_hash},{"samples",samples},{"geometry_id",geometry_id},
        {"semantic_content_id",content_id},{"render_fnv64",render_digest},
        {"scope","CPU preparation versus cooperative cancellation, same binary; no GUI or GPU"}}.dump(2);
    output.close();REQUIRE(output.good());
}


// Opt-in replay of a frozen user failure through the real CPU preparation and
// partition code. Inputs must be copies; no GUI, provider or project is opened.
TEST_CASE("Frozen workbench partitions report their exact preparation result", "[.PortraitPartitionReplay]")
{
    namespace SC = Slic3r::AI::SemanticColoring;
    namespace PC = Slic3r::GUI::PortraitColorBlocks;
    namespace P = Slic3r::GUI::PreviewPalette;
    const auto env = [](const char* key) {
        const auto* value = boost::nowide::getenv(key);
        return value ? std::string(value) : std::string{};
    };
    const auto model = env("ORCA_PARTITION_REPLAY_MODEL");
    const auto evidence = env("ORCA_PARTITION_REPLAY_ANALYSIS");
    const auto report_path = env("ORCA_PARTITION_REPLAY_REPORT");
    if (model.empty() || evidence.empty() || report_path.empty())
        SKIP("Select frozen model/analysis copies and a fresh report path.");
    REQUIRE_FALSE(boost::filesystem::exists(report_path));
    auto record = boost::filesystem::path(model); record.replace_extension(".json");
    ModelPreview3D::PreparedModel prepared;
    std::string error;
    REQUIRE(ModelPreview3D::prepare_model(model, prepared, error, {}, record));
    INFO(error);
    REQUIRE(prepared.semantic_source);
    REQUIRE(prepared.trial_histogram);
    boost::filesystem::ifstream input(evidence);
    const auto doc = nlohmann::json::parse(input);
    SC::Analysis analysis;
    REQUIRE(SC::decode_analysis(doc, *prepared.semantic_source, doc.at("body_identity"),
        doc.at("face_identity"), analysis, error));
    const size_t count = size_t(prepared.ai_source_color_count);
    const auto basis = P::make_automatic_color_basis(*prepared.trial_histogram, count,
        prepared.geometry_id, true, prepared.automatic_color_units.get());
    REQUIRE(basis.valid());
    Slic3r::PaintedMesh painted;
    for (const auto& vertex : prepared.semantic_source->mesh.vertices)
        painted.vertices.push_back({vertex.x(), vertex.y(), vertex.z()});
    for (const auto& face : prepared.semantic_source->mesh.indices)
        painted.indices.push_back({face[0], face[1], face[2]});
    for (const auto& color : basis.source_palette)
        painted.cluster_colors.push_back({size_t(std::lround(color[0] * 255.f)),
            size_t(std::lround(color[1] * 255.f)), size_t(std::lround(color[2] * 255.f))});
    for (const size_t slot : basis.unit_source_slots)
        painted.face_colors.push_back(painted.cluster_colors.at(slot));
    const auto native_partition =
        Slic3r::GUI::NativePortrait::finalize_native_portrait_partition(std::move(painted));
    REQUIRE(native_partition.geometry_id == prepared.geometry_id);
    PC::Input partition{*prepared.semantic_source, analysis};
    partition.strategy = PC::Strategy::ColorRegionMajorityV2;
    partition.native_partition = &native_partition;
    partition.intermediate_palette = basis.source_palette;
    const auto& suite = Slic3r::GUI::honor_people_suite();
    for (size_t slot = 0; slot < suite.model_slot_count; ++slot) {
        const auto& hex = suite.physical_slots[slot].color;
        partition.targets.push_back({float(std::stoul(hex.substr(1, 2), nullptr, 16)) / 255.f,
            float(std::stoul(hex.substr(3, 2), nullptr, 16)) / 255.f,
            float(std::stoul(hex.substr(5, 2), nullptr, 16)) / 255.f});
    }
    partition.mapping = partition.targets;
    partition.final_color_count = partition.targets.size();
    partition.fixed_role_slots = suite.semantic_role_slot_indices();
    nlohmann::json report{{"source_sha256", prepared.artifact_sha256},
        {"geometry_id", prepared.geometry_id}, {"content_id", prepared.semantic_source->content_id},
        {"person_detected", analysis.person_detected}, {"source_count", count},
        {"source_palette", basis.source_palette}, {"basis_signature", basis.basis_signature},
        {"partition_digest", native_partition.partition_digest},
        {"face_count", prepared.triangles}, {"semantic_subfaces", analysis.subface_labels.size()}};
    const auto started = std::chrono::steady_clock::now();
    try {
        const auto result = PC::build(partition);
        report["status"] = "partition_built";
        report["units"] = result->units.size();
        report["regions"] = result->current().blocks.size();
        report["memory_bytes"] = result->memory_bytes();
        SC::FaceColors faces; SC::SubfaceColors children;
        result->appearance(faces, children);
        const auto geometry = Slic3r::GUI::build_semantic_colored_geometry(*prepared.semantic_source, faces, children);
        report["painted_faces"] = faces.size();
        report["subfaces"] = children.size();
        report["geometry_triangles"] = geometry.indices_count() / 3;
        report["source_slots_preserved"] = result->unit_source_slots == native_partition.face_cluster_ids;
        report["used_model_slots"] = PC::used_display_slots(*result);
        report["semantic_bound_regions"] = PC::reliable_fixed_role_blocks(*result, suite.semantic_role_slot_indices());
    } catch (const std::exception& exception) {
        report["status"] = "partition_rejected";
        report["error"] = exception.what();
    }
    report["elapsed_ms"] = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    boost::filesystem::ofstream output(report_path);
    output << report.dump(2); output.close();
    REQUIRE(output.good());
    CHECK(Slic3r::AI::model_artifact_sha256(model) == prepared.artifact_sha256);
    INFO(report.dump(2));
    REQUIRE(report.at("status") == "partition_built");
    CHECK(report.at("units").get<size_t>() == prepared.triangles);
    CHECK(report.at("subfaces").get<size_t>() == 0);
    CHECK(report.at("geometry_triangles").get<size_t>() == prepared.triangles);
    CHECK(report.at("source_slots_preserved").get<bool>());
    for (size_t slot : report.at("used_model_slots").get<std::vector<size_t>>()) CHECK(slot < 5);
}

TEST_CASE("Frozen native workbench sources retain six effective clusters",
          "[.NativeWorkbenchPartitionReplay][PortraitNativePartition][P0]")
{
    const auto* replay_model = boost::nowide::getenv("ORCA_NATIVE_WORKBENCH_REPLAY_MODEL");
    if (!replay_model || !*replay_model)
        SKIP("Set ORCA_NATIVE_WORKBENCH_REPLAY_MODEL to an immutable local GLB copy.");

    const boost::filesystem::path source(replay_model);
    REQUIRE(boost::filesystem::is_regular_file(source));
    const auto source_sha256 = Slic3r::AI::model_artifact_sha256(source);
    REQUIRE(source_sha256.size() == 64);

    Slic3r::GUI::DecodedWorkbenchModel decoded;
    std::string error;
    REQUIRE(Slic3r::GUI::decode_workbench_model(source, decoded, error));
    INFO(error);
    REQUIRE(decoded.texture);
    REQUIRE(Slic3r::GUI::normalize_decoded_workbench_model_to_millimeters(decoded, error));

    Slic3r::GUI::NativePortrait::NativePortraitPartitionBuildReport report;
    const auto partition = Slic3r::GUI::NativePortrait::build_native_workbench_partition(
        *decoded.texture,
        Slic3r::GUI::NativePortrait::native_mesh_repair_callback(&report.strict_repair),
        {}, &report);
    REQUIRE(report.success);
    REQUIRE(partition.valid());
    CHECK(partition.painted.cluster_colors.size() >=
        Slic3r::GUI::NativePortrait::default_model_color_count);
    CHECK(partition.painted.cluster_colors.size() <=
        Slic3r::GUI::NativePortrait::maximum_color_count);

    const auto automatic = Slic3r::GUI::NativePortrait::reduce_native_workbench_partition(
        partition, Slic3r::GUI::NativePortrait::default_model_color_count);
    REQUIRE(automatic.valid_for(partition));
    CHECK(automatic.requested_color_count ==
        Slic3r::GUI::NativePortrait::default_model_color_count);
    CHECK(automatic.actual_color_count ==
        Slic3r::GUI::NativePortrait::default_model_color_count);
    CHECK(Slic3r::AI::model_artifact_sha256(source) == source_sha256);
}

TEST_CASE("Worker history preparation preserves editing state and complete metric records", "[ModelPreviewState]")
{
    SavedPreview fixture;
    auto expected = fixture.metadata(fixture.original.geometry_id);
    expected["beauty_workbench"] = {{"puzzle_base_file", "original.glb"}, {"large_runs", std::vector<int>(10000, 17)},
                                   {"unknown", {{"text", "历史\n\"quoted\""}, {"enabled", true}}}};
    expected["provider"] = "tripo";
    expected["face_limit"] = 2000000;
    expected["geometry_quality"] = "detailed";
    expected["unknown_extension"] = {{"signed", -17}, {"unsigned", uint64_t(9007199254740993ULL)}};
    fixture.write(fixture.record, expected);
    ModelPreview3D::PreparedModel baseline, candidate;
    std::string baseline_error, candidate_error;
    REQUIRE(ModelPreview3D::prepare_model(fixture.model, baseline, baseline_error, {}, fixture.record));
    Slic3r::GUI::ModelHistoryMetadata snapshot;
    auto worker = std::async(std::launch::async, [&] {
        return ModelPreview3D::prepare_model(fixture.model, candidate, candidate_error, {},
                                            fixture.record, true, {}, &snapshot);
    });
    REQUIRE(worker.get());
    fixture.check_restored(candidate);
    REQUIRE(candidate.geometry.vertices.size() == baseline.geometry.vertices.size());
    CHECK(std::memcmp(candidate.geometry.vertices.data(), baseline.geometry.vertices.data(),
                      baseline.geometry.vertices.size() * sizeof(float)) == 0);
    CHECK(candidate.geometry.indices == baseline.geometry.indices);
    CHECK(candidate.geometry_id == baseline.geometry_id);
    CHECK(candidate.triangles == baseline.triangles);
    CHECK(candidate.colors == baseline.colors);
    nlohmann::json fields;
    std::map<std::string, std::string> beauty;
    REQUIRE(snapshot.take_if_current(fixture.record, fields, beauty));
    CHECK_FALSE(fields.contains("beauty_workbench"));
    CHECK(nlohmann::json::parse(beauty.at("beauty_workbench")) == expected["beauty_workbench"]);
    const auto options = Slic3r::GUI::AIModelGenerationClient::restore_generation_options(fields);
    CHECK(options.face_limit == 2000000);
    fields["load_seconds"] = 1.25;
    expected["load_seconds"] = 1.25;
    const auto output = fixture.directory.path() / "metrics.json";
    REQUIRE(Slic3r::GUI::ModelGenerationPresentation::write_json_with_preencoded_fields(
        output, fields, beauty));
    CHECK(Slic3r::GUI::ModelGenerationPresentation::read_json(output) == expected);
    boost::filesystem::ifstream actual(output);
    const std::string bytes((std::istreambuf_iterator<char>(actual)), {});
    CHECK(bytes == expected.dump());
    CHECK_FALSE(snapshot.take_if_current(fixture.record, fields, beauty));
}

TEST_CASE("History handoff detects same size changes even when timestamps are preserved", "[ModelPreviewState]")
{
    SavedPreview fixture;
    const nlohmann::json before {{"prompt", "old"}, {"beauty_workbench", {{"base", "first"}}}};
    const nlohmann::json after {{"prompt", "new"}, {"beauty_workbench", {{"base", "other"}}}};
    REQUIRE(before.dump().size() == after.dump().size());
    fixture.write(fixture.record, before);
    const auto modified = boost::filesystem::last_write_time(fixture.record);
    Slic3r::GUI::ModelHistoryMetadata snapshot;
    REQUIRE(snapshot.read(fixture.record));
    fixture.write(fixture.record, after);
    boost::filesystem::last_write_time(fixture.record, modified);
    nlohmann::json fields = {{"sentinel", true}};
    std::map<std::string, std::string> beauty {{"sentinel", "unchanged"}};
    CHECK_FALSE(snapshot.take_if_current(fixture.record, fields, beauty));
    CHECK(fields == nlohmann::json{{"sentinel", true}});
    CHECK(beauty == (std::map<std::string, std::string>{{"sentinel", "unchanged"}}));
    CHECK(Slic3r::GUI::ModelGenerationPresentation::read_json(fixture.record) == after);
    CHECK_FALSE(snapshot.take_if_current(fixture.record, fields, beauty));
}

TEST_CASE("History handoff rejects missing and different records without modifying outputs", "[ModelPreviewState]")
{
    SavedPreview fixture;
    fixture.write(fixture.record, fixture.metadata(fixture.original.geometry_id));
    Slic3r::GUI::ModelHistoryMetadata snapshot;
    REQUIRE(snapshot.read(fixture.record));
    nlohmann::json fields = {{"sentinel", true}};
    std::map<std::string, std::string> beauty {{"sentinel", "unchanged"}};
    SECTION("Different path")
    {
        fixture.write(fixture.adjacent, fixture.metadata(fixture.original.geometry_id));
        CHECK_FALSE(snapshot.take_if_current(fixture.adjacent, fields, beauty));
    }
    SECTION("Deleted record")
    {
        REQUIRE(boost::filesystem::remove(fixture.record));
        CHECK_FALSE(snapshot.take_if_current(fixture.record, fields, beauty));
    }
    CHECK(fields == nlohmann::json{{"sentinel", true}});
    CHECK(beauty == (std::map<std::string, std::string>{{"sentinel", "unchanged"}}));
    CHECK_FALSE(snapshot.take_if_current(fixture.record, fields, beauty));
}

TEST_CASE("Legacy history fields and arbitrary beauty values survive the existing ordered writer", "[ModelPreviewState]")
{
    SavedPreview fixture;
    const std::vector<nlohmann::json> documents {
        {{"prompt", "legacy"}, {"unknown", {{"nested", {1, 2, 3}}}}},
        {{"prompt", "legacy"}, {"beauty_workbench", nullptr}},
        {{"prompt", "legacy"}, {"beauty_workbench", "future-format"}}
    };
    for (const auto& document : documents) {
        DYNAMIC_SECTION(document.dump())
        {
            fixture.write(fixture.record, document);
            Slic3r::GUI::ModelHistoryMetadata snapshot;
            REQUIRE(snapshot.read(fixture.record));
            nlohmann::json fields;
            std::map<std::string, std::string> beauty;
            REQUIRE(snapshot.take_if_current(fixture.record, fields, beauty));
            const auto output = fixture.directory.path() / "legacy-output.json";
            const bool saved = beauty.empty()
                ? Slic3r::GUI::ModelGenerationPresentation::write_json(output, fields)
                : Slic3r::GUI::ModelGenerationPresentation::write_json_with_preencoded_fields(
                    output, fields, beauty);
            REQUIRE(saved);
            CHECK(Slic3r::GUI::ModelGenerationPresentation::read_json(output) == document);

}
}
}

TEST_CASE("Initial view shows planar assets instead of collapsing their faces", "[ModelPreviewState]")
{
    // Different planar orientations and units exercise the production initial
    // camera without constructing a window or an OpenGL context.
    for (double scale : {0.001, 1.0, 1000.0}) {
        for (int normal_axis = 0; normal_axis < 3; ++normal_axis) {
            Vec3d first = Vec3d::Zero(), second = Vec3d::Zero();
            first[(normal_axis + 1) % 3] = 90.0 * scale;
            second[(normal_axis + 2) % 3] = 30.0 * scale;
            const auto angles = ModelPreview3D::initial_view_angles(first + second);
            const auto rotation =
                Slic3r::Geometry::rotation_transform(angles.second * Vec3d::UnitX()) *
                Slic3r::Geometry::rotation_transform(angles.first * Vec3d::UnitZ());
            const Vec3d a = rotation * first, b = rotation * second;
            const double projected_area = std::abs(a.x() * b.y() - a.y() * b.x());
            CHECK(projected_area > first.norm() * second.norm() * 0.25);
        }
    }
}

TEST_CASE("Failed and canceled history reads discard the prior snapshot", "[ModelPreviewState]")
{
    SavedPreview fixture;
    const auto expected = fixture.metadata(fixture.original.geometry_id);
    fixture.write(fixture.record, expected);
    Slic3r::GUI::ModelHistoryMetadata snapshot;
    REQUIRE(snapshot.read(fixture.record));
    SECTION("Malformed record")
    {
        boost::filesystem::ofstream output(fixture.adjacent);
        output << "{\"unfinished\":";
        output.close();
        CHECK_FALSE(snapshot.read(fixture.adjacent));
    }
    SECTION("Non-object record")
    {
        fixture.write(fixture.adjacent, nlohmann::json::array({1, 2, 3}));
        CHECK_FALSE(snapshot.read(fixture.adjacent));
    }
    SECTION("Missing record") { CHECK_FALSE(snapshot.read(fixture.adjacent)); }
    SECTION("Oversized record")
    {
        fixture.write(fixture.adjacent, expected);
        boost::filesystem::resize_file(fixture.adjacent, 128ULL * 1024 * 1024 + 1);
        CHECK_FALSE(snapshot.read(fixture.adjacent));
    }
    SECTION("Cancellation after parse")
    {
        unsigned checks = 0;
        CHECK_FALSE(snapshot.read(fixture.record, [&] { return ++checks >= 3; }));
        CHECK(checks >= 3);
    }
    SECTION("Preview cancellation")
    {
        ModelPreview3D::PreparedModel canceled;
        std::string error;
        CHECK_FALSE(ModelPreview3D::prepare_model(fixture.model, canceled, error, {}, fixture.record,
                                                 true, [] { return true; }, &snapshot));
        CHECK(canceled.geometry.is_empty());
        CHECK_FALSE(canceled.semantic_source);
    }
    nlohmann::json fields = {{"sentinel", true}};
    std::map<std::string, std::string> beauty {{"sentinel", "unchanged"}};
    CHECK_FALSE(snapshot.take_if_current(fixture.record, fields, beauty));
    CHECK(fields == nlohmann::json{{"sentinel", true}});
    CHECK(beauty == (std::map<std::string, std::string>{{"sentinel", "unchanged"}}));
    CHECK(Slic3r::GUI::ModelGenerationPresentation::read_json(fixture.record) == expected);
}


TEST_CASE("Frozen historical render data keeps geometry, bounds and saved editing state", "[ModelPreviewState][GLModelGeometry]")
{
    SavedPreview saved;
    saved.write(saved.record, saved.metadata(saved.original.geometry_id));
    ModelPreview3D::PreparedModel prepared;
    std::string error;
    REQUIRE(ModelPreview3D::prepare_model(saved.model, prepared, error, {}, saved.record));
    const auto vertices = prepared.geometry.vertices;
    const auto indices = prepared.geometry.indices;
    const auto id = prepared.geometry_id;
    Slic3r::GUI::GLModel reference;
    auto reference_geometry = prepared.geometry;
    reference.init_from(std::move(reference_geometry));
    SECTION("Successful preparation transfers storage without changing rendered data") {
        prepared.prepare_render_geometry();
        REQUIRE(prepared.render_geometry.has_value());
        CHECK(prepared.render_data().vertices == vertices);
        CHECK(prepared.render_data().indices == indices);
        CHECK(prepared.geometry_id == id);
        saved.check_restored(prepared);
        Slic3r::GUI::GLModel adopted;
        adopted.init_from(std::move(*prepared.render_geometry));
        CHECK(adopted.get_geometry().vertices == vertices);
        CHECK(adopted.get_geometry().indices == indices);
        CHECK(adopted.get_bounding_box().min == reference.get_bounding_box().min);
        CHECK(adopted.get_bounding_box().max == reference.get_bounding_box().max);
    }
    SECTION("Canceled preparation keeps owned data intact and can be retried") {
        REQUIRE_THROWS(prepared.prepare_render_geometry([] { return true; }));
        CHECK_FALSE(prepared.render_geometry.has_value());
        CHECK(prepared.geometry.vertices == vertices);
        CHECK(prepared.geometry.indices == indices);
        prepared.prepare_render_geometry();
        REQUIRE(prepared.render_geometry.has_value());
        CHECK(prepared.render_data().vertices == vertices);
        CHECK(prepared.render_data().indices == indices);
        saved.check_restored(prepared);
    }
}

namespace {
struct SharedPreviewFixture {
    indexed_triangle_set mesh;
    Slic3r::GUI::GLModel::Geometry geometry;
    explicit SharedPreviewFixture(size_t faces = 32)
    {
        using Geometry = Slic3r::GUI::GLModel::Geometry;
        mesh.vertices = {{1.f, 2.f, 3.f}, {4.f, 2.f, 3.f}, {1.f, 5.f, 3.f}};
        geometry.format = {Geometry::EPrimitiveType::Triangles, Geometry::EVertexLayout::P3N3T2};
        for (size_t f = 0; f < faces; ++f) {
            mesh.indices.emplace_back(0, 1, 2);
            const unsigned int base = unsigned(geometry.vertices_count());
            for (int v = 0; v < 3; ++v)
                geometry.add_vertex(mesh.vertices[v], Slic3r::Vec3f(0.f, 0.f, 1.f),
                                    Slic3r::Vec2f(float(0x102030 + v), 1.f));
            geometry.add_triangle(base, base + 1, base + 2);
        }
    }
};
void check_ordered_render_bits(const Slic3r::GUI::GLModel::Geometry& raw,
                               const Slic3r::GUI::GLModel::Geometry& shared)
{
    REQUIRE(shared.indices.size() == raw.indices.size());
    bool same = true;
    for (size_t i = 0; i < raw.indices.size(); ++i) {
        if (shared.indices[i] >= shared.vertices_count() ||
            std::memcmp(raw.vertices.data() + raw.indices[i] * 8,
                        shared.vertices.data() + shared.indices[i] * 8, 8 * sizeof(float)) != 0) {
            same = false; break;
        }
    }
    CHECK(same);
}
}

TEST_CASE("Historical render sharing keeps complete attributes and triangle order", "[ModelPreviewState][GLModelGeometry]")
{
    SharedPreviewFixture f;
    const auto original = f.geometry;
    auto shared = Slic3r::GUI::ModelPreviewGeometry::share_exact_vertices(f.mesh, f.geometry);
    REQUIRE(shared.has_value());
    CHECK(shared->vertices_count() < f.geometry.vertices_count());
    check_ordered_render_bits(f.geometry, *shared);
    CHECK(f.geometry.vertices == original.vertices);
    CHECK(f.geometry.indices == original.indices);
    Slic3r::GUI::GLModel a, b;
    auto raw = f.geometry;
    a.init_from(Slic3r::GUI::GLModel::prepare_geometry(std::move(raw)));
    b.init_from(Slic3r::GUI::GLModel::prepare_geometry(std::move(*shared)));
    CHECK(std::memcmp(a.get_bounding_box().min.data(), b.get_bounding_box().min.data(), 3 * sizeof(double)) == 0);
    CHECK(std::memcmp(a.get_bounding_box().max.data(), b.get_bounding_box().max.data(), 3 * sizeof(double)) == 0);
}

TEST_CASE("Historical render sharing separates hard edges colors alpha and signed attribute zeros", "[ModelPreviewState][GLModelGeometry]")
{
    SharedPreviewFixture f;
    f.geometry.vertices[3 * 8 + 3] = 1.f;       // A hard-edge normal at the same source vertex.
    f.geometry.vertices[6 * 8 + 6] = 0xabcdef;  // A local display-color override.
    f.geometry.vertices[9 * 8 + 7] = .25f;     // Original transparency.
    f.geometry.vertices[12 * 8 + 7] = -1.f;    // Locked color marker.
    f.geometry.vertices[15 * 8 + 3] = -0.f;    // Same numeric normal, different bits.
    auto shared = Slic3r::GUI::ModelPreviewGeometry::share_exact_vertices(f.mesh, f.geometry);
    REQUIRE(shared.has_value());
    check_ordered_render_bits(f.geometry, *shared);
    for (size_t corner : {3, 6, 9, 12, 15}) CHECK(shared->indices[corner] != shared->indices[0]);
}

TEST_CASE("Unsupported render sharing leaves the expanded geometry intact", "[ModelPreviewState][GLModelGeometry]")
{
    using Geometry = Slic3r::GUI::GLModel::Geometry;
    SharedPreviewFixture f;
    SECTION("Nontriangle geometry") { f.geometry.format.type = Geometry::EPrimitiveType::Lines; }
    SECTION("Different layout") { f.geometry.format.vertex_layout = Geometry::EVertexLayout::P3N3; }
    SECTION("Different index storage") { f.geometry.index_type = Geometry::EIndexType::USHORT; }
    SECTION("Truncated attributes") { f.geometry.vertices.pop_back(); }
    SECTION("Different face count") { f.mesh.indices.pop_back(); }
    SECTION("Nonsequential expanded indices") { f.geometry.indices[2] = 0; }
    SECTION("Invalid source vertex") { f.mesh.indices[0][0] = -1; }
    SECTION("Unshared source mesh") { f.mesh.vertices.resize(f.geometry.vertices_count()); }
    SECTION("Nonfinite position") { f.geometry.vertices[0] = std::numeric_limits<float>::quiet_NaN(); }
    SECTION("Signed zero position") { f.geometry.vertices[0] = -0.f; }
    const auto raw = f.geometry;
    CHECK_FALSE(Slic3r::GUI::ModelPreviewGeometry::share_exact_vertices(f.mesh, f.geometry));
    REQUIRE(f.geometry.vertices.size() == raw.vertices.size());
    CHECK(std::memcmp(f.geometry.vertices.data(), raw.vertices.data(), raw.vertices.size() * sizeof(float)) == 0);
    CHECK(f.geometry.indices == raw.indices);
}

TEST_CASE("Render sharing bounds work for a vertex with many distinct attributes", "[ModelPreviewState][GLModelGeometry]")
{
    SharedPreviewFixture f;
    for (size_t face = 0; face < f.mesh.indices.size(); ++face)
        f.geometry.vertices[face * 3 * 8 + 6] = float(face);
    const auto raw = f.geometry;
    CHECK_FALSE(Slic3r::GUI::ModelPreviewGeometry::share_exact_vertices(f.mesh, f.geometry));
    CHECK(f.geometry.vertices == raw.vertices);
    CHECK(f.geometry.indices == raw.indices);
}

TEST_CASE("Canceled render sharing and bounds preparation preserve data for retry", "[ModelPreviewState][GLModelGeometry]")
{
    SharedPreviewFixture f(4096);
    ModelPreview3D::PreparedModel prepared;
    prepared.mesh = f.mesh;
    prepared.geometry = f.geometry;
    unsigned calls = 0;
    SECTION("Cancel before allocation") {
        REQUIRE_THROWS(prepared.prepare_render_geometry([] { return true; }, true));
    }
    SECTION("Cancel during sharing") {
        REQUIRE_THROWS(prepared.prepare_render_geometry([&] { return ++calls == 4; }, true));
        CHECK(calls == 4);
    }
    SECTION("Cancel after sharing during bounds") {
        REQUIRE_THROWS(prepared.prepare_render_geometry([&] { return ++calls == 6; }, true));
        CHECK(calls == 6);
    }
    CHECK_FALSE(prepared.render_geometry);
    CHECK(prepared.geometry.vertices == f.geometry.vertices);
    CHECK(prepared.geometry.indices == f.geometry.indices);
    prepared.prepare_render_geometry({}, true);
    REQUIRE(prepared.render_geometry);
    check_ordered_render_bits(f.geometry, prepared.render_data());
    CHECK(prepared.geometry.is_empty());
}

TEST_CASE("Historical render sharing preserves CPU mesh and saved editing state", "[ModelPreviewState][GLModelGeometry]")
{
    SavedPreview saved;
    saved.write(saved.record, saved.metadata(saved.original.geometry_id));
    ModelPreview3D::PreparedModel prepared;
    std::string error;
    REQUIRE(ModelPreview3D::prepare_model(saved.model, prepared, error, {}, saved.record));
    const auto raw = prepared.geometry;
    const auto mesh = prepared.mesh;
    const auto id = prepared.geometry_id;
    prepared.prepare_render_geometry({}, true);
    check_ordered_render_bits(raw, prepared.render_data());
    CHECK(prepared.mesh.vertices == mesh.vertices);
    CHECK(prepared.mesh.indices == mesh.indices);
    CHECK(prepared.geometry_id == id);
    saved.check_restored(prepared);
}

TEST_CASE("Accepted and draft history fields hand off together without losing future values", "[ModelPreviewState]")
{
    SavedPreview fixture;
    const std::vector<nlohmann::json> documents {
        {{"beauty_puzzle_draft", {{"runs", {1, 2, 3}}, {"future", "历史\n\"draft\""}}}},
        {{"beauty_workbench", {{"runs", {4, 5}}}}, {"beauty_puzzle_draft", {{"unsigned", uint64_t(9007199254740993ULL)}, {"negative_zero", -0.0}}}},
        {{"beauty_workbench", nullptr}, {"beauty_puzzle_draft", "future-format"}}
    };
    for (const auto& document : documents) {
        DYNAMIC_SECTION(document.dump()) {
            auto expected = document;
            expected["unknown"] = {{"preserved", true}};
            fixture.write(fixture.record, expected);
            Slic3r::GUI::ModelHistoryMetadata snapshot;
            REQUIRE(snapshot.read(fixture.record));
            CHECK_FALSE(snapshot.restoration_fields().contains("beauty_workbench"));
            CHECK_FALSE(snapshot.restoration_fields().contains("beauty_puzzle_draft"));
            nlohmann::json fields;
            std::map<std::string, std::string> encoded;
            REQUIRE(snapshot.take_if_current(fixture.record, fields, encoded));
            fields["load_seconds"] = 1.25;
            expected["load_seconds"] = 1.25;
            const auto output = fixture.directory.path() / "draft-metrics.json";
            REQUIRE(Slic3r::GUI::ModelGenerationPresentation::write_json_with_preencoded_fields(output, fields, encoded));
            boost::filesystem::ifstream stream(output, std::ios::binary);
            const std::string actual((std::istreambuf_iterator<char>(stream)), {});
            CHECK(actual == expected.dump());
            CHECK(Slic3r::GUI::ModelGenerationPresentation::read_json(output) == expected);
        }
    }
}

TEST_CASE("Large history freezing preserves canonical bytes and ordinary fields", "[ModelPreviewState]")
{
    SavedPreview fixture;
    nlohmann::json expected {
        {"beauty_workbench", {{"values", std::vector<int>(30000, 17)},
            {"unicode", u8"历史\n\"quoted\""}, {"scalars", {nullptr, true, false, -0.0, 1e-300}},
            {"large_unsigned", uint64_t(18446744073709551615ULL)}}},
        {"beauty_puzzle_draft", {{"future", {{"z", 1}, {"a", nlohmann::json::array()}}}}},
        {"unknown", {{"nested_beauty_workbench", {1, 2, 3}}, {"negative_zero", -0.0}, {"exact", uint64_t(9007199254740993ULL)}}}
    };
    REQUIRE(expected.dump().size() >= 64 * 1024);
    fixture.write(fixture.record, expected);
    Slic3r::GUI::ModelHistoryMetadata snapshot;
    REQUIRE(snapshot.read(fixture.record));
    nlohmann::json fields;
    std::map<std::string, std::string> frozen;
    REQUIRE(snapshot.take_if_current(fixture.record, fields, frozen));
    CHECK(fields.at("unknown").dump() == expected.at("unknown").dump());
    CHECK(frozen.at("beauty_workbench") == expected.at("beauty_workbench").dump());
    CHECK(frozen.at("beauty_puzzle_draft") == expected.at("beauty_puzzle_draft").dump());
    const auto output = fixture.directory.path() / "streaming.json";
    REQUIRE(Slic3r::GUI::ModelGenerationPresentation::write_json_with_preencoded_fields(output, fields, frozen));
    boost::filesystem::ifstream input(output, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(input)), {});
    CHECK(bytes == expected.dump());
}

TEST_CASE("Large frozen history keeps the last duplicate root and nested keys", "[ModelPreviewState]")
{
    SavedPreview fixture;
    const std::string text = std::string(64 * 1024, ' ') +
        "{\"beauty_workbench\":null,\"beauty_workbench\":{\"z\":1,\"a\":2,\"z\":3},"
        "\"beauty_puzzle_draft\":[],\"beauty_puzzle_draft\":\"future\","
        "\"unknown\":{\"b\":1,\"b\":2},\"unknown\":{\"b\":3}}";
    { boost::filesystem::ofstream output(fixture.record, std::ios::binary); output << text; }
    const auto expected = nlohmann::json::parse(text);
    Slic3r::GUI::ModelHistoryMetadata snapshot;
    REQUIRE(snapshot.read(fixture.record));
    nlohmann::json fields;
    std::map<std::string, std::string> frozen;
    REQUIRE(snapshot.take_if_current(fixture.record, fields, frozen));
    CHECK(fields == nlohmann::json{{"unknown", expected.at("unknown")}});
    CHECK(frozen.at("beauty_workbench") == expected.at("beauty_workbench").dump());
    CHECK(frozen.at("beauty_puzzle_draft") == expected.at("beauty_puzzle_draft").dump());
}

TEST_CASE("Malformed large frozen records never publish partial history", "[ModelPreviewState]")
{
    const auto broken = GENERATE(std::string("{\"beauty_workbench\":[1,]}"),
        std::string("{\"beauty_workbench\":{\"number\":1e5000}}"),
        std::string("{\"beauty_workbench\":\"\\ud800\"}"),
        std::string("{\"beauty_workbench\":\"") + char(0xff) + "\"}",
        std::string("{\"beauty_workbench\":[]} []"), std::string("[]"));
    SavedPreview fixture;
    fixture.write(fixture.record, {{"prompt", "prior snapshot"}});
    Slic3r::GUI::ModelHistoryMetadata snapshot;
    REQUIRE(snapshot.read(fixture.record));
    { boost::filesystem::ofstream output(fixture.record, std::ios::binary); output << std::string(64 * 1024, ' ') << broken; }
    CHECK_FALSE(snapshot.read(fixture.record));
    CHECK(snapshot.restoration_fields().is_null());
    nlohmann::json fields {{"sentinel", true}};
    std::map<std::string, std::string> frozen {{"sentinel", "unchanged"}};
    CHECK_FALSE(snapshot.take_if_current(fixture.record, fields, frozen));
    CHECK(fields == nlohmann::json{{"sentinel", true}});
    CHECK(frozen == (std::map<std::string, std::string>{{"sentinel", "unchanged"}}));
}

TEST_CASE("Cancellation inside large frozen history parsing discards the snapshot and permits retry", "[ModelPreviewState]")
{
    SavedPreview fixture;
    const nlohmann::json expected {{"beauty_workbench", {{"values", std::vector<int>(30000, 17)}}}, {"prompt", "keep"}};
    REQUIRE(expected.dump().size() >= 64 * 1024);
    fixture.write(fixture.record, expected);
    Slic3r::GUI::ModelHistoryMetadata snapshot;
    REQUIRE(snapshot.read(fixture.record));
    int checks = 0;
    // Initial read, completed IO and parse entry precede the first SAX checkpoint.
    CHECK_FALSE(snapshot.read(fixture.record, [&] { return ++checks >= 4; }));
    CHECK(checks == 4);
    CHECK(snapshot.restoration_fields().is_null());
    nlohmann::json fields {{"sentinel", true}};
    std::map<std::string, std::string> frozen {{"sentinel", "unchanged"}};
    CHECK_FALSE(snapshot.take_if_current(fixture.record, fields, frozen));
    CHECK(fields == nlohmann::json{{"sentinel", true}});
    CHECK(frozen == (std::map<std::string, std::string>{{"sentinel", "unchanged"}}));
    REQUIRE(snapshot.read(fixture.record));
    REQUIRE(snapshot.take_if_current(fixture.record, fields, frozen));
    CHECK(frozen.at("beauty_workbench") == expected.at("beauty_workbench").dump());
}

TEST_CASE("Large ordinary history values keep DOM types and nested beauty fields", "[ModelPreviewState]")
{
    SavedPreview fixture;
    const nlohmann::json expected {{"unknown", {{"beauty_workbench", {{"z", 1}, {"a", 2}}},
        {"values", std::vector<uint64_t>(30000, 9007199254740993ULL)}, {"float", -0.0}}}};
    REQUIRE(expected.dump().size() >= 64 * 1024);
    fixture.write(fixture.record, expected);
    Slic3r::GUI::ModelHistoryMetadata snapshot;
    REQUIRE(snapshot.read(fixture.record));
    nlohmann::json fields;
    std::map<std::string, std::string> frozen;
    REQUIRE(snapshot.take_if_current(fixture.record, fields, frozen));
    CHECK(frozen.empty());
    CHECK(fields.dump() == expected.dump());
    CHECK(fields.at("unknown").at("values").at(0).is_number_unsigned());
    CHECK(fields.at("unknown").at("float").is_number_float());

}

TEST_CASE("Initial view keeps the portrait facing the camera for volumetric models", "[ModelPreviewState]")
{
    const auto angles = ModelPreview3D::initial_view_angles(Vec3d(52.1, 54.0, 100.0));
    const auto rotation =
        Slic3r::Geometry::rotation_transform(angles.second * Vec3d::UnitX()) *
        Slic3r::Geometry::rotation_transform(angles.first * Vec3d::UnitZ());
    const Vec3d facing = rotation * Vec3d::UnitX();
    CHECK(facing.z() > 0.99);
    CHECK(std::abs(facing.x()) < 1e-8);
    CHECK(std::abs(facing.y()) < 1e-8);
}

TEST_CASE("Thin planar assets remain visible before their thickness reaches zero", "[ModelPreviewState]")
{
    const auto angles = ModelPreview3D::initial_view_angles(Vec3d(90.0, 0.01, 30.0));
    const auto rotation =
        Slic3r::Geometry::rotation_transform(angles.second * Vec3d::UnitX()) *
        Slic3r::Geometry::rotation_transform(angles.first * Vec3d::UnitZ());
    const Vec3d facing = rotation * Vec3d::UnitY();
    CHECK(std::abs(facing.z()) > 0.25);
}

TEST_CASE("Saved color blocks validate normal palette and discard only stale candidates", "[PortraitColorBlocks][ModelPreviewState]")
{
    namespace PC=Slic3r::GUI::PortraitColorBlocks;namespace SC=Slic3r::AI::SemanticColoring;
    SavedPreview fixture;ScopedTemporaryDir cache("portrait-blocks-preview-cache");
    struct DataScope {std::string previous=Slic3r::data_dir();~DataScope(){Slic3r::set_data_dir(previous);}} scope;
    Slic3r::set_data_dir(cache.string());
    REQUIRE(fixture.original.semantic_source);
    const auto& source=*fixture.original.semantic_source;
    SC::Analysis analysis;analysis.geometry_id=source.geometry_id;analysis.content_id=source.content_id;analysis.signature=std::string(64,'c');
    analysis.face_labels.assign(fixture.original.triangles,SC::Label::Unknown);analysis.face_confidence.assign(fixture.original.triangles,.95f);
    PC::Input input{source,analysis};input.mapping=fixture.colors.mapping_colors;input.targets=fixture.colors.colors;
    const auto blocks=PC::build(input);auto doc=fixture.metadata(source.geometry_id);
    doc["portrait_color_blocks"]=PC::save(*blocks,cache.path()/"cache"/"portrait-color-blocks");
    doc["semantic_color_state"]={{"signature",analysis.signature}};
    doc["semantic_result"]={{"schema","orca.semantic-result/v1"},{"geometry_id",source.geometry_id},
        {"face_count",fixture.original.triangles},{"faces",{{0,{.9f,.1f,.1f}}}},{"subfaces",nlohmann::json::array()}};
    fixture.write(fixture.adjacent,doc);std::string error;ModelPreview3D::PreparedModel valid;
    REQUIRE(ModelPreview3D::prepare_model(fixture.model,valid,error));REQUIRE(valid.color_blocks);
    REQUIRE(PC::encode(*valid.color_blocks)==PC::encode(*blocks));
    const int fault=GENERATE(0,1,2);
    if(fault==0)doc["color_trial"]["colors"][0]={.5f,.5f,.5f};
    if(fault==1)doc["semantic_color_state"]["signature"]=std::string(64,'d');
    if(fault==2)boost::filesystem::remove(cache.path()/"cache"/"portrait-color-blocks"/(doc["portrait_color_blocks"]["content_id"].get<std::string>()+".json"));
    fixture.write(fixture.adjacent,doc);ModelPreview3D::PreparedModel stale;
    REQUIRE(ModelPreview3D::prepare_model(fixture.model,stale,error));REQUIRE_FALSE(stale.color_blocks);
    REQUIRE(stale.saved_semantic_faces.empty());REQUIRE(stale.face_color_overrides==fixture.intent);
    REQUIRE(stale.semantic_source->content_id==source.content_id);
}

TEST_CASE("Saved v11 native workbench partitions reopen by identity and preserve failed candidates",
          "[PortraitNativePartition][PortraitColorBlocks][ModelPreviewState]")
{
    namespace NP = Slic3r::GUI::NativePortrait;
    namespace PC = Slic3r::GUI::PortraitColorBlocks;
    namespace SC = Slic3r::AI::SemanticColoring;
    SavedPreview fixture;
    ScopedTemporaryDir cache("workbench-v11-preview-cache");
    struct DataScope {
        std::string previous = Slic3r::data_dir();
        ~DataScope() { Slic3r::set_data_dir(previous); }
    } scope;
    Slic3r::set_data_dir(cache.string());
    REQUIRE(fixture.original.semantic_source);
    const auto& source = *fixture.original.semantic_source;

    Slic3r::PaintedMesh painted;
    for (const auto& vertex : source.mesh.vertices)
        painted.vertices.push_back({vertex.x(), vertex.y(), vertex.z()});
    for (const auto& face : source.mesh.indices)
        painted.indices.push_back({face[0], face[1], face[2]});
    painted.cluster_colors = {{35,30,25},{220,160,120},{235,235,230},{170,45,55}};
    for (size_t face = 0; face < painted.indices.size(); ++face)
        painted.face_colors.push_back(painted.cluster_colors[face % painted.cluster_colors.size()]);
    const auto partition = NP::finalize_native_portrait_partition(std::move(painted));
    REQUIRE(partition.geometry_id == source.geometry_id);

    SC::Analysis analysis;
    analysis.geometry_id = source.geometry_id;
    analysis.content_id = source.content_id;
    analysis.signature = std::string(64, 'c');
    analysis.face_labels.assign(source.mesh.indices.size(), SC::Label::Unknown);
    analysis.face_confidence.assign(source.mesh.indices.size(), .95f);
    const std::vector<SC::Color> palette {{.12f,.10f,.08f},{.86f,.62f,.47f},
        {.94f,.94f,.91f},{.67f,.18f,.22f}};
    PC::Input input {source, analysis};
    input.strategy = PC::Strategy::ColorRegionMajorityV2;
    input.mapping = input.targets = palette;
    input.final_color_count = palette.size();
    input.native_partition = &partition;
    const auto blocks = PC::build(input);
    REQUIRE(blocks->algorithm_version == PC::portrait_postprocess_algorithm_version);

    fixture.colors.mapping_colors = fixture.colors.colors = palette;
    fixture.colors.count = fixture.colors.manual_count = int(palette.size());
    const auto cache_root = cache.path() / "cache" / "portrait-color-blocks";
    const auto native_cache_root = cache.path() / "cache" / "native-workbench-partitions";
    auto doc = fixture.metadata(source.geometry_id);
    const auto valid_reference = PC::save(*blocks, cache_root);
    const auto native_cache_reference = NP::save_native_portrait_partition_cache(
        partition, native_cache_root);
    REQUIRE(blocks->native_partition_identity);
    NP::NativePortraitPartitionIdentity cached_identity;
    std::string cache_error;
    REQUIRE(NP::load_native_portrait_partition_cache(
        native_cache_reference, native_cache_root, cached_identity, cache_error));
    CHECK(NP::encode_native_portrait_partition_identity(cached_identity) ==
          NP::encode_native_portrait_partition_identity(*blocks->native_partition_identity));
    const auto directly_restored = NP::restore_native_workbench_partition(
        fixture.original.mesh, cached_identity, cache_error);
    REQUIRE(directly_restored.valid());
    CHECK(NP::encode_native_portrait_partition_identity(
              NP::native_workbench_partition_identity(directly_restored)) ==
          NP::encode_native_portrait_partition_identity(*blocks->native_partition_identity));
    const auto model_hash = Slic3r::AI::model_artifact_sha256(fixture.model);
    const auto millimeter_bounds = NP::encode_native_workbench_bounds(
        NP::native_workbench_bounds(partition));
    const auto native_reference = nlohmann::json {
        {"schema", "orca.native-workbench-partition-reference/v2"},
        {"algorithm", NP::algorithm_version},
        {"partition_digest", partition.partition_digest}, {"topology_hash", std::string(64, 'd')},
        {"source_path", fixture.model.string()}, {"source_sha256", model_hash},
        {"carrier_path", fixture.model.string()}, {"carrier_sha256", model_hash},
        {"appearance_path", fixture.model.string()}, {"appearance_sha256", model_hash},
        {"cache_path", (native_cache_root /
            (native_cache_reference.at("content_id").get<std::string>() + ".json")).string()},
        {"cache_sha256", native_cache_reference.at("content_id")},
        {"metadata_path", fixture.adjacent.string()}, {"geometry_id", source.geometry_id},
        {"appearance_id", source.content_id}, {"coordinate_units", "mm"},
        {"source_bounds_mm", millimeter_bounds}, {"carrier_bounds_mm", millimeter_bounds},
        {"cache_reference", native_cache_reference}};
    doc["portrait_color_blocks"] = valid_reference;
    doc["native_portrait_partition"] = native_reference;
    const auto encoded_blocks = PC::encode(*blocks);
    doc["region_semantics"] = encoded_blocks.at("region_semantics");
    doc["native_color_reduction"] = encoded_blocks.at("native_color_reduction");
    doc["final_slot_binding"] = encoded_blocks.at("final_slot_binding");
    doc["semantic_color_state"] = {{"signature", analysis.signature}};
    fixture.write(fixture.adjacent, doc);

    std::string error;
    ModelPreview3D::PreparedModel reopened;
    REQUIRE(ModelPreview3D::prepare_model(fixture.model, reopened, error));
    INFO(error);
    CHECK(reopened.artifact_sha256 == model_hash);
    CHECK(reopened.geometry_id == source.geometry_id);
    REQUIRE(reopened.semantic_source);
    CHECK(reopened.semantic_source->content_id == source.content_id);
    NP::NativeMillimeterBounds reopened_bounds;
    REQUIRE(NP::decode_native_workbench_bounds(
        native_reference.at("carrier_bounds_mm"), reopened_bounds));
    CHECK(NP::native_workbench_bounds_compatible(
        reopened_bounds, NP::native_workbench_bounds(reopened.mesh), cache_error));
    REQUIRE(reopened.native_portrait_partition);
    CHECK(NP::encode_native_portrait_partition_identity(
              NP::native_workbench_partition_identity(*reopened.native_portrait_partition)) ==
          NP::encode_native_portrait_partition_identity(*blocks->native_partition_identity));
    REQUIRE(reopened.color_blocks);
    CHECK(reopened.color_blocks->algorithm_version == PC::portrait_postprocess_algorithm_version);
    CHECK(reopened.native_portrait_partition->partition_digest == partition.partition_digest);
    CHECK(reopened.native_portrait_partition->boundary_digest == partition.boundary_digest);
    CHECK(reopened.native_portrait_partition->face_cluster_ids == partition.face_cluster_ids);
    CHECK(reopened.native_portrait_partition->region_members == partition.region_members);

    SECTION("old metadata without an independent v11 partition reference stays inactive") {
        doc.erase("native_portrait_partition");
        doc.erase("semantic_binding");
        fixture.write(fixture.adjacent, doc);
        ModelPreview3D::PreparedModel rejected;
        REQUIRE(ModelPreview3D::prepare_model(fixture.model, rejected, error));
        CHECK_FALSE(rejected.color_blocks);
        CHECK_FALSE(rejected.native_portrait_partition);
    }

    SECTION("a conflicting embedded identity cannot replace the independent partition") {
        auto conflicting_painted = partition.painted;
        const auto original_color = conflicting_painted.cluster_colors.front();
        const std::array<size_t, 3> replacement_color {1, 2, 3};
        REQUIRE(std::find(conflicting_painted.cluster_colors.begin(),
                    conflicting_painted.cluster_colors.end(), replacement_color) ==
                conflicting_painted.cluster_colors.end());
        conflicting_painted.cluster_colors.front() = replacement_color;
        for (auto& face_color : conflicting_painted.face_colors)
            if (face_color == original_color) face_color = replacement_color;
        const auto conflicting_partition =
            NP::finalize_native_portrait_partition(std::move(conflicting_painted));
        REQUIRE(conflicting_partition.geometry_id == partition.geometry_id);
        REQUIRE(conflicting_partition.partition_digest != partition.partition_digest);
        auto conflicting_input = input;
        conflicting_input.native_partition = &conflicting_partition;
        const auto conflicting_blocks = PC::build(conflicting_input);
        REQUIRE(conflicting_blocks->native_partition_identity);
        REQUIRE(conflicting_blocks->native_partition_identity->partition_digest ==
                conflicting_partition.partition_digest);
        doc["portrait_color_blocks"] = PC::save(*conflicting_blocks, cache_root);
        fixture.write(fixture.adjacent, doc);
        ModelPreview3D::PreparedModel rejected;
        REQUIRE(ModelPreview3D::prepare_model(fixture.model, rejected, error));
        CHECK_FALSE(rejected.color_blocks);
        REQUIRE(rejected.native_portrait_partition);
        CHECK(rejected.native_portrait_partition->partition_digest == partition.partition_digest);
        const auto stale = cache_root /
            (doc.at("portrait_color_blocks").at("content_id").get<std::string>() + ".json");
        CHECK(boost::filesystem::is_regular_file(stale));
    }

    SECTION("a damaged independent cache remains on disk but cannot activate") {
        const auto native_cache = native_cache_root /
            (native_cache_reference.at("content_id").get<std::string>() + ".json");
        boost::filesystem::ofstream output(native_cache, std::ios::binary | std::ios::app);
        output << "damage";
        output.close();
        REQUIRE(output.good());
        fixture.write(fixture.adjacent, doc);
        ModelPreview3D::PreparedModel rejected;
        REQUIRE(ModelPreview3D::prepare_model(fixture.model, rejected, error));
        CHECK_FALSE(rejected.color_blocks);
        CHECK_FALSE(rejected.native_portrait_partition);
        CHECK(boost::filesystem::is_regular_file(native_cache));
        CHECK(Slic3r::AI::model_artifact_sha256(native_cache) !=
            native_cache_reference.at("content_id").get<std::string>());
    }

    CHECK(Slic3r::AI::model_artifact_sha256(fixture.model) == model_hash);
    const auto valid_cache = cache_root /
        (valid_reference.at("content_id").get<std::string>() + ".json");
    CHECK(boost::filesystem::is_regular_file(valid_cache));
}
