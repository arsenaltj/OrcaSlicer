#include <catch2/catch_all.hpp>
#include "slic3r/GUI/AI/Model/BeautyEditRegions.hpp"

using namespace Slic3r;
using namespace Slic3r::AI;

namespace {
const std::string source_hash(64, 'a');

BeautyPuzzle single_piece(const BeautySurface& surface) {
    BeautyPuzzle puzzle;
    puzzle.geometry_id = surface.geometry_id;
    puzzle.next_id = 2;
    puzzle.face_piece.assign(surface.areas.size(), 1);
    puzzle.palette = {{0, "#202020", "PLA", true}, {1, "#FF8080", "PLA", true}};
    puzzle.paint_filament(1, 0);
    return puzzle;
}
}

TEST_CASE("A logical edit region survives native print-piece splitting", "[BeautyEditRegions]") {
    const auto surface = BeautySurface::build(its_make_cube(10, 10, 10), {});
    auto printing = single_piece(*surface);
    REQUIRE_NOTHROW(printing.validate(*surface));
    const auto before = printing;
    std::vector<int32_t> proposal(printing.face_piece.size(), -1);
    proposal[0] = proposal[6] = 7; // two separated triangles, one user-facing selection
    const auto editing = BeautyEditRegions::from_candidate(before, proposal, source_hash);
    const uint32_t group = editing.at(0);
    REQUIRE(group == editing.at(6));
    REQUIRE(group != editing.at(1));
    CHECK(editing.faces(group) == std::vector<size_t>{0, 6});
    const auto frozen = editing.encode();

    editing.paint_filament(printing, *surface, source_hash, group, 1);
    REQUIRE_NOTHROW(printing.validate(*surface));
    const auto displayed = editing.representative_color(printing, *surface, group);
    CHECK_THAT(displayed[0], Catch::Matchers::WithinAbs(1.f, 1e-6));
    CHECK_THAT(displayed[1], Catch::Matchers::WithinAbs(128.f / 255.f, 1e-6));
    CHECK(printing.piece_count() > before.piece_count());
    CHECK(editing.encode() == frozen);
    for (size_t f = 0; f < printing.face_piece.size(); ++f) {
        const auto actual = printing.filament_slots.at(printing.face_piece[f]);
        CHECK(actual == (proposal[f] == 7 ? 1 : 0));
        CHECK(editing.at(f) == (proposal[f] == 7 ? group : before.face_piece[f]));
    }
    const auto reopened = BeautyEditRegions::decode(frozen, printing.geometry_id, source_hash,
                                                     printing.face_piece.size());
    CHECK(reopened.face_region == editing.face_region);
    CHECK(reopened.faces(group) == editing.faces(group));
}

TEST_CASE("An edit region record rejects another model and damaged coverage", "[BeautyEditRegions]") {
    const auto surface = BeautySurface::build(its_make_cube(10, 10, 10), {});
    const auto printing = single_piece(*surface);
    std::vector<int32_t> proposal(printing.face_piece.size(), -1);
    proposal[0] = 0;
    const auto editing = BeautyEditRegions::from_candidate(printing, proposal, source_hash);
    const auto saved = editing.encode();
    const size_t faces = printing.face_piece.size();
    CHECK_THROWS(BeautyEditRegions::decode(saved, "other-geometry", source_hash, faces));
    CHECK_THROWS(BeautyEditRegions::decode(saved, printing.geometry_id, std::string(64, 'b'), faces));
    CHECK_THROWS(BeautyEditRegions::decode(saved, printing.geometry_id, source_hash, faces - 1));
    auto damaged = saved;
    damaged["runs"][0][1] = 0;
    CHECK_THROWS(BeautyEditRegions::decode(damaged, printing.geometry_id, source_hash, faces));
    damaged = saved;
    damaged["runs"][0][1] = faces + 1;
    CHECK_THROWS(BeautyEditRegions::decode(damaged, printing.geometry_id, source_hash, faces));
    CHECK_THROWS(BeautyEditRegions::from_candidate(printing, std::vector<int32_t>(faces - 1, -1), source_hash));
    proposal[1] = -2;
    CHECK_THROWS(BeautyEditRegions::from_candidate(printing, proposal, source_hash));
}

TEST_CASE("Dragging a logical boundary transfers only touched printable faces", "[BeautyEditRegions]") {
    const auto surface = BeautySurface::build(its_make_cube(10, 10, 10), {});
    auto printing = single_piece(*surface);
    std::vector<int32_t> proposal(printing.face_piece.size(), -1);
    proposal[0] = 7;
    auto editing = BeautyEditRegions::from_candidate(printing, proposal, source_hash);
    const auto group = editing.at(0);
    const auto adjacent = *std::find_if(surface->face_neighbors[0].begin(),
        surface->face_neighbors[0].end(), [](int32_t face) { return face >= 0; });
    REQUIRE(adjacent >= 0);
    editing.paint_filament(printing, *surface, source_hash, group, 1);
    const auto baseline_regions = editing.encode();
    const auto baseline_editing = editing;
    const auto baseline_print = printing;

    REQUIRE(editing.reshape_and_paint(printing, *surface, source_hash, group,
                                      {size_t(adjacent)}, {}));
    CHECK(editing.at(size_t(adjacent)) == group);
    CHECK(printing.filament_slots.at(printing.face_piece[size_t(adjacent)]) == 1);
    for (size_t f = 0; f < printing.face_piece.size(); ++f) if (f != size_t(adjacent)) {
        CHECK(editing.at(f) == baseline_editing.at(f));
        CHECK(printing.filament_slots.at(printing.face_piece[f]) ==
              baseline_print.filament_slots.at(baseline_print.face_piece[f]));
    }
    REQUIRE(editing.reshape_and_paint(printing, *surface, source_hash, group, {}, {0}));
    CHECK(editing.at(0) != group);
    CHECK(editing.at(size_t(adjacent)) == group);
    CHECK(printing.filament_slots.at(printing.face_piece[0]) == 0);
    CHECK(printing.filament_slots.at(printing.face_piece[size_t(adjacent)]) == 1);
    CHECK_NOTHROW(printing.validate(*surface));
    const auto reopened = BeautyEditRegions::decode(editing.encode(), printing.geometry_id,
                                                      source_hash, printing.face_piece.size());
    CHECK(baseline_regions != editing.encode());
    CHECK(reopened.face_region == editing.face_region);
}
