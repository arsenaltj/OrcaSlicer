#include "SixNozzleValidation.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <locale>
#include <limits>
#include <sstream>
#include <set>
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

class CanonicalWriter
{
public:
    CanonicalWriter() { stream.imbue(std::locale::classic()); }

    void text(const std::string& value) { stream << value.size() << ':' << value << ';'; }
    void integer(long long value) { stream << value << ';'; }
    void boolean(bool value) { stream << (value ? 1 : 0) << ';'; }
    void number(double value)
    {
        stream << std::setprecision(std::numeric_limits<double>::max_digits10) << value << ';';
    }
    std::string str() const { return stream.str(); }

private:
    std::ostringstream stream;
};

template<class T>
bool available(const EvidenceValue<T>& evidence)
{
    return evidence.availability == EvidenceAvailability::Available && evidence.value.has_value() &&
           !evidence.source_code.empty();
}

bool finite_nonnegative(double value)
{
    return std::isfinite(value) && value >= 0.0;
}

bool valid_offset(const NozzleOffset& value)
{
    return std::isfinite(value.x_mm) && std::isfinite(value.y_mm);
}

bool valid_reachable_area(const ReachableArea& value)
{
    return std::isfinite(value.minimum_x_mm) && std::isfinite(value.maximum_x_mm) &&
           std::isfinite(value.minimum_y_mm) && std::isfinite(value.maximum_y_mm) &&
           value.minimum_x_mm < value.maximum_x_mm && value.minimum_y_mm < value.maximum_y_mm;
}

template<class T>
void write_evidence(CanonicalWriter& writer, const EvidenceValue<T>& evidence)
{
    writer.integer(static_cast<long long>(evidence.availability));
    writer.text(evidence.source_code);
    if constexpr (std::is_same_v<T, bool>) {
        writer.boolean(evidence.value.value_or(false));
    } else if constexpr (std::is_same_v<T, std::string>) {
        writer.text(evidence.value.value_or(std::string{}));
    }
}

std::string digest(const std::string& canonical)
{
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16) << fnv1a64(canonical);
    return stream.str();
}

void add_unique(std::vector<std::string>& diagnostics, std::string code)
{
    diagnostics.push_back(std::move(code));
}

bool has_machine_reason(const MachineCapabilitySnapshot& machine, MachineCapabilityReason reason)
{
    return std::find(machine.reasons.begin(), machine.reasons.end(), reason) != machine.reasons.end();
}

} // namespace

const char* six_nozzle_validation_status_name(SixNozzleValidationStatus status)
{
    switch (status) {
    case SixNozzleValidationStatus::PendingValidation: return "pending_validation";
    case SixNozzleValidationStatus::Enabled: return "enabled";
    case SixNozzleValidationStatus::Rejected: return "rejected";
    }
    return "rejected";
}

