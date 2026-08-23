#pragma once

#include "slic3r/AI/SmartSlicing/Ports/IParameterAdvisor.hpp"

#include <cstdint>
#include <utility>
#include <vector>

namespace Slic3r::GUI {

struct OrcaInstanceGeometrySnapshot
{
    double width_mm{0.0};
    double depth_mm{0.0};
    double height_mm{0.0};
};

struct OrcaParameterAdvisorInput
{
    int64_t plate_id{-1};
    double current_brim_width{0.0};
    std::vector<OrcaInstanceGeometrySnapshot> printable_instances;
};

class OrcaParameterAdvisor final : public AI::SmartSlicing::IParameterAdvisor
{
public:
    explicit OrcaParameterAdvisor(OrcaParameterAdvisorInput input) : m_input(std::move(input)) {}

    AI::SmartSlicing::ParameterProposal advise(const AI::SmartSlicing::WorkspaceContext& context) override;

private:
    OrcaParameterAdvisorInput m_input;
};

} // namespace Slic3r::GUI
