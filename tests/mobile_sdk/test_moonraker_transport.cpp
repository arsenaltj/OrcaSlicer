#include <catch2/catch_all.hpp>

#include "orca/connectivity/moonraker/MoonrakerTransport.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <utility>

using namespace Orca::SDK;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;
using error_code = boost::system::error_code;
using namespace std::chrono_literals;

namespace {

class LoopbackHttpServer {
public:
    using Handler = std::function<http::response<http::string_body>(const http::request<http::string_body>&)>;

    explicit LoopbackHttpServer(Handler handler)
        : handler(std::move(handler))
        , acceptor(io, {tcp::v4(), 0})
    {
        thread = std::thread([this]() { run(); });
    }

    ~LoopbackHttpServer()
    {
        stopping.store(true);
        error_code ignored;
        acceptor.cancel(ignored);
        acceptor.close(ignored);
        if (thread.joinable()) thread.join();
    }

    unsigned short port() const { return acceptor.local_endpoint().port(); }

private:
    void run()
    {
        while (!stopping.load()) {
            tcp::socket socket(io);
            error_code ec;
            acceptor.accept(socket, ec);
            if (stopping.load()) break;
            if (ec) continue;

            beast::flat_buffer buffer;
            http::request<http::string_body> request;
            http::read(socket, buffer, request, ec);
            if (ec) continue;
            auto response = handler(request);
            response.version(request.version());
            response.keep_alive(false);
            response.prepare_payload();
            http::write(socket, response, ec);
        }
    }

    Handler handler;
    net::io_context io;
    tcp::acceptor acceptor;
    std::atomic_bool stopping{false};
    std::thread thread;
};

class LoopbackWebSocketServer {
public:
    explicit LoopbackWebSocketServer(std::string response,
                                     bool close_after_response = true,
                                     bool send_response = true)
        : response(std::move(response))
        , close_after_response(close_after_response)
        , send_response(send_response)
        , acceptor(io, {tcp::v4(), 0})
    {
        thread = std::thread([this]() { run(); });
    }

    ~LoopbackWebSocketServer()
    {
        stopping.store(true);
        error_code ignored;
        acceptor.cancel(ignored);
        acceptor.close(ignored);
        if (thread.joinable()) thread.join();
    }

    unsigned short port() const { return acceptor.local_endpoint().port(); }
    std::string received_header() const {
        std::lock_guard<std::mutex> lock(state_mutex);
        return api_key;
    }
    std::string received_payload() const {
        std::lock_guard<std::mutex> lock(state_mutex);
        return payload;
    }
    unsigned connection_count() const { return connections.load(); }

private:
    void run()
    {
        while (!stopping.load()) {
            tcp::socket socket(io);
            error_code ec;
            acceptor.accept(socket, ec);
            if (stopping.load()) break;
            if (ec) continue;

            websocket::stream<tcp::socket> ws(std::move(socket));
            http::request<http::string_body> request;
            ws.accept(request, ec);
            if (ec) continue;
            connections.fetch_add(1);
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                api_key = request["X-Api-Key"].to_string();
            }

            beast::flat_buffer buffer;
            ws.read(buffer, ec);
            if (!ec) {
                std::lock_guard<std::mutex> lock(state_mutex);
                payload = beast::buffers_to_string(buffer.data());
            }
            if (!ec && send_response) {
                ws.text(true);
                ws.write(net::buffer(response), ec);
            }
            if (!ec && close_after_response) {
                std::this_thread::sleep_for(200ms);
                ws.close(websocket::close_code::normal, ec);
            }
        }
    }

    std::string response;
    bool close_after_response{true};
    bool send_response{true};
    mutable std::mutex state_mutex;
    std::string api_key;
    std::string payload;
    net::io_context io;
    tcp::acceptor acceptor;
    std::atomic_bool stopping{false};
    std::atomic_uint connections{0};
    std::thread thread;
};

http::response<http::string_body> json_response(unsigned status, std::string body)
{
    http::response<http::string_body> response{http::status(status), 11};
    response.set(http::field::content_type, "application/json");
    response.body() = std::move(body);
    return response;
}

} // namespace

