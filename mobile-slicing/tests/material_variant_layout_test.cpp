#include "orca_mobile_slicing_sdk/material_variant_layout.hpp"
#include <functional>
#include <iostream>

using namespace Slic3r;
using namespace orca::mobile::slicing::detail;
static void require(bool x,const char* why){if(!x)throw std::runtime_error(why);}
static void rejects(const std::function<void()>& f,const char* expected)
{
    try{f();}catch(const std::runtime_error& e){
        require(std::string(e.what()).find(expected)!=std::string::npos,e.what());return;
    }
    throw std::runtime_error("Expected strict variant rejection");
}
int main()
{
    try{
        auto machine=DynamicPrintConfig::full_print_config();
        machine.set_key_value("nozzle_diameter",new ConfigOptionFloats(std::vector<double>(6,.4)));
        auto* types=dynamic_cast<ConfigOptionEnumsGeneric*>(machine.option("extruder_type"));
        auto* volumes=dynamic_cast<ConfigOptionEnumsGeneric*>(machine.option("nozzle_volume_type"));
        require(types && volumes,"registered machine variant types");
        types->values.assign(6,etDirectDrive);types->values[4]=etBowden;
        volumes->values.assign(6,nvtStandard);volumes->values[4]=nvtHighFlow;
        auto native=DynamicPrintConfig::full_print_config();
        native.set_key_value("filament_extruder_variant",new ConfigOptionStrings({"Direct Drive Standard","Bowden High Flow"}));
        native.set_key_value("nozzle_temperature",new ConfigOptionInts({220,251}));
        native.set_key_value("fan_min_speed",new ConfigOptionFloats({20,80}));
        native.set_key_value("filament_flow_ratio",new ConfigOptionFloats({.98,.97}));
        native.set_key_value("filament_diameter",new ConfigOptionFloats({1.75}));
        native.set_key_value("slow_down_layer_time",new ConfigOptionFloats({8}));
        auto forward=machine,reverse=machine;
        for(auto* output:{&forward,&reverse})
            output->set_key_value("filament_map",new ConfigOptionInts({6,5}));
        const auto first=authorized_tool_variant(machine,5),second=authorized_tool_variant(machine,4);
        require(second=="Bowden High Flow","physical tool selects non-first native variant");
        flatten_authorized_material(reverse,native,second,1,2);
        flatten_authorized_material(reverse,native,first,0,2);
        flatten_authorized_material(forward,native,first,0,2);
        flatten_authorized_material(forward,native,second,1,2);
        verify_authorized_materials(forward,reverse);
        require(forward.option<ConfigOptionInts>("filament_self_index")->values==std::vector<int>({1,2}),"logical IDs, not tools or preset group IDs");
        require(forward.option<ConfigOptionInts>("nozzle_temperature")->values==std::vector<int>({220,251}),"selected temperature rows");
        require(forward.option<ConfigOptionFloats>("filament_diameter")->values==std::vector<double>({1.75,1.75}),"nonvariant singleton broadcast");
        auto normalized=forward;auto keys=filament_options_with_variant;keys.insert("filament_self_index");
        normalized.update_values_to_printer_extruders_for_multiple_filaments(forward,6,6,keys,"filament_self_index","filament_extruder_variant");
        verify_authorized_materials(forward,normalized);
        auto drift=normalized;drift.option<ConfigOptionInts>("nozzle_temperature")->values[1]=220;
        rejects([&]{verify_authorized_materials(forward,drift);},"ACTUAL_FILAMENT_NORMALIZATION_CHANGED:nozzle_temperature");
        // Existing registered Wondermaker default is Standard; it does not
        // silently authorize another machine variant.
        auto standard=DynamicPrintConfig::full_print_config();
        require(authorized_material_variant(standard,"Direct Drive Standard")==0,"registered default remains supported");
        rejects([&]{authorized_material_variant(standard,"Bowden High Flow");},"UNSUPPORTED");
        auto bad=native;bad.option<ConfigOptionStrings>("filament_extruder_variant")->values.push_back("Unknown variant");
        rejects([&]{authorized_material_variant(bad,first);},"UNKNOWN");
        bad=native;bad.option<ConfigOptionStrings>("filament_extruder_variant")->values[1]=first;
        rejects([&]{authorized_material_variant(bad,first);},"AMBIGUOUS");
        bad=native;bad.option<ConfigOptionInts>("filament_self_index")->values={1,2};
        rejects([&]{authorized_material_variant(bad,first);},"LAYOUT_INVALID");
        bad=native;bad.option<ConfigOptionFloats>("fan_min_speed")->values={20,40,80};
        rejects([&]{flatten_authorized_material(reverse,bad,first,0,2);},"OPTION_LAYOUT_INVALID:fan_min_speed");
        bad=native;bad.option<ConfigOptionFloats>("filament_diameter")->values={1.75,2.85};
        rejects([&]{flatten_authorized_material(reverse,bad,first,0,2);},"OPTION_LAYOUT_INVALID:filament_diameter");
        rejects([&]{authorized_tool_variant(machine,6);},"DEVICE_VARIANT_LAYOUT_INVALID");
        auto badMachine=machine;dynamic_cast<ConfigOptionEnumsGeneric*>(badMachine.option("extruder_type"))->values={0,0};
        rejects([&]{authorized_tool_variant(badMachine,4);},"DEVICE_VARIANT_LAYOUT_INVALID");
        badMachine=machine;dynamic_cast<ConfigOptionEnumsGeneric*>(badMachine.option("nozzle_volume_type"))->values[4]=4;
        rejects([&]{authorized_tool_variant(badMachine,4);},"DEVICE_VARIANT_UNKNOWN");
        std::cout<<"PASS: trusted material variant selection, logical flattening, normalization guard and strict negatives\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
