#include "libslic3r/MeshRepair.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <ctime>
#include <iostream>
int main() {
    indexed_triangle_set source;
    source.vertices = {{0.f,0.f,0.f},{1.f,0.f,0.f},{0.f,1.f,0.f},{0.f,0.f,1.f}};
    source.indices = {{0,2,1},{0,1,3},{0,3,2},{1,2,3}};
    bool all = true;
    for (const bool duplicate : {false, true}) {
        auto input = source;
        if (duplicate) input.indices.push_back(input.indices.front());
        Slic3r::TriangleMesh enhanced(input);
        Slic3r::MeshRepairStats stats; std::string error;
        nlohmann::json progress = nlohmann::json::array();
        const auto started = std::chrono::steady_clock::now(); const auto cpu = std::clock();
        const bool repaired = Slic3r::repair_mesh(enhanced, &stats, &error, [&](int n){progress.push_back(n);});
        const auto input_id = Slic3r::mesh_repair_geometry_id(input);
        const auto output_id = Slic3r::mesh_repair_geometry_id(enhanced.its);
        const bool result = repaired && enhanced.its.indices.size() == 4 &&
            input_id.size() == 64 && output_id.size() == 64 && (!duplicate || stats.modified);
        all &= result;
        std::cout << nlohmann::json{{"case",duplicate?"duplicate_tetra":"closed_tetra"},
            {"repairEntry","repair_mesh"},{"workflow","desktop-menu"},{"completion","geometry_only_stage"},{"repairSucceeded",repaired},
            {"strictAiChainPerformed",false},{"strictVerified",false},
            {"selfIntersectionCertificate","not_run"},{"modified",stats.modified},
            {"inputFaces",input.indices.size()},{"outputFaces",enhanced.its.indices.size()},
            {"sourceGeometryId",input_id},{"outputGeometryId",output_id},
            {"wallMs",std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count()},
            {"cpuMs",1000.0*(std::clock()-cpu)/CLOCKS_PER_SEC},{"progress",progress},
            {"error",error},{"passed",result}}.dump() << '\n';
    }
    return all ? 0 : 1;
}
