#include "slic3r/GUI/AI/ModelGeneration/PortraitColorBlockPostprocess.hpp"
#include "slic3r/GUI/AI/ModelGeneration/PortraitColorBlockCache.hpp"
#include "slic3r/GUI/AI/ModelGeneration/PortraitColorLayerPublication.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelPreviewPalette.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelSemanticColoring.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelOriginalAppearance.hpp"
#include "slic3r/GUI/AI/ModelGeneration/WorkbenchModelInspection.hpp"
#include "slic3r/GUI/AI/ModelGeneration/PortraitNativePartition.hpp"
#include "slic3r/GUI/AI/ModelGeneration/PortraitNativePartitionCache.hpp"
#include "slic3r/GUI/AI/Model/BeautyPuzzle.hpp"
#include "slic3r/GUI/AI/Orca/FilamentColorPack.hpp"
#include "slic3r/GUI/AI/Orca/WorkbenchModelDecode.hpp"
#include <boost/nowide/cstdlib.hpp>
#include <catch2/catch_all.hpp>
#include <chrono>
#include <cmath>
#include <ctime>
#include <cstring>
#include <iostream>

using namespace Slic3r;
namespace PC=GUI::PortraitColorBlocks;
namespace NP=GUI::NativePortrait;
namespace SC=AI::SemanticColoring;
namespace {
struct Sample {
    SC::MeshSnapshot source;
    SC::Analysis analysis;
    Sample() {
        source.geometry_id=std::string(64,'a');source.content_id=std::string(64,'b');
        source.mesh.vertices={{0,0,0},{1,0,0},{0,1,0},{1,1,0},{0,2,0},{1,2,0}};
        source.mesh.indices={{0,1,2},{1,3,2},{2,3,4},{3,5,4}};
        source.face_colors.assign(4,RGBA{.4f,.4f,.4f,1.f});
        analysis.geometry_id=source.geometry_id;analysis.content_id=source.content_id;
        analysis.signature=std::string(64,'c');analysis.face_labels.assign(4,SC::Label::Unknown);
        analysis.face_confidence.assign(4,.95f);
    }
    PC::Input input() const {return {source,analysis};}
};
const PC::Block& block(const PC::Result& r,size_t face,size_t track=1) {
    const auto unit=std::find_if(r.units.begin(),r.units.end(),[&](const auto& u){return u.face==face;});
    return r.tracks[track].blocks[r.tracks[track].unit_blocks[unit-r.units.begin()]];
}
std::vector<SC::Color> honor_palette() {
    std::vector<SC::Color> result;
    for (size_t i = 0; i < GUI::honor_people_suite().model_slot_count; ++i) {
        const auto& slot = GUI::honor_people_suite().physical_slots[i];
        const auto channel = [&](size_t offset) {
            return float(std::stoul(slot.color.substr(offset, 2), nullptr, 16)) / 255.f;
        };
        result.push_back({channel(1), channel(3), channel(5)});
    }
    return result;
}
PC::Input honor_input(const Sample& sample) {
    auto input = sample.input();
    input.strategy = PC::Strategy::ColorRegionMajorityV2;
    input.intermediate_palette = honor_palette();
    input.mapping = input.intermediate_palette;
    input.targets = input.intermediate_palette;
    input.final_color_count = input.targets.size();
    input.fixed_role_slots = GUI::honor_people_suite().semantic_role_slot_indices();
    return input;
}

PaintedMesh native_partition_sample()
{
    PaintedMesh painted;
    painted.vertices = {{0,0,0},{1,0,0},{0,1,0},{0,0,1}};
    painted.indices = {{0,2,1},{0,1,3},{0,3,2},{1,2,3}};
    painted.cluster_colors = {{220,160,120},{35,30,25},{235,235,230},{170,45,55}};
    painted.face_colors = {painted.cluster_colors[0], painted.cluster_colors[0],
        painted.cluster_colors[1], painted.cluster_colors[1]};
    return painted;
}

TexturedMesh native_workbench_cluster_sample(size_t color_count)
{
    static constexpr std::array<std::array<size_t, 3>, 6> colors {{
        {{230, 70, 60}}, {{45, 45, 45}}, {{235, 220, 190}},
        {{50, 100, 210}}, {{210, 70, 150}}, {{70, 180, 90}}
    }};
    REQUIRE(color_count > 0);
    REQUIRE(color_count <= colors.size());
    TexturedMesh mesh;
    for (size_t component = 0; component < color_count; ++component) {
        const int base = int(mesh.vertices.size());
        const float offset = float(component) * 3.f;
        mesh.vertices.insert(mesh.vertices.end(), {
            {offset, 0.f, 0.f}, {offset + 1.f, 0.f, 0.f},
            {offset, 1.f, 0.f}, {offset, 0.f, 1.f}
        });
        mesh.indices.insert(mesh.indices.end(), {
            {base, base + 2, base + 1}, {base, base + 1, base + 3},
            {base, base + 3, base + 2}, {base + 1, base + 2, base + 3}
        });
        mesh.precomputed_face_colors.insert(
            mesh.precomputed_face_colors.end(), 4, colors[component]);
    }
    return mesh;
}

PaintMeshRepairCallback identity_mesh_repair()
{
    return [](const indexed_triangle_set& input, indexed_triangle_set& output,
              std::function<void(const char*, unsigned)>,
              std::function<bool()> cancel, std::string*) {
        if (cancel && cancel()) return false;
        output = input;
        return true;
    };
}

NP::BindingOptions native_binding_options()
{
    NP::BindingOptions options;
    options.target_palette = {{.12f,.10f,.08f},{.86f,.62f,.47f},{.94f,.94f,.91f},{.67f,.18f,.22f}};
    options.semantic_slots = {{SC::Label::Hair,0},{SC::Label::Lips,3},{SC::Label::FaceSkin,1}};
    return options;
}

SC::Analysis native_analysis(const NP::NativePortraitPartition& partition,
                             std::vector<SC::Label> labels,
                             std::vector<float> confidence)
{
    SC::Analysis analysis;
    analysis.geometry_id = partition.partition_digest;
    analysis.content_id = partition.partition_digest;
    analysis.signature = std::string(64, 'd');
    analysis.face_labels = std::move(labels);
    analysis.face_confidence = std::move(confidence);
    return analysis;
}
}

TEST_CASE("Semantic evidence cannot change a native portrait partition", "[PortraitNativePartition][P0]")
{
    const auto partition = NP::finalize_native_portrait_partition(native_partition_sample());
    REQUIRE(partition.region_members.size() == 2);
    const auto frozen = partition;
    const auto options = native_binding_options();

    const SC::Analysis none;
    const auto normal = native_analysis(partition,
        {SC::Label::FaceSkin, SC::Label::BodySkin, SC::Label::Hair, SC::Label::Hair},
        {.95f, .95f, .90f, .90f});
    const auto conflict = native_analysis(partition,
        {SC::Label::FaceSkin, SC::Label::Hair, SC::Label::Unknown, SC::Label::Unknown},
        {.95f, .95f, 1.f, 1.f});

    const auto without_semantics = NP::annotate_native_portrait_regions(partition, none, options);
    const auto normal_semantics = NP::annotate_native_portrait_regions(partition, normal, options);
    const auto conflicting_semantics = NP::annotate_native_portrait_regions(partition, conflict, options);

    CHECK(partition.painted.vertices == frozen.painted.vertices);
    CHECK(partition.painted.indices == frozen.painted.indices);
    CHECK(partition.painted.cluster_colors == frozen.painted.cluster_colors);
    CHECK(partition.painted.face_colors == frozen.painted.face_colors);
    CHECK(partition.face_cluster_ids == frozen.face_cluster_ids);
    CHECK(partition.face_region_ids == frozen.face_region_ids);
    CHECK(partition.region_members == frozen.region_members);
    CHECK(partition.region_cluster_ids == frozen.region_cluster_ids);
    CHECK(partition.region_adjacency == frozen.region_adjacency);
    CHECK(partition.region_areas == frozen.region_areas);
    CHECK(partition.boundary_digest == frozen.boundary_digest);
    CHECK(partition.partition_digest == frozen.partition_digest);

    for (const auto* annotated : {&without_semantics, &normal_semantics, &conflicting_semantics}) {
        REQUIRE(annotated->valid_for(partition));
        CHECK(annotated->subface_slots.empty());
        CHECK(annotated->face_slots.size() == partition.painted.indices.size());
        REQUIRE(annotated->regions.size() == partition.region_members.size());
        std::vector<size_t> expected_slots(partition.painted.indices.size());
        for (size_t region = 0; region < partition.region_members.size(); ++region) {
            const auto& members = partition.region_members[region];
            REQUIRE_FALSE(members.empty());
            CHECK(annotated->regions[region].region_id == region);
            for (uint32_t face : members) {
                CHECK(partition.face_region_ids[face] == region);
                expected_slots[face] = annotated->regions[region].slot;
            }
        }
        for (const auto& overlay : annotated->face_overrides)
            expected_slots.at(overlay.face) = overlay.slot;
        CHECK(annotated->face_slots == expected_slots);
    }
    CHECK(normal_semantics.regions[0].semantic == SC::Label::FaceSkin);
    CHECK(normal_semantics.regions[0].semantic_bound);
    CHECK(normal_semantics.regions[0].slot == 1);
    CHECK(normal_semantics.regions[1].semantic == SC::Label::Hair);
    CHECK(normal_semantics.regions[1].semantic_bound);
    CHECK(normal_semantics.regions[1].slot == 0);
    CHECK_FALSE(without_semantics.regions[0].semantic_bound);
    CHECK_FALSE(without_semantics.regions[1].semantic_bound);
    CHECK_FALSE(conflicting_semantics.regions[0].semantic_bound);
    CHECK(conflicting_semantics.regions[0].semantic == SC::Label::Unknown);
    CHECK_FALSE(conflicting_semantics.regions[1].semantic_bound);
    CHECK(without_semantics.protected_components.empty());
    CHECK(without_semantics.face_overrides.empty());
    REQUIRE(normal_semantics.protected_components.size() == 1);
    CHECK(normal_semantics.protected_components.front().semantic == SC::Label::Hair);
    CHECK(normal_semantics.protected_components.front().faces == std::vector<uint32_t>{2,3});
    REQUIRE(conflicting_semantics.protected_components.size() == 1);
    CHECK(conflicting_semantics.protected_components.front().semantic == SC::Label::Hair);
    CHECK(conflicting_semantics.protected_components.front().faces == std::vector<uint32_t>{1});
    REQUIRE(conflicting_semantics.face_overrides.size() == 1);
    CHECK(conflicting_semantics.face_overrides.front().face == 1);
    CHECK(conflicting_semantics.face_slots[1] == options.semantic_slots.at(SC::Label::Hair));
    for (uint32_t face : {0u,2u,3u})
        CHECK(conflicting_semantics.face_slots[face] ==
            conflicting_semantics.regions[partition.face_region_ids[face]].slot);
    CHECK(conflicting_semantics.regions[0].role_support[size_t(SC::Label::FaceSkin)] > 0.);
    CHECK(conflicting_semantics.regions[0].role_support[size_t(SC::Label::Hair)] > 0.);
}

TEST_CASE("Native portrait semantic voting falls back by whole region", "[PortraitNativePartition][P0]")
{
    const auto partition = NP::finalize_native_portrait_partition(native_partition_sample());
    const auto options = native_binding_options();
    const auto low = native_analysis(partition,
        {SC::Label::FaceSkin, SC::Label::FaceSkin, SC::Label::Hair, SC::Label::Hair},
        {.69f, .69f, .69f, .69f});
    const auto low_result = NP::annotate_native_portrait_regions(partition, low, options);
    for (const auto& region : low_result.regions) {
        CHECK_FALSE(region.semantic_bound);
        CHECK(region.semantic == SC::Label::Unknown);
        CHECK(region.confidence < NP::minimum_semantic_confidence);
    }

    const auto aliases = native_analysis(partition,
        {SC::Label::BodySkin, SC::Label::BodySkin, SC::Label::MouthInterior, SC::Label::MouthInterior},
        {.95f, .95f, .95f, .95f});
    const auto alias_result = NP::annotate_native_portrait_regions(partition, aliases, options);
    CHECK(alias_result.regions[0].semantic == SC::Label::FaceSkin);
    CHECK(alias_result.regions[0].slot == 1);
    CHECK(alias_result.regions[1].semantic == SC::Label::Lips);
    CHECK(alias_result.regions[1].slot == 3);
    CHECK(alias_result.subface_slots.empty());
}

TEST_CASE("Native portrait carrier splits only cross-color vertex sharing", "[PortraitNativePartition][P0]")
{
    const auto source = native_partition_sample();
    const auto partition = NP::finalize_native_portrait_partition(source);
    CHECK(partition.painted.indices.size() == source.indices.size());
    CHECK(partition.painted.vertices.size() == source.vertices.size());
    indexed_triangle_set carrier;
    std::vector<RGBA> colors;
    std::string error;
    INFO(error);
    REQUIRE(NP::native_portrait_color_carrier(partition, carrier, colors, error));
    CHECK(carrier.indices.size() == source.indices.size());
    CHECK(carrier.vertices.size() > partition.painted.vertices.size());
    CHECK(colors.size() == carrier.vertices.size());
    for (size_t face = 0; face < carrier.indices.size(); ++face) {
        const auto& expected = partition.painted.face_colors[face];
        for (int vertex : carrier.indices[face]) {
            CHECK_THAT(colors[vertex][0], Catch::Matchers::WithinAbs(float(expected[0]) / 255.f, 1e-7f));
            CHECK_THAT(colors[vertex][1], Catch::Matchers::WithinAbs(float(expected[1]) / 255.f, 1e-7f));
            CHECK_THAT(colors[vertex][2], Catch::Matchers::WithinAbs(float(expected[2]) / 255.f, 1e-7f));
        }
    }

    const auto identity = NP::native_portrait_partition_identity(partition);
    const auto restored = NP::restore_native_portrait_partition(carrier, identity, error);
    INFO(error);
    REQUIRE(restored.valid());
    CHECK(restored.partition_digest == partition.partition_digest);
    CHECK(restored.boundary_digest == partition.boundary_digest);
    CHECK(restored.face_cluster_ids == partition.face_cluster_ids);
    CHECK(restored.face_region_ids == partition.face_region_ids);
    CHECK(restored.region_members == partition.region_members);
}

TEST_CASE("Native carrier bounds preserve the millimeter source frame",
          "[PortraitNativePartition][P0]")
{
    const auto partition = NP::finalize_native_portrait_partition(native_partition_sample());
    const auto source_bounds = NP::native_workbench_bounds(partition);
    REQUIRE(source_bounds.valid());

    NP::NativeMillimeterBounds decoded_bounds;
    REQUIRE(NP::decode_native_workbench_bounds(
        NP::encode_native_workbench_bounds(source_bounds), decoded_bounds));
    CHECK(decoded_bounds.minimum == source_bounds.minimum);
    CHECK(decoded_bounds.maximum == source_bounds.maximum);

    std::string error;
    CHECK(NP::native_workbench_bounds_compatible(source_bounds, decoded_bounds, error));
    NP::NativeMillimeterBounds meter_sized = source_bounds;
    for (double& value : meter_sized.minimum) value /= 1000.0;
    for (double& value : meter_sized.maximum) value /= 1000.0;
    CHECK_FALSE(NP::native_workbench_bounds_compatible(source_bounds, meter_sized, error));
    CHECK_FALSE(error.empty());

    const auto directory = boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("orca-native-carrier-units-%%%%-%%%%");
    struct RemoveDirectory {
        boost::filesystem::path path;
        ~RemoveDirectory() { boost::system::error_code ignored; boost::filesystem::remove_all(path, ignored); }
    } cleanup {directory};
    boost::filesystem::create_directories(directory);
    std::string sha256;
    REQUIRE(NP::write_native_portrait_carrier(
        directory / "partition.obj", partition, source_bounds, sha256, error));
    CHECK(sha256.size() == 64);
}

TEST_CASE("Native portrait partition cache is independent and content addressed", "[PortraitNativePartition][P0]")
{
    auto source = native_partition_sample();
    source.vertices[0][0] = -0.f;
    source.vertices[1][0] = std::nextafter(1.f, 2.f);
    const auto partition = NP::finalize_native_portrait_partition(std::move(source));
    const auto directory = boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("orca-native-partition-cache-%%%%-%%%%");
    struct RemoveDirectory {
        boost::filesystem::path path;
        ~RemoveDirectory() { boost::system::error_code ignored; boost::filesystem::remove_all(path, ignored); }
    } cleanup {directory};
    const auto cache = directory / "cache";
    const auto reference = NP::save_native_portrait_partition_cache(partition, cache);
    REQUIRE(reference.at("schema") == "orca.native-workbench-partition-cache-reference/v1");
    REQUIRE(reference.at("partition_digest") == partition.partition_digest);
    REQUIRE(reference.at("native_mesh_sha256").get<std::string>().size() == 64);

    NP::NativePortraitPartitionIdentity identity;
    std::string error;
    INFO(error);
    TriangleMesh exact_mesh;
    REQUIRE(NP::load_native_portrait_partition_cache(reference, cache, identity, error, &exact_mesh));
    REQUIRE(exact_mesh.its.indices.size() == partition.painted.indices.size());
    for (size_t face = 0; face < exact_mesh.its.indices.size(); ++face)
        for (size_t corner = 0; corner < 3; ++corner)
            CHECK(exact_mesh.its.indices[face][corner] == partition.painted.indices[face][corner]);
    REQUIRE(exact_mesh.its.vertices.size() == partition.painted.vertices.size());
    for (size_t vertex = 0; vertex < exact_mesh.its.vertices.size(); ++vertex)
        CHECK(std::memcmp(exact_mesh.its.vertices[vertex].data(),
            partition.painted.vertices[vertex].data(), 3 * sizeof(float)) == 0);
    CHECK(its_num_open_edges(exact_mesh.its) == 0);
    CHECK(identity.partition_digest == partition.partition_digest);
    CHECK(identity.boundary_digest == partition.boundary_digest);
    CHECK(identity.face_region_ids == partition.face_region_ids);

    const auto auxiliaries = directory / "auxiliaries";
    NP::archive_native_portrait_partitions({reference}, cache, auxiliaries);
    const auto restored_cache = directory / "restored";
    REQUIRE(NP::restore_native_portrait_partition_archive(auxiliaries, restored_cache) == 1);
    TriangleMesh reopened_mesh;
    REQUIRE(NP::load_native_portrait_partition_cache(reference, restored_cache, identity, error, &reopened_mesh));
    CHECK(reopened_mesh.its.indices == exact_mesh.its.indices);
    REQUIRE(reopened_mesh.its.vertices.size() == exact_mesh.its.vertices.size());
    for (size_t vertex = 0; vertex < exact_mesh.its.vertices.size(); ++vertex)
        CHECK(std::memcmp(reopened_mesh.its.vertices[vertex].data(),
            exact_mesh.its.vertices[vertex].data(), 3 * sizeof(float)) == 0);
    CHECK(its_num_open_edges(reopened_mesh.its) == 0);
    CHECK(AI::model_artifact_sha256(restored_cache /
        (reference.at("content_id").get<std::string>() + ".json")) == reference.at("content_id"));

    auto corrupt = reference;
    corrupt["partition_digest"] = std::string(64, 'f');
    CHECK_FALSE(NP::load_native_portrait_partition_cache(corrupt, cache, identity, error));
}

