
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "libslic3r/Model.hpp"
#include "libslic3r/Format/3mf.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "libslic3r/Zipper.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/MultiNozzleUtils.hpp"
#include "libslic3r/ProjectTask.hpp"
#include <miniz.h>

#include "test_utils.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/Geometry.hpp"
#include <cstddef>
#include "libslic3r/Point.hpp"
#include "libslic3r/Utils.hpp"
#include <miniz.h>
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/TextureDisplacement.hpp"
#include <memory>
#include <cstdint>
#include <ios>
#include <nlohmann/json.hpp>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/file_status.hpp>   // no_perms, owner_all
#include <boost/nowide/fstream.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <algorithm>
#include <functional>
#include <initializer_list>

#include <fstream>
#ifndef _WIN32
#include <unistd.h> // geteuid
#endif

#include <catch2/catch_tostring.hpp>
#include <Eigen/Core>
#include <Eigen/Geometry>
#include <string>
#include <sstream>
#include <type_traits> // for std::enable_if_t
#include <typeinfo>    // for typeid
#include <array>
#include <algorithm>
#include <chrono>
#include <charconv>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <thread>
#include <atomic>
#include <stdexcept>
#include <regex>
#include <boost/filesystem/fstream.hpp>
#include <nlohmann/json.hpp>

namespace Catch {
    template <typename T>
    struct is_eigen_matrix : std::is_base_of<Eigen::MatrixBase<T>, T> {};

    template <typename T>
    struct StringMaker<T, std::enable_if_t<is_eigen_matrix<T>::value>> {
        static std::string convert(const T& eigen_obj) {
            // Newline at end of rows
            Eigen::IOFormat fmt(4, 0, ", ", "\n", "[", "]");
            std::stringstream ss;
            ss << "Matrix<" << typeid(eigen_obj).name() << "> = \n";
            ss << eigen_obj.format(fmt);
            return ss.str();
        }
    };
    
    // We must manually specialize for Eigen::Transform as it doesn't derive from MatrixBase.
    // It's defined as: Eigen::Transform<Scalar, Dim, Mode, Options>
    template <typename Scalar, int Dim, int Mode, int Options>
    struct StringMaker<Eigen::Transform<Scalar, Dim, Mode, Options>> {
        static std::string convert(const Eigen::Transform<Scalar, Dim, Mode, Options>& trafo) {
            // We print the underlying matrix 
            const auto& matrix = trafo.matrix();

            // Newline at end of rows
            Eigen::IOFormat fmt(4, 0, ", ", "\n", "[", "]");
            std::stringstream ss;
            
            ss << "Transform<Mode=" << Mode << ", Dim=" << Dim << "> = \n"; 
            ss << matrix.format(fmt);
            return ss.str();
        }
    };
    
    // Quaternions also need an explicit specialization
    template <typename Scalar, int Options>
    struct StringMaker<Eigen::Quaternion<Scalar, Options>> {
        static std::string convert(const Eigen::Quaternion<Scalar, Options>& quat) {
            std::stringstream ss;
            ss << "Quaternion(w=" << quat.w() << ", x=" << quat.x() << ", y=" << quat.y() << ", z=" << quat.z() << ")";
            return ss.str();
        }
    };
} // end namespace Catch

#include <catch2/catch_all.hpp>

using namespace Slic3r;

TEST_CASE("Removed continuous-color projects cannot silently use ordinary slicing", "[3mf][LegacyImageMap]")
{
    Model model;
    REQUIRE(load_stl((std::string(TEST_DATA_DIR)+"/test_3mf/Prusa.stl").c_str(), &model));
    model.add_default_instances();
    ScopedTemporaryDir backup("removed_mode"); model.set_backup_path(backup.string());
    ScopedTemporaryFile file(".3mf"); const auto path=file.string();
    DynamicPrintConfig config=DynamicPrintConfig::full_print_config();
    PlateData plate;plate.plate_index=0;
    StoreParams params;params.path=path.c_str();params.model=&model;params.config=&config;
    params.strategy=SaveStrategy::Zip64|SaveStrategy::Silence;params.plate_data_list.push_back(&plate);
    REQUIRE(store_bbs_3mf(params));
    REQUIRE(mz_zip_add_mem_to_archive_file_in_place(path.c_str(),"Metadata/ImageMap/legacy.bin","x",1,nullptr,0,MZ_DEFAULT_COMPRESSION));
    Model restored;ScopedTemporaryDir restored_backup("removed_mode_restore");restored.set_backup_path(restored_backup.string());
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Enable};
    PlateDataPtrs plates;std::vector<Preset*> presets;bool bbl=false,orca=false;Semver version;
    const bool loaded=load_bbs_3mf(path.c_str(),&config,&substitutions,&restored,&plates,&presets,&bbl,&orca,&version,nullptr,
        LoadStrategy::LoadModel|LoadStrategy::LoadConfig);
    release_PlateData_list(plates);for(auto* preset:presets)delete preset;
    CHECK_FALSE(loaded);
}


SCENARIO("Reading 3mf file", "[3mf]") {
    GIVEN("umlauts in the path of the file") {
        Model model;
        WHEN("3mf model is read") {
            std::string path = std::string(TEST_DATA_DIR) + "/test_3mf/Geräte/Büchse.3mf";
            DynamicPrintConfig config;
            ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Disable };
            bool ret = load_3mf(path.c_str(), config, ctxt, &model, false);
            THEN("load should succeed") {
                REQUIRE(ret);
            }
        }
    }
}

SCENARIO("Export+Import geometry to/from 3mf file cycle", "[3mf]") {
    GIVEN("world vertices coordinates before save") {
        // load a model from stl file
        Model src_model;
        std::string src_file = std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl";
        load_stl(src_file.c_str(), &src_model);
        src_model.add_default_instances();

        ModelObject* src_object = src_model.objects.front();

        // apply generic transformation to the 1st volume
        Geometry::Transformation src_volume_transform;
        src_volume_transform.set_offset({ 10.0, 20.0, 0.0 });
        src_volume_transform.set_rotation({ Geometry::deg2rad(25.0), Geometry::deg2rad(35.0), Geometry::deg2rad(45.0) });
        src_volume_transform.set_scaling_factor({ 1.1, 1.2, 1.3 });
        src_volume_transform.set_mirror({ -1.0, 1.0, -1.0 });
        src_object->volumes.front()->set_transformation(src_volume_transform);

        // apply generic transformation to the 1st instance
        Geometry::Transformation src_instance_transform;
        src_instance_transform.set_offset({ 5.0, 10.0, 0.0 });
        src_instance_transform.set_rotation({ Geometry::deg2rad(12.0), Geometry::deg2rad(13.0), Geometry::deg2rad(14.0) });
        src_instance_transform.set_scaling_factor({ 0.9, 0.8, 0.7 });
        src_instance_transform.set_mirror({ 1.0, -1.0, -1.0 });
        src_object->instances.front()->set_transformation(src_instance_transform);

        WHEN("model is saved+loaded to/from 3mf file") {
            ScopedTemporaryFile temp(".3mf");
            const std::string test_file = temp.string();
            store_3mf(test_file.c_str(), &src_model, nullptr, false);

            // load back the model from the 3mf file
            Model dst_model;
            DynamicPrintConfig dst_config;
            {
                ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Disable };
                load_3mf(test_file.c_str(), dst_config, ctxt, &dst_model, false);
            }

            // compare meshes
            TriangleMesh src_mesh = src_model.mesh();
            TriangleMesh dst_mesh = dst_model.mesh();

            bool res = src_mesh.its.vertices.size() == dst_mesh.its.vertices.size();
            if (res) {
                for (size_t i = 0; i < dst_mesh.its.vertices.size(); ++i) {
                    res &= dst_mesh.its.vertices[i].isApprox(src_mesh.its.vertices[i]);
                }
            }
            THEN("world vertices coordinates after load match") {
                REQUIRE(res);
            }
        }
    }
}

SCENARIO("Mirrored instance transforms survive 3mf round trips", "[3mf][Regression]") {
    const bool bbs_format = GENERATE(false, true);

    GIVEN("an instance with a valid mirrored transform") {
        Model model;
        const std::string src_file = std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl";
        REQUIRE(load_stl(src_file.c_str(), &model));
        model.add_default_instances();

        Transform3d mirrored = Transform3d::Identity();
        mirrored.linear() <<
             4.4408921e-16,  0.819152044,  0.573576436,
             1.0,           -4.4408921e-16, -1.11022302e-16,
            -5.55111512e-17, -0.573576436,  0.819152044;
        mirrored.translation() = Vec3d(700.41477, -169.930939, 63.392571);
        REQUIRE_THAT(mirrored.linear().determinant(), Catch::Matchers::WithinAbs(-1.0, 1e-8));
        model.objects.front()->instances.front()->set_transformation(Geometry::Transformation(mirrored));
        const TriangleMesh expected_mesh = model.mesh();

        WHEN("the model is stored and loaded") {
            ScopedTemporaryDir backup_dir("orca_mirrored_transform");
            model.set_backup_path(backup_dir.string());
            ScopedTemporaryFile temp(".3mf");
            const std::string test_file = temp.string();

            DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
            if (bbs_format) {
                StoreParams store_params;
                store_params.path     = test_file.c_str();
                store_params.model    = &model;
                store_params.config   = &config;
                store_params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
                REQUIRE(store_bbs_3mf(store_params));
            } else {
                REQUIRE(store_3mf(test_file.c_str(), &model, &config, false));
            }

            Model dst_model;
            ScopedTemporaryDir loaded_backup_dir("orca_mirrored_transform_loaded");
            dst_model.set_backup_path(loaded_backup_dir.string());
            DynamicPrintConfig dst_config;
            ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Enable };
            PlateDataPtrs dst_plates;
            std::vector<Preset*> project_presets;
            ScopeGuard cleanup([&dst_plates, &project_presets]() {
                release_PlateData_list(dst_plates);
                for (Preset *preset : project_presets)
                    delete preset;
            });
            if (bbs_format) {
                bool is_bbl_3mf = false, is_orca_3mf = false;
                Semver file_version;
                REQUIRE(load_bbs_3mf(test_file.c_str(), &dst_config, &ctxt, &dst_model, &dst_plates,
                                     &project_presets, &is_bbl_3mf, &is_orca_3mf, &file_version, nullptr,
                                     LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
            } else {
                REQUIRE(load_3mf(test_file.c_str(), dst_config, ctxt, &dst_model, false));
            }

            THEN("the mirrored transform and world geometry are preserved") {
                REQUIRE(dst_model.objects.size() == 1);
                REQUIRE(dst_model.objects.front()->instances.size() == 1);
                const Transform3d &loaded = dst_model.objects.front()->instances.front()->get_matrix();
                REQUIRE(loaded.linear().determinant() < 0.0);

                const TriangleMesh loaded_mesh = dst_model.mesh();
                REQUIRE(loaded_mesh.its.vertices.size() == expected_mesh.its.vertices.size());
                for (size_t i = 0; i < loaded_mesh.its.vertices.size(); ++i)
                    REQUIRE(loaded_mesh.its.vertices[i].isApprox(expected_mesh.its.vertices[i], 1e-5f));
            }
        }
    }
}

// The recipe is an opaque binary blob (CadDocument::serialize_recipe()), so the 3mf backend has
// to carry it byte-for-byte — no XML/text mangling, embedded NULs intact.
static std::string make_cad_recipe()
{
    // Built from an explicit length, not append(const char*), which would stop at the first
    // embedded NUL — the one thing this blob exists to prove survives the archive.
    static const char blob[] = "\x01" "RECIPE" "\0" "\xff\xfe\x00\x10" "cad-features-blob";
    return std::string(blob, sizeof(blob) - 1);
}

static const std::string CAD_RECIPE_ENTRY        = "Metadata/orca_cad.bin";
static const std::string LEGACY_CAD_RECIPE_ENTRY = "Metadata/SnapOrca_cad.bin";

// Pulls one named entry out of a 3mf archive; false when it is absent.
static bool read_cad_recipe_entry(const std::string& path, std::string& out,
                                  const std::string& entry = CAD_RECIPE_ENTRY)
{
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    REQUIRE(open_zip_reader(&zip, path));
    bool   found = false;
    mz_uint n    = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        std::string name(st.m_filename);
        std::replace(name.begin(), name.end(), '\\', '/');
        if (boost::algorithm::iequals(name, entry)) {
            out.resize(st.m_uncomp_size);
            found = mz_zip_reader_extract_to_mem(&zip, i, out.data(), out.size(), 0) != 0;
            break;
        }
    }
    close_zip_reader(&zip);
    return found;
}

// Rewrites the archive at `path`, letting `edit` change the name or data of each entry; returns
// whether `edit` reported a change for any of them. miniz cannot edit in place and
// open_zip_writer truncates, so the entries are held across the switch.
static bool rewrite_3mf_entries(const std::string& path, const std::function<bool(std::string& name, std::string& data)>& edit)
{
    std::vector<std::pair<std::string, std::string>> entries;
    bool changed = false;
    {
        mz_zip_archive zip;
        mz_zip_zero_struct(&zip);
        REQUIRE(open_zip_reader(&zip, path));
        mz_uint n = mz_zip_reader_get_num_files(&zip);
        for (mz_uint i = 0; i < n; ++i) {
            mz_zip_archive_file_stat st;
            REQUIRE(mz_zip_reader_file_stat(&zip, i, &st));
            if (st.m_is_directory) continue;
            std::string name(st.m_filename);
            std::replace(name.begin(), name.end(), '\\', '/');
            std::string data((size_t) st.m_uncomp_size, '\0');
            if (st.m_uncomp_size > 0)
                REQUIRE(mz_zip_reader_extract_to_mem(&zip, i, data.data(), data.size(), 0));
            changed |= edit(name, data);
            entries.emplace_back(std::move(name), std::move(data));
        }
        close_zip_reader(&zip);
    }

    Zipper out(path);
    for (const auto& e : entries)
        out.add_entry(e.first, e.second.data(), e.second.size());
    out.finalize();
    return changed;
}

// Rewrites the archive at `path` with the recipe entry back under the name it had before the
// rename, which is what every project saved by an earlier build looks like on disk. Generated
// rather than checked in because a whole project archive is not frozen evidence the way a bare
// recipe blob is -- it has to be whatever today's exporter writes, with only the name aged.
static void rename_cad_recipe_entry_to_legacy(const std::string& path)
{
    const bool renamed = rewrite_3mf_entries(path, [](std::string& name, std::string&) {
        if (!boost::algorithm::iequals(name, CAD_RECIPE_ENTRY))
            return false;
        name = LEGACY_CAD_RECIPE_ENTRY;
        return true;
    });
    // Without this the scenario would degrade silently into re-testing the new name if the
    // exporter's constant ever moved again: every load below would still pass.
    REQUIRE(renamed);
}

