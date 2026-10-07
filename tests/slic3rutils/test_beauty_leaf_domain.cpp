#include <catch2/catch_test_macros.hpp>
#include "slic3r/GUI/AI/Model/BeautyLeafDomain.hpp"
#include "slic3r/AI/ModelGeneration/SemanticColoring/SemanticColoring.hpp"

using namespace Slic3r::AI;
namespace {
BeautyLeafDomain domain() {
    return {std::string(64, 'a'), 200, {{0,1,0},{0,1,1},{0,1,2},{0,1,3}}};
}
}
TEST_CASE("Leaf partitions cover roots and preserve unsplit face identity", "[BeautyLeafDomain]") {
    auto value = domain();
    REQUIRE_NOTHROW(value.validate());
    REQUIRE(value.all_leaves().size() == 203);
    REQUIRE(value.all_leaves().back().source_face_id == 199);
    REQUIRE(value.all_leaves().back().depth == 0);
    REQUIRE(BeautyLeafDomain::decode(value.encode(), value.canonical_geometry_id, 200,
        value.fingerprint()).split_leaves == value.split_leaves);
}
TEST_CASE("Leaf partitions reject gaps overlaps and hash drift", "[BeautyLeafDomain]") {
    auto value = domain();
    value.split_leaves.pop_back();
    REQUIRE_THROWS(value.validate());
    value = domain();
    value.split_leaves.push_back({0,2,0});
    std::sort(value.split_leaves.begin(), value.split_leaves.end());
    REQUIRE_THROWS(value.validate());
    value = domain();
    REQUIRE_THROWS(BeautyLeafDomain::decode(value.encode(), value.canonical_geometry_id, 200, std::string(64,'b')));
}
TEST_CASE("Depth four leaves have finite normalized canonical coordinates", "[BeautyLeafDomain]") {
    for (unsigned path = 0; path < 256; ++path) {
        const BeautyLeafKey leaf{0,4,uint8_t(path)};
        for (const auto& bary : leaf.barycentric()) {
            REQUIRE(bary.allFinite());
            REQUIRE(bary.minCoeff() >= 0.);
            REQUIRE(bary.sum() == 1.);
        }
    }
    REQUIRE_THROWS(BeautyLeafDomain::validate_keys({{0,0,0},{0,4,255}}, 1));
    REQUIRE_THROWS(BeautyLeafDomain::validate_keys({{1,0,0}}, 1));
}
TEST_CASE("Leaf paths match the existing native midpoint convention", "[BeautyLeafDomain]") {
    namespace SC = Slic3r::AI::SemanticColoring;
    for (uint8_t depth = 1; depth <= 2; ++depth) for (unsigned path = 0; path < (1u << (2*depth)); ++path) {
        const auto corners = BeautyLeafKey{0,depth,uint8_t(path)}.barycentric();
        std::array<SC::Barycentric,3> native;
        REQUIRE(SC::subface_vertices({depth,uint8_t(path)}, native));
        for (size_t i = 0; i < 3; ++i) for (size_t j = 0; j < 3; ++j)
            REQUIRE(corners[i][j] == double(native[i][j]));
    }
    const auto value = domain();
    REQUIRE(value.locate(0, Slic3r::Vec3d(.8,.1,.1)).path == 0);
    REQUIRE(value.locate(0, Slic3r::Vec3d(.1,.1,.8)).path == 2);
    REQUIRE(value.locate(2, Slic3r::Vec3d(.8,.1,.1)).depth == 0);
}
TEST_CASE("Derived leaf meshes reject same count geometry drift", "[BeautyLeafDomain]") {
    indexed_triangle_set source;
    source.vertices = {Slic3r::Vec3f(0,0,0),Slic3r::Vec3f(1,0,0),Slic3r::Vec3f(0,1,0)};
    for (size_t i = 0; i < 200; ++i) source.indices.emplace_back(0,1,2);
    auto value = domain();
    value.canonical_geometry_id = SurfaceSelectionPersistence::geometry_fingerprint(source);
    REQUIRE(value.mesh(source).indices.size() == 203);
    source.vertices.front()[0] = .25f;
    REQUIRE_THROWS(value.mesh(source));
}
TEST_CASE("Adaptive leaf adjacency connects shared edge segments without connecting corner contacts", "[BeautyLeafDomain]") {
    indexed_triangle_set mesh;
    mesh.vertices = {Slic3r::Vec3f(0,0,0),Slic3r::Vec3f(1,0,0),Slic3r::Vec3f(0,1,0),Slic3r::Vec3f(1,1,0)};
    mesh.indices.emplace_back(0,1,2);
    mesh.indices.emplace_back(1,3,2);
    for (int i = 2; i < 400; ++i) {
        const auto index = int(mesh.vertices.size());
        mesh.vertices.insert(mesh.vertices.end(),{Slic3r::Vec3f(float(i)*10,0,0),Slic3r::Vec3f(float(i)*10+1,0,0),Slic3r::Vec3f(float(i)*10,1,0)});
        mesh.indices.emplace_back(index,index+1,index+2);
    }
    const auto surface = BeautySurface::build(mesh,{});
    BeautyLeafDomain mapping{surface->geometry_id,mesh.indices.size(),{{0,1,0},{0,1,1},{0,1,2},{0,1,3}}};
    const auto keys = mapping.all_leaves();
    const auto adjacent = mapping.adjacency(mesh,*surface);
    REQUIRE(adjacent[4] == std::vector<size_t>{1,2});
    REQUIRE(std::find(adjacent[3].begin(),adjacent[3].end(),4) == adjacent[3].end());
    for (size_t i = 0; i < adjacent.size(); ++i) for (const auto n : adjacent[i])
        REQUIRE(std::binary_search(adjacent[n].begin(),adjacent[n].end(),i));
    mapping.split_leaves = {{0,1,1},{0,1,2},{0,1,3},{0,2,0},{0,2,1},{0,2,2},{0,2,3}};
    std::sort(mapping.split_leaves.begin(),mapping.split_leaves.end());
    REQUIRE_NOTHROW(mapping.adjacency(mesh,*surface));
}
