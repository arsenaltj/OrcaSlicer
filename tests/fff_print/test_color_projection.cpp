#include <catch2/catch_all.hpp>
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Preset.hpp"
#include "slic3r/GUI/AI/Orca/OrcaPrintColorTargets.hpp"
#include "slic3r/GUI/AI/Orca/OrcaPrintColorLayers.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include "libslic3r/ExtrusionEntityCollection.hpp"

using namespace Slic3r;
namespace {
using Json=nlohmann::json;
using Groups=std::vector<ExPolygons>;
using Layers=std::vector<Groups>;
namespace Target=Slic3r::GUI::OrcaPrintColorTargets;
void trace_enabled(bool enabled) {
#ifdef _WIN32
    _putenv_s("ORCA_COLOR_SLICE_TRACE",enabled ? "1" : "");
#else
    if(enabled)setenv("ORCA_COLOR_SLICE_TRACE","1",1);else unsetenv("ORCA_COLOR_SLICE_TRACE");
#endif
}

Groups read_trace(const std::filesystem::path& path) {
    std::ifstream file(path);if(!file)throw std::runtime_error("Missing exact trace: "+path.string());
    Groups result;size_t group=0;std::string text;
    while(std::getline(file,text)) {
        std::istringstream line(text);char kind;line>>kind;
        if(kind=='G'){line>>group;result.resize(std::max(result.size(),group+1));}
        else if(kind=='C' || kind=='H') {
            Points points;coord_t x,y;while(line>>x>>y)points.emplace_back(x,y);
            if(kind=='C')result[group].emplace_back(Polygon(std::move(points)));
            else result[group].back().holes.emplace_back(std::move(points));
        }
    }
    return result;
}
ExPolygons polygons_from_json(const Json& polygons) {
    auto ring=[](const Json& data){Points out;for(const auto& p:data)out.emplace_back(p.at(0).get<coord_t>(),p.at(1).get<coord_t>());return Polygon(std::move(out));};
    ExPolygons out;
    for(const auto& p:polygons) {
        ExPolygon polygon(ring(p.at("contour")));
        for(const auto& h:p.at("holes"))polygon.holes.push_back(ring(h));
        out.push_back(std::move(polygon));
    }
    return out;
}
Json compare(const Layers& a,const Layers& b) {
    if(a.size()!=b.size())throw std::runtime_error("Different layer counts.");
    Json rows=Json::array();double total=0.;
    for(size_t i=0;i<a.size();++i) {
        if(a[i].size()!=b[i].size())throw std::runtime_error("Different group counts.");
        double difference=0.;
        for(size_t slot=0;slot<a[i].size();++slot)
            difference+=Target::area_mm2(diff_ex(a[i][slot],b[i][slot]))+Target::area_mm2(diff_ex(b[i][slot],a[i][slot]));
        total+=difference;rows.push_back(difference);
    }
    return {{"symmetric_difference_area_sum_mm2",total},{"per_layer_mm2",std::move(rows)}};
}
Json compare_in_mask(const Layers& a,const Layers& b,const std::vector<ExPolygons>& masks) {
    Layers clipped_a=a,clipped_b=b;
    for(size_t i=0;i<a.size();++i)for(size_t group=0;group<a[i].size();++group) {
        clipped_a[i][group]=intersection_ex(a[i][group],masks[i]);
        clipped_b[i][group]=intersection_ex(b[i][group],masks[i]);
    }
    return compare(clipped_a,clipped_b);
}
Layers group_targets(const Layers& source,const std::vector<std::optional<size_t>>& slots,size_t count) {
    Layers out(source.size(),Groups(count));
    for(size_t i=0;i<source.size();++i) {
        for(size_t t=0;t<slots.size();++t)append(out[i].at(*slots[t]),source[i][t]);
        for(auto& polygons:out[i])polygons=union_ex(polygons);
    }
    return out;
}
Groups collapse_trace(const Groups& source,const std::vector<std::optional<size_t>>& slots,size_t count,size_t buffers) {
    if(source.size()!=(slots.size()+1)*buffers)throw std::runtime_error("Unexpected trace group count.");
    Groups out((count+1)*buffers);
    for(size_t color=0;color<=slots.size();++color)for(size_t buffer=0;buffer<buffers;++buffer)
        append(out[(color==0 ? 0 : *slots[color-1]+1)*buffers+buffer],source[color*buffers+buffer]);
    for(auto& polygons:out)polygons=union_ex(polygons);
    return out;
}
}

