#include <catch2/catch_all.hpp>
#include <cstring>

#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;

// A sphere gives well over ExtruderMax original facets, so every extruder state can be assigned
// to a facet of its own without any splitting getting in the way.
static TriangleMesh test_mesh() { return make_sphere(5., 2 * PI / 24); }

// Read the nibble_idx-th 4-bit group of a serialized bitstream, least significant bit first.
static int nibble_at(const std::vector<bool> &bitstream, size_t nibble_idx)
{
    int n = 0;
    for (size_t bit = 0; bit < 4; ++bit)
        n |= int(bitstream[nibble_idx * 4 + bit]) << bit;
    return n;
}

TEST_CASE("Every extruder state survives a serialize/deserialize round trip", "[TriangleSelector]")
{
    const TriangleMesh mesh      = test_mesh();
    const int          max_state = int(EnforcerBlockerType::ExtruderMax);
    REQUIRE(int(mesh.its.indices.size()) >= max_state);

    TriangleSelector selector(mesh);
    for (int state = 1; state <= max_state; ++state)
        selector.set_facet(state - 1, EnforcerBlockerType(state));

    TriangleSelector restored(mesh);
    restored.deserialize(selector.serialize());

    for (int state = 1; state <= max_state; ++state) {
        INFO("Extruder " << state);
        REQUIRE(restored.has_facets(EnforcerBlockerType(state)));
        REQUIRE(restored.num_facets(EnforcerBlockerType(state)) == 1);
    }
}

TEST_CASE("Serialized data reports the extruder states it uses", "[TriangleSelector]")
{
    const TriangleMesh mesh = test_mesh();
    TriangleSelector   selector(mesh);
    selector.set_facet(0, EnforcerBlockerType::Extruder16);
    selector.set_facet(1, EnforcerBlockerType::Extruder32);

    const TriangleSelector::TriangleSplittingData data = selector.serialize();

    REQUIRE(data.used_states.size() == size_t(EnforcerBlockerType::ExtruderMax) + 1);
    REQUIRE(data.used_states[size_t(EnforcerBlockerType::Extruder16)]);
    REQUIRE(data.used_states[size_t(EnforcerBlockerType::Extruder32)]);
    REQUIRE_FALSE(data.used_states[size_t(EnforcerBlockerType::Extruder17)]);

    SECTION("used_states recomputed from the bitstream agrees") {
        TriangleSelector::TriangleSplittingData recomputed = data;
        recomputed.reset_used_states();
        recomputed.update_used_states(0);
        REQUIRE(recomputed.used_states == data.used_states);
    }

    SECTION("has_facets on the raw data agrees") {
        REQUIRE(TriangleSelector::has_facets(data, EnforcerBlockerType::Extruder32));
        REQUIRE_FALSE(TriangleSelector::has_facets(data, EnforcerBlockerType::Extruder17));
    }
}

// States 3..17 must keep the pre-existing encoding ("11" prefix plus one nibble of state-3) so
// projects written by older builds stay readable and newly written ones stay readable by them.
TEST_CASE("Extruder states up to 17 keep the single-nibble encoding", "[TriangleSelector]")
{
    const int state = GENERATE(3, 8, 16, 17);

    TriangleSelector selector(test_mesh());
    selector.set_facet(0, EnforcerBlockerType(state));
    const std::vector<bool> bitstream = selector.serialize().bitstream;

    INFO("Extruder " << state);
    // Two nibbles: the "11"-prefixed leaf code, then the state itself.
    REQUIRE(bitstream.size() == 8);
    REQUIRE(nibble_at(bitstream, 0) == 0b1100);
    REQUIRE(nibble_at(bitstream, 1) == state - 3);
}

// States 18 and above set the state nibble to 0b1111 and carry (state-18) in one more nibble.
TEST_CASE("Extruder states above 17 are encoded in a second nibble", "[TriangleSelector]")
{
    const int state = GENERATE(18, 25, 32);

    TriangleSelector selector(test_mesh());
    selector.set_facet(0, EnforcerBlockerType(state));
    const std::vector<bool> bitstream = selector.serialize().bitstream;

    INFO("Extruder " << state);
    REQUIRE(bitstream.size() == 12);
    REQUIRE(nibble_at(bitstream, 0) == 0b1100);
    REQUIRE(nibble_at(bitstream, 1) == 0b1111);
    REQUIRE(nibble_at(bitstream, 2) == state - 18);
}

