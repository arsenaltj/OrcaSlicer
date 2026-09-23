#pragma once

#include "OrcaPrintColorRegions.hpp"
#include "libslic3r/MultiMaterialSegmentation.hpp"
#include "libslic3r/Model.hpp"
#include <optional>
#include <limits>

namespace Slic3r::GUI::OrcaPrintColorTargets {
using Json = nlohmann::json;

// Logical labels are an independent annotation passed to the native projector.
// They never become printer filaments, ModelVolume painting or print settings.
struct Partition {
    ObjectID volume_id;
    std::shared_ptr<Model> annotations;
    std::vector<ObjectID> annotation_volume_ids;
    std::vector<size_t> face_counts;
    std::vector<std::optional<size_t>> physical_slots;
};
inline Partition partition(const ModelVolume& volume,const std::vector<size_t>& labels,
    const std::vector<std::optional<size_t>>& slots,const std::atomic<bool>& cancelled)
{
    if(slots.empty() || slots.size()>size_t(EnforcerBlockerType::ExtruderMax) ||
        labels.size()!=volume.mesh().its.indices.size() || labels.size()>size_t(std::numeric_limits<int>::max()))
        throw std::runtime_error("Invalid logical target partition.");
    Partition result;result.volume_id=volume.id();result.physical_slots=slots;result.face_counts.resize(slots.size(),0);
    TriangleSelector selector(volume.mesh());
    for(size_t face=0;face<labels.size();++face) {
        if(cancelled.load())throw std::runtime_error("Target inspection cancelled.");
        if(labels[face]>=slots.size())throw std::runtime_error("Logical target index is out of range.");
        ++result.face_counts[labels[face]];
        selector.set_facet(int(face),EnforcerBlockerType(labels[face]+1));
    }
    if(std::find(result.face_counts.begin(),result.face_counts.end(),0)!=result.face_counts.end())
        throw std::runtime_error("A logical target has no source faces.");
    result.annotations=std::make_shared<Model>();
    auto* annotated=result.annotations->add_object(*volume.get_object());
    for(size_t index=0;index<annotated->volumes.size();++index) {
        auto* copy=annotated->volumes[index];const auto id=volume.get_object()->volumes[index]->id();
        result.annotation_volume_ids.push_back(id);
        if(id==volume.id())copy->mmu_segmentation_facets.set_data(selector.serialize());
        else copy->mmu_segmentation_facets.reset();
    }
    return result;
}
inline double area_mm2(const ExPolygons& polygons)
{
    double area=0.;for(const auto& polygon:polygons)area+=polygon.area();
    return area*SCALING_FACTOR*SCALING_FACTOR;
}
inline Json project(const PrintObject& object,const Partition& partition,
    const std::atomic<bool>& cancelled,size_t& remaining_points)
{
    auto check_cancel=[&]{if(cancelled.load())throw std::runtime_error("Target inspection cancelled.");};
    check_cancel();
    const auto& volumes=object.model_object()->volumes;
    if(std::none_of(volumes.begin(),volumes.end(),[&](const ModelVolume* v){return v->id()==partition.volume_id && v->is_model_part();}))
        throw std::runtime_error("The logical target volume is absent from the sliced object.");
    const auto extract=[&](const ModelVolume& volume)->ModelVolumeFacetsInfo {
        for(size_t index=0;index<partition.annotation_volume_ids.size();++index)if(partition.annotation_volume_ids[index]==volume.id())
            return {partition.annotations->objects.front()->volumes[index]->mmu_segmentation_facets,volume.id()==partition.volume_id,false};
        throw std::runtime_error("A projection annotation is missing.");
    };
    const auto projected=segmentation_by_painting(object,extract,partition.face_counts.size()+1,
        float(object.config().mmu_segmented_region_max_width.value),
        float(object.config().mmu_segmented_region_interlocking_depth.value),object.config().interlocking_beam.value,
        IncludeTopAndBottomLayers::Yes,check_cancel);
    if(projected.size()!=object.layers().size())throw std::runtime_error("Logical projection layer count disagrees.");
    Json result={{"scope","logical-paint-projection-on-native-material-regions"},
        {"volume_id",partition.volume_id.id},{"source_face_counts",partition.face_counts},
        {"target_count",partition.face_counts.size()},{"physical_slots_0based",Json::array()},
        {"layers",Json::array()},{"recipe_feasibility_verified",false},{"region_layer_count",0}};
    for(const auto& slot:partition.physical_slots)result["physical_slots_0based"].push_back(slot ? Json(*slot) : Json(nullptr));
    size_t count=0;
    for(size_t i=0;i<projected.size();++i) {
        check_cancel();const auto& layer=*object.layers()[i];
        ExPolygons actual,claimed;
        for(const auto* region:layer.regions())append(actual,to_expolygons(region->slices.surfaces));
        actual=union_ex(actual);
        Json row={{"layer_id",layer.id()},{"bottom_z_mm",layer.bottom_z()},{"top_z_mm",layer.print_z},
            {"targets",Json::array()},{"projected_overlap_mm2",0.},{"projected_outside_model_mm2",0.}};
        double overlap=0.,outside=0.;
        for(size_t target=0;target<partition.face_counts.size();++target) {
            check_cancel();const auto polygons=union_ex(projected[i].at(target));
            overlap+=area_mm2(intersection_ex(polygons,claimed));
            outside+=area_mm2(diff_ex(polygons,actual));
            append(claimed,polygons);claimed=union_ex(claimed);
            Json intersections=Json::array();
            for(const auto* region:layer.regions()) {
                check_cancel();auto piece=intersection_ex(polygons,to_expolygons(region->slices.surfaces));
                if(piece.empty())continue;
                const auto& native=region->region();
                auto geometry=OrcaPrintColorRegions::geometry(piece,native.flow(object,frExternalPerimeter,layer.height,layer.id()==0).width(),cancelled,remaining_points);
                geometry["native_region_id"]=native.print_region_id();
                const unsigned int filament=native.extruder(frExternalPerimeter);
                geometry["native_external_wall_filament_id_1based"]=filament;
                geometry["direct_slot_matches_native_wall"]=partition.physical_slots[target] ? Json(*partition.physical_slots[target]+1==filament) : Json(nullptr);
                intersections.push_back(std::move(geometry));++count;
            }
            if(!intersections.empty())row["targets"].push_back({{"target_index",target},{"native_regions",std::move(intersections)}});
        }
        row["projected_overlap_mm2"]=overlap;row["projected_outside_model_mm2"]=outside;
        row["model_without_projected_target_mm2"]=area_mm2(diff_ex(actual,claimed));
        result["layers"].push_back(std::move(row));
    }
    check_cancel();result["region_layer_count"]=count;
    return result;
}
} // namespace Slic3r::GUI::OrcaPrintColorTargets
