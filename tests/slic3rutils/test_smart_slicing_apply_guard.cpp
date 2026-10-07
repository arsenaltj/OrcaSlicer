#include <catch2/catch_all.hpp>
#include "slic3r/AI/SmartSlicing/Application/SmartSlicingCoordinator.hpp"
#include "slic3r/GUI/AI/Orca/OrcaOfficialSliceGateway.hpp"
#include "slic3r/GUI/AI/Orca/OrcaSlicingRevision.hpp"
#include "slic3r/GUI/AI/Orca/OrcaPrintConfirmation.hpp"
#include <stdexcept>

using namespace Slic3r::AI::SmartSlicing;

TEST_CASE("automatic mapping writeback preserves slicing input identity", "[SmartSlicing][Apply][Revision]")
{
    using namespace Slic3r;
    const auto mode = GENERATE(fmmAutoForFlush, fmmAutoForMatch);
    DynamicPrintConfig plate;
    plate.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    const auto before = GUI::slicing_revision_plate_config(plate, mode);
    plate.set_key_value("filament_map", new ConfigOptionInts(std::vector<int>{1, 1, 2}));
    plate.set_key_value("filament_volume_map", new ConfigOptionInts(std::vector<int>{0, 0, 1}));
    plate.set_key_value("filament_nozzle_map", new ConfigOptionInts(std::vector<int>{1, 1, 3}));
    CHECK(GUI::slicing_revision_plate_config(plate, mode) == before);
    plate.set_key_value("layer_height", new ConfigOptionFloat(0.16));
    CHECK_FALSE(GUI::slicing_revision_plate_config(plate, mode) == before);
    CHECK(plate.has("filament_map")); // The live plate is never altered.
}

TEST_CASE("manual mapping edits remain part of slicing input identity", "[SmartSlicing][Apply][Revision]")
{
    using namespace Slic3r;
    const auto mode = GENERATE(fmmManual, fmmNozzleManual, fmmDefault);
    const std::string key = GENERATE(std::string("filament_map"), std::string("filament_volume_map"));
    DynamicPrintConfig plate;
    plate.set_key_value(key, new ConfigOptionInts(std::vector<int>{1, 2}));
    const auto before = GUI::slicing_revision_plate_config(plate, mode);
    plate.set_key_value(key, new ConfigOptionInts(std::vector<int>{2, 1}));
    CHECK_FALSE(GUI::slicing_revision_plate_config(plate, mode) == before);
}

TEST_CASE("nozzle assignments invalidate only when they are manual inputs", "[SmartSlicing][Apply][Revision]")
{
    using namespace Slic3r;
    const auto mode = GENERATE(fmmManual, fmmNozzleManual, fmmDefault);
    DynamicPrintConfig plate;
    plate.set_key_value("filament_nozzle_map", new ConfigOptionInts(std::vector<int>{1, 2}));
    const auto before = GUI::slicing_revision_plate_config(plate, mode);
    plate.set_key_value("filament_nozzle_map", new ConfigOptionInts(std::vector<int>{2, 1}));
    CHECK((GUI::slicing_revision_plate_config(plate, mode) == before) == (mode == fmmManual));
}

namespace {
struct ApplyTestWorkspace final : IOrcaWorkspace {
    WorkspaceRevision current_revision() const override { return {1, 2, 3, "before"}; }
    WorkspaceContext capture_context() const override {
        WorkspaceContext context;
        context.revision = current_revision();
        context.plate_index = 0;
        context.printer_preset_id = "printer";
        context.process_preset_id = "process";
        context.materials.push_back({"material", "#FFFFFF"});
        context.objects.push_back({42, "cube", 1, 12, 0, false});
        context.native_validation_available = true;
        return context;
    }
};
struct ApplyTestTrial final : ITrialSliceExecutor {
    TrialSliceResult execute_trial_slice(const SliceCandidate& candidate) override {
        return {candidate.id, candidate.base_revision, TrialSliceStatus::Succeeded, SlicingMetrics{}, {}};
    }
    void cancel_trial_slice() override {}
};
} // namespace

TEST_CASE("completed applications recover from undo errors and reject obsolete undo", "[SmartSlicing][Apply]")
{
    ApplyTestWorkspace workspace;
    ApplyTestTrial trial;
    bool throw_on_undo = true;
    const bool can_undo = GENERATE(false, true);
    Slic3r::GUI::OrcaOfficialSliceGateway gateway(
        [&] { return workspace.current_revision(); }, [](const SliceCandidate&) { return std::string{}; },
        [](const SliceCandidate&) { return Slic3r::GUI::OrcaApplyMutationResult{true, true, {}}; },
        [] { return true; }, [] { return true; }, [&] {
            if (throw_on_undo) throw std::runtime_error("temporary undo failure");
            return can_undo;
        });
    SmartSlicingCoordinator coordinator(workspace, trial, gateway);
    coordinator.start();
    REQUIRE(coordinator.plan_and_slice_candidates());
    REQUIRE(coordinator.apply_selected_candidate());
    gateway.notify_slice_completed(true);
    REQUIRE(coordinator.poll_official_slice());
    REQUIRE(coordinator.snapshot().state == WorkflowState::Completed);
    CHECK_FALSE(coordinator.undo_applied_candidate());
    CHECK(coordinator.snapshot().state == WorkflowState::Completed);
    CHECK(coordinator.snapshot().detail == "apply_undo_failed");
    CHECK(coordinator.snapshot().can_undo_apply);
    throw_on_undo = false;
    CHECK(coordinator.undo_applied_candidate() == can_undo);
    CHECK(coordinator.snapshot().state == (can_undo ? WorkflowState::ReadyToApply : WorkflowState::Stale));
    CHECK_FALSE(coordinator.snapshot().can_undo_apply);
    CHECK_FALSE(coordinator.undo_applied_candidate());
}

