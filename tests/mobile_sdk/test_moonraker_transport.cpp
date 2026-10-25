#include <catch2/catch_all.hpp>

#include "orca/connectivity/moonraker/MoonrakerTransport.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/ssl.hpp>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
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

using namespace Orca::MobileDevice;
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
    using Handler=std::function<http::response<http::string_body>(const http::request<http::string_body>&)>;
    explicit LoopbackHttpServer(Handler value):handler(std::move(value)),acceptor(io,{net::ip::make_address("127.0.0.1"),0}) {
        accept_next();thread=std::thread([this] { io.run(); });
    }
    ~LoopbackHttpServer() { io.stop();if(thread.joinable())thread.join(); }
    unsigned short port()const{return acceptor.local_endpoint().port();}
private:
    void accept_next() {
        auto socket=std::make_shared<tcp::socket>(io);
        acceptor.async_accept(*socket,[this,socket](error_code ec) {
            if(ec)return;accept_next();
            auto buffer=std::make_shared<beast::flat_buffer>();
            auto request=std::make_shared<http::request<http::string_body>>();
            http::async_read(*socket,*buffer,*request,[this,socket,buffer,request](error_code ec,std::size_t) {
                if(ec)return;
                auto response=std::make_shared<http::response<http::string_body>>(handler(*request));
                response->version(request->version());response->keep_alive(false);response->prepare_payload();
                http::async_write(*socket,*response,[socket,response](error_code,std::size_t){});
            });
        });
    }
    Handler handler;net::io_context io;tcp::acceptor acceptor;std::thread thread;
};

class LoopbackWebSocketServer {
    struct Peer {
        explicit Peer(net::io_context& io):socket(io),timer(io){}
        websocket::stream<tcp::socket> socket;
        beast::flat_buffer buffer;
        http::request<http::string_body> upgrade;
        net::steady_timer timer;
    };
public:
    explicit LoopbackWebSocketServer(std::string value,bool close_after=true,bool send=true)
        :response(std::move(value)),close_after_response(close_after),send_response(send),
         acceptor(io,{net::ip::make_address("127.0.0.1"),0}) {
        accept_next();thread=std::thread([this]{io.run();});
    }
    ~LoopbackWebSocketServer(){io.stop();if(thread.joinable())thread.join();}
    unsigned short port()const{return acceptor.local_endpoint().port();}
    std::string received_header()const{std::lock_guard<std::mutex> lock(state_mutex);return api_key;}
    std::string received_payload()const{std::lock_guard<std::mutex> lock(state_mutex);return payload;}
    unsigned connection_count()const{return connections.load();}
private:
    void accept_next() {
        auto peer=std::make_shared<Peer>(io);
        acceptor.async_accept(peer->socket.next_layer(),[this,peer](error_code ec) {
            if(ec)return;accept_next();
            // async_accept(request) consumes an already parsed upgrade request.
            http::async_read(peer->socket.next_layer(),peer->buffer,peer->upgrade,[this,peer](error_code ec,std::size_t) {
                if(ec)return;
                peer->socket.async_accept(peer->upgrade,[this,peer](error_code ec) {
                    if(ec)return;connections.fetch_add(1);
                    {std::lock_guard<std::mutex> lock(state_mutex);api_key=std::string(peer->upgrade["X-Api-Key"].data(),peer->upgrade["X-Api-Key"].size());}
                    peer->buffer.consume(peer->buffer.size());
                    peer->socket.async_read(peer->buffer,[this,peer](error_code ec,std::size_t) {
                        if(ec)return;
                        {std::lock_guard<std::mutex> lock(state_mutex);payload=beast::buffers_to_string(peer->buffer.data());}
                        if(!send_response)return;
                        peer->socket.text(true);
                        peer->socket.async_write(net::buffer(response),[this,peer](error_code ec,std::size_t) {
                            if(ec||!close_after_response)return;
                            peer->timer.expires_after(200ms);
                            peer->timer.async_wait([peer](error_code ec) {
                                if(!ec)peer->socket.async_close(websocket::close_code::normal,[peer](error_code){});
                            });
                        });
                    });
                });
            });
        });
    }
    std::string response;bool close_after_response,send_response;
    mutable std::mutex state_mutex;std::string api_key,payload;
    net::io_context io;tcp::acceptor acceptor;std::atomic_uint connections{0};std::thread thread;
};

