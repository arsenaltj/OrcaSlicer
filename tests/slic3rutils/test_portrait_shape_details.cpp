#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "slic3r/GUI/AI/ModelGeneration/PortraitShapeDetails.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelSemanticColoring.hpp"
#include "slic3r/GUI/AI/ModelGeneration/LocalSemanticWorkerClient.hpp"
#include "slic3r/GUI/AI/ModelGeneration/SecondaryRegionEvidence.hpp"
#include "slic3r/GUI/AI/Model/LocalSemanticGeometry.hpp"
#include "slic3r/AI/ModelGeneration/SemanticColoring/MediaPipeRegionRecognizers.hpp"
#include "../test_utils.hpp"
#include <boost/nowide/cstdlib.hpp>
#include <atomic>
#include <chrono>
#include <iterator>
#include <set>
#include <thread>
#include <tuple>

using namespace Slic3r;
using namespace Slic3r::AI;
namespace SC = AI::SemanticColoring;
namespace Cache = GUI::PortraitShapeCache;
using GUI::PortraitShapeDetails;
using Json = nlohmann::json;

namespace {
struct Fixture {
    std::shared_ptr<BeautySurface> surface = BeautySurface::build(its_make_cube(10, 10, 10), {});
    PortraitShapeDetails details;
    SC::Analysis analysis;

    Fixture() {
        auto& locks = details.locks;
        locks.geometry_id = surface->geometry_id;
        locks.source_sha256 = std::string(64, 'a');
        locks.evidence_sha256 = std::string(64, 'b');
        locks.runtime_sha256 = std::string(64, 'c');
        locks.policy_sha256 = std::string(64, 'd');
        locks.face_count = surface->areas.size();
        auto add = [&](const std::string& subject, const std::string& label,
                       std::vector<size_t> faces, std::vector<size_t> nested = {}) {
            ShapeLock lock;
            lock.subject_id = subject;
            lock.label = label;
            lock.parent_label = "face";
            lock.status = "PROTECTED_SHAPE_UNCERTAIN";
            lock.locked_faces = std::move(faces);
            lock.nested_faces = std::move(nested);
            lock.view_support = 2;
            lock.reasons = {"SHAPE_OWNER_CONFLICT"};
            locks.locks.push_back(std::move(lock));
        };
        add("person-a", "re", {0, 1}, {1});
        add("person-b", "re", {2, 3}, {3});
        add("person-a", "ulip", {4, 5});
        add("person-a", "imouth", {6, 7});
        details.base_colors.assign(locks.face_count, RGBA{1, 1, 1, 1});
        for (size_t face = 0; face < 8; ++face) details.base_colors[face] = RGBA{0, 0, 0, 1};
        details.reserved_faces = {8};
        details.runtime_fingerprint = std::string(64, 'e');
        analysis.geometry_id = surface->geometry_id;
        analysis.face_labels.assign(locks.face_count, SC::Label::BodySkin);
        analysis.face_confidence.assign(locks.face_count, .95f);
    }
};

SC::FaceColors all_faces(size_t count, SC::Color color = {1, 0, 0})
{
    SC::FaceColors result;
    for (size_t face = 0; face < count; ++face) result.push_back({face, color});
    return result;
}

Json read_json(const boost::filesystem::path& file)
{
    boost::filesystem::ifstream stream(file, std::ios::binary);
    Json result;
    stream >> result;
    return result;
}

Json publish_json(const boost::filesystem::path& root, const std::string& directory,
                  const std::string& schema, const Json& value)
{
    const auto bytes = value.dump();
    const auto hash = Cache::digest(bytes);
    boost::filesystem::create_directories(root / directory);
    boost::filesystem::ofstream stream(root / directory / (hash + ".json"), std::ios::binary);
    stream << bytes;
    stream.close();
    if (!stream) throw std::runtime_error("Cannot publish temporary portrait shape context.");
    return {{"schema", schema}, {"path", directory + "/" + hash + ".json"}, {"sha256", hash}};
}

void preserve_fixture_source(Fixture& fixture, const boost::filesystem::path& root)
{
    const auto original = its_make_cube(10, 10, 10);
    indexed_triangle_set mesh;
    std::vector<RGBA> colors;
    for (size_t face = 0; face < original.indices.size(); ++face) {
        const int start = int(mesh.vertices.size());
        for (int corner = 0; corner < 3; ++corner) {
            mesh.vertices.push_back(original.vertices[original.indices[face][corner]]);
            colors.push_back(fixture.details.base_colors[face]);
        }
        mesh.indices.emplace_back(start, start + 1, start + 2);
    }
    std::string error;
    const auto source = root / "source-fixture.glb";
    REQUIRE(write_model_artifact(source, mesh, colors, error));
    fixture.details.locks.source_sha256 = model_artifact_sha256(source);
    REQUIRE_FALSE(fixture.details.locks.source_sha256.empty());
    REQUIRE(Cache::preserve_source(root, source, fixture.details.locks.source_sha256) ==
            Cache::source_path(root, fixture.details.locks.source_sha256));
}

class SourceBody final : public SC::IBodyRegionRecognizer {
    std::string m_identity;
    std::shared_ptr<std::atomic<unsigned>> m_calls;
public:
    SourceBody(std::string identity, std::shared_ptr<std::atomic<unsigned>> calls)
        : m_identity(std::move(identity)), m_calls(std::move(calls)) {}
    std::string identity() const override { return m_identity; }
    SC::Prediction predict(const SC::RGBImage& image, const SC::Cancel&) override {
        ++*m_calls;
        SC::Prediction prediction;
        prediction.labels.assign(size_t(image.width) * image.height, SC::Label::FaceSkin);
        prediction.confidence.assign(prediction.labels.size(), .95f);
        prediction.person_detected = true;
        return prediction;
    }
};

class SourceFace final : public SC::IFaceRegionRecognizer {
    std::string m_identity;
public:
    explicit SourceFace(std::string identity) : m_identity(std::move(identity)) {}
    std::string identity() const override { return m_identity; }
    SC::Prediction predict(const SC::RGBImage& image, const SC::Cancel&) override {
        SC::Prediction prediction;
        prediction.labels.assign(size_t(image.width) * image.height, SC::Label::Unknown);
        prediction.confidence.assign(prediction.labels.size(), .95f);
        prediction.face_detected = true;
        return prediction;
    }
};
}

TEST_CASE("Native fine labels retain source appearance when no shape evidence is present", "[PortraitShapeDetails]")
{
    Fixture f;
    f.analysis.face_labels[0] = SC::Label::EyeSclera;
    f.analysis.face_labels[1] = SC::Label::Iris;
    f.analysis.face_labels[2] = SC::Label::Eyebrow;
    f.analysis.face_labels[3] = SC::Label::Lips;
    f.analysis.face_labels[4] = SC::Label::MouthInterior;
    f.analysis.subface_labels = {{5, {1, 0}, SC::Label::Lips, .95f, 4}};
    auto colors = all_faces(12);
    SC::SubfaceColors children{{5, {1, 0}, {1, 0, 0}, .95f}, {6, {1, 0}, {0, 1, 0}, .95f}};
    GUI::compose_portrait_shapes(f.analysis, nullptr, colors, children);
    REQUIRE(colors.size() == 6);
    CHECK(colors.front().first == 6);
    REQUIRE(children.size() == 1);
    CHECK(children.front().face_id == 6);
}

