#include "slic3r/GUI/AI/ModelGeneration/SemanticRegionEvidence.hpp"
#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "slic3r/GUI/AI/ModelGeneration/SecondaryRegionEvidence.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ReadonlyEvidenceRender.hpp"
#include "slic3r/GUI/AI/ModelGeneration/BeautyWorkbenchTransactionController.hpp"
#include "slic3r/GUI/AI/Model/ModelFinishing.hpp"
#include "slic3r/GUI/AI/Model/BeautyDocument.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelSemanticColoring.hpp"
#include "slic3r/AI/ModelGeneration/SemanticColoring/MediaPipeRegionRecognizers.hpp"
#include <catch2/catch_all.hpp>
#include <iterator>

using namespace Slic3r;
using GUI::SemanticRegionEvidence;
using GUI::SecondaryRegionEvidence;
using GUI::SecondaryRegion;
namespace SC = AI::SemanticColoring;
namespace cache = GUI::SemanticRegionEvidenceCache;
namespace secondary_cache = GUI::SecondaryRegionEvidenceCache;
namespace selection = AI::SurfaceSelectionPersistence;

namespace {
struct Fixture {
    boost::filesystem::path directory = boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("orca-region-evidence-%%%%-%%%%-%%%%");
    Fixture() { boost::filesystem::create_directories(directory); }
    ~Fixture() { boost::system::error_code ec; boost::filesystem::remove_all(directory, ec); }
};
void neutral_textured_fixture(const boost::filesystem::path& source) {
    boost::filesystem::ifstream input(boost::filesystem::path(std::string(TEST_DATA_DIR)) /
        "model_artifact" / "baseline.glb", std::ios::binary);
    std::vector<unsigned char> bytes(std::istreambuf_iterator<char>(input), {});
    const auto u32 = [](const unsigned char* p) {
        return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    };
    REQUIRE(bytes.size() >= 28);
    const auto json_size = u32(bytes.data() + 12);
    REQUIRE(28 + json_size <= bytes.size());
    auto doc = nlohmann::json::parse(bytes.begin() + 20, bytes.begin() + 20 + json_size);
    for (auto& material : doc["materials"])
        material["pbrMetallicRoughness"]["baseColorFactor"] = {1, 1, 1, 1};
    for (auto& mesh : doc["meshes"])
        for (auto& primitive : mesh["primitives"]) primitive["attributes"].erase("COLOR_0");
    auto description = doc.dump();
    while (description.size() % 4) description += ' ';
    std::vector<unsigned char> output;
    const auto append = [&](uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) output.push_back(static_cast<unsigned char>(value >> shift));
    };
    append(0x46546c67); append(2);
    append(uint32_t(bytes.size() - json_size + description.size()));
    append(uint32_t(description.size())); append(0x4e4f534a);
    output.insert(output.end(), description.begin(), description.end());
    output.insert(output.end(), bytes.begin() + 20 + json_size, bytes.end());
    boost::filesystem::ofstream stream(source, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(output.data()), output.size());
    REQUIRE(bool(stream));
}
SC::Analysis analysis() {
    SC::Analysis value;
    value.geometry_id = std::string(64, 'a');
    value.content_id = std::string(64, 'b');
    value.signature = "original-analysis-key";
    value.body_identity = "body-v1";
    value.face_identity = "face-v1";
    value.person_detected = true;
    value.face_labels = {SC::Label::Hair, SC::Label::FaceSkin, SC::Label::BodySkin, SC::Label::EyeSclera,
        SC::Label::Iris, SC::Label::Eyebrow, SC::Label::Lips, SC::Label::Clothes, SC::Label::Hair};
    value.face_confidence.assign(value.face_labels.size(), .9f);
    value.face_confidence.back() = .4f;
    return value;
}

