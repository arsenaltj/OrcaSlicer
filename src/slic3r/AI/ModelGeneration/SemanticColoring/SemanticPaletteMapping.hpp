#pragma once

#include "SemanticColoring.hpp"

namespace Slic3r::AI::SemanticColoring {

inline constexpr const char* material_mapping_version = "orca.semantic-material-mapping/v6";

// Stable product identities, independent of list order and RGB equality.
struct PaletteSlot {
    std::string id;
    Color color {};
    bool enabled {true};
};
// Observable bounded discovery cache; counters measure source extraction,
// independently from changing filament candidates and target colors.
struct MaterialDiscoveryCacheStats { uint64_t hits {0}, builds {0}; };
MaterialDiscoveryCacheStats material_discovery_cache_stats();
void clear_material_discovery_cache();

struct MaterialCenter {
    size_t id {0};
    Label label {Label::Unknown};
    Color original_rgb {};
    Color original_oklab {};
    size_t face_count {0};
    double surface_area {0.0};
    std::string region_id;
    Color q05_oklab {}, q50_oklab {}, q95_oklab {}, medoid_oklab {};
    float color_dispersion {0.f};
    float dominant_ratio {0.f};
    float gradient_strength {0.f};
};

// A region-level edit is deliberately separate from a filament-slot edit.
// The analysis signature prevents an edit made for one mesh/recognizer result
// from being applied to a different model or a stale recognition result.
struct RegionColorOverride {
    std::string analysis_signature;
    size_t material_center_id {0};
    std::string slot_id;
    bool locked {true};
    std::string region_id;
    Color target_color {};
    bool has_target_color {false};
    Label semantic_role {Label::Unknown};
    friend bool operator==(const RegionColorOverride& lhs, const RegionColorOverride& rhs) {
        return lhs.analysis_signature == rhs.analysis_signature &&
               lhs.material_center_id == rhs.material_center_id &&
               lhs.slot_id == rhs.slot_id && lhs.locked == rhs.locked &&
               lhs.region_id == rhs.region_id && lhs.target_color == rhs.target_color &&
               lhs.has_target_color == rhs.has_target_color && lhs.semantic_role == rhs.semantic_role;
    }
};

enum class RegionResolutionStatus : uint8_t {
    Automatic,
    Locked,
    TemporarySubstitute,
    Ambiguous,
    StaleIntent,
    MissingSlot,
    NoFeasibleCandidate
};
const char* region_resolution_status_name(RegionResolutionStatus);

struct ResolvedRegionColor {
    std::string region_id;
    std::string intended_slot_id;
    std::string actual_slot_id;
    Color actual_color {};
    RegionResolutionStatus status {RegionResolutionStatus::Automatic};
};

struct RegionColorCandidate {
    std::string slot_id;
    Color color {};
    float score {0.f};
    bool accepted {false};
    std::string rejection_reason;
    float source_cost {0.f};
    float semantic_cost {0.f};
    float role_bonus {0.f};
};

// Diagnostic recommendation for one immutable discovered material region.
// Candidates are ordered deterministically by score and slot ID.
struct RegionColorRecommendation {
    std::string analysis_signature;
    size_t material_center_id {0};
    Label label {Label::Unknown};
    Color original_rgb {};
    Color original_oklab {};
    size_t face_count {0};
    double surface_area {0.0};
    std::string recommended_slot_id;
    std::string region_id;
    float top_score {0.f};
    float second_score {std::numeric_limits<float>::infinity()};
    float score_margin {std::numeric_limits<float>::infinity()};
    bool ambiguous {false};
    std::vector<RegionColorCandidate> candidates;
};
struct FaceSlotAssignment {
    size_t face_id {0};
    std::string slot_id;
    std::string intended_slot_id;
    Color intended_color {};
    size_t material_center_id {0};
    std::string region_id;
};
struct SubfaceSlotAssignment {
    size_t face_id {0};
    SubfacePath path;
    std::string slot_id;
    float confidence {0.f};
    std::string intended_slot_id;
    Color intended_color {};
    size_t material_center_id {0};
    std::string region_id;
};
struct SlotMappingResult {
    FaceColors faces;
    SubfaceColors subfaces;
    std::vector<FaceSlotAssignment> face_slots;
    std::vector<SubfaceSlotAssignment> subface_slots;
    std::vector<MaterialCenter> material_centers;
    std::string analysis_signature;
    std::vector<RegionColorOverride> region_overrides;
    std::vector<ResolvedRegionColor> resolved_regions;
    std::string palette_signature;
    std::vector<Color> portrait_card;
    size_t added_triangles {0}, rejected_candidates {0}, substituted_assignments {0};
    bool boundary_budget_fallback {false};
};

std::string semantic_palette_signature(const std::vector<PaletteSlot>&);

// Uses the same material center for an entire region and every child material.
// Center identity/content never depends on the candidate palette size.
FaceColors map_palette_materials(const MeshSnapshot&, const Analysis&, const std::vector<Color>& palette,
                                const std::vector<Color>& portrait_card, std::vector<MaterialCenter>&, const Cancel& cancel = {});

// Variant that also returns the immutable discovered material ID for every
// original face. The vector is empty on failure and is useful for applying a
// region override consistently to whole faces and boundary children.
FaceColors map_palette_materials(const MeshSnapshot&, const Analysis&, const std::vector<Color>& palette,
                                const std::vector<Color>& portrait_card, std::vector<MaterialCenter>&,
                                std::vector<size_t>& face_material_ids, const Cancel& cancel = {});

// Canonicalizes enabled slots by ID, so reordered/duplicate RGB slots are
// deterministic. Invalid zero/>6 enabled palettes fail transactionally.
bool map_palette_slots(const MeshSnapshot&, const Analysis&, const std::vector<PaletteSlot>&,
                       const std::vector<Color>& portrait_card, const SubfaceBudget&,
                       SlotMappingResult&, std::string& error, const Cancel& cancel = {});

// Region overrides are optional and are applied after automatic discovery but
// before child mapping. The old overload above remains source compatible.
bool map_palette_slots(const MeshSnapshot&, const Analysis&, const std::vector<PaletteSlot>&,
                       const std::vector<Color>& portrait_card, const SubfaceBudget&,
                       const std::vector<RegionColorOverride>&,
                       SlotMappingResult&, std::string& error, const Cancel& cancel = {});
// Alternate argument order for callers that keep overrides beside analysis.
bool map_palette_slots(const MeshSnapshot&, const Analysis&, const std::vector<PaletteSlot>&,
                       const std::vector<Color>& portrait_card,
                       const std::vector<RegionColorOverride>&, const SubfaceBudget&,
                       SlotMappingResult&, std::string& error, const Cancel& cancel = {});

std::vector<RegionColorRecommendation> recommend_region_slots(const SlotMappingResult&,
                                                              const std::vector<PaletteSlot>&);

// Explicit edits preserve assignment identity. A disabled intended slot gets
// a temporary active replacement; its original ID/color survive restoration.
// This is not the API for adding candidate colors: call map_palette_slots for
// a new automatic competition, retaining the unchanged Analysis.
bool remap_palette_slots(const SlotMappingResult&, const std::vector<PaletteSlot>&,
                         SlotMappingResult&, std::string& error);

// Select source-material medoids, not new pixel/histogram clusters. Locked
// IDs/colors survive; requested_count is raised to their count (at most six).
// Nearby source centers may share a suggestion, so fewer than N is valid.
// Empty/no-area material evidence reports failure for an explicit safe fallback.
bool suggest_material_slots(const std::vector<MaterialCenter>&, size_t requested_count,
                            const std::vector<PaletteSlot>& locked_slots,
                            std::vector<PaletteSlot>& output, std::string& error);

// Independent mapping persistence. Analysis/geometry validation belongs to the
// owning validation session; this decoder checks face/path and slot contracts.
nlohmann::json encode_slot_mapping(const SlotMappingResult&);
bool decode_slot_mapping(const nlohmann::json&, size_t face_count,
                         const std::vector<PaletteSlot>&, SlotMappingResult&, std::string& error);

} // namespace Slic3r::AI::SemanticColoring
