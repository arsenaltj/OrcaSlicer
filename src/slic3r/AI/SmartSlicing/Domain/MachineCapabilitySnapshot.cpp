#include "MachineCapabilitySnapshot.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace Slic3r::AI::SmartSlicing {
namespace {

struct MachineRegistryEntry
{
    const char* setting_id;
    const char* parent;
    const char* fingerprint;
};

constexpr std::array<MachineRegistryEntry, 2> MACHINE_REGISTRY{{
    {"xJBdCljSZVDINXnP", "fdm_klipper_common", "bea5e153635e3e0dc884f9d21ad604d21d8efaf18ff016ec814331b7387330e7"},
    {"8xoZ6vYv9ws0J97u", "fdm_klipper_common", "32a2d25a673c1f54fb27e85ba925354e31ad0593c83e62269a89091c9fa6cb81"},
}};

const MachineRegistryEntry* find_registry_entry(const ProfileIdentity& profile)
{
    const auto found = std::find_if(MACHINE_REGISTRY.begin(), MACHINE_REGISTRY.end(), [&](const MachineRegistryEntry& entry) {
        return profile.setting_id == entry.setting_id &&
               profile.inheritance_chain == std::vector<std::string>{entry.parent};
    });
    return found == MACHINE_REGISTRY.end() ? nullptr : &*found;
}

void add_reason(MachineCapabilitySnapshot& snapshot, MachineCapabilityReason reason)
{
    snapshot.reasons.push_back(reason);
}

template<class T, class Predicate>
bool has_valid_evidence(const EvidenceValue<T>& evidence, Predicate predicate)
{
    return evidence.availability == EvidenceAvailability::Available && evidence.value.has_value() &&
           !evidence.source_code.empty() && predicate(*evidence.value);
}

bool finite_nonnegative(double value) { return std::isfinite(value) && value >= 0.0; }

} // namespace

const char* machine_capability_registry_version() { return "wondermaker-capabilities-v1-20260928"; }

const char* machine_capability_reason_name(MachineCapabilityReason reason)
{
    switch (reason) {
    case MachineCapabilityReason::ProfileNotAllowlisted: return "profile_not_allowlisted";
    case MachineCapabilityReason::ProfileFingerprintMismatch: return "profile_fingerprint_mismatch";
    case MachineCapabilityReason::UnsupportedNozzleCount: return "unsupported_nozzle_count";
    case MachineCapabilityReason::SixNozzlePendingValidation: return "six_nozzle_pending_validation";
    case MachineCapabilityReason::UnsupportedNozzleDiameter: return "unsupported_nozzle_diameter";
    case MachineCapabilityReason::InvalidPhysicalNumbering: return "invalid_physical_numbering";
    case MachineCapabilityReason::MissingIndependentAddressing: return "missing_independent_addressing";
    case MachineCapabilityReason::MissingNozzleOffset: return "missing_nozzle_offset";
    case MachineCapabilityReason::MissingReachableArea: return "missing_reachable_area";
    case MachineCapabilityReason::MissingCollisionClearance: return "missing_collision_clearance";
    case MachineCapabilityReason::MissingToolChangeGcode: return "missing_tool_change_gcode";
    case MachineCapabilityReason::MissingPreheatBehavior: return "missing_preheat_behavior";
    case MachineCapabilityReason::MissingStandbyBehavior: return "missing_standby_behavior";
    case MachineCapabilityReason::MissingRetractionBehavior: return "missing_retraction_behavior";
    case MachineCapabilityReason::MissingToolChangeTimeModel: return "missing_tool_change_time_model";
    case MachineCapabilityReason::MissingPrimeOrWipeCapability: return "missing_prime_or_wipe_capability";
    case MachineCapabilityReason::MissingFlushMatrix: return "missing_flush_matrix";
    case MachineCapabilityReason::MissingWipeTowerSpaceConstraints: return "missing_wipe_tower_space_constraints";
    case MachineCapabilityReason::SpecializedValidationIncomplete: return "specialized_validation_incomplete";
    }
    return "unknown_machine_capability_reason";
}

