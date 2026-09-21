#pragma once

#include <array>
#include <cassert>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json_fwd.hpp>
#include "libslic3r/Color.hpp"
#include "libslic3r/TriangleMesh.hpp"

namespace Slic3r::AI::SemanticColoring {

using Color = std::array<float, 3>;
using Barycentric = std::array<float, 3>;
using FaceColors = std::vector<std::pair<size_t, Color>>;
using Cancel = std::function<bool()>;
using Progress = std::function<void(int, const std::string&)>;

// A path through the same four-way midpoint split used by TriangleSelector.
// Each level stores one child in two bits, most-significant level first:
// 0/1/2 are the original-corner children and 3 is the centre child.
struct SubfacePath {
    uint8_t depth {0};
    uint8_t value {0};
    friend bool operator==(const SubfacePath& lhs, const SubfacePath& rhs) {
        return lhs.depth == rhs.depth && lhs.value == rhs.value;
    }
    friend bool operator<(const SubfacePath& lhs, const SubfacePath& rhs) {
        return lhs.depth != rhs.depth ? lhs.depth < rhs.depth : lhs.value < rhs.value;
    }
};

struct SubfaceColor {
    size_t face_id {0};
    SubfacePath path;
    Color color {};
    float confidence {0.f};
    friend bool operator==(const SubfaceColor& lhs, const SubfaceColor& rhs) {
        return lhs.face_id == rhs.face_id && lhs.path == rhs.path &&
               lhs.color == rhs.color && lhs.confidence == rhs.confidence;
    }
};
using SubfaceColors = std::vector<SubfaceColor>;

struct SubfaceBudget {
    size_t maximum_added_triangles {200000};
    float maximum_added_ratio {.20f};
    float minimum_confidence {.70f};
};

struct SubfaceBudgetResult {
    SubfaceColors accepted;
    size_t added_triangles {0};
    size_t rejected_candidates {0};
};

enum class Label : uint8_t {
    Unknown, Background, Hair, FaceSkin, BodySkin, Clothes, Accessories, Lips, MouthInterior,
    EyeSclera, Iris, Eyebrow
};
inline constexpr size_t label_count = 12;
inline constexpr float minimum_confidence = .70f;
// Version of stored recognition evidence; palette-mapping edits do not invalidate it.
inline constexpr const char* pipeline_version = "orca.semantic-coloring/v26-macro-owned-fallback";

struct SubfaceLabelEvidence {
    size_t face_id {0};
    SubfacePath path;
    Label label {Label::Unknown};
    float confidence {0.f};
    uint32_t samples {0};
    friend bool operator==(const SubfaceLabelEvidence& lhs, const SubfaceLabelEvidence& rhs) {
        return lhs.face_id == rhs.face_id && lhs.path == rhs.path && lhs.label == rhs.label &&
               lhs.confidence == rhs.confidence && lhs.samples == rhs.samples;
    }
};

// Convert between raster evidence and the fixed midpoint tree. Both functions
// reject invalid/non-finite barycentric input without changing the output.
bool locate_subface(const Barycentric&, uint8_t depth, SubfacePath& output);
bool subface_vertices(const SubfacePath&, std::array<Barycentric, 3>& output);

// Top-left origin; tightly packed RGB8, with no alpha or row padding.
struct RGBImage {
    int width {0}, height {0};
    std::vector<uint8_t> pixels;
    bool valid() const;
};

// Providers return masks at exactly the input dimensions. A face provider owns
// its model-specific landmark topology; only these semantic masks cross the port.
enum class BoundaryPart : uint8_t { Ear, Eye, Eyebrow, Hairline, ClothesSkin };
enum class BoundarySide : uint8_t { Unspecified, Left, Right };
// Adapter-owned anatomical support. person_id is local to this input view.
struct FaceRegionHint {
    uint32_t person_id {0};
    BoundaryPart part {BoundaryPart::Ear};
    BoundarySide side {BoundarySide::Unspecified};
    std::array<int, 4> box {};
    std::vector<std::array<float, 2>> support_polygon;
};
struct Prediction {
    std::vector<Label> labels;
    std::vector<float> confidence;
    bool person_detected {false};
    bool face_detected {false};
    bool canceled {false};
    std::string error;
    std::vector<FaceRegionHint> regions;
    bool valid_for(const RGBImage& image) const;
};
struct PoseLandmark {
    float x {-1.f}, y {-1.f}, z {0.f};
    float visibility {0.f}, presence {0.f};
    bool has_visibility {false}, has_presence {false};
};
struct PosePerson {
    std::vector<PoseLandmark> landmarks;
};
struct PosePrediction {
    std::vector<PosePerson> persons;
    bool person_detected {false};
    bool canceled {false};
    std::string error;
    bool valid_for(const RGBImage&) const;
};
class IBodyRegionRecognizer {
public:
    virtual ~IBodyRegionRecognizer() = default;
    // Include model/weights hash, label mapping and preprocessing versions.
    virtual std::string identity() const = 0;
    virtual Prediction predict(const RGBImage&, const Cancel&) = 0;
};
class IFaceRegionRecognizer {
public:
    virtual ~IFaceRegionRecognizer() = default;
    virtual std::string identity() const = 0;
    virtual Prediction predict(const RGBImage&, const Cancel&) = 0;
};
class IPoseRegionRecognizer {
public:
    virtual ~IPoseRegionRecognizer() = default;
    virtual std::string identity() const = 0;
    virtual PosePrediction predict(const RGBImage&, const Cancel&) = 0;
};

struct BoundaryRegion {
    int left {0}, top {0}, right {0}, bottom {0};
    bool valid_for(const RGBImage&) const;
};
struct BoundaryPrompt {
    int x {0}, y {0};
    bool positive {false};
};
struct BoundaryTarget {
    uint32_t person_id {0};
    BoundaryPart part {BoundaryPart::Ear};
    BoundarySide side {BoundarySide::Unspecified};
    BoundaryRegion region;
    Label foreground {Label::FaceSkin}, background {Label::Hair};
    std::vector<BoundaryPrompt> prompts;
    std::vector<std::array<float, 2>> support_polygon;
};
struct BoundaryRefinementRequest {
    Label foreground {Label::FaceSkin};
    Label background {Label::Hair};
    std::vector<BoundaryRegion> regions;
    std::vector<BoundaryPrompt> prompts;
    std::vector<BoundaryTarget> targets;
};
struct BoundaryRunDiagnostic {
    uint32_t person_id {0};
    BoundaryPart part {BoundaryPart::Ear};
    BoundarySide side {BoundarySide::Unspecified};
    BoundaryRegion region;
    std::string status, reason;
    double loading_ms {0}, encoding_ms {0}, decoding_ms {0};
    float model_score {0};
    size_t changed_pixels {0};
    int view_id {-1};
    std::string crop_id;
    std::string reason_code;
};
struct BoundaryRefinement {
    int width {0}, height {0};
    // NaN outside a processed ROI. Probabilities and confidence are normalized.
    std::vector<float> foreground_probability;
    std::vector<float> confidence;
    bool canceled {false};
    // Successful inference may decline an inconsistent prompt/mask. This is a
    // safe policy fallback, distinct from runtime or resource failure.
    bool rejected {false};
    std::string rejection_reason;
    std::string error;
    float model_score {0.f};
    double loading_ms {0}, encoding_ms {0}, decoding_ms {0};
    bool valid_for(const RGBImage&) const;
};
class IBoundaryRefiner {
public:
    virtual ~IBoundaryRefiner() = default;
    virtual std::string identity() const = 0;
    virtual BoundaryRefinement refine(const RGBImage&, const Prediction& coarse,
                                      const BoundaryRefinementRequest&, const Cancel&) = 0;
};

// Immutable during analysis. Geometry is the exact preview/import canonical
// Z-up millimetre mesh; colors are original sampled sRGB, before any trial/paint.
struct MeshSnapshot {
    indexed_triangle_set mesh;
    std::vector<RGBA> vertex_colors;
    std::vector<RGBA> face_colors;
    std::string geometry_id;
    std::string content_id;
};
// Geometry identity plus the ordered original face/corner sRGB float32 values.
// Vertex duplication at color seams does not change this identity.
std::string content_fingerprint(const MeshSnapshot&);

struct Analysis {
    std::string geometry_id, content_id, body_identity, face_identity, pose_identity, boundary_identity {"none"}, signature;
    std::vector<Label> face_labels;
    std::vector<float> face_confidence;
    std::vector<Label> baseline_face_labels;
    std::vector<float> baseline_face_confidence;
    std::vector<SubfaceLabelEvidence> subface_labels;
    // Frozen depth-two evidence before optional boundary refinement. Budget
    // failures must retain this verified tree instead of evicting safe leaves.
    std::vector<SubfaceLabelEvidence> baseline_subface_labels;
    enum class MacroRegion : uint8_t { Unknown, Face, Neck, LeftArm, RightArm, TorsoClothes, Hair, Accessory };
    struct SubfaceMacroEvidence {
        size_t face_id {0};
        SubfacePath path;
        MacroRegion region {MacroRegion::Unknown};
        uint32_t person_instance {UINT32_MAX};
        float confidence {0.f};
    };
    std::vector<MacroRegion> face_macro_regions;
    std::vector<uint32_t> face_person_instances;
    std::vector<float> face_macro_confidence;
    std::vector<SubfaceMacroEvidence> subface_macro_regions;
    bool person_detected {false};
    bool canceled {false};
    size_t rendered_views {0}, face_views {0}, observed_faces {0}, reliable_faces {0};
    std::string error;
    std::string pose_error;
    std::vector<BoundaryRunDiagnostic> boundary_runs;
};

// CPU z-buffer. A pixel refers to an original mesh face, never a sampled mesh.
// Background face id is UINT32_MAX; depth increases toward the camera.
struct RenderedView {
    RGBImage image;
    std::vector<uint32_t> face_ids;
    std::vector<Barycentric> barycentric;
    std::vector<float> depth;
    std::vector<float> facing;
    // For a visible subpixel face with no direct raster sample: an adjacent
    // pixel checked against its plane and normal. Consumers must additionally
    // require source-color compatibility and neighboring semantic agreement.
    std::vector<uint32_t> surface_samples;
    bool canceled {false};
    std::string error;
};
// Coordinates in the full-view camera, normalized to [0,1]. A nonsquare region
// is letterboxed without distortion. Region rendering rerasterizes every source
// triangle; it does not enlarge a previously sampled RGB/face-id image.
struct ViewRegion { float left {0}, top {0}, width {1}, height {1}; };
RenderedView render_view(const MeshSnapshot&, float yaw_degrees, int image_size = 512,
                         const Cancel& = {});
RenderedView render_region(const MeshSnapshot&, float yaw_degrees, const ViewRegion&,
                           int image_size = 512, const Cancel& = {});

// Project one continuous image-space contour sample to the depth-two leaves
// visible around it. Barycentric coordinates are interpolated only when the
// complete 2x2 raster cell belongs to one original face. At a face boundary,
// each visible corner is projected through its own face instead.
std::vector<std::pair<uint32_t, SubfacePath>> project_boundary_sample(
    const RenderedView&, float image_x, float image_y);
Analysis analyze(const MeshSnapshot&, IBodyRegionRecognizer&, IFaceRegionRecognizer&,
                 const Cancel& = {}, const Progress& = {});
Analysis analyze(const MeshSnapshot&, IBodyRegionRecognizer&, IFaceRegionRecognizer&,
                 IBoundaryRefiner*, const Cancel& = {}, const Progress& = {});
// Optional developer evidence hook. Keeps render geometry out of model APIs;
// ordinary analysis has no observer and performs no diagnostic disk writes.
using RenderObserver = std::function<void(const RenderedView&, int view_index,
                                         const ViewRegion&, bool face_crop)>;
Analysis analyze(const MeshSnapshot&, IBodyRegionRecognizer&, IFaceRegionRecognizer&,
                 IBoundaryRefiner*, const Cancel&, const Progress&, const RenderObserver&);
Analysis analyze(const MeshSnapshot&, IBodyRegionRecognizer&, IFaceRegionRecognizer&,
                 IBoundaryRefiner*, IPoseRegionRecognizer*, const Cancel&, const Progress&, const RenderObserver&);

// portrait_card, when present, is ordered skin/dark/light/lips/cool/mid. Its
// role colors must exist in palette. All outputs are exact members of palette;
// many regions may reuse one color. An invalid/stale analysis returns no paint.
FaceColors map_palette(const MeshSnapshot&, const Analysis&, const std::vector<Color>& palette,
                       const std::vector<Color>& portrait_card = {});

// Candidate scoring used by palette mapping. Scores are costs (lower is
// better) and each evidence value is normalized to [0, 1]. Keeping this
// small value object public makes the weighting auditable without exposing
// any model-specific labels or geometry types to the palette chooser.
struct PaletteCandidateEvidence {
    float source_distance {0.f};
    float semantic_compatibility {1.f};
    float multi_view_support {1.f};
    float geometry_support {1.f};
    float continuity_support {1.f};
    // A portrait-card role may provide a small preference after compatibility
    // checks. It is deliberately capped by the implementation at 0.10.
    float role_bonus {0.f};
    bool hard_rejected {false};
};
struct PaletteCandidateScore {
    float source_component {0.f};
    float semantic_component {0.f};
    float multi_view_component {0.f};
    float geometry_component {0.f};
    float continuity_component {0.f};
    float role_component {0.f};
    float total {0.f};
    bool accepted {true};
};
PaletteCandidateScore score_palette_candidate(const PaletteCandidateEvidence&);

enum class PaletteDecisionReason : uint8_t {
    None,
    SourceColorIncompatible,
    ProtectedRegionConflict,
    PaletteAmbiguous
};
const char* palette_decision_reason_name(PaletteDecisionReason);

struct RegionPaletteCandidateDecision {
    size_t palette_index {0};
    float source_cost {0.f};
    float semantic_cost {0.f};
    float role_bonus {0.f};
    float total_cost {0.f};
    bool accepted {true};
    PaletteDecisionReason reason {PaletteDecisionReason::None};
};

struct RegionPaletteDecision {
    size_t selected_index {0};
    float best_cost {0.f};
    float second_cost {std::numeric_limits<float>::infinity()};
    float score_margin {std::numeric_limits<float>::infinity()};
    bool ambiguous {false};
    std::vector<RegionPaletteCandidateDecision> candidates;
};

// One decision path for production mapping, recommendations and diagnostics.
// original_oklab is the immutable source-region representative, while palette
// and portrait_card contain normalized sRGB colors.
RegionPaletteDecision decide_region_palette(const Color& original_oklab, Label,
                                            const std::vector<Color>& palette,
                                            const std::vector<Color>& portrait_card = {});

// Edit an established material assignment by slot; do not run recognition or
// nearest-color competition again. Invalid/ambiguous palettes return no auto layer.
FaceColors remap_palette_targets(const FaceColors& suggestions, const std::vector<Color>& original_candidates,
                                const std::vector<Color>& target_candidates);
FaceColors compose(const FaceColors& automatic, const FaceColors& manual, bool automatic_enabled);

// Budgeting is transactional: malformed input produces no accepted leaves.
// Candidates are considered by confidence, then stable face/path order. Each
// unique split node costs three added triangles; omitted leaves inherit the
// existing safe whole-face assignment.
bool enforce_subface_budget(const SubfaceColors& candidates, size_t original_face_count,
                            const SubfaceBudget&, SubfaceBudgetResult&, std::string& error);
bool map_subface_palette(const MeshSnapshot&, const Analysis&, const FaceColors& whole_face,
                         const std::vector<Color>& palette, const std::vector<Color>& portrait_card,
                         const SubfaceBudget&, SubfaceBudgetResult&, std::string& error, const Cancel& cancel = {});
SubfaceColors remap_subface_palette_targets(const SubfaceColors& suggestions,
                                            const std::vector<Color>& original_candidates,
                                            const std::vector<Color>& target_candidates);
// Manual whole-face paint suppresses every automatic child of that face.
SubfaceColors compose_subfaces(const SubfaceColors& automatic, const FaceColors& manual,
                               bool automatic_enabled);

// Cache stores analysis only: changing the physical palette never reruns vision.
// Decode is transactional and validates all identities before allocating masks.
std::string analysis_cache_key(const MeshSnapshot&, const std::string& body_identity,
                               const std::string& face_identity);
std::string analysis_cache_key(const MeshSnapshot&, const std::string& body_identity,
                               const std::string& face_identity, const std::string& boundary_identity);
std::string analysis_cache_key(const MeshSnapshot&, const std::string& body_identity,
                               const std::string& face_identity, const std::string& boundary_identity,
                               const std::string& pose_identity);
nlohmann::json encode_analysis(const Analysis&);
bool decode_analysis(const nlohmann::json&, const MeshSnapshot&, const std::string& body_identity,
                     const std::string& face_identity, Analysis&, std::string& error);
bool decode_analysis(const nlohmann::json&, const MeshSnapshot&, const std::string& body_identity,
                     const std::string& face_identity, const std::string& boundary_identity,
                     Analysis&, std::string& error);
bool decode_analysis(const nlohmann::json&, const MeshSnapshot&, const std::string& body_identity,
                     const std::string& face_identity, const std::string& boundary_identity,
                     const std::string& pose_identity, Analysis&, std::string& error);

} // namespace Slic3r::AI::SemanticColoring
