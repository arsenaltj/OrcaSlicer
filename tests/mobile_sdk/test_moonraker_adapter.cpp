#include <catch2/catch_all.hpp>

#include "orca/connectivity/moonraker/MoonrakerAdapter.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace Orca::SDK;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

struct FakeHttp final : IMoonrakerHttpTransport {
    mutable std::mutex mutex;
    std::vector<MoonrakerHttpRequest> requests;
    std::function<MoonrakerHttpResponse(const MoonrakerHttpRequest&)> handler;

    MoonrakerHttpResponse perform(const MoonrakerHttpRequest& request) override
    {
        std::lock_guard<std::mutex> lock(mutex);
        requests.push_back(request);
        return handler ? handler(request) : MoonrakerHttpResponse{500, {}, "no fake response"};
    }
};

struct FakeWebSocket final : IMoonrakerWebSocketTransport {
    mutable std::mutex mutex;
    std::condition_variable condition;
    bool connect_result{true};
    bool send_result{true};
    bool connected{false};
    bool block_connect{false};
    bool connect_entered{false};
    bool release_connect{false};
    bool disconnect_on_send{false};
    int close_count{0};
    std::string url;
    std::map<std::string, std::string> headers;
    std::vector<std::string> sent;
    MessageHandler on_message;
    CloseHandler on_close;

    bool connect(const std::string& target,
                 const std::map<std::string, std::string>& request_headers,
                 MessageHandler message_handler,
                 CloseHandler close_handler) override
    {
        std::unique_lock<std::mutex> lock(mutex);
        url = target;
        headers = request_headers;
        on_message = std::move(message_handler);
        on_close = std::move(close_handler);
        connect_entered = true;
        condition.notify_all();
        condition.wait(lock, [&]() { return !block_connect || release_connect; });
        connected = connect_result;
        return connect_result;
    }

    bool send(const std::string& payload) override
    {
        CloseHandler close_callback;
        bool result = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            sent.push_back(payload);
            result = send_result && connected;
            if (result && disconnect_on_send) {
                connected = false;
                close_callback = on_close;
            }
        }
        if (close_callback) close_callback();
        return result;
    }

    void close() override
    {
        std::lock_guard<std::mutex> lock(mutex);
        connected = false;
        ++close_count;
    }

    void wait_for_connect()
    {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&]() { return connect_entered; });
    }

    void release_blocked_connect()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            release_connect = true;
        }
        condition.notify_all();
    }

    bool is_connected() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return connected;
    }

    int close_calls() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return close_count;
    }

    void emit(const std::string& payload)
    {
        MessageHandler callback;
        {
            std::lock_guard<std::mutex> lock(mutex);
            callback = on_message;
        }
        if (callback) callback(payload);
    }

    void disconnect_from_server()
    {
        CloseHandler callback;
        {
            std::lock_guard<std::mutex> lock(mutex);
            connected = false;
            callback = on_close;
        }
        if (callback) callback();
    }
};

DeviceEndpoint endpoint(const char* host = "printer.local")
{
    return {"http", host, 7125, false, {}};
}

MoonrakerHttpResponse ok(std::string body)
{
    return {200, std::move(body), {}};
}

std::string server_info(const char* hostname = "printer")
{
    return nlohmann::json{
        {"result", {
            {"klippy_state", "ready"},
            {"moonraker_version", "v0.9"},
            {"hostname", hostname},
        }}
    }.dump();
}

std::string object_list(const std::vector<std::string>& objects)
{
    nlohmann::json values = nlohmann::json::array();
    for (const auto& object : objects) values.push_back(object);
    return nlohmann::json{{"result", {{"objects", std::move(values)}}}}.dump();
}

std::string status_response(const char* state = "printing", double progress = 0.42)
{
    return nlohmann::json{
        {"result", {
            {"status", {
                {"print_stats", {{"state", state}, {"progress", progress}, {"filename", "benchy.gcode"}}},
                {"virtual_sdcard", {{"progress", progress}}},
                {"extruder", {{"temperature", 205.0}}},
                {"heater_bed", {{"temperature", 60.0}}}
            }}
        }}
    }.dump();
}

