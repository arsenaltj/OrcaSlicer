#include "SemanticColoring.hpp"
#include "SemanticBoundaryRefinement.hpp"
#include "SemanticMaskRefinement.hpp"
#include "SemanticMaterialRegions.hpp"
#include "SemanticPaletteMapping.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

namespace Slic3r::AI::SemanticColoring {

PaletteCandidateScore score_palette_candidate(const PaletteCandidateEvidence& evidence)
{
    const auto unit = [](float value) {
        return std::isfinite(value) ? std::clamp(value, 0.f, 1.f) : 0.f;
    };
    PaletteCandidateScore result;
    result.accepted = !evidence.hard_rejected;
    if (!result.accepted) {
        result.source_component = result.semantic_component = result.multi_view_component =
            result.geometry_component = result.continuity_component = result.role_component = 0.f;
        result.total = std::numeric_limits<float>::infinity();
        return result;
    }

    // Existing color matching works in a squared Oklab distance. Normalize
    // that distance to a conservative perceptual range before combining it
    // with the other evidence terms. Values above this range are all treated
    // as materially incompatible and are then separated by the semantic and
    // geometry terms.
    result.source_component = unit(evidence.source_distance / .09f);
    result.semantic_component = 1.f - unit(evidence.semantic_compatibility);
    result.multi_view_component = 1.f - unit(evidence.multi_view_support);
    result.geometry_component = 1.f - unit(evidence.geometry_support);
    result.continuity_component = 1.f - unit(evidence.continuity_support);
    result.role_component = std::min(.10f, unit(evidence.role_bonus));
    result.total = .35f * result.source_component +
        .30f * result.semantic_component +
        .15f * result.multi_view_component +
        .10f * result.geometry_component +
        .10f * result.continuity_component - result.role_component;
    return result;
}

const char* palette_decision_reason_name(PaletteDecisionReason reason)
{
    switch (reason) {
    case PaletteDecisionReason::None: return "NONE";
    case PaletteDecisionReason::SourceColorIncompatible: return "SOURCE_COLOR_INCOMPATIBLE";
    case PaletteDecisionReason::ProtectedRegionConflict: return "PROTECTED_REGION_CONFLICT";
    case PaletteDecisionReason::PaletteAmbiguous: return "PALETTE_AMBIGUOUS";
    }
    return "NONE";
}

namespace {
constexpr uint32_t no_face = std::numeric_limits<uint32_t>::max();
constexpr size_t maximum_faces = 2000000, maximum_vertices = 6000000;
constexpr size_t maximum_subface_evidence = 1000000;
bool stopped(const Cancel& cancel) { return cancel && cancel(); }
bool valid_color(const Color& c)
{
    return std::all_of(c.begin(), c.end(), [](float v) { return std::isfinite(v) && v >= 0 && v <= 1; });
}
bool valid_label(Label label) { return size_t(label) < label_count; }
bool paintable(Label label) { return valid_label(label) && label != Label::Unknown && label != Label::Background; }
// Face details are small, high-value regions. A coarse boundary result may
// refine one of these regions only when it remains in the same anatomical
// family; otherwise the previously accepted detail is the safer material.
bool protected_face_detail(Label label)
{
    return label == Label::EyeSclera || label == Label::Iris || label == Label::Eyebrow ||
           label == Label::Lips || label == Label::MouthInterior;
}
bool compatible_boundary_detail(Label baseline, Label refined)
{
    if (!protected_face_detail(baseline)) return true;
    if (baseline == Label::EyeSclera || baseline == Label::Iris)
        return refined == Label::EyeSclera || refined == Label::Iris;
    return refined == baseline;
}
constexpr std::array<Label, 7> face_detail_labels {{
    Label::Lips, Label::MouthInterior, Label::EyeSclera, Label::Iris, Label::Eyebrow, Label::FaceSkin, Label::Hair
}};
size_t face_detail_index(Label label)
{
    return size_t(std::find(face_detail_labels.begin(), face_detail_labels.end(), label) - face_detail_labels.begin());
}
Color corner_color(const MeshSnapshot& source, size_t face, int corner)
{
    const auto& color = source.vertex_colors.size() == source.mesh.vertices.size()
        ? source.vertex_colors[source.mesh.indices[face][corner]] : source.face_colors[face];
    return {color[0], color[1], color[2]};
}
std::string validate_snapshot(const MeshSnapshot& source)
{
    if (source.mesh.indices.empty() || source.mesh.indices.size() > maximum_faces ||
        source.mesh.vertices.empty() || source.mesh.vertices.size() > maximum_vertices)
        return "The semantic analysis mesh is empty or exceeds the editable mesh limit.";
    if (source.vertex_colors.size() != source.mesh.vertices.size() && source.face_colors.size() != source.mesh.indices.size())
        return "Semantic analysis requires original vertex or face colors.";
    for (const auto& vertex : source.mesh.vertices)
        if (!vertex.allFinite()) return "The semantic analysis mesh contains invalid positions.";
    for (size_t face = 0; face < source.mesh.indices.size(); ++face)
        for (int corner = 0; corner < 3; ++corner) {
            const int index = source.mesh.indices[face][corner];
            if (index < 0 || size_t(index) >= source.mesh.vertices.size()) return "The semantic analysis mesh contains an invalid face.";
            if (!valid_color(corner_color(source, face, corner))) return "Semantic analysis requires finite normalized sRGB colors.";
        }
    return {};
}

// Public-domain Oklab matrices, matching the existing preview color space.
Color lab(Color rgb)
{
    for (float& c : rgb) c = c <= .04045f ? c / 12.92f : std::pow((c + .055f) / 1.055f, 2.4f);
    const float l = std::cbrt(.4122214708f*rgb[0] + .5363325363f*rgb[1] + .0514459929f*rgb[2]);
    const float m = std::cbrt(.2119034982f*rgb[0] + .6806995451f*rgb[1] + .1073969566f*rgb[2]);
    const float s = std::cbrt(.0883024619f*rgb[0] + .2817188376f*rgb[1] + .6299787005f*rgb[2]);
    return {.2104542553f*l + .793617785f*m - .0040720468f*s,
        1.9779984951f*l - 2.428592205f*m + .4505937099f*s,
        .0259040371f*l + .7827717662f*m - .808675766f*s};
}
float distance(const Color& a, const Color& b)
{
    const float dl = (a[0] - b[0]) * .65f, da = a[1] - b[1], db = a[2] - b[2];
    return dl*dl + da*da + db*db;
}
float chroma(const Color& a) { return std::hypot(a[1], a[2]); }
bool compatible_skin(const Color& source, const Color& center, Label label)
{
    // Body segmentation includes eyebrows, eyes, teeth and sometimes lips in
    // face-skin. These source outliers must retain the existing fine color map.
    const float source_chroma = chroma(source), center_chroma = chroma(center);
    if (source_chroma < .015f || source[1] < 0.f || source[2] <= 0.f || center_chroma < .015f) return false;
    const float hue_alignment = (source[1] * center[1] + source[2] * center[2]) / (source_chroma * center_chroma);
    if (label == Label::BodySkin) {
        // Neck and hand shadows do not contain the facial details protected
        // below. Accept supported warm shadows, while rejecting saturated red
        // clothing even when the body mask incorrectly calls it skin.
        return source[0] >= center[0] - .55f && source[0] <= center[0] + .24f &&
            hue_alignment >= .80f && source[1] <= source[2] * 1.6f &&
            source_chroma <= std::min(.16f, center_chroma * 2.4f + .02f);
    }
    if (source[0] < center[0] - .16f || source[0] > center[0] + .20f) return false;
    if (hue_alignment < .90f) return false;
    if (source[1] > center[1] + .015f && source[2] < center[2] * .85f) return false;
    return source_chroma <= center_chroma * 1.8f + .02f;
}
bool same_muted_cloth(const Color& source, const Color& center)
{
    const float source_chroma = chroma(source), center_chroma = chroma(center);
    // Weak but consistent dye color is different from an actual gray stripe.
    // Let its local appearance prototype absorb small lighting/texture changes
    // across the neutral cutoff; never borrow color from a saturated garment.
    return source_chroma >= .005f && source_chroma < .035f &&
        center_chroma >= .005f && center_chroma < .035f &&
        std::abs(source[0] - center[0]) < .15f &&
        source[1] * center[1] + source[2] * center[2] >= .94f * source_chroma * center_chroma;
}
bool compatible_sclera(const Color& source, const Color& center)
{
    // Eye masks are geometric and may touch eyeliner, iris or adjacent skin.
    // Require a plausible eye-white source even when the region has support.
    return source[0] >= .40f && source[0] >= center[0] - .18f && chroma(source) <= .055f;
}
bool natural_dark_hair(const Color& color)
{
    // Brown/black appearance, not a red, golden, cool-dyed or white material.
    // This is only used after the region has been recognized as hair.
    const float saturation = chroma(color);
    return color[0] < .67f && saturation < .115f &&
        (saturation >= .008f || color[0] < .40f) &&
        color[1] >= -.012f && color[2] >= -.012f &&
        color[1] <= color[2] * 1.15f + .018f;
}
float candidate_semantic_compatibility(const Color& source, const Color& candidate, Label label,
                                       bool boundary_supported = false)
{
    const float source_chroma = chroma(source), candidate_chroma = chroma(candidate);
    // These are soft penalties. Existing label-specific guards remain the
    // hard protection; this term only ranks otherwise valid palette choices.
    if (label == Label::EyeSclera) {
        if (candidate_chroma > .055f || candidate[0] < .40f) return .05f;
        // Eye-white shadows are lighting evidence, not separate printable
        // materials. Prefer the brighter compatible neutral while retaining a
        // gray fallback when a small palette contains no white slot.
        return std::clamp((candidate[0] - .35f) / .50f, .10f, 1.f);
    }
    if (label == Label::MouthInterior) {
        if (candidate_chroma > std::max(.08f, source_chroma + .035f)) return .10f;
        return std::clamp((.78f - candidate[0]) / .58f, .10f, 1.f);
    }
    if (label == Label::Iris)
        return candidate_chroma <= source_chroma + .035f && candidate[0] <= .72f ? 1.f : .20f;
    if (label == Label::Eyebrow)
        return candidate[0] <= .72f ? 1.f : .25f;
    if (label == Label::Lips)
        return candidate_chroma >= .025f && candidate[1] >= candidate[2] - .015f ? 1.f : .45f;
    if (label == Label::FaceSkin || label == Label::BodySkin) {
        if (label == Label::BodySkin && source[0] < .32f) return .05f;
        const Label compatibility_label = boundary_supported && label == Label::FaceSkin ? Label::BodySkin : label;
        return compatible_skin(source, candidate, compatibility_label) ? 1.f : .05f;
    }
    if (label == Label::Hair && candidate_chroma > source_chroma + .025f)
        return .35f;
    if (label == Label::Hair && source_chroma >= .12f && candidate_chroma < source_chroma * .35f)
        return .30f;
    return 1.f;
}
Color face_lab(const MeshSnapshot& source, size_t face);
bool facial_eye_detail(Label label)
{
    return label == Label::EyeSclera || label == Label::Iris || label == Label::Eyebrow;
}
bool lip_like_material(const Color& rgb, const std::vector<Color>& portrait_card, const std::vector<Color>& palette,
                      size_t& lip_slot)
{
    lip_slot = palette.size();
    if (portrait_card.size() == 6) {
        for (size_t index = 0; index < palette.size(); ++index)
            if (palette[index] == portrait_card[3]) { lip_slot = index; break; }
    }
    if (lip_slot < palette.size() && rgb == palette[lip_slot]) return true;
    if (lip_slot < palette.size()) return false;
    const Color color = lab(rgb);
    // Warm red pigments have a clear positive-a-over-b bias in Oklab. Brown
    // irises and hair usually have b >= a and must remain available.
    const bool heuristic_lip = chroma(color) >= .04f && color[1] > .02f &&
        color[1] > color[2] + .01f && color[2] > -.03f;
    if (heuristic_lip)
        for (size_t index = 0; index < palette.size(); ++index)
            if (palette[index] == rgb) { lip_slot = index; break; }
    return heuristic_lip;
}
size_t eye_safe_target(const MeshSnapshot& source, size_t face_id, Label label,
                       const std::vector<Color>& palette, const std::vector<Color>& palette_labs,
                       size_t lip_slot)
{
    if (palette.empty()) return 0;
    if (label == Label::EyeSclera) {
        size_t selected = palette.size();
        float best = -std::numeric_limits<float>::max();
        for (size_t index = 0; index < palette.size(); ++index) {
            if (index == lip_slot) continue;
            const float c = chroma(palette_labs[index]);
            if (c >= .055f) continue;
            if (palette_labs[index][0] > best) { best = palette_labs[index][0]; selected = index; }
        }
        if (selected < palette.size()) return selected;
    }
    const Color source_color = face_lab(source, face_id);
    size_t selected = 0;
    float best = std::numeric_limits<float>::max();
    for (size_t index = 0; index < palette.size(); ++index) {
        if (index == lip_slot) continue;
        const float candidate_chroma = chroma(palette_labs[index]);
        const float source_chroma = chroma(source_color);
        if (candidate_chroma > source_chroma + .035f) continue;
        if (label == Label::Eyebrow && palette_labs[index][0] > .72f) continue;
        const float score = distance(source_color, palette_labs[index]) +
            (label == Label::Iris && candidate_chroma < .02f ? .015f : 0.f);
        if (score < best) { best = score; selected = index; }
    }
    return selected;
}
void sanitize_eye_edge_targets(const MeshSnapshot& source, const Analysis& analysis,
                               const std::vector<Color>& palette, const std::vector<Color>& portrait_card,
                               FaceColors& output)
{
    if (output.empty() || palette.empty() || analysis.face_labels.size() != source.mesh.indices.size()) return;
    std::vector<Color> palette_labs; palette_labs.reserve(palette.size());
    for (const auto& color : palette) palette_labs.push_back(lab(color));
    std::vector<std::vector<size_t>> adjacent(source.mesh.vertices.size());
    for (size_t face_id = 0; face_id < source.mesh.indices.size(); ++face_id)
        for (int corner = 0; corner < 3; ++corner)
            adjacent[source.mesh.indices[face_id][corner]].push_back(face_id);
    const auto label_for = [&](size_t face_id) {
        Label label = analysis.face_labels[face_id];
        if (face_id < analysis.baseline_face_labels.size() &&
            facial_eye_detail(analysis.baseline_face_labels[face_id]))
            label = analysis.baseline_face_labels[face_id];
        return label;
    };
    std::vector<uint8_t> eye_face(source.mesh.indices.size(), 0);
    for (size_t face_id = 0; face_id < source.mesh.indices.size(); ++face_id)
        eye_face[face_id] = facial_eye_detail(label_for(face_id)) ? 1 : 0;
    for (auto& assignment : output) {
        if (assignment.first >= eye_face.size()) continue;
        const Label label = label_for(assignment.first);
        bool adjacent_to_eye = eye_face[assignment.first] != 0;
        if (!adjacent_to_eye) {
            for (int corner = 0; corner < 3 && !adjacent_to_eye; ++corner)
                for (const size_t neighbor : adjacent[source.mesh.indices[assignment.first][corner]])
                    if (neighbor < eye_face.size() && eye_face[neighbor]) { adjacent_to_eye = true; break; }
        }
        if (!adjacent_to_eye) continue;
        if (label == Label::Lips || label == Label::MouthInterior) continue;
        size_t lip_slot = palette.size();
        const bool is_lip = lip_like_material(assignment.second, portrait_card, palette, lip_slot);
        const Color source_color = face_lab(source, assignment.first);
        const bool eyebrow_like = (label == Label::FaceSkin || label == Label::BodySkin || label == Label::Unknown) &&
            source_color[0] < .43f && chroma(source_color) < .10f &&
            source_color[1] >= -.02f && source_color[2] >= -.02f;
        if (!is_lip && !eyebrow_like) continue;
        size_t replacement = palette.size();
        if (eyebrow_like && portrait_card.size() == 6)
            for (size_t index = 0; index < palette.size(); ++index)
                if (palette[index] == portrait_card[5]) { replacement = index; break; }
        if (replacement == palette.size())
            replacement = eye_safe_target(source, assignment.first,
                eyebrow_like ? Label::Eyebrow : label, palette, palette_labs, lip_slot);
        if (replacement < palette.size()) assignment.second = palette[replacement];
    }
}
void sanitize_hair_edge_targets(const MeshSnapshot& source, const Analysis& analysis,
                                const std::vector<Color>& palette, const std::vector<Color>& portrait_card,
                                FaceColors& output)
{
    if (output.empty() || palette.empty() || analysis.face_labels.size() != source.mesh.indices.size()) return;
    std::vector<Color> palette_labs; palette_labs.reserve(palette.size());
    for (const auto& color : palette) palette_labs.push_back(lab(color));
    std::vector<std::vector<size_t>> adjacent(source.mesh.vertices.size());
    for (size_t face_id = 0; face_id < source.mesh.indices.size(); ++face_id)
        for (int corner = 0; corner < 3; ++corner)
            adjacent[source.mesh.indices[face_id][corner]].push_back(face_id);
    const auto label_for = [&](size_t face_id) {
        Label label = analysis.face_labels[face_id];
        if (face_id < analysis.baseline_face_labels.size() && analysis.baseline_face_labels[face_id] == Label::Hair)
            label = Label::Hair;
        return label;
    };
    size_t skin_slot = palette.size(), hair_slot = palette.size();
    if (portrait_card.size() == 6) {
        for (size_t index = 0; index < palette.size(); ++index) {
            if (palette[index] == portrait_card[0]) skin_slot = index;
            if (palette[index] == portrait_card[1]) hair_slot = index;
        }
    }
    for (auto& assignment : output) {
        if (assignment.first >= source.mesh.indices.size()) continue;
        const Label label = label_for(assignment.first);
        if (label == Label::FaceSkin || label == Label::Hair || label == Label::Lips || label == Label::MouthInterior ||
            label == Label::EyeSclera || label == Label::Iris || label == Label::Eyebrow) continue;
        bool next_to_hair = false;
        for (int corner = 0; corner < 3 && !next_to_hair; ++corner)
            for (const size_t neighbor : adjacent[source.mesh.indices[assignment.first][corner]])
                if (neighbor < source.mesh.indices.size() && label_for(neighbor) == Label::Hair) { next_to_hair = true; break; }
        if (!next_to_hair) continue;
        const Color source_color = face_lab(source, assignment.first);
        const bool hair_like = natural_dark_hair(source_color) ||
            (source_color[0] < .50f && chroma(source_color) < .11f && source_color[1] >= -.02f && source_color[2] >= -.02f);
        if (!hair_like) continue;
        size_t lip_slot = palette.size();
        const bool lip_target = lip_like_material(assignment.second, portrait_card, palette, lip_slot);
        const bool skin_target = skin_slot < palette.size() && assignment.second == palette[skin_slot];
        if (!lip_target && !skin_target) continue;
        size_t replacement = hair_slot;
        if (replacement >= palette.size()) {
            replacement = palette.size(); float best = std::numeric_limits<float>::max();
            for (size_t index = 0; index < palette.size(); ++index) {
                if (index == skin_slot || index == lip_slot) continue;
                const float candidate_chroma = chroma(palette_labs[index]);
                if (candidate_chroma > chroma(source_color) + .035f || palette_labs[index][0] > source_color[0] + .18f) continue;
                const float score = distance(source_color, palette_labs[index]);
                if (score < best) { best = score; replacement = index; }
            }
        }
        if (replacement < palette.size()) assignment.second = palette[replacement];
    }
}
void sanitize_lip_leak_targets(const MeshSnapshot& source, const Analysis& analysis,
                               const std::vector<Color>& palette, const std::vector<Color>& portrait_card,
                               FaceColors& output)
{
    if (output.empty() || palette.empty() || analysis.face_labels.size() != source.mesh.indices.size()) return;
    size_t lip_slot = palette.size(), skin_slot = palette.size();
    if (portrait_card.size() == 6) {
        for (size_t index = 0; index < palette.size(); ++index) {
            if (palette[index] == portrait_card[3]) lip_slot = index;
            if (palette[index] == portrait_card[0]) skin_slot = index;
        }
    }
    if (lip_slot >= palette.size()) return;
    std::vector<std::vector<size_t>> adjacent(source.mesh.vertices.size());
    for (size_t face_id = 0; face_id < source.mesh.indices.size(); ++face_id)
        for (int corner = 0; corner < 3; ++corner)
            adjacent[source.mesh.indices[face_id][corner]].push_back(face_id);
    const auto label_for = [&](size_t face_id) {
        Label label = analysis.face_labels[face_id];
        if (face_id < analysis.baseline_face_labels.size() &&
            (analysis.baseline_face_labels[face_id] == Label::Lips ||
             analysis.baseline_face_labels[face_id] == Label::MouthInterior))
            label = analysis.baseline_face_labels[face_id];
        return label;
    };
    std::vector<uint8_t> lip_face(source.mesh.indices.size(), 0);
    for (size_t face_id = 0; face_id < lip_face.size(); ++face_id)
        lip_face[face_id] = label_for(face_id) == Label::Lips || label_for(face_id) == Label::MouthInterior;
    std::vector<Color> palette_labs; palette_labs.reserve(palette.size());
    for (const auto& color : palette) palette_labs.push_back(lab(color));
    for (auto& assignment : output) {
        if (assignment.first >= lip_face.size() || assignment.second != palette[lip_slot]) continue;
        const Label label = label_for(assignment.first);
        if (label == Label::Lips || label == Label::MouthInterior || label == Label::Hair || label == Label::Clothes)
            continue;
        bool near_lips = lip_face[assignment.first] != 0;
        if (!near_lips)
            for (int corner = 0; corner < 3 && !near_lips; ++corner)
                for (const size_t neighbor : adjacent[source.mesh.indices[assignment.first][corner]])
                    if (neighbor < lip_face.size() && lip_face[neighbor]) { near_lips = true; break; }
        if (near_lips) continue;
        if (skin_slot < palette.size() && (label == Label::FaceSkin || label == Label::BodySkin || label == Label::Unknown)) {
            assignment.second = palette[skin_slot];
            continue;
        }
        const Color source_color = face_lab(source, assignment.first);
        size_t replacement = palette.size(); float best = std::numeric_limits<float>::max();
        for (size_t index = 0; index < palette.size(); ++index) {
            if (index == lip_slot) continue;
            const float score = distance(source_color, palette_labs[index]);
            if (score < best) { best = score; replacement = index; }
        }
        if (replacement < palette.size()) assignment.second = palette[replacement];
    }
}
bool same_hair_material(const Color& source, const Color& center)
{
    if (!natural_dark_hair(center) || source[0] > .75f ||
        std::abs(source[0] - center[0]) > .35f || chroma(source) >= .12f ||
        source[1] < -.012f || source[2] < -.012f || source[1] > source[2] * 1.15f + .018f)
        return false;
    const float source_chroma = chroma(source), center_chroma = chroma(center);
    // Keep a real gray streak separate from brown highlights, including when
    // both belong to one connected hair surface.
    if (source_chroma < .008f && source[0] > center[0] + .12f) return false;
    return source_chroma < .015f || center_chroma < .015f ||
        source[1] * center[1] + source[2] * center[2] >= .85f * source_chroma * center_chroma;
}
Color face_lab(const MeshSnapshot& source, size_t face)
{
    Color rgb {};
    for (int corner = 0; corner < 3; ++corner) {
        const auto c = corner_color(source, face, corner);
        for (int channel = 0; channel < 3; ++channel) rgb[channel] += c[channel] / 3.f;
    }
    return lab(rgb);
}
double area(const MeshSnapshot& source, size_t face)
{
    const auto& f = source.mesh.indices[face];
    return .5 * double((source.mesh.vertices[f[1]] - source.mesh.vertices[f[0]])
        .cross(source.mesh.vertices[f[2]] - source.mesh.vertices[f[0]]).norm());
}
std::string sha256(const std::string& input)
{
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx.get(), input.data(), input.size()) != 1) return {};
    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes {};
    unsigned int count = 0;
    if (EVP_DigestFinal_ex(ctx.get(), bytes.data(), &count) != 1 || count != 32) return {};
    constexpr char hex[] = "0123456789abcdef";
    std::string result; result.reserve(64);
    for (unsigned int i = 0; i < count; ++i) { result += hex[bytes[i] >> 4]; result += hex[bytes[i] & 15]; }
    return result;
}
int unhex(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

std::vector<ViewRegion> face_regions(const RenderedView& view, const Prediction& body)
{
    const int width = view.image.width, height = view.image.height;
    struct Box { int left, top, right, bottom; size_t pixels; };
    std::vector<uint8_t> remaining(size_t(width) * height, 0);
    for (size_t pixel = 0; pixel < remaining.size(); ++pixel)
        remaining[pixel] = view.face_ids[pixel] != no_face && body.labels[pixel] == Label::FaceSkin &&
            body.confidence[pixel] >= .55f;
    std::vector<Box> boxes;
    std::vector<size_t> pending;
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const size_t first = size_t(y) * width + x;
        if (!remaining[first]) continue;
        remaining[first] = 0;
        pending.assign(1, first);
        Box box {x, y, x, y, 0};
        for (size_t cursor = 0; cursor < pending.size(); ++cursor) {
            const size_t pixel = pending[cursor];
            const int px = int(pixel % width), py = int(pixel / width);
            box.left = std::min(box.left, px); box.right = std::max(box.right, px);
            box.top = std::min(box.top, py); box.bottom = std::max(box.bottom, py); ++box.pixels;
            constexpr int dx[] {-1, 1, 0, 0}, dy[] {0, 0, -1, 1};
            for (size_t direction = 0; direction < 4; ++direction) {
                const int nx = px + dx[direction], ny = py + dy[direction];
                if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
                const size_t neighbor = size_t(ny) * width + nx;
                if (!remaining[neighbor]) continue;
                remaining[neighbor] = 0; pending.push_back(neighbor);
            }
        }
        if (box.pixels < 16) continue;
        const int padding = std::max(4, std::max(box.right - box.left + 1, box.bottom - box.top + 1) / 5);
        box.left = std::max(0, box.left - padding); box.right = std::min(width - 1, box.right + padding);
        box.top = std::max(0, box.top - padding); box.bottom = std::min(height - 1, box.bottom + padding);
        boxes.push_back(box);
    }
    // Eye or mouth holes may split one face-skin mask into nearby islands. The
    // padded boxes overlap for one face, while distinct people remain separate.
    for (size_t first = 0; first < boxes.size(); ++first) for (size_t second = first + 1; second < boxes.size();) {
        auto& a = boxes[first]; const auto& b = boxes[second];
        if (a.right < b.left || b.right < a.left || a.bottom < b.top || b.bottom < a.top) {
            ++second; continue;
        }
        a.left = std::min(a.left, b.left); a.top = std::min(a.top, b.top);
        a.right = std::max(a.right, b.right); a.bottom = std::max(a.bottom, b.bottom);
        a.pixels += b.pixels; boxes.erase(boxes.begin() + second); second = first + 1;
    }
    std::sort(boxes.begin(), boxes.end(), [](const Box& lhs, const Box& rhs) {
        if (lhs.pixels != rhs.pixels) return lhs.pixels > rhs.pixels;
        if (lhs.left != rhs.left) return lhs.left < rhs.left;
        return lhs.top < rhs.top;
    });
    if (boxes.size() > 4) boxes.resize(4);
    std::sort(boxes.begin(), boxes.end(), [](const Box& lhs, const Box& rhs) {
        return lhs.left != rhs.left ? lhs.left < rhs.left : lhs.top < rhs.top;
    });
    std::vector<ViewRegion> result;
    result.reserve(boxes.size());
    for (const Box& box : boxes)
        result.push_back({float(box.left) / width, float(box.top) / height,
            float(box.right - box.left + 1) / width, float(box.bottom - box.top + 1) / height});
    return result;
}