TEST_CASE("Shape composition replaces only legal locked faces and reserves rejected detail context", "[PortraitShapeDetails]")
{
    Fixture f;
    auto colors = all_faces(12);
    SC::SubfaceColors children{{4, {1, 0}, {1, 0, 0}, .95f}, {8, {1, 0}, {1, 0, 0}, .95f},
                              {9, {1, 0}, {0, 1, 0}, .95f}};
    const SC::FaceColors locked{{0, {0, 0, 0}}, {1, {1, 1, 1}}, {4, {1, 0, 1}}, {5, {1, 0, 1}}};
    GUI::compose_portrait_shapes(f.analysis, &f.details, colors, children, locked);
    REQUIRE(colors.size() == 7);
    for (const auto& item : locked) CHECK(std::find(colors.begin(), colors.end(), item) != colors.end());
    for (const auto& item : colors) CHECK(item.first != 8);
    for (const auto& item : colors) CHECK(item.first != 6);
    for (const auto& item : colors) CHECK(item.first != 7);
    REQUIRE(children.size() == 1);
    CHECK(children.front().face_id == 9);
    CHECK(std::is_sorted(colors.begin(), colors.end(), [](const auto& a, const auto& b) { return a.first < b.first; }));
}

TEST_CASE("Foreign shape geometry and unauthorized or oral color output are rejected", "[PortraitShapeDetails]")
{
    Fixture f;
    auto colors = all_faces(12);
    SC::SubfaceColors children;
    const int change = GENERATE(0, 1, 2, 3, 4);
    SC::FaceColors locked;
    if (change == 0) f.details.locks.geometry_id = std::string(64, 'f');
    if (change == 1) locked = {{9, {0, 0, 0}}};
    if (change == 2) locked = {{6, {0, 0, 0}}};
    if (change == 3) locked = {{12, {0, 0, 0}}};
    if (change == 4) f.details.base_colors.pop_back();
    CHECK_THROWS(GUI::compose_portrait_shapes(f.analysis, &f.details, colors, children, locked));
}

TEST_CASE("Parent palette overrides cannot restore unverified native facial colors", "[PortraitShapeDetails][SemanticColoring]")
{
    Fixture f;
    f.analysis.face_labels[0] = SC::Label::EyeSclera;
    f.analysis.face_labels[1] = SC::Label::Iris;
    f.analysis.face_labels[2] = SC::Label::Eyebrow;
    f.analysis.face_labels[3] = SC::Label::Lips;
    f.analysis.face_labels[4] = SC::Label::MouthInterior;
    f.analysis.subface_labels = {{5, {1, 0}, SC::Label::Lips, .95f, 4}};
    const std::vector<SC::Color> palette{{1, 0, 0}, {0, 0, 1}};
    const SC::SemanticRegionSlotBindings overrides{{0, 0, 0, 0}};
    const auto automatic = all_faces(12, {0, 0, 1});
    auto remapped = SC::apply_semantic_region_slot_overrides(automatic, f.analysis, overrides, palette);
    SC::SubfaceColors children{{5, {1, 0}, {0, 0, 1}, .95f}};
    children = SC::apply_semantic_region_slot_overrides(children, f.analysis, overrides, palette);
    REQUIRE(remapped.size() == automatic.size());
    CHECK(remapped.front().second == palette.front());
    REQUIRE(children.size() == 1);
    CHECK(children.front().color == palette.front());
    GUI::compose_portrait_shapes(f.analysis, nullptr, remapped, children);
    for (const auto& item : remapped) CHECK(item.first >= 6);
    CHECK(children.empty());
    REQUIRE(remapped.size() == 6);
    for (const auto& item : remapped) CHECK(item.second == SC::Color{0, 0, 1});
}

TEST_CASE("Shape catalogs keep duplicate labels separate across persons and isolate nested iris", "[PortraitShapeDetails]")
{
    const Fixture f;
    const auto catalog = f.details.catalog("eyes");
    REQUIRE(catalog.size() == 4);
    std::set<std::string> keys;
    std::set<size_t> faces;
    for (const auto& detail : catalog) {
        CHECK(keys.insert(detail.key).second);
        CHECK(detail.status == "PROTECTED_SHAPE_UNCERTAIN");
        CHECK(detail.reasons == std::vector<std::string>{"SHAPE_OWNER_CONFLICT"});
        for (const auto face : detail.faces) CHECK(faces.insert(face).second);
    }
    CHECK(keys.count("person-a:re") == 1);
    CHECK(keys.count("person-b:re") == 1);
    CHECK(keys.count("person-a:re:iris") == 1);
    CHECK(keys.count("person-b:re:iris") == 1);
    CHECK(catalog[0].faces == std::vector<size_t>{0});
    CHECK(catalog[1].faces == std::vector<size_t>{1});
    const auto guidance = f.details.guidance();
    CHECK(guidance.names[size_t(guidance.labels[0])] == "re");
    CHECK(guidance.names[size_t(guidance.labels[1])] == "iris");
    CHECK(guidance.labels[0] != guidance.labels[2]);
    CHECK(guidance.labels[8] == -1);
}

TEST_CASE("Portrait matching samples only locks and manual layers retain precedence everywhere", "[PortraitShapeDetails][BeautyWorkbench]")
{
    const Fixture f;
    const auto source_before = f.details.base_colors;
    const std::vector<PhysicalFilamentChannel> palette{{0, "#FFFFFF", "PLA", true}, {1, "#000000", "PLA", true}};
    const auto matched = GUI::match_portrait_shape_colors(f.details, *f.surface, palette);
    REQUIRE(matched.size() == 6);
    for (const auto& item : matched) {
        CHECK(item.first < 6);
        CHECK(item.second == SC::Color{0, 0, 0});
    }
    CHECK(f.details.base_colors == source_before);
    const SC::FaceColors manual{{0, {1, 0, 0}}, {10, {0, 1, 0}}};
    const auto final = SC::compose(matched, manual, true);
    for (const auto& item : manual) CHECK(std::find(final.begin(), final.end(), item) != final.end());
    for (const auto& item : final) CHECK(item.first != 6);
    for (const auto& item : final) CHECK(item.first != 7);
    CHECK(std::find(final.begin(), final.end(), SC::FaceColors::value_type{0, {0, 0, 0}}) == final.end());
}

