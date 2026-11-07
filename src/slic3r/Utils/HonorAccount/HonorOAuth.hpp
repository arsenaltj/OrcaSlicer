#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace Slic3r::HonorAccount {

struct Config {
    std::string api_url;
    std::string callback_uri {"honorforge://auth/callback"};
    // Preserves the dedicated WebView profile when migrating from direct HONOR OAuth.
    std::string browser_profile_key;

    std::string validate() const;
    std::string key() const;
    std::string browser_key() const;
};

struct Identity {
    std::string subject;
    std::optional<std::string> email;
    std::string name;
    std::optional<std::string> picture;
    bool display_name_customized {false};
    bool avatar_customized {false};
    std::string status;
    std::vector<std::string> roles;
};

struct PlatformSession {
    std::string session_id;
    std::string access_token;
    std::string refresh_token;
    std::int64_t access_expires_at {0};
    std::int64_t refresh_expires_at {0};
    Identity identity;
};

struct Authorization {
    std::string url;
    std::int64_t expires_at {0};
};

enum class Failure { Temporary, InvalidCredentials, Configuration, Protocol, Cancelled };
class Error : public std::runtime_error {
public:
    Error(Failure kind, const std::string& safe_message) : std::runtime_error(safe_message), kind(kind) {}
    Failure kind;
};

std::int64_t now_seconds();
std::int64_t parse_timestamp(const std::string& value);
std::string random_value();
std::string pkce_verifier();
std::string pkce_challenge(const std::string& verifier);
std::string digest(const std::string& value);
std::string encode(const std::string& value);

bool is_callback(const std::string& url, const std::string& callback);
// Throws only safe messages; callback URLs and codes are never included in errors.
std::string callback_code(const std::string& url, const std::string& callback);
bool valid_authorization_url(const std::string& url);

enum class AuthorizationPage {
    Blank,
    CasPortal,
    OAuthLogin,
    OAuthAuthorize,
    Callback,
    OtherHttps,
    Blocked
};
AuthorizationPage classify_authorization_page(const std::string& url,
                                                const std::string& callback);
const char* authorization_page_name(AuthorizationPage page) noexcept;
inline bool authorization_submit_allowed(AuthorizationPage page) noexcept
{
    return page == AuthorizationPage::CasPortal || page == AuthorizationPage::OAuthLogin ||
           page == AuthorizationPage::OAuthAuthorize;
}

std::string serialize_config(const Config&);
Config parse_config(const std::string&);
// Returns only the old profile fingerprint. No secret or token is retained.
std::optional<std::string> legacy_profile_key(const std::string&);

std::string serialize_session(const PlatformSession&);
PlatformSession parse_session(const std::string&);
std::string serialize_refresh_tombstone();
bool is_refresh_tombstone(const std::string&);

PlatformSession parse_session_response(const std::string&);
Identity parse_account_response(const std::string&);

} // namespace Slic3r::HonorAccount
