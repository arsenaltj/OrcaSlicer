#include "AIModelGenerationClient.hpp"
#include "slic3r/GUI/AI/ModelGeneration/SidecarModelGenerationService.hpp"

namespace Slic3r::GUI {
AIModelGenerationClient::AIModelGenerationClient(std::string endpoint)
    : AI::ModelGeneration::ModelGenerationClient(std::make_unique<SidecarModelGenerationService>(std::move(endpoint)))
{}

bool AIModelGenerationClient::is_loopback_endpoint(const std::string& endpoint)
{ return SidecarModelGenerationService::is_loopback_endpoint(endpoint); }

AIModelGenerationClient::GenerationOptions AIModelGenerationClient::restore_generation_options(const nlohmann::json& saved)
{ return SidecarModelGenerationService::restore_generation_options(saved); }

bool AIModelGenerationClient::validate_color_intent_manifest_file(const boost::filesystem::path& manifest_path,
    const std::string& schema, const std::string& sha256, const boost::filesystem::path& artifact_path)
{ return SidecarModelGenerationService::validate_color_intent_manifest_file(manifest_path, schema, sha256, artifact_path); }
} // namespace Slic3r::GUI
