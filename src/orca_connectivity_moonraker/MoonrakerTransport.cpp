#include "orca/connectivity/moonraker/MoonrakerTransport.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <curl/curl.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <deque>
#include <future>
#include <type_traits>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>

#include <utility>
#include <vector>

namespace Orca::MobileDevice {
namespace {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace websocket = beast::websocket;
using tcp = net::ip::tcp;
using error_code = boost::system::error_code;

std::once_flag curl_init_once;

void ensure_curl_initialized()
{
    std::call_once(curl_init_once, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

std::string url_decode(std::string value)
{
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const int high = hex(value[i + 1]);
            const int low = hex(value[i + 2]);
            if (high >= 0 && low >= 0) {
                decoded.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        decoded.push_back(value[i]);
    }
    return decoded;
}

std::filesystem::path upload_path_from_uri(const std::string& source, std::string& error)
{
    if (source.rfind("file://", 0) != 0) {
        std::filesystem::path path=std::filesystem::u8path(source);
        if (path.empty()) error = "Moonraker upload source is empty";
        return path;
    }

    std::string rest = source.substr(7);
    const auto slash = rest.find('/');
    if (!rest.empty() && rest.front() != '/') {
        const auto authority = slash == std::string::npos ? rest : rest.substr(0, slash);
        if (!authority.empty() && authority != "localhost") {
            error = "Moonraker upload source must be a local file URI";
            return {};
        }
        rest = slash == std::string::npos ? std::string{} : rest.substr(slash);
    }
    rest = url_decode(rest);
#if defined(_WIN32)
    if (rest.size() >= 3 && rest.front() == '/' && std::isalpha(static_cast<unsigned char>(rest[1])) && rest[2] == ':') {
        rest.erase(rest.begin());
    }
#endif
    if (rest.empty()) error = "Moonraker upload file URI has no path";
    return std::filesystem::u8path(rest);
}

std::string lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool has_api_key_header(const std::map<std::string, std::string>& headers)
{
    for (const auto& header : headers) {
        if (lowercase(header.first) == "x-api-key") return true;
    }
    return false;
}

bool sha256_file(const std::filesystem::path& path, std::string& digest, std::string& error)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "Moonraker upload source cannot be opened";
        return false;
    }

    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (!context || EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1) {
        if (context) EVP_MD_CTX_free(context);
        error = "SHA-256 initialization failed";
        return false;
    }

    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0 && EVP_DigestUpdate(context, buffer.data(), static_cast<std::size_t>(count)) != 1) {
            EVP_MD_CTX_free(context);
            error = "SHA-256 update failed";
            return false;
        }
    }
    if (!input.eof()) {
        EVP_MD_CTX_free(context);
        error = "Moonraker upload source could not be read";
        return false;
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
    unsigned int length = 0;
    const bool finalized = EVP_DigestFinal_ex(context, bytes.data(), &length) == 1;
    EVP_MD_CTX_free(context);
    if (!finalized || length != 32) {
        error = "SHA-256 finalization failed";
        return false;
    }

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < length; ++i) output << std::setw(2) << static_cast<unsigned int>(bytes[i]);
    digest = output.str();
    return true;
}

struct ParsedWebSocketUrl {
    bool secure{false};
    std::string host;
    std::string port;
    std::string authority;
    std::string target;
};