float supported_surface_sample(const RenderedView& view, const Prediction& prediction,
                               size_t face, const std::vector<Color>& original)
{
    const uint32_t pixel = view.surface_samples[face];
    if (pixel == no_face) return 0;
    const uint32_t neighbor = view.face_ids[pixel];
    if (neighbor == no_face || distance(original[face], original[neighbor]) > .0009f) return 0;
    const int x = int(pixel % view.image.width), y = int(pixel / view.image.width);
    if (x < 1 || y < 1 || x + 1 >= view.image.width || y + 1 >= view.image.height) return 0;
    const Label label = prediction.labels[pixel];
    if (!paintable(label)) return 0;
    float confidence = 1.f;
    // Do not infer across silhouettes, semantic edges, or a differently colored
    // overlapping surface. One original view, not iterative label diffusion.
    for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
        const size_t nearby = size_t(y + dy) * view.image.width + x + dx;
        if (view.face_ids[nearby] == no_face || prediction.labels[nearby] != label ||
            prediction.confidence[nearby] < .80f) return 0;
        confidence = std::min(confidence, prediction.confidence[nearby]);
    }
    return confidence;
}

struct Sample { Color color; double weight; };
std::vector<Color> prototypes(const std::vector<Sample>& samples, size_t limit)
{
    double total = 0;
    for (const auto& sample : samples) total += sample.weight;
    if (total <= 0) return {};
    // A coordinate-wise weighted median does not let baked highlights or a few
    // dark shadow faces dictate the material of an entire semantic region.
    Color median {};
    std::vector<std::pair<float, double>> sorted; sorted.reserve(samples.size());
    for (int channel = 0; channel < 3; ++channel) {
        sorted.clear();
        for (const auto& sample : samples) sorted.emplace_back(sample.color[channel], sample.weight);
        std::sort(sorted.begin(), sorted.end());
        double seen = 0;
        for (const auto& entry : sorted) { seen += entry.second; if (seen >= total * .5) { median[channel] = entry.first; break; } }
    }
    std::vector<Color> centers {median};
    // Supported histogram bins seed additional genuine source colors. A lone
    // tiny texture speck cannot consume a local prototype.
    struct Bin { double weight {0}; std::array<double, 3> sum {}; };
    std::map<std::array<int, 3>, Bin> bins;
    for (const auto& sample : samples) {
        const auto& c = sample.color;
        auto& bin = bins[{int(c[0] * 12), int((c[1] + .5f) * 16), int((c[2] + .5f) * 16)}];
        bin.weight += sample.weight;
        for (int channel = 0; channel < 3; ++channel) bin.sum[channel] += c[channel] * sample.weight;
    }
    while (centers.size() < limit) {
        float best = .0025f; Color chosen {}; bool found = false;
        for (const auto& entry : bins) {
            if (entry.second.weight < total * .08) continue;
            Color color {};
            for (int channel = 0; channel < 3; ++channel) color[channel] = float(entry.second.sum[channel] / entry.second.weight);
            float closest = std::numeric_limits<float>::max();
            for (const auto& center : centers) closest = std::min(closest, distance(color, center));
            if (closest > best) { best = closest; chosen = color; found = true; }
        }
        if (!found) break;
        centers.push_back(chosen);
    }
    return centers;
}

struct HairSeed { Color appearance {}; int slot {-1}; };
void extend_hair_edges(const MeshSnapshot& source, const Analysis& analysis,
                       const std::vector<HairSeed>& seeds, const std::vector<Color>& palette,
                       FaceColors& output)
{
    const size_t count = source.mesh.indices.size();
    if (std::none_of(seeds.begin(), seeds.end(), [](const HairSeed& seed) { return seed.slot >= 0; })) return;
    std::vector<uint32_t> owner(count, no_face);
    std::vector<uint8_t> hops(count, 255), eligible(count, 0);
    std::vector<Color> colors(count);
    std::vector<Vec3f> normals(count, Vec3f::Zero());
    std::vector<Vec3f> positions(count, Vec3f::Zero());
    Vec3f lower = source.mesh.vertices.front(), upper = lower;
    for (const auto& vertex : source.mesh.vertices) { lower = lower.cwiseMin(vertex); upper = upper.cwiseMax(vertex); }
    const float maximum_distance_squared = (upper - lower).squaredNorm() * (.008f * .008f);
    bool have_seed = false, have_border = false;
    for (size_t id = 0; id < count; ++id) {
        const Label label = analysis.face_labels[id];
        const bool seed = seeds[id].slot >= 0 && analysis.face_confidence[id] >= .80f;
        const bool border = analysis.face_confidence[id] < minimum_confidence &&
            (label == Label::Hair || label == Label::Unknown || label == Label::Background || label == Label::Clothes);
        if (!seed && !border) continue;
        colors[id] = face_lab(source, id);
        if (!natural_dark_hair(colors[id])) continue;
        const auto& face = source.mesh.indices[id];
        positions[id] = (source.mesh.vertices[face[0]] + source.mesh.vertices[face[1]] + source.mesh.vertices[face[2]]) / 3.f;
        normals[id] = (source.mesh.vertices[face[1]] - source.mesh.vertices[face[0]]).cross(
            source.mesh.vertices[face[2]] - source.mesh.vertices[face[0]]);
        const float length = normals[id].norm();
        if (length <= 0) continue;
        normals[id] /= length;
        if (seed) { owner[id] = uint32_t(id); hops[id] = 0; have_seed = true; }
        else { eligible[id] = 1; have_border = true; }
    }
    if (!have_seed || !have_border) return;

    struct Edge { uint64_t vertices; uint32_t face; };
    std::vector<Edge> edges; edges.reserve(count * 3);
    for (size_t id = 0; id < count; ++id) {
        const auto& face = source.mesh.indices[id];
        for (int corner = 0; corner < 3; ++corner) {
            const uint32_t a = uint32_t(face[corner]), b = uint32_t(face[(corner + 1) % 3]);
            if (a != b) edges.push_back({(uint64_t(std::min(a, b)) << 32) | std::max(a, b), uint32_t(id)});
        }
    }
    std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) { return a.vertices < b.vertices; });
    std::vector<std::pair<uint32_t, uint32_t>> neighbours;
    for (size_t first = 0; first < edges.size();) {
        size_t end = first + 1; bool touches_skin = false;
        while (end < edges.size() && edges[end].vertices == edges[first].vertices) ++end;
        for (size_t edge = first; edge < end; ++edge) {
            const uint32_t id = edges[edge].face;
            if (analysis.face_confidence[id] >= minimum_confidence &&
                (analysis.face_labels[id] == Label::FaceSkin || analysis.face_labels[id] == Label::BodySkin ||
                 analysis.face_labels[id] == Label::EyeSclera || analysis.face_labels[id] == Label::Iris ||
                 analysis.face_labels[id] == Label::Eyebrow)) touches_skin = true;
        }
        // A dark uncertain iris/eyelid or skin contour must not become hair.
        // The face model need not expose eye-specific landmark topology here.
        if (touches_skin) for (size_t edge = first; edge < end; ++edge) eligible[edges[edge].face] = 0;
        if (end - first == 2 && edges[first].face != edges[first + 1].face) {
            const uint32_t a = edges[first].face, b = edges[first + 1].face;
            // With a shared edge, equal sums mean the third index is equal as
            // well: reject duplicate triangles of either winding.
            if (source.mesh.indices[a].sum() != source.mesh.indices[b].sum()) neighbours.emplace_back(a, b);
        }
        first = end;
    }
    // The full edge list was needed to detect nonmanifold and skin boundaries.
    // Propagation now uses just the ordinary shared-edge pairs.
    std::vector<Edge>().swap(edges);
    std::vector<float> cost(count, 0.f), proposed_cost(count);
    std::vector<uint32_t> proposed(count);
    // Fill only a narrow uncertain border. Shared edges, original appearance,
    // and a fixed seed constrain every step; accepted skin/clothes never move.
    // Proposals are applied together, so face traversal cannot cause a flood.
    for (uint8_t step = 0; step < 3; ++step) {
        std::fill(proposed.begin(), proposed.end(), no_face);
        std::fill(proposed_cost.begin(), proposed_cost.end(), std::numeric_limits<float>::max());
        const auto propose = [&](uint32_t from, uint32_t to) {
            if (hops[from] != step || !eligible[to] || owner[to] != no_face) return;
            const auto seed = owner[from];
            // Three very large triangles must not turn a narrow repair into a
            // broad fill. Keep the radius relative to the original model size.
            if ((positions[to] - positions[seed]).squaredNorm() > maximum_distance_squared) return;
            if (normals[from].dot(normals[to]) < .75f || !same_hair_material(colors[to], seeds[seed].appearance)) return;
            const float difference = distance(colors[from], colors[to]), total = cost[from] + difference;
            if (difference > .0016f || total > .0025f) return;
            if (total < proposed_cost[to] || (total == proposed_cost[to] && seed < proposed[to])) {
                proposed_cost[to] = total; proposed[to] = seed;
            }
        };
        for (const auto& pair : neighbours) {
            propose(pair.first, pair.second);
            propose(pair.second, pair.first);
        }
        bool changed = false;
        for (size_t id = 0; id < count; ++id) if (proposed[id] != no_face) {
            owner[id] = proposed[id]; hops[id] = step + 1; cost[id] = proposed_cost[id];
            output.emplace_back(id, palette[size_t(seeds[owner[id]].slot)]); changed = true;
        }
        if (!changed) break;
    }
}

} // namespace

bool RGBImage::valid() const
{
    return width > 0 && height > 0 && width <= 4096 && height <= 4096 &&
        pixels.size() == size_t(width) * height * 3;
}
bool Prediction::valid_for(const RGBImage& image) const
{
    if (!image.valid() || canceled || !error.empty() || labels.size() != size_t(image.width) * image.height ||
        confidence.size() != labels.size()) return false;
    for (size_t i = 0; i < labels.size(); ++i)
        if (!valid_label(labels[i]) || !std::isfinite(confidence[i]) || confidence[i] < 0 || confidence[i] > 1) return false;
    return true;
}

std::string content_fingerprint(const MeshSnapshot& source)
{
    if (source.geometry_id.empty() || !validate_snapshot(source).empty()) return {};
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) return {};
    const std::string prefix = std::string("orca.semantic-source/v1:") + source.geometry_id + ":" + std::to_string(source.mesh.indices.size()) + ":";
    if (EVP_DigestUpdate(ctx.get(), prefix.data(), prefix.size()) != 1) return {};
    std::array<unsigned char, 1024 * 3 * 3 * 4> buffer {};
    size_t used = 0;
    for (size_t face = 0; face < source.mesh.indices.size(); ++face) {
        for (int corner = 0; corner < 3; ++corner) {
            for (float color : corner_color(source, face, corner)) {
                const float normalized = color == 0.f ? 0.f : color;
                uint32_t bits; std::memcpy(&bits, &normalized, sizeof(bits));
                for (size_t byte = 0; byte < 4; ++byte) buffer[used++] = static_cast<unsigned char>(bits >> (byte * 8));
            }
        }
        if (used == buffer.size()) { if (EVP_DigestUpdate(ctx.get(), buffer.data(), used) != 1) return {}; used = 0; }
    }
    if (used && EVP_DigestUpdate(ctx.get(), buffer.data(), used) != 1) return {};
    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes {}; unsigned int count = 0;
    if (EVP_DigestFinal_ex(ctx.get(), bytes.data(), &count) != 1 || count != 32) return {};
    constexpr char hex[] = "0123456789abcdef";
    std::string result; result.reserve(64);
    for (unsigned int i = 0; i < count; ++i) { result += hex[bytes[i] >> 4]; result += hex[bytes[i] & 15]; }
    return result;
}

RenderedView render_view(const MeshSnapshot& source, float yaw_degrees, int image_size, const Cancel& cancel)
{
    return render_region(source, yaw_degrees, {}, image_size, cancel);
}

RenderedView render_region(const MeshSnapshot& source, float yaw_degrees, const ViewRegion& region,
                           int image_size, const Cancel& cancel)
{
    RenderedView result;
    result.error = validate_snapshot(source);
    if (!result.error.empty()) return result;
    if (image_size < 8 || image_size > 2048 || !std::isfinite(yaw_degrees) ||
        !std::isfinite(region.left) || !std::isfinite(region.top) || !std::isfinite(region.width) || !std::isfinite(region.height) ||
        region.left < 0 || region.top < 0 || region.width < 1.f/4096 || region.height < 1.f/4096 ||
        region.left + region.width > 1.000001f || region.top + region.height > 1.000001f) {
        result.error = "Invalid semantic view dimensions or camera."; return result;
    }
    if (stopped(cancel)) { result.canceled = true; return result; }
    constexpr float radians = 3.14159265358979323846f / 180.f;
    const float yaw = yaw_degrees * radians;
    const Vec3f direction = Vec3f(std::sin(yaw), -std::cos(yaw), .10f).normalized();
    const Vec3f right(std::cos(yaw), std::sin(yaw), 0.f);
    const Vec3f up = direction.cross(right).normalized();
    std::vector<Vec3f> screen; screen.reserve(source.mesh.vertices.size());
    float min_x = std::numeric_limits<float>::max(), min_y = min_x, max_x = -min_x, max_y = -min_x;
    for (const auto& vertex : source.mesh.vertices) {
        const Vec3f p(vertex.dot(right), vertex.dot(up), vertex.dot(direction)); screen.push_back(p);
        min_x = std::min(min_x, p.x()); max_x = std::max(max_x, p.x());
        min_y = std::min(min_y, p.y()); max_y = std::max(max_y, p.y());
    }
    const float span = std::max(max_x - min_x, max_y - min_y);
    if (!std::isfinite(span) || span <= 0) { result.error = "The semantic view has no projected surface."; return result; }
    // Use one normalized full-view camera regardless of target size. ROI
    // magnification changes the viewport transform only, not mesh coordinates
    // or the camera's center, depth convention, and source face ordering.
    const float scale = (.88f * 511.f / 512.f) / span;
    const float magnification = image_size / std::max(region.width, region.height);
    const float offset_x = (image_size - region.width * magnification) * .5f;
    const float offset_y = (image_size - region.height * magnification) * .5f;
    for (auto& p : screen) {
        p.x() = (.5f + (p.x() - (min_x + max_x) * .5f) * scale - region.left) * magnification + offset_x;
        p.y() = (.5f - (p.y() - (min_y + max_y) * .5f) * scale - region.top) * magnification + offset_y;
    }
    const size_t pixel_count = size_t(image_size) * image_size;
    result.image = {image_size, image_size, std::vector<uint8_t>(pixel_count * 3, 128)};
    result.face_ids.assign(pixel_count, no_face);
    result.barycentric.assign(pixel_count, {});
    result.depth.assign(pixel_count, -std::numeric_limits<float>::infinity());
    result.facing.assign(source.mesh.indices.size(), 0.f);
    const auto edge = [](const Vec3f& a, const Vec3f& b, float x, float y) {
        return (b.x() - a.x()) * (y - a.y()) - (b.y() - a.y()) * (x - a.x());
    };
    for (size_t face = 0; face < source.mesh.indices.size(); ++face) {
        if ((face & 1023) == 0 && stopped(cancel)) { result = {}; result.canceled = true; return result; }
        const auto& f = source.mesh.indices[face];
        const auto& a = screen[f[0]]; const auto& b = screen[f[1]]; const auto& c = screen[f[2]];
        const float cross = edge(a, b, c.x(), c.y());
        if (std::abs(cross) < 1e-9f) continue;
        const Vec3f normal = (source.mesh.vertices[f[1]] - source.mesh.vertices[f[0]])
            .cross(source.mesh.vertices[f[2]] - source.mesh.vertices[f[0]]);
        if (normal.squaredNorm() <= 1e-18f) continue;
        result.facing[face] = std::max(.05f, std::abs(normal.normalized().dot(direction)));
        const int left = std::max(0, int(std::floor(std::min({a.x(), b.x(), c.x()}))));
        const int right_pixel = std::min(image_size - 1, int(std::ceil(std::max({a.x(), b.x(), c.x()}))));
        const int top = std::max(0, int(std::floor(std::min({a.y(), b.y(), c.y()}))));
        const int bottom = std::min(image_size - 1, int(std::ceil(std::max({a.y(), b.y(), c.y()}))));
        const auto ca = corner_color(source, face, 0), cb = corner_color(source, face, 1), cc = corner_color(source, face, 2);
        for (int y = top; y <= bottom; ++y) for (int x = left; x <= right_pixel; ++x) {
            const float wa = edge(b, c, x + .5f, y + .5f) / cross;
            const float wb = edge(c, a, x + .5f, y + .5f) / cross;
            const float wc = 1.f - wa - wb;
            if (wa < -1e-6f || wb < -1e-6f || wc < -1e-6f) continue;
            const float depth = wa*a.z() + wb*b.z() + wc*c.z();
            const size_t pixel = size_t(y) * image_size + x;
            if (depth <= result.depth[pixel]) continue;
            result.depth[pixel] = depth; result.face_ids[pixel] = uint32_t(face);
            result.barycentric[pixel] = {wa, wb, wc};
            for (int channel = 0; channel < 3; ++channel)
                result.image.pixels[pixel * 3 + channel] = uint8_t(std::lround(std::clamp(
                    wa*ca[channel] + wb*cb[channel] + wc*cc[channel], 0.f, 1.f) * 255));
        }
    }
    std::vector<uint8_t> directly_seen(source.mesh.indices.size(), 0);
    for (uint32_t face : result.face_ids) if (face != no_face) directly_seen[face] = 1;
    result.surface_samples.assign(source.mesh.indices.size(), no_face);
    const float depth_tolerance = std::max(span * 1e-7f, .10f / (scale * magnification));
    for (size_t face = 0; face < source.mesh.indices.size(); ++face) {
        if ((face & 4095) == 0 && stopped(cancel)) { result = {}; result.canceled = true; return result; }
        if (directly_seen[face] || result.facing[face] < .15f) continue;
        const auto& f = source.mesh.indices[face];
        const auto& a = screen[f[0]]; const auto& b = screen[f[1]]; const auto& c = screen[f[2]];
        const float cross = edge(a, b, c.x(), c.y());
        if (std::abs(cross) < 1e-9f) continue;
        const float cx = (a.x() + b.x() + c.x()) / 3.f, cy = (a.y() + b.y() + c.y()) / 3.f;
        if (cx < 0 || cy < 0 || cx >= image_size || cy >= image_size) continue;
        const int x = int(cx), y = int(cy);
        const size_t pixel = size_t(y) * image_size + x;
        const uint32_t neighbor = result.face_ids[pixel]; if (neighbor == no_face) continue;
        // Evaluate this face's plane at the neighboring pixel center. Comparing
        // raw centroid depth would incorrectly reject an inclined surface.
        const float wa = edge(b, c, x + .5f, y + .5f) / cross;
        const float wb = edge(c, a, x + .5f, y + .5f) / cross;
        const float plane_depth = wa*a.z() + wb*b.z() + (1.f-wa-wb)*c.z();
        if (std::abs(plane_depth - result.depth[pixel]) > depth_tolerance) continue;
        const auto& n = source.mesh.indices[neighbor];
        const Vec3f normal = (source.mesh.vertices[f[1]] - source.mesh.vertices[f[0]])
            .cross(source.mesh.vertices[f[2]] - source.mesh.vertices[f[0]]);
        const Vec3f neighbor_normal = (source.mesh.vertices[n[1]] - source.mesh.vertices[n[0]])
            .cross(source.mesh.vertices[n[2]] - source.mesh.vertices[n[0]]);
        if (normal.squaredNorm() <= 1e-18f || neighbor_normal.squaredNorm() <= 1e-18f ||
            normal.normalized().dot(neighbor_normal.normalized()) < .94f) continue;
        result.surface_samples[face] = uint32_t(pixel);
    }
    return result;
}

bool locate_subface(const Barycentric& input, uint8_t depth, SubfacePath& output)
{
    if (depth == 0 || depth > 3 || !std::all_of(input.begin(), input.end(), [](float value) {
            return std::isfinite(value) && value >= -1e-5f && value <= 1.00001f;
        }) || std::abs(input[0] + input[1] + input[2] - 1.f) > 1e-4f)
        return false;
    Barycentric bary {std::max(0.f, input[0]), std::max(0.f, input[1]), std::max(0.f, input[2])};
    uint8_t path = 0;
    for (uint8_t level = 0; level < depth; ++level) {
        uint8_t child = 3;
        if (bary[0] >= .5f) child = 0;
        else if (bary[1] >= .5f) child = 1;
        else if (bary[2] >= .5f) child = 2;
        path = uint8_t((path << 2) | child);
        if (child == 0) bary = {2.f * bary[0] - 1.f, 2.f * bary[1], 2.f * bary[2]};
        else if (child == 1) bary = {2.f * bary[0], 2.f * bary[1] - 1.f, 2.f * bary[2]};
        // TriangleSelector stores corner 2 as (midpoint 1-2, vertex 2,
        // midpoint 2-0), so its local barycentric axes rotate with the child.
        else if (child == 2) bary = {2.f * bary[1], 2.f * bary[2] - 1.f, 2.f * bary[0]};
        else bary = {bary[0] + bary[1] - bary[2], bary[1] + bary[2] - bary[0],
                     bary[2] + bary[0] - bary[1]};
        for (float& value : bary) value = std::max(0.f, value);
        const float sum = bary[0] + bary[1] + bary[2];
        if (sum <= 0.f) return false;
        for (float& value : bary) value /= sum;
    }
    output = {depth, path};
    return true;
}

