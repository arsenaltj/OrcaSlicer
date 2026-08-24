#pragma once

#include "slic3r/AI/SmartSlicing/Ports/IWorkflowRuntimeStore.hpp"

#include <boost/filesystem/path.hpp>

#include <mutex>

namespace Slic3r::GUI {

class OrcaWorkflowRuntimeStore final : public AI::SmartSlicing::IWorkflowRuntimeStore
{
public:
    explicit OrcaWorkflowRuntimeStore(boost::filesystem::path journal_path);

    std::optional<AI::SmartSlicing::WorkflowRuntimeRecord> load() override;
    void save(const AI::SmartSlicing::WorkflowRuntimeRecord& record) override;
    void clear(AI::SmartSlicing::WorkflowId workflow_id) override;

private:
    boost::filesystem::path m_journal_path;
    std::mutex m_mutex;
    AI::SmartSlicing::WorkflowId m_known_workflow_id{0};
};

} // namespace Slic3r::GUI