TEST_CASE("undo targets the applied revision and leaves subsequent edits untouched", "[SmartSlicing][Apply]")
{
    WorkspaceRevision current{1, 2, 3, "before"};
    size_t undo_calls = 0;
    Slic3r::GUI::OrcaOfficialSliceGateway gateway(
        [&] { return current; }, [](const SliceCandidate&) { return std::string{}; },
        [&](const SliceCandidate&) {
            current.fingerprint = "applied";
            return Slic3r::GUI::OrcaApplyMutationResult{true, true, {}};
        }, [] { return true; }, [] { return true; }, [&] { ++undo_calls; return true; });
    SliceCandidate candidate;
    candidate.base_revision = current;
    REQUIRE(gateway.commit(candidate, candidate.base_revision).phase == OfficialSlicePhase::Slicing);
    gateway.notify_slice_completed(true);
    REQUIRE(gateway.poll().phase == OfficialSlicePhase::Completed);
    const bool subsequently_edited = GENERATE(false, true);
    if (subsequently_edited) current.fingerprint = "user-edit";
    CHECK(gateway.undo_last_apply() == !subsequently_edited);
    CHECK(undo_calls == (subsequently_edited ? 0 : 1));
    CHECK_FALSE(gateway.undo_last_apply());
}

TEST_CASE("native undo rejection preserves later history even when the content matches", "[SmartSlicing][Apply]")
{
    const WorkspaceRevision revision{1, 2, 3, "same-content"};
    Slic3r::GUI::OrcaOfficialSliceGateway gateway(
        [=] { return revision; }, [](const SliceCandidate&) { return std::string{}; },
        [](const SliceCandidate&) { return Slic3r::GUI::OrcaApplyMutationResult{true, true, {}}; },
        [] { return false; }, [] { return true; }, [] { return false; });
    SliceCandidate candidate;
    candidate.base_revision = revision;
    REQUIRE(gateway.commit(candidate, revision).phase == OfficialSlicePhase::Failed);
    CHECK_FALSE(gateway.undo_last_apply());
}


namespace {
struct ConfirmationWorkspace final : IOrcaWorkspace {
    WorkspaceRevision revision{1, 2, 3, "native-slice"};
    bool unavailable{false};
    WorkspaceRevision current_revision() const override {
        if (unavailable) throw std::runtime_error("workspace no longer available");
        return revision;
    }
    WorkspaceContext capture_context() const override {
        WorkspaceContext context;
        context.revision = current_revision();
        return context;
    }
};
}

TEST_CASE("print confirmation permits only the captured complete workspace version", "[SmartSlicing][Apply][Revision]")
{
    ConfirmationWorkspace workspace;
    const auto confirmed = Slic3r::GUI::capture_orca_print_confirmation_revision(workspace);
    REQUIRE(confirmed.has_value());
    CHECK(Slic3r::GUI::orca_print_confirmation_revision_current(confirmed, workspace));
    const int changed_input = GENERATE(0, 1, 2, 3);
    // The same old slice file/ID cannot qualify changes in any input category.
    if (changed_input == 0) ++workspace.revision.model_revision;
    if (changed_input == 1) ++workspace.revision.config_revision;
    if (changed_input == 2) ++workspace.revision.plate_revision;
    if (changed_input == 3) workspace.revision.fingerprint = "replacement-project";
    CHECK_FALSE(Slic3r::GUI::orca_print_confirmation_revision_current(confirmed, workspace));
    workspace.revision = *confirmed;
    CHECK(Slic3r::GUI::orca_print_confirmation_revision_current(confirmed, workspace));
}

TEST_CASE("print confirmation rejects missing and invalid version snapshots", "[SmartSlicing][Apply][Revision]")
{
    ConfirmationWorkspace workspace;
    CHECK_FALSE(Slic3r::GUI::orca_print_confirmation_revision_current(std::nullopt, workspace));
    CHECK_FALSE(Slic3r::GUI::orca_print_confirmation_revision_current(WorkspaceRevision{}, workspace));
    workspace.revision.fingerprint.clear();
    CHECK_FALSE(Slic3r::GUI::capture_orca_print_confirmation_revision(workspace).has_value());
}

