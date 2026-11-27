#pragma once

#include <regex>
#include <string>

namespace Slic3r {

// Keep relative asset diagnostics while removing the absolute profile and user
// identity. Match serialized Windows separators and paths concatenated directly
// to a diagnostic label too, without corrupting JSON or relative Unix paths.
inline std::string redact_log_user_paths(const std::string& message)
{
    static const std::regex profile(
        R"((?:[a-z]:[\\/]+(?:users|documents and settings)[\\/]+[^\\/\r\n"'<>]+|(^|[^A-Za-z0-9_])/(?:home|users)/[^/\r\n"'<>]+))",
        std::regex::icase);
    return std::regex_replace(message, profile, "$1<user-home>");
}

} // namespace Slic3r
