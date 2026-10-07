#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "slic3r/GUI/AI/Model/BeautyShapeLock.hpp"
#include "slic3r/GUI/AI/Model/BeautyRecognition.hpp"
#include "slic3r/GUI/AI/Model/LocalSemanticGeometry.hpp"
#include "../test_utils.hpp"
#include <boost/nowide/cstdlib.hpp>
#include <iterator>
#include <map>

using namespace Slic3r;
using namespace Slic3r::AI;
namespace Semantic = GUI::LocalSemanticEvidence;
namespace Geometry = GUI::LocalSemanticGeometry;
using Json = nlohmann::json;

TEST_CASE("Leaf shape locks round trip without changing their canonical source", "[BeautyShapeLock][BeautyLeafDomain]") {
    ShapeLockSet locks;
    locks.geometry_id = std::string(64,'a'); locks.source_sha256 = std::string(64,'b');
    locks.evidence_sha256 = std::string(64,'c'); locks.runtime_sha256 = std::string(64,'d');
    locks.policy_sha256 = std::string(64,'e'); locks.baseline_sha256 = std::string(64,'f');
    locks.boundary_policy_sha256 = std::string(64,'0'); locks.face_count = 200;
    locks.leaf_domain = BeautyLeafDomain{locks.geometry_id, 200, {{0,1,0},{0,1,1},{0,1,2},{0,1,3}}};
    ShapeLock eye;
    eye.subject_id = "person-a"; eye.label = "le"; eye.parent_label = "face";
    eye.status = "PROTECTED_SHAPE_UNCERTAIN"; eye.view_support = 2;
    eye.locked_faces = {0}; eye.nested_faces = {0};
    eye.locked_leaves = {{0,1,0},{0,1,1}}; eye.nested_leaves = {{0,1,1}};
    locks.locks.push_back(eye);
    ShapeLock brow = eye;
    brow.label = "lb"; brow.locked_leaves = {{0,1,2}}; brow.nested_leaves.clear(); brow.nested_faces.clear();
    locks.locks.push_back(brow);
    const auto document = locks.encode();
    REQUIRE(document.at("schema") == "orca.beauty-shape-lock/v2");
    const auto restored = ShapeLockSet::decode(document, locks.geometry_id, locks.source_sha256, 200);
    REQUIRE(restored.encode() == document);
    REQUIRE(restored.compatible(locks.geometry_id, locks.source_sha256, 200));
    REQUIRE_FALSE(restored.boundary_compatible(std::string(64,'a'), locks.boundary_policy_sha256, locks.leaf_domain->fingerprint()));
    REQUIRE_THROWS(ShapeLockSet::decode(document, locks.geometry_id, locks.source_sha256, 200, {}, {}, {}, std::string(64,'a')));
    locks.locks.front().nested_leaves = {{0,1,3}};
    REQUIRE_THROWS(locks.encode());
    locks.locks.front().nested_leaves.clear();
    locks.locks.front().nested_faces.clear();
    locks.baseline_sha256 = "invalid";
    REQUIRE_THROWS(locks.encode());
}