std::vector<std::pair<uint32_t, SubfacePath>> project_boundary_sample(
    const RenderedView& view, float image_x, float image_y)
{
    std::vector<std::pair<uint32_t, SubfacePath>> result;
    if (!view.image.valid() || view.image.width < 2 || view.image.height < 2 ||
        !std::isfinite(image_x) || !std::isfinite(image_y)) return result;
    const size_t pixel_count = size_t(view.image.width) * size_t(view.image.height);
    if (view.face_ids.size() != pixel_count || view.depth.size() != pixel_count ||
        view.barycentric.size() != pixel_count) return result;

    const float px = image_x - .5f, py = image_y - .5f;
    const int x = std::clamp(int(std::floor(px)), 0, view.image.width - 2);
    const int y = std::clamp(int(std::floor(py)), 0, view.image.height - 2);
    const size_t p0 = size_t(y) * size_t(view.image.width) + size_t(x);
    const std::array<size_t, 4> pixels {
        p0, p0 + 1, p0 + size_t(view.image.width), p0 + size_t(view.image.width) + 1
    };
    const uint32_t face_id = view.face_ids[p0];
    const bool one_face = face_id != no_face && std::all_of(pixels.begin(), pixels.end(), [&](size_t pixel) {
        return view.face_ids[pixel] == face_id && std::isfinite(view.depth[pixel]);
    });
    if (one_face) {
        const float u = std::clamp(px - x, 0.f, 1.f), v = std::clamp(py - y, 0.f, 1.f);
        const std::array<float, 4> factors {(1-u)*(1-v), u*(1-v), (1-u)*v, u*v};
        Barycentric bary {};
        for (size_t corner = 0; corner < pixels.size(); ++corner)
            for (size_t axis = 0; axis < bary.size(); ++axis)
                bary[axis] += view.barycentric[pixels[corner]][axis] * factors[corner];
        SubfacePath parent;
        if (locate_subface(bary, 2, parent)) result.emplace_back(face_id, parent);
        if (!result.empty()) return result;
    }

    // Dense meshes commonly place each corner of a contour cell on a distinct
    // triangle. Interpolating those unrelated barycentric frames is invalid,
    // but discarding the complete cell removes nearly every usable boundary.
    // Project the visible corner samples independently and deduplicate them.
    for (size_t pixel : pixels) {
        const uint32_t corner_face = view.face_ids[pixel];
        if (corner_face == no_face || !std::isfinite(view.depth[pixel])) continue;
        SubfacePath parent;
        if (!locate_subface(view.barycentric[pixel], 2, parent)) continue;
        const auto projection = std::make_pair(corner_face, parent);
        if (std::find(result.begin(), result.end(), projection) == result.end())
            result.push_back(projection);
    }
    return result;
}

bool subface_vertices(const SubfacePath& path, std::array<Barycentric, 3>& output)
{
    if (path.depth == 0 || path.depth > 3 || unsigned(path.value) >= (1u << (2u * path.depth)))
        return false;
    std::array<Barycentric, 3> vertices {{{1.f,0.f,0.f}, {0.f,1.f,0.f}, {0.f,0.f,1.f}}};
    const auto midpoint = [](const Barycentric& lhs, const Barycentric& rhs) {
        return Barycentric {(lhs[0] + rhs[0]) * .5f, (lhs[1] + rhs[1]) * .5f,
                            (lhs[2] + rhs[2]) * .5f};
    };
    for (uint8_t level = 0; level < path.depth; ++level) {
        const unsigned shift = 2u * unsigned(path.depth - level - 1);
        const unsigned child = (unsigned(path.value) >> shift) & 3u;
        const auto ab = midpoint(vertices[0], vertices[1]);
        const auto bc = midpoint(vertices[1], vertices[2]);
        const auto ca = midpoint(vertices[2], vertices[0]);
        if (child == 0) vertices = {vertices[0], ab, ca};
        else if (child == 1) vertices = {ab, vertices[1], bc};
        else if (child == 2) vertices = {bc, vertices[2], ca};
        else vertices = {ab, bc, ca};
    }
    output = vertices;
    return true;
}

static bool subfaces_share_edge(const SubfacePath& lhs, const SubfacePath& rhs)
{
    if (lhs.depth != rhs.depth || lhs == rhs) return false;
    std::array<Barycentric, 3> lhs_vertices, rhs_vertices;
    if (!subface_vertices(lhs, lhs_vertices) || !subface_vertices(rhs, rhs_vertices)) return false;
    size_t shared = 0;
    for (const Barycentric& lhs_vertex : lhs_vertices)
        if (std::find(rhs_vertices.begin(), rhs_vertices.end(), lhs_vertex) != rhs_vertices.end())
            ++shared;
    return shared == 2;
}

std::string analysis_cache_key(const MeshSnapshot& source, const std::string& body, const std::string& face)
{
    return analysis_cache_key(source, body, face, "none");
}

std::string analysis_cache_key(const MeshSnapshot& source, const std::string& body, const std::string& face,
                               const std::string& boundary)
{
    if (source.geometry_id.empty() || source.content_id.empty() || body.empty() || face.empty() || boundary.empty()) return {};
    return sha256(nlohmann::json::array({pipeline_version, source.geometry_id, source.content_id,
        source.mesh.indices.size(), body, face, boundary,
        "cpu-zbuffer-8x512-face-roi512-visible-source-samples-v2-joint-boundary-v2"}).dump());
}

Analysis analyze(const MeshSnapshot& source, IBodyRegionRecognizer& body, IFaceRegionRecognizer& face,
                 const Cancel& cancel, const Progress& progress)
{
    return analyze(source, body, face, nullptr, cancel, progress);
}

Analysis analyze(const MeshSnapshot& source, IBodyRegionRecognizer& body, IFaceRegionRecognizer& face,
                 IBoundaryRefiner* boundary, const Cancel& cancel, const Progress& progress)
{
    return analyze(source, body, face, boundary, cancel, progress, {});
}

Analysis analyze(const MeshSnapshot& source, IBodyRegionRecognizer& body, IFaceRegionRecognizer& face,
                 IBoundaryRefiner* boundary, const Cancel& cancel, const Progress& progress,
                 const RenderObserver& observe)
{
    Analysis result;
    result.geometry_id = source.geometry_id; result.content_id = source.content_id;
    try {
        result.body_identity = body.identity(); result.face_identity = face.identity();
        result.boundary_identity = boundary ? boundary->identity() : "none";
        result.signature = analysis_cache_key(source, result.body_identity, result.face_identity,
                                              result.boundary_identity);
        result.error = validate_snapshot(source);
        if (!result.error.empty()) return result;
        if (result.signature.empty()) { result.error = "Semantic analysis requires complete model and recognizer identities."; return result; }
        const size_t count = source.mesh.indices.size();
        std::vector<std::array<float, label_count>> votes(count);
        std::vector<float> weights(count, 0.f), detail_weights(count, 0.f);
        std::vector<std::array<float, face_detail_labels.size()>> detail_votes(count);
        std::vector<std::array<float, face_detail_labels.size()>> peak_detail_confidence(count);
        std::vector<float> view_detail_weights(count, 0.f);
        std::vector<std::array<float, face_detail_labels.size()>> view_detail_votes(count);
        std::vector<size_t> view_detail_faces;
        struct LeafVotes {
            float weight {0.f};
            std::array<float, face_detail_labels.size()> votes {};
            std::array<float, face_detail_labels.size()> peak {};
            uint32_t samples {0};
        };
        std::unordered_map<uint64_t, LeafVotes> leaf_votes;
        struct BoundaryLeafVotes {
            float weight {0.f};
            std::array<float, label_count> votes {};
            uint32_t samples {0};
            uint16_t view_mask {0};
        };
        std::unordered_map<uint64_t, BoundaryLeafVotes> boundary_leaf_votes;
        std::unordered_map<size_t, BoundaryLeafVotes> boundary_root_votes;
        std::vector<Color> original; original.reserve(count);
        for (size_t id = 0; id < count; ++id) original.push_back(face_lab(source, id));
        for (int view_index = 0; view_index < 8; ++view_index) {
            if (stopped(cancel)) { result.canceled = true; return result; }
            if (progress) progress(view_index * 100 / 8, "Recognizing model regions");
            auto view = render_view(source, view_index * 45.f, 512, cancel);
            if (view.canceled) { result.canceled = true; return result; }
            if (!view.error.empty()) { result.error = view.error; return result; }
            ++result.rendered_views;
            if (observe) observe(view, view_index, {0.f,0.f,1.f,1.f}, false);
            auto prediction = body.predict(view.image, cancel);
            if (prediction.canceled || stopped(cancel)) { result.canceled = true; return result; }
            if (!prediction.valid_for(view.image)) {
                result.error = prediction.error.empty() ? "The body recognizer returned an invalid mask." : prediction.error; return result;
            }
            if (!refine_dark_hair_mask(view.image, prediction, cancel)) {
                result.canceled = true; return result;
            }
            for (size_t pixel = 0; pixel < view.face_ids.size(); ++pixel) {
                const uint32_t id = view.face_ids[pixel]; if (id == no_face) continue;
                const float weight = view.facing[id]; weights[id] += weight;
                votes[id][size_t(prediction.labels[pixel])] += weight * prediction.confidence[pixel];
            }
            for (size_t id = 0; id < count; ++id) {
                const float confidence = supported_surface_sample(view, prediction, id, original);
                if (confidence <= 0) continue;
                const Label label = prediction.labels[view.surface_samples[id]];
                const float weight = view.facing[id] * .75f;
                weights[id] += weight; votes[id][size_t(label)] += weight * confidence;
            }
            const auto regions = face_regions(view, prediction);
            if (regions.empty()) continue;
            ++result.face_views;
            for (size_t crop_index = 0; crop_index < regions.size(); ++crop_index) {
                const ViewRegion& region = regions[crop_index];
                auto crop = render_region(source, view_index * 45.f, region, 512, cancel);
                if (crop.canceled) { result.canceled = true; return result; }
                if (!crop.error.empty()) { result.error = crop.error; return result; }
                if (observe) observe(crop, view_index, region, true);
                auto details = face.predict(crop.image, cancel);
                if (details.canceled || stopped(cancel)) { result.canceled = true; return result; }
                if (!details.valid_for(crop.image)) {
                    result.error = details.error.empty() ? "The face recognizer returned an invalid mask." : details.error; return result;
                }
                const Prediction coarse_details = details;
                EarHairBoundaryResult boundary_result;
                if (boundary && details.face_detected) {
                    auto crop_body = body.predict(crop.image, cancel);
                    if (crop_body.canceled || stopped(cancel)) { result.canceled = true; return result; }
                    if (crop_body.valid_for(crop.image)) {
                        boundary_result = apply_facial_boundaries(crop.image, crop_body, details, *boundary, cancel);
                        if (boundary_result.prediction.canceled) { result.canceled = true; return result; }
                        for (auto& diagnostic : boundary_result.diagnostics) {
                            diagnostic.view_id = view_index;
                            diagnostic.crop_id = std::to_string(view_index) + ":" + std::to_string(crop_index);
                            if (diagnostic.reason_code.empty()) diagnostic.reason_code = diagnostic.status == "rejected" ?
                                "SEMANTIC_LOW_CONFIDENCE" : diagnostic.status == "error" ?
                                "GEOMETRY_REPROJECT_MISSING" : "NONE";
                            result.boundary_runs.push_back(std::move(diagnostic));
                        }
                        if (boundary_result.refinement.valid_for(crop.image) &&
                            boundary_result.accepted.size() == crop.face_ids.size()) {
                            for (size_t pixel = 0; pixel < crop.face_ids.size(); ++pixel) {
                                const uint32_t id=crop.face_ids[pixel]; if(id==no_face)continue;
                                auto& root=boundary_root_votes[id];const float weight=crop.facing[id];root.weight+=weight;
                                if(!boundary_result.accepted[pixel])continue;
                                const auto label=boundary_result.prediction.labels[pixel];
                                root.votes[size_t(label)]+=weight*boundary_result.prediction.confidence[pixel];
                                if(root.samples!=std::numeric_limits<uint32_t>::max())++root.samples;
                            }
                            // Continuous probability contours identify only the depth-2
                            // parents which actually need a third split. At a face edge
                            // stop interpolation instead of crossing an occlusion.
                            std::set<uint64_t> crossing_parents;
                            for (const auto& segment : boundary_result.contours) {
                                const float length = std::hypot(segment.b[0]-segment.a[0], segment.b[1]-segment.a[1]);
                                const int steps = std::max(1, int(std::ceil(length * 4.f)));
                                for (int step = 0; step <= steps; ++step) {
                                    const float t = float(step) / steps;
                                    const float px = segment.a[0] + t * (segment.b[0]-segment.a[0]);
                                    const float py = segment.a[1] + t * (segment.b[1]-segment.a[1]);
                                    for (const auto& projection : project_boundary_sample(crop, px, py))
                                        crossing_parents.insert(uint64_t(projection.first)*16u+projection.second.value);
                                }
                            }
                            for (size_t pixel = 0; pixel < crop.face_ids.size(); ++pixel) {
                                const uint32_t id = crop.face_ids[pixel];
                                if (id == no_face || !boundary_result.accepted[pixel]) continue;
                                SubfacePath path;
                                if (!locate_subface(crop.barycentric[pixel], 3, path) ||
                                    crossing_parents.count(uint64_t(id)*16u+(path.value>>2))==0) continue;
                                const Label label=boundary_result.prediction.labels[pixel];
                                if(label!=Label::FaceSkin&&label!=Label::Hair&&label!=Label::EyeSclera&&label!=Label::Iris&&label!=Label::Eyebrow)continue;
                                BoundaryLeafVotes& leaf=boundary_leaf_votes[uint64_t(id)*64u+path.value];
                                float color_support=0.f,geometry_support=0.f;
                                const int x=int(pixel%size_t(crop.image.width)),y=int(pixel/size_t(crop.image.width));
                                const std::array<std::array<int,2>,4> offsets {{{-1,0},{1,0},{0,-1},{0,1}}};
                                for(const auto& offset:offsets){const int nx=x+offset[0],ny=y+offset[1];
                                    if(nx<0||ny<0||nx>=crop.image.width||ny>=crop.image.height)continue;
                                    const size_t neighbor=size_t(ny)*crop.image.width+nx;
                                    if(!boundary_result.accepted[neighbor]||boundary_result.prediction.labels[neighbor]==label)continue;
                                    float rgb_delta=0.f;for(int channel=0;channel<3;++channel){
                                        const float delta=float(crop.image.pixels[pixel*3+channel])-float(crop.image.pixels[neighbor*3+channel]);
                                        rgb_delta+=delta*delta;}
                                    color_support=std::max(color_support,std::clamp(std::sqrt(rgb_delta)/(255.f*.35f),0.f,1.f));
                                    if(crop.face_ids[neighbor]!=id)geometry_support=1.f;
                                    else if(std::isfinite(crop.depth[pixel])&&std::isfinite(crop.depth[neighbor])){
                                        const float scale=std::max(1e-4f,std::abs(crop.depth[pixel])*.02f);
                                        geometry_support=std::max(geometry_support,std::clamp(std::abs(crop.depth[pixel]-crop.depth[neighbor])/scale,0.f,1.f));}
                                }
                                const float semantic_support=boundary_result.prediction.confidence[pixel];
                                const float joint_support=.45f*semantic_support+.25f*color_support+.10f*geometry_support;
                                const float weight=crop.facing[id]; leaf.weight+=weight;
                                leaf.votes[size_t(label)]+=weight*joint_support;
                                leaf.view_mask|=uint16_t(1u<<unsigned(view_index));
                                if(leaf.samples!=std::numeric_limits<uint32_t>::max())++leaf.samples;
                            }
                        }
                    }
                    if (details.canceled || stopped(cancel)) { result.canceled = true; return result; }
                }
                // A segmentation mask alone is not proof that a rendered animal
                // or object is a person. Require the independent face detector.
                if (!details.face_detected) continue;
                result.person_detected = true;
                view_detail_faces.clear();
                std::unordered_map<uint64_t, LeafVotes> view_leaf_votes;
                for (size_t pixel = 0; pixel < crop.face_ids.size(); ++pixel) {
                    const uint32_t id = crop.face_ids[pixel]; if (id == no_face) continue;
                    const float weight = crop.facing[id];
                    if (view_detail_weights[id] == 0.f) view_detail_faces.push_back(id);
                    view_detail_weights[id] += weight; detail_weights[id] += weight;
                    const Label label = details.labels[pixel];
                    const size_t detail = face_detail_index(label);
                    if (detail < face_detail_labels.size()) {
                        view_detail_votes[id][detail] += weight * details.confidence[pixel];
                        detail_votes[id][detail] += weight * details.confidence[pixel];
                    }
                    SubfacePath path;
                    if (locate_subface(crop.barycentric[pixel], 2, path)) {
                        LeafVotes& leaf = view_leaf_votes[uint64_t(id) * 16u + path.value];
                        leaf.weight += weight;
                        if (leaf.samples != std::numeric_limits<uint32_t>::max()) ++leaf.samples;
                        const size_t safe_detail = face_detail_index(coarse_details.labels[pixel]);
                        if (safe_detail < face_detail_labels.size())
                            leaf.votes[safe_detail] += weight * coarse_details.confidence[pixel];
                    }
                }
                for (const auto& item : view_leaf_votes) {
                    LeafVotes& accumulated = leaf_votes[item.first];
                    accumulated.weight += item.second.weight;
                    accumulated.samples = item.second.samples > std::numeric_limits<uint32_t>::max() - accumulated.samples
                        ? std::numeric_limits<uint32_t>::max() : accumulated.samples + item.second.samples;
                    for (size_t detail = 0; detail < face_detail_labels.size(); ++detail) {
                        accumulated.votes[detail] += item.second.votes[detail];
                        accumulated.peak[detail] = std::max(accumulated.peak[detail],
                            item.second.weight > 0.f ?
                                std::clamp(item.second.votes[detail] / item.second.weight, 0.f, 1.f) : 0.f);
                    }
                }
                for (size_t id = 0; id < count; ++id) {
                    const float confidence = supported_surface_sample(crop, details, id, original);
                    if (confidence <= 0) continue;
                    const Label label = details.labels[crop.surface_samples[id]];
                    const size_t detail = face_detail_index(label);
                    if (detail == face_detail_labels.size()) continue;
                    const float weight = crop.facing[id] * .75f;
                    if (view_detail_weights[id] == 0.f) view_detail_faces.push_back(id);
                    view_detail_weights[id] += weight;
                    view_detail_votes[id][detail] += weight * confidence;
                    detail_weights[id] += weight;
                    detail_votes[id][detail] += weight * confidence;
                }
                for (size_t id : view_detail_faces) {
                    for (size_t detail = 0; detail < face_detail_labels.size(); ++detail) {
                        peak_detail_confidence[id][detail] = std::max(peak_detail_confidence[id][detail],
                            std::clamp(view_detail_votes[id][detail] / view_detail_weights[id], 0.f, 1.f));
                        view_detail_votes[id][detail] = 0.f;
                    }
                    view_detail_weights[id] = 0.f;
                }
            }
        }
        if (stopped(cancel)) { result.canceled = true; return result; }
        result.face_labels.assign(count, Label::Unknown); result.face_confidence.assign(count, 0.f);
        for (size_t id = 0; id < count; ++id) {
            if (weights[id] <= 0 && detail_weights[id] <= 0) continue;
            ++result.observed_faces;
            if (weights[id] > 0) {
                const size_t chosen = size_t(std::max_element(votes[id].begin(), votes[id].end()) - votes[id].begin());
                result.face_labels[id] = Label(chosen);
                result.face_confidence[id] = std::clamp(votes[id][chosen] / weights[id], 0.f, 1.f);
            }
            // Details replace a face only when they cover most of its visible
            // samples. A tiny lip pixel cannot recolor a coarse cheek triangle.
            if (detail_weights[id] > 0) {
                const size_t detail = size_t(std::max_element(detail_votes[id].begin(), detail_votes[id].end()) - detail_votes[id].begin());
                const Label detail_label = face_detail_labels[detail];
                const float accumulated = std::clamp(detail_votes[id][detail] / detail_weights[id], 0.f, 1.f);
                const float recognized = std::accumulate(detail_votes[id].begin(), detail_votes[id].end(), 0.f);
                const float dominance = recognized > 0.f ? std::clamp(detail_votes[id][detail] / recognized, 0.f, 1.f) : 0.f;
                float confidence = accumulated;
                // Brow geometry may only be visible in one face crop, so a
                // dominant best view may survive Unknown side views. Eye,
                // lip and mouth details retain the stricter accumulated rule;
                // recovering their partial triangles needs subface boundaries.
                if (detail_label == Label::Eyebrow && dominance >= minimum_confidence)
                    confidence = std::max(confidence,
                        std::min(peak_detail_confidence[id][detail], dominance));
                if (confidence >= minimum_confidence) {
                    result.face_labels[id] = detail_label;
                    result.face_confidence[id] = confidence;
                }
            }
            if (paintable(result.face_labels[id]) && result.face_confidence[id] >= minimum_confidence) ++result.reliable_faces;
        }
        result.subface_labels.reserve(std::min(leaf_votes.size() + boundary_leaf_votes.size(),
                                                maximum_subface_evidence));
        for (const auto& item : leaf_votes) {
            if (item.second.weight <= 0.f || item.second.samples == 0) continue;
            const size_t detail = size_t(std::max_element(item.second.votes.begin(), item.second.votes.end()) -
                                         item.second.votes.begin());
            const Label label = face_detail_labels[detail];
            const size_t face_id = size_t(item.first / 16u);
            if (face_id >= count) continue;
            const bool supported_skin_boundary = label == Label::FaceSkin &&
                (result.face_labels[face_id] == Label::EyeSclera ||
                 result.face_labels[face_id] == Label::Hair);
            if (label != Label::EyeSclera && label != Label::Iris && label != Label::Eyebrow &&
                !supported_skin_boundary) continue;
            const float recognized = std::accumulate(item.second.votes.begin(), item.second.votes.end(), 0.f);
            const float accumulated = std::clamp(item.second.votes[detail] / item.second.weight, 0.f, 1.f);
            const float dominance = recognized > 0.f ?
                std::clamp(item.second.votes[detail] / recognized, 0.f, 1.f) : 0.f;
            const float confidence = std::max(accumulated,
                dominance >= minimum_confidence ? std::min(item.second.peak[detail], dominance) : 0.f);
            // Dense generated meshes often give a depth-2 leaf only one direct
            // pixel in the 512px face crop. Keep such evidence only when the
            // model is correspondingly more certain; three-pixel leaves retain
            // the ordinary semantic threshold.
            const float evidence_threshold = supported_skin_boundary ?
                (item.second.samples >= 2 ? .82f : .86f) :
                (item.second.samples >= 3 ? minimum_confidence : item.second.samples == 2 ? .80f : .90f);
            if (confidence < evidence_threshold || dominance < evidence_threshold) continue;
            if (result.subface_labels.size() >= maximum_subface_evidence) {
                result.error = "The face-detail boundary evidence exceeds the local processing limit.";
                result.subface_labels.clear();
                return result;
            }
            result.subface_labels.push_back({face_id, {2, uint8_t(item.first % 16u)}, label,
                                             confidence, item.second.samples});
        }
        std::sort(result.subface_labels.begin(), result.subface_labels.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.face_id != rhs.face_id ? lhs.face_id < rhs.face_id : lhs.path < rhs.path;
        });
        result.baseline_subface_labels = result.subface_labels;
        if(boundary){
            result.baseline_face_labels=result.face_labels;
            result.baseline_face_confidence=result.face_confidence;
            for(const auto& item:boundary_root_votes){
                if(item.second.weight<=0.f||item.second.samples==0)continue;
                const size_t chosen=size_t(std::max_element(item.second.votes.begin(),item.second.votes.end())-item.second.votes.begin());
                const float confidence=std::clamp(item.second.votes[chosen]/item.second.weight,0.f,1.f);
                if(confidence>=minimum_confidence && item.first < result.baseline_face_labels.size() &&
                   compatible_boundary_detail(result.baseline_face_labels[item.first], Label(chosen))) {
                    result.face_labels[item.first]=Label(chosen);result.face_confidence[item.first]=confidence;
                }
            }
            result.reliable_faces=0;
            for(size_t id=0;id<count;++id)if(paintable(result.face_labels[id])&&result.face_confidence[id]>=minimum_confidence)++result.reliable_faces;
        }
        std::map<std::pair<size_t,uint8_t>,SubfaceLabelEvidence> refined;
        for(const auto& item:boundary_leaf_votes){
            if(item.second.weight<=0.f||item.second.samples==0)continue;
            const size_t face_id=size_t(item.first/64u);if(face_id>=count)continue;
            const size_t chosen=size_t(std::max_element(item.second.votes.begin(),item.second.votes.end())-item.second.votes.begin());
            unsigned views=0;for(uint16_t bits=item.second.view_mask;bits;bits>>=1)views+=bits&1u;
            const float multi_view_support=std::min(1.f,float(views)/2.f);
            const float confidence=std::clamp(item.second.votes[chosen]/item.second.weight+
                                              .20f*multi_view_support,0.f,1.f);
            if(confidence<.70f)continue;
            const uint8_t path=uint8_t(item.first%64u);
            const Label refined_label = Label(chosen);
            if (face_id < result.baseline_face_labels.size() &&
                !compatible_boundary_detail(result.baseline_face_labels[face_id], refined_label)) continue;
            refined.emplace(std::make_pair(face_id,path),SubfaceLabelEvidence{face_id,{3,path},refined_label,confidence,item.second.samples});
        }
        // Expand only a replaced parent. Its omitted children inherit verified
        // depth-2 evidence, keeping an unambiguous tree and a rollback layer.
        for (const auto& safe : result.baseline_subface_labels) {
            if (!protected_face_detail(safe.label)) continue;
            for (int child = 0; child < 4; ++child) {
                const auto key = std::make_pair(safe.face_id, uint8_t(safe.path.value * 4 + child));
                const auto found = refined.find(key);
                if (found != refined.end() && !compatible_boundary_detail(safe.label, found->second.label))
                    refined.erase(found);
            }
        }
        result.subface_labels.clear();
        for(const auto& safe:result.baseline_subface_labels){
            bool split=false;for(int child=0;child<4;++child)split|=refined.count({safe.face_id,uint8_t(safe.path.value*4+child)})!=0;
            if(!split){result.subface_labels.push_back(safe);continue;}
            for(int child=0;child<4;++child){const uint8_t path=uint8_t(safe.path.value*4+child);if(refined.count({safe.face_id,path})==0){auto inherited=safe;inherited.path={3,path};result.subface_labels.push_back(inherited);}}
        }
        for(const auto& item:refined)result.subface_labels.push_back(item.second);
        if(result.subface_labels.size()>maximum_subface_evidence){result.subface_labels=result.baseline_subface_labels;}
        std::sort(result.subface_labels.begin(), result.subface_labels.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.face_id != rhs.face_id ? lhs.face_id < rhs.face_id : lhs.path < rhs.path;
        });
        if (!boundary) result.baseline_subface_labels.clear();
        if (progress) progress(100, "Model region recognition complete");
    } catch (const std::exception& error) { result.error = error.what(); result.face_labels.clear(); result.face_confidence.clear(); }
    return result;
}