// Replaces the first occurrence of `from` in any entry whose name ends with `suffix`.
static bool replace_in_3mf_entry(const std::string& path, const std::string& suffix, const std::string& from, const std::string& to)
{
    bool replaced = false;
    rewrite_3mf_entries(path, [&](std::string& name, std::string& data) {
        if (replaced || !boost::algorithm::ends_with(name, suffix))
            return false;
        if (const size_t pos = data.find(from); pos != std::string::npos) {
            data.replace(pos, from.size(), to);
            replaced = true;
        }
        return replaced;
    });
    return replaced;
}

// Stores `model` as a one-plate project.
static void store_project(Model& model, const std::string& path, SaveStrategy strategy)
{
    ScopedTemporaryDir backup_dir("orca_paint_src");
    model.set_backup_path(backup_dir.string());

    DynamicPrintConfig cfg;
    PlateData          plate;
    plate.plate_index = 0;
    StoreParams sp;
    sp.path     = path.c_str();
    sp.model    = &model;
    sp.config   = &cfg;
    sp.strategy = strategy;
    sp.plate_data_list.push_back(&plate);
    REQUIRE(store_bbs_3mf(sp));
}

// Stores a one-plate project holding a cube whose first two facets are painted Extruder2 and
// Extruder3, which the exporter writes as paint_color="8" and paint_color="0C".
static void store_painted_cube(const std::string& path)
{
    Model        model;
    ModelObject* object = model.add_object();
    ModelVolume* volume = object->add_volume(make_cube(10., 10., 10.));
    object->add_instance();
    {
        TriangleSelector selector(volume->mesh());
        selector.set_facet(0, EnforcerBlockerType::Extruder2);
        selector.set_facet(1, EnforcerBlockerType::Extruder3);
        REQUIRE(volume->mmu_segmentation_facets.set(selector));
    }
    store_project(model, path, SaveStrategy::Zip64 | SaveStrategy::Silence);
}

// Loads `path` through the BBS importer into `model`, releasing the plates it returns. The
// importer stages metadata through `backup_dir`, which has to outlive the model.
static bool load_project(const std::string& path, Model& model, const ScopedTemporaryDir& backup_dir)
{
    model.set_backup_path(backup_dir.string());
    DynamicPrintConfig        config;
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Enable };
    PlateDataPtrs             plates;
    std::vector<Preset*>      project_presets;
    bool   is_bbl_3mf = false, is_orca_3mf = false;
    Semver file_version;
    const bool loaded = load_bbs_3mf(path.c_str(), &config, &ctxt, &model, &plates, &project_presets, &is_bbl_3mf,
                                     &is_orca_3mf, &file_version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig);
    release_PlateData_list(plates);
    return loaded;
}

TEST_CASE("A plate's G-code statistics are stored next to its G-code", "[3mf]")
{
    const bool with_statistics = GENERATE(false, true);
    ScopedTemporaryFile gcode(".gcode");
    save_string_file(gcode.path(), "G1 X10 Y10 E1\n");

    Model        model;
    ModelObject* object = model.add_object();
    object->add_volume(make_cube(10., 10., 10.));
    object->add_instance();
    ScopedTemporaryDir backup_dir("orca_statistics_src");
    model.set_backup_path(backup_dir.string());

    // A sliced plate's info reads the printer settings.
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    PlateData          plate;
    plate.plate_index     = 0;
    plate.gcode_file      = gcode.string();
    plate.is_sliced_valid = true;
    if (with_statistics)
        plate.statistics = R"({ "version": 1 })";
    ScopedTemporaryFile file(".3mf");
    const std::string   path = file.string();
    StoreParams         sp;
    sp.path     = path.c_str();
    sp.model    = &model;
    sp.config   = &cfg;
    sp.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::WithGcode;
    sp.plate_data_list.push_back(&plate);
    REQUIRE(store_bbs_3mf(sp));

    std::string stored;
    REQUIRE(read_cad_recipe_entry(path, stored, "Metadata/plate_1.gcode"));
    CHECK(read_cad_recipe_entry(path, stored, "Metadata/plate_1.statistics.json") == with_statistics);
    if (with_statistics)
        CHECK(stored == plate.statistics);
}

TEST_CASE("A project with a plate id below 1 fails to load", "[3mf][Regression]")
{
    const int plate_id = GENERATE(0, -1);
    INFO("plater_id " << plate_id);

    ScopedTemporaryFile temp(".3mf");
    store_painted_cube(temp.string());
    {
        ScopedTemporaryDir backup_dir("orca_plate_dst");
        Model              model;
        REQUIRE(load_project(temp.string(), model, backup_dir));
    }

    REQUIRE(replace_in_3mf_entry(temp.string(), "model_settings.config", "key=\"plater_id\" value=\"1\"",
                                 "key=\"plater_id\" value=\"" + std::to_string(plate_id) + "\""));
    ScopedTemporaryDir backup_dir("orca_plate_dst");
    Model              model;
    bool               loaded = true;
    REQUIRE_NOTHROW(loaded = load_project(temp.string(), model, backup_dir));
    REQUIRE_FALSE(loaded);
}

TEST_CASE("A project whose components reference themselves fails to load", "[3mf][Regression]")
{
    // One self-reference keeps the expansion going without ever reaching a mesh. A thousand also make
    // each expansion queue a thousand more, so the bound has to hold the work list, not just the loop.
    const int references = GENERATE(1, 1000);
    INFO("self-references " << references);

    ScopedTemporaryFile temp(".3mf");
    store_painted_cube(temp.string());

    // Point the component back at the object that holds it, repeated `references` times.
    REQUIRE(rewrite_3mf_entries(temp.string(), [references](std::string& name, std::string& data) {
        if (!boost::algorithm::ends_with(name, "3dmodel.model"))
            return false;
        std::smatch match;
        if (!std::regex_search(data, match, std::regex("<object id=\"([0-9]+)\"[^>]*>\\s*<components")))
            return false;
        data = std::regex_replace(data, std::regex("objectid=\"[0-9]+\""), "objectid=\"" + match[1].str() + "\"");
        std::smatch component;
        if (!std::regex_search(data, component, std::regex("<component [^>]*/>")))
            return false;
        std::string repeated;
        for (int i = 0; i < references; ++i)
            repeated += component.str();
        data.replace(component.position(), component.length(), repeated);
        return true;
    }));

    ScopedTemporaryDir backup_dir("orca_cycle_dst");
    Model              model;
    bool               loaded = true;
    REQUIRE_NOTHROW(loaded = load_project(temp.string(), model, backup_dir));
    REQUIRE_FALSE(loaded);
}

TEST_CASE("An object loads up to the component reference budget and fails past it", "[3mf][Regression]")
{
    // The importer queues at most 100000 component references per object. Every reference besides the
    // cube's own points at an object the file does not define: it counts toward the budget, then expands
    // to nothing, so the object stays a single part whatever the count.
    const auto [references, loads] = GENERATE(table<int, bool>({ { 100000, true }, { 100001, false } }));
    INFO("component references " << references);

    ScopedTemporaryFile temp(".3mf");
    store_painted_cube(temp.string());

    std::string dangling;
    for (int i = 1; i < references; ++i)
        dangling += "<component objectid=\"999999\"/>";
    REQUIRE(replace_in_3mf_entry(temp.string(), "3dmodel.model", "</components>", dangling + "</components>"));

    ScopedTemporaryDir backup_dir("orca_budget_dst");
    Model              model;
    bool               loaded = !loads;
    REQUIRE_NOTHROW(loaded = load_project(temp.string(), model, backup_dir));
    REQUIRE(loaded == loads);
    if (loads) {
        REQUIRE(model.objects.size() == 1);
        CHECK(model.objects.front()->volumes.size() == 1);
    }
}

TEST_CASE("A project with malformed paint data loads without the damaged facet", "[3mf][Regression]")
{
    ScopedTemporaryFile temp(".3mf");
    store_painted_cube(temp.string());
    // Split codes with no children behind them: the stream runs out mid-tree.
    REQUIRE(replace_in_3mf_entry(temp.string(), ".model", "paint_color=\"8\"", "paint_color=\"FFFFFFFFFFFFFFFF3\""));

    ScopedTemporaryDir backup_dir("orca_paint_dst");
    Model              model;
    REQUIRE(load_project(temp.string(), model, backup_dir));
    REQUIRE(model.objects.size() == 1);
    const ModelVolume& volume = *model.objects.front()->volumes.front();
    const auto&        data   = volume.mmu_segmentation_facets.get_data();
    REQUIRE_FALSE(data.used_states[size_t(EnforcerBlockerType::Extruder2)]);
    REQUIRE(data.used_states[size_t(EnforcerBlockerType::Extruder3)]);

    TriangleSelector selector(volume.mesh());
    REQUIRE_NOTHROW(selector.deserialize(data));
    REQUIRE(selector.num_facets(EnforcerBlockerType::Extruder2) == 0);
    REQUIRE(selector.num_facets(EnforcerBlockerType::Extruder3) == 1);
}

// Paints `triangles` of `volume` on texture displacement layer slot `slot`.
static void paint_texture_slot(ModelVolume& volume, int slot, std::initializer_list<int> triangles)
{
    TriangleSelector selector(volume.mesh());
    for (const int triangle : triangles)
        selector.set_facet(triangle, EnforcerBlockerType::ENFORCER);
    REQUIRE(volume.texture_displacement_facet(slot).set(selector));
}

// Every triangle's texture paint on every slot, in the form the exporter writes it, so two volumes
// compare equal only when the same triangles are painted on the same slots.
static std::vector<std::string> texture_paint(const ModelVolume& volume)
{
    std::vector<std::string> paint;
    for (int slot = 0; slot < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++slot)
        for (size_t i = 0; i < volume.mesh().its.indices.size(); ++i)
            paint.push_back(volume.texture_displacement_facet(slot).get_triangle_as_string(int(i)));
    return paint;
}

// A cube with one texture displacement layer in slot 2 and texture paint on gapped triangles of two
// slots. The layer's image bytes need not decode: the archive stores them as they are.
static ModelVolume& add_texture_displaced_cube(Model& model)
{
    ModelObject* object = model.add_object();
    ModelVolume* volume = object->add_volume(make_cube(10., 10., 10.));
    object->add_instance();

    TextureDisplacementLayer layer;
    layer.slot       = 2;
    layer.name       = "Bark";
    layer.depth_mm   = 1.25f;
    layer.image_data = std::make_shared<std::vector<unsigned char>>(std::vector<unsigned char>{ 0x89, 'P', 'N', 'G', 1, 2, 3 });
    volume->texture_displacement_layers.push_back(layer);
    volume->texture_displacement_options.active_slot = 2;

    paint_texture_slot(*volume, 2, { 3, 7 });
    paint_texture_slot(*volume, 5, { 10 });
    return *volume;
}

TEST_CASE("Texture displacement layers, textures and paint survive a project round trip", "[3mf]")
{
    // A single model file, and one file per object (read by a separate importer).
    const SaveStrategy strategy = GENERATE(SaveStrategy::Zip64 | SaveStrategy::Silence,
                                           SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SplitModel);
    INFO("save strategy " << int(strategy));

    ScopedTemporaryFile temp(".3mf");
    Model               source;
    const ModelVolume&  painted = add_texture_displaced_cube(source);
    const std::vector<std::string> paint = texture_paint(painted);
    REQUIRE(std::count_if(paint.begin(), paint.end(), [](const std::string& s) { return !s.empty(); }) == 3);
    store_project(source, temp.string(), strategy);

    ScopedTemporaryDir backup_dir("orca_texdisp_dst");
    Model              model;
    REQUIRE(load_project(temp.string(), model, backup_dir));
    REQUIRE(model.objects.size() == 1);
    const ModelVolume& volume = *model.objects.front()->volumes.front();

    REQUIRE(volume.texture_displacement_layers.size() == 1);
    const TextureDisplacementLayer& layer = volume.texture_displacement_layers.front();
    CHECK(layer.slot == 2);
    CHECK(layer.name == "Bark");
    CHECK_THAT(layer.depth_mm, Catch::Matchers::WithinAbs(1.25f, 1e-6f));
    REQUIRE(layer.image_data);
    CHECK(*layer.image_data == *painted.texture_displacement_layers.front().image_data);
    CHECK(volume.texture_displacement_options.active_slot == 2);
    CHECK(texture_paint(volume) == paint);
}

TEST_CASE("Volumes sharing a mesh keep their own texture displacement paint", "[3mf]")
{
    Model        source;
    ModelObject* first_object = source.add_object();
    ModelVolume* first        = first_object->add_volume(make_cube(10., 10., 10.));
    first_object->add_instance();
    ModelVolume* second = source.add_object(*first_object)->volumes.front();
    REQUIRE(second->mesh_ptr() == first->mesh_ptr());
    paint_texture_slot(*first, 0, { 3 });
    paint_texture_slot(*second, 0, { 7 });

    // What a project save from the app uses: one file per object, an identical mesh written once.
    ScopedTemporaryFile temp(".3mf");
    store_project(source, temp.string(),
                  SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SplitModel | SaveStrategy::ShareMesh);

    ScopedTemporaryDir backup_dir("orca_texdisp_dst");
    Model              model;
    REQUIRE(load_project(temp.string(), model, backup_dir));
    REQUIRE(model.objects.size() == 2);
    CHECK(texture_paint(*model.objects[0]->volumes.front()) == texture_paint(*first));
    CHECK(texture_paint(*model.objects[1]->volumes.front()) == texture_paint(*second));
}

TEST_CASE("A texture displacement layer never loads its image from outside the project", "[3mf]")
{
    // A file the layer names, on the machine opening the project, that the project did not carry.
    ScopedTemporaryFile texture(".png");
    {
        boost::nowide::ofstream file(texture.string(), std::ios::binary);
        file << "not part of the project";
    }
    REQUIRE(boost::filesystem::file_size(texture.path()) > 0);

    Model        source;
    ModelObject* object = source.add_object();
    ModelVolume* volume = object->add_volume(make_cube(10., 10., 10.));
    object->add_instance();
    TextureDisplacementLayer layer;
    layer.slot     = 0;
    layer.name     = "Elsewhere";
    layer.path     = texture.string();
    layer.depth_mm = 0.75f;
    volume->texture_displacement_layers.push_back(layer);

    ScopedTemporaryFile temp(".3mf");
    store_project(source, temp.string(), SaveStrategy::Zip64 | SaveStrategy::Silence);

    ScopedTemporaryDir backup_dir("orca_texdisp_dst");
    Model              model;
    REQUIRE(load_project(temp.string(), model, backup_dir));
    REQUIRE(model.objects.size() == 1);
    const std::vector<TextureDisplacementLayer>& layers = model.objects.front()->volumes.front()->texture_displacement_layers;
    REQUIRE(layers.size() == 1);
    CHECK(layers.front().name == "Elsewhere");
    CHECK_THAT(layers.front().depth_mm, Catch::Matchers::WithinAbs(0.75f, 1e-6f));
    CHECK_FALSE(layers.front().image_data);
}

