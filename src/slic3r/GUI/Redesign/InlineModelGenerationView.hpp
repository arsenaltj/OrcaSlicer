#pragma once

#include "InlineModelGenerationState.hpp"
#include <wx/panel.h>
#include <wx/image.h>
#include <functional>
#include <memory>

namespace Slic3r::GUI {

struct InlineModelCallbacks {
    std::function<void()> recover;
    std::function<void()> changed;
    std::function<void()> completed;
};

class InlineModelGenerationView final : public wxPanel {
public:
    InlineModelGenerationView(wxWindow* parent, InlineModelCallbacks callbacks);
    ~InlineModelGenerationView() override;
    void apply_snapshot(const GenerationBinding& binding, const ModelGenerationUIState& state,
                        std::optional<WorkbenchOpenRequest> artifact);
    void set_presented(bool active);
    void retry_preview();
    void generation_started();
    void invalidate_context();
    void shutdown();
    bool has_content() const;
    bool ready() const;
    std::optional<WorkbenchOpenRequest> displayed_artifact() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

class GenerationCompletionActions final : public wxPanel {
public:
    GenerationCompletionActions(wxWindow* parent, std::function<void()> regenerate,
                                std::function<void()> open_workbench);
    void set_enabled(bool regenerate, bool open_workbench);
private:
    void paint();
    wxWindow* m_regenerate;
    wxWindow* m_workbench;
    wxImage m_glow_source;
    wxBitmap m_glow;
    wxSize m_glow_size;
};

} // namespace Slic3r::GUI
