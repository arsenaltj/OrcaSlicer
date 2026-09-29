#pragma once

#include "RecommendationTypes.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

struct ProfileIdentity
{
    std::string setting_id;
    std::vector<std::string> inheritance_chain;
    std::string fingerprint;
};

enum class MachineSupportStatus { Disabled, PendingValidation, Enabled };

enum class MachineCapabilityReason {
    ProfileNotAllowlisted,
    ProfileFingerprintMismatch,
    UnsupportedNozzleCount,
    SixNozzlePendingValidation,
    UnsupportedNozzleDiameter,
    InvalidPhysicalNumbering,
    MissingIndependentAddressing,
    MissingNozzleOffset,
    MissingReachableArea,
    MissingCollisionClearance,
    MissingToolChangeGcode,
    MissingPreheatBehavior,
    MissingStandbyBehavior,
    MissingRetractionBehavior,
    MissingToolChangeTimeModel,
    MissingPrimeOrWipeCapability,
    MissingFlushMatrix,
    MissingWipeTowerSpaceConstraints,
    SpecializedValidationIncomplete,
};

struct NozzleOffset
{
    double x_mm{0.0};
    double y_mm{0.0};
};

struct ReachableArea
{
    double minimum_x_mm{0.0};
    double maximum_x_mm{0.0};
    double minimum_y_mm{0.0};
    double maximum_y_mm{0.0};
};

struct CollisionClearanceLimits
{
    double nozzle_radius_mm{0.0};
    double carriage_radius_mm{0.0};
    double vertical_clearance_mm{0.0};
};

struct ToolChangeGcodeCapability
{
    std::string command_template;
    bool supports_target_tool{false};
};

struct PreheatBehavior
{
    double lead_time_seconds{0.0};
    double minimum_ready_temperature_c{0.0};
};

struct StandbyBehavior
{
    double standby_temperature_c{0.0};
    double resume_temperature_tolerance_c{0.0};
};

struct RetractionBehavior
{
    double length_mm{0.0};
    double speed_mm_per_second{0.0};
};

struct ToolChangeTimeModel
{
    double fixed_seconds{0.0};
    double thermal_recovery_seconds{0.0};
};

struct PrimeOrWipeCapability
{
    bool prime_tower_supported{false};
    bool precharge_supported{false};
};

struct FlushMatrix
{
    size_t tool_count{0};
    std::vector<double> volume_mm3;
};

struct WipeTowerSpaceConstraints
{
    double minimum_width_mm{0.0};
    double minimum_depth_mm{0.0};
    double clearance_mm{0.0};
};

struct NozzleCapabilitySnapshot
{
    int physical_number{0};
    double diameter_mm{0.0};
    EvidenceValue<NozzleOffset> offset;
    EvidenceValue<ReachableArea> reachable_area;
};

struct MachineCapabilityEvidence
{
    EvidenceValue<bool> independently_addressable;
    std::vector<NozzleCapabilitySnapshot> nozzles;
    EvidenceValue<CollisionClearanceLimits> collision_clearance;
    EvidenceValue<ToolChangeGcodeCapability> tool_change_gcode;
    EvidenceValue<PreheatBehavior> preheat_behavior;
    EvidenceValue<StandbyBehavior> standby_behavior;
    EvidenceValue<RetractionBehavior> retraction_behavior;
    EvidenceValue<ToolChangeTimeModel> tool_change_time_model;
    EvidenceValue<PrimeOrWipeCapability> prime_or_wipe;
    EvidenceValue<FlushMatrix> flush_matrix;
    EvidenceValue<WipeTowerSpaceConstraints> wipe_tower_space_constraints;
    EvidenceValue<bool> specialized_validation_complete;
};

struct MachineCapabilitySnapshot
{
    std::string registry_version;
    ProfileIdentity profile;
    MachineCapabilityEvidence evidence;
    MachineSupportStatus support_status{MachineSupportStatus::PendingValidation};
    std::vector<MachineCapabilityReason> reasons;

    bool enabled() const { return support_status == MachineSupportStatus::Enabled; }
};

const char* machine_capability_registry_version();
const char* machine_capability_reason_name(MachineCapabilityReason reason);
MachineCapabilitySnapshot evaluate_machine_capability(ProfileIdentity profile,
                                                       MachineCapabilityEvidence evidence);

} // namespace Slic3r::AI::SmartSlicing
