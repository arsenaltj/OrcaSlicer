#include "BeautyGuidanceProtectedRegions.hpp"

#include "slic3r/GUI/AI/Model/LocalSemanticEvidence.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace Slic3r::GUI {
namespace {

BeautyGuidanceConversionResult rejected(BeautyGuidanceConversionRejectionCode code,
                                        std::string diagnostic)
{
    BeautyGuidanceConversionResult result;
    result.status = BeautyGuidanceConversionStatus::Rejected;
    result.rejection_code = code;
    result.diagnostic_code = std::move(diagnostic);
    return result;
}

std::optional<AI::ProtectedRegionKind> protected_kind(const std::string& name)
{
    if (name == "face") return AI::ProtectedRegionKind::Face;
    if (name == "re" || name == "le" || name == "iris") return AI::ProtectedRegionKind::Eye;
    if (name == "nose") return AI::ProtectedRegionKind::Nose;
    if (name == "imouth" || name == "llip" || name == "ulip")
        return AI::ProtectedRegionKind::Mouth;
    if (name == "lr" || name == "rr") return AI::ProtectedRegionKind::FrontContour;
    return std::nullopt;
}

size_t kind_index(AI::ProtectedRegionKind kind)
{
    switch (kind) {
    case AI::ProtectedRegionKind::Face: return 0;
    case AI::ProtectedRegionKind::Eye: return 1;
    case AI::ProtectedRegionKind::Nose: return 2;
    case AI::ProtectedRegionKind::Mouth: return 3;
    case AI::ProtectedRegionKind::FrontContour: return 4;
    case AI::ProtectedRegionKind::UserMarkedSurface: break;
    }
    return 5;
}

std::vector<AI::ProtectedFacetRange> compress_ranges(std::vector<uint64_t> facets)
{
    std::sort(facets.begin(), facets.end());
    std::vector<AI::ProtectedFacetRange> ranges;
    for (const uint64_t facet : facets) {
        if (ranges.empty() || ranges.back().end != facet)
            ranges.push_back({facet, facet + 1});
        else
            ++ranges.back().end;
    }
    return ranges;
}

} // namespace

BeautyGuidanceConversionResult beauty_guidance_to_protected_regions(
    const AI::BeautyGuidance& guidance,
    const BeautyGuidanceBindingEvidence& evidence,
    const ProtectedRegionConversionTarget& target)
{
    if (!evidence.completed || !guidance.completed(static_cast<size_t>(evidence.facet_count)))
        return rejected(BeautyGuidanceConversionRejectionCode::NotCompleted,
                        "beauty_guidance_not_completed");
    if (target.object_id == 0 || target.volume_id == 0 || target.geometry_fingerprint.empty() ||
        target.facet_count == 0)
        return rejected(BeautyGuidanceConversionRejectionCode::InvalidTarget,
                        "protected_region_conversion_target_invalid");
    if (evidence.object_id == 0 || evidence.volume_id == 0 ||
        evidence.geometry_fingerprint.empty() || evidence.source_version.empty() ||
        evidence.facet_count == 0 || !std::isfinite(evidence.binding_confidence) ||
        evidence.binding_confidence < 0.0 || evidence.binding_confidence > 1.0)
        return rejected(BeautyGuidanceConversionRejectionCode::InvalidEvidence,
                        "beauty_guidance_binding_evidence_invalid");
    if (evidence.object_id != target.object_id)
        return rejected(BeautyGuidanceConversionRejectionCode::ObjectMismatch,
                        "beauty_guidance_object_mismatch");
    if (evidence.volume_id != target.volume_id)
        return rejected(BeautyGuidanceConversionRejectionCode::VolumeMismatch,
                        "beauty_guidance_volume_mismatch");
    if (evidence.geometry_fingerprint != target.geometry_fingerprint)
        return rejected(BeautyGuidanceConversionRejectionCode::GeometryFingerprintMismatch,
                        "beauty_guidance_geometry_fingerprint_mismatch");
    if (evidence.facet_count != target.facet_count)
        return rejected(BeautyGuidanceConversionRejectionCode::FacetCountMismatch,
                        "beauty_guidance_facet_count_mismatch");
    if (evidence.facet_count > std::numeric_limits<size_t>::max() ||
        evidence.native_facet_by_guidance_facet.size() !=
            static_cast<size_t>(evidence.facet_count))
        return rejected(BeautyGuidanceConversionRejectionCode::InvalidFacetMapping,
                        "beauty_guidance_facet_mapping_invalid");

    std::vector<uint8_t> native_seen(static_cast<size_t>(evidence.facet_count), 0);
    for (const uint64_t native_facet : evidence.native_facet_by_guidance_facet) {
        if (native_facet >= evidence.facet_count || native_seen[static_cast<size_t>(native_facet)] != 0)
            return rejected(BeautyGuidanceConversionRejectionCode::InvalidFacetMapping,
                            "beauty_guidance_facet_mapping_invalid");
        native_seen[static_cast<size_t>(native_facet)] = 1;
    }
    if (std::find(native_seen.begin(), native_seen.end(), uint8_t{0}) != native_seen.end())
        return rejected(BeautyGuidanceConversionRejectionCode::InvalidFacetMapping,
                        "beauty_guidance_facet_mapping_invalid");

    std::array<std::vector<uint64_t>, 5> protected_facets;
    for (size_t guidance_facet = 0; guidance_facet < guidance.labels.size(); ++guidance_facet) {
        const int32_t label = guidance.labels[guidance_facet];
        if (label < -1 || (label >= 0 && static_cast<size_t>(label) >= guidance.names.size()))
            return rejected(BeautyGuidanceConversionRejectionCode::InvalidLabelIndex,
                            "beauty_guidance_label_index_invalid");
        if (label < 0)
            continue;
        const std::string& name = guidance.names[static_cast<size_t>(label)];
        if (!LocalSemanticEvidence::supported_label(name) && name != "iris")
            return rejected(BeautyGuidanceConversionRejectionCode::UnknownReferencedSemantic,
                            "beauty_guidance_semantic_unknown");
        const auto kind = protected_kind(name);
        if (!kind)
            continue;
        protected_facets[kind_index(*kind)].push_back(
            evidence.native_facet_by_guidance_facet[guidance_facet]);
    }

    static constexpr std::array<AI::ProtectedRegionKind, 5> KINDS{
        AI::ProtectedRegionKind::Face,
        AI::ProtectedRegionKind::Eye,
        AI::ProtectedRegionKind::Nose,
        AI::ProtectedRegionKind::Mouth,
        AI::ProtectedRegionKind::FrontContour,
    };
    BeautyGuidanceConversionResult result;
    for (size_t index = 0; index < protected_facets.size(); ++index) {
        if (protected_facets[index].empty())
            continue;
        result.manifests.emplace_back(
            AI::kProtectedRegionManifestSchema, evidence.source_version,
            evidence.object_id, evidence.volume_id, evidence.geometry_fingerprint,
            evidence.facet_count, KINDS[index], AI::ProtectedRegionSource::GeneratedSemantic,
            compress_ranges(std::move(protected_facets[index])), evidence.binding_confidence);
    }
    if (result.manifests.empty()) {
        result.status = BeautyGuidanceConversionStatus::Unknown;
        result.diagnostic_code = "beauty_guidance_has_no_protected_semantics";
        return result;
    }
    result.status = BeautyGuidanceConversionStatus::Converted;
    result.diagnostic_code = "beauty_guidance_converted";
    return result;
}

} // namespace Slic3r::GUI
