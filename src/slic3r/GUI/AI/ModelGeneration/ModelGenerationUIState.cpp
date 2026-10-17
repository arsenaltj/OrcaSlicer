#include "ModelGenerationHost.hpp"

namespace Slic3r::GUI {
bool ModelGenerationUIInput::operator==(const ModelGenerationUIInput& other) const
{
    return image_path == other.image_path && prompt == other.prompt && style == other.style &&
           custom_style == other.custom_style;
}

bool ModelGenerationUIOptions::operator==(const ModelGenerationUIOptions& other) const
{
    return provider == other.provider && face_limit == other.face_limit &&
           geometry_quality == other.geometry_quality && texture_quality == other.texture_quality &&
           output_format == other.output_format;
}

bool ModelGenerationUIHistoryEntry::operator==(const ModelGenerationUIHistoryEntry& other) const
{
    return asset_key == other.asset_key && job_id == other.job_id && title == other.title && details == other.details &&
           design_only == other.design_only && thumbnail_width == other.thumbnail_width &&
           thumbnail_height == other.thumbnail_height && thumbnail_rgb == other.thumbnail_rgb &&
           thumbnail_alpha == other.thumbnail_alpha;
}

bool ModelGenerationUIState::same_content(const ModelGenerationUIState& other) const
{
    return stage == other.stage && input == other.input && options == other.options &&
           service_available == other.service_available &&
           service_availability_known == other.service_availability_known && busy == other.busy &&
           can_generate_design == other.can_generate_design && can_generate_model == other.can_generate_model &&
           can_stop == other.can_stop && can_retry_service == other.can_retry_service &&
           can_retry_model == other.can_retry_model &&
           can_restore_latest == other.can_restore_latest && can_import == other.can_import &&
           can_open_workbench == other.can_open_workbench &&
           can_restart == other.can_restart && design_ready == other.design_ready && model_ready == other.model_ready &&
           model_generation_context == other.model_generation_context &&
           model_generation_session == other.model_generation_session && inputs_match_job == other.inputs_match_job &&
           model_options_match == other.model_options_match &&
           progress == other.progress && job_id == other.job_id && model_asset_id == other.model_asset_id &&
           job_state == other.job_state &&
           design_elapsed_seconds == other.design_elapsed_seconds &&
           design_estimated_seconds == other.design_estimated_seconds &&
           job_phase == other.job_phase && provider_error_code == other.provider_error_code &&
           provider_error_category == other.provider_error_category && provider_name == other.provider_name &&
           provider_task_id == other.provider_task_id &&
           provider_conversion_task_id == other.provider_conversion_task_id &&
           provider_error_retryable == other.provider_error_retryable &&
           provider_error_ambiguous == other.provider_error_ambiguous &&
           status_text == other.status_text && summary_text == other.summary_text &&
           workflow_phase == other.workflow_phase && workflow_guidance == other.workflow_guidance &&
           cost_summary == other.cost_summary && original_image_path == other.original_image_path &&
           design_image_path == other.design_image_path && model_path == other.model_path &&
           history_entries == other.history_entries && history_loading == other.history_loading &&
           history_error == other.history_error;
}

}