std::string canonical_six_nozzle_fingerprint(const SixNozzleValidationInput& input)
{
    CanonicalWriter writer;
    writer.text(input.schema);
    writer.text(input.version);
    writer.text(input.policy_version);
    writer.text(input.machine.registry_version);
    writer.text(input.machine.profile.setting_id);
    writer.text(input.machine.profile.fingerprint);
    for (const std::string& parent : input.machine.profile.inheritance_chain)
        writer.text(parent);
    writer.integer(static_cast<long long>(input.machine.evidence.nozzles.size()));
    auto nozzles = input.machine.evidence.nozzles;
    std::sort(nozzles.begin(), nozzles.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.physical_number < rhs.physical_number;
    });
    for (const auto& nozzle : nozzles) {
        writer.integer(nozzle.physical_number);
        writer.number(nozzle.diameter_mm);
        write_evidence(writer, nozzle.offset);
        if (nozzle.offset.value) {
            writer.number(nozzle.offset.value->x_mm);
            writer.number(nozzle.offset.value->y_mm);
        }
        write_evidence(writer, nozzle.reachable_area);
        if (nozzle.reachable_area.value) {
            writer.number(nozzle.reachable_area.value->minimum_x_mm);
            writer.number(nozzle.reachable_area.value->maximum_x_mm);
            writer.number(nozzle.reachable_area.value->minimum_y_mm);
            writer.number(nozzle.reachable_area.value->maximum_y_mm);
        }
    }
    writer.boolean(input.machine.evidence.independently_addressable.value.value_or(false));
    write_evidence(writer, input.machine.evidence.independently_addressable);
    write_evidence(writer, input.machine.evidence.collision_clearance);
    if (input.machine.evidence.collision_clearance.value) {
        const auto& value = *input.machine.evidence.collision_clearance.value;
        writer.number(value.nozzle_radius_mm);
        writer.number(value.carriage_radius_mm);
        writer.number(value.vertical_clearance_mm);
    }
    write_evidence(writer, input.machine.evidence.tool_change_gcode);
    if (input.machine.evidence.tool_change_gcode.value) {
        writer.text(input.machine.evidence.tool_change_gcode.value->command_template);
        writer.boolean(input.machine.evidence.tool_change_gcode.value->supports_target_tool);
    }
    write_evidence(writer, input.machine.evidence.preheat_behavior);
    if (input.machine.evidence.preheat_behavior.value) {
        writer.number(input.machine.evidence.preheat_behavior.value->lead_time_seconds);
        writer.number(input.machine.evidence.preheat_behavior.value->minimum_ready_temperature_c);
    }
    write_evidence(writer, input.machine.evidence.standby_behavior);
    if (input.machine.evidence.standby_behavior.value) {
        writer.number(input.machine.evidence.standby_behavior.value->standby_temperature_c);
        writer.number(input.machine.evidence.standby_behavior.value->resume_temperature_tolerance_c);
    }
    write_evidence(writer, input.machine.evidence.retraction_behavior);
    if (input.machine.evidence.retraction_behavior.value) {
        writer.number(input.machine.evidence.retraction_behavior.value->length_mm);
        writer.number(input.machine.evidence.retraction_behavior.value->speed_mm_per_second);
    }
    write_evidence(writer, input.machine.evidence.tool_change_time_model);
    if (input.machine.evidence.tool_change_time_model.value) {
        writer.number(input.machine.evidence.tool_change_time_model.value->fixed_seconds);
        writer.number(input.machine.evidence.tool_change_time_model.value->thermal_recovery_seconds);
    }
    write_evidence(writer, input.machine.evidence.prime_or_wipe);
    if (input.machine.evidence.prime_or_wipe.value) {
        writer.boolean(input.machine.evidence.prime_or_wipe.value->prime_tower_supported);
        writer.boolean(input.machine.evidence.prime_or_wipe.value->precharge_supported);
    }
    write_evidence(writer, input.machine.evidence.specialized_validation_complete);
    writer.text(input.materials.registry_version);
    auto materials = input.materials.materials;
    std::sort(materials.begin(), materials.end(), [](const auto& lhs, const auto& rhs) {
        return std::tie(lhs.profile.setting_id, lhs.profile.fingerprint) <
               std::tie(rhs.profile.setting_id, rhs.profile.fingerprint);
    });
    for (const auto& material : materials) {
        writer.text(material.profile.setting_id);
        writer.text(material.profile.fingerprint);
        writer.integer(static_cast<long long>(material.family));
        writer.integer(static_cast<long long>(material.support_status));
    }
    write_evidence(writer, input.machine.evidence.flush_matrix);
    if (input.machine.evidence.flush_matrix.value) {
        writer.integer(static_cast<long long>(input.machine.evidence.flush_matrix.value->tool_count));
        for (double value : input.machine.evidence.flush_matrix.value->volume_mm3)
            writer.number(value);
    }
    write_evidence(writer, input.machine.evidence.wipe_tower_space_constraints);
    if (input.machine.evidence.wipe_tower_space_constraints.value) {
        const auto& value = *input.machine.evidence.wipe_tower_space_constraints.value;
        writer.number(value.minimum_width_mm);
        writer.number(value.minimum_depth_mm);
        writer.number(value.clearance_mm);
    }
    auto mappings = input.slot_mappings;
    std::sort(mappings.begin(), mappings.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.physical_number < rhs.physical_number;
    });
    for (const auto& mapping : mappings) {
        writer.integer(mapping.physical_number);
        write_evidence(writer, mapping.material_id);
        write_evidence(writer, mapping.color_id);
        if (mapping.material_id.value)
            writer.text(*mapping.material_id.value);
        if (mapping.color_id.value)
            writer.text(*mapping.color_id.value);
    }
    write_evidence(writer, input.persistence.save_restore_round_trip);
    return digest(writer.str());
}