void install_standard_http(const std::shared_ptr<FakeHttp>& http,
                           const char* state = "paused",
                           double progress = 0.5,
                           std::initializer_list<const char*> objects = {
                               "print_stats", "virtual_sdcard", "extruder", "heater_bed", "fan", "toolhead"})
{
    const auto available_objects = std::vector<std::string>(objects.begin(), objects.end());
    http->handler = [state, progress, available_objects](const MoonrakerHttpRequest& request) {
        if (request.path == "/server/info") return ok(server_info());
        if (request.path == "/printer/objects/list") return ok(object_list(available_objects));
        if (request.path == "/printer/objects/query?print_stats&virtual_sdcard&extruder&heater_bed&fan&toolhead") {
            return ok(status_response(state, progress));
        }
        return MoonrakerHttpResponse{404, {}, "unexpected path"};
    };
}

} // namespace

TEST_CASE("Moonraker probe, object discovery, query and API key stay GUI independent", "[Moonraker][MobileSDK]")
{
    auto http = std::make_shared<FakeHttp>();
    install_standard_http(http, "printing", 0.42);

    MoonrakerAdapter adapter(http, nullptr, "secret-key");
    REQUIRE(adapter.id() == std::string("moonraker"));
    REQUIRE(adapter.probe(endpoint()));
    REQUIRE(adapter.server_info().klippy_state == "ready");
    REQUIRE(adapter.refresh_objects());
    REQUIRE(adapter.available_objects().count("print_stats") == 1);

    const auto status = adapter.get_status();
    REQUIRE(status.state == PrinterState::Printing);
    REQUIRE_THAT(status.progress, WithinAbs(0.42, 1e-9));
    REQUIRE_THAT(status.nozzle_temperature_c, WithinRel(205.0, 1e-12));
    REQUIRE_THAT(status.bed_temperature_c, WithinRel(60.0, 1e-12));
    REQUIRE(status.current_file == "benchy.gcode");
    REQUIRE(adapter.polling_fallback());

    REQUIRE(http->requests.size() == 3);
    for (const auto& request : http->requests) {
        REQUIRE(request.headers.at("X-Api-Key") == "secret-key");
    }
}

TEST_CASE("Moonraker subscribes only to objects reported by objects list", "[Moonraker][MobileSDK]")
{
    auto http = std::make_shared<FakeHttp>();
    install_standard_http(http, "printing", 0.42, {"print_stats", "extruder"});
    auto websocket = std::make_shared<FakeWebSocket>();
    MoonrakerAdapter adapter(http, websocket, "ws-key");

    REQUIRE(adapter.probe(endpoint()));
    REQUIRE(adapter.subscribe_status());
    REQUIRE(adapter.websocket_connected());
    REQUIRE_FALSE(adapter.polling_fallback());
    REQUIRE(websocket->url == "ws://printer.local:7125/websocket");
    REQUIRE(websocket->headers.at("X-Api-Key") == "ws-key");
    REQUIRE(websocket->sent.size() == 1);

    const auto subscribe = nlohmann::json::parse(websocket->sent.front());
    const auto& objects = subscribe.at("params").at("objects");
    REQUIRE(subscribe.at("method") == "printer.objects.subscribe");
    REQUIRE(objects.contains("print_stats"));
    REQUIRE(objects.contains("extruder"));
    REQUIRE_FALSE(objects.contains("virtual_sdcard"));
    REQUIRE_FALSE(objects.contains("heater_bed"));
    REQUIRE_FALSE(objects.contains("fan"));
    REQUIRE_FALSE(objects.contains("toolhead"));

    websocket->emit(R"({"jsonrpc":"2.0","method":"notify_status_update","params":[{"print_stats":{"state":"paused","progress":0.8,"filename":"pause.gcode"},"extruder":{"temperature":190}}]})");
    const auto status = adapter.get_status();
    REQUIRE(status.state == PrinterState::Paused);
    REQUIRE_THAT(status.progress, WithinAbs(0.8, 1e-9));
    REQUIRE_THAT(status.nozzle_temperature_c, WithinRel(190.0, 1e-12));
    REQUIRE(status.current_file == "pause.gcode");
}

