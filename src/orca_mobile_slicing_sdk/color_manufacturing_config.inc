Json prepare_color_config(const LoadedProject& source,const Json& request)
{
    using namespace Slic3r;
    const auto presetId=request.at("devicePresetId").get<std::string>();
    const auto catalog=offline_presets();const auto& devices=catalog.at("devices");
    const auto device=std::find_if(devices.begin(),devices.end(),[&](const auto& x){return x.value("id",std::string())==presetId&&!x.value("restoreSource",false);});
    if(device==devices.end())throw std::runtime_error("COLOR_CONFIG_DEVICE_PRESET_UNKNOWN");
    LoadedProject staged;staged.config=source.config;staged.sourceConfig=source.sourceConfig;
    apply_preset(staged,"device",presetId);
    // Keep the authoritative project's native process, including support and
    // quality. Machine/nozzle/macro fields still come from the native device.
    staged.config.apply_only(source.config,Preset::print_options(),true);
    if(presetId=="honor/forge-6-head-0.4"){
        const auto sourceTemplate=device->at("config").at("machine_start_gcode").get<std::string>();
        if(sourceTemplate!=HonorForgeToolRouting::source_start_template())
            throw std::runtime_error("COLOR_CONFIG_HONOR_START_TEMPLATE_CHANGED");
        staged.config.set_key_value("machine_start_gcode",new ConfigOptionString(HonorForgeToolRouting::mapped_start_template()));
    }
    const auto& bindings=request.at("bindings");
    if(!bindings.is_array()||bindings.empty()||bindings.size()>32)throw std::runtime_error("COLOR_CONFIG_BINDINGS_REQUIRED");
    const auto* nozzles=staged.config.option<ConfigOptionFloats>("nozzle_diameter");
    if(!nozzles||nozzles->values.empty())throw std::runtime_error("COLOR_CONFIG_NOZZLES_REQUIRED");
    const size_t count=bindings.size();std::vector<int> maps(count);std::vector<std::string> colors(count),presets(count);
    const auto& materialKeys=Preset::filament_options();std::set<size_t> ids,slots,tools;
    Json resolvedMaterialPresets=Json::array();Json palette=Json::array();
    for(const auto& binding:bindings){
        const size_t id=binding.at("materialId").get<size_t>(),slot=binding.at("slot").get<size_t>(),tool=binding.at("tool").get<size_t>();
        const std::string materialId=binding.at("materialPresetId"),purpose=binding.at("purpose");
        if(id>=count||!ids.insert(id).second||slot>=32||tool>=nozzles->values.size()||!slots.insert(slot).second||!tools.insert(tool).second)
            throw std::runtime_error("COLOR_CONFIG_SLOT_TOOL_INVALID");
        if(purpose!="model"&&purpose!="support")throw std::runtime_error("COLOR_CONFIG_PURPOSE_INVALID");
        auto hex=binding.at("color").get<std::string>();if(hex.size()!=7||hex[0]!='#'||hex.find_first_not_of("0123456789abcdefABCDEF",1)!=std::string::npos)throw std::runtime_error("COLOR_CONFIG_COLOR_INVALID");
        std::transform(hex.begin(),hex.end(),hex.begin(),[](unsigned char c){return char(std::toupper(c));});
        const auto& available=catalog.at("materials");const auto material=std::find_if(available.begin(),available.end(),[&](const auto& x){return x.value("id",std::string())==materialId&&!x.value("restoreSource",false);});
        if(material==available.end())throw std::runtime_error("COLOR_CONFIG_MATERIAL_PRESET_UNKNOWN");
        if(binding.contains("materialPresetVersion") &&
           binding.at("materialPresetVersion")!=material->at("presetVersion"))
            throw std::runtime_error("COLOR_CONFIG_MATERIAL_PRESET_VERSION_CHANGED");
        resolvedMaterialPresets.push_back(Json{{"materialId",id},{"slot",slot},{"id",materialId},
            {"presetVersion",material->at("presetVersion")},{"family",material->at("family")}});
        DynamicPrintConfig native=DynamicPrintConfig::full_print_config();apply_config_overrides(native,material->at("config"));
        for(const auto& key:materialKeys){const auto* option=native.option(key);if(!option||!option->is_vector())continue;
            const auto* vector=dynamic_cast<const ConfigOptionVectorBase*>(option);if(!vector||vector->empty())continue;
            auto* target=dynamic_cast<ConfigOptionVectorBase*>(staged.config.option(key));if(!target)continue;
            target->resize(count,option);target->set_at(option,id,0);
        }
        maps[id]=int(tool+1);colors[id]=hex;presets[id]=materialId;
        const auto* types=native.option<ConfigOptionStrings>("filament_type");
        if(!types||types->values.empty())throw std::runtime_error("COLOR_CONFIG_MATERIAL_TYPE_REQUIRED");
        if(purpose=="model"){
            Json rgb=Json::array();for(size_t c=0;c<3;++c)rgb.push_back(double(std::stoul(hex.substr(1+c*2,2),nullptr,16))/255.);
            palette.push_back(Json{{"slot",slot},{"materialId",id},{"color",rgb},{"materialType",types->values[0]}});
        }
    }
    if(palette.empty()||palette.size()>6)throw std::runtime_error("COLOR_CONFIG_DIRECT_PALETTE_LIMIT");
    staged.config.set_key_value("filament_colour",new ConfigOptionStrings(colors));
    staged.config.set_key_value("filament_map",new ConfigOptionInts(maps));
    staged.config.set_key_value("filament_volume_map",new ConfigOptionInts(std::vector<int>(count,0)));
    staged.config.set_key_value("filament_map_mode",new ConfigOptionEnum<FilamentMapMode>(fmmManual));
    staged.config.set_key_value("filament_settings_id",new ConfigOptionStrings(presets));
    normalize_filament_flush_layout(
        staged.config.option<ConfigOptionFloats>("flush_volumes_matrix")->values,
        staged.config.option<ConfigOptionFloats>("flush_multiplier")->values,
        staged.config.option<ConfigOptionFloats>("flush_volumes_vector")->values,
        count,nozzles->values.size(),count);
    for(const auto& binding:bindings)if(binding.at("purpose")=="support"){
        const int slot=binding.at("materialId").get<int>()+1;
        staged.config.set_key_value("support_filament",new ConfigOptionInt(slot));
        staged.config.set_key_value("support_interface_filament",new ConfigOptionInt(slot));
    }
    // Use the same composite canonicalization as native 3MF import (not a
    // textual hash exception for a field that happens to differ today).
    staged.config.handle_legacy_composite();
    const auto config=config_json(staged.config);
    return Json{{"schemaVersion",2},{"devicePresetId",presetId},{"bindings",bindings},{"manufacturingConfig",config},
        {"resolvedMaterialPresets",resolvedMaterialPresets},
        {"toolRoutingAdapter",presetId=="honor/forge-6-head-0.4"?Json{{"id","honor-forge-physical-tools-v1"},{"startTemplateSourceJsonSha256",json_sha256(HonorForgeToolRouting::source_start_template())}}:Json()},
        {"processFingerprint",json_sha256(config)},{"materialFingerprint",json_sha256(Json{{"bindings",bindings},{"palette",palette}})},
        {"palette",palette},{"routingVerified",false},{"actualFilamentLoadingVerified",false},
        {"nativeMaterialConfigurationVerified",true},{"filamentConfirmationSource","operator"},
        {"sourceProjectRevision",source.metadata.at("revision")}};
}
