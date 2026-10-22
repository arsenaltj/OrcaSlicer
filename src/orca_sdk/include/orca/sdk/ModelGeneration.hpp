#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Orca::SDK {

enum class ModelInputSource : uint8_t {
    Text = 0,
    Image = 1,
};

enum class ModelJobState : uint8_t {
    Queued = 0,
    Preprocessing = 1,
    AwaitingConfirmation = 2,
    Generating = 3,
    Succeeded = 4,
    Failed = 5,
    Canceled = 6,
};

enum class ModelArtifactFormat : uint8_t {
    Glb = 0,
    Obj = 1,
    ThreeMf = 2,
    Stl = 3,
};

struct GenerationOptions {
    std::string provider;
    uint32_t face_limit{1000000};
    std::string geometry_quality{"standard"};
    std::string texture_quality{"standard"};
    ModelArtifactFormat output_format{ModelArtifactFormat::Glb};
};

struct ModelJob {
    std::string protocol_version{"orca.model-job/v1"};
    std::string request_id;
    std::string job_id;
    ModelInputSource source{ModelInputSource::Text};
    ModelJobState state{ModelJobState::Queued};
    uint32_t progress{0};
    std::string message;
    std::string provider_task_id;
    GenerationOptions options;
    std::string artifact_id;
    std::string artifact_sha256;
    std::string error_code;
    bool retryable{false};
};

struct ArtifactManifest {
    std::string artifact_id;
    std::string job_id;
    ModelArtifactFormat format{ModelArtifactFormat::Glb};
    std::string media_type;
    uint64_t size_bytes{0};
    std::string sha256;
    std::string download_url;
    std::string color_intent_url;
};

struct TextModelRequest {
    std::string request_id;
    std::string prompt;
    GenerationOptions options;
};

struct ImageModelRequest {
    std::string request_id;
    std::string image_uri;
    std::string instruction;
    GenerationOptions options;
};

class IModelGenerationClient {
public:
    virtual ~IModelGenerationClient() = default;

    virtual ModelJob create_text_job(const TextModelRequest& request) = 0;
    virtual ModelJob create_image_job(const ImageModelRequest& request) = 0;
    virtual ModelJob get_status(const std::string& job_id) = 0;
    virtual ModelJob stop(const std::string& job_id) = 0;
    virtual ArtifactManifest get_artifact(const std::string& job_id,
                                          ModelArtifactFormat format) = 0;
    virtual bool remove(const std::string& job_id, std::string* error_code = nullptr) = 0;
};

const char* model_input_source_name(ModelInputSource source);
const char* model_job_state_name(ModelJobState state);
const char* model_artifact_format_name(ModelArtifactFormat format);

void to_json(nlohmann::json& json, ModelInputSource value);
void from_json(const nlohmann::json& json, ModelInputSource& value);
void to_json(nlohmann::json& json, ModelJobState value);
void from_json(const nlohmann::json& json, ModelJobState& value);
void to_json(nlohmann::json& json, ModelArtifactFormat value);
void from_json(const nlohmann::json& json, ModelArtifactFormat& value);
void to_json(nlohmann::json& json, const GenerationOptions& value);
void from_json(const nlohmann::json& json, GenerationOptions& value);
void to_json(nlohmann::json& json, const ModelJob& value);
void from_json(const nlohmann::json& json, ModelJob& value);
void to_json(nlohmann::json& json, const ArtifactManifest& value);
void from_json(const nlohmann::json& json, ArtifactManifest& value);

} // namespace Orca::SDK