TEST_CASE("Native portrait exact mesh cache rejects changed topology and reads legacy identity only",
          "[PortraitNativePartition][WorkbenchNativeImport][P0]")
{
    const auto partition = NP::finalize_native_portrait_partition(native_partition_sample());
    const auto directory = boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("orca-native-exact-cache-%%%%-%%%%");
    struct RemoveDirectory {
        boost::filesystem::path path;
        ~RemoveDirectory() { boost::system::error_code ignored; boost::filesystem::remove_all(path, ignored); }
    } cleanup {directory};
    auto reference = NP::save_native_portrait_partition_cache(partition, directory);
    const auto file = directory / (reference.at("content_id").get<std::string>() + ".json");
    nlohmann::json document;
    { boost::filesystem::ifstream input(file, std::ios::binary); document = nlohmann::json::parse(input); }
    REQUIRE(document.at("native_topology_mesh").at("schema") == "orca.native-workbench-topology-mesh/v1");
    REQUIRE(document.at("native_topology_mesh").at("coordinate_encoding") == "ieee754-binary32-bits");
    CHECK(document.at("native_mesh_sha256") == reference.at("native_mesh_sha256"));
    bool legacy = false;
    bool resign_mesh = false;
    SECTION("legacy v12 without an embedded mesh stays readable only as an identity") {
        document.erase("native_topology_mesh");
        document.erase("native_mesh_sha256");
        reference.erase("native_mesh_sha256");
        legacy = true;
    }
    SECTION("one changed mesh float cannot retain the old digest") {
        auto& bits = document["native_topology_mesh"]["vertices"][0][0];
        bits = bits.get<uint32_t>() ^ 1u;
    }
    SECTION("reordered original face indices cannot retain the old digest") {
        std::swap(document["native_topology_mesh"]["indices"][0],
            document["native_topology_mesh"]["indices"][1]);
    }
    SECTION("an out of range original corner cannot be restored") {
        document["native_topology_mesh"]["indices"][0][0] = partition.painted.vertices.size();
    }
    SECTION("a mismatching reference mesh digest cannot be restored") {
        reference["native_mesh_sha256"] = std::string(64, 'f');
    }
    SECTION("a mismatching embedded mesh digest cannot be restored") {
        document["native_mesh_sha256"] = std::string(64, 'f');
    }
    SECTION("an out of range corner fails even with a valid replacement mesh digest") {
        document["native_topology_mesh"]["indices"][0][0] = partition.painted.vertices.size();
        resign_mesh = true;
    }
    SECTION("an infinite float fails even with a valid replacement mesh digest") {
        document["native_topology_mesh"]["vertices"][0][0] = uint32_t(0x7f800000u);
        resign_mesh = true;
    }
    SECTION("an open original mesh fails even with a valid replacement mesh digest") {
        auto& mesh = document["native_topology_mesh"];
        const size_t original_corner = mesh["indices"][0][0].get<size_t>();
        const size_t split_corner = mesh["vertices"].size();
        const auto coordinate = mesh["vertices"][original_corner];
        mesh["vertices"].push_back(coordinate);
        mesh["indices"][0][0] = split_corner;
        resign_mesh = true;
    }
    SECTION("a changed closed geometry fails even with a valid replacement mesh digest") {
        document["native_topology_mesh"]["vertices"][1][0] = uint32_t(0x40000000u);
        resign_mesh = true;
    }
    if (resign_mesh) {
        const auto mesh_sha256 = AI::beauty_leaf_digest(document.at("native_topology_mesh").dump());
        document["native_mesh_sha256"] = reference["native_mesh_sha256"] = mesh_sha256;
    }
    // Give changed JSON a valid content address so this exercises the exact
    // mesh contract beyond the outer cache-file checksum.
    const auto changed = directory / "changed.json";
    { boost::filesystem::ofstream output(changed, std::ios::binary); output << document.dump(); }
    const auto content_id = AI::model_artifact_sha256(changed);
    const auto addressed = directory / (content_id + ".json");
    if (boost::filesystem::exists(addressed)) boost::filesystem::remove(changed);
    else boost::filesystem::rename(changed, addressed);
    reference["content_id"] = content_id;
    NP::NativePortraitPartitionIdentity identity;
    TriangleMesh exact_mesh;
    std::string error;
    INFO(error);
    if (legacy)
        REQUIRE(NP::load_native_portrait_partition_cache(reference, directory, identity, error));
    CHECK_FALSE(NP::load_native_portrait_partition_cache(reference, directory, identity, error, &exact_mesh));
    CHECK_FALSE(error.empty());
}

TEST_CASE("Native workbench clustering is independent of requested display colors", "[PortraitNativePartition][P0]")
{
    const auto mesh = native_workbench_cluster_sample(6);
    const auto repair = identity_mesh_repair();
    const auto first = NP::build_native_workbench_partition(mesh, repair);
    const auto second = NP::build_native_workbench_partition(mesh, repair);
    REQUIRE(first.valid());
    REQUIRE(second.valid());
    CHECK(first.painted.cluster_colors.size() >= NP::default_model_color_count);
    CHECK(first.painted.indices.size() <= NP::maximum_face_count);
    CHECK(first.partition_digest == second.partition_digest);
    CHECK(first.boundary_digest == second.boundary_digest);
    CHECK(first.face_cluster_ids == second.face_cluster_ids);
    CHECK(first.region_members == second.region_members);
    for (const size_t count : {size_t(4), size_t(5), size_t(6)}) {
        const auto compatibility = NP::build_native_portrait_partition(mesh, count, repair);
        CHECK(compatibility.partition_digest == first.partition_digest);
        CHECK(compatibility.face_cluster_ids == first.face_cluster_ids);
        CHECK(compatibility.region_members == first.region_members);
    }
}

TEST_CASE("Native workbench clustering permits genuine low color sources", "[PortraitNativePartition][P0]")
{
    const auto partition = NP::build_native_workbench_partition(
        native_workbench_cluster_sample(2), identity_mesh_repair());
    REQUIRE(partition.valid());
    CHECK(partition.painted.cluster_colors.size() == 2);
}

TEST_CASE("Native workbench clustering rejects smoothing below six for a six color source",
          "[PortraitNativePartition][P0]")
{
    TexturedMesh mesh;
    mesh.vertices = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    mesh.indices = {{0,2,4},{2,1,4},{1,3,4},{3,0,4},
        {2,0,5},{1,2,5},{3,1,5},{0,3,5}};
    mesh.precomputed_face_colors = {{230,70,60},{45,45,45},{235,220,190},{50,100,210},
        {210,70,150},{70,180,90},{230,70,60},{45,45,45}};
    CHECK_THROWS_AS(NP::build_native_workbench_partition(
        mesh, identity_mesh_repair()), std::runtime_error);
}

TEST_CASE("Six color requests preserve independent actual color counts", "[PortraitNativePartition][P0]")
{
    for (const size_t actual : {size_t(2), size_t(5), size_t(6)}) {
        DYNAMIC_SECTION("actual colors " << actual) {
            const auto partition = NP::build_native_workbench_partition(
                native_workbench_cluster_sample(actual), identity_mesh_repair());
            const auto reduction = NP::reduce_native_workbench_partition(
                partition, NP::default_model_color_count);
            REQUIRE(reduction.valid_for(partition));
            CHECK(reduction.requested_color_count == NP::default_model_color_count);
            CHECK(reduction.actual_color_count == actual);

            NP::NativeColorReduction restored;
            std::string error;
            const auto identity = NP::native_workbench_partition_identity(partition);
            REQUIRE(NP::decode_native_color_reduction(
                NP::encode_native_color_reduction(reduction), identity, restored, error));
            CHECK(restored.requested_color_count == NP::default_model_color_count);
            CHECK(restored.actual_color_count == actual);
            CHECK(restored.mapping_digest == reduction.mapping_digest);
        }
    }
}

