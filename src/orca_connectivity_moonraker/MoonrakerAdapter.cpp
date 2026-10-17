#include "orca/connectivity/moonraker/MoonrakerAdapter.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <condition_variable>
#include <sstream>
#include <utility>

namespace Orca::SDK {
namespace {

constexpr const char* kObjectsQuery =
    "/printer/objects/query?print_stats&virtual_sdcard&extruder&heater_bed&fan&toolhead";
constexpr std::array<const char*, 6> kSubscriptionObjects = {
    "print_stats", "virtual_sdcard", "extruder", "heater_bed", "fan", "toolhead"};

std::uint64_t now_epoch_seconds()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

const nlohmann::json* object_member(const nlohmann::json& object, const char* name)
{
    if (!object.is_object()) {
        return nullptr;
    }
    const auto it = object.find(name);
    return it == object.end() ? nullptr : &*it;
}

std::string string_member(const nlohmann::json& object, const char* name)
{
    const auto* value = object_member(object, name);
    return value && value->is_string() ? value->get<std::string>() : std::string{};
}

double number_member(const nlohmann::json& object, const char* name, double fallback = 0.0)
{
    const auto* value = object_member(object, name);
    return value && value->is_number() ? value->get<double>() : fallback;
}

bool is_success_status(int status)
{
    return status >= 200 && status < 300;
}

} // namespace

struct MoonrakerAdapter::CallbackGuard {
    std::mutex mutex;
    std::condition_variable condition;
    MoonrakerAdapter* owner{nullptr};
    bool accepting{true};
    std::size_t active_callbacks{0};

    void leave()
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (--active_callbacks == 0) condition.notify_all();
    }

    struct Scope {
        std::shared_ptr<CallbackGuard> guard;
        ~Scope()
        {
            if (guard) guard->leave();
        }
    };
};

MoonrakerAdapter::MoonrakerAdapter(std::shared_ptr<IMoonrakerHttpTransport> http,
                                   std::shared_ptr<IMoonrakerWebSocketTransport> websocket,
                                   std::string api_key)
    : m_callback_guard(std::make_shared<CallbackGuard>())
    , m_http(std::move(http))
    , m_websocket(std::move(websocket))
    , m_api_key(std::move(api_key))
{
    m_callback_guard->owner = this;
}

MoonrakerAdapter::~MoonrakerAdapter()
{
    const auto guard = m_callback_guard;
    {
        std::lock_guard<std::mutex> lock(guard->mutex);
        guard->accepting = false;
        guard->owner = nullptr;
    }
    if (m_websocket) {
        m_websocket->close();
    }
    std::unique_lock<std::mutex> lock(guard->mutex);
    guard->condition.wait(lock, [&]() { return guard->active_callbacks == 0; });
}

const char* MoonrakerAdapter::id() const
{
    return "moonraker";
}

bool MoonrakerAdapter::probe(const DeviceEndpoint& endpoint)
{
    const auto websocket = m_websocket;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        ++m_websocket_generation;
    }
    if (websocket) websocket->close();

    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    reset_probe_state(endpoint);
    if (!validate_probe_prerequisites()) {
        return false;
    }

    const auto response = perform("GET", "/server/info");
    if (!parse_server_info(response)) {
        return false;
    }
    m_connected = true;
    m_last_result = success_result();
    return true;
}

PrinterCapability MoonrakerAdapter::capabilities() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    PrinterCapability value;
    value.status_stream = m_websocket != nullptr || m_has_endpoint;
    value.upload_gcode = true;
    value.start_print = true;
    value.pause_resume = true;
    value.cancel = true;
    value.local_discovery = false;
    return value;
}

PrinterStatus MoonrakerAdapter::get_status()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_has_endpoint) {
        m_status.state = PrinterState::Offline;
        m_status.diagnostic_code = "moonraker.invalid_endpoint";
        return m_status;
    }
    if (!m_http) {
        m_status.state = PrinterState::Offline;
        m_status.diagnostic_code = "moonraker.transport_error";
        set_failure("moonraker.transport_error", "Moonraker HTTP transport is unavailable", true);
        return m_status;
    }

    if (!m_websocket_connected && !poll_status()) {
        m_status.state = PrinterState::Offline;
        m_status.diagnostic_code = m_last_result.code;
    }
    return m_status;
}