// Opt-in because this diagnostic uses a user-owned local fixture and writes
// exact slicing evidence; it cannot run in ordinary self-contained CI.
TEST_CASE("Saved target projection distinguishes input stage changes from label granularity", "[ColorProjectionEvidence][.]")
{
    const char* input=std::getenv("ORCA_PROJECTION_PROJECT");
    const char* version=std::getenv("ORCA_PROJECTION_VERSION");
    const char* baseline=std::getenv("ORCA_PROJECTION_BASELINE");
    const char* destination=std::getenv("ORCA_PROJECTION_EVIDENCE");
    if(!input || !version || !baseline || !destination)SKIP("Explicit local project, version, baseline and empty evidence directory are required.");
    const std::filesystem::path root(destination);
    REQUIRE(std::filesystem::exists(root));REQUIRE(std::filesystem::is_empty(root));
    struct State {
        std::string data=data_dir();bool trace=std::getenv("ORCA_COLOR_SLICE_TRACE") && std::string(std::getenv("ORCA_COLOR_SLICE_TRACE"))=="1";
        ~State(){trace_enabled(trace);set_data_dir(data);}
    } state;
    set_data_dir(root.string());
    std::filesystem::create_directory(root/"model-backup");
    std::filesystem::create_directory(root/"model-backup"/"Metadata");
    Model model;model.set_backup_path((root/"model-backup").string());
    // Follow the 3MF import contract: deserialize into an empty dictionary,
    // then fill missing settings from the full defaults before constructing Print.
    DynamicPrintConfig config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Enable};
    PlateDataPtrs plates;std::vector<Preset*> presets;bool bbl=false,orca=false;Semver version_number;
    REQUIRE(load_bbs_3mf(input,&config,&substitutions,&model,&plates,&presets,&bbl,&orca,&version_number,nullptr,LoadStrategy::LoadModel|LoadStrategy::LoadConfig));
    release_PlateData_list(plates);for(auto* preset:presets)delete preset;
    auto complete=DynamicPrintConfig::full_print_config();complete.apply(config);config=std::move(complete);
    REQUIRE(model.objects.size()==1);REQUIRE(model.objects.front()->volumes.size()==1);
    auto* volume=model.objects.front()->volumes.front();
    Json saved;{std::ifstream file(version);file>>saved;}saved=saved.at("result");
    const auto labels=saved.at("face_targets").get<std::vector<size_t>>();
    REQUIRE(labels.size()==volume->mesh().its.indices.size());
    std::vector<std::optional<size_t>> slots;
    for(const auto& target:saved.at("targets"))slots.push_back(target.at("physical_slot").get<size_t>());
    std::atomic<bool> cancel{false};
    const auto logical=Target::partition(*volume,labels,slots,cancel);
    const auto identity=GUI::OrcaPrintColorLayers::identity(*model.objects.front(),config);
    Print print;print.set_status_silent();print.set_plate_index(0);print.set_plate_origin(Vec3d::Zero());print.apply(model,config);
    REQUIRE(print.objects().size()==1);
    trace_enabled(true);auto* object=print.get_object(0);object->slice();trace_enabled(false);
    REQUIRE(object->layers().size()==500);
    const size_t count=print.config().filament_colour.size();REQUIRE(count==4);
    Layers actual(object->layers().size(),Groups(count));
    std::vector<std::vector<Surfaces>> backup;
    for(size_t i=0;i<object->layers().size();++i) {
        backup.emplace_back();
        for(const auto* region:object->layers()[i]->regions()) {
            backup.back().push_back(region->slices.surfaces);
            append(actual[i].at(region->region().extruder(frExternalPerimeter)-1),to_expolygons(region->slices.surfaces));
        }
        for(auto& polygons:actual[i])polygons=union_ex(polygons);
    }
    Json old;{std::ifstream file(baseline);file>>old;}
    Layers old_actual(object->layers().size(),Groups(count));
    for(size_t i=0;i<old_actual.size();++i)for(const auto& region:old.at("objects").at(0).at("layers").at(i).at("regions"))
        append(old_actual[i].at(region.at("role_filament_ids_1based").at("external_wall").get<size_t>()-1),polygons_from_json(region.at("polygons")));
    const auto physical=[&]{return multi_material_segmentation_by_painting(*object,[]{});};
    const auto targets=[&]{return segmentation_by_painting(*object,[&](const ModelVolume& mv)->ModelVolumeFacetsInfo {
        if(mv.id()!=logical.volume_id)throw std::runtime_error("Projection target identity changed.");
        return {logical.annotations->objects.front()->volumes.front()->mmu_segmentation_facets,true,false};
    },slots.size()+1,float(object->config().mmu_segmented_region_max_width.value),float(object->config().mmu_segmented_region_interlocking_depth.value),object->config().interlocking_beam.value,IncludeTopAndBottomLayers::Yes,[]{});};
    const auto physical_after=physical();const auto targets_after=group_targets(targets(),slots,count);
    Layers raw,original_projection;
    for(size_t i=0;i<object->layers().size();++i) {
        raw.push_back(read_trace(root/"SVG"/("mm-exact-0-raw-"+std::to_string(i)+".txt")));
        original_projection.push_back(read_trace(root/"SVG"/("mm-exact-0-merged-"+std::to_string(i)+".txt")));
        REQUIRE(raw.back().size()==object->layers()[i]->regions().size());
        for(size_t region=0;region<raw.back().size();++region)object->layers()[i]->get_region(region)->slices.set(raw.back()[region],stInternal);
    }
    const auto physical_before=physical();
    trace_enabled(true);const auto targets_before=group_targets(targets(),slots,count);trace_enabled(false);
    Json stages=Json::object();
    for(const std::string stage : {"raw","processed","sides","topown","bottomown","topshell","bottomshell","topbottom"}) {
        Layers coarse,fine;
        const bool input_stage=stage=="raw" || stage=="processed";
        const size_t buffers=(stage=="topown" || stage=="bottomown" || stage=="topshell" || stage=="bottomshell") ? 2 : 1;
        for(size_t i=0;i<object->layers().size();++i) {
            coarse.push_back(read_trace(root/"SVG"/("mm-exact-0-"+stage+"-"+std::to_string(i)+".txt")));
            auto groups=read_trace(root/"SVG"/("mm-exact-1-"+stage+"-"+std::to_string(i)+".txt"));
            fine.push_back(input_stage ? std::move(groups) : collapse_trace(groups,slots,count,buffers));
        }
        stages[stage]=compare(coarse,fine);
    }
    Json result={{"gui_baseline_vs_loaded_native",compare(old_actual,actual)},
        {"original_projection_vs_raw_replay",compare(original_projection,physical_before)},
        {"physical_projection_before_vs_after",compare(physical_before,physical_after)},
        {"logical_grouped_vs_physical_same_raw",compare(targets_before,physical_before)},
        {"logical_grouped_vs_physical_same_final",compare(targets_after,physical_after)},
        {"physical_projection_final_vs_actual",compare(physical_after,actual)},
        {"logical_projection_final_vs_actual",compare(targets_after,actual)},
        {"logical_projection_raw_vs_actual",compare(targets_before,actual)},
        {"layer_count",object->layers().size()},{"physical_filaments",count},{"logical_targets",slots.size()},
        {"same_raw_stage_comparisons",std::move(stages)}};
    // Locate differences near the exterior separately from the interior. These
    // geometric bands are diagnostics, not generated wall paths or print proof.
    Json bands=Json::object();
    for(const double width : {0.2,0.4,0.8,0.0}) {
        std::vector<ExPolygons> masks;Json widths=Json::array();double mask_area=0.;
        for(size_t i=0;i<actual.size();++i) {
            const auto& layer=*object->layers()[i];double used_width=width;
            if(width==0.)for(const auto* region:layer.regions())
                used_width=std::max(used_width,double(region->region().flow(*object,frExternalPerimeter,layer.height,layer.id()==0).width()));
            ExPolygons shape;for(const auto& polygons:actual[i])append(shape,polygons);shape=union_ex(shape);
            masks.push_back(diff_ex(shape,offset_ex(shape,-float(scale_(used_width)))));
            widths.push_back(used_width);mask_area+=Target::area_mm2(masks.back());
        }
        bands[width==0. ? "native_max_external_wall_width" : std::to_string(width)]={
            {"widths_mm",std::move(widths)},{"band_area_sum_mm2",mask_area},
            {"same_raw_labels",compare_in_mask(targets_before,physical_before,masks)},
            {"same_physical_before_after",compare_in_mask(physical_before,physical_after,masks)},
            {"logical_final_vs_actual",compare_in_mask(targets_after,actual,masks)}};
    }
    result["exterior_band_diagnostics"]=std::move(bands);
    for(size_t i=0;i<backup.size();++i)for(size_t region=0;region<backup[i].size();++region)object->layers()[i]->get_region(region)->slices.set(std::move(backup[i][region]));
    {std::ofstream file(root/"comparison.json");file<<result.dump(2);REQUIRE(bool(file));}
    CHECK(GUI::OrcaPrintColorLayers::identity(*model.objects.front(),config)==identity);
    CHECK_THAT(result.at("original_projection_vs_raw_replay").at("symmetric_difference_area_sum_mm2").get<double>(),Catch::Matchers::WithinAbs(0.,1e-6));
    CHECK_THAT(result.at("gui_baseline_vs_loaded_native").at("symmetric_difference_area_sum_mm2").get<double>(),Catch::Matchers::WithinAbs(0.,1e-6));
}

