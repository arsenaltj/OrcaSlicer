#pragma once
#include "ModelArtifactAcquisition.hpp"
#include "AdvancedModelTaskFlow.hpp"
#include <wx/timer.h>
#include <atomic>
#include <thread>
#include <optional>

namespace Slic3r::GUI {

// Follows an explicit advanced job. It owns no model panel, Beauty session or Plater.
class AdvancedModelTaskMonitor final : public wxEvtHandler {
public:
    AdvancedModelTaskMonitor();
    ~AdvancedModelTaskMonitor() override;
    void observe(const std::string& job, boost::filesystem::path reference, bool restart = false);
    void reset();
    void set_available(bool available);
    void set_listener(ModelGenerationUIStateListener listener);
    const ModelGenerationUIState& snapshot() const { return m_flow->snapshot(); }
    const std::optional<ModelLibraryEntry>& artifact() const { return m_flow->artifact(); }
    void retry_local();
    void stop_local() { m_flow->stop_local(); }
private:
    void cancel_requests();
    void prepare(AI::GeneratedModelArtifact artifact, AIModelGenerationClient::JobStatus status,
        boost::filesystem::path reference, std::function<void(ModelLibraryEntry)> complete,
        AIModelGenerationClient::ErrorFn failed);
    AIModelGenerationClient m_client;
    ModelArtifactAcquisition m_acquisition;
    wxTimer m_timer;
    std::thread m_worker;
    std::shared_ptr<std::atomic<bool>> m_cancel;
    std::unique_ptr<AdvancedModelTaskFlow> m_flow;
};
}