TEST_CASE("Both eyes of one person keep distinct sclera and iris catalog entries and pieces", "[PortraitShapeDetails][BeautyPuzzle]")
{
    Fixture f;
    f.details.locks.locks[1].subject_id = "person-a";
    f.details.locks.locks[1].label = "le";
    const auto catalog = f.details.catalog("eyes");
    REQUIRE(catalog.size() == 4);
    const std::map<std::string, std::vector<size_t>> expected {
        {"person-a:re", {0}}, {"person-a:re:iris", {1}},
        {"person-a:le", {2}}, {"person-a:le:iris", {3}}};
    std::set<size_t> owned;
    std::set<std::string> keys;
    for (const auto& detail : catalog) {
        REQUIRE(expected.count(detail.key) == 1);
        CHECK(keys.insert(detail.key).second);
        CHECK(detail.subject == "person-a");
        CHECK(detail.faces == expected.at(detail.key));
        for (const auto face : detail.faces) CHECK(owned.insert(face).second);
    }
    const auto guidance = f.details.guidance();
    for (size_t face = 0; face < 4; ++face)
        for (size_t other = face + 1; other < 4; ++other)
            CHECK(guidance.labels[face] != guidance.labels[other]);
    CHECK(guidance.names.at(size_t(guidance.labels[0])) == "re");
    CHECK(guidance.names.at(size_t(guidance.labels[1])) == "iris");
    CHECK(guidance.names.at(size_t(guidance.labels[2])) == "le");
    CHECK(guidance.names.at(size_t(guidance.labels[3])) == "iris");
    auto puzzle = BeautyPuzzle::create(*f.surface);
    f.details.locks.isolate(puzzle, *f.surface);
    REQUIRE_NOTHROW(puzzle.validate(*f.surface));
    for (size_t face = 0; face < 4; ++face)
        for (size_t other = face + 1; other < 12; ++other)
            CHECK(puzzle.face_piece[face] != puzzle.face_piece[other]);
    CHECK(f.details.locks.preserves(puzzle, puzzle));
}

TEST_CASE("Paired eye matching colors sclera and nested irises without crossing either eye or parent", "[PortraitShapeDetails][BeautyWorkbench]")
{
    Fixture f;
    f.details.locks.locks[1].subject_id = "person-a";
    f.details.locks.locks[1].label = "le";
    f.details.base_colors[0] = f.details.base_colors[2] = RGBA{1, 1, 1, 1};
    const auto identity = f.details.locks.encode();
    const auto source = f.details.base_colors;
    const std::vector<PhysicalFilamentChannel> palette {
        {0, "#FFFFFF", "PLA", true}, {1, "#000000", "PLA", true}, {2, "#FF00FF", "PLA", true}};
    const auto matched = GUI::match_portrait_shape_colors(f.details, *f.surface, palette);
    REQUIRE(matched.size() == 6);
    std::map<size_t, SC::Color> eye_colors;
    for (const auto& item : matched) {
        CHECK(f.details.locks.face_locked(item.first));
        CHECK(item.first < 6);
        CHECK(eye_colors.emplace(item).second);
    }
    CHECK(eye_colors.at(0) == SC::Color{1, 1, 1});
    CHECK(eye_colors.at(1) == SC::Color{0, 0, 0});
    CHECK(eye_colors.at(2) == SC::Color{1, 1, 1});
    CHECK(eye_colors.at(3) == SC::Color{0, 0, 0});
    auto candidate = all_faces(12);
    SC::SubfaceColors children {{0, {1, 0}, {1, 0, 0}, .95f}, {9, {1, 0}, {0, 1, 0}, .95f}};
    GUI::compose_portrait_shapes(f.analysis, &f.details, candidate, children, matched);
    const std::map<size_t, SC::Color> composed(candidate.begin(), candidate.end());
    for (const auto& item : matched) CHECK(composed.at(item.first) == item.second);
    CHECK(composed.count(6) == 0);
    CHECK(composed.count(7) == 0);
    CHECK(composed.count(8) == 0);
    for (size_t face = 9; face < 12; ++face) CHECK(composed.at(face) == SC::Color{1, 0, 0});
    REQUIRE(children.size() == 1);
    CHECK(children.front().face_id == 9);
    const SC::FaceColors manual {{1, {1, 0, 1}}};
    const auto final = SC::compose(candidate, manual, true);
    const std::map<size_t, SC::Color> displayed(final.begin(), final.end());
    CHECK(displayed.at(1) == SC::Color{1, 0, 1});
    CHECK(displayed.at(0) == composed.at(0));
    CHECK(displayed.at(2) == composed.at(2));
    CHECK(displayed.at(3) == composed.at(3));
    CHECK(f.details.locks.encode() == identity);
    CHECK(f.details.base_colors == source);
}

TEST_CASE("Locked facial matching preserves semantic palette roles across three to six colors", "[PortraitShapeDetails][SemanticColoring]")
{
    Fixture f;
    f.details.locks.locks[0].label = "lb";
    f.details.locks.locks[0].nested_faces.clear();
    f.details.base_colors[0] = f.details.base_colors[1] = RGBA{.46f, .37f, .32f, 1};
    f.details.base_colors[2] = RGBA{.60f, .58f, .55f, 1};
    f.details.base_colors[3] = RGBA{.07f, .06f, .06f, 1};
    f.details.base_colors[4] = f.details.base_colors[5] = RGBA{.65f, .24f, .25f, 1};
    const size_t count = GENERATE(3, 4, 5, 6);
    std::vector<PhysicalFilamentChannel> palette {{0, "#F7E2DA", "PLA", true},
        {1, "#282629", "PLA", true}, {2, "#F6F7F9", "PLA", true},
        {3, "#EA9A92", "PLA", true}, {4, "#668C99", "PLA", true}, {5, "#99958F", "PLA", true}};
    palette.resize(count);
    const auto before = f.details.locks.encode();
    const auto matched = GUI::match_portrait_shape_colors(f.details, *f.surface, palette);
    REQUIRE(matched.size() == 6);
    for (const auto& item : matched) {
        if (item.first < 2) CHECK(item.second == SC::Color{count == 6 ? .6f : 40.f/255.f,
            count == 6 ? 149.f/255.f : 38.f/255.f, count == 6 ? 143.f/255.f : 41.f/255.f});
        if (item.first == 3) CHECK(item.second == SC::Color{40.f/255.f, 38.f/255.f, 41.f/255.f});
        if (item.first < 4 && count >= 4) CHECK(item.second != SC::Color{234.f/255.f, 154.f/255.f, 146.f/255.f});
    }
    CHECK(f.details.locks.encode() == before);
}

TEST_CASE("A locked eye or brow with no legal palette target retains prior appearance", "[PortraitShapeDetails]")
{
    Fixture f;
    f.details.locks.locks[0].label = "lb";
    f.details.locks.locks[0].nested_faces.clear();
    const std::vector<PhysicalFilamentChannel> palette {{4, "#EA9A92", "PLA", true}};
    const auto matched = GUI::match_portrait_shape_colors(f.details, *f.surface, palette);
    REQUIRE(matched.size() == 2);
    for (const auto& item : matched) CHECK((item.first == 4 || item.first == 5));
}

