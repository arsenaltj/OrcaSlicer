#pragma once
#include "slic3r/AI/SmartSlicing/Ports/IParameterAdvisor.hpp"
#include <algorithm>
#include <memory>

namespace Slic3r::AI::SmartSlicing {
// Preserve the existing small/slender footprint proposal as the initial strategy.
class BrimParameterAdvisor final : public IParameterAdvisor {
public:
    const char* algorithm_id() const noexcept override { return "brim-stability"; }
    const char* algorithm_version() const noexcept override { return "brim-stability-v1"; }
    ParameterProposal advise(const WorkspaceContext& context) override
    {
        ParameterProposal proposal;
        if (!context.brim_scope_consistent || context.parameter_plate_id < 0 || !(context.current_brim_width < 10.0))
            return proposal;
        const bool benefits_from_brim = std::any_of(context.printable_instance_sizes_mm.begin(),
            context.printable_instance_sizes_mm.end(), [](const auto& size) {
                const double footprint = std::min(size[0], size[1]);
                return footprint > 0.0 && (footprint <= 8.0 || size[2] >= 2.0 * footprint);
            });
        if (benefits_from_brim)
            proposal.entries.push_back({ConfigScope::Plate, PresetOwner::Process,
                context.parameter_plate_id, "brim_width", context.current_brim_width,
                std::min(10.0, std::max(5.0, context.current_brim_width + 2.0)),
                "improve_small_footprint_adhesion"});
        return proposal;
    }
};
inline std::shared_ptr<IParameterAdvisor> baseline_parameter_advisor()
{ return std::make_shared<BrimParameterAdvisor>(); }
} // namespace Slic3r::AI::SmartSlicing