TEST_CASE("V10 native partition records cannot become active V11 identities",
          "[PortraitNativePartition][P0]")
{
    const auto partition = NP::finalize_native_workbench_partition(native_partition_sample());
    const auto identity = NP::native_workbench_partition_identity(partition);
    auto stale_identity = NP::encode_native_portrait_partition_identity(identity);
    stale_identity["schema"] = "orca.native-workbench-partition-identity/v10";
    stale_identity["algorithm"] = 10;
    NP::NativePortraitPartitionIdentity decoded_identity;
    std::string error;
    CHECK_FALSE(NP::decode_native_portrait_partition_identity(
        stale_identity, decoded_identity, error));
    CHECK_FALSE(error.empty());

    const auto reduction = NP::reduce_native_workbench_partition(partition, 2);
    auto stale_reduction = NP::encode_native_color_reduction(reduction);
    stale_reduction["schema"] = "orca.native-workbench-color-reduction/v10";
    stale_reduction["algorithm"] = 10;
    NP::NativeColorReduction decoded_reduction;
    CHECK_FALSE(NP::decode_native_color_reduction(
        stale_reduction, identity, decoded_reduction, error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("Native workbench reductions are deterministic nested cuts", "[PortraitNativePartition][P0]")
{
    PaintedMesh painted;
    painted.vertices = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    painted.indices = {{0,2,4},{2,1,4},{1,3,4},{3,0,4},
        {2,0,5},{1,2,5},{3,1,5},{0,3,5}};
    painted.cluster_colors = {{230,70,60},{45,45,45},{235,220,190},{50,100,210},
        {210,70,150},{70,180,90}};
    const std::array<size_t, 8> slots {{0,1,2,3,4,5,0,1}};
    for (size_t slot : slots) painted.face_colors.push_back(painted.cluster_colors[slot]);
    const auto partition = NP::finalize_native_workbench_partition(std::move(painted));
    REQUIRE(partition.valid());

    const auto six = NP::reduce_native_workbench_partition(partition, 6);
    const auto five = NP::reduce_native_workbench_partition(partition, 5);
    const auto four = NP::reduce_native_workbench_partition(partition, 4);
    REQUIRE(six.valid_for(partition));
    REQUIRE(five.valid_for(partition));
    REQUIRE(four.valid_for(partition));
    CHECK(six.actual_color_count == 6);
    CHECK(five.actual_color_count == 5);
    CHECK(four.actual_color_count == 4);
    for (size_t left = 0; left < partition.region_members.size(); ++left) {
        for (size_t right = left + 1; right < partition.region_members.size(); ++right) {
            if (six.region_color_groups[left] == six.region_color_groups[right])
                CHECK(five.region_color_groups[left] == five.region_color_groups[right]);
            if (five.region_color_groups[left] == five.region_color_groups[right])
                CHECK(four.region_color_groups[left] == four.region_color_groups[right]);
        }
    }
    CHECK(NP::reduce_native_workbench_partition(partition, 5).mapping_digest == five.mapping_digest);
}

TEST_CASE("Semantic evidence and person identities cannot change native color reductions", "[PortraitNativePartition][P0]")
{
    PaintedMesh painted;
    painted.vertices = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    painted.indices = {{0,2,4},{2,1,4},{1,3,4},{3,0,4},
        {2,0,5},{1,2,5},{3,1,5},{0,3,5}};
    painted.cluster_colors = {{255,0,0},{0,0,255},{128,128,128},{130,130,130}};
    const std::array<size_t, 8> slots {{0,1,2,3,2,3,2,3}};
    for (size_t slot : slots) painted.face_colors.push_back(painted.cluster_colors[slot]);
    const auto partition = NP::finalize_native_workbench_partition(std::move(painted));
    NP::BindingOptions options;
    options.target_palette = {{1,0,0},{0,0,1},{.5f,.5f,.5f},{.51f,.51f,.51f},{0,0,0}};
    options.semantic_slots = {{SC::Label::FaceSkin, 0}};
    SC::Analysis analysis;
    analysis.face_labels.assign(partition.painted.indices.size(), SC::Label::Unknown);
    analysis.face_confidence.assign(partition.painted.indices.size(), 1.f);
    for (size_t face = 0; face < partition.face_cluster_ids.size(); ++face)
        if (partition.face_cluster_ids[face] < 2) analysis.face_labels[face] = SC::Label::FaceSkin;
    const auto annotations = NP::annotate_native_portrait_regions(partition, analysis, options);
    const auto single = NP::reduce_native_workbench_partition(partition, 3, &annotations);
    const auto multiple = NP::reduce_native_workbench_partition(partition, 3, &annotations, true);
    const auto without_semantics = NP::reduce_native_workbench_partition(partition, 3);
    REQUIRE(single.valid_for(partition));
    REQUIRE(multiple.valid_for(partition));
    REQUIRE(without_semantics.valid_for(partition));
    CHECK(single.mapping_digest == without_semantics.mapping_digest);
    CHECK(multiple.mapping_digest == without_semantics.mapping_digest);
    CHECK(single.region_color_groups == without_semantics.region_color_groups);
    CHECK(multiple.region_color_groups == without_semantics.region_color_groups);
}

TEST_CASE("Mixed native groups match every cluster instead of only their largest representative",
          "[PortraitNativePartition][PortraitV12][P0]")
{
    // The frozen portrait has a mixed group with almost equal gray and skin
    // areas. Its gray member is slightly larger; choosing only that member
    // incorrectly spends the skin slot on a smaller shadow group.
    PaintedMesh painted;
    painted.cluster_colors = {{46,41,39},{83,69,62},{121,111,106},{122,90,75},
        {155,146,140},{158,81,70},{162,123,105},{196,190,189},{198,160,143}};
    const std::array<double,9> cluster_area {{12304.25124381203,322.208853557606,
        109.876085177195,417.578562125867,2143.385522164862,31.273962677007,
        536.970257681703,5496.534516912711,2106.264908508066}};
    const auto tetrahedron = native_partition_sample();
    const double tetrahedron_area = 1.5 + std::sqrt(3.) / 2.;
    for (size_t cluster = 0; cluster < cluster_area.size(); ++cluster) {
        const int base = int(painted.vertices.size());
        const float scale = float(std::sqrt(cluster_area[cluster] / tetrahedron_area));
        for (const auto& point : tetrahedron.vertices)
            painted.vertices.push_back({point[0]*scale + float(cluster)*1000.f,
                point[1]*scale,point[2]*scale});
        for (const auto& triangle : tetrahedron.indices) {
            painted.indices.push_back({base+triangle[0],base+triangle[1],base+triangle[2]});
            painted.face_colors.push_back(painted.cluster_colors[cluster]);
        }
    }
    const auto partition = NP::finalize_native_workbench_partition(std::move(painted));
    REQUIRE(partition.region_members.size() == 9);
    REQUIRE(partition.region_cluster_ids == std::vector<size_t>{0,1,2,3,4,5,6,7,8});
    const auto identity = NP::native_workbench_partition_identity(partition);
    const auto frozen = NP::encode_native_portrait_partition_identity(identity);
    const auto reduction = NP::reduce_native_workbench_partition(partition, 5);
    REQUIRE(reduction.actual_color_count == 5);
    const auto palette = honor_palette();
    REQUIRE(palette.size() == 5);
    const auto slots = NP::match_reduced_palette_slots(identity, reduction, palette);
    const size_t mixed_group = reduction.region_color_groups[4];
    REQUIRE(reduction.region_color_groups[8] == mixed_group);
    REQUIRE(reduction.group_cluster_ids[mixed_group] == 4);
    CHECK(slots[mixed_group] == 2); // The complete mixed group receives skin.
    CHECK(slots[reduction.region_color_groups[0]] == 0);
    CHECK(slots[reduction.region_color_groups[5]] == 1);
    CHECK(slots[reduction.region_color_groups[7]] == 3);
    CHECK(slots[reduction.region_color_groups[6]] == 4);
    auto used = slots;
    std::sort(used.begin(), used.end());
    CHECK(used == std::vector<size_t>{0,1,2,3,4});
    CHECK(NP::match_reduced_palette_slots(identity, reduction, palette) == slots);

    // An exhaustive five-slot oracle checks the full native group mean cost,
    // independently of the Hungarian implementation and representative colors.
    const auto objective = [&](const std::vector<size_t>& candidate) {
        std::array<double,5> cost {}, area {};
        for (size_t region = 0; region < partition.region_cluster_ids.size(); ++region) {
            const size_t group = reduction.region_color_groups[region];
            const auto& rgb8 = partition.painted.cluster_colors[partition.region_cluster_ids[region]];
            const SC::Color rgb {float(rgb8[0])/255.f,float(rgb8[1])/255.f,float(rgb8[2])/255.f};
            const double weight = partition.region_areas[region];
            cost[group] += weight * GUI::PreviewPalette::distance(
                GUI::PreviewPalette::to_lab(rgb), GUI::PreviewPalette::to_lab(palette[candidate[group]]));
            area[group] += weight;
        }
        double sum = 0.;
        for (size_t group = 0; group < 5; ++group) sum += cost[group] / area[group];
        return sum;
    };
    const double matched_cost = objective(slots);
    std::vector<size_t> permutation {0,1,2,3,4};
    do { CHECK(matched_cost <= objective(permutation) + 1e-8); }
    while (std::next_permutation(permutation.begin(), permutation.end()));

    auto options = native_binding_options();
    options.target_palette = palette;
    options.semantic_slots = {{SC::Label::FaceSkin,0}}; // Deliberately conflicting role slot.
    const auto analysis = native_analysis(partition,
        std::vector<SC::Label>(partition.painted.indices.size(), SC::Label::FaceSkin),
        std::vector<float>(partition.painted.indices.size(), .99f));
    const auto annotations = NP::annotate_native_portrait_regions(partition, analysis, options);
    const auto semantic_reduction = NP::reduce_native_workbench_partition(partition,5,&annotations);
    CHECK(semantic_reduction.mapping_digest == reduction.mapping_digest);
    CHECK(semantic_reduction.region_color_groups == reduction.region_color_groups);
    CHECK(NP::match_reduced_palette_slots(identity,semantic_reduction,palette) == slots);
    CHECK(NP::encode_native_portrait_partition_identity(
        NP::native_workbench_partition_identity(partition)) == frozen);
}

TEST_CASE("Skin evidence spanning nine native clusters retains five independent color groups",
          "[PortraitNativePartition][PortraitV12][P0]")
{
    PaintedMesh painted;
    const auto tetrahedron = native_partition_sample();
    for (size_t cluster = 0; cluster < 9; ++cluster) {
        const int base = int(painted.vertices.size());
        const float offset = float(cluster) * 3.f;
        for (const auto& point : tetrahedron.vertices)
            painted.vertices.push_back({point[0] + offset, point[1], point[2]});
        painted.cluster_colors.push_back({210 + cluster, 155 + cluster, 115 + cluster});
        for (const auto& triangle : tetrahedron.indices) {
            painted.indices.push_back({base + triangle[0], base + triangle[1], base + triangle[2]});
            painted.face_colors.push_back(painted.cluster_colors.back());
        }
    }
    const auto partition = NP::finalize_native_workbench_partition(std::move(painted));
    REQUIRE(partition.valid());
    REQUIRE(partition.region_members.size() == 9);
    const auto frozen = NP::encode_native_portrait_partition_identity(
        NP::native_workbench_partition_identity(partition));
    auto options = native_binding_options();
    options.target_palette.push_back({.1f,.2f,.8f});
    options.person_ids.assign(partition.painted.indices.size(), 17);
    const auto analysis = native_analysis(partition,
        std::vector<SC::Label>(partition.painted.indices.size(), SC::Label::FaceSkin),
        std::vector<float>(partition.painted.indices.size(), .99f));
    const auto annotations = NP::annotate_native_portrait_regions(partition, analysis, options);
    for (const auto& region : annotations.regions) {
        CHECK(region.semantic_bound);
        CHECK(region.semantic == SC::Label::FaceSkin);
    }
    const auto native = NP::reduce_native_workbench_partition(partition, 5);
    const auto annotated = NP::reduce_native_workbench_partition(partition, 5, &annotations);
    const auto multiple = NP::reduce_native_workbench_partition(partition, 5, &annotations, true,
        std::vector<uint32_t>(partition.region_members.size(), 17));
    REQUIRE(native.valid_for(partition));
    CHECK(native.actual_color_count == 5);
    CHECK(annotated.actual_color_count == 5);
    CHECK(multiple.actual_color_count == 5);
    CHECK(annotated.mapping_digest == native.mapping_digest);
    CHECK(multiple.mapping_digest == native.mapping_digest);
    CHECK(annotated.region_color_groups == native.region_color_groups);
    CHECK(multiple.region_color_groups == native.region_color_groups);
    const auto identity = NP::native_workbench_partition_identity(partition);
    const auto slots = NP::match_reduced_palette_slots(identity, native, options.target_palette);
    REQUIRE(slots.size() == 5);
    auto used = slots;
    std::sort(used.begin(), used.end());
    CHECK(used == std::vector<size_t>{0,1,2,3,4});
    CHECK(NP::match_reduced_palette_slots(identity, native, options.target_palette) == slots);
    // Every source center independently prefers the same skin slot. The
    // global assignment must still preserve all five distinct model slots.
    std::vector<SC::Color> target_labs;
    for (const auto& color : options.target_palette)
        target_labs.push_back(GUI::PreviewPalette::to_lab(color));
    for (const auto& color : identity.cluster_colors) {
        const SC::Color rgb {float(color[0])/255.f, float(color[1])/255.f, float(color[2])/255.f};
        CHECK(GUI::PreviewPalette::nearest_lab_index(GUI::PreviewPalette::to_lab(rgb), target_labs) == 1);
    }
    CHECK(NP::encode_native_portrait_partition_identity(
        NP::native_workbench_partition_identity(partition)) == frozen);
}

TEST_CASE("Mixed native regions retain each protected facial component and its person identity",
          "[PortraitNativePartition][PortraitV12][P0]")
{
    // A closed bipyramid has one large native color region. Its top faces
    // provide connected hair/lips and disconnected bilateral brows/eyes.
    PaintedMesh painted;
    painted.vertices = {{0,0,1},{0,0,-1}};
    constexpr size_t ring_size = 24;
    constexpr double two_pi = 6.2831853071795864769;
    for (size_t corner = 0; corner < ring_size; ++corner) {
        const double angle = two_pi * double(corner) / double(ring_size);
        painted.vertices.push_back({float(std::cos(angle)), float(std::sin(angle)), 0.f});
    }
    for (size_t corner = 0; corner < ring_size; ++corner) {
        const int a = int(corner + 2), b = int((corner + 1) % ring_size + 2);
        painted.indices.push_back({0,a,b});
    }
    for (size_t corner = 0; corner < ring_size; ++corner) {
        const int a = int(corner + 2), b = int((corner + 1) % ring_size + 2);
        painted.indices.push_back({1,b,a});
    }
    painted.cluster_colors = {{220,160,120}};
    painted.face_colors.assign(painted.indices.size(), painted.cluster_colors.front());
    const auto partition = NP::finalize_native_workbench_partition(std::move(painted));
    REQUIRE(partition.valid());
    REQUIRE(partition.region_members.size() == 1);
    const auto frozen = NP::encode_native_portrait_partition_identity(
        NP::native_workbench_partition_identity(partition));
    auto options = native_binding_options();
    options.semantic_slots[SC::Label::Eyebrow] = 0;
    options.semantic_slots[SC::Label::EyeSclera] = 2;
    options.semantic_slots[SC::Label::Iris] = 0;
    options.person_ids.assign(partition.painted.indices.size(), 7);
    auto analysis = native_analysis(partition,
        std::vector<SC::Label>(partition.painted.indices.size(), SC::Label::FaceSkin),
        std::vector<float>(partition.painted.indices.size(), .98f));
    for (size_t face = ring_size; face < analysis.face_labels.size(); ++face)
        analysis.face_labels[face] = SC::Label::Unknown;
    const std::map<SC::Label, std::vector<uint32_t>> features {
        {SC::Label::Hair, {0,1}}, {SC::Label::Eyebrow, {4,6}},
        {SC::Label::EyeSclera, {8,10}}, {SC::Label::Iris, {12,14}},
        {SC::Label::Lips, {16,17}}
    };
    for (const auto& [label, faces] : features)
        for (uint32_t face : faces) analysis.face_labels[face] = label;
    analysis.face_labels[20] = SC::Label::Hair;
    analysis.face_confidence[20] = .69f;
    const auto annotations = NP::annotate_native_portrait_regions(partition, analysis, options);
    REQUIRE(annotations.valid_for(partition));
    REQUIRE(annotations.protected_components.size() == 8);
    REQUIRE(annotations.face_overrides.size() == 10);
    CHECK_FALSE(annotations.regions.front().semantic_bound);
    CHECK(annotations.regions.front().semantic == SC::Label::Unknown);
    for (const auto& [label, faces] : features) {
        CHECK(annotations.regions.front().role_support[size_t(label)] > 0.);
        for (uint32_t face : faces) {
            const auto overlay = std::find_if(annotations.face_overrides.begin(), annotations.face_overrides.end(),
                [&](const auto& entry) { return entry.face == face; });
            REQUIRE(overlay != annotations.face_overrides.end());
            const auto& component = annotations.protected_components.at(overlay->component_id);
            CHECK(component.semantic == label);
            CHECK(component.person_id == 7);
            CHECK(overlay->slot == options.semantic_slots.at(label));
            CHECK(annotations.face_slots[face] == options.semantic_slots.at(label));
        }
    }
    const auto component_for = [](const NP::PortraitRegionAnnotations& bound, uint32_t face) {
        return std::find_if(bound.face_overrides.begin(), bound.face_overrides.end(),
            [&](const auto& entry) { return entry.face == face; })->component_id;
    };
    CHECK(component_for(annotations, 0) == component_for(annotations, 1));
    CHECK(component_for(annotations, 4) != component_for(annotations, 6));
    CHECK(component_for(annotations, 8) != component_for(annotations, 10));
    CHECK(component_for(annotations, 12) != component_for(annotations, 14));
    CHECK(component_for(annotations, 16) == component_for(annotations, 17));
    CHECK(annotations.face_slots[20] == annotations.regions.front().slot);
    CHECK(annotations.face_slots[30] == annotations.regions.front().slot);
    CHECK(std::none_of(annotations.face_overrides.begin(), annotations.face_overrides.end(),
        [](const auto& entry) { return entry.face == 20; }));

    auto cross_person = options;
    cross_person.person_ids[1] = 8;
    const auto separated = NP::annotate_native_portrait_regions(partition, analysis, cross_person);
    REQUIRE(separated.valid_for(partition));
    CHECK(separated.protected_components.size() == annotations.protected_components.size() + 1);
    CHECK(component_for(separated, 0) != component_for(separated, 1));
    CHECK(separated.protected_components.at(component_for(separated, 0)).person_id == 7);
    CHECK(separated.protected_components.at(component_for(separated, 1)).person_id == 8);
    CHECK(separated.face_slots == annotations.face_slots);

    // The two brows share one dark slot, but their component identities
    // cannot be swapped merely because their final RGB/slot agrees.
    auto corrupt = annotations;
    const auto left = component_for(corrupt, 4), right = component_for(corrupt, 6);
    REQUIRE(corrupt.protected_components[left].slot == corrupt.protected_components[right].slot);
    const auto wrong = std::find_if(corrupt.face_overrides.begin(), corrupt.face_overrides.end(),
        [](const auto& entry) { return entry.face == 4; });
    REQUIRE(wrong != corrupt.face_overrides.end());
    wrong->component_id = right;
    CHECK_FALSE(corrupt.valid_for(partition));
    CHECK(NP::encode_native_portrait_partition_identity(
        NP::native_workbench_partition_identity(partition)) == frozen);
}

TEST_CASE("Native portrait repair runs once before clustering", "[PortraitNativePartition][P0]")
{
    const auto source = native_partition_sample();
    TexturedMesh mesh;
    mesh.vertices = source.vertices;
    mesh.indices = source.indices;
    mesh.precomputed_face_colors = {{220,160,120},{35,30,25},{235,235,230},{170,45,55}};

    size_t repair_calls = 0;
    const PaintMeshRepairCallback repair = [&](const indexed_triangle_set& input,
                                                indexed_triangle_set& output,
                                                std::function<void(const char*, unsigned)>,
                                                std::function<bool()> cancel,
                                                std::string*) {
        ++repair_calls;
        if (cancel && cancel()) return false;
        output = input;
        output.vertices.front().x() = -0.05f;
        return true;
    };

    const auto partition = NP::build_native_portrait_partition(mesh, 4, repair);
    REQUIRE(partition.valid());
    CHECK(repair_calls == 1);
    CHECK(std::any_of(partition.painted.vertices.begin(), partition.painted.vertices.end(),
        [](const auto& vertex) { return vertex[0] == -0.05f; }));
}

TEST_CASE("Native portrait build reports every stage without contributing to partition identity",
          "[PortraitNativePartition][StrictMeshRepair][P0]")
{
    const auto source = native_partition_sample();
    TexturedMesh mesh;
    mesh.vertices = source.vertices;
    mesh.indices = source.indices;
    mesh.precomputed_face_colors = {{220,160,120},{35,30,25},{235,235,230},{170,45,55}};

    NP::NativePortraitPartitionBuildReport report;
    const auto partition = NP::build_native_portrait_partition(mesh, 4,
        NP::native_mesh_repair_callback(&report.strict_repair), {}, &report);
    REQUIRE(partition.valid());
    REQUIRE(report.success);
    CHECK(report.strict_repair.success);
    const auto encoded = NP::encode_native_portrait_partition_build_report(report);
    REQUIRE(encoded.at("stages").size() == size_t(NP::BuildStage::Count));
    const std::vector<std::string> valid_statuses {"succeeded", "skipped", "failed", "not_run"};
    for (size_t index = 0; index < size_t(NP::BuildStage::Count); ++index) {
        const auto name = NP::native_portrait_build_stage_name(NP::BuildStage(index));
        REQUIRE(encoded.at("stages").contains(name));
        const auto status = encoded.at("stages").at(name).at("status").get<std::string>();
        CHECK(std::find(valid_statuses.begin(), valid_statuses.end(), status) != valid_statuses.end());
    }
    CHECK(encoded.at("stages").at("strict_repair").at("status") == "succeeded");
    CHECK(encoded.at("stages").at("color_clustering").at("status") == "succeeded");
    CHECK(encoded.at("stages").at("region_smoothing").at("status") == "succeeded");

    const auto identity = NP::encode_native_portrait_partition_identity(
        NP::native_portrait_partition_identity(partition));
    auto changed_timing = report;
    changed_timing.total_ms += 123456;
    for (auto& stage : changed_timing.stages) stage.duration_ms += 987;
    CHECK(NP::encode_native_portrait_partition_identity(
        NP::native_portrait_partition_identity(partition)) == identity);
    CHECK(partition.partition_digest == identity.at("partition_digest"));
    CHECK(NP::encode_native_portrait_partition_build_report(changed_timing) != encoded);
}

TEST_CASE("Native portrait repair failure leaves clustering smoothing and publication unstarted",
          "[PortraitNativePartition][StrictMeshRepair][P0]")
{
    const auto source = native_partition_sample();
    TexturedMesh mesh;
    mesh.vertices = source.vertices;
    mesh.indices = source.indices;
    mesh.precomputed_face_colors = {{220,160,120},{35,30,25},{235,235,230},{170,45,55}};
    const PaintMeshRepairCallback fail = [](
        const indexed_triangle_set&, indexed_triangle_set&,
        std::function<void(const char*, unsigned)> progress,
        std::function<bool()>, std::string* error) {
        if (progress) progress("Repairing native portrait mesh with injected failure", 0);
        if (error) *error = "injected strict repair failure";
        return false;
    };
    NP::NativePortraitPartitionBuildReport report;
    CHECK_THROWS(NP::build_native_portrait_partition(mesh, 4, fail, {}, &report));
    CHECK_FALSE(report.success);
    CHECK(report.stage(NP::BuildStage::StrictRepair).status == NP::BuildStageStatus::Failed);
    CHECK(report.stage(NP::BuildStage::ColorClustering).status == NP::BuildStageStatus::NotRun);
    CHECK(report.stage(NP::BuildStage::RegionSmoothing).status == NP::BuildStageStatus::NotRun);
    CHECK(report.stage(NP::BuildStage::RegionEnumeration).status == NP::BuildStageStatus::NotRun);
    CHECK(report.stage(NP::BuildStage::CarrierPublish).status == NP::BuildStageStatus::NotRun);
    CHECK(report.stage(NP::BuildStage::MetadataPublish).status == NP::BuildStageStatus::NotRun);
    CHECK(report.stage(NP::BuildStage::PreviewSwitch).status == NP::BuildStageStatus::NotRun);
}

TEST_CASE("Native portrait partition refuses to run without strict repair",
          "[PortraitNativePartition][StrictMeshRepair][P0]")
{
    const auto source = native_partition_sample();
    TexturedMesh mesh;
    mesh.vertices = source.vertices;
    mesh.indices = source.indices;
    mesh.precomputed_face_colors = {{220,160,120},{35,30,25},{235,235,230},{170,45,55}};
    NP::NativePortraitPartitionBuildReport report;

    CHECK_THROWS_AS(NP::build_native_portrait_partition(mesh, 4, {}, {}, &report),
        std::invalid_argument);
    CHECK_FALSE(report.success);
    CHECK(report.error.find("strict mesh repair callback") != std::string::npos);
    CHECK(report.stage(NP::BuildStage::StrictRepair).status == NP::BuildStageStatus::NotRun);
    CHECK(report.stage(NP::BuildStage::ColorClustering).status == NP::BuildStageStatus::NotRun);
    CHECK(report.stage(NP::BuildStage::RegionSmoothing).status == NP::BuildStageStatus::NotRun);
}

TEST_CASE("Adjacent equal colors form one unresolved region across a fold", "[PortraitColorBlocks]") {
    Sample s;s.source.mesh.vertices[4].z()=2;s.source.mesh.vertices[5].z()=2;
    auto r=PC::build(s.input());
    REQUIRE(r->current().blocks.size()==1);
    REQUIRE(r->current().blocks.front().members.size()==4);
    REQUIRE(r->unresolved_count()==1);
}
TEST_CASE("ColorRegionMajorityV2 partitions by color before semantic voting", "[PortraitColorBlocks]") {
    Sample s;
    s.analysis.face_labels={SC::Label::FaceSkin,SC::Label::Hair,SC::Label::Unknown,SC::Label::Eyebrow};
    auto in=s.input(); in.strategy=PC::Strategy::ColorRegionMajorityV2;
    in.intermediate_palette={{.4f,.4f,.4f},{.8f,.8f,.8f}}; in.final_color_count=4;
    auto r=PC::build(in);
    REQUIRE(r->strategy==PC::Strategy::ColorRegionMajorityV2);
    REQUIRE(r->current().blocks.size()==1);
    REQUIRE(r->final_palette.size()==4);
    REQUIRE_FALSE(r->intermediate_palette.empty());
}
TEST_CASE("ColorRegionMajorityV2 cache round trips role locks and palettes", "[PortraitColorBlocks]") {
    Sample s; auto in=s.input(); in.strategy=PC::Strategy::ColorRegionMajorityV2;
    in.intermediate_palette={{.4f,.4f,.4f}}; in.final_color_count=4;
    auto r=PC::build(in); const auto doc=PC::encode(*r);
    REQUIRE(doc.at("schema")=="orca.portrait-color-blocks/v2");
    std::string error; auto restored=PC::decode(doc,s.source.geometry_id,4,s.source.content_id,s.analysis.signature,r->palette_signature,error);
    REQUIRE(restored); REQUIRE(restored->strategy==PC::Strategy::ColorRegionMajorityV2);
    REQUIRE(restored->final_palette==r->final_palette); REQUIRE(restored->role_locks==r->role_locks);
    REQUIRE(restored->final_slot_indices==r->final_slot_indices);
    REQUIRE(restored->role_slot_locks==r->role_slot_locks);
    REQUIRE(restored->role_regions==r->role_regions);
}
TEST_CASE("Honor portrait V2 binds confirmed anatomy to its shared physical slots", "[PortraitColorBlocks][HonorFilamentSuite]") {
    Sample sample;
    const auto palette = honor_palette();
    sample.analysis.face_labels = {SC::Label::FaceSkin, SC::Label::Hair,
        SC::Label::EyeSclera, SC::Label::MouthInterior};
    sample.analysis.face_confidence.assign(4, .95f);
    sample.source.face_colors = {{palette[2][0], palette[2][1], palette[2][2], 1.f},
        {palette[0][0], palette[0][1], palette[0][2], 1.f},
        {palette[3][0], palette[3][1], palette[3][2], 1.f},
        {palette[1][0], palette[1][1], palette[1][2], 1.f}};

    const auto result = PC::build(honor_input(sample));
    REQUIRE(block(*result, 0).display_slot == 2);
    REQUIRE(block(*result, 1).display_slot == 0);
    REQUIRE(block(*result, 2).display_slot == 3);
    REQUIRE(block(*result, 3).display_slot == 1);
    REQUIRE(PC::reliable_fixed_role_blocks(*result,
        GUI::honor_people_suite().semantic_role_slot_indices()) == 4);
    REQUIRE(PC::used_display_slots(*result) == std::vector<size_t>{0, 1, 2, 3});

    const auto document = PC::encode(*result);
    std::string error;
    const auto restored = PC::decode(document, sample.source.geometry_id, 4,
        sample.source.content_id, sample.analysis.signature, result->palette_signature, error);
    REQUIRE(restored);
    REQUIRE(restored->role_slot_locks == result->role_slot_locks);
    REQUIRE(PC::used_display_slots(*restored) == std::vector<size_t>{0, 1, 2, 3});
}
TEST_CASE("Honor portrait V2 lets hair iris and eyebrows share charcoal", "[PortraitColorBlocks][HonorFilamentSuite]") {
    Sample sample;
    const auto palette = honor_palette();
    sample.analysis.face_labels = {SC::Label::Hair, SC::Label::Iris,
        SC::Label::Eyebrow, SC::Label::EyeSclera};
    sample.analysis.face_confidence.assign(4, .95f);
    sample.source.face_colors = {{.15f, .12f, .10f, 1.f}, {.08f, .06f, .04f, 1.f},
        {.22f, .16f, .12f, 1.f}, {.92f, .92f, .90f, 1.f}};

    const auto result = PC::build(honor_input(sample));
    REQUIRE(block(*result, 0).display_slot == 0);
    REQUIRE(block(*result, 1).display_slot == 0);
    REQUIRE(block(*result, 2).display_slot == 0);
    REQUIRE(block(*result, 3).display_slot == 3);
    REQUIRE(block(*result, 0).display == palette[0]);
    REQUIRE(block(*result, 3).display == palette[3]);
}
TEST_CASE("Honor portrait V2 leaves low confidence and non anatomy regions on nearest colors", "[PortraitColorBlocks][HonorFilamentSuite]") {
    Sample sample;
    const auto palette = honor_palette();
    sample.analysis.face_labels = {SC::Label::FaceSkin, SC::Label::Clothes,
        SC::Label::Accessories, SC::Label::Unknown};
    sample.analysis.face_confidence = {.69f, .95f, .95f, .95f};
    sample.source.face_colors = {{palette[4][0], palette[4][1], palette[4][2], 1.f},
        {palette[0][0], palette[0][1], palette[0][2], 1.f},
        {palette[1][0], palette[1][1], palette[1][2], 1.f},
        {palette[3][0], palette[3][1], palette[3][2], 1.f}};

    const auto result = PC::build(honor_input(sample));
    REQUIRE(block(*result, 0).status == PC::Status::Unresolved);
    REQUIRE(block(*result, 0).display_slot == 4);
    REQUIRE(block(*result, 1).display_slot == 0);
    REQUIRE(block(*result, 2).display_slot == 1);
    REQUIRE(block(*result, 3).display_slot == 3);
    REQUIRE(PC::reliable_fixed_role_blocks(*result,
        GUI::honor_people_suite().semantic_role_slot_indices()) == 0);
}
TEST_CASE("Stale V2 algorithm caches are rejected", "[PortraitColorBlocks]") {
    Sample s; auto in = s.input(); in.strategy = PC::Strategy::ColorRegionMajorityV2;
    in.intermediate_palette = {{.4f, .4f, .4f}}; in.final_color_count = 4;
    const auto result = PC::build(in);
    auto doc = PC::encode(*result);
    doc["algorithm_version"] = uint32_t(5);
    std::string error;
    REQUIRE_FALSE(PC::decode(doc, s.source.geometry_id, 4, s.source.content_id,
        s.analysis.signature, result->palette_signature, error));
    REQUIRE_FALSE(error.empty());
}
TEST_CASE("Version six color layers remain readable without becoming new source partitions", "[PortraitColorBlocks]") {
    Sample s;auto in=honor_input(s);const auto result=PC::build(in);
    auto doc=PC::encode(*result);doc["algorithm_version"]=uint32_t(6);
    std::string error;const auto saved=PC::decode(doc,s.source.geometry_id,4,s.source.content_id,
        s.analysis.signature,result->palette_signature,error);
    REQUIRE(saved);CHECK(saved->source_palette.empty());CHECK(saved->algorithm_version==6);
}

namespace {
NP::NativePortraitPartition frozen_native_partition(Sample& sample, const std::vector<size_t>& slots,
                                                    size_t count = 4) {
    if (slots.size() != sample.source.mesh.indices.size())
        throw std::invalid_argument("Native test slots do not match the mesh.");
    // Native v9 partitions are publishable solids; planar semantic fixtures
    // remain useful elsewhere but must not masquerade as repaired topology.
    sample.source.mesh.vertices = {{0,0,0},{1,0,0},{0,1,0},{0,0,1}};
    sample.source.mesh.indices = {{0,2,1},{0,1,3},{0,3,2},{1,2,3}};
    PaintedMesh painted;
    for (const auto& vertex : sample.source.mesh.vertices)
        painted.vertices.push_back({vertex.x(), vertex.y(), vertex.z()});
    for (const auto& face : sample.source.mesh.indices)
        painted.indices.push_back({face[0], face[1], face[2]});
    painted.cluster_colors = {{133,128,128},{0,0,0},{255,255,255},{255,0,0},{0,255,0},{0,0,255}};
    painted.cluster_colors.resize(count);
    for (const auto slot : slots) painted.face_colors.push_back(painted.cluster_colors.at(slot));
    auto partition = NP::finalize_native_portrait_partition(std::move(painted));
    sample.source.geometry_id = partition.geometry_id;
    sample.analysis.geometry_id = partition.geometry_id;
    return partition;
}

std::vector<uint32_t> beauty_piece_ids(const NP::NativePortraitPartition& partition)
{
    auto result = partition.face_region_ids;
    for (uint32_t& region : result) ++region; // BeautyPuzzle reserves zero as empty.
    return result;
}
}

TEST_CASE("Native fallback components retain their own slots across palette synchronization",
          "[PortraitNativePartition][PortraitColorBlocks][PortraitV12]")
{
    Sample sample;
    sample.analysis.face_labels = {SC::Label::Hair, SC::Label::Hair,
        SC::Label::Unknown, SC::Label::Unknown};
    const auto partition = frozen_native_partition(sample, {0,1,2,3});
    auto input = honor_input(sample); input.native_partition = &partition;
    input.fixed_role_slots.clear();
    input.person_ids = {7,8,0,0};
    const auto result = PC::build(input);
    REQUIRE(result->protected_components.size() == 2);
    const auto first_slot = result->protected_components[0].slot;
    const auto second_slot = result->protected_components[1].slot;
    REQUIRE(first_slot != second_slot);
    CHECK(result->role_slot_locks.empty());
    CHECK(result->role_locks.empty());
    SC::FaceColors before; SC::SubfaceColors before_children;
    result->appearance(before, before_children);
    REQUIRE(PC::remap_to_palette(*result, result->final_palette, true, result->role_slot_locks));
    CHECK(result->protected_components[0].slot == first_slot);
    CHECK(result->protected_components[1].slot == second_slot);
    SC::FaceColors after; SC::SubfaceColors after_children;
    result->appearance(after, after_children);
    CHECK(after == before);
}

TEST_CASE("Native role overlays retain their aliased slot locks and cache identities",
          "[PortraitNativePartition][PortraitColorBlocks][PortraitV12][HonorFilamentSuite]")
{
    Sample sample;
    sample.analysis.face_labels = {SC::Label::Hair, SC::Label::Eyebrow,
        SC::Label::EyeSclera, SC::Label::Lips};
    const auto partition = frozen_native_partition(sample, {0,0,0,0});
    auto input = honor_input(sample); input.native_partition = &partition;
    const auto result = PC::build(input);
    REQUIRE(result->protected_components.size() == 4);
    REQUIRE(result->face_overrides.size() == 4);
    const auto expected_roles = result->role_slot_locks;
    REQUIRE(expected_roles.count("Eyebrow") == 1);
    REQUIRE(expected_roles.count("Hair") == 1);
    REQUIRE(expected_roles.count("Lips") == 1);
    REQUIRE(expected_roles.count("EyeSclera") == 1);
    auto card = result->final_palette;
    for (auto& color : card) for (auto& channel : color) channel = std::min(1.f, channel + .025f);
    // The production project-color callbacks pass this member as the const
    // input argument. Clearing output locks must not erase that same input.
    REQUIRE(PC::remap_to_palette(*result, card, true, result->role_slot_locks));
    CHECK(result->role_slot_locks == expected_roles);
    SC::FaceColors faces; SC::SubfaceColors children;
    result->appearance(faces, children);
    REQUIRE(faces.size() == 4);
    CHECK(children.empty());
    CHECK(faces[0].second == card[expected_roles.at("Hair")]);
    CHECK(faces[1].second == card[expected_roles.at("Eyebrow")]);
    CHECK(faces[2].second == card[expected_roles.at("EyeSclera")]);
    CHECK(faces[3].second == card[expected_roles.at("Lips")]);
    CHECK(PC::used_display_slots(*result) == std::vector<size_t>{0,1,3});

    const auto document = PC::encode(*result);
    const auto directory = boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("orca-v12-overlays-%%%%-%%%%");
    struct Remove {
        boost::filesystem::path path;
        ~Remove() { boost::system::error_code ec; boost::filesystem::remove_all(path, ec); }
    } cleanup {directory};
    const auto reference = PC::save(*result, directory);
    std::string error;
    const auto restored = PC::load(reference, directory, sample.source.geometry_id, 4,
        sample.source.content_id, error);
    INFO(error);
    REQUIRE(restored);
    CHECK(PC::encode(*restored) == document);
    SC::FaceColors reopened; SC::SubfaceColors reopened_children;
    restored->appearance(reopened, reopened_children);
    CHECK(reopened == faces);
    CHECK(reopened_children.empty());
    CHECK(PC::used_display_slots(*restored) == PC::used_display_slots(*result));
    CHECK(restored->semantic_annotations.front().role_support == result->semantic_annotations.front().role_support);

    auto wrong_count = document;
    wrong_count["final_slot_binding"]["component_count"] = result->protected_components.size() + 1;
    CHECK_FALSE(PC::decode(wrong_count, sample.source.geometry_id, 4, sample.source.content_id,
        sample.analysis.signature, result->palette_signature, error));
    auto wrong_owner = document;
    REQUIRE(document["final_slot_binding"]["face_overrides"][0][2] ==
        document["final_slot_binding"]["face_overrides"][1][2]);
    wrong_owner["final_slot_binding"]["face_overrides"][0][1] =
        document["final_slot_binding"]["face_overrides"][1][1];
    CHECK_FALSE(PC::decode(wrong_owner, sample.source.geometry_id, 4, sample.source.content_id,
        sample.analysis.signature, result->palette_signature, error));
    auto wrong_slot = document;
    wrong_slot["final_slot_binding"]["face_overrides"][0][2] = size_t(2);
    CHECK_FALSE(PC::decode(wrong_slot, sample.source.geometry_id, 4, sample.source.content_id,
        sample.analysis.signature, result->palette_signature, error));

    auto wrong_component_slot = document;
    const auto hair_component = wrong_component_slot["final_slot_binding"]["face_overrides"][0][1].get<size_t>();
    wrong_component_slot["region_semantics"]["protected_components"][hair_component]["slot"] = size_t(2);
    for (auto& overlay : wrong_component_slot["final_slot_binding"]["face_overrides"])
        if (overlay[1].get<size_t>() == hair_component) overlay[2] = size_t(2);
    CHECK_FALSE(PC::decode(wrong_component_slot, sample.source.geometry_id, 4, sample.source.content_id,
        sample.analysis.signature, result->palette_signature, error));

    const SC::Color manual {.21f,.31f,.41f};
    REQUIRE(result->recolor(0, manual));
    REQUIRE(PC::remap_to_palette(*result, result->final_palette, true, result->role_slot_locks));
    result->appearance(faces, children);
    for (const auto& face : faces) CHECK(face.second == manual);
    // Every face is now explicitly manual; hidden role/base slots must not
    // be counted as though they remained visible on the model.
    CHECK(PC::used_display_slots(*result).empty());
}

TEST_CASE("Native cache restoration enforces both automatic track bindings",
          "[PortraitNativePartition][PortraitColorBlocks][PortraitV12]")
{
    Sample sample;
    const auto partition = frozen_native_partition(sample, {0,1,2,3});
    auto input = honor_input(sample); input.native_partition = &partition;
    const auto result = PC::build(input);
    const auto document = PC::encode(*result);
    const size_t changed_region = partition.face_region_ids[0];
    const size_t wrong_slot = (result->final_region_slots[changed_region] + 1) % result->final_palette.size();
    std::string error;
    for (size_t track = 0; track < 2; ++track) {
        INFO(track);
        auto inconsistent = document;
        auto& block = inconsistent["tracks"][track]["blocks"][changed_region];
        block["display_slot"] = wrong_slot;
        block["display"] = result->final_palette[wrong_slot];
        // Range and RGB agree with each other, but the automatic base binding
        // must still agree with the immutable final_slot_binding in both tracks.
        CHECK_FALSE(PC::decode(inconsistent, sample.source.geometry_id, 4, sample.source.content_id,
            sample.analysis.signature, result->palette_signature, error));
    }
}

TEST_CASE("Native cache restoration preserves manual colors independently on both tracks",
          "[PortraitNativePartition][PortraitColorBlocks][PortraitV12]")
{
    Sample sample;
    sample.analysis.face_labels = {SC::Label::Hair, SC::Label::Unknown,
        SC::Label::Unknown, SC::Label::Unknown};
    const auto partition = frozen_native_partition(sample, {0,1,2,3});
    auto input = honor_input(sample); input.native_partition = &partition;
    const auto result = PC::build(input);
    const SC::Color manual {.21f,.31f,.41f};
    std::string error;
    for (const auto track : {PC::Track::SourceAppearance, PC::Track::ProcessedFaceColor}) {
        INFO(size_t(track));
        auto edited = *result;
        edited.active = track;
        REQUIRE(edited.recolor(partition.face_region_ids[0], manual));
        const auto document = PC::encode(edited);
        const auto restored = PC::decode(document, sample.source.geometry_id, 4, sample.source.content_id,
            sample.analysis.signature, result->palette_signature, error);
        INFO(error);
        REQUIRE(restored);
        CHECK(PC::encode(*restored) == document);
        CHECK(restored->active == track);
        CHECK(restored->final_region_slots == result->final_region_slots);
        CHECK(restored->current().blocks[partition.face_region_ids[0]].status == PC::Status::ManualConfirmed);
        CHECK(restored->current().blocks[partition.face_region_ids[0]].display_slot == -1);
        SC::FaceColors faces; SC::SubfaceColors children;
        restored->appearance(faces, children);
        REQUIRE(faces.size() == 4);
        CHECK(faces[0].second == manual);
        CHECK(children.empty());
    }
}

TEST_CASE("Native portrait caches reject missing source identity fields",
          "[PortraitNativePartition][PortraitColorBlocks][PortraitV12]")
{
    Sample sample;
    const auto partition = frozen_native_partition(sample, {0,0,0,0});
    auto input = honor_input(sample); input.native_partition = &partition;
    const auto result = PC::build(input);
    const auto document = PC::encode(*result);
    std::string error;
    for (const char* key : {"source_palette", "source_basis_signature", "unit_source_slots"}) {
        INFO(key);
        auto missing = document;
        missing.erase(key);
        CHECK_FALSE(PC::decode(missing, sample.source.geometry_id, 4, sample.source.content_id,
            sample.analysis.signature, result->palette_signature, error));
    }
    auto empty_palette = document;
    empty_palette["source_palette"] = nlohmann::json::array();
    CHECK_FALSE(PC::decode(empty_palette, sample.source.geometry_id, 4, sample.source.content_id,
        sample.analysis.signature, result->palette_signature, error));
}

// This opt-in test uses immutable local inputs and writes only a fresh JSON
// evidence file. It rebuilds the native geometry from its original source;
// relabelling a v11 cache as v12 is deliberately not a supported replay path.
TEST_CASE("Frozen portrait evidence retains native atoms and protected roles after a genuine rebuild",
          "[.PortraitV12Replay][PortraitNativePartition][PortraitV12]")
{
    const auto env = [](const char* key) {
        const auto* value = boost::nowide::getenv(key);
        return value ? std::string(value) : std::string{};
    };
    const auto blocks_path = env("ORCA_PORTRAIT_V12_BLOCKS");
    const auto semantics_path = env("ORCA_PORTRAIT_V12_SEMANTICS");
    const auto carrier_path = env("ORCA_PORTRAIT_V12_CARRIER");
    const auto report_path = env("ORCA_PORTRAIT_V12_REPORT");
    auto original_path = env("ORCA_PORTRAIT_V12_ORIGINAL");
    if (blocks_path.empty() || semantics_path.empty() || carrier_path.empty() || report_path.empty())
        SKIP("Set ORCA_PORTRAIT_V12_{BLOCKS,SEMANTICS,CARRIER,REPORT}; ORIGINAL optionally overrides the carrier metadata source.");
    REQUIRE_FALSE(boost::filesystem::exists(report_path));
    const auto read = [](const boost::filesystem::path& path) {
        boost::filesystem::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("Frozen replay input is missing.");
        return nlohmann::json::parse(input);
    };
    const auto old = read(blocks_path);
    const auto semantic_document = read(semantics_path);
    const auto& old_identity = old.at("native_partition_identity");
    REQUIRE(old_identity.at("algorithm") == 11);
    REQUIRE(old_identity.at("face_count") == 986276);
    REQUIRE(old_identity.at("region_cluster_ids").size() == 789);
    REQUIRE(old_identity.at("cluster_colors").size() == 9);
    auto input_metadata_path = boost::filesystem::path(carrier_path);
    input_metadata_path.replace_extension(".json");
    const auto input_metadata = read(input_metadata_path);
    const auto& input_reference = input_metadata.at("native_portrait_partition");
    REQUIRE(input_metadata.at("schema") == "orca.native-workbench-partition-commit/v1");
    REQUIRE(input_metadata.at("transaction_status") == "committed");
    if (original_path.empty()) original_path = input_reference.at("source_path").get<std::string>();
    const auto original_hash = AI::model_artifact_sha256(original_path);
    const auto carrier_hash = AI::model_artifact_sha256(carrier_path);
    const auto blocks_hash = AI::model_artifact_sha256(blocks_path);
    const auto semantics_hash = AI::model_artifact_sha256(semantics_path);
    REQUIRE(original_hash.size() == 64);
    REQUIRE(carrier_hash.size() == 64);
    REQUIRE(original_hash == input_reference.at("source_sha256"));
    REQUIRE(carrier_hash == input_reference.at("carrier_sha256"));
    REQUIRE(input_reference.at("appearance_id") == semantic_document.at("content_id"));
    GUI::DecodedWorkbenchModel decoded;
    std::string error;
    REQUIRE(GUI::decode_workbench_model(original_path, decoded, error));
    INFO(error);
    REQUIRE(decoded.texture);
    REQUIRE(GUI::normalize_decoded_workbench_model_to_millimeters(decoded, error));
    NP::NativePortraitPartitionBuildReport build_report;
    const auto native = NP::build_native_workbench_partition(*decoded.texture,
        NP::native_mesh_repair_callback(&build_report.strict_repair), {}, &build_report);
    REQUIRE(build_report.success);
    REQUIRE(native.valid());
    // Immutable face/corner geometry and every old atomic membership must
    // remain traceable; only the versioned digest contract is new.
    REQUIRE(native.geometry_id == old_identity.at("geometry_id"));
    REQUIRE(native.painted.indices.size() == old_identity.at("face_count").get<size_t>());
    REQUIRE(native.face_region_ids == old_identity.at("face_region_ids").get<std::vector<uint32_t>>());
    REQUIRE(native.face_cluster_ids == old_identity.at("face_cluster_ids").get<std::vector<size_t>>());
    REQUIRE(native.region_cluster_ids == old_identity.at("region_cluster_ids").get<std::vector<size_t>>());
    REQUIRE(native.painted.cluster_colors == old_identity.at("cluster_colors").get<std::vector<std::array<size_t,3>>>());
    REQUIRE(native.region_members.size() == 789);
    CHECK(native.partition_digest != old_identity.at("partition_digest"));
    TriangleMesh carrier; ObjInfo carrier_colors;
    REQUIRE(AI::load_model_artifact(carrier_path, carrier, carrier_colors, error));
    REQUIRE(AI::SurfaceSelectionPersistence::geometry_fingerprint(carrier.its) == native.geometry_id);

    SC::MeshSnapshot source;
    source.geometry_id = native.geometry_id;
    source.content_id = old.at("source_signature").get<std::string>();
    REQUIRE(source.content_id == semantic_document.at("content_id"));
    source.sampling_signature = "original-surface/v1:" + original_hash;
    for (const auto& vertex : native.painted.vertices)
        source.mesh.vertices.emplace_back(vertex[0], vertex[1], vertex[2]);
    for (const auto& face : native.painted.indices)
        source.mesh.indices.emplace_back(face[0], face[1], face[2]);
    for (const auto& color : native.painted.face_colors)
        source.face_colors.push_back({float(color[0])/255.f,float(color[1])/255.f,float(color[2])/255.f,1.f});
    SC::Analysis analysis;
    const bool semantic_decoded = SC::decode_analysis(semantic_document, source,
        semantic_document.at("body_identity"), semantic_document.at("face_identity"), analysis, error);
    INFO(error);
    REQUIRE(semantic_decoded);
    PC::Input input {source, analysis};
    input.strategy = PC::Strategy::ColorRegionMajorityV2;
    input.native_partition = &native;
    input.targets = old.at("final_palette").get<std::vector<SC::Color>>();
    input.mapping = input.targets;
    input.final_color_count = input.targets.size();
    input.fixed_role_slots = GUI::honor_people_suite().semantic_role_slot_indices();
    const auto result = PC::build(input);
    REQUIRE(result->native_color_reduction);
    REQUIRE(result->native_color_reduction->actual_color_count == 5);
    REQUIRE(result->current().blocks.size() == 789);
    const auto visible_slots = PC::used_display_slots(*result);
    CHECK(visible_slots == std::vector<size_t>{0,1,2,3,4});
    SC::FaceColors appearance; SC::SubfaceColors children;
    result->appearance(appearance, children);
    REQUIRE(appearance.size() == native.painted.indices.size());
    CHECK(children.empty());
    std::vector<size_t> final_slots(source.mesh.indices.size());
    for (size_t face = 0; face < final_slots.size(); ++face)
        final_slots[face] = result->final_region_slots[native.face_region_ids[face]];
    std::vector<uint8_t> covered(final_slots.size(), 0);
    for (const auto& overlay : result->face_overrides) {
        final_slots.at(overlay.face) = overlay.slot;
        covered.at(overlay.face) = 1;
    }
    std::map<int, size_t> reliable_faces, protected_faces, component_counts;
    std::map<int, std::map<size_t, size_t>> reliable_role_slots;
    struct BrowProbe { size_t faces; Vec3d center; };
    std::vector<BrowProbe> brows;
    Vec3d brow_normal = Vec3d::Zero();
    nlohmann::json component_report = nlohmann::json::array();
    for (const auto& component : result->protected_components) {
        ++component_counts[int(component.semantic)];
        protected_faces[int(component.semantic)] += component.faces.size();
        component_report.push_back({{"id",component.id},{"semantic",int(component.semantic)},
            {"person_id",component.person_id},{"face_count",component.faces.size()},{"slot",component.slot}});
        if (component.semantic == SC::Label::Eyebrow) {
            Vec3d center = Vec3d::Zero();
            for (const auto face : component.faces) {
                const auto& triangle = source.mesh.indices[face];
                const Vec3d a = source.mesh.vertices[triangle[0]].cast<double>();
                const Vec3d b = source.mesh.vertices[triangle[1]].cast<double>();
                const Vec3d c = source.mesh.vertices[triangle[2]].cast<double>();
                center += (a + b + c) / 3.;
                brow_normal += (b - a).cross(c - a);
            }
            brows.push_back({component.faces.size(), center / double(component.faces.size())});
        }
    }
    for (size_t face = 0; face < final_slots.size(); ++face) {
        auto role = analysis.face_labels[face];
        if (role == SC::Label::MouthInterior) role = SC::Label::Lips;
        if (analysis.face_confidence[face] >= NP::minimum_semantic_confidence)
            ++reliable_role_slots[int(role)][final_slots[face]];
        const bool protected_role = role == SC::Label::Hair || role == SC::Label::Eyebrow ||
            role == SC::Label::EyeSclera || role == SC::Label::Iris || role == SC::Label::Lips;
        if (protected_role && analysis.face_confidence[face] >= NP::minimum_semantic_confidence) {
            ++reliable_faces[int(role)];
            CHECK(covered[face] == 1);
        } else if (role == SC::Label::Unknown) {
            CHECK(covered[face] == 0);
            CHECK(final_slots[face] == result->final_region_slots[native.face_region_ids[face]]);
        }
        CHECK(appearance[face].second == result->final_palette[final_slots[face]]);
    }
    for (const auto role : {SC::Label::Hair,SC::Label::Eyebrow,SC::Label::EyeSclera,SC::Label::Iris,SC::Label::Lips}) {
        CHECK(reliable_faces[int(role)] > 0);
        CHECK(protected_faces[int(role)] == reliable_faces[int(role)]);
    }
    CHECK(component_counts[int(SC::Label::Eyebrow)] >= 2);
    CHECK(component_counts[int(SC::Label::EyeSclera)] >= 2);
    CHECK(component_counts[int(SC::Label::Iris)] >= 2);
    // This frozen portrait faces world +X, so x=0 cannot distinguish its brows.
    // Compare its two dominant evidence components along a face-local side axis.
    REQUIRE(brows.size() >= 2);
    std::sort(brows.begin(), brows.end(), [](const auto& a, const auto& b) { return a.faces > b.faces; });
    const Vec3d side_vector = brow_normal.cross(Vec3d::UnitZ());
    REQUIRE(side_vector.norm() > 1e-8);
    const Vec3d side_axis = side_vector.normalized();
    if (brows[0].center.dot(side_axis) > brows[1].center.dot(side_axis)) std::swap(brows[0], brows[1]);
    const size_t left_brow = brows[0].faces, right_brow = brows[1].faces;
    CHECK(left_brow > 400);
    CHECK(right_brow > 400);
    CHECK((brows[1].center - brows[0].center).dot(side_axis) > 5.);
    CHECK(reliable_role_slots[int(SC::Label::FaceSkin)][2] > reliable_role_slots[int(SC::Label::FaceSkin)][4]);
    CHECK(reliable_role_slots[int(SC::Label::BodySkin)][2] > reliable_role_slots[int(SC::Label::BodySkin)][4]);
    nlohmann::json report {
        {"scope","offline original native rebuild and frozen face-evidence replay; no GUI, user visual or print acceptance"},
        {"original_path",original_path},{"original_sha256",original_hash},
        {"carrier_path",carrier_path},{"carrier_sha256",carrier_hash},
        {"blocks_sha256",blocks_hash},{"semantics_sha256",semantics_hash},
        {"algorithm",NP::algorithm_version},{"geometry_id",native.geometry_id},
        {"face_count",final_slots.size()},{"atomic_region_count",native.region_members.size()},
        {"atomic_membership_matches_v11",true},{"partition_digest",native.partition_digest},
        {"actual_color_count",result->native_color_reduction->actual_color_count},
        {"visible_slots",visible_slots},{"palette",result->final_palette},{"face_slots",std::move(final_slots)},
        {"protected_components",std::move(component_report)},
        {"reliable_faces_by_role",reliable_faces},{"protected_faces_by_role",protected_faces},
        {"reliable_role_slot_counts",reliable_role_slots},
        {"left_eyebrow_faces",left_brow},{"right_eyebrow_faces",right_brow},
        {"eyebrow_side_axis",{side_axis.x(),side_axis.y(),side_axis.z()}},
        {"dominant_eyebrow_centroids",{{brows[0].center.x(),brows[0].center.y(),brows[0].center.z()},
            {brows[1].center.x(),brows[1].center.y(),brows[1].center.z()}}}
    };
    // Preserve the genuine rebuilt result in a separate evidence tree. The
    // live user library, old cache and running application remain untouched.
    const auto report_file = boost::filesystem::path(report_path);
    const auto artifact_root = report_file.parent_path() / (report_file.stem().string() + "-artifacts");
    REQUIRE_FALSE(boost::filesystem::exists(artifact_root));
    REQUIRE(boost::filesystem::create_directories(artifact_root));
    const auto text_path = [](const boost::filesystem::path& path) {
        return boost::nowide::narrow(path.wstring());
    };
    const auto write_json = [&](const boost::filesystem::path& path, const nlohmann::json& document) {
        REQUIRE_FALSE(boost::filesystem::exists(path));
        boost::filesystem::ofstream stream(path, std::ios::binary);
        stream << document.dump(); stream.close();
        REQUIRE(stream.good());
        CHECK(read(path) == document);
    };
    const auto result_document = PC::encode(*result);
    const auto result_path = artifact_root / "portrait-color-result.json";
    write_json(result_path, result_document);
    const auto applied_layer = PC::publish_verified_color_layer(*result, artifact_root / "portrait-color-blocks");
    const auto applied_path = artifact_root / "portrait-applied-color-layer.json";
    write_json(applied_path, applied_layer.encode());
    REQUIRE(applied_layer.painted_faces == native.painted.indices.size());
    REQUIRE(applied_layer.painted_subfaces == 0);

    const auto identity_document = NP::encode_native_portrait_partition_identity(
        NP::native_workbench_partition_identity(native));
    const auto identity_path = artifact_root / "native-partition-identity.json";
    write_json(identity_path, identity_document);
    const auto cache_root = artifact_root / "native-workbench-partitions";
    const auto cache_reference = NP::save_native_portrait_partition_cache(native, cache_root);
    const auto cache_sha256 = cache_reference.at("content_id").get<std::string>();
    const auto cache_path = cache_root / (cache_sha256 + ".json");
    NP::NativePortraitPartitionIdentity restored_identity;
    TriangleMesh restored_exact_mesh;
    REQUIRE(NP::load_native_portrait_partition_cache(
        cache_reference, cache_root, restored_identity, error, &restored_exact_mesh));
    REQUIRE(restored_exact_mesh.its.indices.size() == native.painted.indices.size());
    for (size_t face = 0; face < native.painted.indices.size(); ++face)
        for (size_t corner = 0; corner < 3; ++corner)
            REQUIRE(restored_exact_mesh.its.indices[face][corner] == native.painted.indices[face][corner]);
    REQUIRE(restored_exact_mesh.its.vertices.size() == native.painted.vertices.size());
    for (size_t vertex = 0; vertex < native.painted.vertices.size(); ++vertex)
        REQUIRE(std::memcmp(restored_exact_mesh.its.vertices[vertex].data(),
            native.painted.vertices[vertex].data(), 3 * sizeof(float)) == 0);
    CHECK(its_num_open_edges(restored_exact_mesh.its) == 0);
    CHECK(NP::encode_native_portrait_partition_identity(restored_identity) == identity_document);

    const auto source_bounds = NP::native_workbench_bounds(*decoded.texture);
    const auto carrier_bounds = NP::native_workbench_bounds(native);
    REQUIRE(NP::native_workbench_bounds_compatible(source_bounds, carrier_bounds, error));
    const auto new_carrier = artifact_root / "native-workbench-partition-v12.obj";
    std::string new_carrier_hash;
    const auto publish_started = std::chrono::steady_clock::now();
    REQUIRE(NP::write_native_portrait_carrier(new_carrier, native, source_bounds, new_carrier_hash, error));
    REQUIRE(AI::model_artifact_sha256(new_carrier) == new_carrier_hash);
    // Same immutable face/corner sequence and cluster colors must reproduce
    // the original 58 MB carrier bytes; the new evidence has new v12 digests.
    CHECK(new_carrier_hash == carrier_hash);
    auto& carrier_stage = build_report.stage(NP::BuildStage::CarrierPublish);
    carrier_stage.status = NP::BuildStageStatus::Succeeded;
    carrier_stage.duration_ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - publish_started).count());
    carrier_stage.detail = "Offline rebuilt native carrier written and SHA-256 verified.";
    auto& cache_stage = build_report.stage(NP::BuildStage::CachePublish);
    cache_stage.status = NP::BuildStageStatus::Succeeded;
    cache_stage.detail = "Offline v12 identity published and exactly decoded from its addressed cache.";

    const auto appearance_source = boost::filesystem::path(input_reference.at("appearance_path").get<std::string>());
    const auto appearance_hash = input_reference.at("appearance_sha256").get<std::string>();
    REQUIRE(AI::model_artifact_sha256(appearance_source) == appearance_hash);
    REQUIRE(input_metadata.at("original_appearance").at("carrier_sha256") == appearance_hash);
    TriangleMesh appearance_mesh; ObjInfo appearance_colors;
    REQUIRE(AI::load_model_artifact(appearance_source, appearance_mesh, appearance_colors, error));
    REQUIRE(AI::SurfaceSelectionPersistence::geometry_fingerprint(appearance_mesh.its) == native.geometry_id);
    const auto appearance_topology = AI::SurfaceSelectionPersistence::topology_fingerprint(appearance_mesh.its);
    REQUIRE(input_metadata.at("original_appearance").at("topology_sha256") == appearance_topology);
    const auto appearance_path = artifact_root / "native-workbench-partition-v12.source-appearance.obj";
    boost::filesystem::copy_file(appearance_source, appearance_path);
    REQUIRE(AI::model_artifact_sha256(appearance_path) == appearance_hash);
    const auto original_copy = artifact_root / ("immutable-original" + boost::filesystem::path(original_path).extension().string());
    boost::filesystem::copy_file(original_path, original_copy);
    REQUIRE(AI::model_artifact_sha256(original_copy) == original_hash);
    const GUI::OriginalAppearanceReference original_appearance {
        appearance_path, appearance_hash, appearance_topology, original_copy, original_hash};
    REQUIRE(original_appearance.valid());
    auto& appearance_stage = build_report.stage(NP::BuildStage::AppearancePublish);
    appearance_stage.status = NP::BuildStageStatus::Succeeded;
    appearance_stage.detail = "Frozen original appearance SHA, ordered geometry and topology verified before copying.";
    REQUIRE(build_report.strict_repair.success);
    REQUIRE(build_report.strict_repair.geometry_verified);
    REQUIRE(build_report.strict_repair.output_topology.safe());
    const auto build_document = NP::encode_native_portrait_partition_build_report(build_report);
    const auto build_path = artifact_root / "native-partition-build-report.json";
    write_json(build_path, build_document);
    const auto& strict = build_document.at("strict_repair");
    const nlohmann::json topology_certificate {
        {"schema","orca.native-workbench-topology-certificate/v1"},
        {"geometry_id",native.geometry_id},{"carrier_sha256",new_carrier_hash},
        {"native_mesh_sha256",cache_reference.at("native_mesh_sha256")},
        {"partition_digest",native.partition_digest},{"coordinate_units","mm"},
        {"source_bounds_mm",NP::encode_native_workbench_bounds(source_bounds)},
        {"carrier_bounds_mm",NP::encode_native_workbench_bounds(carrier_bounds)},
        {"topology",strict.at("output_topology")},
        {"sampled_deviation_ratio",strict.at("sampled_deviation_ratio")},
        {"area_change_ratio",strict.at("area_change_ratio")}
    };
    const auto topology_hash = AI::beauty_leaf_digest(topology_certificate.dump());
    const auto new_metadata_path = artifact_root / "native-workbench-partition-v12.json";
    const std::string appearance_id = input_reference.at("appearance_id").get<std::string>();
    const nlohmann::json native_reference {
        {"schema","orca.native-workbench-partition-reference/v2"},{"algorithm",NP::algorithm_version},
        {"partition_digest",native.partition_digest},{"topology_hash",topology_hash},{"coordinate_units","mm"},
        {"source_bounds_mm",NP::encode_native_workbench_bounds(source_bounds)},
        {"carrier_bounds_mm",NP::encode_native_workbench_bounds(carrier_bounds)},
        {"source_path",text_path(original_copy)},{"source_sha256",original_hash},
        {"carrier_path",text_path(new_carrier)},{"carrier_sha256",new_carrier_hash},
        {"appearance_path",text_path(appearance_path)},{"appearance_sha256",appearance_hash},
        {"cache_path",text_path(cache_path)},{"cache_sha256",cache_sha256},
        {"metadata_path",text_path(new_metadata_path)},{"geometry_id",native.geometry_id},
        {"appearance_id",appearance_id},{"cache_reference",cache_reference}
    };
    REQUIRE(GUI::PortraitPrint::valid_native_partition_reference(native_reference, native.partition_digest, topology_hash));
    nlohmann::json commit {
        {"schema","orca.native-workbench-partition-commit/v1"},{"schema_version",6},
        {"transaction_status","committed"},{"job_id","native-workbench-partition-v12-offline-replay"},
        {"portrait_print_id",""},{"requesting_portrait_print_id",""},{"source","native_workbench_partition"},
        {"source_model",text_path(original_copy)},{"source_sha256",original_hash},
        {"model_path",text_path(new_carrier)},{"model_sha256",new_carrier_hash},{"generated_at",std::time(nullptr)},
        {"triangle_count",native.painted.indices.size()},{"native_portrait_partition",native_reference},
        {"native_portrait_topology_certificate",topology_certificate},{"partition_build_report",build_document},
        {"source_check",input_metadata.value("source_check",nlohmann::json{})},
        {"original_appearance",original_appearance.encode(artifact_root)}
    };
    GUI::WorkbenchCheckResult certified;
    const auto& topology = build_report.strict_repair.output_topology;
    certified.status = GUI::WorkbenchCheckStatus::Normal;
    certified.source = GUI::WorkbenchCheckSource::Automatic;
    certified.purpose = GUI::WorkbenchCheckPurpose::PortraitPostColorReadOnly;
    certified.model_sha256 = certified.source_sha256 = new_carrier_hash;
    certified.summary = "Offline original native rebuild passed strict repair and geometry verification.";
    certified.boundary_edges = topology.open_edges;
    certified.nonmanifold_edges = topology.nonmanifold_edges;
    certified.nonmanifold_vertices = topology.nonmanifold_vertices;
    certified.degenerate_faces = topology.degenerate_faces;
    certified.duplicate_faces = topology.duplicate_faces;
    certified.inconsistent_edges = topology.inconsistent_edges;
    certified.self_intersections_checked = topology.self_intersections_checked;
    certified.self_intersections = topology.self_intersects ? 1 : 0;
    certified.before_faces = native.painted.indices.size();
    certified.before_vertices = native.painted.vertices.size();
    certified.candidate_status = GUI::RepairCandidateStatus::None;
    certified.appearance_mapped = true; certified.appearance_confidence = 1.;
    certified.appearance_reason = "Frozen appearance and its topology verified separately from the native carrier.";
    REQUIRE(GUI::workbench_topology_safe(certified));
    commit["workbench_check_certificate"] = GUI::PortraitPrint::encode_workbench_check_certificate(certified,
        GUI::WorkbenchCheckPurpose::PortraitPostColorReadOnly, native.geometry_id, appearance_id,
        text_path(new_metadata_path), commit, native.partition_digest, topology_hash);
    REQUIRE(GUI::PortraitPrint::valid_workbench_check_certificate(commit.at("workbench_check_certificate"),
        GUI::WorkbenchCheckPurpose::PortraitPostColorReadOnly, new_carrier_hash, native.geometry_id,
        appearance_id, text_path(new_metadata_path), commit, native.partition_digest, topology_hash));
    CHECK(NP::publish_native_portrait_partition_commit(new_metadata_path, new_carrier, original_copy, commit) == commit);
    GUI::WorkbenchNativeTopologyCertificate proof;
    proof.partition_metadata = read(new_metadata_path);
    proof.partition_metadata_path = text_path(new_metadata_path);
    proof.native_reference = native_reference;
    proof.candidate_sha256 = proof.carrier_sha256 = new_carrier_hash;
    proof.geometry_id = native.geometry_id; proof.appearance_id = appearance_id;
    proof.partition_digest = native.partition_digest; proof.topology_hash = topology_hash;
    proof.face_count = native.painted.indices.size();
    GUI::WorkbenchCheckResult verified;
    std::string rejection;
    const bool exact_certificate = GUI::decode_native_publication_certificate(
        new_carrier, new_carrier, proof, verified, rejection);
    INFO(rejection);
    REQUIRE(exact_certificate);
    CHECK(GUI::workbench_topology_safe(verified));
    report["artifact_root"] = text_path(artifact_root);
    report["color_result_path"] = text_path(result_path);
    report["color_result_sha256"] = AI::model_artifact_sha256(result_path);
    report["applied_color_layer_path"] = text_path(applied_path);
    report["applied_color_layer"] = applied_layer.encode();
    report["native_identity_path"] = text_path(identity_path);
    report["native_identity_sha256"] = AI::model_artifact_sha256(identity_path);
    report["native_build_report_path"] = text_path(build_path);
    report["native_cache_reference"] = cache_reference;
    report["native_metadata_path"] = text_path(new_metadata_path);
    report["native_carrier_path"] = text_path(new_carrier);
    report["native_carrier_sha256"] = new_carrier_hash;
    report["native_topology_hash"] = topology_hash;
    report["native_certificate_exactly_verified"] = exact_certificate;
    boost::filesystem::ofstream output(report_path, std::ios::binary);
    output << report.dump(); output.close();
    REQUIRE(output.good());
    CHECK(AI::model_artifact_sha256(original_path) == original_hash);
    CHECK(AI::model_artifact_sha256(carrier_path) == carrier_hash);
    CHECK(AI::model_artifact_sha256(blocks_path) == blocks_hash);
    CHECK(AI::model_artifact_sha256(semantics_path) == semantics_hash);
}

TEST_CASE("Native source counts keep atomic regions without inventing unused colors", "[PortraitNativePartition][PortraitColorBlocks][HonorFilamentSuite]") {
    const size_t count = GENERATE(4u,5u,6u);
    Sample s; s.source.mesh.vertices[4].z()=2; s.source.mesh.vertices[5].z()=2;
    s.source.face_colors={{.50f,.50f,.50f,1},{.51f,.50f,.50f,1},{.52f,.50f,.50f,1},{.53f,.50f,.50f,1}};
    const auto partition = frozen_native_partition(s, {0,0,0,0}, count);
    const auto identity = NP::encode_native_portrait_partition_identity(NP::native_portrait_partition_identity(partition));
    auto in = honor_input(s); in.native_partition = &partition;
    const auto result = PC::build(in);
    REQUIRE(result->source_palette.size() == 1);
    REQUIRE(result->native_color_reduction);
    CHECK(result->native_color_reduction->requested_color_count == 5);
    CHECK(result->native_color_reduction->actual_color_count == 1);
    REQUIRE(result->final_palette.size() == 5);
    REQUIRE(result->native_partition_identity);
    CHECK(result->current().blocks.size() == 1);
    CHECK(result->units.size() == s.source.mesh.indices.size());
    CHECK(result->unit_source_slots == partition.face_cluster_ids);
    CHECK(result->face_partition() == beauty_piece_ids(partition));
    CHECK(NP::encode_native_portrait_partition_identity(*result->native_partition_identity) == identity);
    CHECK(NP::encode_native_portrait_partition_identity(NP::native_portrait_partition_identity(partition)) == identity);
    for (const auto& unit : result->units) CHECK(unit.path.depth == 0);
    for (const auto& b : result->current().blocks) CHECK(b.display_slot < 5);
    auto changed_painting = partition.painted;
    changed_painting.cluster_colors.back() = {17,23,31};
    const auto changed = NP::finalize_native_portrait_partition(std::move(changed_painting));
    in.native_partition = &changed;
    CHECK(PC::build(in)->palette_signature != result->palette_signature);
}

TEST_CASE("Semantic and subface evidence do not split a frozen native region", "[PortraitNativePartition][PortraitColorBlocks][HonorFilamentSuite]") {
    Sample s;
    s.analysis.face_labels.assign(4,SC::Label::FaceSkin);
    s.analysis.subface_labels={{1,{4,255},SC::Label::Eyebrow,.99f},
        {2,{4,255},SC::Label::Iris,.99f},{3,{4,255},SC::Label::Lips,.99f},
        {0,{4,255},SC::Label::EyeSclera,.99f}};
    const auto partition = frozen_native_partition(s, {0,0,0,0});
    const auto identity = NP::encode_native_portrait_partition_identity(NP::native_portrait_partition_identity(partition));
    auto in = honor_input(s); in.native_partition = &partition;
    const auto result = PC::build(in);
    REQUIRE(result->current().blocks.size() == 1);
    CHECK(result->current().blocks[0].semantic == SC::Label::FaceSkin);
    REQUIRE(result->final_region_slots.size() == 1);
    CHECK(result->current().blocks[0].display_slot == int64_t(result->final_region_slots[0]));
    CHECK(result->units.size() == 4);
    CHECK(result->face_partition() == beauty_piece_ids(partition));
    // Skin and subface-only evidence cannot replace the native base binding.
    CHECK(result->protected_components.empty());
    CHECK(result->face_overrides.empty());
    SC::FaceColors faces; SC::SubfaceColors children; result->appearance(faces,children);
    CHECK(children.empty());
    REQUIRE(faces.size() == 4);
    for (const auto& face : faces)
        CHECK(face.second == result->final_palette[result->final_region_slots[0]]);
    s.analysis.face_labels = {SC::Label::Hair,SC::Label::Iris,SC::Label::Eyebrow,SC::Label::Unknown};
    const auto rebound = PC::build(in);
    CHECK(rebound->face_partition() == beauty_piece_ids(partition));
    CHECK(rebound->current().blocks.size() == 1);
    CHECK(rebound->units.size() == result->units.size());
    REQUIRE(rebound->native_partition_identity);
    CHECK(NP::encode_native_portrait_partition_identity(*rebound->native_partition_identity) == identity);
    CHECK(NP::encode_native_portrait_partition_identity(NP::native_portrait_partition_identity(partition)) == identity);
    REQUIRE(result->native_color_reduction);
    REQUIRE(rebound->native_color_reduction);
    CHECK(rebound->native_color_reduction->mapping_digest == result->native_color_reduction->mapping_digest);
    CHECK(rebound->final_region_slots == result->final_region_slots);
    REQUIRE(rebound->protected_components.size() == 3);
    REQUIRE(rebound->face_overrides.size() == 3);
    rebound->appearance(faces,children);
    CHECK(children.empty());
    REQUIRE(faces.size() == 4);
    for (size_t face = 0; face < 3; ++face) {
        CHECK(rebound->face_overrides[face].face == face);
        CHECK(rebound->face_overrides[face].slot == 0);
        CHECK(faces[face].second == rebound->final_palette[0]);
    }
    CHECK(faces[3].second == rebound->final_palette[rebound->final_region_slots[0]]);
}

TEST_CASE("Native regions bind anatomy to fixed slots and allow shared target colors", "[PortraitNativePartition][PortraitColorBlocks][HonorFilamentSuite]") {
    Sample s;
    s.analysis.face_labels = {SC::Label::FaceSkin,SC::Label::Hair,SC::Label::EyeSclera,SC::Label::Lips};
    const auto partition = frozen_native_partition(s, {0,1,2,3});
    const auto identity = NP::encode_native_portrait_partition_identity(NP::native_portrait_partition_identity(partition));
    auto in = honor_input(s); in.native_partition = &partition;
    const auto result = PC::build(in);
    REQUIRE(result->final_region_slots.size() == 4);
    auto base_slots = result->final_region_slots;
    std::sort(base_slots.begin(), base_slots.end());
    CHECK(std::adjacent_find(base_slots.begin(), base_slots.end()) == base_slots.end());
    for (size_t face = 0; face < 4; ++face)
        CHECK(block(*result,face).display_slot == int64_t(result->final_region_slots[partition.face_region_ids[face]]));
    REQUIRE(result->protected_components.size() == 3);
    REQUIRE(result->face_overrides.size() == 3);
    SC::FaceColors faces; SC::SubfaceColors children; result->appearance(faces,children);
    REQUIRE(faces.size() == 4);
    CHECK(children.empty());
    CHECK(faces[0].second == result->final_palette[result->final_region_slots[partition.face_region_ids[0]]]);
    CHECK(faces[1].second == result->final_palette[0]);
    CHECK(faces[2].second == result->final_palette[3]);
    CHECK(faces[3].second == result->final_palette[1]);
    s.analysis.face_labels = {SC::Label::Hair,SC::Label::Iris,SC::Label::Eyebrow,SC::Label::Unknown};
    const auto shared = PC::build(in);
    REQUIRE(result->native_color_reduction);
    REQUIRE(shared->native_color_reduction);
    CHECK(shared->native_color_reduction->mapping_digest == result->native_color_reduction->mapping_digest);
    CHECK(shared->final_region_slots == result->final_region_slots);
    REQUIRE(shared->protected_components.size() == 3);
    REQUIRE(shared->face_overrides.size() == 3);
    for (const auto& overlay : shared->face_overrides) CHECK(overlay.slot == 0);
    shared->appearance(faces,children);
    REQUIRE(faces.size() == 4);
    CHECK(children.empty());
    for (size_t face = 0; face < 3; ++face) CHECK(faces[face].second == shared->final_palette[0]);
    CHECK(faces[3].second == shared->final_palette[shared->final_region_slots[partition.face_region_ids[3]]]);
    CHECK(shared->current().blocks.size() == 4);
    CHECK(shared->face_partition() == beauty_piece_ids(partition));
    REQUIRE(shared->native_partition_identity);
    CHECK(NP::encode_native_portrait_partition_identity(*shared->native_partition_identity) == identity);
    CHECK(NP::encode_native_portrait_partition_identity(NP::native_portrait_partition_identity(partition)) == identity);
}

TEST_CASE("Disconnected native islands retain identity when Honor target colors are shared", "[PortraitNativePartition][PortraitColorBlocks][HonorFilamentSuite]") {
    Sample s;
    s.source.mesh.vertices = {{0,0,0},{1,0,0},{0,1,0},{0,0,1},
        {3,0,0},{4,0,0},{3,1,0},{3,0,1}};
    s.source.mesh.indices = {{0,2,1},{0,1,3},{0,3,2},{1,2,3},
        {4,6,5},{4,5,7},{4,7,6},{5,6,7}};
    s.source.face_colors.assign(8,{.01f,.01f,.01f,1});
    s.analysis.face_labels.assign(8,SC::Label::Unknown);
    s.analysis.face_confidence.assign(8,.95f);
    PaintedMesh painting;
    for (const auto& vertex : s.source.mesh.vertices)
        painting.vertices.push_back({vertex.x(), vertex.y(), vertex.z()});
    for (const auto& face : s.source.mesh.indices)
        painting.indices.push_back({face[0], face[1], face[2]});
    painting.cluster_colors = {{0,0,0},{4,3,2},{255,255,255},{255,0,0}};
    const std::array<size_t,8> clusters {{0,0,0,0,1,0,0,0}};
    for (const auto cluster : clusters) painting.face_colors.push_back(painting.cluster_colors[cluster]);
    auto partition = NP::finalize_native_portrait_partition(std::move(painting));
    s.source.geometry_id = s.analysis.geometry_id = partition.geometry_id;
    auto in = honor_input(s); in.native_partition = &partition;
    const auto result = PC::build(in);
    REQUIRE(result->current().blocks.size() == 3);
    CHECK(block(*result,0).id != block(*result,4).id);
    CHECK(block(*result,0).id != block(*result,5).id);
    CHECK(block(*result,5).id == block(*result,6).id);
    CHECK(block(*result,6).id == block(*result,7).id);
    // Equal native colors share one palette group across disconnected atoms;
    // the distinct second color group keeps an independent palette slot.
    REQUIRE(result->native_color_reduction);
    CHECK(result->native_color_reduction->actual_color_count == 2);
    CHECK(block(*result,0).display_slot == block(*result,5).display_slot);
    CHECK(block(*result,0).display_slot != block(*result,4).display_slot);
    CHECK(PC::used_display_slots(*result).size() == 2);
    CHECK(result->protected_components.empty());
    CHECK(result->face_overrides.empty());
    CHECK(result->unit_source_slots == partition.face_cluster_ids);
    CHECK(result->face_partition() == beauty_piece_ids(partition));
    REQUIRE(result->native_partition_identity);
    CHECK(NP::encode_native_portrait_partition_identity(*result->native_partition_identity) ==
          NP::encode_native_portrait_partition_identity(NP::native_portrait_partition_identity(partition)));
}

TEST_CASE("Native regions export and reopen actual face colors without refinement", "[PortraitNativePartition][PortraitColorBlocks][HonorFilamentSuite]") {
    Sample s;
    s.source.sample_original=[](size_t face,const SC::Barycentric& bary)->SC::Color {
        return face==1 && bary[0]>.37f ? SC::Color{0,0,0} : SC::Color{1,1,1};
    };
    auto partition = frozen_native_partition(s, {0,0,1,1});
    auto in = honor_input(s); in.native_partition = &partition;
    const auto result = PC::build(in);
    REQUIRE(result->units.size() == s.source.mesh.indices.size());
    CHECK(result->borders.empty());
    SC::FaceColors faces; SC::SubfaceColors children; result->appearance(faces,children);
    REQUIRE(children.empty());
    const auto geometry = GUI::build_semantic_colored_geometry(s.source,faces,children);
    CHECK(geometry.indices_count()/3 == s.source.mesh.indices.size());
    const auto document = PC::encode(*result); std::string error;
    CHECK_FALSE(document.contains("borders"));
    const auto directory=boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("orca-native-regions-%%%%-%%%%");
    struct Remove {boost::filesystem::path path;~Remove(){boost::system::error_code ec;boost::filesystem::remove_all(path,ec);}} cleanup{directory};
    const auto published = PC::publish_verified_color_layer(*result,directory);
    CHECK(published.painted_subfaces == 0);
    const auto saved = PC::decode(document,s.source.geometry_id,4,s.source.content_id,s.analysis.signature,result->palette_signature,error);
    INFO(error); REQUIRE(saved);
    REQUIRE(saved->native_partition_identity);
    CHECK(NP::encode_native_portrait_partition_identity(*saved->native_partition_identity) ==
          NP::encode_native_portrait_partition_identity(NP::native_portrait_partition_identity(partition)));
    SC::FaceColors reopened_faces; SC::SubfaceColors reopened_children; saved->appearance(reopened_faces,reopened_children);
    CHECK(reopened_faces == faces); CHECK(reopened_children.empty());
    CHECK(PC::encode(*saved) == document);
    CHECK(PC::encode(*PC::build(in)) == document);
    auto corrupt=document; corrupt["unit_source_slots"][0]=6;
    CHECK_FALSE(PC::decode(corrupt,s.source.geometry_id,4,s.source.content_id,s.analysis.signature,result->palette_signature,error));
    auto historical=document; historical["algorithm_version"]=uint32_t(7); historical["borders"]=nlohmann::json::array();
    historical.erase("native_partition_identity"); historical.erase("semantic_binding");
    historical.erase("region_semantics"); historical.erase("native_color_reduction");
    historical.erase("final_slot_binding");
    const auto legacy=PC::decode(historical,s.source.geometry_id,4,s.source.content_id,s.analysis.signature,result->palette_signature,error);
    INFO(error); REQUIRE(legacy); CHECK(PC::encode(*legacy) == historical);
    CHECK_THROWS(PC::build(in,[]{return true;}));
    partition.geometry_id="other-model"; CHECK_THROWS(PC::build(in));
}
TEST_CASE("ColorRegionMajorityV2 keeps non protected semantic ownership on color blocks", "[PortraitColorBlocks]") {
    Sample s;
    s.analysis.face_labels = {SC::Label::Clothes, SC::Label::Accessories,
        SC::Label::Clothes, SC::Label::Unknown};
    auto in = s.input(); in.strategy = PC::Strategy::ColorRegionMajorityV2;
    in.intermediate_palette = {{.4f, .4f, .4f}}; in.final_color_count = 4;
    const auto r = PC::build(in);
    REQUIRE(r->current().blocks.size() == 1);
    REQUIRE(r->current().blocks.front().semantic == SC::Label::Clothes);
}
TEST_CASE("ColorRegionMajorityV2 reserves independent core anatomy colors", "[PortraitColorBlocks]") {
    Sample s;
    s.analysis.face_labels = {SC::Label::FaceSkin, SC::Label::Lips, SC::Label::EyeSclera, SC::Label::Iris};
    s.source.face_colors = {{.72f,.42f,.30f,1.f}, {.75f,.08f,.12f,1.f},
        {.92f,.92f,.92f,1.f}, {.06f,.04f,.03f,1.f}};
    auto in = s.input(); in.strategy = PC::Strategy::ColorRegionMajorityV2;
    in.intermediate_palette = {{.72f,.42f,.30f},{.75f,.08f,.12f},{.92f,.92f,.92f},{.06f,.04f,.03f}};
    in.final_color_count = 4;
    const auto result = PC::build(in);
    REQUIRE(result->role_locks.size() == 4);
    REQUIRE(result->role_regions.size() == 4);
    const auto skin = result->role_locks.at("Skin");
    const auto lips = result->role_locks.at("Lips");
    const auto sclera = result->role_locks.at("EyeSclera");
    const auto iris = result->role_locks.at("Iris");
    REQUIRE(GUI::PreviewPalette::distance(GUI::PreviewPalette::to_lab(skin), GUI::PreviewPalette::to_lab(lips)) > 1e-8f);
    REQUIRE(GUI::PreviewPalette::distance(GUI::PreviewPalette::to_lab(sclera), GUI::PreviewPalette::to_lab(iris)) > 1e-8f);
}
TEST_CASE("Portrait color postprocess is restricted to four through six colors", "[PortraitColorBlocks]") {
    REQUIRE_FALSE(PC::portrait_color_postprocess_supported(3));
    REQUIRE(PC::portrait_color_postprocess_supported(4));
    REQUIRE(PC::portrait_color_postprocess_supported(5));
    REQUIRE(PC::portrait_color_postprocess_supported(6));
    REQUIRE_FALSE(PC::portrait_color_postprocess_supported(7));

    Sample s;
    auto in = s.input();
    in.strategy = PC::Strategy::ColorRegionMajorityV2;
    in.intermediate_palette = {{.1f, .1f, .1f}, {.9f, .9f, .9f}};
    in.final_color_count = 7;
    REQUIRE_THROWS(PC::build(in));
}
TEST_CASE("Portrait optimization preserves automatic suggestion slot identity at four five and six colors", "[PortraitColorBlocks]") {
    const size_t count = GENERATE(size_t(4), size_t(5), size_t(6));
    Sample s;
    s.analysis.face_labels = {SC::Label::FaceSkin, SC::Label::Lips, SC::Label::EyeSclera, SC::Label::Iris};
    s.source.face_colors = {{.72f,.42f,.30f,1.f}, {.75f,.08f,.12f,1.f},
        {.92f,.92f,.92f,1.f}, {.06f,.04f,.03f,1.f}};
    auto in = s.input();
    in.strategy = PC::Strategy::ColorRegionMajorityV2;
    in.intermediate_palette = {{.72f,.42f,.30f}, {.75f,.08f,.12f}, {.92f,.92f,.92f},
        {.06f,.04f,.03f}, {.05f,.55f,.75f}, {.20f,.65f,.25f}};
    in.targets = {{.05f,.55f,.75f}, {.92f,.92f,.92f}, {.06f,.04f,.03f},
        {.75f,.08f,.12f}, {.72f,.42f,.30f}, {.20f,.65f,.25f}};
    in.targets.resize(count);
    in.mapping = in.targets;
    in.final_color_count = count;
    const auto result = PC::build(in);

    REQUIRE(result->final_palette == in.targets);
    REQUIRE(result->final_slot_indices.size() == count);
    for (size_t slot = 0; slot < count; ++slot)
        REQUIRE(result->final_slot_indices[slot] == slot);
    for (const auto& [role, slot] : result->role_slot_locks) {
        REQUIRE(slot < count);
        REQUIRE(result->role_locks.at(role) == in.targets[slot]);
    }
    for (const auto& block : result->current().blocks) {
        REQUIRE(block.display_slot >= 0);
        REQUIRE(size_t(block.display_slot) < count);
        REQUIRE(block.display == in.targets[size_t(block.display_slot)]);
    }
}
TEST_CASE("Dark portrait roles prefer the darkest legal card slot", "[PortraitColorBlocks]") {
    Sample s;
    s.analysis.face_labels = {SC::Label::FaceSkin, SC::Label::Iris, SC::Label::Hair, SC::Label::Eyebrow};
    s.source.face_colors = {{.72f, .42f, .30f, 1.f}, {.28f, .12f, .05f, 1.f},
        {.40f, .22f, .12f, 1.f}, {.52f, .30f, .16f, 1.f}};
    auto in = s.input();
    in.strategy = PC::Strategy::ColorRegionMajorityV2;
    in.intermediate_palette = {{.72f, .42f, .30f}, {.28f, .12f, .05f}, {.40f, .22f, .12f}, {.52f, .30f, .16f}};
    in.targets = {{.72f, .42f, .30f}, {.55f, .25f, .12f}, {.02f, .02f, .02f}, {.95f, .95f, .95f}};
    in.final_color_count = 4;
    const auto result = PC::build(in);
    REQUIRE(result->role_locks.at("Iris") == SC::Color{.02f, .02f, .02f});
    REQUIRE(result->role_locks.at("Hair") == SC::Color{.02f, .02f, .02f});
    REQUIRE(result->role_locks.at("Eyebrow") == SC::Color{.02f, .02f, .02f});
}
TEST_CASE("Clothes compete for a remaining slot using the original source color", "[PortraitColorBlocks]") {
    Sample s;
    s.source.mesh.vertices.push_back({10, 0, 0});
    s.source.mesh.vertices.push_back({11, 0, 0});
    s.source.mesh.vertices.push_back({10, 1, 0});
    s.source.mesh.indices.push_back({6, 7, 8});
    s.source.face_colors.push_back({.05f, .55f, .75f, 1.f});
    s.analysis.face_labels = {SC::Label::FaceSkin, SC::Label::Lips, SC::Label::EyeSclera,
        SC::Label::Iris, SC::Label::Clothes};
    s.analysis.face_confidence.assign(5, .95f);
    s.source.face_colors[0] = {.72f, .42f, .30f, 1.f};
    s.source.face_colors[1] = {.75f, .08f, .12f, 1.f};
    s.source.face_colors[2] = {.92f, .92f, .92f, 1.f};
    s.source.face_colors[3] = {.06f, .04f, .03f, 1.f};
    auto in = s.input();
    in.strategy = PC::Strategy::ColorRegionMajorityV2;
    in.intermediate_palette = {{.72f, .42f, .30f}, {.75f, .08f, .12f}, {.92f, .92f, .92f},
        {.06f, .04f, .03f}, {.05f, .55f, .75f}};
    in.targets = in.intermediate_palette;
    in.final_color_count = 5;
    const auto result = PC::build(in);
    REQUIRE(result->final_palette.size() == 5);
    const auto clothes = std::find_if(result->current().blocks.begin(), result->current().blocks.end(),
        [](const auto &item) { return item.semantic == SC::Label::Clothes; });
    REQUIRE(clothes != result->current().blocks.end());
    REQUIRE(clothes->display == SC::Color{.05f, .55f, .75f});
}
TEST_CASE("Color card remapping preserves portrait regions and core slot separation", "[PortraitColorBlocks]") {
    Sample s;
    s.analysis.face_labels = {SC::Label::FaceSkin, SC::Label::Lips, SC::Label::EyeSclera, SC::Label::Iris};
    s.source.face_colors = {{.72f,.42f,.30f,1.f}, {.75f,.08f,.12f,1.f},
        {.92f,.92f,.92f,1.f}, {.06f,.04f,.03f,1.f}};
    auto in = s.input(); in.strategy = PC::Strategy::ColorRegionMajorityV2;
    in.intermediate_palette = {{.72f,.42f,.30f},{.75f,.08f,.12f},{.92f,.92f,.92f},{.06f,.04f,.03f}};
    in.final_color_count = 4;
    auto result = PC::build(in);
    const auto regions = result->role_regions;
    const std::vector<SC::Color> card = {{.60f,.30f,.20f},{.90f,.10f,.20f},{.98f,.98f,.98f},{.01f,.01f,.01f}};
    REQUIRE(PC::remap_to_palette(*result, card));
    REQUIRE(result->role_regions == regions);
    REQUIRE(result->final_palette == card);
    REQUIRE(result->role_locks.at("EyeSclera") != result->role_locks.at("Iris"));
    REQUIRE_FALSE(PC::remap_to_palette(*result, {{.5f,.5f,.5f},{.5f,.5f,.5f},{.7f,.7f,.7f},{.8f,.8f,.8f}}));
}
TEST_CASE("Gradual garment shading compares neighboring faces instead of one seed", "[PortraitColorBlocks]") {
    Sample s;
    s.source.face_colors[0]={.30f,.30f,.30f,1};
    s.source.face_colors[1]={.34f,.34f,.34f,1};
    s.source.face_colors[2]={.38f,.38f,.38f,1};
    s.source.face_colors[3]={.42f,.42f,.42f,1};
    const auto r=PC::build(s.input());
    REQUIRE(r->current().blocks.size()==1);
}
TEST_CASE("ColorRegionMajorityV2 does not chain a gradient through one semantic region", "[PortraitColorBlocks]") {
    Sample s;
    s.source.face_colors[0]={.30f,.30f,.30f,1};
    s.source.face_colors[1]={.34f,.34f,.34f,1};
    s.source.face_colors[2]={.38f,.38f,.38f,1};
    s.source.face_colors[3]={.42f,.42f,.42f,1};
    auto in=s.input();
    in.strategy=PC::Strategy::ColorRegionMajorityV2;
    in.intermediate_palette={{.30f,.30f,.30f},{.42f,.42f,.42f}};
    in.targets={{.30f,.30f,.30f},{.42f,.42f,.42f},{.15f,.15f,.15f},{.75f,.75f,.75f}};
    in.final_color_count=4;
    const auto r=PC::build(in);
    REQUIRE(r->current().blocks.size()>1);
}
TEST_CASE("Diagnostics attribute a color split to the color threshold", "[PortraitColorBlocks]") {
    Sample s;
    s.source.face_colors[0]={.1f,.1f,.1f,1};
    s.source.face_colors[1]={.9f,.9f,.9f,1};
    const auto r=PC::build(s.input());
    REQUIRE(r->current().blocks.size()>=2);
    REQUIRE(r->diagnostic_summary().color_blocked_edges>0);
    REQUIRE(std::any_of(r->current().blocks.begin(),r->current().blocks.end(),[](const auto &b) {
        return b.diagnostic.primary==PC::BlockCause::ColorThreshold;
    }));
}
TEST_CASE("Diagnostics keep semantic boundaries separate from color thresholds", "[PortraitColorBlocks]") {
    Sample s;
    s.analysis.face_labels[0]=SC::Label::FaceSkin;
    s.analysis.face_labels[1]=SC::Label::BodySkin;
    const auto r=PC::build(s.input());
    REQUIRE(r->current().blocks.size()>=2);
    REQUIRE(r->diagnostic_summary().semantic_blocked_edges>0);
    REQUIRE(std::any_of(r->current().blocks.begin(),r->current().blocks.end(),[](const auto &b) {
        return b.diagnostic.primary==PC::BlockCause::SemanticBoundary;
    }));
}
TEST_CASE("Uncertain skin boundaries do not split one same color region", "[PortraitColorBlocks]") {
    Sample s;
    s.analysis.face_labels[0]=SC::Label::FaceSkin;
    const auto r=PC::build(s.input());
    REQUIRE(r->current().blocks.size()==1);
    REQUIRE(r->diagnostic_summary().semantic_blocked_edges==0);
}
TEST_CASE("Confirmed eye feature boundaries remain protected", "[PortraitColorBlocks]") {
    Sample s;
    s.analysis.face_labels[0]=SC::Label::Iris;
    const auto r=PC::build(s.input());
    REQUIRE(r->current().blocks.size()>=2);
    REQUIRE(r->diagnostic_summary().semantic_blocked_edges>0);
    REQUIRE(std::any_of(r->current().blocks.begin(),r->current().blocks.end(),[](const auto &b) {
        return b.diagnostic.primary==PC::BlockCause::SemanticBoundary;
    }));
}
TEST_CASE("Diagnostics expose open and non manifold topology", "[PortraitColorBlocks]") {
    Sample open;
    const auto open_result=PC::build(open.input());
    REQUIRE(open_result->diagnostic_summary().topology_disconnected>0);

    Sample non_manifold;
    non_manifold.source.mesh.vertices.push_back({0,-1,0});
    non_manifold.source.mesh.vertices.push_back({0,0,-1});
    non_manifold.source.mesh.indices.push_back({0,1,6});
    non_manifold.source.mesh.indices.push_back({0,1,7});
    non_manifold.source.face_colors.push_back({.4f,.4f,.4f,1});
    non_manifold.source.face_colors.push_back({.4f,.4f,.4f,1});
    non_manifold.analysis.face_labels.push_back(SC::Label::Unknown);
    non_manifold.analysis.face_labels.push_back(SC::Label::Unknown);
    non_manifold.analysis.face_confidence.push_back(.95f);
    non_manifold.analysis.face_confidence.push_back(.95f);
    const auto non_manifold_result=PC::build(non_manifold.input());
    REQUIRE(non_manifold_result->diagnostic_summary().nonmanifold_edges>0);
}
TEST_CASE("Diagnostic fields are optional for old color block caches", "[PortraitColorBlocks]") {
    Sample s; const auto r=PC::build(s.input()); auto doc=PC::encode(*r);
    for(auto &track:doc["tracks"]) for(auto &block:track["blocks"]) block.erase("diagnostic");
    std::string error;
    const auto restored=PC::decode(doc,s.source.geometry_id,4,s.source.content_id,s.analysis.signature,r->palette_signature,error);
    REQUIRE(restored); REQUIRE(error.empty());
    for(const auto &b:restored->current().blocks)
        REQUIRE(b.diagnostic.primary==PC::BlockCause::None);
}
TEST_CASE("Equal colors on disconnected surfaces remain separate", "[PortraitColorBlocks]") {
    Sample s;s.source.mesh.vertices.push_back({10,0,0});s.source.mesh.vertices.push_back({11,0,0});s.source.mesh.vertices.push_back({10,1,0});
    s.source.mesh.indices[3]={6,7,8};
    auto r=PC::build(s.input());REQUIRE(r->current().blocks.size()==2);
}
TEST_CASE("Non anatomy labels do not fragment one continuous color region", "[PortraitColorBlocks]") {
    Sample s;
    s.analysis.face_labels={SC::Label::Clothes,SC::Label::Accessories,SC::Label::Unknown,SC::Label::Clothes};
    const auto r=PC::build(s.input());
    REQUIRE(r->current().blocks.size()==1);
}
TEST_CASE("Face partition follows connected color block identity", "[PortraitColorBlocks]") {
    Sample s;
    s.source.face_colors[0] = {.9f,.1f,.1f,1.f};
    s.source.face_colors[1] = {.9f,.1f,.1f,1.f};
    s.source.face_colors[2] = {.1f,.1f,.9f,1.f};
    s.source.face_colors[3] = {.1f,.1f,.9f,1.f};
    const auto r = PC::build(s.input());
    const auto partition = r->face_partition();
    REQUIRE(partition.size() == s.source.mesh.indices.size());
    REQUIRE(partition[0] == partition[1]);
    REQUIRE(partition[2] == partition[3]);
    REQUIRE(partition[0] != partition[2]);
    REQUIRE(std::all_of(partition.begin(), partition.end(), [](uint32_t id) { return id != 0; }));
}
TEST_CASE("Face partition splits disconnected islands after unit collapse", "[PortraitColorBlocks]") {
    PC::Result result;
    result.face_count = 4;
    result.units = {{0,{0,0}},{1,{0,0}},{2,{0,0}},{3,{0,0}}};
    result.tracks[1].unit_blocks = {0,0,0,0};
    result.tracks[1].blocks.push_back({0,{0,1,2,3},{.4f,.4f,.4f},{.4f,.4f,.4f},SC::Label::Unknown,PC::Status::Unresolved,0.,{}, {}, -1});
    const auto partition = result.face_partition({{1},{0},{3},{2}});
    REQUIRE(partition == std::vector<uint32_t>{1,1,2,2});
}
TEST_CASE("Minor source texture errors share a stable color block", "[PortraitColorBlocks]") {
    Sample s;s.source.face_colors[1]={.401f,.402f,.4f,1};
    auto r=PC::build(s.input());REQUIRE(r->current().blocks.size()==1);
    REQUIRE(PC::encode(*r)==PC::encode(*PC::build(s.input())));
}
TEST_CASE("High confidence anatomy seeds label only their same color neighbors", "[PortraitColorBlocks]") {
    const auto label=GENERATE(SC::Label::Iris,SC::Label::EyeSclera,SC::Label::Lips,SC::Label::FaceSkin,SC::Label::BodySkin);
    Sample s;s.analysis.face_labels[0]=label;
    auto r=PC::build(s.input());REQUIRE(block(*r,1).semantic==label);
    REQUIRE(block(*r,1).status==PC::Status::Confirmed);
    s.analysis.face_confidence[0]=.1f;r=PC::build(s.input());
    REQUIRE(block(*r,1).status==PC::Status::Unresolved);
}
TEST_CASE("Conflicting anatomy seeds do not claim the unknown color region", "[PortraitColorBlocks]") {
    Sample s;s.analysis.face_labels[0]=SC::Label::Iris;s.analysis.face_labels[3]=SC::Label::Lips;
    auto r=PC::build(s.input());
    REQUIRE(block(*r,1).semantic==SC::Label::Unknown);
    REQUIRE(block(*r,1).candidate==-1);
    REQUIRE(block(*r,1).reason=="conflicting_semantic_seeds");
}
TEST_CASE("Person identity prevents color propagation and temporary replacement", "[PortraitColorBlocks]") {
    Sample s;s.analysis.face_labels[0]=SC::Label::FaceSkin;
    auto in=s.input();in.person_ids={1,2,2,2};auto r=PC::build(in);
    REQUIRE(block(*r,1).status==PC::Status::Unresolved);
    REQUIRE(block(*r,1).candidate==-1);
}
TEST_CASE("An unresolved region previews a confirmed lighter neighbor without gaining its label", "[PortraitColorBlocks]") {
    Sample s;s.source.face_colors[0]={.9f,.9f,.9f,1};s.analysis.face_labels[0]=SC::Label::FaceSkin;
    auto r=PC::build(s.input());const auto& b=block(*r,1);
    REQUIRE(b.members.size()==3);
    REQUIRE(b.status==PC::Status::Unresolved);REQUIRE(b.semantic==SC::Label::Unknown);
    REQUIRE(b.candidate==block(*r,0).id);REQUIRE(b.display==block(*r,0).original);
}
TEST_CASE("Temporary neighbors never cascade their candidate through another region", "[PortraitColorBlocks]") {
    Sample s;s.source.face_colors[0]={.9f,.9f,.9f,1};s.source.face_colors[3]={.1f,.1f,.1f,1};
    s.analysis.face_labels[0]=SC::Label::FaceSkin;auto r=PC::build(s.input());
    REQUIRE(block(*r,1).candidate>=0);REQUIRE(block(*r,3).candidate==-1);
    REQUIRE(block(*r,3).display==block(*r,3).original);
}
TEST_CASE("A darker confirmed neighbor leaves unresolved source colors unchanged", "[PortraitColorBlocks]") {
    Sample s;s.source.face_colors[0]={.1f,.1f,.1f,1};s.analysis.face_labels[0]=SC::Label::FaceSkin;
    auto r=PC::build(s.input());REQUIRE(block(*r,1).candidate==-1);
    REQUIRE(block(*r,1).display==block(*r,1).original);
}
TEST_CASE("Source and ordinary color tracks are independent and reversible", "[PortraitColorBlocks]") {
    Sample s;auto in=s.input();in.mapping={{.4f,.4f,.4f}};in.targets={{.9f,.1f,.1f}};
    auto r=PC::build(in);REQUIRE(r->active==PC::Track::ProcessedFaceColor);
    REQUIRE(r->tracks[0].blocks[0].display!=r->tracks[1].blocks[0].display);
    const auto before=PC::encode(*r);auto edited=*r;
    REQUIRE(edited.recolor(0,{.2f,.3f,.4f}));REQUIRE(edited.unresolved_count()==0);
    REQUIRE(edited.current().blocks[0].status==PC::Status::ManualConfirmed);
    REQUIRE(PC::encode(*r)==before);REQUIRE(edited.tracks[0].blocks[0].status==PC::Status::Unresolved);
}
TEST_CASE("Existing subfaces retain complete coverage and exact member identity", "[PortraitColorBlocks]") {
    Sample s;auto in=s.input();in.processed_subfaces={{1,{1,0},{.95f,.1f,.1f},1}};
    auto r=PC::build(in);REQUIRE(r->units.size()==7);
    SC::FaceColors faces;SC::SubfaceColors children;r->appearance(faces,children);
    REQUIRE(faces.size()==3);REQUIRE(children.size()==4);
    REQUIRE(std::any_of(children.begin(),children.end(),[](const auto& c){return c.face_id==1 && c.path.value==0 && c.color[0]==.95f;}));
    std::string error;auto decoded=PC::decode(PC::encode(*r),s.source.geometry_id,4,s.source.content_id,s.analysis.signature,r->palette_signature,error);
    REQUIRE(decoded);REQUIRE(PC::encode(*decoded)==PC::encode(*r));
}
TEST_CASE("Corrupt or stale block caches are rejected without changing ordinary appearance", "[PortraitColorBlocks]") {
    Sample s;auto r=PC::build(s.input());const auto original=PC::encode(*r);auto doc=original;
    const int failure=GENERATE(0,1,2,3,4,5);
    switch(failure) {
    case 0:doc["geometry_id"]="changed";break;case 1:doc["source_signature"]="changed";break;
    case 2:doc["semantic_signature"]="changed";break;case 3:doc["palette_signature"]="changed";break;
    case 4:doc["tracks"][1]["blocks"][0]["members"].push_back(0);break;
    default:doc["units"][0][1]=4;break;
    }
    std::string error;REQUIRE_FALSE(PC::decode(doc,s.source.geometry_id,4,s.source.content_id,s.analysis.signature,r->palette_signature,error));
    REQUIRE_FALSE(error.empty());REQUIRE(PC::encode(*r)==original);
}
TEST_CASE("Addressed block cache preserves both tracks and rejects missing files", "[PortraitColorBlocks]") {
    Sample s;auto r=PC::build(s.input());
    const auto root=boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("orca-color-blocks-%%%%-%%%%");
    const auto ref=PC::save(*r,root);std::string error;
    auto restored=PC::load(ref,root,s.source.geometry_id,4,s.source.content_id,error);
    REQUIRE(restored);REQUIRE(PC::encode(*restored)==PC::encode(*r));
    boost::filesystem::remove(root/(ref.at("content_id").get<std::string>()+".json"));
    REQUIRE_FALSE(PC::load(ref,root,s.source.geometry_id,4,s.source.content_id,error));
    boost::filesystem::remove(root);
}
TEST_CASE("Portrait publication reads back actual face and child colors and refuses a corrupt layer", "[PortraitColorBlocks][PortraitPrintFlow]") {
    Sample sample;
    auto input = honor_input(sample);
    input.processed_subfaces = {{1, {1, 0}, {.95f, .1f, .1f}, 1.f}};
    const auto state = PC::build(input);
    const auto root = boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("orca-portrait-publication-%%%%-%%%%");
    const auto layer = PC::publish_verified_color_layer(*state, root);
    REQUIRE(layer.painted_faces > 0);
    REQUIRE(layer.painted_subfaces > 0);
    CHECK(layer.encode().at("schema") == "orca.portrait-applied-color-layer/v1");
    const auto path = root / (layer.reference.at("content_id").get<std::string>() + ".json");
    { boost::filesystem::ofstream corrupt(path, std::ios::binary | std::ios::app); corrupt << "corrupt"; }
    CHECK_THROWS(PC::publish_verified_color_layer(*state, root));
    PC::Result empty;
    CHECK_THROWS(PC::publish_verified_color_layer(empty, root));
    boost::filesystem::remove(path);
    boost::filesystem::remove(root);
}

TEST_CASE("Canceled block generation never returns a partially colored candidate", "[PortraitColorBlocks]") {
    Sample s;REQUIRE_THROWS(PC::build(s.input(),[]{return true;}));
}
TEST_CASE("Known non anatomy colors remain unresolved semantic barriers", "[PortraitColorBlocks]") {
    Sample s;s.analysis.face_labels[0]=SC::Label::FaceSkin;s.analysis.face_labels[1]=SC::Label::Hair;
    s.source.face_colors[0]={.95f,.95f,.95f,1};
    const auto r=PC::build(s.input());
    REQUIRE(block(*r,1).status==PC::Status::Unresolved);
    REQUIRE(block(*r,1).semantic==SC::Label::Hair);
    REQUIRE(block(*r,1).candidate==-1);
    REQUIRE(block(*r,2).candidate==-1);
}
TEST_CASE("Explicit reruns preserve manual confirmations on both tracks and child boundaries", "[PortraitColorBlocks]") {
    Sample s;auto in=s.input();in.processed_subfaces={{1,{2,15},{.95f,.1f,.1f},1}};
    auto previous=PC::build(in);
    const auto unit=std::find_if(previous->units.begin(),previous->units.end(),[](const auto& u){return u.face==1 && u.path.depth==2 && u.path.value==15;});
    REQUIRE(unit!=previous->units.end());const auto index=size_t(unit-previous->units.begin());
    REQUIRE(previous->recolor(previous->tracks[1].unit_blocks[index],{.1f,.2f,.9f}));
    previous->active=PC::Track::SourceAppearance;
    REQUIRE(previous->recolor(previous->tracks[0].unit_blocks[0],{.2f,.8f,.1f}));
    auto rerun=s.input();rerun.previous=previous;
    const auto result=PC::build(rerun);
    const auto found=std::find_if(result->units.begin(),result->units.end(),[](const auto& u){return u.face==1 && u.path.depth==2 && u.path.value==15;});
    REQUIRE(found!=result->units.end());
    const auto& child=result->tracks[1].blocks[result->tracks[1].unit_blocks[found-result->units.begin()]];
    REQUIRE(child.status==PC::Status::ManualConfirmed);
    REQUIRE(child.display==(SC::Color{.1f,.2f,.9f}));
    REQUIRE(block(*result,0,0).status==PC::Status::ManualConfirmed);
    REQUIRE(block(*result,0,0).display==(SC::Color{.2f,.8f,.1f}));
    REQUIRE(result->active==PC::Track::ProcessedFaceColor);
    rerun.previous=previous;s.source.content_id=std::string(64,'f');
    REQUIRE_THROWS(PC::build(rerun));
}
TEST_CASE("Equal boundary candidates use perceptual distance and stable block identity", "[PortraitColorBlocks]") {
    Sample s;s.source.mesh.vertices={{0,0,0},{1,0,0},{0,1,0},{1,1,0},{0,2,0},{1,2,0}};
    s.analysis.face_labels[0]=SC::Label::FaceSkin;s.analysis.face_labels[3]=SC::Label::BodySkin;
    s.source.face_colors[0]={.95f,.95f,.95f,1};s.source.face_colors[3]={.65f,.65f,.65f,1};
    auto r=PC::build(s.input());REQUIRE(block(*r,1).candidate==block(*r,3).id);
    s.source.face_colors[3]=s.source.face_colors[0];
    r=PC::build(s.input());REQUIRE(block(*r,1).candidate==block(*r,0).id);
}
TEST_CASE("Oversized cache indices never wrap to valid block members", "[PortraitColorBlocks]") {
    Sample s;auto r=PC::build(s.input());auto doc=PC::encode(*r);
    const int part=GENERATE(0,1,2,3,4);
    const uint64_t huge=uint64_t(1)<<32;
    if(part==0)doc["units"][0][1]=huge;
    if(part==1)doc["tracks"][0]["unit_blocks"][0]=huge;
    if(part==2)doc["tracks"][0]["blocks"][0]["members"][0]=huge;
    if(part==3)doc["active_track"]=huge;
    if(part==4)doc["tracks"][0]["blocks"][0]["semantic"]=huge;
    std::string error;REQUIRE_FALSE(PC::decode(doc,s.source.geometry_id,4,s.source.content_id,s.analysis.signature,r->palette_signature,error));
}
TEST_CASE("High confidence midpoint evidence keeps small anatomy seeds as separate units", "[PortraitColorBlocks]") {
    Sample s;s.analysis.subface_labels.push_back({1,{4,255},SC::Label::Iris,.95f});
    auto r=PC::build(s.input());
    const auto found=std::find_if(r->units.begin(),r->units.end(),[](const auto& u){return u.face==1 && u.path.depth==4 && u.path.value==255;});
    REQUIRE(found!=r->units.end());
    REQUIRE(r->current().blocks[r->current().unit_blocks[found-r->units.begin()]].semantic==SC::Label::Iris);
    std::string error;REQUIRE(PC::decode(PC::encode(*r),s.source.geometry_id,4,s.source.content_id,s.analysis.signature,r->palette_signature,error));
}
TEST_CASE("A longer shared boundary wins over a closer brighter color", "[PortraitColorBlocks]") {
    Sample s;s.source.mesh.vertices[4]={-3,2,0};
    s.source.face_colors[0]={.6f,.6f,.6f,1};s.source.face_colors[3]={.95f,.95f,.95f,1};
    s.analysis.face_labels[0]=SC::Label::FaceSkin;s.analysis.face_labels[3]=SC::Label::BodySkin;
    auto r=PC::build(s.input());
    REQUIRE(block(*r,1).candidate==block(*r,3).id);
    REQUIRE(block(*r,1).semantic==SC::Label::Unknown);
}
TEST_CASE("Confirmed anatomy boundaries never merge equal colors", "[PortraitColorBlocks]") {
    Sample s;s.analysis.face_labels={SC::Label::Iris,SC::Label::EyeSclera,SC::Label::Lips,SC::Label::FaceSkin};
    auto r=PC::build(s.input());REQUIRE(r->current().blocks.size()==4);
    for(size_t i=0;i<4;++i)REQUIRE(block(*r,i).semantic==s.analysis.face_labels[i]);
}
TEST_CASE("Fractional cache identities never become triangle or block indices", "[PortraitColorBlocks]") {
    Sample s;auto r=PC::build(s.input());auto doc=PC::encode(*r);
    const int part=GENERATE(0,1,2);
    if(part==0)doc["units"][0][0]=.5;
    if(part==1)doc["tracks"][0]["unit_blocks"][0]=.5;
    if(part==2)doc["tracks"][0]["blocks"][0]["members"][0]=.5;
    std::string error;REQUIRE_FALSE(PC::decode(doc,s.source.geometry_id,4,s.source.content_id,s.analysis.signature,r->palette_signature,error));
}
TEST_CASE("One million connected triangles stay within the retained color block budget", "[PortraitColorBlocksPerformance]") {
    Sample s;constexpr int w=1000,h=500;
    s.source.mesh.vertices.clear();s.source.mesh.indices.clear();
    for(int y=0;y<=h;++y)for(int x=0;x<=w;++x)s.source.mesh.vertices.emplace_back(float(x),float(y),0.f);
    for(int y=0;y<h;++y)for(int x=0;x<w;++x) {
        const int a=y*(w+1)+x;s.source.mesh.indices.emplace_back(a,a+1,a+w+1);s.source.mesh.indices.emplace_back(a+1,a+w+2,a+w+1);
    }
    const auto n=s.source.mesh.indices.size();s.source.face_colors.assign(n,{.4f,.4f,.4f,1});
    s.analysis.face_labels.assign(n,SC::Label::Unknown);s.analysis.face_confidence.assign(n,.95f);
    const auto start=std::chrono::steady_clock::now();const auto r=PC::build(s.input());
    const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    std::cout<<"PortraitColorBlocks faces="<<n<<" elapsed_ms="<<ms<<" retained_bytes="<<r->memory_bytes()<<std::endl;
    REQUIRE(r->units.size()==n);REQUIRE(r->current().blocks.size()==1);
    REQUIRE(r->memory_bytes()<256ULL*1024*1024);
}