TEST_CASE("Moonraker close callback cannot restore a failed subscription", "[Moonraker][MobileSDK][Concurrency]")
{
    auto http = std::make_shared<FakeHttp>();
    install_standard_http(http);
    auto websocket = std::make_shared<FakeWebSocket>();
    websocket->disconnect_on_send = true;
    MoonrakerAdapter adapter(http, websocket);

    REQUIRE(adapter.probe(endpoint()));
    REQUIRE_FALSE(adapter.subscribe_status());
    REQUIRE_FALSE(adapter.websocket_connected());
    REQUIRE(adapter.polling_fallback());
    REQUIRE(websocket->sent.size() == 1);
}

TEST_CASE("Moonraker probe closes a stale socket created by an interleaved subscription", "[Moonraker][MobileSDK][Concurrency]")
{
    auto http = std::make_shared<FakeHttp>();
    install_standard_http(http);
    auto websocket = std::make_shared<FakeWebSocket>();
    websocket->block_connect = true;
    MoonrakerAdapter adapter(http, websocket);

    REQUIRE(adapter.probe(endpoint("first.local")));

    std::atomic_bool subscribe_result{true};
    std::thread subscriber([&]() { subscribe_result.store(adapter.subscribe_status()); });
    websocket->wait_for_connect();

    REQUIRE(adapter.probe(endpoint("second.local")));
    websocket->release_blocked_connect();
    subscriber.join();

    REQUIRE_FALSE(subscribe_result.load());
    REQUIRE_FALSE(adapter.websocket_connected());
    REQUIRE_FALSE(websocket->is_connected());
    REQUIRE(websocket->close_calls() >= 2);
}

TEST_CASE("Moonraker clamps progress to the SDK range and maps cancelled states", "[Moonraker][MobileSDK]")
{
    SECTION("progress values remain in 0..1")
    {
        auto http = std::make_shared<FakeHttp>();
        install_standard_http(http, "printing", 1.75);
        MoonrakerAdapter adapter(http);
        REQUIRE(adapter.probe(endpoint()));
        REQUIRE_THAT(adapter.get_status().progress, WithinAbs(1.0, 1e-12));

        http->handler = [](const MoonrakerHttpRequest& request) {
            if (request.path == "/printer/objects/query?print_stats&virtual_sdcard&extruder&heater_bed&fan&toolhead") {
                return ok(status_response("printing", -0.25));
            }
            return ok(server_info());
        };
        REQUIRE_THAT(adapter.get_status().progress, WithinAbs(0.0, 1e-12));
    }

    SECTION("cancelled and canceled are terminal completed states")
    {
        const auto state = GENERATE("cancelled", "canceled");
        auto http = std::make_shared<FakeHttp>();
        install_standard_http(http, state, 0.25);
        MoonrakerAdapter adapter(http);
        REQUIRE(adapter.probe(endpoint()));
        REQUIRE(adapter.get_status().state == PrinterState::Completed);
        REQUIRE_THAT(adapter.get_status().progress, WithinRel(0.25, 1e-12));
    }
}

TEST_CASE("Moonraker falls back to polling and reconciles after reconnect", "[Moonraker][MobileSDK]")
{
    auto http = std::make_shared<FakeHttp>();
    install_standard_http(http, "paused", 0.5);
    auto websocket = std::make_shared<FakeWebSocket>();
    websocket->connect_result = false;
    MoonrakerAdapter adapter(http, websocket);

    REQUIRE(adapter.probe(endpoint()));
    REQUIRE_FALSE(adapter.subscribe_status());
    REQUIRE_FALSE(adapter.websocket_connected());
    REQUIRE(adapter.polling_fallback());
    REQUIRE(adapter.get_status().state == PrinterState::Paused);

    websocket->connect_result = true;
    REQUIRE(adapter.reconnect_and_reconcile());
    REQUIRE(adapter.websocket_connected());
    REQUIRE_FALSE(adapter.polling_fallback());
    REQUIRE(http->requests.size() == 4);

    websocket->disconnect_from_server();
    REQUIRE_FALSE(adapter.websocket_connected());
    REQUIRE(adapter.polling_fallback());
    REQUIRE_FALSE(adapter.connected());
}