TEST_CASE("A project with a malformed texture displacement value still loads", "[3mf][Regression]")
{
    const auto [from, to] = GENERATE(table<std::string, std::string>({
        { "\"islands\":[]", "\"islands\":[[null,0,0,1]]" }, // a number that is null
        { "\"slot\":2", "\"slot\":8" },                     // a slot past the last
    }));
    INFO(from << " -> " << to);

    ScopedTemporaryFile temp(".3mf");
    Model               source;
    const ModelVolume&  painted = add_texture_displaced_cube(source);
    store_project(source, temp.string(), SaveStrategy::Zip64 | SaveStrategy::Silence);
    REQUIRE(replace_in_3mf_entry(temp.string(), ".json", from, to));

    ScopedTemporaryDir backup_dir("orca_texdisp_dst");
    Model              model;
    bool               loaded = false;
    REQUIRE_NOTHROW(loaded = load_project(temp.string(), model, backup_dir));
    REQUIRE(loaded);
    REQUIRE(model.objects.size() == 1);
    const ModelVolume& volume = *model.objects.front()->volumes.front();
    // The layer stack is dropped as a whole; the paint, stored apart from it, still loads.
    CHECK(volume.texture_displacement_layers.empty());
    CHECK(texture_paint(volume) == texture_paint(painted));
}

// The recipe lives only in the BBS-native backend, because that is the only one that runs:
// store_bbs_3mf is the sole exporter the app calls, and 3mf.cpp's load_3mf is reached only for
// files fingerprinted as PrusaSlicer's, which never carry a recipe. This locks in both halves:
// the archive entry is at the exact path the importer looks for, and the recipe comes back
// through the real importer.
SCENARIO("CAD recipe is embedded in the BBS 3mf archive", "[3mf][CAD]") {
    GIVEN("a model carrying a binary cad_recipe") {
        Model model;
        std::string src = std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl";
        REQUIRE(load_stl(src.c_str(), &model));
        model.add_default_instances();

        // store_bbs_3mf stages its metadata through the model's backup path; point it at a
        // writable temp dir, as the sibling BBS scenarios do. The process-global
        // set_temporary_dir() would leak into every test that ran afterwards.
        ScopedTemporaryDir backup_dir("orca_cad");
        model.set_backup_path(backup_dir.string());

        const std::string recipe = make_cad_recipe();
        model.cad_recipe = recipe;

        WHEN("saved through the BBS backend (the format the GUI uses)") {
            ScopedTemporaryFile temp(".3mf");
            const std::string test_file = temp.string();

            DynamicPrintConfig cfg;
            StoreParams sp;
            sp.path     = test_file.c_str();
            sp.model    = &model;
            sp.config   = &cfg;
            sp.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
            REQUIRE(store_bbs_3mf(sp));

            THEN("the archive entry is present byte-for-byte") {
                std::string got;
                REQUIRE(read_cad_recipe_entry(test_file, got));
                REQUIRE(got.size() == recipe.size());
                REQUIRE(got == recipe);
            }

            THEN("the importer restores it onto the loaded model") {
                Model dst_model;
                ScopedTemporaryDir dst_backup_dir("orca_cad_dst");
                dst_model.set_backup_path(dst_backup_dir.string());

                DynamicPrintConfig dst_config;
                ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Enable };
                PlateDataPtrs        dst_plates;
                std::vector<Preset*> project_presets;
                bool   is_bbl_3mf = false, is_orca_3mf = false;
                Semver file_version;
                REQUIRE(load_bbs_3mf(test_file.c_str(), &dst_config, &ctxt, &dst_model, &dst_plates,
                                     &project_presets, &is_bbl_3mf, &is_orca_3mf, &file_version, nullptr,
                                     LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
                REQUIRE(dst_model.cad_recipe.size() == recipe.size());
                REQUIRE(dst_model.cad_recipe == recipe);

                release_PlateData_list(dst_plates);
            }
        }

        WHEN("the same model is saved with no recipe") {
            model.cad_recipe.clear();
            ScopedTemporaryFile temp(".3mf");
            const std::string test_file = temp.string();

            DynamicPrintConfig cfg;
            StoreParams sp;
            sp.path     = test_file.c_str();
            sp.model    = &model;
            sp.config   = &cfg;
            sp.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
            REQUIRE(store_bbs_3mf(sp));

            THEN("no entry is written at all") {
                std::string got;
                REQUIRE_FALSE(read_cad_recipe_entry(test_file, got));
            }
        }
    }
}

// The recipe entry was renamed from Metadata/SnapOrca_cad.bin to Metadata/orca_cad.bin. Nothing
// in the blob marks that move, so a reader that knows only the new name loads a project written
// before it with an empty cad_recipe and no error at all — a feature tree gone with no symptom
// but an empty Design tab. The importer must still accept the old name; the exporter may never
// write it.
SCENARIO("a project saved under the pre-rename recipe name still loads", "[3mf][CAD]") {
    GIVEN("a project whose recipe entry carries the old name") {
        Model model;
        std::string src = std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl";
        REQUIRE(load_stl(src.c_str(), &model));
        model.add_default_instances();
        const std::string recipe = make_cad_recipe();
        model.cad_recipe = recipe;

        WHEN("it was written by the BBS backend") {
            ScopedTemporaryDir backup_dir("orca_cad_legacy");
            model.set_backup_path(backup_dir.string());

            ScopedTemporaryFile temp(".3mf");
            const std::string test_file = temp.string();
            DynamicPrintConfig cfg;
            StoreParams sp;
            sp.path     = test_file.c_str();
            sp.model    = &model;
            sp.config   = &cfg;
            sp.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
            REQUIRE(store_bbs_3mf(sp));
            rename_cad_recipe_entry_to_legacy(test_file);

            // Catch2 replays the enclosing sections per THEN, so one load here serves both.
            Model dst_model;
            ScopedTemporaryDir dst_backup_dir("orca_cad_legacy_dst");
            dst_model.set_backup_path(dst_backup_dir.string());

            DynamicPrintConfig dst_config;
            ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Enable };
            PlateDataPtrs        dst_plates;
            std::vector<Preset*> project_presets;
            bool   is_bbl_3mf = false, is_orca_3mf = false;
            Semver file_version;
            REQUIRE(load_bbs_3mf(test_file.c_str(), &dst_config, &ctxt, &dst_model, &dst_plates,
                                 &project_presets, &is_bbl_3mf, &is_orca_3mf, &file_version, nullptr,
                                 LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
            release_PlateData_list(dst_plates);

            THEN("the recipe still comes back byte-for-byte") {
                REQUIRE(dst_model.cad_recipe == recipe);
            }

            THEN("re-saving migrates it to the new name and leaves the old one behind") {
                ScopedTemporaryFile again(".3mf");
                const std::string resaved = again.string();
                DynamicPrintConfig cfg2;
                StoreParams sp2;
                sp2.path     = resaved.c_str();
                sp2.model    = &dst_model;
                sp2.config   = &cfg2;
                sp2.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
                REQUIRE(store_bbs_3mf(sp2));

                std::string got;
                REQUIRE(read_cad_recipe_entry(resaved, got));
                REQUIRE(got == recipe);
                REQUIRE_FALSE(read_cad_recipe_entry(resaved, got, LEGACY_CAD_RECIPE_ENTRY));
            }
        }
    }
}

// .3mf multi-nozzle round-trip.
// Locks the load/save handling for the H2C multi-nozzle plate metadata:
//   * filament_volume_maps  -> plate config "filament_volume_map" (with the >1 -> 0 clamp)
//   * nozzle_volume_type    -> PlateData::nozzle_volume_types (previously write-only)
// and pins the deliberately-lossy keys (enable_filament_dynamic_map) so a future change has to
// consciously unpin them. Uses a store_bbs_3mf -> load_bbs_3mf cycle (no external fixture needed).
SCENARIO("H2C multi-nozzle .3mf round-trip", "[3mf][MultiNozzle]") {
    GIVEN("a plate carrying multi-nozzle filament assignment metadata") {
        Model model;
        std::string src_file = std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl";
        REQUIRE(load_stl(src_file.c_str(), &model));
        model.add_default_instances();

        // store_bbs_3mf stages Metadata/project_settings.config through the model's backup path;
        // point it at a writable temp dir (the default lives under a read-only root in CI).
        ScopedTemporaryDir backup_dir("orca_mn");
        model.set_backup_path(backup_dir.string());

        // Global (printer) config: give nozzle_volume_type a non-default value so the slice_info
        // read-back is a meaningful assertion (High Flow == 1).
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_key_value("nozzle_volume_type",
                             new ConfigOptionEnumsGeneric({ (int) NozzleVolumeType::nvtHighFlow }));

        PlateData* plate = new PlateData();
        plate->plate_index      = 0;
        plate->is_sliced_valid  = true; // gate for the slice_info.config writer (nozzle_volume_type)
        plate->filament_maps    = { 1, 2, 1 }; // slice_info uses this; keep it == model_settings' value
        plate->config.set_key_value("filament_map_mode", new ConfigOptionEnum<FilamentMapMode>(fmmManual));
        plate->config.set_key_value("filament_map", new ConfigOptionInts({ 1, 2, 1 }));
        // Deliberately include out-of-range volume-type ids (2 == Hybrid, 3 == TPU High Flow):
        // the loader must clamp them back to Standard (0).
        plate->config.set_key_value("filament_volume_map", new ConfigOptionInts({ 0, 2, 1, 3 }));
        // Known-lossy: a true value must NOT survive the round-trip (slice_info hardcodes false,
        // model_settings never writes it).
        plate->config.set_key_value("enable_filament_dynamic_map", new ConfigOptionBool(true));

        WHEN("stored to and reloaded from a .3mf") {
            ScopedTemporaryFile temp(".3mf");
            const std::string test_file = temp.string();

            StoreParams store_params;
            store_params.path    = test_file.c_str();
            store_params.model   = &model;
            store_params.config  = &config;
            store_params.plate_data_list.push_back(plate);
            store_params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
            REQUIRE(store_bbs_3mf(store_params));

            Model dst_model;
            DynamicPrintConfig dst_config;
            ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Enable };
            PlateDataPtrs        dst_plates;
            std::vector<Preset*> project_presets;
            bool   is_bbl_3mf = false, is_orca_3mf = false;
            Semver file_version;
            // LoadConfig is required for slice_info.config (nozzle_volume_type) to be parsed —
            // matches how the app loads projects.
            bool loaded = load_bbs_3mf(test_file.c_str(), &dst_config, &ctxt, &dst_model, &dst_plates,
                                       &project_presets, &is_bbl_3mf, &is_orca_3mf, &file_version, nullptr,
                                       LoadStrategy::LoadModel | LoadStrategy::LoadConfig);
            THEN("every multi-nozzle key round-trips as expected") {
                REQUIRE(loaded);
                REQUIRE(dst_plates.size() >= 1);
                PlateData* rt = dst_plates.front();

                // filament_map (model_settings + slice_info; already round-tripped)
                auto* fmap = rt->config.option<ConfigOptionInts>("filament_map");
                REQUIRE(fmap != nullptr);
                REQUIRE(fmap->values == std::vector<int>({ 1, 2, 1 }));

                // filament_volume_map (model_settings) with the >1 -> 0 clamp
                auto* fvmap = rt->config.option<ConfigOptionInts>("filament_volume_map");
                REQUIRE(fvmap != nullptr);
                REQUIRE(fvmap->values == std::vector<int>({ 0, 0, 1, 0 }));

                // nozzle_volume_type read-back into PlateData::nozzle_volume_types
                REQUIRE(rt->nozzle_volume_types == "1");

                // enable_filament_dynamic_map pinned lossy: model_settings never serializes it and
                // slice_info hardcodes false, so the `true` we set is dropped. Pinned here
                // (absent or false, never true) so a future change that persists it must update this.
                auto* dyn = rt->config.option<ConfigOptionBool>("enable_filament_dynamic_map");
                const bool persisted_true = (dyn != nullptr && dyn->value);
                REQUIRE_FALSE(persisted_true);
            }

            release_PlateData_list(dst_plates);
        }
        delete plate; // store_bbs_3mf does not take ownership of the source plate
    }
}

// Saved nozzle diameter for a single-nozzle-per-extruder printer with a non-standard nozzle.
// The grouping result rounds every nozzle diameter to the nearest of {0.2,0.4,0.6,0.8} for its
// internal matching key. That rounded value must NOT reach the saved <filament>/<nozzle> metadata on
// a printer whose extruders each carry one nozzle: the exact per-extruder config diameter is written
// instead, so a 0.5 mm nozzle is preserved rather than saved as 0.4. (Only an extruder that carries a
// nozzle cluster, which the per-extruder config cannot express, keeps the grouping result's diameter.)
SCENARIO("Non-standard nozzle diameter survives .3mf save on a single-nozzle printer", "[3mf][MultiNozzle]") {
    GIVEN("a single-extruder plate whose nozzle is 0.5 mm and whose stamped diameter was rounded to 0.4") {
        Model model;
        std::string src_file = std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl";
        REQUIRE(load_stl(src_file.c_str(), &model));
        model.add_default_instances();

        ScopedTemporaryDir backup_dir("orca_nd");
        model.set_backup_path(backup_dir.string());

        // Single extruder with a non-standard 0.5 mm nozzle; extruder_max_nozzle_count stays at its
        // default (no nozzle cluster), so the writer must emit the exact config diameter.
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_key_value("nozzle_diameter", new ConfigOptionFloats({ 0.5 }));

        PlateData* plate = new PlateData();
        plate->plate_index     = 0;
        plate->is_sliced_valid = true;      // gate for the slice_info.config writer
        plate->filament_maps   = { 1 };

        // Seed the stamped diameter with the grouping result's rounded value (0.5 -> 0.4) so the
        // assertion proves the writer ignores it and emits the exact config diameter instead.
        FilamentInfo fi;
        fi.id              = 0;
        fi.type            = "PLA";
        fi.color           = "#FFFFFFFF";
        fi.group_id        = { 0 };
        fi.nozzle_diameter = 0.4; // rounded; must NOT be the value written
        plate->slice_filaments_info.push_back(fi);

        WHEN("stored to and reloaded from a .3mf") {
            ScopedTemporaryFile temp(".3mf");
            const std::string test_file = temp.string();

            StoreParams store_params;
            store_params.path    = test_file.c_str();
            store_params.model   = &model;
            store_params.config  = &config;
            store_params.plate_data_list.push_back(plate);
            store_params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
            REQUIRE(store_bbs_3mf(store_params));

            Model dst_model;
            DynamicPrintConfig dst_config;
            ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Enable };
            PlateDataPtrs        dst_plates;
            std::vector<Preset*> project_presets;
            bool   is_bbl_3mf = false, is_orca_3mf = false;
            Semver file_version;
            bool loaded = load_bbs_3mf(test_file.c_str(), &dst_config, &ctxt, &dst_model, &dst_plates,
                                       &project_presets, &is_bbl_3mf, &is_orca_3mf, &file_version, nullptr,
                                       LoadStrategy::LoadModel | LoadStrategy::LoadConfig);
            THEN("the saved nozzle diameter is the exact 0.5, not the rounded 0.4") {
                REQUIRE(loaded);
                REQUIRE(dst_plates.size() >= 1);
                PlateData* rt = dst_plates.front();

                // <nozzle> tag: device-facing per-nozzle diameter string, written verbatim.
                REQUIRE(rt->nozzles_info.size() >= 1);
                REQUIRE(rt->nozzles_info.front().diameter == "0.5");

                // <filament> tag: per-filament nozzle_diameter parsed back as 0.5, not 0.4.
                REQUIRE(rt->slice_filaments_info.size() >= 1);
                REQUIRE_THAT(rt->slice_filaments_info.front().nozzle_diameter, Catch::Matchers::WithinAbs(0.5, 1e-6));
            }

            release_PlateData_list(dst_plates);
        }
        delete plate; // store_bbs_3mf does not take ownership of the source plate
    }
}

