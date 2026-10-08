#include "ProtectedRegionBinder.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace Slic3r::AI::SmartSlicing {
namespace {

bool known_kind(AI::ProtectedRegionKind kind)
{
    switch (kind) {
    case AI::ProtectedRegionKind::Face:
    case AI::ProtectedRegionKind::Eye:
    case AI::ProtectedRegionKind::Nose:
    case AI::ProtectedRegionKind::Mouth:
    case AI::ProtectedRegionKind::FrontContour:
    case AI::ProtectedRegionKind::UserMarkedSurface: return true;
    }
    return false;
}

bool known_source(AI::ProtectedRegionSource source)
{
    switch (source) {
    case AI::ProtectedRegionSource::GeneratedSemantic:
    case AI::ProtectedRegionSource::UserMarked: return true;
    }
    return false;
}

bool source_matches_kind(const AI::ProtectedRegionManifest& manifest)
{
    return manifest.source() == AI::ProtectedRegionSource::UserMarked ?
               manifest.kind() == AI::ProtectedRegionKind::UserMarkedSurface :
               manifest.kind() != AI::ProtectedRegionKind::UserMarkedSurface;
}

ProtectedRegionBindingResult rejected(ProtectedRegionRejectionCode code)
{
    ProtectedRegionBindingResult result;
    result.status = ProtectedRegionBindingStatus::Rejected;
    result.rejection_code = code;
    result.log_summary.binding_status = result.status;
    result.log_summary.diagnostic_code = protected_region_rejection_code_name(code);
    return result;
}

std::optional<ProtectedRegionRejectionCode> validate_target(const ProtectedRegionBindingTarget& target)
{
    if (!target.workspace_revision.valid() || target.geometry_fingerprint.empty() || target.facet_count == 0)
        return ProtectedRegionRejectionCode::InvalidBindingTarget;
    if (target.facet_areas_mm2.empty())
        return std::nullopt;
    if (target.facet_count > std::numeric_limits<size_t>::max() ||
        target.facet_areas_mm2.size() != static_cast<size_t>(target.facet_count))
        return ProtectedRegionRejectionCode::InvalidFacetAreaEvidence;
    if (!std::all_of(target.facet_areas_mm2.begin(), target.facet_areas_mm2.end(), [](double area) {
            return std::isfinite(area) && area >= 0.0;
        }))
        return ProtectedRegionRejectionCode::InvalidFacetAreaEvidence;
    return std::nullopt;
}

} // namespace

ProtectedRegionBindingResult ProtectedRegionBinder::bind(
    const std::vector<AI::ProtectedRegionManifest>& manifests,
    const ProtectedRegionBindingTarget& target) const
{
    if (const auto invalid_target = validate_target(target))
        return rejected(*invalid_target);

    if (manifests.empty()) {
        ProtectedRegionBindingResult result;
        result.status = ProtectedRegionBindingStatus::Unknown;
        result.log_summary.binding_status = result.status;
        result.log_summary.diagnostic_code = "protected_region_manifest_unavailable";
        return result;
    }

    std::vector<ProtectedRegionSnapshot> accepted_regions;
    std::vector<ProtectedRegionLogEntry> log_regions;
    accepted_regions.reserve(manifests.size());
    log_regions.reserve(manifests.size());

    for (const AI::ProtectedRegionManifest& manifest : manifests) {
        if (manifest.schema() != AI::kProtectedRegionManifestSchema)
            return rejected(ProtectedRegionRejectionCode::UnsupportedSchema);
        if (manifest.source_version().empty())
            return rejected(ProtectedRegionRejectionCode::MissingSourceVersion);
        if (!known_kind(manifest.kind()))
            return rejected(ProtectedRegionRejectionCode::InvalidKind);
        if (!known_source(manifest.source()))
            return rejected(ProtectedRegionRejectionCode::InvalidSource);
        if (!source_matches_kind(manifest))
            return rejected(ProtectedRegionRejectionCode::KindSourceMismatch);
        if (manifest.object_id() != target.object_id)
            return rejected(ProtectedRegionRejectionCode::ObjectMismatch);
        if (manifest.volume_id() != target.volume_id)
            return rejected(ProtectedRegionRejectionCode::VolumeMismatch);
        if (manifest.geometry_fingerprint() != target.geometry_fingerprint)
            return rejected(ProtectedRegionRejectionCode::GeometryFingerprintMismatch);
        if (manifest.facet_count() != target.facet_count)
            return rejected(ProtectedRegionRejectionCode::FacetCountMismatch);
        if (!std::isfinite(manifest.confidence()) || manifest.confidence() < 0.0 ||
            manifest.confidence() > 1.0)
            return rejected(ProtectedRegionRejectionCode::InvalidConfidence);
        if (manifest.facet_ranges().empty())
            return rejected(ProtectedRegionRejectionCode::EmptyRanges);

        uint64_t selected_facet_count = 0;
        std::optional<double> surface_area;
        if (!target.facet_areas_mm2.empty())
            surface_area = 0.0;
        const AI::ProtectedFacetRange* previous = nullptr;
        for (const AI::ProtectedFacetRange& range : manifest.facet_ranges()) {
            if (range.begin >= range.end)
                return rejected(ProtectedRegionRejectionCode::EmptyRange);
            if (range.end > target.facet_count)
                return rejected(ProtectedRegionRejectionCode::RangeOutOfBounds);
            if (previous != nullptr && range.begin < previous->begin)
                return rejected(ProtectedRegionRejectionCode::RangesUnsorted);
            if (previous != nullptr && range.begin < previous->end)
                return rejected(ProtectedRegionRejectionCode::RangesOverlap);
            selected_facet_count += range.size();
            if (surface_area) {
                for (uint64_t facet = range.begin; facet < range.end; ++facet) {
                    const double next_area = *surface_area + target.facet_areas_mm2[static_cast<size_t>(facet)];
                    if (!std::isfinite(next_area))
                        return rejected(ProtectedRegionRejectionCode::InvalidFacetAreaEvidence);
                    *surface_area = next_area;
                }
            }
            previous = &range;
        }

        accepted_regions.push_back({manifest.object_id(), manifest.volume_id(), manifest.geometry_fingerprint(),
                                    manifest.facet_count(), manifest.kind(), manifest.source(),
                                    manifest.source_version(), manifest.facet_ranges(), manifest.confidence()});
        log_regions.push_back({manifest.kind(), manifest.source(), selected_facet_count, surface_area});
    }

    ProtectedRegionBindingResult result;
    result.status = ProtectedRegionBindingStatus::Accepted;
    result.regions = std::move(accepted_regions);
    result.log_summary.binding_status = result.status;
    result.log_summary.diagnostic_code = "protected_regions_bound";
    result.log_summary.regions = std::move(log_regions);
    return result;
}

} // namespace Slic3r::AI::SmartSlicing
