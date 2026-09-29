#include "ApplyContract.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <tuple>
#include <type_traits>

namespace Slic3r::AI::SmartSlicing {
namespace {

uint64_t fnv1a64(const std::string& value)
{
    uint64_t hash = 14695981039346656037ull;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string digest(const std::string& canonical)
{
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16) << fnv1a64(canonical);
    return stream.str();
}

std::vector<RiskConfirmationKind> canonical_confirmations(std::vector<RiskConfirmationKind> values)
{
    std::sort(values.begin(), values.end());
    return values;
}

class CanonicalWriter
{
public:
    CanonicalWriter() { m_stream.imbue(std::locale::classic()); }
    void text(const std::string& value) { m_stream << value.size() << ':' << value; }
    void boolean(bool value) { m_stream << (value ? "1" : "0") << ';'; }
    template<class T> void integer(T value)
    {
        if constexpr (std::is_enum_v<T>)
            integer(static_cast<std::underlying_type_t<T>>(value));
        else if constexpr (std::is_unsigned_v<T>)
            m_stream << static_cast<unsigned long long>(value) << ';';
        else
            m_stream << static_cast<long long>(value) << ';';
    }
    void number(double value)
    {
        m_stream << std::setprecision(std::numeric_limits<double>::max_digits10) << value << ';';
    }
    std::string str() const { return m_stream.str(); }

private:
    std::ostringstream m_stream;
};

void append_evidence_value(CanonicalWriter& writer, const EvidenceValue<double>& evidence)
{
    writer.integer(evidence.availability);
    writer.boolean(evidence.value.has_value());
    if (evidence.value)
        writer.number(*evidence.value);
    writer.text(evidence.source_code);
}

void append_config_value(CanonicalWriter& writer, const ConfigValue& value)
{
    writer.integer(value.index());
    std::visit([&](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, bool>)
            writer.boolean(item);
        else if constexpr (std::is_same_v<T, int64_t>)
            writer.integer(item);
        else if constexpr (std::is_same_v<T, double>)
            writer.number(item);
        else
            writer.text(item);
    }, value);
}

void append_profile(CanonicalWriter& writer, const ProfileIdentity& profile)
{
    writer.text(profile.setting_id);
    writer.integer(profile.inheritance_chain.size());
    for (const std::string& parent : profile.inheritance_chain)
        writer.text(parent);
    writer.text(profile.fingerprint);
}

void append_value(CanonicalWriter& writer, bool value) { writer.boolean(value); }
void append_value(CanonicalWriter& writer, const NozzleOffset& value)
{
    writer.number(value.x_mm);
    writer.number(value.y_mm);
}
void append_value(CanonicalWriter& writer, const ReachableArea& value)
{
    writer.number(value.minimum_x_mm);
    writer.number(value.maximum_x_mm);
    writer.number(value.minimum_y_mm);
    writer.number(value.maximum_y_mm);
}
void append_value(CanonicalWriter& writer, const CollisionClearanceLimits& value)
{
    writer.number(value.nozzle_radius_mm);
    writer.number(value.carriage_radius_mm);
    writer.number(value.vertical_clearance_mm);
}
void append_value(CanonicalWriter& writer, const ToolChangeGcodeCapability& value)
{
    writer.text(value.command_template);
    writer.boolean(value.supports_target_tool);
}
void append_value(CanonicalWriter& writer, const PreheatBehavior& value)
{
    writer.number(value.lead_time_seconds);
    writer.number(value.minimum_ready_temperature_c);
}
void append_value(CanonicalWriter& writer, const StandbyBehavior& value)
{
    writer.number(value.standby_temperature_c);
    writer.number(value.resume_temperature_tolerance_c);
}
void append_value(CanonicalWriter& writer, const RetractionBehavior& value)
{
    writer.number(value.length_mm);
    writer.number(value.speed_mm_per_second);
}
void append_value(CanonicalWriter& writer, const ToolChangeTimeModel& value)
{
    writer.number(value.fixed_seconds);
    writer.number(value.thermal_recovery_seconds);
}
void append_value(CanonicalWriter& writer, const PrimeOrWipeCapability& value)
{
    writer.boolean(value.prime_tower_supported);
    writer.boolean(value.precharge_supported);
}
void append_value(CanonicalWriter& writer, const FlushMatrix& value)
{
    writer.integer(value.tool_count);
    writer.integer(value.volume_mm3.size());
    for (double volume : value.volume_mm3)
        writer.number(volume);
}
void append_value(CanonicalWriter& writer, const WipeTowerSpaceConstraints& value)
{
    writer.number(value.minimum_width_mm);
    writer.number(value.minimum_depth_mm);
    writer.number(value.clearance_mm);
}

template<class T> void append_evidence(CanonicalWriter& writer, const EvidenceValue<T>& evidence)
{
    writer.integer(evidence.availability);
    writer.text(evidence.source_code);
    writer.boolean(evidence.value.has_value());
    if (evidence.value)
        append_value(writer, *evidence.value);
}

} // namespace