// A legacy / foreign project (no multi-nozzle metadata) must load crash-safe through the BBS
// importer and must not fabricate a filament_volume_map.
SCENARIO("Legacy project loads crash-safe via load_bbs_3mf", "[3mf][MultiNozzle]") {
    GIVEN("a project without any multi-nozzle metadata") {
        std::string path = std::string(TEST_DATA_DIR) + "/test_3mf/Geräte/Büchse.3mf";
        Model                model;
        DynamicPrintConfig   config;
        ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Enable };
        PlateDataPtrs        plates;
        std::vector<Preset*> project_presets;
        bool   is_bbl_3mf = false, is_orca_3mf = false;
        Semver file_version;

        WHEN("loaded through the BBS importer") {
            bool loaded = false;
            REQUIRE_NOTHROW(loaded = load_bbs_3mf(path.c_str(), &config, &ctxt, &model, &plates,
                                                  &project_presets, &is_bbl_3mf, &is_orca_3mf,
                                                  &file_version, nullptr,
                                                  LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
            THEN("it does not crash and invents no per-filament volume map") {
                for (PlateData* p : plates) {
                    REQUIRE(p->config.option<ConfigOptionInts>("filament_volume_map") == nullptr);
                }
            }
            release_PlateData_list(plates);
        }
    }
}

// Device-side nozzle-grouping serialization surface.
// Direct unit coverage for the pure serialize/deserialize + StaticNozzleGroupResult helpers that the
// gcode.3mf writer/reader lean on.
SCENARIO("MultiNozzle serialization helpers", "[3mf][MultiNozzle]") {
    using namespace Slic3r::MultiNozzleUtils;

    GIVEN("NozzleInfo / NozzleGroupInfo") {
        NozzleInfo n0; n0.group_id = 0; n0.extruder_id = 0; n0.diameter = "0.4"; n0.volume_type = nvtStandard;
        NozzleInfo n1; n1.group_id = 1; n1.extruder_id = 1; n1.diameter = "0.4"; n1.volume_type = nvtHighFlow;

        THEN("NozzleInfo::serialize matches the <nozzle> tag attributes (extruder_id 1-based)") {
            REQUIRE(n0.serialize() == "id=\"0\" extruder_id=\"1\" nozzle_diameter=\"0.4\" volume_type=\"Standard\"");
            REQUIRE(n1.serialize() == "id=\"1\" extruder_id=\"2\" nozzle_diameter=\"0.4\" volume_type=\"High Flow\"");
        }
        THEN("NozzleGroupInfo serialize/deserialize round-trips and rejects malformed input") {
            NozzleGroupInfo g("0.4", nvtHighFlow, 1, 3);
            REQUIRE(g.serialize() == "1-0.4-High Flow-3");
            auto rt = NozzleGroupInfo::deserialize(g.serialize());
            REQUIRE(rt.has_value());
            REQUIRE(*rt == g);
            REQUIRE_FALSE(NozzleGroupInfo::deserialize("1-0.4-Standard").has_value()); // too few tokens
            REQUIRE_FALSE(NozzleGroupInfo::deserialize("x-0.4-Standard-3").has_value()); // non-numeric extruder
        }
    }

    GIVEN("a StaticNozzleGroupResult built from filament + nozzle infos") {
        std::vector<NozzleInfo> nozzles;
        { NozzleInfo n; n.group_id = 0; n.extruder_id = 0; n.diameter = "0.4"; n.volume_type = nvtStandard; nozzles.push_back(n); }
        { NozzleInfo n; n.group_id = 1; n.extruder_id = 1; n.diameter = "0.4"; n.volume_type = nvtHighFlow; nozzles.push_back(n); }

        std::vector<FilamentInfo> filaments(3);
        filaments[0].id = 0; filaments[0].group_id = { 0 };
        filaments[1].id = 1; filaments[1].group_id = { 1 };
        filaments[2].id = 2; filaments[2].group_id = { 0, 1 };

        auto result = StaticNozzleGroupResult::create(filaments, nozzles, { 0, 1, 2 }, { 0, 1, 0 }, false);
        REQUIRE(result.has_value());

        THEN("filament->nozzle queries resolve to the stored mapping") {
            REQUIRE(result->get_extruder_count() == 2);
            REQUIRE(result->get_used_extruders() == std::vector<int>({ 0, 1 }));
            REQUIRE(result->get_used_filaments() == std::vector<unsigned int>({ 0, 1, 2 }));
            REQUIRE(result->get_nozzles_for_filament(0).size() == 1);
            REQUIRE(result->get_nozzles_for_filament(2).size() == 2);
            // first-use resolves through the (filament,nozzle) change sequences.
            auto first = result->get_first_nozzle_for_filament(1);
            REQUIRE(first.has_value());
            REQUIRE(first->group_id == 1);
        }
        THEN("empty inputs yield nullopt") {
            REQUIRE_FALSE(StaticNozzleGroupResult::create({}, nozzles, {}, {}, false).has_value());
            REQUIRE_FALSE(StaticNozzleGroupResult::create(filaments, {}, {}, {}, false).has_value());
        }
    }

    GIVEN("load_nozzle_infos_with_compatibility fallbacks") {
        std::vector<NozzleInfo> new_format;
        { NozzleInfo n; n.group_id = 1; n.extruder_id = 1; n.diameter = "0.4"; n.volume_type = nvtHighFlow; new_format.push_back(n); }
        { NozzleInfo n; n.group_id = 0; n.extruder_id = 0; n.diameter = "0.4"; n.volume_type = nvtStandard; new_format.push_back(n); }

        THEN("new-format <nozzle> tags are returned sorted by logical id") {
            auto out = load_nozzle_infos_with_compatibility(new_format, {}, {}, {}, {});
            REQUIRE(out.size() == 2);
            REQUIRE(out[0].group_id == 0);
            REQUIRE(out[1].group_id == 1);
        }
        THEN("oldest single-nozzle 3mf (no tags, no filament group_id) rebuilds from diameters/volume types") {
            std::vector<NozzleVolumeType> vt = { nvtStandard, nvtHighFlow };
            std::vector<double>           dia = { 0.4, 0.4 };
            auto out = load_nozzle_infos_with_compatibility({}, {}, {}, vt, dia);
            REQUIRE(out.size() == 2);
            REQUIRE(out[0].extruder_id == 0);
            REQUIRE(out[0].volume_type == nvtStandard);
            REQUIRE(out[1].volume_type == nvtHighFlow);
        }
    }
}

// The layer-aware grouping result must survive the gcode.3mf write/read as
// <nozzle> tags and the enable_filament_dynamic_map flag. Proves the parse_filament_info stamping,
// the NOZZLE_TAG writer, the _handle_config_nozzle reader, and the nozzles_info plate copy.
SCENARIO("Nozzle-group metadata .3mf round-trip", "[3mf][MultiNozzle]") {
    GIVEN("a plate carrying a two-nozzle LayeredNozzleGroupResult") {
        Model model;
        std::string src_file = std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl";
        REQUIRE(load_stl(src_file.c_str(), &model));
        model.add_default_instances();

        ScopedTemporaryDir backup_dir("orca_ng");
        model.set_backup_path(backup_dir.string());

        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();

        std::vector<MultiNozzleUtils::NozzleInfo> nozzles;
        { MultiNozzleUtils::NozzleInfo n; n.group_id = 0; n.extruder_id = 0; n.diameter = "0.4"; n.volume_type = NozzleVolumeType::nvtStandard; nozzles.push_back(n); }
        { MultiNozzleUtils::NozzleInfo n; n.group_id = 1; n.extruder_id = 1; n.diameter = "0.4"; n.volume_type = NozzleVolumeType::nvtHighFlow; nozzles.push_back(n); }
        auto group = MultiNozzleUtils::LayeredNozzleGroupResult::create(
            std::vector<int>{ 0, 1, 0 }, nozzles, std::vector<unsigned int>{ 0, 1, 2 });
        REQUIRE(group.has_value());

        PlateData* plate = new PlateData();
        plate->plate_index     = 0;
        plate->is_sliced_valid = true;
        plate->filament_maps   = { 1, 2, 1 };
        plate->nozzle_group_result = group;
        plate->config.set_key_value("filament_map_mode", new ConfigOptionEnum<FilamentMapMode>(fmmManual));
        plate->config.set_key_value("filament_map", new ConfigOptionInts({ 1, 2, 1 }));

        WHEN("stored to and reloaded from a .3mf") {
            ScopedTemporaryFile temp(".3mf");
            const std::string test_file = temp.string();

            StoreParams store_params;
            store_params.path    = test_file.c_str();
            store_params.model   = &model;
            store_params.config  = &config;
            store_params.plate_data_list.push_back(plate);
            store_params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
            REQUIRE(store_bbs_3mf(store_params));

            Model dst_model;
            DynamicPrintConfig dst_config;
            ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Enable };
            PlateDataPtrs        dst_plates;
            std::vector<Preset*> project_presets;
            bool   is_bbl_3mf = false, is_orca_3mf = false;
            Semver file_version;
            bool loaded = load_bbs_3mf(test_file.c_str(), &dst_config, &ctxt, &dst_model, &dst_plates,
                                       &project_presets, &is_bbl_3mf, &is_orca_3mf, &file_version, nullptr,
                                       LoadStrategy::LoadModel | LoadStrategy::LoadConfig);
            THEN("the <nozzle> tags round-trip into the loaded plate's nozzles_info") {
                REQUIRE(loaded);
                REQUIRE(dst_plates.size() >= 1);
                PlateData* rt = dst_plates.front();

                REQUIRE(rt->nozzles_info.size() == 2);
                // reader stores extruder_id 0-based (tag is 1-based), diameter/volume_type preserved.
                std::sort(rt->nozzles_info.begin(), rt->nozzles_info.end());
                REQUIRE(rt->nozzles_info[0].group_id == 0);
                REQUIRE(rt->nozzles_info[0].extruder_id == 0);
                REQUIRE(rt->nozzles_info[0].diameter == "0.4");
                REQUIRE(rt->nozzles_info[0].volume_type == NozzleVolumeType::nvtStandard);
                REQUIRE(rt->nozzles_info[1].group_id == 1);
                REQUIRE(rt->nozzles_info[1].extruder_id == 1);
                REQUIRE(rt->nozzles_info[1].volume_type == NozzleVolumeType::nvtHighFlow);

                // A static (non-selector) result must persist enable_filament_dynamic_map = false.
                auto* dyn = rt->config.option<ConfigOptionBool>("enable_filament_dynamic_map");
                const bool persisted_true = (dyn != nullptr && dyn->value);
                REQUIRE_FALSE(persisted_true);
            }

            release_PlateData_list(dst_plates);
        }
        delete plate;
    }
}

SCENARIO("BBS 3MF round-trips per-plate IMEX state (parallel mode + head filament map)", "[3mf][IMEX]") {
    // Regression guard for the class of bug where per-plate state silently drops through
    // save/load (the variant-truncation bug was the precipitating example; IMEX plate state
    // rides the same XML metadata path and is equally vulnerable).
    //
    // The mode name is user-typed free text, so the values below deliberately carry every
    // character class that is special inside an XML attribute value:
    //   &  and <  must be escaped or the document is not well formed and the project will
    //             not load at all,
    //   "         must be escaped or it terminates the attribute early,
    //   tab       must be written as a numeric character reference, because XML normalizes
    //             literal whitespace in attribute values on read and the mode would be
    //             silently renamed,
    //   '  and >  are legal raw inside a double-quoted value and must survive untouched.
    // The head filament map value is machine generated in practice; it is given hostile
    // content here only to pin the escaping of the sibling attribute, so this asserts the
    // XML transport, not the map grammar (the loader stores the string verbatim).
    //
    // BBS exporter scaffolds a backup dir under temporary_dir() for the project config file;
    // point it at a writable location for the test process.
    set_temporary_dir(boost::filesystem::temp_directory_path().string());

    const std::string hostile_mode = "PLA & ABS <hot> \"2x\"\tcopy's mode";
    const std::string hostile_hfm  = "1:2,2:3 & <\"\tx>";

    GIVEN("A Model with a single object on plate 0 and IMEX plate state set") {
        Model src_model;
        std::string src_file = std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl";
        REQUIRE(load_stl(src_file.c_str(), &src_model));
        src_model.add_default_instances();

        DynamicPrintConfig src_config;

        PlateDataPtrs src_plates;
        auto *plate0 = new PlateData();
        plate0->plate_index = 0;
        plate0->config.set_key_value("imex_parallel_mode",     new ConfigOptionString(hostile_mode));
        plate0->config.set_key_value("imex_head_filament_map", new ConfigOptionString(hostile_hfm));
        src_plates.push_back(plate0);

        WHEN("the model is saved to BBS 3MF and loaded back") {
            std::string test_file = std::string(TEST_DATA_DIR) + "/test_3mf/imex_roundtrip.3mf";

            StoreParams store_params;
            store_params.path            = test_file.c_str();
            store_params.model           = &src_model;
            store_params.plate_data_list = src_plates;
            store_params.config          = &src_config;
            REQUIRE(store_bbs_3mf(store_params));

            Model dst_model;
            DynamicPrintConfig dst_config;
            PlateDataPtrs dst_plates;
            std::vector<Preset*> dst_presets;
            bool is_bbl = false;
            bool is_orca = false;
            Semver file_version;
            ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Disable };
            bool loaded = load_bbs_3mf(test_file.c_str(), &dst_config, &ctxt, &dst_model,
                                       &dst_plates, &dst_presets, &is_bbl, &is_orca, &file_version);
            boost::filesystem::remove(test_file);

            THEN("load succeeds") {
                REQUIRE(loaded);
            }
            THEN("the loaded plate list has the same number of plates") {
                REQUIRE(dst_plates.size() == src_plates.size());
            }
            THEN("imex_parallel_mode round-trips byte for byte, XML metacharacters included") {
                REQUIRE(dst_plates.size() >= 1);
                auto *mode_opt = dst_plates[0]->config.option<ConfigOptionString>("imex_parallel_mode");
                REQUIRE(mode_opt != nullptr);
                REQUIRE(mode_opt->value == hostile_mode);
            }
            THEN("imex_head_filament_map round-trips byte for byte, XML metacharacters included") {
                REQUIRE(dst_plates.size() >= 1);
                auto *hfm_opt = dst_plates[0]->config.option<ConfigOptionString>("imex_head_filament_map");
                REQUIRE(hfm_opt != nullptr);
                REQUIRE(hfm_opt->value == hostile_hfm);
            }

            release_PlateData_list(dst_plates);
        }

        release_PlateData_list(src_plates);
    }
}