// Explicit local fixture only: records native generated walls and, when asked,
// exports G-code locally without sending it to a printer.
TEST_CASE("Native wall paths expose logical projection disagreement at the exterior", "[ColorToolpathEvidence][.]")
{
    const char* input=std::getenv("ORCA_PROJECTION_PROJECT");
    const char* version=std::getenv("ORCA_PROJECTION_VERSION");
    const char* traces=std::getenv("ORCA_TOOLPATH_PROJECTION_TRACES");
    const char* destination=std::getenv("ORCA_TOOLPATH_EVIDENCE");
    const char* resources=std::getenv("ORCA_TOOLPATH_RESOURCES");
    if(!input || !version || !traces || !destination || !resources)SKIP("Explicit local project, target version, R77 traces, runtime resources and empty evidence directory are required.");
    REQUIRE(std::filesystem::is_regular_file(std::filesystem::path(resources)/"info/nozzle_info.json"));
    struct RestoreResources {std::string original=resources_dir();~RestoreResources(){set_resources_dir(original);}} restore_resources;
    set_resources_dir(resources);
    const std::filesystem::path root(destination),trace_root(traces);
    REQUIRE(std::filesystem::is_directory(root));REQUIRE(std::filesystem::is_empty(root));
    std::filesystem::create_directories(root/"model-backup"/"Metadata");
    Model model;model.set_backup_path((root/"model-backup").string());DynamicPrintConfig config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Enable};
    PlateDataPtrs plates;std::vector<Preset*> presets;bool bbl=false,orca=false;Semver version_number;
    REQUIRE(load_bbs_3mf(input,&config,&substitutions,&model,&plates,&presets,&bbl,&orca,&version_number,nullptr,LoadStrategy::LoadModel|LoadStrategy::LoadConfig));
    release_PlateData_list(plates);for(auto* preset:presets)delete preset;
    auto complete=DynamicPrintConfig::full_print_config();complete.apply(config);config=std::move(complete);
    REQUIRE(model.objects.size()==1);REQUIRE(model.objects.front()->volumes.size()==1);
    const auto identity=GUI::OrcaPrintColorLayers::identity(*model.objects.front(),config);
    Json saved;{std::ifstream file(version);file>>saved;}saved=saved.at("result");
    std::vector<std::optional<size_t>> slots;
    for(const auto& target:saved.at("targets"))slots.push_back(target.at("physical_slot").get<size_t>());
    REQUIRE(slots.size()==12);
    Print print;print.set_status_silent();print.set_plate_index(0);print.set_plate_origin(Vec3d::Zero());print.apply(model,config);
    REQUIRE(print.objects().size()==1);auto* object=print.get_object(0);object->slice();
    std::vector<ExPolygons> actual_shapes;
    for(const auto* layer:object->layers()) {
        ExPolygons shape;for(const auto* region:layer->regions())append(shape,to_expolygons(region->slices.surfaces));
        actual_shapes.push_back(union_ex(shape));
    }
    print.process();
    REQUIRE(object->layers().size()==500);REQUIRE(print.config().filament_colour.size()==4);
    auto length=[](const Polylines& lines){double result=0.;for(const auto& line:lines)result+=line.length()*SCALING_FACTOR;return result;};
    auto coordinates=[](const Polylines& lines){Json result=Json::array();for(const auto& line:lines){Json points=Json::array();for(const auto& p:line.points)points.push_back({p.x(),p.y()});result.push_back(std::move(points));}return result;};
    REQUIRE(object->instances().size()==1);
    Json evidence={{"scope","native-perimeter-paths-after-print-process-before-gcode-export"},{"layers",Json::array()},
        {"instance_shift_scaled",{object->instances().front().shift.x(),object->instances().front().shift.y()}},
        {"final_tool_assignment_verified",false},{"surface_color_fidelity_verified",false}};
    size_t path_count=0;double total_external_length=0.;
    for(size_t i=0;i<object->layers().size();++i) {
        const auto& layer=*object->layers()[i];
        auto coarse=read_trace(trace_root/("mm-exact-0-merged-"+std::to_string(i)+".txt"));
        auto logical=read_trace(trace_root/("mm-exact-1-merged-"+std::to_string(i)+".txt"));
        const auto fine=group_targets(Layers{std::move(logical)},slots,4).front();
        ExPolygons processed_shape,all_coarse,all_fine;
        for(const auto* region:layer.regions())append(processed_shape,to_expolygons(region->slices.surfaces));processed_shape=union_ex(processed_shape);
        const auto& shape=actual_shapes.at(i);
        for(const auto& p:coarse)append(all_coarse,p);all_coarse=union_ex(all_coarse);
        for(const auto& p:fine)append(all_fine,p);all_fine=union_ex(all_fine);
        Json row={{"layer_id",layer.id()},{"top_z_mm",layer.print_z},{"paths",Json::array()},
            {"slice_vs_processed_shape_difference_mm2",Target::area_mm2(diff_ex(shape,processed_shape))+Target::area_mm2(diff_ex(processed_shape,shape))},
            {"external_length_mm",0.},{"near_exterior_external_length_mm",0.},
            {"near_exterior_outside_physical_projection_mm",0.},{"near_exterior_outside_logical_slot_mm",0.},
            {"near_exterior_without_any_logical_target_mm",0.},{"overhang_length_mm",0.}};
        const bool keep_geometry=i==215 || i==283 || i==284 || i==358;
        for(const auto* region:layer.regions()) {
            const size_t slot=region->region().extruder(frExternalPerimeter)-1;
            REQUIRE(slot<4);
            std::function<void(const ExtrusionEntity&)> visit;
            visit=[&](const ExtrusionEntity& entity) {
                if(const auto* collection=dynamic_cast<const ExtrusionEntityCollection*>(&entity)){for(const auto* child:collection->entities)visit(*child);return;}
                if(const auto* loop=dynamic_cast<const ExtrusionLoop*>(&entity)){for(const auto& path:loop->paths)visit(path);return;}
                if(const auto* multi=dynamic_cast<const ExtrusionMultiPath*>(&entity)){for(const auto& path:multi->paths)visit(path);return;}
                const auto* path=dynamic_cast<const ExtrusionPath*>(&entity);
                if(!path)throw std::runtime_error("Unsupported native extrusion entity.");
                if(path->is_force_no_extrusion())return;
                if(path->role()!=erExternalPerimeter && path->role()!=erOverhangPerimeter)return;
                if(path->z_contoured)throw std::runtime_error("A two-dimensional projection cannot validate a contoured path.");
                if(!std::isfinite(path->width) || path->width<=0.)throw std::runtime_error("Invalid generated path width.");
                const Polylines lines{path->polyline.to_polyline()};
                // A full generated width is a conservative geometric vicinity,
                // not proof that this path reaches the three-dimensional surface.
                const auto band=diff_ex(shape,offset_ex(shape,-float(scale_(path->width))));
                const auto boundary_lines=intersection_pl(lines,band);
                const double whole_length=length(lines),near_length=length(boundary_lines);
                const auto outside_coarse=diff_pl(boundary_lines,coarse.at(slot)),outside_fine=diff_pl(boundary_lines,fine.at(slot));
                const auto uncovered=diff_pl(boundary_lines,all_fine);
                const bool external=path->role()==erExternalPerimeter;
                if(external) {
                    row["external_length_mm"]=row["external_length_mm"].get<double>()+whole_length;
                    row["near_exterior_external_length_mm"]=row["near_exterior_external_length_mm"].get<double>()+near_length;
                    row["near_exterior_outside_physical_projection_mm"]=row["near_exterior_outside_physical_projection_mm"].get<double>()+length(outside_coarse);
                    row["near_exterior_outside_logical_slot_mm"]=row["near_exterior_outside_logical_slot_mm"].get<double>()+length(outside_fine);
                    row["near_exterior_without_any_logical_target_mm"]=row["near_exterior_without_any_logical_target_mm"].get<double>()+length(uncovered);
                    total_external_length+=whole_length;
                } else row["overhang_length_mm"]=row["overhang_length_mm"].get<double>()+whole_length;
                if(keep_geometry)row["paths"].push_back({{"role",external?"external_perimeter":"overhang_perimeter"},
                    {"region_id",region->region().print_region_id()},{"configured_external_wall_filament_1based",slot+1},
                    {"width_mm",path->width},{"height_mm",path->height},{"polyline",coordinates(lines)},
                    {"near_exterior",coordinates(boundary_lines)},{"outside_physical_projection",coordinates(outside_coarse)},
                    {"outside_logical_slot",coordinates(outside_fine)},{"without_any_logical_target",coordinates(uncovered)}});
                ++path_count;
            };
            visit(region->perimeters);
        }
        evidence["layers"].push_back(std::move(row));
    }
    evidence["path_count"]=path_count;evidence["external_length_mm"]=total_external_length;
    {std::ofstream file(root/"wall-paths.json");file<<evidence.dump(2);REQUIRE(bool(file));}
    REQUIRE(path_count>0);REQUIRE(total_external_length>0.);
    CHECK(GUI::OrcaPrintColorLayers::identity(*model.objects.front(),config)==identity);
    if(const char* generate=std::getenv("ORCA_TOOLPATH_EXPORT_GCODE");generate && std::string(generate)=="1") {
        print.process();print.export_gcode((root/"native-n12.gcode").string(),nullptr,nullptr);
        REQUIRE(std::filesystem::file_size(root/"native-n12.gcode")>0);
        CHECK(GUI::OrcaPrintColorLayers::identity(*model.objects.front(),config)==identity);
    }
}