TEST_CASE("Risky warm skin colored brow bands stay editable without becoming a second pigment line", "[PortraitShapeDetails]")
{
    Fixture f;
    f.details.locks.locks[0].label = "lb";
    f.details.locks.locks[0].nested_faces.clear();
    f.details.base_colors[0] = f.details.base_colors[1] = RGBA{207.f/255.f, 170.f/255.f, 153.f/255.f, 1};
    const std::vector<PhysicalFilamentChannel> palette {{0, "#F7E2DA", "PLA", true},
        {1, "#282629", "PLA", true}, {2, "#F6F7F9", "PLA", true},
        {3, "#EA9A92", "PLA", true}, {4, "#668C99", "PLA", true}, {5, "#99958F", "PLA", true}};
    const auto matched = GUI::match_portrait_shape_colors(f.details, *f.surface, palette);
    for (const auto& item : matched) CHECK(item.first >= 2);
    CHECK(f.details.catalog().front().faces == std::vector<size_t>{0, 1});
    CHECK(f.details.locks.face_locked(0));
    const SC::Color skin {247.f/255.f, 226.f/255.f, 218.f/255.f};
    const auto explicit_parent = GUI::match_portrait_shape_colors(f.details, *f.surface, palette, {{0, skin}, {1, skin}});
    const auto face = std::find_if(explicit_parent.begin(), explicit_parent.end(), [](const auto& item) { return item.first == 0; });
    REQUIRE(face != explicit_parent.end());
    CHECK(face->second == skin);
}

TEST_CASE("Portrait shape contexts restore colors locks masks and risks from an addressed cache", "[PortraitShapeDetails]")
{
    Fixture f;
    ScopedTemporaryDir directory("portrait-shape-context");
    preserve_fixture_source(f, directory.path());
    const auto reference = Cache::save(f.details, directory.path());
    const auto restored = Cache::load(reference, directory.path(), f.surface->geometry_id, 12, f.details.runtime_fingerprint);
    CHECK(restored->locks.encode() == f.details.locks.encode());
    CHECK(restored->base_colors == f.details.base_colors);
    CHECK(restored->reserved_faces == f.details.reserved_faces);
    CHECK(restored->runtime_fingerprint == f.details.runtime_fingerprint);
    CHECK(Cache::save(f.details, directory.path()) == reference);
}

TEST_CASE("Portrait context references reject unsafe paths hashes runtimes and geometry", "[PortraitShapeDetails]")
{
    Fixture f;
    ScopedTemporaryDir directory("portrait-shape-context");
    preserve_fixture_source(f, directory.path());
    auto reference = Cache::save(f.details, directory.path());
    auto geometry = f.surface->geometry_id;
    auto runtime = f.details.runtime_fingerprint;
    size_t count = 12;
    const int change = GENERATE(0, 1, 2, 3, 4, 5, 6);
    if (change == 0) reference["path"] = "../portrait-shapes/context.json";
    if (change == 1) reference["path"] = "C:/portrait-shapes/context.json";
    if (change == 2) reference["sha256"] = std::string(64, 'f');
    if (change == 3) runtime = std::string(64, 'f');
    if (change == 4) geometry = std::string(64, 'f');
    if (change == 5) count = 11;
    if (change == 6) {
        boost::filesystem::ofstream tamper(directory.path() / reference.at("path").get<std::string>(), std::ios::binary | std::ios::app);
        tamper << ' ';
        tamper.close();
    }
    CHECK_THROWS(Cache::load(reference, directory.path(), geometry, count, runtime));
}

TEST_CASE("Even hash bound contexts reject malformed source colors reserved faces and sidecars", "[PortraitShapeDetails]")
{
    Fixture f;
    ScopedTemporaryDir directory("portrait-shape-context");
    preserve_fixture_source(f, directory.path());
    const auto original = Cache::save(f.details, directory.path());
    auto value = read_json(directory.path() / original.at("path").get<std::string>());
    const int change = GENERATE(0, 1, 2, 3, 4, 5, 6);
    if (change == 0) value["base_colors"].erase(value["base_colors"].end() - 1);
    if (change == 1) value["base_colors"][0][0] = -0.1;
    if (change == 2) value["base_colors"][0][0] = 1.1;
    if (change == 3) value["base_colors"][0][0] = nullptr;
    if (change == 4) value["reserved_faces"] = {8, 8};
    if (change == 5) value["reserved_faces"] = {12};
    if (change == 6) value["shape_lock_ref"]["path"] = "../shape-locks/file.json";
    const auto reference = publish_json(directory.path(), "portrait-shapes", "orca.portrait-shape-reference/v1", value);
    CHECK_THROWS(Cache::load(reference, directory.path(), f.surface->geometry_id, 12, f.details.runtime_fingerprint));
}

TEST_CASE("Portrait shape contexts preserve older seven field sidecar records", "[PortraitShapeDetails][Regression]")
{
    Fixture f;
    ScopedTemporaryDir directory("portrait-shape-old-context");
    preserve_fixture_source(f, directory.path());
    const auto original = Cache::save(f.details, directory.path());
    auto context = read_json(directory.path() / original.at("path").get<std::string>());
    auto sidecar = context["identity"];
    for (auto& lock : sidecar["locks"]) lock.erase("reasons");
    const auto sidecar_ref = publish_json(directory.path(), "shape-locks", ShapeLockReference::schema, sidecar);
    context["identity"] = sidecar;
    context["shape_lock_ref"] = sidecar_ref;
    const auto reference = publish_json(directory.path(), "portrait-shapes", "orca.portrait-shape-reference/v1", context);
    const auto restored = Cache::load(reference, directory.path(), f.surface->geometry_id, 12, f.details.runtime_fingerprint);
    REQUIRE(restored->locks.locks.size() == 4);
    for (size_t index = 0; index < 4; ++index) {
        CHECK(restored->locks.locks[index].locked_faces == f.details.locks.locks[index].locked_faces);
        CHECK(restored->locks.locks[index].reasons.empty());
    }
}

TEST_CASE("Stale portrait runtimes restore verified source colors without restoring old shape locks", "[PortraitShapeDetails]")
{
    Fixture f;
    ScopedTemporaryDir directory("portrait-shape-stale-runtime");
    preserve_fixture_source(f, directory.path());
    const auto reference = Cache::save(f.details, directory.path());
    const auto new_runtime = std::string(64, 'f');
    CHECK_THROWS(Cache::load(reference, directory.path(), f.surface->geometry_id, 12, new_runtime));
    const auto restored = Cache::load(reference, directory.path(), f.surface->geometry_id, 12, new_runtime, true);
    CHECK(restored->locks.empty());
    CHECK(restored->base_colors == f.details.base_colors);
    CHECK(restored->locks.source_sha256 == f.details.locks.source_sha256);
    CHECK(restored->runtime_fingerprint == f.details.runtime_fingerprint);
    CHECK(restored->catalog().empty());
}

