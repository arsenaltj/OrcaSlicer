#include "orca/mobile/device/CommandEnvelope.hpp"

#include <limits>

namespace Orca::MobileDevice {

namespace {

CommandValidationError fail(CommandValidationError error, const char* message, std::string* detail)
{
    if (detail != nullptr)
        *detail = message;
    return error;
}

} // namespace

CommandValidationError CommandEnvelope::validate(int64_t now_epoch_seconds, std::string* detail) const
{
    if (protocol_version.empty())
        return fail(CommandValidationError::MissingProtocolVersion, "protocol_version is required", detail);
    if (protocol_version != kCommandProtocolVersion)
        return fail(CommandValidationError::UnsupportedProtocolVersion, "unsupported command protocol", detail);
    if (session_id.empty())
        return fail(CommandValidationError::MissingSessionId, "session_id is required", detail);
    if (device_id.empty())
        return fail(CommandValidationError::MissingDeviceId, "device_id is required", detail);
    if (command_id.empty())
        return fail(CommandValidationError::MissingCommandId, "command_id is required", detail);
    if (sequence == 0)
        return fail(CommandValidationError::InvalidSequence, "sequence must be positive", detail);
    if (type.empty())
        return fail(CommandValidationError::MissingType, "type is required", detail);
    if (expires_at_epoch_seconds <= now_epoch_seconds)
        return fail(CommandValidationError::Expired, "command has expired", detail);
    if (detail != nullptr)
        detail->clear();
    return CommandValidationError::None;
}

bool CommandEnvelope::is_retry_of(const CommandEnvelope& other) const
{
    return command_id == other.command_id && session_id == other.session_id &&
           device_id == other.device_id && type == other.type && payload == other.payload;
}

nlohmann::json CommandEnvelope::to_json() const
{
    return {
        {"protocol_version", protocol_version},
        {"session_id", session_id},
        {"device_id", device_id},
        {"command_id", command_id},
        {"sequence", sequence},
        {"expires_at", expires_at_epoch_seconds},
        {"type", type},
        {"payload", payload},
    };
}

std::optional<CommandEnvelope> CommandEnvelope::from_json(const nlohmann::json& json, std::string* detail)
{
    if (!json.is_object()) {
        fail(CommandValidationError::MissingProtocolVersion, "command envelope must be an object", detail);
        return std::nullopt;
    }

    CommandEnvelope envelope;
    envelope.protocol_version = json.value("protocol_version", std::string());
    envelope.session_id = json.value("session_id", std::string());
    envelope.device_id = json.value("device_id", std::string());
    envelope.command_id = json.value("command_id", std::string());
    envelope.sequence = json.value("sequence", uint64_t{0});
    envelope.expires_at_epoch_seconds = json.value("expires_at", int64_t{0});
    envelope.type = json.value("type", std::string());
    envelope.payload = json.value("payload", nlohmann::json::object());

    if (envelope.validate(std::numeric_limits<int64_t>::min(), detail) != CommandValidationError::None)
        return std::nullopt;
    return envelope;
}

const char* command_validation_error_name(CommandValidationError error)
{
    switch (error) {
    case CommandValidationError::None: return "none";
    case CommandValidationError::MissingProtocolVersion: return "missing_protocol_version";
    case CommandValidationError::UnsupportedProtocolVersion: return "unsupported_protocol_version";
    case CommandValidationError::MissingSessionId: return "missing_session_id";
    case CommandValidationError::MissingDeviceId: return "missing_device_id";
    case CommandValidationError::MissingCommandId: return "missing_command_id";
    case CommandValidationError::InvalidSequence: return "invalid_sequence";
    case CommandValidationError::MissingType: return "missing_type";
    case CommandValidationError::Expired: return "expired";
    }
    return "unknown";
}

} // namespace Orca::MobileDevice
