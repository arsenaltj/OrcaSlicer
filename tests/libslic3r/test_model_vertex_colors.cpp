#include <catch2/catch_all.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;

namespace {

Model make_single_triangle_model()
{
    indexed_triangle_set its;
    its.vertices = {{0.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}, {0.0f, 10.0f, 0.0f}};
    its.indices = {{0, 1, 2}};

    Model model;
    ModelObject* object = model.add_object();
    object->add_volume(TriangleMesh(std::move(its)), ModelVolumeType::MODEL_PART, false);
    return model;
}

} // namespace

TEST_CASE("OBJ vertex colors retain non-base filament facets", "[Model][OBJ][MMU]")
{
    Model model = make_single_triangle_model();
    ModelVolume* volume = model.objects.front()->volumes.front();

    REQUIRE(Model::obj_import_vertex_color_deal({1, 1, 1}, 1, &model));
    CHECK(volume->mmu_segmentation_facets.empty());

    REQUIRE(Model::obj_import_vertex_color_deal({1, 1, 2}, 1, &model));
    CHECK_FALSE(volume->mmu_segmentation_facets.empty());
    CHECK_FALSE(volume->mmu_segmentation_facets.get_triangle_as_string(0).empty());
}