SCENARIO("BBS 3MF round-trips distinct IMEX state across multiple plates", "[3mf][IMEX]") {
    // Guards against a plate-indexing regression where IMEX metadata lands on the wrong
    // plate or bleeds across plates on reload. Each plate carries distinct mode + head-
    // filament-map values; the reload must reproduce them in the same order.
    set_temporary_dir(boost::filesystem::temp_directory_path().string());

    GIVEN("A Model with two plates each carrying different IMEX state") {
        Model src_model;
        std::string src_file = std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl";
        REQUIRE(load_stl(src_file.c_str(), &src_model));
        src_model.add_default_instances();

        DynamicPrintConfig src_config;

        PlateDataPtrs src_plates;
        auto *plate0 = new PlateData();
        plate0->plate_index = 0;
        plate0->config.set_key_value("imex_parallel_mode",     new ConfigOptionString("copy_mode"));
        plate0->config.set_key_value("imex_head_filament_map", new ConfigOptionString("1:2"));
        src_plates.push_back(plate0);

        auto *plate1 = new PlateData();
        plate1->plate_index = 1;
        plate1->config.set_key_value("imex_parallel_mode",     new ConfigOptionString("mirror_mode"));
        plate1->config.set_key_value("imex_head_filament_map", new ConfigOptionString("2:4,3:5"));
        src_plates.push_back(plate1);

        WHEN("the model is saved to BBS 3MF and loaded back") {
            std::string test_file = std::string(TEST_DATA_DIR) + "/test_3mf/imex_multiplate_roundtrip.3mf";

            StoreParams store_params;
            store_params.path            = test_file.c_str();
            store_params.model           = &src_model;
            store_params.plate_data_list = src_plates;
            store_params.config          = &src_config;
            REQUIRE(store_bbs_3mf(store_params));

            Model dst_model;
            DynamicPrintConfig dst_config;
            PlateDataPtrs dst_plates;
            std::vector<Preset*> dst_presets;
            bool is_bbl = false;
            bool is_orca = false;
            Semver file_version;
            ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Disable };
            bool loaded = load_bbs_3mf(test_file.c_str(), &dst_config, &ctxt, &dst_model,
                                       &dst_plates, &dst_presets, &is_bbl, &is_orca, &file_version);
            boost::filesystem::remove(test_file);

            THEN("load succeeds and both plates are returned") {
                REQUIRE(loaded);
                REQUIRE(dst_plates.size() == 2);
            }
            THEN("plate 0 retains its own IMEX state (copy_mode, 1:2) and does not inherit plate 1's") {
                REQUIRE(dst_plates.size() == 2);
                auto *mode = dst_plates[0]->config.option<ConfigOptionString>("imex_parallel_mode");
                auto *hfm  = dst_plates[0]->config.option<ConfigOptionString>("imex_head_filament_map");
                REQUIRE(mode != nullptr);
                REQUIRE(hfm  != nullptr);
                REQUIRE(mode->value == "copy_mode");
                REQUIRE(hfm->value  == "1:2");
            }
            THEN("plate 1 retains its own IMEX state (mirror_mode, 2:4,3:5) and does not inherit plate 0's") {
                REQUIRE(dst_plates.size() == 2);
                auto *mode = dst_plates[1]->config.option<ConfigOptionString>("imex_parallel_mode");
                auto *hfm  = dst_plates[1]->config.option<ConfigOptionString>("imex_head_filament_map");
                REQUIRE(mode != nullptr);
                REQUIRE(hfm  != nullptr);
                REQUIRE(mode->value == "mirror_mode");
                REQUIRE(hfm->value  == "2:4,3:5");
            }

            release_PlateData_list(dst_plates);
        }

        release_PlateData_list(src_plates);
    }
}

SCENARIO("BBS 3MF does not emit IMEX metadata when plate is in primary mode", "[3mf][IMEX]") {
    // The serialization guard short-circuits when the mode is empty or "primary", so loading
    // a plate that was saved in primary mode must not leave a stale imex_parallel_mode option
    // on the plate's config. If this regressed, we'd see ghost "primary" strings appearing on
    // plates that had no IMEX state at all.
    set_temporary_dir(boost::filesystem::temp_directory_path().string());

    GIVEN("A Model with plate 0 in primary mode and an empty head-filament map") {
        Model src_model;
        std::string src_file = std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl";
        REQUIRE(load_stl(src_file.c_str(), &src_model));
        src_model.add_default_instances();

        DynamicPrintConfig src_config;
        PlateDataPtrs src_plates;
        auto *plate0 = new PlateData();
        plate0->plate_index = 0;
        plate0->config.set_key_value("imex_parallel_mode",     new ConfigOptionString("primary"));
        plate0->config.set_key_value("imex_head_filament_map", new ConfigOptionString(""));
        src_plates.push_back(plate0);

        WHEN("the model is saved to BBS 3MF and loaded back") {
            std::string test_file = std::string(TEST_DATA_DIR) + "/test_3mf/imex_primary_roundtrip.3mf";

            StoreParams store_params;
            store_params.path            = test_file.c_str();
            store_params.model           = &src_model;
            store_params.plate_data_list = src_plates;
            store_params.config          = &src_config;
            REQUIRE(store_bbs_3mf(store_params));

            Model dst_model;
            DynamicPrintConfig dst_config;
            PlateDataPtrs dst_plates;
            std::vector<Preset*> dst_presets;
            bool is_bbl = false;
            bool is_orca = false;
            Semver file_version;
            ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Disable };
            bool loaded = load_bbs_3mf(test_file.c_str(), &dst_config, &ctxt, &dst_model,
                                       &dst_plates, &dst_presets, &is_bbl, &is_orca, &file_version);
            boost::filesystem::remove(test_file);

            THEN("load succeeds") {
                REQUIRE(loaded);
            }
            THEN("the loaded plate has no imex_parallel_mode option set (primary is not serialized)") {
                REQUIRE(dst_plates.size() >= 1);
                auto *mode_opt = dst_plates[0]->config.option<ConfigOptionString>("imex_parallel_mode");
                REQUIRE(mode_opt == nullptr);
            }
            THEN("the loaded plate has no imex_head_filament_map option set (empty is not serialized)") {
                REQUIRE(dst_plates.size() >= 1);
                auto *hfm_opt = dst_plates[0]->config.option<ConfigOptionString>("imex_head_filament_map");
                REQUIRE(hfm_opt == nullptr);
            }

            release_PlateData_list(dst_plates);
        }

        release_PlateData_list(src_plates);
    }
}

TEST_CASE("Logical filaments beyond the nozzle count retain painting and assignments through a 3MF", "[3mf][MultiNozzle][SubfaceColor][Regression]")
{
    Model model;
    const std::string src_file = std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl";
    REQUIRE(load_stl(src_file.c_str(), &model));
    model.add_default_instances();
    ScopedTemporaryDir source_backup("logical_filaments_source");
    model.set_backup_path(source_backup.string());

    auto* object = model.objects.front();
    auto* volume = object->volumes.front();
    object->config.set_key_value("extruder", new ConfigOptionInt(5));
    volume->config.set_key_value("extruder", new ConfigOptionInt(5));
    const std::vector<EnforcerBlockerType> states{
        EnforcerBlockerType::Extruder5, EnforcerBlockerType::Extruder6,
        EnforcerBlockerType::Extruder7, EnforcerBlockerType::Extruder17};
    const size_t face_count = volume->mesh().its.indices.size();
    REQUIRE(face_count >= states.size());
    TriangleSelector selector(volume->mesh());
    for (size_t i = 0; i < face_count; ++i)
        selector.set_facet(static_cast<int>(i), states[i % states.size()]);
    REQUIRE(selector.set_facet_midpoint_subfaces(0, EnforcerBlockerType::Extruder5,
        {{1, 0, EnforcerBlockerType::Extruder6},
         {2, uint8_t((1u << 2) | 3u), EnforcerBlockerType::Extruder7},
         {4, 255, EnforcerBlockerType::Extruder17}}));
    REQUIRE(volume->mmu_segmentation_facets.set(selector));

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(4);
    config.set_num_filaments(17);
    const std::vector<std::string> colors{
        "#C3B7AB", "#4E4137", "#7C6F60", "#A59788", "#59675C", "#E4D7CB",
        "#2A2321", "#2F6B5E", "#3167A8", "#9A3F77", "#D86B42", "#312F29",
        "#624231", "#6A625A", "#915D45", "#B07A5C", "#D19D7A"};
    config.set_key_value("filament_colour", new ConfigOptionStrings(colors));
    config.set_key_value("single_extruder_multi_material", new ConfigOptionBool(false));
    config.set_key_value("printer_extruder_id", new ConfigOptionInts({1, 2, 3, 4}));

    ScopedTemporaryFile file(".3mf");
    const std::string path = file.string();
    PlateData plate;
    plate.plate_index = 0;
    StoreParams params;
    params.path = path.c_str();
    params.model = &model;
    params.config = &config;
    params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
    params.plate_data_list.push_back(&plate);
    REQUIRE(store_bbs_3mf(params));

    Model restored;
    ScopedTemporaryDir restored_backup("logical_filaments_restored");
    restored.set_backup_path(restored_backup.string());
    DynamicPrintConfig restored_config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Enable};
    PlateDataPtrs plates;
    std::vector<Preset*> presets;
    bool is_bbl = false, is_orca = false;
    Semver version;
    const bool loaded = load_bbs_3mf(path.c_str(), &restored_config, &substitutions, &restored,
        &plates, &presets, &is_bbl, &is_orca, &version, nullptr,
        LoadStrategy::LoadModel | LoadStrategy::LoadConfig);
    release_PlateData_list(plates);
    for (auto* preset : presets)
        delete preset;
    REQUIRE(loaded);

    const auto* restored_colors = restored_config.option<ConfigOptionStrings>("filament_colour");
    REQUIRE(restored_colors != nullptr);
    CHECK(restored_colors->values == colors);
    const auto* restored_nozzles = restored_config.option<ConfigOptionFloats>("nozzle_diameter");
    REQUIRE(restored_nozzles != nullptr);
    CHECK(restored_nozzles->values.size() == 4);
    REQUIRE(restored.objects.size() == 1);
    const auto* restored_object = restored.objects.front();
    REQUIRE(restored_object->volumes.size() == 1);
    const auto* restored_volume = restored_object->volumes.front();
    REQUIRE(restored_object->config.has("extruder"));
    CHECK(restored_object->config.extruder() == 5);
    // A single part inherits the object assignment after the importer's normalization.
    CHECK(restored_volume->extruder_id() == 5);
    REQUIRE(restored_volume->mesh().its.indices.size() == face_count);
    CHECK(restored_volume->mmu_segmentation_facets.get_data() == volume->mmu_segmentation_facets.get_data());
    for (size_t i = 0; i < face_count; ++i) {
        CAPTURE(i);
        CHECK(restored_volume->mmu_segmentation_facets.get_triangle_as_string(static_cast<int>(i)) ==
              volume->mmu_segmentation_facets.get_triangle_as_string(static_cast<int>(i)));
    }
    for (const auto state : states)
        CHECK(restored_volume->mmu_segmentation_facets.has_facets(*restored_volume, state));
}

// A mixed-color filament occupies an ordinary filament slot, and painting with it stores an
// ordinary extruder state: a project saved by BambuStudio encodes filament 5 of a 5-slot setup
// as paint state 5, with the mix described by the parallel filament_mixed_* project arrays.
SCENARIO("Mixed-color filament setup and painting round-trip through a .3mf", "[3mf][MixedFilament]") {
    GIVEN("a painted model whose project config describes a mixed filament in the last slot") {
        Model model;
        std::string src_file = std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl";
        REQUIRE(load_stl(src_file.c_str(), &model));
        model.add_default_instances();

        // Both the exporter and the importer stage Metadata/project_settings.config through the
        // model's backup path; point them at writable temp dirs.
        ScopedTemporaryDir backup_dir("orca_mixed_src");
        model.set_backup_path(backup_dir.string());

        ModelVolume* mv = model.objects.front()->volumes.front();
        {
            TriangleSelector selector(mv->mesh());
            selector.set_facet(0, EnforcerBlockerType::Extruder5); // the mixed slot
            selector.set_facet(1, EnforcerBlockerType::Extruder2);
            REQUIRE(mv->mmu_segmentation_facets.set(selector));
        }

        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_key_value("filament_colour", new ConfigOptionStrings(
            { "#00AE42", "#FFFF00", "#FF0000", "#0000FF", "#FF6A26" }));
        config.set_key_value("filament_is_mixed", new ConfigOptionBools(
            { false, false, false, false, true }));
        config.set_key_value("filament_mixed_components", new ConfigOptionStrings(
            { "", "", "", "", "3,2" }));
        config.set_key_value("filament_mixed_sublayer_ratios", new ConfigOptionStrings(
            { "", "", "", "", "0.4200,0.5800" }));

        WHEN("stored to and reloaded from a .3mf") {
            ScopedTemporaryFile temp(".3mf");
            const std::string test_file = temp.string();

            PlateData* plate = new PlateData();
            plate->plate_index = 0;

            StoreParams store_params;
            store_params.path     = test_file.c_str();
            store_params.model    = &model;
            store_params.config   = &config;
            store_params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
            store_params.plate_data_list.push_back(plate);
            REQUIRE(store_bbs_3mf(store_params));

            Model dst_model;
            ScopedTemporaryDir dst_backup_dir("orca_mixed_dst");
            dst_model.set_backup_path(dst_backup_dir.string());
            DynamicPrintConfig dst_config;
            ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Enable };
            PlateDataPtrs        dst_plates;
            std::vector<Preset*> project_presets;
            bool   is_bbl_3mf = false, is_orca_3mf = false;
            Semver file_version;
            REQUIRE(load_bbs_3mf(test_file.c_str(), &dst_config, &ctxt, &dst_model, &dst_plates,
                                 &project_presets, &is_bbl_3mf, &is_orca_3mf, &file_version, nullptr,
                                 LoadStrategy::LoadModel | LoadStrategy::LoadConfig));

            THEN("the mixed-filament project keys survive") {
                auto* is_mixed = dst_config.option<ConfigOptionBools>("filament_is_mixed");
                REQUIRE(is_mixed != nullptr);
                REQUIRE(is_mixed->values == std::vector<unsigned char>({ 0, 0, 0, 0, 1 }));

                auto* components = dst_config.option<ConfigOptionStrings>("filament_mixed_components");
                REQUIRE(components != nullptr);
                REQUIRE(components->values.size() == 5);
                REQUIRE(components->values[4] == "3,2");

                auto* ratios = dst_config.option<ConfigOptionStrings>("filament_mixed_sublayer_ratios");
                REQUIRE(ratios != nullptr);
                REQUIRE(ratios->values.size() == 5);
                REQUIRE(ratios->values[4] == "0.4200,0.5800");
            }

            THEN("the painted facets survive, including the one painted with the mixed slot") {
                REQUIRE(dst_model.objects.size() == 1);
                ModelVolume* dst_mv = dst_model.objects.front()->volumes.front();
                REQUIRE_FALSE(dst_mv->mmu_segmentation_facets.empty());
                REQUIRE(dst_mv->mmu_segmentation_facets.has_facets(*dst_mv, EnforcerBlockerType::Extruder2));
                REQUIRE(dst_mv->mmu_segmentation_facets.has_facets(*dst_mv, EnforcerBlockerType::Extruder5));
            }

            release_PlateData_list(dst_plates);
            delete plate; // store_bbs_3mf does not take ownership of the source plate
        }
    }
}

