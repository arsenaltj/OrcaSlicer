#pragma once

#include <cstdint>
#include <string>

namespace Slic3r::AI::SmartSlicing {

using WorkflowId  = uint64_t;
using CandidateId = std::string;

enum class Severity { Info, Warning, Error };
enum class IssueScope { Workspace, Plate, Object, Material, Configuration };
enum class Readiness { Ready, NeedsAttention, Blocked };
enum class CandidateGoal { Stability, Quality, Speed, MaterialSaving };
enum class CandidateStatus { Draft, TrialSlicing, Ready, Stale, Failed };

enum class IssueCode {
    EmptyPlate,
    OpenMesh,
    OutsideBuildVolume,
    MissingPrinter,
    MissingProcess,
    MissingMaterial,
    IncompatiblePhysicalSlots,
    InvalidMaterialTemperatureRange,
    ColorMappingDegraded,
    MulticolorEvidenceUnavailable,
    NativeValidationUnavailable,
    ConfigurationValidationError,
    ConfigurationValidationWarning
};

inline const char* issue_code_name(IssueCode code)
{
    switch (code) {
    case IssueCode::EmptyPlate: return "empty_plate";
    case IssueCode::OpenMesh: return "open_mesh";
    case IssueCode::OutsideBuildVolume: return "outside_build_volume";
    case IssueCode::MissingPrinter: return "missing_printer";
    case IssueCode::MissingProcess: return "missing_process";
    case IssueCode::MissingMaterial: return "missing_material";
    case IssueCode::IncompatiblePhysicalSlots: return "incompatible_physical_slots";
    case IssueCode::InvalidMaterialTemperatureRange: return "invalid_material_temperature_range";
    case IssueCode::ColorMappingDegraded: return "color_mapping_degraded";
    case IssueCode::MulticolorEvidenceUnavailable: return "multicolor_evidence_unavailable";
    case IssueCode::NativeValidationUnavailable: return "native_validation_unavailable";
    case IssueCode::ConfigurationValidationError: return "configuration_validation_error";
    case IssueCode::ConfigurationValidationWarning: return "configuration_validation_warning";
    }
    return "unknown";
}

} // namespace Slic3r::AI::SmartSlicing