const char* risk_confirmation_kind_name(RiskConfirmationKind kind)
{
    switch (kind) {
    case RiskConfirmationKind::ProtectedRegionSupportContact:
        return "protected_region_support_contact";
    case RiskConfirmationKind::ProtectedRegionSeam: return "protected_region_seam";
    }
    return "unknown";
}

bool operator==(const ReadyApplyBinding& lhs, const ReadyApplyBinding& rhs)
{
    return lhs.workflow_id == rhs.workflow_id && lhs.attempt_id == rhs.attempt_id &&
           lhs.base_revision == rhs.base_revision && lhs.goal == rhs.goal &&
           lhs.goal_task_candidate_id == rhs.goal_task_candidate_id &&
           lhs.selected_candidate_id == rhs.selected_candidate_id &&
           lhs.strategy_version == rhs.strategy_version &&
           lhs.selection_policy_version == rhs.selection_policy_version &&
           lhs.parameter_policy_version == rhs.parameter_policy_version &&
           lhs.risk_evidence_digest == rhs.risk_evidence_digest &&
           lhs.risk_confirmation_policy_version == rhs.risk_confirmation_policy_version &&
           lhs.risk_confirmation_evidence_digest == rhs.risk_confirmation_evidence_digest &&
           canonical_confirmations(lhs.required_confirmations) ==
               canonical_confirmations(rhs.required_confirmations) &&
           lhs.intent_evidence_revision == rhs.intent_evidence_revision &&
           lhs.machine_registry_version == rhs.machine_registry_version &&
           lhs.material_registry_version == rhs.material_registry_version &&
           lhs.machine_capability_evidence_digest == rhs.machine_capability_evidence_digest &&
           lhs.material_compatibility_evidence_digest == rhs.material_compatibility_evidence_digest &&
           lhs.candidate_payload_digest == rhs.candidate_payload_digest;
}

RiskConfirmationToken::RiskConfirmationToken(std::string token_id, ReadyApplyBinding binding)
    : m_token_id(std::move(token_id)), m_binding(std::move(binding))
{}

const char* apply_rejection_code_name(ApplyRejectionCode code)
{
    switch (code) {
    case ApplyRejectionCode::None: return "none";
    case ApplyRejectionCode::InvalidContract: return "apply_contract_invalid";
    case ApplyRejectionCode::InvalidCommandId: return "apply_command_id_invalid";
    case ApplyRejectionCode::CommandAlreadyConsumed: return "apply_command_already_consumed";
    case ApplyRejectionCode::WorkflowChanged: return "apply_workflow_changed";
    case ApplyRejectionCode::AttemptChanged: return "apply_attempt_changed";
    case ApplyRejectionCode::WorkspaceChanged: return "workspace_changed";
    case ApplyRejectionCode::GoalChanged: return "apply_goal_changed";
    case ApplyRejectionCode::GoalNotReady: return "apply_goal_not_ready";
    case ApplyRejectionCode::CandidateNotReady: return "candidate_not_ready";
    case ApplyRejectionCode::GoalTaskCandidateChanged: return "apply_task_candidate_changed";
    case ApplyRejectionCode::SelectedCandidateChanged: return "apply_selected_candidate_changed";
    case ApplyRejectionCode::StrategyVersionChanged: return "strategy_version_changed";
    case ApplyRejectionCode::SelectionPolicyVersionChanged: return "selection_policy_version_changed";
    case ApplyRejectionCode::ParameterPolicyVersionChanged: return "parameter_policy_version_changed";
    case ApplyRejectionCode::RiskEvidenceChanged: return "risk_evidence_changed";
    case ApplyRejectionCode::RequiredConfirmationsChanged: return "required_confirmations_changed";
    case ApplyRejectionCode::RiskConfirmationRequired: return "risk_confirmation_required";
    case ApplyRejectionCode::RiskConfirmationTokenMismatch: return "risk_confirmation_token_mismatch";
    case ApplyRejectionCode::IntentConstraintChanged: return "intent_constraint_changed";
    case ApplyRejectionCode::MachineCapabilityChanged: return "machine_capability_changed";
    case ApplyRejectionCode::MaterialCompatibilityChanged: return "material_compatibility_changed";
    case ApplyRejectionCode::MachineCapabilityUnavailable: return "machine_capability_unavailable";
    case ApplyRejectionCode::UnsupportedMaterialCombination: return "unsupported_material_combination";
    case ApplyRejectionCode::ActiveModelTool: return "active_model_tool";
    case ApplyRejectionCode::ActiveOfficialTransaction: return "active_official_transaction";
    case ApplyRejectionCode::InvalidPlacement: return "candidate_placement_invalid";
    case ApplyRejectionCode::PlacementIntentConflict: return "candidate_placement_intent_conflict";
    case ApplyRejectionCode::ParameterRevalidationFailed: return "parameter_revalidation_failed";
    }
    return "apply_rejected";
}