PrinterOperationResult MoonrakerAdapter::upload_gcode(const GcodeUploadRequest& request)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!ensure_ready()) {
        return m_last_result;
    }
    if (request.source_uri.empty() || request.filename.empty()) {
        return set_failure("moonraker.invalid_argument", "Upload requires source_uri and filename", false), m_last_result;
    }

    std::map<std::string, std::string> form_fields{{"root", "gcodes"}, {"print", "false"}};
    const auto response = perform("POST", "/server/files/upload", {}, std::move(form_fields),
                                  request.source_uri, request.filename, request.sha256);
    const auto result = result_from_response(response, true);
    if (!result.success) {
        m_last_result = result;
        return result;
    }

    m_last_uploaded_path = request.filename;
    const auto json = nlohmann::json::parse(response.body, nullptr, false);
    if (!json.is_discarded()) {
        const auto* result_node = object_member(json, "result");
        const auto* item = result_node ? object_member(*result_node, "item") : nullptr;
        const auto path = item ? string_member(*item, "path") : std::string{};
        if (!path.empty()) {
            m_last_uploaded_path = path;
        }
    }
    m_last_result = success_result();
    return m_last_result;
}

PrinterOperationResult MoonrakerAdapter::start_print(const PrintCommand& command)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!ensure_ready()) {
        return m_last_result;
    }
    std::string filename = command.filename;
    if (filename.empty()) {
        filename = m_last_uploaded_path;
    }
    if (filename.empty()) {
        filename = command.file_uri;
    }
    if (filename.empty()) {
        return set_failure("moonraker.invalid_argument", "Start print requires a filename", false), m_last_result;
    }
    send_print_start(filename);
    return m_last_result;
}

PrinterOperationResult MoonrakerAdapter::pause()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    send_gcode("PAUSE");
    return m_last_result;
}

PrinterOperationResult MoonrakerAdapter::resume()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    send_gcode("RESUME");
    return m_last_result;
}

PrinterOperationResult MoonrakerAdapter::cancel()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    send_gcode("CANCEL_PRINT");
    return m_last_result;
}

bool MoonrakerAdapter::refresh_objects()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!ensure_ready()) {
        return false;
    }
    m_available_objects.clear();
    m_objects_loaded = false;
    const bool parsed = parse_object_list(perform("GET", "/printer/objects/list"));
    if (parsed) {
        m_objects_loaded = true;
    }
    return parsed;
}

bool MoonrakerAdapter::subscribe_status()
{
    std::unique_lock<std::recursive_mutex> lock(m_mutex);
    m_websocket_connected = false;
    if (!ensure_ready()) {
        return false;
    }
    if (!m_websocket) {
        return enable_polling_fallback(
            "moonraker.websocket_unavailable",
            "Moonraker WebSocket transport is unavailable; polling fallback enabled",
            true);
    }
    if (!ensure_objects_loaded()) {
        const auto failure = m_last_result;
        return enable_polling_fallback(
            failure.code.empty() ? "moonraker.protocol_error" : failure.code,
            failure.message.empty() ? "Moonraker object discovery failed; polling fallback enabled"
                                    : failure.message + "; polling fallback enabled",
            failure.retryable);
    }

    nlohmann::json objects = nlohmann::json::object();
    for (const auto* name : kSubscriptionObjects) {
        if (m_available_objects.count(name) != 0) {
            objects[name] = nullptr;
        }
    }
    if (objects.empty()) {
        return enable_polling_fallback(
            "moonraker.no_status_objects",
            "Moonraker object discovery returned no subscribable status objects; polling fallback enabled",
            false);
    }

    const auto websocket = m_websocket;
    const auto headers = m_api_key.empty()
        ? std::map<std::string, std::string>{}
        : std::map<std::string, std::string>{{"X-Api-Key", m_api_key}};
    const auto target = websocket_url(m_endpoint);
    const auto generation = ++m_websocket_generation;
    const auto guard = m_callback_guard;
    const auto connected_callback = [guard, generation](const std::string& payload) {
        MoonrakerAdapter* owner = nullptr;
        {
            std::lock_guard<std::mutex> guard_lock(guard->mutex);
            if (!guard->accepting || !guard->owner) return;
            ++guard->active_callbacks;
            owner = guard->owner;
        }
        MoonrakerAdapter::CallbackGuard::Scope scope{guard};
        owner->handle_websocket_message_for_generation(payload, generation);
    };
    const auto close_callback = [guard, generation]() {
        MoonrakerAdapter* owner = nullptr;
        {
            std::lock_guard<std::mutex> guard_lock(guard->mutex);
            if (!guard->accepting || !guard->owner) return;
            ++guard->active_callbacks;
            owner = guard->owner;
        }
        MoonrakerAdapter::CallbackGuard::Scope scope{guard};
        {
            std::lock_guard<std::recursive_mutex> lock(owner->m_mutex);
            if (generation == owner->m_websocket_generation) {
                owner->enable_polling_fallback(
                    "moonraker.websocket_disconnected",
                    "Moonraker WebSocket disconnected; polling fallback enabled",
                    true);
            }
        }
    };

    // The transport may join its old reader while invoking the close callback.
    // Do not hold the adapter mutex across connect/send, or that callback could
    // wait forever for this thread to release it.
    lock.unlock();
    const bool connected = websocket->connect(target, headers, connected_callback, close_callback);
    if (!connected) {
        lock.lock();
        if (generation == m_websocket_generation) {
            return enable_polling_fallback(
                "moonraker.websocket_connect_failed",
                "Moonraker WebSocket connection failed; polling fallback enabled",
                true);
        }
        return false;
    }

    lock.lock();
    if (generation != m_websocket_generation) return false;
    nlohmann::json subscribe;
    subscribe["jsonrpc"] = "2.0";
    subscribe["method"] = "printer.objects.subscribe";
    subscribe["params"]["objects"] = std::move(objects);
    subscribe["id"] = m_next_rpc_id++;
    lock.unlock();
    if (!websocket->send(subscribe.dump())) {
        websocket->close();
        lock.lock();
        if (generation == m_websocket_generation) {
            return enable_polling_fallback(
                "moonraker.websocket_send_failed",
                "Moonraker WebSocket subscription failed; polling fallback enabled",
                true);
        }
        return false;
    }

    lock.lock();
    if (generation != m_websocket_generation) return false;
    m_websocket_connected = true;
    m_polling_fallback = false;
    m_connected = true;
    m_last_result = success_result();
    return true;
}