// Certificates are generated for loopback tests at runtime; no private key is
// checked in or written to disk. The only saved fixture is a public CA cert.
class LoopbackTlsServer {
    struct Peer {
        Peer(net::io_context& io,net::ssl::context& tls):socket(io,tls){}
        websocket::stream<net::ssl::stream<tcp::socket>> socket;
        beast::flat_buffer buffer;
        http::request<http::string_body> request;
        std::string payload{"{\"result\":\"tls-loopback\"}"};
    };
public:
    explicit LoopbackTlsServer(bool websocket_mode):ws_mode(websocket_mode),tls(net::ssl::context::tls_server),
        acceptor(io,{net::ip::make_address("127.0.0.1"),0}) {
        std::unique_ptr<EVP_PKEY_CTX,decltype(&EVP_PKEY_CTX_free)> gen(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA,nullptr),EVP_PKEY_CTX_free);
        EVP_PKEY* raw=nullptr;
        if(!gen||EVP_PKEY_keygen_init(gen.get())!=1||EVP_PKEY_CTX_set_rsa_keygen_bits(gen.get(),2048)!=1||EVP_PKEY_keygen(gen.get(),&raw)!=1)
            throw std::runtime_error("test TLS key generation failed");
        std::unique_ptr<EVP_PKEY,decltype(&EVP_PKEY_free)> key(raw,EVP_PKEY_free);
        std::unique_ptr<X509,decltype(&X509_free)> cert(X509_new(),X509_free);
        if(!cert)throw std::runtime_error("test TLS certificate allocation failed");
        X509_set_version(cert.get(),2);ASN1_INTEGER_set(X509_get_serialNumber(cert.get()),1);
        X509_gmtime_adj(X509_getm_notBefore(cert.get()),-60);X509_gmtime_adj(X509_getm_notAfter(cert.get()),3600);
        X509_set_pubkey(cert.get(),key.get());
        std::unique_ptr<X509_NAME,decltype(&X509_NAME_free)> subject(X509_NAME_new(),X509_NAME_free);auto* name=subject.get();
        X509_NAME_add_entry_by_txt(name,"CN",MBSTRING_ASC,reinterpret_cast<const unsigned char*>("SDK loopback"),-1,-1,0);
        X509_set_subject_name(cert.get(),name);X509_set_issuer_name(cert.get(),name);
        auto add_extension=[&](int nid,const char* value) {
            X509V3_CTX ctx;X509V3_set_ctx(&ctx,cert.get(),cert.get(),nullptr,nullptr,0);
            std::unique_ptr<X509_EXTENSION,decltype(&X509_EXTENSION_free)> ext(
                X509V3_EXT_conf_nid(nullptr,&ctx,nid,const_cast<char*>(value)),X509_EXTENSION_free);
            if(!ext||X509_add_ext(cert.get(),ext.get(),-1)!=1)throw std::runtime_error("test certificate extension failed");
        };
        add_extension(NID_basic_constraints,"critical,CA:TRUE");
        add_extension(NID_key_usage,"critical,digitalSignature,keyEncipherment,keyCertSign");
        add_extension(NID_subject_alt_name,"IP:127.0.0.1");
        if(X509_sign(cert.get(),key.get(),EVP_sha256())<=0||
           SSL_CTX_use_certificate(tls.native_handle(),cert.get())!=1||SSL_CTX_use_PrivateKey(tls.native_handle(),key.get())!=1)
            throw std::runtime_error("test TLS identity setup failed");
        std::unique_ptr<BIO,decltype(&BIO_free)> pem(BIO_new(BIO_s_mem()),BIO_free);
        PEM_write_bio_X509(pem.get(),cert.get());char* bytes=nullptr;auto length=BIO_get_mem_data(pem.get(),&bytes);
        ca_path=std::filesystem::temp_directory_path()/ ("orca-moonraker-ca-"+std::to_string(acceptor.local_endpoint().port())+".pem");
        std::ofstream(ca_path,std::ios::binary).write(bytes,length);
        accept_next();thread=std::thread([this]{io.run();});
    }
    ~LoopbackTlsServer(){io.stop();if(thread.joinable())thread.join();std::error_code ignored;std::filesystem::remove(ca_path,ignored);}
    unsigned short port()const{return acceptor.local_endpoint().port();}
    std::string ca_file()const{return ca_path.string();}