MachineCapabilitySnapshot evaluate_machine_capability(ProfileIdentity profile, MachineCapabilityEvidence evidence)
{
    MachineCapabilitySnapshot snapshot;
    snapshot.registry_version = machine_capability_registry_version();
    snapshot.profile = std::move(profile);
    snapshot.evidence = std::move(evidence);

    const MachineRegistryEntry* entry = find_registry_entry(snapshot.profile);
    if (entry == nullptr) {
        add_reason(snapshot, MachineCapabilityReason::ProfileNotAllowlisted);
        if (snapshot.evidence.nozzles.size() == 6) {
            snapshot.support_status = MachineSupportStatus::PendingValidation;
            add_reason(snapshot, MachineCapabilityReason::SixNozzlePendingValidation);
        } else {
            snapshot.support_status = MachineSupportStatus::Disabled;
        }
        return snapshot;
    }
    if (snapshot.profile.fingerprint != entry->fingerprint)
        add_reason(snapshot, MachineCapabilityReason::ProfileFingerprintMismatch);

    const size_t nozzle_count = snapshot.evidence.nozzles.size();
    if (nozzle_count == 6)
        add_reason(snapshot, MachineCapabilityReason::SixNozzlePendingValidation);
    else if (nozzle_count != 4)
        add_reason(snapshot, MachineCapabilityReason::UnsupportedNozzleCount);

    if (std::any_of(snapshot.evidence.nozzles.begin(), snapshot.evidence.nozzles.end(),
                    [](const NozzleCapabilitySnapshot& nozzle) { return std::abs(nozzle.diameter_mm - 0.4) > 1e-9; }))
        add_reason(snapshot, MachineCapabilityReason::UnsupportedNozzleDiameter);
    std::vector<int> physical_numbers;
    physical_numbers.reserve(snapshot.evidence.nozzles.size());
    for (const NozzleCapabilitySnapshot& nozzle : snapshot.evidence.nozzles)
        physical_numbers.push_back(nozzle.physical_number);
    std::sort(physical_numbers.begin(), physical_numbers.end());
    for (size_t index = 0; index < physical_numbers.size(); ++index) {
        if (physical_numbers[index] != static_cast<int>(index + 1)) {
            add_reason(snapshot, MachineCapabilityReason::InvalidPhysicalNumbering);
            break;
        }
    }
    if (!has_valid_evidence(snapshot.evidence.independently_addressable, [](bool value) { return value; }))
        add_reason(snapshot, MachineCapabilityReason::MissingIndependentAddressing);
    if (std::any_of(snapshot.evidence.nozzles.begin(), snapshot.evidence.nozzles.end(),
                    [](const NozzleCapabilitySnapshot& nozzle) {
                        return !has_valid_evidence(nozzle.offset, [](const NozzleOffset& value) {
                            return std::isfinite(value.x_mm) && std::isfinite(value.y_mm);
                        });
                    }))
        add_reason(snapshot, MachineCapabilityReason::MissingNozzleOffset);
    if (std::any_of(snapshot.evidence.nozzles.begin(), snapshot.evidence.nozzles.end(),
                    [](const NozzleCapabilitySnapshot& nozzle) {
                        return !has_valid_evidence(nozzle.reachable_area, [](const ReachableArea& value) {
                            return std::isfinite(value.minimum_x_mm) && std::isfinite(value.maximum_x_mm) &&
                                   std::isfinite(value.minimum_y_mm) && std::isfinite(value.maximum_y_mm) &&
                                   value.minimum_x_mm < value.maximum_x_mm && value.minimum_y_mm < value.maximum_y_mm;
                        });
                    }))
        add_reason(snapshot, MachineCapabilityReason::MissingReachableArea);
    if (!has_valid_evidence(snapshot.evidence.collision_clearance, [](const CollisionClearanceLimits& value) {
            return value.nozzle_radius_mm > 0.0 && value.carriage_radius_mm >= value.nozzle_radius_mm &&
                   value.vertical_clearance_mm > 0.0 && std::isfinite(value.nozzle_radius_mm) &&
                   std::isfinite(value.carriage_radius_mm) && std::isfinite(value.vertical_clearance_mm);
        }))
        add_reason(snapshot, MachineCapabilityReason::MissingCollisionClearance);
    if (!has_valid_evidence(snapshot.evidence.tool_change_gcode, [](const ToolChangeGcodeCapability& value) {
            return !value.command_template.empty() && value.supports_target_tool;
        }))
        add_reason(snapshot, MachineCapabilityReason::MissingToolChangeGcode);
    if (!has_valid_evidence(snapshot.evidence.preheat_behavior, [](const PreheatBehavior& value) {
            return finite_nonnegative(value.lead_time_seconds) && value.minimum_ready_temperature_c > 0.0 &&
                   std::isfinite(value.minimum_ready_temperature_c);
        }))
        add_reason(snapshot, MachineCapabilityReason::MissingPreheatBehavior);
    if (!has_valid_evidence(snapshot.evidence.standby_behavior, [](const StandbyBehavior& value) {
            return finite_nonnegative(value.standby_temperature_c) &&
                   finite_nonnegative(value.resume_temperature_tolerance_c);
        }))
        add_reason(snapshot, MachineCapabilityReason::MissingStandbyBehavior);
    if (!has_valid_evidence(snapshot.evidence.retraction_behavior, [](const RetractionBehavior& value) {
            return finite_nonnegative(value.length_mm) && finite_nonnegative(value.speed_mm_per_second);
        }))
        add_reason(snapshot, MachineCapabilityReason::MissingRetractionBehavior);
    if (!has_valid_evidence(snapshot.evidence.tool_change_time_model, [](const ToolChangeTimeModel& value) {
            return finite_nonnegative(value.fixed_seconds) && finite_nonnegative(value.thermal_recovery_seconds);
        }))
        add_reason(snapshot, MachineCapabilityReason::MissingToolChangeTimeModel);
    if (!has_valid_evidence(snapshot.evidence.prime_or_wipe, [](const PrimeOrWipeCapability& value) {
            return value.prime_tower_supported || value.precharge_supported;
        }))
        add_reason(snapshot, MachineCapabilityReason::MissingPrimeOrWipeCapability);
    if (!has_valid_evidence(snapshot.evidence.flush_matrix, [nozzle_count](const FlushMatrix& value) {
            return value.tool_count == nozzle_count && value.volume_mm3.size() == nozzle_count * nozzle_count &&
                   std::all_of(value.volume_mm3.begin(), value.volume_mm3.end(), finite_nonnegative);
        }))
        add_reason(snapshot, MachineCapabilityReason::MissingFlushMatrix);
    if (!has_valid_evidence(snapshot.evidence.wipe_tower_space_constraints,
                            [](const WipeTowerSpaceConstraints& value) {
                                return value.minimum_width_mm > 0.0 && value.minimum_depth_mm > 0.0 &&
                                       finite_nonnegative(value.clearance_mm) && std::isfinite(value.minimum_width_mm) &&
                                       std::isfinite(value.minimum_depth_mm);
                            }))
        add_reason(snapshot, MachineCapabilityReason::MissingWipeTowerSpaceConstraints);
    if (!has_valid_evidence(snapshot.evidence.specialized_validation_complete, [](bool value) { return value; }))
        add_reason(snapshot, MachineCapabilityReason::SpecializedValidationIncomplete);

    snapshot.support_status = snapshot.reasons.empty() ? MachineSupportStatus::Enabled :
                                                         MachineSupportStatus::PendingValidation;
    return snapshot;
}

} // namespace Slic3r::AI::SmartSlicing
