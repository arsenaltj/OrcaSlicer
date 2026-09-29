#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::AI {

inline constexpr const char* kProtectedRegionManifestSchema = "orcaslicer.protected-region.v1";

enum class ProtectedRegionKind : uint8_t {
    Face,
    Eye,
    Nose,
    Mouth,
    FrontContour,
    UserMarkedSurface,
};

enum class ProtectedRegionSource : uint8_t { GeneratedSemantic, UserMarked };

struct ProtectedFacetRange
{
    uint64_t begin{0};
    uint64_t end{0};

    uint64_t size() const { return end >= begin ? end - begin : 0; }
};

inline bool operator==(const ProtectedFacetRange& lhs, const ProtectedFacetRange& rhs)
{
    return lhs.begin == rhs.begin && lhs.end == rhs.end;
}

inline bool operator!=(const ProtectedFacetRange& lhs, const ProtectedFacetRange& rhs) { return !(lhs == rhs); }

// Immutable neutral hand-off. Validation belongs to the receiving boundary so
// malformed or future-version payloads can be rejected without partial use.
class ProtectedRegionManifest final
{
public:
    ProtectedRegionManifest(std::string schema, std::string source_version,
                            uint64_t object_id, uint64_t volume_id,
                            std::string geometry_fingerprint, uint64_t facet_count,
                            ProtectedRegionKind kind, ProtectedRegionSource source,
                            std::vector<ProtectedFacetRange> facet_ranges, double confidence)
        : m_schema(std::move(schema))
        , m_source_version(std::move(source_version))
        , m_object_id(object_id)
        , m_volume_id(volume_id)
        , m_geometry_fingerprint(std::move(geometry_fingerprint))
        , m_facet_count(facet_count)
        , m_kind(kind)
        , m_source(source)
        , m_facet_ranges(std::move(facet_ranges))
        , m_confidence(confidence)
    {}

    const std::string& schema() const { return m_schema; }
    const std::string& source_version() const { return m_source_version; }
    uint64_t object_id() const { return m_object_id; }
    uint64_t volume_id() const { return m_volume_id; }
    const std::string& geometry_fingerprint() const { return m_geometry_fingerprint; }
    uint64_t facet_count() const { return m_facet_count; }
    ProtectedRegionKind kind() const { return m_kind; }
    ProtectedRegionSource source() const { return m_source; }
    const std::vector<ProtectedFacetRange>& facet_ranges() const { return m_facet_ranges; }
    double confidence() const { return m_confidence; }

private:
    std::string m_schema;
    std::string m_source_version;
    uint64_t m_object_id;
    uint64_t m_volume_id;
    std::string m_geometry_fingerprint;
    uint64_t m_facet_count;
    ProtectedRegionKind m_kind;
    ProtectedRegionSource m_source;
    std::vector<ProtectedFacetRange> m_facet_ranges;
    double m_confidence;
};

inline const char* protected_region_kind_name(ProtectedRegionKind kind)
{
    switch (kind) {
    case ProtectedRegionKind::Face: return "face";
    case ProtectedRegionKind::Eye: return "eye";
    case ProtectedRegionKind::Nose: return "nose";
    case ProtectedRegionKind::Mouth: return "mouth";
    case ProtectedRegionKind::FrontContour: return "front_contour";
    case ProtectedRegionKind::UserMarkedSurface: return "user_marked_surface";
    }
    return "unknown";
}

inline const char* protected_region_source_name(ProtectedRegionSource source)
{
    switch (source) {
    case ProtectedRegionSource::GeneratedSemantic: return "generated_semantic";
    case ProtectedRegionSource::UserMarked: return "user_marked";
    }
    return "unknown";
}

} // namespace Slic3r::AI