private:
    void accept_next(){
        auto peer=std::make_shared<Peer>(io,tls);
        acceptor.async_accept(peer->socket.next_layer().next_layer(),[this,peer](error_code ec){
            if(ec)return;accept_next();
            peer->socket.next_layer().async_handshake(net::ssl::stream_base::server,[this,peer](error_code ec){
                if(ec)return;
                http::async_read(peer->socket.next_layer(),peer->buffer,peer->request,[this,peer](error_code ec,std::size_t){
                    if(ec)return;
                    if(ws_mode){
                        peer->socket.async_accept(peer->request,[peer](error_code ec){
                            if(ec)return;peer->buffer.consume(peer->buffer.size());
                            peer->socket.async_read(peer->buffer,[peer](error_code ec,std::size_t){
                                if(ec)return;peer->socket.text(true);
                                peer->socket.async_write(net::buffer(peer->payload),[peer](error_code,std::size_t){});
                            });
                        });
                    }else{
                        auto response=std::make_shared<http::response<http::string_body>>(http::status::ok,11);
                        response->body()=peer->payload;response->prepare_payload();response->keep_alive(false);
                        http::async_write(peer->socket.next_layer(),*response,[peer,response](error_code,std::size_t){});
                    }
                });
            });
        });
    }
    bool ws_mode;net::io_context io;net::ssl::context tls;tcp::acceptor acceptor;std::thread thread;std::filesystem::path ca_path;
};
class SilentHandshakeServer {
public:
    SilentHandshakeServer():acceptor(io,{net::ip::make_address("127.0.0.1"),0}) {accept_next();thread=std::thread([this]{io.run();});}
    ~SilentHandshakeServer(){io.stop();if(thread.joinable())thread.join();}
    unsigned short port()const{return acceptor.local_endpoint().port();}
private:
    void accept_next(){auto socket=std::make_shared<tcp::socket>(io);acceptor.async_accept(*socket,[this,socket](error_code ec){if(!ec){sockets.push_back(socket);accept_next();}});}
    net::io_context io;tcp::acceptor acceptor;std::thread thread;std::vector<std::shared_ptr<tcp::socket>> sockets;
};