bool parse_websocket_url(const std::string& value, ParsedWebSocketUrl& result, std::string& error)
{
    const auto scheme_end = value.find("://");
    if (scheme_end == std::string::npos) {
        error = "Moonraker WebSocket URL has no scheme";
        return false;
    }
    const auto scheme = lowercase(value.substr(0, scheme_end));
    if (scheme != "ws" && scheme != "wss") {
        error = "Moonraker WebSocket URL must use ws or wss";
        return false;
    }
    result.secure = scheme == "wss";

    const auto authority_start = scheme_end + 3;
    const auto target_start = value.find_first_of("/?", authority_start);
    result.authority = value.substr(authority_start, target_start == std::string::npos ? std::string::npos : target_start - authority_start);
    result.target = target_start == std::string::npos ? "/" : value.substr(target_start);
    if (result.authority.empty()) {
        error = "Moonraker WebSocket URL has no host";
        return false;
    }

    if (result.authority.front() == '[') {
        const auto closing = result.authority.find(']');
        if (closing == std::string::npos) {
            error = "Moonraker WebSocket URL has an invalid IPv6 host";
            return false;
        }
        result.host = result.authority.substr(1, closing - 1);
        if (closing + 1 < result.authority.size()) {
            if (result.authority[closing + 1] != ':') {
                error = "Moonraker WebSocket URL has an invalid port";
                return false;
            }
            result.port = result.authority.substr(closing + 2);
        }
    } else {
        const auto colon = result.authority.rfind(':');
        if (colon != std::string::npos && result.authority.find(':') == colon) {
            result.host = result.authority.substr(0, colon);
            result.port = result.authority.substr(colon + 1);
        } else {
            result.host = result.authority;
        }
    }
    if (result.host.empty()) {
        error = "Moonraker WebSocket URL has no host";
        return false;
    }
    if (result.port.empty()) result.port = result.secure ? "443" : "80";
    return true;
}

template <typename WebSocket>
void decorate_websocket(WebSocket& socket,
                        const std::map<std::string, std::string>& headers)
{
    socket.set_option(websocket::stream_base::decorator([headers](websocket::request_type& request) {
        request.set(http::field::user_agent, "OrcaSlicer-Moonraker-SDK/1.0");
        for (const auto& header : headers) request.set(header.first, header.second);
    }));
}


// All socket operations, deadlines and write queues are confined to one io_context.
// External threads only post work. User callbacks may close or reconnect without
// joining themselves or retaining a lifecycle mutex across callbacks.
class WebSocketSession {
public:
    using Ready = std::function<void(bool, std::string)>;
    using Message = std::function<void(std::string)>;
    using Closed = std::function<void(std::string)>;
    using Written = std::function<void(bool)>;
    virtual ~WebSocketSession() = default;
    virtual void open(const ParsedWebSocketUrl&, const std::map<std::string,std::string>&,
                      Ready, Message, Closed) = 0;
    virtual void send(std::string, Written) = 0;
    virtual void stop() = 0;
};

