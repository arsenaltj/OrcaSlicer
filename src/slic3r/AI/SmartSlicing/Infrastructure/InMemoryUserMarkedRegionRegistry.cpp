#include "InMemoryUserMarkedRegionRegistry.hpp"

#include <utility>
#include <vector>

namespace Slic3r::AI::SmartSlicing {

ProtectedRegionBindingResult InMemoryUserMarkedRegionRegistry::replace(
    const ProtectedRegionBindingTarget& target, std::vector<AI::ProtectedFacetRange> ranges)
{
    std::vector<AI::ProtectedRegionManifest> manifests;
    manifests.emplace_back(AI::kProtectedRegionManifestSchema, SOURCE_VERSION,
                           target.object_id, target.volume_id, target.geometry_fingerprint,
                           target.facet_count, AI::ProtectedRegionKind::UserMarkedSurface,
                           AI::ProtectedRegionSource::UserMarked, std::move(ranges), 1.0);
    ProtectedRegionBindingResult result = m_binder.bind(manifests, target);
    if (!result.accepted())
        return result;

    const Key key{target.object_id, target.volume_id};
    m_entries.erase(key);
    m_entries.emplace(key, Entry{target.workspace_revision, manifests.front()});
    return result;
}

ProtectedRegionSourceResult InMemoryUserMarkedRegionRegistry::regions_for(
    const ProtectedRegionSourceQuery& query)
{
    if (!query.workspace_revision.valid() || query.geometry_fingerprint.empty() || query.facet_count == 0)
        return {ProtectedRegionSourceStatus::Rejected, {}, "protected_region_invalid_source_query"};

    const Key key{query.object_id, query.volume_id};
    const auto found = m_entries.find(key);
    if (found == m_entries.end())
        return {ProtectedRegionSourceStatus::Unknown, {}, "user_marked_region_unavailable"};

    const AI::ProtectedRegionManifest& manifest = found->second.manifest;
    if (found->second.workspace_revision != query.workspace_revision ||
        manifest.geometry_fingerprint() != query.geometry_fingerprint ||
        manifest.facet_count() != query.facet_count) {
        m_entries.erase(found);
        return {ProtectedRegionSourceStatus::Unknown, {}, "user_marked_region_stale"};
    }

    ProtectedRegionSourceResult result;
    result.status = ProtectedRegionSourceStatus::Available;
    result.manifests.emplace_back(manifest);
    return result;
}

bool InMemoryUserMarkedRegionRegistry::clear(uint64_t object_id, uint64_t volume_id)
{
    return m_entries.erase({object_id, volume_id}) != 0;
}

size_t InMemoryUserMarkedRegionRegistry::invalidate_for_revision(const WorkspaceRevision& current_revision)
{
    size_t removed = 0;
    for (auto entry = m_entries.begin(); entry != m_entries.end();) {
        if (entry->second.workspace_revision != current_revision) {
            entry = m_entries.erase(entry);
            ++removed;
        } else {
            ++entry;
        }
    }
    return removed;
}

void InMemoryUserMarkedRegionRegistry::clear_all()
{
    m_entries.clear();
}

} // namespace Slic3r::AI::SmartSlicing
