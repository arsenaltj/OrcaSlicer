#pragma once

#include "slic3r/AI/SmartSlicing/Ports/IParameterAdvisor.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {
class ModelInstance;
class ModelObject;
}

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
    std::string current_brim_type;
    double current_brim_width{0.0};
    std::optional<double> current_layer_height;
    std::vector<OrcaInstanceGeometrySnapshot> printable_instances;
};

std::optional<double>
orca_bed_adhesion_risk_score(const std::vector<OrcaInstanceGeometrySnapshot>& printable_instances);

std::optional<OrcaInstanceGeometrySnapshot>
orca_printable_instance_geometry(const ModelObject* object, const ModelInstance* instance);

class OrcaParameterAdvisor final : public AI::SmartSlicing::IParameterAdvisor
{
public:
    explicit OrcaParameterAdvisor(OrcaParameterAdvisorInput input) : m_input(std::move(input)) {}

    AI::SmartSlicing::ParameterProposal advise(const AI::SmartSlicing::WorkspaceContext& context,
                                               AI::SmartSlicing::CandidateGoal goal) override;

private:
    OrcaParameterAdvisorInput m_input;
};

} // namespace Slic3r::GUI