TEST_CASE("Repeated split project saves preserve embedded G-code bytes", "[3mf][ThreeMFArchiveLifetime]")
{
    Model model;
    REQUIRE(load_stl((std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl").c_str(), &model));
    model.add_default_instances();
    ScopedTemporaryDir backup("archive_lifetime");
    model.set_backup_path(backup.string());
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    ScopedTemporaryFile gcode(".gcode");
    std::string contents;
    for (int i = 0; i < 10000; ++i)
        contents += "G1 X" + std::to_string(i % 200) + " Y20 E0.5\n";
    {
        boost::filesystem::ofstream stream(gcode.string(), std::ios::binary);
        stream.write(contents.data(), contents.size());
        REQUIRE(stream.good());
    }
    for (int iteration = 0; iteration < 3; ++iteration) {
        ScopedTemporaryFile output(".3mf");
        PlateData plate;
        plate.plate_index = 0;
        plate.is_sliced_valid = true;
        plate.gcode_file = gcode.string();
        StoreParams params;
        params.path = output.string();
        params.model = &model;
        params.config = &config;
        params.plate_data_list.push_back(&plate);
        params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SplitModel | SaveStrategy::WithGcode;
        REQUIRE(store_bbs_3mf(params));
        struct Reader {
            mz_zip_archive archive{};
            ~Reader() { mz_zip_end(&archive); }
        } reader;
        REQUIRE(mz_zip_reader_init_file(&reader.archive, output.string().c_str(), 0));
        size_t size = 0;
        std::unique_ptr<void, decltype(&mz_free)> data(
            mz_zip_reader_extract_file_to_heap(&reader.archive, "Metadata/plate_1.gcode", &size, 0), &mz_free);
        REQUIRE(static_cast<bool>(data));
        CHECK(std::string(static_cast<const char*>(data.get()), size) == contents);
        CHECK(mz_zip_validate_archive(&reader.archive, 0));
    }
}

TEST_CASE("Face properties retain their positions through split 3MF import", "[3mf][Regression]")
{
    Model model;
    const std::string source = std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl";
    REQUIRE(load_stl(source.c_str(), &model));
    model.add_default_instances();
    ScopedTemporaryDir backup("three_mf_face_props");
    model.set_backup_path(backup.string());
    REQUIRE(model.objects.size() == 1);
    REQUIRE(model.objects.front()->volumes.size() == 1);
    auto* volume = model.objects.front()->volumes.front();
    TriangleMesh painted = volume->mesh();
    const size_t faces = painted.its.indices.size();
    REQUIRE(faces >= 3);
    painted.its.properties.assign(faces, FaceProperty{eNormal, 0.0});
    painted.its.properties[faces - 2] = FaceProperty{eSmallHole, 1.25};
    painted.its.properties[faces - 1] = FaceProperty{eExteriorAppearance, 0.0};
    volume->set_mesh(std::move(painted));
    TriangleSelector selector(volume->mesh());
    selector.set_facet(static_cast<int>(faces - 1), EnforcerBlockerType::Extruder2);
    REQUIRE(volume->mmu_segmentation_facets.set(selector));

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    ScopedTemporaryFile file(".3mf");
    PlateData plate;
    plate.plate_index = 0;
    StoreParams params;
    params.path = file.string();
    params.model = &model;
    params.config = &config;
    params.plate_data_list.push_back(&plate);
    params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SplitModel;
    REQUIRE(store_bbs_3mf(params));

    Model restored;
    ScopedTemporaryDir restored_backup("three_mf_face_props_restore");
    restored.set_backup_path(restored_backup.string());
    DynamicPrintConfig restored_config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Enable};
    PlateDataPtrs plates;
    std::vector<Preset*> presets;
    bool bbl = false, orca = false;
    Semver version;
    const bool loaded = load_bbs_3mf(file.string().c_str(), &restored_config, &substitutions,
        &restored, &plates, &presets, &bbl, &orca, &version, nullptr,
        LoadStrategy::LoadModel | LoadStrategy::LoadConfig);
    release_PlateData_list(plates);
    for (auto* preset : presets) delete preset;
    REQUIRE(loaded);
    REQUIRE(restored.objects.size() == 1);
    REQUIRE(restored.objects.front()->volumes.size() == 1);
    const auto* restored_volume = restored.objects.front()->volumes.front();
    const auto& properties = restored_volume->mesh().its.properties;
    REQUIRE(properties.size() == faces);
    CHECK(properties[0].type == eNormal);
    CHECK_THAT(properties[0].area, Catch::Matchers::WithinAbs(0.0, 1e-6));
    CHECK(properties[faces - 2].type == eSmallHole);
    CHECK_THAT(properties[faces - 2].area, Catch::Matchers::WithinAbs(1.25, 1e-6));
    CHECK(properties[faces - 1].type == eExteriorAppearance);
    CHECK_THAT(properties[faces - 1].area, Catch::Matchers::WithinAbs(0.0, 1e-6));
    CHECK(restored_volume->mmu_segmentation_facets.get_triangle_as_string(0).empty());
    CHECK(restored_volume->mmu_segmentation_facets.get_triangle_as_string(static_cast<int>(faces - 1)) ==
          volume->mmu_segmentation_facets.get_triangle_as_string(static_cast<int>(faces - 1)));
}

TEST_CASE("Imported volume hulls match their final mesh", "[3mf][Regression]")
{
    const int shape = GENERATE(0, 1, 2, 3);
    CAPTURE(shape);
    Model model;
    ScopedTemporaryDir backup("three_mf_hull");
    model.set_backup_path(backup.string());
    auto* object = model.add_object();
    object->name = "hull_roundtrip";
    TriangleMesh mesh = make_cube(13.25, 20.5, 17.75);
    mesh.translate(-6.625f, -10.25f, -8.875f);
    if (shape == 1)
        mesh.translate(3.141f, 10000.25f, -17.125f);
    if (shape == 3) {
        indexed_triangle_set triangle;
        triangle.vertices = {Vec3f(-1.f, -1.f, 0.f), Vec3f(1.f, -1.f, 0.f), Vec3f(0.f, 1.f, 0.f)};
        triangle.indices = {Vec3i32(0, 1, 2)};
        mesh = TriangleMesh(std::move(triangle));
    }
    auto* volume = object->add_volume(std::move(mesh), ModelVolumeType::MODEL_PART, false);
    if (shape == 2) {
        auto* shared = object->add_volume_with_shared_mesh(*volume);
        shared->translate(Vec3d(30.0, 0.0, 0.0));
    }
    model.add_default_instances();
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    ScopedTemporaryFile file(".3mf");
    PlateData plate;
    plate.plate_index = 0;
    StoreParams params;
    params.path = file.string();
    params.model = &model;
    params.config = &config;
    params.plate_data_list.push_back(&plate);
    params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SplitModel | SaveStrategy::ShareMesh;
    REQUIRE(store_bbs_3mf(params));

    Model restored;
    ScopedTemporaryDir restored_backup("three_mf_hull_restore");
    restored.set_backup_path(restored_backup.string());
    REQUIRE(boost::filesystem::create_directories(restored_backup.path() / "Metadata"));
    DynamicPrintConfig restored_config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Enable};
    PlateDataPtrs plates;
    std::vector<Preset*> presets;
    bool bbl = false, orca = false;
    Semver version;
    const bool loaded = load_bbs_3mf(file.string().c_str(), &restored_config, &substitutions,
        &restored, &plates, &presets, &bbl, &orca, &version, nullptr,
        LoadStrategy::LoadModel | LoadStrategy::LoadConfig);
    release_PlateData_list(plates);
    for (auto* preset : presets) delete preset;
    REQUIRE(loaded);
    REQUIRE(restored.objects.size() == 1);
    const auto& volumes = restored.objects.front()->volumes;
    REQUIRE(volumes.size() == (shape == 2 ? 2 : 1));
    if (shape == 2)
        CHECK(volumes[0]->get_mesh_shared_ptr() == volumes[1]->get_mesh_shared_ptr());
    if (shape == 1)
        CHECK_FALSE(volumes.front()->mesh().get_init_shift().isApprox(Vec3d::Zero()));
    else
        CHECK(volumes.front()->mesh().get_init_shift().isApprox(Vec3d::Zero()));

    for (const auto* imported : volumes) {
        const auto& hull = imported->get_convex_hull_shared_ptr();
        REQUIRE(hull);
        const TriangleMesh expected = imported->mesh().convex_hull_3d();
        REQUIRE(hull->its.vertices.size() == expected.its.vertices.size());
        REQUIRE(hull->its.indices.size() == expected.its.indices.size());
        // Reusing a hull must preserve the exact ordered output of the previous
        // recomputation, including float bits; approximate geometry is insufficient.
        for (size_t i = 0; i < expected.its.vertices.size(); ++i)
            for (int axis = 0; axis < 3; ++axis)
                CHECK(std::memcmp(&hull->its.vertices[i][axis], &expected.its.vertices[i][axis], sizeof(float)) == 0);
        for (size_t i = 0; i < expected.its.indices.size(); ++i)
            CHECK(hull->its.indices[i] == expected.its.indices[i]);
    }
}

// Hidden diagnostic: explicitly supplied local fixture; no provider or GUI calls.
TEST_CASE("A real project separates archive inflation from model import", "[.ThreeMFReadProbe]")
{
    const char* fixture = std::getenv("ORCASLICER_3MF_FIXTURE");
    const char* report_path = std::getenv("ORCASLICER_3MF_REPORT");
    REQUIRE(fixture != nullptr);
    REQUIRE(report_path != nullptr);
    struct RestoreLogging {
        unsigned level = get_logging_level();
        ~RestoreLogging() { set_logging_level(level); }
    } restore_logging;
    set_logging_level(3);
    struct Archive {
        mz_zip_archive zip{};
        ~Archive() { mz_zip_end(&zip); }
    } archive;
    REQUIRE(mz_zip_reader_init_file(&archive.zip, fixture, 0));
    std::vector<mz_uint> model_entries;
    for (mz_uint entry = 0; entry < mz_zip_reader_get_num_files(&archive.zip); ++entry) {
        mz_zip_archive_file_stat stat{};
        REQUIRE(mz_zip_reader_file_stat(&archive.zip, entry, &stat));
        const std::string name = stat.m_filename;
        if (name.size() >= 6 && name.substr(name.size() - 6) == ".model")
            model_entries.push_back(entry);
    }
    REQUIRE_FALSE(model_entries.empty());
    boost::filesystem::ofstream report(report_path);
    REQUIRE(report.good());
    report.imbue(std::locale::classic());
    report << "iteration,inflate_ms,import_ms,model_xml_bytes,objects,triangles,index_hash,geometry_hash,hull_hash,annotation_hash,transform_hash,zero_shift_volumes,read_callbacks,max_read_gap_ms,cancel_return_ms,progress_attempt_ms\n";
    const char* progress_env = std::getenv("ORCASLICER_3MF_PROGRESS_MODE");
    const std::string progress_mode = progress_env ? progress_env : "";
    const std::array<std::string, 4> supported_modes{"", "normal", "cancel", "throw"};
    REQUIRE(std::find(supported_modes.begin(), supported_modes.end(), progress_mode) != supported_modes.end());
    using Clock = std::chrono::steady_clock;
    size_t expected_triangles = 0;
    std::uint64_t expected_index_hash = 0;
    std::array<std::uint64_t, 4> expected_full{};
    for (int iteration = 0; iteration < 4; ++iteration) {
        size_t xml_bytes = 0;
        const auto inflate_started = Clock::now();
        for (mz_uint entry : model_entries) {
            size_t size = 0;
            std::unique_ptr<void, decltype(&mz_free)> data(
                mz_zip_reader_extract_to_heap(&archive.zip, entry, &size, 0), &mz_free);
            REQUIRE(data != nullptr);
            xml_bytes += size;
        }
        const double inflate_ms = std::chrono::duration<double, std::milli>(Clock::now() - inflate_started).count();

        Model model;
        ScopedTemporaryDir backup("three_mf_read_probe");
        model.set_backup_path(backup.string());
        REQUIRE(boost::filesystem::create_directories(backup.path() / "Metadata"));
        DynamicPrintConfig config;
        ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Enable};
        PlateDataPtrs plates;
        std::vector<Preset*> presets;
        bool bbl = false, orca = false;
        Semver version;
        const auto caller = std::this_thread::get_id();
        std::atomic<bool> wrong_thread{false};
        size_t read_callbacks = 0;
        double max_read_gap_ms = 0.0, cancel_return_ms = 0.0, progress_attempt_ms = 0.0;
        Clock::time_point previous_read{}, cancel_requested{};
        Import3mfProgressFn progress;
        if (!progress_mode.empty()) {
            progress = [&](int stage, int current, int total, bool& cancel) {
                if (std::this_thread::get_id() != caller) {
                    wrong_thread.store(true);
                    cancel = true;
                    return;
                }
                if (stage != IMPORT_STAGE_READ_FILES || current != 1 || total != 3)
                    return;
                const auto now = Clock::now();
                if (read_callbacks != 0)
                    max_read_gap_ms = std::max(max_read_gap_ms,
                        std::chrono::duration<double, std::milli>(now - previous_read).count());
                previous_read = now;
                ++read_callbacks;
                if (progress_mode == "cancel" || progress_mode == "throw") {
                    cancel_requested = now;
                    if (progress_mode == "throw")
                        throw std::runtime_error("3MF progress callback failure");
                    cancel = true;
                }
            };
        }
        auto load = [&](Import3mfProgressFn callback) {
            return load_bbs_3mf(fixture, &config, &substitutions, &model,
                &plates, &presets, &bbl, &orca, &version, callback,
                LoadStrategy::LoadModel | LoadStrategy::LoadConfig);
        };
        auto import_started = Clock::now();
        bool loaded = false, callback_threw = false;
        try { loaded = load(progress); }
        catch (const std::runtime_error& error) {
            callback_threw = std::string(error.what()) == "3MF progress callback failure";
            if (!callback_threw) throw;
        }
        double import_ms = std::chrono::duration<double, std::milli>(Clock::now() - import_started).count();
        REQUIRE_FALSE(wrong_thread.load());
        if (!progress_mode.empty()) {
            progress_attempt_ms = import_ms;
            REQUIRE(read_callbacks > 0);
        }
        if (progress_mode == "cancel" || progress_mode == "throw") {
            cancel_return_ms = std::chrono::duration<double, std::milli>(Clock::now() - cancel_requested).count();
            REQUIRE_FALSE(loaded);
            REQUIRE(callback_threw == (progress_mode == "throw"));
            REQUIRE(model.objects.empty());
            release_PlateData_list(plates);
            for (auto* preset : presets) delete preset;
            presets.clear();
            import_started = Clock::now();
            loaded = load(nullptr);
            import_ms = std::chrono::duration<double, std::milli>(Clock::now() - import_started).count();
        }
        release_PlateData_list(plates);
        for (auto* preset : presets) delete preset;
        REQUIRE(loaded);
        REQUIRE_FALSE(model.objects.empty());
        size_t triangles = 0;
        std::uint64_t index_hash = 14695981039346656037ull;
        for (const auto* object : model.objects)
            for (const auto* volume : object->volumes) {
                triangles += volume->mesh().its.indices.size();
                for (const auto& triangle : volume->mesh().its.indices)
                    for (int corner = 0; corner < 3; ++corner) {
                        index_hash ^= static_cast<std::uint32_t>(triangle[corner]);
                        index_hash *= 1099511628211ull;
                    }
            }
        std::array<std::uint64_t, 4> full{
            14695981039346656037ull, 14695981039346656037ull,
            14695981039346656037ull, 14695981039346656037ull};
        const auto hash_bytes = [](std::uint64_t& hash, const void* data, size_t size) {
            const auto* bytes = static_cast<const unsigned char*>(data);
            for (size_t j = 0; j < size; ++j) {
                hash ^= bytes[j];
                hash *= 1099511628211ull;
            }
        };
        const auto hash_mesh = [&](std::uint64_t& hash, const indexed_triangle_set& its) {
            const size_t vertices = its.vertices.size(), faces = its.indices.size();
            hash_bytes(hash, &vertices, sizeof(vertices));
            hash_bytes(hash, &faces, sizeof(faces));
            // Hash scalar fields rather than Eigen/struct storage and padding.
            for (const auto& vertex : its.vertices)
                for (int axis = 0; axis < 3; ++axis)
                    hash_bytes(hash, &vertex[axis], sizeof(float));
            for (const auto& face : its.indices)
                for (int corner = 0; corner < 3; ++corner)
                    hash_bytes(hash, &face[corner], sizeof(int));
        };
        size_t zero_shift_volumes = 0;
        for (const auto* object : model.objects) {
            for (const auto* instance : object->instances) {
                const auto matrix = instance->get_matrix().matrix();
                hash_bytes(full[3], matrix.data(), sizeof(double) * 16);
            }
            for (const auto* volume : object->volumes) {
                hash_mesh(full[0], volume->mesh().its);
                for (const auto& property : volume->mesh().its.properties) {
                    hash_bytes(full[0], &property.type, sizeof(property.type));
                    hash_bytes(full[0], &property.area, sizeof(property.area));
                }
                const auto& hull = volume->get_convex_hull_shared_ptr();
                const bool has_hull = bool(hull);
                hash_bytes(full[1], &has_hull, sizeof(has_hull));
                if (hull) hash_mesh(full[1], hull->its);
                const auto matrix = volume->get_matrix().matrix();
                hash_bytes(full[3], matrix.data(), sizeof(double) * 16);
                hash_bytes(full[3], volume->source.mesh_offset.data(), sizeof(double) * 3);
                // Historical source metadata may overwrite source.mesh_offset;
                // the mesh records the actual translation made during import.
                zero_shift_volumes += volume->mesh().get_init_shift().isApprox(Vec3d::Zero());
                for (size_t face = 0; face < volume->mesh().its.indices.size(); ++face)
                    for (const auto* annotation : {&volume->supported_facets, &volume->seam_facets,
                            &volume->mmu_segmentation_facets, &volume->fuzzy_skin_facets}) {
                        const auto value = annotation->get_triangle_as_string(int(face));
                        const size_t size = value.size();
                        hash_bytes(full[2], &size, sizeof(size));
                        hash_bytes(full[2], value.data(), size);
                    }
            }
        }
        if (iteration == 0) expected_full = full;
        CHECK(full == expected_full);
        if (iteration == 0) {
            expected_triangles = triangles;
            expected_index_hash = index_hash;
        }
        CHECK(triangles == expected_triangles);
        CHECK(index_hash == expected_index_hash);
        report << iteration << ',' << inflate_ms << ',' << import_ms << ',' << xml_bytes
               << ',' << model.objects.size() << ',' << triangles << ',' << index_hash
               << ',' << full[0] << ',' << full[1] << ',' << full[2] << ',' << full[3] << ',' << zero_shift_volumes
               << ',' << read_callbacks << ',' << max_read_gap_ms << ',' << cancel_return_ms << ',' << progress_attempt_ms << '\n';
        report.flush();
        REQUIRE(report.good());
    }
}

