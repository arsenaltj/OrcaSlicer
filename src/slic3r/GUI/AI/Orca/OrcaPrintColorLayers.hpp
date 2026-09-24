#pragma once

#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include "slic3r/GUI/AI/Model/LocalPrintRecipeProofState.hpp"
#include "OrcaPrintColorRegions.hpp"
#include "OrcaPrintColorTargets.hpp"
#include <atomic>
#include <chrono>
#include <thread>

namespace Slic3r::GUI::OrcaPrintColorLayers {
using Json = nlohmann::json;

inline Json config_identity(const DynamicPrintConfig& config)
{
    Json out=Json::object();
    for(const auto& key:config.keys()) out[key]=config.option(key)->serialize();
    return out;
}
inline Json matrix_identity(const Transform3d& transform)
{
    Json out=Json::array();
    for(int r=0;r<4;++r) for(int c=0;c<4;++c) out.push_back(transform.matrix()(r,c));
    return out;
}
inline std::shared_ptr<Model> snapshot_model(const ModelObject& object)
{
    auto model=std::make_shared<Model>();
    for(const auto* volume:object.volumes) if(!volume->material_id().empty()) {
        const auto* material=volume->material();
        if(!material) throw std::runtime_error("The target references a missing model material.");
        if(!model->get_material(volume->material_id())) model->add_material(volume->material_id(),*material);
    }
    model->add_object(object);
    return model;
}
// In-session identity includes geometry, transforms and every object override.
// The emitted digest contains no raw connection or configuration values.
inline std::string identity(const ModelObject& object,const DynamicPrintConfig& config)
{
    Json out={{"config",config_identity(config)},{"object",object.id().id},
        {"overrides",config_identity(object.config.get())},{"profile",object.layer_height_profile.get()},
        {"instances",Json::array()},{"volumes",Json::array()},{"ranges",Json::array()}};
    for(const auto* instance:object.instances) out["instances"].push_back(matrix_identity(instance->get_matrix()));
    for(const auto& range:object.layer_config_ranges)
        out["ranges"].push_back({range.first.first,range.first.second,config_identity(range.second.get())});
    for(const auto* volume:object.volumes) {
        Json material=nullptr;
        if(!volume->material_id().empty()) {
            if(!volume->material()) throw std::runtime_error("The target references a missing model material.");
            material={{"id",volume->material_id()},{"config",config_identity(volume->material()->config.get())}};
        }
        const auto& mesh=volume->mesh().its;
        // Hash exact stored mesh arrays, used only within this application run.
        auto hash_bytes=[](const void* data,size_t size) {
            unsigned char hash[EVP_MAX_MD_SIZE];unsigned int count=0;
            if(EVP_Digest(data,size,hash,&count,EVP_sha256(),nullptr)!=1) throw std::runtime_error("Cannot identify slice geometry.");
            const char* hex="0123456789abcdef";std::string text;
            for(unsigned int i=0;i<count;++i){text+=hex[hash[i]>>4];text+=hex[hash[i]&15];}
            return text;
        };
        out["volumes"].push_back({{"id",volume->id().id},{"type",int(volume->type())},
            {"matrix",matrix_identity(volume->get_matrix())},{"config",config_identity(volume->config.get())},{"material",material},
            {"paint_stamp",volume->mmu_segmentation_facets.timestamp()},
            {"vertices",hash_bytes(mesh.vertices.data(),mesh.vertices.size()*sizeof(Vec3f))},
            {"faces",hash_bytes(mesh.indices.data(),mesh.indices.size()*sizeof(mesh.indices.front()))}});
    }
    return LocalPrintRecipeProofState::digest(out);
}

// Explicitly requested, isolated native slice. It never touches the workspace
// Print or exports G-code. Region occupancy is not minimum-feature/tool proof.
inline Json inspect(const Model& model,const DynamicPrintConfig& config,const std::atomic<bool>& cancelled,
    const Vec3d& plate_origin=Vec3d::Zero(),int plate_index=0,const OrcaPrintColorTargets::Partition* logical=nullptr)
{
    if(cancelled.load()) throw std::runtime_error("Layer inspection cancelled.");
    Print print;
    print.set_plate_origin(plate_origin);print.set_plate_index(plate_index);
    print.set_status_silent();
    print.apply(model,config);
    const auto error=print.validate();
    if(!error.string.empty()) throw std::runtime_error(error.string);
    if(print.objects().empty()) throw std::runtime_error("No printable object exists in this snapshot.");
    struct Cancellation {
        Print& print;const std::atomic<bool>& requested;std::atomic<bool> done{false};std::thread worker;
        Cancellation(Print& p,const std::atomic<bool>& r):print(p),requested(r),worker([this]{
            while(!done.load()) {if(requested.load()){print.cancel();return;} std::this_thread::sleep_for(std::chrono::milliseconds(25));}
        }){}
        ~Cancellation(){done=true;worker.join();}
    } cancellation(print,cancelled);
    Json result={{"schema","orcaslicer.local-object-layer-snapshot.v3"},{"scope","native-material-region-geometry"},
        {"xy_coordinate_space","native-print-object-slice"},{"xy_unit_mm",SCALING_FACTOR},
        {"logical_target_mapping_verified",false},
        {"objects",Json::array()},{"layer_count",0},{"occupied_layer_count",0},
        {"region_feature_size_verified",false},{"final_tool_assignment_verified",false}};
    size_t total=0,occupied=0,region_layers=0,thin_islands=0,remaining_points=10000000;
    for(size_t index=0;index<print.objects().size();++index) {
        auto* object=print.get_object(index);
        object->slice();
        Json entry={{"object_id",object->model_object()->id().id},{"instance_count",object->instances().size()},{"layers",Json::array()}};
        for(const auto* layer:object->layers()) {
            if(cancelled.load()) throw std::runtime_error("Layer inspection cancelled.");
            if(!std::isfinite(layer->height) || layer->height<=0.) throw std::runtime_error("Invalid native layer thickness.");
            auto regions=OrcaPrintColorRegions::capture(*object,*layer,cancelled,remaining_points);
            region_layers+=regions.size();
            for(const auto& region:regions)thin_islands+=region.at("islands_without_inset_core").get<size_t>();
            entry["layers"].push_back({{"id",layer->id()},{"bottom_z_mm",layer->bottom_z()},
                {"top_z_mm",layer->print_z},{"height_mm",layer->height},{"has_geometry",!layer->empty()},
                {"regions",std::move(regions)}});
            ++total;if(!layer->empty())++occupied;
        }
        if(logical)entry["logical_projection"]=OrcaPrintColorTargets::project(*object,*logical,cancelled,remaining_points);
        result["objects"].push_back(std::move(entry));
    }
    if(cancelled.load()) throw std::runtime_error("Layer inspection cancelled.");
    result["layer_count"]=total;result["occupied_layer_count"]=occupied;
    result["region_layer_count"]=region_layers;result["islands_without_inset_core"]=thin_islands;
    return result;
}
} // namespace Slic3r::GUI::OrcaPrintColorLayers
