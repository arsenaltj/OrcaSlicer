#include "slic3r/GUI/AI/Model/SurfacePartition.hpp"
#include "slic3r/GUI/AI/Model/BeautySurfaceShapeLock.hpp"
#include "slic3r/GUI/AI/ModelGeneration/PortraitSurfaceOwnershipV2.hpp"
#include "slic3r/GUI/AI/Model/LocalSemanticGeometry.hpp"
#include "slic3r/GUI/AI/Model/ParentSurfaceVisibility.hpp"
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/nowide/args.hpp>
#include <iostream>
#include <tbb/parallel_for.h>
#include <tbb/task_arena.h>

int main(int argc, char** argv) {
    boost::nowide::args utf8(argc, argv);
    try {
        if(argc==4 && std::string(argv[1])=="--rebuild") {
            if(boost::filesystem::exists(argv[3])) throw std::invalid_argument("Rebuilt output already exists.");
            boost::filesystem::ifstream input(argv[2],std::ios::binary);
            if(!input) throw std::invalid_argument("Missing contour draft.");
            nlohmann::json old;input>>old;
            const auto repaired=Slic3r::AI::SurfacePartition::rebuild_tessellation(old);
            boost::filesystem::ofstream output(argv[3],std::ios::binary);output<<repaired.dump();output.close();
            if(!output) throw std::runtime_error("Cannot write rebuilt contour draft.");
            std::cout<<"rebuilt_faces="<<repaired.at("faces").size()<<" added_triangles="<<repaired.at("added_triangles")<<'\n';
            return 0;
        }
        if (argc==4 && std::string(argv[1])=="--parent-cut-costs") {
            namespace Partition=Slic3r::AI::SurfacePartition;
            if (boost::filesystem::exists(argv[3])) throw std::invalid_argument("Cut cost output already exists.");
            boost::filesystem::ifstream source(argv[2],std::ios::binary);
            if (!source) throw std::invalid_argument("Missing cut cost input.");
            nlohmann::json request;source>>request;
            if (request.at("schema")!="orca.surface-partition-request/v1" || request.at("repair_scope")!="parent")
                throw std::invalid_argument("Invalid parent cost request.");
            auto identity=request.at("identity");identity.erase("boundary_policy_sha256");
            Partition::validate(request.at("incremental_baseline"),identity);
            nlohmann::json costs=nlohmann::json::array();
            for (const auto& face:request.at("faces")) if (!face.at("layers").empty()) {
                try { costs.push_back(Partition::incremental_cut_cost(face,request.at("identity"))); }
                catch (const std::exception& error) {
                    costs.push_back({{"source_face_id",face.at("source_face_id")},
                                     {"status","INVALID_LOCAL_CLIP"},{"reason",error.what()}});
                }
            }
            boost::filesystem::ofstream destination(argv[3],std::ios::binary);
            destination<<costs.dump();destination.close();
            if (!destination) throw std::runtime_error("Could not write parent cut costs.");
            return 0;
        }
        if (argc==4 && (std::string(argv[1])=="--parent-visibility" ||
                       std::string(argv[1])=="--parent-cell-visibility")) {
            const bool cells=std::string(argv[1])=="--parent-cell-visibility";
            namespace Geometry=Slic3r::GUI::LocalSemanticGeometry;
            const auto read=[](const boost::filesystem::path& path) {
                boost::filesystem::ifstream stream(path,std::ios::binary);
                if (!stream) throw std::invalid_argument("Missing visibility input.");
                return std::string(std::istreambuf_iterator<char>(stream),{});
            };
            if (boost::filesystem::exists(argv[3])) throw std::invalid_argument("Visibility output already exists.");
            const auto request_bytes=read(argv[2]); const auto request=nlohmann::json::parse(request_bytes);
            if (request.at("schema")!=(cells ? "orca.parent-cell-visibility-request/v1" : "orca.parent-visibility-request/v1") || request.at("cameras").empty() ||
                request.at("cameras").size()>16)
                throw std::invalid_argument("Invalid visibility request.");
            const auto packet_bytes=read(request.at("geometry_path").get<std::string>());
            if (Slic3r::AI::beauty_leaf_digest(packet_bytes)!=request.at("geometry_packet_sha256"))
                throw std::invalid_argument("Visibility packet drift.");
            Geometry::Packet packet; std::string error;
            if (!Geometry::decode(packet_bytes,request.at("source_sha256"),packet,error) ||
                packet.geometry_id!=request.at("geometry_id") || packet.mesh.indices.size()!=request.at("face_count"))
                throw std::invalid_argument("Visibility source geometry drift.");
            const Slic3r::AI::ParentSurfaceVisibility mesh(packet.mesh);
            const auto count=cells ? request.at("cells").size() : packet.mesh.indices.size();
            const size_t views=request.at("cameras").size();
            std::vector<size_t> source_faces;
            std::vector<std::array<std::array<double,3>,7>> cell_samples;
            if (cells) {
                if (!count || count>100000) throw std::invalid_argument("Invalid cell visibility count.");
                for (const auto& cell:request.at("cells")) {
                    const auto face=cell.at("source_face_id").get<size_t>();
                    if (face>=packet.mesh.indices.size()) throw std::invalid_argument("Cell visibility source face out of range.");
                    const auto samples=cell.at("samples").get<std::array<std::array<double,3>,7>>();
                    for (const auto& weights:samples) {
                        double sum=0.;
                        for (double value:weights) {
                            if (!std::isfinite(value) || value<0. || value>1.)
                                throw std::invalid_argument("Invalid cell visibility barycentric sample.");
                            sum+=value;
                        }
                        if (std::abs(sum-1.)>1e-8) throw std::invalid_argument("Unbound cell visibility sample.");
                    }
                    source_faces.push_back(face);cell_samples.push_back(samples);
                }
            }
            std::string output=cells ? "ORCAPC01" : "ORCAPV01";
            Geometry::detail::put(output,count,8); Geometry::detail::put(output,views,8);
            output+=packet.source_sha256;output+=packet.geometry_id;output+=Slic3r::AI::beauty_leaf_digest(request_bytes);
            const size_t header=output.size();output.resize(header+views*count,char(0));
            double radius=0.;
            for (const auto& vertex:packet.mesh.vertices) radius=std::max(radius,double(vertex.norm()));
            const double distance=radius*4.+1.;
            tbb::task_arena arena(2);
            for (size_t view=0;view<views;++view) {
                const auto& camera=request.at("cameras")[view];
                const auto vector=camera.at("basis")[2].get<std::array<double,3>>();
                const Slic3r::Vec3d direction(vector[0],vector[1],vector[2]);
                if (!direction.allFinite() || std::abs(direction.norm()-1.)>1e-6)
                    throw std::invalid_argument("Invalid visibility camera.");
                arena.execute([&] {tbb::parallel_for(tbb::blocked_range<size_t>(0,count,2048),[&](const auto& range) {
                    for (size_t face=range.begin();face!=range.end();++face)
                        output[header+view*count+face]=char(cells ?
                            Slic3r::AI::parent_visible_samples(mesh,source_faces[face],direction,distance,cell_samples[face]) :
                            Slic3r::AI::parent_visible_samples(mesh,face,direction,distance));
                });});
                std::cout<<"visibility view="<<view<<" faces="<<count<<std::endl;
            }
            output+=Geometry::detail::digest(output.data(),output.size(),nullptr);
            boost::filesystem::ofstream stream(argv[3],std::ios::binary);stream.write(output.data(),output.size());stream.close();
            if (!stream) throw std::runtime_error("Could not write visibility output.");
            return 0;
        }
        if (argc == 6 && std::string(argv[1]) == "--validate-ownership-v2") {
            const auto read = [](const boost::filesystem::path& path) {
                boost::filesystem::ifstream stream(path,std::ios::binary);
                if (!stream) throw std::invalid_argument("Missing ownership validation input.");
                nlohmann::json value; stream >> value; return value;
            };
            const auto ownership=read(argv[2]),partition=read(argv[3]),locks=read(argv[4]);
            Slic3r::GUI::PortraitSurfaceCellOwnership::decode(ownership,partition,locks,argv[5]);
            std::cout << "v2_identity_cells_frozen_parents=PASS\n";
            return 0;
        }
        if (argc == 5 && std::string(argv[1]) == "--validate-v3") {
            const auto read = [](const boost::filesystem::path& path) {
                boost::filesystem::ifstream stream(path,std::ios::binary);
                if (!stream) throw std::invalid_argument("Missing validation input.");
                return std::string(std::istreambuf_iterator<char>(stream),{});
            };
            const auto request=nlohmann::json::parse(read(argv[2]));
            const auto lock=nlohmann::json::parse(read(argv[3]));
            const auto& ref=lock.at("partition_ref");
            if (!Slic3r::AI::BeautySurfaceShapeLock::safe_partition_path(ref.at("path"),ref.at("sha256")))
                throw std::invalid_argument("Unsafe partition reference.");
            const auto bytes=read(boost::filesystem::path(argv[4])/ref.at("path").get<std::string>());
            Slic3r::AI::BeautySurfaceShapeLock::decode(lock,nlohmann::json::parse(bytes),request.at("identity"),
                                                    Slic3r::AI::beauty_leaf_digest(bytes));
            std::cout << "v3_identity_coverage_nested=PASS\n";
            return 0;
        }
        if (argc != 3) throw std::invalid_argument("Usage: surface_partition_tool input.json NEW-output.json");
        const boost::filesystem::path input(argv[1]), output(argv[2]);
        if (boost::filesystem::exists(output)) throw std::invalid_argument("Output already exists.");
        boost::filesystem::ifstream stream(input, std::ios::binary);
        if (!stream) throw std::invalid_argument("Missing clipping input.");
        nlohmann::json request; stream >> request;
        auto result = Slic3r::AI::SurfacePartition::build(request);
        Slic3r::AI::SurfacePartition::validate(result, request.at("identity"));
        boost::filesystem::ofstream destination(output, std::ios::binary);
        destination << result.dump(); destination.close();
        if (!destination) throw std::runtime_error("Could not write offline partition.");
        std::cout << "faces=" << result.at("faces").size() << " added_triangles=" << result.at("added_triangles") << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