// Hidden diagnostic: explicitly supplied local fixture; no provider or GUI calls.
TEST_CASE("A real project retains its mesh through repeated timed saves", "[.ThreeMFSaveProbe]")
{
    const char* fixture = std::getenv("ORCASLICER_3MF_FIXTURE");
    const char* report_path = std::getenv("ORCASLICER_3MF_REPORT");
    const char* captured_path = std::getenv("ORCASLICER_3MF_CAPTURE");
    REQUIRE(fixture != nullptr);
    REQUIRE(report_path != nullptr);
    struct RestoreLogging {
        unsigned level = get_logging_level();
        ~RestoreLogging() { set_logging_level(level); }
    } restore_logging;
    set_logging_level(3);
    Model model;
    ScopedTemporaryDir backup("three_mf_probe");
    model.set_backup_path(backup.string());
    REQUIRE(boost::filesystem::create_directories(backup.path() / "Metadata"));
    // Match Plater loading: deserialize into an empty dynamic configuration.
    DynamicPrintConfig config;
    auto read = [](const char* path, Model& target, DynamicPrintConfig& target_config) {
        ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Enable};
        PlateDataPtrs plates;
        std::vector<Preset*> presets;
        bool bbl = false, orca = false;
        Semver version;
        const bool ok = load_bbs_3mf(path, &target_config, &substitutions, &target,
            &plates, &presets, &bbl, &orca, &version, nullptr,
            LoadStrategy::LoadModel | LoadStrategy::LoadConfig);
        release_PlateData_list(plates);
        for (auto* preset : presets) delete preset;
        return ok;
    };
    REQUIRE(read(fixture, model, config));
    REQUIRE_FALSE(model.objects.empty());
    boost::filesystem::ofstream report(report_path);
    REQUIRE(report.good());
    report.imbue(std::locale::classic());
    report << "iteration,save_ms,reload_ms\n";
    using Clock = std::chrono::steady_clock;
    for (int iteration = 0; iteration < 6; ++iteration) {
        ScopedTemporaryFile output(".3mf");
        PlateData plate;
        plate.plate_index = 0;
        StoreParams params;
        params.path = output.string();
        params.model = &model;
        params.config = &config;
        params.plate_data_list.push_back(&plate);
        params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SplitModel | SaveStrategy::ShareMesh;
        const auto started = Clock::now();
        const bool saved = store_bbs_3mf(params);
        const double save_ms = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        REQUIRE(saved);
        Model restored;
        ScopedTemporaryDir restored_backup("three_mf_probe_restore");
        restored.set_backup_path(restored_backup.string());
        REQUIRE(boost::filesystem::create_directories(restored_backup.path() / "Metadata"));
        DynamicPrintConfig restored_config;
        const auto load_started = Clock::now();
        const bool loaded = read(output.string().c_str(), restored, restored_config);
        const double load_ms = std::chrono::duration<double, std::milli>(Clock::now() - load_started).count();
        REQUIRE(loaded);
        REQUIRE(restored.objects.size() == model.objects.size());
        double max_coordinate_error = 0.0;
        bool indices_equal = true;
        for (size_t o = 0; o < model.objects.size(); ++o) {
            REQUIRE(restored.objects[o]->volumes.size() == model.objects[o]->volumes.size());
            for (size_t v = 0; v < model.objects[o]->volumes.size(); ++v) {
                const auto& a = model.objects[o]->volumes[v]->mesh().its;
                const auto& b = restored.objects[o]->volumes[v]->mesh().its;
                REQUIRE(a.vertices.size() == b.vertices.size());
                REQUIRE(a.indices.size() == b.indices.size());
                for (size_t i = 0; i < a.vertices.size(); ++i)
                    max_coordinate_error = std::max(max_coordinate_error, double((a.vertices[i] - b.vertices[i]).cwiseAbs().maxCoeff()));
                for (size_t i = 0; i < a.indices.size(); ++i)
                    indices_equal = indices_equal && (a.indices[i].array() == b.indices[i].array()).all();
            }
        }
        CHECK_THAT(max_coordinate_error, Catch::Matchers::WithinAbs(0.0, 1e-7));
        CHECK(indices_equal);
        report << iteration << ',' << save_ms << ',' << load_ms << '\n';
        report.flush();
        REQUIRE(report.good());
        if (captured_path && iteration == 5)
            REQUIRE(boost::filesystem::copy_file(output.path(), captured_path, boost::filesystem::copy_option::overwrite_if_exists));
    }
}

// Hidden diagnostic for the GUI save handoff: measure the copy that would
// still run on the caller thread before a background exporter could start.
TEST_CASE("A painted project model can be copied without sharing mutable annotations", "[.ThreeMFSnapshotProbe]")
{
    const char* fixture = std::getenv("ORCASLICER_3MF_FIXTURE");
    const char* report_path = std::getenv("ORCASLICER_3MF_REPORT");
    REQUIRE(fixture != nullptr);
    REQUIRE(report_path != nullptr);
    Model model;
    ScopedTemporaryDir backup("three_mf_snapshot_probe");
    model.set_backup_path(backup.string());
    REQUIRE(boost::filesystem::create_directories(backup.path() / "Metadata"));
    DynamicPrintConfig config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Enable};
    PlateDataPtrs plates;
    std::vector<Preset*> presets;
    bool bbl = false, orca = false;
    Semver version;
    const bool loaded = load_bbs_3mf(fixture, &config, &substitutions, &model,
        &plates, &presets, &bbl, &orca, &version, nullptr,
        LoadStrategy::LoadModel | LoadStrategy::LoadConfig);
    release_PlateData_list(plates);
    for (auto* preset : presets) delete preset;
    REQUIRE(loaded);
    REQUIRE_FALSE(model.objects.empty());

    boost::filesystem::ofstream report(report_path);
    REQUIRE(report.good());
    report.imbue(std::locale::classic());
    report << "iteration,model_copy_ms,config_copy_ms,shared_meshes,painted_volumes\n";
    using Clock = std::chrono::steady_clock;
    for (int iteration = 0; iteration < 6; ++iteration) {
        const auto model_started = Clock::now();
        Model snapshot(model);
        const double model_ms = std::chrono::duration<double, std::milli>(Clock::now() - model_started).count();
        const auto config_started = Clock::now();
        DynamicPrintConfig config_snapshot(config);
        const double config_ms = std::chrono::duration<double, std::milli>(Clock::now() - config_started).count();
        REQUIRE(snapshot.objects.size() == model.objects.size());
        size_t shared_meshes = 0;
        size_t painted_volumes = 0;
        for (size_t o = 0; o < model.objects.size(); ++o) {
            REQUIRE(snapshot.objects[o] != model.objects[o]);
            REQUIRE(snapshot.objects[o]->volumes.size() == model.objects[o]->volumes.size());
            for (size_t v = 0; v < model.objects[o]->volumes.size(); ++v) {
                const auto* source = model.objects[o]->volumes[v];
                auto* copied = snapshot.objects[o]->volumes[v];
                shared_meshes += source->get_mesh_shared_ptr() == copied->get_mesh_shared_ptr();
                REQUIRE(source->mmu_segmentation_facets.equals(copied->mmu_segmentation_facets));
                if (!source->mmu_segmentation_facets.empty()) {
                    ++painted_volumes;
                    copied->mmu_segmentation_facets.reset();
                    CHECK_FALSE(source->mmu_segmentation_facets.empty());
                    CHECK(copied->mmu_segmentation_facets.empty());
                }
            }
        }
        CHECK(painted_volumes > 0);
        report << iteration << ',' << model_ms << ',' << config_ms << ','
               << shared_meshes << ',' << painted_volumes << '\n';
        report.flush();
        REQUIRE(report.good());
    }
}

TEST_CASE("Model XML remains identical across timed compression levels", "[.ThreeMFCompressionProbe]")
{
    const char* fixture = std::getenv("ORCASLICER_3MF_FIXTURE");
    const char* report_path = std::getenv("ORCASLICER_3MF_REPORT");
    REQUIRE(fixture != nullptr);
    REQUIRE(report_path != nullptr);
    struct Archive {
        mz_zip_archive zip{};
        ~Archive() { mz_zip_end(&zip); }
    } input;
    REQUIRE(mz_zip_reader_init_file(&input.zip, fixture, 0));
    boost::filesystem::ofstream report(report_path);
    REQUIRE(report.good());
    report.imbue(std::locale::classic());
    report << "entry,iteration,level,xml_bytes,zip_bytes,compress_ms\n";
    size_t mesh_entries = 0;
    for (mz_uint entry = 0; entry < mz_zip_reader_get_num_files(&input.zip); ++entry) {
        mz_zip_archive_file_stat stat{};
        REQUIRE(mz_zip_reader_file_stat(&input.zip, entry, &stat));
        const std::string name = stat.m_filename;
        if (name.size() < 6 || name.substr(name.size() - 6) != ".model") continue;
        size_t size = 0;
        std::unique_ptr<void, decltype(&mz_free)> data(mz_zip_reader_extract_to_heap(&input.zip, entry, &size, 0), &mz_free);
        REQUIRE(data != nullptr);
        const std::string xml(static_cast<const char*>(data.get()), size);
        if (xml.find("<vertices>") == std::string::npos) continue;
        ++mesh_entries;
        for (int iteration = 0; iteration < 6; ++iteration) {
            const int levels[] = {6, 3, 1};
            for (int order = 0; order < 3; ++order) {
                const int level = levels[(order + iteration) % 3];
                Archive output;
                REQUIRE(mz_zip_writer_init_heap(&output.zip, 0, 1024 * 1024));
                mz_zip_writer_staged_context context{};
                const auto started = std::chrono::steady_clock::now();
                REQUIRE(mz_zip_writer_add_staged_open(&output.zip, &context, "3D/model.model",
                    (uint64_t(1) << 32) - 1, nullptr, nullptr, 0, level, nullptr, 0, nullptr, 0));
                for (size_t offset = 0; offset < size; offset += 1024 * 1024)
                    REQUIRE(mz_zip_writer_add_staged_data(&context, xml.data() + offset, std::min(size - offset, size_t(1024 * 1024))));
                REQUIRE(mz_zip_writer_add_staged_finish(&context));
                void* bytes = nullptr;
                size_t byte_count = 0;
                REQUIRE(mz_zip_writer_finalize_heap_archive(&output.zip, &bytes, &byte_count));
                const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
                std::unique_ptr<void, decltype(&mz_free)> archive_data(bytes, &mz_free);
                Archive verify;
                REQUIRE(mz_zip_reader_init_mem(&verify.zip, bytes, byte_count, 0));
                std::string restored(size, '\0');
                REQUIRE(mz_zip_reader_extract_to_mem(&verify.zip, 0, restored.data(), restored.size(), 0));
                CHECK(restored == xml);
                report << entry << ',' << iteration << ',' << level << ',' << size << ',' << byte_count << ',' << elapsed << '\n';
                report.flush();
                REQUIRE(report.good());
            }
        }
    }
    REQUIRE(mesh_entries > 0);
}