// Model.cpp writes these hex strings into the 3MF for colored mesh imports; the selector must
// decode exactly the states CONST_FILAMENTS assigns to them.
TEST_CASE("Extruder states match the CONST_FILAMENTS hex encoding", "[TriangleSelector]")
{
    struct Case { const char *hex; int state; };
    const auto c = GENERATE(values<Case>({
        {"8", 2}, {"0C", 3}, {"DC", 16}, {"EC", 17}, {"0FC", 18}, {"EFC", 32},
    }));

    // get_triangle_as_string emits the nibbles most significant first, so read the hex backwards.
    const std::string hex = c.hex;
    std::vector<bool> bitstream;
    for (auto it = hex.rbegin(); it != hex.rend(); ++it) {
        const int nibble = *it >= 'A' ? (*it - 'A' + 10) : (*it - '0');
        for (int bit = 0; bit < 4; ++bit)
            bitstream.push_back((nibble >> bit) & 1);
    }

    TriangleSelector::TriangleSplittingData data;
    data.triangles_to_split.emplace_back(0, 0);
    data.bitstream = bitstream;

    INFO("Hex " << c.hex << " -> extruder " << c.state);
    REQUIRE(TriangleSelector::has_facets(data, EnforcerBlockerType(c.state)));
}

TEST_CASE("Deterministic midpoint subfaces survive MMU serialization", "[TriangleSelector][SubfaceColor]")
{
    const TriangleMesh mesh = test_mesh();
    TriangleSelector selector(mesh);
    const std::vector<TriangleSelector::MidpointSubfaceState> leaves {
        {1, 0, EnforcerBlockerType::Extruder2},
        // First child 1, then centre child 3.
        {2, uint8_t((1u << 2) | 3u), EnforcerBlockerType::Extruder3},
    };
    REQUIRE(selector.set_facet_midpoint_subfaces(0, EnforcerBlockerType::Extruder1, leaves));
    CHECK(selector.num_facets(EnforcerBlockerType::Extruder1) == 5);
    CHECK(selector.num_facets(EnforcerBlockerType::Extruder2) == 1);
    CHECK(selector.num_facets(EnforcerBlockerType::Extruder3) == 1);

    const auto encoded = selector.serialize();
    TriangleSelector restored(mesh);
    restored.deserialize(encoded);
    CHECK(restored.serialize() == encoded);
    CHECK(restored.num_facets(EnforcerBlockerType::Extruder1) == 5);
    CHECK(restored.num_facets(EnforcerBlockerType::Extruder2) == 1);
    CHECK(restored.num_facets(EnforcerBlockerType::Extruder3) == 1);
}

TEST_CASE("Invalid midpoint subface input leaves the original facet unchanged", "[TriangleSelector][SubfaceColor]")
{
    const TriangleMesh mesh = test_mesh();
    TriangleSelector selector(mesh);
    selector.set_facet(0, EnforcerBlockerType::Extruder4);
    const auto before = selector.serialize();
    CHECK_FALSE(selector.set_facet_midpoint_subfaces(0, EnforcerBlockerType::Extruder1,
        {{2, 16, EnforcerBlockerType::Extruder2}}));
    CHECK(selector.serialize() == before);
    CHECK_FALSE(selector.set_facet_midpoint_subfaces(0, EnforcerBlockerType::Extruder1,
        {{1, 2, EnforcerBlockerType::Extruder2}, {1, 2, EnforcerBlockerType::Extruder3}}));
    CHECK(selector.serialize() == before);
    CHECK_FALSE(selector.set_facet_midpoint_subfaces(0, EnforcerBlockerType::Extruder1,
        {{5, 0, EnforcerBlockerType::Extruder2}}));
    CHECK(selector.serialize() == before);
}

TEST_CASE("Fine midpoint details survive native painting serialization", "[TriangleSelector][SubfaceColor]")
{
    const auto depth = GENERATE(uint8_t(3), uint8_t(4));
    const TriangleMesh mesh = test_mesh();
    TriangleSelector selector(mesh);
    const uint8_t path = uint8_t((1u << (2u * depth)) - 1u);
    REQUIRE(selector.set_facet_midpoint_subfaces(0, EnforcerBlockerType::Extruder1,
        {{depth, path, EnforcerBlockerType::Extruder2}}));
    CHECK(selector.num_facets(EnforcerBlockerType::Extruder2) == 1);
    CHECK(selector.num_facets(EnforcerBlockerType::Extruder1) == 3 * depth);
    const auto encoded = selector.serialize();
    TriangleSelector restored(mesh);
    restored.deserialize(encoded);
    CHECK(restored.serialize() == encoded);
    CHECK(restored.num_facets(EnforcerBlockerType::Extruder2) == 1);
}

