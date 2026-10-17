#include "orca/connectivity/moonraker/MoonrakerTransport.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
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
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>

#include <utility>
#include <vector>

namespace Orca::SDK {
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
        std::filesystem::path path(source);
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
    return std::filesystem::path(rest);
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

class WebSocketSession {
public:
    virtual ~WebSocketSession() = default;
    virtual bool open(const ParsedWebSocketUrl& url,
                      const std::map<std::string, std::string>& headers,
                      const MoonrakerTransportOptions& options,
                      std::string& error) = 0;
    virtual bool write(const std::string& payload,
                       std::chrono::milliseconds timeout,
                       std::string& error) = 0;
    virtual bool read(std::string& payload,
                      std::chrono::milliseconds timeout,
                      error_code& ec) = 0;
    virtual void cancel() = 0;
    virtual void close() = 0;
};

class PlainWebSocketSession final : public WebSocketSession {
public:
    explicit PlainWebSocketSession(net::io_context& io) : resolver(io), socket(io) {}

    bool open(const ParsedWebSocketUrl& url,
              const std::map<std::string, std::string>& headers,
              const MoonrakerTransportOptions& options,
              std::string& error) override
    {
        error_code ec;
        const auto results = resolver.resolve(url.host, url.port, ec);
        if (ec) {
            error = "Moonraker WebSocket resolve failed: " + ec.message();
            return false;
        }
        beast::get_lowest_layer(socket).expires_after(options.connect_timeout);
        beast::get_lowest_layer(socket).connect(results, ec);
        if (ec) {
            error = "Moonraker WebSocket connect failed: " + ec.message();
            return false;
        }
        decorate_websocket(socket, headers);
        beast::get_lowest_layer(socket).expires_after(options.total_timeout);
        socket.handshake(url.authority, url.target, ec);
        if (ec) {
            error = "Moonraker WebSocket handshake failed: " + ec.message();
            return false;
        }
        return true;
    }

    bool write(const std::string& payload,
               std::chrono::milliseconds timeout,
               std::string& error) override
    {
        error_code ec;
        beast::get_lowest_layer(socket).expires_after(timeout);
        socket.write(net::buffer(payload), ec);
        if (ec) {
            error = "Moonraker WebSocket send failed: " + ec.message();
            return false;
        }
        return true;
    }

    bool read(std::string& payload,
              std::chrono::milliseconds timeout,
              error_code& ec) override
    {
        beast::flat_buffer buffer;
        beast::get_lowest_layer(socket).expires_after(timeout);
        socket.read(buffer, ec);
        if (!ec) payload = beast::buffers_to_string(buffer.data());
        return !ec;
    }

    void cancel() override
    {
        error_code ignored;
        beast::get_lowest_layer(socket).socket().cancel(ignored);
        beast::get_lowest_layer(socket).socket().close(ignored);
    }

    void close() override
    {
        error_code ignored;
        socket.close(websocket::close_code::normal, ignored);
    }

private:
    tcp::resolver resolver;
    websocket::stream<beast::tcp_stream> socket;
};

class TlsWebSocketSession final : public WebSocketSession {
public:
    explicit TlsWebSocketSession(net::io_context& io)
        : resolver(io)
        , context(net::ssl::context::tls_client)
        , socket(io, context)
    {}

    bool open(const ParsedWebSocketUrl& url,
              const std::map<std::string, std::string>& headers,
              const MoonrakerTransportOptions& options,
              std::string& error) override
    {
        error_code ec;
        if (options.verify_tls) {
            context.set_verify_mode(net::ssl::verify_peer);
            context.set_default_verify_paths(ec);
            if (ec) {
                error = "Moonraker TLS certificate store setup failed: " + ec.message();
                return false;
            }
            socket.next_layer().set_verify_callback(net::ssl::host_name_verification(url.host));
        } else {
            context.set_verify_mode(net::ssl::verify_none);
        }
        if (!options.ca_file.empty()) {
            context.load_verify_file(options.ca_file, ec);
            if (ec) {
                error = "Moonraker TLS CA file setup failed: " + ec.message();
                return false;
            }
        }
        if (SSL_set_tlsext_host_name(socket.next_layer().native_handle(), url.host.c_str()) != 1) {
            error = "Moonraker TLS SNI setup failed";
            return false;
        }

        const auto results = resolver.resolve(url.host, url.port, ec);
        if (ec) {
            error = "Moonraker WebSocket resolve failed: " + ec.message();
            return false;
        }
        beast::get_lowest_layer(socket).expires_after(options.connect_timeout);
        beast::get_lowest_layer(socket).connect(results, ec);
        if (ec) {
            error = "Moonraker WebSocket connect failed: " + ec.message();
            return false;
        }
        beast::get_lowest_layer(socket).expires_after(options.total_timeout);
        socket.next_layer().handshake(net::ssl::stream_base::client, ec);
        if (ec) {
            error = "Moonraker TLS handshake failed: " + ec.message();
            return false;
        }
        decorate_websocket(socket, headers);
        socket.handshake(url.authority, url.target, ec);
        if (ec) {
            error = "Moonraker WebSocket handshake failed: " + ec.message();
            return false;
        }
        return true;
    }