TEST_CASE("Legacy cached analysis restores selection labels without running either recognizer", "[SemanticRegionEvidence][BeautyWorkbench]")
{
    class Body final : public SC::IBodyRegionRecognizer {
    public:
        std::string identity() const override { return "region-cache-body-v1"; }
        SC::Prediction predict(const SC::RGBImage&, const SC::Cancel&) override {
            throw std::runtime_error("Cache restoration must never run body prediction");
        }
    };
    class Face final : public SC::IFaceRegionRecognizer {
    public:
        std::string identity() const override { return "region-cache-face-v1"; }
        SC::Prediction predict(const SC::RGBImage&, const SC::Cancel&) override {
            throw std::runtime_error("Cache restoration must never run face prediction");
        }
    };
    Fixture f;
    const auto body = f.directory.filename().string() + ".body";
    const auto face = f.directory.filename().string() + ".face";
    REQUIRE(SC::register_body_recognizer_factory(body, [](const std::filesystem::path&) { return std::make_unique<Body>(); }));
    REQUIRE(SC::register_face_recognizer_factory(face, [](const std::filesystem::path&) { return std::make_unique<Face>(); }));
    { boost::filesystem::ofstream output(f.directory / "providers.json");
      output << nlohmann::json {{"body_provider", body}, {"face_provider", face}}; }
    SC::MeshSnapshot source;
    source.mesh.vertices = {Vec3f(0,0,0), Vec3f(1,0,0), Vec3f(0,1,0)};
    source.mesh.indices = {stl_triangle_vertex_indices(0,1,2)};
    source.vertex_colors.assign(3, RGBA {.2f,.3f,.4f,1.f});
    source.geometry_id = selection::geometry_fingerprint(source.mesh);
    source.content_id = SC::content_fingerprint(source);
    auto value = analysis();
    value.geometry_id = source.geometry_id;
    value.content_id = source.content_id;
    value.body_identity = Body().identity();
    value.face_identity = Face().identity();
    value.signature = SC::analysis_cache_key(source, value.body_identity, value.face_identity);
    value.face_labels = {SC::Label::Hair};
    value.face_confidence = {.9f};
    value.rendered_views = 8;
    value.face_views = 1;
    const auto file = f.directory / (value.signature + ".json");
    { boost::filesystem::ofstream output(file); output << SC::encode_analysis(value); }
    std::string error;
    const auto restored = GUI::load_legacy_semantic_region_evidence(source,
        std::filesystem::path(f.directory.native()), std::filesystem::path(f.directory.native()), "runtime-v1", error);
    INFO(error);
    REQUIRE(restored);
    CHECK(restored->labels == value.face_labels);
    source.vertex_colors[0][0] = .8f;
    source.content_id = SC::content_fingerprint(source);
    CHECK_FALSE(GUI::load_legacy_semantic_region_evidence(source, std::filesystem::path(f.directory.native()),
        std::filesystem::path(f.directory.native()), "runtime-v1", error));
}
selection::SelectionState selected_state(size_t faces) {
    selection::SelectionState state;
    state.selected.assign(faces, 0);
    state.selected.back() = 1;
    state.protected_faces.assign(faces, 0);
    state.foreground = state.selected;
    state.domain = state.selected;
    return state;
}
}

TEST_CASE("Region evidence matches all Beauty regions without palette or recognition state", "[SemanticRegionEvidence][BeautyWorkbench]")
{
    const auto region = GENERATE(std::string("hair"), std::string("skin"), std::string("eyes"),
        std::string("lips"), std::string("clothes"));
    const auto value = analysis();
    const auto evidence = SemanticRegionEvidence::from_analysis(value, "runtime-v1");
    REQUIRE(evidence);
    auto state = selected_state(evidence->labels.size());
    const auto match = evidence->select(region, state);
    CHECK(match.selected == (region == "eyes" ? 3 : region == "skin" ? 2 : 1));
    CHECK(evidence->labels == value.face_labels);
    CHECK(evidence->confidence == value.face_confidence);
    CHECK(evidence->compatible(value.geometry_id, value.face_labels.size(), "runtime-v1"));
}

