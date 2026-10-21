#pragma once
// Read-only OPC/XML feature inspection. Never extracts/rewrites the source.
#include <expat.h>
#include <miniz.h>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <stdexcept>
#include <string_view>
#include <cctype>
namespace orca::slicing::detail {
struct ThreeMfScan {
    Json result{{"schemaVersion",1},{"features",Json::array()},{"blockedFeatures",Json::array()},{"plates",Json::array()},
        {"buildItems",Json::array()},{"sourcePreserved",true},{"currentPlate",nullptr}};
    std::set<std::string> seen,archivePaths;
    std::map<std::string,std::string> namespaces;
    std::map<std::string,size_t> instanceCounts;
    static std::string lower(std::string value) {std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});return value;}
    std::string file,error,optionKey,optionValue,settingsFile;XML_Parser parser=nullptr;
    Json sourceConfig=Json::object();std::vector<std::pair<std::string,Json>> embeddedProfiles;
    bool model=false,settings=false,ranges=false,inPlate=false,inMember=false,paint=false,support=false,seam=false,fuzzy=false;
    Json plate,member;
    void feature(const std::string& id,const std::string& status,const std::string& message) {
        if(!seen.insert(id+":"+file).second) return;
        Json item{{"id",id},{"status",status},{"path",file},{"message",message},
            {"blocksAllOutputModes",status=="blocked"},{"requiresExplicitSingleMaterial",status=="requires_single_material"}};
        result["features"].push_back(item);
        if(status=="blocked") result["blockedFeatures"].push_back(item);
    }
    static bool ends_with(const std::string& value,const char* suffix) {const size_t n=std::strlen(suffix);return value.size()>=n && value.compare(value.size()-n,n,suffix)==0;}
    static std::string local(const char* name) {const char* p=std::strrchr(name,'|');return p?p+1:name;}
    static const char* attr(const XML_Char** at,const char* key) {
        for(size_t i=0;at[i];i+=2) {const char* p=std::strrchr(at[i],'|');if(std::strcmp(p?p+1:at[i],key)==0) return at[i+1];}return "";
    }
    static void ns(void* data,const XML_Char* prefix,const XML_Char* uri) {
        auto& s=*static_cast<ThreeMfScan*>(data);s.namespaces[prefix?prefix:""]=uri?uri:"";
    }
    static void start(void* data,const XML_Char* raw,const XML_Char** at) {
        auto& s=*static_cast<ThreeMfScan*>(data);
        try {s.on_start(raw,at);} catch(const std::exception& e) {s.error=e.what();XML_StopParser(s.parser,XML_FALSE);}
    }
    static void end(void* data,const XML_Char* raw) {
        auto& s=*static_cast<ThreeMfScan*>(data);
        try {
            const auto name=local(raw);
            if(s.ranges && name=="option") {s.config(Json{{s.optionKey,s.optionValue}});s.optionKey.clear();s.optionValue.clear();}
            if(s.settings && name=="model_instance") {s.plate["members"].push_back(s.member);s.inMember=false;}
            if(s.settings && name=="plate") {s.result["plates"].push_back(s.plate);s.inPlate=false;}
        } catch(const std::exception& e) {s.error=e.what();XML_StopParser(s.parser,XML_FALSE);}
    }
    static void text(void* data,const XML_Char* value,int count) {
        auto& s=*static_cast<ThreeMfScan*>(data);
        try {if(!s.optionKey.empty()) s.optionValue.append(value,size_t(count));}
        catch(const std::exception& e) {s.error=e.what();XML_StopParser(s.parser,XML_FALSE);}
    }
    static void doctype(void* data,const XML_Char*,const XML_Char*,const XML_Char*,int) {
        auto& s=*static_cast<ThreeMfScan*>(data);s.error="3MF_DTD_UNSUPPORTED";XML_StopParser(s.parser,XML_FALSE);
    }
    void on_start(const char* raw,const XML_Char** at) {
        const auto name=local(raw);
        const char* separator=std::strrchr(raw,'|');
        if(model && separator) {
            const std::string_view uri(raw,size_t(separator-raw));
            if(uri!="http://schemas.microsoft.com/3dmanufacturing/core/2015/02" &&
               uri!="http://schemas.microsoft.com/3dmanufacturing/production/2015/06" &&
               uri!="http://schemas.microsoft.com/3dmanufacturing/material/2015/02")
                feature("model_namespace:"+std::string(uri),"blocked","Unknown model extension namespace");
        }
        if(model) for(size_t i=0;at[i];i+=2) {
            const char* bar=std::strrchr(at[i],'|');if(!bar) continue;
            const std::string_view uri(at[i],size_t(bar-at[i]));
            if(uri!="http://schemas.microsoft.com/3dmanufacturing/production/2015/06" && uri!="http://www.w3.org/XML/1998/namespace")
                feature("attribute_namespace:"+std::string(uri),"blocked","Unknown attribute namespace may change model semantics");
        }
        if(ranges) {
            if(name=="option") {optionKey=attr(at,"opt_key");optionValue.clear();}
            else if(name!="objects"&&name!="object"&&name!="range") feature("layer_range_element:"+name,"blocked","Unsupported layer-range content");
        }
        if(model && (name=="vertex" || name=="triangle")) {
            if(name=="triangle") for(size_t i=0;at[i];i+=2) {
                const auto key=local(at[i]);
                if(key=="paint_color" && *at[i+1]) paint=true;
                else if(key=="paint_supports" && *at[i+1]) support=true;
                else if(key=="paint_seam" && *at[i+1]) seam=true;
                else if(key=="paint_fuzzy_skin" && *at[i+1]) fuzzy=true;
                else if(key=="pid" || key=="p1" || key=="p2" || key=="p3") feature("triangle_material_properties","blocked","Per-triangle standard material properties are not consumed by the native importer");
                else if(key!="v1" && key!="v2" && key!="v3" && key!="face_property") feature("unknown_triangle_attribute:"+key,"blocked","Unrecognized triangle semantics");
            }
            return;
        }
        if(model) {
            if(name=="model") {
                feature("mesh_geometry","supported_native","Native mesh/component/transform loader; source retained");
                std::istringstream extensions(attr(at,"requiredextensions"));std::string prefix;
                while(extensions>>prefix) {
                    const auto uri=namespaces[prefix];
                    if(uri=="http://schemas.microsoft.com/3dmanufacturing/production/2015/06") feature("production_components","supported_native","Production components use the native archive importer");
                    else feature("required_extension:"+prefix,"blocked","Required extension is not supported: "+uri);
                }
            } else if(name=="item") {
                const std::string object=attr(at,"objectid");const size_t instance=instanceCounts[object]++;
                result["buildItems"].push_back(Json{{"sourceObjectId",object},{"sourceInstanceId",std::to_string(instance)}});
            } else if(name=="component") {
                const std::string reference=attr(at,"path");
                if(!reference.empty()) {
                    auto target=std::filesystem::u8path(reference);
                    if(!target.is_absolute()) target=std::filesystem::u8path(file).parent_path()/target;
                    std::string normalized=target.lexically_normal().generic_u8string();if(!normalized.empty()&&normalized.front()=='/') normalized.erase(0,1);
                    if(reference.find(':')!=std::string::npos || normalized.rfind("..",0)==0 || !archivePaths.count(lower(normalized)))
                        feature("external_component:"+reference,"blocked","Component geometry is not available inside the unchanged source archive");
                }
            } else if(name=="object" && *attr(at,"type") && std::string(attr(at,"type"))!="model") feature("object_type:"+std::string(attr(at,"type")),"blocked","Unsupported object semantics cannot be treated as ordinary model mesh");
            else if(name=="metadata" || name=="resources" || name=="object" || name=="mesh" || name=="vertices" ||
                      name=="triangles" || name=="components" || name=="build") { }
            else if(name=="colorgroup" || name=="color") feature("object_color_group","supported_native","Native importer maps object color groups to filament slots");
            else feature("model_element:"+name,"blocked","Unknown/unsupported model element; geometry semantics cannot be discarded");
        } else if(settings) {
            if(name=="plate") {inPlate=true;plate=Json{{"id",nullptr},{"members",Json::array()},{"directives",Json::object()}};}
            else if(name=="model_instance" && inPlate) {inMember=true;member=Json::object();}
            else if(name=="metadata") {
                const std::string key=attr(at,"key"),value=attr(at,"value");
                if(inMember) {
                    if(key=="object_id") member["sourceObjectId"]=value;
                    else if(key=="instance_id") member["sourceInstanceId"]=value;
                    else if(key=="identify_id") member["identifyId"]=value;
                } else if(inPlate) {
                    if(key=="plater_id") plate["id"]=value;
                    else if(key=="plater_name") plate["name"]=value;
                    else plate["directives"][key]=value;
                } else if((key=="post_process" || key=="plugins" || key=="slicing_pipeline_plugin" || key.find("plugin_config_overrides")!=std::string::npos) && active(Json(value))) feature("external_processing:"+key,"blocked","Object or part requires an unavailable external processor");
                else if(key=="name" || key=="matrix" || key=="extruder" || key.rfind("source_",0)==0 || key=="volume_type" || key=="part_type") { }
                else if(!Slic3r::print_config_def.get(key)) feature("object_setting:"+key,"blocked","Unrecognized object/part setting");
            } else if(name=="part" || name=="volume") {
                const std::string subtype=attr(at,"subtype");
                const std::set<std::string> types{"normal_part","negative_part","modifier_part","support_enforcer","support_blocker"};
                if(!subtype.empty() && !types.count(subtype)) feature("part_type:"+subtype,"blocked","Unknown part type must not fall back to a solid model part");
                else feature("parts_modifiers","supported_native","Part types, transforms and configuration are kept in the native Model");
            } else if(name=="assemble" || name=="assemble_item") feature("assembly_edit_pose","preserved_metadata","Assembly edit pose retained in source; print build transforms are authoritative");
            else if(name!="config" && name!="object" && name!="mesh_stat") feature("settings_element:"+name,"blocked","Unknown model-setting semantics");
        }
    }
    static size_t chunk(void* data,mz_uint64,const void* buffer,size_t bytes) {
        auto& s=*static_cast<ThreeMfScan*>(data);
        return XML_Parse(s.parser,static_cast<const char*>(buffer),int(bytes),XML_FALSE)==XML_STATUS_OK?bytes:0;
    }
    void xml(mz_zip_archive& zip,mz_uint index,const std::string& path) {
        file=path;const std::string kind=lower(path);model=kind.size()>=6&&kind.substr(kind.size()-6)==".model";settings=kind=="metadata/model_settings.config";ranges=kind=="metadata/layer_config_ranges.xml";
        if(settings) settingsFile=path;
        namespaces.clear();instanceCounts.clear();paint=support=seam=fuzzy=false;error.clear();inPlate=inMember=false;
        parser=XML_ParserCreateNS(nullptr,'|');if(!parser) throw std::bad_alloc();
        XML_SetUserData(parser,this);XML_SetElementHandler(parser,start,end);XML_SetNamespaceDeclHandler(parser,ns,nullptr);XML_SetStartDoctypeDeclHandler(parser,doctype);
        if(ranges) XML_SetCharacterDataHandler(parser,text);
        const bool ok=mz_zip_reader_extract_to_callback(&zip,index,chunk,this,0) && XML_Parse(parser,"",0,XML_TRUE)==XML_STATUS_OK;
        std::string why=error;
        if(!ok&&why.empty()) {const char* reason=XML_ErrorString(XML_GetErrorCode(parser));why=reason?reason:"3MF_XML_PARSE_FAILED";}
        XML_ParserFree(parser);parser=nullptr;
        if(!ok) feature("xml_parse_failure","blocked",why);
        if(paint) feature("paint_color","requires_single_material","Original painted scene preserved; output needs an explicit supported material assignment");
        if(support) feature("paint_supports","supported_native","Native support annotations retained");
        if(seam) feature("paint_seam","supported_native","Native seam annotations retained");
        if(fuzzy) feature("paint_fuzzy_skin","supported_native","Native fuzzy-skin annotations retained");
    }
    static bool active(const Json& value) {
        if(value.is_array()) {for(const auto& v:value) if(active(v)) return true;return false;}
        if(value.is_null()) return false;
        if(value.is_string()) {
            const auto s=value.get<std::string>();return s.find_first_not_of(" \t\r\n")!=std::string::npos && s!="nil" && s!="{}" && s!="[]";
        }
        return true;
    }
    void config(const Json& values) {
        static const std::set<std::string> metadata{"version","name","type","from","setting_id","filament_id","description","instantiation","inherits","renamed_from","url"};
        for(auto it=values.begin();it!=values.end();++it) {
            if(it.key()=="silent_mode" && (it.value()=="0" || it.value()=="false" || it.value()==false || it.value()==0)) {
                feature("retired_disabled_silent_mode","preserved_metadata","Native explicitly retires this disabled legacy flag; original value is retained");continue;
            }
            if(it.key()=="inherits" && active(it.value())) feature("unresolved_profile_inheritance","blocked","External preset inheritance is not resolved by the mobile slicing contract");
            if(metadata.count(it.key())) continue;
            if(it.key()=="filament_colour" && it.value().is_array() && it.value().size()>1) feature("source_palette","supported_native","Source palette is preserved; its size alone is not a geometry/output blocker");
            if(it.key()=="enable_prime_tower" && (it.value()==true || it.value()==1 || it.value()=="1")) feature("prime_tower","requires_single_material","Prime-tower output is unsupported; only an explicit user command may disable it");
            if(it.key()=="post_process" || it.key()=="plugins" || it.key()=="slicing_pipeline_plugin" || it.key().find("plugin_config_overrides")!=std::string::npos) {
                if(active(it.value())) feature("external_processing:"+it.key(),"blocked","Android does not execute the required external script/Python plugin");
                continue;
            }
            std::string key=it.key(),value=it.value().is_string()?it.value().get<std::string>():it.value().dump();
            Slic3r::PrintConfigDef::handle_legacy(key,value);
            const Slic3r::ConfigOptionDef* definition=Slic3r::print_config_def.get(key);
            if(!definition) for(const auto& option:Slic3r::print_config_def.options)
                if(std::find(option.second.aliases.begin(),option.second.aliases.end(),key)!=option.second.aliases.end()) {definition=&option.second;break;}
            if(!definition && active(it.value())) feature("unknown_config:"+it.key(),"blocked","Unknown/deleted source option would otherwise be ignored");
        }
    }
    Json finish() {
        file=settingsFile.empty()?"Metadata/model_settings.config":settingsFile;
        if(result["plates"].size()>1) feature("multiple_plates","blocked","Multiple plates are retained in source; P1 has no selected-plate slicing contract");
        if(result["plates"].size()==1) {
            if(!result["plates"][0]["id"].is_string() || result["plates"][0]["id"].get<std::string>().empty()) feature("invalid_plate_identity","blocked","Current plate identity is missing");
            result["currentPlate"]=result["plates"][0]["id"];
            const std::set<std::string> supportedDirectives{"locked","bed_type","print_sequence","first_layer_print_sequence","other_layers_print_sequence","other_layers_print_sequence_nums","spiral_mode","spiral_vase_mode","filament_map_mode","thumbnail_file","thumbnail_no_light_file","top_file","pick_file"};
            for(auto it=result["plates"][0]["directives"].begin();it!=result["plates"][0]["directives"].end();++it)
                if(!supportedDirectives.count(it.key()) && active(it.value())) feature("plate_directive:"+it.key(),"blocked","Unsupported per-plate directive must not be dropped");
            std::set<std::string> owned;
            for(const auto& member:result["plates"][0]["members"])
                owned.insert(member.value("sourceObjectId",std::string())+":"+member.value("sourceInstanceId",std::string()));
            for(const auto& item:result["buildItems"]) if(!owned.count(item["sourceObjectId"].get<std::string>()+":"+item["sourceInstanceId"].get<std::string>()))
                feature("unassigned_build_items","blocked","Build item has no unambiguous membership in the single current plate");
        } else if(result["plates"].empty()) {
            result["currentPlate"]="implicit-single";
            result["plates"].push_back(Json{{"id","implicit-single"},{"members",result["buildItems"]},{"directives",Json::object()}});
        }
        for(const auto& profile:embeddedProfiles) {
            file=profile.first;config(profile.second);
            for(auto option=profile.second.begin();option!=profile.second.end();++option)
                if(Slic3r::print_config_def.get(option.key()) && !sourceConfig.contains(option.key()))
                    feature("unresolved_embedded_option:"+option.key(),"blocked","Embedded profile option is not present in the expanded project settings");
            feature("embedded_profile","preserved_metadata","Profile retained in source; explicit project settings remain authoritative");
        }
        result["plateCount"]=result["plates"].size();return result;
    }
};
inline Json inspect_archive(const std::string& path) {
    mz_zip_archive zip{};
    if(!mz_zip_reader_init_file(&zip,path.c_str(),0)) throw std::runtime_error("3MF_ARCHIVE_READ_FAILED");
    struct Close{mz_zip_archive& z;~Close(){mz_zip_reader_end(&z);}} close{zip};ThreeMfScan scan;
    const auto count=mz_zip_reader_get_num_files(&zip);
    // Validate archive ENTRY names before parsing or handing the ZIP to the aux
    // loader. Production component p:path="/3D/..." references are not entries.
    for(mz_uint i=0;i<count;++i) {
        mz_zip_archive_file_stat stat{};if(!mz_zip_reader_file_stat(&zip,i,&stat)) throw std::runtime_error("3MF_ARCHIVE_INDEX_FAILED");
        std::string entry=stat.m_filename;std::replace(entry.begin(),entry.end(),'\\','/');
        bool unsafe=entry.empty() || entry.front()=='/' || (entry.size()>1 && std::isalpha(static_cast<unsigned char>(entry[0])) && entry[1]==':');
        std::istringstream parts(entry);std::string part;while(std::getline(parts,part,'/')) unsafe=unsafe||part=="..";
        if(unsafe) throw std::runtime_error("3MF_UNSAFE_ARCHIVE_ENTRY:"+std::string(stat.m_filename));
        scan.archivePaths.insert(ThreeMfScan::lower(entry));
    }
    for(mz_uint i=0;i<count;++i) {
        mz_zip_archive_file_stat stat{};if(!mz_zip_reader_file_stat(&zip,i,&stat)) throw std::runtime_error("3MF_ARCHIVE_INDEX_FAILED");
        const std::string name=stat.m_filename,kind=ThreeMfScan::lower(name);scan.file=name;
        if((kind.size()>=6&&kind.substr(kind.size()-6)==".model") || kind=="metadata/model_settings.config") scan.xml(zip,i,name);
        else if(kind=="metadata/project_settings.config" || kind=="metadata/filament_sequence.json" || kind.rfind("metadata/filament_settings_",0)==0 || kind.rfind("metadata/machine_settings_",0)==0 || kind.rfind("metadata/process_settings_",0)==0) {
            if(stat.m_uncomp_size>8*1024*1024) {scan.feature("settings_size","blocked","Source configuration exceeds inspection budget");continue;}
            size_t bytes=0;void* buffer=mz_zip_reader_extract_to_heap(&zip,i,&bytes,0);
            if(!buffer) throw std::runtime_error("3MF_SETTINGS_READ_FAILED");
            try {const Json parsed=Json::parse(static_cast<const char*>(buffer),static_cast<const char*>(buffer)+bytes);
                if(kind=="metadata/project_settings.config") {scan.sourceConfig=parsed;scan.config(parsed);}
                else if(kind=="metadata/filament_sequence.json") {
                    bool custom=false;
                    if(!parsed.is_object()) scan.feature("filament_sequence_format","blocked","Unrecognized filament-sequence data");
                    else for(const auto& plate:parsed) {
                        if(!plate.is_object()) {scan.feature("filament_sequence_format","blocked","Unrecognized plate sequence");continue;}
                        for(auto option=plate.begin();option!=plate.end();++option) {
                            if((option.key()!="sequence" && option.key()!="filament_sequence" && option.key()!="nozzle_sequence" && option.key()!="optimal_assignment") || !option.value().is_array())
                                scan.feature("filament_sequence_directive:"+option.key(),"blocked","Unrecognized sequence directive");
                            else custom=custom||!option.value().empty();
                        }
                    }
                    scan.feature(custom?"custom_filament_sequence":"empty_filament_sequence",custom?"blocked":"preserved_metadata",
                        custom?"Custom switching/assignment order is not replayed by the P1 contract":"No custom sequence is enabled; source metadata is retained");
                } else scan.embeddedProfiles.emplace_back(name,parsed);
            }
            catch(...) {mz_free(buffer);throw;}mz_free(buffer);
        } else if(kind=="metadata/layer_config_ranges.xml") {scan.xml(zip,i,name);scan.feature("variable_layers","supported_native","Native object layer profiles/ranges are retained");}
        else if(kind=="metadata/brim_ear_points.txt") scan.feature("brim_ears","supported_native","Native object brim-ear annotations are retained");
        else if(kind=="metadata/layer_heights_profile.txt") scan.feature("variable_layers","supported_native","Native object layer profiles/ranges are retained");
        else if(kind=="metadata/cut_information.xml") scan.feature("cut_metadata","preserved_metadata","Cut edit metadata retained in source; existing printable part geometry is used");
        else if(kind=="metadata/custom_gcode_per_layer.xml" || kind=="metadata/custom_gcode_per_print_z.xml") scan.feature("custom_layer_gcode","blocked","Per-plate custom G-code is not mapped to a selected mobile print plate");
        else if(kind.rfind("metadata/imagemap/",0)==0 || kind.find("sla_support")!=std::string::npos || kind.find("sla_drain")!=std::string::npos)
            scan.feature("unsupported_geometry_asset","blocked","Unsupported geometry-generation data cannot be dropped");
        else if(kind.size()>3 && (kind.substr(kind.size()-3)==".py" || kind.substr(kind.size()-3)==".sh")) scan.feature("embedded_external_program","blocked","Embedded script/plugin code is not executed by Android");
        else if(kind.rfind("metadata/",0)==0 && !ThreeMfScan::ends_with(kind,".png") && kind!="metadata/slice_info.config" &&
                !ThreeMfScan::ends_with(kind,".gcode") && !ThreeMfScan::ends_with(kind,".md5") &&
                kind.rfind("metadata/filament_settings_",0)!=0 && kind.rfind("metadata/machine_settings_",0)!=0 && kind.rfind("metadata/process_settings_",0)!=0)
            scan.feature("unknown_asset:"+name,"blocked","Unrecognized vendor/project payload is retained but cannot be assumed irrelevant to output");
    }
    return scan.finish();
}
} // namespace orca::slicing::detail