namespace {
struct PalettePolicy {
    const std::vector<Color>& palette;
    std::vector<Color> palette_labs;
    std::array<size_t, 6> role_slots {};
    bool has_card {false};
    PalettePolicy(const std::vector<Color>& colors, const std::vector<Color>& portrait_card) : palette(colors) {
        for (const auto& color : palette) palette_labs.push_back(lab(color));

        role_slots.fill(palette.size());
        has_card = portrait_card.size() == 6;
    if (has_card) for (size_t role = 0; role < 6; ++role) {
        for (size_t slot = 0; slot < palette.size(); ++slot) {
            bool same = true;
            for (int channel = 0; channel < 3; ++channel)
                same = same && std::abs(palette[slot][channel] - portrait_card[role][channel]) <= .5f / 255.f;
            if (same) { role_slots[role] = slot; break; }
        }
    }
    }
    bool role_available(size_t role) const { return has_card && role_slots[role] < palette.size(); }
    bool source_supports_role(const Color& center, Label label, size_t role) const {
        if (!has_card || role >= anchors.size()) return false;
        const float source_chroma = chroma(center);
        const float anchor_chroma = chroma(anchors[role]);
        // A role binding is a suggestion, not permission to overwrite a
        // materially incompatible source. In particular, a dark neutral iris
        // must not become the portrait card's blue iris slot merely because
        // its Oklab b channel is slightly negative after lighting removal.
        if (label == Label::Iris && role == 4) {
            if (source_chroma < .04f || anchor_chroma < .04f) return false;
            const float dot = center[1] * anchors[role][1] + center[2] * anchors[role][2];
            if (dot <= 0.f) return false;
            const float source_norm = std::sqrt(center[1] * center[1] + center[2] * center[2]);
            const float anchor_norm = std::sqrt(anchors[role][1] * anchors[role][1] + anchors[role][2] * anchors[role][2]);
            // Hue agreement is deliberately loose for small printed palettes,
            // while still rejecting neutral/brown material as blue.
            if (source_norm <= 0.f || anchor_norm <= 0.f || dot / (source_norm * anchor_norm) < .25f) return false;
        }
        // The other portrait roles already have label-specific guards below;
        // only the blue-iris override needs this additional source check.
        return label != Label::Iris || role != 4 || distance(center, anchors[role]) <= .09f;
    }
    // These are appearance anchors from the existing portrait-card mapping,
    // not output colors. Comparing source colors with pastel print colors would
    // turn warm gray clothing into skin and dark green clothing into black.
    const std::array<Color, 6> anchors {{
        lab({.76f,.55f,.41f}), lab({.12f,.11f,.12f}), lab({.94f,.94f,.94f}),
        lab({.68f,.26f,.24f}), lab({.24f,.39f,.38f}), lab({.40f,.39f,.38f})
    }};
    RegionPaletteDecision decide(const Color& center, Label label, bool boundary_supported = false) const {
        RegionPaletteDecision result;
        result.selected_index = palette.size();
        const float source_chroma = chroma(center);
        const bool has_neutral = std::any_of(palette_labs.begin(), palette_labs.end(),
            [](const Color& color) { return chroma(color) < .035f; });
        const bool has_dark_natural = std::any_of(palette_labs.begin(), palette_labs.end(),
            [&](const Color& color) { return color[0] <= center[0] + .10f && natural_dark_hair(color); });
        const bool has_detail_dark = std::any_of(palette_labs.begin(), palette_labs.end(),
            [](const Color& color) { return color[0] <= .72f; });
        const bool has_bright_neutral = std::any_of(palette_labs.begin(), palette_labs.end(),
            [](const Color& color) { return chroma(color) <= .055f && color[0] >= .78f; });
        const bool has_dark_neutral = std::any_of(palette_labs.begin(), palette_labs.end(),
            [](const Color& color) { return chroma(color) <= .08f && color[0] <= .42f; });
        float brightest_neutral_l = 0.f;
        for (const Color& color : palette_labs)
            if (chroma(color) <= .055f) brightest_neutral_l = std::max(brightest_neutral_l, color[0]);
        const bool has_iris_compatible = std::any_of(palette_labs.begin(), palette_labs.end(),
            [&](const Color& color) { return chroma(color) <= source_chroma + .035f; });

        size_t nearest_anchor = anchors.size();
        if (has_card) {
            float best = std::numeric_limits<float>::max();
            for (size_t role = 0; role < anchors.size(); ++role) {
                const float candidate = distance(center, anchors[role]);
                if (candidate < best) { best = candidate; nearest_anchor = role; }
            }
        }

        for (size_t slot = 0; slot < palette.size(); ++slot) {
            const Color& target = palette_labs[slot];
            const float target_chroma = chroma(target);
            RegionPaletteCandidateDecision candidate;
            candidate.palette_index = slot;
            if (label == Label::EyeSclera && has_neutral &&
                (target_chroma > .06f || target[0] < .35f ||
                 (has_bright_neutral && target[0] < brightest_neutral_l - .03f))) {
                candidate.accepted = false;
                candidate.reason = PaletteDecisionReason::ProtectedRegionConflict;
            } else if (label == Label::MouthInterior && has_dark_neutral &&
                       (target_chroma > .08f || target[0] > .55f)) {
                candidate.accepted = false;
                candidate.reason = PaletteDecisionReason::ProtectedRegionConflict;
            } else if (label == Label::Iris && has_iris_compatible &&
                       (target_chroma > source_chroma + .035f || (has_detail_dark && target[0] > .72f) ||
                        (has_dark_neutral && center[1] >= -.04f && center[2] >= -.015f && target[0] > .58f))) {
                candidate.accepted = false;
                candidate.reason = PaletteDecisionReason::SourceColorIncompatible;
            } else if (label == Label::Eyebrow && has_detail_dark && target[0] > .72f) {
                candidate.accepted = false;
                candidate.reason = PaletteDecisionReason::ProtectedRegionConflict;
            } else if ((label == Label::Hair || label == Label::Iris) &&
                       natural_dark_hair(center) && has_dark_natural &&
                       (target[0] > center[0] + .10f || !natural_dark_hair(target))) {
                candidate.accepted = false;
                candidate.reason = PaletteDecisionReason::SourceColorIncompatible;
            } else if (label == Label::Hair && center[2] > center[1] + .05f &&
                       target[1] > target[2] + .02f && role_available(3) && slot == role_slots[3]) {
                candidate.accepted = false;
                candidate.reason = PaletteDecisionReason::ProtectedRegionConflict;
            } else if (label == Label::Hair && source_chroma > .10f && target_chroma > .04f &&
                       center[1] * target[1] + center[2] * target[2] < 0.f) {
                candidate.accepted = false;
                candidate.reason = PaletteDecisionReason::SourceColorIncompatible;
            } else if (source_chroma < .015f && has_neutral && target_chroma >= .035f) {
                candidate.accepted = false;
                candidate.reason = PaletteDecisionReason::SourceColorIncompatible;
            } else if ((label == Label::FaceSkin || label == Label::BodySkin) && role_available(3) &&
                       slot == role_slots[3] && role_available(0) &&
                       !compatible_skin(center, target, label)) {
                candidate.accepted = false;
                candidate.reason = PaletteDecisionReason::ProtectedRegionConflict;
            } else if ((label == Label::FaceSkin || label == Label::BodySkin) &&
                       target_chroma > .08f && target[1] > target[2] + .025f && has_neutral) {
                candidate.accepted = false;
                candidate.reason = PaletteDecisionReason::ProtectedRegionConflict;
            }

            float role_bonus = 0.f;
            const bool skin = label == Label::FaceSkin || label == Label::BodySkin;
            if (skin && role_available(0) && slot == role_slots[0] && source_chroma >= .015f &&
                center[1] >= 0.f && center[2] > 0.f) role_bonus = .10f;
            else if (label == Label::Lips && role_available(3) && slot == role_slots[3] &&
                     source_chroma >= .04f && center[1] > .02f && center[2] > -.03f) role_bonus = .10f;
            else if ((label == Label::EyeSclera || label == Label::MouthInterior) &&
                     role_available(2) && slot == role_slots[2]) role_bonus = .10f;
            else if (label == Label::Eyebrow && role_available(5) && slot == role_slots[5] &&
                     center[0] < .72f && source_chroma < .12f) role_bonus = .10f;
            else if (label == Label::Iris) {
                const size_t role = center[2] < -.015f || center[1] < -.04f ? 4 : 1;
                if (role_available(role) && slot == role_slots[role] && source_supports_role(center, label, role))
                    role_bonus = .10f;
            } else if (label == Label::Hair && role_available(1) && slot == role_slots[1] &&
                       natural_dark_hair(center)) role_bonus = .08f;
            else if (label == Label::Unknown && has_card && nearest_anchor < anchors.size() && role_available(nearest_anchor) &&
                     slot == role_slots[nearest_anchor]) role_bonus = .06f;

            float semantic_compatibility = candidate_semantic_compatibility(center, target, label, boundary_supported);
            if (label == Label::Clothes && role_available(0) && slot == role_slots[0]) {
                float alternative = std::numeric_limits<float>::max();
                for (size_t other = 0; other < palette_labs.size(); ++other)
                    if (other != role_slots[0]) alternative = std::min(alternative, distance(center, palette_labs[other]));
                if (alternative <= distance(center, target) + .02f)
                    semantic_compatibility = std::min(semantic_compatibility, .35f);
            }
            if (role_bonus > 0.f) semantic_compatibility = 1.f;
            // A known portrait role is still semantic evidence, but it enters
            // the same auditable candidate calculation instead of bypassing it.
            // Only source appearances compatible with the role may penalize an
            // alternative, so dyed hair/irises and scarce palettes remain free.
            if (skin && role_available(0) && distance(center, anchors[0]) < .035f && slot != role_slots[0])
                semantic_compatibility = std::min(semantic_compatibility, .35f);
            else if (label == Label::Lips && role_available(3) && distance(center, anchors[3]) < .04f && slot != role_slots[3])
                semantic_compatibility = std::min(semantic_compatibility, .40f);
            else if ((label == Label::EyeSclera || label == Label::MouthInterior) && role_available(2) &&
                     source_chroma < .10f && slot != role_slots[2])
                semantic_compatibility = std::min(semantic_compatibility, .45f);
            else if (label == Label::Eyebrow && role_available(5) && center[0] < .72f &&
                     source_chroma < .12f && slot != role_slots[5])
                semantic_compatibility = std::min(semantic_compatibility, .05f);
            const auto scored = score_palette_candidate({distance(center, target),
                semantic_compatibility, 1.f, 1.f, 1.f, 0.f, false});
            candidate.source_cost = scored.source_component;
            candidate.semantic_cost = scored.semantic_component;
            candidate.role_bonus = role_bonus;
            const float excess = std::max(0.f, target_chroma - source_chroma - .025f);
            candidate.total_cost = (.35f * candidate.source_cost + .30f * candidate.semantic_cost) / .65f
                                 - role_bonus + 2.f * excess * excess;
            result.candidates.push_back(candidate);
        }

        std::vector<const RegionPaletteCandidateDecision*> accepted;
        for (const auto& candidate : result.candidates) if (candidate.accepted) accepted.push_back(&candidate);
        if (accepted.empty()) for (const auto& candidate : result.candidates) accepted.push_back(&candidate);
        std::stable_sort(accepted.begin(), accepted.end(), [](const auto* lhs, const auto* rhs) {
            return lhs->total_cost != rhs->total_cost ? lhs->total_cost < rhs->total_cost :
                   lhs->palette_index < rhs->palette_index;
        });
        if (!accepted.empty()) {
            result.selected_index = accepted.front()->palette_index;
            result.best_cost = accepted.front()->total_cost;
            if (accepted.size() > 1) {
                result.second_cost = accepted[1]->total_cost;
                result.score_margin = result.second_cost - result.best_cost;
                result.ambiguous = result.score_margin < .03f &&
                    std::sqrt(distance(palette_labs[accepted[0]->palette_index],
                                       palette_labs[accepted[1]->palette_index])) > .04f;
            }
        }
        return result;
    }
    size_t choose(const Color& center, Label label, bool boundary_supported = false) const {
        return decide(center, label, boundary_supported).selected_index;
    }
};

struct DiscoveredMaterial {
    Label label;
    Color center;
    std::vector<size_t> faces;
    double surface_area {0.0};
};
// No palette, target color or slot identity is stored in this immutable cache.
struct MaterialDiscovery {
    std::vector<DiscoveredMaterial> entries;
    std::vector<size_t> face_material;
    std::vector<uint8_t> hair_seed_faces;
    std::set<std::pair<size_t,size_t>> contrast_pairs;
    std::map<std::pair<size_t,Label>,size_t> leaf_component_at_face;
    std::map<size_t,std::vector<Color>> leaf_centers;
    std::map<size_t,Color> sclera_centers;
    std::vector<size_t> sclera_support_component;
    std::vector<uint8_t> eyebrow_supported_faces;
};
struct MappedMaterial { Label label; Color center; size_t target; };
struct MappedMaterials {
    std::vector<MappedMaterial> entries;
    std::shared_ptr<const MaterialDiscovery> discovery;
};
struct DiscoveryCache {
    struct Entry { std::string key; std::shared_ptr<const MaterialDiscovery> value; uint64_t used {0}; };
    std::mutex mutex;
    std::array<Entry,2> entries;
    MaterialDiscoveryCacheStats stats;
    uint64_t clock {0};
};
DiscoveryCache& discovery_cache() { static DiscoveryCache cache; return cache; }

std::string discovery_key(const MeshSnapshot& source, const Analysis& analysis, const Cancel& cancel)
{
    if (stopped(cancel) || analysis.canceled || !analysis.error.empty() || !analysis.person_detected) return {};
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!ctx || EVP_DigestInit_ex(ctx.get(),EVP_sha256(),nullptr)!=1) return {};
    bool okay=true;
    const auto bytes = [&](const void* data,size_t count) {
        if (okay && count) okay=EVP_DigestUpdate(ctx.get(),data,count)==1;
    };
    const auto number = [&](uint64_t value) { bytes(&value,sizeof(value)); };
    const auto string = [&](const std::string& value) { number(value.size()); bytes(value.data(),value.size()); };
    string("orca.material-discovery/v3-supported-skin-gaps"); string(source.geometry_id); string(source.content_id); string(analysis.signature);
    number(source.mesh.indices.size()); number(source.mesh.vertices.size());
    // These are exact in-process evidence arrays, not a JSON re-encoding or
    // the recognition signature alone. Tests and callers can replace masks
    // under the same model identity, and confidence changes are meaningful.
    for (const auto* labels : {&analysis.face_labels,&analysis.baseline_face_labels}) {
        number(labels->size()); bytes(labels->data(),labels->size()*sizeof(Label));
    }
    for (const auto* confidence : {&analysis.face_confidence,&analysis.baseline_face_confidence}) {
        number(confidence->size()); bytes(confidence->data(),confidence->size()*sizeof(float));
    }
    std::array<uint64_t,3*1024> block {};
    for (const auto* leaves : {&analysis.subface_labels,&analysis.baseline_subface_labels}) {
        number(leaves->size()); size_t used=0;
        for (const auto& leaf : *leaves) {
            uint32_t confidence; std::memcpy(&confidence,&leaf.confidence,sizeof(confidence));
            block[used++]=uint64_t(leaf.face_id);
            block[used++]=uint64_t(leaf.path.depth) | (uint64_t(leaf.path.value)<<8) |
                (uint64_t(leaf.label)<<16) | (uint64_t(leaf.samples)<<24);
            block[used++]=confidence;
            if (used==block.size()) {
                bytes(block.data(),used*sizeof(uint64_t)); used=0;
                if (stopped(cancel)) return {};
            }
        }
        bytes(block.data(),used*sizeof(uint64_t));
    }
    std::array<unsigned char,32> digest {}; unsigned size=0;
    if (!okay || stopped(cancel) || EVP_DigestFinal_ex(ctx.get(),digest.data(),&size)!=1 || size!=32) return {};
    std::string key; key.reserve(64); constexpr char hex[]="0123456789abcdef";
    for (unsigned char byte : digest) { key+=hex[byte>>4]; key+=hex[byte&15]; }
    return key;
}

bool subface_appearance(const MeshSnapshot& source, const SubfaceLabelEvidence& evidence, Color& output_color) {
        std::array<Barycentric, 3> vertices;
        if (evidence.face_id >= source.mesh.indices.size() || !subface_vertices(evidence.path, vertices)) return false;
        Barycentric centroid {};
        for (const Barycentric& vertex : vertices)
            for (size_t channel = 0; channel < 3; ++channel)
                centroid[channel] += vertex[channel] / 3.f;
        Color rgb {};
        for (size_t corner = 0; corner < 3; ++corner) {
            const Color color = corner_color(source, evidence.face_id, int(corner));
            for (size_t channel = 0; channel < 3; ++channel)
                rgb[channel] += centroid[corner] * color[channel];
        }
        output_color = lab(rgb);
        return true;
    }

void assign_spatially_supported_skin_materials(const MeshSnapshot& source, const Analysis& analysis,
                                               const std::vector<Label>& effective_labels,
                                               const std::vector<std::vector<size_t>>& faces_at_vertex,
                                               MaterialDiscovery& discovery)
{
    const size_t count = source.mesh.indices.size();
    if (count == 0 || source.mesh.vertices.empty() || discovery.entries.empty()) return;

    Vec3f lower = source.mesh.vertices.front(), upper = lower;
    for (const Vec3f& vertex : source.mesh.vertices) {
        lower = lower.cwiseMin(vertex);
        upper = upper.cwiseMax(vertex);
    }
    const float radius = (upper - lower).norm() * .012f;
    if (!(radius > 0.f) || !std::isfinite(radius)) return;

    using Cell = std::array<long long, 3>;
    const auto cell = [radius](const Vec3f& point) {
        Cell result {};
        for (int axis = 0; axis < 3; ++axis)
            result[axis] = static_cast<long long>(std::floor(point[axis] / radius));
        return result;
    };
    const auto skin_appearance = [](const Color& color) {
        const float source_chroma = chroma(color);
        return color[0] >= .32f && color[0] <= .90f && source_chroma >= .018f &&
            source_chroma <= .16f && color[1] > .005f && color[2] > .005f &&
            color[1] <= color[2] * 1.8f;
    };
    const auto detail = [](Label label) {
        return label == Label::Lips || label == Label::MouthInterior || label == Label::EyeSclera ||
            label == Label::Iris || label == Label::Eyebrow;
    };

    std::vector<Vec3f> centers(count, Vec3f::Zero()), normals(count, Vec3f::Zero());
    std::vector<Color> appearances(count);
    for (size_t face_id = 0; face_id < count; ++face_id) {
        const auto& triangle = source.mesh.indices[face_id];
        centers[face_id] = (source.mesh.vertices[triangle[0]] + source.mesh.vertices[triangle[1]] +
                            source.mesh.vertices[triangle[2]]) / 3.f;
        normals[face_id] = (source.mesh.vertices[triangle[1]] - source.mesh.vertices[triangle[0]])
            .cross(source.mesh.vertices[triangle[2]] - source.mesh.vertices[triangle[0]]);
        const float length = normals[face_id].norm();
        if (length > 1e-9f) normals[face_id] /= length;
        appearances[face_id] = face_lab(source, face_id);
    }

    std::map<Cell, std::vector<size_t>> donor_cells, protected_cells;
    for (size_t face_id = 0; face_id < count; ++face_id) {
        const size_t material_id = discovery.face_material[face_id];
        if (material_id < discovery.entries.size()) {
            const Label label = discovery.entries[material_id].label;
            if ((label == Label::FaceSkin || label == Label::BodySkin) &&
                skin_appearance(appearances[face_id]) && normals[face_id].squaredNorm() > 0.f)
                donor_cells[cell(centers[face_id])].push_back(face_id);
        }
        const Label label = analysis.face_labels[face_id];
        if (analysis.face_confidence[face_id] >= minimum_confidence &&
            (label == Label::Hair || label == Label::Clothes || label == Label::Accessories || detail(label)))
            protected_cells[cell(centers[face_id])].push_back(face_id);
    }
    if (donor_cells.empty()) return;

    const float radius_squared = radius * radius;
    // A low-confidence skin band may sit between a reliable seed and a small
    // background hole. Resolve at most three simultaneous rings so each new
    // ring can support the next one without making face traversal order affect
    // the result. Every ring still has to pass the original color, normal,
    // protection and single-owner checks.
    for (int pass = 0; pass < 3; ++pass) {
        std::vector<std::pair<size_t, size_t>> accepted;
        for (size_t face_id = 0; face_id < count; ++face_id) {
            if (discovery.face_material[face_id] < discovery.entries.size() ||
                normals[face_id].squaredNorm() <= 0.f) continue;
            const Label label = effective_labels[face_id];
            const bool semantic_skin = label == Label::FaceSkin || label == Label::BodySkin;
            const bool reliable_skin = semantic_skin && analysis.face_confidence[face_id] >= minimum_confidence;
            // Multi-view voting commonly leaves narrow neck and ear strips below
            // the whole-face confidence threshold, or labels an occluded strip as
            // background. They are still valid skin candidates when their source
            // appearance and surrounding surface independently agree. Hair and
            // clothes remain ineligible even at low confidence.
            const bool uncertain_skin =
                (label == Label::Unknown || label == Label::Background ||
                 (semantic_skin && analysis.face_confidence[face_id] < minimum_confidence)) &&
                skin_appearance(appearances[face_id]);
            if (!reliable_skin && !uncertain_skin) continue;

            const Cell origin = cell(centers[face_id]);
            std::map<size_t, size_t> edge_votes;
            bool protected_edge = false;
            const auto& triangle = source.mesh.indices[face_id];
            for (int edge = 0; edge < 3; ++edge) {
                const int first_vertex = triangle[edge];
                const int second_vertex = triangle[(edge + 1) % 3];
                size_t neighbor = count;
                bool manifold = true;
                for (size_t candidate : faces_at_vertex[first_vertex]) {
                    if (candidate == face_id) continue;
                    const auto& candidate_triangle = source.mesh.indices[candidate];
                    if (candidate_triangle[0] != second_vertex && candidate_triangle[1] != second_vertex &&
                        candidate_triangle[2] != second_vertex) continue;
                    if (neighbor != count) { manifold = false; break; }
                    neighbor = candidate;
                }
                if (!manifold || neighbor == count) continue;
                const Label neighbor_label = analysis.face_labels[neighbor];
                if (analysis.face_confidence[neighbor] >= minimum_confidence &&
                    (neighbor_label == Label::Hair || neighbor_label == Label::Clothes ||
                     neighbor_label == Label::Accessories || detail(neighbor_label))) {
                    protected_edge = true;
                    continue;
                }
                // A recovered background/unknown face is a terminal repair,
                // never a semantic skin seed. Otherwise a valid repair at the
                // wrist or neckline can walk across warm-tinted white cloth.
                if (analysis.face_confidence[neighbor] < minimum_confidence ||
                    (neighbor_label != Label::FaceSkin && neighbor_label != Label::BodySkin))
                    continue;
                const size_t material_id = discovery.face_material[neighbor];
                if (material_id >= discovery.entries.size() || normals[face_id].dot(normals[neighbor]) < .50f)
                    continue;
                const Label donor_label = discovery.entries[material_id].label;
                if (donor_label != Label::FaceSkin && donor_label != Label::BodySkin) continue;
                if (reliable_skin && donor_label != label) continue;
                const Label compatibility_label = donor_label == Label::BodySkin
                    ? Label::BodySkin : Label::FaceSkin;
                if (compatible_skin(appearances[face_id], discovery.entries[material_id].center,
                                    compatibility_label))
                    ++edge_votes[material_id];
            }
            const bool edge_owned_skin = !reliable_skin && !protected_edge && edge_votes.size() == 1;
            bool protected_neighbor = false;
            if (!edge_owned_skin)
                for (int z = -1; z <= 1 && !protected_neighbor; ++z)
                    for (int y = -1; y <= 1 && !protected_neighbor; ++y)
                        for (int x = -1; x <= 1 && !protected_neighbor; ++x) {
                            const auto found = protected_cells.find({origin[0] + x, origin[1] + y, origin[2] + z});
                            if (found == protected_cells.end()) continue;
                            for (size_t other : found->second) {
                                const Label protected_label = analysis.face_labels[other];
                                if (reliable_skin && label == Label::BodySkin && !detail(protected_label)) continue;
                                if (normals[face_id].dot(normals[other]) < .35f ||
                                    (centers[face_id] - centers[other]).squaredNorm() > radius_squared) continue;
                                protected_neighbor = true;
                                break;
                            }
                        }
            if (protected_neighbor) continue;

            std::map<size_t, size_t> votes = edge_owned_skin ? edge_votes : std::map<size_t, size_t>{};
            if (!edge_owned_skin)
                for (int z = -1; z <= 1; ++z)
                    for (int y = -1; y <= 1; ++y)
                        for (int x = -1; x <= 1; ++x) {
                            const auto found = donor_cells.find({origin[0] + x, origin[1] + y, origin[2] + z});
                            if (found == donor_cells.end()) continue;
                            for (size_t donor : found->second) {
                                const size_t material_id = discovery.face_material[donor];
                                if (material_id >= discovery.entries.size() ||
                                    normals[face_id].dot(normals[donor]) < .50f ||
                                    (centers[face_id] - centers[donor]).squaredNorm() > radius_squared) continue;
                                const Label donor_label = discovery.entries[material_id].label;
                                if (reliable_skin && donor_label != label) continue;
                                // Uncertain/background gaps inherit the donor's detail
                                // policy: neck/body donors may absorb broad lighting
                                // shadows, while face donors keep the tighter eye/lip
                                // protection range.
                                const Label compatibility_label = donor_label == Label::BodySkin
                                    ? Label::BodySkin : Label::FaceSkin;
                                if (!compatible_skin(appearances[face_id], discovery.entries[material_id].center,
                                                     compatibility_label)) continue;
                                ++votes[material_id];
                            }
                        }
            // Conflicting nearby skin regions can represent two people or two
            // touching body parts. Do not infer ownership without instance data.
            if (votes.size() != 1 || (!edge_owned_skin && votes.begin()->second < 2)) continue;
            accepted.emplace_back(face_id, votes.begin()->first);
        }
        if (accepted.empty()) break;
        for (const auto& item : accepted) {
            discovery.face_material[item.first] = item.second;
            discovery.entries[item.second].faces.push_back(item.first);
            const Label accepted_label = effective_labels[item.first];
            if (accepted_label == Label::FaceSkin || accepted_label == Label::BodySkin)
                donor_cells[cell(centers[item.first])].push_back(item.first);
        }
    }
}

