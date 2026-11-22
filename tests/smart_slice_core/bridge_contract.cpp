#include "orca_mobile_slicing_sdk/smart_slice_bridge.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include <iostream>
#include <cmath>
#include <stdexcept>
#include <limits>
#include <algorithm>
using namespace orca::mobile::slicing;
void require(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }

Json mixed_plans(double ordinary_z, double reflected_z, bool ordinary, bool reflected)
{
    LoadedProject project;
    project.config = Slic3r::DynamicPrintConfig::full_print_config();
    auto* object = project.model.add_object();
    object->add_volume(Slic3r::TriangleMesh(Slic3r::its_make_cube(10.,10.,10.)));
    if (ordinary) object->add_instance()->set_offset(Slic3r::Vec3d(0.,0.,ordinary_z));
    if (reflected) {
        auto* instance = object->add_instance();
        instance->set_mirror(Slic3r::Vec3d(-1.,1.,1.));
        instance->set_offset(Slic3r::Vec3d(30.,0.,reflected_z));
    }
    return smart_slice_plans(project,Json::object());
}
void check_mixed_orientation()
{
    for (double a : {0.,4.}) for (double b : {0.,4.}) {
        const auto only_a=mixed_plans(a,b,true,false);
        const auto only_b=mixed_plans(a,b,false,true);
        const auto both=mixed_plans(a,b,true,true);
        for (size_t i=0;i<3;++i) {
            const double expected=.5*(only_a.at("plans").at(i).at("overhangRisk").get<double>()+
                only_b.at("plans").at(i).at("overhangRisk").get<double>());
            const auto& plan=both.at("plans").at(i);
            require(std::abs(plan.at("overhangRisk").get<double>()-expected)<1e-9,
                    "mixed instance risks match independent geometry");
            require(plan.at("patch").at("enable_support").get<bool>()==(a>0. || b>0.),
                    "mixed bed/floating instance support");
        }
    }
    // Volume-local reflection and instance reflection compose: two mirrors
    // restore orientation; this must work as well as a single instance mirror.
    LoadedProject project;
    project.config=Slic3r::DynamicPrintConfig::full_print_config();
    auto* object=project.model.add_object();
    object->add_volume(Slic3r::TriangleMesh(Slic3r::its_make_cube(10.,10.,10.)));
    auto* volume=object->add_volume(Slic3r::TriangleMesh(Slic3r::its_make_cube(10.,10.,10.)));
    volume->set_mirror(Slic3r::Vec3d(-1.,1.,1.));
    volume->set_offset(Slic3r::Vec3d(30.,0.,0.));
    auto* instance=object->add_instance();
    for (double mirror : {1.,-1.}) {
        instance->set_mirror(Slic3r::Vec3d(mirror,1.,1.));
        const auto plans=smart_slice_plans(project,Json::object());
        for (const auto& plan:plans.at("plans")) {
            require(!plan.at("patch").at("enable_support").get<bool>(),"mixed volume bed support");
            require(std::abs(plan.at("overhangRisk").get<double>())<1e-9,"mixed volume bed risk");
        }
    }
    std::cout << "PASS mixed reflected instances: 4 bed/floating combinations plus volume/instance mirror composition\n";
}
void check_partition_and_errors()
{
    LoadedProject project;
    project.config=Slic3r::DynamicPrintConfig::full_print_config();
    auto* object=project.model.add_object();
    // Two thin model partitions span a thick object. Keep the existing single
    // whole-model thin-span semantics; per-volume analysis would warn here.
    object->add_volume(Slic3r::TriangleMesh(Slic3r::its_make_cube(10.,10.,.5)));
    auto* upper=object->add_volume(Slic3r::TriangleMesh(Slic3r::its_make_cube(10.,10.,.5)));
    upper->set_offset(Slic3r::Vec3d(0.,0.,9.5));
    auto* instance=object->add_instance();
    const auto plans=smart_slice_plans(project,Json::object());
    for(const auto& plan:plans.at("plans")) {
        const auto warnings=plan.at("warnings").get<std::vector<std::string>>();
        require(std::find(warnings.begin(),warnings.end(),"thin_wall_features")==warnings.end(),"whole model partition span");
        require(std::find(warnings.begin(),warnings.end(),"open_or_non_manifold_mesh")==warnings.end(),"closed partition bodies remain closed");
    }
    // An invalid transformed coordinate still propagates the core diagnostic.
    instance->set_offset(Slic3r::Vec3d(0.,0.,std::numeric_limits<double>::quiet_NaN()));
    bool rejected=false;
    try { smart_slice_plans(project,Json::object()); }
    catch(const std::runtime_error& e) { rejected=std::string(e.what())=="smart_slice_geometry_non_finite"; }
    require(rejected,"nonfinite transform propagates");
    instance->set_offset(Slic3r::Vec3d::Zero());
    require(smart_slice_plans(project,Json::object()).at("plans").size()==3,"valid request after transform error");
    rejected=false;
    try { smart_slice_config(project,Json::object(),"unknown"); }
    catch(const std::invalid_argument& e) { rejected=std::string(e.what())=="QUALITY_UNSUPPORTED"; }
    require(rejected,"invalid quality propagates");
    std::cout << "PASS whole-model partition semantics and transform/quality error recovery\n";
}
int main() {
    try {
        check_mixed_orientation();
        check_partition_and_errors();
        LoadedProject project;
        project.config = Slic3r::DynamicPrintConfig::full_print_config();
        auto* object = project.model.add_object();
        object->add_volume(Slic3r::TriangleMesh(Slic3r::its_make_cube(10., 10., 10.)));
        auto* instance = object->add_instance();
        const Json request{{"smartSliceContext", {{"profileHash", "parity-profile"}, {"capabilityHash", "parity-capability"}}}};
        const auto bed = smart_slice_plans(project, request);
        require(bed.at("plans").size() == 3, "three plans required");
        require(bed.at("algorithmVersion") == "smart-slicing-core/1", "algorithm identity");
        require(bed.at("profileHash") == "parity-profile", "profile identity");
        require(bed.at("capabilityHash") == "parity-capability", "capability identity");
        require(bed.at("effectiveConfigHash") == json_sha256(config_json(project.config)), "config identity");
        const char* modes[] = {"balanced", "speed", "quality"};
        const char* qualities[] = {"standard", "fast", "fine"};
        for (size_t i=0; i<3; ++i) {
            const auto& plan=bed.at("plans").at(i);
            require(plan.at("mode") == modes[i], "mode order");
            require(!plan.at("patch").at("enable_support").get<bool>(), "bed support disabled");
            require(plan.at("estimatedTimeSeconds").get<double>() > 0., "positive time");
            require(plan.at("estimatedMaterialGrams").get<double>() > 0., "positive material");
            const auto cfg=smart_slice_config(project,request,qualities[i]);
            const auto height=config_json(cfg).at("layer_height");
            const double mm=height.is_number()?height.get<double>():std::stod(height.get<std::string>());
            require(std::abs(mm-plan.at("patch").at("layer_height").get<double>()) < 1e-9, "quality dispatch");
        }
        instance->set_offset(Slic3r::Vec3d(0., 0., 4.));
        const auto floating=smart_slice_plans(project,request);
        for (const auto& plan:floating.at("plans")) {
            require(plan.at("patch").at("enable_support").get<bool>(), "floating support enabled");
            require(plan.at("overhangRisk").get<double>() > .1, "floating overhang risk");
        }
        apply_config_overrides(project.config,Json{{"min_layer_height", Json::array({.23})}, {"max_layer_height", Json::array({.21})}});
        bool rejected=false;
        try { smart_slice_plans(project,request); }
        catch (const std::runtime_error& e) { rejected=std::string(e.what()) == "smart_slice_layer_height_no_common_range"; }
        require(rejected,"bridge rejects empty layer range");
        std::cout << "PASS actual smart_slice_plans/smart_slice_config: three-plan JSON, all quality mappings, transformed floating geometry, empty layer interval\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
