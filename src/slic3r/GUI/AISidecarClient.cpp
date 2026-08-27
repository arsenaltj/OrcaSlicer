#include "AISidecarClient.hpp"

#include "slic3r/Utils/Http.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <mutex>
#include <regex>
#include <sstream>
#include <utility>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

namespace Slic3r::GUI {

bool AISidecarClient::is_loopback_endpoint(const std::string& endpoint)
{
    static const std::regex pattern(R"(^https?://(\[[^\]]+\]|[^/:?#]+)(?::[0-9]+)?(?:[/?#]|$))", std::regex::icase);
    std::smatch match;
    if (!std::regex_search(endpoint, match, pattern))
        return false;
    std::string host = match[1].str();
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return host == "localhost" || host == "127.0.0.1" || host == "[::1]";
}

namespace {

constexpr const char* SESSION_TOKEN_ENVIRONMENT = "ORCASLICER_AI_SESSION_TOKEN";

struct LocalSession
{
    std::mutex  mutex;
    std::string token;
    std::string server_nonce;
};

LocalSession& local_session()
{
    static LocalSession session;
    return session;
}

bool valid_hex_capability(const std::string& value)
{
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isxdigit(ch) != 0;
    });
}

std::string bytes_to_hex(const unsigned char* bytes, size_t size)
{
    std::ostringstream encoded;
    encoded << std::hex << std::setfill('0');
    for (size_t index = 0; index < size; ++index)
        encoded << std::setw(2) << static_cast<unsigned>(bytes[index]);
    return encoded.str();
}

std::string random_capability()
{
    std::array<unsigned char, 32> bytes {};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
        return {};
    return bytes_to_hex(bytes.data(), bytes.size());
}

std::string environment_session_token()
{
    const char* value = std::getenv(SESSION_TOKEN_ENVIRONMENT);
    const std::string token = value == nullptr ? std::string() : std::string(value);
    return valid_hex_capability(token) ? token : std::string();
}

void clear_environment_session_token()
{
#ifdef _WIN32
    _putenv_s(SESSION_TOKEN_ENVIRONMENT, "");
#else
    unsetenv(SESSION_TOKEN_ENVIRONMENT);
#endif
}

std::string active_session_token()
{
    auto& session = local_session();
    std::lock_guard<std::mutex> lock(session.mutex);
    return !session.token.empty() ? session.token : environment_session_token();
}

std::string hmac_sha256_hex(const std::string& key, const std::string& message)
{
    unsigned int result_size = 0;
    std::array<unsigned char, EVP_MAX_MD_SIZE> result {};
    if (HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
             reinterpret_cast<const unsigned char*>(message.data()), message.size(),
             result.data(), &result_size) == nullptr || result_size != 32)
        return {};
    return bytes_to_hex(result.data(), result_size);
}

bool constant_time_equal(const std::string& left, const std::string& right)
{
    return left.size() == right.size() && !left.empty() &&
           CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
}

std::string proposal_url(const std::string& endpoint)
{
    if (!endpoint.empty() && endpoint.back() == '/')
        return endpoint.substr(0, endpoint.size() - 1) + "/v1/orcaslicer/config-proposal";
    return endpoint + "/v1/orcaslicer/config-proposal";
}

} // namespace

bool AISidecarClient::initialize_local_session()
{
    auto& session = local_session();
    std::lock_guard<std::mutex> lock(session.mutex);
    if (!session.token.empty())
        return true;
    session.token = random_capability();
    session.server_nonce.clear();
    // Never let a capability inherited from the shell flow into unrelated Orca
    // child processes. The generated token remains in memory and is copied only
    // into the dedicated Sidecar child environment.
    clear_environment_session_token();
    return valid_hex_capability(session.token);
}

bool AISidecarClient::session_protection_enabled()
{
    return valid_hex_capability(active_session_token());
}

std::string AISidecarClient::create_session_nonce()
{
    return random_capability();
}

bool AISidecarClient::accept_session_challenge(const std::string& client_nonce,
                                               const std::string& server_nonce,
                                               const std::string& server_proof)
{
    const std::string token = active_session_token();
    if (!valid_hex_capability(token) || !valid_hex_capability(client_nonce) ||
        !valid_hex_capability(server_nonce) || !valid_hex_capability(server_proof))
        return false;
    const std::string expected = hmac_sha256_hex(token, "server:" + client_nonce + ":" + server_nonce);
    if (!constant_time_equal(expected, server_proof))
        return false;
    auto& session = local_session();
    std::lock_guard<std::mutex> lock(session.mutex);
    session.server_nonce = server_nonce;
    return true;
}

std::string AISidecarClient::session_token_for_child()
{
    auto& session = local_session();
    std::lock_guard<std::mutex> lock(session.mutex);
    return session.token;
}

void AISidecarClient::configure_native_request(Http& request)
{
    request.local_only();
    request.header("X-OrcaSlicer-Client", "native");
    const std::string token = active_session_token();
    std::string server_nonce;
    {
        auto& session = local_session();
        std::lock_guard<std::mutex> lock(session.mutex);
        server_nonce = session.server_nonce;
    }
    if (!token.empty() && !server_nonce.empty()) {
        const std::string proof = hmac_sha256_hex(token, "client:" + server_nonce);
        if (!proof.empty())
            request.header("X-OrcaSlicer-Session-Proof", proof);
    }
}

AISidecarClient::AISidecarClient(std::string endpoint)
    : m_endpoint(std::move(endpoint))
{
}

AISidecarClient::~AISidecarClient()
{
    cancel_current();
}

std::string AISidecarClient::default_endpoint()
{
    if (const char* endpoint = std::getenv("ORCASLICER_AI_SIDECAR_URL"); endpoint != nullptr && endpoint[0] != '\0')
        return endpoint;
    return "http://127.0.0.1:18764";
}

void AISidecarClient::propose_config_changes(const json& request, CompleteFn on_complete, ErrorFn on_error)
{
    cancel_current();
    if (!is_loopback_endpoint(m_endpoint)) {
        if (on_error)
            on_error("AI configuration proposals require a loopback Sidecar endpoint.");
        return;
    }

    auto http = Http::post(proposal_url(m_endpoint));
    configure_native_request(http);
    http.header("Content-Type", "application/json")
        .timeout_connect(5)
        .timeout_max(120)
        .size_limit(5 * 1024 * 1024)
        .set_post_body(request.dump());

    if (!is_loopback_endpoint(m_endpoint))
        http.tls_verify(true);

    http.on_complete([on_complete = std::move(on_complete), on_error](std::string body, unsigned) mutable {
        auto parsed = json::parse(body, nullptr, false);
        if (parsed.is_discarded()) {
            if (on_error)
                on_error("AI sidecar returned invalid JSON.");
            return;
        }

        Response response;
        response.raw = parsed;
        response.request_id = parsed.value("request_id", std::string());
        response.assistant_text = parsed.value("assistant_text", std::string());
        if (parsed.contains("proposal"))
            response.proposal = parsed["proposal"];

        if (on_complete)
            on_complete(std::move(response));
    });

    http.on_error([on_error = std::move(on_error)](std::string body, std::string error, unsigned status) {
        if (!error.empty()) {
            if (on_error)
                on_error(error);
            return;
        }

        if (on_error)
            on_error("AI sidecar request failed with HTTP " + std::to_string(status) + (body.empty() ? std::string() : ": " + body));
    });

    m_active_request = http.perform();
}

void AISidecarClient::cancel_current()
{
    if (m_active_request) {
        m_active_request->cancel();
        m_active_request.reset();
    }
}

} // namespace Slic3r::GUI