namespace {
Semantic::Evidence shape_evidence()
{
    Semantic::Evidence result;
    result.identity.geometry_id = std::string(64, 'a');
    result.identity.source_sha256 = std::string(64, 'b');
    result.identity.runtime_sha256 = std::string(64, 'c');
    result.identity.policy_sha256 = std::string(64, 'd');
    result.identity.face_count = 12;
    result.subjects = {"person-a"};
    PrintColorRegion parent;
    parent.subject_id = "person-a";
    parent.label = "face";
    for (size_t face = 0; face < 12; ++face) parent.faces.push_back(face);
    result.regions.push_back(parent);
    result.face_regions.assign(12, 0);
    Semantic::ShapeDetail eye;
    eye.subject_id = "person-a";
    eye.label = "re";
    eye.status = "VALID_SHAPE";
    eye.accepted_faces = {0, 1, 2};
    eye.nested_faces = {1};
    eye.view_support = 2;
    result.shape_details.push_back(eye);
    Semantic::ShapeDetail brow;
    brow.subject_id = "person-a";
    brow.label = "lb";
    brow.status = "PROTECTED_SHAPE_UNCERTAIN";
    brow.accepted_faces = {3, 4};
    brow.view_support = 3;
    brow.reasons = {"SHAPE_COMPONENT_DISCONNECTED"};
    result.shape_details.push_back(brow);
    return result;
}

ShapeLockSet shape_locks()
{
    return ShapeLockSet::from_evidence(shape_evidence(), std::string(64, 'e'));
}

Semantic::Evidence paired_eye_evidence()
{
    auto result = shape_evidence();
    result.regions.clear();
    result.shape_details.clear();
    for (const auto& side : std::vector<std::pair<std::string, size_t>>{{"re", 0}, {"le", 3}}) {
        PrintColorRegion parent;
        parent.subject_id = "person-a";
        parent.label = side.first;
        Semantic::ShapeDetail shape;
        shape.subject_id = parent.subject_id;
        shape.label = parent.label;
        shape.status = "PROTECTED_SHAPE_UNCERTAIN";
        shape.reasons = {"SHAPE_IRIS_FIT_RESIDUAL"};
        shape.view_support = 2;
        for (size_t face = side.second; face < side.second + 3; ++face) {
            parent.faces.push_back(face);
            result.face_regions[face] = int32_t(result.regions.size());
        }
        shape.accepted_faces = parent.faces;
        shape.nested_faces = {side.second + 1};
        result.regions.push_back(std::move(parent));
        result.shape_details.push_back(std::move(shape));
    }
    PrintColorRegion face_parent;
    face_parent.subject_id = "person-a";
    face_parent.label = "face";
    for (size_t face = 6; face < result.identity.face_count; ++face) {
        face_parent.faces.push_back(face);
        result.face_regions[face] = int32_t(result.regions.size());
    }
    result.regions.push_back(std::move(face_parent));
    return result;
}

ShapeLockSet decode(const Json& value, const ShapeLockSet& expected)
{
    return ShapeLockSet::decode(value, expected.geometry_id, expected.source_sha256,
        expected.face_count, expected.evidence_sha256, expected.runtime_sha256, expected.policy_sha256);
}

BeautyPuzzle single_piece(const BeautySurface& surface)
{
    BeautyPuzzle result;
    result.geometry_id = surface.geometry_id;
    result.face_piece.assign(surface.areas.size(), 1);
    result.next_id = 2;
    return result;
}

std::string read_bytes(const boost::filesystem::path& path)
{
    boost::filesystem::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read shape replay input.");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
}

TEST_CASE("Valid and risk marked shapes create separate bound locks", "[BeautyShapeLock]")
{
    const auto locks = shape_locks();
    REQUIRE(locks.locks.size() == 2);
    CHECK(locks.locks[0].label == "re");
    CHECK(locks.locks[0].nested_faces == std::vector<size_t>{1});
    CHECK(locks.locks[1].label == "lb");
    CHECK(locks.locks[1].status == "PROTECTED_SHAPE_UNCERTAIN");
    CHECK(locks.locks[1].reasons == std::vector<std::string>{"SHAPE_COMPONENT_DISCONNECTED"});
    CHECK(locks.face_locked(4));
    CHECK_FALSE(locks.face_locked(5));
    CHECK_FALSE(locks.face_locked(locks.face_count));
    const auto restored = decode(locks.encode(), locks);
    CHECK(restored.encode() == locks.encode());
    CHECK_THROWS(ShapeLockSet::decode(locks.encode(), locks.geometry_id, locks.source_sha256,
        locks.face_count, {}, {}, {}, std::string(64, 'a')));
    CHECK_THROWS(ShapeLockSet::decode(locks.encode(), locks.geometry_id, locks.source_sha256,
        locks.face_count, {}, {}, {}, {}, std::string(64, 'b')));
}

TEST_CASE("Hard conflicts never create locks even when a fitted shape claims validity", "[BeautyShapeLock]")
{
    auto evidence = shape_evidence();
    const std::string status = GENERATE("VALID_SHAPE", "PROTECTED_SHAPE_UNCERTAIN", "INVALID_SHAPE_CONFLICT");
    const std::string reason = GENERATE("CROSS_SUBJECT", "CROSS_EYE", "PARENT_UNBOUND", "SOURCE_MAPPING");
    for (auto& shape : evidence.shape_details) {
        shape.status = status;
        shape.reasons = {reason};
    }
    CHECK(ShapeLockSet::from_evidence(evidence, std::string(64, 'e')).empty());
}

TEST_CASE("Rejected owner conflict faces do not discard a separate accepted shape subset", "[BeautyShapeLock]")
{
    auto evidence = shape_evidence();
    const std::string reason = GENERATE("SHAPE_OWNER_CONFLICT", "SHAPE_CONFLICTING_COMPONENT");
    evidence.shape_details[0].status = "PROTECTED_SHAPE_UNCERTAIN";
    evidence.shape_details[0].reasons = {reason};
    evidence.shape_details[0].rejected_faces = {5, 6};
    const auto locks = ShapeLockSet::from_evidence(evidence, std::string(64, 'e'));
    REQUIRE(locks.locks.size() == 2);
    CHECK(locks.locks[0].locked_faces == std::vector<size_t>{0, 1, 2});
    CHECK_FALSE(locks.face_locked(5));
    CHECK_FALSE(locks.face_locked(6));
    CHECK(decode(locks.encode(), locks).locks[0].reasons == std::vector<std::string>{reason});
}

TEST_CASE("Empty or invalid shape proposals preserve the parent without fabricating a lock", "[BeautyShapeLock]")
{
    auto evidence = shape_evidence();
    const bool conflict = GENERATE(false, true);
    for (auto& shape : evidence.shape_details) {
        if (conflict) shape.status = "INVALID_SHAPE_CONFLICT";
        else shape.accepted_faces.clear();
    }
    const auto parent = evidence.face_regions;
    CHECK(ShapeLockSet::from_evidence(evidence, std::string(64, 'e')).empty());
    CHECK(evidence.face_regions == parent);
}

TEST_CASE("Shape locks reject ownership and face set corruption", "[BeautyShapeLock]")
{
    auto evidence = shape_evidence();
    const int change = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8, 9);
    auto& shape = evidence.shape_details[0];
    if (change == 0) shape.rejected_faces = {1};
    if (change == 1) shape.accepted_faces = {0, 12};
    if (change == 2) shape.accepted_faces = {0, 0};
    if (change == 3) shape.accepted_faces = {2, 0};
    if (change == 4) shape.subject_id = "person-b";
    if (change == 5) evidence.regions[0].label = "le";
    if (change == 6) evidence.face_regions[0] = -1;
    if (change == 7) shape.nested_faces = {3};
    if (change == 8) evidence.shape_details[1].accepted_faces = {2, 3};
    if (change == 9) shape.view_support = 1;
    CHECK_THROWS(ShapeLockSet::from_evidence(evidence, std::string(64, 'e')));
}

