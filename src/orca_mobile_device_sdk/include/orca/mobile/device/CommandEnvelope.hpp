#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace Orca::MobileDevice {

inline constexpr const char* kCommandProtocolVersion = "orca.command/v1";

enum class CommandValidationError : uint8_t {
    None = 0,
    MissingProtocolVersion,
    UnsupportedProtocolVersion,
    MissingSessionId,
    MissingDeviceId,
    MissingCommandId,
    InvalidSequence,
    MissingType,
    Expired,
};

struct CommandEnvelope {
    std::string protocol_version{kCommandProtocolVersion};
    std::string session_id;
    std::string device_id;
    std::string command_id;
    uint64_t sequence{0};
    int64_t expires_at_epoch_seconds{0};
    std::string type;
    nlohmann::json payload{nlohmann::json::object()};

    CommandValidationError validate(
        int64_t now_epoch_seconds = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count(),
        std::string* detail = nullptr) const;

    bool is_retry_of(const CommandEnvelope& other) const;
    nlohmann::json to_json() const;
    static std::optional<CommandEnvelope> from_json(const nlohmann::json& json,
                                                    std::string* detail = nullptr);
};

const char* command_validation_error_name(CommandValidationError error);

} // namespace Orca::MobileDevice
