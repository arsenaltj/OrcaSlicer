#pragma once

#include "../AI/ModelGeneration/ModelGenerationHost.hpp"
#include "../AI/ModelGeneration/ModelGenerationPresentation.hpp"

namespace Slic3r::GUI {

// Historical records may omit their style. A new editable input uses the
// default shown by the picker, without rewriting the historical record.
inline ModelGenerationUIInput image_design_input_for_editing(ModelGenerationUIInput input)
{
    if (!ModelGenerationPresentation::is_supported_style(input.style))
        input.style = "sculpture";
    return input;
}

inline bool image_design_restores_input(const ModelGenerationUIState& state,
                                       const ModelGenerationUIState& current)
{
    return state.model_generation_session > current.model_generation_session;
}

inline bool image_design_refreshes_history(const ModelGenerationUIState& state,
                                          const ModelGenerationUIState& current)
{
    // A preview can arrive before the completed job is persisted. Refresh again
    // when the design becomes final so that history does not retain that early scan.
    return state.design_ready && (!current.design_ready || state.job_id != current.job_id ||
        state.design_image_path != current.design_image_path ||
        (state.stage == ModelGenerationUIStage::DesignReady &&
         current.stage != ModelGenerationUIStage::DesignReady));
}

// A successful upload starts a new image draft even when the same file is
// selected again. The retained 3D asset/task is not this draft's 2D result.
class ImageDesignDraft
{
public:
    void begin(const ModelGenerationUIState& state)
    {
        m_pending = true;
        m_submission_requested = false;
        m_revision = state.revision;
        m_model_session = state.model_generation_session;
    }

    void observe(const ModelGenerationUIState& state)
    {
        if (!m_pending || state.revision <= m_revision)
            return;
        // Confirmation alone does not start generation. The host publishes
        // GeneratingDesign only after confirmation; history opens a new session.
        if (state.model_generation_session > m_model_session ||
            (m_submission_requested && state.stage == ModelGenerationUIStage::GeneratingDesign &&
             !state.model_generation_context))
            m_pending = false;
    }

    void begin_submission() { m_submission_requested = true; }
    void end_submission(const ModelGenerationUIState& state)
    {
        observe(state);
        m_submission_requested = false;
    }

    bool pending() const { return m_pending; }
    ModelGenerationUIStage stage(const ModelGenerationUIState& state) const
    {
        return m_pending ? ModelGenerationUIStage::Input : state.stage;
    }

private:
    bool m_pending {false};
    bool m_submission_requested {false};
    std::uint64_t m_revision {0};
    std::uint64_t m_model_session {0};
};

} // namespace Slic3r::GUI
