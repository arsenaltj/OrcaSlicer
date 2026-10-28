#pragma once

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>

namespace Slic3r::HonorAccount {

struct Config {
    // Public integration values; the shared secret is provisioned separately
    // into the OS store for this internal trial, never compiled into the app.
    std::string environment {"test"};
    std::string client_id {"114909105"};
    std::string client_secret;
    std::string redirect_uri {"honorid://redirect_uri"};
    std::string authorize_url {"https://hnoauth-login-test-drcn.cloud.honor.com/oauth2/v3/authorize"};
    std::string token_url {"https://hnoauth-login-test-drcn.cloud.honor.com/oauth2/v3/token"};
    std::string jwks_url {"https://hnoauth-login-test-drcn.cloud.honor.com/oauth2/v3/certs"};
    // Documented identity claim, not a network endpoint. Keep strict verification
    // until an actual test ID token establishes whether the test issuer differs.
    std::string issuer {"https://honorid.hihonor.com"};
    // Kept for test provisioning; the public OAuth protocol does not define them.
    std::string login_channel {"99013101"};
    std::string req_client_type {"131"};
    std::string validate() const;
    std::string key() const;
};

struct Identity {
    std::string subject; // ID-token sub is UnionId, not user-info openID.
    std::string name;
    std::string picture;
    std::int64_t expires_at {0};
};

struct Credentials {
    std::string access_token;
    std::string refresh_token;
    std::string id_token;
    std::int64_t expires_at {0};
    Identity identity;
};

enum class Failure { Temporary, InvalidCredentials, Configuration, Protocol, Cancelled };
class Error : public std::runtime_error {
public:
    Error(Failure kind, const std::string& safe_message) : std::runtime_error(safe_message), kind(kind) {}
    Failure kind;
};

std::int64_t now_seconds();
std::string random_value();
std::string digest(const std::string& value);
std::string encode(const std::string& value);
std::string authorization_url(const Config&, const std::string& state, const std::string& nonce);
bool is_callback(const std::string& url, const std::string& redirect);
// Throws only safe messages; callback URLs/codes are never included in errors.
std::string authorization_code(const std::string& url, const std::string& redirect, const std::string& state);
Identity verify_id_token(const std::string& jwt, const std::string& jwks, const Config&,
                         const std::string& nonce, std::int64_t now);
Failure oauth_failure(unsigned http_status, const std::string& body, bool refreshing);
std::string serialize_config(const Config&);
Config parse_config(const std::string&);
std::string serialize_credentials(const Credentials&);
Credentials parse_credentials(const std::string&);

} // namespace Slic3r::HonorAccount
