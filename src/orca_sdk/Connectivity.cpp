#include "orca/sdk/Connectivity.hpp"

#include <algorithm>
#include <stdexcept>

namespace Orca::SDK {

namespace {

constexpr std::array<ConnectionPath, 3> kPriority{
    ConnectionPath::Lan,
    ConnectionPath::P2p,
    ConnectionPath::Relay,
};

bool selectable(const PathObservation& observation)
{
    return observation.reachable && observation.authenticated && observation.consecutive_failures == 0;
}

} // namespace

void ConnectionPathSelector::reset()
{
    m_current_path = ConnectionPath::Lan;
    m_has_path = false;
}

const PathObservation* ConnectionPathSelector::find(
    const std::array<PathObservation, 3>& observations, ConnectionPath path)
{
    const auto it = std::find_if(observations.begin(), observations.end(), [path](const PathObservation& value) {
        return value.path == path;
    });
    return it == observations.end() ? nullptr : &*it;
}

PathSelection ConnectionPathSelector::select(const std::array<PathObservation, 3>& observations)
{
    const PathObservation* current = m_has_path ? find(observations, m_current_path) : nullptr;
    if (current != nullptr && selectable(*current))
        return {m_current_path, true, false, false};

    const PathObservation* selected = nullptr;
    for (const ConnectionPath path : kPriority) {
        const PathObservation* candidate = find(observations, path);
        if (candidate != nullptr && selectable(*candidate)) {
            selected = candidate;
            break;
        }
    }

    if (selected == nullptr) {
        const bool changed = m_has_path;
        m_has_path = false;
        return {ConnectionPath::Lan, false, changed, changed};
    }

    const bool changed = !m_has_path || m_current_path != selected->path;
    m_current_path = selected->path;
    m_has_path = true;
    return {m_current_path, true, changed, changed};
}

void ConnectionStateMachine::reset()
{
    m_selector.reset();
    m_state = ConnectionState::Disconnected;
}

void ConnectionStateMachine::begin_discovery()
{
    m_state = ConnectionState::Discovering;
}

void ConnectionStateMachine::begin_probe()
{
    m_state = ConnectionState::Probing;
}

PathSelection ConnectionStateMachine::select_path(
    const std::array<PathObservation, 3>& observations)
{
    const bool had_path = m_selector.has_path();
    const PathSelection selection = m_selector.select(observations);
    if (!selection.available)
        m_state = ConnectionState::Offline;
    else if (selection.changed && had_path)
        m_state = ConnectionState::Switching;
    else
        m_state = ConnectionState::Connecting;
    return selection;
}

void ConnectionStateMachine::mark_connecting()
{
    m_state = ConnectionState::Connecting;
}

void ConnectionStateMachine::mark_connected()
{
    m_state = ConnectionState::Connected;
}

void ConnectionStateMachine::mark_disconnected()
{
    m_state = ConnectionState::Disconnected;
}

void ConnectionStateMachine::mark_offline()
{
    m_state = ConnectionState::Offline;
}

const char* connection_path_name(ConnectionPath path)
{
    switch (path) {
    case ConnectionPath::Lan: return "lan";
    case ConnectionPath::P2p: return "p2p";
    case ConnectionPath::Relay: return "relay";
    }
    return "unknown";
}

const char* connection_state_name(ConnectionState state)
{
    switch (state) {
    case ConnectionState::Disconnected: return "disconnected";
    case ConnectionState::Discovering: return "discovering";
    case ConnectionState::Probing: return "probing";
    case ConnectionState::Connecting: return "connecting";
    case ConnectionState::Connected: return "connected";
    case ConnectionState::Switching: return "switching";
    case ConnectionState::Offline: return "offline";
    }
    return "unknown";
}

const char* printer_state_name(PrinterState state)
{
    switch (state) {
    case PrinterState::Unknown: return "unknown";
    case PrinterState::Idle: return "idle";
    case PrinterState::Preparing: return "preparing";
    case PrinterState::Printing: return "printing";
    case PrinterState::Paused: return "paused";
    case PrinterState::Completed: return "completed";
    case PrinterState::Error: return "error";
    case PrinterState::Offline: return "offline";
    }
    return "unknown";
}

void to_json(nlohmann::json& json, ConnectionPath value)
{
    json = connection_path_name(value);
}

void from_json(const nlohmann::json& json, ConnectionPath& value)
{
    const std::string name = json.get<std::string>();
    if (name == "lan") value = ConnectionPath::Lan;
    else if (name == "p2p") value = ConnectionPath::P2p;
    else if (name == "relay") value = ConnectionPath::Relay;
    else throw std::invalid_argument("Unknown connection path: " + name);
}

void to_json(nlohmann::json& json, const DeviceEndpoint& value)
{
    json = {
        {"scheme", value.scheme},
        {"host", value.host},
        {"port", value.port},
        {"tls", value.tls},
        {"certificate_fingerprint", value.certificate_fingerprint},
    };
}

void from_json(const nlohmann::json& json, DeviceEndpoint& value)
{
    value.scheme = json.value("scheme", std::string());
    value.host = json.value("host", std::string());
    value.port = json.value("port", uint16_t{0});
    value.tls = json.value("tls", false);
    value.certificate_fingerprint = json.value("certificate_fingerprint", std::string());
}

void to_json(nlohmann::json& json, const PrinterCapability& value)
{
    json = {
        {"status_stream", value.status_stream},
        {"camera", value.camera},
        {"upload_gcode", value.upload_gcode},
        {"start_print", value.start_print},
        {"pause_resume", value.pause_resume},
        {"cancel", value.cancel},
        {"local_discovery", value.local_discovery},
    };
}

void from_json(const nlohmann::json& json, PrinterCapability& value)
{
    value.status_stream = json.value("status_stream", false);
    value.camera = json.value("camera", false);
    value.upload_gcode = json.value("upload_gcode", false);
    value.start_print = json.value("start_print", false);
    value.pause_resume = json.value("pause_resume", false);
    value.cancel = json.value("cancel", false);
    value.local_discovery = json.value("local_discovery", false);
}

void to_json(nlohmann::json& json, const PrinterStatus& value)
{
    json = {
        {"device_id", value.device_id},
        {"state", printer_state_name(value.state)},
        {"progress", value.progress},
        {"nozzle_temperature_c", value.nozzle_temperature_c},
        {"bed_temperature_c", value.bed_temperature_c},
        {"current_file", value.current_file},
        {"diagnostic_code", value.diagnostic_code},
        {"observed_at_epoch_seconds", value.observed_at_epoch_seconds},
    };
}

void from_json(const nlohmann::json& json, PrinterStatus& value)
{
    value.device_id = json.value("device_id", std::string());
    const std::string state = json.value("state", std::string("unknown"));
    if (state == "idle") value.state = PrinterState::Idle;
    else if (state == "preparing") value.state = PrinterState::Preparing;
    else if (state == "printing") value.state = PrinterState::Printing;
    else if (state == "paused") value.state = PrinterState::Paused;
    else if (state == "completed") value.state = PrinterState::Completed;
    else if (state == "error") value.state = PrinterState::Error;
    else if (state == "offline") value.state = PrinterState::Offline;
    else value.state = PrinterState::Unknown;
    value.progress = json.value("progress", 0.0);
    value.nozzle_temperature_c = json.value("nozzle_temperature_c", 0.0);
    value.bed_temperature_c = json.value("bed_temperature_c", 0.0);
    value.current_file = json.value("current_file", std::string());
    value.diagnostic_code = json.value("diagnostic_code", std::string());
    value.observed_at_epoch_seconds = json.value("observed_at_epoch_seconds", uint64_t{0});
}

} // namespace Orca::SDK
