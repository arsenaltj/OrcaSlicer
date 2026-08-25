#include "OrcaParameterAdvisor.hpp"

#include "slic3r/AI/SmartSlicing/Domain/ParameterProposalValidator.hpp"
#include "slic3r/AI/SmartSlicing/Domain/SlicingMetrics.hpp"

#include "libslic3r/Model.hpp"

#include <algorithm>
#include <cmath>

namespace Slic3r::GUI {
namespace {

AI::SmartSlicing::ParameterIntent parameter_intent(AI::SmartSlicing::CandidateGoal goal)
{
    using namespace AI::SmartSlicing;
    switch (goal) {
    case CandidateGoal::Stability: return ParameterIntent::Stability;
    case CandidateGoal::Quality: return ParameterIntent::Quality;
    case CandidateGoal::Speed: return ParameterIntent::Speed;
    case CandidateGoal::MaterialSaving: return ParameterIntent::MaterialSaving;
    }
    return ParameterIntent::Unspecified;
}

std::optional<double> smallest_valid_nozzle(const AI::SmartSlicing::WorkspaceContext& context)
{
    std::optional<double> nozzle;
    for (const double diameter : context.nozzle_diameters) {
        if (!std::isfinite(diameter) || diameter <= 0.0)
            continue;
        nozzle = nozzle ? std::min(*nozzle, diameter) : diameter;
    }
    return nozzle;
}

AI::SmartSlicing::ParameterProposal advise_layer_height(
    const OrcaParameterAdvisorInput& input, const AI::SmartSlicing::WorkspaceContext& context,
    AI::SmartSlicing::CandidateGoal goal)
{
    using namespace AI::SmartSlicing;
    ParameterProposal proposal;
    proposal.intent = parameter_intent(goal);
    if (!input.current_layer_height || !std::isfinite(*input.current_layer_height))
        return {};
    const std::optional<double> nozzle = smallest_valid_nozzle(context);
    if (!nozzle)
        return {};

    const double current = *input.current_layer_height;
    double selected = current;
    const char* reason_code = nullptr;
    const char* explanation_code = nullptr;
    if (goal == CandidateGoal::Quality) {
        const double minimum = std::max(0.04, *nozzle * 0.25);
        selected = std::max(minimum, current - 0.04);
        reason_code = "use_finer_validated_layer_height";
        explanation_code = "finer_effective_layer_height";
    } else if (goal == CandidateGoal::Speed) {
        const double maximum = std::min(0.40, *nozzle * 0.75);
        selected = std::min(maximum, current + 0.04);
        reason_code = "use_coarser_validated_layer_height";
        explanation_code = "coarser_effective_layer_height";
    } else {
        return {};
    }
    if (!std::isfinite(selected) || std::abs(selected - current) < 1e-9)
        return {};

    proposal.entries.push_back({ConfigScope::Plate, PresetOwner::Process, input.plate_id, "layer_height",
                                current, selected, reason_code});
    proposal.explanation_codes.push_back(explanation_code);
    return ParameterProposalValidator().validate(proposal).accepted() ? proposal : ParameterProposal{};
}

} // namespace

std::optional<OrcaInstanceGeometrySnapshot>
orca_printable_instance_geometry(const ModelObject* object, const ModelInstance* instance)
{
    if (object == nullptr || instance == nullptr || !object->printable || !instance->printable)
        return std::nullopt;
    const Vec3d size = object->instance_bounding_box(*instance).size();
    return OrcaInstanceGeometrySnapshot{size.x(), size.y(), size.z()};
}

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
        if (!std::isfinite(risk))
            continue;
        maximum_risk = maximum_risk ? std::max(*maximum_risk, risk) : risk;
    }
    return maximum_risk;
}

AI::SmartSlicing::ParameterProposal
OrcaParameterAdvisor::advise(const AI::SmartSlicing::WorkspaceContext& context,
                             AI::SmartSlicing::CandidateGoal goal)
{
    using namespace AI::SmartSlicing;
    if (context.plate_index < 0 || m_input.plate_id < 0)
        return {};
    if (goal == CandidateGoal::Quality || goal == CandidateGoal::Speed)
        return advise_layer_height(m_input, context, goal);
    if (goal == CandidateGoal::MaterialSaving)
        return {};

    ParameterProposal proposal;
    proposal.intent = parameter_intent(goal);
    if (!std::isfinite(m_input.current_brim_width) || m_input.current_brim_width < 0.0)
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