SixNozzleValidationResult validate_six_nozzle(const SixNozzleValidationInput& input)
{
    SixNozzleValidationResult result;
    result.schema = input.schema;
    result.version = input.version;
    result.policy_version = input.policy_version;
    result.canonical_fingerprint = canonical_six_nozzle_fingerprint(input);

    const bool contract_supported = input.schema == SIX_NOZZLE_VALIDATION_SCHEMA &&
                                    input.version == SIX_NOZZLE_VALIDATION_VERSION &&
                                    input.policy_version == SIX_NOZZLE_VALIDATION_POLICY_VERSION;
    if (!contract_supported) {
        result.diagnostic_codes.emplace_back("unsupported_six_nozzle_schema_or_policy");
        result.status = SixNozzleValidationStatus::Rejected;
        return result;
    }

    bool pending = false;
    bool rejected = false;
    if (input.machine.support_status == MachineSupportStatus::Disabled) {
        result.diagnostic_codes.emplace_back("machine_capability_disabled");
        rejected = true;
    }
    if (input.machine.registry_version != machine_capability_registry_version()) {
        result.diagnostic_codes.emplace_back("unsupported_machine_registry_version");
        rejected = true;
    }
    if (input.machine.profile.setting_id.empty() || input.machine.profile.fingerprint.empty()) {
        result.diagnostic_codes.emplace_back("machine_profile_identity_missing");
        pending = true;
    }
    if (has_machine_reason(input.machine, MachineCapabilityReason::ProfileNotAllowlisted)) {
        result.diagnostic_codes.emplace_back("machine_profile_not_allowlisted");
        pending = true;
    }
    if (has_machine_reason(input.machine, MachineCapabilityReason::ProfileFingerprintMismatch)) {
        result.diagnostic_codes.emplace_back("machine_profile_fingerprint_mismatch");
        rejected = true;
    }
    if (input.machine.evidence.nozzles.size() != SIX_NOZZLE_COUNT) {
        result.diagnostic_codes.emplace_back("six_nozzle_count_invalid");
        rejected = true;
    }
    std::vector<int> numbers;
    for (const auto& nozzle : input.machine.evidence.nozzles) {
        numbers.push_back(nozzle.physical_number);
        if (std::abs(nozzle.diameter_mm - 0.4) > 1e-9 || !std::isfinite(nozzle.diameter_mm)) {
            result.diagnostic_codes.emplace_back("six_nozzle_diameter_invalid");
            rejected = true;
        }
    }
    std::sort(numbers.begin(), numbers.end());
    for (size_t index = 0; index < numbers.size(); ++index) {
        if (numbers[index] != static_cast<int>(index + 1)) {
            result.diagnostic_codes.emplace_back("six_nozzle_physical_numbering_invalid");
            rejected = true;
            break;
        }
    }
    if (!available(input.machine.evidence.independently_addressable) ||
        !*input.machine.evidence.independently_addressable.value) {
        result.diagnostic_codes.emplace_back("six_nozzle_independent_addressing_missing");
        pending = true;
    }
    if (std::any_of(input.machine.evidence.nozzles.begin(), input.machine.evidence.nozzles.end(), [](const auto& nozzle) {
            return !available(nozzle.offset) || !available(nozzle.reachable_area) ||
                   !valid_offset(*nozzle.offset.value) || !valid_reachable_area(*nozzle.reachable_area.value);
        })) {
        result.diagnostic_codes.emplace_back("six_nozzle_geometry_evidence_missing");
        pending = true;
    }
    if (!available(input.machine.evidence.collision_clearance) ||
        !std::isfinite(input.machine.evidence.collision_clearance.value->nozzle_radius_mm) ||
        !std::isfinite(input.machine.evidence.collision_clearance.value->carriage_radius_mm) ||
        !std::isfinite(input.machine.evidence.collision_clearance.value->vertical_clearance_mm) ||
        input.machine.evidence.collision_clearance.value->nozzle_radius_mm <= 0.0 ||
        input.machine.evidence.collision_clearance.value->carriage_radius_mm <
            input.machine.evidence.collision_clearance.value->nozzle_radius_mm ||
        input.machine.evidence.collision_clearance.value->vertical_clearance_mm <= 0.0) {
        result.diagnostic_codes.emplace_back("six_nozzle_collision_clearance_missing");
        pending = true;
    }
    if (!available(input.machine.evidence.tool_change_gcode) ||
        !input.machine.evidence.tool_change_gcode.value->supports_target_tool) {
        result.diagnostic_codes.emplace_back("six_nozzle_tool_change_gcode_missing");
        pending = true;
    }
    if (!available(input.machine.evidence.preheat_behavior) ||
        !finite_nonnegative(input.machine.evidence.preheat_behavior.value->lead_time_seconds) ||
        !std::isfinite(input.machine.evidence.preheat_behavior.value->minimum_ready_temperature_c) ||
        input.machine.evidence.preheat_behavior.value->minimum_ready_temperature_c <= 0.0 ||
        !available(input.machine.evidence.standby_behavior) ||
        !finite_nonnegative(input.machine.evidence.standby_behavior.value->standby_temperature_c) ||
        !finite_nonnegative(input.machine.evidence.standby_behavior.value->resume_temperature_tolerance_c) ||
        !available(input.machine.evidence.retraction_behavior) ||
        !finite_nonnegative(input.machine.evidence.retraction_behavior.value->length_mm) ||
        !finite_nonnegative(input.machine.evidence.retraction_behavior.value->speed_mm_per_second) ||
        !available(input.machine.evidence.tool_change_time_model) ||
        !finite_nonnegative(input.machine.evidence.tool_change_time_model.value->fixed_seconds) ||
        !finite_nonnegative(input.machine.evidence.tool_change_time_model.value->thermal_recovery_seconds)) {
        result.diagnostic_codes.emplace_back("six_nozzle_thermal_or_retraction_evidence_missing");
        pending = true;
    }
    if (!available(input.machine.evidence.prime_or_wipe)) {
        result.diagnostic_codes.emplace_back("six_nozzle_prime_wipe_evidence_missing");
        pending = true;
    }
    if (!available(input.machine.evidence.flush_matrix) ||
        input.machine.evidence.flush_matrix.value->tool_count != SIX_NOZZLE_COUNT ||
        input.machine.evidence.flush_matrix.value->volume_mm3.size() != SIX_NOZZLE_COUNT * SIX_NOZZLE_COUNT ||
        std::any_of(input.machine.evidence.flush_matrix.value->volume_mm3.begin(),
                    input.machine.evidence.flush_matrix.value->volume_mm3.end(),
                    [](double value) { return !finite_nonnegative(value); })) {
        result.diagnostic_codes.emplace_back("six_nozzle_flush_matrix_missing_or_invalid");
        pending = true;
    }
    if (!available(input.machine.evidence.wipe_tower_space_constraints) ||
        !std::isfinite(input.machine.evidence.wipe_tower_space_constraints.value->minimum_width_mm) ||
        !std::isfinite(input.machine.evidence.wipe_tower_space_constraints.value->minimum_depth_mm) ||
        !finite_nonnegative(input.machine.evidence.wipe_tower_space_constraints.value->clearance_mm) ||
        input.machine.evidence.wipe_tower_space_constraints.value->minimum_width_mm <= 0.0 ||
        input.machine.evidence.wipe_tower_space_constraints.value->minimum_depth_mm <= 0.0) {
        result.diagnostic_codes.emplace_back("six_nozzle_wipe_tower_constraints_missing");
        pending = true;
    }
    if (!available(input.machine.evidence.specialized_validation_complete) ||
        !*input.machine.evidence.specialized_validation_complete.value) {
        result.diagnostic_codes.emplace_back("six_nozzle_specialized_validation_incomplete");
        pending = true;
    }
    if (input.materials.registry_version != material_compatibility_registry_version()) {
        result.diagnostic_codes.emplace_back("unsupported_material_registry_version");
        rejected = true;
    }
    if (!input.materials.compatible() || input.materials.materials.empty() || !input.materials.reasons.empty()) {
        result.diagnostic_codes.emplace_back("six_nozzle_material_compatibility_missing");
        pending = true;
    }
    if (input.slot_mappings.size() != SIX_NOZZLE_COUNT) {
        result.diagnostic_codes.emplace_back("six_nozzle_slot_mapping_count_invalid");
        rejected = true;
    }
    std::vector<int> mapping_numbers;
    std::set<std::string> known_material_ids;
    for (const auto& material : input.materials.materials)
        known_material_ids.insert(material.profile.setting_id);
    for (const auto& mapping : input.slot_mappings) {
        mapping_numbers.push_back(mapping.physical_number);
        if (!available(mapping.material_id) || !available(mapping.color_id)) {
            result.diagnostic_codes.emplace_back("six_nozzle_slot_mapping_evidence_missing");
            pending = true;
        } else {
            if (known_material_ids.find(*mapping.material_id.value) == known_material_ids.end()) {
                result.diagnostic_codes.emplace_back("six_nozzle_slot_mapping_conflict");
                rejected = true;
            }
        }
    }
    std::sort(mapping_numbers.begin(), mapping_numbers.end());
    if (std::adjacent_find(mapping_numbers.begin(), mapping_numbers.end()) != mapping_numbers.end()) {
        result.diagnostic_codes.emplace_back("six_nozzle_slot_mapping_duplicate");
        rejected = true;
    }
    for (size_t index = 0; index < mapping_numbers.size(); ++index) {
        if (mapping_numbers[index] != static_cast<int>(index + 1)) {
            result.diagnostic_codes.emplace_back("six_nozzle_slot_mapping_conflict");
            rejected = true;
            break;
        }
    }
    if (!available(input.persistence.save_restore_round_trip) ||
        !*input.persistence.save_restore_round_trip.value || !available(input.persistence.restored_fingerprint)) {
        result.diagnostic_codes.emplace_back("six_nozzle_save_restore_evidence_missing");
        pending = true;
    } else if (*input.persistence.restored_fingerprint.value != result.canonical_fingerprint) {
        result.diagnostic_codes.emplace_back("six_nozzle_save_restore_fingerprint_mismatch");
        rejected = true;
    }

    std::sort(result.diagnostic_codes.begin(), result.diagnostic_codes.end());
    result.diagnostic_codes.erase(std::unique(result.diagnostic_codes.begin(), result.diagnostic_codes.end()),
                                  result.diagnostic_codes.end());
    if (rejected)
        result.status = SixNozzleValidationStatus::Rejected;
    else if (pending)
        result.status = SixNozzleValidationStatus::PendingValidation;
    else
        result.status = SixNozzleValidationStatus::Enabled;
    return result;
}

SixNozzleValidationResult production_six_nozzle_validation()
{
    SixNozzleValidationResult result;
    result.diagnostic_codes.emplace_back("six_nozzle_profile_pending_validation");
    result.status = SixNozzleValidationStatus::PendingValidation;
    return result;
}

} // namespace Slic3r::AI::SmartSlicing