template<bool Secure>
class AsyncWebSocketSession final : public WebSocketSession,
    public std::enable_shared_from_this<AsyncWebSocketSession<Secure>> {
    using Stream = std::conditional_t<Secure,
        websocket::stream<beast::ssl_stream<beast::tcp_stream>>,
        websocket::stream<beast::tcp_stream>>;
    struct Write { std::string payload; Written complete; };
    net::io_context& io;
    net::ssl::context tls{net::ssl::context::tls_client};
    tcp::resolver resolver;
    Stream socket;
    net::steady_timer opening, reading, writing;
    MoonrakerTransportOptions options;
    ParsedWebSocketUrl url;
    beast::flat_buffer incoming;
    std::deque<std::shared_ptr<Write>> writes;
    Ready ready;
    Message message;
    Closed closed;
    bool stopped{false}, connected{false};

    Stream make_stream() {
        if constexpr (Secure) return Stream(io, tls);
        else return Stream(io);
    }
    void complete_open(bool ok, std::string error) {
        if(ready) { auto fn=std::move(ready); fn(ok,std::move(error)); }
    }
    void fail(std::string error, bool notify=true) {
        if(stopped) return;
        stopped=true; connected=false;
        resolver.cancel(); opening.cancel(); reading.cancel(); writing.cancel();
        error_code ignored;
        auto& tcp_socket=beast::get_lowest_layer(socket).socket();
        tcp_socket.cancel(ignored);
        tcp_socket.shutdown(tcp::socket::shutdown_both,ignored);
        tcp_socket.close(ignored);
        complete_open(false,error);
        auto pending=std::move(writes);
        for(auto& item:pending) if(item->complete) { auto fn=std::move(item->complete); fn(false); }
        if(notify && closed) { auto fn=std::move(closed); fn(std::move(error)); }
    }
    void handshake() {
        auto self=this->shared_from_this();
        socket.async_handshake(url.authority,url.target,[self](error_code ec) {
            if(self->stopped) return;
            if(ec) return self->fail("Moonraker WebSocket handshake failed: "+ec.message());
            self->opening.cancel(); self->connected=true;
            self->complete_open(true,{});
            self->read_next();
        });
    }
    void read_next() {
        if(stopped) return;
        auto self=this->shared_from_this();
        if(options.read_idle_timeout.count()>0) {
            reading.expires_after(options.read_idle_timeout);
            reading.async_wait([self](error_code ec) {
                if(!ec) self->fail("Moonraker WebSocket idle timeout");
            });
        }
        socket.async_read(incoming,[self](error_code ec,std::size_t) {
            self->reading.cancel();
            if(self->stopped) return;
            if(ec) return self->fail("Moonraker WebSocket disconnected: "+ec.message());
            auto payload=beast::buffers_to_string(self->incoming.data());
            self->incoming.consume(self->incoming.size());
            try { if(self->message) self->message(std::move(payload)); }
            catch(const std::exception& e) { return self->fail(std::string("Moonraker callback failed: ")+e.what()); }
            catch(...) { return self->fail("Moonraker callback failed"); }
            self->read_next();
        });
    }
    void write_next() {
        if(stopped || writes.empty()) return;
        auto self=this->shared_from_this();
        auto item=writes.front(); // Retain buffer until the async completion, including on cancellation.
        writing.expires_after(options.total_timeout);
        writing.async_wait([self](error_code ec) { if(!ec) self->fail("Moonraker WebSocket send timeout"); });
        socket.text(true);
        socket.async_write(net::buffer(item->payload),[self,item](error_code ec,std::size_t) {
            self->writing.cancel();
            if(self->stopped) return;
            self->writes.pop_front();
            if(item->complete) { auto fn=std::move(item->complete); fn(!ec); }
            if(ec) return self->fail("Moonraker WebSocket send failed: "+ec.message());
            self->write_next();
        });
    }
public:
    AsyncWebSocketSession(net::io_context& context, MoonrakerTransportOptions value)
        :io(context),resolver(context),socket(make_stream()),opening(context),reading(context),writing(context),options(std::move(value)) {}
    void open(const ParsedWebSocketUrl& value,const std::map<std::string,std::string>& headers,
              Ready on_ready,Message on_message,Closed on_closed) override {
        ready=std::move(on_ready);message=std::move(on_message);closed=std::move(on_closed);url=value;
        if(stopped) return complete_open(false,"Moonraker connection canceled");
        error_code ec;
        if constexpr (Secure) {
            socket.next_layer().set_verify_mode(options.verify_tls?net::ssl::verify_peer:net::ssl::verify_none,ec);
            if(ec) return fail("Moonraker TLS verification setup failed: "+ec.message());
            if(options.verify_tls) {
                if(options.ca_file.empty()) tls.set_default_verify_paths(ec);
                else tls.load_verify_file(options.ca_file,ec);
                if(ec) return fail("Moonraker TLS CA setup failed: "+ec.message());
                socket.next_layer().set_verify_callback(net::ssl::host_name_verification(url.host));
            }
            if(!SSL_set_tlsext_host_name(socket.next_layer().native_handle(),url.host.c_str()))
                return fail("Moonraker TLS hostname setup failed");
        }
        decorate_websocket(socket,headers);
        socket.read_message_max(8*1024*1024);
        auto self=this->shared_from_this();
        opening.expires_after(std::min(options.connect_timeout,options.total_timeout));
        opening.async_wait([self](error_code ec) { if(!ec) self->fail("Moonraker WebSocket connection timeout"); });
        resolver.async_resolve(url.host,url.port,[self](error_code ec,tcp::resolver::results_type addresses) {
            if(self->stopped) return;
            if(ec) return self->fail("Moonraker resolve failed: "+ec.message());
            beast::get_lowest_layer(self->socket).async_connect(addresses,[self](error_code ec,const tcp::endpoint&) {
                if(self->stopped) return;
                if(ec) return self->fail("Moonraker connect failed: "+ec.message());
                if constexpr (Secure) {
                    self->socket.next_layer().async_handshake(net::ssl::stream_base::client,[self](error_code ec) {
                        if(self->stopped) return;
                        if(ec) return self->fail("Moonraker TLS handshake failed: "+ec.message());
                        self->handshake();
                    });
                } else self->handshake();
            });
        });
    }
    void send(std::string payload,Written result) override {
        if(stopped || !connected || writes.size()>=256 || payload.size()>8*1024*1024) { result(false);return; }
        bool idle=writes.empty();
        writes.push_back(std::make_shared<Write>(Write{std::move(payload),std::move(result)}));
        if(idle) write_next();
    }
    void stop() override { fail("Moonraker connection canceled",false); }
};

} // namespace

