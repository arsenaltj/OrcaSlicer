#include "libslic3r/GCode.hpp"
#include "libslic3r/HonorForgeToolRouting.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <functional>
#include <regex>

using namespace Slic3r;
namespace fs=std::filesystem;
struct TestGCode : GCode { using GCode::set_extruders; };
static void require(bool x,const char* why){if(!x)throw std::runtime_error(why);}
static PrintConfig config(const std::vector<int>& map,bool honor)
{
    PrintConfig c;
    c.printer_model.value=honor?"Honor Forge":"generic";
    c.machine_start_gcode.value=HonorForgeToolRouting::mapped_start_template();
    c.gcode_flavor.value=gcfKlipper;c.single_extruder_multi_material.value=false;
    c.filament_map.values=map;
    c.use_relative_e_distances.value=true;
    c.nozzle_diameter.values=std::vector<double>(6,0.4);
    c.fan_min_speed.values={20,75,35,45,55,65};c.fan_max_speed.values=c.fan_min_speed.values;
    c.slow_down_for_layer_cooling.values={true,true,true,true,true,true};
    c.slow_down_layer_time.values={6,18,9,10,11,12};
    c.slow_down_min_speed.values={2,4,3,3,3,3};
    c.close_fan_the_first_x_layers.values={0};c.full_fan_speed_layer.values={0};
    c.initial_layer_fan_speed.values={-1};c.reduce_fan_stop_start_freq.values={true};
    c.overhang_fan_speed.values={40,90,60,65,70,80};
    c.support_material_interface_fan_speed.values={30,85,95,65,70,80};
    c.overhang_fan_threshold.values={0};
    c.filament_diameter.values={1.75,2.85,1.9,2,2.1,2.2};
    return c;
}
// Derived comparison view only: the emitted bytes are saved before analysis.
static std::string view(std::string s,const std::vector<int>& map)
{
    std::istringstream in(s);std::ostringstream out;std::string line;
    while(std::getline(in,line)) {
        if(line.size()>1 && line[0]=='T')line="T"+std::to_string(HonorForgeToolRouting::logical(map,unsigned(std::stoul(line.substr(1)))));
        out<<line<<'\n';
    }
    return out.str();
}
static std::string cooling(const std::vector<int>& map,const std::vector<unsigned>& active,const fs::path& folder,bool honor=true)
{
    TestGCode gen;auto c=config(map,honor);gen.apply_print_config(c);gen.set_extruders(active);
    CoolingBuffer filter(gen);filter.set_current_extruder(active.front(),0);
    std::string result;
    // Multiple transitions and a flush boundary expose both parser ownership
    // and the persisted apply-stage state. Include real adjustable moves and
    // bridge/support-interface sections with unequal material settings.
    for(unsigned layer=0;layer<2;++layer) {
        std::string input;
        auto sequence=active;sequence.insert(sequence.end(),{active.front(),active.back(),active.front()});
        for(unsigned logical:sequence) {
            unsigned tool=honor?HonorForgeToolRouting::physical(map,logical):logical;
            input+="T"+std::to_string(tool)+"\nG1 F6000 ;_EXTRUDE_SET_SPEED\nG1 X10 Y10 E1\nG1 X20 Y10 E1\n;_EXTRUDE_END\n";
            input+=";_OVERHANG_FAN_START\nG1 X20 Y20 E1\n;_OVERHANG_FAN_END\n;_SUPP_INTERFACE_FAN_START\nG1 X10 Y20 E1\n;_SUPP_INTERFACE_FAN_END\n";
        }
        result+=filter.process_layer(std::move(input),layer,true);
    }
    std::ofstream(folder/"cooling.gcode",std::ios::binary)<<result;
    return honor?view(result,map):result;
}
static void rejected(const std::function<void()>& f,const char* code)
{
    try{f();}catch(const std::exception& e){require(std::string(e.what()).find(code)!=std::string::npos,e.what());return;}
    throw std::runtime_error(std::string("Expected rejection: ")+code);
}
#ifndef ORCA_COOLING_BASELINE
static std::string pressure(const std::vector<int>& map,const fs::path& folder,bool equal_diameters=false,bool honor=true,unsigned initial=0)
{
    auto c=config(map,honor);c.max_volumetric_extrusion_rate_slope.value=1.8;
    if(equal_diameters)c.filament_diameter.values[1]=c.filament_diameter.values[0];
    c.max_volumetric_extrusion_rate_slope_segment_length.value=0.5;
    c.extrusion_rate_smoothing_external_perimeter_only.value=false;
    PressureEqualizer pe(c);pe.set_tool_routing(c,{0,1},initial);
    std::string output;
    for(unsigned layer=0;layer<2;++layer) {
        std::string input=";_EXTRUSION_ROLE:"+std::to_string(int(ExtrusionRole::erExternalPerimeter))+"\n";
        for(unsigned logical:{initial,1u-initial,initial}) {
            input+="T"+std::to_string(honor?HonorForgeToolRouting::physical(map,logical):logical)+"\n";
            // At least five consecutive extrusions: the unchanged PE algorithm
            // needs a look-back span >=2 and does not iterate the terminal move.
            input+="G1 X0 Y0 F6000\nG1 F6000 ;_EXTRUDE_SET_SPEED\nG1 X30 Y0 E3\nG1 X30 Y30 E0.3\nG1 X0 Y30 E3\nG1 X0 Y0 E0.3\nG1 X30 Y0 E3\n;_EXTRUDE_END\n";
        }
        output+=pe.process_layer(LayerResult{input,layer}).gcode;
    }
    output+=pe.process_layer(LayerResult::make_nop_layer_result()).gcode;
    std::ofstream(folder/"pressure.gcode",std::ios::binary)<<output;
    const std::regex move("G1 [^\\n;]*\\bE-?(?:[0-9]|\\.[0-9])");
    require(std::distance(std::sregex_iterator(output.begin(),output.end(),move),std::sregex_iterator())>30,"PE must really split/adjust the unequal volumetric-rate moves");
    return honor?view(output,map):output;
}
#endif
int main(int argc,char** argv)
{
    try {
        require(argc==2,"cooling-test <NEW evidence directory>");const auto out=fs::u8path(argv[1]);require(!fs::exists(out),"new evidence required");fs::create_directories(out);
        auto run=[&](const char* name,const std::vector<int>& map,const std::vector<unsigned>& active,bool honor=true){auto path=out/name;fs::create_directory(path);return cooling(map,active,path,honor);};
        const auto identity=run("identity",{1,2},{0,1});
        bool slowed=false;const std::regex speed("G1 F([0-9.]+)");
        for(auto i=std::sregex_iterator(identity.begin(),identity.end(),speed);i!=std::sregex_iterator();++i)slowed|=std::stod((*i)[1])<5900;
        require(identity.find("M106 S51")!=std::string::npos && identity.find("M106 S191")!=std::string::npos && slowed,"Fixture must produce actual distinct fan and slowed feedrates");
        for(const auto pwm:{102,229,76,217})require(identity.find("M106 S"+std::to_string(pwm))!=std::string::npos,"Distinct bridge/support fan settings must actually execute");
        const auto sparse=run("sparse45",{5,6},{0,1});require(identity==sparse,"Sparse T4/T5 changed actual logical fan/feedrate behavior");
        require(identity==run("permutation10",{2,1},{0,1}),"In-range permutation changed actual cooling behavior");
        require(run("initial1-identity",{1,2},{1,0})==run("initial1-sparse",{5,6},{1,0}),"Already-logical cooling initial tool was reinterpreted");
        require(run("support-identity",{1,2,3},{0,2})==run("support3",{5,6,4},{0,2}),"Independent support T3 changed cooling behavior");
        require(run("six-identity",{1,2,3,4,5,6},{0,1,2,3,4,5})==run("six-generic",{1,2,3,4,5,6},{0,1,2,3,4,5},false),"Generic/identity semantics changed");
#ifndef ORCA_COOLING_BASELINE
        auto pe_run=[&](const char* name,const std::vector<int>& map){auto folder=out/name;fs::create_directory(folder);return pressure(map,folder);};
        const auto pe_identity=pe_run("pe-identity",{1,2});
        require(pe_identity==pe_run("pe-sparse45",{5,6}),"PE sparse tools changed actual logical flow-rate adjustment");
        require(pe_identity==pe_run("pe-permutation10",{2,1}),"PE in-range permutation changed actual logical flow-rate adjustment");
        fs::create_directory(out/"pe-equal-diameters");
        require(pe_identity!=pressure({1,2},out/"pe-equal-diameters",true),"PE oracle must really depend on material cross-section");
        fs::create_directory(out/"pe-generic");
        require(pe_identity==pressure({1,2},out/"pe-generic",false,false),"Generic valid PE behavior changed");
        fs::create_directory(out/"pe-initial1-identity");fs::create_directory(out/"pe-initial1-sparse");
        require(pressure({1,2},out/"pe-initial1-identity",false,true,1)==pressure({5,6},out/"pe-initial1-sparse",false,true,1),"Already-logical PE initial tool was reinterpreted");
        auto c=config({5,6,4},true);TestGCode gen;gen.apply_print_config(c);gen.set_extruders({0,1});
        auto bad=[&](const char* line){CoolingBuffer f(gen);f.set_current_extruder(0,0);f.process_layer(std::string(line),1,true);};
        rejected([&]{bad("T3\n");},"HONOR_LOGICAL_MATERIAL_INACTIVE");
        rejected([&]{bad("T2\n");},"HONOR_PHYSICAL_TOOL_UNMAPPED");
        rejected([&]{bad("T999999999999999999999999999\n");},"HONOR_PHYSICAL_TOOL_INVALID");
        rejected([&]{bad("T-1\n");},"HONOR_PHYSICAL_TOOL_INVALID");
        rejected([&]{bad("T4oops\n");},"HONOR_PHYSICAL_TOOL_INVALID");
        auto pe_bad=[&](const char* tool){auto pc=config({5,6,4},true);pc.max_volumetric_extrusion_rate_slope.value=0.1;PressureEqualizer pe(pc);pe.set_tool_routing(pc,{0,1},0);pe.process_layer(LayerResult{tool,0});};
        rejected([&]{pe_bad("T3\n");},"HONOR_LOGICAL_MATERIAL_INACTIVE");
        rejected([&]{pe_bad("T2\n");},"HONOR_PHYSICAL_TOOL_UNMAPPED");
        rejected([&]{pe_bad("T-1\n");},"HONOR_PHYSICAL_TOOL_INVALID");
        rejected([&]{pe_bad("T4294967296\n");},"HONOR_PHYSICAL_TOOL_INVALID");
        rejected([&]{pe_bad("T\n");},"HONOR_PHYSICAL_TOOL_INVALID");
        rejected([&]{pe_bad("T4oops\n");},"HONOR_PHYSICAL_TOOL_INVALID");
        TestGCode generic;generic.apply_print_config(config({1,2},false));generic.set_extruders({1});
        CoolingBuffer generic_filter(generic);generic_filter.set_current_extruder(1,0);
        const auto ignored=generic_filter.process_layer("T0\nT99999999999999999999999\nG1 F6000 ;_EXTRUDE_SET_SPEED\nG1 X10 Y10 E1\n;_EXTRUDE_END\n",1,true);
        require(ignored.find("M106 S191")!=std::string::npos && ignored.find("M106 S51")==std::string::npos,"Generic inactive/invalid tool must preserve current logical material");
#endif
        std::cout<<"PASS actual CoolingBuffer fan/feedrate metamorphic sparse/permutation/support/identity6 across flush and negative tools\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
