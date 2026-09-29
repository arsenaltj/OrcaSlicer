#include <catch2/catch_all.hpp>

#include "slic3r/AI/SmartSlicing/Application/ProtectedRegionBinder.hpp"
#include "slic3r/GUI/AI/ModelGeneration/BeautyGuidanceProtectedRegions.hpp"

#include <limits>
#include <numeric>

using namespace Slic3r;
using namespace Slic3r::AI;
using namespace Slic3r::AI::SmartSlicing;
using namespace Slic3r::GUI;

namespace {

BeautyGuidance guidance()
{
    BeautyGuidance value;
    value.names = {"face", "re", "nose", "ulip", "lr", "hair"};
    value.labels = {0, 0, 1, 1, 2, 3, 3, 4, 5, 5, -1, -1};
    return value;
}

BeautyGuidanceBindingEvidence evidence()
{
    return {true, "beauty-guidance/v2", 11, 22, "geometry-v1", 12,
            {4, 5, 1, 2, 8, 10, 9, 0, 11, 7, 3, 6}, 0.92};
}

ProtectedRegionConversionTarget target()
{
    return {11, 22, "geometry-v1", 12};
}

void check_rejected(const BeautyGuidanceConversionResult& result,
                    BeautyGuidanceConversionRejectionCode code)
{
    CHECK(result.status == BeautyGuidanceConversionStatus::Rejected);
    REQUIRE(result.rejection_code);
    CHECK(*result.rejection_code == code);
    CHECK(result.manifests.empty());
}

} // namespace

TEST_CASE("Beauty guidance converts verified native facets into stable neutral manifests",
          "[AI][BeautyWorkbench][BeautyProtectedRegions]")
{
    const auto converted = beauty_guidance_to_protected_regions(guidance(), evidence(), target());
    REQUIRE(converted.status == BeautyGuidanceConversionStatus::Converted);
    REQUIRE(converted.manifests.size() == 5);
    CHECK(converted.manifests[0].kind() == ProtectedRegionKind::Face);
    CHECK(converted.manifests[0].facet_ranges() ==
          std::vector<ProtectedFacetRange>{{4, 6}});
    CHECK(converted.manifests[1].kind() == ProtectedRegionKind::Eye);
    CHECK(converted.manifests[1].facet_ranges() ==
          std::vector<ProtectedFacetRange>{{1, 3}});
    CHECK(converted.manifests[2].kind() == ProtectedRegionKind::Nose);
    CHECK(converted.manifests[2].facet_ranges() ==
          std::vector<ProtectedFacetRange>{{8, 9}});
    CHECK(converted.manifests[3].kind() == ProtectedRegionKind::Mouth);
    CHECK(converted.manifests[3].facet_ranges() ==
          std::vector<ProtectedFacetRange>{{9, 11}});
    CHECK(converted.manifests[4].kind() == ProtectedRegionKind::FrontContour);
    CHECK(converted.manifests[4].facet_ranges() ==
          std::vector<ProtectedFacetRange>{{0, 1}});
    for (const auto& manifest : converted.manifests) {
        CHECK(manifest.object_id() == 11);
        CHECK(manifest.volume_id() == 22);
        CHECK(manifest.geometry_fingerprint() == "geometry-v1");
        CHECK(manifest.facet_count() == 12);
        CHECK(manifest.source() == ProtectedRegionSource::GeneratedSemantic);
        CHECK(manifest.confidence() == 0.92);
    }

    const auto repeated = beauty_guidance_to_protected_regions(guidance(), evidence(), target());
    REQUIRE(repeated.manifests.size() == converted.manifests.size());
    for (size_t index = 0; index < converted.manifests.size(); ++index) {
        CHECK(repeated.manifests[index].kind() == converted.manifests[index].kind());
        CHECK(repeated.manifests[index].facet_ranges() ==
              converted.manifests[index].facet_ranges());
    }

    ProtectedRegionBindingTarget binding_target{
        {1, 2, 3, "workspace-v1"}, 11, 22, "geometry-v1", 12,
        std::vector<double>(12, 1.0)};
    const auto bound = ProtectedRegionBinder().bind(converted.manifests, binding_target);
    CHECK(bound.status == ProtectedRegionBindingStatus::Accepted);
    CHECK(bound.regions.size() == converted.manifests.size());
}

