#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/AISidecarClient.hpp"

#include <array>
#include <cstdlib>
#include <iomanip>
#include <sstream>

#include <openssl/evp.h>
#include <openssl/hmac.h>

using Slic3r::GUI::AISidecarClient;

namespace {

std::string hmac_hex(const std::string& key, const std::string& message)
{
    unsigned int size = 0;
    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes {};
    REQUIRE(HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
                 reinterpret_cast<const unsigned char*>(message.data()), message.size(),
                 bytes.data(), &size) != nullptr);
    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (unsigned index = 0; index < size; ++index)
        result << std::setw(2) << static_cast<unsigned>(bytes[index]);
    return result.str();
}

} // namespace

TEST_CASE("AI Sidecar endpoints stay on loopback", "[AI][Sidecar]")
{
    REQUIRE(AISidecarClient::is_loopback_endpoint("http://127.0.0.1:18764"));
    REQUIRE(AISidecarClient::is_loopback_endpoint("http://localhost:18764/"));
    REQUIRE(AISidecarClient::is_loopback_endpoint("http://[::1]:18764"));
    REQUIRE_FALSE(AISidecarClient::is_loopback_endpoint("https://example.com/v1"));
    REQUIRE_FALSE(AISidecarClient::is_loopback_endpoint("not-a-url"));
}

TEST_CASE("AI Sidecar local session gets a process capability", "[AI][Sidecar][Security]")
{
    REQUIRE(AISidecarClient::initialize_local_session());
    REQUIRE(AISidecarClient::session_protection_enabled());
    const char* inherited = std::getenv("ORCASLICER_AI_SESSION_TOKEN");
    REQUIRE((inherited == nullptr || inherited[0] == '\0'));

    const std::string token = AISidecarClient::session_token_for_child();
    const std::string client_nonce = AISidecarClient::create_session_nonce();
    const std::string server_nonce(64, 'a');
    REQUIRE(token.size() == 64);
    REQUIRE(client_nonce.size() == 64);
    REQUIRE_FALSE(AISidecarClient::accept_session_challenge(client_nonce, server_nonce, std::string(64, '0')));
    REQUIRE(AISidecarClient::accept_session_challenge(
        client_nonce,
        server_nonce,
        hmac_hex(token, "server:" + client_nonce + ":" + server_nonce)));
}
