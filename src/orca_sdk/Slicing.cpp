#include "orca/sdk/Slicing.hpp"

#include <stdexcept>

namespace Orca::SDK {

namespace {

template <typename Enum>
Enum unsupported(const std::string& value, const char* name)
{
    throw std::invalid_argument(std::string("Unknown ") + name + ": " + value);
}

} // namespace

const char* slicing_input_format_name(SlicingInputFormat format)
{
    switch (format) {
    case SlicingInputFormat::Stl: return "stl";
    case SlicingInputFormat::Obj: return "obj";
    case SlicingInputFormat::ThreeMf: return "3mf";
    case SlicingInputFormat::Step: return "step";
    }
    return "unknown";
}

const char* slicing_execution_mode_name(SlicingExecutionMode mode)
{
    switch (mode) {
    case SlicingExecutionMode::Local: return "local";
    case SlicingExecutionMode::Remote: return "remote";
    }
    return "unknown";
}

const char* slicing_state_name(SlicingState state)
{
    switch (state) {
    case SlicingState::Ready: return "ready";
    case SlicingState::Validating: return "validating";
    case SlicingState::Slicing: return "slicing";
    case SlicingState::Completed: return "completed";
    case SlicingState::Failed: return "failed";
    case SlicingState::Canceled: return "canceled";
    }
    return "unknown";
}

void to_json(nlohmann::json& json, SlicingInputFormat value) { json = slicing_input_format_name(value); }
void to_json(nlohmann::json& json, SlicingExecutionMode value) { json = slicing_execution_mode_name(value); }
void to_json(nlohmann::json& json, SlicingState value) { json = slicing_state_name(value); }

void from_json(const nlohmann::json& json, SlicingInputFormat& value)
{
    const std::string name = json.get<std::string>();
    if (name == "stl") value = SlicingInputFormat::Stl;
    else if (name == "obj") value = SlicingInputFormat::Obj;
    else if (name == "3mf") value = SlicingInputFormat::ThreeMf;
    else if (name == "step") value = SlicingInputFormat::Step;
    else value = unsupported<SlicingInputFormat>(name, "slicing input format");
}

void from_json(const nlohmann::json& json, SlicingExecutionMode& value)
{
    const std::string name = json.get<std::string>();
    if (name == "local") value = SlicingExecutionMode::Local;
    else if (name == "remote") value = SlicingExecutionMode::Remote;
    else value = unsupported<SlicingExecutionMode>(name, "slicing execution mode");
}

void from_json(const nlohmann::json& json, SlicingState& value)
{
    const std::string name = json.get<std::string>();
    if (name == "ready") value = SlicingState::Ready;
    else if (name == "validating") value = SlicingState::Validating;
    else if (name == "slicing") value = SlicingState::Slicing;
    else if (name == "completed") value = SlicingState::Completed;
    else if (name == "failed") value = SlicingState::Failed;
    else if (name == "canceled") value = SlicingState::Canceled;
    else value = unsupported<SlicingState>(name, "slicing state");
}

void to_json(nlohmann::json& json, const SlicingRequest& value)
{
    json = {
        {"protocol_version", value.protocol_version},
        {"request_id", value.request_id},
        {"source_uri", value.source_uri},
        {"source_sha256", value.source_sha256},
        {"input_format", value.input_format},
        {"execution_mode", value.execution_mode},
        {"printer_profile_id", value.printer_profile_id},
        {"process_profile_id", value.process_profile_id},
        {"material_profile_id", value.material_profile_id},
        {"engine_version", value.engine_version},
        {"plate_index", value.plate_index},
    };
}

void from_json(const nlohmann::json& json, SlicingRequest& value)
{
    value.protocol_version = json.value("protocol_version", std::string("orca.slicing/v1"));
    value.request_id = json.value("request_id", std::string());
    value.source_uri = json.value("source_uri", std::string());
    value.source_sha256 = json.value("source_sha256", std::string());
    value.input_format = json.value("input_format", SlicingInputFormat::ThreeMf);
    value.execution_mode = json.value("execution_mode", SlicingExecutionMode::Local);
    value.printer_profile_id = json.value("printer_profile_id", std::string());
    value.process_profile_id = json.value("process_profile_id", std::string());
    value.material_profile_id = json.value("material_profile_id", std::string());
    value.engine_version = json.value("engine_version", std::string());
    value.plate_index = json.value("plate_index", uint32_t{0});
}

void to_json(nlohmann::json& json, const SlicingResult& value)
{
    json = {
        {"protocol_version", value.protocol_version},
        {"request_id", value.request_id},
        {"state", value.state},
        {"gcode_uri", value.gcode_uri},
        {"gcode_sha256", value.gcode_sha256},
        {"warnings", value.warnings},
        {"error_code", value.error_code},
        {"error_message", value.error_message},
        {"layer_count", value.layer_count},
        {"material_volume_mm3", value.material_volume_mm3},
        {"elapsed_ms", value.elapsed_ms},
    };
}

void from_json(const nlohmann::json& json, SlicingResult& value)
{
    value.protocol_version = json.value("protocol_version", std::string("orca.slicing/v1"));
    value.request_id = json.value("request_id", std::string());
    value.state = json.value("state", SlicingState::Ready);
    value.gcode_uri = json.value("gcode_uri", std::string());
    value.gcode_sha256 = json.value("gcode_sha256", std::string());
    value.warnings = json.value("warnings", std::vector<std::string>{});
    value.error_code = json.value("error_code", std::string());
    value.error_message = json.value("error_message", std::string());
    value.layer_count = json.value("layer_count", uint64_t{0});
    value.material_volume_mm3 = json.value("material_volume_mm3", 0.0);
    value.elapsed_ms = json.value("elapsed_ms", uint64_t{0});
}

} // namespace Orca::SDK
