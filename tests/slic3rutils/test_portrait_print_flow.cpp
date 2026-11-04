#include <catch2/catch_test_macros.hpp>
#include "slic3r/GUI/AI/ModelGeneration/PortraitPrintFlow.hpp"
#include "slic3r/GUI/AI/ModelGeneration/PortraitRecognitionState.hpp"
#include "slic3r/GUI/AI/ModelGeneration/PortraitWorkbenchEntry.hpp"
#include "slic3r/GUI/AIModelGenerationClient.hpp"

namespace PP = Slic3r::GUI::PortraitPrint;
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
    PP::State s;
    s.id = "workbench-session"; s.entry = "completed_model"; s.stage = PP::Stage::Project;
    s.authorized = true; s.geometry_id = "geometry"; s.appearance_id = "appearance";
    s.model_hash = s.original_model_hash = "model-sha";
    s.model_path = s.original_model_path = "portrait.glb";
    CHECK_FALSE(s.reserve_design(s.id, "request"));
    CHECK_FALSE(s.reserve_model(s.id, "job"));
    CHECK_FALSE(s.advance(s.id, PP::Stage::Project, PP::Stage::Palette));
    REQUIRE(s.advance(s.id, PP::Stage::Project, PP::Stage::Check));
    CHECK_FALSE(s.advance("late-session", PP::Stage::Check, PP::Stage::Color));
    CHECK_FALSE(s.advance(s.id, PP::Stage::Check, PP::Stage::Color));
    CHECK_FALSE(s.advance(s.id, PP::Stage::Check, PP::Stage::Partition));
    REQUIRE(s.accept_repair(s.model_hash, "repaired-sha", true, true));
    REQUIRE(s.advance(s.id, PP::Stage::Check, PP::Stage::Partition));
    CHECK_FALSE(s.advance(s.id, PP::Stage::Partition, PP::Stage::Color));
    s.partition_completed = true; s.basis_signature = "ai-four-source";
    s.boundary_algorithm_version = 7;
    REQUIRE(s.advance(s.id, PP::Stage::Partition, PP::Stage::Color));
    CHECK_FALSE(s.advance(s.id, PP::Stage::Check, PP::Stage::Color));
    auto doc = PP::encode(s);
    REQUIRE(doc.at("schema") == "orca.portrait-print/v4");
    PP::State reopened;
    REQUIRE(PP::decode(doc, reopened));
    CHECK(reopened.from_workbench()); CHECK(reopened.paused);
    CHECK_FALSE(reopened.authorized); CHECK_FALSE(reopened.running());
    CHECK(reopened.geometry_id == s.geometry_id);
    CHECK(reopened.repair_checked); CHECK(reopened.partition_completed);
    CHECK(reopened.boundary_algorithm_version == 7);
    CHECK_FALSE(reopened.reserve_design(reopened.id, "paid"));
    doc["stage"] = int(PP::Stage::Model);
    CHECK_FALSE(PP::decode(doc, reopened));
}

TEST_CASE("Workbench processing keeps repair partition base and import in strict order", "[PortraitPrintFlow]")
{
    PP::State s;
    s.id = "local-flow"; s.entry = "completed_model"; s.authorized = true;
    s.stage = PP::Stage::Check; s.model_hash = "original";
    CHECK_FALSE(s.accept_repair("original", "repair", false, true));
    CHECK_FALSE(s.accept_repair("foreign", "repair", true, true));
    REQUIRE(s.accept_repair("original", "repair", true, true));
    CHECK_FALSE(s.native_repair_required);
    REQUIRE(s.advance(s.id, PP::Stage::Check, PP::Stage::Partition));
    CHECK_FALSE(s.accept_repair("original", "late-repair", true, true));
    s.partition_completed = true; s.basis_signature = "basis";
    const std::array<PP::Stage,9> stages{PP::Stage::Partition,PP::Stage::Color,PP::Stage::SaveColor,
        PP::Stage::Base,PP::Stage::Filaments,PP::Stage::Import,PP::Stage::Recommend,PP::Stage::Slice,PP::Stage::PrintReady};
    for (size_t i=0;i+1<stages.size();++i) {
        CHECK_FALSE(s.advance("foreign-flow", stages[i], stages[i+1]));
        if (i+2<stages.size()) CHECK_FALSE(s.advance(s.id,stages[i],stages[i+2]));
        REQUIRE(s.advance(s.id,stages[i],stages[i+1]));
        CHECK_FALSE(s.advance(s.id,stages[i],stages[i+1]));
    }
}

TEST_CASE("Old automatic partitions restart from the saved repair without reusing downstream results", "[PortraitPrintFlow]")
{
    PP::State s;
    s.id = "restored-flow"; s.entry = "completed_model"; s.stage = PP::Stage::Slice;
    s.paused = s.restored = s.repair_checked = s.repair_saved = true;
    s.boundary_algorithm_version = 7; s.partition_completed = s.colored_saved = s.semantic_applied = true;
    s.model_path = "old-painted.obj"; s.model_hash = "painted";
    s.geometry_id = "repair-geometry"; s.appearance_id = "original-appearance";
    s.original_model_path = "original.glb"; s.original_model_hash = "original";
    s.repair_result_hash = "repair"; s.repair_source_hash = "original";
    s.basis_signature = "old-basis"; s.region_binding_digest = "old-regions";
    s.slice_started = s.native_repair_completed = true;
    s.slice_revision = s.project_revision = "old-project"; s.gcode_path = "old.gcode";
    s.preview_result_id = "old-preview";
    const auto old_asset = s.color_asset_id();
    REQUIRE(s.partition_needs_rebuild(8));
    CHECK_FALSE(s.rebuild_partition(8, "repair.obj", "wrong-hash"));
    CHECK(s.model_path == "old-painted.obj");
    REQUIRE(s.rebuild_partition(8, "repair.obj", "repair"));
    CHECK(s.stage == PP::Stage::Check);
    CHECK(s.model_path == "repair.obj"); CHECK(s.model_hash == "repair");
    CHECK(s.repair_saved); CHECK(s.repair_checked); CHECK_FALSE(s.native_repair_required);
    CHECK_FALSE(s.partition_completed); CHECK_FALSE(s.colored_saved); CHECK_FALSE(s.semantic_applied);
    CHECK_FALSE(s.slice_started); CHECK_FALSE(s.native_repair_completed);
    CHECK(s.gcode_path.empty()); CHECK(s.slice_revision.empty()); CHECK(s.preview_result_id.empty());
    CHECK(s.paused); CHECK_FALSE(s.authorized);
    CHECK(s.original_model_path == "original.glb"); CHECK(s.repair_source_hash == "original");
    CHECK(s.color_asset_id() != old_asset);
    CHECK_FALSE(s.partition_needs_rebuild(8));
    CHECK_FALSE(s.rebuild_partition(8, "repair.obj", "repair"));
    PP::State reopened;
    REQUIRE(PP::decode(PP::encode(s), reopened));
    CHECK(reopened.model_path == "repair.obj"); CHECK(reopened.repair_checked);
    CHECK(reopened.geometry_id == "repair-geometry");
    CHECK(reopened.appearance_id == "original-appearance");
    CHECK_FALSE(reopened.partition_completed); CHECK_FALSE(reopened.running());
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