TEST_CASE("Read-only render creates a fresh evidence run and never overwrites it", "[SemanticRegionEvidence][BeautyWorkbench]")
{
    Fixture fixture;
    SC::MeshSnapshot source;
    source.mesh.vertices = {Vec3f(0,0,0), Vec3f(1,0,0), Vec3f(0,1,0)};
    source.mesh.indices = {stl_triangle_vertex_indices(0,1,2)};
    source.vertex_colors.assign(3, RGBA {.2f,.3f,.4f,1.f});
    source.geometry_id = selection::geometry_fingerprint(source.mesh);
    source.content_id = SC::content_fingerprint(source);
    const auto output = fixture.directory / "run-1";
    GUI::ReadonlyEvidenceRenderResult result;
    std::string error;
    REQUIRE(GUI::write_readonly_evidence_render_package(source, output, std::string(64, 'a'), 64, result, error));
    CHECK(result.view_count == 6);
    CHECK(boost::filesystem::is_regular_file(output / "render-manifest.json"));
    const auto manifest_size = boost::filesystem::file_size(output / "render-manifest.json");
    CHECK_FALSE(GUI::write_readonly_evidence_render_package(source, output, std::string(64, 'a'), 64, result, error));
    CHECK(error == "证据输出目录已存在，未覆盖已有运行");
    CHECK(boost::filesystem::file_size(output / "render-manifest.json") == manifest_size);
}

namespace { selection::SelectionState selected_state(size_t faces); }

TEST_CASE("Post-generation UI state locks history and commits while processing", "[ModelGenerationPresentation][BeautyWorkbench]")
{
    using State = GUI::PostGenerationUiState;
    const auto empty = GUI::derive_post_generation_ui_state(
        State::Mode::Workbench, false, false, false, false, false, false, false, false, false, false);
    CHECK(empty.status == State::Status::Empty);
    CHECK_FALSE(empty.can_edit);
    CHECK(empty.can_switch_version);

    const auto ready = GUI::derive_post_generation_ui_state(
        State::Mode::Workbench, true, true, false, false, false, false, false, false, false, false);
    CHECK(ready.status == State::Status::Ready);
    CHECK(ready.can_edit);
    CHECK(ready.can_switch_version);
    CHECK(ready.can_import);

    const auto processing = GUI::derive_post_generation_ui_state(
        State::Mode::Workbench, true, true, false, true, false, false, false, true, true, true);
    CHECK(processing.status == State::Status::Processing);
    CHECK_FALSE(processing.can_switch_version);
    CHECK_FALSE(processing.can_accept);
    CHECK_FALSE(processing.can_undo);
    CHECK_FALSE(processing.can_redo);

    const auto candidate = GUI::derive_post_generation_ui_state(
        State::Mode::Workbench, true, true, false, false, false, true, false, true, false, false);
    CHECK(candidate.status == State::Status::CandidateReady);
    CHECK_FALSE(candidate.can_switch_version);
    CHECK(candidate.can_accept);
    CHECK(candidate.can_discard);
    CHECK_FALSE(candidate.can_import);
}

TEST_CASE("Post-generation UI state exposes undo/redo and blocks edits during before comparison", "[ModelGenerationPresentation][BeautyWorkbench]")
{
    using State = GUI::PostGenerationUiState;
    const auto editing = GUI::derive_post_generation_ui_state(
        State::Mode::Workbench, true, true, false, false, false, false, false, true, true, false);
    CHECK(editing.status == State::Status::Editing);
    CHECK(editing.can_undo);
    CHECK_FALSE(editing.can_redo);

    const auto before = GUI::derive_post_generation_ui_state(
        State::Mode::Workbench, true, true, false, false, false, true, true, true, true, true);
    CHECK(before.status == State::Status::ComparingBefore);
    CHECK_FALSE(before.can_edit);
    CHECK_FALSE(before.can_accept);
    CHECK_FALSE(before.can_switch_version);
    CHECK_FALSE(before.can_undo);
    CHECK_FALSE(before.can_redo);
}

TEST_CASE("Workbench loading and failure keep saved model permissions consistent", "[ModelGenerationPresentation][BeautyWorkbench]")
{
    using State = GUI::PostGenerationUiState;
    const auto loading = GUI::derive_post_generation_ui_state(
        State::Mode::Workbench, false, true, true, false, false, false, false, false, true, true);
    CHECK(loading.status == State::Status::Loading);
    CHECK_FALSE(loading.can_import);
    CHECK_FALSE(loading.can_undo);
    CHECK_FALSE(loading.can_redo);
    const auto failed_switch = GUI::derive_post_generation_ui_state(
        State::Mode::Workbench, true, true, false, false, false, false, false, false, true, false, true);
    CHECK(failed_switch.status == State::Status::Error);
    CHECK(failed_switch.can_edit);
    CHECK(failed_switch.can_import);
    const auto accepted = GUI::derive_post_generation_ui_state(
        State::Mode::Workbench, true, true, false, false, false, false, false, false, true, false);
    CHECK(accepted.can_switch_version);
    CHECK(accepted.can_undo);
    CHECK_FALSE(accepted.can_redo);
}

