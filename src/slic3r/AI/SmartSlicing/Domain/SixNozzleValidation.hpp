#pragma once

#include "MachineCapabilitySnapshot.hpp"
#include "MaterialCompatibility.hpp"

#include <string>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

inline constexpr const char* SIX_NOZZLE_VALIDATION_SCHEMA = "orcaslicer.smart-slicing.six-nozzle-validation";
inline constexpr const char* SIX_NOZZLE_VALIDATION_VERSION = "v1";
inline constexpr const char* SIX_NOZZLE_VALIDATION_POLICY_VERSION = "six-nozzle-validation-policy/v1";
inline constexpr size_t SIX_NOZZLE_COUNT = 6;

enum class SixNozzleValidationStatus { PendingValidation, Enabled, Rejected };

const char* six_nozzle_validation_status_name(SixNozzleValidationStatus status);

struct SixNozzleSlotMapping
{
    int physical_number{0};
    EvidenceValue<std::string> material_id;
    EvidenceValue<std::string> color_id;
};

struct SixNozzlePersistenceEvidence
{
    EvidenceValue<bool> save_restore_round_trip;
    EvidenceValue<std::string> restored_fingerprint;
};

struct SixNozzleValidationInput
{
    std::string schema{SIX_NOZZLE_VALIDATION_SCHEMA};
    std::string version{SIX_NOZZLE_VALIDATION_VERSION};
    std::string policy_version{SIX_NOZZLE_VALIDATION_POLICY_VERSION};
    MachineCapabilitySnapshot machine;
    MaterialCompatibilitySnapshot materials;
    std::vector<SixNozzleSlotMapping> slot_mappings;
    SixNozzlePersistenceEvidence persistence;
};

struct SixNozzleValidationResult
{
    std::string schema{SIX_NOZZLE_VALIDATION_SCHEMA};
    std::string version{SIX_NOZZLE_VALIDATION_VERSION};
    std::string policy_version{SIX_NOZZLE_VALIDATION_POLICY_VERSION};
    SixNozzleValidationStatus status{SixNozzleValidationStatus::PendingValidation};
    std::string canonical_fingerprint;
    std::vector<std::string> diagnostic_codes;

    bool enabled() const { return status == SixNozzleValidationStatus::Enabled; }
};

SixNozzleValidationResult validate_six_nozzle(const SixNozzleValidationInput& input);
SixNozzleValidationResult production_six_nozzle_validation();
std::string canonical_six_nozzle_fingerprint(const SixNozzleValidationInput& input);

} // namespace Slic3r::AI::SmartSlicing