TEST_CASE("All supported eye brow and lip details bind only to their legal parent", "[BeautyShapeLock]")
{
    auto evidence = shape_evidence();
    evidence.shape_details.resize(1);
    const std::string label = GENERATE("re", "le", "lb", "rb", "ulip", "llip", "imouth", "lip-line-corner");
    evidence.shape_details[0].label = label;
    if (label != "re" && label != "le") evidence.shape_details[0].nested_faces.clear();
    const auto locks = ShapeLockSet::from_evidence(evidence, std::string(64, 'e'));
    REQUIRE(locks.locks.size() == 1);
    CHECK(locks.locks[0].label == label);
    evidence.regions[0].label = "cloth";
    CHECK_THROWS(ShapeLockSet::from_evidence(evidence, std::string(64, 'e')));
}

TEST_CASE("Nested iris faces are rejected outside a corresponding eye lock", "[BeautyShapeLock]")
{
    auto evidence = shape_evidence();
    evidence.shape_details.resize(1);
    const std::string label = GENERATE("lb", "rb", "ulip", "llip", "imouth", "lip-line-corner");
    evidence.shape_details[0].label = label;
    CHECK_THROWS(ShapeLockSet::from_evidence(evidence, std::string(64, 'e')));
    const auto locks = shape_locks();
    auto saved = locks.encode();
    saved["locks"][0]["label"] = label;
    CHECK_THROWS(decode(saved, locks));
}