TEST_CASE("Secondary evidence selects only accepted faces and preserves the primary cache contract", "[SemanticRegionEvidence][BeautyWorkbench]")
{
    SecondaryRegionEvidence evidence;
    evidence.geometry_id = std::string(64, 'a');
    evidence.source_sha256 = std::string(64, 'b');
    evidence.runtime_identity = "runtime-v1";
    evidence.face_count = 4;
    SecondaryRegion detail;
    detail.detail_id = "teeth";
    detail.parent_region = "lips";
    detail.source_module = "teeth-oral";
    detail.accepted_faces = {0, 1};
    detail.low_confidence_faces = {2};
    detail.view_support = {"front", "left"};
    detail.confidence = .9f;
    detail.status = "PASS";
    detail.evidence_sha256 = std::string(64, 'c');
    evidence.regions = {detail};
    REQUIRE(evidence.valid());
    auto state = selected_state(4);
    state.protected_faces[1] = 1;
    const auto match = evidence.select("teeth", state);
    CHECK(match.selected == 1);
    CHECK(match.protected_count == 1);
    CHECK(match.low_confidence == 0);
    CHECK(match.selection.selected[0] == 1);
    CHECK(match.selection.selected[1] == 0);
    CHECK(match.selection.selected[2] == 0);
    CHECK(evidence.compatible(evidence.geometry_id, evidence.source_sha256, 4, "runtime-v1"));
    CHECK(evidence.detail_ids("lips") == std::vector<std::string>{"teeth"});
}

TEST_CASE("Secondary evidence rejects duplicate or overlapping detail faces", "[SemanticRegionEvidence][BeautyWorkbench]")
{
    SecondaryRegionEvidence evidence;
    evidence.geometry_id = std::string(64, 'a');
    evidence.source_sha256 = std::string(64, 'b');
    evidence.runtime_identity = "runtime-v1";
    evidence.face_count = 2;
    SecondaryRegion first;
    first.detail_id = "nose"; first.parent_region = "skin"; first.source_module = "nose";
    first.accepted_faces = {0}; first.protected_faces = {0}; first.confidence = .9f;
    first.status = "PASS"; first.evidence_sha256 = std::string(64, 'c');
    evidence.regions = {first};
    CHECK_FALSE(evidence.valid());
    first.protected_faces.clear();
    evidence.regions = {first, first};
    CHECK_FALSE(evidence.valid());
}

TEST_CASE("Provider evidence maps read-only detail records without enabling material writes", "[SemanticRegionEvidence][BeautyWorkbench]")
{
    const std::string geometry(64, 'a'), source(64, 'b'), evidence_hash(64, 'c');
    nlohmann::json provider = {
        {"schema", "orca.region-proposal-evidence/v1"},
        {"source", {{"glb_sha256", source}, {"geometry_sha256", geometry}}},
        {"authorization", {{"candidate_slot", nullptr}, {"material_write_authorized", false}, {"material_tree_changed", false}}},
        {"records", {{{"module", "nose"}, {"region", "nose"}, {"person_instance", "p1"},
            {"source_face_id", 0}, {"view_ids", {"front", "left"}}, {"confidence", .9f},
            {"offline_candidate_authorized", false}, {"evidence_sha256", evidence_hash}}}}
    };
    std::string error;
    const auto evidence = SecondaryRegionEvidence::decode(provider, geometry, source, 1, "runtime-v1", error);
    INFO(error);
    REQUIRE(evidence);
    REQUIRE(evidence->regions.size() == 1);
    CHECK(evidence->regions.front().detail_id == "nose");
    CHECK(evidence->regions.front().protected_faces == std::vector<size_t>{0});
    CHECK(evidence->regions.front().accepted_faces.empty());
    CHECK_FALSE(evidence->regions.front().selectable());
    const auto protected_match = evidence->select("nose", selected_state(1));
    CHECK(protected_match.selected == 0);
    CHECK(protected_match.protected_count == 1);
    const auto preview_match = evidence->select("nose", selected_state(1), true);
    CHECK(preview_match.selected == 1);
    CHECK(preview_match.selection.selected[0] == 1);

}

