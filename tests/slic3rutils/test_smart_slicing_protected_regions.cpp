#include <catch2/catch_all.hpp>

#include "slic3r/AI/SmartSlicing/Application/ProtectedRegionBinder.hpp"
#include "slic3r/AI/SmartSlicing/Domain/IntentConstraintSnapshot.hpp"
#include "slic3r/AI/SmartSlicing/Infrastructure/InMemoryUserMarkedRegionRegistry.hpp"

#include <cmath>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace Slic3r::AI;
using namespace Slic3r::AI::SmartSlicing;

namespace {

template<class T, class = void> struct HasFacetRanges : std::false_type {};
template<class T>
struct HasFacetRanges<T, std::void_t<decltype(std::declval<T>().facet_ranges)>> : std::true_type {};

template<class T, class = void> struct HasGeometryFingerprint : std::false_type {};
template<class T>
struct HasGeometryFingerprint<T, std::void_t<decltype(std::declval<T>().geometry_fingerprint)>> : std::true_type {};

template<class T, class = void> struct HasObjectId : std::false_type {};
template<class T>
struct HasObjectId<T, std::void_t<decltype(std::declval<T>().object_id)>> : std::true_type {};

template<class T, class = void> struct HasVolumeId : std::false_type {};
template<class T>
struct HasVolumeId<T, std::void_t<decltype(std::declval<T>().volume_id)>> : std::true_type {};

WorkspaceRevision revision(std::string fingerprint = "workspace-v1")
{
    return {1, 2, 3, std::move(fingerprint)};
}

ProtectedRegionBindingTarget target()
{
    return {revision(), 11, 22, "geometry-v1", 8, {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0}};
}

ProtectedRegionManifest manifest(
    ProtectedRegionKind kind = ProtectedRegionKind::Face,
    ProtectedRegionSource source = ProtectedRegionSource::GeneratedSemantic,
    std::vector<ProtectedFacetRange> ranges = {{0, 2}},
    std::string schema = kProtectedRegionManifestSchema,
    std::string source_version = "generator-semantic/v1",
    uint64_t object_id = 11,
    uint64_t volume_id = 22,
    std::string geometry_fingerprint = "geometry-v1",
    uint64_t facet_count = 8,
    double confidence = 0.9)
{
    return {std::move(schema), std::move(source_version), object_id, volume_id,
            std::move(geometry_fingerprint), facet_count, kind, source,
            std::move(ranges), confidence};
}

void check_rejected(const ProtectedRegionBindingResult& result, ProtectedRegionRejectionCode code)
{
    CHECK(result.status == ProtectedRegionBindingStatus::Rejected);
    REQUIRE(result.rejection_code.has_value());
    CHECK(*result.rejection_code == code);
    CHECK(result.regions.empty());
    CHECK(result.log_summary.regions.empty());
}

} // namespace

TEST_CASE("valid semantic regions bind atomically with aggregate-only logging", "[AI][SmartSlicing][ProtectedRegions]")
{
    static_assert(!HasFacetRanges<ProtectedRegionLogEntry>::value);
    static_assert(!HasGeometryFingerprint<ProtectedRegionLogEntry>::value);
    static_assert(!HasObjectId<ProtectedRegionLogEntry>::value);
    static_assert(!HasVolumeId<ProtectedRegionLogEntry>::value);

    ProtectedRegionBinder binder;
    const std::vector<ProtectedRegionManifest> manifests {
        manifest(ProtectedRegionKind::Face, ProtectedRegionSource::GeneratedSemantic, {{0, 3}, {5, 6}}),
        manifest(ProtectedRegionKind::Eye, ProtectedRegionSource::GeneratedSemantic, {{1, 2}}),
    };

    const ProtectedRegionBindingResult result = binder.bind(manifests, target());

    REQUIRE(result.status == ProtectedRegionBindingStatus::Accepted);
    CHECK_FALSE(result.rejection_code.has_value());
    REQUIRE(result.regions.size() == 2);
    CHECK(result.regions.front().facet_ranges == std::vector<ProtectedFacetRange> {{0, 3}, {5, 6}});
    REQUIRE(result.log_summary.regions.size() == 2);
    CHECK(result.log_summary.regions.front().selected_facet_count == 4);
    REQUIRE(result.log_summary.regions.front().surface_area_mm2.has_value());
    CHECK_THAT(*result.log_summary.regions.front().surface_area_mm2, Catch::Matchers::WithinAbs(12.0, 1e-12));
    CHECK(result.log_summary.regions.back().selected_facet_count == 1);
    CHECK(result.log_summary.diagnostic_code == "protected_regions_bound");
}

