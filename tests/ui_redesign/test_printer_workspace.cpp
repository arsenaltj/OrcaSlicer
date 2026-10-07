#include <catch2/catch_test_macros.hpp>
#include "slic3r/GUI/Redesign/PrinterWorkspaceState.hpp"
#include <limits>

using namespace Slic3r::GUI;

namespace {
PrinterWorkspaceSnapshot printing()
{
    PrinterWorkspaceSnapshot state;
    state.device_id = "printer-a";
    state.task_id = "job-1";
    state.connected = true;
    state.info_ready = true;
    state.phase = PrinterTaskPhase::Printing;
    state.can_pause = true;
    state.can_resume = true;
    state.can_stop = true;
    state.can_print = true;
    state.native_dispatch = true;
    state.gcode_ready = true;
    return state;
}
const PrinterCommandTarget target { "printer-a", "job-1" };
}

TEST_CASE("Printer controls reject a changed device or job after confirmation", "[UiRedesign][PrinterWorkspace]")
{
    auto state = printing();
    REQUIRE(printer_command_allowed(state, target, PrinterTaskAction::Pause));
    state.device_id = "printer-b";
    CHECK_FALSE(printer_command_allowed(state, target, PrinterTaskAction::Pause));
    CHECK_FALSE(printer_command_allowed(state, target, PrinterTaskAction::Stop));
    state.device_id = target.device_id;
    state.task_id = "job-2";
    CHECK_FALSE(printer_command_allowed(state, target, PrinterTaskAction::Pause));
    CHECK_FALSE(printer_command_allowed(state, target, PrinterTaskAction::Stop));
}

TEST_CASE("Printer controls reject disconnected and unreported devices", "[UiRedesign][PrinterWorkspace]")
{
    auto state = printing();
    state.connected = false;
    CHECK_FALSE(printer_command_allowed(state, target, PrinterTaskAction::Pause));
    CHECK_FALSE(printer_command_allowed(state, target, PrinterTaskAction::Stop));
    state.connected = true; state.info_ready = false;
    CHECK_FALSE(printer_command_allowed(state, target, PrinterTaskAction::Pause));
    CHECK_FALSE(printer_can_start(state));
    state = printing();
    CHECK_FALSE(printer_command_allowed(state, {"", "job-1"}, PrinterTaskAction::Stop));
    state.task_id.clear();
    CHECK_FALSE(printer_command_allowed(state, {"printer-a", ""}, PrinterTaskAction::Stop));
}

TEST_CASE("Printer controls follow the reported task phase and capability", "[UiRedesign][PrinterWorkspace]")
{
    auto state = printing();
    CHECK_FALSE(printer_command_allowed(state, target, PrinterTaskAction::Resume));
    state.phase = PrinterTaskPhase::Paused;
    CHECK(printer_command_allowed(state, target, PrinterTaskAction::Resume));
    CHECK(printer_command_allowed(state, target, PrinterTaskAction::Stop));
    CHECK_FALSE(printer_command_allowed(state, target, PrinterTaskAction::Pause));
    state.can_resume = false;
    CHECK_FALSE(printer_command_allowed(state, target, PrinterTaskAction::Resume));
    state.phase = PrinterTaskPhase::Preparing;
    CHECK(printer_command_allowed(state, target, PrinterTaskAction::Stop));
    state.can_stop = false;
    CHECK_FALSE(printer_command_allowed(state, target, PrinterTaskAction::Stop));
    state.phase = PrinterTaskPhase::Complete; state.can_stop = true;
    CHECK_FALSE(printer_command_allowed(state, target, PrinterTaskAction::Stop));
}

TEST_CASE("Printing requires a current slice and a ready native device", "[UiRedesign][PrinterWorkspace]")
{
    auto state = printing();
    CHECK_FALSE(printer_can_start(state));
    state.phase = PrinterTaskPhase::Idle;
    REQUIRE(printer_can_start(state));
    state.gcode_ready = false;
    CHECK_FALSE(printer_can_start(state));
    state.gcode_ready = true; state.slicing = true;
    CHECK_FALSE(printer_can_start(state));
    state.slicing = false; state.can_print = false;
    CHECK_FALSE(printer_can_start(state));
    state.can_print = true; state.phase = PrinterTaskPhase::Unknown;
    CHECK_FALSE(printer_can_start(state));
    state.phase = PrinterTaskPhase::Complete;
    CHECK(printer_can_start(state));
    state.phase = PrinterTaskPhase::Failed;
    CHECK(printer_can_start(state));
    state.device_id.clear();
    CHECK_FALSE(printer_can_start(state));
}

TEST_CASE("Configured print hosts retain the existing dispatch path", "[UiRedesign][PrinterWorkspace]")
{
    PrinterWorkspaceSnapshot state;
    state.gcode_ready = true;
    CHECK_FALSE(printer_can_start(state));
    state.configured_host = true;
    CHECK(printer_can_start(state));
    state.phase = PrinterTaskPhase::Paused;
    CHECK_FALSE(printer_can_start(state));
    state.phase = PrinterTaskPhase::Idle; state.native_dispatch = true;
    CHECK_FALSE(printer_can_start(state));
}

TEST_CASE("Slice preparation never starts a printer job", "[UiRedesign][PrinterWorkspace]")
{
    PrinterWorkspaceSnapshot state;
    state.can_slice = true;
    CHECK(printer_can_slice(state));
    CHECK_FALSE(printer_can_start(state));
    state.slicing = true;
    CHECK_FALSE(printer_can_slice(state));
    state.slicing = false; state.gcode_ready = true;
    CHECK_FALSE(printer_can_slice(state));
    state.gcode_ready = false; state.phase = PrinterTaskPhase::Printing;
    CHECK_FALSE(printer_can_slice(state));
}

TEST_CASE("Printer status and missing telemetry stay explicit", "[UiRedesign][PrinterWorkspace]")
{
    CHECK(printer_task_phase("RUNNING") == PrinterTaskPhase::Printing);
    CHECK(printer_task_phase("PAUSE") == PrinterTaskPhase::Paused);
    CHECK(printer_task_phase("PREPARE") == PrinterTaskPhase::Preparing);
    CHECK(printer_task_phase("SLICING") == PrinterTaskPhase::Preparing);
    CHECK(printer_task_phase("FINISH") == PrinterTaskPhase::Complete);
    CHECK(printer_task_phase("FAILED") == PrinterTaskPhase::Failed);
    CHECK(printer_task_phase("") == PrinterTaskPhase::Unknown);
    CHECK(printer_task_phase("future-status") == PrinterTaskPhase::Unknown);
    PrinterWorkspaceSnapshot state;
    CHECK_FALSE(state.progress.has_value());
    CHECK_FALSE(state.remaining_seconds.has_value());
    CHECK_FALSE(state.filament_percent.has_value());
    CHECK_FALSE(state.nozzle_temperature.has_value());
}

TEST_CASE("Printer remaining time uses seconds and rounds partial minutes up", "[UiRedesign][PrinterWorkspace]")
{
    CHECK(printer_duration(0) == "0m");
    CHECK(printer_duration(-1) == "0m");
    CHECK(printer_duration(1) == "1m");
    CHECK(printer_duration(60) == "1m");
    CHECK(printer_duration(61) == "2m");
    CHECK(printer_duration(3600) == "1h 0m");
    CHECK(printer_duration(4320) == "1h 12m");
    CHECK(printer_duration(std::numeric_limits<int>::max()) == "596523h 15m");
}