TEST_CASE("Render manifest cannot be imported as secondary semantic evidence", "[SemanticRegionEvidence][BeautyWorkbench]")
{
    const std::string geometry(64, 'a'), source(64, 'b');
    const nlohmann::json render = {
        {"schema", "orca.readonly-render-package/v1"},
        {"geometry_id", geometry}, {"source_sha256", source}, {"face_count", 1},
        {"status", "RENDERED_PENDING_PROVIDER"}
    };
    std::string error;
    CHECK_FALSE(SecondaryRegionEvidence::decode(render, geometry, source, 1, "runtime-v1", error));
    CHECK(error.find("不能直接导入") != std::string::npos);

    auto mismatched = nlohmann::json {
        {"schema", "orca.secondary-region-evidence/v1"},
        {"geometry_id", geometry}, {"source_sha256", std::string(64, 'c')},
        {"runtime_identity", "runtime-v1"}, {"face_count", 1}, {"regions", nlohmann::json::array()}
    };
    CHECK_FALSE(SecondaryRegionEvidence::decode(mismatched, geometry, source, 1, "runtime-v1", error));
    CHECK(error == "Secondary evidence source hash mismatch");
}

TEST_CASE("Secondary preview can include protected and low-confidence faces", "[SemanticRegionEvidence][BeautyWorkbench]")
{
    SecondaryRegionEvidence evidence;
    evidence.geometry_id = std::string(64, 'a');
    evidence.source_sha256 = std::string(64, 'b');
    evidence.runtime_identity = "runtime-v1";
    evidence.face_count = 4;
    SecondaryRegion detail;
    detail.detail_id = "teeth";
    detail.parent_region = "lips";
    detail.source_module = "teeth-oral";
    detail.accepted_faces = {0};
    detail.protected_faces = {1};
    detail.low_confidence_faces = {2};
    detail.confidence = .9f;
    detail.status = "PROTECTED_CONTINUE";
    detail.evidence_sha256 = std::string(64, 'c');
    evidence.regions = {detail};
    REQUIRE(evidence.valid());
    const auto all_preview = evidence.select("teeth", selected_state(4), true);
    CHECK(all_preview.selected == 3);
    CHECK(all_preview.selection.selected[0] == 1);
    CHECK(all_preview.selection.selected[1] == 1);
    CHECK(all_preview.selection.selected[2] == 1);
}

TEST_CASE("Zero region matches preserve every selection mask and protection", "[SemanticRegionEvidence][BeautyWorkbench]")
{
    const auto evidence = SemanticRegionEvidence::from_analysis(analysis(), "runtime-v1");
    auto state = selected_state(evidence->labels.size());
    state.protected_faces[0] = 1;
    const auto match = evidence->select("hair", state);
    CHECK(match.selected == 0);
    CHECK(match.protected_count == 1);
    CHECK(match.low_confidence == 1);
    CHECK(match.selection.selected == state.selected);
    CHECK(match.selection.protected_faces == state.protected_faces);
    CHECK(match.selection.foreground == state.foreground);
    CHECK(match.selection.domain == state.domain);
}

