#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/HonorForgeToolRouting.hpp"
#include "orca_mobile_slicing_sdk/material_variant_layout.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <map>
#include <set>
#include <functional>
#include <cmath>
#include <algorithm>
#include <cctype>
#include <vector>
#include <tbb/global_control.h>

using namespace Slic3r;
namespace fs = std::filesystem;
static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static void rejects(const std::function<void()>& f, const char* expected)
{
    try { f(); } catch (const std::exception& error) {
        require(std::string(error.what()).find(expected) != std::string::npos, error.what()); return;
    }
    throw std::runtime_error("Expected mapping rejection");
}
static DynamicPrintConfig config()
{
    auto c = DynamicPrintConfig::full_print_config();
    c.set_key_value("nozzle_diameter", new ConfigOptionFloats(std::vector<double>(6, .4)));
    auto* types = dynamic_cast<ConfigOptionEnumsGeneric*>(c.option("extruder_type"));
    auto* volumes = dynamic_cast<ConfigOptionEnumsGeneric*>(c.option("nozzle_volume_type"));
    require(types && volumes, "Native machine variants");
    types->values.assign(6, etDirectDrive); volumes->values.assign(6, nvtStandard);
    for (size_t i = 0; i < 4; ++i) {
        auto native = DynamicPrintConfig::full_print_config();
        native.set_key_value("nozzle_temperature", new ConfigOptionInts({i == 2 ? 255 : 220}));
        native.set_key_value("nozzle_temperature_initial_layer", new ConfigOptionInts({i == 2 ? 255 : 220}));
        orca::mobile::slicing::detail::flatten_authorized_material(c, native, "Direct Drive Standard", i, 4);
    }
    c.set_deserialize_strict({{"printer_model", "Honor Forge"}, {"gcode_flavor", "klipper"},
        {"single_extruder_multi_material", "0"}, {"filament_map", "5,6,4,3"},
        {"filament_colour", "#FF0000;#0000FF;#FFFFFF;#00FF00"}, {"filament_map_mode", "Manual"},
        {"machine_end_gcode", ""}, {"before_layer_change_gcode", "G92 E0"}, {"layer_change_gcode", ""},
        {"filament_start_gcode", "\"\";\"\";\"\";\"\""}, {"filament_end_gcode", "\"\";\"\";\"\";\"\""},
        {"enable_prime_tower", "0"}, {"enable_support", "0"}, {"support_type", "normal(auto)"},
        {"support_filament", "3"}, {"support_interface_filament", "3"}, {"support_on_build_plate_only", "0"},
        {"ooze_prevention", "0"}, {"standby_temperature_delta", "-5"}, {"layer_height", "0.2"},
        {"initial_layer_print_height", "0.2"}, {"skirt_loops", "0"}, {"brim_type", "no_brim"},
        {"brim_width", "0"}, {"timelapse_type", "0"}, {"extruder_clearance_radius", "1"},
        {"gcode_comments", "1"}, {"use_relative_e_distances", "1"}});
    c.set_key_value("machine_start_gcode", new ConfigOptionString(HonorForgeToolRouting::mapped_start_template()));
    c.set_key_value("filament_volume_map", new ConfigOptionInts({0, 0, 0, 0}));
    c.set_key_value("idle_temperature", new ConfigOptionInts({0, 177, 188, 199}));
    c.set_key_value("flush_volumes_matrix", new ConfigOptionFloats(std::vector<double>(4*4*6, 0.)));
    c.set_key_value("flush_multiplier", new ConfigOptionFloats(std::vector<double>(6, 1.)));
    c.set_key_value("flush_volumes_vector", new ConfigOptionFloats(std::vector<double>(8, 0.)));
    return c;
}
struct Export { std::string text; GCodeProcessorResult processed; std::vector<unsigned> ordered; };
static Export export_case(const fs::path& output, const DynamicPrintConfig& c, bool support=false, bool single=false)
{
    Model model;
    for (unsigned id = 0; id < (single ? 1u : 2u); ++id) {
        auto* object = model.add_object(); object->name = "thermal-" + std::to_string(id);
        object->add_volume(make_cube(4., 4., 2.))->config.set("extruder", int(id + 1));
        object->add_instance()->set_offset(Vec3d(20. + id * 40., 20., support && id == 1 ? 4. : 0.));
    }
    Print print; print.set_status_silent(); print.apply(model, c);
    auto validation = print.validate(); if (!validation.string.empty()) throw std::runtime_error(validation.string);
    print.process();
    Export result;
    result.ordered = print.tool_ordering().all_extruders();
    const auto original_start = print.config().machine_start_gcode.value;
    const auto original_map = print.config().filament_map.values;
    print.export_gcode(output.u8string(), &result.processed, nullptr);
    require(print.config().machine_start_gcode.value == original_start, "Exporter changed machine template");
    require(print.config().filament_map.values == original_map, "Exporter changed routing map");
    std::ifstream stream(output, std::ios::binary); std::ostringstream data; data << stream.rdbuf(); result.text = data.str();
    require(!result.text.empty(), "Actual G-code export must exist");
    return result;
}
struct Trace {
    struct Heat { size_t line; int tool; int target; bool wait; };
    std::vector<Heat> heat;
    std::map<int, size_t> first_extrusion;
    std::map<int, int> target_at_first;
    std::map<int, int> wait_at_first;
    size_t startup = 0;
    int first_tool = -1;
};
static Trace trace(const std::string& gcode)
{
    Trace result; std::map<int,int> targets, waits; std::map<int,double> e;
    int tool = -1; bool relative = false; size_t line = 0; std::istringstream input(gcode); std::string text;
    while (std::getline(input, text)) {
        ++line; text = text.substr(0, text.find(';')); std::istringstream tokens(text); std::string command; tokens >> command;
        if (command.empty()) continue;
        if (command == "START_PRINT") {
            result.startup = line;
            const auto at = text.find("INITIAL_TOOL="); require(at != std::string::npos, "Startup initial tool");
            tool = std::stoi(text.substr(at + 13));
            continue;
        }
        if (command[0] == 'T' && command.size() > 1 && std::all_of(command.begin()+1, command.end(), [](unsigned char c){return std::isdigit(c)!=0;})) { tool = std::stoi(command.substr(1)); continue; }
        if (command == "M83") { relative = true; continue; }
        if (command == "M82") { relative = false; continue; }
        std::map<char,double> p; std::string token;
        while (tokens >> token) if (token.size()>1 && std::isalpha(static_cast<unsigned char>(token[0]))) {
            try { p[token[0]] = std::stod(token.substr(1)); } catch (const std::exception&) {}
        }
        if (command == "M104" || command == "M109") {
            const int heater = p.count('T') ? int(p['T']) : tool;
            if (p.count('S')) {
                targets[heater] = int(p['S']);
                if (command == "M109") waits[heater] = int(p['S']);
                result.heat.push_back({line, heater, int(p['S']), command == "M109"});
            }
        }
        if (command == "G92" && p.count('E')) e[tool] = p['E'];
        if ((command == "G1" || command == "G2" || command == "G3") && p.count('E')) {
            const double delta = relative ? p['E'] : p['E'] - e[tool]; e[tool] = p['E'];
            if (delta > 1e-7 && !result.first_extrusion.count(tool)) {
                result.first_extrusion[tool] = line; result.target_at_first[tool] = targets[tool]; result.wait_at_first[tool] = waits[tool];
                if (result.first_tool < 0) result.first_tool = tool;
            }
        }
    }
    return result;
}
static void initial_heat(const Export& data, const std::map<int,int>& expected)
{
    const auto t = trace(data.text); std::set<int> heated;
    for (const auto& h : t.heat) if (h.wait && h.line > t.startup && h.line < t.first_extrusion.at(t.first_tool)) heated.insert(h.tool);
    std::set<int> want;
    for (const auto& item : expected) {
        want.insert(item.first);
        require(t.first_extrusion.count(item.first) != 0, "Expected actual physical tool extrusion");
        require(t.target_at_first.at(item.first) == item.second, "First extrusion needs its physical heater target");
        require(t.wait_at_first.at(item.first) == item.second, "First extrusion needs its heater wait");
    }
    require(heated == want, "Startup must heat exactly participating physical heads");
    require(t.first_tool == 4, "Initial model tool must stay T4");
    for (const auto& item : expected) {
        const auto logical = HonorForgeToolRouting::logical({5,6,4,3}, unsigned(item.first));
        require(data.processed.print_statistics.total_volumes_per_extruder.at(logical) > 0, "Processor must invert physical heater/tool ids to logical usage");
    }
}
int main(int argc, char** argv)
{
    try {
        tbb::global_control one_worker(tbb::global_control::max_allowed_parallelism, 1);
        require(argc == 2, "thermal-test <NEW evidence directory>");
        const auto out = fs::u8path(argv[1]); require(!fs::exists(out), "New evidence only"); fs::create_directories(out);
        auto c = config();
        initial_heat(export_case(out/"constant-two.gcode", c), {{4,220},{5,220}});
        auto support = c; support.set_deserialize_strict("enable_support", "1");
        initial_heat(export_case(out/"model-support.gcode", support, true), {{4,220},{5,220},{3,255}});
        auto sequential = c; sequential.set_deserialize_strict("print_sequence", "by object");
        initial_heat(export_case(out/"sequential.gcode", sequential), {{4,220},{5,220}});
        auto changed = c; changed.option<ConfigOptionInts>("nozzle_temperature_initial_layer")->values = {210,210,245,200};
        auto transition = export_case(out/"transition.gcode", changed); initial_heat(transition, {{4,210},{5,210}});
        const auto tt = trace(transition.text); bool transition4=false, transition5=false;
        for (const auto& h : tt.heat) if (!h.wait && h.target==220) { transition4 |= h.tool==4; transition5 |= h.tool==5; }
        require(transition4 && transition5, "Normal-layer temperatures must still change through the same mapping");
        auto ooze = c; ooze.set_deserialize_strict("ooze_prevention", "1");
        auto cooling = trace(export_case(out/"ooze.gcode", ooze).text); bool standby=false;
        for (const auto& h : cooling.heat) standby |= h.wait && h.tool==5 && h.target==177;
        require(standby, "Noninitial head must preserve configured idle target");
        require(cooling.target_at_first.at(5)==220 && cooling.wait_at_first.at(5)==220, "Existing ooze post-toolchange must restore actual print temperature and wait");
        auto custom = c; custom.set_key_value("filament_start_gcode", new ConfigOptionStrings({"M104 S211 T4 ;USER_THERMAL", "", "", ""}));
        auto user = trace(export_case(out/"user-filament.gcode", custom).text);
        require(user.target_at_first.at(4)==211, "Filament user temperature must run after automatic startup, without overwrite");
        auto single = trace(export_case(out/"single.gcode", c, false, true).text);
        require(single.heat.empty(), "Mapped single-head startup remains unchanged");
        auto desktop = c; desktop.set_key_value("machine_start_gcode", new ConfigOptionString(HonorForgeToolRouting::source_start_template()));
        require(trace(export_case(out/"desktop-source.gcode", desktop).text).heat.empty(), "Ordinary desktop source template remains unchanged");
        auto generic = c; generic.set_deserialize_strict("printer_model", "Generic");
        require(trace(export_case(out/"generic.gcode", generic).text).heat.empty(), "Non-Honor Klipper remains unchanged");
        auto custom_start = c; custom_start.set_key_value("machine_start_gcode", new ConfigOptionString(HonorForgeToolRouting::mapped_start_template()+"M104 S219 T4\n"));
        auto untouched = trace(export_case(out/"custom-start.gcode", custom_start).text);
        require(untouched.heat.size()==1 && !untouched.heat.front().wait && untouched.heat.front().target==219, "Custom machine heater commands must not be replaced");
        auto semm = c; semm.set_deserialize_strict("single_extruder_multi_material", "1");
        require(!HonorForgeToolRouting::enabled(semm), "SEMM cannot enter SDK independent-head initialization");
        rejects([]{HonorForgeToolRouting::physical({},0);}, "HONOR_TOOL_MAPPING_AMBIGUOUS");
        rejects([]{HonorForgeToolRouting::physical({5,5},0);}, "HONOR_TOOL_MAPPING_AMBIGUOUS");
        rejects([]{HonorForgeToolRouting::physical({5},1);}, "HONOR_LOGICAL_MATERIAL_UNMAPPED");
        std::ofstream(out/"result.json") << "{\"state\":\"development_tests_passed\",\"actualCoreExports\":10,\"physicalPrinterVerified\":false}\n";
        std::cout << "PASS actual Core startup/temperature/export boundaries; no hardware qualification\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