bool MoonrakerAdapter::handle_websocket_message(const std::string& payload)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return handle_websocket_message_for_generation(payload, m_websocket_generation);
}

bool MoonrakerAdapter::handle_websocket_message_for_generation(const std::string& payload,
                                                               std::uint64_t generation)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (generation != m_websocket_generation) {
        return false;
    }
    const auto json = nlohmann::json::parse(payload, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        return enable_polling_fallback(
            "moonraker.invalid_json",
            "Moonraker WebSocket message is not valid JSON; polling fallback enabled",
            true);
    }
    if (json.contains("error")) {
        const auto& error = json["error"];
        const auto message = error.is_object() ? string_member(error, "message") : std::string{};
        return enable_polling_fallback(
            "moonraker.rpc_error",
            (message.empty() ? "Moonraker WebSocket RPC error" : message) +
                "; polling fallback enabled",
            true);
    }

    const nlohmann::json* status = nullptr;
    const auto method = string_member(json, "method");
    if (method == "notify_status_update") {
        const auto* params = object_member(json, "params");
        if (params && params->is_array() && !params->empty()) {
            status = &(*params)[0];
        } else if (params && params->is_object()) {
            status = object_member(*params, "status");
            if (!status) {
                status = params;
            }
        }
    } else if (const auto* result = object_member(json, "result")) {
        status = object_member(*result, "status");
    }

    if (!status || !status->is_object() || status->empty() || !apply_status_update(*status)) {
        return enable_polling_fallback(
            "moonraker.protocol_error",
            "Moonraker WebSocket message has no status update; polling fallback enabled",
            true);
    }
    m_websocket_connected = true;
    m_connected = true;
    m_polling_fallback = false;
    m_last_result = success_result();
    return true;
}

bool MoonrakerAdapter::reconnect_and_reconcile()
{
    bool has_websocket = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (!ensure_ready()) return false;
        has_websocket = m_websocket != nullptr;
    }
    if (has_websocket) {
        subscribe_status();
    } else {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_polling_fallback = true;
    }
    return poll_status();
}

bool MoonrakerAdapter::poll_status()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!ensure_ready()) {
        return false;
    }
    const bool parsed = parse_status(perform("GET", kObjectsQuery));
    if (parsed) {
        m_connected = true;
        if (!m_websocket_connected) {
            m_polling_fallback = true;
        }
    } else {
        m_connected = false;
    }
    return parsed;
}

bool MoonrakerAdapter::websocket_connected() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_websocket_connected;
}

bool MoonrakerAdapter::polling_fallback() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_polling_fallback;
}

bool MoonrakerAdapter::connected() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_connected;
}

std::set<std::string> MoonrakerAdapter::available_objects() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_available_objects;
}

