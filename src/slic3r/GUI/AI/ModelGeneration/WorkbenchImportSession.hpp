#pragma once

#include "slic3r/AI/Contracts/IModelArtifactConsumer.hpp"
#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace Slic3r::GUI {

enum class WorkbenchImportPhase { Reading, Colors, Placement, Committing, UpdatingView, Completed, Cancelled, Failed };

// The cancellation boundary is the project commit, shared by UI and worker.
class WorkbenchImportSession
{
public:
    WorkbenchImportPhase phase() const { return m_phase.load(); }
    bool valid() const { return m_valid.load(); }
    bool cancelled() const { return !valid() || phase() == WorkbenchImportPhase::Cancelled; }
    bool can_cancel() const { return valid() && phase() < WorkbenchImportPhase::Committing; }
    bool cancel()
    {
        auto previous = phase();
        while (previous < WorkbenchImportPhase::Committing)
            if (m_phase.compare_exchange_weak(previous, WorkbenchImportPhase::Cancelled)) return true;
        return false;
    }
    bool advance(WorkbenchImportPhase next)
    {
        auto previous = phase();
        while (valid() && previous < WorkbenchImportPhase::Completed && next >= previous) {
            if (next == WorkbenchImportPhase::Committing && previous == next) return false;
            if (m_phase.compare_exchange_weak(previous, next)) return true;
        }
        return false;
    }
    void invalidate() { m_valid = false; cancel(); }

private:
    std::atomic<WorkbenchImportPhase> m_phase {WorkbenchImportPhase::Reading};
    std::atomic<bool> m_valid {true};
};

using WorkbenchImportProgress = std::function<void(WorkbenchImportPhase, const std::string&)>;
using WorkbenchImportCompletion = std::function<void(const AI::ModelImportResult&)>;

}