std::string recommendation_risk_evidence_digest(const RecommendationEvidence& evidence)
{
    CanonicalWriter writer;
    writer.text(evidence.selection_policy_version);
    std::vector<std::string> codes = evidence.explanation_codes;
    std::sort(codes.begin(), codes.end());
    writer.integer(codes.size());
    for (const std::string& code : codes)
        writer.text(code);
    append_evidence_value(writer, evidence.estimated_time_ratio);
    append_evidence_value(writer, evidence.material_ratio);
    append_evidence_value(writer, evidence.appearance_risk);
    append_evidence_value(writer, evidence.dimensional_risk);
    append_evidence_value(writer, evidence.strength_risk);
    append_evidence_value(writer, evidence.retained_strength_ratio);
    append_evidence_value(writer, evidence.reliability_risk);
    append_evidence_value(writer, evidence.protected_region_risk);
    return digest(writer.str());
}

std::string apply_candidate_payload_digest(const SliceCandidate& candidate)
{
    CanonicalWriter writer;
    writer.text(candidate.id);
    writer.integer(candidate.base_revision.model_revision);
    writer.integer(candidate.base_revision.config_revision);
    writer.integer(candidate.base_revision.plate_revision);
    writer.text(candidate.base_revision.fingerprint);
    writer.integer(candidate.goal);
    writer.integer(candidate.status);
    writer.text(candidate.parameters.policy_version);
    writer.integer(candidate.parameters.goal);

    std::vector<ObjectTransform> transforms = candidate.placement.transforms;
    std::stable_sort(transforms.begin(), transforms.end(), [](const ObjectTransform& lhs, const ObjectTransform& rhs) {
        return std::tie(lhs.object_id, lhs.instance_id) < std::tie(rhs.object_id, rhs.instance_id);
    });
    writer.integer(transforms.size());
    for (const ObjectTransform& transform : transforms) {
        writer.integer(transform.object_id);
        writer.integer(transform.instance_id);
        for (const double value : transform.matrix)
            writer.number(value);
    }

    std::vector<ConfigPatchEntry> entries = candidate.parameters.entries;
    std::stable_sort(entries.begin(), entries.end(), [](const ConfigPatchEntry& lhs, const ConfigPatchEntry& rhs) {
        return std::tie(lhs.scope, lhs.owner, lhs.target_id, lhs.key) <
               std::tie(rhs.scope, rhs.owner, rhs.target_id, rhs.key);
    });
    writer.integer(entries.size());
    for (const ConfigPatchEntry& entry : entries) {
        writer.integer(entry.scope);
        writer.integer(entry.owner);
        writer.integer(entry.target_id);
        writer.text(entry.key);
        append_config_value(writer, entry.expected_value);
        append_config_value(writer, entry.new_value);
        writer.text(entry.reason_code);
    }
    writer.boolean(candidate.repair.has_value());
    if (candidate.repair) {
        writer.boolean(candidate.repair->changes_geometry_semantics);
        writer.integer(candidate.repair->operation_codes.size());
        for (const std::string& operation : candidate.repair->operation_codes)
            writer.text(operation);
    }
    return digest(writer.str());
}

