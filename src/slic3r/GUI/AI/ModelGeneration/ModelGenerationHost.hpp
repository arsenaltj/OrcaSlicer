#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Slic3r::GUI {

struct ModelGenerationUIInput
{
    std::string image_path;
    std::string prompt;
    std::string style { "sculpture" };
    // Keep the editable draft intact; submission trims it only for custom style.
    std::string custom_style;

    bool operator==(const ModelGenerationUIInput& other) const;
};

struct ModelGenerationUIOptions
{
    std::string provider { "tripo" };
    int face_limit { 1000000 };
    std::string geometry_quality { "standard" };
    std::string texture_quality { "standard" };
    std::string output_format { "glb" };

    bool operator==(const ModelGenerationUIOptions& other) const;
};

enum class ModelGenerationUIStage
{
    Input,
    Saving3DOptions,
    GeneratingDesign,
    Stopping,
    DesignReady,
    GeneratingModel,
    ModelReady,
    Failed,
    Stopped
};

struct ModelGenerationUIHistoryEntry
{
    std::string job_id;
    std::string title;
    std::string details;
    bool design_only { false };
    int thumbnail_width { 0 };
    int thumbnail_height { 0 };
    std::vector<unsigned char> thumbnail_rgb;
    std::vector<unsigned char> thumbnail_alpha;

    bool operator==(const ModelGenerationUIHistoryEntry& other) const;
};

struct ModelGenerationUIState
{
    std::uint64_t revision { 0 };
    ModelGenerationUIStage stage { ModelGenerationUIStage::Input };
    ModelGenerationUIInput input;
    ModelGenerationUIOptions options;
    bool service_available { false };
    bool service_availability_known { false };
    bool busy { false };
    bool can_generate_design { false };
    bool can_generate_model { false };
    bool can_stop { false };
    bool can_retry_service { false };
    bool can_restore_latest { false };
    bool can_import { false };
    bool can_restart { false };
    bool design_ready { false };
    bool model_ready { false };
    bool model_generation_context { false };
    bool inputs_match_job { true };
    int progress { 0 };
    std::string job_id;
    std::string job_state;
    std::string job_phase;
    std::string status_text;
    std::string summary_text;
    std::string workflow_phase;
    std::string workflow_guidance;
    std::string cost_summary;
    std::string original_image_path;
    std::string design_image_path;
    std::string model_path;
    std::vector<ModelGenerationUIHistoryEntry> history_entries;
    bool history_loading { false };
    std::string history_error;

    bool same_content(const ModelGenerationUIState& other) const;
};

using ModelGenerationUIStateListener = std::function<void(const ModelGenerationUIState&)>;

} // namespace Slic3r::GUI
