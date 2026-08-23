#include "OrcaParameterAdvisor.hpp"

#include "slic3r/AI/SmartSlicing/Domain/ParameterProposalValidator.hpp"

#include <algorithm>
#include <cmath>

namespace Slic3r::GUI {

AI::SmartSlicing::ParameterProposal
OrcaParameterAdvisor::advise(const AI::SmartSlicing::WorkspaceContext& context)
{
    using namespace AI::SmartSlicing;
    ParameterProposal proposal;
    if (context.plate_index < 0 || m_input.plate_id < 0 || !std::isfinite(m_input.current_brim_width) ||
        m_input.current_brim_width < 0.0 || m_input.current_brim_width >= 10.0)
        return proposal;

    const bool benefits_from_brim =
        std::any_of(m_input.printable_instances.begin(), m_input.printable_instances.end(), [](const auto& instance) {
            if (!std::isfinite(instance.width_mm) || !std::isfinite(instance.depth_mm) ||
                !std::isfinite(instance.height_mm))
                return false;
            const double minimum_footprint = std::min(instance.width_mm, instance.depth_mm);
            return minimum_footprint > 0.0 && instance.height_mm > 0.0 &&
                   (minimum_footprint <= 8.0 || instance.height_mm >= 2.0 * minimum_footprint);
        });
    if (!benefits_from_brim)
        return proposal;

    const double proposed_brim_width =
        std::min(10.0, std::max(5.0, m_input.current_brim_width + 2.0));
    proposal.entries.push_back({ConfigScope::Plate,
                                PresetOwner::Process,
                                m_input.plate_id,
                                "brim_width",
                                m_input.current_brim_width,
                                proposed_brim_width,
                                "improve_small_footprint_adhesion"});
    proposal.explanation_codes.push_back("small_or_slender_footprint");
    if (!ParameterProposalValidator().validate(proposal).accepted())
        return {};
    return proposal;
}

} // namespace Slic3r::GUI