struct MoonrakerHttpTransport::Impl {
    explicit Impl(MoonrakerTransportOptions value) : options(std::move(value)) {}

    struct Operation {
        std::atomic_bool cancelled{false};
    };

    MoonrakerTransportOptions options;
    mutable std::mutex mutex;
    std::vector<std::shared_ptr<Operation>> active;
    std::string error;

    void set_error(std::string value)
    {
        std::lock_guard<std::mutex> lock(mutex);
        error = std::move(value);
    }

    std::string last_error() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return error;
    }

    void cancel()
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& operation : active) operation->cancelled.store(true);
    }

    static std::size_t write_body(char* data, std::size_t size, std::size_t count, void* userp)
    {
        auto* output = static_cast<std::string*>(userp);
        constexpr std::size_t maximum_body=16*1024*1024;
        if(size!=0 && count>maximum_body/size) return 0;
        const auto bytes=size*count;
        if(output->size()>maximum_body-bytes) return 0;
        try { output->append(data,bytes);return bytes; }
        catch(...) { return 0; } // No exception may unwind through libcurl's C callback.

    }

    static std::size_t read_upload(char* data,std::size_t size,std::size_t count,void* userp)
    {
        auto& input=*static_cast<std::ifstream*>(userp);
        const auto bytes=size*count;
        input.read(data,static_cast<std::streamsize>(bytes));
        if(input.bad()) return CURL_READFUNC_ABORT;
        return static_cast<std::size_t>(input.gcount());
    }
    static int progress(void* userp,
                        curl_off_t,
                        curl_off_t,
                        curl_off_t,
                        curl_off_t)
    {
        return static_cast<Operation*>(userp)->cancelled.load() ? 1 : 0;
    }

    MoonrakerHttpResponse perform(const MoonrakerHttpRequest& request)
    {
        ensure_curl_initialized();
        auto operation = std::make_shared<Operation>();
        {
            std::lock_guard<std::mutex> lock(mutex);
            active.push_back(operation);
        }
        const auto remove_operation = [&]() {
            std::lock_guard<std::mutex> lock(mutex);
            active.erase(std::remove(active.begin(), active.end(), operation), active.end());
        };

        MoonrakerHttpResponse response;
        try {
            if (request.url.empty()) {
                response.error = "Moonraker HTTP request has no URL";
                remove_operation();
                set_error(response.error);
                return response;
            }

            using CurlPtr = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
            CurlPtr curl(curl_easy_init(), &curl_easy_cleanup);
            if (!curl) {
                response.error = "Moonraker HTTP curl initialization failed";
                remove_operation();
                set_error(response.error);
                return response;
            }
            using HeaderPtr = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>;
            HeaderPtr header_list(nullptr, &curl_slist_free_all);
            using MimePtr = std::unique_ptr<curl_mime, decltype(&curl_mime_free)>;
            MimePtr mime(nullptr, &curl_mime_free);
            std::ifstream upload_stream;

            auto setopt = [&](CURLoption option, auto value) {
                return curl_easy_setopt(curl.get(), option, value) == CURLE_OK;
            };
            if (!setopt(CURLOPT_URL, request.url.c_str()) ||
                !setopt(CURLOPT_NOSIGNAL, 1L) ||
                !setopt(CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(std::max<std::int64_t>(1, options.connect_timeout.count()))) ||
                !setopt(CURLOPT_TIMEOUT_MS, static_cast<long>(std::max<std::int64_t>(1, options.total_timeout.count()))) ||
                !setopt(CURLOPT_WRITEFUNCTION, &write_body) ||
                !setopt(CURLOPT_WRITEDATA, &response.body) ||
                !setopt(CURLOPT_NOPROGRESS, 0L) ||
                !setopt(CURLOPT_XFERINFOFUNCTION, &progress) ||
                !setopt(CURLOPT_XFERINFODATA, operation.get()) ||
                !setopt(CURLOPT_SSL_VERIFYPEER, options.verify_tls ? 1L : 0L) ||
                !setopt(CURLOPT_SSL_VERIFYHOST, options.verify_tls ? 2L : 0L)) {
                response.error = "Moonraker HTTP curl option setup failed";
                remove_operation();
                set_error(response.error);
                return response;
            }
            if (!options.ca_file.empty() && !setopt(CURLOPT_CAINFO, options.ca_file.c_str())) {
                response.error = "Moonraker HTTP CA file setup failed";
                remove_operation();
                set_error(response.error);
                return response;
            }

            auto request_headers = request.headers;
            if (!options.api_key.empty() && !has_api_key_header(request_headers)) {
                request_headers.emplace("X-Api-Key", options.api_key);
            }
            for (const auto& header : request_headers) {
                const std::string value = header.first + ": " + header.second;
                curl_slist* appended = curl_slist_append(header_list.get(), value.c_str());
                if (!appended) {
                    response.error = "Moonraker HTTP header allocation failed";
                    remove_operation();
                    set_error(response.error);
                    return response;
                }
                header_list.release();
                header_list.reset(appended);
            }
            if (header_list && !setopt(CURLOPT_HTTPHEADER, header_list.get())) {
                response.error = "Moonraker HTTP header setup failed";
                remove_operation();
                set_error(response.error);
                return response;
            }

            const auto method = request.method.empty() ? std::string("GET") : request.method;
            if (!setopt(CURLOPT_CUSTOMREQUEST, method.c_str())) {
                response.error = "Moonraker HTTP method setup failed";
                remove_operation();
                set_error(response.error);
                return response;
            }

            if (!request.upload_source.empty()) {
                std::string path_error;
                const auto path = upload_path_from_uri(request.upload_source, path_error);
                std::error_code filesystem_error;
                if (!path_error.empty() || path.empty() || !std::filesystem::is_regular_file(path, filesystem_error)) {
                    response.error = path_error.empty() ? "Moonraker upload source is not a regular file" : path_error;
                    remove_operation();
                    set_error(response.error);
                    return response;
                }
                if (!request.upload_sha256.empty()) {
                    std::string actual;
                    if (!sha256_file(path, actual, response.error)) {
                        remove_operation();
                        set_error(response.error);
                        return response;
                    }
                    if (lowercase(actual) != lowercase(request.upload_sha256)) {
                        response.error = "Moonraker upload SHA-256 does not match the requested digest";
                        remove_operation();
                        set_error(response.error);
                        return response;
                    }
                }

                mime.reset(curl_mime_init(curl.get()));
                if (!mime) {
                    response.error = "Moonraker multipart form initialization failed";
                    remove_operation();
                    set_error(response.error);
                    return response;
                }
                for (const auto& field : request.form_fields) {
                    curl_mimepart* part = curl_mime_addpart(mime.get());
                    if (!part || curl_mime_name(part, field.first.c_str()) != CURLE_OK ||
                        curl_mime_data(part, field.second.c_str(), CURL_ZERO_TERMINATED) != CURLE_OK) {
                        response.error = "Moonraker multipart form field setup failed";
                        remove_operation();
                        set_error(response.error);
                        return response;
                    }
                }
                curl_mimepart* file_part = curl_mime_addpart(mime.get());
                const auto filename = request.upload_filename.empty() ? path.filename().u8string() : request.upload_filename;
                upload_stream.open(path,std::ios::binary);
                if(!upload_stream) throw std::runtime_error("Moonraker upload source cannot be read");
                const auto upload_size=std::filesystem::file_size(path);
                if (!file_part || curl_mime_name(file_part, "file") != CURLE_OK ||
                    curl_mime_data_cb(file_part,static_cast<curl_off_t>(upload_size),&read_upload,nullptr,nullptr,&upload_stream) != CURLE_OK ||
                    curl_mime_filename(file_part, filename.c_str()) != CURLE_OK ||
                    !setopt(CURLOPT_MIMEPOST, mime.get())) {
                    response.error = "Moonraker multipart file setup failed";
                    remove_operation();
                    set_error(response.error);
                    return response;
                }
            } else if (!request.form_fields.empty()) {
                mime.reset(curl_mime_init(curl.get()));
                if (!mime) {
                    response.error = "Moonraker multipart form initialization failed";
                    remove_operation();
                    set_error(response.error);
                    return response;
                }
                for (const auto& field : request.form_fields) {
                    curl_mimepart* part = curl_mime_addpart(mime.get());
                    if (!part || curl_mime_name(part, field.first.c_str()) != CURLE_OK ||
                        curl_mime_data(part, field.second.c_str(), CURL_ZERO_TERMINATED) != CURLE_OK) {
                        response.error = "Moonraker multipart form field setup failed";
                        remove_operation();
                        set_error(response.error);
                        return response;
                    }
                }
                if (!setopt(CURLOPT_MIMEPOST, mime.get())) {
                    response.error = "Moonraker multipart form setup failed";
                    remove_operation();
                    set_error(response.error);
                    return response;
                }
            } else if (!request.body.empty()) {
                if (!setopt(CURLOPT_POSTFIELDS, request.body.c_str()) ||
                    !setopt(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()))) {
                    response.error = "Moonraker HTTP body setup failed";
                    remove_operation();
                    set_error(response.error);
                    return response;
                }
            }
            if (method == "POST" && request.body.empty() && request.form_fields.empty() && request.upload_source.empty() &&
                !setopt(CURLOPT_POST, 1L)) {
                response.error = "Moonraker HTTP POST setup failed";
                remove_operation();
                set_error(response.error);
                return response;
            }

            const CURLcode result = curl_easy_perform(curl.get());
            if (result != CURLE_OK) {
                response.status_code = 0;
                if (result == CURLE_ABORTED_BY_CALLBACK && operation->cancelled.load()) {
                    response.error = "Moonraker HTTP request cancelled";
                } else {
                    response.error = std::string("Moonraker HTTP request failed: ") + curl_easy_strerror(result);
                }
            } else {
                long status = 0;
                curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
                response.status_code = static_cast<int>(status);
            }
        } catch (const std::exception& exception) {
            response.status_code = 0;
            response.error = std::string("Moonraker HTTP transport exception: ") + exception.what();
        }
        remove_operation();
        set_error(response.error);
        return response;
    }
};