TEST_CASE("Current native labels create only source-bound selectable details", "[SemanticRegionEvidence][BeautyWorkbench]")
{
    auto value = analysis();
    value.signature = std::string(64, 'c');
    const auto primary = SemanticRegionEvidence::from_analysis(value, "runtime-v1");
    REQUIRE(primary);
    std::string error;
    const auto secondary = SecondaryRegionEvidence::from_primary(*primary, std::string(64, 'd'),
        primary->source_content_id, error);
    INFO(error);
    REQUIRE(secondary);
    REQUIRE(secondary->valid());
    CHECK(secondary->compatible(primary->geometry_id, std::string(64, 'd'),
        primary->labels.size(), "runtime-v1"));
    CHECK_FALSE(secondary->compatible(primary->geometry_id, std::string(64, 'd'),
        primary->labels.size(), "runtime-v2"));
    const auto iris = std::find_if(secondary->regions.begin(), secondary->regions.end(),
        [](const SecondaryRegion& region) { return region.detail_id == "iris"; });
    REQUIRE(iris != secondary->regions.end());
    CHECK(iris->accepted_faces == std::vector<size_t> {4});
    CHECK(secondary->select("iris", selected_state(value.face_labels.size())).selected == 1);
    auto no_protection_mask = selected_state(value.face_labels.size());
    no_protection_mask.protected_faces.clear();
    CHECK(secondary->select("iris", no_protection_mask).selected == 1);
    no_protection_mask.protected_faces.assign(value.face_labels.size(), 0);
    no_protection_mask.protected_faces[4] = 1;
    CHECK(secondary->select("iris", no_protection_mask).selected == 0);
    CHECK(secondary->select("iris", no_protection_mask, true).selected == 1);
    const auto hair = std::find_if(secondary->regions.begin(), secondary->regions.end(),
        [](const SecondaryRegion& region) { return region.detail_id == "hair"; });
    REQUIRE(hair != secondary->regions.end());
    CHECK(hair->accepted_faces == std::vector<size_t> {0});
    CHECK(hair->low_confidence_faces == std::vector<size_t> {8});
    CHECK(secondary->detail_ids("lips").empty());
    CHECK_FALSE(SecondaryRegionEvidence::from_primary(*primary, std::string(64, 'd'),
        std::string(64, 'e'), error));
    CHECK_FALSE(error.empty());
    const auto referenced = SecondaryRegionEvidence::from_primary(*primary, std::string(64, 'e'),
        std::string(64, 'f'), error, true);
    REQUIRE(referenced);
    CHECK(referenced->source_sha256 == std::string(64, 'e'));
    CHECK(referenced->regions.size() == secondary->regions.size());
}

TEST_CASE("Secondary details follow an appearance-only candidate with verified identity", "[SemanticRegionEvidence][BeautyWorkbench]")
{
    Fixture f;
    auto value = analysis();
    value.signature = std::string(64, 'c');
    const auto primary = SemanticRegionEvidence::from_analysis(value, "runtime-v1");
    REQUIRE(primary);
    std::string error;
    const std::string original_hash(64, 'd'), candidate_hash(64, 'e');
    const auto original = SecondaryRegionEvidence::from_primary(*primary, original_hash,
        primary->source_content_id, error);
    REQUIRE(original);
    const auto candidate = SecondaryRegionEvidence::for_derived_model(*original, original_hash,
        primary->geometry_id, primary->labels.size(), "runtime-v1", candidate_hash, error);
    INFO(error);
    REQUIRE(candidate);
    CHECK(candidate->source_sha256 == candidate_hash);
    CHECK(original->source_sha256 == original_hash);
    CHECK(candidate->regions.front().accepted_faces == original->regions.front().accepted_faces);
    const auto reference = secondary_cache::save(*candidate, f.directory, error);
    REQUIRE_FALSE(reference.empty());
    CHECK(secondary_cache::load(reference, f.directory, primary->geometry_id,
        candidate_hash, primary->labels.size(), "runtime-v1", error));
    CHECK_FALSE(secondary_cache::load(reference, f.directory, primary->geometry_id,
        original_hash, primary->labels.size(), "runtime-v1", error));
    CHECK_FALSE(SecondaryRegionEvidence::for_derived_model(*original, candidate_hash,
        primary->geometry_id, primary->labels.size(), "runtime-v1", candidate_hash, error));
    CHECK_FALSE(SecondaryRegionEvidence::for_derived_model(*original, original_hash,
        std::string(64, 'f'), primary->labels.size(), "runtime-v1", candidate_hash, error));
    CHECK_FALSE(SecondaryRegionEvidence::for_derived_model(*original, original_hash,
        primary->geometry_id, primary->labels.size() + 1, "runtime-v1", candidate_hash, error));
    CHECK_FALSE(SecondaryRegionEvidence::for_derived_model(*original, original_hash,
        primary->geometry_id, primary->labels.size(), "runtime-v2", candidate_hash, error));
    CHECK_FALSE(SecondaryRegionEvidence::for_derived_model(*original, original_hash,
        primary->geometry_id, primary->labels.size(), "runtime-v1", "../unsafe", error));
}

