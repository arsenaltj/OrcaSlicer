#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace Slic3r::GUI {

// A failed status GET says nothing about whether an already submitted job ran.
inline bool model_status_http_retryable(unsigned status)
{
    return status == 408 || status == 429 || (status >= 500 && status <= 599);
}

inline std::string model_generation_http_error(const std::string& body,
                                              const std::string& transport_error,
                                              unsigned status, bool status_query = false)
{
    if (status == 401)
        return "A valid OrcaSlicer AI session is required.";
    if (!transport_error.empty()) {
        if (transport_error.find("connect") != std::string::npos ||
            transport_error.find("Connection") != std::string::npos)
            return "AI sidecar is not reachable.";
        if (transport_error.find("timed out") != std::string::npos ||
            transport_error.find("Timeout") != std::string::npos)
            return "AI sidecar request timed out.";
        return "AI sidecar request failed.";
    }
    if (status_query && model_status_http_retryable(status))
        return "AI sidecar request failed with HTTP " + std::to_string(status) + ".";

    auto parsed = nlohmann::json::parse(body, nullptr, false);
    if (!parsed.is_discarded()) {
        if (parsed.contains("error") && parsed["error"].is_object())
            return parsed["error"].value("message", "Model generation request failed.");
        if (parsed.contains("error") && parsed["error"].is_string())
            return parsed["error"].get<std::string>();
    }
    return "Model generation request failed with HTTP " + std::to_string(status) + ".";
}

inline bool model_submission_http_error_ambiguous(const std::string& error)
{
    const std::string prefix = "Model generation request failed with HTTP ";
    if (error.size() != prefix.size() + 4 || error.compare(0, prefix.size(), prefix) != 0 ||
        error.back() != '.')
        return false;
    const auto offset = prefix.size();
    unsigned status = 0;
    for (size_t i = offset; i < offset + 3; ++i) {
        if (error[i] < '0' || error[i] > '9') return false;
        status = status * 10 + unsigned(error[i] - '0');
    }
    return status == 401 || model_status_http_retryable(status);
}

} // namespace Slic3r::GUI