TEST_CASE("Shape lock identity drift prevents restoration", "[BeautyShapeLock]")
{
    const auto locks = shape_locks();
    auto saved = locks.encode();
    const std::string field = GENERATE("geometry_id", "source_sha256", "evidence_sha256", "runtime_sha256", "policy_sha256");
    saved[field] = std::string(64, 'f');
    CHECK_THROWS(decode(saved, locks));
    auto changed = locks;
    if (field == "geometry_id") changed.geometry_id = std::string(64, 'f');
    if (field == "source_sha256") changed.source_sha256 = std::string(64, 'f');
    if (field == "evidence_sha256") changed.evidence_sha256 = std::string(64, 'f');
    if (field == "runtime_sha256") changed.runtime_sha256 = std::string(64, 'f');
    if (field == "policy_sha256") changed.policy_sha256 = std::string(64, 'f');
    CHECK_FALSE(changed.compatible(locks.geometry_id, locks.source_sha256, locks.face_count,
        locks.evidence_sha256, locks.runtime_sha256, locks.policy_sha256));
}

TEST_CASE("Old seven field shape locks restore without inventing risk reasons", "[BeautyShapeLock][Regression]")
{
    const auto locks = shape_locks();
    auto saved = locks.encode();
    for (auto& lock : saved["locks"]) lock.erase("reasons");
    const auto restored = decode(saved, locks);
    REQUIRE(restored.locks.size() == locks.locks.size());
    for (size_t index = 0; index < restored.locks.size(); ++index) {
        CHECK(restored.locks[index].locked_faces == locks.locks[index].locked_faces);
        CHECK(restored.locks[index].status == locks.locks[index].status);
        CHECK(restored.locks[index].reasons.empty());
    }
}

TEST_CASE("Restored shape sidecars reject invalid ownership and hard conflicts", "[BeautyShapeLock]")
{
    const auto locks = shape_locks();
    auto saved = locks.encode();
    const int change = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
    auto& record = saved["locks"][0];
    if (change == 0) record["status"] = "INVALID_SHAPE_CONFLICT";
    if (change == 1) record["parent_label"] = "le";
    if (change == 2) record["locked_faces"] = {0, 0};
    if (change == 3) record["nested_faces"] = {4};
    if (change == 4) record["reasons"] = {"CROSS_SUBJECT"};
    if (change == 5) saved["face_count"] = size_t(13);
    if (change == 6) saved["locks"].push_back(record);
    if (change == 7) record["locked_faces"] = Json::array();
    CHECK_THROWS(decode(saved, locks));
}

TEST_CASE("Lock isolation separates nested details from their parent and unlocked faces", "[BeautyShapeLock][BeautyPuzzle]")
{
    const auto surface = BeautySurface::build(its_make_cube(10, 10, 10), {});
    const auto locks = shape_locks();
    auto puzzle = single_piece(*surface);
    locks.isolate(puzzle, *surface);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    CHECK(puzzle.face_piece[0] != puzzle.face_piece[1]);
    CHECK(puzzle.face_piece[1] != puzzle.face_piece[2]);
    CHECK(puzzle.face_piece[0] != puzzle.face_piece[3]);
    CHECK(puzzle.face_piece[3] != puzzle.face_piece[5]);
    CHECK(locks.preserves(puzzle, puzzle));
    for (size_t face = 0; face < 5; ++face)
        for (size_t other = 5; other < puzzle.face_piece.size(); ++other)
            CHECK(puzzle.face_piece[face] != puzzle.face_piece[other]);
}