TEST_CASE("Region evidence survives persistence and rejects incompatible references", "[SemanticRegionEvidence][BeautyWorkbench]")
{
    Fixture f;
    auto evidence = SemanticRegionEvidence::from_analysis(analysis(), "runtime-v1");
    std::string error;
    auto reference = cache::save(*evidence, f.directory, error);
    REQUIRE_FALSE(reference.empty());
    REQUIRE(error.empty());
    auto restored = cache::load(reference, f.directory, evidence->geometry_id, evidence->labels.size(), "runtime-v1", error);
    REQUIRE(restored);
    CHECK(restored->labels == evidence->labels);
    CHECK(restored->confidence == evidence->confidence);
    CHECK(restored->source_content_id == evidence->source_content_id);
    CHECK_FALSE(cache::load(reference, f.directory, std::string(64, 'c'), evidence->labels.size(), "runtime-v1", error));
    CHECK_FALSE(cache::load(reference, f.directory, evidence->geometry_id, evidence->labels.size() + 1, "runtime-v1", error));
    CHECK_FALSE(cache::load(reference, f.directory, evidence->geometry_id, evidence->labels.size(), "runtime-v2", error));
    reference["sha256"] = "../outside";
    CHECK_FALSE(cache::load(reference, f.directory, evidence->geometry_id, evidence->labels.size(), "runtime-v1", error));
}

TEST_CASE("Missing and tampered region cache files fail without changing existing evidence", "[SemanticRegionEvidence][BeautyWorkbench]")
{
    Fixture f;
    const auto evidence = SemanticRegionEvidence::from_analysis(analysis(), "runtime-v1");
    std::string error;
    auto reference = cache::save(*evidence, f.directory, error);
    const auto file = f.directory / (reference.at("sha256").get<std::string>() + ".json");
    { boost::filesystem::ofstream output(file); output << "{}"; }
    CHECK_FALSE(cache::load(reference, f.directory, evidence->geometry_id, evidence->labels.size(), "runtime-v1", error));
    // Saving again repairs the same content-addressed entry.
    REQUIRE_FALSE(cache::save(*evidence, f.directory, error).empty());
    CHECK(cache::load(reference, f.directory, evidence->geometry_id, evidence->labels.size(), "runtime-v1", error));
    boost::filesystem::remove(file);
    CHECK_FALSE(cache::load(reference, f.directory, evidence->geometry_id, evidence->labels.size(), "runtime-v1", error));
    CHECK(evidence->valid());
}

