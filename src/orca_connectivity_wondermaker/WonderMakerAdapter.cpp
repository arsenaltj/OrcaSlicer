#include "orca/connectivity/wondermaker/WonderMakerAdapter.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>

#include <nlohmann/json.hpp>

namespace Orca::SDK::WonderMaker {

namespace {

constexpr uint64_t kStatusStaleAfterSeconds = 10;
constexpr uint64_t kPollingIntervalSeconds = 2;

bool is_object(const nlohmann::json& value)
{
    return value.is_object();
}

} // namespace

const char* monitor_state_name(MonitorState state)
{
    switch (state) {
    case MonitorState::Disconnected: return "disconnected";
    case MonitorState::Discovering: return "discovering";
    case MonitorState::Probing: return "probing";
    case MonitorState::Pairing: return "pairing";
    case MonitorState::Authenticating: return "authenticating";
    case MonitorState::Connecting: return "connecting";
    case MonitorState::Syncing: return "syncing";
    case MonitorState::Connected: return "connected";
    case MonitorState::Reconnecting: return "reconnecting";
    case MonitorState::Stale: return "stale";
    case MonitorState::Offline: return "offline";
    }
    return "unknown";
}

const char* control_operation_name(ControlOperation operation)
{
    switch (operation) {
    case ControlOperation::Start: return "start";
    case ControlOperation::Pause: return "pause";
    case ControlOperation::Resume: return "resume";
    case ControlOperation::Stop: return "stop";
    }
    return "unknown";
}

WonderMakerAdapter::WonderMakerAdapter(std::shared_ptr<IWonderMakerHttpTransport> http,
                                       std::shared_ptr<IWonderMakerWebSocketTransport> websocket,
                                       std::shared_ptr<IWonderMakerDiscoveryTransport> discovery,
                                       Clock clock)
    : m_http(std::move(http))
    , m_websocket(std::move(websocket))
    , m_discovery(std::move(discovery))
    , m_clock(clock ? std::move(clock) : &WonderMakerAdapter::default_now)
{
}

WonderMakerAdapter::~WonderMakerAdapter()
{
    disconnect();
}

uint64_t WonderMakerAdapter::default_now()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

const char* WonderMakerAdapter::id() const
{
    return "wondermaker";
}

const char* WonderMakerAdapter::control_operation_name(ControlOperation operation)
{
    return WonderMaker::control_operation_name(operation);
}

std::vector<DiscoveryCandidate> WonderMakerAdapter::discover()
{
    m_monitor_state = MonitorState::Discovering;
    if (!m_discovery) {
        m_monitor_state = MonitorState::Disconnected;
        m_last_result = success_result();
        return {};
    }

    auto candidates = m_discovery->discover();
    m_monitor_state = MonitorState::Disconnected;
    m_last_result = success_result();
    return candidates;
}

void WonderMakerAdapter::set_manual_endpoint(DeviceEndpoint endpoint)
{
    disconnect();
    m_endpoint = std::move(endpoint);
    m_paired = false;
    m_authenticated = false;
    m_snapshot = {};
    m_command_receipts.clear();
}

bool WonderMakerAdapter::probe(const DeviceEndpoint& endpoint)
{
    set_manual_endpoint(endpoint);
    m_monitor_state = MonitorState::Probing;

    WonderMakerHttpRequest request;
    request.resource = HttpResource::Capabilities;
    request.endpoint = endpoint;
    const auto response = this->request(std::move(request));
    if (!response_succeeded(response) || !apply_capabilities_body(response.body)) {
        m_monitor_state = MonitorState::Offline;
        if (m_last_result.success)
            m_last_result = result_from_response(response);
        return false;
    }

    m_monitor_state = MonitorState::Disconnected;
    m_last_result = success_result();
    return true;
}

PrinterCapability WonderMakerAdapter::capabilities() const
{
    return m_capabilities;
}

PrinterStatus WonderMakerAdapter::get_status()
{
    if (!m_snapshot.last_synced_at_epoch_seconds)
        poll_status();
    return m_snapshot.status;
}

PrinterOperationResult WonderMakerAdapter::pair(const std::string& opaque_credential)
{
    if (!ready_for_http())
        return m_last_result = failure_result("not_ready", "an endpoint and HTTP transport are required", false);

    m_monitor_state = MonitorState::Pairing;
    WonderMakerHttpRequest request;
    request.resource = HttpResource::Pair;
    request.endpoint = *m_endpoint;
    request.credential = opaque_credential;
    const auto response = this->request(std::move(request));
    m_last_result = result_from_response(response);
    if (m_last_result.success) {
        m_paired = true;
        m_monitor_state = MonitorState::Disconnected;
    } else {
        m_monitor_state = MonitorState::Offline;
    }
    return m_last_result;
}

PrinterOperationResult WonderMakerAdapter::authenticate(const std::string& opaque_credential,
                                                        const std::string& certificate_fingerprint)
{
    if (!ready_for_http())
        return m_last_result = failure_result("not_ready", "an endpoint and HTTP transport are required", false);
    if (!m_paired)
        return m_last_result = failure_result("not_paired", "pairing is required before authentication", false);

    m_monitor_state = MonitorState::Authenticating;
    WonderMakerHttpRequest request;
    request.resource = HttpResource::Authenticate;
    request.endpoint = *m_endpoint;
    request.credential = opaque_credential;
    request.certificate_fingerprint = certificate_fingerprint;
    const auto response = this->request(std::move(request));
    m_last_result = result_from_response(response);
    if (m_last_result.success) {
        m_authenticated = true;
        m_monitor_state = MonitorState::Disconnected;
    } else {
        m_monitor_state = MonitorState::Offline;
    }
    return m_last_result;
}

PrinterOperationResult WonderMakerAdapter::connect()
{
    if (!ready_for_http())
        return m_last_result = failure_result("not_ready", "an endpoint and HTTP transport are required", false);
    if (!m_paired)
        return m_last_result = failure_result("not_paired", "pairing is required before connection", false);
    if (!m_authenticated)
        return m_last_result = failure_result("not_authenticated", "authentication is required before connection", false);

    m_monitor_state = MonitorState::Connecting;
    m_connected = true;
    m_polling_fallback = false;

    if (m_websocket) {
        m_websocket_connected = m_websocket->connect(
            *m_endpoint,
            [this](const std::string& payload) { handle_websocket_message(payload); },
            [this]() { on_websocket_closed(); });
        if (!m_websocket_connected) {
            m_polling_fallback = true;
            m_next_poll_at_epoch_seconds = m_clock();
        }
    } else {
        m_polling_fallback = true;
        m_next_poll_at_epoch_seconds = m_clock();
    }

    m_monitor_state = MonitorState::Syncing;
    if (!reconcile_status()) {
        m_connected = false;
        m_monitor_state = MonitorState::Offline;
        return m_last_result;
    }

    if (m_websocket_connected && !subscribe_status()) {
        m_websocket_connected = false;
        m_polling_fallback = true;
        m_next_poll_at_epoch_seconds = m_clock();
    }

    m_monitor_state = MonitorState::Connected;
    m_last_result = success_result();
    return m_last_result;
}

void WonderMakerAdapter::disconnect()
{
    if (m_websocket && m_websocket_connected)
        m_websocket->close();
    m_websocket_connected = false;
    m_polling_fallback = false;
    m_connected = false;
    m_monitor_state = MonitorState::Disconnected;
}

bool WonderMakerAdapter::reconnect_and_reconcile()
{
    if (!m_connected && !ready_for_http()) {
        m_last_result = failure_result("not_ready", "an endpoint and HTTP transport are required", false);
        m_monitor_state = MonitorState::Offline;
        return false;
    }
    if (!m_authenticated) {
        m_last_result = failure_result("not_authenticated", "authentication is required before reconnection", false);
        m_monitor_state = MonitorState::Offline;
        return false;
    }

    m_monitor_state = MonitorState::Reconnecting;
    m_connected = true;
    if (m_websocket) {
        m_websocket_connected = m_websocket->connect(
            *m_endpoint,
            [this](const std::string& payload) { handle_websocket_message(payload); },
            [this]() { on_websocket_closed(); });
    }
    m_polling_fallback = !m_websocket_connected;

    m_monitor_state = MonitorState::Syncing;
    if (!reconcile_status()) {
        m_monitor_state = MonitorState::Stale;
        m_polling_fallback = true;
        m_next_poll_at_epoch_seconds = m_clock();
        return false;
    }
    if (m_websocket_connected && !subscribe_status()) {
        m_websocket_connected = false;
        m_polling_fallback = true;
    }
    m_monitor_state = MonitorState::Connected;
    m_last_result = success_result();
    return true;
}

bool WonderMakerAdapter::subscribe_status()
{
    if (!m_websocket_connected)
        return false;
    const nlohmann::json payload{{"type", "subscribe"}, {"stream", "status"}};
    if (!m_websocket->send(payload.dump())) {
        m_last_result = failure_result("transport_error", "status subscription failed", true);
        return false;
    }
    return true;
}

bool WonderMakerAdapter::handle_websocket_message(const std::string& payload)
{
    const auto json = nlohmann::json::parse(payload, nullptr, false);
    if (!is_object(json) || json.value("type", std::string()) != "status") {
        m_last_result = failure_result("protocol_error", "status event is not a canonical status object", false);
        return false;
    }
    const auto status = json.find("status");
    if (status == json.end() || !status->is_object()) {
        m_last_result = failure_result("protocol_error", "status event has no status object", false);
        return false;
    }

    const uint64_t sequence = json.value("sequence", uint64_t{0});
    const bool applied = apply_status_body(nlohmann::json{{"status", *status}, {"sequence", sequence}}.dump(), false);
    if (applied && m_connected)
        m_monitor_state = MonitorState::Connected;
    return applied;
}

bool WonderMakerAdapter::poll_status()
{
    if (!ready_for_http()) {
        m_last_result = failure_result("not_ready", "an endpoint and HTTP transport are required", false);
        return false;
    }

    WonderMakerHttpRequest request;
    request.resource = HttpResource::Status;
    request.endpoint = *m_endpoint;
    const auto response = this->request(std::move(request));
    if (!response_succeeded(response) || !apply_status_body(response.body, false)) {
        m_last_result = result_from_response(response);
        return false;
    }
    m_next_poll_at_epoch_seconds = m_clock() + kPollingIntervalSeconds;
    if (m_connected && !m_websocket_connected)
        m_monitor_state = MonitorState::Connected;
    return true;
}

bool WonderMakerAdapter::reconcile_status()
{
    if (!ready_for_http()) {
        m_last_result = failure_result("not_ready", "an endpoint and HTTP transport are required", false);
        return false;
    }

    WonderMakerHttpRequest request;
    request.resource = HttpResource::Status;
    request.endpoint = *m_endpoint;
    const auto response = this->request(std::move(request));
    if (!response_succeeded(response) || !apply_status_body(response.body, true)) {
        m_last_result = result_from_response(response);
        return false;
    }
    return true;
}

void WonderMakerAdapter::tick()
{
    if (!m_connected)
        return;

    const auto now = m_clock();
    if (!m_snapshot.last_synced_at_epoch_seconds ||
        now >= m_snapshot.last_synced_at_epoch_seconds + kStatusStaleAfterSeconds) {
        m_snapshot.stale = true;
        m_monitor_state = MonitorState::Stale;
    }

    if (m_polling_fallback && now >= m_next_poll_at_epoch_seconds) {
        if (poll_status()) {
            m_snapshot.stale = false;
            if (m_monitor_state == MonitorState::Stale || m_monitor_state == MonitorState::Reconnecting)
                m_monitor_state = MonitorState::Connected;
        }
    }
}

uint64_t WonderMakerAdapter::status_age_seconds() const
{
    if (!m_snapshot.last_synced_at_epoch_seconds)
        return std::numeric_limits<uint64_t>::max();
    const auto now = m_clock();
    return now >= m_snapshot.last_synced_at_epoch_seconds
               ? now - m_snapshot.last_synced_at_epoch_seconds
               : 0;
}

ControlReceipt WonderMakerAdapter::send_control(const ControlRequest& control)
{
    const auto existing = m_command_receipts.find(control.command_id);
    if (!control.command_id.empty() && existing != m_command_receipts.end()) {
        auto duplicate = existing->second;
        duplicate.duplicate = true;
        return duplicate;
    }

    ControlReceipt receipt;
    receipt.command_id = control.command_id;
    if (control.command_id.empty()) {
        receipt.result = failure_result("command_id_required", "command_id is required", false);
        m_last_result = receipt.result;
        return receipt;
    }
    if (!ready_for_control()) {
        receipt.result = failure_result("not_connected", "a paired authenticated connection is required", true);
        m_last_result = receipt.result;
        return receipt;
    }

    WonderMakerHttpRequest request;
    request.resource = HttpResource::Control;
    request.endpoint = *m_endpoint;
    request.command_id = control.command_id;
    request.control_operation = control.operation;
    request.body = nlohmann::json{
        {"command_id", control.command_id},
        {"operation", control_operation_name(control.operation)},
        {"file_uri", control.file_uri},
        {"filename", control.filename},
        {"file_sha256", control.file_sha256},
    }.dump();

    const auto response = this->request(std::move(request));
    receipt.result = result_from_response(response);
    if (response_succeeded(response)) {
        const auto json = nlohmann::json::parse(response.body, nullptr, false);
        if (is_object(json))
            receipt.duplicate = json.value("duplicate", false);
    }
    m_command_receipts[control.command_id] = receipt;
    m_last_result = receipt.result;
    return receipt;
}

FileTransferReceipt WonderMakerAdapter::upload_file(const FileTransferRequest& transfer)
{
    FileTransferReceipt receipt;
    if (!ready_for_control()) {
        receipt.code = "not_connected";
        receipt.message = "a paired authenticated connection is required";
        m_last_result = failure_result(receipt.code, receipt.message, true);
        return receipt;
    }

    WonderMakerHttpRequest request;
    request.resource = HttpResource::FileTransfer;
    request.endpoint = *m_endpoint;
    request.body = transfer.chunk;
    request.source_uri = transfer.source_uri;
    request.filename = transfer.filename;
    request.sha256 = transfer.sha256;
    request.offset = transfer.offset;
    request.total_size = transfer.total_size;
    const auto response = this->request(std::move(request));
    if (!response_succeeded(response)) {
        const auto result = result_from_response(response);
        receipt.code = result.code;
        receipt.message = result.message;
        m_last_result = result;
        return receipt;
    }

    const auto json = nlohmann::json::parse(response.body, nullptr, false);
    if (!is_object(json)) {
        receipt.code = "protocol_error";
        receipt.message = "file transfer response is not an object";
        m_last_result = failure_result(receipt.code, receipt.message, false);
        return receipt;
    }
    receipt.success = json.value("accepted", true);
    receipt.complete = json.value("complete", false);
    receipt.digest_verified = json.value("digest_verified", false);
    receipt.next_offset = json.value("next_offset", uint64_t{0});
    receipt.code = json.value("code", receipt.success ? std::string("accepted") : std::string("rejected"));
    receipt.message = json.value("message", std::string());
    m_last_result = receipt.success ? success_result() : failure_result(receipt.code, receipt.message, false);
    return receipt;
}

PrinterOperationResult WonderMakerAdapter::upload_gcode(const GcodeUploadRequest& request)
{
    const auto receipt = upload_file({request.source_uri, request.filename, request.sha256, {}, 0, 0});
    return receipt.success ? success_result() : m_last_result;
}

PrinterOperationResult WonderMakerAdapter::start_print(const PrintCommand& command)
{
    const auto suffix = !command.file_sha256.empty() ? command.file_sha256 : command.filename;
    const auto receipt = send_control({stable_command_id("start", suffix), ControlOperation::Start,
                                       command.file_uri, command.filename, command.file_sha256});
    return receipt.result;
}

PrinterOperationResult WonderMakerAdapter::pause()
{
    return send_control({stable_command_id("pause", {}), ControlOperation::Pause, {}, {}, {}}).result;
}

PrinterOperationResult WonderMakerAdapter::resume()
{
    return send_control({stable_command_id("resume", {}), ControlOperation::Resume, {}, {}, {}}).result;
}

PrinterOperationResult WonderMakerAdapter::cancel()
{
    return send_control({stable_command_id("stop", {}), ControlOperation::Stop, {}, {}, {}}).result;
}

PrinterOperationResult WonderMakerAdapter::success_result()
{
    return {true, false, "ok", {}};
}

PrinterOperationResult WonderMakerAdapter::failure_result(std::string code,
                                                          std::string message,
                                                          bool retryable)
{
    return {false, retryable, std::move(code), std::move(message)};
}

bool WonderMakerAdapter::response_succeeded(const WonderMakerHttpResponse& response)
{
    return response.transport_ok && response.status_code >= 200 && response.status_code < 300;
}

PrinterOperationResult WonderMakerAdapter::result_from_response(const WonderMakerHttpResponse& response)
{
    if (!response.transport_ok)
        return failure_result("transport_error", response.error.empty() ? "transport request failed" : response.error, true);

    const auto json = nlohmann::json::parse(response.body, nullptr, false);
    if (response.status_code < 200 || response.status_code >= 300) {
        if (is_object(json) && json.contains("code"))
            return failure_result(json.value("code", std::string("http_error")),
                                  json.value("message", response.error.empty() ? std::string("HTTP request failed") : response.error),
                                  json.value("retryable", response.status_code >= 500));
        return failure_result("http_error", response.error.empty() ? "HTTP request failed" : response.error, response.status_code >= 500);
    }
    if (is_object(json) && json.contains("ok") && !json.value("ok", false))
        return failure_result(json.value("code", std::string("rejected")),
                              json.value("message", std::string("request rejected")),
                              json.value("retryable", response.status_code >= 500));
    if (!is_object(json))
        return success_result();
    return {true, false, json.value("code", std::string("ok")), json.value("message", std::string())};
}

std::string WonderMakerAdapter::stable_command_id(const char* operation, const std::string& suffix)
{
    return std::string(operation) + (suffix.empty() ? std::string() : ":" + suffix);
}

WonderMakerHttpResponse WonderMakerAdapter::request(WonderMakerHttpRequest request)
{
    if (!m_http)
        return {0, {}, "HTTP transport is not configured", false};
    return m_http->request(request);
}

bool WonderMakerAdapter::apply_status_body(const std::string& body, bool reconciled)
{
    const auto json = nlohmann::json::parse(body, nullptr, false);
    if (!is_object(json)) {
        m_last_result = failure_result("protocol_error", "status response is not an object", false);
        return false;
    }
    const auto status = json.find("status");
    if (status == json.end() || !status->is_object()) {
        m_last_result = failure_result("protocol_error", "status response has no status object", false);
        return false;
    }

    try {
        m_snapshot.status = status->get<PrinterStatus>();
    } catch (const std::exception&) {
        m_last_result = failure_result("protocol_error", "status response has invalid canonical fields", false);
        return false;
    }
    const uint64_t sequence = json.value("sequence", uint64_t{0});
    mark_status_received(sequence, reconciled);
    m_last_result = success_result();
    return true;
}

bool WonderMakerAdapter::apply_capabilities_body(const std::string& body)
{
    const auto json = nlohmann::json::parse(body, nullptr, false);
    if (!is_object(json)) {
        m_last_result = failure_result("protocol_error", "capabilities response is not an object", false);
        return false;
    }
    try {
        m_capabilities = json.get<PrinterCapability>();
    } catch (const std::exception&) {
        m_last_result = failure_result("protocol_error", "capabilities response has invalid canonical fields", false);
        return false;
    }
    return true;
}

void WonderMakerAdapter::on_websocket_closed()
{
    m_websocket_connected = false;
    if (!m_connected)
        return;
    m_polling_fallback = true;
    m_next_poll_at_epoch_seconds = m_clock();
    m_monitor_state = MonitorState::Reconnecting;
}

void WonderMakerAdapter::mark_status_received(uint64_t event_sequence, bool reconciled)
{
    m_snapshot.event_sequence = event_sequence;
    m_snapshot.last_synced_at_epoch_seconds = m_clock();
    m_snapshot.stale = false;
    if (reconciled)
        m_last_reconciled_at_epoch_seconds = m_snapshot.last_synced_at_epoch_seconds;
}

bool WonderMakerAdapter::ready_for_http() const
{
    return m_http != nullptr && m_endpoint.has_value();
}

bool WonderMakerAdapter::ready_for_control() const
{
    return ready_for_http() && m_connected && m_paired && m_authenticated;
}

ControlReceipt WonderMakerAdapter::duplicate_receipt(const ControlRequest& request) const
{
    const auto it = m_command_receipts.find(request.command_id);
    if (it == m_command_receipts.end())
        return {request.command_id, failure_result("command_not_found", "command receipt is not cached", false), true};
    auto receipt = it->second;
    receipt.duplicate = true;
    return receipt;
}

WonderMakerSimulator::WonderMakerSimulator(DeviceEndpoint endpoint, std::string device_id)
    : m_endpoint(std::move(endpoint))
    , m_device_id(std::move(device_id))
{
    m_capability.status_stream = true;
    m_capability.camera = true;
    m_capability.upload_gcode = true;
    m_capability.start_print = true;
    m_capability.pause_resume = true;
    m_capability.cancel = true;
    m_capability.local_discovery = true;
    m_status.device_id = m_device_id;
    m_status.state = PrinterState::Idle;
}

WonderMakerHttpResponse WonderMakerSimulator::request(const WonderMakerHttpRequest& request)
{
    m_requests.push_back(request);
    if (!m_http_available)
        return {0, {}, "simulator HTTP transport unavailable", false};
    switch (request.resource) {
    case HttpResource::Pair:
        if (request.credential.empty())
            return failure(400, "credential_required");
        m_paired = true;
        return response(200, R"({"ok":true,"code":"paired"})");
    case HttpResource::Authenticate:
        if (!m_paired)
            return failure(401, "not_paired");
        if (request.credential.empty())
            return failure(401, "credential_required");
        m_authenticated = true;
        return response(200, R"({"ok":true,"code":"authenticated"})");
    case HttpResource::Capabilities:
        return response(200, nlohmann::json(m_capability).dump());
    case HttpResource::Status:
        ++m_status_queries;
        return response(200, nlohmann::json{{"status", m_status}, {"sequence", m_status_sequence}}.dump());
    case HttpResource::Control: {
        const auto body = nlohmann::json::parse(request.body, nullptr, false);
        if (!is_object(body) || body.value("command_id", std::string()).empty())
            return failure(400, "command_id_required");
        const auto command_id = body.value("command_id", std::string());
        const auto existing = m_control_receipts.find(command_id);
        if (existing != m_control_receipts.end()) {
            return response(200, nlohmann::json{{"ok", existing->second.result.success},
                                                {"code", existing->second.result.code},
                                                {"message", existing->second.result.message},
                                                {"duplicate", true}}.dump());
        }
        const auto operation = body.value("operation", std::string());
        PrinterOperationResult result{true, false, "accepted", {}};
        if (operation == "start") m_status.state = PrinterState::Printing;
        else if (operation == "pause") m_status.state = PrinterState::Paused;
        else if (operation == "resume") m_status.state = PrinterState::Printing;
        else if (operation == "stop") m_status.state = PrinterState::Idle;
        else result = {false, false, "unsupported_operation", "unknown canonical control operation"};
        ++m_control_counts[command_id];
        if (result.success) {
            ++m_status_sequence;
            m_control_receipts.emplace(command_id, ControlReceipt{command_id, result, false});
        }
        return response(200, nlohmann::json{{"ok", result.success},
                                            {"code", result.code},
                                            {"message", result.message},
                                            {"duplicate", false}}.dump());
    }
    case HttpResource::FileTransfer: {
        ++m_file_transfer_requests;
        if (request.offset != m_accepted_file_offset)
            return response(409, nlohmann::json{{"accepted", false}, {"complete", false},
                                                {"next_offset", m_accepted_file_offset},
                                                {"code", "offset_mismatch"},
                                                {"message", "resume offset does not match"}}.dump());
        const auto chunk_size = static_cast<uint64_t>(request.body.size());
        m_accepted_file_offset += chunk_size;
        const bool complete = request.total_size == 0 || m_accepted_file_offset >= request.total_size;
        const bool digest_verified = complete &&
            (m_expected_file_digest.empty() || request.sha256 == m_expected_file_digest);
        if (complete && !digest_verified)
            return response(422, nlohmann::json{{"accepted", false}, {"complete", true},
                                                {"digest_verified", false},
                                                {"next_offset", m_accepted_file_offset},
                                                {"code", "digest_mismatch"},
                                                {"message", "file digest did not match"}}.dump());
        return response(200, nlohmann::json{{"accepted", true}, {"complete", complete},
                                            {"digest_verified", digest_verified},
                                            {"next_offset", m_accepted_file_offset},
                                            {"code", complete ? "complete" : "accepted"}}.dump());
    }
    }
    return failure(400, "unsupported_resource");
}

bool WonderMakerSimulator::connect(const DeviceEndpoint& endpoint,
                                   MessageHandler on_message,
                                   CloseHandler on_close)
{
    if (!m_websocket_available)
        return false;
    if (endpoint.host != m_endpoint.host || endpoint.port != m_endpoint.port || endpoint.scheme != m_endpoint.scheme ||
        endpoint.tls != m_endpoint.tls)
        return false;
    m_on_message = std::move(on_message);
    m_on_close = std::move(on_close);
    m_websocket_open = true;
    m_status_subscription_active = false;
    return true;
}

bool WonderMakerSimulator::send(const std::string& payload)
{
    if (!m_websocket_open)
        return false;
    const auto json = nlohmann::json::parse(payload, nullptr, false);
    if (!is_object(json) || json.value("type", std::string()) != "subscribe" ||
        json.value("stream", std::string()) != "status")
        return false;
    m_status_subscription_active = true;
    return true;
}

void WonderMakerSimulator::close()
{
    m_websocket_open = false;
    m_status_subscription_active = false;
    m_on_message = {};
    m_on_close = {};
}

std::vector<DiscoveryCandidate> WonderMakerSimulator::discover()
{
    return {{m_endpoint, m_device_id}};
}

uint64_t WonderMakerSimulator::control_invocations(const std::string& command_id) const
{
    const auto it = m_control_counts.find(command_id);
    return it == m_control_counts.end() ? 0 : it->second;
}

void WonderMakerSimulator::set_status(PrinterStatus status)
{
    if (status.device_id.empty())
        status.device_id = m_device_id;
    m_status = std::move(status);
    ++m_status_sequence;
}

void WonderMakerSimulator::set_expected_file_digest(std::string sha256, uint64_t total_size)
{
    m_expected_file_digest = std::move(sha256);
    m_expected_file_size = total_size;
    m_accepted_file_offset = 0;
}

void WonderMakerSimulator::emit_status()
{
    if (m_websocket_open && m_status_subscription_active && m_on_message)
        m_on_message(status_event(m_status, m_status_sequence));
}

void WonderMakerSimulator::drop_websocket()
{
    if (!m_websocket_open)
        return;
    m_websocket_open = false;
    m_status_subscription_active = false;
    if (m_on_close)
        m_on_close();
}

void WonderMakerSimulator::restart_device()
{
    drop_websocket();
    ++m_status_sequence;
}

std::string WonderMakerSimulator::status_event(const PrinterStatus& status, uint64_t sequence)
{
    return nlohmann::json{{"type", "status"}, {"sequence", sequence}, {"status", status}}.dump();
}

WonderMakerHttpResponse WonderMakerSimulator::response(int status_code, const std::string& body) const
{
    return {status_code, body, {}, true};
}

WonderMakerHttpResponse WonderMakerSimulator::failure(int status_code, const std::string& code) const
{
    return {status_code,
            nlohmann::json{{"ok", false}, {"code", code}, {"message", code}}.dump(),
            code,
            true};
}

} // namespace Orca::SDK::WonderMaker