#include "smart_slice_bridge.hpp"
#include "slic3r/AI/SmartSlicing/Application/SmartSlicingEngine.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace orca::mobile::slicing {
namespace {
using namespace Slic3r::AI::SmartSlicing;
double number(const Json& j, const char* key, double fallback) {
    if (!j.contains(key)) return fallback;
    const auto& raw=j.at(key);
    if(raw.is_array()) {if(raw.empty()) return fallback; Json scalar{{key,raw.front()}};return number(scalar,key,fallback);}
    try {
        size_t consumed=0;
        const double value=raw.is_number()?raw.get<double>():std::stod(raw.get<std::string>(),&consumed);
        if(!std::isfinite(value)) throw std::invalid_argument("nonfinite");
        return value;
    } catch(...) {throw std::invalid_argument(std::string("SMART_SLICE_INVALID_PARAMETER:")+key);}
}
Json core_input(const LoadedProject& project,const Json& request) {
    const Json cfg=config_json(project.config);
    const Json context=request.value("smartSliceContext",Json::object());
    if(!context.is_object()) throw std::invalid_argument("SMART_SLICE_CONTEXT_INVALID");
    const Json machine=context.value("machine",Json::object());
    const Json filament=context.value("filament",Json::object());
    return Json{{"config",cfg},{"context",context},{"machine",machine},{"filament",filament}};
}
}
Json smart_slice_plans(const LoadedProject& project,const Json& request) {
    const auto input=core_input(project,request);
    const auto& cfg=input.at("config");const auto& ctx=input.at("context");
    const auto& m=input.at("machine");const auto& f=input.at("filament");
    MachinePreset machine;FilamentPreset filament;
    machine.nozzle_diameter_mm=number(cfg,"nozzle_diameter",0.4);
    machine.minimum_layer_height_mm=number(cfg,"min_layer_height",machine.nozzle_diameter_mm*0.1);
    const double configured_maximum_layer=number(cfg,"max_layer_height",machine.nozzle_diameter_mm*0.7);
    machine.maximum_layer_height_mm=configured_maximum_layer>0.0?
        configured_maximum_layer:machine.nozzle_diameter_mm*0.7;
    machine.maximum_outer_wall_speed_mm_s=number(cfg,"outer_wall_speed",80);
    machine.machine_max_speed_mm_s=number(m,"machineMaxSpeed",number(cfg,"machine_max_speed_x",machine.maximum_outer_wall_speed_mm_s));
    machine.outer_wall_speed_cap_mm_s=number(m,"smartSliceOuterWallSpeedCap",machine.maximum_outer_wall_speed_mm_s);
    machine.maximum_volumetric_speed_mm3_s=number(m,"maxVolumetricFlow",number(cfg,"filament_max_volumetric_speed",12));
    machine.inner_wall_speed_mm_s=number(cfg,"inner_wall_speed",100);
    machine.sparse_infill_speed_mm_s=number(cfg,"sparse_infill_speed",120);
    machine.top_surface_speed_mm_s=number(cfg,"top_surface_speed",60);
    machine.support_speed_mm_s=number(cfg,"support_speed",80);
    machine.travel_speed_mm_s=number(cfg,"travel_speed",180);
    machine.default_acceleration_mm_s2=number(m,"defaultPrintAcceleration",number(cfg,"default_acceleration",1000));
    machine.outer_wall_acceleration_mm_s2=number(m,"outerWallAcceleration",machine.default_acceleration_mm_s2);
    machine.travel_acceleration_mm_s2=number(m,"travelAcceleration",machine.default_acceleration_mm_s2);
    const double acceleration_limit=number(m,"machineMaxAcceleration",machine.default_acceleration_mm_s2);
    if(acceleration_limit<=0) throw std::invalid_argument("SMART_SLICE_ACCELERATION_LIMIT_INVALID");
    machine.default_acceleration_mm_s2=std::clamp(machine.default_acceleration_mm_s2,1.0,acceleration_limit);
    machine.outer_wall_acceleration_mm_s2=std::clamp(machine.outer_wall_acceleration_mm_s2,1.0,acceleration_limit);
    machine.travel_acceleration_mm_s2=std::clamp(machine.travel_acceleration_mm_s2,1.0,acceleration_limit);
    machine.baseline_layer_height_mm=number(cfg,"layer_height",0.2);
    machine.baseline_wall_loops=int(number(cfg,"wall_loops",3));
    machine.baseline_infill_density_percent=number(cfg,"sparse_infill_density",15);
    machine.profile_hash=ctx.value("profileHash",std::string());
    machine.capability_hash=ctx.value("capabilityHash",std::string());
    machine.effective_config_hash=json_sha256(cfg);
    filament.id=f.value("materialId",std::string());
    filament.density_g_cm3=number(f,"density",number(cfg,"filament_density",1.24));
    filament.extrusion_multiplier=number(f,"extrusionMultiplier",number(cfg,"filament_flow_ratio",1));
    filament.maximum_volumetric_speed_mm3_s=number(f,"maxVolumetricFlow",number(cfg,"filament_max_volumetric_speed",12));
    // Analyze the actual transformed source geometry, including every instance.
    TriangleMeshSnapshot mesh;
    for(const auto* object:project.model.objects) for(const auto* instance:object->instances)
        for(const auto* volume:object->volumes) if(volume->is_model_part()) {
            const auto transform=instance->get_matrix()*volume->get_matrix();
            const auto& its=volume->mesh().its;const auto base=uint32_t(mesh.vertices.size());
            for(const auto& point:its.vertices) {const auto v=transform*point.cast<double>();mesh.vertices.push_back({v.x(),v.y(),v.z()});}
            for(const auto& triangle:its.indices) mesh.triangles.push_back({{base+uint32_t(triangle[0]),base+uint32_t(triangle[1]),base+uint32_t(triangle[2])}});
        }
    const auto analysis=SmartSlicingEngine().AnalyzeAndPrecompute(mesh,machine,filament);
    if(!analysis.completed) throw std::runtime_error(analysis.diagnostic_code);
    Json plans=Json::array();
    for(const auto& plan:analysis.recommendations) {
        Json patch{{"layer_height",plan.layer_height_mm},{"wall_loops",plan.wall_loops},
            {"sparse_infill_density",std::to_string(plan.sparse_infill_density_percent)+"%"},
            {"sparse_infill_pattern",plan.sparse_infill_pattern},{"outer_wall_speed",plan.outer_wall_speed_mm_s},
            {"enable_support",plan.enable_support},{"support_threshold_angle",plan.support_threshold_angle_deg},
            {"wall_generator",plan.arachne_wall_generator?"arachne":"classic"},
            {"outer_wall_line_width",plan.line_width_mm},{"default_acceleration",Json::array({machine.default_acceleration_mm_s2})},
            {"outer_wall_acceleration",Json::array({machine.outer_wall_acceleration_mm_s2})},
            {"travel_acceleration",Json::array({machine.travel_acceleration_mm_s2})}};
        if(plan.enable_support)patch["support_type"]=plan.support_type;
        plans.push_back(Json{{"mode",slice_mode_id(plan.mode)},{"patch",patch},
            {"warnings",plan.warnings},{"adaptiveLayerHeightRequested",plan.adaptive_layer_height},
            {"estimatedTimeSeconds",plan.estimated_time_seconds},{"estimatedMaterialGrams",plan.estimated_material_grams},
            {"surfaceQualityScore",plan.surface_quality_score},{"overhangRisk",plan.overhang_risk_score}});
    }
    return Json{{"algorithmVersion",SMART_SLICE_ALGORITHM_VERSION},
        {"profileHash",machine.profile_hash},{"capabilityHash",machine.capability_hash},
        {"effectiveConfigHash",machine.effective_config_hash},{"requestedConfig",cfg},
        {"effectiveFlow",std::min(machine.maximum_volumetric_speed_mm3_s,filament.maximum_volumetric_speed_mm3_s)},
        {"machineMaxSpeed",machine.machine_max_speed_mm_s},{"outerWallSpeedCap",machine.outer_wall_speed_cap_mm_s},
        {"plans",plans}};
}
Slic3r::DynamicPrintConfig smart_slice_config(const LoadedProject& project,const Json& request,const std::string& quality) {
    const std::string mode=quality=="fast"?"speed":quality=="fine"?"quality":quality=="standard"?"balanced":"";
    if(mode.empty())throw std::invalid_argument("QUALITY_UNSUPPORTED");
    const auto analysis=smart_slice_plans(project,request);
    auto result=project.config;
    for(const auto& plan:analysis.at("plans")) if(plan.at("mode")==mode) {
        apply_config_overrides(result,plan.at("patch"));return result;
    }
    throw std::runtime_error("SMART_SLICE_PLAN_MISSING");
}
}
