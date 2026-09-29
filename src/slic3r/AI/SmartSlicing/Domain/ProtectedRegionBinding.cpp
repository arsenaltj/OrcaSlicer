#include "ProtectedRegionBinding.hpp"

namespace Slic3r::AI::SmartSlicing {

const char* protected_region_binding_status_name(ProtectedRegionBindingStatus status)
{
    switch (status) {
    case ProtectedRegionBindingStatus::Accepted: return "accepted";
    case ProtectedRegionBindingStatus::Unknown: return "unknown";
    case ProtectedRegionBindingStatus::Rejected: return "rejected";
    }
    return "rejected";
}

const char* protected_region_rejection_code_name(ProtectedRegionRejectionCode code)
{
    switch (code) {
    case ProtectedRegionRejectionCode::InvalidBindingTarget: return "protected_region_invalid_binding_target";
    case ProtectedRegionRejectionCode::UnsupportedSchema: return "protected_region_unsupported_schema";
    case ProtectedRegionRejectionCode::MissingSourceVersion: return "protected_region_missing_source_version";
    case ProtectedRegionRejectionCode::InvalidKind: return "protected_region_invalid_kind";
    case ProtectedRegionRejectionCode::InvalidSource: return "protected_region_invalid_source";
    case ProtectedRegionRejectionCode::KindSourceMismatch: return "protected_region_kind_source_mismatch";
    case ProtectedRegionRejectionCode::ObjectMismatch: return "protected_region_object_mismatch";
    case ProtectedRegionRejectionCode::VolumeMismatch: return "protected_region_volume_mismatch";
    case ProtectedRegionRejectionCode::GeometryFingerprintMismatch:
        return "protected_region_geometry_fingerprint_mismatch";
    case ProtectedRegionRejectionCode::FacetCountMismatch: return "protected_region_facet_count_mismatch";
    case ProtectedRegionRejectionCode::InvalidConfidence: return "protected_region_invalid_confidence";
    case ProtectedRegionRejectionCode::EmptyRanges: return "protected_region_empty_ranges";
    case ProtectedRegionRejectionCode::EmptyRange: return "protected_region_empty_range";
    case ProtectedRegionRejectionCode::RangesUnsorted: return "protected_region_ranges_unsorted";
    case ProtectedRegionRejectionCode::RangesOverlap: return "protected_region_ranges_overlap";
    case ProtectedRegionRejectionCode::RangeOutOfBounds: return "protected_region_range_out_of_bounds";
    case ProtectedRegionRejectionCode::InvalidFacetAreaEvidence:
        return "protected_region_invalid_facet_area_evidence";
    }
    return "protected_region_rejected";
}

} // namespace Slic3r::AI::SmartSlicing