MoonrakerServerInfo MoonrakerAdapter::server_info() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_server_info;
}

PrinterOperationResult MoonrakerAdapter::last_result() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_last_result;
}

std::string MoonrakerAdapter::last_uploaded_path() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_last_uploaded_path;
}

std::string MoonrakerAdapter::endpoint_url(const DeviceEndpoint& endpoint, const std::string& path)
{
    std::string scheme = endpoint.scheme;
    if (scheme.empty()) {
        scheme = endpoint.tls ? "https" : "http";
    }
    if (scheme == "ws") scheme = "http";
    if (scheme == "wss") scheme = "https";

    std::ostringstream url;
    url << scheme << "://";
    const bool ipv6 = endpoint.host.find(':') != std::string::npos &&
                      (endpoint.host.empty() || endpoint.host.front() != '[');
    if (ipv6) url << '[';
    url << endpoint.host;
    if (ipv6) url << ']';
    if (endpoint.port != 0) url << ':' << endpoint.port;
    if (path.empty() || path.front() != '/') url << '/';
    url << path;
    return url.str();
}

std::string MoonrakerAdapter::websocket_url(const DeviceEndpoint& endpoint)
{
    auto value = endpoint_url(endpoint, "");
    const auto scheme_end = value.find("://");
    if (scheme_end != std::string::npos) {
        const auto scheme = endpoint.tls || endpoint.scheme == "https" || endpoint.scheme == "wss" ? "wss" : "ws";
        value.replace(0, scheme_end, scheme);
    }
    if (!value.empty() && value.back() == '/') value.pop_back();
    return value + "/websocket";
}

std::string MoonrakerAdapter::endpoint_device_id(const DeviceEndpoint& endpoint)
{
    std::ostringstream value;
    value << "moonraker:" << endpoint.host;
    if (endpoint.port != 0) value << ':' << endpoint.port;
    return value.str();
}

