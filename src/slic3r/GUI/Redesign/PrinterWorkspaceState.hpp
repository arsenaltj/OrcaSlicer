#pragma once

#include "../AI/Orca/OrcaPrintConfirmation.hpp"
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace Slic3r::GUI {

enum class PrinterTaskPhase { Idle, Preparing, Printing, Paused, Complete, Failed, Unknown };
enum class PrinterTaskAction { Pause, Resume, Stop };
enum class PrinterDeviceConnectionState { Offline, Available, Connecting, Connected, Ready };

enum class PrinterDiscoveryTrigger { Activation, Picker, Retry };
enum class PrinterDiscoveryPhase { Idle, Discovering, DevicesAvailable, Failed, TimedOut };
enum class PrinterDiscoveryError {
    None,
    DeviceManagerUnavailable,
    NetworkAgentUnavailable,
    PrinterAgentUnavailable,
    AgentMismatch,
    DiscoveryUnsupported,
    RequestRejected,
    AgentChanged,
    SelectionChanged,
    BackendEmpty,
    NoEligibleInterface,
    AuthenticationRequired,
    ProtocolMismatch,
    BackendNetworkFailure,
    BackendTimeout,
    BackendInternalError
};

inline bool printer_discovery_agent_matches(const std::string& network_agent_id,
                                            const std::string& manager_agent_id)
{
    return !network_agent_id.empty() && network_agent_id == manager_agent_id;
}

inline bool printer_discovery_device_matches(const std::string& request_agent_id,
                                             const std::string& current_agent_id,
                                             const std::string& device_agent_id)
{
    return printer_discovery_agent_matches(request_agent_id, current_agent_id) &&
           request_agent_id == device_agent_id;
}

struct PrinterDiscoveryState
{
    void begin(std::uint64_t id, PrinterDiscoveryTrigger source, std::string agent, size_t count)
    {
        request_id = id;
        trigger = source;
        phase = PrinterDiscoveryPhase::Discovering;
        error = PrinterDiscoveryError::None;
        agent_id = std::move(agent);
        before_count = count;
        current_count = count;
    }

    void reject(PrinterDiscoveryError reason)
    {
        error = reason;
        phase = PrinterDiscoveryPhase::Failed;
    }

    void observe(const std::string& current_agent, size_t count,
                 std::chrono::milliseconds elapsed, std::chrono::milliseconds timeout)
    {
        if (phase != PrinterDiscoveryPhase::Discovering &&
            phase != PrinterDiscoveryPhase::DevicesAvailable &&
            phase != PrinterDiscoveryPhase::TimedOut)
            return;
        current_count = count;
        if (current_agent != agent_id) {
            reject(PrinterDiscoveryError::AgentChanged);
            return;
        }
        if (phase == PrinterDiscoveryPhase::TimedOut && count > 0) {
            current_count = count;
            error = PrinterDiscoveryError::None;
            phase = PrinterDiscoveryPhase::DevicesAvailable;
            return;
        }
        if (phase == PrinterDiscoveryPhase::TimedOut)
            return;
        if (count > 0) {
            phase = PrinterDiscoveryPhase::DevicesAvailable;
            return;
        }
        if (elapsed >= timeout)
            phase = PrinterDiscoveryPhase::TimedOut;
    }

    bool retryable() const
    {
        return phase == PrinterDiscoveryPhase::Failed || phase == PrinterDiscoveryPhase::TimedOut;
    }

    std::uint64_t request_id {0};
    PrinterDiscoveryTrigger trigger {PrinterDiscoveryTrigger::Activation};
    PrinterDiscoveryPhase phase {PrinterDiscoveryPhase::Idle};
    PrinterDiscoveryError error {PrinterDiscoveryError::None};
    std::string agent_id;
    size_t before_count {0};
    size_t current_count {0};
};

inline PrinterDeviceConnectionState printer_device_connection_state(
    bool online, bool connecting, bool connected, bool information_ready)
{
    if (connecting)
        return PrinterDeviceConnectionState::Connecting;
    if (connected && information_ready)
        return PrinterDeviceConnectionState::Ready;
    if (connected)
        return PrinterDeviceConnectionState::Connected;
    if (online)
        return PrinterDeviceConnectionState::Available;
    return PrinterDeviceConnectionState::Offline;
}

inline std::string printer_device_display_address(std::string address)
{
    const size_t scheme = address.find("://");
    if (scheme != std::string::npos)
        address.erase(0, scheme + 3);
    const size_t path = address.find_first_of("/?#");
    if (path != std::string::npos)
        address.erase(path);
    if (!address.empty() && address.front() == '[') {
        const size_t closing = address.find(']');
        if (closing != std::string::npos)
            return address.substr(1, closing - 1);
    }
    const size_t colon = address.rfind(':');
    if (colon != std::string::npos && address.find(':') == colon && colon + 1 < address.size()) {
        bool numeric_port = true;
        for (size_t i = colon + 1; i < address.size(); ++i)
            numeric_port = numeric_port && address[i] >= '0' && address[i] <= '9';
        if (numeric_port)
            address.erase(colon);
    }
    return address;
}

inline std::string printer_device_display_name(const std::string& name, const std::string& address)
{
    const std::string display_address = printer_device_display_address(address);
    if (name.empty())
        return display_address;
    if (display_address.empty() || name.find(display_address) != std::string::npos)
        return name;
    return name + " · " + display_address;
}

// Display data only. DeviceManager and Plater remain the owners of device/jobs
// and sliced projects; the new workspace never changes their state optimistically.
struct PrinterWorkspaceSnapshot
{
    std::string device_id;
    std::string device_name;
    std::string device_address;
    std::string preset_name;
    std::string active_agent_id;
    std::string task_id;
    std::string task_name;
    std::string gcode_name;
    std::string printer_type;
    OrcaPrintConnectionFacts connection;
    OrcaDeviceMediaState media_state { OrcaDeviceMediaState::ConnectionRequired };
    bool connected { false };
    bool connecting { false };
    bool device_selected { false };
    bool device_online { false };
    bool device_info_ready { false };
    bool info_ready { false };
    bool slicing { false };
    bool gcode_ready { false };
    bool can_slice { false };
    bool configured_host { false };
    bool native_dispatch { false };
    bool transfer_target_ready { false };
    bool supports_upload_only { false };
    bool supports_upload_and_print { false };
    bool requires_task_identity_for_control { true };
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
    std::optional<int> nozzle_target_temperature;
    std::optional<int> bed_temperature;
    std::optional<int> bed_target_temperature;
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
    return state.gcode_ready && !state.slicing;
}

inline bool printer_transfer_target_ready(const PrinterWorkspaceSnapshot& state)
{
    if (!state.native_dispatch || state.device_id.empty() || !state.transfer_target_ready)
        return false;
    // Printer-agent snapshots already fold online, connected and information
    // readiness into transfer_target_ready. Native telemetry backends must also
    // reject a stale or future-dated status sample.
    return !state.connection.native_backend || printer_telemetry_ready(state);
}

inline bool printer_can_start(const PrinterWorkspaceSnapshot& state)
{
    if (!state.gcode_ready || state.slicing || printer_task_active(state.phase)) return false;
    if (!state.native_dispatch) return state.configured_host;
    if (!printer_transfer_target_ready(state)) return false;
    return state.supports_upload_and_print && state.can_print &&
           (state.phase == PrinterTaskPhase::Idle || state.phase == PrinterTaskPhase::Complete ||
            state.phase == PrinterTaskPhase::Failed);
}

inline bool printer_can_upload(const PrinterWorkspaceSnapshot& state)
{
    return state.gcode_ready && !state.slicing && printer_transfer_target_ready(state) &&
           state.supports_upload_only;
}

inline bool printer_can_upload_and_print(const PrinterWorkspaceSnapshot& state)
{
    return state.gcode_ready && !state.slicing && printer_transfer_target_ready(state) &&
           state.supports_upload_and_print && state.can_print &&
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
    if (target.device_id.empty() || target.device_id != state.device_id || !state.connected ||
        !printer_telemetry_ready(state)) return false;
    if (state.requires_task_identity_for_control &&
        (target.task_id.empty() || target.task_id != state.task_id)) return false;
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
