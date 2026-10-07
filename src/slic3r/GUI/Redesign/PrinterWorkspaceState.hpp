#pragma once

#include "../AI/Orca/OrcaPrintConfirmation.hpp"
#include <algorithm>
#include <optional>
#include <string>

namespace Slic3r::GUI {

enum class PrinterTaskPhase { Idle, Preparing, Printing, Paused, Complete, Failed, Unknown };
enum class PrinterTaskAction { Pause, Resume, Stop };

// Display data only. DeviceManager and Plater remain the owners of device/jobs
// and sliced projects; the new workspace never changes their state optimistically.
struct PrinterWorkspaceSnapshot
{
    std::string device_id;
    std::string device_name;
    std::string task_id;
    std::string task_name;
    std::string gcode_name;
    std::string printer_type;
    OrcaPrintConnectionFacts connection;
    OrcaDeviceMediaState media_state { OrcaDeviceMediaState::ConnectionRequired };
    bool connected { false };
    bool connecting { false };
    bool info_ready { false };
    bool slicing { false };
    bool gcode_ready { false };
    bool can_slice { false };
    bool configured_host { false };
    bool native_dispatch { false };
    bool can_print { false };
    bool can_pause { false };
    bool can_resume { false };
    bool can_stop { false };
    bool has_camera { false };
    bool can_record { false };
    bool recording { false };
    bool can_level { false };
    bool bed_leveling { true };
    bool can_detect { false };
    bool detection_enabled { false };
    bool device_warning { false };
    PrinterTaskPhase phase { PrinterTaskPhase::Idle };
    std::optional<int> progress;
    std::optional<int> nozzle_temperature;
    std::optional<int> bed_temperature;
    std::optional<int> remaining_seconds;
    std::optional<int> estimated_seconds;
    std::optional<int> filament_percent;
};

inline PrinterTaskPhase printer_task_phase(const std::string& status)
{
    if (status == "IDLE") return PrinterTaskPhase::Idle;
    if (status == "PREPARE" || status == "SLICING") return PrinterTaskPhase::Preparing;
    if (status == "RUNNING") return PrinterTaskPhase::Printing;
    if (status == "PAUSE") return PrinterTaskPhase::Paused;
    if (status == "FINISH") return PrinterTaskPhase::Complete;
    if (status == "FAILED") return PrinterTaskPhase::Failed;
    return PrinterTaskPhase::Unknown;
}

inline bool printer_task_active(PrinterTaskPhase phase)
{
    return phase == PrinterTaskPhase::Preparing || phase == PrinterTaskPhase::Printing ||
           phase == PrinterTaskPhase::Paused;
}

inline bool printer_telemetry_ready(const PrinterWorkspaceSnapshot& state)
{
    return state.info_ready && orca_print_connection_state(state.connection) == OrcaPrintConnectionState::Online;
}

// Reviewing a current slice also permits local export when a device is offline.
// Dispatch stays inside the existing revision-bound native confirmation.
inline bool printer_can_review(const PrinterWorkspaceSnapshot& state)
{
    return state.gcode_ready && !state.slicing && !printer_task_active(state.phase);
}

inline bool printer_can_start(const PrinterWorkspaceSnapshot& state)
{
    if (!state.gcode_ready || state.slicing || printer_task_active(state.phase)) return false;
    if (!state.native_dispatch) return state.configured_host;
    return !state.device_id.empty() && state.connected && printer_telemetry_ready(state) && state.can_print &&
           (state.phase == PrinterTaskPhase::Idle || state.phase == PrinterTaskPhase::Complete ||
            state.phase == PrinterTaskPhase::Failed);
}

inline bool printer_can_slice(const PrinterWorkspaceSnapshot& state)
{
    return state.can_slice && !state.gcode_ready && !state.slicing && !printer_task_active(state.phase);
}

struct PrinterCommandTarget
{
    std::string device_id;
    std::string task_id;
};

inline bool printer_command_allowed(const PrinterWorkspaceSnapshot& state,
                                    const PrinterCommandTarget& target, PrinterTaskAction action)
{
    // A modal confirmation can outlive a device selection or a completed job.
    // Recheck the target against a fresh snapshot before sending anything.
    if (target.device_id.empty() || target.task_id.empty() || target.device_id != state.device_id || target.task_id != state.task_id ||
        !state.connected || !printer_telemetry_ready(state)) return false;
    switch (action) {
    case PrinterTaskAction::Pause: return state.phase == PrinterTaskPhase::Printing && state.can_pause;
    case PrinterTaskAction::Resume: return state.phase == PrinterTaskPhase::Paused && state.can_resume;
    case PrinterTaskAction::Stop: return printer_task_active(state.phase) && state.can_stop;
    }
    return false;
}

inline std::string printer_duration(int seconds)
{
    seconds = std::max(0, seconds);
    const int minutes = seconds / 60 + (seconds % 60 != 0);
    if (minutes < 60) return std::to_string(minutes) + "m";
    return std::to_string(minutes / 60) + "h " + std::to_string(minutes % 60) + "m";
}

} // namespace Slic3r::GUI