void constrain_eyebrow_material_regions(const MeshSnapshot& source, const Analysis& analysis,
                                        const std::vector<std::vector<size_t>>& faces_at_vertex,
                                        std::vector<Label>& effective_labels,
                                        std::vector<uint8_t>& supported_faces)
{
    const size_t count = source.mesh.indices.size();
    supported_faces.assign(count, 0);
    std::vector<size_t> eye_faces;
    for (size_t face_id = 0; face_id < count; ++face_id)
        if ((analysis.face_labels[face_id] == Label::EyeSclera || analysis.face_labels[face_id] == Label::Iris) &&
            analysis.face_confidence[face_id] >= minimum_confidence)
            eye_faces.push_back(face_id);
    // A recognizer without eye evidence keeps its conservative brow fallback.
    // Once eyes are present, they provide the anatomical bound that prevents a
    // high-confidence hair texture from becoming a remote eyebrow component.
    if (eye_faces.empty()) {
        for (size_t face_id = 0; face_id < count; ++face_id)
            supported_faces[face_id] = effective_labels[face_id] == Label::Eyebrow;
        for (const auto& evidence : analysis.subface_labels)
            if (evidence.face_id < count && evidence.label == Label::Eyebrow)
                supported_faces[evidence.face_id] = 1;
        return;
    }

    Vec3f lower = source.mesh.vertices.front(), upper = lower;
    for (const Vec3f& vertex : source.mesh.vertices) {
        lower = lower.cwiseMin(vertex);
        upper = upper.cwiseMax(vertex);
    }
    const float maximum_distance = (upper - lower).norm() * .030f;
    const float maximum_distance_squared = maximum_distance * maximum_distance;
    const auto center = [&](size_t face_id) {
        const auto& triangle = source.mesh.indices[face_id];
        return (source.mesh.vertices[triangle[0]] + source.mesh.vertices[triangle[1]] +
                source.mesh.vertices[triangle[2]]) / 3.f;
    };
    std::vector<Vec3f> eye_centers;
    eye_centers.reserve(eye_faces.size());
    for (size_t face_id : eye_faces) eye_centers.push_back(center(face_id));

    std::vector<size_t> parent(count); std::iota(parent.begin(), parent.end(), 0);
    const auto root = [&parent](size_t id) {
        while (parent[id] != id) { parent[id] = parent[parent[id]]; id = parent[id]; }
        return id;
    };
    for (size_t face_id = 0; face_id < count; ++face_id) {
        if (effective_labels[face_id] != Label::Eyebrow ||
            analysis.face_confidence[face_id] < minimum_confidence) continue;
        for (int corner = 0; corner < 3; ++corner)
            for (size_t neighbor : faces_at_vertex[source.mesh.indices[face_id][corner]]) {
                if (effective_labels[neighbor] != Label::Eyebrow ||
                    analysis.face_confidence[neighbor] < minimum_confidence) continue;
                const size_t a = root(face_id), b = root(neighbor);
                if (a != b) parent[std::max(a, b)] = std::min(a, b);
            }
    }
    std::map<size_t, std::vector<size_t>> components;
    for (size_t face_id = 0; face_id < count; ++face_id)
        if (effective_labels[face_id] == Label::Eyebrow &&
            analysis.face_confidence[face_id] >= minimum_confidence)
            components[root(face_id)].push_back(face_id);

    for (const auto& component : components) {
        bool near_eye = true;
        size_t skin_boundary = 0;
        for (size_t face_id : component.second) {
            const Vec3f face_center = center(face_id);
            const bool face_near_eye = std::any_of(eye_centers.begin(), eye_centers.end(), [&](const Vec3f& eye) {
                return (face_center - eye).squaredNorm() <= maximum_distance_squared;
            });
            near_eye &= face_near_eye;
            for (int corner = 0; corner < 3; ++corner)
                for (size_t neighbor : faces_at_vertex[source.mesh.indices[face_id][corner]])
                    if ((analysis.face_labels[neighbor] == Label::FaceSkin ||
                         analysis.face_labels[neighbor] == Label::BodySkin) &&
                        analysis.face_confidence[neighbor] >= minimum_confidence)
                        ++skin_boundary;
        }
        if (near_eye && skin_boundary > 0) {
            for (size_t face_id : component.second) supported_faces[face_id] = 1;
            continue;
        }

        for (size_t face_id : component.second) {
            std::vector<Sample> local_skin;
            size_t hair_support = 0;
            for (int corner = 0; corner < 3; ++corner)
                for (size_t neighbor : faces_at_vertex[source.mesh.indices[face_id][corner]]) {
                    if (analysis.face_confidence[neighbor] < minimum_confidence) continue;
                    const Label label = analysis.face_labels[neighbor];
                    if (label == Label::Hair) ++hair_support;
                    else if (label == Label::FaceSkin || label == Label::BodySkin)
                        local_skin.push_back({face_lab(source, neighbor), area(source, neighbor)});
                }
            const Color original = face_lab(source, face_id);
            const auto skin_centers = prototypes(local_skin, 1);
            if (hair_support > local_skin.size() && natural_dark_hair(original))
                effective_labels[face_id] = Label::Hair;
            else if (!skin_centers.empty() && compatible_skin(original, skin_centers.front(), Label::BodySkin))
                effective_labels[face_id] = Label::FaceSkin;
            else
                effective_labels[face_id] = Label::Unknown;
        }
    }

    // A brow may be represented only by a child leaf on a FaceSkin root. It
    // has no whole-face eyebrow component above, so validate the containing
    // face directly against the same anatomical eye bound. Reliable skin at
    // the root supplies the required face-side support.
    for (const auto& evidence : analysis.subface_labels) {
        if (evidence.face_id >= count || evidence.label != Label::Eyebrow ||
            evidence.confidence < minimum_confidence) continue;
        const size_t face_id = evidence.face_id;
        const bool near_eye = std::any_of(eye_centers.begin(), eye_centers.end(), [&](const Vec3f& eye) {
            return (center(face_id) - eye).squaredNorm() <= maximum_distance_squared;
        });
        const bool skin_root = (analysis.face_labels[face_id] == Label::FaceSkin ||
                                analysis.face_labels[face_id] == Label::BodySkin) &&
                               analysis.face_confidence[face_id] >= minimum_confidence;
        if (near_eye && skin_root) supported_faces[face_id] = 1;
    }
}

std::shared_ptr<const MaterialDiscovery> discover_materials(const MeshSnapshot& source,
                                                           const Analysis& analysis, const Cancel& cancel)
{
    const size_t count=source.mesh.indices.size();
    auto discovery=std::make_shared<MaterialDiscovery>();
    { // Release whole-region topology before collecting child evidence.
    discovery->face_material.assign(count, std::numeric_limits<size_t>::max());
    std::vector<Label> effective_labels = analysis.face_labels;
    std::vector<std::vector<size_t>> semantic_faces_at_vertex(source.mesh.vertices.size());
    for (size_t face_id = 0; face_id < count; ++face_id)
        for (int corner = 0; corner < 3; ++corner)
            semantic_faces_at_vertex[source.mesh.indices[face_id][corner]].push_back(face_id);
    // A coarse face mask can include a dark brow strip in FaceSkin. Promote it
    // to an explicit material region only when it touches reliable eye evidence;
    // this keeps the correction visible to mapping, overrides and diagnostics.
    for (size_t face_id = 0; face_id < count; ++face_id) {
        if (effective_labels[face_id] != Label::FaceSkin ||
            analysis.face_confidence[face_id] < minimum_confidence) continue;
        const Color original = face_lab(source, face_id);
        if (original[0] >= .50f || chroma(original) >= .10f ||
            original[1] < -.02f || original[2] < -.02f) continue;
        bool touches_eye = false;
        for (int corner = 0; corner < 3 && !touches_eye; ++corner)
            for (size_t neighbor : semantic_faces_at_vertex[source.mesh.indices[face_id][corner]]) {
                const Label label = analysis.face_labels[neighbor];
                if (analysis.face_confidence[neighbor] >= minimum_confidence &&
                    (label == Label::EyeSclera || label == Label::Iris || label == Label::Eyebrow)) {
                    touches_eye = true;
                    break;
                }
            }
        if (touches_eye) effective_labels[face_id] = Label::Eyebrow;
    }
    for (size_t face_id = 0; face_id < count; ++face_id) {
        if (effective_labels[face_id] != Label::BodySkin ||
            analysis.face_confidence[face_id] < minimum_confidence) continue;
        const Color original = face_lab(source, face_id);
        bool matches_neighbor_hair = false;
        for (int corner = 0; corner < 3 && !matches_neighbor_hair; ++corner)
            for (size_t neighbor : semantic_faces_at_vertex[source.mesh.indices[face_id][corner]]) {
                if (analysis.face_labels[neighbor] != Label::Hair ||
                    analysis.face_confidence[neighbor] < minimum_confidence) continue;
                if (same_hair_material(original, face_lab(source, neighbor))) {
                    matches_neighbor_hair = true;
                    break;
                }
            }
        if (matches_neighbor_hair) effective_labels[face_id] = Label::Hair;
    }
    constrain_eyebrow_material_regions(source, analysis, semantic_faces_at_vertex, effective_labels,
                                       discovery->eyebrow_supported_faces);
    // Eyebrows are a detail overlay on the face. Treat them as face skin only
    // while building the underlying material topology, so removing a thin brow
    // strip cannot split the forehead into different skin prototypes.
    const auto topology_label = [](Label label) {
        return label == Label::Eyebrow ? Label::FaceSkin : label;
    };
    // Connected semantic regions share prototypes, but never share centers with
    // a different material label. Reuse of one physical filament is permitted.
    std::vector<size_t> parent(count); std::iota(parent.begin(), parent.end(), 0);
    const auto root = [&parent](size_t id) {
        while (parent[id] != id) { parent[id] = parent[parent[id]]; id = parent[id]; } return id;
    };
    std::vector<size_t> first(source.mesh.vertices.size(), count);
    for (size_t label = 2; label < label_count; ++label) {
        std::fill(first.begin(), first.end(), count);
        for (size_t id = 0; id < count; ++id) {
            if (size_t(topology_label(effective_labels[id])) != label || analysis.face_confidence[id] < minimum_confidence) continue;
            for (int corner = 0; corner < 3; ++corner) {
                const int vertex = source.mesh.indices[id][corner];
                if (first[vertex] == count) first[vertex] = id;
                else { const size_t a = root(first[vertex]), b = root(id); if (a != b) parent[b] = a; }
            }
        }
    }
    std::map<size_t, std::vector<size_t>> regions;
    for (size_t id = 0; id < count; ++id)
        if (paintable(effective_labels[id]) && analysis.face_confidence[id] >= minimum_confidence) regions[root(id)].push_back(id);
    discovery->hair_seed_faces.assign(count,0);
    for (const auto& region : regions) {
        if (stopped(cancel)) return {};
        const Label label = topology_label(effective_labels[region.second.front()]);
        const bool skin = label == Label::FaceSkin || label == Label::BodySkin;
        const bool eye = label == Label::EyeSclera || label == Label::Iris;
        const bool eyebrow = label == Label::Eyebrow;
        std::vector<Color> appearances; appearances.reserve(region.second.size());
        std::vector<Sample> samples; samples.reserve(region.second.size());
        for (size_t id : region.second) {
            const Color appearance = face_lab(source, id);
            appearances.push_back(appearance);
            // The brow is only a topological bridge in this pass. Its dark
            // pigment belongs to the detail overlay below, not the skin center.
            if (effective_labels[id] != Label::Eyebrow)
                samples.push_back({appearance, area(source, id)});
        }
        auto centers = prototypes(samples, skin || eye || eyebrow || label == Label::Lips || label == Label::MouthInterior ? 1 : 6);
        const size_t ordinary_center_count = centers.size();
        if (skin) {
            // Preserve genuinely neutral teeth/cloth accidentally included in
            // the coarse skin mask, using stable supported neutral centers.
            std::vector<Sample> neutral_samples;
            for (const auto& sample : samples) if (chroma(sample.color) < .015f) neutral_samples.push_back(sample);
            const auto neutral_centers = prototypes(neutral_samples, 6);
            centers.insert(centers.end(), neutral_centers.begin(), neutral_centers.end());
        }
        if (centers.empty()) continue;
        size_t sclera_seeds = 0;
        bool bright_sclera = false;
        if (label == Label::EyeSclera) for (const auto& sample : samples) {
            const float source_chroma = chroma(sample.color);
            if (sample.color[0] >= .55f && source_chroma <= .055f) ++sclera_seeds;
            bright_sclera |= sample.color[0] >= .68f && source_chroma <= .035f;
        }
        const bool supported_sclera = label != Label::EyeSclera || sclera_seeds >= 2 || bright_sclera;
        const size_t first_material = discovery->entries.size();
        for (const auto& center : centers)
            discovery->entries.push_back({label, center, {}});
        const auto assign_material = [&](size_t face_id, size_t center_index) {
            discovery->face_material[face_id] = first_material + center_index;
            discovery->entries[first_material + center_index].faces.push_back(face_id);
        };
        for (size_t i = 0; i < region.second.size(); ++i) {
            if (effective_labels[region.second[i]] == Label::Eyebrow) continue;
            const auto& original = appearances[i];
            const bool supported_shadow = bright_sclera && original[0] >= .55f &&
                original[0] >= centers.front()[0] - .235f && chroma(original) <= .055f;
            if (label == Label::EyeSclera && (!supported_sclera ||
                (!compatible_sclera(original, centers.front()) && !supported_shadow)))
                continue;
            size_t nearest = 0; float best = std::numeric_limits<float>::max();
            for (size_t center = 0; center < centers.size(); ++center) {
                const float d = distance(original, centers[center]);
                if (d < best) { best = d; nearest = center; }
            }
            if (label == Label::Hair && same_hair_material(original, centers.front())) {
                assign_material(region.second[i], 0);
                discovery->hair_seed_faces[region.second[i]]=1;
                continue;
            }
            if (skin && chroma(original) < .015f && centers.size() > ordinary_center_count) {
                nearest = ordinary_center_count;
                for (size_t center = ordinary_center_count + 1; center < centers.size(); ++center)
                    if (distance(original, centers[center]) < distance(original, centers[nearest])) nearest = center;
            } else if (skin && !compatible_skin(original, centers.front(), label)) continue;
            assign_material(region.second[i], nearest);
        }
    }
    // Map each connected brow from its own stable material center after the
    // underlying skin pass. This preserves one supported brow material without
    // allowing it to alter the face-skin prototype or borrow hair across the
    // forehead.
    std::vector<size_t> eyebrow_parent(count); std::iota(eyebrow_parent.begin(), eyebrow_parent.end(), 0);
    const auto eyebrow_root = [&eyebrow_parent](size_t id) {
        while (eyebrow_parent[id] != id) {
            eyebrow_parent[id] = eyebrow_parent[eyebrow_parent[id]];
            id = eyebrow_parent[id];
        }
        return id;
    };
    std::fill(first.begin(), first.end(), count);
    for (size_t id = 0; id < count; ++id) {
        if (effective_labels[id] != Label::Eyebrow || analysis.face_confidence[id] < minimum_confidence) continue;
        for (int corner = 0; corner < 3; ++corner) {
            const int vertex = source.mesh.indices[id][corner];
            if (first[vertex] == count) first[vertex] = id;
            else {
                const size_t a = eyebrow_root(first[vertex]), b = eyebrow_root(id);
                if (a != b) eyebrow_parent[b] = a;
            }
        }
    }
    std::map<size_t, std::vector<size_t>> eyebrow_regions;
    for (size_t id = 0; id < count; ++id)
        if (effective_labels[id] == Label::Eyebrow && analysis.face_confidence[id] >= minimum_confidence)
            eyebrow_regions[eyebrow_root(id)].push_back(id);
    for (const auto& region : eyebrow_regions) {
        if (stopped(cancel)) return {};
        std::vector<Sample> samples; samples.reserve(region.second.size());
        for (size_t id : region.second) samples.push_back({face_lab(source, id), area(source, id)});
        const auto centers = prototypes(samples, 1);
        if (centers.empty()) continue;
        const size_t material_id = discovery->entries.size();
        discovery->entries.push_back({Label::Eyebrow, centers.front(), region.second});
        for (size_t id : region.second) {
            discovery->face_material[id] = material_id;
        }
    }
    assign_spatially_supported_skin_materials(source, analysis, effective_labels,
                                              semantic_faces_at_vertex, *discovery);
    const auto contrast_pair = [](Label a, Label b) {
        if (a > b) std::swap(a, b);
        return (a == Label::FaceSkin && (b == Label::Lips || b == Label::Eyebrow)) ||
               (a == Label::EyeSclera && b == Label::Iris);
    };
    std::vector<std::vector<size_t>> at_vertex(source.mesh.vertices.size());
    for (size_t face_id = 0; face_id < count; ++face_id) {
        const size_t material_id = discovery->face_material[face_id];
        if (material_id >= discovery->entries.size()) continue;
        for (int corner = 0; corner < 3; ++corner)
            at_vertex[source.mesh.indices[face_id][corner]].push_back(material_id);
    }
    auto& pairs=discovery->contrast_pairs;
    for (auto& entries : at_vertex) {
        std::sort(entries.begin(), entries.end());
        entries.erase(std::unique(entries.begin(), entries.end()), entries.end());
        for (size_t a = 0; a < entries.size(); ++a)
            for (size_t b = a + 1; b < entries.size(); ++b)
                if (contrast_pair(discovery->entries[entries[a]].label, discovery->entries[entries[b]].label))
                    pairs.emplace(entries[a], entries[b]);
    }
    }
    for (auto& material:discovery->entries)
        for (size_t face_id:material.faces) material.surface_area+=area(source,face_id);
    if (stopped(cancel)) return {};
    std::vector<std::vector<size_t>> faces_at_vertex(source.mesh.vertices.size());
    for (size_t face_id = 0; face_id < count; ++face_id)
        for (int corner = 0; corner < 3; ++corner)
            faces_at_vertex[source.mesh.indices[face_id][corner]].push_back(face_id);

    // All leaves in a material use the exact center/target of its whole-face
    // region. Regions represented only by leaves have their own palette-
    // independent center; no individual leaf competes against the palette.
    std::vector<size_t> leaf_parent(analysis.subface_labels.size());
    std::iota(leaf_parent.begin(), leaf_parent.end(), 0);
    const auto leaf_root = [&leaf_parent](size_t id) {
        while (leaf_parent[id] != id) { leaf_parent[id] = leaf_parent[leaf_parent[id]]; id = leaf_parent[id]; }
        return id;
    };
    std::map<std::pair<size_t, Label>, size_t> leaf_at_vertex;
    for (size_t id = 0; id < analysis.subface_labels.size(); ++id) {
        const auto& evidence = analysis.subface_labels[id];
        if (evidence.face_id >= count || evidence.confidence < minimum_confidence) continue;
        for (int corner = 0; corner < 3; ++corner) {
            const auto key = std::make_pair(size_t(source.mesh.indices[evidence.face_id][corner]), evidence.label);
            const auto found = leaf_at_vertex.emplace(key, id);
            if (!found.second) {
                const size_t a = leaf_root(id), b = leaf_root(found.first->second);
                if (a != b) leaf_parent[std::max(a, b)] = std::min(a, b);
            }
        }
    }
    std::map<size_t, std::vector<Sample>> leaf_samples;
    auto& leaf_component_at_face=discovery->leaf_component_at_face;
    for (size_t id = 0; id < analysis.subface_labels.size(); ++id) {
        const auto& evidence = analysis.subface_labels[id];
        Color original;
        if (evidence.confidence < minimum_confidence || !subface_appearance(source,evidence,original)) continue;
        const size_t component = leaf_root(id);
        leaf_samples[component].push_back({original, area(source, evidence.face_id) /
                                                    double(1u << (2u * evidence.path.depth))});
        leaf_component_at_face[{evidence.face_id, evidence.label}] = component;
    }
    auto& leaf_centers=discovery->leaf_centers;
    for (const auto& item : leaf_samples) {
        const auto label = analysis.subface_labels[item.first].label;
        leaf_centers.emplace(item.first, prototypes(item.second, label == Label::Hair ? 6 : 1));
    }
    if (stopped(cancel)) return {};
    // Eye-white appearance support is local to a connected reliable eye patch.
    // A brighter other eye (or another person's eye) cannot set this patch's
    // lightness floor, and an isolated anchor cannot borrow remote support.
    std::vector<size_t> sclera_parent(count);
    std::iota(sclera_parent.begin(), sclera_parent.end(), 0);
    const auto sclera_root = [&sclera_parent](size_t id) {
        while (sclera_parent[id] != id) {
            sclera_parent[id] = sclera_parent[sclera_parent[id]];
            id = sclera_parent[id];
        }
        return id;
    };
    std::vector<uint8_t> sclera_anchor_candidate(count, 0);
    for (size_t face_id = 0; face_id < count; ++face_id) {
        if (analysis.face_labels[face_id] != Label::EyeSclera ||
            analysis.face_confidence[face_id] < minimum_confidence) continue;
        const Color source_color = face_lab(source, face_id);
        if (source_color[0] >= .40f && chroma(source_color) <= .055f)
            sclera_anchor_candidate[face_id] = 1;
    }
    for (size_t face_id = 0; face_id < count; ++face_id) {
        if (!sclera_anchor_candidate[face_id]) continue;
        for (int corner = 0; corner < 3; ++corner)
            for (size_t neighbor : faces_at_vertex[source.mesh.indices[face_id][corner]]) {
                if (!sclera_anchor_candidate[neighbor]) continue;
                const size_t a = sclera_root(face_id), b = sclera_root(neighbor);
                if (a != b) sclera_parent[std::max(a, b)] = std::min(a, b);
            }
    }
    std::map<size_t, std::vector<Sample>> sclera_samples;
    for (size_t face_id = 0; face_id < count; ++face_id) {
        if (!sclera_anchor_candidate[face_id]) continue;
        const Color source_color = face_lab(source, face_id);
        if (source_color[0] >= .68f && chroma(source_color) <= .035f)
            sclera_samples[sclera_root(face_id)].push_back({source_color, area(source, face_id)});
    }
    auto& sclera_centers=discovery->sclera_centers;
    for (const auto& component : sclera_samples) {
        if (component.second.size() < 2) continue;
        const auto centers = prototypes(component.second, 1);
        if (!centers.empty()) sclera_centers.emplace(component.first, centers.front());
    }
    // count is unsupported, count+1 is conflicting support. One immutable
    // anchor ring is used: newly supported boundary faces cannot spread again.
    auto& sclera_support_component=discovery->sclera_support_component;
    sclera_support_component.assign(count,count);
    for (size_t face_id = 0; face_id < count; ++face_id) {
        if (!sclera_anchor_candidate[face_id]) continue;
        const auto center = sclera_centers.find(sclera_root(face_id));
        if (center != sclera_centers.end() && compatible_sclera(face_lab(source, face_id), center->second))
            sclera_support_component[face_id] = center->first;
    }
    const auto anchors = sclera_support_component;
    for (size_t face_id = 0; face_id < count; ++face_id) {
        if (anchors[face_id] < count) continue;
        size_t component = count;
        for (int corner = 0; corner < 3; ++corner)
            for (size_t neighbor : faces_at_vertex[source.mesh.indices[face_id][corner]]) {
                if (anchors[neighbor] >= count) continue;
                if (component == count) component = anchors[neighbor];
                else if (component != anchors[neighbor]) component = count + 1;
            }
        sclera_support_component[face_id] = component;
    }
    if (stopped(cancel)) return {};
    return discovery;
}

