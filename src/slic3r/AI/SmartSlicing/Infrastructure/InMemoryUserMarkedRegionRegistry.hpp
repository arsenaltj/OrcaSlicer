#pragma once

#include "slic3r/AI/SmartSlicing/Application/ProtectedRegionBinder.hpp"
#include "slic3r/AI/SmartSlicing/Ports/IProtectedRegionSource.hpp"

#include <map>
#include <utility>

namespace Slic3r::AI::SmartSlicing {

class InMemoryUserMarkedRegionRegistry final : public IProtectedRegionSource
{
public:
    static constexpr const char* SOURCE_VERSION = "user-marked-runtime/v1";

    ProtectedRegionBindingResult replace(const ProtectedRegionBindingTarget& target,
                                         std::vector<AI::ProtectedFacetRange> ranges);
    ProtectedRegionSourceResult regions_for(const ProtectedRegionSourceQuery& query) override;
    bool clear(uint64_t object_id, uint64_t volume_id);
    size_t invalidate_for_revision(const WorkspaceRevision& current_revision);
    void clear_all();
    size_t size() const { return m_entries.size(); }

private:
    using Key = std::pair<uint64_t, uint64_t>;

    struct Entry
    {
        WorkspaceRevision workspace_revision;
        AI::ProtectedRegionManifest manifest;
    };

    std::map<Key, Entry> m_entries;
    ProtectedRegionBinder m_binder;
};

} // namespace Slic3r::AI::SmartSlicing
