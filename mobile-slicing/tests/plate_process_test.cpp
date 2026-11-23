#include "libslic3r/Format/PlateProcess.hpp"
#include <iostream>
#include <stdexcept>
using namespace Slic3r;
int main(){try {
    const std::pair<const char*,const char*> valid[]={
        {"layer_height","0.16"},{"wall_loops","4"},{"sparse_infill_density","23%"},{"sparse_infill_pattern","gyroid"},
        {"outer_wall_line_width","110%"},{"outer_wall_speed","50,60"},{"enable_support","1"},{"support_type","normal(auto)"},
        {"support_threshold_angle","38"},{"support_on_build_plate_only","0"},{"wall_generator","arachne"}};
    DynamicPrintConfig config;
    for(const auto& entry:valid) {PlateProcess::apply(config,entry.first,entry.second);PlateProcess::apply(config,entry.first,entry.second);}
    if(config.keys().size()!=11)throw std::runtime_error("11 keys not consumed");
    const std::pair<const char*,const char*> invalid[]={
        {"unknown","1"},{"layer_height","nan"},{"layer_height","0.2junk"},{"layer_height","0.2%"},{"layer_height","-0.1"},
        {"wall_loops","1.2"},{"wall_loops","1001"},{"wall_loops","-1"},{"sparse_infill_density","101%"},
        {"sparse_infill_pattern","unknown"},{"outer_wall_line_width","10.1"},{"outer_wall_line_width","1001%"},
        {"outer_wall_speed","0"},{"outer_wall_speed","50,nan"},{"outer_wall_speed","50,"},{"outer_wall_speed","50,bad"},
        {"enable_support","true"},{"enable_support","2"},{"support_type","none"},{"support_threshold_angle","91"},
        {"support_on_build_plate_only","false"},{"wall_generator","unknown"},{"layer_height",""}};
    for(const auto& entry:invalid) {
        bool rejected=false;try{PlateProcess::parse(entry.first,entry.second);}catch(const std::exception&){rejected=true;}
        if(!rejected)throw std::runtime_error(std::string("accepted invalid ")+entry.first+":"+entry.second);
    }
    bool rejected=false;try{PlateProcess::parse("layer_height",std::string(1025,'1'));}catch(const std::exception&){rejected=true;}
    if(!rejected)throw std::runtime_error("oversized metadata accepted");
    rejected=false;try{PlateProcess::apply(config,"wall_loops","5");}catch(const std::exception&){rejected=true;}
    if(!rejected)throw std::runtime_error("conflicting duplicate accepted");
    for(const auto value:{"0","10","1000%"})PlateProcess::parse("outer_wall_line_width",value);
    PlateProcess::parse("outer_wall_speed","nil,50");
    for(const auto value:{"normal(auto)","tree(auto)","normal(manual)","tree(manual)"})PlateProcess::parse("support_type",value);
    // The definition permits a 9mm literal, but the real source 0.4mm
    // profile rejects it. Source import must run this Native semantic check
    // before a correct explicit override could erase the invalid source.
    auto physical=DynamicPrintConfig::full_print_config();
    physical.set_deserialize_strict("nozzle_diameter","0.4");
    physical.apply(PlateProcess::parse("outer_wall_line_width","9"));
    if(!physical.validate().count("outer_wall_line_width"))throw std::runtime_error("Native source physical width error missing");
    physical.apply(PlateProcess::parse("outer_wall_line_width","110%"));
    if(physical.validate().count("outer_wall_line_width"))throw std::runtime_error("Native legal relative source width rejected");
    std::cout<<"PASS 11 native keys, 23 invalid values, oversized and duplicate, exact enums/nullable vector/unit boundaries\n";
    return 0;
} catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