namespace {
void require_same_facet_mesh(const indexed_triangle_set& actual, const indexed_triangle_set& expected)
{
    REQUIRE(actual.vertices.size() == expected.vertices.size());
    REQUIRE(actual.indices.size() == expected.indices.size());
    for (size_t i = 0; i < actual.vertices.size(); ++i) {
        // Color groups must retain original vertex bits, including signed zero.
        REQUIRE(std::memcmp(actual.vertices[i].data(), expected.vertices[i].data(), 3 * sizeof(float)) == 0);
    }
    for (size_t i = 0; i < actual.indices.size(); ++i)
        REQUIRE(std::memcmp(actual.indices[i].data(), expected.indices[i].data(), 3 * sizeof(int)) == 0);
}

void require_all_facet_meshes(const TriangleSelector& selector)
{
    std::vector<indexed_triangle_set> all;
    selector.get_facets(all);
    REQUIRE(all.size() == size_t(EnforcerBlockerType::ExtruderMax) + 1);
    for (size_t state = 0; state < all.size(); ++state) {
        CAPTURE(state);
        require_same_facet_mesh(all[state], selector.get_facets(EnforcerBlockerType(state)));
    }
}
}

TEST_CASE("All color facet meshes retain vertex bits and face order for every state", "[TriangleSelector][FacetMeshes]")
{
    TriangleMesh mesh = test_mesh();
    mesh.its.vertices.front().x() = -0.0f;
    const int states = int(EnforcerBlockerType::ExtruderMax) + 1;
    const int painted_states = GENERATE(1, 2, 6, 33);
    REQUIRE(painted_states <= states);
    TriangleSelector selector(mesh);
    for (size_t i = 0; i < mesh.its.indices.size(); ++i)
        selector.set_facet(int(i), EnforcerBlockerType(i % painted_states));
    const auto before = selector.serialize();
    require_all_facet_meshes(selector);
    CHECK(selector.serialize() == before);
}

TEST_CASE("All color facet meshes retain nested split leaves across recovery and replacement", "[TriangleSelector][FacetMeshes]")
{
    const TriangleMesh mesh = test_mesh();
    TriangleSelector selector(mesh);
    REQUIRE(selector.set_facet_midpoint_subfaces(0, EnforcerBlockerType::Extruder1,
        {{1, 0, EnforcerBlockerType::Extruder2}, {2, uint8_t((1u << 2) | 3u), EnforcerBlockerType::Extruder32}}));
    REQUIRE(selector.set_facet_midpoint_subfaces(1, EnforcerBlockerType::Extruder18,
        {{2, uint8_t((2u << 2) | 1u), EnforcerBlockerType::Extruder3}}));
    const auto before = selector.serialize();
    require_all_facet_meshes(selector);
    TriangleSelector restored(mesh);
    auto historical = before;
    historical.used_states.clear();
    restored.deserialize(historical);
    require_all_facet_meshes(restored);
    std::vector<indexed_triangle_set> original, recovered;
    selector.get_facets(original);
    restored.get_facets(recovered);
    REQUIRE(original.size() == recovered.size());
    for (size_t i = 0; i < original.size(); ++i)
        require_same_facet_mesh(original[i], recovered[i]);
    CHECK(selector.serialize() == before);
    restored.set_facet(0, EnforcerBlockerType::Extruder4);
    require_all_facet_meshes(restored);
    restored.reset();
    require_all_facet_meshes(restored);
}

TEST_CASE("All color facet meshes replace reused output slots and retain empty groups", "[TriangleSelector][FacetMeshes]")
{
    const TriangleMesh empty_mesh;
    TriangleSelector empty(empty_mesh);
    std::vector<indexed_triangle_set> all(2, test_mesh().its);
    empty.get_facets(all);
    REQUIRE(all.size() == size_t(EnforcerBlockerType::ExtruderMax) + 1);
    for (const auto& group : all) {
        CHECK(group.vertices.empty());
        CHECK(group.indices.empty());
    }
    const TriangleMesh mesh = test_mesh();
    TriangleSelector selector(mesh);
    selector.set_facet(0, EnforcerBlockerType::Extruder32);
    selector.get_facets(all);
    require_same_facet_mesh(all[32], selector.get_facets(EnforcerBlockerType::Extruder32));
    selector.reset();
    selector.get_facets(all);
    require_same_facet_mesh(all[0], selector.get_facets(EnforcerBlockerType::NONE));
    for (size_t state = 1; state < all.size(); ++state) {
        CHECK(all[state].vertices.empty());
        CHECK(all[state].indices.empty());
    }
}
