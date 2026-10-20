#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Orca::SDK {

enum class SlicingInputFormat : uint8_t {
    Stl = 0,
    Obj = 1,
    ThreeMf = 2,
    Step = 3,
};

enum class SlicingExecutionMode : uint8_t {
    Local = 0,
    Remote = 1,
};

enum class SlicingState : uint8_t {
    Ready = 0,
    Validating = 1,
    Slicing = 2,
    Completed = 3,
    Failed = 4,
    Canceled = 5,
};

struct SlicingRequest {
    std::string protocol_version{"orca.slicing/v1"};
    std::string request_id;
    std::string source_uri;
    std::string source_sha256;
    SlicingInputFormat input_format{SlicingInputFormat::ThreeMf};
    SlicingExecutionMode execution_mode{SlicingExecutionMode::Local};
    std::string printer_profile_id;
    std::string process_profile_id;
    std::string material_profile_id;
    std::string engine_version;
    uint32_t plate_index{0};
};

struct SlicingResult {
    std::string protocol_version{"orca.slicing/v1"};
    std::string request_id;
    SlicingState state{SlicingState::Ready};
    std::string gcode_uri;
    std::string gcode_sha256;
    std::vector<std::string> warnings;
    std::string error_code;
    std::string error_message;
    uint64_t layer_count{0};
    double material_volume_mm3{0.0};
    uint64_t elapsed_ms{0};
};

struct SlicingProgress {
    uint32_t percent{0};
    SlicingState state{SlicingState::Ready};
    std::string message;
};

using SlicingProgressCallback = std::function<void(const SlicingProgress&)>;

class ISlicingEngine {
public:
    virtual ~ISlicingEngine() = default;

    virtual SlicingResult validate(const SlicingRequest& request) = 0;
    virtual SlicingResult slice(const SlicingRequest& request,
                                SlicingProgressCallback progress = {}) = 0;
    virtual void cancel() = 0;
};

const char* slicing_input_format_name(SlicingInputFormat format);
const char* slicing_execution_mode_name(SlicingExecutionMode mode);
const char* slicing_state_name(SlicingState state);

void to_json(nlohmann::json& json, SlicingInputFormat value);
void from_json(const nlohmann::json& json, SlicingInputFormat& value);
void to_json(nlohmann::json& json, SlicingExecutionMode value);
void from_json(const nlohmann::json& json, SlicingExecutionMode& value);
void to_json(nlohmann::json& json, SlicingState value);
void from_json(const nlohmann::json& json, SlicingState& value);
void to_json(nlohmann::json& json, const SlicingRequest& value);
void from_json(const nlohmann::json& json, SlicingRequest& value);
void to_json(nlohmann::json& json, const SlicingResult& value);
void from_json(const nlohmann::json& json, SlicingResult& value);

} // namespace Orca::SDK