std::shared_ptr<const MaterialDiscovery> get_material_discovery(const MeshSnapshot& source,
                                                               const Analysis& analysis, const Cancel& cancel)
{
    const std::string key=discovery_key(source,analysis,cancel);
    if (key.empty()) return {};
    // A failed boundary view can still use its safe recognition fallback, but
    // it must not populate a successful reusable evidence cache.
    if (std::any_of(analysis.boundary_runs.begin(),analysis.boundary_runs.end(),[](const auto& run) {
            return run.status=="error" || run.status=="canceled";
        })) return discover_materials(source,analysis,cancel);
    auto& cache=discovery_cache();
    {
        std::lock_guard<std::mutex> lock(cache.mutex);
        for (auto& entry:cache.entries) if (entry.key==key && entry.value) {
            entry.used=++cache.clock; ++cache.stats.hits; return entry.value;
        }
    }
    auto value=discover_materials(source,analysis,cancel);
    if (!value || stopped(cancel)) return {};
    std::lock_guard<std::mutex> lock(cache.mutex);
    if (stopped(cancel)) return {};
    // Concurrent callers may have completed the same discovery while the
    // expensive work ran outside the lock. Keep the first immutable result.
    for (auto& entry:cache.entries) if (entry.key==key && entry.value) {
        entry.used=++cache.clock; ++cache.stats.hits; return entry.value;
    }
    auto selected=std::min_element(cache.entries.begin(),cache.entries.end(),
        [](const auto& a,const auto& b) { return a.used<b.used; });
    *selected={key,value,++cache.clock}; ++cache.stats.builds;
    return value;
}

} // namespace

RegionPaletteDecision decide_region_palette(const Color& original_oklab, Label label,
                                            const std::vector<Color>& palette,
                                            const std::vector<Color>& portrait_card)
{
    if (palette.empty() || palette.size() > 6 ||
        std::any_of(palette.begin(), palette.end(), [](const Color& color) { return !valid_color(color); }))
        return {};
    return PalettePolicy(palette, portrait_card).decide(original_oklab, label);
}

MaterialDiscoveryCacheStats material_discovery_cache_stats()
{
    auto& cache=discovery_cache(); std::lock_guard<std::mutex> lock(cache.mutex); return cache.stats;
}
void clear_material_discovery_cache()
{
    auto& cache=discovery_cache(); std::lock_guard<std::mutex> lock(cache.mutex);
    cache.entries={}; cache.stats={}; cache.clock=0;
}