std::string MoonrakerAdapter::map_state(const std::string& state)
{
    std::string value = state;
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

PrinterState MoonrakerAdapter::map_printer_state(const std::string& state)
{
    const auto value = map_state(state);
    if (value == "printing") return PrinterState::Printing;
    if (value == "paused") return PrinterState::Paused;
    if (value == "complete" || value == "completed" || value == "cancelled" || value == "canceled") {
        return PrinterState::Completed;
    }
    if (value == "error") return PrinterState::Error;
    if (value == "startup" || value == "ready" || value == "preparing") return PrinterState::Preparing;
    if (value == "idle" || value == "standby") return PrinterState::Idle;
    if (value == "shutdown" || value == "disconnected" || value == "offline") return PrinterState::Offline;
    return PrinterState::Unknown;
}

std::string MoonrakerAdapter::error_message(const MoonrakerHttpResponse& response)
{
    if (!response.error.empty()) return response.error;
    if (response.status_code != 0) return "Moonraker HTTP status " + std::to_string(response.status_code);
    return "Moonraker transport failed";
}

PrinterOperationResult MoonrakerAdapter::success_result()
{
    return {true, false, "ok", ""};
}

PrinterOperationResult MoonrakerAdapter::failure_result(std::string code, std::string message, bool retryable)
{
    return {false, retryable, std::move(code), std::move(message)};
}

void MoonrakerAdapter::reset_probe_state(const DeviceEndpoint& endpoint)
{
    m_endpoint = endpoint;
    m_server_info = {};
    m_status = {};
    m_status.device_id = endpoint_device_id(endpoint);
    m_available_objects.clear();
    m_last_uploaded_path.clear();
    m_last_result = success_result();
    m_next_rpc_id = 1;
    m_has_endpoint = false;
    m_connected = false;
    m_websocket_connected = false;
    m_polling_fallback = false;
    m_objects_loaded = false;
    ++m_websocket_generation;
}

bool MoonrakerAdapter::validate_probe_prerequisites()
{
    if (m_endpoint.host.empty()) {
        return set_failure("moonraker.invalid_endpoint", "Moonraker endpoint has no host", false);
    }
    m_has_endpoint = true;
    if (!m_http) {
        return set_failure("moonraker.transport_error", "Moonraker HTTP transport is unavailable", true);
    }
    return true;
}

bool MoonrakerAdapter::ensure_ready()
{
    if (!m_has_endpoint) {
        return set_failure("moonraker.invalid_endpoint", "Moonraker endpoint has not been probed", false);
    }
    if (!m_http) {
        return set_failure("moonraker.transport_error", "Moonraker HTTP transport is unavailable", true);
    }
    return true;
}

bool MoonrakerAdapter::ensure_objects_loaded()
{
    return m_objects_loaded || refresh_objects();
}

bool MoonrakerAdapter::enable_polling_fallback(std::string code, std::string message, bool retryable)
{
    m_websocket_connected = false;
    m_polling_fallback = true;
    m_connected = false;
    return set_failure(std::move(code), std::move(message), retryable);
}

MoonrakerHttpRequest MoonrakerAdapter::make_request(std::string method, std::string path) const
{
    MoonrakerHttpRequest request;
    request.method = std::move(method);
    request.path = std::move(path);
    request.url = endpoint_url(m_endpoint, request.path);
    if (!m_api_key.empty()) {
        request.headers.emplace("X-Api-Key", m_api_key);
    }
    return request;
}

MoonrakerHttpResponse MoonrakerAdapter::perform(std::string method,
                                                std::string path,
                                                std::string body,
                                                std::map<std::string, std::string> form_fields,
                                                std::string upload_source,
                                                std::string upload_filename,
                                  std::string upload_sha256) const
{
    if (!m_http) {
        return {0, {}, "Moonraker HTTP transport is unavailable"};
    }
    auto request = make_request(std::move(method), std::move(path));
    request.body = std::move(body);
    request.form_fields = std::move(form_fields);
    request.upload_source = std::move(upload_source);
    request.upload_filename = std::move(upload_filename);
    request.upload_sha256 = std::move(upload_sha256);
    if (!request.body.empty()) {
        request.headers.emplace("Content-Type", "application/json");
    }
    return m_http->perform(request);
}

PrinterOperationResult MoonrakerAdapter::result_from_response(const MoonrakerHttpResponse& response,
                                                              bool parse_json_envelope) const
{
    if (response.status_code == 401 || response.status_code == 403) {
        return failure_result("moonraker.unauthorized", error_message(response), false);
    }
    if (response.status_code == 404) {
        return failure_result("moonraker.not_found", error_message(response), false);
    }
    if (!response.error.empty()) {
        const auto error = map_state(response.error);
        if (error.find("cancelled") != std::string::npos || error.find("canceled") != std::string::npos) {
            return failure_result("moonraker.cancelled", response.error, false);
        }
        if (error.find("sha-256") != std::string::npos) {
            return failure_result("moonraker.integrity_error", response.error, false);
        }
        if (error.find("timed out") != std::string::npos || error.find("timeout") != std::string::npos) {
            return failure_result("moonraker.timeout", response.error, true);
        }
        return failure_result("moonraker.transport_error", response.error, true);
    }
    if (!is_success_status(response.status_code)) {
        const bool retryable = response.status_code == 0 || response.status_code >= 500;
        return failure_result(response.status_code == 0 ? "moonraker.transport_error" : "moonraker.http_error",
                              error_message(response), retryable);
    }
    if (!parse_json_envelope || response.body.empty()) {
        return success_result();
    }

    const auto json = nlohmann::json::parse(response.body, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        return failure_result("moonraker.invalid_json", "Moonraker response is not valid JSON", false);
    }
    if (json.contains("error")) {
        const auto& error = json["error"];
        const auto message = error.is_object() ? string_member(error, "message") : std::string{};
        return failure_result("moonraker.rpc_error", message.empty() ? "Moonraker RPC error" : message, false);
    }
    if (!json.contains("result")) {
        return failure_result("moonraker.protocol_error", "Moonraker response has no result envelope", false);
    }
    return success_result();
}

bool MoonrakerAdapter::parse_server_info(const MoonrakerHttpResponse& response)
{
    const auto result = result_from_response(response, true);
    if (!result.success) {
        m_last_result = result;
        return false;
    }
    const auto json = nlohmann::json::parse(response.body, nullptr, false);
    const auto* server = json.is_object() ? object_member(json, "result") : nullptr;
    const auto state = server ? string_member(*server, "klippy_state") : std::string{};
    if (state.empty()) {
        return set_failure("moonraker.protocol_error", "Moonraker /server/info has no result.klippy_state", false);
    }
    m_server_info.klippy_state = state;
    m_server_info.moonraker_version = string_member(*server, "moonraker_version");
    m_server_info.klipper_path = string_member(*server, "klipper_path");
    m_server_info.hostname = string_member(*server, "hostname");
    m_last_result = success_result();
    return true;
}

bool MoonrakerAdapter::parse_object_list(const MoonrakerHttpResponse& response)
{
    const auto result = result_from_response(response, true);
    if (!result.success) {
        m_last_result = result;
        return false;
    }
    const auto json = nlohmann::json::parse(response.body, nullptr, false);
    const auto* envelope = json.is_object() ? object_member(json, "result") : nullptr;
    const auto* objects = envelope ? object_member(*envelope, "objects") : nullptr;
    if (!objects || !objects->is_array()) {
        return set_failure("moonraker.protocol_error", "Moonraker /printer/objects/list has no result.objects array", false);
    }
    for (const auto& object : *objects) {
        if (object.is_string()) {
            m_available_objects.insert(object.get<std::string>());
        }
    }
    m_last_result = success_result();
    return true;
}

bool MoonrakerAdapter::parse_status(const MoonrakerHttpResponse& response)
{
    const auto result = result_from_response(response, true);
    if (!result.success) {
        m_last_result = result;
        return false;
    }
    const auto json = nlohmann::json::parse(response.body, nullptr, false);
    const auto* envelope = json.is_object() ? object_member(json, "result") : nullptr;
    const auto* status = envelope ? object_member(*envelope, "status") : nullptr;
    if (!status || !status->is_object() || status->empty()) {
        return set_failure("moonraker.protocol_error", "Moonraker /printer/objects/query has no result.status object", false);
    }
    if (!apply_status_update(*status)) {
        return set_failure("moonraker.protocol_error", "Moonraker /printer/objects/query has no recognized status fields", false);
    }
    m_last_result = success_result();
    return true;
}

bool MoonrakerAdapter::apply_status_update(const nlohmann::json& status)
{
    if (!status.is_object() || status.empty()) {
        return false;
    }

    const auto* print_stats = object_member(status, "print_stats");
    const auto* virtual_sdcard = object_member(status, "virtual_sdcard");
    const auto* extruder = object_member(status, "extruder");
    const auto* heater_bed = object_member(status, "heater_bed");
    bool updated = false;

    std::string state;
    if (print_stats) {
        updated = true;
        state = string_member(*print_stats, "state");
    }
    if (state.empty() && virtual_sdcard) {
        updated = true;
        state = string_member(*virtual_sdcard, "state");
    }
    if (!state.empty()) {
        m_status.state = map_printer_state(state);
    }

    double progress = 0.0;
    bool has_progress = false;
    if (print_stats) {
        const auto* value = object_member(*print_stats, "progress");
        if (value && value->is_number()) {
            progress = value->get<double>();
            has_progress = true;
        }
    }
    if (!has_progress && virtual_sdcard) {
        const auto* value = object_member(*virtual_sdcard, "progress");
        if (value && value->is_number()) {
            progress = value->get<double>();
            has_progress = true;
        }
    }
    if (has_progress && std::isfinite(progress)) {
        m_status.progress = std::clamp(progress, 0.0, 1.0);
        updated = true;
    }
    if (print_stats) {
        const auto filename = string_member(*print_stats, "filename");
        if (!filename.empty()) m_status.current_file = filename;
        const auto message = string_member(*print_stats, "message");
        if (!message.empty()) m_status.diagnostic_code = message;
    }
    if (extruder) {
        m_status.nozzle_temperature_c = number_member(*extruder, "temperature", m_status.nozzle_temperature_c);
        updated = true;
    }
    if (heater_bed) {
        m_status.bed_temperature_c = number_member(*heater_bed, "temperature", m_status.bed_temperature_c);
        updated = true;
    }
    if (!updated) {
        return false;
    }
    if (m_status.device_id.empty()) m_status.device_id = endpoint_device_id(m_endpoint);
    m_status.observed_at_epoch_seconds = now_epoch_seconds();
    return true;
}

bool MoonrakerAdapter::send_gcode(const std::string& script)
{
    if (!ensure_ready()) {
        return false;
    }
    nlohmann::json body{{"script", script}};
    const auto result = result_from_response(perform("POST", "/printer/gcode/script", body.dump()), true);
    m_last_result = result;
    return result.success;
}

bool MoonrakerAdapter::send_print_start(const std::string& filename)
{
    nlohmann::json body{{"filename", filename}};
    const auto result = result_from_response(perform("POST", "/printer/print/start", body.dump()), true);
    m_last_result = result;
    return result.success;
}

bool MoonrakerAdapter::set_failure(std::string code, std::string message, bool retryable)
{
    m_last_result = failure_result(std::move(code), std::move(message), retryable);
    return false;
}

} // namespace Orca::SDK
