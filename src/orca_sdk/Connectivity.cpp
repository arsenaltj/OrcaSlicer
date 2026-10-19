#include "orca/sdk/Connectivity.hpp"

#include <algorithm>
#include <stdexcept>

namespace Orca::SDK {

namespace {

constexpr std::array<ConnectionPath, 3> kPriority{
    ConnectionPath::Lan,
    ConnectionPath::RemoteBroker,
    ConnectionPath::Relay,
};

bool selectable(const PathObservation& observation)
{
    return observation.reachable && observation.authenticated && observation.consecutive_failures == 0;
}

PrinterState printer_state_from_name(const std::string& name)
{
    if (name == "idle") return PrinterState::Idle;
    if (name == "preparing") return PrinterState::Preparing;
    if (name == "printing") return PrinterState::Printing;
    if (name == "paused") return PrinterState::Paused;
    if (name == "completed") return PrinterState::Completed;
    if (name == "error") return PrinterState::Error;
    if (name == "offline") return PrinterState::Offline;
    return PrinterState::Unknown;
}

TransferJobState transfer_job_state_from_name(const std::string& name)
{
    if (name == "uploading") return TransferJobState::Uploading;
    if (name == "paused") return TransferJobState::Paused;
    if (name == "completed") return TransferJobState::Completed;
    if (name == "failed") return TransferJobState::Failed;
    if (name == "canceled") return TransferJobState::Canceled;
    return TransferJobState::Queued;
}

TransferRetryPolicy transfer_retry_policy_from_name(const std::string& name)
{
    if (name == "fixed") return TransferRetryPolicy::Fixed;
    if (name == "exponential_backoff") return TransferRetryPolicy::ExponentialBackoff;
    return TransferRetryPolicy::None;
}

DeviceDownloadState device_download_state_from_name(const std::string& name)
{
    if (name == "pending") return DeviceDownloadState::Pending;
    if (name == "downloading") return DeviceDownloadState::Downloading;
    if (name == "completed") return DeviceDownloadState::Completed;
    if (name == "failed") return DeviceDownloadState::Failed;
    return DeviceDownloadState::Unknown;
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
    m_state_sync_required = false;
}

void ConnectionStateMachine::begin_discovery()
{
    m_state = ConnectionState::Discovering;
}

void ConnectionStateMachine::begin_probe()
{
    m_state = ConnectionState::Probing;
}

void ConnectionStateMachine::begin_pairing()
{
    m_state = ConnectionState::Pairing;
}

void ConnectionStateMachine::begin_authentication()
{
    m_state = ConnectionState::Authenticating;
}

PathSelection ConnectionStateMachine::select_path(
    const std::array<PathObservation, 3>& observations)
{
    const bool had_path = m_selector.has_path();
    const PathSelection selection = m_selector.select(observations);
    if (!selection.available) {
        m_state = ConnectionState::Offline;
        m_state_sync_required = had_path;
    } else if (selection.changed) {
        m_state = had_path ? ConnectionState::Switching : ConnectionState::Connecting;
        m_state_sync_required = selection.requires_state_sync;
    } else if (m_state != ConnectionState::Connected && m_state != ConnectionState::StateSyncing &&
               m_state != ConnectionState::Expired) {
        m_state = ConnectionState::Connecting;
    }
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

void ConnectionStateMachine::begin_state_sync()
{
    m_state = ConnectionState::StateSyncing;
    m_state_sync_required = true;
}

void ConnectionStateMachine::mark_state_synchronized()
{
    m_state = ConnectionState::Connected;
    m_state_sync_required = false;
}

void ConnectionStateMachine::mark_state_expired()
{
    m_state = ConnectionState::Expired;
    m_state_sync_required = true;
}

void ConnectionStateMachine::mark_disconnected()
{
    m_state = ConnectionState::Disconnected;
    m_state_sync_required = false;
}

void ConnectionStateMachine::mark_offline()
{
    m_state = ConnectionState::Offline;
    m_state_sync_required = true;
}

const char* connection_path_name(ConnectionPath path)
{
    switch (path) {
    case ConnectionPath::Lan: return "lan";
    case ConnectionPath::RemoteBroker: return "remote_broker";
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
    case ConnectionState::Pairing: return "pairing";
    case ConnectionState::Authenticating: return "authenticating";
    case ConnectionState::StateSyncing: return "state_syncing";
    case ConnectionState::Expired: return "expired";
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

const char* transfer_job_state_name(TransferJobState state)
{
    switch (state) {
    case TransferJobState::Queued: return "queued";
    case TransferJobState::Uploading: return "uploading";
    case TransferJobState::Paused: return "paused";
    case TransferJobState::Completed: return "completed";
    case TransferJobState::Failed: return "failed";
    case TransferJobState::Canceled: return "canceled";
    }
    return "queued";
}

const char* transfer_retry_policy_name(TransferRetryPolicy policy)
{
    switch (policy) {
    case TransferRetryPolicy::None: return "none";
    case TransferRetryPolicy::Fixed: return "fixed";
    case TransferRetryPolicy::ExponentialBackoff: return "exponential_backoff";
    }
    return "none";
}

const char* device_download_state_name(DeviceDownloadState state)
{
    switch (state) {
    case DeviceDownloadState::Unknown: return "unknown";
    case DeviceDownloadState::Pending: return "pending";
    case DeviceDownloadState::Downloading: return "downloading";
    case DeviceDownloadState::Completed: return "completed";
    case DeviceDownloadState::Failed: return "failed";
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
    else if (name == "remote_broker" || name == "p2p") value = ConnectionPath::RemoteBroker;
    else if (name == "relay") value = ConnectionPath::Relay;
    else throw std::invalid_argument("Unknown connection path: " + name);
}

void to_json(nlohmann::json& json, ConnectionState value)
{
    json = connection_state_name(value);
}

void from_json(const nlohmann::json& json, ConnectionState& value)
{
    const std::string name = json.get<std::string>();
    if (name == "disconnected") value = ConnectionState::Disconnected;
    else if (name == "discovering") value = ConnectionState::Discovering;
    else if (name == "probing") value = ConnectionState::Probing;
    else if (name == "connecting") value = ConnectionState::Connecting;
    else if (name == "connected") value = ConnectionState::Connected;
    else if (name == "switching") value = ConnectionState::Switching;
    else if (name == "offline") value = ConnectionState::Offline;
    else if (name == "pairing") value = ConnectionState::Pairing;
    else if (name == "authenticating") value = ConnectionState::Authenticating;
    else if (name == "state_syncing") value = ConnectionState::StateSyncing;
    else if (name == "expired") value = ConnectionState::Expired;
    else throw std::invalid_argument("Unknown connection state: " + name);
}

void to_json(nlohmann::json& json, PrinterState value)
{
    json = printer_state_name(value);
}

void from_json(const nlohmann::json& json, PrinterState& value)
{
    value = printer_state_from_name(json.get<std::string>());
}

void to_json(nlohmann::json& json, TransferJobState value)
{
    json = transfer_job_state_name(value);
}

void from_json(const nlohmann::json& json, TransferJobState& value)
{
    value = transfer_job_state_from_name(json.get<std::string>());
}

void to_json(nlohmann::json& json, TransferRetryPolicy value)
{
    json = transfer_retry_policy_name(value);
}

void from_json(const nlohmann::json& json, TransferRetryPolicy& value)
{
    value = transfer_retry_policy_from_name(json.get<std::string>());
}

void to_json(nlohmann::json& json, DeviceDownloadState value)
{
    json = device_download_state_name(value);
}

void from_json(const nlohmann::json& json, DeviceDownloadState& value)
{
    value = device_download_state_from_name(json.get<std::string>());
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
        {"device_pairing", value.device_pairing},
        {"certificate_authentication", value.certificate_authentication},
        {"camera", value.camera},
        {"upload_gcode", value.upload_gcode},
        {"resumable_upload", value.resumable_upload},
        {"start_print", value.start_print},
        {"pause_print", value.pause_print},
        {"resume_print", value.resume_print},
        {"stop_print", value.stop_print},
        {"local_discovery", value.local_discovery},
        {"pause_resume", value.pause_resume},
        {"cancel", value.cancel},
    };
}

void from_json(const nlohmann::json& json, PrinterCapability& value)
{
    value.status_stream = json.value("status_stream", false);
    value.device_pairing = json.value("device_pairing", false);
    value.certificate_authentication = json.value("certificate_authentication", false);
    value.camera = json.value("camera", false);
    value.upload_gcode = json.value("upload_gcode", false);
    value.resumable_upload = json.value("resumable_upload", false);
    value.start_print = json.value("start_print", false);
    value.pause_resume = json.value("pause_resume", false);
    value.cancel = json.value("cancel", false);
    value.pause_print = json.value("pause_print", value.pause_resume);
    value.resume_print = json.value("resume_print", value.pause_resume);
    value.stop_print = json.value("stop_print", value.cancel);
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
        {"device_time_epoch_seconds", value.device_time_epoch_seconds},
        {"event_sequence", value.event_sequence},
        {"last_sync_epoch_seconds", value.last_sync_epoch_seconds},
        {"state_expired", value.state_expired},
        {"connection_path", value.connection_path},
        {"protocol_version", value.protocol_version},
    };
}

void from_json(const nlohmann::json& json, PrinterStatus& value)
{
    value.device_id = json.value("device_id", std::string());
    value.state = printer_state_from_name(json.value("state", std::string("unknown")));
    value.progress = json.value("progress", 0.0);
    value.nozzle_temperature_c = json.value("nozzle_temperature_c", 0.0);
    value.bed_temperature_c = json.value("bed_temperature_c", 0.0);
    value.current_file = json.value("current_file", std::string());
    value.diagnostic_code = json.value("diagnostic_code", std::string());
    value.observed_at_epoch_seconds = json.value("observed_at_epoch_seconds", uint64_t{0});
    value.device_time_epoch_seconds = json.value("device_time_epoch_seconds", uint64_t{0});
    value.event_sequence = json.value("event_sequence", uint64_t{0});
    value.last_sync_epoch_seconds = json.value("last_sync_epoch_seconds", uint64_t{0});
    value.state_expired = json.value("state_expired", false);
    value.connection_path = json.value("connection_path", ConnectionPath::Lan);
    value.protocol_version = json.value("protocol_version", std::string());
}

void to_json(nlohmann::json& json, const TransferJob& value)
{
    json = {
        {"file_id", value.file_id},
        {"size_bytes", value.size_bytes},
        {"digest_algorithm", value.digest_algorithm},
        {"digest", value.digest},
        {"uploaded_offset", value.uploaded_offset},
        {"state", value.state},
        {"retry_policy", value.retry_policy},
        {"device_download_state", value.device_download_state},
    };
}

void from_json(const nlohmann::json& json, TransferJob& value)
{
    value.file_id = json.value("file_id", std::string());
    value.size_bytes = json.value("size_bytes", uint64_t{0});
    value.digest_algorithm = json.value("digest_algorithm", std::string("sha256"));
    value.digest = json.value("digest", std::string());
    value.uploaded_offset = json.value("uploaded_offset", uint64_t{0});
    value.state = json.value("state", TransferJobState::Queued);
    value.retry_policy = json.value("retry_policy", TransferRetryPolicy::None);
    value.device_download_state = json.value("device_download_state", DeviceDownloadState::Unknown);
}

void to_json(nlohmann::json& json, const RemoteSession& value)
{
    json = {
        {"device_id", value.device_id},
        {"platform_session", value.platform_session},
        {"device_certificate_id", value.device_certificate_id},
        {"connection_path", value.connection_path},
        {"provider", value.provider},
        {"lease_expires_at_epoch_seconds", value.lease_expires_at_epoch_seconds},
        {"state_sync_required", value.state_sync_required},
        {"state_synced", value.state_synced},
    };
}

void from_json(const nlohmann::json& json, RemoteSession& value)
{
    value.device_id = json.value("device_id", std::string());
    value.platform_session = json.value("platform_session", std::string());
    value.device_certificate_id = json.value("device_certificate_id", std::string());
    value.connection_path = json.value("connection_path", ConnectionPath::Lan);
    value.provider = json.value("provider", std::string());
    value.lease_expires_at_epoch_seconds = json.value("lease_expires_at_epoch_seconds", uint64_t{0});
    value.state_sync_required = json.value("state_sync_required", false);
    value.state_synced = json.value("state_synced", false);
}

} // namespace Orca::SDK