TEST_CASE("Moonraker WebSocket faults switch to polling fallback", "[Moonraker][MobileSDK]")
{
    SECTION("cancelled request")
    {
        auto http = std::make_shared<FakeHttp>();
        http->handler = [](const MoonrakerHttpRequest&) { return MoonrakerHttpResponse{0, "", "Moonraker HTTP request cancelled"}; };
        MoonrakerAdapter adapter(http);
        REQUIRE_FALSE(adapter.probe(endpoint()));
        REQUIRE(adapter.last_result().code == "moonraker.cancelled");
        REQUIRE_FALSE(adapter.last_result().retryable);
    }

    SECTION("invalid JSON")
    {
        auto http = std::make_shared<FakeHttp>();
        install_standard_http(http);
        auto websocket = std::make_shared<FakeWebSocket>();
        MoonrakerAdapter adapter(http, websocket);
        REQUIRE(adapter.probe(endpoint()));
        REQUIRE(adapter.subscribe_status());
        websocket->emit("not-json");
        REQUIRE_FALSE(adapter.websocket_connected());
        REQUIRE(adapter.polling_fallback());
        REQUIRE(adapter.last_result().code == "moonraker.invalid_json");
        REQUIRE(adapter.get_status().state == PrinterState::Paused);
    }

    SECTION("RPC error")
    {
        auto http = std::make_shared<FakeHttp>();
        install_standard_http(http);
        auto websocket = std::make_shared<FakeWebSocket>();
        MoonrakerAdapter adapter(http, websocket);
        REQUIRE(adapter.probe(endpoint()));
        REQUIRE(adapter.subscribe_status());
        websocket->emit(R"({"jsonrpc":"2.0","error":{"code":-32600,"message":"bad subscription"}})");
        REQUIRE_FALSE(adapter.websocket_connected());
        REQUIRE(adapter.polling_fallback());
        REQUIRE(adapter.last_result().code == "moonraker.rpc_error");
        REQUIRE(adapter.get_status().state == PrinterState::Paused);
    }

    SECTION("missing status")
    {
        auto http = std::make_shared<FakeHttp>();
        install_standard_http(http);
        auto websocket = std::make_shared<FakeWebSocket>();
        MoonrakerAdapter adapter(http, websocket);
        REQUIRE(adapter.probe(endpoint()));
        REQUIRE(adapter.subscribe_status());
        websocket->emit(R"({"jsonrpc":"2.0","method":"notify_status_update","params":[]})");
        REQUIRE_FALSE(adapter.websocket_connected());
        REQUIRE(adapter.polling_fallback());
        REQUIRE(adapter.last_result().code == "moonraker.protocol_error");
        REQUIRE(adapter.get_status().state == PrinterState::Paused);
    }

    SECTION("send failure")
    {
        auto http = std::make_shared<FakeHttp>();
        install_standard_http(http);
        auto websocket = std::make_shared<FakeWebSocket>();
        websocket->send_result = false;
        MoonrakerAdapter adapter(http, websocket);
        REQUIRE(adapter.probe(endpoint()));
        REQUIRE_FALSE(adapter.subscribe_status());
        REQUIRE_FALSE(adapter.websocket_connected());
        REQUIRE(adapter.polling_fallback());
        REQUIRE(adapter.last_result().code == "moonraker.websocket_send_failed");
        REQUIRE(adapter.get_status().state == PrinterState::Paused);
    }
}