TEST_CASE("Concrete Moonraker HTTP transport sends API key and JSON requests", "[Moonraker][Transport][Loopback]")
{
    std::string request_api_key;
    std::string request_body;
    std::mutex request_mutex;
    LoopbackHttpServer server([&](const auto& request) {
        {
            std::lock_guard<std::mutex> lock(request_mutex);
            request_api_key = request["X-Api-Key"].to_string();
            request_body = request.body();
        }
        return json_response(200, R"({"result":{"ok":true}})");
    });

    MoonrakerHttpTransport transport;
    MoonrakerHttpRequest request;
    request.method = "POST";
    request.url = "http://127.0.0.1:" + std::to_string(server.port()) + "/printer/gcode/script";
    request.headers["X-Api-Key"] = "loopback-key";
    request.headers["Content-Type"] = "application/json";
    request.body = R"({"script":"STATUS"})";

    const auto response = transport.perform(request);
    REQUIRE(response.status_code == 200);
    REQUIRE(response.body == R"({"result":{"ok":true}})");
    {
        std::lock_guard<std::mutex> lock(request_mutex);
        REQUIRE(request_api_key == "loopback-key");
        REQUIRE(request_body == request.body);
    }
}

TEST_CASE("Concrete Moonraker HTTP transport validates upload SHA-256 and multipart data", "[Moonraker][Transport][Loopback]")
{
    std::string request_body;
    std::mutex request_mutex;
    LoopbackHttpServer server([&](const auto& request) {
        {
            std::lock_guard<std::mutex> lock(request_mutex);
            request_body = request.body();
        }
        return json_response(200, R"({"result":{"item":{"path":"gcodes/benchy.gcode"}}})");
    });

    const auto path = std::filesystem::temp_directory_path() / "orca-moonraker-transport-upload.gcode";
    {
        std::ofstream output(path, std::ios::binary);
        output << "hello";
    }

    MoonrakerHttpTransport transport;
    MoonrakerHttpRequest request;
    request.method = "POST";
    request.url = "http://127.0.0.1:" + std::to_string(server.port()) + "/server/files/upload";
    request.form_fields = {{"root", "gcodes"}, {"print", "false"}};
    request.upload_source = path.string();
    request.upload_filename = "benchy.gcode";
    request.upload_sha256 = "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824";

    const auto response = transport.perform(request);
    REQUIRE(response.status_code == 200);
    {
        std::lock_guard<std::mutex> lock(request_mutex);
        REQUIRE(request_body.find("benchy.gcode") != std::string::npos);
        REQUIRE(request_body.find("hello") != std::string::npos);
    }

    request.upload_sha256 = std::string(64, '0');
    const auto mismatch = transport.perform(request);
    REQUIRE(mismatch.status_code == 0);
    REQUIRE(mismatch.error.find("SHA-256") != std::string::npos);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

TEST_CASE("Concrete Moonraker HTTP transport cancellation interrupts a request", "[Moonraker][Transport][Loopback]")
{
    LoopbackHttpServer server([](const auto&) {
        std::this_thread::sleep_for(2s);
        return json_response(200, R"({"result":null})");
    });

    MoonrakerTransportOptions options;
    options.total_timeout = 10s;
    MoonrakerHttpTransport transport(options);
    MoonrakerHttpRequest request;
    request.url = "http://127.0.0.1:" + std::to_string(server.port()) + "/slow";

    MoonrakerHttpResponse response;
    std::thread worker([&]() { response = transport.perform(request); });
    std::this_thread::sleep_for(150ms);
    transport.cancel();
    worker.join();

    REQUIRE(response.status_code == 0);
    REQUIRE(response.error.find("cancelled") != std::string::npos);
}

TEST_CASE("Concrete Moonraker WebSocket transport sends headers and receives callbacks", "[Moonraker][Transport][Loopback]")
{
    LoopbackWebSocketServer server(R"({"method":"notify_status_update","params":[{"print_stats":{"state":"printing","progress":0.5}}]})");
    MoonrakerWebSocketTransport transport;

    std::mutex mutex;
    std::condition_variable condition;
    std::string message;
    bool received = false;
    REQUIRE(transport.connect(
        "ws://127.0.0.1:" + std::to_string(server.port()) + "/websocket",
        {{"X-Api-Key", "websocket-key"}},
        [&](const std::string& value) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                message = value;
                received = true;
            }
            condition.notify_one();
        },
        []() {}));

    REQUIRE(transport.send(R"({"jsonrpc":"2.0","method":"printer.objects.subscribe","id":1})"));
    {
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(condition.wait_for(lock, 2s, [&]() { return received; }));
    }
    REQUIRE(message.find("notify_status_update") != std::string::npos);
    REQUIRE(server.received_header() == "websocket-key");
    REQUIRE(server.received_payload().find("printer.objects.subscribe") != std::string::npos);
    transport.close();
}