MoonrakerHttpTransport::MoonrakerHttpTransport(MoonrakerTransportOptions options)
    : m_impl(std::make_shared<Impl>(std::move(options)))
{}

MoonrakerHttpTransport::~MoonrakerHttpTransport()
{
    cancel();
}

MoonrakerHttpResponse MoonrakerHttpTransport::perform(const MoonrakerHttpRequest& request)
{
    return m_impl->perform(request);
}

void MoonrakerHttpTransport::cancel()
{
    if (m_impl) m_impl->cancel();
}

std::string MoonrakerHttpTransport::last_error() const
{
    return m_impl ? m_impl->last_error() : std::string{};
}


struct MoonrakerWebSocketTransport::Impl : std::enable_shared_from_this<MoonrakerWebSocketTransport::Impl> {
    struct Connection {
        net::io_context io;
        net::executor_work_guard<net::io_context::executor_type> work{net::make_work_guard(io)};
        std::shared_ptr<WebSocketSession> session;
        std::atomic_bool stopping{false},connected{false},opened{false};
        std::promise<bool> open_result;
        std::shared_future<bool> ready{open_result.get_future().share()};
        std::promise<void> done_result;
        std::shared_future<void> done{done_result.get_future().share()};
        std::thread::id thread_id;
        std::mutex posting;
        void complete_open(bool ok) { if(!opened.exchange(true)) open_result.set_value(ok); }
    };
    explicit Impl(MoonrakerTransportOptions value):options(std::move(value)) {
        options.connect_timeout=std::max(options.connect_timeout,std::chrono::milliseconds(1));
        options.total_timeout=std::max(options.total_timeout,std::chrono::milliseconds(1));
    }
    MoonrakerTransportOptions options;
    mutable std::mutex mutex;
    std::shared_ptr<Connection> active;
    std::string error;
    void set_error(std::string value) { std::lock_guard<std::mutex> lock(mutex);error=std::move(value); }
    std::string last_error() const { std::lock_guard<std::mutex> lock(mutex);return error; }
    static void stop_connection(const std::shared_ptr<Connection>& connection) {
        if(!connection) return;
        {
            std::lock_guard<std::mutex> posting_lock(connection->posting);
            if(!connection->stopping.exchange(true)) {
                connection->connected.store(false);connection->complete_open(false);
                net::post(connection->io,[connection] {
                    connection->session->stop();
                    connection->work.reset();
                });
            }
        }
        if(connection->thread_id!=std::this_thread::get_id()) connection->done.wait();
    }
    bool connect(const std::string& url,const std::map<std::string,std::string>& headers,
                 MessageHandler on_message,CloseHandler on_close) {
        ParsedWebSocketUrl parsed;std::string parse_error;
        if(!parse_websocket_url(url,parsed,parse_error)) { set_error(parse_error);return false; }
        auto connection=std::make_shared<Connection>();
        connection->session=parsed.secure
            ?std::static_pointer_cast<WebSocketSession>(std::make_shared<AsyncWebSocketSession<true>>(connection->io,options))
            :std::static_pointer_cast<WebSocketSession>(std::make_shared<AsyncWebSocketSession<false>>(connection->io,options));
        std::thread worker([connection] { connection->io.run();connection->done_result.set_value(); });
        connection->thread_id=worker.get_id();worker.detach();
        std::shared_ptr<Connection> old;
        { std::lock_guard<std::mutex> lock(mutex);old=std::exchange(active,connection);error.clear(); }
        stop_connection(old);
        if(connection->stopping.load()) return false;
        auto effective_headers=headers;
        if(!options.api_key.empty()&&!has_api_key_header(effective_headers)) effective_headers.emplace("X-Api-Key",options.api_key);
        std::weak_ptr<Impl> weak_self=shared_from_this();std::weak_ptr<Connection> weak_connection=connection;
        {
        std::lock_guard<std::mutex> posting_lock(connection->posting);
        if(connection->stopping.load()) return false;
        net::post(connection->io,[weak_self,weak_connection,parsed,effective_headers,on_message=std::move(on_message),on_close=std::move(on_close)]() mutable {
            auto current=weak_connection.lock();if(!current||current->stopping.load()) return;
            current->session->open(parsed,effective_headers,
                [weak_self,weak_connection](bool ok,std::string message) {
                    auto current=weak_connection.lock();if(!current) return;
                    auto owner=weak_self.lock();
                    if(owner) { std::lock_guard<std::mutex> lock(owner->mutex);
                        if(owner->active==current) { owner->error=message;current->connected.store(ok); }
                    }
                    current->complete_open(ok);
                },
                [weak_connection,on_message=std::move(on_message)](std::string payload) {
                    auto current=weak_connection.lock();
                    if(current&&!current->stopping.load()&&on_message) on_message(payload);
                },
                [weak_self,weak_connection,on_close=std::move(on_close)](std::string message) {
                    auto current=weak_connection.lock();auto owner=weak_self.lock();if(!current||!owner) return;
                    bool notify=false;
                    { std::lock_guard<std::mutex> lock(owner->mutex);current->connected.store(false);
                        if(owner->active==current) { owner->error=std::move(message);notify=!current->stopping.load(); }
                    }
                    // Keep the disconnected connection until close/reconnect drains its io_context.
                    if(notify&&on_close) { try { on_close(); } catch(...) {} }
                });
        });
        }
        const bool ready=connection->ready.wait_for(options.total_timeout)==std::future_status::ready;
        if(!ready) { stop_connection(connection);return false; }
        bool selected;
        { std::lock_guard<std::mutex> lock(mutex);selected=active==connection; }
        if(!selected || !connection->ready.get()) { stop_connection(connection);return false; }
        return true;
    }
    bool send(const std::string& payload) {
        std::shared_ptr<Connection> connection;
        { std::lock_guard<std::mutex> lock(mutex);connection=active; }
        if(!connection||connection->stopping.load()||!connection->connected.load()) return false;
        auto promise=std::make_shared<std::promise<bool>>();auto result=promise->get_future();
        {
            std::lock_guard<std::mutex> posting_lock(connection->posting);
            if(connection->stopping.load()||!connection->connected.load()) return false;
            std::weak_ptr<Connection> weak_connection=connection;
            net::post(connection->io,[weak_connection,payload,promise] {
                auto current=weak_connection.lock();
                if(!current||current->stopping.load()) { promise->set_value(false);return; }
                current->session->send(payload,[promise](bool ok) { promise->set_value(ok); });
            });
        }
        // A message callback executes on this connection's serial io thread.
        // Queueing is its send acknowledgement; blocking here would deadlock IO.
        if(connection->thread_id==std::this_thread::get_id()) return true;
        if(result.wait_for(options.total_timeout)!=std::future_status::ready) { stop_connection(connection);return false; }
        return result.get();
    }
    void close() {
        std::shared_ptr<Connection> connection;
        { std::lock_guard<std::mutex> lock(mutex);connection=std::exchange(active,{}); }
        stop_connection(connection);
    }
};

MoonrakerWebSocketTransport::MoonrakerWebSocketTransport(MoonrakerTransportOptions options)
    : m_impl(std::make_shared<Impl>(std::move(options)))
{}

MoonrakerWebSocketTransport::~MoonrakerWebSocketTransport()
{
    close();
}

bool MoonrakerWebSocketTransport::connect(const std::string& url,
                                          const std::map<std::string, std::string>& headers,
                                          MessageHandler on_message,
                                          CloseHandler on_close)
{
    return m_impl->connect(url, headers, std::move(on_message), std::move(on_close));
}

bool MoonrakerWebSocketTransport::send(const std::string& payload)
{
    return m_impl->send(payload);
}

void MoonrakerWebSocketTransport::close()
{
    if (m_impl) m_impl->close();
}

std::string MoonrakerWebSocketTransport::last_error() const
{
    return m_impl ? m_impl->last_error() : std::string{};
}

} // namespace Orca::MobileDevice
