#pragma once
#include "slic3r/GUI/AIModelGenerationClient.hpp"
#include "slic3r/AI/Contracts/GeneratedModelArtifact.hpp"
#include <wx/event.h>
#include <nlohmann/json.hpp>

namespace Slic3r::GUI {

// Shared local acquisition, with its own request lifetime. No submission or editor.
class ModelArtifactAcquisition final : public wxEvtHandler {
public:
    using Complete = std::function<void(AI::GeneratedModelArtifact)>;
    explicit ModelArtifactAcquisition();
    ~ModelArtifactAcquisition() override;
    void start(const AIModelGenerationClient::JobStatus& status, Complete complete,
               AIModelGenerationClient::ErrorFn failed,
               std::function<void(size_t, size_t)> progress = {});
    void cancel();
    static bool supported(const std::string& format, const std::string& color_encoding);
    static bool publish_library_record(const std::string& job, nlohmann::json metadata);
private:
    AIModelGenerationClient m_client;
    std::uint64_t m_generation {0};
};
}