TEST_CASE("Paired eye locks isolate both nested irises from sclera and the other eye", "[BeautyShapeLock][BeautyPuzzle]")
{
    const auto surface = BeautySurface::build(its_make_cube(10, 10, 10), {});
    const auto locks = ShapeLockSet::from_evidence(paired_eye_evidence(), std::string(64, 'e'));
    REQUIRE(locks.locks.size() == 2);
    CHECK(locks.locks[0].parent_label == "re");
    CHECK(locks.locks[1].parent_label == "le");
    CHECK(locks.locks[0].nested_faces == std::vector<size_t>{1});
    CHECK(locks.locks[1].nested_faces == std::vector<size_t>{4});
    CHECK(decode(locks.encode(), locks).encode() == locks.encode());
    auto puzzle = single_piece(*surface);
    locks.isolate(puzzle, *surface);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    for (size_t right = 0; right < 3; ++right)
        for (size_t left = 3; left < 6; ++left)
            CHECK(puzzle.face_piece[right] != puzzle.face_piece[left]);
    for (const auto outer : {size_t(0), size_t(2)}) CHECK(puzzle.face_piece[outer] != puzzle.face_piece[1]);
    for (const auto outer : {size_t(3), size_t(5)}) CHECK(puzzle.face_piece[outer] != puzzle.face_piece[4]);
    for (size_t locked = 0; locked < 6; ++locked)
        for (size_t parent = 6; parent < 12; ++parent)
            CHECK(puzzle.face_piece[locked] != puzzle.face_piece[parent]);
    auto edited = puzzle;
    edited.colors[puzzle.face_piece[1]] = RGBA{0, 0, 0, 1};
    CHECK(locks.preserves(puzzle, edited));
    edited.face_piece[4] = puzzle.face_piece[1];
    CHECK_FALSE(locks.preserves(puzzle, edited));
}

TEST_CASE("Paired eye evidence rejects faces and nested irises crossing the same person's eye side", "[BeautyShapeLock]")
{
    auto evidence = paired_eye_evidence();
    const int change = GENERATE(0, 1, 2);
    if (change == 0) {
        evidence.shape_details[0].accepted_faces = {0, 1, 3};
        evidence.shape_details[1].accepted_faces = {4, 5};
    }
    if (change == 1) evidence.shape_details[0].nested_faces = {4};
    if (change == 2) evidence.regions[0].subject_id = "person-b";
    CHECK_THROWS(ShapeLockSet::from_evidence(evidence, std::string(64, 'e')));
}

TEST_CASE("Lock preservation allows piece renaming and color changes but blocks merge split and growth", "[BeautyShapeLock][BeautyPuzzle]")
{
    const auto surface = BeautySurface::build(its_make_cube(10, 10, 10), {});
    const auto locks = shape_locks();
    auto before = single_piece(*surface);
    locks.isolate(before, *surface);
    auto renamed = before;
    for (auto& piece : renamed.face_piece) piece += 100;
    CHECK(locks.preserves(before, renamed));
    auto painted = before;
    painted.colors[before.face_piece[1]] = RGBA{1, 0, 0, 1};
    CHECK(locks.preserves(before, painted));
    auto grown = before;
    grown.face_piece[5] = before.face_piece[0];
    CHECK_FALSE(locks.preserves(before, grown));
    auto merged = before;
    merged.face_piece[1] = before.face_piece[0];
    CHECK_FALSE(locks.preserves(before, merged));
    auto split = before;
    split.face_piece[4] = split.next_id++;
    CHECK_FALSE(locks.preserves(before, split));
    auto crossed = before;
    crossed.face_piece[3] = before.face_piece[0];
    CHECK_FALSE(locks.preserves(before, crossed));
    auto unlocked = before;
    unlocked.face_piece[6] = unlocked.next_id++;
    CHECK(locks.preserves(before, unlocked));
}

TEST_CASE("Shape sidecars are content addressed and restore only unchanged bytes", "[BeautyShapeLock]")
{
    ScopedTemporaryDir directory("shape-lock-test");
    const auto locks = shape_locks();
    const auto reference = write_shape_lock_sidecar(directory.path(), locks);
    CHECK(ShapeLockReference::safe_path(reference.path));
    CHECK(ShapeLockReference::decode(reference.encode()).sha256 == reference.sha256);
    const auto restored = read_shape_lock_sidecar(directory.path(), reference, locks.geometry_id, locks.source_sha256,
        locks.face_count, locks.evidence_sha256, locks.runtime_sha256, locks.policy_sha256);
    CHECK(restored.encode() == locks.encode());
    CHECK(write_shape_lock_sidecar(directory.path(), locks).sha256 == reference.sha256);
    boost::filesystem::ofstream tamper(directory.path() / reference.path, std::ios::binary | std::ios::app);
    tamper << ' ';
    tamper.close();
    CHECK_THROWS(read_shape_lock_sidecar(directory.path(), reference, locks.geometry_id, locks.source_sha256, locks.face_count));
    CHECK_THROWS(write_shape_lock_sidecar(directory.path(), locks));
}

