#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "slic3r/GUI/AI/ModelGeneration/PortraitColorBlockPostprocess.hpp"
#include "slic3r/GUI/AI/ModelGeneration/PortraitNativePartitionCache.hpp"
#include "slic3r/GUI/AI/ModelGeneration/WorkbenchArtifactValidation.hpp"
#include "slic3r/GUI/AI/ModelGeneration/PortraitWorkbenchEntry.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelGenerationSubmissionResult.hpp"
#include "slic3r/GUI/AIModelGenerationClient.hpp"
#include "test_utils.hpp"
#include <tuple>

namespace PP = Slic3r::GUI::WorkbenchArtifacts;
namespace PC = Slic3r::GUI::PortraitColorBlocks;
namespace NP = Slic3r::GUI::NativePortrait;
namespace {
std::string sha(char c) { return std::string(64,c); }
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
}


TEST_CASE("Other suites and explicit palettes retain manual workbench processing", "[WorkbenchArtifactValidation]")
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

TEST_CASE("Portrait source risk certificates are content addressed and reusable only for the same identity",
    "[WorkbenchArtifactValidation]")
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

TEST_CASE("Native portrait commit publication is all or nothing at every checkpoint",
          "[WorkbenchArtifactValidation][PortraitNativePartition][P0]")
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
    rebound["test_revision"] = 2;
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