std::string apply_capability_evidence_digest(const MachineCapabilitySnapshot& machine,
                                             const MaterialCompatibilitySnapshot& materials)
{
    CanonicalWriter writer;
    writer.text(machine.registry_version);
    append_profile(writer, machine.profile);
    writer.integer(machine.support_status);
    std::vector<MachineCapabilityReason> machine_reasons = machine.reasons;
    std::sort(machine_reasons.begin(), machine_reasons.end());
    writer.integer(machine_reasons.size());
    for (const MachineCapabilityReason reason : machine_reasons)
        writer.integer(reason);

    append_evidence(writer, machine.evidence.independently_addressable);
    writer.integer(machine.evidence.nozzles.size());
    for (const NozzleCapabilitySnapshot& nozzle : machine.evidence.nozzles) {
        writer.integer(nozzle.physical_number);
        writer.number(nozzle.diameter_mm);
        append_evidence(writer, nozzle.offset);
        append_evidence(writer, nozzle.reachable_area);
    }
    append_evidence(writer, machine.evidence.collision_clearance);
    append_evidence(writer, machine.evidence.tool_change_gcode);
    append_evidence(writer, machine.evidence.preheat_behavior);
    append_evidence(writer, machine.evidence.standby_behavior);
    append_evidence(writer, machine.evidence.retraction_behavior);
    append_evidence(writer, machine.evidence.tool_change_time_model);
    append_evidence(writer, machine.evidence.prime_or_wipe);
    append_evidence(writer, machine.evidence.flush_matrix);
    append_evidence(writer, machine.evidence.wipe_tower_space_constraints);
    append_evidence(writer, machine.evidence.specialized_validation_complete);

    writer.text(materials.registry_version);
    writer.integer(materials.combination_status);
    writer.integer(materials.common_family);
    std::vector<MaterialCompatibilityReason> material_reasons = materials.reasons;
    std::sort(material_reasons.begin(), material_reasons.end());
    writer.integer(material_reasons.size());
    for (const MaterialCompatibilityReason reason : material_reasons)
        writer.integer(reason);

    // Material order follows the captured physical/logical slot order. Only policy lists and
    // reason vectors are semantic sets and are normalized before encoding.
    writer.integer(materials.materials.size());
    for (const MaterialCapabilitySnapshot& material : materials.materials) {
        append_profile(writer, material.profile);
        writer.integer(material.family);
        writer.integer(material.support_status);
        writer.boolean(material.speed_policy.may_increase_process_speed);
        writer.boolean(material.speed_policy.may_modify_max_volumetric_flow);
        writer.boolean(material.speed_policy.may_modify_material_temperature);
        writer.boolean(material.speed_policy.may_modify_machine_limits);

        std::vector<std::string> machine_ids = material.boundary_policy.allowed_machine_setting_ids;
        std::sort(machine_ids.begin(), machine_ids.end());
        writer.integer(machine_ids.size());
        for (const std::string& id : machine_ids)
            writer.text(id);
        std::vector<double> diameters = material.boundary_policy.allowed_nozzle_diameters_mm;
        std::sort(diameters.begin(), diameters.end());
        writer.integer(diameters.size());
        for (double diameter : diameters)
            writer.number(diameter);
        writer.text(material.boundary_policy.temperature_boundary_source);
        writer.text(material.boundary_policy.flow_boundary_source);
        writer.boolean(material.boundary_policy.validated);

        std::vector<MaterialCompatibilityReason> reasons = material.reasons;
        std::sort(reasons.begin(), reasons.end());
        writer.integer(reasons.size());
        for (const MaterialCompatibilityReason reason : reasons)
            writer.integer(reason);
    }
    return digest(writer.str());
}

std::string apply_machine_capability_evidence_digest(const MachineCapabilitySnapshot& machine)
{
    return apply_capability_evidence_digest(machine, {});
}

std::string apply_material_compatibility_evidence_digest(const MaterialCompatibilitySnapshot& materials)
{
    return apply_capability_evidence_digest({}, materials);
}

} // namespace Slic3r::AI::SmartSlicing