TEST_CASE("Locked color matching changes complete lock pieces and preserves other source regions", "[BeautyShapeLock][BeautyWorkbench]")
{
    const auto surface = BeautySurface::build(its_make_cube(10, 10, 10), {});
    const auto locks = shape_locks();
    auto puzzle = single_piece(*surface);
    locks.isolate(puzzle, *surface);
    BeautyGuidance guidance;
    guidance.names = {"face", "re", "lb"};
    guidance.labels.assign(12, 0);
    for (size_t face = 0; face < 3; ++face) guidance.labels[face] = 1;
    for (size_t face = 3; face < 5; ++face) guidance.labels[face] = 2;
    std::vector<RGBA> source(12, RGBA{1, 1, 1, 1});
    for (size_t face = 0; face < 5; ++face) source[face] = RGBA{0, 0, 0, 1};
    const std::vector<PhysicalFilamentChannel> palette{{0, "#FFFFFF", "PLA", true}, {1, "#000000", "PLA", true}};
    const auto before = puzzle;
    beauty_match_feature_filaments(puzzle, *surface, guidance, source, palette, {}, &locks);
    CHECK(puzzle.face_piece == before.face_piece);
    for (size_t face = 0; face < 5; ++face) CHECK(puzzle.filament_slots.at(puzzle.face_piece[face]) == 1);
    for (size_t face = 5; face < 12; ++face) {
        CHECK_FALSE(puzzle.colors.count(puzzle.face_piece[face]));
        CHECK_FALSE(puzzle.filament_slots.count(puzzle.face_piece[face]));
    }
    puzzle.paint_filament(puzzle.face_piece[1], 0);
    const auto manual = puzzle;
    beauty_match_feature_filaments(puzzle, *surface, guidance, source, palette, {}, &locks);
    CHECK(puzzle.same_edit(manual));
}

TEST_CASE("Mixed locked and parent pieces never receive automatic locked color", "[BeautyShapeLock][BeautyWorkbench]")
{
    const auto surface = BeautySurface::build(its_make_cube(10, 10, 10), {});
    const auto locks = shape_locks();
    auto puzzle = single_piece(*surface);
    BeautyGuidance guidance;
    guidance.names = {"face"};
    guidance.labels.assign(12, 0);
    const auto before = puzzle;
    const std::vector<RGBA> source(12, RGBA{0, 0, 0, 1});
    beauty_match_feature_filaments(puzzle, *surface, guidance, source,
        {{0, "#FFFFFF", "PLA", true}, {1, "#000000", "PLA", true}}, {}, &locks);
    CHECK(puzzle.face_piece == before.face_piece);
    CHECK(puzzle.colors.empty());
    CHECK(puzzle.filament_slots.empty());
}

TEST_CASE("Mixed sclera and nested iris pieces refuse automatic color and retain saved manual paint", "[BeautyShapeLock][BeautyWorkbench]")
{
    const auto surface = BeautySurface::build(its_make_cube(10, 10, 10), {});
    const auto locks = ShapeLockSet::from_evidence(paired_eye_evidence(), std::string(64, 'e'));
    auto puzzle = single_piece(*surface);
    locks.isolate(puzzle, *surface);
    const size_t outer = GENERATE(size_t(0), size_t(3));
    const bool manual = GENERATE(false, true);
    const auto mixed = puzzle.face_piece[outer];
    puzzle.face_piece[outer + 1] = mixed;
    const std::vector<PhysicalFilamentChannel> palette {
        {0, "#FFFFFF", "PLA", true}, {1, "#000000", "PLA", true}, {2, "#FF00FF", "PLA", true}};
    puzzle.palette = palette;
    if (manual) puzzle.paint_filament(mixed, 2);
    const auto before = puzzle;
    BeautyGuidance guidance;
    guidance.names = {"face", "re", "le", "iris"};
    guidance.labels.assign(12, 0);
    for (size_t face = 0; face < 3; ++face) guidance.labels[face] = 1;
    for (size_t face = 3; face < 6; ++face) guidance.labels[face] = 2;
    guidance.labels[1] = guidance.labels[4] = 3;
    std::vector<RGBA> source(12, RGBA{1, 1, 1, 1});
    source[1] = source[4] = RGBA{0, 0, 0, 1};
    REQUIRE_NOTHROW(beauty_match_feature_filaments(puzzle, *surface, guidance, source, palette, {}, &locks));
    CHECK(puzzle.face_piece == before.face_piece);
    if (manual) {
        CHECK(puzzle.colors.at(mixed) == before.colors.at(mixed));
        CHECK(puzzle.filament_slots.at(mixed) == before.filament_slots.at(mixed));
    } else {
        CHECK(puzzle.colors.count(mixed) == 0);
        CHECK(puzzle.filament_slots.count(mixed) == 0);
    }
    const size_t other = outer == 0 ? 3 : 0;
    CHECK(puzzle.filament_slots.at(puzzle.face_piece[other]) == 0);
    CHECK(puzzle.filament_slots.at(puzzle.face_piece[other + 1]) == 1);
    for (size_t face = 6; face < 12; ++face) CHECK(puzzle.colors.count(puzzle.face_piece[face]) == 0);
}

