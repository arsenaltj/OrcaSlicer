#pragma once
#include "slic3r/AI/ModelGeneration/ModelGenerationClient.hpp"
#include <nlohmann/json.hpp>

namespace Slic3r::GUI {
// Source-compatible desktop facade. UI keeps its current request/sequence owner;
// the selected service contains transport and provider-independent job conversion.
class AIModelGenerationClient : public AI::ModelGeneration::ModelGenerationClient {
public:
    using AI::ModelGeneration::ModelGenerationClient::ModelGenerationClient;
    explicit AIModelGenerationClient(std::string endpoint);
    ~AIModelGenerationClient() override = default;
    static bool is_loopback_endpoint(const std::string& endpoint);
    // Restores editable choices without changing the historical job or silently
    // upgrading pre-option Tripo jobs to a costlier geometry tier.
    static GenerationOptions restore_generation_options(const nlohmann::json& saved);
    static bool validate_color_intent_manifest_file(const boost::filesystem::path& manifest_path,
                                                    const std::string& schema, const std::string& sha256,
                                                    const boost::filesystem::path& artifact_path);

};
} // namespace Slic3r::GUI