TEST_CASE("Stale runtime recovery preserves saved user filament colors while removing old boundaries", "[PortraitShapeDetails][BeautyWorkbench][Regression]")
{
    Fixture f;
    ScopedTemporaryDir directory("portrait-shape-stale-user-edit");
    preserve_fixture_source(f, directory.path());
    const auto reference = Cache::save(f.details, directory.path());
    auto puzzle = BeautyPuzzle::create(*f.surface);
    f.details.locks.isolate(puzzle, *f.surface);
    puzzle.palette = {{0, "#FFFFFF", "PLA", true}, {1, "#000000", "PLA", true}, {2, "#FF00FF", "PLA", true}};
    puzzle.paint_filament(puzzle.face_piece[1], 2);
    puzzle.paint_filament(puzzle.face_piece[10], 0);
    const Json metadata {{"portrait_shape_reference", reference}, {"beauty_workbench", {{"puzzle", puzzle.encode()}}}};
    const auto record = publish_json(directory.path(), "saved-workbench", "fixture.workbench-record/v1", metadata);
    const auto saved = read_json(directory.path() / record.at("path").get<std::string>());
    const auto restored = Cache::load(saved.at("portrait_shape_reference"), directory.path(), f.surface->geometry_id,
                                     12, std::string(64, 'f'), true);
    CHECK(restored->locks.empty());
    CHECK(restored->catalog().empty());
    CHECK(restored->base_colors == f.details.base_colors);
    auto reopened = BeautyPuzzle::decode(saved.at("beauty_workbench").at("puzzle"), f.surface->geometry_id, 12);
    REQUIRE_NOTHROW(reopened.validate(*f.surface));
    CHECK(reopened.same_edit(puzzle));
    restored->locks.isolate(reopened, *f.surface);
    CHECK(reopened.same_edit(puzzle));
    CHECK(reopened.filament_slots.at(reopened.face_piece[1]) == 2);
    CHECK(reopened.filament_slots.at(reopened.face_piece[10]) == 0);
    const auto matched = GUI::match_portrait_shape_colors(*restored, *f.surface, reopened.palette);
    CHECK(matched.empty());
    SC::FaceColors manual;
    for (size_t face = 0; face < reopened.face_piece.size(); ++face) {
        const auto color = reopened.colors.find(reopened.face_piece[face]);
        if (color != reopened.colors.end()) manual.push_back({face, {color->second[0], color->second[1], color->second[2]}});
    }
    REQUIRE_FALSE(manual.empty());
    const auto displayed = SC::compose(all_faces(12), manual, true);
    const std::map<size_t, SC::Color> colors(displayed.begin(), displayed.end());
    for (const auto& item : manual) CHECK(colors.at(item.first) == item.second);
    CHECK(colors.at(1) == SC::Color{1, 0, 1});
    CHECK(colors.at(10) == SC::Color{1, 1, 1});
    CHECK(model_artifact_sha256(Cache::source_path(directory.path(), restored->locks.source_sha256)) ==
          f.details.locks.source_sha256);
}

TEST_CASE("Missing or changed canonical portrait sources cannot supply cached source colors", "[PortraitShapeDetails]")
{
    Fixture f;
    ScopedTemporaryDir directory("portrait-shape-source-drift");
    preserve_fixture_source(f, directory.path());
    const auto reference = Cache::save(f.details, directory.path());
    const auto source = Cache::source_path(directory.path(), f.details.locks.source_sha256);
    const bool missing = GENERATE(false, true);
    if (missing) boost::filesystem::remove(source);
    else {
        boost::filesystem::ofstream tamper(source, std::ios::binary | std::ios::app);
        tamper << ' ';
        tamper.close();
    }
    CHECK_THROWS(Cache::verified_source(directory.path(), f.details.locks.source_sha256));
    CHECK_THROWS(Cache::load(reference, directory.path(), f.surface->geometry_id, 12, f.details.runtime_fingerprint));
    CHECK_THROWS(Cache::save(f.details, directory.path()));
    CHECK_THROWS(Cache::preserve_source(directory.path(), directory.path() / "source-fixture.glb",
                                      std::string(64, 'f')));
}

TEST_CASE("Editing a portrait appearance keeps native recognition bound to its canonical source", "[PortraitShapeDetails][ModelSemanticColoring]")
{
    Fixture f;
    ScopedTemporaryDir directory("portrait-canonical-recognition");
    const auto cache = directory.path() / "cache";
    preserve_fixture_source(f, cache);
    TriangleMesh mesh;
    ObjInfo colors;
    std::string error;
    REQUIRE(load_model_artifact(Cache::source_path(cache, f.details.locks.source_sha256), mesh, colors, error));
    auto canonical = std::make_shared<SC::MeshSnapshot>();
    canonical->mesh = mesh.its;
    canonical->vertex_colors = colors.vertex_colors;
    canonical->face_colors = colors.face_colors;
    canonical->geometry_id = AI::SurfaceSelectionPersistence::geometry_fingerprint(mesh.its);
    canonical->content_id = SC::content_fingerprint(*canonical);
    auto shapes = std::make_shared<PortraitShapeDetails>(f.details);
    shapes->locks.geometry_id = canonical->geometry_id;
    REQUIRE(shapes->compatible(canonical->geometry_id, canonical->mesh.indices.size()));
    const auto body_id = directory.path().filename().string() + ".body";
    const auto face_id = directory.path().filename().string() + ".face";
    const auto calls = std::make_shared<std::atomic<unsigned>>(0);
    REQUIRE(SC::register_body_recognizer_factory(body_id, [body_id, calls](const std::filesystem::path&) {
        return std::make_unique<SourceBody>(body_id, calls);
    }));
    REQUIRE(SC::register_face_recognizer_factory(face_id, [face_id](const std::filesystem::path&) {
        return std::make_unique<SourceFace>(face_id);
    }));
    boost::filesystem::ofstream configuration(directory.path() / "providers.json", std::ios::binary);
    configuration << Json{{"body_provider", body_id}, {"face_provider", face_id}};
    configuration.close();
    REQUIRE(bool(configuration));
    struct ScopedData {
        std::string previous = Slic3r::data_dir();
        ~ScopedData() { Slic3r::set_data_dir(previous); }
    } scoped_data;
    Slic3r::set_data_dir(directory.path().string());
    GUI::ModelSemanticColoring coordinator(std::filesystem::path(directory.path().native()),
        std::filesystem::path((directory.path() / "native-cache").native()));
    const auto await = [&] {
        std::unique_ptr<GUI::ModelSemanticColoring::Result> result;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!result && std::chrono::steady_clock::now() < deadline) {
            result = coordinator.poll();
            if (!result && !coordinator.busy()) break;
            if (!result) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return result;
    };
    const std::vector<SC::Color> palette{{1, 1, 1}, {0, 0, 0}, {1, 0, 1}};
    auto edited = std::make_shared<SC::MeshSnapshot>(*canonical);
    edited->vertex_colors.assign(edited->mesh.vertices.size(), RGBA{1, 0, 1, 1});
    edited->face_colors.assign(edited->mesh.indices.size(), RGBA{1, 0, 1, 1});
    edited->content_id = SC::content_fingerprint(*edited);
    REQUIRE(edited->content_id != canonical->content_id);
    REQUIRE(coordinator.request(edited, palette, palette, {}, {}, {}, shapes));
    const auto first = await();
    REQUIRE(first);
    CAPTURE(first->error);
    REQUIRE(first->error.empty());
    REQUIRE(first->analysis);
    CHECK(first->verified_original_source);
    CHECK(first->analysis->content_id == canonical->content_id);
    const auto primary = GUI::SemanticRegionEvidence::from_analysis(*first->analysis, std::string(64, 'f'));
    REQUIRE(primary);
    const auto appearance = directory.path() / "edited-appearance.glb";
    REQUIRE(write_model_artifact(appearance, edited->mesh, edited->vertex_colors, error));
    const auto appearance_hash = model_artifact_sha256(appearance);
    REQUIRE(appearance_hash != f.details.locks.source_sha256);
    CHECK_FALSE(GUI::SecondaryRegionEvidence::from_primary(*primary, appearance_hash, edited->content_id, error));
    CHECK_FALSE(error.empty());
    const auto secondary = GUI::SecondaryRegionEvidence::from_primary(*primary, appearance_hash,
        edited->content_id, error, first->verified_original_source);
    REQUIRE(secondary);
    CHECK(error.empty());
    CHECK(secondary->source_sha256 == appearance_hash);
    CHECK(secondary->geometry_id == canonical->geometry_id);
    CHECK(secondary->face_count == canonical->mesh.indices.size());
    REQUIRE(calls->load() > 0);
    const auto body_calls = calls->load();
    auto edited_again = std::make_shared<SC::MeshSnapshot>(*edited);
    edited_again->vertex_colors.assign(edited_again->mesh.vertices.size(), RGBA{0, 1, 0, 1});
    edited_again->face_colors.assign(edited_again->mesh.indices.size(), RGBA{0, 1, 0, 1});
    edited_again->content_id = SC::content_fingerprint(*edited_again);
    REQUIRE(edited_again->content_id != edited->content_id);
    REQUIRE(coordinator.request(edited_again, palette, palette, {}, {}, {}, shapes));
    const auto second = await();
    REQUIRE(second);
    CAPTURE(second->error);
    REQUIRE(second->error.empty());
    REQUIRE(second->analysis);
    CHECK(second->verified_original_source);
    CHECK(second->analysis->content_id == canonical->content_id);
    CHECK(second->analysis->signature == first->analysis->signature);
    CHECK(second->cache_hit);
    CHECK(second->native_automatic == first->native_automatic);
    CHECK(calls->load() == body_calls);
    CHECK(model_artifact_sha256(Cache::source_path(cache, f.details.locks.source_sha256)) == f.details.locks.source_sha256);
}

