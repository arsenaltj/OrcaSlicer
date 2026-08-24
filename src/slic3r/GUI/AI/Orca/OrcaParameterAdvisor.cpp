#include "OrcaParameterAdvisor.hpp"

#include "slic3r/AI/SmartSlicing/Domain/ParameterProposalValidator.hpp"
#include "slic3r/AI/SmartSlicing/Domain/SlicingMetrics.hpp"

#include <algorithm>
#include <cmath>

namespace Slic3r::GUI {

std::optional<double>
orca_bed_adhesion_risk_score(const std::vector<OrcaInstanceGeometrySnapshot>& printable_instances)
{
    std::optional<double> maximum_risk;
    for (const OrcaInstanceGeometrySnapshot& instance : printable_instances) {
        if (!std::isfinite(instance.width_mm) || !std::isfinite(instance.depth_mm) ||
            !std::isfinite(instance.height_mm))
            continue;
        const double minimum_footprint = std::min(instance.width_mm, instance.depth_mm);
        if (minimum_footprint <= 0.0 || instance.height_mm <= 0.0)
            continue;
        const double risk = std::max(8.0 / minimum_footprint,
                                     instance.height_mm / (2.0 * minimum_footprint));
        maximum_risk = maximum_risk ? std::max(*maximum_risk, risk) : risk;
    }
    return maximum_risk;
}

AI::SmartSlicing::ParameterProposal
OrcaParameterAdvisor::advise(const AI::SmartSlicing::WorkspaceContext& context)
{
    using namespace AI::SmartSlicing;
    ParameterProposal proposal;
    if (context.plate_index < 0 || m_input.plate_id < 0 || !std::isfinite(m_input.current_brim_width) ||
        m_input.current_brim_width < 0.0)
        return proposal;

    const std::optional<double> adhesion_risk = orca_bed_adhesion_risk_score(m_input.printable_instances);
    if (!adhesion_risk || *adhesion_risk < BED_ADHESION_RISK_ATTENTION_THRESHOLD ||
        m_input.current_brim_type == "auto_brim")
        return proposal;

    if (m_input.current_brim_type == "no_brim") {
        proposal.entries.push_back({ConfigScope::Plate,
                                    PresetOwner::Process,
                                    m_input.plate_id,
                                    "brim_type",
                                    std::string("no_brim"),
                                    std::string("auto_brim"),
                                    "enable_native_auto_brim"});
        proposal.explanation_codes.push_back("small_or_slender_footprint");
        return ParameterProposalValidator().validate(proposal).accepted() ? proposal : ParameterProposal{};
    }

    const bool bounded_manual_brim = m_input.current_brim_type == "outer_only" ||
                                     m_input.current_brim_type == "outer_and_inner";
    if (!bounded_manual_brim || m_input.current_brim_width >= 10.0)
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