TEST_CASE("missing protected region evidence remains unknown", "[AI][SmartSlicing][ProtectedRegions]")
{
    const ProtectedRegionBindingResult result = ProtectedRegionBinder().bind({}, target());

    CHECK(result.status == ProtectedRegionBindingStatus::Unknown);
    CHECK(result.regions.empty());
    CHECK_FALSE(result.rejection_code.has_value());
    CHECK(result.log_summary.regions.empty());
    CHECK(result.log_summary.diagnostic_code == "protected_region_manifest_unavailable");
}

TEST_CASE("binding rejects target identity and facet-area evidence separately", "[AI][SmartSlicing][ProtectedRegions]")
{
    ProtectedRegionBinder binder;
    ProtectedRegionBindingTarget input = target();

    input.workspace_revision = {};
    check_rejected(binder.bind({manifest()}, input), ProtectedRegionRejectionCode::InvalidBindingTarget);
    input = target();
    input.geometry_fingerprint.clear();
    check_rejected(binder.bind({manifest()}, input), ProtectedRegionRejectionCode::InvalidBindingTarget);
    input = target();
    input.facet_count = 0;
    check_rejected(binder.bind({manifest()}, input), ProtectedRegionRejectionCode::InvalidBindingTarget);
    input = target();
    input.facet_areas_mm2.pop_back();
    check_rejected(binder.bind({manifest()}, input), ProtectedRegionRejectionCode::InvalidFacetAreaEvidence);
    input = target();
    input.facet_areas_mm2.front() = -1.0;
    check_rejected(binder.bind({manifest()}, input), ProtectedRegionRejectionCode::InvalidFacetAreaEvidence);
    input = target();
    input.facet_areas_mm2.front() = std::numeric_limits<double>::quiet_NaN();
    check_rejected(binder.bind({manifest()}, input), ProtectedRegionRejectionCode::InvalidFacetAreaEvidence);
    input = target();
    input.facet_areas_mm2[0] = std::numeric_limits<double>::max();
    input.facet_areas_mm2[1] = std::numeric_limits<double>::max();
    check_rejected(binder.bind({manifest()}, input), ProtectedRegionRejectionCode::InvalidFacetAreaEvidence);
}

TEST_CASE("binding rejects manifests that do not identify the current geometry", "[AI][SmartSlicing][ProtectedRegions]")
{
    ProtectedRegionBinder binder;
    const ProtectedRegionBindingTarget input = target();

    check_rejected(binder.bind({manifest(ProtectedRegionKind::Face, ProtectedRegionSource::GeneratedSemantic,
                                         {{0, 2}}, kProtectedRegionManifestSchema, "v1", 12)}, input),
                   ProtectedRegionRejectionCode::ObjectMismatch);
    check_rejected(binder.bind({manifest(ProtectedRegionKind::Face, ProtectedRegionSource::GeneratedSemantic,
                                         {{0, 2}}, kProtectedRegionManifestSchema, "v1", 11, 23)}, input),
                   ProtectedRegionRejectionCode::VolumeMismatch);
    check_rejected(binder.bind({manifest(ProtectedRegionKind::Face, ProtectedRegionSource::GeneratedSemantic,
                                         {{0, 2}}, kProtectedRegionManifestSchema, "v1", 11, 22,
                                         "geometry-v2")}, input),
                   ProtectedRegionRejectionCode::GeometryFingerprintMismatch);
    check_rejected(binder.bind({manifest(ProtectedRegionKind::Face, ProtectedRegionSource::GeneratedSemantic,
                                         {{0, 2}}, kProtectedRegionManifestSchema, "v1", 11, 22,
                                         "geometry-v1", 9)}, input),
                   ProtectedRegionRejectionCode::FacetCountMismatch);
}

