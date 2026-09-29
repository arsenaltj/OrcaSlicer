#pragma once

#include "MachineCapabilitySnapshot.hpp"

#include <string>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

enum class MaterialFamily { Unknown, PLA, PETG, TPU };
enum class MaterialSupportStatus { Unsupported, Enabled };
enum class MaterialCombinationStatus { Unsupported, Compatible };

enum class MaterialCompatibilityReason {
    ProfileNotAllowlisted,
    ProfileFingerprintMismatch,
    RestrictedVariant,
    NoMaterialInUse,
    MixedMaterialFamilies,
};

struct MaterialSpeedPolicy
{
    bool may_increase_process_speed{false};
    bool may_modify_max_volumetric_flow{false};
    bool may_modify_material_temperature{false};
    bool may_modify_machine_limits{false};
};

struct MaterialBoundaryPolicy
{
    std::vector<std::string> allowed_machine_setting_ids;
    std::vector<double> allowed_nozzle_diameters_mm;
    std::string temperature_boundary_source;
    std::string flow_boundary_source;
    bool validated{false};
};

struct MaterialCapabilitySnapshot
{
    ProfileIdentity profile;
    MaterialFamily family{MaterialFamily::Unknown};
    MaterialSupportStatus support_status{MaterialSupportStatus::Unsupported};
    MaterialSpeedPolicy speed_policy;
    MaterialBoundaryPolicy boundary_policy;
    std::vector<MaterialCompatibilityReason> reasons;

    bool enabled() const { return support_status == MaterialSupportStatus::Enabled; }
};

struct MaterialCompatibilitySnapshot
{
    std::string registry_version;
    std::vector<MaterialCapabilitySnapshot> materials;
    MaterialCombinationStatus combination_status{MaterialCombinationStatus::Unsupported};
    MaterialFamily common_family{MaterialFamily::Unknown};
    std::vector<MaterialCompatibilityReason> reasons;

    bool compatible() const { return combination_status == MaterialCombinationStatus::Compatible; }
};

const char* material_compatibility_registry_version();
const char* material_family_name(MaterialFamily family);
const char* material_compatibility_reason_name(MaterialCompatibilityReason reason);
MaterialCapabilitySnapshot evaluate_material_profile(ProfileIdentity profile);
MaterialCompatibilitySnapshot evaluate_material_compatibility(std::vector<ProfileIdentity> used_profiles);
std::string capability_registry_revision_token(const MachineCapabilitySnapshot& machine,
                                               const MaterialCompatibilitySnapshot& materials);

} // namespace Slic3r::AI::SmartSlicing
