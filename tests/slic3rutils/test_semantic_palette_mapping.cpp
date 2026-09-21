#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "slic3r/AI/ModelGeneration/SemanticColoring/SemanticPaletteMapping.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <set>
#include <future>

using namespace Slic3r;
using namespace Slic3r::AI::SemanticColoring;
using Catch::Matchers::WithinAbs;

namespace {
MeshSnapshot strip(const std::vector<Color>& colors)
{
    MeshSnapshot source;
    for (size_t vertex = 0; vertex < colors.size() + 2; ++vertex)
        source.mesh.vertices.emplace_back(float(vertex / 2), 0.f, float(vertex % 2));
    for (size_t face = 0; face < colors.size(); ++face) {
        const int first = int(face);
        source.mesh.indices.emplace_back(first, first + (face % 2 ? 2 : 1), first + (face % 2 ? 1 : 2));
        source.face_colors.push_back({colors[face][0], colors[face][1], colors[face][2], 1.f});
    }
    source.geometry_id = "palette-regression-strip";
    source.content_id = content_fingerprint(source);
    return source;
}
MeshSnapshot layered_skin_fixture(bool clothes_barrier = false, bool opposite_shell = false,
                                  bool conflicting_skin_region = false)
{
    MeshSnapshot source;
    const auto triangle = [&](Vec3f a, Vec3f b, Vec3f c, const Color& color) {
        const int first = int(source.mesh.vertices.size());
        source.mesh.vertices.insert(source.mesh.vertices.end(), {a, b, c});
        source.mesh.indices.emplace_back(first, first + 1, first + 2);
        source.face_colors.push_back({color[0], color[1], color[2], 1.f});
    };
    const Color skin {.76f, .55f, .41f}, shadow {.62f, .38f, .28f};
    source.mesh.vertices = {{0.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {1.f, 1.f, 0.f}};
    source.mesh.indices = {{0, 1, 2}, {1, 3, 2}};
    source.face_colors = {{skin[0], skin[1], skin[2], 1.f}, {skin[0], skin[1], skin[2], 1.f}};
    if (opposite_shell)
        triangle({0.f, 1.f, .02f}, {1.f, 0.f, .02f}, {0.f, 0.f, .02f}, shadow);
    else
        triangle({0.f, 0.f, .02f}, {1.f, 0.f, .02f}, {0.f, 1.f, .02f}, shadow);
    if (clothes_barrier)
        triangle({0.f, 0.f, .03f}, {1.f, 0.f, .03f}, {0.f, 1.f, .03f}, {.75f, .1f, .08f});
    if (conflicting_skin_region) {
        triangle({0.f, 0.f, .04f}, {1.f, 0.f, .04f}, {0.f, 1.f, .04f}, {.68f, .47f, .35f});
        triangle({1.f, 0.f, .04f}, {1.f, 1.f, .04f}, {0.f, 1.f, .04f}, {.68f, .47f, .35f});
    }
    // Keep the seam local while exercising the model-relative search radius.
    triangle({100.f, 100.f, 0.f}, {101.f, 100.f, 0.f}, {100.f, 101.f, 0.f}, {.5f, .5f, .5f});
    source.geometry_id = std::string("palette-spatial-skin-fixture:") +
        (clothes_barrier ? "clothes:" : "plain:") +
        (opposite_shell ? "opposite:" : "aligned:") +
        (conflicting_skin_region ? "conflict" : "single");
    source.content_id = content_fingerprint(source);
    return source;
}
MeshSnapshot eyebrow_extent_fixture()
{
    MeshSnapshot source;
    const auto add = [&](Vec3f a, Vec3f b, Vec3f c, const Color& color) {
        const int first = int(source.mesh.vertices.size());
        source.mesh.vertices.insert(source.mesh.vertices.end(), {a, b, c});
        source.mesh.indices.emplace_back(first, first + 1, first + 2);
        source.face_colors.push_back({color[0], color[1], color[2], 1.f});
    };
    const Color dark {.12f, .11f, .10f};
    add({0.f,0.f,0.f}, {1.f,0.f,0.f}, {0.f,1.f,0.f}, {.92f,.92f,.91f}); // eye
    add({0.f,1.f,0.f}, {1.f,1.f,0.f}, {0.f,2.f,0.f}, {.76f,.55f,.41f}); // skin
    // Reuse the skin vertex index so the valid brow has a real topological
    // boundary. Equal coordinates with separate indices are not adjacent.
    source.mesh.vertices.emplace_back(1.f,1.4f,0.f);
    source.mesh.vertices.emplace_back(0.f,1.4f,0.f);
    source.mesh.indices.emplace_back(3,6,7);
    source.face_colors.push_back({dark[0],dark[1],dark[2],1.f});
    add({10.f,0.f,0.f}, {11.f,0.f,0.f}, {10.f,1.f,0.f}, dark); // hair
    source.mesh.vertices.emplace_back(11.f,.4f,0.f);
    source.mesh.vertices.emplace_back(10.f,.4f,0.f);
    source.mesh.indices.emplace_back(8,11,12);
    source.face_colors.push_back({dark[0],dark[1],dark[2],1.f}); // false remote brow
    add({100.f,0.f,0.f}, {101.f,0.f,0.f}, {100.f,1.f,0.f}, {.5f,.5f,.5f});
    source.geometry_id = "palette-eyebrow-extent-fixture";
    source.content_id = content_fingerprint(source);
    return source;
}
Analysis evidence(const MeshSnapshot& source, const std::vector<Label>& labels)
{
    Analysis analysis;
    analysis.geometry_id = source.geometry_id; analysis.content_id = source.content_id;
    analysis.body_identity = "palette-body"; analysis.face_identity = "palette-face";
    analysis.signature = analysis_cache_key(source, analysis.body_identity, analysis.face_identity);
    analysis.face_labels = labels; analysis.face_confidence.assign(labels.size(), .95f);
    analysis.person_detected = true; analysis.rendered_views = 8; analysis.face_views = 1;
    analysis.observed_faces = analysis.reliable_faces = labels.size();
    return analysis;
}
std::vector<Color> portrait()
{
    return {{247.f/255,226.f/255,218.f/255}, {40.f/255,38.f/255,41.f/255},
        {246.f/255,247.f/255,249.f/255}, {234.f/255,154.f/255,146.f/255},
        {102.f/255,140.f/255,182.f/255}, {149.f/255,139.f/255,134.f/255}};
}
std::vector<PaletteSlot> slots(const std::vector<Color>& colors)
{
    std::vector<PaletteSlot> result;
    for (size_t i = 0; i < colors.size(); ++i) result.push_back({"filament-" + std::to_string(i), colors[i], true});
    return result;
}
} // namespace

TEST_CASE("Reducing and restoring one through six filaments preserves material evidence", "[SemanticPaletteMapping]")
{
    const auto card = portrait();
    const auto source = strip({{.76f,.55f,.41f}, {.68f,.26f,.24f}, {.12f,.11f,.12f},
                               {.92f,.92f,.92f}, {.25f,.39f,.38f}, {.4f,.39f,.38f}});
    const auto analysis = evidence(source, {Label::FaceSkin, Label::Lips, Label::Hair,
        Label::Clothes, Label::Clothes, Label::Clothes});
    const auto frozen = encode_analysis(analysis);
    const auto all_slots = slots(card);
    SlotMappingResult baseline;
    std::string error;
    REQUIRE(map_palette_slots(source, analysis, all_slots, card, {}, baseline, error));
    REQUIRE_FALSE(baseline.faces.empty());
    for (size_t active_count : {6u,5u,4u,3u,2u,1u,2u,3u,4u,5u,6u,2u,6u,1u,6u}) {
        auto active = all_slots;
        for (size_t index = 0; index < active.size(); ++index) active[index].enabled = index < active_count;
        SlotMappingResult mapped;
        REQUIRE(map_palette_slots(source, analysis, active, card, {}, mapped, error));
        REQUIRE(mapped.material_centers.size() == baseline.material_centers.size());
        for (size_t index = 0; index < mapped.material_centers.size(); ++index) {
            REQUIRE(mapped.material_centers[index].id == baseline.material_centers[index].id);
            REQUIRE(mapped.material_centers[index].original_oklab == baseline.material_centers[index].original_oklab);
        }
        for (const auto& assignment : mapped.face_slots)
            REQUIRE(std::any_of(active.begin(), active.end(), [&](const PaletteSlot& slot) {
                return slot.enabled && slot.id == assignment.slot_id;
            }));
        if (active_count == 6) REQUIRE(mapped.faces == baseline.faces);
        REQUIRE(encode_analysis(analysis) == frozen);
    }
}

TEST_CASE("Missing one portrait role preserves all available explicit roles", "[SemanticPaletteMapping]")
{
    const auto card = portrait();
    const auto source = strip({{.76f,.55f,.41f}, {.68f,.26f,.24f}, {.12f,.11f,.12f}});
    const auto analysis = evidence(source, {Label::FaceSkin, Label::Lips, Label::Hair});
    for (size_t removed = 0; removed < card.size(); ++removed) {
        auto active = slots(card); active[removed].enabled = false;
        SlotMappingResult mapped; std::string error;
        REQUIRE(map_palette_slots(source, analysis, active, card, {}, mapped, error));
        REQUIRE(mapped.faces.size() == 3);
        if (removed != 0) REQUIRE(mapped.faces[0].second == card[0]);
        if (removed != 3) REQUIRE(mapped.faces[1].second == card[3]);
        if (removed != 1) REQUIRE(mapped.faces[2].second == card[1]);
    }
}

TEST_CASE("Missing face skin labels never turns the unknown face into lips", "[SemanticPaletteMapping]")
{
    const auto source = strip({{.76f,.55f,.41f}, {.68f,.26f,.24f}});
    const auto analysis = evidence(source, {Label::Unknown, Label::Lips});
    SlotMappingResult mapped; std::string error;
    REQUIRE(map_palette_slots(source, analysis, slots(portrait()), portrait(), {}, mapped, error));
    REQUIRE(mapped.faces.size() == 1);
    REQUIRE(mapped.faces.front().first == 1);
    REQUIRE(mapped.faces.front().second == portrait()[3]);
}

TEST_CASE("Disconnected warm skin evidence joins one stable material region",
          "[SemanticPaletteMapping][SpatialSkin]")
{
    const auto source = layered_skin_fixture();
    const auto analysis = evidence(source, {Label::FaceSkin, Label::FaceSkin, Label::Unknown, Label::Background});
    SlotMappingResult mapped; std::string error;
    REQUIRE(map_palette_slots(source, analysis, slots(portrait()), portrait(), {}, mapped, error));
    const auto donor = std::find_if(mapped.face_slots.begin(), mapped.face_slots.end(),
                                    [](const auto& face) { return face.face_id == 0; });
    const auto seam = std::find_if(mapped.face_slots.begin(), mapped.face_slots.end(),
                                   [](const auto& face) { return face.face_id == 2; });
    REQUIRE(donor != mapped.face_slots.end());
    REQUIRE(seam != mapped.face_slots.end());
    CHECK(seam->material_center_id == donor->material_center_id);
    CHECK(seam->region_id == donor->region_id);
    CHECK(seam->slot_id == donor->slot_id);
    const auto recommendations = recommend_region_slots(mapped, slots(portrait()));
    const auto recommendation = std::find_if(recommendations.begin(), recommendations.end(),
        [&](const auto& item) { return item.region_id == seam->region_id; });
    REQUIRE(recommendation != recommendations.end());
    CHECK(recommendation->recommended_slot_id == seam->slot_id);
}

TEST_CASE("Spatial skin discovery rejects protected and ambiguous seams",
          "[SemanticPaletteMapping][SpatialSkin]")
{
    const auto verify_rejected = [](const MeshSnapshot& source, std::vector<Label> labels) {
        const auto analysis = evidence(source, labels);
        SlotMappingResult mapped; std::string error;
        REQUIRE(map_palette_slots(source, analysis, slots(portrait()), portrait(), {}, mapped, error));
        CHECK(std::none_of(mapped.face_slots.begin(), mapped.face_slots.end(),
                           [](const auto& face) { return face.face_id == 2; }));
    };
    SECTION("nearby clothes protect their material boundary")
    {
        verify_rejected(layered_skin_fixture(true),
                        {Label::FaceSkin, Label::FaceSkin, Label::Unknown, Label::Clothes, Label::Background});
    }
    SECTION("an opposite thin shell cannot borrow front-facing skin")
    {
        verify_rejected(layered_skin_fixture(false, true),
                        {Label::FaceSkin, Label::FaceSkin, Label::Unknown, Label::Background});
    }
    SECTION("two nearby skin regions cannot lend an unknown face across people")
    {
        verify_rejected(layered_skin_fixture(false, false, true),
                        {Label::FaceSkin, Label::FaceSkin, Label::Unknown,
                         Label::FaceSkin, Label::FaceSkin, Label::Background});
    }
}

TEST_CASE("Eyebrow materials remain near an eye and cannot spread into remote hair",
          "[SemanticPaletteMapping][EyebrowExtent]")
{
    const auto source = eyebrow_extent_fixture();
    auto analysis = evidence(source, {Label::EyeSclera, Label::FaceSkin, Label::Eyebrow,
                                      Label::Hair, Label::Eyebrow, Label::Background});
    analysis.subface_labels = {{4, {2, 0}, Label::Eyebrow, .96f, 4}};
    SlotMappingResult mapped; std::string error;
    SubfaceBudget budget; budget.maximum_added_ratio = 1.f;
    const bool mapped_ok = map_palette_slots(source, analysis, slots(portrait()), portrait(), budget, mapped, error);
    INFO(error);
    REQUIRE(mapped_ok);
    const auto slot_for_face = [&](size_t face_id) {
        const auto found = std::find_if(mapped.face_slots.begin(), mapped.face_slots.end(),
                                        [face_id](const auto& item) { return item.face_id == face_id; });
        return found == mapped.face_slots.end() ? std::string() : found->slot_id;
    };
    CHECK(slot_for_face(2) == "filament-5");
    CHECK(slot_for_face(4) == "filament-1");
    CHECK(std::none_of(mapped.subface_slots.begin(), mapped.subface_slots.end(),
                       [](const auto& leaf) { return leaf.face_id == 4; }));
}

TEST_CASE("Four supported cloth pigments survive the former three center limit", "[SemanticPaletteMapping]")
{
    const std::vector<Color> palette {{.72f,.13f,.12f}, {.12f,.55f,.19f}, {.13f,.22f,.75f}, {.81f,.69f,.10f}};
    std::vector<Color> colors;
    for (const auto& color : palette) for (int i = 0; i < 6; ++i) colors.push_back(color);
    const auto source = strip(colors);
    const auto analysis = evidence(source, std::vector<Label>(colors.size(), Label::Clothes));
    SlotMappingResult mapped; std::string error;
    REQUIRE(map_palette_slots(source, analysis, slots(palette), {}, {}, mapped, error));
    std::set<std::string> used;
    for (const auto& face : mapped.face_slots) used.insert(face.slot_id);
    REQUIRE(used.size() == 4);
    REQUIRE(mapped.material_centers.size() >= 4);
}

TEST_CASE("Neighboring lip and skin materials retain contrast only with a compatible alternative", "[SemanticPaletteMapping]")
{
    const auto source = strip({{.76f,.55f,.41f}, {.70f,.40f,.38f}});
    const auto analysis = evidence(source, {Label::FaceSkin, Label::Lips});
    SlotMappingResult mapped; std::string error;
    REQUIRE(map_palette_slots(source, analysis, slots({{.60f,.36f,.34f}, {.60f,.36f,.38f}}), {}, {}, mapped, error));
    REQUIRE(mapped.faces.size() == 2);
    REQUIRE(mapped.faces[0].second != mapped.faces[1].second);
    REQUIRE(map_palette_slots(source, analysis, slots({{.60f,.36f,.34f}, {0.f,0.f,1.f}}), {}, {}, mapped, error));
    REQUIRE(mapped.faces.size() == 2);
    REQUIRE(mapped.faces[0].second == mapped.faces[1].second);
}

TEST_CASE("Adding a nearby filament does not split one skin material into lighting fragments", "[SemanticPaletteMapping]")
{
    const auto source = strip({{.76f,.55f,.41f}, {.74f,.53f,.39f}, {.72f,.51f,.38f}, {.77f,.56f,.42f}});
    const auto analysis = evidence(source, std::vector<Label>(4, Label::FaceSkin));
    SlotMappingResult mapped; std::string error;
    REQUIRE(map_palette_slots(source, analysis, slots({{.76f,.55f,.41f}, {.73f,.52f,.40f}, {0.f,0.f,1.f}}),
                              {}, {}, mapped, error));
    REQUIRE(mapped.faces.size() == 4);
    REQUIRE(mapped.material_centers.size() == 1);
    for (const auto& face : mapped.faces) REQUIRE(face.second == mapped.faces.front().second);
}

TEST_CASE("More specific material children inherit safe sibling leaves", "[SemanticPaletteMapping]")
{
    const Color skin {.7f,.5f,.4f}, hair {.1f,.1f,.1f};
    const SubfaceColors candidates {{0,{2,0},skin,.9f}, {0,{3,0},hair,.95f}};
    SubfaceBudgetResult result; std::string error;
    REQUIRE(enforce_subface_budget(candidates, 100, {}, result, error));
    REQUIRE(result.accepted.size() == 4);
    REQUIRE(result.added_triangles == 9);
    for (const auto& leaf : result.accepted) {
        REQUIRE(leaf.path.depth == 3);
        if (leaf.path.value == 0) REQUIRE(leaf.color == hair);
        else REQUIRE(leaf.color == skin);
    }
}

TEST_CASE("Duplicate RGB slots retain identity across reorder and explicit edits", "[SemanticPaletteMapping]")
{
    const Color black {.12f,.12f,.12f}, white {.9f,.9f,.9f};
    const auto source = strip({black}); const auto analysis = evidence(source, {Label::Hair});
    std::vector<PaletteSlot> palette {{"b", black}, {"a", black}, {"c", white}};
    SlotMappingResult mapped, reordered; std::string error;
    REQUIRE(map_palette_slots(source, analysis, palette, {}, {}, mapped, error));
    std::reverse(palette.begin(), palette.end());
    REQUIRE(map_palette_slots(source, analysis, palette, {}, {}, reordered, error));
    REQUIRE(mapped.face_slots.front().slot_id == reordered.face_slots.front().slot_id);
    // A user can explicitly choose the other physically distinct black slot.
    mapped.face_slots.front().slot_id = "b";
    mapped.face_slots.front().intended_slot_id = "b";
    for (auto& slot : palette) if (slot.id == "b") slot.color = {.3f,.25f,.2f};
    REQUIRE(remap_palette_slots(mapped, palette, reordered, error));
    REQUIRE(reordered.face_slots.front().slot_id == "b");
    REQUIRE(reordered.faces.front().second == Color{.3f,.25f,.2f});
}

TEST_CASE("Disabled manually bound slots use valid substitutes and restore their intent", "[SemanticPaletteMapping]")
{
    const Color skin {.76f,.55f,.41f};
    const auto source = strip({skin}); const auto analysis = evidence(source, {Label::FaceSkin});
    auto palette = slots({skin, {.1f,.1f,.1f}});
    SlotMappingResult initial, disabled, restored; std::string error;
    REQUIRE(map_palette_slots(source, analysis, palette, {}, {}, initial, error));
    const auto intended = initial.face_slots.front().slot_id;
    for (auto& slot : palette) if (slot.id == intended) slot.enabled = false;
    REQUIRE(remap_palette_slots(initial, palette, disabled, error));
    REQUIRE(disabled.face_slots.front().slot_id != intended);
    REQUIRE(disabled.face_slots.front().intended_slot_id == intended);
    REQUIRE(disabled.substituted_assignments == 1);
    SlotMappingResult recovered;
    REQUIRE(decode_slot_mapping(encode_slot_mapping(disabled), 1, palette, recovered, error));
    REQUIRE(recovered.face_slots.front().intended_slot_id == intended);
    for (auto& slot : palette) slot.enabled = true;
    REQUIRE(remap_palette_slots(recovered, palette, restored, error));
    REQUIRE(restored.face_slots.front().slot_id == intended);
    REQUIRE(restored.faces == initial.faces);
}

TEST_CASE("Invalid active slot counts and duplicate identities fail transactionally", "[SemanticPaletteMapping]")
{
    const auto source = strip({{.2f,.2f,.2f}}); const auto analysis = evidence(source, {Label::Hair});
    SlotMappingResult mapped; std::string error;
    REQUIRE_FALSE(map_palette_slots(source, analysis, {}, {}, {}, mapped, error));
    REQUIRE_FALSE(error.empty());
    auto palette = slots(std::vector<Color>(7, {.2f,.2f,.2f}));
    REQUIRE_FALSE(map_palette_slots(source, analysis, palette, {}, {}, mapped, error));
    palette.resize(2); palette[1].id = palette[0].id;
    REQUIRE_FALSE(map_palette_slots(source, analysis, palette, {}, {}, mapped, error));
    REQUIRE(mapped.faces.empty());
}

TEST_CASE("A region override changes only its discovered material and survives slot restoration",
          "[SemanticPaletteMapping][RegionOverride]")
{
    const auto source = strip({{.76f,.55f,.41f}, {.12f,.11f,.12f}, {.70f,.40f,.38f}});
    const auto analysis = evidence(source, {Label::FaceSkin, Label::Hair, Label::Lips});
    auto palette = slots(portrait());
    SlotMappingResult automatic, edited, restored;
    std::string error;
    REQUIRE(map_palette_slots(source, analysis, palette, portrait(), {}, automatic, error));
    REQUIRE(automatic.face_slots.size() == 3);
    const size_t region = automatic.face_slots[0].material_center_id;
    const std::vector<RegionColorOverride> overrides {{analysis.signature, region, "filament-5", true}};
    REQUIRE(map_palette_slots(source, analysis, palette, portrait(), {}, overrides, edited, error));
    CHECK(edited.faces[0].second == portrait()[5]);
    CHECK(edited.faces[1] == automatic.faces[1]);
    CHECK(edited.faces[2] == automatic.faces[2]);
    CHECK(edited.region_overrides == overrides);
    REQUIRE(decode_slot_mapping(encode_slot_mapping(edited), 3, palette, restored, error));
    CHECK(restored.region_overrides == overrides);
    CHECK(restored.faces == edited.faces);
    palette[5].enabled = false;
    REQUIRE(remap_palette_slots(edited, palette, restored, error));
    CHECK(restored.face_slots[0].intended_slot_id == "filament-5");
    palette[5].enabled = true;
    REQUIRE(remap_palette_slots(restored, palette, edited, error));
    CHECK(edited.face_slots[0].slot_id == "filament-5");
}

TEST_CASE("A stale region override is retained without blocking automatic output",
          "[SemanticPaletteMapping][RegionOverride]")
{
    const auto source = strip({{.76f,.55f,.41f}});
    const auto analysis = evidence(source, {Label::FaceSkin});
    SlotMappingResult mapped;
    std::string error;
    const std::vector<RegionColorOverride> overrides {{"stale-analysis", 0, "filament-1", true}};
    REQUIRE(map_palette_slots(source, analysis, slots(portrait()), portrait(), {}, overrides, mapped, error));
    CHECK(error.empty());
    REQUIRE_FALSE(mapped.faces.empty());
    REQUIRE(mapped.resolved_regions.size() >= 1);
    CHECK(mapped.resolved_regions.front().status == RegionResolutionStatus::StaleIntent);
}

TEST_CASE("A child region override never recolors its parent material",
          "[SemanticPaletteMapping][RegionOverride][SubfaceRegion]")
{
    const auto source = strip(std::vector<Color>(64, {.42f,.28f,.20f}));
    auto analysis = evidence(source, std::vector<Label>(64, Label::FaceSkin));
    analysis.subface_labels.push_back({0,{2,0},Label::Eyebrow,.96f,4});
    const auto palette = slots(portrait());
    SlotMappingResult automatic, edited; std::string error;
    REQUIRE(map_palette_slots(source,analysis,palette,portrait(),{},automatic,error));
    REQUIRE_FALSE(automatic.subface_slots.empty());
    const auto child = automatic.subface_slots.front();
    REQUIRE_FALSE(child.region_id.empty());
    RegionColorOverride intent;
    intent.analysis_signature=analysis.signature; intent.region_id=child.region_id;
    intent.slot_id="filament-4"; intent.locked=true; intent.semantic_role=Label::Eyebrow;
    REQUIRE(map_palette_slots(source,analysis,palette,portrait(),{},std::vector<RegionColorOverride>{intent},edited,error));
    REQUIRE(edited.faces == automatic.faces);
    REQUIRE_FALSE(edited.subface_slots.empty());
    CHECK(edited.subface_slots.front().slot_id == "filament-4");
    CHECK(edited.subface_slots.front().region_id == child.region_id);
}

TEST_CASE("Multiple child region intents survive v5 mapping persistence",
          "[SemanticPaletteMapping][RegionOverride][Persistence]")
{
    const auto source = strip(std::vector<Color>(64, {.42f,.28f,.20f}));
    auto analysis = evidence(source, std::vector<Label>(64, Label::FaceSkin));
    analysis.subface_labels.push_back({0,{2,0},Label::Eyebrow,.96f,4});
    const auto palette = slots(portrait());
    SlotMappingResult mapped; std::string error;
    REQUIRE(map_palette_slots(source,analysis,palette,portrait(),{},mapped,error));
    REQUIRE(mapped.material_centers.size() == 1);
    REQUIRE(mapped.subface_slots.size() == 1);
    auto second = mapped.subface_slots.front();
    second.face_id = 1;
    second.region_id = "region:second-child";
    mapped.subface_slots.push_back(second);
    auto second_color = mapped.subfaces.front();
    second_color.face_id = 1;
    mapped.subfaces.push_back(second_color);

    std::vector<RegionColorOverride> intents;
    for (size_t index = 0; index < 2; ++index) {
        RegionColorOverride intent;
        intent.analysis_signature = analysis.signature;
        intent.region_id = mapped.subface_slots[index].region_id;
        intent.slot_id = index == 0 ? "filament-5" : "filament-2";
        intent.locked = true;
        intents.push_back(intent);
    }
    mapped.region_overrides = intents;
    SlotMappingResult restored;
    const auto document = encode_slot_mapping(mapped);
    REQUIRE(decode_slot_mapping(document,source.mesh.indices.size(),palette,restored,error));
    REQUIRE(restored.region_overrides == intents);
}

TEST_CASE("Duplicate RGB region intent preserves the selected physical slot",
          "[SemanticPaletteMapping][RegionOverride][SlotIdentity]")
{
    const Color black {.12f,.12f,.12f};
    const auto source=strip({black}); const auto analysis=evidence(source,{Label::Hair});
    const std::vector<PaletteSlot> palette {{"left",black},{"right",black},{"light",{.9f,.9f,.9f}}};
    SlotMappingResult automatic,edited; std::string error;
    REQUIRE(map_palette_slots(source,analysis,palette,{}, {},automatic,error));
    REQUIRE_FALSE(automatic.material_centers.front().region_id.empty());
    RegionColorOverride intent;
    intent.analysis_signature=analysis.signature; intent.region_id=automatic.material_centers.front().region_id;
    intent.slot_id="right"; intent.locked=true;
    REQUIRE(map_palette_slots(source,analysis,palette,{}, {},std::vector<RegionColorOverride>{intent},edited,error));
    REQUIRE(edited.face_slots.front().slot_id == "right");
    REQUIRE(edited.face_slots.front().intended_slot_id == "right");
}

TEST_CASE("Reliable hair evidence preserves final material and survives analysis caching", "[SemanticPaletteMapping]")
{
    const Color hair {.12f,.11f,.12f};
    const auto source = strip(std::vector<Color>(64, hair));
    std::vector<Label> labels(64, Label::Background); labels[0] = Label::Hair; labels[1] = Label::FaceSkin;
    auto analysis = evidence(source, labels);
    analysis.subface_labels.push_back({1, {3, 1}, Label::Hair, .95f, 5});
    const auto document = encode_analysis(analysis);
    Analysis restored; std::string error;
    REQUIRE(decode_analysis(document, source, analysis.body_identity, analysis.face_identity, restored, error));
    REQUIRE(restored.subface_labels == analysis.subface_labels);
    clear_material_discovery_cache();
    SlotMappingResult mapped;
    REQUIRE(map_palette_slots(source, restored, slots(portrait()), portrait(), {}, mapped, error));
    // This fixture is intrinsically dark and neutral even on the FaceSkin
    // root. Whole-face neutral protection already selects the hair filament;
    // requiring a duplicate child here would waste geometry without improving
    // the final printable material.
    REQUIRE(std::find(mapped.faces.begin(), mapped.faces.end(),
                      std::make_pair(size_t(1), portrait()[1])) != mapped.faces.end());
    REQUIRE(mapped.subfaces.empty());
    REQUIRE(mapped.added_triangles == 0);
    REQUIRE(mapped.rejected_candidates == 0);
    REQUIRE(material_discovery_cache_stats().builds == 1);

    // The same restored evidence must still produce a genuine depth-three
    // boundary when the caller supplies a different existing root material.
    // This distinguishes correct redundant-leaf removal from dropping Hair
    // evidence altogether.
    SubfaceBudgetResult contrasted;
    REQUIRE(map_subface_palette(source, restored, {{1, portrait()[0]}},
                                portrait(), portrait(), {}, contrasted, error));
    REQUIRE(contrasted.accepted.size() == 1);
    REQUIRE(contrasted.accepted.front().face_id == 1);
    REQUIRE(contrasted.accepted.front().path == SubfacePath{3, 1});
    REQUIRE(contrasted.accepted.front().color == portrait()[1]);
    REQUIRE(contrasted.added_triangles == 9);
    REQUIRE(contrasted.rejected_candidates == 0);

    // One-color display can elide the boundary while the original semantic
    // leaf and original material centers remain available for restoration.
    SlotMappingResult one_color, again;
    REQUIRE(map_palette_slots(source, restored, slots({portrait()[0]}), portrait(), {}, one_color, error));
    REQUIRE(one_color.subfaces.empty());
    REQUIRE(restored.subface_labels == analysis.subface_labels);
    REQUIRE(map_palette_slots(source, restored, slots(portrait()), portrait(), {}, again, error));
    REQUIRE(again.faces == mapped.faces);
    REQUIRE(again.subfaces == mapped.subfaces);
    REQUIRE(material_discovery_cache_stats().builds == 1);
    REQUIRE(material_discovery_cache_stats().hits >= 5);
}

TEST_CASE("A depth three budget failure retains the previously safe depth two material", "[SemanticPaletteMapping]")
{
    const auto source = strip(std::vector<Color>(64, {.12f,.11f,.12f}));
    auto analysis = evidence(source, std::vector<Label>(64, Label::Hair));
    analysis.baseline_subface_labels.push_back({0,{2,0},Label::Eyebrow,.95f,4});
    analysis.subface_labels.push_back({0,{3,0},Label::Eyebrow,.98f,4});
    Analysis restored; std::string error;
    REQUIRE(decode_analysis(encode_analysis(analysis), source, analysis.body_identity, analysis.face_identity, restored, error));
    REQUIRE(restored.baseline_subface_labels == analysis.baseline_subface_labels);
    SubfaceBudget budget; budget.maximum_added_triangles = 6;
    SlotMappingResult mapped;
    REQUIRE(map_palette_slots(source, restored, slots(portrait()), portrait(), budget, mapped, error));
    REQUIRE(mapped.subfaces.size() == 1);
    REQUIRE(mapped.subfaces.front().path.depth == 2);
    REQUIRE(mapped.subfaces.front().color == portrait()[5]);
    REQUIRE(mapped.added_triangles == 6);
    REQUIRE(mapped.rejected_candidates > 0);
}

TEST_CASE("Rejected boundary geometry restores the safe root and child together", "[SemanticPaletteMapping]")
{
    const auto source = strip(std::vector<Color>(64, {.42f,.28f,.20f}));
    auto analysis = evidence(source, std::vector<Label>(64, Label::Hair));
    analysis.baseline_face_labels.assign(64,Label::FaceSkin);
    analysis.baseline_face_confidence.assign(64,.95f);
    analysis.baseline_subface_labels.push_back({0,{2,0},Label::Eyebrow,.95f,4});
    analysis.subface_labels.push_back({0,{3,0},Label::Eyebrow,.98f,4});
    SubfaceBudget budget; budget.maximum_added_triangles = 6;
    SlotMappingResult mapped; std::string error;
    REQUIRE(map_palette_slots(source,analysis,slots(portrait()),portrait(),budget,mapped,error));
    REQUIRE(mapped.boundary_budget_fallback);
    REQUIRE_FALSE(mapped.faces.empty());
    REQUIRE(mapped.faces.front().second == portrait()[0]);
    REQUIRE(mapped.subfaces.size() == 1);
    REQUIRE(mapped.subfaces.front().path.depth == 2);
    REQUIRE(mapped.subfaces.front().color == portrait()[5]);
}

TEST_CASE("Boundary hair candidates cannot overwrite a stable eyebrow child",
          "[SemanticPaletteMapping][Regression][EyebrowColor]")
{
    const auto source = strip(std::vector<Color>(64, {.42f,.28f,.20f}));
    auto analysis = evidence(source, std::vector<Label>(64, Label::Hair));
    analysis.baseline_face_labels.assign(64, Label::FaceSkin);
    analysis.baseline_face_confidence.assign(64, .95f);
    analysis.baseline_subface_labels.push_back({0, {2, 0}, Label::Eyebrow, .95f, 4});
    analysis.subface_labels.push_back({0, {3, 0}, Label::Hair, .98f, 4});
    SlotMappingResult mapped;
    std::string error;
    REQUIRE(map_palette_slots(source, analysis, slots(portrait()), portrait(), {}, mapped, error));
    REQUIRE_FALSE(mapped.subfaces.empty());
    CHECK(mapped.subfaces.front().face_id == 0);
    CHECK(mapped.subfaces.front().path.depth == 2);
    CHECK(mapped.subfaces.front().color == portrait()[5]);
}

TEST_CASE("Boundary hair candidates cannot overwrite a stable sclera root",
          "[SemanticPaletteMapping][Regression][EyeColor]")
{
    const auto source = strip(std::vector<Color>(64, {.82f,.82f,.80f}));
    auto analysis = evidence(source, std::vector<Label>(64, Label::Hair));
    analysis.baseline_face_labels.assign(64, Label::FaceSkin);
    analysis.baseline_face_labels[0] = Label::EyeSclera;
    analysis.baseline_face_confidence.assign(64, .95f);
    analysis.subface_labels.push_back({0, {3, 0}, Label::Hair, .98f, 4});
    SlotMappingResult mapped;
    std::string error;
    REQUIRE(map_palette_slots(source, analysis, slots(portrait()), portrait(), {}, mapped, error));
    REQUIRE_FALSE(mapped.faces.empty());
    CHECK(mapped.faces.front().first == 0);
    CHECK(mapped.faces.front().second == portrait()[2]);
    CHECK(mapped.subfaces.empty());
}

TEST_CASE("Automatic one through six suggestions use frozen material centers and recover deterministically", "[SemanticPaletteMapping]")
{
    const std::vector<Color> colors {{.76f,.55f,.41f}, {.12f,.11f,.12f}, {.72f,.25f,.23f},
                                    {.94f,.94f,.94f}, {.18f,.45f,.63f}, {.40f,.39f,.38f}};
    const auto source = strip(colors);
    const auto analysis = evidence(source, {Label::FaceSkin,Label::Hair,Label::Lips,
                                          Label::Clothes,Label::Clothes,Label::Clothes});
    std::vector<MaterialCenter> centers;
    map_palette_materials(source, analysis, colors, {}, centers);
    REQUIRE_FALSE(centers.empty());
    for (const auto& center : centers) if (center.face_count > 0) REQUIRE(center.surface_area > 0.0);
    std::vector<PaletteSlot> six; std::string error;
    REQUIRE(suggest_material_slots(centers, 6, {}, six, error));
    for (size_t requested : {6u,5u,4u,3u,2u,1u,2u,3u,4u,5u,6u,1u,6u}) {
        std::vector<PaletteSlot> actual;
        REQUIRE(suggest_material_slots(centers, requested, {}, actual, error));
        REQUIRE_FALSE(actual.empty()); REQUIRE(actual.size() <= requested);
        for (const auto& slot : actual)
            REQUIRE(std::any_of(centers.begin(), centers.end(), [&](const auto& center) {
                return center.original_rgb == slot.color;
            }));
        if (requested == 6) {
            REQUIRE(actual.size() == six.size());
            for (size_t index = 0; index < actual.size(); ++index) {
                REQUIRE(actual[index].id == six[index].id);
                REQUIRE(actual[index].color == six[index].color);
            }
        }
    }
    std::reverse(centers.begin(), centers.end());
    std::vector<PaletteSlot> reordered;
    REQUIRE(suggest_material_slots(centers, 6, {}, reordered, error));
    REQUIRE(reordered.size() == six.size());
    for (size_t index = 0; index < six.size(); ++index) REQUIRE(reordered[index].id == six[index].id);
}

TEST_CASE("Automatic suggestions protect tiny lip and eye materials from surface-area dominance", "[SemanticPaletteMapping]")
{
    const std::vector<Color> colors {{.76f,.55f,.41f}, {.12f,.11f,.12f}, {.72f,.25f,.23f},
                                    {.94f,.94f,.94f}, {.24f,.38f,.60f}, {.30f,.65f,.35f}};
    const auto source = strip(colors);
    const auto analysis = evidence(source, {Label::FaceSkin,Label::Hair,Label::Lips,
                                          Label::EyeSclera,Label::Clothes,Label::Clothes});
    std::vector<MaterialCenter> centers;
    map_palette_materials(source, analysis, colors, {}, centers);
    for (auto& center : centers)
        center.surface_area = center.label == Label::Lips || center.label == Label::EyeSclera ? .00001 : 100.0;
    std::vector<PaletteSlot> suggested; std::string error;
    REQUIRE(suggest_material_slots(centers, 6, {}, suggested, error));
    for (Label detail : {Label::Lips, Label::EyeSclera}) {
        const auto center = std::find_if(centers.begin(), centers.end(), [&](const auto& value) {
            return value.label == detail && value.face_count > 0;
        });
        REQUIRE(center != centers.end());
        REQUIRE(std::any_of(suggested.begin(), suggested.end(), [&](const auto& slot) {
            return slot.color == center->original_rgb;
        }));
    }
}

TEST_CASE("Locked suggestions raise the effective count and never lose slot identity", "[SemanticPaletteMapping]")
{
    const auto source = strip({{.76f,.55f,.41f}, {.12f,.11f,.12f}});
    const auto analysis = evidence(source, {Label::FaceSkin,Label::Hair});
    std::vector<MaterialCenter> centers;
    map_palette_materials(source, analysis, portrait(), {}, centers);
    const std::vector<PaletteSlot> locked {{"user-right",{.3f,.6f,.2f}}, {"user-left",{.2f,.3f,.7f}}};
    std::vector<PaletteSlot> suggested; std::string error;
    REQUIRE(suggest_material_slots(centers, 1, locked, suggested, error));
    REQUIRE(suggested.size() == 2);
    for (const auto& slot : locked)
        REQUIRE(std::any_of(suggested.begin(), suggested.end(), [&](const auto& candidate) {
            return candidate.id == slot.id && candidate.color == slot.color;
        }));
    auto too_many = slots(std::vector<Color>(7,{.2f,.2f,.2f}));
    REQUIRE_FALSE(suggest_material_slots(centers, 6, too_many, suggested, error));
    REQUIRE(suggested.empty());
}

TEST_CASE("Recovered old mapping data remains readable without inventing source surface area", "[SemanticPaletteMapping]")
{
    const auto source = strip({{.76f,.55f,.41f}}); const auto analysis = evidence(source,{Label::FaceSkin});
    const auto palette = slots(portrait()); SlotMappingResult mapped, restored; std::string error;
    REQUIRE(map_palette_slots(source,analysis,palette,portrait(),{},mapped,error));
    auto old = encode_slot_mapping(mapped); old["schema"] = "orca.semantic-material-mapping/v2";
    for (auto& face : old["faces"]) while (face.size() > 4) face.erase(face.end()-1);
    for (auto& leaf : old["subfaces"]) while (leaf.size() > 7) leaf.erase(leaf.end()-1);
    for (auto& center : old["centers"]) while (center.size() > 5) center.erase(center.end()-1);
    old.erase("resolved_regions"); old.erase("palette_signature"); old.erase("portrait_card");
    REQUIRE(decode_slot_mapping(old,1,palette,restored,error));
    REQUIRE(restored.faces == mapped.faces);
    std::vector<PaletteSlot> suggested;
    REQUIRE_FALSE(suggest_material_slots(restored.material_centers,3,{},suggested,error));
    REQUIRE_FALSE(error.empty());
}

TEST_CASE("The previous region mapping schema remains readable after material refinement changes",
          "[SemanticPaletteMapping][Regression]")
{
    const auto source=strip({{.76f,.55f,.41f},{.12f,.11f,.12f}});
    const auto analysis=evidence(source,{Label::FaceSkin,Label::Hair});
    const auto palette=slots(portrait());
    SlotMappingResult mapped,restored;std::string error;
    REQUIRE(map_palette_slots(source,analysis,palette,portrait(),{},mapped,error));
    auto previous=encode_slot_mapping(mapped);
    previous["schema"]="orca.semantic-material-mapping/v5";
    REQUIRE(decode_slot_mapping(previous,source.mesh.indices.size(),palette,restored,error));
    CHECK(restored.faces==mapped.faces);
    CHECK(restored.subfaces==mapped.subfaces);
    CHECK(restored.region_overrides==mapped.region_overrides);
}

TEST_CASE("Filament remapping reuses one immutable whole and child material discovery across workers",
          "[SemanticPaletteMapping][MaterialDiscoveryCache]")
{
    const auto source=strip({{.76f,.55f,.41f},{.74f,.53f,.39f},{.68f,.26f,.24f},{.12f,.11f,.12f}});
    const auto analysis=evidence(source,{Label::FaceSkin,Label::FaceSkin,Label::Lips,Label::Hair});
    const auto palette=slots(portrait());
    clear_material_discovery_cache();
    SlotMappingResult first; std::string error;
    REQUIRE(map_palette_slots(source,analysis,palette,portrait(),{},first,error));
    REQUIRE(material_discovery_cache_stats().builds==1);
    REQUIRE(material_discovery_cache_stats().hits>=1); // whole and child reuse
    for (size_t count:{1u,2u,3u,4u,5u,6u,2u,6u,1u,6u}) {
        auto reduced=palette;
        for (size_t i=0;i<reduced.size();++i) reduced[i].enabled=i<count;
        auto next=std::async(std::launch::async,[&source,&analysis,reduced] {
            SlotMappingResult mapped; std::string message;
            const bool okay=map_palette_slots(source,analysis,reduced,portrait(),{},mapped,message);
            return std::make_pair(okay,std::move(mapped));
        }).get();
        REQUIRE(next.first);
        REQUIRE(next.second.material_centers.size()==first.material_centers.size());
        for (size_t i=0;i<first.material_centers.size();++i)
            CHECK(next.second.material_centers[i].original_oklab==first.material_centers[i].original_oklab);
        if (count==6) CHECK(next.second.faces==first.faces);
    }
    CHECK(material_discovery_cache_stats().builds==1);
}

TEST_CASE("Material discovery invalidates changed evidence even under an unchanged model signature",
          "[SemanticPaletteMapping][MaterialDiscoveryCache]")
{
    const auto source=strip({{.76f,.55f,.41f},{.68f,.26f,.24f},{.12f,.11f,.12f}});
    const auto base=evidence(source,{Label::FaceSkin,Label::Lips,Label::Hair});
    const auto palette=slots(portrait());
    clear_material_discovery_cache();
    SlotMappingResult mapped; std::string error;
    REQUIRE(map_palette_slots(source,base,palette,portrait(),{},mapped,error));
    uint64_t builds=material_discovery_cache_stats().builds;
    auto changed=base;
    changed.face_labels[1]=Label::FaceSkin;
    REQUIRE(changed.signature==base.signature);
    REQUIRE(map_palette_slots(source,changed,palette,portrait(),{},mapped,error));
    CHECK(material_discovery_cache_stats().builds==++builds);
    CHECK(std::none_of(mapped.material_centers.begin(),mapped.material_centers.end(),[](const auto& center) {
        return center.label==Label::Lips;
    }));
    changed.face_confidence[0]=.2f;
    REQUIRE(map_palette_slots(source,changed,palette,portrait(),{},mapped,error));
    CHECK(material_discovery_cache_stats().builds==++builds);
    changed.subface_labels={{2,{3,0},Label::Iris,.95f,3}};
    REQUIRE(map_palette_slots(source,changed,palette,portrait(),{},mapped,error));
    CHECK(material_discovery_cache_stats().builds==++builds);
    changed.subface_labels[0].samples=4;
    REQUIRE(map_palette_slots(source,changed,palette,portrait(),{},mapped,error));
    CHECK(material_discovery_cache_stats().builds==++builds);
    changed.baseline_face_labels=base.face_labels;
    changed.baseline_face_confidence=base.face_confidence;
    // Avoid a tiny-fixture subface budget rejection invoking the baseline path.
    changed.subface_labels.clear();
    REQUIRE(map_palette_slots(source,changed,palette,portrait(),{},mapped,error));
    CHECK(material_discovery_cache_stats().builds==++builds);
    changed.baseline_face_confidence[0]=.8f;
    REQUIRE(map_palette_slots(source,changed,palette,portrait(),{},mapped,error));
    CHECK(material_discovery_cache_stats().builds==++builds);
    changed.baseline_subface_labels={{2,{2,0},Label::Hair,.95f,3}};
    REQUIRE(map_palette_slots(source,changed,palette,portrait(),{},mapped,error));
    CHECK(material_discovery_cache_stats().builds>=++builds);
}

TEST_CASE("Candidate and baseline discoveries coexist and source changes invalidate them",
          "[SemanticPaletteMapping][MaterialDiscoveryCache]")
{
    auto source=strip({{.76f,.55f,.41f},{.68f,.26f,.24f}});
    auto baseline=evidence(source,{Label::FaceSkin,Label::Lips});
    auto candidate=baseline; candidate.face_confidence[0]=.9f;
    std::vector<MaterialCenter> centers;
    clear_material_discovery_cache();
    map_palette_materials(source,baseline,portrait(),portrait(),centers);
    map_palette_materials(source,candidate,portrait(),portrait(),centers);
    REQUIRE(material_discovery_cache_stats().builds==2);
    map_palette_materials(source,baseline,portrait(),portrait(),centers);
    map_palette_materials(source,candidate,portrait(),portrait(),centers);
    CHECK(material_discovery_cache_stats().builds==2);
    CHECK(material_discovery_cache_stats().hits==2);
    source.face_colors[0]={.6f,.4f,.3f,1.f}; source.content_id=content_fingerprint(source);
    const auto changed=evidence(source,{Label::FaceSkin,Label::Lips});
    map_palette_materials(source,changed,portrait(),portrait(),centers);
    CHECK(material_discovery_cache_stats().builds==3);
    const auto new_color=centers.front().original_oklab;
    CHECK(new_color!=Color{});
    auto new_provider=changed; new_provider.face_identity="replacement-face-v2";
    new_provider.signature=analysis_cache_key(source,new_provider.body_identity,new_provider.face_identity);
    map_palette_materials(source,new_provider,portrait(),portrait(),centers);
    CHECK(material_discovery_cache_stats().builds==4);
}

TEST_CASE("Canceled and failed analysis never populate material discovery cache",
          "[SemanticPaletteMapping][MaterialDiscoveryCache]")
{
    const auto source=strip({{.76f,.55f,.41f},{.68f,.26f,.24f}});
    auto analysis=evidence(source,{Label::FaceSkin,Label::Lips});
    clear_material_discovery_cache();
    SlotMappingResult mapped; std::string error;
    REQUIRE_FALSE(map_palette_slots(source,analysis,slots(portrait()),portrait(),{},mapped,error,[]{return true;}));
    CHECK(material_discovery_cache_stats().builds==0);
    analysis.canceled=true;
    REQUIRE_FALSE(map_palette_slots(source,analysis,slots(portrait()),portrait(),{},mapped,error));
    CHECK(material_discovery_cache_stats().builds==0);
    analysis.canceled=false; analysis.error="recognizer unavailable";
    REQUIRE_FALSE(map_palette_slots(source,analysis,slots(portrait()),portrait(),{},mapped,error));
    CHECK(material_discovery_cache_stats().builds==0);
    analysis.error.clear();
    REQUIRE(map_palette_slots(source,analysis,slots(portrait()),portrait(),{},mapped,error));
    CHECK(material_discovery_cache_stats().builds==1);
}

TEST_CASE("Rejected boundary masks reuse safe material discovery while inference failures remain uncached",
          "[SemanticPaletteMapping][MaterialDiscoveryCache]")
{
    const auto source = strip({{.76f,.55f,.41f}, {.68f,.26f,.24f}, {.12f,.11f,.12f}});
    const auto baseline = evidence(source, {Label::FaceSkin, Label::Lips, Label::Hair});
    const auto palette = slots(portrait());
    SlotMappingResult safe; std::string error;
    REQUIRE(map_palette_slots(source, baseline, palette, portrait(), {}, safe, error));
    REQUIRE(safe.faces.size() == 3);

    auto fallback = baseline;
    BoundaryRunDiagnostic diagnostic;
    diagnostic.person_id = 1;
    diagnostic.part = BoundaryPart::Ear;
    diagnostic.side = BoundarySide::Right;
    diagnostic.status = "rejected";
    diagnostic.reason = "No boundary mask satisfies the positive and negative prompts.";
    fallback.boundary_runs.push_back(diagnostic);
    clear_material_discovery_cache();

    SECTION("A valid but rejected contour keeps original material evidence reusable across workers")
    {
        for (size_t count : {6u, 2u, 1u, 6u}) {
            auto active = palette;
            for (size_t slot = 0; slot < active.size(); ++slot) active[slot].enabled = slot < count;
            auto result = std::async(std::launch::async, [&source, &fallback, active] {
                SlotMappingResult mapped; std::string message;
                const bool okay = map_palette_slots(source, fallback, active, portrait(), {}, mapped, message);
                return std::make_pair(okay, std::move(mapped));
            }).get();
            REQUIRE(result.first);
            REQUIRE(result.second.faces.size() == safe.faces.size());
            REQUIRE(result.second.material_centers.size() == safe.material_centers.size());
            for (size_t material = 0; material < safe.material_centers.size(); ++material) {
                CHECK(result.second.material_centers[material].label == safe.material_centers[material].label);
                CHECK(result.second.material_centers[material].original_oklab == safe.material_centers[material].original_oklab);
            }
            if (count == 6) CHECK(result.second.faces == safe.faces);
        }
        CHECK(material_discovery_cache_stats().builds == 1);
        CHECK(material_discovery_cache_stats().hits >= 7);
        CHECK(fallback.boundary_runs.front().status == "rejected");
    }

    SECTION("A real inference error uses safe colors without creating successful reusable entries")
    {
        fallback.boundary_runs.front().status = "error";
        fallback.boundary_runs.front().reason = "ONNX Runtime inference failed.";
        for (size_t count : {6u, 2u, 6u}) {
            auto active = palette;
            for (size_t slot = 0; slot < active.size(); ++slot) active[slot].enabled = slot < count;
            SlotMappingResult mapped;
            REQUIRE(map_palette_slots(source, fallback, active, portrait(), {}, mapped, error));
            REQUIRE(mapped.faces.size() == safe.faces.size());
            if (count == 6) CHECK(mapped.faces == safe.faces);
            CHECK(material_discovery_cache_stats().builds == 0);
            CHECK(material_discovery_cache_stats().hits == 0);
        }
        // A later valid inference may still reject its contour safely. That
        // normal fallback should recover caching instead of remaining poisoned
        // by the previous runtime failure.
        fallback.boundary_runs.front() = diagnostic;
        SlotMappingResult recovered;
        REQUIRE(map_palette_slots(source, fallback, palette, portrait(), {}, recovered, error));
        CHECK(recovered.faces == safe.faces);
        CHECK(material_discovery_cache_stats().builds == 1);
        CHECK(material_discovery_cache_stats().hits >= 1);
        CHECK(fallback.boundary_runs.front().status == "rejected");
    }
}