TEST_CASE("binding validates schema enums source pairing and confidence", "[AI][SmartSlicing][ProtectedRegions]")
{
    ProtectedRegionBinder binder;
    const ProtectedRegionBindingTarget input = target();

    check_rejected(binder.bind({manifest(ProtectedRegionKind::Face, ProtectedRegionSource::GeneratedSemantic,
                                         {{0, 2}}, "orcaslicer.protected-region.v2")}, input),
                   ProtectedRegionRejectionCode::UnsupportedSchema);
    check_rejected(binder.bind({manifest(ProtectedRegionKind::Face, ProtectedRegionSource::GeneratedSemantic,
                                         {{0, 2}}, kProtectedRegionManifestSchema, "")}, input),
                   ProtectedRegionRejectionCode::MissingSourceVersion);
    check_rejected(binder.bind({manifest(static_cast<ProtectedRegionKind>(255))}, input),
                   ProtectedRegionRejectionCode::InvalidKind);
    check_rejected(binder.bind({manifest(ProtectedRegionKind::Face, static_cast<ProtectedRegionSource>(255))}, input),
                   ProtectedRegionRejectionCode::InvalidSource);
    check_rejected(binder.bind({manifest(ProtectedRegionKind::UserMarkedSurface,
                                         ProtectedRegionSource::GeneratedSemantic)}, input),
                   ProtectedRegionRejectionCode::KindSourceMismatch);
    check_rejected(binder.bind({manifest(ProtectedRegionKind::Face, ProtectedRegionSource::UserMarked)}, input),
                   ProtectedRegionRejectionCode::KindSourceMismatch);
    check_rejected(binder.bind({manifest(ProtectedRegionKind::Face, ProtectedRegionSource::GeneratedSemantic,
                                         {{0, 2}}, kProtectedRegionManifestSchema, "v1", 11, 22,
                                         "geometry-v1", 8, -0.01)}, input),
                   ProtectedRegionRejectionCode::InvalidConfidence);
    check_rejected(binder.bind({manifest(ProtectedRegionKind::Face, ProtectedRegionSource::GeneratedSemantic,
                                         {{0, 2}}, kProtectedRegionManifestSchema, "v1", 11, 22,
                                         "geometry-v1", 8, std::numeric_limits<double>::infinity())}, input),
                   ProtectedRegionRejectionCode::InvalidConfidence);
}

TEST_CASE("binding requires nonempty sorted nonoverlapping in-bounds half-open ranges", "[AI][SmartSlicing][ProtectedRegions]")
{
    ProtectedRegionBinder binder;
    const ProtectedRegionBindingTarget input = target();

    check_rejected(binder.bind({manifest(ProtectedRegionKind::Face, ProtectedRegionSource::GeneratedSemantic, {})}, input),
                   ProtectedRegionRejectionCode::EmptyRanges);
    check_rejected(binder.bind({manifest(ProtectedRegionKind::Face, ProtectedRegionSource::GeneratedSemantic,
                                         {{2, 2}})}, input),
                   ProtectedRegionRejectionCode::EmptyRange);
    check_rejected(binder.bind({manifest(ProtectedRegionKind::Face, ProtectedRegionSource::GeneratedSemantic,
                                         {{3, 4}, {1, 2}})}, input),
                   ProtectedRegionRejectionCode::RangesUnsorted);
    check_rejected(binder.bind({manifest(ProtectedRegionKind::Face, ProtectedRegionSource::GeneratedSemantic,
                                         {{1, 4}, {3, 5}})}, input),
                   ProtectedRegionRejectionCode::RangesOverlap);
    check_rejected(binder.bind({manifest(ProtectedRegionKind::Face, ProtectedRegionSource::GeneratedSemantic,
                                         {{7, 9}})}, input),
                   ProtectedRegionRejectionCode::RangeOutOfBounds);
}

TEST_CASE("one invalid manifest rejects the entire batch without partial result or log", "[AI][SmartSlicing][ProtectedRegions]")
{
    const std::vector<ProtectedRegionManifest> manifests {
        manifest(ProtectedRegionKind::Face, ProtectedRegionSource::GeneratedSemantic, {{0, 2}}),
        manifest(ProtectedRegionKind::Eye, ProtectedRegionSource::GeneratedSemantic, {{2, 2}}),
    };

    check_rejected(ProtectedRegionBinder().bind(manifests, target()), ProtectedRegionRejectionCode::EmptyRange);
}