TEST_CASE("Moonraker probe clears cached state and enforces preconditions", "[Moonraker][MobileSDK]")
{
    SECTION("probe requires an endpoint and HTTP transport")
    {
        MoonrakerAdapter without_http(nullptr);
        REQUIRE_FALSE(without_http.probe(endpoint()));
        REQUIRE(without_http.last_result().code == "moonraker.transport_error");

        auto http = std::make_shared<FakeHttp>();
        MoonrakerAdapter without_probe(http);
        REQUIRE_FALSE(without_probe.refresh_objects());
        REQUIRE(without_probe.last_result().code == "moonraker.invalid_endpoint");
        REQUIRE(without_probe.pause().code == "moonraker.invalid_endpoint");
    }

    SECTION("a later probe removes object and upload caches")
    {
        auto http = std::make_shared<FakeHttp>();
        http->handler = [](const MoonrakerHttpRequest& request) {
            if (request.path == "/server/info") return ok(server_info("first"));
            if (request.path == "/printer/objects/list") return ok(object_list({"print_stats"}));
            if (request.path == "/server/files/upload") return ok(R"({"result":{"item":{"path":"gcodes/old.gcode"}}})");
            return MoonrakerHttpResponse{404, {}, "unexpected path"};
        };
        MoonrakerAdapter adapter(http);
        REQUIRE(adapter.probe(endpoint("first.local")));
        REQUIRE(adapter.refresh_objects());
        REQUIRE(adapter.upload_gcode({"file:///tmp/old.gcode", "sha256", "old.gcode"}).success);
        REQUIRE_FALSE(adapter.available_objects().empty());
        REQUIRE_FALSE(adapter.last_uploaded_path().empty());

        http->handler = [](const MoonrakerHttpRequest& request) {
            if (request.path == "/server/info") return ok(server_info("second"));
            return MoonrakerHttpResponse{404, {}, "unexpected path"};
        };
        REQUIRE(adapter.probe(endpoint("second.local")));
        REQUIRE(adapter.server_info().hostname == "second");
        REQUIRE(adapter.available_objects().empty());
        REQUIRE(adapter.last_uploaded_path().empty());
    }
}

TEST_CASE("Moonraker upload and print commands use stable transport requests", "[Moonraker][MobileSDK]")
{
    auto http = std::make_shared<FakeHttp>();
    http->handler = [](const MoonrakerHttpRequest& request) {
        if (request.path == "/server/info") return ok(server_info());
        if (request.path == "/server/files/upload") return ok(R"({"result":{"item":{"path":"gcodes/uploaded.gcode"}}})");
        if (request.path == "/printer/print/start" || request.path == "/printer/gcode/script") return ok(R"({"result":null})");
        return MoonrakerHttpResponse{404, {}, "unexpected path"};
    };

    MoonrakerAdapter adapter(http);
    REQUIRE(adapter.probe(endpoint()));
    REQUIRE(adapter.upload_gcode({"file:///tmp/benchy.gcode", "sha256", "benchy.gcode"}).success);
    REQUIRE(adapter.last_uploaded_path() == "gcodes/uploaded.gcode");
    REQUIRE(adapter.start_print({}).success);
    REQUIRE(adapter.pause().success);
    REQUIRE(adapter.resume().success);
    REQUIRE(adapter.cancel().success);

    REQUIRE(http->requests.size() == 6);
    REQUIRE(http->requests[1].form_fields.at("root") == "gcodes");
    REQUIRE(http->requests[1].upload_source == "file:///tmp/benchy.gcode");
    REQUIRE(http->requests[1].upload_filename == "benchy.gcode");
    REQUIRE(http->requests[1].upload_sha256 == "sha256");
    REQUIRE(nlohmann::json::parse(http->requests[2].body).at("filename") == "gcodes/uploaded.gcode");
    REQUIRE(nlohmann::json::parse(http->requests[3].body).at("script") == "PAUSE");
    REQUIRE(nlohmann::json::parse(http->requests[4].body).at("script") == "RESUME");
    REQUIRE(nlohmann::json::parse(http->requests[5].body).at("script") == "CANCEL_PRINT");
}

