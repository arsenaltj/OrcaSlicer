#include "slic3r/GUI/ModelGenerationPanel.hpp"
#include "ModelGenerationPresentation.hpp"
#include "ModelPreview3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Presentation/UiRuntime.hpp"
#include <wx/sizer.h>
#include <wx/weakref.h>

namespace Slic3r::GUI {
using namespace ModelGenerationPresentation;

void ModelGenerationPanel::initialize_workbench_assets()
{
    if (m_role != Role::WorkbenchAssets || m_workbench_instance) return;
    m_workbench_instance = std::make_unique<WorkbenchInstanceLease>();
    m_beauty_session = std::make_unique<BeautyWorkbenchSession<BeautyCandidateSnapshot>>();
    m_beauty_transactions = &m_beauty_session->transactions();
    initialize_for_shell_host();
}

WorkbenchOpenRequest ModelGenerationPanel::generated_workbench_request(WorkbenchOpenRequest::Origin origin) const
{
    WorkbenchOpenRequest request;
    request.origin = origin;
    request.generation = m_model_generation_session;
    request.sha256 = m_model_preview ? m_model_preview->model_sha256() : std::string{};
    request.return_context = origin == WorkbenchOpenRequest::Origin::Smart ? "smart" : "advanced";
    request.artifact.local_path = m_displayed_model_path;
    request.artifact.job_id = m_displayed_model_job_id;
    request.artifact.format = m_artifact_format;
    request.artifact.color_encoding = m_artifact_color_encoding;
    request.artifact.generation_palette = m_displayed_model_palette;
    request.artifact.used_printable_colors = m_job_use_printable_colors;
    if (!m_color_intent_path.empty()) request.artifact.color_intent_manifest = AI::ColorIntentManifestRef {
        m_color_intent_path.string(), m_color_intent_schema, m_color_intent_sha256};
    auto& entry = request.entry;
    entry.job_id = m_displayed_model_job_id;
    entry.model_path = m_displayed_model_path;
    entry.reference_image_path = m_reference_image_path;
    entry.ai_image_path = m_raw_preview_path;
    entry.palette = m_displayed_model_palette;
    entry.palette_roles = m_displayed_model_palette_roles;
    entry.use_printable_colors = m_job_use_printable_colors;
    entry.color_intent_path = m_color_intent_path;
    entry.color_intent_schema = m_color_intent_schema;
    entry.color_intent_sha256 = m_color_intent_sha256;
    entry.title = m_job_prompt;
    return request;
}

bool ModelGenerationPanel::open_workbench_asset(WorkbenchOpenRequest request)
{
    if (m_role != Role::WorkbenchAssets || m_shutdown || !can_replace_model_asset()) return false;
    const auto& entry = request.entry;
    if (!request.identifies_asset()) return false;
    return load_library_entry(entry.model_path, entry.reference_image_path, entry.ai_image_path,
        entry.palette, entry.palette_roles, entry.use_printable_colors, entry.color_intent_path,
        entry.color_intent_schema, entry.color_intent_sha256, entry.job_id, entry.title, request.new_asset, entry.repair_index,
        {}, request.sha256);
}

} // namespace Slic3r::GUI