TEST_CASE("Shape sidecar references reject traversal absolute paths and mismatched addresses", "[BeautyShapeLock]")
{
    const std::string hash(64, 'a');
    const std::string path = GENERATE("../shape-locks/a.json", "/shape-locks/a.json", "C:/shape-locks/a.json",
        "shape-locks/../a.json", "shape-locks\\a.json", "shape-locks//a.json", "shape-locks/a.json");
    CHECK_FALSE(ShapeLockReference::safe_path(path));
    CHECK_THROWS(ShapeLockReference::decode({{"schema", ShapeLockReference::schema}, {"path", path}, {"sha256", hash}}));
    CHECK_THROWS(ShapeLockReference::decode({{"schema", ShapeLockReference::schema},
        {"path", "shape-locks/" + hash + ".json"}, {"sha256", std::string(64, 'b')}}));
}

TEST_CASE("Fixed portrait evidence restores the accepted six detail boundaries without changing its source", "[.][ShapeLockReplay]")
{
    const auto env = [](const char* key) {
        const auto value = boost::nowide::getenv(key);
        return value ? std::string(value) : std::string{};
    };
    const boost::filesystem::path source_root(env("ORCA_SHAPE_REPLAY_ROOT"));
    const boost::filesystem::path output(env("ORCA_SHAPE_REPLAY_REPORT"));
    if (source_root.empty() || output.empty()) SKIP("Explicit source checkout and fresh report path are required.");
    REQUIRE_FALSE(boost::filesystem::exists(output));
    const auto replay = source_root / ".tmp/dev/data-shape/shape-replay-20261006-a";
    const auto cached = source_root / ".tmp/dev/data-shape-replay-20261006-b/beauty_semantic_cache/7c7d33fab335488e2c84ac1f13c8486e64fb8639d19e09e7143249dfb09458a0";
    const auto model = source_root / ".tmp/dev/data-shape/generated_models/99d36f6d-7534-49d7-8256-52c34644f7bf/model.glb";
    const std::string source_hash = "793c69491bc1039da75aca8eda4b5dd4307335fe0f02b84696562a86f722c066";
    const std::string geometry_hash = "ed727689bd1658ac0c59a5726214d851fdaad3945b4475bb53129b85bb3f7026";
    const std::string fixed_hash = "c34c2c2600f36fe3331007f6531cf3e193079c284754bb291593051ea451efd9";
    const std::string cached_hash = "a132e2057989f366d5ed34dd2e2ca2ab073c4ab5303e26ed68f74073aed70b6b";
    const std::string sidecar_hash = "d1bcb2f3f0affcf820011833a8eedc3a40b2740e598075c77334a2dd50fc9a74";
    REQUIRE(model_artifact_sha256(model) == source_hash);
    REQUIRE(model_artifact_sha256(replay / "evidence.json") == fixed_hash);
    REQUIRE(model_artifact_sha256(cached / "evidence.json") == cached_hash);
    Geometry::Packet native, rendered;
    std::string error;
    REQUIRE(Geometry::decode(read_bytes(replay / "native.bin"), source_hash, native, error));
    REQUIRE(Geometry::decode(read_bytes(cached / "rendered.bin"), source_hash, rendered, error));
    Semantic::VerifiedFaceBinding binding;
    REQUIRE(Semantic::prove_ordered_faces(source_hash, native.mesh, rendered.mesh, binding, error));
    REQUIRE(binding.geometry_id() == geometry_hash);
    REQUIRE(binding.face_count() == 976825);
    auto parse_evidence = [&](const boost::filesystem::path& path) {
        const auto bytes = read_bytes(path);
        const auto document = Json::parse(bytes);
        Semantic::ExpectedIdentity expected;
        expected.request_id = document.at("request_id");
        expected.source_sha256 = source_hash;
        expected.geometry_id = geometry_hash;
        expected.face_count = binding.face_count();
        expected.weights_sha256 = document.at("weights_sha256");
        expected.runtime_sha256 = document.at("runtime_sha256");
        expected.policy_sha256 = document.at("policy_sha256");
        Semantic::Evidence evidence;
        const auto success = Semantic::decode(bytes, expected, binding, evidence, error);
        CAPTURE(error);
        REQUIRE(success);
        return evidence;
    };
    const auto evidence = parse_evidence(replay / "evidence.json");
    const auto cached_evidence = parse_evidence(cached / "evidence.json");
    const auto locks = ShapeLockSet::from_evidence(evidence, fixed_hash);
    const auto cached_locks = ShapeLockSet::from_evidence(cached_evidence, cached_hash);
    const auto old_locks = read_shape_lock_sidecar(source_root / ".tmp/dev/data-shape-replay-20261006-b/generated_models",
        {"shape-locks/" + sidecar_hash + ".json", sidecar_hash}, geometry_hash, source_hash, binding.face_count(),
        cached_hash, cached_evidence.identity.runtime_sha256, cached_evidence.identity.policy_sha256);
    const std::map<std::string, size_t> expected_counts{{"rb", 152}, {"lb", 169}, {"re", 56}, {"le", 19}, {"ulip", 259}, {"llip", 515}};
    REQUIRE(locks.locks.size() == expected_counts.size());
    REQUIRE(cached_locks.locks.size() == expected_counts.size());
    REQUIRE(old_locks.locks.size() == expected_counts.size());
    Json details = Json::array();
    for (size_t index = 0; index < locks.locks.size(); ++index) {
        const auto& lock = locks.locks[index];
        REQUIRE(lock.locked_faces.size() == expected_counts.at(lock.label));
        REQUIRE(lock.locked_faces == cached_locks.locks[index].locked_faces);
        REQUIRE(lock.locked_faces == old_locks.locks[index].locked_faces);
        REQUIRE(lock.nested_faces == old_locks.locks[index].nested_faces);
        REQUIRE(lock.status == "PROTECTED_SHAPE_UNCERTAIN");
        details.push_back({{"label", lock.label}, {"face_count", lock.locked_faces.size()},
            {"status", lock.status}, {"view_support", lock.view_support}, {"reasons", lock.reasons}});
    }
    REQUIRE(model_artifact_sha256(model) == source_hash);
    REQUIRE(model_artifact_sha256(replay / "evidence.json") == fixed_hash);
    REQUIRE(model_artifact_sha256(cached / "evidence.json") == cached_hash);
    const Json report{{"schema", "orca.shape-lock-integration-replay/v1"}, {"status", "PASS"},
        {"source_sha256", source_hash}, {"geometry_id", geometry_hash}, {"face_count", binding.face_count()},
        {"fixed_evidence_sha256", fixed_hash}, {"cached_evidence_sha256", cached_hash}, {"frozen_sidecar_sha256", sidecar_hash},
        {"runtime_sha256", evidence.identity.runtime_sha256}, {"policy_sha256", evidence.identity.policy_sha256},
        {"lock_count", locks.locks.size()}, {"details", details}, {"accepted_face_sets_unchanged", true},
        {"source_changed", false}, {"material_tree_changed", false}, {"new_recognition_performed", false},
        {"visual_acceptance", "USER_ACCEPTED_SOURCE_BASELINE_MERGED_RESULT_PENDING"}};
    boost::filesystem::create_directories(output.parent_path());
    boost::filesystem::ofstream result(output, std::ios::binary);
    result << report.dump(2);
    result.close();
    REQUIRE(bool(result));
}
