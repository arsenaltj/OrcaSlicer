#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "slic3r/GUI/AI/ModelGeneration/PortraitColorBlockPostprocess.hpp"
#include "slic3r/GUI/AI/ModelGeneration/PortraitNativePartitionCache.hpp"
#include "slic3r/GUI/AI/ModelGeneration/PortraitPrintFlow.hpp"
#include "slic3r/GUI/AI/ModelGeneration/PortraitSaveLedger.hpp"
#include "slic3r/GUI/AI/ModelGeneration/PortraitRecognitionState.hpp"
#include "slic3r/GUI/AI/ModelGeneration/PortraitWorkbenchEntry.hpp"
#include "slic3r/GUI/AI/ModelGeneration/PortraitColorSaveRetry.hpp"
#include "slic3r/GUI/AIModelGenerationClient.hpp"
#include "test_utils.hpp"
#include <tuple>

namespace PP = Slic3r::GUI::PortraitPrint;
namespace PC = Slic3r::GUI::PortraitColorBlocks;
namespace NP = Slic3r::GUI::NativePortrait;
TEST_CASE("Portrait version publication removes incomplete copies and preserves existing versions", "[PortraitPrintFlow][PortraitSave][P0]")
{
    ScopedTemporaryFile source(".obj"), destination(".obj");
    const auto write = [](const auto& path, const std::string& bytes) {
        boost::filesystem::ofstream output(path, std::ios::binary);
        output << bytes; output.close();
        if (!output) throw std::runtime_error("fixture write failed");
    };
    const auto read = [](const auto& path) {
        boost::filesystem::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), {});
    };
    write(source.path(), "complete version");
    boost::filesystem::path partial;
    CHECK_THROWS(PP::publish_color_version(source.path(), destination.path(), [&](const auto&, const auto& temporary) {
        partial = temporary;
        write(temporary, "partial");
        throw std::runtime_error("injected version copy failure");
    }));
    CHECK_FALSE(boost::filesystem::exists(partial));
    CHECK_FALSE(boost::filesystem::exists(destination.path()));
    PP::publish_color_version(source.path(), destination.path());
    CHECK(read(destination.path()) == "complete version");
    write(source.path(), "replacement");
    CHECK_THROWS(PP::publish_color_version(source.path(), destination.path(), [&](const auto& from, const auto& temporary) {
        partial = temporary;
        boost::filesystem::copy_file(from, temporary);
    }));
    CHECK_FALSE(boost::filesystem::exists(partial));
    CHECK(read(destination.path()) == "complete version");
}

TEST_CASE("A portrait ledger failure restores both original records", "[PortraitPrintFlow][PortraitSave][P0]")
{
    const size_t fail_after = GENERATE(1u, 2u);
    const bool existing = GENERATE(false, true);
    ScopedTemporaryFile first(".json");
    ScopedTemporaryFile second(".json");
    const std::array<boost::filesystem::path, 2> paths {first.path(), second.path()};
    const auto write = [](const auto& path, const nlohmann::json& document) {
        boost::filesystem::ofstream output(path, std::ios::binary); output << document.dump();
        output.close(); if (!output) throw std::runtime_error("fixture write failed");
    };
    if (existing) for (const auto& path : paths) write(path, {{"old", true}});
    size_t committed = 0;
    CHECK_THROWS(PP::commit_save_ledger(paths, {{"new", true}}, [&](const auto& path, const auto& doc) {
        write(path, doc);
        if (++committed == fail_after) throw std::runtime_error("injected ledger commit failure");
    }));
    for (const auto& path : paths) {
        CHECK(boost::filesystem::exists(path) == existing);
        if (existing) {
            boost::filesystem::ifstream input(path);
            const nlohmann::json expected {{"old", true}};
            CHECK(nlohmann::json::parse(input) == expected);
        }
    }
}

TEST_CASE("A failed portrait save returns to editable color with its native partition", "[PortraitPrintFlow][PortraitSave][P0]")
{
    PP::State state;
    state.stage = PP::Stage::SaveColor; state.paused = false; state.authorized = true;
    state.partition_committed = true; state.basis_signature = std::string(64, '2');
    state.native_portrait_partition = {{"preserved", true}};
    state.colored_saved = true; state.region_semantics = {{"pending", true}};
    state.final_slot_binding = {{"pending", true}}; state.failed = true;
    state.rollback_color_save();
    CHECK(state.stage == PP::Stage::Color); CHECK(state.paused); CHECK_FALSE(state.authorized);
    CHECK_FALSE(state.failed); CHECK_FALSE(state.colored_saved);
    CHECK(state.region_semantics.is_null()); CHECK(state.final_slot_binding.is_null());
    CHECK(state.partition_committed); CHECK(state.native_portrait_partition.at("preserved") == true);
    CHECK(state.basis_signature == std::string(64, '2'));
}
TEST_CASE("Other suites and explicit palettes retain manual workbench processing", "[PortraitPrintFlow]")
{
    CHECK(PP::palette_allows_automatic_workbench(0, {}, false));
    CHECK(PP::palette_allows_automatic_workbench(2, "honor.people.six", false));
    CHECK(PP::palette_allows_automatic_workbench(1, "honor.people.six", false));
    CHECK_FALSE(PP::palette_allows_automatic_workbench(2, "honor.pets.six", false));
    CHECK_FALSE(PP::palette_allows_automatic_workbench(0, "other.suite", false));
    CHECK_FALSE(PP::palette_allows_automatic_workbench(0, {}, true));
    CHECK_FALSE(PP::palette_allows_automatic_workbench(1, {}, false));
    CHECK_FALSE(PP::palette_allows_automatic_workbench(2, {}, false));
}

TEST_CASE("Portrait workbench entry intent requires the exact published owner",
          "[PortraitPrintFlow][PortraitContext][P0]")
{
    const std::string source_sha256(64, '1');
    PP::WorkbenchEntryIntent intent {17, "D:/models/person.glb", source_sha256,
        "geometry", "appearance"};
    REQUIRE(intent.valid());
    CHECK(intent.matches(17, "D:/models/person.glb", source_sha256, "geometry", "appearance"));
    CHECK_FALSE(intent.matches(18, "D:/models/person.glb", source_sha256, "geometry", "appearance"));
    CHECK_FALSE(intent.matches(17, "D:/models/other.glb", source_sha256, "geometry", "appearance"));
    CHECK_FALSE(intent.matches(17, "D:/models/person.glb", std::string(64, '2'), "geometry", "appearance"));
    CHECK_FALSE(intent.matches(17, "D:/models/person.glb", source_sha256, "other", "appearance"));
    CHECK_FALSE(intent.matches(17, "D:/models/person.glb", source_sha256, "geometry", "other"));
    intent.clear();
    CHECK_FALSE(intent.valid());
}

TEST_CASE("Portrait partition requests automatic and portrait consumers together",
          "[PortraitPrintFlow][P0]")
{
    const auto consumers = PP::native_workbench_partition_consumers(
        PP::NativeWorkbenchPartitionConsumer::PortraitFlow,
        PP::NativeWorkbenchPartitionConsumer::AutomaticPalette);
    CHECK(PP::native_workbench_partition_requested(
        consumers, PP::NativeWorkbenchPartitionConsumer::PortraitFlow));
    CHECK(PP::native_workbench_partition_requested(
        consumers, PP::NativeWorkbenchPartitionConsumer::AutomaticPalette));
    CHECK_FALSE(PP::native_workbench_partition_requested(
        consumers, PP::NativeWorkbenchPartitionConsumer::HonorSuite));
}

