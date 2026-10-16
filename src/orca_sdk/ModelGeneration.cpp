#include "orca/sdk/ModelGeneration.hpp"

#include <stdexcept>

namespace Orca::SDK {

namespace {

template <typename Enum>
Enum unsupported(const std::string& value, const char* name)
{
    throw std::invalid_argument(std::string("Unknown ") + name + ": " + value);
}

} // namespace

const char* model_input_source_name(ModelInputSource source)
{
    switch (source) {
    case ModelInputSource::Text: return "text";
    case ModelInputSource::Image: return "image";
    }
    return "unknown";
}

const char* model_job_state_name(ModelJobState state)
{
    switch (state) {
    case ModelJobState::Queued: return "queued";
    case ModelJobState::Preprocessing: return "preprocessing";
    case ModelJobState::AwaitingConfirmation: return "awaiting_confirmation";
    case ModelJobState::Generating: return "generating";
    case ModelJobState::Succeeded: return "succeeded";
    case ModelJobState::Failed: return "failed";
    case ModelJobState::Canceled: return "canceled";
    }
    return "unknown";
}

const char* model_artifact_format_name(ModelArtifactFormat format)
{
    switch (format) {
    case ModelArtifactFormat::Glb: return "glb";
    case ModelArtifactFormat::Obj: return "obj";
    case ModelArtifactFormat::ThreeMf: return "3mf";
    case ModelArtifactFormat::Stl: return "stl";
    }
    return "unknown";
}

void to_json(nlohmann::json& json, ModelInputSource value) { json = model_input_source_name(value); }
void to_json(nlohmann::json& json, ModelJobState value) { json = model_job_state_name(value); }
void to_json(nlohmann::json& json, ModelArtifactFormat value) { json = model_artifact_format_name(value); }

void from_json(const nlohmann::json& json, ModelInputSource& value)
{
    const std::string name = json.get<std::string>();
    if (name == "text") value = ModelInputSource::Text;
    else if (name == "image") value = ModelInputSource::Image;
    else value = unsupported<ModelInputSource>(name, "model input source");
}

void from_json(const nlohmann::json& json, ModelJobState& value)
{
    const std::string name = json.get<std::string>();
    if (name == "queued") value = ModelJobState::Queued;
    else if (name == "preprocessing") value = ModelJobState::Preprocessing;
    else if (name == "awaiting_confirmation") value = ModelJobState::AwaitingConfirmation;
    else if (name == "generating") value = ModelJobState::Generating;
    else if (name == "succeeded") value = ModelJobState::Succeeded;
    else if (name == "failed") value = ModelJobState::Failed;
    else if (name == "canceled") value = ModelJobState::Canceled;
    else value = unsupported<ModelJobState>(name, "model job state");
}

void from_json(const nlohmann::json& json, ModelArtifactFormat& value)
{
    const std::string name = json.get<std::string>();
    if (name == "glb") value = ModelArtifactFormat::Glb;
    else if (name == "obj") value = ModelArtifactFormat::Obj;
    else if (name == "3mf") value = ModelArtifactFormat::ThreeMf;
    else if (name == "stl") value = ModelArtifactFormat::Stl;
    else value = unsupported<ModelArtifactFormat>(name, "model artifact format");
}

void to_json(nlohmann::json& json, const GenerationOptions& value)
{
    json = {
        {"provider", value.provider},
        {"face_limit", value.face_limit},
        {"geometry_quality", value.geometry_quality},
        {"texture_quality", value.texture_quality},
        {"output_format", value.output_format},
    };
}

void from_json(const nlohmann::json& json, GenerationOptions& value)
{
    value.provider = json.value("provider", std::string());
    value.face_limit = json.value("face_limit", uint32_t{1000000});
    value.geometry_quality = json.value("geometry_quality", std::string("standard"));
    value.texture_quality = json.value("texture_quality", std::string("standard"));
    value.output_format = json.value("output_format", ModelArtifactFormat::Glb);
}

void to_json(nlohmann::json& json, const ModelJob& value)
{
    json = {
        {"protocol_version", value.protocol_version},
        {"request_id", value.request_id},
        {"job_id", value.job_id},
        {"source", value.source},
        {"state", value.state},
        {"progress", value.progress},
        {"message", value.message},
        {"provider_task_id", value.provider_task_id},
        {"options", value.options},
        {"artifact_id", value.artifact_id},
        {"artifact_sha256", value.artifact_sha256},
        {"error_code", value.error_code},
        {"retryable", value.retryable},
    };
}

void from_json(const nlohmann::json& json, ModelJob& value)
{
    value.protocol_version = json.value("protocol_version", std::string("orca.model-job/v1"));
    value.request_id = json.value("request_id", std::string());
    value.job_id = json.value("job_id", std::string());
    value.source = json.value("source", ModelInputSource::Text);
    value.state = json.value("state", ModelJobState::Queued);
    value.progress = json.value("progress", uint32_t{0});
    value.message = json.value("message", std::string());
    value.provider_task_id = json.value("provider_task_id", std::string());
    value.options = json.value("options", GenerationOptions{});
    value.artifact_id = json.value("artifact_id", std::string());
    value.artifact_sha256 = json.value("artifact_sha256", std::string());
    value.error_code = json.value("error_code", std::string());
    value.retryable = json.value("retryable", false);
}

void to_json(nlohmann::json& json, const ArtifactManifest& value)
{
    json = {
        {"artifact_id", value.artifact_id},
        {"job_id", value.job_id},
        {"format", value.format},
        {"media_type", value.media_type},
        {"size_bytes", value.size_bytes},
        {"sha256", value.sha256},
        {"download_url", value.download_url},
        {"color_intent_url", value.color_intent_url},
    };
}

void from_json(const nlohmann::json& json, ArtifactManifest& value)
{
    value.artifact_id = json.value("artifact_id", std::string());
    value.job_id = json.value("job_id", std::string());
    value.format = json.value("format", ModelArtifactFormat::Glb);
    value.media_type = json.value("media_type", std::string());
    value.size_bytes = json.value("size_bytes", uint64_t{0});
    value.sha256 = json.value("sha256", std::string());
    value.download_url = json.value("download_url", std::string());
    value.color_intent_url = json.value("color_intent_url", std::string());
}

} // namespace Orca::SDK
