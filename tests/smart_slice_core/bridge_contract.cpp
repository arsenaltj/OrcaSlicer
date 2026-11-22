#include "orca_mobile_slicing_sdk/smart_slice_bridge.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include <iostream>
#include <cmath>
#include <stdexcept>
using namespace orca::mobile::slicing;
void require(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
int main() {
    try {
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
