#include "slic3r/GUI/AI/ModelGeneration/SemanticRegionEvidence.hpp"
#include "slic3r/GUI/AI/ModelGeneration/BeautyWorkbenchTransactionController.hpp"
#include "slic3r/GUI/AI/Model/ModelFinishing.hpp"
#include "slic3r/GUI/AI/Model/BeautyDocument.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelSemanticColoring.hpp"
#include "slic3r/AI/ModelGeneration/SemanticColoring/MediaPipeRegionRecognizers.hpp"
#include <catch2/catch_all.hpp>
#include <iterator>

using namespace Slic3r;
using GUI::SemanticRegionEvidence;
namespace SC = AI::SemanticColoring;
namespace cache = GUI::SemanticRegionEvidenceCache;
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
