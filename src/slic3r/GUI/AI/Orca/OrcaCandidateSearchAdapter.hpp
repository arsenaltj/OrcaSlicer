#pragma once

#include "libslic3r/PrintConfig.hpp"
#include "slic3r/AI/SmartSlicing/Application/CandidateSearchPipeline.hpp"
#include "slic3r/AI/SmartSlicing/Domain/WorkspaceContext.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r::GUI {

enum class OrcaCandidateSearchCaptureStatus {
    Complete,
    InvalidInput,
    MachineCapabilityPendingValidation,
    MachineCapabilityDisabled,
    MaterialCompatibilityUnavailable,
    ProcessProfileDirty,
    ProcessProfileIdentityUnavailable,
    ProcessProfileFingerprintUnavailable,
    ProfileEvidenceConflict,
};

struct OrcaCandidateSearchObjectSource
{
    uint64_t object_id{0};
    uint64_t instance_id{0};
    std::array<double, 16> current_transform{};
    bool locked{false};
    std::vector<AI::SmartSlicing::OrientationSearchOption> verified_orientation_options;
    std::vector<AI::SmartSlicing::ModelFeatureSnapshot> model_features;
    AI::SmartSlicing::ProtectedRegionBindingStatus protected_region_status{
        AI::SmartSlicing::ProtectedRegionBindingStatus::Unknown};
    std::vector<AI::SmartSlicing::ProtectedRegionSnapshot> protected_regions;
};

struct OrcaCandidateSearchProfileSource
{
    bool dirty{false};
    AI::SmartSlicing::ProfileIdentity identity;
    DynamicPrintConfig process_config;
    DynamicPrintConfig effective_config;
};

struct OrcaCandidateSearchCaptureSource
{
    AI::SmartSlicing::WorkspaceContext context;
    int64_t plate_id{0};
    bool plate_locked{false};
    AI::SmartSlicing::UsagePurpose usage_purpose{AI::SmartSlicing::UsagePurpose::Unknown};
    std::vector<OrcaCandidateSearchObjectSource> objects;
    OrcaCandidateSearchProfileSource process_profile;
};

struct OrcaCandidateSearchCaptureResult
{
    OrcaCandidateSearchCaptureStatus status{OrcaCandidateSearchCaptureStatus::InvalidInput};
    std::string diagnostic_code;
    std::optional<AI::SmartSlicing::CandidateSearchInput> input;

    bool completed() const
    {
        return status == OrcaCandidateSearchCaptureStatus::Complete && input.has_value();
    }
};

class OrcaCandidateSearchAdapter
{
public:
    static OrcaCandidateSearchCaptureResult capture(const OrcaCandidateSearchCaptureSource& source);
};

} // namespace Slic3r::GUI