namespace {
PP::State flow() {
    const Slic3r::GUI::AIModelGenerationClient::ImagePrintSettings print;
    PP::State s; s.id = "portrait-session"; s.stage = PP::Stage::Palette;
    s.authorized = true;
    s.frozen = {{"image", ""}, {"prompt", "portrait"}, {"style", "realistic"}, {"custom_style", ""},
        {"provider", "tripo"}, {"design_provider", "gpt"}, {"design_model", ""}, {"geometry_model", ""},
        {"geometry_quality", "standard"}, {"texture_quality", "standard"}, {"output_format", "glb"},
        {"profile", "quality"}, {"print_mode", ""}, {"shadow_color", "blue"}, {"face_limit", 1000000},
        {"width_mm", print.width_mm}, {"nozzle_mm", print.nozzle_mm}, {"line_width_mm", print.line_width_mm},
        {"minimum_feature_mm", print.minimum_feature_mm}, {"color_distance", print.color_distance}};
    return s;
}

std::string sha(char digit) { return std::string(64, digit); }

PP::State workbench_flow(PP::Stage stage = PP::Stage::Check)
{
    PP::State state;
    state.id = "workbench-session";
    state.entry = "completed_model";
    state.stage = stage;
    state.authorized = true;
    state.original_model_path = state.model_path = "original.glb";
    state.original_model_hash = state.model_hash = sha('1');
    state.geometry_id = "source-geometry";
    state.appearance_id = "source-appearance";
    return state;
}

nlohmann::json native_partition_reference()
{
    const nlohmann::json bounds {{"minimum", {0.0, 0.0, 0.0}},
        {"maximum", {100.0, 80.0, 60.0}}};
    return {{"schema", "orca.native-workbench-partition-reference/v2"},
        {"algorithm", NP::algorithm_version},
        {"partition_digest", sha('2')}, {"topology_hash", sha('3')},
        {"source_path", "original.glb"}, {"source_sha256", sha('1')},
        {"carrier_path", "partition.obj"}, {"carrier_sha256", sha('4')},
        {"appearance_path", "partition.source-appearance.obj"}, {"appearance_sha256", sha('5')},
        {"cache_path", "native-workbench-partitions/cache.json"}, {"cache_sha256", sha('6')},
        {"metadata_path", "partition.json"}, {"geometry_id", "partition-geometry"},
        {"appearance_id", "source-appearance"}, {"coordinate_units", "mm"},
        {"source_bounds_mm", bounds}, {"carrier_bounds_mm", bounds},
        {"cache_reference", {{"schema", "orca.native-workbench-partition-cache-reference/v1"},
            {"content_id", sha('6')}, {"partition_digest", sha('2')},
            {"native_mesh_sha256", sha('7')},
            {"geometry_id", "partition-geometry"}, {"face_count", 4}}}};
}

nlohmann::json native_partition_commit(const char* transaction_status = "committed")
{
    const auto reference = native_partition_reference();
    return {{"schema", "orca.native-workbench-partition-commit/v1"}, {"schema_version", 6},
        {"transaction_status", transaction_status}, {"portrait_print_id", "workbench-session"},
        {"source_model", "original.glb"}, {"source_sha256", sha('1')},
        {"model_path", "partition.obj"}, {"model_sha256", sha('4')},
        {"native_portrait_partition", reference},
        {"partition_build_report", {{"schema", "orca.native-workbench-partition-build-report/v1"},
            {"success", true}, {"total_ms", 25'000}, {"stages", nlohmann::json::object()}}}};
}

nlohmann::json semantic_binding()
{
    nlohmann::json regions = nlohmann::json::array();
    regions.push_back({{"region_id", 0}, {"semantic", 0}, {"confidence", 0.95},
        {"slot", 1}, {"semantic_bound", true}});
    return {{"schema", "orca.native-portrait-semantic-binding/v9"},
        {"partition_digest", sha('2')},
        {"regions", std::move(regions)},
        {"subface_slots", nlohmann::json::array()}};
}

nlohmann::json region_semantics()
{
    return {{"schema", "orca.native-workbench-region-semantics/v12"},
        {"partition_digest", sha('2')},
        {"face_count", 4}, {"protected_components", nlohmann::json::array()},
        {"regions", {{{"region_id", 0}, {"semantic", 0}, {"confidence", 0.95},
            {"semantic_bound", false}, {"role_support", std::vector<double>(12, 0.)}, {"observed_area", 1.}}}}};
}

nlohmann::json native_color_reduction()
{
    return {{"schema", "orca.native-workbench-color-reduction/v12"},
        {"algorithm", NP::algorithm_version},
        {"partition_digest", sha('2')}, {"requested_color_count", 5},
        {"actual_color_count", 1},
        {"region_color_groups", {0}}, {"group_cluster_ids", {0}},
        {"merge_order", nlohmann::json::array()}, {"mapping_digest", sha('7')}};
}

nlohmann::json final_slot_binding()
{
    return {{"schema", "orca.native-workbench-final-slot-binding/v12"},
        {"partition_digest", sha('2')}, {"reduction_digest", sha('7')},
        {"face_count", 4}, {"component_count", 0}, {"face_overrides", nlohmann::json::array()},
        {"regions", {{{"region_id", 0}, {"slot", 1}}}},
        {"subface_slots", nlohmann::json::array()}};
}

nlohmann::json post_color_check(const nlohmann::json& reference)
{
    return {{"schema", "orca.portrait-post-color-check/v1"}, {"status", "normal"},
        {"model_sha256", reference.at("carrier_sha256")},
        {"geometry_id", reference.at("geometry_id")},
        {"partition_digest", reference.at("partition_digest")},
        {"topology_hash", reference.at("topology_hash")},
        {"faces", 4}, {"vertices", 4}, {"boundary_edges", 0}, {"boundary_loops", 0},
        {"boundary_loop_sizes", nlohmann::json::array()}, {"nonmanifold_edges", 0},
        {"nonmanifold_vertices", 0}, {"degenerate_faces", 0}, {"duplicate_faces", 0},
        {"inconsistent_edges", 0}, {"isolated_components", 1}, {"small_faces", 0},
        {"self_intersections_checked", true}, {"self_intersections", 0},
        {"issue_faces", nlohmann::json::array()}};
}

Slic3r::GUI::WorkbenchCheckResult certified_check(
    const std::string& model_sha256,
    Slic3r::GUI::WorkbenchCheckPurpose purpose =
        Slic3r::GUI::WorkbenchCheckPurpose::PortraitPostColorReadOnly)
{
    Slic3r::GUI::WorkbenchCheckResult check;
    check.status = purpose == Slic3r::GUI::WorkbenchCheckPurpose::PortraitSourceRisk
        ? Slic3r::GUI::WorkbenchCheckStatus::Invalid
        : Slic3r::GUI::WorkbenchCheckStatus::Normal;
    check.purpose = purpose;
    check.model_sha256 = check.source_sha256 = model_sha256;
    check.summary = purpose == Slic3r::GUI::WorkbenchCheckPurpose::PortraitSourceRisk
        ? "source risk recorded" : "strict topology passed";
    check.before_faces = 4;
    check.before_vertices = 4;
    check.boundary_edges = purpose == Slic3r::GUI::WorkbenchCheckPurpose::PortraitSourceRisk ? 2 : 0;
    check.boundary_loops = purpose == Slic3r::GUI::WorkbenchCheckPurpose::PortraitSourceRisk ? 1 : 0;
    check.boundary_loop_sizes = purpose == Slic3r::GUI::WorkbenchCheckPurpose::PortraitSourceRisk
        ? std::vector<size_t>{2} : std::vector<size_t>{};
    check.isolated_components = 1;
    check.self_intersections_checked =
        purpose != Slic3r::GUI::WorkbenchCheckPurpose::PortraitSourceRisk;
    return check;
}
TEST_CASE("Explicitly failed portrait attempts can replace task identity transactionally", "[PortraitPrintFlow]")
{
    auto s = flow(); REQUIRE(s.reserve_design(s.id, "request"));
    REQUIRE(s.observe_job(s.id, "job", "failed", "conversion", 1));
    const auto before = PP::encode(s);
    CHECK_FALSE(s.observe_job(s.id, "foreign", "new", {}, 2, {"failed"}));
    CHECK(PP::encode(s) == before);
    CHECK_FALSE(s.observe_job(s.id, "job", "new", {}, 2));
    CHECK(PP::encode(s) == before);
    REQUIRE(s.observe_job(s.id, "job", "new", {}, 2, {"failed"}));
    CHECK(s.provider_task_id == "new"); CHECK(s.conversion_task_id.empty());
    const auto retried = PP::encode(s);
    CHECK_FALSE(s.observe_job(s.id, "job", "failed", {}, 1));
    CHECK_FALSE(s.observe_job(s.id, "job", "failed", {}, 2, {"new"}));
    CHECK(PP::encode(s) == retried);
    REQUIRE(s.observe_job(s.id, "job", "new", "conversion-2", 2));
    REQUIRE(s.observe_job(s.id, "job", "new", "conversion-3", 2, {"conversion-2"}));
    CHECK(s.generation_attempt == 2);
}

TEST_CASE("Portrait preparation and retries survive reopening without authorizing paid continuation", "[PortraitPrintFlow]")
{
    auto s = flow(); s.stage = PP::Stage::Import;
    s.model_retries = s.design_retries = 1; s.native_repair_completed = true;
    s.native_geometry_id = "repaired"; s.semantic_status = "partial_regions";
    s.notices = {"near color fallback"}; s.recovery_project = "recovery.3mf";
    PP::State reopened; REQUIRE(PP::decode(PP::encode(s), reopened));
    CHECK(reopened.round_base_enabled); CHECK(reopened.native_repair_completed);
    CHECK(reopened.native_geometry_id == "repaired"); CHECK(reopened.model_retries == 1);
    CHECK(reopened.semantic_status == "partial_regions"); CHECK(reopened.notices == s.notices);
    CHECK_FALSE(reopened.running());
    auto legacy = PP::encode(s); legacy["schema"] = "orca.portrait-print/v1";
    legacy.erase("native_repair_completed"); legacy.erase("model_retries");
    REQUIRE(PP::decode(legacy, reopened)); CHECK_FALSE(reopened.native_repair_completed);
    CHECK(reopened.model_retries == 0); CHECK_FALSE(reopened.authorized);
}
}

TEST_CASE("Portrait source risk certificates are content addressed and reusable only for the same identity",
    "[PortraitPrintFlow]")
{
    const auto check = certified_check(sha('1'),
        Slic3r::GUI::WorkbenchCheckPurpose::PortraitSourceRisk);
    nlohmann::json report { {"schema", "orca.portrait-source-check/v1"},
        {"status", "invalid"}, {"model_sha256", sha('1')} };
    const auto certificate = PP::encode_workbench_check_certificate(check,
        Slic3r::GUI::WorkbenchCheckPurpose::PortraitSourceRisk,
        "source-geometry", "source-appearance", {}, report);
    report["workbench_check_certificate"] = certificate;

    CHECK(PP::valid_workbench_check_certificate(certificate,
        Slic3r::GUI::WorkbenchCheckPurpose::PortraitSourceRisk,
        sha('1'), "source-geometry", "source-appearance", {}, report));
    CHECK_FALSE(PP::valid_workbench_check_certificate(certificate,
        Slic3r::GUI::WorkbenchCheckPurpose::PortraitSourceRisk,
        sha('9'), "source-geometry", "source-appearance", {}, report));
    auto tampered_report = report;
    tampered_report["status"] = "normal";
    CHECK_FALSE(PP::valid_workbench_check_certificate(certificate,
        Slic3r::GUI::WorkbenchCheckPurpose::PortraitSourceRisk,
        sha('1'), "source-geometry", "source-appearance", {}, tampered_report));

    auto wrong_version = certificate;
    wrong_version["checker_version"] = PP::workbench_check_certificate_version + 1;
    wrong_version.erase("certificate_sha256");
    wrong_version["certificate_sha256"] = Slic3r::AI::beauty_leaf_digest(wrong_version.dump());
    CHECK_FALSE(PP::valid_workbench_check_certificate(wrong_version,
        Slic3r::GUI::WorkbenchCheckPurpose::PortraitSourceRisk,
        sha('1'), "source-geometry", "source-appearance", {}, report));

    Slic3r::GUI::WorkbenchCheckResult restored;
    REQUIRE(PP::decode_workbench_check_certificate(certificate, restored));
    CHECK(restored.status == Slic3r::GUI::WorkbenchCheckStatus::Invalid);
    CHECK(restored.purpose == Slic3r::GUI::WorkbenchCheckPurpose::PortraitSourceRisk);
    CHECK(restored.boundary_edges == 2);
}

TEST_CASE("A shared automatic partition commit binds a late portrait consumer exactly once",
          "[PortraitPrintFlow][PortraitNativePartition][P0]")
{
    auto metadata = native_partition_commit();
    metadata["portrait_print_id"] = "";
    metadata["requesting_portrait_print_id"] = "";
    metadata["workbench_check_certificate"] = nlohmann::json::object();
    const nlohmann::json source_check {{"schema", "orca.portrait-source-check/v1"},
        {"model_sha256", sha('1')}, {"status", "invalid"}};

    REQUIRE(PP::bind_native_workbench_partition_commit_identity(
        metadata, "workbench-session", source_check));
    CHECK(metadata.at("portrait_print_id") == "workbench-session");
    CHECK(metadata.at("requesting_portrait_print_id") == "workbench-session");
    CHECK(metadata.at("source_check") == source_check);
    CHECK_FALSE(metadata.contains("workbench_check_certificate"));

    auto foreign = metadata;
    CHECK_FALSE(PP::bind_native_workbench_partition_commit_identity(
        foreign, "foreign-session", source_check));
    CHECK(foreign == metadata);
}

TEST_CASE("Portrait carrier check certificates bind metadata and survive optional v6 persistence",
    "[PortraitPrintFlow]")
{
    const auto reference = native_partition_reference();
    const auto check = certified_check(sha('4'));
    nlohmann::json metadata { {"schema_version", 4}, {"portrait_print_id", "workbench-session"},
        {"model_sha256", sha('4')}, {"native_portrait_partition", reference},
        {"post_color_check", post_color_check(reference)} };
    const auto certificate = PP::encode_workbench_check_certificate(check,
        Slic3r::GUI::WorkbenchCheckPurpose::PortraitPostColorReadOnly,
        "partition-geometry", "source-appearance", "partition.json", metadata,
        sha('2'), sha('3'));
    metadata["workbench_check_certificate"] = certificate;
    REQUIRE(PP::valid_workbench_check_certificate(certificate,
        Slic3r::GUI::WorkbenchCheckPurpose::PortraitPostColorReadOnly,
        sha('4'), "partition-geometry", "source-appearance", "partition.json",
        metadata, sha('2'), sha('3')));

    Slic3r::GUI::WorkbenchCheckResult restored;
    REQUIRE(PP::decode_workbench_check_certificate(certificate, restored));
    CHECK(restored.status == Slic3r::GUI::WorkbenchCheckStatus::Normal);
    CHECK(restored.boundary_edges == 0);
    CHECK(restored.self_intersections_checked);

    auto state = workbench_flow();
    state.workbench_check_certificate = certificate;
    PP::State reopened;
    REQUIRE(PP::decode(PP::encode(state), reopened));
    CHECK(reopened.workbench_check_certificate == certificate);

    auto legacy = PP::encode(state);
    legacy.erase("workbench_check_certificate");
    REQUIRE(PP::decode(legacy, reopened));
    CHECK(reopened.workbench_check_certificate.empty());

    auto changed_metadata = metadata;
    changed_metadata["model_sha256"] = sha('9');
    CHECK_FALSE(PP::valid_workbench_check_certificate(certificate,
        Slic3r::GUI::WorkbenchCheckPurpose::PortraitPostColorReadOnly,
        sha('4'), "partition-geometry", "source-appearance", "partition.json",
        changed_metadata, sha('2'), sha('3')));
    CHECK_FALSE(PP::valid_workbench_check_certificate(certificate,
        Slic3r::GUI::WorkbenchCheckPurpose::PortraitPostColorReadOnly,
        sha('4'), "partition-geometry", "source-appearance", "partition.json",
        metadata, sha('2'), sha('8')));
}
TEST_CASE("Portrait flow reserves each paid step once and rejects foreign callbacks", "[PortraitPrintFlow]")
{
    auto s = flow();
    CHECK_FALSE(s.reserve_design("old-session", "request"));
    REQUIRE(s.reserve_design(s.id, "design-request"));
    CHECK_FALSE(s.reserve_design(s.id, "duplicate"));
    CHECK_FALSE(s.observe_job("old-session", "old-job"));
    REQUIRE(s.observe_job(s.id, "actual-job"));
    CHECK_FALSE(s.observe_job(s.id, "other-job"));
    REQUIRE(s.advance(s.id, PP::Stage::Palette, PP::Stage::Design));
    CHECK_FALSE(s.reserve_model(s.id, "actual-job"));
    REQUIRE(s.advance(s.id, PP::Stage::Design, PP::Stage::Model));
    CHECK_FALSE(s.reserve_model(s.id, "other-job"));
    REQUIRE(s.reserve_model(s.id, "actual-job"));
    CHECK_FALSE(s.reserve_model(s.id, "actual-job"));
    CHECK(s.model_job_id == "actual-job");
}
TEST_CASE("Cancelled or paused portrait flow cannot submit or advance on late results", "[PortraitPrintFlow]")
{
    auto s = flow(); s.pause("User cancelled");
    CHECK_FALSE(s.reserve_design(s.id, "request"));
    CHECK_FALSE(s.advance(s.id, PP::Stage::Palette, PP::Stage::Design));
    CHECK_FALSE(PP::accepted(PP::Submission::Cancelled));
    CHECK_FALSE(PP::accepted(PP::Submission::Rejected));
    CHECK(PP::accepted(PP::Submission::Pending));
    CHECK(PP::accepted(PP::Submission::Submitted));
}
TEST_CASE("Late portrait phases retain submitted remote task identities across restart", "[PortraitPrintFlow]")
{
    auto s = flow();
    REQUIRE(s.reserve_design(s.id, "request"));
    REQUIRE(s.observe_job(s.id, "job", "remote-model", "remote-conversion"));
    REQUIRE(s.observe_job(s.id, "job"));
    CHECK(s.provider_task_id == "remote-model");
    CHECK(s.conversion_task_id == "remote-conversion");
    PP::State reopened;
    REQUIRE(PP::decode(PP::encode(s), reopened));
    REQUIRE(reopened.observe_job(reopened.id, "job", "remote-model"));
    CHECK(reopened.provider_task_id == "remote-model");
    CHECK(reopened.conversion_task_id == "remote-conversion");
    CHECK(reopened.paused);
    CHECK_FALSE(reopened.authorized);
    CHECK_FALSE(reopened.reserve_design(reopened.id, "retry"));
}
TEST_CASE("Conflicting portrait remote tasks cannot replace any saved identity", "[PortraitPrintFlow]")
{
    auto s = flow();
    REQUIRE(s.reserve_design(s.id, "request"));
    REQUIRE(s.observe_job(s.id, "job", {}, "remote-conversion"));
    const auto conversion_only = PP::encode(s);
    CHECK_FALSE(s.observe_job(s.id, "job", "unexpected-model", "other-conversion"));
    CHECK(PP::encode(s) == conversion_only);
    REQUIRE(s.observe_job(s.id, "job", "remote-model", "remote-conversion"));
    const auto identities = PP::encode(s);
    CHECK_FALSE(s.observe_job(s.id, "job", "other-model"));
    CHECK_FALSE(s.observe_job(s.id, "job", {}, "other-conversion"));
    CHECK_FALSE(s.observe_job(s.id, "other-job", "remote-model", "remote-conversion"));
    CHECK_FALSE(s.observe_job("old-session", "job", "remote-model", "remote-conversion"));
    CHECK(PP::encode(s) == identities);
}
TEST_CASE("Restart restores task reservations but revokes automatic fee authorization", "[PortraitPrintFlow]")
{
    auto s = flow(); REQUIRE(s.reserve_design(s.id, "request"));
    REQUIRE(s.observe_job(s.id, "actual-job"));
    PP::State restored;
    REQUIRE(PP::decode(PP::encode(s), restored));
    CHECK(restored.design_reserved);
    CHECK(restored.design_job_id == "actual-job");
    CHECK(restored.request_id == "request");
    CHECK(restored.restored);
    CHECK(restored.paused);
    CHECK_FALSE(restored.authorized);
    CHECK_FALSE(restored.reserve_model(restored.id, "actual-job"));
    CHECK_FALSE(restored.advance(restored.id, PP::Stage::Palette, PP::Stage::Design));
    auto corrupt = PP::encode(s); corrupt["model_reserved"] = true;
    CHECK_FALSE(PP::decode(corrupt, restored));
}
TEST_CASE("Repair only advances identity after accepted publication", "[PortraitPrintFlow]")
{
    auto s = flow(); s.stage = PP::Stage::Check; s.model_hash = "original";
    CHECK_FALSE(s.accept_repair("original", "candidate", false, true));
    CHECK_FALSE(s.accept_repair("other-source", "candidate", true, true));
    CHECK_FALSE(s.accept_repair("original", "candidate", true, false));
    CHECK(s.model_hash == "original");
    REQUIRE(s.accept_repair("original", "saved", true, true));
    CHECK(s.repair_saved);
    CHECK(s.model_hash == "saved");
    CHECK_FALSE(s.accept_repair("original", "late", true, true));
}

TEST_CASE("V6 source inspection records risk evidence without claiming a repair", "[PortraitPrintFlow][PortraitNativePartition]")
{
    auto state = workbench_flow();
    const nlohmann::json risk_report {{"status", "attention"}, {"open_edges", 12}};
    CHECK_FALSE(state.accept_source_check(sha('9'), risk_report));
    REQUIRE(state.accept_source_check(state.model_hash, risk_report));
    CHECK(state.source_checked);
    CHECK(state.source_check == risk_report);
    CHECK_FALSE(state.repair_checked);
    CHECK_FALSE(state.repair_saved);
    CHECK(state.repair_result_hash.empty());
    REQUIRE(state.advance(state.id, PP::Stage::Check, PP::Stage::Partition));

    PP::State reopened;
    REQUIRE(PP::decode(PP::encode(state), reopened));
    CHECK(reopened.source_checked);
    CHECK(reopened.source_check == risk_report);
    CHECK_FALSE(reopened.repair_checked);
    CHECK(reopened.paused);
}

TEST_CASE("V6 partition publication commits metadata before activating its carrier", "[PortraitPrintFlow][PortraitNativePartition]")
{
    auto state = workbench_flow();
    REQUIRE(state.accept_source_check(state.model_hash));
    REQUIRE(state.advance(state.id, PP::Stage::Check, PP::Stage::Partition));
    REQUIRE(state.begin_partition("partition.json"));
    CHECK(state.partition_pending);
    CHECK_FALSE(state.partition_committed);
    CHECK_FALSE(state.advance(state.id, PP::Stage::Partition, PP::Stage::Color));

    const auto reference = native_partition_reference();
    const nlohmann::json report {{"schema", "orca.native-workbench-partition-build-report/v1"},
        {"total_ms", 25000}};
    CHECK_FALSE(state.commit_partition(reference, sha('8'), sha('2'), report));
    REQUIRE(state.commit_partition(reference, sha('3'), sha('2'), report));
    CHECK_FALSE(state.partition_pending);
    CHECK(state.partition_committed);
    CHECK(state.partition_completed);
    CHECK_FALSE(state.advance(state.id, PP::Stage::Partition, PP::Stage::Color));
    CHECK_FALSE(state.activate_partition("other.obj", sha('4'), "partition-geometry", "source-appearance"));
    CHECK_FALSE(state.activate_partition("partition.obj", sha('7'), "partition-geometry", "source-appearance"));
    REQUIRE(state.activate_partition("partition.obj", sha('4'), "partition-geometry", "source-appearance"));
    REQUIRE(state.advance(state.id, PP::Stage::Partition, PP::Stage::Color));

    const auto identity = std::make_tuple(state.native_portrait_partition, state.partition_topology_hash,
        state.basis_signature, state.model_hash);
    state.partition_build_report["total_ms"] = 1;
    CHECK(std::make_tuple(state.native_portrait_partition, state.partition_topology_hash,
        state.basis_signature, state.model_hash) == identity);
}

TEST_CASE("Interrupted V6 partition checkpoints remain paused and cannot masquerade as committed", "[PortraitPrintFlow][PortraitNativePartition]")
{
    auto state = workbench_flow();
    REQUIRE(state.accept_source_check(state.model_hash));
    REQUIRE(state.advance(state.id, PP::Stage::Check, PP::Stage::Partition));
    REQUIRE(state.begin_partition("partition.json"));
    PP::State reopened;
    REQUIRE(PP::decode(PP::encode(state), reopened));
    CHECK(reopened.partition_pending);
    CHECK_FALSE(reopened.partition_committed);
    CHECK_FALSE(reopened.partition_completed);
    CHECK(reopened.paused);

    reopened.authorized = true;
    reopened.paused = false;
    const nlohmann::json failure {{"schema", "orca.native-workbench-partition-build-report/v1"},
        {"success", false}};
    REQUIRE(reopened.fail_partition(failure));
    CHECK_FALSE(reopened.partition_pending);
    CHECK_FALSE(reopened.partition_committed);
    CHECK(reopened.partition_build_report == failure);
    REQUIRE(reopened.begin_partition("retry.json"));
}

TEST_CASE("Native portrait commit publication is all or nothing at every checkpoint",
          "[PortraitPrintFlow][PortraitNativePartition][P0]")
{
    ScopedTemporaryDir directory("native-portrait-commit");
    const auto source = directory.path() / "source.obj";
    const auto model = directory.path() / "partition.obj";
    {
        boost::filesystem::ofstream output(source, std::ios::binary);
        output << "source";
    }
    {
        boost::filesystem::ofstream output(model, std::ios::binary);
        output << "partition";
    }
    const nlohmann::json metadata {
        {"schema", "orca.native-workbench-partition-commit/v1"},
        {"transaction_status", "committed"},
        {"model_sha256", Slic3r::AI::model_artifact_sha256(model)},
        {"source_sha256", Slic3r::AI::model_artifact_sha256(source)}};

    const auto preflight = directory.path() / "preflight.tmp";
    NP::write_validate_remove_native_portrait_partition_staging(preflight, metadata);
    CHECK_FALSE(boost::filesystem::exists(preflight));

    for (const auto interruption : {NP::PartitionCommitCheckpoint::BeforeAtomicPublish,
             NP::PartitionCommitCheckpoint::AfterAtomicPublish,
             NP::PartitionCommitCheckpoint::AfterReadbackValidation}) {
        DYNAMIC_SECTION("interruption " << int(interruption)) {
            const auto path = directory.path() /
                ("interrupted-" + std::to_string(int(interruption)) + ".json");
            CHECK_THROWS(NP::publish_native_portrait_partition_commit(
                path, model, source, metadata,
                [interruption](NP::PartitionCommitCheckpoint checkpoint) {
                    if (checkpoint == interruption) throw std::runtime_error("injected interruption");
                }));
            CHECK_FALSE(boost::filesystem::exists(path));
        }
    }

    const auto committed = directory.path() / "committed.json";
    CHECK(NP::publish_native_portrait_partition_commit(
        committed, model, source, metadata) == metadata);
    CHECK(boost::filesystem::is_regular_file(committed));
    auto rebound = metadata;
    rebound["portrait_print_id"] = "workbench-session";
    rebound["requesting_portrait_print_id"] = "workbench-session";
    for (const auto interruption : {NP::PartitionCommitCheckpoint::BeforeAtomicPublish,
             NP::PartitionCommitCheckpoint::AfterAtomicPublish,
             NP::PartitionCommitCheckpoint::AfterReadbackValidation}) {
        DYNAMIC_SECTION("replacement interruption " << int(interruption)) {
            const auto path = directory.path() /
                ("replacement-" + std::to_string(int(interruption)) + ".json");
            REQUIRE(NP::publish_native_portrait_partition_commit(
                path, model, source, metadata) == metadata);
            CHECK_THROWS(NP::replace_native_portrait_partition_commit(
                path, model, source, rebound,
                [interruption](NP::PartitionCommitCheckpoint checkpoint) {
                    if (checkpoint == interruption) throw std::runtime_error("injected interruption");
                }));
            boost::filesystem::ifstream restored_stream(path, std::ios::binary);
            REQUIRE(restored_stream);
            CHECK(nlohmann::json::parse(restored_stream) == metadata);
        }
    }
    CHECK(NP::replace_native_portrait_partition_commit(
        committed, model, source, rebound) == rebound);
    {
        boost::filesystem::ifstream rebound_stream(committed, std::ios::binary);
        REQUIRE(rebound_stream);
        CHECK(nlohmann::json::parse(rebound_stream) == rebound);
    }
    auto staging = metadata;
    staging["transaction_status"] = "staging";
    CHECK_THROWS(NP::publish_native_portrait_partition_commit(
        directory.path() / "staging.json", model, source, staging));
}

TEST_CASE("Only a fully committed V6 checkpoint can restore the native partition",
          "[PortraitPrintFlow][PortraitNativePartition][P0]")
{
    auto state = workbench_flow();
    REQUIRE(state.accept_source_check(state.model_hash));
    REQUIRE(state.advance(state.id, PP::Stage::Check, PP::Stage::Partition));
    REQUIRE(state.begin_partition("partition.json"));
    PP::State reopened;
    REQUIRE(PP::decode(PP::encode(state), reopened));
    const auto pending_identity = PP::encode(reopened);

    CHECK_FALSE(reopened.adopt_partition_commit(native_partition_commit("staging")));
    CHECK(PP::encode(reopened) == pending_identity);
    auto wrong_source = native_partition_commit();
    wrong_source["source_sha256"] = sha('9');
    CHECK_FALSE(reopened.adopt_partition_commit(wrong_source));
    CHECK(PP::encode(reopened) == pending_identity);

    REQUIRE(reopened.adopt_partition_commit(native_partition_commit()));
    CHECK_FALSE(reopened.partition_pending);
    CHECK(reopened.partition_committed);
    CHECK(reopened.partition_completed);
    CHECK(reopened.stage == PP::Stage::Color);
    CHECK(reopened.model_path == "partition.obj");
    CHECK(reopened.model_hash == sha('4'));
    CHECK(reopened.geometry_id == "partition-geometry");
    CHECK(reopened.appearance_id == "source-appearance");
    CHECK(reopened.basis_signature == sha('2'));
    CHECK(reopened.partition_topology_hash == sha('3'));
    CHECK_FALSE(reopened.native_repair_required);
    CHECK_FALSE(reopened.repair_checked);
}

TEST_CASE("A damaged V6 reference resets only from the immutable original model",
          "[PortraitPrintFlow][PortraitNativePartition][P0]")
{
    auto state = workbench_flow();
    REQUIRE(state.accept_source_check(state.model_hash));
    REQUIRE(state.advance(state.id, PP::Stage::Check, PP::Stage::Partition));
    REQUIRE(state.begin_partition("partition.json"));
    PP::State reopened;
    REQUIRE(PP::decode(PP::encode(state), reopened));
    const auto before = PP::encode(reopened);
    CHECK_FALSE(reopened.reset_partition_to_original(10, "partition.obj", sha('4')));
    CHECK(PP::encode(reopened) == before);
    REQUIRE(reopened.reset_partition_to_original(10, "original.glb", sha('1')));
    CHECK(reopened.stage == PP::Stage::Check);
    CHECK(reopened.record_version == 7);
    CHECK(reopened.model_path == "original.glb");
    CHECK(reopened.model_hash == sha('1'));
    CHECK_FALSE(reopened.partition_pending);
    CHECK_FALSE(reopened.partition_committed);
    CHECK(reopened.native_portrait_partition.is_null());
}
TEST_CASE("Print preparation requires a completed valid slice of the same revision", "[PortraitPrintFlow]")
{
    auto s = flow(); s.stage = PP::Stage::Slice;
    CHECK_FALSE(s.slice_complete(s.id, "current-revision", true, true, "model.gcode"));
    REQUIRE(s.bind_slice_input(s.id, "current-revision"));
    CHECK_FALSE(s.bind_slice_input(s.id, "late-revision"));
    CHECK_FALSE(s.slice_complete("old-session", "current-revision", true, true, "model.gcode"));
    CHECK_FALSE(s.slice_complete(s.id, "old-revision", true, true, "model.gcode"));
    CHECK_FALSE(s.slice_complete(s.id, "current-revision", false, true, "model.gcode"));
    CHECK_FALSE(s.slice_complete(s.id, "current-revision", true, false, "model.gcode"));
    CHECK_FALSE(s.slice_complete(s.id, "current-revision", true, true, ""));
    REQUIRE(s.slice_complete(s.id, "current-revision", true, true, "model.gcode"));
    CHECK(s.stage == PP::Stage::PrintReady);
    CHECK_FALSE(s.slice_complete(s.id, "current-revision", true, true, "late.gcode"));
    CHECK(s.gcode_path == "model.gcode");
}

TEST_CASE("A manually published repair remains paused until explicit continuation", "[PortraitPrintFlow]")
{
    auto s = flow(); s.stage = PP::Stage::Check; s.model_hash = "original";
    s.pause("Review candidate");
    REQUIRE(s.accept_repair("original", "published", true, true));
    CHECK(s.paused);
    CHECK_FALSE(s.advance(s.id, PP::Stage::Check, PP::Stage::Color));
    PP::State reopened;
    REQUIRE(PP::decode(PP::encode(s), reopened));
    CHECK(reopened.repair_saved);
    CHECK(reopened.model_hash == "published");
    CHECK_FALSE(reopened.running());
    reopened.paused = false; reopened.authorized = true;
    CHECK(reopened.advance(s.id, PP::Stage::Check, PP::Stage::Color));
}

TEST_CASE("Durable portrait records reject corrupt counts identities and paths", "[PortraitPrintFlow]")
{
    auto s = flow(); s.authorization_id = "confirmed-fees";
    s.final_count = 4; s.final_palette = {"#000000", "#ffffff", "#af6e5e", "#ecc3b2"};
    PP::State reopened;
    auto doc = PP::encode(s);
    REQUIRE(PP::decode(doc, reopened));
    CHECK(reopened.authorization_id == "confirmed-fees");
    CHECK_FALSE(reopened.authorized);
    doc["final_palette"].push_back("#a7a5a4");
    CHECK_FALSE(PP::decode(doc, reopened));
    doc = PP::encode(s); doc["id"] = "../another-record";
    CHECK_FALSE(PP::decode(doc, reopened));
}

TEST_CASE("A failed portrait retains its diagnostic after restart and cannot continue automatically", "[PortraitPrintFlow]")
{
    auto s = flow();
    REQUIRE(s.reserve_design(s.id, "request"));
    REQUIRE(s.observe_job(s.id, "real-task"));
    s.pause("Palette service is not configured", true);
    PP::State reopened;
    REQUIRE(PP::decode(PP::encode(s), reopened));
    CHECK(reopened.failed);
    CHECK(reopened.reason == s.reason);
    CHECK(reopened.design_job_id == s.design_job_id);
    CHECK_FALSE(reopened.running());
    CHECK_FALSE(reopened.reserve_design(s.id, "retry"));
}

TEST_CASE("Portrait recovery accepts native string distance algorithms and rejects invalid persisted types", "[PortraitPrintFlow]")
{
    auto s = flow();
    PP::State reopened;
    for (const auto* algorithm : {"ciede2000", "delta_e76"}) {
        s.frozen["color_distance"] = algorithm;
        REQUIRE(PP::decode(PP::encode(s), reopened));
        CHECK(reopened.frozen == s.frozen);
        CHECK_FALSE(reopened.authorized);
    }
    s.frozen["color_distance"] = 0.0;
    CHECK_FALSE(PP::decode(PP::encode(s), reopened));
    s.frozen["color_distance"] = "unknown";
    CHECK_FALSE(PP::decode(PP::encode(s), reopened));
}

TEST_CASE("Unavailable portrait evidence is distinct from a completed negative detection", "[PortraitPrintFlow]")
{
    CHECK(std::string(PP::recognition_status(false, false, false)) == "unavailable");
    CHECK(std::string(PP::recognition_status(true, false, false)) == "unavailable");
    CHECK(std::string(PP::recognition_status(false, true, false)) == "no_person");
    CHECK(std::string(PP::recognition_status(true, true, false)) == "person_without_reliable_bindings");
    CHECK(std::string(PP::recognition_status(true, true, true)) == "partial_regions_applied");
}

TEST_CASE("Completed model flows never reserve generation and survive restart without authorization", "[PortraitPrintFlow]")
{
    auto s = workbench_flow(PP::Stage::Project);
    CHECK_FALSE(s.reserve_design(s.id, "request"));
    CHECK_FALSE(s.reserve_model(s.id, "job"));
    CHECK_FALSE(s.advance(s.id, PP::Stage::Project, PP::Stage::Palette));
    REQUIRE(s.advance(s.id, PP::Stage::Project, PP::Stage::Check));
    CHECK_FALSE(s.advance("late-session", PP::Stage::Check, PP::Stage::Color));
    CHECK_FALSE(s.advance(s.id, PP::Stage::Check, PP::Stage::Color));
    CHECK_FALSE(s.advance(s.id, PP::Stage::Check, PP::Stage::Partition));
    REQUIRE(s.accept_source_check(s.model_hash));
    REQUIRE(s.advance(s.id, PP::Stage::Check, PP::Stage::Partition));
    CHECK_FALSE(s.advance(s.id, PP::Stage::Partition, PP::Stage::Color));
    REQUIRE(s.begin_partition("partition.json"));
    const auto reference = native_partition_reference();
    REQUIRE(s.commit_partition(reference, sha('3'), sha('2'), nlohmann::json::object()));
    REQUIRE(s.activate_partition("partition.obj", sha('4'), "partition-geometry", "source-appearance"));
    s.boundary_algorithm_version = PC::portrait_postprocess_algorithm_version;
    REQUIRE(s.advance(s.id, PP::Stage::Partition, PP::Stage::Color));
    CHECK_FALSE(s.advance(s.id, PP::Stage::Check, PP::Stage::Color));
    auto doc = PP::encode(s);
    REQUIRE(doc.at("schema") == "orca.portrait-print/v7");
    PP::State reopened;
    REQUIRE(PP::decode(doc, reopened));
    CHECK(reopened.from_workbench()); CHECK(reopened.paused);
    CHECK_FALSE(reopened.authorized); CHECK_FALSE(reopened.running());
    CHECK(reopened.geometry_id == s.geometry_id);
    CHECK(reopened.source_checked); CHECK_FALSE(reopened.repair_checked);
    CHECK(reopened.partition_completed); CHECK(reopened.partition_committed);
    CHECK(reopened.boundary_algorithm_version == PC::portrait_postprocess_algorithm_version);
    CHECK_FALSE(reopened.reserve_design(reopened.id, "paid"));
    doc["stage"] = int(PP::Stage::Model);
    CHECK_FALSE(PP::decode(doc, reopened));
}

TEST_CASE("Workbench processing keeps repair partition base and import in strict order", "[PortraitPrintFlow]")
{
    auto s = workbench_flow();
    CHECK_FALSE(s.accept_source_check(sha('9')));
    REQUIRE(s.accept_source_check(s.model_hash));
    CHECK(s.native_repair_required);
    REQUIRE(s.advance(s.id, PP::Stage::Check, PP::Stage::Partition));
    REQUIRE(s.begin_partition("partition.json"));
    const auto reference = native_partition_reference();
    REQUIRE(s.commit_partition(reference, sha('3'), sha('2'), nlohmann::json::object()));
    REQUIRE(s.activate_partition("partition.obj", sha('4'), "partition-geometry", "source-appearance"));
    const std::array<PP::Stage,9> stages{PP::Stage::Partition,PP::Stage::Color,PP::Stage::SaveColor,
        PP::Stage::Base,PP::Stage::Filaments,PP::Stage::Import,PP::Stage::Recommend,PP::Stage::Slice,PP::Stage::PrintReady};
    for (size_t i=0;i+1<stages.size();++i) {
        if (stages[i] == PP::Stage::SaveColor) {
            auto colored = reference;
            colored["carrier_path"] = "colored.obj";
            colored["metadata_path"] = "colored.json";
            REQUIRE(s.commit_colored_carrier(colored, region_semantics(), native_color_reduction(),
                final_slot_binding(), post_color_check(colored), "colored.obj", sha('4'),
                "partition-geometry", "source-appearance"));
        }
        CHECK_FALSE(s.advance("foreign-flow", stages[i], stages[i+1]));
        if (i+2<stages.size()) CHECK_FALSE(s.advance(s.id,stages[i],stages[i+2]));
        REQUIRE(s.advance(s.id,stages[i],stages[i+1]));
        CHECK_FALSE(s.advance(s.id,stages[i],stages[i+1]));
    }
}

TEST_CASE("Version six through eight partitions restart as v9 without reusing downstream results", "[PortraitPrintFlow][PortraitNativePartition]")
{
    for (const uint32_t legacy_version : {uint32_t(6), uint32_t(7),
            PC::legacy_color_region_algorithm_version, PC::portrait_postprocess_algorithm_version}) {
        DYNAMIC_SECTION("legacy version " << legacy_version) {
            PP::State s;
            s.record_version = 4;
            s.id = "restored-flow"; s.entry = "completed_model"; s.stage = PP::Stage::Slice;
            s.paused = s.restored = s.repair_checked = s.repair_saved = true;
            s.boundary_algorithm_version = legacy_version;
            s.partition_completed = s.colored_saved = s.semantic_applied = true;
            s.model_path = "old-painted.obj"; s.model_hash = "painted";
            s.geometry_id = "repair-geometry"; s.appearance_id = "original-appearance";
            s.original_model_path = "original.glb"; s.original_model_hash = "original";
            s.repair_result_hash = "repair"; s.repair_source_hash = "original";
            s.basis_signature = "old-basis"; s.region_binding_digest = "old-regions";
            s.slice_started = s.native_repair_completed = true;
            s.slice_revision = s.project_revision = "old-project"; s.gcode_path = "old.gcode";
            s.preview_result_id = "old-preview";
            const auto old_asset = s.color_asset_id();
            const auto before_failed_rebuild = PP::encode(s);
            REQUIRE(s.partition_needs_rebuild(PC::portrait_postprocess_algorithm_version));
            CHECK_FALSE(s.rebuild_partition(PC::portrait_postprocess_algorithm_version,
                "original.glb", "wrong-hash"));
            CHECK(PP::encode(s) == before_failed_rebuild);
            REQUIRE(s.rebuild_partition(PC::portrait_postprocess_algorithm_version,
                "original.glb", "original"));
            CHECK(s.stage == PP::Stage::Check);
            CHECK(s.record_version == 7);
            CHECK(s.model_path == "original.glb"); CHECK(s.model_hash == "original");
            CHECK_FALSE(s.repair_saved); CHECK_FALSE(s.repair_checked); CHECK(s.native_repair_required);
            CHECK_FALSE(s.partition_completed); CHECK_FALSE(s.colored_saved); CHECK_FALSE(s.semantic_applied);
            CHECK_FALSE(s.slice_started); CHECK_FALSE(s.native_repair_completed);
            CHECK(s.gcode_path.empty()); CHECK(s.slice_revision.empty()); CHECK(s.preview_result_id.empty());
            CHECK(s.paused); CHECK_FALSE(s.authorized);
            CHECK(s.original_model_path == "original.glb"); CHECK(s.repair_source_hash.empty());
            CHECK(s.color_asset_id() != old_asset);
            CHECK(s.boundary_algorithm_version == PC::portrait_postprocess_algorithm_version);
            CHECK_FALSE(s.partition_needs_rebuild(PC::portrait_postprocess_algorithm_version));
            CHECK_FALSE(s.rebuild_partition(PC::portrait_postprocess_algorithm_version,
                "original.glb", "original"));
            PP::State reopened;
            REQUIRE(PP::decode(PP::encode(s), reopened));
            CHECK(reopened.model_path == "original.glb"); CHECK_FALSE(reopened.repair_checked);
            CHECK(reopened.geometry_id.empty());
            CHECK(reopened.appearance_id.empty());
            CHECK_FALSE(reopened.partition_completed); CHECK_FALSE(reopened.running());
        }
    }
}

TEST_CASE("Legacy workbench records retain their assets without trusting import-time repair", "[PortraitPrintFlow]")
{
    PP::State s;
    s.id="legacy";s.entry="completed_model";s.stage=PP::Stage::SaveColor;
    s.geometry_id="geometry";s.appearance_id="appearance";
    s.original_model_path=s.model_path="portrait.glb";s.original_model_hash=s.model_hash="sha";
    auto doc=PP::encode(s);doc["schema"]="orca.portrait-print/v3";
    doc["repair_checked"]=true;doc["partition_completed"]=true;
    PP::State reopened;REQUIRE(PP::decode(doc,reopened));
    CHECK(reopened.record_version==3);CHECK_FALSE(reopened.repair_checked);CHECK_FALSE(reopened.partition_completed);
    CHECK(PP::encode(reopened).at("schema")=="orca.portrait-print/v3");
    CHECK(reopened.paused);CHECK_FALSE(reopened.running());CHECK(reopened.model_path==s.model_path);
    const auto legacy_asset=reopened.color_asset_id();reopened.record_version=4;
    CHECK(reopened.color_asset_id()!=legacy_asset);
}

TEST_CASE("Workbench flows retain model ownership after completion and restart", "[PortraitPrintFlow]")
{
    PP::State s;
    s.record_version = 4;
    s.id = "workbench-session"; s.entry = "completed_model";
    s.geometry_id = "geometry"; s.appearance_id = "appearance";
    s.original_model_path = "original.glb"; s.model_path = "colored.glb";
    s.original_model_hash = s.model_hash = "model-sha";
    for (const auto stage : {PP::Stage::Check, PP::Stage::SaveColor, PP::Stage::PrintReady}) {
        DYNAMIC_SECTION("Stage " << int(stage)) {
            s.stage = stage;
            CHECK(s.owns_workbench_model("original.glb", "geometry", "appearance"));
            CHECK(s.owns_workbench_model("colored.glb", "geometry", "appearance"));
            PP::State reopened;
            REQUIRE(PP::decode(PP::encode(s), reopened));
            CHECK(reopened.owns_workbench_model("colored.glb", "geometry", "appearance"));
            CHECK_FALSE(reopened.owns_workbench_model("another.glb", "geometry", "appearance"));
            CHECK_FALSE(reopened.owns_workbench_model("colored.glb", "changed", "appearance"));
            CHECK_FALSE(reopened.owns_workbench_model("colored.glb", "geometry", "changed"));
            CHECK_FALSE(reopened.owns_workbench_model("", "geometry", "appearance"));
            reopened.entry = "generation";
            CHECK_FALSE(reopened.owns_workbench_model("colored.glb", "geometry", "appearance"));
            reopened.entry = "completed_model"; reopened.id.clear();
            CHECK_FALSE(reopened.owns_workbench_model("colored.glb", "geometry", "appearance"));
        }
    }
}

TEST_CASE("V6 workflow ownership requires path file geometry and appearance identity", "[PortraitPrintFlow][PortraitContext]")
{
    auto state = workbench_flow();
    CHECK(state.owns_display_context("original.glb", sha('1'), "source-geometry", "source-appearance"));
    CHECK_FALSE(state.owns_display_context("other.glb", sha('1'), "source-geometry", "source-appearance"));
    CHECK_FALSE(state.owns_display_context("original.glb", sha('2'), "source-geometry", "source-appearance"));
    CHECK_FALSE(state.owns_display_context("original.glb", sha('1'), "other-geometry", "source-appearance"));
    CHECK_FALSE(state.owns_display_context("original.glb", sha('1'), "source-geometry", "other-appearance"));
    state.entry = "generation";
    CHECK_FALSE(state.owns_display_context("original.glb", sha('1'), "source-geometry", "source-appearance"));
}

TEST_CASE("SaveColor candidates require the exact flow asset partition and model identities",
          "[PortraitPrintFlow][PortraitContext][P0]")
{
    auto state = workbench_flow(PP::Stage::SaveColor);
    const auto reference = native_partition_reference();
    state.source_checked = true;
    state.source_check_hash = sha('1');
    state.partition_completed = state.partition_committed = true;
    state.native_portrait_partition = reference;
    state.partition_topology_hash = sha('3');
    state.basis_signature = sha('2');
    state.model_path = "partition.obj";
    state.model_hash = sha('4');
    state.geometry_id = "partition-geometry";

    PP::ColorCandidateIdentity candidate {state.id, state.color_asset_id(), "colored.obj",
        sha('4'), sha('8'), "partition-geometry", "source-appearance", sha('2'), sha('3')};
    REQUIRE(candidate.valid());
    CHECK(candidate.owns(state, state.color_asset_id(), "colored.obj", sha('4'), sha('8'),
        "partition-geometry", "source-appearance"));
    CHECK_FALSE(candidate.owns(state, "foreign-asset", "colored.obj", sha('4'), sha('8'),
        "partition-geometry", "source-appearance"));
    CHECK_FALSE(candidate.owns(state, state.color_asset_id(), "other.obj", sha('4'), sha('8'),
        "partition-geometry", "source-appearance"));
    CHECK_FALSE(candidate.owns(state, state.color_asset_id(), "colored.obj", sha('4'), sha('9'),
        "partition-geometry", "source-appearance"));
    CHECK_FALSE(candidate.owns(state, state.color_asset_id(), "colored.obj", sha('4'), sha('8'),
        "other-geometry", "source-appearance"));
    CHECK_FALSE(candidate.owns(state, state.color_asset_id(), "colored.obj", sha('4'), sha('8'),
        "partition-geometry", "other-appearance"));
    state.basis_signature = sha('9');
    CHECK_FALSE(candidate.owns(state, state.color_asset_id(), "colored.obj", sha('4'), sha('8'),
        "partition-geometry", "source-appearance"));
}

TEST_CASE("Paused v6 portrait sources adopt only their canonical history alias",
          "[PortraitPrintFlow][PortraitContext][P0]")
{
    constexpr const char* asset = "b8f895ae-ad58-4d07-af82-e96272de94b6";
    const std::string download = std::string("D:/models/downloads/orcaslicer-ai-") + asset + ".glb";
    const std::string history = std::string("D:/models/") + asset + "/model.glb";
    const auto paused_source = [&] {
        auto state = workbench_flow(PP::Stage::Partition);
        state.original_model_path = state.model_path = download;
        state.paused = true;
        state.authorized = false;
        state.restored = true;
        state.frozen = {{"model_path", download}};
        return state;
    };

    CHECK(PP::portrait_source_asset_id_from_path(download) == asset);
    CHECK(PP::portrait_source_asset_id_from_path(history) == asset);
    auto state = paused_source();
    CHECK_FALSE(state.owns_display_context(history, sha('1'), "source-geometry", "source-appearance"));
    REQUIRE(state.adopt_source_alias(history, sha('1'), "source-geometry", "source-appearance"));
    CHECK(state.original_model_path == history);
    CHECK(state.model_path == history);
    CHECK(state.frozen.at("model_path") == history);
    CHECK(state.frozen.at("asset_id") == asset);
    CHECK(state.owns_display_context(history, sha('1'), "source-geometry", "source-appearance"));
    CHECK(state.paused);
    CHECK_FALSE(state.running());

    auto wrong_asset = paused_source();
    CHECK_FALSE(wrong_asset.adopt_source_alias("D:/models/other/model.glb", sha('1'),
        "source-geometry", "source-appearance", "other"));
    auto wrong_hash = paused_source();
    CHECK_FALSE(wrong_hash.adopt_source_alias(history, sha('2'),
        "source-geometry", "source-appearance"));
    auto wrong_geometry = paused_source();
    CHECK_FALSE(wrong_geometry.adopt_source_alias(history, sha('1'),
        "other-geometry", "source-appearance"));
    auto wrong_appearance = paused_source();
    CHECK_FALSE(wrong_appearance.adopt_source_alias(history, sha('1'),
        "source-geometry", "other-appearance"));
    auto committed = paused_source();
    committed.partition_committed = committed.partition_completed = true;
    CHECK_FALSE(committed.adopt_source_alias(history, sha('1'),
        "source-geometry", "source-appearance"));
}

TEST_CASE("Workbench entry ownership isolates current aliases and foreign portrait flows",
          "[PortraitPrintFlow][PortraitContext][Regression]")
{
    constexpr const char* asset = "b8f895ae-ad58-4d07-af82-e96272de94b6";
    const std::string download = std::string("D:/models/downloads/orcaslicer-ai-") + asset + ".glb";
    const std::string history = std::string("D:/models/") + asset + "/model.glb";

    auto current = workbench_flow(PP::Stage::Check);
    current.original_model_path = current.model_path = download;
    current.frozen = {{"model_path", download}, {"asset_id", asset}};
    CHECK(PP::classify_workbench_entry(current, download, sha('1'),
        "source-geometry", "source-appearance", asset) ==
        PP::WorkbenchEntryOwnership::Current);

    auto alias = current;
    alias.paused = true;
    alias.authorized = false;
    CHECK(PP::classify_workbench_entry(alias, history, sha('1'),
        "source-geometry", "source-appearance", asset) ==
        PP::WorkbenchEntryOwnership::Alias);

    CHECK(PP::classify_workbench_entry(current, "D:/models/other/model.glb", sha('9'),
        "other-geometry", "other-appearance", "other") ==
        PP::WorkbenchEntryOwnership::Foreign);

    auto completed = current;
    completed.stage = PP::Stage::PrintReady;
    CHECK(PP::classify_workbench_entry(completed, "D:/models/other/model.glb", sha('9'),
        "other-geometry", "other-appearance", "other") ==
        PP::WorkbenchEntryOwnership::Available);
}

TEST_CASE("Legacy generation flows retire without losing their recoverable identity",
          "[PortraitPrintFlow][PortraitContext][Regression]")
{
    auto legacy = flow();
    legacy.stage = PP::Stage::Model;
    legacy.request_id = "request";
    legacy.design_job_id = "design-job";
    legacy.model_job_id = "model-job";
    legacy.provider_task_id = "provider-task";
    legacy.conversion_task_id = "conversion-task";
    legacy.source_image_copy = "source.png";
    legacy.design_path = "design.png";
    legacy.original_model_path = "original.glb";
    legacy.model_path = "generated.glb";
    const auto before = PP::encode(legacy);

    REQUIRE(PP::retire_legacy_generation_for_workbench(legacy));
    CHECK(legacy.paused);
    CHECK_FALSE(legacy.authorized);
    CHECK_FALSE(legacy.running());
    const auto after = PP::encode(legacy);
    for (const char* key : {"id", "stage", "request_id", "design_job_id", "model_job_id",
            "provider_task_id", "conversion_task_id", "source_image_copy", "design_path",
            "original_model_path", "model_path", "frozen"}) {
        CAPTURE(key);
        CHECK(after.at(key) == before.at(key));
    }

    auto workbench = workbench_flow();
    CHECK_FALSE(PP::retire_legacy_generation_for_workbench(workbench));
    PP::State inactive;
    CHECK_FALSE(PP::retire_legacy_generation_for_workbench(inactive));
}

TEST_CASE("Deferred Honor suite continuation is bound to one model and partition",
          "[PortraitPrintFlow][PortraitContext][HonorFilamentSuite][Regression]")
{
    const PP::HonorSuiteContinuationIdentity identity {
        "D:/models/partition.obj", sha('1'), "geometry", "appearance", 17, sha('2')
    };
    REQUIRE(identity.valid());
    CHECK(identity.matches("D:/models/partition.obj", sha('1'), "geometry", "appearance",
        17, sha('2'), sha('2')));
    CHECK_FALSE(identity.matches("D:/models/other.obj", sha('1'), "geometry", "appearance",
        17, sha('2'), sha('2')));
    CHECK_FALSE(identity.matches("D:/models/partition.obj", sha('9'), "geometry", "appearance",
        17, sha('2'), sha('2')));
    CHECK_FALSE(identity.matches("D:/models/partition.obj", sha('1'), "other-geometry", "appearance",
        17, sha('2'), sha('2')));
    CHECK_FALSE(identity.matches("D:/models/partition.obj", sha('1'), "geometry", "other-appearance",
        17, sha('2'), sha('2')));
    CHECK_FALSE(identity.matches("D:/models/partition.obj", sha('1'), "geometry", "appearance",
        18, sha('2'), sha('2')));
    CHECK_FALSE(identity.matches("D:/models/partition.obj", sha('1'), "geometry", "appearance",
        17, sha('3'), sha('2')));
    CHECK_FALSE(identity.matches("D:/models/partition.obj", sha('1'), "geometry", "appearance",
        17, sha('2'), sha('3')));
}

TEST_CASE("History switches revoke every portrait callback while leaving other models unclaimed",
          "[PortraitPrintFlow][PortraitContext][P0]")
{
    const auto stage = GENERATE(PP::Stage::Check, PP::Stage::Partition,
        PP::Stage::Color, PP::Stage::SaveColor);
    CAPTURE(int(stage));
    auto state = workbench_flow(stage);
    const nlohmann::json failure { {"schema", "orca.native-portrait-partition-build-report/v1"},
        {"success", false} };
    if (stage == PP::Stage::Partition) {
        state.source_checked = true;
        state.source_check_hash = state.model_hash;
        REQUIRE(state.begin_partition("pending-partition.json"));
    }
    if (stage == PP::Stage::SaveColor) {
        state.semantic_applied = true;
        state.person_detected = true;
        state.near_color_fallback = true;
        state.colored_saved = true;
        state.evidence_status = "partial_available";
        state.application_status = "semantic_regions_applied";
        state.semantic_status = "partial_regions_applied";
        state.region_binding_digest = sha('7');
        state.base_binding = nlohmann::json::object({{"physical_slot", 1}});
        state.semantic_binding = semantic_binding();
        state.region_semantics = region_semantics();
        state.native_color_reduction = native_color_reduction();
        state.final_slot_binding = final_slot_binding();
        state.post_color_check = post_color_check(native_partition_reference());
    }

    REQUIRE(state.pause_for_history_switch(stage == PP::Stage::SaveColor));
    CHECK(state.paused);
    CHECK_FALSE(state.authorized);
    CHECK_FALSE(state.running());
    CHECK(state.owns_display_context("original.glb", sha('1'),
        "source-geometry", "source-appearance"));
    CHECK_FALSE(state.owns_display_context("history.glb", sha('1'),
        "source-geometry", "source-appearance"));
    CHECK_FALSE(state.owns_display_context("original.glb", sha('9'),
        "source-geometry", "source-appearance"));

    if (stage == PP::Stage::Check) {
        CHECK_FALSE(state.accept_source_check(state.model_hash));
        CHECK_FALSE(state.source_checked);
    } else if (stage == PP::Stage::Partition) {
        CHECK_FALSE(state.fail_partition(failure));
        CHECK(state.partition_pending);
        CHECK(state.pending_partition_metadata_path == "pending-partition.json");
    } else if (stage == PP::Stage::Color) {
        CHECK_FALSE(state.advance(state.id, PP::Stage::Color, PP::Stage::SaveColor));
    } else {
        CHECK(state.stage == PP::Stage::Color);
        CHECK_FALSE(state.colored_saved);
        CHECK_FALSE(state.semantic_applied);
        CHECK_FALSE(state.person_detected);
        CHECK_FALSE(state.near_color_fallback);
        CHECK(state.evidence_status.empty());
        CHECK(state.application_status.empty());
        CHECK(state.semantic_status.empty());
        CHECK(state.region_binding_digest.empty());
        CHECK(state.base_binding.is_null());
        CHECK(state.semantic_binding.is_null());
        CHECK(state.region_semantics.is_null());
        CHECK(state.native_color_reduction.is_null());
        CHECK(state.final_slot_binding.is_null());
        CHECK(state.post_color_check.is_null());
        CHECK_FALSE(state.commit_colored_carrier(native_partition_reference(), region_semantics(),
            native_color_reduction(), final_slot_binding(),
            post_color_check(native_partition_reference()), "partition.obj", sha('4'),
            "partition-geometry", "source-appearance"));
    }
}

TEST_CASE("History switching is limited to active completed-model flows", "[PortraitPrintFlow][PortraitContext]")
{
    auto generation = flow();
    CHECK_FALSE(generation.pause_for_history_switch(false));
    auto completed = workbench_flow(PP::Stage::PrintReady);
    CHECK_FALSE(completed.pause_for_history_switch(false));
    auto failed = workbench_flow();
    failed.failed = true;
    CHECK_FALSE(failed.pause_for_history_switch(false));
}

TEST_CASE("Completed legacy models can enter the new workbench flow while current attempts remain unique", "[PortraitPrintFlow]")
{
    PP::State saved;
    saved.id = "saved-workbench"; saved.entry = "completed_model";
    saved.geometry_id = "geometry"; saved.appearance_id = "appearance";
    saved.original_model_path = "original.glb"; saved.model_path = "colored.glb";
    saved.original_model_hash = "original-sha"; saved.model_hash = "colored-sha";
    saved.record_version = 3; saved.stage = PP::Stage::PrintReady;
    PP::State restored;
    REQUIRE(PP::decode(PP::encode(saved), restored));
    CHECK_FALSE(restored.active());
    CHECK_FALSE(PP::workbench_entry_is_duplicate(restored, "original.glb", "original-sha"));
    CHECK_FALSE(PP::workbench_entry_is_duplicate(restored, "colored.glb", "colored-sha"));
    restored.stage = PP::Stage::Check;
    CHECK(PP::workbench_entry_is_duplicate(restored, "original.glb", "original-sha"));
    restored.record_version = 4;
    for (const auto stage : {PP::Stage::Check, PP::Stage::Partition, PP::Stage::PrintReady}) {
        DYNAMIC_SECTION("Current attempt stage " << int(stage)) {
            restored.stage = stage;
            CHECK(PP::workbench_entry_is_duplicate(restored, "original.glb", "original-sha"));
            CHECK(PP::workbench_entry_is_duplicate(restored, "colored.glb", "colored-sha"));
            CHECK_FALSE(PP::workbench_entry_is_duplicate(restored, "original.glb", "changed-sha"));
            CHECK_FALSE(PP::workbench_entry_is_duplicate(restored, "another.glb", "original-sha"));
        }
    }
    restored.stage = PP::Stage::Idle;
    CHECK_FALSE(PP::workbench_entry_is_duplicate(restored, "original.glb", "original-sha"));
}

TEST_CASE("Windows asset path spellings preserve completed portrait ownership", "[PortraitPrintFlow]")
{
    PP::State s;
    s.record_version = 4;
    s.id = "completed-session"; s.entry = "completed_model"; s.stage = PP::Stage::PrintReady;
    s.geometry_id = "geometry"; s.appearance_id = "appearance";
    s.model_path = "D:\\portraits\\downloads\\colored.glb";
    s.original_model_path = "D:\\portraits\\original.glb";
    s.model_hash = s.original_model_hash = "model-sha";
    PP::State reopened;
    REQUIRE(PP::decode(PP::encode(s), reopened));
#ifdef _WIN32
    CHECK(reopened.owns_workbench_model("D:\\portraits\\downloads/colored.glb", "geometry", "appearance"));
    CHECK(reopened.owns_workbench_model("d:/portraits/downloads/colored.glb", "geometry", "appearance"));
    CHECK(reopened.owns_workbench_model("D:/portraits/original.glb", "geometry", "appearance"));
#else
    CHECK_FALSE(reopened.owns_workbench_model("D:/portraits/downloads/colored.glb", "geometry", "appearance"));
#endif
    CHECK_FALSE(reopened.owns_workbench_model("D:/other/downloads/colored.glb", "geometry", "appearance"));
    CHECK_FALSE(reopened.owns_workbench_model("D:/portraits/downloads/colored.glb", "changed", "appearance"));
    CHECK_FALSE(PP::same_model_path("", ""));
}

namespace {
struct RetryPartition {
    bool ready {true};
    std::string geometry_id {"partition-geometry"}, partition_digest {sha('2')};
    bool valid() const { return ready; }
};
struct RetryIdentity : RetryPartition {
    uint32_t algorithm {12};
    size_t face_count {4};
};
struct RetryColors {
    uint32_t algorithm_version {12};
    std::string geometry_id {"partition-geometry"}, source_basis_signature {sha('2')};
    size_t face_count {4};
    std::optional<RetryIdentity> native_partition_identity {RetryIdentity{}};
};
struct RetryPreview {
    struct Trial { std::string suite_id {"honor.people.six"}; } trial;
    RetryPartition native;
    RetryColors colors;
    bool manual {false};
    std::map<size_t, int> faces {{0, 1}, {1, 1}, {2, 1}, {3, 1}};
    std::vector<int> subfaces;
    Trial color_trial_state() const { return trial; }
    bool has_native_portrait_partition_conflict() const { return manual; }
    const RetryPartition* native_portrait_partition() const { return &native; }
    const RetryColors* portrait_color_blocks() const { return &colors; }
    size_t triangle_count() const { return 4; }
    const auto& import_subface_color_overrides(bool) const { return subfaces; }
    const auto& printer_filament_export_faces() const { return faces; }
};
PP::State paused_color_retry()
{
    auto flow = workbench_flow(PP::Stage::SaveColor);
    flow.model_path = "partition.obj"; flow.model_hash = sha('4');
    flow.geometry_id = "partition-geometry";
    flow.partition_committed = flow.partition_completed = true;
    flow.boundary_algorithm_version = 12;
    flow.basis_signature = sha('2'); flow.partition_topology_hash = sha('3');
    flow.native_portrait_partition = native_partition_reference();
    flow.workbench_check_certificate = {{"preserved", true}};
    flow.rollback_color_save();
    return flow;
}
}

TEST_CASE("Explicit portrait retry preserves the native owner and re-enters SaveColor",
    "[PortraitPrintFlow][PortraitSave][PortraitRetry][P0]")
{
    auto flow = paused_color_retry();
    RetryPreview preview;
    const auto reference = flow.native_portrait_partition;
    const auto certificate = flow.workbench_check_certificate;
    CHECK_FALSE(flow.running());
    size_t resumes = 0, ordinary_saves = 0;
    const auto resumed = PP::resume_paused_color_save_after_explicit_action(flow, true, preview, [&] {
        ++resumes; flow.authorized = true; flow.paused = false; return true;
    });
    if (!resumed.has_value()) ++ordinary_saves;
    REQUIRE(resumed.has_value()); REQUIRE(*resumed);
    CHECK(resumes == 1); CHECK(ordinary_saves == 0);
    CHECK(flow.advance(flow.id, PP::Stage::Color, PP::Stage::SaveColor));
    CHECK(flow.native_portrait_partition == reference);
    CHECK(flow.workbench_check_certificate == certificate);
    CHECK(flow.basis_signature == sha('2')); CHECK(flow.partition_topology_hash == sha('3'));
}

TEST_CASE("Portrait retry blocks stale cards partitions manual edits and incomplete face coverage",
    "[PortraitPrintFlow][PortraitSave][PortraitRetry][P0]")
{
    const int broken = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13);
    auto flow = paused_color_retry();
    RetryPreview preview;
    switch (broken) {
    case 0: preview.trial.suite_id = "honor.pets.six"; break;
    case 1: preview.native.ready = false; break;
    case 2: preview.colors.algorithm_version = 11; break;
    case 3: preview.colors.native_partition_identity.reset(); break;
    case 4: preview.colors.source_basis_signature = sha('8'); break;
    case 5: preview.manual = true; break;
    case 6: preview.faces.erase(3); break;
    case 7: preview.faces.erase(3); preview.faces[4] = 1; break;
    case 8: preview.subfaces.push_back(1); break;
    case 9: flow.native_portrait_partition["algorithm"] = 11; break;
    case 10: flow.partition_topology_hash = sha('8'); break;
    case 11: flow.native_portrait_partition["carrier_sha256"] = sha('8'); break;
    case 12: flow.native_portrait_partition["appearance_id"] = "foreign-appearance"; break;
    case 13: flow.authorized = true; break;
    }
    size_t resumes = 0;
    CHECK_FALSE(PP::eligible_paused_color_save_retry(flow, true, preview));
    const auto result = PP::resume_paused_color_save_after_explicit_action(flow, true, preview,
        [&] { ++resumes; return true; });
    REQUIRE(result.has_value()); CHECK_FALSE(*result); CHECK(resumes == 0);
    CHECK(flow.paused); CHECK(flow.authorized == (broken == 13)); CHECK(flow.stage == PP::Stage::Color);
}

TEST_CASE("Portrait retry rejects failed continuation and isolates every owner identity",
    "[PortraitPrintFlow][PortraitSave][PortraitRetry][PortraitContext][P0]")
{
    auto flow = paused_color_retry();
    RetryPreview preview;
    size_t resumes = 0;
    const auto rejected = PP::resume_paused_color_save_after_explicit_action(flow, true, preview,
        [&] { ++resumes; return false; });
    REQUIRE(rejected.has_value()); CHECK_FALSE(*rejected); CHECK(resumes == 1);
    CHECK(flow.paused); CHECK_FALSE(flow.authorized);
    for (int changed = 0; changed < 4; ++changed) {
        auto path = flow.model_path, hash = flow.model_hash;
        auto geometry = flow.geometry_id, appearance = flow.appearance_id;
        if (changed == 0) path = "another.obj";
        if (changed == 1) hash = sha('9');
        if (changed == 2) geometry = "another-geometry";
        if (changed == 3) appearance = "another-appearance";
        const bool owner = flow.owns_display_context(path, hash, geometry, appearance);
        CHECK_FALSE(owner);
        CHECK_FALSE(PP::resume_paused_color_save_after_explicit_action(flow, owner, preview,
            [&] { ++resumes; return true; }).has_value());
    }
    CHECK(resumes == 1); CHECK(flow.paused); CHECK_FALSE(flow.authorized);
    flow.failed = true;
    REQUIRE_FALSE(PP::resume_paused_color_save_after_explicit_action(flow, true, preview,
        [&] { ++resumes; return true; }).value_or(true));
    CHECK(resumes == 1);
}

TEST_CASE("Legacy v12 partition identities rebuild until an exact native mesh is present",
          "[PortraitPrintFlow][PortraitNativePartition][WorkbenchNativeImport]")
{
    const auto current = native_partition_reference();
    REQUIRE(PP::native_partition_has_exact_mesh(current));
    auto legacy = current;
    legacy["cache_reference"].erase("native_mesh_sha256");
    REQUIRE(PP::valid_native_partition_reference(legacy));
    CHECK_FALSE(PP::native_partition_has_exact_mesh(legacy));
    auto state = workbench_flow(PP::Stage::Color);
    state.record_version = 7;
    state.boundary_algorithm_version = NP::algorithm_version;
    state.partition_completed = state.partition_committed = true;
    state.basis_signature = current.at("partition_digest").get<std::string>();
    state.partition_topology_hash = current.at("topology_hash").get<std::string>();
    state.native_portrait_partition = current;
    REQUIRE_FALSE(state.partition_needs_rebuild(NP::algorithm_version));
    state.native_portrait_partition = legacy;
    CHECK(state.partition_needs_rebuild(NP::algorithm_version));
    for (const auto& digest : {nlohmann::json("not-a-sha"), nlohmann::json(std::string(64, 'g')),
            nlohmann::json(nullptr)}) {
        auto corrupt = current;
        corrupt["cache_reference"]["native_mesh_sha256"] = digest;
        CHECK_FALSE(PP::native_partition_has_exact_mesh(corrupt));
        state.native_portrait_partition = corrupt;
        CHECK(state.partition_needs_rebuild(NP::algorithm_version));
    }
}