// Reproduce libcurl's automatic 417 retry only after MIME file bytes have
// already been read. The successful retry must carry the whole file again.
class ExpectRetryServer {
    struct First {
        explicit First(net::io_context& io):socket(io),parser() {parser.body_limit(32*1024*1024);}
        tcp::socket socket;beast::flat_buffer buffer;http::request_parser<http::buffer_body> parser;
        std::array<char,1024> bytes{};std::size_t body_bytes{0};std::string tail;
        std::string response{"HTTP/1.1 100 Continue\r\n\r\n"};
    };
public:
    ExpectRetryServer():acceptor(io,{net::ip::make_address("127.0.0.1"),0}) {accept_next();thread=std::thread([this]{io.run();});}
    ~ExpectRetryServer(){io.stop();if(thread.joinable())thread.join();}
    unsigned short port()const{return acceptor.local_endpoint().port();}
    bool consumed_file_bytes()const{return consumed.load();}
    std::string retried_body()const{std::lock_guard<std::mutex> lock(mutex);return retry_body;}
private:
    void read_retry(const std::shared_ptr<First>& peer) {
        peer->socket.async_read_some(net::buffer(peer->bytes),[this,peer](error_code ec,std::size_t count){
            if(ec)return;peer->tail.append(peer->bytes.data(),count);
            auto at=peer->tail.find("POST /upload HTTP/1.1\r\n");
            if(at==std::string::npos){if(peer->tail.size()>4096)peer->tail.erase(0,peer->tail.size()-4096);return read_retry(peer);}
            peer->buffer.consume(peer->buffer.size());
            auto copy=peer->tail.substr(at);net::buffer_copy(peer->buffer.prepare(copy.size()),net::buffer(copy));peer->buffer.commit(copy.size());
            auto parser=std::make_shared<http::request_parser<http::string_body>>();parser->body_limit(32*1024*1024);
            http::async_read(peer->socket,peer->buffer,*parser,[this,peer,parser](error_code ec,std::size_t){
                if(ec)return;{std::lock_guard<std::mutex> lock(mutex);retry_body=parser->get().body();}
                auto reply=std::make_shared<http::response<http::string_body>>(http::status::ok,11);
                reply->body()="{}";reply->prepare_payload();reply->keep_alive(false);
                http::async_write(peer->socket,*reply,[peer,reply](error_code,std::size_t){});
            });
        });
    }
    void read_prefix(const std::shared_ptr<First>& peer) {
        peer->parser.get().body().data=peer->bytes.data();peer->parser.get().body().size=peer->bytes.size();
        http::async_read_some(peer->socket,peer->buffer,peer->parser,[this,peer](error_code ec,std::size_t){
            if(ec && ec!=http::error::need_buffer)return;
            peer->body_bytes+=peer->bytes.size()-peer->parser.get().body().size;
            if(peer->body_bytes<2048)return read_prefix(peer);
            consumed.store(true);
            auto reply=std::make_shared<http::response<http::string_body>>(http::status::expectation_failed,11);
            reply->keep_alive(true);reply->prepare_payload();
            http::async_write(peer->socket,*reply,[this,peer,reply](error_code ec,std::size_t){if(!ec)read_retry(peer);});
        });
    }
    void accept_next(){
        auto socket=std::make_shared<tcp::socket>(io);
        acceptor.async_accept(*socket,[this,socket](error_code ec){
            if(ec)return;bool first=connections++==0;accept_next();
            if(first){
                auto peer=std::make_shared<First>(io);peer->socket=std::move(*socket);held.push_back(peer);
                http::async_read_header(peer->socket,peer->buffer,peer->parser,[this,peer](error_code ec,std::size_t){
                    if(ec)return;
                    net::async_write(peer->socket,net::buffer(peer->response),[this,peer](error_code ec,std::size_t){
                        if(ec)return;
                        read_prefix(peer);
                    });
                });
            }else{
                auto buffer=std::make_shared<beast::flat_buffer>();
                auto parser=std::make_shared<http::request_parser<http::string_body>>();parser->body_limit(32*1024*1024);
                http::async_read(*socket,*buffer,*parser,[this,socket,buffer,parser](error_code ec,std::size_t){
                    if(ec)return;{std::lock_guard<std::mutex> lock(mutex);retry_body=parser->get().body();}
                    auto reply=std::make_shared<http::response<http::string_body>>(http::status::ok,11);
                    reply->body()="{}";reply->keep_alive(false);reply->prepare_payload();
                    http::async_write(*socket,*reply,[socket,reply](error_code,std::size_t){});
                });
            }
        });
    }
    net::io_context io;tcp::acceptor acceptor;std::thread thread;unsigned connections{0};
    std::atomic_bool consumed{false};mutable std::mutex mutex;std::string retry_body;
    std::vector<std::shared_ptr<First>> held;
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
            request_api_key = std::string(request["X-Api-Key"].data(),request["X-Api-Key"].size());
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


TEST_CASE("Native Moonraker HTTP enforces its total request timeout", "[Moonraker][Transport][Timeout]")
{
    LoopbackHttpServer server([](const auto&){std::this_thread::sleep_for(300ms);return json_response(200,"{}");});
    MoonrakerTransportOptions options;options.total_timeout=60ms;
    MoonrakerHttpTransport transport(options);MoonrakerHttpRequest request;
    request.url="http://127.0.0.1:"+std::to_string(server.port())+"/timeout";
    auto start=std::chrono::steady_clock::now();auto result=transport.perform(request);
    CHECK(result.status_code==0);CHECK_FALSE(result.error.empty());
    CHECK(std::chrono::steady_clock::now()-start<2s);
}
TEST_CASE("Native Moonraker WebSocket bounds silent upgrade and TLS handshakes", "[Moonraker][Transport][Timeout][TLS]")
{
    SilentHandshakeServer server;
    for(const std::string scheme:{"ws://","wss://"}) {
        MoonrakerTransportOptions options;options.connect_timeout=75ms;options.total_timeout=100ms;
        MoonrakerWebSocketTransport transport(options);
        auto start=std::chrono::steady_clock::now();
        CHECK_FALSE(transport.connect(scheme+"127.0.0.1:"+std::to_string(server.port())+"/websocket",{},[](const auto&){},[]{}));
        CHECK(std::chrono::steady_clock::now()-start<2s);
        CHECK(transport.last_error().find("timeout")!=std::string::npos);
    }
}
TEST_CASE("Native Moonraker WSS verifies trust and certificate hostnames", "[Moonraker][Transport][TLS]")
{
    LoopbackTlsServer server(true);
    MoonrakerTransportOptions options;options.ca_file=server.ca_file();options.total_timeout=2s;
    MoonrakerWebSocketTransport transport(options);
    std::mutex mutex;std::condition_variable condition;bool received=false;
    REQUIRE(transport.connect("wss://127.0.0.1:"+std::to_string(server.port())+"/websocket",{},[&](const std::string& text){
        {std::lock_guard<std::mutex> lock(mutex);received=text.find("tls-loopback")!=std::string::npos;}condition.notify_all();
    },[]{}));
    REQUIRE(transport.send("{}"));
    {std::unique_lock<std::mutex> lock(mutex);REQUIRE(condition.wait_for(lock,2s,[&]{return received;}));}
    transport.close();
    CHECK_FALSE(transport.connect("wss://localhost:"+std::to_string(server.port())+"/websocket",{},[](const auto&){},[]{}));
    MoonrakerWebSocketTransport untrusted;
    CHECK_FALSE(untrusted.connect("wss://127.0.0.1:"+std::to_string(server.port())+"/websocket",{},[](const auto&){},[]{}));
}
TEST_CASE("Native Moonraker HTTPS accepts explicit trust and rejects untrusted servers", "[Moonraker][Transport][TLS]")
{
    LoopbackTlsServer server(false);MoonrakerTransportOptions options;options.ca_file=server.ca_file();options.total_timeout=2s;
    MoonrakerHttpTransport trusted(options);MoonrakerHttpRequest request;
    request.url="https://127.0.0.1:"+std::to_string(server.port())+"/server/info";
    auto accepted=trusted.perform(request);
    INFO(accepted.error);
    CHECK(accepted.status_code==200);CHECK(accepted.body.find("tls-loopback")!=std::string::npos);
    MoonrakerHttpTransport untrusted;auto rejected=untrusted.perform(request);
    CHECK(rejected.status_code==0);CHECK_FALSE(rejected.error.empty());
}


TEST_CASE("Native Moonraker upload reads UTF8 file URI paths through the host filesystem", "[Moonraker][Transport][Loopback]")
{
    std::string uploaded;
    LoopbackHttpServer server([&](const auto& request){uploaded=request.body();return json_response(200,"{}");});
    const auto file=std::filesystem::temp_directory_path()/std::filesystem::u8path(u8"orca-native-模型 space.gcode");
    {std::ofstream stream(file,std::ios::binary);stream<<"hello";}
    auto uri=file.generic_u8string();
    std::string encoded;constexpr char digits[]="0123456789ABCDEF";
    for(unsigned char byte:uri) {
        if(byte>127||byte==' '){encoded+='%' ;encoded+=digits[byte>>4];encoded+=digits[byte&15];}
        else encoded+=static_cast<char>(byte);
    }
#ifdef _WIN32
    encoded='/'+encoded;
#endif
    MoonrakerHttpRequest request;request.method="POST";
    request.url="http://127.0.0.1:"+std::to_string(server.port())+"/upload";
    request.upload_source="file://"+encoded;request.upload_filename="model.gcode";
    request.upload_sha256="2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824";
    MoonrakerHttpTransport transport;auto result=transport.perform(request);
    std::error_code ignored;std::filesystem::remove(file,ignored);
    INFO(result.error);CHECK(result.status_code==200);CHECK(uploaded.find("hello")!=std::string::npos);
}


TEST_CASE("Native Moonraker multipart rewinds consumed file data for an automatic Expect retry", "[Moonraker][Transport][Loopback][UploadRetry]")
{
    ExpectRetryServer server;
    const auto file=std::filesystem::temp_directory_path()/"orca-moonraker-rewind.gcode";
    const std::string payload(16*1024*1024,'A');
    {std::ofstream output(file,std::ios::binary);output<<payload;}
    MoonrakerTransportOptions options;options.total_timeout=3s;
    MoonrakerHttpTransport transport(options);MoonrakerHttpRequest request;
    request.method="POST";request.url="http://127.0.0.1:"+std::to_string(server.port())+"/upload";
    request.upload_source=file.u8string();request.upload_filename="retry.gcode";
    const auto result=transport.perform(request);std::error_code ignored;std::filesystem::remove(file,ignored);
    INFO(result.error);CHECK(server.consumed_file_bytes());CHECK(result.status_code==200);
    CHECK(server.retried_body().find(payload)!=std::string::npos);
}