TEST_CASE("A fixed portrait exports source native and integrated palettes without writing production", "[.][PortraitIntegrationReplay]")
{
    const auto env = [](const char* key) {
        const auto value = boost::nowide::getenv(key);
        return value ? std::string(value) : std::string{};
    };
    const boost::filesystem::path checkout(env("ORCA_SHAPE_REPLAY_ROOT"));
    const boost::filesystem::path output(env("ORCA_PORTRAIT_EXPORT_ROOT"));
    const std::filesystem::path runtime(std::filesystem::u8path(env("ORCA_PORTRAIT_RUNTIME")));
    const boost::filesystem::path palettes(env("ORCA_PORTRAIT_PALETTE_ROOT"));
    const boost::filesystem::path supplied_evidence(env("ORCA_PORTRAIT_EVIDENCE_ROOT"));
    const bool fresh_evidence = !supplied_evidence.empty();
    const auto resources = env("ORCA_PORTRAIT_RESOURCES"), data = env("ORCA_PORTRAIT_DATA");
    if (checkout.empty() || output.empty() || runtime.empty() || palettes.empty() || resources.empty() || data.empty())
        SKIP("Explicit frozen source, native runtime, palette, staged resources, isolated data and fresh output are required.");
    REQUIRE_FALSE(boost::filesystem::exists(output));
    struct ScopedDirectories {
        std::string resources = Slic3r::resources_dir(), data = Slic3r::data_dir();
        ~ScopedDirectories() { Slic3r::set_resources_dir(resources); Slic3r::set_data_dir(data); }
    } directories;
    Slic3r::set_resources_dir(resources);
    Slic3r::set_data_dir(data);
    const auto current_runtime = GUI::portrait_shape_runtime_fingerprint();
    REQUIRE_FALSE(current_runtime.empty());
    const auto model = checkout / ".tmp/dev/data-shape/generated_models/99d36f6d-7534-49d7-8256-52c34644f7bf/model.glb";
    const auto frozen = fresh_evidence ? supplied_evidence : checkout / ".tmp/dev/data-shape/shape-replay-20261006-a";
    const std::string source_hash = "793c69491bc1039da75aca8eda4b5dd4307335fe0f02b84696562a86f722c066";
    const std::string evidence_hash = fresh_evidence ? env("ORCA_PORTRAIT_EVIDENCE_SHA256") :
        "c34c2c2600f36fe3331007f6531cf3e193079c284754bb291593051ea451efd9";
    const std::string geometry_hash = "ed727689bd1658ac0c59a5726214d851fdaad3945b4475bb53129b85bb3f7026";
    REQUIRE(ShapeLockSet::sha256(evidence_hash));
    REQUIRE(model_artifact_sha256(model) == source_hash);
    REQUIRE(model_artifact_sha256(frozen / "evidence.json") == evidence_hash);
    TriangleMesh mesh;
    ObjInfo colors;
    std::string error;
    REQUIRE(load_model_artifact(model, mesh, colors, error));
    REQUIRE(mesh.its.indices.size() == 976825);
    const auto read = [](const boost::filesystem::path& path) {
        boost::filesystem::ifstream stream(path, std::ios::binary);
        if (!stream) throw std::runtime_error("Cannot read fixed integration evidence.");
        return std::string(std::istreambuf_iterator<char>(stream), {});
    };
    namespace Semantic = GUI::LocalSemanticEvidence;
    namespace Geometry = GUI::LocalSemanticGeometry;
    Geometry::Packet packet;
    REQUIRE(Geometry::decode(read(frozen / "rendered.bin"), source_hash, packet, error));
    Semantic::VerifiedFaceBinding binding;
    REQUIRE(Semantic::prove_ordered_faces(source_hash, mesh.its, packet.mesh, binding, error));
    REQUIRE(binding.geometry_id() == geometry_hash);
    const auto bytes = read(frozen / "evidence.json");
    const auto document = Json::parse(bytes);
    Semantic::ExpectedIdentity expected;
    expected.request_id = document.at("request_id");
    expected.source_sha256 = source_hash;
    expected.geometry_id = geometry_hash;
    expected.face_count = mesh.its.indices.size();
    expected.weights_sha256 = document.at("weights_sha256");
    expected.runtime_sha256 = document.at("runtime_sha256");
    expected.policy_sha256 = document.at("policy_sha256");
    Json capability_report;
    if (fresh_evidence) {
        const auto expected_runtime = env("ORCA_PORTRAIT_EVIDENCE_RUNTIME_SHA256");
        const auto expected_policy = env("ORCA_PORTRAIT_EVIDENCE_POLICY_SHA256");
        REQUIRE(ShapeLockSet::sha256(expected_runtime));
        REQUIRE(ShapeLockSet::sha256(expected_policy));
        REQUIRE(expected.runtime_sha256 == expected_runtime);
        REQUIRE(expected.policy_sha256 == expected_policy);
        const auto response = read_json(frozen / "result.json");
        REQUIRE(response.at("status") == "ok");
        REQUIRE(response.at("request_id") == expected.request_id);
        REQUIRE(response.at("runtime_fingerprint") == expected_runtime);
        REQUIRE(response.at("policy_sha256") == expected_policy);
        REQUIRE(Cache::digest(response.at("identity").dump()) == expected_runtime);
        REQUIRE(response.at("files").at("evidence.json").at("sha256") == evidence_hash);
        REQUIRE(response.at("files").at("rendered.bin").at("sha256") == model_artifact_sha256(frozen / "rendered.bin"));
        REQUIRE(response.at("files").at("evidence.json").at("bytes") == bytes.size());
        REQUIRE(response.at("files").at("rendered.bin").at("bytes") == boost::filesystem::file_size(frozen / "rendered.bin"));
        GUI::LocalSemanticWorker::Configuration config;
        REQUIRE(GUI::LocalSemanticWorker::read_runtime_configuration(boost::filesystem::path(data) / "local_semantic_runtime.json",
            boost::filesystem::path(resources) / "beauty-runtime", config, error));
        REQUIRE(config.enabled);
        const auto modules = boost::filesystem::path(resources) / "tools" / "ai";
        const auto& module_hashes = response.at("identity").at("modules_sha256");
        REQUIRE(module_hashes.is_object());
        std::vector<std::string> names {"glb_artifact.py", "local_semantic_worker.py", "local_semantic_geometry.py",
            "local_semantic_render.py", "local_semantic_transform.py", "local_semantic_views.py", "local_semantic_projection.py",
            "local_semantic_pipeline.py", "local_semantic_request.py", "local_eye_landmarks.py", "local_face_landmarks.py",
            "local_shape_constraints.py", "local_brow_boundary.py", "local_body_regions.py"};
        if (boost::filesystem::exists(modules / "local_semantic_raster.dll")) names.push_back("local_semantic_raster.dll");
        REQUIRE(module_hashes.size() == names.size());
        for (const auto& name : names) REQUIRE(module_hashes.at(name) == model_artifact_sha256(modules / name));
        const auto& probe = response.at("identity").at("probe_identity");
        REQUIRE(probe.at("python_executable_sha256") == model_artifact_sha256(config.python_executable));
        REQUIRE(probe.at("worker_sha256") == module_hashes.at("local_semantic_worker.py"));
        REQUIRE(Cache::digest(probe.at("weights").dump()) == expected.weights_sha256);
        for (auto weight = probe.at("weights").begin(); weight != probe.at("weights").end(); ++weight) {
            REQUIRE(boost::filesystem::path(weight.key()).filename().string() == weight.key());
            REQUIRE(weight.value() == model_artifact_sha256(config.weights_directory / weight.key()));
        }
        const std::atomic<bool> cancelled {false};
        const auto capability = GUI::LocalSemanticWorker::probe(config, modules / "local_semantic_worker.py",
            output / "capability-probe", cancelled);
        CAPTURE(capability.reason);
        REQUIRE(capability.status == GUI::LocalSemanticWorker::Status::Ready);
        const auto capability_response = Json::parse(capability.response_json);
        REQUIRE(capability_response.at("identity").at("packages") == probe.at("packages"));
        capability_report = {{"status", "READY"}, {"packages", capability_response.at("identity").at("packages")},
            {"identity_sha256", Cache::digest(capability_response.at("identity").dump())},
            {"request_directory", capability.request_directory.generic_string()}};
    }
    Semantic::Evidence evidence;
    const bool decoded = Semantic::decode(bytes, expected, binding, evidence, error);
    CAPTURE(error);
    REQUIRE(decoded);
    auto shapes = std::make_shared<PortraitShapeDetails>();
    shapes->locks = ShapeLockSet::from_evidence(evidence, evidence_hash);
    shapes->base_colors = colors.face_colors.size() == mesh.its.indices.size() ? colors.face_colors :
        beauty_source_face_colors(mesh.its, colors.vertex_colors);
    shapes->runtime_fingerprint = current_runtime;
    std::set<size_t> reserved;
    for (const auto& region : evidence.regions) if (ShapeLockSet::supported_label(region.label))
        reserved.insert(region.faces.begin(), region.faces.end());
    for (const auto& shape : evidence.shape_details) {
        reserved.insert(shape.accepted_faces.begin(), shape.accepted_faces.end());
        reserved.insert(shape.rejected_faces.begin(), shape.rejected_faces.end());
    }
    shapes->reserved_faces.assign(reserved.begin(), reserved.end());
    const std::map<std::string, size_t> counts{{"rb", 152}, {"lb", 169}, {"re", 56}, {"le", 19}, {"ulip", 259}, {"llip", 515}};
    Json lock_report = Json::array();
    std::map<std::string, std::set<std::string>> eye_sides;
    for (const auto& lock : shapes->locks.locks) {
        if (!fresh_evidence) REQUIRE(lock.locked_faces.size() == counts.at(lock.label));
        if (fresh_evidence && (lock.label == "re" || lock.label == "le")) {
            REQUIRE_FALSE(lock.nested_faces.empty());
            eye_sides[lock.subject_id].insert(lock.label);
        }
        lock_report.push_back({{"subject_id", lock.subject_id}, {"label", lock.label}, {"status", lock.status},
            {"face_count", lock.locked_faces.size()}, {"nested_face_count", lock.nested_faces.size()},
            {"view_support", lock.view_support}, {"reasons", lock.reasons}});
    }
    if (!fresh_evidence) REQUIRE(shapes->locks.locks.size() == counts.size());
    else {
        REQUIRE_FALSE(eye_sides.empty());
        for (const auto& sides : eye_sides) {
            REQUIRE(sides.second.count("re") == 1);
            REQUIRE(sides.second.count("le") == 1);
        }
    }
    auto source = std::make_shared<SC::MeshSnapshot>();
    source->mesh = mesh.its;
    source->vertex_colors = colors.vertex_colors;
    source->face_colors = colors.face_colors;
    source->geometry_id = geometry_hash;
    source->content_id = SC::content_fingerprint(*source);
    REQUIRE_FALSE(source->content_id.empty());
    boost::filesystem::create_directories(output);
    Cache::preserve_source(output, model, source_hash);
    Cache::preserve_source(boost::filesystem::path(data) / "cache", model, source_hash);
    const auto context_ref = Cache::save(*shapes, output);
    boost::filesystem::ofstream lock_output(output / "shape-locks.json");
    lock_output << shapes->locks.encode().dump();
    lock_output.close();
    const auto write_overrides = [&](const std::string& name, const SC::FaceColors& values) {
        const auto path = output / name;
        REQUIRE_FALSE(boost::filesystem::exists(path));
        boost::filesystem::ofstream stream(path, std::ios::binary);
        for (const auto& item : values) {
            REQUIRE(item.first < source->mesh.indices.size());
            const uint32_t face = uint32_t(item.first);
            stream.write(reinterpret_cast<const char*>(&face), sizeof(face));
            stream.write(reinterpret_cast<const char*>(item.second.data()), 3 * sizeof(float));
        }
        stream.close();
        REQUIRE(bool(stream));
        return name;
    };
    const auto write_children = [&](const std::string& name, const SC::SubfaceColors& values) {
        Json items = Json::array();
        for (const auto& item : values) items.push_back({{"face_id", item.face_id}, {"path", {{"depth", item.path.depth}, {"value", item.path.value}}},
            {"color", item.color}, {"confidence", item.confidence}});
        boost::filesystem::ofstream stream(output / name, std::ios::binary);
        stream << items.dump();
        stream.close();
        REQUIRE(bool(stream));
        return name;
    };
    Json variants = Json::array(), checks = Json::array(), palettes_report = Json::array();
    GUI::ModelSemanticColoring coordinator(runtime, std::filesystem::path((output / "native-cache").native()));
    for (const size_t count : {3, 4, 5, 6}) {
        const auto palette_path = palettes / ("colors-" + std::to_string(count)) / "material-tree.json";
        const auto tree = read_json(palette_path);
        REQUIRE(tree.at("palette").size() == count);
        std::vector<SC::Color> palette;
        for (const auto& entry : tree.at("palette")) palette.push_back(entry.at("rgb").get<SC::Color>());
        palettes_report.push_back({{"color_count", count}, {"sha256", model_artifact_sha256(palette_path)}, {"palette", tree.at("palette")}});
        REQUIRE(coordinator.request(source, palette, palette, {}, {}, std::filesystem::path(model.native()), shapes));
        std::unique_ptr<GUI::ModelSemanticColoring::Result> result;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(15);
        while (!result && std::chrono::steady_clock::now() < deadline) {
            result = coordinator.poll();
            if (!result && !coordinator.busy()) break;
            if (!result) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!result) coordinator.cancel();
        REQUIRE(result);
        CAPTURE(result->error, result->shape_error);
        REQUIRE(result->error.empty());
        REQUIRE(result->shape_error.empty());
        REQUIRE(result->shape_details);
        REQUIRE(result->shape_details->locks.encode() == shapes->locks.encode());
        REQUIRE(result->analysis);
        REQUIRE(result->verified_original_source);
        REQUIRE(result->analysis->geometry_id == geometry_hash);
        const auto& analysis = *result->analysis;
        std::set<size_t> protected_context(reserved);
        for (size_t face = 0; face < analysis.face_labels.size(); ++face)
            if (GUI::native_facial_detail(analysis.face_labels[face])) protected_context.insert(face);
        for (const auto& child : analysis.subface_labels)
            if (GUI::native_facial_detail(child.label)) protected_context.insert(child.face_id);
        std::map<size_t, SC::Color> native, merged;
        for (const auto& item : result->native_automatic) REQUIRE(native.emplace(item).second);
        for (const auto& item : result->automatic) REQUIRE(merged.emplace(item).second);
        for (const auto& item : merged) {
            if (!protected_context.count(item.first)) {
                REQUIRE(native.count(item.first) == 1);
                REQUIRE(item.second == native.at(item.first));
            }
            if (protected_context.count(item.first)) REQUIRE(shapes->locks.face_locked(item.first));
        }
        for (const auto& item : native) if (!protected_context.count(item.first)) REQUIRE(merged.count(item.first) == 1);
        using ChildKey = std::tuple<size_t, uint8_t, uint8_t>;
        std::map<ChildKey, SC::Color> native_children, merged_children;
        for (const auto& item : result->native_automatic_subfaces)
            REQUIRE(native_children.emplace(ChildKey{item.face_id, item.path.depth, item.path.value}, item.color).second);
        for (const auto& item : result->automatic_subfaces) {
            REQUIRE(protected_context.count(item.face_id) == 0);
            const ChildKey key{item.face_id, item.path.depth, item.path.value};
            REQUIRE(native_children.count(key) == 1);
            REQUIRE(item.color == native_children.at(key));
            REQUIRE(merged_children.emplace(key, item.color).second);
        }
        for (const auto& item : native_children)
            if (!protected_context.count(std::get<0>(item.first))) REQUIRE(merged_children.count(item.first) == 1);
        for (const auto& lock : shapes->locks.locks) for (const auto face : lock.locked_faces)
            if (lock.label == "imouth") REQUIRE(merged.count(face) == 0);
        const auto prefix = "colors-" + std::to_string(count);
        for (const auto& variant : std::vector<std::pair<std::string, SC::FaceColors>>{{"raw", {}},
                 {"target-native", result->native_automatic}, {"merged", result->automatic}}) {
            const auto file = write_overrides(prefix + "-" + variant.first + ".overrides.bin", variant.second);
            const auto& children = variant.first == "target-native" ? result->native_automatic_subfaces :
                variant.first == "merged" ? result->automatic_subfaces : SC::SubfaceColors{};
            const auto child_file = write_children(prefix + "-" + variant.first + ".subfaces.json", children);
            variants.push_back({{"color_count", count}, {"name", variant.first}, {"palette", palette},
                {"face_colors_path", file}, {"subface_colors_path", child_file}, {"format", "u32_f32x3_le"}});
        }
        checks.push_back({{"color_count", count}, {"native_override_count", result->native_automatic.size()},
            {"merged_override_count", result->automatic.size()}, {"native_subface_count", result->native_automatic_subfaces.size()},
            {"merged_subface_count", result->automatic_subfaces.size()}, {"cache_hit", result->cache_hit}, {"elapsed_ms", result->elapsed_ms},
            {"non_target_assignments_unchanged", true}, {"non_target_subface_assignments_unchanged", true},
            {"native_fine_outside_lock_suppressed", true}});
    }
    const auto original_metadata = model.parent_path() / "model.json";
    bool has_saved_accepted_reference = false;
    if (boost::filesystem::is_regular_file(original_metadata)) {
        const auto metadata = read_json(original_metadata);
        has_saved_accepted_reference = metadata.contains("beauty_workbench") && metadata.at("beauty_workbench").contains("puzzle");
    }
    // The accepted UI source is not a saved candidate unless an independently
    // source-bound draft exists. Missing acceptance is reported explicitly.
    const Json manifest{{"schema", "orca.portrait-shape-offline-export/v1"}, {"source_glb", model.generic_string()},
        {"source_sha256", source_hash}, {"geometry_id", geometry_hash}, {"face_count", mesh.its.indices.size()},
        {"evidence_root", frozen.generic_string()}, {"evidence_sha256", evidence_hash},
        {"evidence_runtime_sha256", expected.runtime_sha256}, {"evidence_policy_sha256", expected.policy_sha256},
        {"shape_runtime_fingerprint", current_runtime}, {"evidence_mode", fresh_evidence ? "CURRENT_RUNTIME_PINNED" : "FIXED_LEGACY_REPLAY"},
        {"capability_probe", capability_report},
        {"lock_count", shapes->locks.locks.size()}, {"lock_details", lock_report},
        {"shape_locks_file", "shape-locks.json"}, {"shape_context_ref", context_ref}, {"variants", variants}, {"palettes", palettes_report},
        {"accepted_reference_status", has_saved_accepted_reference ? "SAVED_DRAFT_REQUIRES_IDENTITY_VALIDATION" : "MISSING_SAVED_ACCEPTED_DRAFT"},
        {"accepted_source_baseline_visual", "USER_ACCEPTED_REGION_BOUNDARIES_ONLY"},
        {"accepted_appearance_status", "MISSING_SAVED_APPEARANCE"}, {"checks", checks}, {"local_recognition_rerun", false},
        {"native_offline_recognition", true}, {"paid_services_used", false}, {"material_write_authorized", false}, {"slot_count_changed", false}};
    REQUIRE(model_artifact_sha256(model) == source_hash);
    REQUIRE(model_artifact_sha256(frozen / "evidence.json") == evidence_hash);
    boost::filesystem::ofstream export_file(output / "export.json", std::ios::binary);
    export_file << manifest.dump(2);
    export_file.close();
    REQUIRE(bool(export_file));
}
