#pragma once
#include "ModelArtifactAcquisition.hpp"
#include "AdvancedModelTaskFlow.hpp"
#include "ModelLibraryApplication.hpp"
#include <wx/timer.h>
#include <atomic>
#include <memory>
#include <thread>
#include "slic3r/GUI/Presentation/UiRuntime.hpp"
#include <optional>

namespace Slic3r::GUI {

// Follows an explicit advanced job. It owns no model panel, Beauty session or Plater.
class AdvancedModelTaskMonitor final : public wxEvtHandler {
public:
    AdvancedModelTaskMonitor();
    ~AdvancedModelTaskMonitor() override;
    void observe(const std::string& job, boost::filesystem::path reference, bool restart = false,
                 ModelGenerationVisualReference visual = {});
    void reset();
    void set_available(bool available);
    void set_listener(ModelGenerationUIStateListener listener);
    const ModelGenerationUIState& snapshot() const { return m_flow->snapshot(); }
    const std::optional<ModelLibraryEntry>& artifact() const { return m_flow->artifact(); }
    void retry_local();
    void set_library_commit_listener(std::function<void(ModelLibraryCommit)> listener) { m_library_commit_listener = std::move(listener); }
    void stop_local() { m_flow->stop_local(); }
    void request_stop() { m_flow->request_stop(); }
    void finish_stop(const std::string& job = {}) { m_flow->finish_stop(job); }
private:
    void cancel_requests();
    void prepare(AI::GeneratedModelArtifact artifact, AIModelGenerationClient::JobStatus status,
        boost::filesystem::path reference, std::function<void(ModelLibraryEntry)> complete,
        AIModelGenerationClient::ErrorFn failed);
    AIModelGenerationClient m_client;
    ModelArtifactAcquisition m_acquisition;
    wxTimer m_timer;
    Presentation::TaskScope m_preparation;
    std::unique_ptr<AdvancedModelTaskFlow> m_flow;
    std::shared_ptr<int> m_lifetime {std::make_shared<int>(0)};
    std::function<void(ModelLibraryCommit)> m_library_commit_listener;
};
}