TEST_CASE("Moonraker maps HTTP and JSON fault matrix to stable errors", "[Moonraker][MobileSDK]")
{
    SECTION("authentication")
    {
        auto http = std::make_shared<FakeHttp>();
        http->handler = [](const MoonrakerHttpRequest&) { return MoonrakerHttpResponse{401, "", "unauthorized"}; };
        MoonrakerAdapter adapter(http);
        REQUIRE_FALSE(adapter.probe(endpoint()));
        REQUIRE(adapter.last_result().code == "moonraker.unauthorized");
        REQUIRE_FALSE(adapter.last_result().retryable);
    }

    SECTION("transport failure")
    {
        auto http = std::make_shared<FakeHttp>();
        http->handler = [](const MoonrakerHttpRequest&) { return MoonrakerHttpResponse{0, "", "connection reset"}; };
        MoonrakerAdapter adapter(http);
        REQUIRE_FALSE(adapter.probe(endpoint()));
        REQUIRE(adapter.last_result().code == "moonraker.transport_error");
        REQUIRE(adapter.last_result().retryable);
    }

    SECTION("cancelled request")
    {
        auto http = std::make_shared<FakeHttp>();
        http->handler = [](const MoonrakerHttpRequest&) { return MoonrakerHttpResponse{0, "", "Moonraker HTTP request cancelled"}; };
        MoonrakerAdapter adapter(http);
        REQUIRE_FALSE(adapter.probe(endpoint()));
        REQUIRE(adapter.last_result().code == "moonraker.cancelled");
        REQUIRE_FALSE(adapter.last_result().retryable);
    }

    SECTION("invalid JSON")
    {
        auto http = std::make_shared<FakeHttp>();
        http->handler = [](const MoonrakerHttpRequest&) { return ok("not-json"); };
        MoonrakerAdapter adapter(http);
        REQUIRE_FALSE(adapter.probe(endpoint()));
        REQUIRE(adapter.last_result().code == "moonraker.invalid_json");
        REQUIRE_FALSE(adapter.last_result().retryable);
    }

    SECTION("RPC error")
    {
        auto http = std::make_shared<FakeHttp>();
        http->handler = [](const MoonrakerHttpRequest&) { return ok(R"({"error":{"code":-1,"message":"rpc failed"}})"); };
        MoonrakerAdapter adapter(http);
        REQUIRE_FALSE(adapter.probe(endpoint()));
        REQUIRE(adapter.last_result().code == "moonraker.rpc_error");
        REQUIRE(adapter.last_result().message == "rpc failed");
    }

    SECTION("missing result and status")
    {
        auto http = std::make_shared<FakeHttp>();
        http->handler = [](const MoonrakerHttpRequest& request) {
            if (request.path == "/server/info") return ok(R"({"result":{"klippy_state":"ready"}})");
            if (request.path == "/printer/objects/query?print_stats&virtual_sdcard&extruder&heater_bed&fan&toolhead") {
                return ok(R"({"result":{}})");
            }
            return ok(R"({"result":{}})");
        };
        MoonrakerAdapter adapter(http);
        REQUIRE(adapter.probe(endpoint()));
        REQUIRE_FALSE(adapter.poll_status());
        REQUIRE(adapter.last_result().code == "moonraker.protocol_error");
    }
}

TEST_CASE("Moonraker adapter serializes callbacks with synchronous operations and teardown", "[Moonraker][MobileSDK][Concurrency]")
{
    auto http = std::make_shared<FakeHttp>();
    install_standard_http(http, "printing", 0.25);
    auto websocket = std::make_shared<FakeWebSocket>();
    auto adapter = std::make_unique<MoonrakerAdapter>(http, websocket, "concurrent-key");
    REQUIRE(adapter->probe(endpoint()));
    REQUIRE(adapter->subscribe_status());
    websocket->emit(R"({"jsonrpc":"2.0","method":"notify_status_update","params":[{"print_stats":{"state":"printing","progress":0.7}}]})");
    REQUIRE(adapter->get_status().state == PrinterState::Printing);

    std::atomic_bool start{false};
    std::atomic_bool all_status_ok{true};
    std::vector<std::thread> workers;
    for (int index = 0; index < 3; ++index) {
        workers.emplace_back([&]() {
            while (!start.load()) std::this_thread::yield();
            for (int count = 0; count < 50; ++count) {
                websocket->emit(R"({"jsonrpc":"2.0","method":"notify_status_update","params":[{"print_stats":{"state":"printing","progress":0.7}}]})");
            }
        });
    }
    workers.emplace_back([&]() {
        while (!start.load()) std::this_thread::yield();
        for (int count = 0; count < 50; ++count) {
            const auto status = adapter->get_status();
            if (status.state != PrinterState::Printing) all_status_ok.store(false);
        }
    });
    start.store(true);
    for (auto& worker : workers) worker.join();

    REQUIRE(all_status_ok.load());
    REQUIRE(adapter->get_status().state == PrinterState::Printing);
    adapter.reset();

    // The transport can retain callback functors after close. They must become
    // no-ops once the adapter lifetime has ended.
    websocket->emit(R"({"jsonrpc":"2.0","method":"notify_status_update","params":[{"print_stats":{"state":"error"}}]})");
    websocket->disconnect_from_server();
}