TEST_CASE("Beauty guidance accepts all protected facial aliases and ignores known non-protected labels",
          "[AI][BeautyWorkbench][BeautyProtectedRegions]")
{
    BeautyGuidance value;
    value.names = {"le", "iris", "imouth", "llip", "rr", "neck", "rb", "lb", "cloth"};
    value.labels = {0, 1, 2, 3, 4, 5, 6, 7, 8};
    auto binding = evidence();
    binding.facet_count = value.labels.size();
    binding.native_facet_by_guidance_facet.resize(value.labels.size());
    std::iota(binding.native_facet_by_guidance_facet.begin(),
              binding.native_facet_by_guidance_facet.end(), uint64_t{0});
    auto current = target();
    current.facet_count = value.labels.size();

    const auto result = beauty_guidance_to_protected_regions(value, binding, current);
    REQUIRE(result.status == BeautyGuidanceConversionStatus::Converted);
    REQUIRE(result.manifests.size() == 3);
    CHECK(result.manifests[0].kind() == ProtectedRegionKind::Eye);
    CHECK(result.manifests[0].facet_ranges() ==
          std::vector<ProtectedFacetRange>{{0, 2}});
    CHECK(result.manifests[1].kind() == ProtectedRegionKind::Mouth);
    CHECK(result.manifests[1].facet_ranges() ==
          std::vector<ProtectedFacetRange>{{2, 4}});
    CHECK(result.manifests[2].kind() == ProtectedRegionKind::FrontContour);

    value.labels.assign(value.labels.size(), 8);
    const auto ignored = beauty_guidance_to_protected_regions(value, binding, current);
    CHECK(ignored.status == BeautyGuidanceConversionStatus::Unknown);
    CHECK(ignored.manifests.empty());
}

TEST_CASE("Beauty guidance conversion rejects incomplete stale or malformed evidence atomically",
          "[AI][BeautyWorkbench][BeautyProtectedRegions]")
{
    SECTION("not completed") {
        auto binding = evidence();
        binding.completed = false;
        check_rejected(beauty_guidance_to_protected_regions(guidance(), binding, target()),
                       BeautyGuidanceConversionRejectionCode::NotCompleted);
        binding = evidence();
        auto value = guidance();
        value.labels.pop_back();
        check_rejected(beauty_guidance_to_protected_regions(value, binding, target()),
                       BeautyGuidanceConversionRejectionCode::NotCompleted);
    }
    SECTION("target and binding identity") {
        auto current = target(); current.object_id = 0;
        check_rejected(beauty_guidance_to_protected_regions(guidance(), evidence(), current),
                       BeautyGuidanceConversionRejectionCode::InvalidTarget);
        auto binding = evidence(); binding.binding_confidence =
            std::numeric_limits<double>::quiet_NaN();
        check_rejected(beauty_guidance_to_protected_regions(guidance(), binding, target()),
                       BeautyGuidanceConversionRejectionCode::InvalidEvidence);
        binding = evidence(); binding.object_id = 99;
        check_rejected(beauty_guidance_to_protected_regions(guidance(), binding, target()),
                       BeautyGuidanceConversionRejectionCode::ObjectMismatch);
        binding = evidence(); binding.volume_id = 99;
        check_rejected(beauty_guidance_to_protected_regions(guidance(), binding, target()),
                       BeautyGuidanceConversionRejectionCode::VolumeMismatch);
        binding = evidence(); binding.geometry_fingerprint = "stale";
        check_rejected(beauty_guidance_to_protected_regions(guidance(), binding, target()),
                       BeautyGuidanceConversionRejectionCode::GeometryFingerprintMismatch);
        binding = evidence(); binding.facet_count = 11;
        check_rejected(beauty_guidance_to_protected_regions(guidance(), binding, target()),
                       BeautyGuidanceConversionRejectionCode::NotCompleted);
        current = target(); current.facet_count = 13;
        check_rejected(beauty_guidance_to_protected_regions(guidance(), evidence(), current),
                       BeautyGuidanceConversionRejectionCode::FacetCountMismatch);
    }
    SECTION("facet mapping") {
        auto binding = evidence(); binding.native_facet_by_guidance_facet.pop_back();
        check_rejected(beauty_guidance_to_protected_regions(guidance(), binding, target()),
                       BeautyGuidanceConversionRejectionCode::InvalidFacetMapping);
        binding = evidence(); binding.native_facet_by_guidance_facet[0] =
            binding.native_facet_by_guidance_facet[1];
        check_rejected(beauty_guidance_to_protected_regions(guidance(), binding, target()),
                       BeautyGuidanceConversionRejectionCode::InvalidFacetMapping);
        binding = evidence(); binding.native_facet_by_guidance_facet[0] = 12;
        check_rejected(beauty_guidance_to_protected_regions(guidance(), binding, target()),
                       BeautyGuidanceConversionRejectionCode::InvalidFacetMapping);
    }
    SECTION("labels") {
        auto value = guidance(); value.labels[0] = 99;
        check_rejected(beauty_guidance_to_protected_regions(value, evidence(), target()),
                       BeautyGuidanceConversionRejectionCode::InvalidLabelIndex);
        value = guidance(); value.names.push_back("future-label"); value.labels[0] = 6;
        check_rejected(beauty_guidance_to_protected_regions(value, evidence(), target()),
                       BeautyGuidanceConversionRejectionCode::UnknownReferencedSemantic);
        value = guidance(); value.names.push_back("future-label");
        CHECK(beauty_guidance_to_protected_regions(value, evidence(), target()).status ==
              BeautyGuidanceConversionStatus::Converted);
    }
}
