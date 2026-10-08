#pragma once

#include "slic3r/AI/Contracts/ProtectedRegionManifest.hpp"
#include "WorkspaceRevision.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

enum class ProtectedRegionBindingStatus { Accepted, Unknown, Rejected };

enum class ProtectedRegionRejectionCode {
    InvalidBindingTarget,
    UnsupportedSchema,
    MissingSourceVersion,
    InvalidKind,
    InvalidSource,
    KindSourceMismatch,
    ObjectMismatch,
    VolumeMismatch,
    GeometryFingerprintMismatch,
    FacetCountMismatch,
    InvalidConfidence,
    EmptyRanges,
    EmptyRange,
    RangesUnsorted,
    RangesOverlap,
    RangeOutOfBounds,
    InvalidFacetAreaEvidence,
};

struct ProtectedRegionBindingTarget
{
    WorkspaceRevision workspace_revision;
    uint64_t object_id{0};
    uint64_t volume_id{0};
    std::string geometry_fingerprint;
    uint64_t facet_count{0};
    std::vector<double> facet_areas_mm2;
};

struct ProtectedRegionSnapshot
{
    uint64_t object_id{0};
    uint64_t volume_id{0};
    std::string geometry_fingerprint;
    uint64_t facet_count{0};
    AI::ProtectedRegionKind kind{AI::ProtectedRegionKind::Face};
    AI::ProtectedRegionSource source{AI::ProtectedRegionSource::GeneratedSemantic};
    std::string source_version;
    std::vector<AI::ProtectedFacetRange> facet_ranges;
    double confidence{0.0};
};

struct ProtectedRegionLogEntry
{
    AI::ProtectedRegionKind kind{AI::ProtectedRegionKind::Face};
    AI::ProtectedRegionSource source{AI::ProtectedRegionSource::GeneratedSemantic};
    uint64_t selected_facet_count{0};
    std::optional<double> surface_area_mm2;
};

struct ProtectedRegionLogSummary
{
    ProtectedRegionBindingStatus binding_status{ProtectedRegionBindingStatus::Unknown};
    std::string diagnostic_code;
    std::vector<ProtectedRegionLogEntry> regions;
};

struct ProtectedRegionBindingResult
{
    ProtectedRegionBindingStatus status{ProtectedRegionBindingStatus::Unknown};
    std::vector<ProtectedRegionSnapshot> regions;
    std::optional<ProtectedRegionRejectionCode> rejection_code;
    ProtectedRegionLogSummary log_summary;

    bool accepted() const { return status == ProtectedRegionBindingStatus::Accepted; }
};

const char* protected_region_binding_status_name(ProtectedRegionBindingStatus status);
const char* protected_region_rejection_code_name(ProtectedRegionRejectionCode code);

} // namespace Slic3r::AI::SmartSlicing
