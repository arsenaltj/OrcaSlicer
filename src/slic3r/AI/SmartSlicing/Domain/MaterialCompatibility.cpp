#include "MaterialCompatibility.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace Slic3r::AI::SmartSlicing {
namespace {

struct MaterialRegistryEntry
{
    const char* setting_id;
    const char* parent;
    const char* fingerprint;
    MaterialFamily family;
    bool restricted;
};

constexpr std::array<MaterialRegistryEntry, 8> MATERIAL_REGISTRY{{
    {"HRFI81hKAaT1LwjF", "fdm_filament_pla", "9752c34b03a95c963f9085d658d904a5b9f5b7d95fbe5e3c2491c3ef1bda0cb1", MaterialFamily::PLA, false},
    {"ZtPZlMNIqq3Gaavc", "fdm_filament_pet", "ad769ce90f0ef80a2bda3b5967bf053f4759c03a2c4fd90ffe4232727c6b71c2", MaterialFamily::PETG, false},
    {"eUyRprcb22IdYNv3", "fdm_filament_tpu", "1ff207fb8ec4e396ca5c2b749bad6877ff1ba71c577795be859d2a4b3697ad82", MaterialFamily::TPU, false},
    {"Rnm03E1ifi5A2vrQ", "fdm_filament_pet", "0ee35c31725dbfc30208717ad755ca06d622f4c9e0664d0c35cad292aa4e774c", MaterialFamily::Unknown, true},
    {"4B7gD5YwPbUCvLAh", "fdm_filament_pla", "eba50b6924031a426c9b3b3f7ab3d989122db10489d42086c2d07ef5d546a3ee", MaterialFamily::PLA, true},
    {"eudiDwK2EVLlGnha", "fdm_filament_pla", "b0cfb5d937a77e2a91a7e7e5b2c8ac5c7093d928ef3f58cd984d1ccbc2a6bad7", MaterialFamily::PLA, true},
    {"8kSWEkCpitdwX11c", "fdm_filament_pla", "fe209511f86b563277ed93e873597f566480e84dbd38d23e09f64049a51f6f01", MaterialFamily::PLA, true},
    {"PRdEoagTzltjzJCQ", "fdm_filament_pva", "d067cb79dfb6beb6b24f3f92b424e84fe714d70ad30b6718077662d7f417090f", MaterialFamily::Unknown, true},
}};

const MaterialRegistryEntry* find_registry_entry(const ProfileIdentity& profile)
{
    const auto found = std::find_if(MATERIAL_REGISTRY.begin(), MATERIAL_REGISTRY.end(), [&](const MaterialRegistryEntry& entry) {
        return profile.setting_id == entry.setting_id &&
               profile.inheritance_chain == std::vector<std::string>{entry.parent};
    });
    return found == MATERIAL_REGISTRY.end() ? nullptr : &*found;
}

void add_unique_reason(std::vector<MaterialCompatibilityReason>& reasons, MaterialCompatibilityReason reason)
{
    if (std::find(reasons.begin(), reasons.end(), reason) == reasons.end())
        reasons.push_back(reason);
}

} // namespace

const char* material_compatibility_registry_version() { return "wondermaker-materials-v1-20260928"; }

const char* material_family_name(MaterialFamily family)
{
    switch (family) {
    case MaterialFamily::Unknown: return "unknown";
    case MaterialFamily::PLA: return "pla";
    case MaterialFamily::PETG: return "petg";
    case MaterialFamily::TPU: return "tpu";
    }
    return "unknown";
}

const char* material_compatibility_reason_name(MaterialCompatibilityReason reason)
{
    switch (reason) {
    case MaterialCompatibilityReason::ProfileNotAllowlisted: return "profile_not_allowlisted";
    case MaterialCompatibilityReason::ProfileFingerprintMismatch: return "profile_fingerprint_mismatch";
    case MaterialCompatibilityReason::RestrictedVariant: return "restricted_variant";
    case MaterialCompatibilityReason::NoMaterialInUse: return "no_material_in_use";
    case MaterialCompatibilityReason::MixedMaterialFamilies: return "unsupported_material_combination";
    }
    return "unknown_material_compatibility_reason";
}

MaterialCapabilitySnapshot evaluate_material_profile(ProfileIdentity profile)
{
    MaterialCapabilitySnapshot snapshot;
    snapshot.profile = std::move(profile);
    const MaterialRegistryEntry* entry = find_registry_entry(snapshot.profile);
    if (entry == nullptr) {
        snapshot.reasons.push_back(MaterialCompatibilityReason::ProfileNotAllowlisted);
        return snapshot;
    }
    if (snapshot.profile.fingerprint != entry->fingerprint) {
        snapshot.reasons.push_back(MaterialCompatibilityReason::ProfileFingerprintMismatch);
        return snapshot;
    }
    snapshot.family = entry->family;
    if (entry->restricted) {
        snapshot.reasons.push_back(MaterialCompatibilityReason::RestrictedVariant);
        return snapshot;
    }
    snapshot.support_status = MaterialSupportStatus::Enabled;
    snapshot.boundary_policy.allowed_machine_setting_ids = {"xJBdCljSZVDINXnP", "8xoZ6vYv9ws0J97u"};
    snapshot.boundary_policy.allowed_nozzle_diameters_mm = {0.4};
    snapshot.boundary_policy.temperature_boundary_source = "profile:nozzle_temperature_range";
    snapshot.boundary_policy.flow_boundary_source = "profile:filament_max_volumetric_speed";
    snapshot.boundary_policy.validated = true;
    snapshot.speed_policy.may_increase_process_speed = snapshot.family != MaterialFamily::TPU;
    return snapshot;
}

MaterialCompatibilitySnapshot evaluate_material_compatibility(std::vector<ProfileIdentity> used_profiles)
{
    MaterialCompatibilitySnapshot snapshot;
    snapshot.registry_version = material_compatibility_registry_version();
    if (used_profiles.empty()) {
        snapshot.reasons.push_back(MaterialCompatibilityReason::NoMaterialInUse);
        return snapshot;
    }

    snapshot.materials.reserve(used_profiles.size());
    for (ProfileIdentity& profile : used_profiles) {
        snapshot.materials.push_back(evaluate_material_profile(std::move(profile)));
        for (const MaterialCompatibilityReason reason : snapshot.materials.back().reasons)
            add_unique_reason(snapshot.reasons, reason);
    }
    if (!snapshot.reasons.empty())
        return snapshot;

    snapshot.common_family = snapshot.materials.front().family;
    if (std::any_of(snapshot.materials.begin(), snapshot.materials.end(), [&](const MaterialCapabilitySnapshot& material) {
            return material.family != snapshot.common_family;
        })) {
        snapshot.common_family = MaterialFamily::Unknown;
        snapshot.reasons.push_back(MaterialCompatibilityReason::MixedMaterialFamilies);
        return snapshot;
    }
    snapshot.combination_status = MaterialCombinationStatus::Compatible;
    return snapshot;
}

std::string capability_registry_revision_token(const MachineCapabilitySnapshot& machine,
                                               const MaterialCompatibilitySnapshot& materials)
{
    return "machine=" + machine.registry_version + "\nmaterials=" + materials.registry_version;
}

} // namespace Slic3r::AI::SmartSlicing
