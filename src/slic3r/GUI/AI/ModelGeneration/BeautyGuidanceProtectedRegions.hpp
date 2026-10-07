#pragma once

#include "slic3r/AI/Contracts/ProtectedRegionManifest.hpp"
#include "slic3r/GUI/AI/Model/BeautyGuidance.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r::GUI {

struct BeautyGuidanceBindingEvidence
{
    bool completed{false};
    std::string source_version;
    uint64_t object_id{0};
    uint64_t volume_id{0};
    std::string geometry_fingerprint;
    uint64_t facet_count{0};
    std::vector<uint64_t> native_facet_by_guidance_facet;
    // Confidence in the verified host-side binding, not a reconstructed
    // per-facet semantic probability.
    double binding_confidence{0.0};
};

struct ProtectedRegionConversionTarget
{
    uint64_t object_id{0};
    uint64_t volume_id{0};
    std::string geometry_fingerprint;
    uint64_t facet_count{0};
};

enum class BeautyGuidanceConversionStatus { Converted, Unknown, Rejected };

enum class BeautyGuidanceConversionRejectionCode {
    NotCompleted,
    InvalidTarget,
    InvalidEvidence,
    ObjectMismatch,
    VolumeMismatch,
    GeometryFingerprintMismatch,
    FacetCountMismatch,
    InvalidFacetMapping,
    InvalidLabelIndex,
    UnknownReferencedSemantic,
};

struct BeautyGuidanceConversionResult
{
    BeautyGuidanceConversionStatus status{BeautyGuidanceConversionStatus::Rejected};
    std::vector<AI::ProtectedRegionManifest> manifests;
    std::optional<BeautyGuidanceConversionRejectionCode> rejection_code;
    std::string diagnostic_code;
};

BeautyGuidanceConversionResult beauty_guidance_to_protected_regions(
    const AI::BeautyGuidance& guidance,
    const BeautyGuidanceBindingEvidence& evidence,
    const ProtectedRegionConversionTarget& target);

} // namespace Slic3r::GUI
