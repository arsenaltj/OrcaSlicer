#pragma once

#include "slic3r/AI/SmartSlicing/Ports/IOrcaWorkspace.hpp"

namespace Slic3r::GUI {

class Plater;

// Read-only anti-corruption layer for smart slicing. Capturing or inspecting
// through this adapter must never dirty the project or invalidate official
// slicing results.
class OrcaSmartSlicingAdapter final : public AI::SmartSlicing::IOrcaWorkspace
{
public:
    explicit OrcaSmartSlicingAdapter(Plater* plater) : m_plater(plater) {}

    AI::SmartSlicing::WorkspaceRevision current_revision() const override;
    AI::SmartSlicing::WorkspaceContext capture_context() const override;

private:
    AI::SmartSlicing::WorkspaceContext capture_context_impl(bool include_diagnostics) const;

    Plater* m_plater{nullptr};
};

} // namespace Slic3r::GUI