TEST_CASE("Concrete Moonraker WebSocket reconnect from a message callback keeps the new reader", "[Moonraker][Transport][Concurrency]")
{
    LoopbackWebSocketServer server(R"({"method":"notify_status_update","params":[{"print_stats":{"state":"printing"}}]})");
    MoonrakerWebSocketTransport transport;
    const auto url = "ws://127.0.0.1:" + std::to_string(server.port()) + "/websocket";

    std::mutex mutex;
    std::condition_variable condition;
    unsigned messages = 0;
    std::atomic_bool reconnect_ok{false};
    auto handler = std::make_shared<IMoonrakerWebSocketTransport::MessageHandler>();
    *handler = [&](const std::string&) {
        unsigned count;
        {
            std::lock_guard<std::mutex> lock(mutex);
            count = ++messages;
        }
        if (count == 1) {
            reconnect_ok = transport.connect(url, {}, *handler, []() {});
            if (reconnect_ok) reconnect_ok = transport.send(R"({"jsonrpc":"2.0","method":"printer.objects.subscribe"})");
        }
        condition.notify_all();
    };

    REQUIRE(transport.connect(url, {}, *handler, []() {}));
    REQUIRE(transport.send(R"({"jsonrpc":"2.0","method":"printer.objects.subscribe"})"));
    {
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(condition.wait_for(lock, 3s, [&]() { return messages >= 2; }));
    }
    REQUIRE(reconnect_ok.load());
    REQUIRE(server.connection_count() >= 2);
    transport.close();
}

TEST_CASE("Concrete Moonraker WebSocket serializes concurrent close send and reconnect", "[Moonraker][Transport][Concurrency]")
{
    LoopbackWebSocketServer server(R"({"result":null})");
    MoonrakerWebSocketTransport transport;
    const auto url = "ws://127.0.0.1:" + std::to_string(server.port()) + "/websocket";
    REQUIRE(transport.connect(url, {}, [](const std::string&) {}, []() {}));

    std::atomic_bool start{false};
    std::vector<std::thread> workers;
    for (int index = 0; index < 4; ++index) {
        workers.emplace_back([&]() {
            while (!start.load()) std::this_thread::yield();
            for (int count = 0; count < 40; ++count) {
                transport.send("{}" );
            }
        });
    }
    workers.emplace_back([&]() {
        while (!start.load()) std::this_thread::yield();
        for (int count = 0; count < 8; ++count) {
            transport.close();
            transport.connect(url, {}, [](const std::string&) {}, []() {});
        }
    });
    start.store(true);
    for (auto& worker : workers) worker.join();
    transport.close();
    REQUIRE(server.connection_count() >= 1);
}

TEST_CASE("Concrete Moonraker WebSocket does not treat a quiet connection as idle timeout", "[Moonraker][Transport][Timeout]")
{
    SECTION("default read timeout is disabled")
    {
        LoopbackWebSocketServer server({}, false, false);
        MoonrakerTransportOptions options;
        options.total_timeout = 100ms;
        MoonrakerWebSocketTransport transport(options);
        std::atomic_uint closes{0};
        REQUIRE(transport.connect(
            "ws://127.0.0.1:" + std::to_string(server.port()) + "/websocket",
            {}, [](const std::string&) {}, [&]() { closes.fetch_add(1); }));
        std::this_thread::sleep_for(250ms);
        REQUIRE(closes.load() == 0);
        transport.close();
    }

    SECTION("an explicit read idle timeout closes the connection")
    {
        LoopbackWebSocketServer server({}, false, false);
        MoonrakerTransportOptions options;
        options.total_timeout = 1s;
        options.read_idle_timeout = 75ms;
        MoonrakerWebSocketTransport transport(options);
        std::mutex mutex;
        std::condition_variable condition;
        bool closed = false;
        REQUIRE(transport.connect(
            "ws://127.0.0.1:" + std::to_string(server.port()) + "/websocket",
            {}, [](const std::string&) {}, [&]() {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    closed = true;
                }
                condition.notify_all();
            }));
        {
            std::unique_lock<std::mutex> lock(mutex);
            REQUIRE(condition.wait_for(lock, 2s, [&]() { return closed; }));
        }
        transport.close();
    }
}