TEST_CASE("Region decoding rejects malformed labels confidences lengths and versions", "[SemanticRegionEvidence][BeautyWorkbench]")
{
    const int mutation = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
    const auto evidence = SemanticRegionEvidence::from_analysis(analysis(), "runtime-v1");
    auto doc = evidence->encode();
    if (mutation == 0) doc["labels"][0] = 256;
    if (mutation == 1) doc["labels"][0] = -1;
    if (mutation == 2) doc["confidence"][0] = -0.1;
    if (mutation == 3) doc["confidence"][0] = 1.1;
    if (mutation == 4) doc["labels"].erase(doc["labels"].begin());
    if (mutation == 5) doc["pipeline"] = "old-pipeline";
    if (mutation == 6) doc["schema"] = "unknown-schema";
    if (mutation == 7) doc["confidence"][0] = nullptr;
    std::string error;
    CHECK_FALSE(SemanticRegionEvidence::decode(doc, evidence->geometry_id, evidence->labels.size(), "runtime-v1", error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("Unreliable failed or cancelled analysis cannot create region evidence", "[SemanticRegionEvidence][BeautyWorkbench]")
{
    auto value = analysis();
    value.canceled = true;
    CHECK_FALSE(SemanticRegionEvidence::from_analysis(value, "runtime-v1"));
    value.canceled = false;
    value.error = "worker failed";
    CHECK_FALSE(SemanticRegionEvidence::from_analysis(value, "runtime-v1"));
    value.error.clear();
    value.face_confidence.assign(value.face_labels.size(), .1f);
    CHECK_FALSE(SemanticRegionEvidence::from_analysis(value, "runtime-v1"));
    value.face_confidence.assign(value.face_labels.size(), .9f);
    CHECK_FALSE(SemanticRegionEvidence::from_analysis(value, ""));
}

TEST_CASE("Beauty history restores immutable region evidence without recognition", "[SemanticRegionEvidence][BeautyWorkbench]")
{
    GUI::BeautyWorkbenchTransactionController controller;
    const auto before = SemanticRegionEvidence::from_analysis(analysis(), "runtime-v1");
    std::shared_ptr<const SemanticRegionEvidence> active = before;
    active.reset(); // Geometry edit invalidates the evidence.
    controller.record({GUI::BeautyWorkbenchTransactionController::OperationKind::SurfaceSoften, "geometry edit",
        [&] { active = before; }, [&] { active.reset(); }});
    REQUIRE(controller.undo());
    REQUIRE(active);
    CHECK(active->labels == before->labels);
    REQUIRE(controller.redo());
    CHECK_FALSE(active);
}

TEST_CASE("Ordered geometry rejects moved or reordered faces but permits color seam duplication", "[SemanticRegionEvidence][BeautyWorkbench]")
{
    indexed_triangle_set mesh;
    mesh.vertices = {Vec3f(0,0,0), Vec3f(1,0,0), Vec3f(0,1,0), Vec3f(1,1,0)};
    mesh.indices = {stl_triangle_vertex_indices(0,1,2), stl_triangle_vertex_indices(1,3,2)};
    auto value = analysis();
    value.geometry_id = selection::geometry_fingerprint(mesh);
    value.face_labels.resize(2);
    value.face_confidence.resize(2);
    const auto evidence = SemanticRegionEvidence::from_analysis(value, "runtime-v1");
    auto duplicate = mesh;
    duplicate.vertices.push_back(mesh.vertices[1]);
    duplicate.indices[1][0] = 4;
    CHECK(evidence->compatible(selection::geometry_fingerprint(duplicate), 2, "runtime-v1"));
    std::swap(duplicate.indices[0], duplicate.indices[1]);
    CHECK_FALSE(evidence->compatible(selection::geometry_fingerprint(duplicate), 2, "runtime-v1"));
    mesh.vertices[0].z() = .1f;
    CHECK_FALSE(evidence->compatible(selection::geometry_fingerprint(mesh), 2, "runtime-v1"));
}

TEST_CASE("Real GLB appearance candidates retain ordered geometry for region selection", "[SemanticRegionEvidence][BeautyWorkbench]")
{
    Fixture f;
    const auto source = f.directory / "source.glb", candidate = f.directory / "candidate.glb";
    TriangleMesh mesh;
    ObjInfo info;
    std::string error;
    // Keep the real embedded texture/UVs while normalizing fixture multipliers
    // required by the existing absolute texture-color edit contract.
    neutral_textured_fixture(source);
    REQUIRE(AI::load_model_artifact(source, mesh, info, error));
    auto value = analysis();
    value.geometry_id = selection::geometry_fingerprint(mesh.its);
    value.face_labels.assign(mesh.its.indices.size(), SC::Label::Hair);
    value.face_confidence.assign(mesh.its.indices.size(), .9f);
    const auto evidence = SemanticRegionEvidence::from_analysis(value, "runtime-v1");
    AI::ModelFinishingOptions options;
    options.smooth_surface = options.repair_mesh = false;
    options.beauty_appearance = true;
    options.appearance.face_weights.assign(value.face_labels.size(), 1.f);
    options.appearance.face_target_colors.assign(value.face_labels.size(), {.2f,.3f,.4f});
    AI::BeautyDocument document;
    document.geometry_id = value.geometry_id;
    document.face_count = value.face_labels.size();
    document.face_patch.assign(document.face_count, 0);
    options.beauty_document = document.encode();
    for (size_t face = 0; face < value.face_labels.size(); ++face) options.selected_faces.push_back(face);
    const auto result = AI::finish_model_artifact(source, candidate, options);
    INFO(result.error);
    REQUIRE(result.success);
    REQUIRE(AI::load_model_artifact(candidate, mesh, info, error));
    CHECK(evidence->compatible(selection::geometry_fingerprint(mesh.its), mesh.its.indices.size(), "runtime-v1"));
    auto state = selected_state(mesh.its.indices.size());
    CHECK(evidence->select("hair", state).selected == mesh.its.indices.size());
}
