#pragma once

#include "slic3r/AI/ModelGeneration/SemanticColoring/SemanticColoring.hpp"
#include <memory>
#include <stdexcept>
#include <map>
#include <nlohmann/json.hpp>

namespace Slic3r::GUI::PortraitColorBlocks {
namespace SC = AI::SemanticColoring;
enum class Track : uint8_t { SourceAppearance, ProcessedFaceColor };
enum class Status : uint8_t { Confirmed, Unresolved, ManualConfirmed };
// The reason a connected block remains separate from its neighboring units.
// This is diagnostic metadata only; it never changes the partition itself.
enum class BlockCause : uint8_t {
    None,
    ColorThreshold,
    SemanticBoundary,
    TopologyDisconnected,
    NonManifold,
    HistoricalLock
};
struct BlockDiagnostic {
    BlockCause primary = BlockCause::None;
    size_t boundary_count = 0;
    size_t color_blocked_edges = 0;
    size_t semantic_blocked_edges = 0;
    size_t topology_blocked_edges = 0;
    size_t nonmanifold_edges = 0;
    size_t historical_locked_edges = 0;
    float max_color_distance = 0.f;
};
struct DiagnosticSummary {
    size_t color_threshold = 0;
    size_t semantic_boundary = 0;
    size_t topology_disconnected = 0;
    size_t nonmanifold = 0;
    size_t historical_locked = 0;
    size_t color_blocked_edges = 0;
    size_t semantic_blocked_edges = 0;
    size_t topology_blocked_edges = 0;
    size_t nonmanifold_edges = 0;
    size_t historical_locked_edges = 0;
};
// Kept separate from the legacy expansion strategy: saved legacy results are
// never reinterpreted as connected blocks.
enum class Strategy : uint8_t {
    LegacyReadOnly,
    ConnectedColorBlocksV1,
    // Color regions are established before semantic labels are assigned.
    // Semantic evidence annotates a region by weighted majority and never
    // creates a new boundary.
    ColorRegionMajorityV2
};
inline constexpr float same_color_distance = .0025f;
inline constexpr size_t maximum_units = 2000000;
// The color-region postprocess is deliberately limited to the four-to-six
// color portrait workflow.  Other counts keep the ordinary palette matcher;
// this is shared by the UI, worker and cache migration code.
inline constexpr size_t portrait_postprocess_min_colors = 4;
inline constexpr size_t portrait_postprocess_max_colors = 6;
// V2 region membership now uses a fixed source-seed comparison.  Bump the
// persisted algorithm contract so candidates produced by the chained-gradient
// traversal are rebuilt instead of being interpreted as valid regions.
inline constexpr uint32_t portrait_postprocess_algorithm_version = 5;
inline constexpr bool portrait_color_postprocess_supported(size_t count) {
    return count >= portrait_postprocess_min_colors && count <= portrait_postprocess_max_colors;
}
struct Unit {
    size_t face = 0;
    SC::SubfacePath path;
};
inline std::array<SC::Barycentric,3> corners(SC::SubfacePath path) {
    if(path.depth>4 || unsigned(path.value)>=(1u<<(2*path.depth)))throw std::runtime_error("Invalid color block unit");
    std::array<SC::Barycentric,3> v{{{1,0,0},{0,1,0},{0,0,1}}};
    const auto mid=[](const auto& a,const auto& b){return SC::Barycentric{(a[0]+b[0])*.5f,(a[1]+b[1])*.5f,(a[2]+b[2])*.5f};};
    for(int d=path.depth-1;d>=0;--d) {
        const auto ab=mid(v[0],v[1]),bc=mid(v[1],v[2]),ca=mid(v[2],v[0]);
        switch((path.value>>(2*d))&3) {
        case 0:v={v[0],ab,ca};break;case 1:v={ab,v[1],bc};break;
        case 2:v={bc,v[2],ca};break;default:v={ab,bc,ca};break;
        }
    }
    return v;
}
struct Block {
    uint32_t id = 0;
    std::vector<uint32_t> members;
    SC::Color original {}, display {};
    SC::Label semantic = SC::Label::Unknown;
    Status status = Status::Unresolved;
    float confidence = 0;
    std::string reason;
    std::vector<std::pair<uint32_t, double>> neighbors;
    int64_t candidate = -1;
    // Slot identity in final_palette. RGB remains the display compatibility
    // value, while this index distinguishes equal colors in different slots.
    int64_t display_slot = -1;
    BlockDiagnostic diagnostic;
    // Canonical source appearance used for V2 region matching and palette
    // scoring.  Older caches did not persist this field and decode it from
    // `original`; new builds keep it independent from the processed track.
    SC::Color canonical {};
};
struct ColorTrack {
    std::vector<Block> blocks;
    std::vector<uint32_t> unit_blocks;
};
struct Result {
    std::string geometry_id, source_signature, semantic_signature, palette_signature, control_signature;
    Strategy strategy = Strategy::ConnectedColorBlocksV1;
    uint32_t algorithm_version = 0;
    // The ten-color palette is an internal analysis artifact. final_palette is
    // the user-visible four-to-six-color result after protected-role compaction.
    std::vector<SC::Color> intermediate_palette, final_palette;
    std::map<std::string, SC::Color> role_locks;
    // final_slot_indices preserves the original automatic-suggestion order.
    // Role locks bind semantic identities to those slots without rebuilding
    // or reordering the palette.
    std::vector<size_t> final_slot_indices;
    std::map<std::string, size_t> role_slot_locks;
    // Region identities are kept separately from the displayed color.  This
    // lets a later color-card switch remap the same protected anatomy without
    // rebuilding the partition or collapsing equal RGB values.
    std::map<std::string, std::vector<uint32_t>> role_regions;
    std::map<std::string, float> role_confidence;
    size_t face_count = 0;
    Track active = Track::ProcessedFaceColor;
    std::vector<Unit> units;
    std::array<ColorTrack, 2> tracks;
    const ColorTrack& current() const { return tracks[size_t(active)]; }
    // Collapse the unit-level connected-color result to a face-level
    // partition for the existing BeautyPuzzle editor. Faces containing
    // subface units use the block covering the largest barycentric area;
    // block identities are preserved and are never merged by RGB equality.
    // Collapse unit blocks to a face partition. When face neighbors are
    // available, split a block that became disconnected during the unit to
    // face reduction so BeautyPuzzle's connected-piece invariant remains
    // valid. The optional argument keeps the cache/test API compatible.
    std::vector<uint32_t> face_partition(
        const std::vector<std::vector<int32_t>>& face_neighbors = {}) const;
    size_t unresolved_count() const;
    DiagnosticSummary diagnostic_summary() const;
    void appearance(SC::FaceColors&, SC::SubfaceColors&) const;
    bool recolor(uint32_t block, const SC::Color&);
    size_t memory_bytes() const;
};
struct Input {
    const SC::MeshSnapshot& source;
    const SC::Analysis& analysis;
    std::vector<SC::Color> mapping, targets;
    // V2 keeps its ten-color analysis palette separate from the user-facing
    // palette.  The latter is generated by the postprocessor and never
    // changes the color-count controls in the workbench.
    Strategy strategy = Strategy::ConnectedColorBlocksV1;
    std::vector<SC::Color> intermediate_palette;
    size_t final_color_count = 6;
    SC::FaceColors processed_faces;
    SC::SubfaceColors processed_subfaces;
    // Optional provider instance identities. Zero is unknown, never an
    // invitation to join two different known people.
    std::vector<uint32_t> person_ids;
    // Immutable previous candidate; only explicit manual confirmations survive
    // a rerun. Temporary suggestions never become ordinary input or seeds.
    std::shared_ptr<const Result> previous;
};
std::shared_ptr<Result> build(const Input&, const SC::Cancel& = {});
// Rebind an existing color-region result to a new color card.  The region and
// semantic identities remain stable; protected roles receive distinct slots
// whenever the target card has enough legal colors.  Ordinary regions use
// nearest-color matching, while manual confirmations remain hard locks.
bool remap_to_palette(Result&, const std::vector<SC::Color>& target,
    bool preserve_regions = true);
std::string controls_signature(const std::vector<SC::Color>& mapping,const std::vector<SC::Color>& targets);
nlohmann::json encode(const Result&);
std::shared_ptr<Result> decode(const nlohmann::json&, const std::string& geometry,
    size_t faces, const std::string& source, const std::string& semantic,
    const std::string& palette, std::string& error);
} // namespace Slic3r::GUI::PortraitColorBlocks