    bool write(const std::string& payload,
               std::chrono::milliseconds timeout,
               std::string& error) override
    {
        error_code ec;
        beast::get_lowest_layer(socket).expires_after(timeout);
        socket.write(net::buffer(payload), ec);
        if (ec) {
            error = "Moonraker WebSocket send failed: " + ec.message();
            return false;
        }
        return true;
    }

    bool read(std::string& payload,
              std::chrono::milliseconds timeout,
              error_code& ec) override
    {
        beast::flat_buffer buffer;
        beast::get_lowest_layer(socket).expires_after(timeout);
        socket.read(buffer, ec);
        if (!ec) payload = beast::buffers_to_string(buffer.data());
        return !ec;
    }

    void cancel() override
    {
        error_code ignored;
        beast::get_lowest_layer(socket).socket().cancel(ignored);
        beast::get_lowest_layer(socket).socket().close(ignored);
    }

    void close() override
    {
        error_code ignored;
        socket.close(websocket::close_code::normal, ignored);
    }

private:
    tcp::resolver resolver;
    net::ssl::context context;
    websocket::stream<beast::ssl_stream<beast::tcp_stream>> socket;
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
        output->append(data, size * count);
        return size * count;
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
                const auto filename = request.upload_filename.empty() ? path.filename().string() : request.upload_filename;
                if (!file_part || curl_mime_name(file_part, "file") != CURLE_OK ||
                    curl_mime_filedata(file_part, path.string().c_str()) != CURLE_OK ||
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
    explicit Impl(MoonrakerTransportOptions value) : options(std::move(value)) {}

    MoonrakerTransportOptions options;
    net::io_context io;
    mutable std::mutex mutex;
    std::mutex write_mutex;
    std::shared_ptr<WebSocketSession> session;
    std::thread reader;
    IMoonrakerWebSocketTransport::MessageHandler on_message;
    IMoonrakerWebSocketTransport::CloseHandler on_close;
    bool connected{false};
    bool stopping{false};
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

    bool connect(const std::string& url,
                 const std::map<std::string, std::string>& headers,
                 IMoonrakerWebSocketTransport::MessageHandler message_handler,
                 IMoonrakerWebSocketTransport::CloseHandler close_handler)
    {
        close();
        ParsedWebSocketUrl parsed;
        std::string open_error;
        if (!parse_websocket_url(url, parsed, open_error)) {
            set_error(open_error);
            return false;
        }

        auto effective_headers = headers;
        if (!options.api_key.empty() && !has_api_key_header(effective_headers)) {
            effective_headers.emplace("X-Api-Key", options.api_key);
        }

        std::shared_ptr<WebSocketSession> new_session = parsed.secure
            ? std::static_pointer_cast<WebSocketSession>(std::make_shared<TlsWebSocketSession>(io))
            : std::static_pointer_cast<WebSocketSession>(std::make_shared<PlainWebSocketSession>(io));
        if (!new_session->open(parsed, effective_headers, options, open_error)) {
            set_error(open_error);
            new_session->close();
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            session = std::move(new_session);
            on_message = std::move(message_handler);
            on_close = std::move(close_handler);
            connected = true;
            stopping = false;
            error.clear();
        }
        const auto self = shared_from_this();
        reader = std::thread([self]() { self->read_loop(); });
        return true;
    }

    bool send(const std::string& payload)
    {
        std::shared_ptr<WebSocketSession> current;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!connected || !session) return false;
            current = session;
        }
        std::lock_guard<std::mutex> write_lock(write_mutex);
        std::string send_error;
        if (!current->write(payload, options.total_timeout, send_error)) {
            set_error(std::move(send_error));
            return false;
        }
        return true;
    }

    void close()
    {
        std::shared_ptr<WebSocketSession> current;
        bool called_from_reader = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            current = session;
            stopping = true;
            connected = false;
            called_from_reader = reader.joinable() && reader.get_id() == std::this_thread::get_id();
        }
        {
            std::lock_guard<std::mutex> write_lock(write_mutex);
            if (current) current->cancel();
        }
        if (reader.joinable()) {
            if (called_from_reader) {
                reader.detach();
            } else {
                reader.join();
            }
        }
        if (!called_from_reader) {
            std::lock_guard<std::mutex> write_lock(write_mutex);
            if (current) current->close();
            std::lock_guard<std::mutex> lock(mutex);
            if (session == current) session.reset();
            on_message = {};
            on_close = {};
        }
    }

    void read_loop()
    {
        std::shared_ptr<WebSocketSession> current;
        {
            std::lock_guard<std::mutex> lock(mutex);
            current = session;
        }
        if (!current) return;

        error_code ec;
        while (true) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (stopping || !connected) break;
            }
            std::string payload;
            if (!current->read(payload, options.total_timeout, ec)) break;
            IMoonrakerWebSocketTransport::MessageHandler handler;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (stopping || !connected) break;
                handler = on_message;
            }
            if (handler) handler(payload);
        }

        IMoonrakerWebSocketTransport::CloseHandler close_handler;
        bool notify = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            notify = connected && !stopping;
            connected = false;
            if (ec) error = "Moonraker WebSocket disconnected: " + ec.message();
            close_handler = on_close;
            if (session == current) session.reset();
        }
        if (notify && close_handler) close_handler();
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

} // namespace Orca::SDK
