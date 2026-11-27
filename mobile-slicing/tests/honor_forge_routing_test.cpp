#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/HonorForgeToolRouting.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <functional>

namespace fs=std::filesystem;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void expected_failure(const std::function<void()>& operation,const std::string& code)
{
    try{operation();}catch(const std::exception& error){
        require(std::string(error.what()).find(code)!=std::string::npos,error.what());return;
    }
    throw std::runtime_error("Expected failure was not raised: "+code);
}
int main(int argc,char** argv)
{
    try{
        require(argc==3,"Usage: routing-test <real-sparse-gcode> <new-evidence-directory>");
        const fs::path source=fs::u8path(argv[1]),output=fs::u8path(argv[2]);
        require(!fs::exists(output),"Evidence directory must be new");fs::create_directories(output);
        Slic3r::GCodeProcessor processor;processor.process_file(source.u8string());
        std::set<unsigned> ids;
        for(const auto& usage:processor.get_result().print_statistics.total_volumes_per_extruder)
            if(usage.second>1e-9)ids.insert(usage.first);
        require(ids==std::set<unsigned>({0,1}),"Reopened physical T4/T5 must retain logical material usage0/1");
        for(const auto& move:processor.get_result().moves)
            if(move.type==Slic3r::EMoveType::Extrude)
                require(move.extruder_id<2,"Preview must retain logical material identity");
        std::ifstream stream(source,std::ios::binary);std::ostringstream buffer;buffer<<stream.rdbuf();
        const std::string original=buffer.str();
        auto malformed=[&](const std::string& name,const std::string& from,const std::string& to,const std::string& error){
            auto text=original;const auto at=text.find(from);require(at!=std::string::npos,"Expected actual native token absent");
            text.replace(at,from.size(),to);const auto path=output/(name+".gcode");
            {std::ofstream write(path,std::ios::binary);write<<text;}
            expected_failure([&]{Slic3r::GCodeProcessor parsed;parsed.process_file(path.u8string());},error);
        };
        malformed("unmapped-physical-tool","\nT4\n","\nT3\n","HONOR_PHYSICAL_TOOL_UNMAPPED");
        malformed("duplicate-tool-map","; filament_map = 5,6","; filament_map = 5,5","HONOR_TOOL_MAPPING_AMBIGUOUS");
        malformed("unmapped-temperature-tool","M104 S220 T4","M104 S220 T3","HONOR_PHYSICAL_TOOL_UNMAPPED");
        expected_failure([]{Slic3r::HonorForgeToolRouting::physical({5,5},0);},"HONOR_TOOL_MAPPING_AMBIGUOUS");
        Slic3r::FullPrintConfig generic;
        require(!Slic3r::HonorForgeToolRouting::enabled(generic),"Generic printer must not enable Honor routing");
        std::ofstream(output/"result.json")<<"{\"state\":\"passed\",\"reopenedLogicalMaterials\":[0,1],\"negativeCases\":4,\"genericAdapterEnabled\":false}\n";
        std::cout<<"PASS: native file re-open, logical preview identity, strict unknown/duplicate tool and temperature routing\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