TEST_CASE("print confirmation handles capture failure without trusting the previous slice", "[SmartSlicing][Apply][Revision]")
{
    ConfirmationWorkspace workspace;
    const auto confirmed = Slic3r::GUI::capture_orca_print_confirmation_revision(workspace);
    REQUIRE(confirmed.has_value());
    workspace.unavailable = true;
    CHECK_FALSE(Slic3r::GUI::capture_orca_print_confirmation_revision(workspace).has_value());
    CHECK_FALSE(Slic3r::GUI::orca_print_confirmation_revision_current(confirmed, workspace));
    workspace.unavailable = false;
    CHECK(Slic3r::GUI::orca_print_confirmation_revision_current(confirmed, workspace));
}


TEST_CASE("print center distinguishes an unconfigured device from a connection entry", "[SmartSlicing][PrintCenter]")
{
    using namespace Slic3r::GUI;
    OrcaPrintConnectionFacts facts;
    CHECK(orca_print_connection_state(facts) == OrcaPrintConnectionState::Unconfigured);
    facts.web_entry_configured = true;
    facts.native_selected = true;
    facts.native_online = true;
    facts.native_connected = true;
    facts.telemetry_age = std::chrono::milliseconds(0);
    CHECK(orca_print_connection_state(facts) == OrcaPrintConnectionState::Unknown);
    facts.web_entry_configured = false;
    facts.native_backend = true;
    facts.native_selected = false;
    CHECK(orca_print_connection_state(facts) == OrcaPrintConnectionState::SelectionRequired);
}

TEST_CASE("print center rejects missing stale and future device telemetry", "[SmartSlicing][PrintCenter]")
{
    using namespace Slic3r::GUI;
    OrcaPrintConnectionFacts facts;
    facts.native_backend = facts.native_selected = facts.native_online = facts.native_connected = true;
    CHECK(orca_print_connection_state(facts) == OrcaPrintConnectionState::Unknown);
    const long long age = GENERATE(-1LL, 0LL, 15000LL, 15001LL, 60000LL);
    facts.telemetry_age = std::chrono::milliseconds(age);
    const auto expected = age >= 0 && age <= 15000 ?
        OrcaPrintConnectionState::Online : OrcaPrintConnectionState::Unknown;
    CHECK(orca_print_connection_state(facts) == expected);
}

TEST_CASE("print center never carries native online status into another backend", "[SmartSlicing][PrintCenter]")
{
    using namespace Slic3r::GUI;
    OrcaPrintConnectionFacts facts;
    facts.native_backend = facts.native_selected = true;
    CHECK(orca_print_connection_state(facts) == OrcaPrintConnectionState::Offline);
    facts.native_online = true;
    facts.telemetry_age = std::chrono::milliseconds(0);
    CHECK(orca_print_connection_state(facts) == OrcaPrintConnectionState::Unknown);
    facts.native_connected = true;
    CHECK(orca_print_connection_state(facts) == OrcaPrintConnectionState::Online);
    facts.native_backend = false;
    CHECK(orca_print_connection_state(facts) == OrcaPrintConnectionState::Unconfigured);
    facts.web_entry_configured = true;
    CHECK(orca_print_connection_state(facts) == OrcaPrintConnectionState::Unknown);
}

TEST_CASE("device media requires current backend telemetry before any file request", "[SmartSlicing][PrintMedia]")
{
    using namespace Slic3r::GUI;
    OrcaDeviceMediaFacts facts;
    facts.information_ready = facts.storage_readable = facts.file_protocol_supported = true;
    CHECK(orca_device_media_state(facts) == OrcaDeviceMediaState::ConnectionRequired);
    facts.connection.native_backend = facts.connection.native_selected = facts.connection.native_online = facts.connection.native_connected = true;
    const auto age = GENERATE(-1LL, 0LL, 15000LL, 15001LL);
    facts.connection.telemetry_age = std::chrono::milliseconds(age);
    CHECK(orca_device_media_state(facts) == (age >= 0 && age <= 15000 ? OrcaDeviceMediaState::Available : OrcaDeviceMediaState::ConnectionRequired));
    facts.connection.native_backend = false;
    facts.connection.web_entry_configured = true;
    CHECK(orca_device_media_state(facts) == OrcaDeviceMediaState::ConnectionRequired);
}

TEST_CASE("device media distinguishes capability storage and busy recovery", "[SmartSlicing][PrintMedia]")
{
    using namespace Slic3r::GUI;
    OrcaDeviceMediaFacts facts;
    facts.connection.native_backend = facts.connection.native_selected = facts.connection.native_online = facts.connection.native_connected = true;
    facts.connection.telemetry_age = std::chrono::milliseconds(0);
    CHECK(orca_device_media_state(facts) == OrcaDeviceMediaState::InformationPending);
    facts.information_ready = true;
    CHECK(orca_device_media_state(facts) == OrcaDeviceMediaState::Unsupported);
    facts.file_protocol_supported = true;
    CHECK(orca_device_media_state(facts) == OrcaDeviceMediaState::StorageUnavailable);
    facts.storage_readable = true;
    facts.busy = true;
    CHECK(orca_device_media_state(facts) == OrcaDeviceMediaState::Busy);
    facts.busy = false;
    CHECK(orca_device_media_state(facts) == OrcaDeviceMediaState::Available);
}