TEST_CASE("runtime user marks replace clear and reject invalid replacement atomically", "[AI][SmartSlicing][ProtectedRegions]")
{
    InMemoryUserMarkedRegionRegistry registry;
    const ProtectedRegionBindingTarget input = target();

    REQUIRE(registry.replace(input, {{0, 2}}).accepted());
    CHECK(registry.size() == 1);
    ProtectedRegionSourceResult query = registry.regions_for(
        {input.workspace_revision, input.object_id, input.volume_id, input.geometry_fingerprint, input.facet_count});
    REQUIRE(query.status == ProtectedRegionSourceStatus::Available);
    REQUIRE(query.manifests.size() == 1);
    CHECK(query.manifests.front().source() == ProtectedRegionSource::UserMarked);
    CHECK(query.manifests.front().kind() == ProtectedRegionKind::UserMarkedSurface);
    CHECK(query.manifests.front().facet_ranges() == std::vector<ProtectedFacetRange> {{0, 2}});

    REQUIRE(registry.replace(input, {{4, 6}}).accepted());
    query = registry.regions_for(
        {input.workspace_revision, input.object_id, input.volume_id, input.geometry_fingerprint, input.facet_count});
    REQUIRE(query.manifests.size() == 1);
    CHECK(query.manifests.front().facet_ranges() == std::vector<ProtectedFacetRange> {{4, 6}});

    check_rejected(registry.replace(input, {{5, 5}}), ProtectedRegionRejectionCode::EmptyRange);
    query = registry.regions_for(
        {input.workspace_revision, input.object_id, input.volume_id, input.geometry_fingerprint, input.facet_count});
    REQUIRE(query.manifests.size() == 1);
    CHECK(query.manifests.front().facet_ranges() == std::vector<ProtectedFacetRange> {{4, 6}});

    CHECK(registry.clear(input.object_id, input.volume_id));
    CHECK_FALSE(registry.clear(input.object_id, input.volume_id));
    CHECK(registry.size() == 0);
}

TEST_CASE("runtime user marks expire on revision geometry or facet-count changes", "[AI][SmartSlicing][ProtectedRegions]")
{
    InMemoryUserMarkedRegionRegistry registry;
    ProtectedRegionBindingTarget input = target();
    REQUIRE(registry.replace(input, {{0, 2}}).accepted());

    ProtectedRegionSourceQuery query {
        revision("workspace-v2"), input.object_id, input.volume_id, input.geometry_fingerprint, input.facet_count};
    ProtectedRegionSourceResult result = registry.regions_for(query);
    CHECK(result.status == ProtectedRegionSourceStatus::Unknown);
    CHECK(result.diagnostic_code == "user_marked_region_stale");
    CHECK(registry.size() == 0);

    REQUIRE(registry.replace(input, {{0, 2}}).accepted());
    query = {input.workspace_revision, input.object_id, input.volume_id, "geometry-v2", input.facet_count};
    result = registry.regions_for(query);
    CHECK(result.status == ProtectedRegionSourceStatus::Unknown);
    CHECK(registry.size() == 0);

    REQUIRE(registry.replace(input, {{0, 2}}).accepted());
    query = {input.workspace_revision, input.object_id, input.volume_id, input.geometry_fingerprint,
             input.facet_count + 1};
    result = registry.regions_for(query);
    CHECK(result.status == ProtectedRegionSourceStatus::Unknown);
    CHECK(registry.size() == 0);

    input.object_id = 99;
    REQUIRE(registry.replace(input, {{0, 2}}).accepted());
    CHECK(registry.invalidate_for_revision(revision("workspace-v2")) == 1);
    CHECK(registry.size() == 0);
}

TEST_CASE("runtime protected regions do not mutate native paint intent constraints", "[AI][SmartSlicing][ProtectedRegions]")
{
    IntentConstraintSnapshot intents;
    intents.records = {
        {IntentConstraintType::SupportPainting, IntentConstraintSource::ModelVolumeAnnotation,
         IntentConstraintState::Active, 11, 0, 22, 7},
        {IntentConstraintType::SeamPainting, IntentConstraintSource::ModelVolumeAnnotation,
         IntentConstraintState::Active, 11, 0, 22, 8},
        {IntentConstraintType::MulticolorPainting, IntentConstraintSource::ModelVolumeAnnotation,
         IntentConstraintState::Active, 11, 0, 22, 9},
    };
    const IntentConstraintSnapshot original = intents;
    const uint64_t before = intent_constraints_revision(intents);

    InMemoryUserMarkedRegionRegistry registry;
    const ProtectedRegionBindingTarget input = target();
    REQUIRE(registry.replace(input, {{0, 2}}).accepted());
    registry.clear_all();

    CHECK(intent_constraints_revision(intents) == before);
    CHECK(intents.records.size() == original.records.size());
    for (size_t index = 0; index < intents.records.size(); ++index) {
        CHECK(intents.records[index].type == original.records[index].type);
        CHECK(intents.records[index].source_revision == original.records[index].source_revision);
    }
}
