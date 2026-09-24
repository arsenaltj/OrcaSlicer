#include <catch2/catch_all.hpp>
#include "slic3r/GUI/AI/Model/BeautyPuzzle.hpp"

using namespace Slic3r;
using namespace Slic3r::AI;

TEST_CASE("Beauty partitions remain selectable and restorable after a local boundary edit", "[BeautyPartition]")
{
    const auto mesh = its_make_cube(10, 10, 10);
    const auto surface = BeautySurface::build(mesh, {});
    auto partition = BeautyPuzzle::create_regions(*surface);
    REQUIRE(partition.face_piece.size() == mesh.indices.size());
    REQUIRE_NOTHROW(partition.validate(*surface));

    const auto selected_piece = partition.face_piece.front();
    const auto selected = partition.faces(selected_piece);
    REQUIRE_FALSE(selected.empty());
    REQUIRE(selected.front() < mesh.indices.size());

    const auto new_piece = partition.assign_region({selected.front()}, *surface);
    REQUIRE(new_piece != selected_piece);
    REQUIRE_NOTHROW(partition.validate(*surface));

    const auto restored = BeautyPuzzle::decode(partition.encode(), surface->geometry_id, mesh.indices.size());
    CHECK(restored.same_edit(partition));
    REQUIRE_NOTHROW(restored.validate(*surface));
    CHECK_THROWS(BeautyPuzzle::decode(partition.encode(), "different geometry", mesh.indices.size()));
}