TEST_CASE("Reordered triangle attributes retain all sparse annotations", "[3mf][Regression]")
{
    const bool split_model = GENERATE(false, true);
    CAPTURE(split_model);
    Model model;
    ScopedTemporaryDir backup("three_mf_attribute_order");
    model.set_backup_path(backup.string());
    TriangleMesh mesh = make_cube(13.25, 20.5, 17.75);
    const size_t faces = mesh.its.indices.size();
    REQUIRE(faces >= 12);
    mesh.its.properties.assign(faces, FaceProperty{eNormal, 0.0});
    mesh.its.properties[4] = FaceProperty{eSmallHole, 1.25};
    mesh.its.properties[8] = FaceProperty{eExteriorAppearance, 0.0};
    auto* volume = model.add_object()->add_volume(std::move(mesh), ModelVolumeType::MODEL_PART, false);
    std::array<FacetsAnnotation*, 4> annotations = {
        &volume->supported_facets, &volume->seam_facets,
        &volume->mmu_segmentation_facets, &volume->fuzzy_skin_facets};
    for (size_t i = 0; i != annotations.size(); ++i) {
        TriangleSelector selector(volume->mesh());
        selector.set_facet(static_cast<int>(1 + 2 * i), EnforcerBlockerType::ENFORCER);
        selector.set_facet(static_cast<int>(faces - 2), EnforcerBlockerType::BLOCKER);
        REQUIRE(annotations[i]->set(selector));
    }
    model.add_default_instances();
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    ScopedTemporaryFile original(".3mf"), reordered(".3mf");
    PlateData plate;
    plate.plate_index = 0;
    StoreParams params;
    params.path = original.string();
    params.model = &model;
    params.config = &config;
    params.plate_data_list.push_back(&plate);
    params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
    if (split_model)
        params.strategy = params.strategy | SaveStrategy::SplitModel;
    REQUIRE(store_bbs_3mf(params));

    // Exercise the actual importers with legal external ordering and unknown
    // names. Keep the original XML attribute values and all other archive data.
    size_t reordered_faces = 0;
    {
        struct Archive {
            mz_zip_archive zip{};
            ~Archive() { mz_zip_end(&zip); }
        } input, output;
        REQUIRE(mz_zip_reader_init_file(&input.zip, original.string().c_str(), 0));
        REQUIRE(mz_zip_writer_init_file(&output.zip, reordered.string().c_str(), 0));
        const std::regex attribute(R"attr(([^\s=]+)="([^"]*)")attr");
        for (mz_uint entry = 0; entry < mz_zip_reader_get_num_files(&input.zip); ++entry) {
            mz_zip_archive_file_stat stat{};
            REQUIRE(mz_zip_reader_file_stat(&input.zip, entry, &stat));
            const std::string name = stat.m_filename;
            size_t size = 0;
            std::unique_ptr<void, decltype(&mz_free)> data(
                mz_zip_reader_extract_to_heap(&input.zip, entry, &size, 0), &mz_free);
            REQUIRE(data != nullptr);
            std::string bytes(static_cast<const char*>(data.get()), size);
            if (name.size() >= 6 && name.substr(name.size() - 6) == ".model") {
                size_t position = 0;
                while ((position = bytes.find("<triangle ", position)) != std::string::npos) {
                    const size_t end = bytes.find("/>", position);
                    REQUIRE(end != std::string::npos);
                    const std::string fields = bytes.substr(position + 10, end - position - 10);
                    std::vector<std::string> values;
                    for (std::sregex_iterator it(fields.begin(), fields.end(), attribute), stop; it != stop; ++it)
                        values.push_back(it->str());
                    REQUIRE(values.size() >= 3);
                    std::string triangle = "<triangle opaque=\"ignored\" paint_unused=\"x\" face_unused=\"y\"";
                    for (auto it = values.rbegin(); it != values.rend(); ++it)
                        triangle += " " + *it;
                    triangle += "/>";
                    bytes.replace(position, end + 2 - position, triangle);
                    position += triangle.size();
                    ++reordered_faces;
                }
            }
            REQUIRE(mz_zip_writer_add_mem(&output.zip, name.c_str(), bytes.data(), bytes.size(), MZ_DEFAULT_COMPRESSION));
        }
        REQUIRE(mz_zip_writer_finalize_archive(&output.zip));
    }
    REQUIRE(reordered_faces == faces);
    Model restored;
    ScopedTemporaryDir restored_backup("three_mf_attribute_order_restore");
    restored.set_backup_path(restored_backup.string());
    REQUIRE(boost::filesystem::create_directories(restored_backup.path() / "Metadata"));
    DynamicPrintConfig restored_config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Enable};
    PlateDataPtrs plates;
    std::vector<Preset*> presets;
    bool bbl = false, orca = false;
    Semver version;
    const bool loaded = load_bbs_3mf(reordered.string().c_str(), &restored_config, &substitutions,
        &restored, &plates, &presets, &bbl, &orca, &version, nullptr,
        LoadStrategy::LoadModel | LoadStrategy::LoadConfig);
    release_PlateData_list(plates);
    for (auto* preset : presets) delete preset;
    REQUIRE(loaded);
    REQUIRE(restored.objects.size() == 1);
    REQUIRE(restored.objects.front()->volumes.size() == 1);
    const auto* actual = restored.objects.front()->volumes.front();
    std::array<const FacetsAnnotation*, 4> actual_annotations = {
        &actual->supported_facets, &actual->seam_facets,
        &actual->mmu_segmentation_facets, &actual->fuzzy_skin_facets};
    REQUIRE(actual->mesh().its.indices.size() == faces);
    for (size_t i = 0; i != annotations.size(); ++i)
        for (size_t face = 0; face != faces; ++face)
            CHECK(actual_annotations[i]->get_triangle_as_string(static_cast<int>(face)) ==
                  annotations[i]->get_triangle_as_string(static_cast<int>(face)));
    const auto& actual_properties = actual->mesh().its.properties;
    const auto& expected_properties = volume->mesh().its.properties;
    REQUIRE(actual_properties.size() == expected_properties.size());
    for (size_t face = 0; face != faces; ++face) {
        CHECK(actual_properties[face].type == expected_properties[face].type);
        CHECK_THAT(actual_properties[face].area,
            Catch::Matchers::WithinAbs(expected_properties[face].area, 1e-6));
    }
}

TEST_CASE("Large archived model parts retain geometry and recover from XML failures and cancellation", "[3mf][ArchiveRead]")
{
    const bool split_model = GENERATE(false, true);
    const int failure = GENERATE(0, 1, 2, 3, 4);
    const int compression = GENERATE(MZ_NO_COMPRESSION, MZ_DEFAULT_COMPRESSION);
    CAPTURE(split_model, failure, compression);
    Model model;
    ScopedTemporaryDir backup("three_mf_large_part");
    model.set_backup_path(backup.string());
    auto* volume = model.add_object()->add_volume(make_cube(13.25, 20.5, 17.75), ModelVolumeType::MODEL_PART, false);
    TriangleSelector selector(volume->mesh());
    selector.set_facet(2, EnforcerBlockerType::ENFORCER);
    selector.set_facet(9, EnforcerBlockerType::BLOCKER);
    REQUIRE(volume->mmu_segmentation_facets.set(selector));
    if (split_model)
        model.add_object()->add_volume(make_cube(11.0, 7.0, 8.0), ModelVolumeType::MODEL_PART, false);
    model.add_default_instances();
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    ScopedTemporaryFile original(".3mf"), padded(".3mf");
    PlateData plate;
    plate.plate_index = 0;
    StoreParams params;
    params.path = original.string();
    params.model = &model;
    params.config = &config;
    params.plate_data_list.push_back(&plate);
    params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
    if (split_model)
        params.strategy = params.strategy | SaveStrategy::SplitModel;
    REQUIRE(store_bbs_3mf(params));
    size_t large_parts = 0;
    {
        struct Archive {
            mz_zip_archive zip{};
            ~Archive() { mz_zip_end(&zip); }
        } input, output;
        REQUIRE(mz_zip_reader_init_file(&input.zip, original.string().c_str(), 0));
        REQUIRE(mz_zip_writer_init_file(&output.zip, padded.string().c_str(), 0));
        for (mz_uint entry = 0; entry < mz_zip_reader_get_num_files(&input.zip); ++entry) {
            mz_zip_archive_file_stat stat{};
            REQUIRE(mz_zip_reader_file_stat(&input.zip, entry, &stat));
            size_t size = 0;
            std::unique_ptr<void, decltype(&mz_free)> data(mz_zip_reader_extract_to_heap(&input.zip, entry, &size, 0), &mz_free);
            REQUIRE(data != nullptr);
            std::string bytes(static_cast<const char*>(data.get()), size);
            const std::string name = stat.m_filename;
            const auto mesh = bytes.find("<mesh>");
            if (large_parts == 0 && name.size() >= 6 && name.substr(name.size() - 6) == ".model" && mesh != std::string::npos) {
                // Legal whitespace spans many ZIP output blocks. Fail either
                // before the reader fills its buffers or after draining them.
                std::string prefix;
                if (failure == 1)
                    prefix = "<broken></wrong>";
                bytes.insert(mesh + 6, prefix + std::string(9 * 1024 * 1024, ' '));
                if (failure == 2) {
                    const auto end = bytes.find("</mesh>");
                    REQUIRE(end != std::string::npos);
                    bytes.insert(end, "<broken></wrong>");
                }
                ++large_parts;
            }
            REQUIRE(mz_zip_writer_add_mem(&output.zip, name.c_str(), bytes.data(), bytes.size(), compression));
        }
        REQUIRE(mz_zip_writer_finalize_archive(&output.zip));
    }
    REQUIRE(large_parts == 1);
    Model restored;
    ScopedTemporaryDir restored_backup("three_mf_large_part_restore");
    restored.set_backup_path(restored_backup.string());
    REQUIRE(boost::filesystem::create_directories(restored_backup.path() / "Metadata"));
    DynamicPrintConfig restored_config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Enable};
    PlateDataPtrs plates;
    std::vector<Preset*> presets;
    bool bbl = false, orca = false;
    Semver version;
    const auto caller = std::this_thread::get_id();
    bool saw_read_checkpoint = false, finished = false, read_regressed = false;
    std::atomic<bool> wrong_thread{false};
    int last_read = -1;
    Import3mfProgressFn progress;
    if (failure == 0 || failure >= 3) {
        progress = [&](int stage, int current, int total, bool& cancel) {
            if (std::this_thread::get_id() != caller) {
                wrong_thread = true;
                cancel = true;
                return;
            }
            finished |= stage == IMPORT_STAGE_FINISH;
            if (stage == IMPORT_STAGE_READ_FILES) {
                read_regressed |= current < last_read;
                last_read = current;
            }
            if (stage == IMPORT_STAGE_READ_FILES && current == 1 && total == 3) {
                saw_read_checkpoint = true;
                if (failure == 4)
                    throw std::runtime_error("3MF progress callback failure");
                cancel = failure == 3;
            }
        };
    }
    auto load = [&](Import3mfProgressFn callback) {
        return load_bbs_3mf(padded.string().c_str(), &restored_config, &substitutions,
            &restored, &plates, &presets, &bbl, &orca, &version, callback,
            LoadStrategy::LoadModel | LoadStrategy::LoadConfig);
    };
    bool loaded = false, callback_threw = false;
    try { loaded = load(progress); }
    catch (const std::runtime_error& error) {
        callback_threw = std::string(error.what()) == "3MF progress callback failure";
        if (!callback_threw) throw;
    }
    release_PlateData_list(plates);
    for (auto* preset : presets) delete preset;
    presets.clear();
    REQUIRE_FALSE(wrong_thread.load());
    REQUIRE_FALSE(read_regressed);
    if (failure == 0) {
        REQUIRE(saw_read_checkpoint);
        REQUIRE(finished);
    }
    if (failure >= 3) {
        REQUIRE(saw_read_checkpoint);
        REQUIRE_FALSE(finished);
        REQUIRE_FALSE(loaded);
        REQUIRE(callback_threw == (failure == 4));
        REQUIRE(restored.objects.empty());
        // Return includes draining/joining the archive readers. The same file
        // and destination remain usable immediately, without changing config.
        loaded = load(nullptr);
        release_PlateData_list(plates);
        for (auto* preset : presets) delete preset;
    }
    if (failure == 1 || failure == 2) {
        REQUIRE_FALSE(loaded);
        return;
    }
    REQUIRE(loaded);
    REQUIRE(restored.objects.size() == model.objects.size());
    if (split_model) {
        REQUIRE(restored.objects.back()->volumes.size() == 1);
        const auto size = restored.objects.back()->volumes.front()->mesh().bounding_box().size();
        CHECK_THAT(size.x(), Catch::Matchers::WithinAbs(11.0, 1e-6));
        CHECK_THAT(size.y(), Catch::Matchers::WithinAbs(7.0, 1e-6));
        CHECK_THAT(size.z(), Catch::Matchers::WithinAbs(8.0, 1e-6));
    }
    REQUIRE(restored.objects.front()->volumes.size() == 1);
    const auto* actual = restored.objects.front()->volumes.front();
    REQUIRE(actual->mesh().its.indices.size() == volume->mesh().its.indices.size());
    CHECK_THAT(actual->mesh().bounding_box().size().x(), Catch::Matchers::WithinAbs(13.25, 1e-6));
    CHECK_THAT(actual->mesh().bounding_box().size().y(), Catch::Matchers::WithinAbs(20.5, 1e-6));
    CHECK_THAT(actual->mesh().bounding_box().size().z(), Catch::Matchers::WithinAbs(17.75, 1e-6));
    for (size_t face = 0; face != volume->mesh().its.indices.size(); ++face) {
        CHECK(actual->mesh().its.indices[face] == volume->mesh().its.indices[face]);
        CHECK(actual->mmu_segmentation_facets.get_triangle_as_string(static_cast<int>(face)) ==
            volume->mmu_segmentation_facets.get_triangle_as_string(static_cast<int>(face)));
    }
}