static FaceColors map_palette_impl(const MeshSnapshot& source, const Analysis& analysis, const std::vector<Color>& palette,
                       const std::vector<Color>& portrait_card, MappedMaterials* materials, const Cancel& cancel = {}, bool whole_output = true)
{
    const size_t count = source.mesh.indices.size();
    if (!analysis.person_detected || analysis.canceled || !analysis.error.empty() || palette.empty() || palette.size() > 6 ||
        analysis.geometry_id != source.geometry_id || analysis.content_id != source.content_id ||
        analysis.signature != analysis_cache_key(source, analysis.body_identity, analysis.face_identity,
                                                  analysis.boundary_identity) ||
        analysis.face_labels.size() != count || analysis.face_confidence.size() != count || !validate_snapshot(source).empty()) return {};
    for (const auto& color : palette) if (!valid_color(color)) return {};
    for (size_t id = 0; id < count; ++id)
        if (!valid_label(analysis.face_labels[id]) || !std::isfinite(analysis.face_confidence[id]) ||
            analysis.face_confidence[id] < 0 || analysis.face_confidence[id] > 1) return {};
    const PalettePolicy policy(palette, portrait_card);
    const auto& palette_labs = policy.palette_labs;
    const auto choose_target = [&](const Color& center, Label label) { return policy.choose(center, label); };
    MappedMaterials local_materials;
    if (!materials) materials = &local_materials;
    materials->entries.clear();
    materials->discovery=get_material_discovery(source,analysis,cancel);
    if (!materials->discovery) return {};
    const auto& discovery=*materials->discovery;
    materials->entries.reserve(discovery.entries.size());
    for (const auto& material:discovery.entries)
        materials->entries.push_back({material.label,material.center,choose_target(material.center,material.label)});
    std::vector<HairSeed> hair_seeds(whole_output ? count : 0);
    for (size_t id=0;whole_output && id<count;++id) {
        const size_t material_id=discovery.face_material[id];
        if (material_id>=materials->entries.size() || !discovery.hair_seed_faces[id]) continue;
        const auto& material=materials->entries[material_id];
        if (natural_dark_hair(palette_labs[material.target]))
            hair_seeds[id]={material.center,int(material.target)};
    }
    FaceColors output; if (whole_output) output.reserve(analysis.reliable_faces);
    for (size_t id=0;whole_output && id<count;++id) {
        const size_t material=discovery.face_material[id];
        if (material<materials->entries.size()) output.emplace_back(id,palette[materials->entries[material].target]);
    }
    // Preserve a real neighboring feature only when an equally plausible
    // candidate exists. Limited palettes may legitimately reuse a slot.
    const auto norm = [](const Color& a, const Color& b) {
        float total = 0.f;
        for (size_t channel = 0; channel < 3; ++channel) {
            const float delta = a[channel] - b[channel]; total += delta * delta;
        }
        return std::sqrt(total);
    };
    const auto& pairs=discovery.contrast_pairs;
    for (const auto& pair : pairs) {
        auto& left = materials->entries[pair.first];
        auto& right = materials->entries[pair.second];
        if (palette[left.target] != palette[right.target] || norm(left.center, right.center) < .04f) continue;
        MappedMaterial* selected = nullptr;
        size_t replacement = palette.size();
        float best_cost = std::numeric_limits<float>::max();
        for (MappedMaterial* material : {&left, &right}) {
            const size_t role = material->label == Label::FaceSkin ? 0 : material->label == Label::Lips ? 3 :
                material->label == Label::EyeSclera ? 2 : material->label == Label::Eyebrow ? 5 : 1;
            if (policy.role_available(role)) continue; // Explicit role bindings win.
            const float base_cost = norm(material->center, palette_labs[material->target]);
            for (size_t slot = 0; slot < palette.size(); ++slot) {
                const auto& candidate = palette_labs[slot];
                if (palette[slot] == palette[material->target]) continue;
                const float candidate_chroma = chroma(candidate), source_chroma = chroma(material->center);
                if (candidate_chroma > source_chroma + .025f ||
                    (source_chroma < .015f && candidate_chroma >= .035f)) continue;
                if (source_chroma > .025f && candidate_chroma > .025f &&
                    material->center[1] * candidate[1] + material->center[2] * candidate[2] <
                        source_chroma * candidate_chroma * .5f) continue;
                const float cost = norm(material->center, candidate);
                if (cost > base_cost + .03f || cost >= best_cost) continue;
                selected = material; replacement = slot; best_cost = cost;
            }
        }
        if (selected) selected->target = replacement;
    }
    // Child matching needs only common region targets, not a second whole
    // surface extension/material-patch pass with the same palette.
    if (!whole_output) return {};
    for (auto& assignment : output) {
        const size_t material = discovery.face_material[assignment.first];
        if (material < materials->entries.size()) assignment.second = palette[materials->entries[material].target];
    }
    extend_hair_edges(source, analysis, hair_seeds, palette, output);
    refine_material_patches(source, analysis, palette, portrait_card, output);
    // Facial edge pixels can remain coarse FaceSkin/BodySkin/Unknown faces
    // after recognition fallback. Keep a lip-colored slot from leaking into
    // an eye or its immediate ring, while leaving real lips untouched.
    sanitize_eye_edge_targets(source, analysis, palette, portrait_card, output);
    // Keep dark hair-edge faces from borrowing the skin/lip slot when a
    // coarse body mask reaches across the ear or hairline.
    sanitize_hair_edge_targets(source, analysis, palette, portrait_card, output);
    // The lip role is a local facial detail. Do not let its slot become a
    // fallback color for distant ear, neck, or cheek faces.
    sanitize_lip_leak_targets(source, analysis, palette, portrait_card, output);
    // Post-processors may repair low-confidence gaps, but a discovered source
    // material has already passed semantic and source-color filtering. Reassert
    // that single region decision so legacy patch rules cannot silently choose
    // a different slot than recommendations and diagnostics report.
    for (auto& assignment : output) {
        const size_t material = discovery.face_material[assignment.first];
        if (material < materials->entries.size()) assignment.second = palette[materials->entries[material].target];
    }
    std::sort(output.begin(), output.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return output;
}

FaceColors map_palette(const MeshSnapshot& source, const Analysis& analysis, const std::vector<Color>& palette,
                       const std::vector<Color>& portrait_card)
{
    return map_palette_impl(source, analysis, palette, portrait_card, nullptr);
}

FaceColors map_palette_materials(const MeshSnapshot& source, const Analysis& analysis,
                                const std::vector<Color>& palette, const std::vector<Color>& portrait_card,
                                std::vector<MaterialCenter>& centers, const Cancel& cancel)
{
    std::vector<size_t> ignored_face_material_ids;
    return map_palette_materials(source, analysis, palette, portrait_card, centers,
                                 ignored_face_material_ids, cancel);
}

FaceColors map_palette_materials(const MeshSnapshot& source, const Analysis& analysis,
                                const std::vector<Color>& palette, const std::vector<Color>& portrait_card,
                                std::vector<MaterialCenter>& centers, std::vector<size_t>& face_material_ids,
                                const Cancel& cancel)
{
    centers.clear();
    face_material_ids.clear();
    MappedMaterials materials;
    auto output = map_palette_impl(source, analysis, palette, portrait_card, &materials, cancel);
    if (materials.discovery) face_material_ids = materials.discovery->face_material;
    for (size_t id = 0; id < materials.entries.size(); ++id) {
        const auto& material = materials.entries[id];
        const auto& color = material.center;
        const float l = std::pow(color[0] + .3963377774f * color[1] + .2158037573f * color[2], 3.f);
        const float m = std::pow(color[0] - .1055613458f * color[1] - .0638541728f * color[2], 3.f);
        const float s = std::pow(color[0] - .0894841775f * color[1] - 1.2914855480f * color[2], 3.f);
        Color rgb {4.0767416621f*l - 3.3077115913f*m + .2309699292f*s,
                  -1.2684380046f*l + 2.6097574011f*m - .3413193965f*s,
                  -.0041960863f*l - .7034186147f*m + 1.7076147010f*s};
        for (float& channel : rgb) {
            channel = std::clamp(channel, 0.f, 1.f);
            channel = channel <= .0031308f ? channel * 12.92f : 1.055f * std::pow(channel, 1.f/2.4f) - .055f;
        }
        const auto& original=materials.discovery->entries[id];
        centers.push_back({id, material.label, rgb, color, original.faces.size(), original.surface_area});
    }
    return output;
}

FaceColors remap_palette_targets(const FaceColors& suggestions, const std::vector<Color>& original_candidates,
                                const std::vector<Color>& target_candidates)
{
    if (original_candidates.empty() || original_candidates.size() > 6 || target_candidates.size() != original_candidates.size()) return {};
    for (size_t slot = 0; slot < original_candidates.size(); ++slot) {
        if (!valid_color(original_candidates[slot]) || !valid_color(target_candidates[slot])) return {};
        for (size_t previous = 0; previous < slot; ++previous)
            if (original_candidates[previous] == original_candidates[slot] && target_candidates[previous] != target_candidates[slot]) return {};
    }
    FaceColors result; result.reserve(suggestions.size());
    for (const auto& suggestion : suggestions) {
        const auto found = std::find(original_candidates.begin(), original_candidates.end(), suggestion.second);
        if (found == original_candidates.end()) return {};
        result.emplace_back(suggestion.first, target_candidates[size_t(found - original_candidates.begin())]);
    }
    return result;
}

FaceColors compose(const FaceColors& automatic, const FaceColors& manual, bool automatic_enabled)
{
    std::map<size_t, Color> colors;
    if (automatic_enabled) for (const auto& item : automatic) colors[item.first] = item.second;
    for (const auto& item : manual) colors[item.first] = item.second;
    return {colors.begin(), colors.end()};
}

bool enforce_subface_budget(const SubfaceColors& candidates, size_t original_face_count,
                            const SubfaceBudget& budget, SubfaceBudgetResult& output, std::string& error)
{
    output = {};
    error.clear();
    const auto fail = [&](const char* message) {
        output = {};
        error = message;
        return false;
    };
    if (original_face_count == 0 || original_face_count > maximum_faces ||
        !std::isfinite(budget.maximum_added_ratio) || budget.maximum_added_ratio < 0.f ||
        budget.maximum_added_ratio > 1.f || !std::isfinite(budget.minimum_confidence) ||
        budget.minimum_confidence < 0.f || budget.minimum_confidence > 1.f)
        return fail("Invalid semantic subface budget.");

    std::map<std::pair<size_t, SubfacePath>, SubfaceColor> unique;
    for (const SubfaceColor& candidate : candidates) {
        if (candidate.face_id >= original_face_count || candidate.path.depth == 0 || candidate.path.depth > 3 ||
            !valid_color(candidate.color) ||
            !std::isfinite(candidate.confidence) || candidate.confidence < 0.f || candidate.confidence > 1.f)
            return fail("Invalid semantic subface candidate.");
        const unsigned path_limit = 1u << (2u * candidate.path.depth);
        if (unsigned(candidate.path.value) >= path_limit)
            return fail("Invalid semantic subface candidate.");
        const auto key = std::make_pair(candidate.face_id, candidate.path);
        const auto found = unique.find(key);
        if (found == unique.end() || candidate.confidence > found->second.confidence)
            unique[key] = candidate;
    }

    // Canonicalize ancestor/descendant assignments into disjoint leaves.
    // A more specific observation overrides only its child; sibling materials
    // inherit the old safe parent rather than being discarded.
    decltype(unique) normalized;
    for (const auto& item : unique) {
        const auto& candidate = item.second;
        for (uint8_t depth = 1; depth < candidate.path.depth; ++depth) {
            const unsigned shift = 2u * unsigned(candidate.path.depth - depth);
            const SubfacePath ancestor {depth, uint8_t(candidate.path.value >> shift)};
            auto found = normalized.find({candidate.face_id, ancestor});
            if (found == normalized.end()) continue;
            const auto inherited = found->second;
            normalized.erase(found);
            for (uint8_t child = 0; child < 4; ++child) {
                auto sibling = inherited;
                sibling.path = {uint8_t(depth + 1), uint8_t(ancestor.value * 4 + child)};
                normalized[{candidate.face_id, sibling.path}] = sibling;
            }
        }
        normalized[item.first] = candidate;
    }
    unique.swap(normalized);

    SubfaceColors ordered;
    ordered.reserve(unique.size());
    for (const auto& item : unique) ordered.push_back(item.second);
    std::sort(ordered.begin(), ordered.end(), [](const SubfaceColor& lhs, const SubfaceColor& rhs) {
        if (lhs.confidence != rhs.confidence) return lhs.confidence > rhs.confidence;
        if (lhs.face_id != rhs.face_id) return lhs.face_id < rhs.face_id;
        return lhs.path < rhs.path;
    });

    const double ratio_product = double(original_face_count) * double(budget.maximum_added_ratio);
    const double ratio_epsilon = std::max(1.0, ratio_product) *
        double(std::numeric_limits<float>::epsilon()) * 2.0;
    const size_t ratio_limit = size_t(std::floor(ratio_product + ratio_epsilon));
    const size_t triangle_limit = std::min(budget.maximum_added_triangles, ratio_limit);
    // Every accepted leaf requires each ancestor on its midpoint path to be
    // split. One node adds three triangles regardless of tree depth.
    using SplitNode = std::tuple<size_t, uint8_t, uint8_t>;
    std::set<SplitNode> split_nodes;
    for (const SubfaceColor& candidate : ordered) {
        if (candidate.confidence < budget.minimum_confidence) {
            ++output.rejected_candidates;
            continue;
        }
        std::array<SplitNode, 3> required {};
        const size_t required_count = candidate.path.depth;
        for (uint8_t ancestor_depth = 0; ancestor_depth < candidate.path.depth; ++ancestor_depth) {
            const unsigned shift = 2u * unsigned(candidate.path.depth - ancestor_depth);
            const uint8_t prefix = ancestor_depth == 0 ? 0 : uint8_t(candidate.path.value >> shift);
            required[ancestor_depth] = {candidate.face_id, ancestor_depth, prefix};
        }
        size_t additional_nodes = 0;
        for (size_t i = 0; i < required_count; ++i)
            additional_nodes += split_nodes.count(required[i]) == 0;
        if ((split_nodes.size() + additional_nodes) * 3 > triangle_limit) {
            ++output.rejected_candidates;
            continue;
        }
        for (size_t i = 0; i < required_count; ++i) split_nodes.insert(required[i]);
        output.accepted.push_back(candidate);
    }
    output.added_triangles = split_nodes.size() * 3;
    std::sort(output.accepted.begin(), output.accepted.end(), [](const SubfaceColor& lhs, const SubfaceColor& rhs) {
        if (lhs.face_id != rhs.face_id) return lhs.face_id < rhs.face_id;
        return lhs.path < rhs.path;
    });
    return true;
}

bool map_subface_palette(const MeshSnapshot& source, const Analysis& analysis, const FaceColors& whole_face,
                         const std::vector<Color>& palette, const std::vector<Color>& portrait_card,
                         const SubfaceBudget& budget, SubfaceBudgetResult& output, std::string& error, const Cancel& cancel)
{
    output = {};
    error.clear();
    const size_t count = source.mesh.indices.size();
    if (!analysis.person_detected || analysis.canceled || !analysis.error.empty() ||
        analysis.geometry_id != source.geometry_id || analysis.content_id != source.content_id ||
        analysis.signature != analysis_cache_key(source, analysis.body_identity, analysis.face_identity,
                                                  analysis.boundary_identity) ||
        analysis.face_labels.size() != count || analysis.face_confidence.size() != count ||
        palette.empty() || palette.size() > 6 || !validate_snapshot(source).empty()) {
        error = "Semantic subface mapping requires current model recognition evidence.";
        return false;
    }
    for (const Color& color : palette) if (!valid_color(color)) {
        error = "Semantic subface mapping requires normalized palette colors.";
        return false;
    }

    const PalettePolicy policy(palette, portrait_card);
    MappedMaterials shared_materials;
    map_palette_impl(source, analysis, palette, portrait_card, &shared_materials, cancel, false);
    if (!shared_materials.discovery) { error="Material discovery unavailable or canceled."; return false; }
    const auto& discovery=*shared_materials.discovery;

    const auto appearance = [&source](const SubfaceLabelEvidence& evidence,Color& color) {
        return subface_appearance(source,evidence,color);
    };

    std::vector<std::vector<size_t>> faces_at_vertex(source.mesh.vertices.size());
    for (size_t face_id = 0; face_id < count; ++face_id)
        for (int corner = 0; corner < 3; ++corner)
            faces_at_vertex[source.mesh.indices[face_id][corner]].push_back(face_id);

    const auto& leaf_component_at_face=discovery.leaf_component_at_face;
    const auto& leaf_centers=discovery.leaf_centers;
    const auto shared_target = [&](size_t face_id, Label label, const Color& original,
                                   Color* selected_center = nullptr) {
        const MappedMaterial* selected = nullptr;
        float best = std::numeric_limits<float>::max();
        const auto consider = [&](size_t neighbor) {
            if (neighbor >= discovery.face_material.size()) return;
            const size_t material_id = discovery.face_material[neighbor];
            if (material_id >= shared_materials.entries.size()) return;
            const auto& material = shared_materials.entries[material_id];
            if (material.label != label && !(label == Label::FaceSkin && material.label == Label::BodySkin)) return;
            const float score = distance(original, material.center);
            if (score < best) { selected = &material; best = score; }
        };
        consider(face_id);
        // A reliable same-label root already owns its entire child tree.
        if (!selected && face_id < count) for (int corner = 0; corner < 3; ++corner)
            for (size_t neighbor : faces_at_vertex[source.mesh.indices[face_id][corner]]) consider(neighbor);
        if (selected) {
            if (selected_center) *selected_center = selected->center;
            return selected->target;
        }
        const auto component = leaf_component_at_face.find({face_id, label});
        if (component == leaf_component_at_face.end()) return palette.size();
        const auto found = leaf_centers.find(component->second);
        if (found == leaf_centers.end() || found->second.empty()) return palette.size();
        const Color* center = &found->second.front();
        for (const auto& candidate : found->second)
            if (distance(original, candidate) < distance(original, *center)) center = &candidate;
        if (selected_center) *selected_center = *center;
        return policy.choose(*center, label, true);
    };

    const auto& sclera_centers=discovery.sclera_centers;
    const auto& sclera_support_component=discovery.sclera_support_component;
    const auto local_sclera_center = [&](size_t face_id) -> const Color* {
        if (face_id >= count) return nullptr;
        const auto found = sclera_centers.find(sclera_support_component[face_id]);
        return found == sclera_centers.end() ? nullptr : &found->second;
    };
    const auto compatible_local_sclera = [&](size_t face_id, const Color& color) {
        const Color* center = local_sclera_center(face_id);
        return center && compatible_sclera(color, *center);
    };

    std::vector<uint8_t> compatible_sclera_leaf(analysis.subface_labels.size(), 0);
    for (size_t index = 0; index < analysis.subface_labels.size(); ++index) {
        const auto& evidence = analysis.subface_labels[index];
        if (evidence.label != Label::EyeSclera || evidence.confidence < minimum_confidence ||
            evidence.face_id >= count ||
            !local_sclera_center(evidence.face_id)) continue;
        Color source_color;
        if (!appearance(evidence, source_color) || !compatible_local_sclera(evidence.face_id, source_color)) continue;
        const float source_chroma = chroma(source_color);
        // A single direct raster sample is useful on a dense mesh, but a dark,
        // warm singleton at the eye contour is more likely adjacent skin. More
        // samples retain the wider reflected-eye-white tolerance.
        if (evidence.samples == 1 &&
            (source_chroma > .05f || (source_chroma > .03f && source_color[0] < .60f))) continue;
            compatible_sclera_leaf[index] = 1;
    }
    std::map<size_t, Color> roots;
    for (const auto& item : whole_face) roots[item.first] = item.second;
    SubfaceColors candidates;
    candidates.reserve(analysis.subface_labels.size() + roots.size() * 4);
    struct SkinBoundarySeed {
        size_t face_id;
        SubfacePath path;
        Color appearance;
        Color target;
        float confidence;
    };
    struct ScleraBoundarySeed {
        size_t face_id;
        SubfacePath path;
        Color appearance;
        Color target;
        float confidence;
    };
    std::set<size_t> refined_boundary_faces;
    for (const auto& evidence : analysis.subface_labels)
        if (evidence.path.depth == 3 && evidence.confidence >= minimum_confidence)
            refined_boundary_faces.insert(evidence.face_id);
    std::vector<SkinBoundarySeed> skin_boundary_seeds;
    std::vector<ScleraBoundarySeed> sclera_boundary_seeds;
    std::set<std::pair<size_t, SubfacePath>> protected_detail_leaves;
    std::set<std::pair<size_t, SubfacePath>> non_sclera_detail_leaves;
    std::set<std::pair<size_t, SubfacePath>> iris_detail_leaves;
    std::set<size_t> iris_detail_faces;
    std::set<size_t> direct_skin_detail_faces;
    for (const SubfaceLabelEvidence& evidence : analysis.subface_labels)
        if (evidence.confidence >= minimum_confidence) {
            if (evidence.label != Label::FaceSkin)
                protected_detail_leaves.emplace(evidence.face_id, evidence.path);
            if (evidence.label != Label::EyeSclera)
                non_sclera_detail_leaves.emplace(evidence.face_id, evidence.path);
            if (evidence.label == Label::Iris) {
                iris_detail_leaves.emplace(evidence.face_id, evidence.path);
                iris_detail_faces.insert(evidence.face_id);
            }
            if (evidence.label == Label::FaceSkin)
                direct_skin_detail_faces.insert(evidence.face_id);
        }
    const auto baseline_subface_allows = [&](const SubfaceLabelEvidence& evidence) {
        if (evidence.face_id < analysis.baseline_face_labels.size() &&
            !compatible_boundary_detail(analysis.baseline_face_labels[evidence.face_id], evidence.label))
            return false;
        for (const auto& baseline : analysis.baseline_subface_labels) {
            if (baseline.face_id != evidence.face_id || !protected_face_detail(baseline.label)) continue;
            const bool same_or_descendant = baseline.path.depth == evidence.path.depth
                ? baseline.path.value == evidence.path.value
                : baseline.path.depth < evidence.path.depth &&
                  (evidence.path.value >> (2u * (evidence.path.depth - baseline.path.depth))) == baseline.path.value;
            if (same_or_descendant && !compatible_boundary_detail(baseline.label, evidence.label)) return false;
        }
        return true;
    };
    for (size_t evidence_index = 0; evidence_index < analysis.subface_labels.size(); ++evidence_index) {
        const SubfaceLabelEvidence& evidence = analysis.subface_labels[evidence_index];
        if (!baseline_subface_allows(evidence)) continue;
        Color source_color;
        if (evidence.confidence < minimum_confidence || !appearance(evidence, source_color)) continue;
        size_t target = palette.size();
        if (evidence.label == Label::EyeSclera) {
            if (!compatible_sclera_leaf[evidence_index]) continue;
            target = shared_target(evidence.face_id, evidence.label, source_color);
        } else if (evidence.label == Label::FaceSkin) {
            const auto root = roots.find(evidence.face_id);
            const Label root_label = analysis.face_labels[evidence.face_id];
            if ((root_label != Label::EyeSclera && root_label != Label::Hair) ||
                analysis.face_confidence[evidence.face_id] < minimum_confidence) continue;
            std::vector<Sample> local_skin;
            std::set<size_t> visited;
            for (int corner = 0; corner < 3; ++corner) {
                for (size_t neighbor : faces_at_vertex[source.mesh.indices[evidence.face_id][corner]]) {
                    if (!visited.insert(neighbor).second ||
                        (analysis.face_labels[neighbor] != Label::FaceSkin &&
                         analysis.face_labels[neighbor] != Label::BodySkin) ||
                        analysis.face_confidence[neighbor] < minimum_confidence) continue;
                    local_skin.push_back({face_lab(source, neighbor), area(source, neighbor)});
                }
            }
            const auto centers = prototypes(local_skin, 1);
            if (centers.empty() || !compatible_skin(source_color, centers.front(), Label::FaceSkin)) continue;
            // FaceSkin detail is emitted by the face adapter for the ear window.
            // On a Hair root it may recover only the locally skin-compatible
            // leaves touching reliable skin; detached brown hair cannot prove
            // itself to be an ear from its own color.
            if (root_label == Label::Hair && root == roots.end()) continue;
            target = shared_target(evidence.face_id, Label::FaceSkin, source_color);
        } else if (evidence.label == Label::Iris || evidence.label == Label::Eyebrow) {
            if (evidence.label == Label::Eyebrow &&
                (!natural_dark_hair(source_color) || evidence.face_id >= discovery.eyebrow_supported_faces.size() ||
                 !discovery.eyebrow_supported_faces[evidence.face_id])) continue;
            target = shared_target(evidence.face_id, evidence.label, source_color);
        } else if (evidence.label == Label::Hair) {
            const Label root_label = analysis.face_labels[evidence.face_id];
            if (root_label != Label::FaceSkin && root_label != Label::Unknown && root_label != Label::Hair) continue;
            bool reliable_hair_neighbor = root_label == Label::Hair &&
                analysis.face_confidence[evidence.face_id] >= minimum_confidence;
            for (int corner = 0; corner < 3; ++corner)
                for (size_t neighbor : faces_at_vertex[source.mesh.indices[evidence.face_id][corner]])
                    reliable_hair_neighbor |= analysis.face_labels[neighbor] == Label::Hair &&
                        analysis.face_confidence[neighbor] >= minimum_confidence;
            Color center;
            target = shared_target(evidence.face_id, Label::Hair, source_color, &center);
            if (!reliable_hair_neighbor || target >= palette.size() ||
                (!same_hair_material(source_color, center) && distance(source_color, center) > .0064f)) continue;
        }
        if (target >= palette.size()) continue;
        const auto root = roots.find(evidence.face_id);
        if (root != roots.end() && root->second == palette[target] &&
            (evidence.label == Label::Eyebrow || evidence.label == Label::Iris ||
             evidence.label == Label::EyeSclera)) {
            const auto decision = policy.decide(source_color, evidence.label);
            size_t alternative = palette.size();
            float best = std::numeric_limits<float>::max();
            for (const auto& candidate : decision.candidates) {
                if (!candidate.accepted || candidate.palette_index >= palette.size() ||
                    palette[candidate.palette_index] == root->second ||
                    candidate.total_cost > decision.best_cost + .40f) continue;
                if (evidence.label == Label::Eyebrow &&
                    policy.palette_labs[candidate.palette_index][0] > source_color[0] + .45f) continue;
                if (candidate.total_cost < best) { best = candidate.total_cost; alternative = candidate.palette_index; }
            }
            if (alternative < palette.size()) target = alternative;
        }
        if (root != roots.end() && root->second == palette[target]) continue;
        candidates.push_back({evidence.face_id, evidence.path, palette[target], evidence.confidence});
        if (evidence.label == Label::EyeSclera)
            sclera_boundary_seeds.push_back({evidence.face_id, evidence.path, source_color,
                                             palette[target], evidence.confidence});
        if (evidence.label == Label::FaceSkin &&
            analysis.face_labels[evidence.face_id] == Label::Hair)
            skin_boundary_seeds.push_back({evidence.face_id, evidence.path, source_color,
                                           palette[target], evidence.confidence});
    }

    // Close at most two raster-sized gaps inside a coarse eye triangle. Each
    // synchronous ring must share a full sub-edge with the preceding reliable
    // or inferred sclera leaf, remain locally color-continuous, and avoid any
    // explicit skin, iris or brow evidence. The bounded second ring reaches a
    // narrow core gap without allowing a seed to flood its whole root face.
    // The later .85 skin recovery still wins over these .80 inferred leaves on
    // a warm eyelid face.
    std::set<std::pair<size_t, SubfacePath>> accepted_sclera_leaves;
    std::map<std::pair<size_t, SubfacePath>, ScleraBoundarySeed> accepted_sclera_evidence;
    for (const ScleraBoundarySeed& seed : sclera_boundary_seeds)
        accepted_sclera_evidence.emplace(
            std::make_pair(seed.face_id, seed.path), seed);
    for (const auto& item : accepted_sclera_evidence)
        accepted_sclera_leaves.insert(item.first);
    std::vector<ScleraBoundarySeed> sclera_frontier = sclera_boundary_seeds;
    for (uint8_t ring = 0; ring < 2 && !sclera_frontier.empty(); ++ring) {
        std::vector<ScleraBoundarySeed> next_frontier;
        for (const ScleraBoundarySeed& seed : sclera_frontier) {
            if (refined_boundary_faces.count(seed.face_id) != 0) continue;
            for (uint8_t value = 0; value < 16; ++value) {
                const SubfacePath path {2, value};
                const auto key = std::make_pair(seed.face_id, path);
                if (!subfaces_share_edge(seed.path, path) ||
                    accepted_sclera_leaves.count(key) != 0 ||
                    non_sclera_detail_leaves.count(key) != 0) continue;
                const SubfaceLabelEvidence neighbor {seed.face_id, path, Label::EyeSclera,
                                                      seed.confidence, 0};
                Color neighbor_color;
                if (!appearance(neighbor, neighbor_color) ||
                    !compatible_local_sclera(seed.face_id, neighbor_color) ||
                    distance(neighbor_color, seed.appearance) > .0064f) continue;
                accepted_sclera_leaves.insert(key);
                const float confidence = std::min(seed.confidence, .80f);
                candidates.push_back({seed.face_id, path, seed.target, confidence});
                const ScleraBoundarySeed inferred {seed.face_id, path, neighbor_color, seed.target, confidence};
                accepted_sclera_evidence.emplace(key, inferred);
                next_frontier.push_back(inferred);
            }
        }
        sclera_frontier.swap(next_frontier);
    }

    // Close one eye-white gap across an original-face edge. Only depth-2 leaf
    // edges that coincide exactly in model space are paired; point contacts,
    // projected neighbors and nonmanifold boundaries are excluded. Proposals
    // are synchronous, so a newly inferred leaf cannot continue into another
    // root face during this pass.
    struct BoundaryLeaf { size_t face_id; SubfacePath path; };
    // Depth two divides every original edge into four exact topological
    // segments. Key them by original vertex ids and segment ordinal instead of
    // interpolated positions; adjacent faces may evaluate the same point in a
    // different floating-point order.
    using BoundaryKey = std::array<size_t, 4>;
    std::set<size_t> boundary_faces;
    std::set<std::pair<size_t, size_t>> boundary_face_pairs;
    for (const auto& item : accepted_sclera_evidence) {
        const size_t face_id = item.first.first;
        boundary_faces.insert(face_id);
        std::map<size_t, uint8_t> shared_vertices;
        for (int corner = 0; corner < 3; ++corner)
            for (size_t neighbor : faces_at_vertex[source.mesh.indices[face_id][corner]])
                if (neighbor != face_id) ++shared_vertices[neighbor];
        for (const auto& neighbor : shared_vertices) if (neighbor.second >= 2) {
            boundary_faces.insert(neighbor.first);
            boundary_face_pairs.emplace(std::min(face_id, neighbor.first),
                                        std::max(face_id, neighbor.first));
        }
    }
    std::map<BoundaryKey, std::vector<BoundaryLeaf>> boundary_leaves;
    for (size_t face_id : boundary_faces) for (uint8_t value = 0; value < 16; ++value) {
        if (refined_boundary_faces.count(face_id) != 0) continue;
        const SubfacePath path {2, value};
        std::array<Barycentric, 3> leaf_vertices;
        if (!subface_vertices(path, leaf_vertices)) continue;
        for (int edge = 0; edge < 3; ++edge) {
            const int next = (edge + 1) % 3;
            int zero_axis = -1;
            for (int axis = 0; axis < 3; ++axis)
                if (leaf_vertices[edge][axis] == 0.f && leaf_vertices[next][axis] == 0.f) {
                    zero_axis = axis;
                    break;
                }
            if (zero_axis < 0) continue;
            const int first_axis = (zero_axis + 1) % 3;
            const int second_axis = (zero_axis + 2) % 3;
            const size_t first_vertex = size_t(source.mesh.indices[face_id][first_axis]);
            const size_t second_vertex = size_t(source.mesh.indices[face_id][second_axis]);
            const int parameter_axis = first_vertex < second_vertex ? second_axis : first_axis;
            const size_t first_quarter = size_t(std::lround(leaf_vertices[edge][parameter_axis] * 4.f));
            const size_t second_quarter = size_t(std::lround(leaf_vertices[next][parameter_axis] * 4.f));
            boundary_leaves[{std::min(first_vertex, second_vertex),
                             std::max(first_vertex, second_vertex),
                             std::min(first_quarter, second_quarter),
                             std::max(first_quarter, second_quarter)}].push_back({face_id, path});
        }
    }
    std::map<std::pair<size_t, SubfacePath>, ScleraBoundarySeed> cross_face_proposals;
    const auto propose_cross_face = [&](const BoundaryLeaf& from, const BoundaryLeaf& to) {
        const auto seed = accepted_sclera_evidence.find({from.face_id, from.path});
        const auto key = std::make_pair(to.face_id, to.path);
        if (seed == accepted_sclera_evidence.end() || accepted_sclera_leaves.count(key) != 0 ||
            non_sclera_detail_leaves.count(key) != 0 || to.face_id >= count ||
            sclera_support_component[from.face_id] != sclera_support_component[to.face_id]) return;
        const auto root = roots.find(to.face_id);
        if (root != roots.end() && root->second == seed->second.target) return;
        const Label root_label = analysis.face_labels[to.face_id];
        if (root_label != Label::EyeSclera && root_label != Label::FaceSkin &&
            root_label != Label::Unknown) return;
        const SubfaceLabelEvidence neighbor {to.face_id, to.path, Label::EyeSclera,
                                              seed->second.confidence, 0};
        Color neighbor_color;
        if (!appearance(neighbor, neighbor_color) ||
            !compatible_local_sclera(to.face_id, neighbor_color) ||
            distance(neighbor_color, seed->second.appearance) > .0064f) return;
        const float confidence = std::min(seed->second.confidence, .78f);
        cross_face_proposals.emplace(key, ScleraBoundarySeed {
            to.face_id, to.path, neighbor_color, seed->second.target, confidence});
    };
    for (const auto& boundary : boundary_leaves) {
        if (boundary.second.size() != 2 ||
            boundary.second[0].face_id == boundary.second[1].face_id) continue;
        const auto face_pair = std::minmax(boundary.second[0].face_id,
                                           boundary.second[1].face_id);
        if (boundary_face_pairs.count(face_pair) == 0) continue;
        propose_cross_face(boundary.second[0], boundary.second[1]);
        propose_cross_face(boundary.second[1], boundary.second[0]);
    }
    std::map<size_t, size_t> landing_face_seed_count;
    for (const auto& proposal : cross_face_proposals)
        ++landing_face_seed_count[proposal.second.face_id];
    std::set<std::pair<size_t, SubfacePath>> final_sclera_support = accepted_sclera_leaves;
    std::map<size_t, size_t> final_sclera_support_count;
    for (const auto& item : final_sclera_support)
        ++final_sclera_support_count[item.first];
    for (const auto& proposal : cross_face_proposals) {
        candidates.push_back({proposal.second.face_id, proposal.second.path,
                              proposal.second.target, proposal.second.confidence});
        if (final_sclera_support.emplace(proposal.first).second)
            ++final_sclera_support_count[proposal.second.face_id];
        // The raster may miss the leaf immediately behind the shared edge on
        // the landing face. Close exactly one local ring only when at least two
        // independent edge segments support that face and it has no explicit
        // iris detail. The tighter local color threshold admits a continuous
        // eye-white patch without spreading into a similarly pale iris rim.
        // These leaves never enter the cross-face frontier, so they cannot
        // reach a second original face.
        if (landing_face_seed_count[proposal.second.face_id] < 2 ||
            iris_detail_faces.count(proposal.second.face_id) != 0) continue;
        for (uint8_t value = 0; value < 16; ++value) {
            const SubfacePath path {2, value};
            const auto key = std::make_pair(proposal.second.face_id, path);
            if (!subfaces_share_edge(proposal.second.path, path) ||
                accepted_sclera_leaves.count(key) != 0 ||
                non_sclera_detail_leaves.count(key) != 0 ||
                cross_face_proposals.count(key) != 0) continue;
            const SubfaceLabelEvidence neighbor {proposal.second.face_id, path, Label::EyeSclera,
                                                  proposal.second.confidence, 0};
            Color neighbor_color;
            if (!appearance(neighbor, neighbor_color) ||
                !compatible_local_sclera(proposal.second.face_id, neighbor_color) ||
                distance(neighbor_color, proposal.second.appearance) > .0001f) continue;
            candidates.push_back({proposal.second.face_id, path, proposal.second.target,
                                  std::min(proposal.second.confidence, .76f)});
            if (final_sclera_support.emplace(key).second)
                ++final_sclera_support_count[proposal.second.face_id];
        }
    }

    // Reconsider only direct sclera observations on a face whose final local
    // component is already established. Multiple raster samples may prove a
    // neutral eye-white shadow that falls below the general material floor;
    // a warm dark singleton remains blocked at the eye/skin boundary.
    for (const SubfaceLabelEvidence& evidence : analysis.subface_labels) {
        if (evidence.label != Label::EyeSclera || evidence.confidence < minimum_confidence ||
            evidence.face_id >= count || final_sclera_support_count[evidence.face_id] < 3 ||
            refined_boundary_faces.count(evidence.face_id) != 0) continue;
        const auto key = std::make_pair(evidence.face_id, evidence.path);
        if (final_sclera_support.count(key) != 0 ||
            non_sclera_detail_leaves.count(key) != 0) continue;
        const Label root_label = analysis.face_labels[evidence.face_id];
        if (root_label != Label::EyeSclera && root_label != Label::FaceSkin &&
            root_label != Label::Unknown) continue;
        Color source_color;
        const Color* supported_center = local_sclera_center(evidence.face_id);
        if (!supported_center || !appearance(evidence, source_color)) continue;
        const float source_chroma = chroma(source_color);
        size_t shared_sclera_support = 0;
        for (const auto& support : final_sclera_support)
            if (support.first == evidence.face_id &&
                subfaces_share_edge(support.second, evidence.path))
                ++shared_sclera_support;
        bool adjacent_iris = false;
        for (const auto& detail : iris_detail_leaves)
            if (detail.first == evidence.face_id &&
                subfaces_share_edge(detail.second, evidence.path)) {
                adjacent_iris = true;
                break;
            }
        const bool supported_singleton_gap = evidence.samples == 1 &&
            root_label == Label::Unknown && iris_detail_faces.count(evidence.face_id) == 0 &&
            shared_sclera_support > 0 && source_color[0] >= .40f &&
            source_color[0] >= (*supported_center)[0] - .19f && source_chroma <= .035f &&
            !(source_chroma > .03f && source_color[0] < .60f);
        const bool supported_deep_iris_edge = evidence.samples >= 2 &&
            root_label == Label::FaceSkin && adjacent_iris && source_color[0] >= .40f &&
            source_color[0] >= (*supported_center)[0] - .27f && source_chroma <= .01f;
        if (!supported_singleton_gap && !supported_deep_iris_edge) continue;
        const size_t target = shared_target(evidence.face_id, Label::EyeSclera, source_color);
        if (target >= palette.size()) continue;
        const auto root = roots.find(evidence.face_id);
        if (root != roots.end() && root->second == palette[target]) continue;
        candidates.push_back({evidence.face_id, evidence.path, palette[target],
                              std::min(evidence.confidence, .82f)});
        if (final_sclera_support.emplace(key).second)
            ++final_sclera_support_count[evidence.face_id];
    }

    // A 512px face crop can miss one depth-2 leaf along a narrow ear rim even
    // when the adjacent leaf is a reliable ear-skin observation. Fill only one
    // shared-edge ring inside the same Hair root. The neighboring leaf must
    // retain the same intrinsic skin appearance, and explicit eye/brow detail
    // evidence always blocks the fill.
    for (const SkinBoundarySeed& seed : skin_boundary_seeds) {
        if (refined_boundary_faces.count(seed.face_id) != 0) continue;
        for (uint8_t value = 0; value < 16; ++value) {
            const SubfacePath path {2, value};
            if (!subfaces_share_edge(seed.path, path) ||
                protected_detail_leaves.count({seed.face_id, path}) != 0) continue;
            const SubfaceLabelEvidence neighbor {seed.face_id, path, Label::FaceSkin,
                                                  seed.confidence, 0};
            Color neighbor_color;
            if (!appearance(neighbor, neighbor_color) ||
                !compatible_skin(neighbor_color, seed.appearance, Label::FaceSkin)) continue;
            candidates.push_back({seed.face_id, path, seed.target,
                                  std::min(seed.confidence, .80f)});
        }
    }

    // Generated portrait meshes may split the visible ear rim across nearby
    // surface patches that share neither an edge nor a vertex. Bridge that
    // seam once from the adapter's direct ear-skin leaves. Candidates must be
    // reliable Hair roots with matching intrinsic skin color and surface
    // direction. Accepted candidates never become new spatial seeds.
    if (!skin_boundary_seeds.empty()) {
        Vec3f minimum = source.mesh.vertices.front(), maximum = minimum;
        for (const Vec3f& vertex : source.mesh.vertices) {
            minimum = minimum.cwiseMin(vertex);
            maximum = maximum.cwiseMax(vertex);
        }
        const float radius = (maximum - minimum).norm() * .012f;
        if (radius > 0.f && std::isfinite(radius)) {
            using Cell = std::array<long long, 3>;
            struct SpatialSkinSeed {
                SkinBoundarySeed seed;
                Vec3f center;
                Vec3f normal;
            };
            const auto cell = [radius](const Vec3f& point) {
                Cell result {};
                for (int axis = 0; axis < 3; ++axis)
                    result[axis] = static_cast<long long>(std::floor(point[axis] / radius));
                return result;
            };
            const auto leaf_center = [&](size_t face_id, const SubfacePath& path, Vec3f& output) {
                std::array<Barycentric, 3> vertices;
                if (face_id >= count || !subface_vertices(path, vertices)) return false;
                Barycentric centroid {};
                for (const Barycentric& vertex : vertices)
                    for (size_t axis = 0; axis < 3; ++axis)
                        centroid[axis] += vertex[axis] / 3.f;
                output = Vec3f::Zero();
                for (size_t corner = 0; corner < 3; ++corner)
                    output += centroid[corner] * source.mesh.vertices[source.mesh.indices[face_id][corner]];
                return output.allFinite();
            };
            const auto face_normal = [&](size_t face_id) {
                const auto& triangle = source.mesh.indices[face_id];
                Vec3f normal = (source.mesh.vertices[triangle[1]] - source.mesh.vertices[triangle[0]])
                    .cross(source.mesh.vertices[triangle[2]] - source.mesh.vertices[triangle[0]]);
                return normal.squaredNorm() > 1e-18f ? normal.normalized() : Vec3f::Zero();
            };
            std::vector<SpatialSkinSeed> spatial_seeds;
            std::map<Cell, std::vector<size_t>> seed_cells;
            for (const SkinBoundarySeed& seed : skin_boundary_seeds) {
                if (refined_boundary_faces.count(seed.face_id) != 0) continue;
                Vec3f center;
                if (!leaf_center(seed.face_id, seed.path, center)) continue;
                const Vec3f normal = face_normal(seed.face_id);
                if (normal.squaredNorm() <= 0.f) continue;
                seed_cells[cell(center)].push_back(spatial_seeds.size());
                spatial_seeds.push_back({seed, center, normal});
            }
            const float radius_squared = radius * radius;
            for (size_t face_id = 0; face_id < count; ++face_id) {
                const auto root = roots.find(face_id);
                if (analysis.face_labels[face_id] != Label::Hair ||
                    refined_boundary_faces.count(face_id) != 0 ||
                    analysis.face_confidence[face_id] < minimum_confidence || root == roots.end()) continue;
                const Vec3f normal = face_normal(face_id);
                if (normal.squaredNorm() <= 0.f) continue;
                for (uint8_t value = 0; value < 16; ++value) {
                    const SubfacePath path {2, value};
                    const auto key = std::make_pair(face_id, path);
                    if (protected_detail_leaves.count(key) != 0) continue;
                    Vec3f center;
                    if (!leaf_center(face_id, path, center)) continue;
                    const Cell origin = cell(center);
                    const SpatialSkinSeed* nearest = nullptr;
                    float best_distance = radius_squared;
                    for (int z = -1; z <= 1; ++z)
                        for (int y = -1; y <= 1; ++y)
                            for (int x = -1; x <= 1; ++x) {
                                const auto found = seed_cells.find({origin[0] + x, origin[1] + y, origin[2] + z});
                                if (found == seed_cells.end()) continue;
                                for (size_t index : found->second) {
                                    const SpatialSkinSeed& seed = spatial_seeds[index];
                                    if (root->second == seed.seed.target || normal.dot(seed.normal) < .75f) continue;
                                    const float distance = (center - seed.center).squaredNorm();
                                    if (distance < best_distance) { best_distance = distance; nearest = &seed; }
                                }
                            }
                    if (!nearest) continue;
                    const SubfaceLabelEvidence evidence {face_id, path, Label::FaceSkin,
                                                          nearest->seed.confidence, 0};
                    Color source_color;
                    if (!appearance(evidence, source_color) ||
                        !compatible_skin(source_color, nearest->seed.appearance, Label::FaceSkin)) continue;
                    candidates.push_back({face_id, path, nearest->seed.target,
                                          std::min(nearest->seed.confidence, .76f)});
                }
            }
        }
    }

    // A coarse eyelid triangle can receive a reliable whole-face sclera vote
    // even when its intrinsic material is skin. Correct only a complete warm
    // skin-colored face attached to reliable local skin. Dark sclera shadows
    // remain neutral in hue and do not satisfy this material signature.
    for (size_t face_id = 0; face_id < count; ++face_id) {
        const auto root = roots.find(face_id);
        // Whole-face mapping may omit a warm eye-mask face when it fails the
        // sclera material test. Such a face still needs a skin child override
        // if reliable skin touches it; absence from roots is evidence of
        // rejection, not evidence that the existing material is correct.
        if (!local_sclera_center(face_id) ||
            (root != roots.end() && direct_skin_detail_faces.count(face_id) == 0) ||
            face_id >= count || refined_boundary_faces.count(face_id) != 0 || analysis.face_labels[face_id] != Label::EyeSclera ||
            analysis.face_confidence[face_id] < minimum_confidence) continue;
        std::set<size_t> visited;
        std::vector<Sample> local_skin;
        for (int corner = 0; corner < 3; ++corner) {
            for (size_t neighbor : faces_at_vertex[source.mesh.indices[face_id][corner]]) {
                if ((analysis.face_labels[neighbor] != Label::FaceSkin &&
                     analysis.face_labels[neighbor] != Label::BodySkin) ||
                    analysis.face_confidence[neighbor] < minimum_confidence) continue;
                if (visited.insert(neighbor).second)
                    local_skin.push_back({face_lab(source, neighbor), area(source, neighbor)});
            }
        }
        if (local_skin.empty()) continue;
        const auto skin_centers = prototypes(local_skin, 1);
        if (skin_centers.empty()) continue;
        const size_t skin_target = shared_target(face_id, Label::FaceSkin, skin_centers.front());
        if (skin_target >= palette.size() ||
            (root != roots.end() && palette[skin_target] == root->second)) continue;
        const Color face_color = face_lab(source, face_id);
        const float face_chroma = chroma(face_color);
        const bool dark_warm_skin = face_color[0] < .48f && face_chroma > .037f &&
            face_color[1] > 0.f && face_color[2] > 0.f;
        const bool red_yellow_skin = face_chroma > .045f && face_color[2] > .012f &&
            face_color[1] > face_color[2] * 1.35f;
        if ((!dark_warm_skin && !red_yellow_skin) ||
            !compatible_skin(face_color, skin_centers.front(), Label::FaceSkin)) continue;
        for (uint8_t value = 0; value < 16; ++value)
            candidates.push_back({face_id, {2, value}, palette[skin_target], .85f});
    }
    if (stopped(cancel)) { error="Material mapping canceled."; return false; }
    if (analysis.baseline_subface_labels.empty())
        return enforce_subface_budget(candidates, count, budget, output, error);
    Analysis baseline = analysis;
    baseline.subface_labels = analysis.baseline_subface_labels;
    baseline.baseline_subface_labels.clear();
    if (!analysis.baseline_face_labels.empty()) {
        baseline.face_labels = analysis.baseline_face_labels;
        baseline.face_confidence = analysis.baseline_face_confidence;
    }
    baseline.baseline_face_labels.clear(); baseline.baseline_face_confidence.clear();
    const auto safe_whole = map_palette_impl(source, baseline, palette, portrait_card, nullptr, cancel);
    SubfaceBudgetResult safe;
    if (!map_subface_palette(source, baseline, safe_whole, palette, portrait_card, budget, safe, error, cancel)) return false;
    std::map<size_t, SubfaceColors> upgrades;
    for (const auto& candidate : candidates)
        if (refined_boundary_faces.count(candidate.face_id) != 0 && candidate.confidence >= budget.minimum_confidence)
            upgrades[candidate.face_id].push_back(candidate);
    size_t rejected = safe.rejected_candidates;
    std::map<size_t, SubfaceColors> accepted_faces;
    for (const auto& leaf : safe.accepted) accepted_faces[leaf.face_id].push_back(leaf);
    const auto cost = [](const SubfaceColors& leaves) {
        std::set<std::pair<uint8_t, uint8_t>> nodes;
        for (const auto& leaf : leaves) for (uint8_t depth = 0; depth < leaf.path.depth; ++depth)
            nodes.emplace(depth, depth == 0 ? 0 : uint8_t(leaf.path.value >> (2u * (leaf.path.depth - depth))));
        return nodes.size() * 3;
    };
    const double ratio = double(count) * double(budget.maximum_added_ratio);
    const size_t limit = std::min(budget.maximum_added_triangles,
        size_t(std::floor(ratio + std::max(1.0, ratio) * double(std::numeric_limits<float>::epsilon()) * 2.0)));
    size_t used = safe.added_triangles;
    struct RankedUpgrade { size_t face_id; const SubfaceColors* leaves; float benefit; };
    std::vector<RankedUpgrade> ranked_upgrades;
    for(const auto& upgrade:upgrades){float benefit=0.f;for(const auto& leaf:upgrade.second)benefit=std::max(benefit,leaf.confidence);
        ranked_upgrades.push_back({upgrade.first,&upgrade.second,benefit});}
    std::sort(ranked_upgrades.begin(),ranked_upgrades.end(),[](const auto& lhs,const auto& rhs){
        return lhs.benefit!=rhs.benefit?lhs.benefit>rhs.benefit:lhs.face_id<rhs.face_id;});
    for (const auto& upgrade : ranked_upgrades) {
        SubfaceBudgetResult checked;
        if (!enforce_subface_budget(*upgrade.leaves, count, budget, checked, error)) return false;
        const auto previous = accepted_faces.find(upgrade.face_id);
        const size_t old_cost = previous == accepted_faces.end() ? 0 : cost(previous->second);
        if (checked.rejected_candidates != 0 || used - old_cost + checked.added_triangles > limit) {
            rejected += upgrade.leaves->size(); continue;
        }
        used = used - old_cost + checked.added_triangles;
        accepted_faces[upgrade.face_id] = std::move(checked.accepted);
    }
    safe.accepted.clear();
    for (const auto& face : accepted_faces)
        safe.accepted.insert(safe.accepted.end(), face.second.begin(), face.second.end());
    safe.added_triangles = used;
    safe.rejected_candidates = rejected;
    output = std::move(safe);
    return true;
}

SubfaceColors remap_subface_palette_targets(const SubfaceColors& suggestions,
                                            const std::vector<Color>& original_candidates,
                                            const std::vector<Color>& target_candidates)
{
    if (original_candidates.empty() || original_candidates.size() > 6 ||
        target_candidates.size() != original_candidates.size()) return {};
    for (size_t slot = 0; slot < original_candidates.size(); ++slot) {
        if (!valid_color(original_candidates[slot]) || !valid_color(target_candidates[slot])) return {};
        for (size_t previous = 0; previous < slot; ++previous)
            if (original_candidates[previous] == original_candidates[slot] &&
                target_candidates[previous] != target_candidates[slot]) return {};
    }
    SubfaceColors result;
    result.reserve(suggestions.size());
    for (const SubfaceColor& suggestion : suggestions) {
        const auto found = std::find(original_candidates.begin(), original_candidates.end(), suggestion.color);
        if (found == original_candidates.end()) return {};
        SubfaceColor mapped = suggestion;
        mapped.color = target_candidates[size_t(found - original_candidates.begin())];
        result.push_back(mapped);
    }
    return result;
}

SubfaceColors compose_subfaces(const SubfaceColors& automatic, const FaceColors& manual,
                               bool automatic_enabled)
{
    if (!automatic_enabled) return {};
    std::set<size_t> manual_faces;
    for (const auto& item : manual) manual_faces.insert(item.first);
    SubfaceColors result;
    result.reserve(automatic.size());
    for (const SubfaceColor& item : automatic)
        if (manual_faces.count(item.face_id) == 0) result.push_back(item);
    return result;
}

nlohmann::json encode_analysis(const Analysis& analysis)
{
    if (analysis.canceled || !analysis.error.empty() || analysis.signature.empty() ||
        analysis.geometry_id.empty() || analysis.content_id.empty() || analysis.body_identity.empty() || analysis.face_identity.empty() ||
        analysis.boundary_identity.empty() ||
        analysis.rendered_views != 8 || analysis.face_views > 8 || analysis.face_labels.empty() ||
        analysis.observed_faces > analysis.face_labels.size() || (analysis.person_detected && analysis.face_views == 0) ||
        analysis.face_labels.size() != analysis.face_confidence.size() || analysis.face_labels.size() > maximum_faces)
        throw std::invalid_argument("Cannot cache incomplete semantic analysis.");
    constexpr char hex[] = "0123456789abcdef";
    std::string labels, confidence;
    labels.reserve(analysis.face_labels.size()); confidence.reserve(analysis.face_labels.size() * 8);
    for (size_t i = 0; i < analysis.face_labels.size(); ++i) {
        const float value = analysis.face_confidence[i];
        if (!valid_label(analysis.face_labels[i]) || !std::isfinite(value) || value < 0 || value > 1)
            throw std::invalid_argument("Invalid semantic label or confidence.");
        labels += hex[size_t(analysis.face_labels[i])];
        // Preserve float32 exactly so a cache round-trip cannot move a face
        // across the automatic-paint confidence threshold.
        uint32_t encoded; std::memcpy(&encoded, &value, sizeof(encoded));
        for (int nibble = 0; nibble < 8; ++nibble) confidence += hex[(encoded >> (nibble * 4)) & 15];
    }
    if (analysis.subface_labels.size() > maximum_subface_evidence)
        throw std::invalid_argument("Too much semantic subface evidence to cache.");
    nlohmann::json subfaces = nlohmann::json::array();
    std::pair<size_t, SubfacePath> previous_key {0, {0, 0}};
    bool has_previous = false;
    for (const SubfaceLabelEvidence& evidence : analysis.subface_labels) {
        const unsigned path_limit = evidence.path.depth > 0 && evidence.path.depth <= 3
            ? 1u << (2u * evidence.path.depth) : 0u;
        const auto key = std::make_pair(evidence.face_id, evidence.path);
        if (evidence.face_id >= analysis.face_labels.size() || path_limit == 0 ||
            unsigned(evidence.path.value) >= path_limit ||
            (evidence.label != Label::EyeSclera && evidence.label != Label::Iris &&
             evidence.label != Label::Eyebrow && evidence.label != Label::FaceSkin && evidence.label != Label::Hair) ||
            !std::isfinite(evidence.confidence) || evidence.confidence < minimum_confidence ||
            evidence.confidence > 1.f || evidence.samples == 0 || (has_previous && !(previous_key < key)))
            throw std::invalid_argument("Invalid semantic subface evidence.");
        uint32_t encoded; std::memcpy(&encoded, &evidence.confidence, sizeof(encoded));
        std::string confidence_hex;
        confidence_hex.reserve(8);
        for (int nibble = 0; nibble < 8; ++nibble) confidence_hex += hex[(encoded >> (nibble * 4)) & 15];
        subfaces.push_back({evidence.face_id, evidence.path.depth, evidence.path.value,
                            size_t(evidence.label), confidence_hex, evidence.samples});
        previous_key = key;
        has_previous = true;
    }
    nlohmann::json document = {{"schema", pipeline_version}, {"signature", analysis.signature},
        {"geometry_id", analysis.geometry_id}, {"content_id", analysis.content_id},
        {"body_identity", analysis.body_identity}, {"face_identity", analysis.face_identity},
        {"boundary_identity", analysis.boundary_identity},
        {"face_count", analysis.face_labels.size()}, {"labels", labels}, {"confidence_f32", confidence},
        {"subfaces", std::move(subfaces)},
        {"person_detected", analysis.person_detected}, {"rendered_views", analysis.rendered_views},
        {"face_views", analysis.face_views}, {"observed_faces", analysis.observed_faces}};
    document["boundary_runs"] = nlohmann::json::array();
    for (const auto& run : analysis.boundary_runs) {
        if (run.view_id < 0 || run.view_id >= 8 || size_t(run.part) > size_t(BoundaryPart::ClothesSkin) ||
            size_t(run.side) > size_t(BoundarySide::Right) ||
            !std::isfinite(run.model_score) || !std::isfinite(run.loading_ms) ||
            !std::isfinite(run.encoding_ms) || !std::isfinite(run.decoding_ms))
            throw std::invalid_argument("Invalid semantic boundary diagnostic.");
        document["boundary_runs"].push_back({run.person_id, size_t(run.part), size_t(run.side),
            run.region.left, run.region.top, run.region.right, run.region.bottom,
            run.status, run.reason, run.loading_ms, run.encoding_ms, run.decoding_ms,
            run.model_score, run.changed_pixels, run.view_id, run.crop_id, run.reason_code});
    }
    if (!analysis.baseline_subface_labels.empty() || !analysis.baseline_face_labels.empty()) {
        Analysis baseline = analysis;
        baseline.subface_labels = analysis.baseline_subface_labels;
        baseline.baseline_subface_labels.clear();
        baseline.baseline_face_labels.clear(); baseline.baseline_face_confidence.clear();
        if (!analysis.baseline_face_labels.empty()) {
            if (analysis.baseline_face_labels.size()!=analysis.face_labels.size() ||
                analysis.baseline_face_confidence.size()!=analysis.face_labels.size())
                throw std::invalid_argument("Invalid baseline semantic face evidence.");
            baseline.face_labels=analysis.baseline_face_labels;
            baseline.face_confidence=analysis.baseline_face_confidence;
        }
        const auto encoded=encode_analysis(baseline);
        document["baseline_subfaces"] = encoded.at("subfaces");
        if (!analysis.baseline_face_labels.empty()) {
            document["baseline_labels"] = encoded.at("labels");
            document["baseline_confidence_f32"] = encoded.at("confidence_f32");
        }
    }
    return document;
}

bool decode_analysis(const nlohmann::json& doc, const MeshSnapshot& source, const std::string& body,
                     const std::string& face, Analysis& output, std::string& error)
{
    return decode_analysis(doc, source, body, face, "none", output, error);
}

bool decode_analysis(const nlohmann::json& doc, const MeshSnapshot& source, const std::string& body,
                     const std::string& face, const std::string& boundary,
                     Analysis& output, std::string& error)
{
    error.clear();
    try {
        const size_t count = source.mesh.indices.size();
        const auto signature = analysis_cache_key(source, body, face, boundary);
        if (signature.empty() || count == 0 || count > maximum_faces || !doc.is_object() ||
            doc.at("schema") != pipeline_version || doc.at("signature") != signature ||
            doc.at("geometry_id") != source.geometry_id || doc.at("content_id") != source.content_id ||
            doc.at("body_identity") != body || doc.at("face_identity") != face ||
            doc.at("boundary_identity") != boundary ||
            !doc.at("face_count").is_number_unsigned() || doc.at("face_count").get<size_t>() != count)
            throw std::invalid_argument("The cached semantic analysis belongs to another model or recognizer version.");
        const auto& labels = doc.at("labels").get_ref<const std::string&>();
        const auto& confidence = doc.at("confidence_f32").get_ref<const std::string&>();
        if (labels.size() != count || confidence.size() != count * 8)
            throw std::invalid_argument("The cached semantic mask size is invalid.");
        Analysis restored;
        restored.geometry_id = source.geometry_id; restored.content_id = source.content_id;
        restored.body_identity = body; restored.face_identity = face; restored.boundary_identity = boundary;
        restored.signature = signature;
        restored.person_detected = doc.at("person_detected").get<bool>();
        restored.rendered_views = doc.at("rendered_views").get<size_t>();
        restored.face_views = doc.at("face_views").get<size_t>();
        restored.observed_faces = doc.at("observed_faces").get<size_t>();
        if (restored.rendered_views != 8 || restored.face_views > 8 || restored.observed_faces > count ||
            (restored.person_detected && restored.face_views == 0))
            throw std::invalid_argument("The cached semantic analysis is incomplete.");
        if (doc.contains("boundary_runs")) {
            const auto& runs = doc.at("boundary_runs");
            if (!runs.is_array() || runs.size() > 4096)
                throw std::invalid_argument("The cached boundary diagnostics are invalid.");
            for (const auto& entry : runs) {
                if (!entry.is_array() || entry.size() != 17 ||
                    entry[1].get<size_t>() > size_t(BoundaryPart::ClothesSkin) ||
                    entry[2].get<size_t>() > size_t(BoundarySide::Right))
                    throw std::invalid_argument("The cached boundary diagnostic is malformed.");
                BoundaryRunDiagnostic run;
                run.person_id = entry[0].get<uint32_t>();
                run.part = BoundaryPart(entry[1].get<size_t>());
                run.side = BoundarySide(entry[2].get<size_t>());
                run.region = {entry[3].get<int>(), entry[4].get<int>(), entry[5].get<int>(), entry[6].get<int>()};
                run.status = entry[7].get<std::string>(); run.reason = entry[8].get<std::string>();
                run.loading_ms = entry[9].get<double>(); run.encoding_ms = entry[10].get<double>();
                run.decoding_ms = entry[11].get<double>(); run.model_score = entry[12].get<float>();
                run.changed_pixels = entry[13].get<size_t>(); run.view_id = entry[14].get<int>();
                run.crop_id = entry[15].get<std::string>(); run.reason_code = entry[16].get<std::string>();
                if (run.view_id < 0 || run.view_id >= 8 || !std::isfinite(run.model_score) ||
                    !std::isfinite(run.loading_ms) || !std::isfinite(run.encoding_ms) ||
                    !std::isfinite(run.decoding_ms))
                    throw std::invalid_argument("The cached boundary diagnostic is out of range.");
                restored.boundary_runs.push_back(std::move(run));
            }
        }
        restored.face_labels.reserve(count); restored.face_confidence.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            const int label = unhex(labels[i]);
            if (label < 0 || label >= int(label_count))
                throw std::invalid_argument("The cached semantic analysis contains invalid mask data.");
            uint32_t bits = 0;
            for (int nibble = 0; nibble < 8; ++nibble) {
                const int digit = unhex(confidence[i * 8 + nibble]);
                if (digit < 0) throw std::invalid_argument("The cached semantic confidence is invalid.");
                bits |= uint32_t(digit) << (nibble * 4);
            }
            float value; std::memcpy(&value, &bits, sizeof(value));
            if (!std::isfinite(value) || value < 0 || value > 1) throw std::invalid_argument("The cached semantic confidence is out of range.");
            restored.face_labels.push_back(Label(label));
            restored.face_confidence.push_back(value);
            if (paintable(Label(label)) && restored.face_confidence.back() >= minimum_confidence) ++restored.reliable_faces;
        }
        const auto& subfaces = doc.at("subfaces");
        if (!subfaces.is_array() || subfaces.size() > maximum_subface_evidence)
            throw std::invalid_argument("The cached semantic subface evidence is invalid.");
        std::pair<size_t, SubfacePath> previous_key {0, {0, 0}};
        bool has_previous = false;
        restored.subface_labels.reserve(subfaces.size());
        for (const auto& entry : subfaces) {
            if (!entry.is_array() || entry.size() != 6 || !entry[0].is_number_unsigned() ||
                !entry[1].is_number_unsigned() || !entry[2].is_number_unsigned() ||
                !entry[3].is_number_unsigned() || !entry[4].is_string() || !entry[5].is_number_unsigned())
                throw std::invalid_argument("The cached semantic subface entry is malformed.");
            const size_t face_id = entry[0].get<size_t>();
            const size_t depth = entry[1].get<size_t>();
            const size_t path_value = entry[2].get<size_t>();
            const size_t label_value = entry[3].get<size_t>();
            const uint32_t samples = entry[5].get<uint32_t>();
            const auto& confidence_hex = entry[4].get_ref<const std::string&>();
            if (face_id >= count || depth == 0 || depth > 3 || path_value >= (size_t(1) << (2 * depth)) ||
                (label_value != size_t(Label::EyeSclera) && label_value != size_t(Label::Iris) &&
                 label_value != size_t(Label::Eyebrow) && label_value != size_t(Label::FaceSkin) &&
                 label_value != size_t(Label::Hair)) ||
                samples == 0 || confidence_hex.size() != 8)
                throw std::invalid_argument("The cached semantic subface entry is out of range.");
            uint32_t bits = 0;
            for (int nibble = 0; nibble < 8; ++nibble) {
                const int digit = unhex(confidence_hex[nibble]);
                if (digit < 0) throw std::invalid_argument("The cached semantic subface confidence is invalid.");
                bits |= uint32_t(digit) << (nibble * 4);
            }
            float value; std::memcpy(&value, &bits, sizeof(value));
            if (!std::isfinite(value) || value < minimum_confidence || value > 1.f)
                throw std::invalid_argument("The cached semantic subface confidence is out of range.");
            const SubfacePath path {uint8_t(depth), uint8_t(path_value)};
            const auto key = std::make_pair(face_id, path);
            if (has_previous && !(previous_key < key))
                throw std::invalid_argument("The cached semantic subface paths are not canonical.");
            restored.subface_labels.push_back({face_id, path, Label(label_value), value, samples});
            previous_key = key;
            has_previous = true;
        }
        if (doc.contains("baseline_subfaces") || doc.contains("baseline_labels") || doc.contains("baseline_confidence_f32")) {
            auto baseline_document = doc;
            baseline_document["subfaces"] = doc.value("baseline_subfaces", nlohmann::json::array());
            if (doc.contains("baseline_labels") || doc.contains("baseline_confidence_f32")) {
                baseline_document["labels"] = doc.at("baseline_labels");
                baseline_document["confidence_f32"] = doc.at("baseline_confidence_f32");
            }
            baseline_document.erase("baseline_subfaces");
            baseline_document.erase("baseline_labels");
            baseline_document.erase("baseline_confidence_f32");
            Analysis baseline;
            std::string baseline_error;
            if (!decode_analysis(baseline_document, source, body, face, boundary, baseline, baseline_error))
                throw std::invalid_argument("Invalid baseline semantic evidence: " + baseline_error);
            restored.baseline_subface_labels=std::move(baseline.subface_labels);
            if (doc.contains("baseline_labels")) {
                restored.baseline_face_labels=std::move(baseline.face_labels);
                restored.baseline_face_confidence=std::move(baseline.face_confidence);
            }
        }
        output = std::move(restored); return true;
    } catch (const std::exception& failure) { error = failure.what(); return false; }
}
} // namespace Slic3r::AI::SemanticColoring
