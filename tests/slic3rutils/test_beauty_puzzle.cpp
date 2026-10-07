#include <catch2/catch_all.hpp>
#include "slic3r/GUI/AI/Model/BeautyPuzzle.hpp"
#include "slic3r/GUI/AI/Model/BeautyDocument.hpp"
#include "slic3r/GUI/AI/Model/BeautyEyeDetail.hpp"
#include "slic3r/GUI/AI/Model/BeautyGuidance.hpp"
#include "slic3r/GUI/AI/Model/BeautyRecognition.hpp"
#include "slic3r/GUI/AI/Model/LocalSemanticGeometry.hpp"
#include "slic3r/GUI/AI/Model/BeautyMetadata.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "slic3r/GUI/AI/Model/ModelFinishing.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelPreviewPuzzle.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelGenerationPresentation.hpp"
#include <boost/filesystem/fstream.hpp>
#include <boost/nowide/cstdlib.hpp>
#include "libslic3r/FilamentMixer.hpp"
#include "slic3r/GUI/NativeMixedFilamentSuggestion.hpp"
#include <limits>
#include <iterator>
#include <numeric>
#include <cstring>
#include <chrono>
#include <atomic>
#include <thread>
#include <string_view>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
// RPC headers define this legacy macro; keep ordinary test identifiers intact.
#undef small
#endif

using namespace Slic3r;
using namespace Slic3r::AI;

TEST_CASE("Puzzle preview repaints base and painted colors after a source replacement", "[ModelPreviewPuzzle]") {
    indexed_triangle_set mesh;
    mesh.vertices={{0,0,0},{1,0,0},{0,1,0},{1,1,0}};
    mesh.indices={{0,1,2},{2,1,3}};
    std::vector<std::array<float,4>> base{{1,0,0,1},{0,1,0,1},{0,0,1,.8f},{.5f,.5f,.5f,1}};
    const auto surface=BeautySurface::build(mesh,base);
    BeautyPuzzle puzzle;puzzle.geometry_id=surface->geometry_id;puzzle.face_piece={1,2};puzzle.next_id=3;
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    Slic3r::GUI::ModelPreviewPuzzle preview;
    preview.update(mesh,base,*surface,puzzle,UINT32_MAX,true);
    REQUIRE(preview.fill);
    CHECK(preview.fill->get_geometry().vertices[2*8+6]==float(0x0000FF));
    puzzle.paint(1,{0,1,0,1});
    preview.update(mesh,base,*surface,puzzle,UINT32_MAX,true);
    CHECK(preview.fill->get_geometry().vertices[0*8+6]==float(0x00FF00));
    CHECK(preview.fill->get_geometry().vertices[3*8+6]==float(0x0000FF));
    auto replacement=base;replacement[2]={1,1,0,.6f};
    preview.update(mesh,replacement,*surface,puzzle,UINT32_MAX,true);
    CHECK(preview.fill->get_geometry().vertices[2*8+6]==float(0x00FF00));
    CHECK_THAT(preview.fill->get_geometry().vertices[2*8+7],Catch::Matchers::WithinAbs(.6f,1e-6f));
    CHECK(preview.fill->get_geometry().vertices[3*8+6]==float(0xFFFF00));
    preview.reset();
    preview.update(mesh,base,*surface,puzzle,UINT32_MAX,true);
    CHECK(preview.fill->get_geometry().vertices[3*8+6]==float(0x0000FF));
}

TEST_CASE("Puzzle preview preserves complete geometry across repaint and topology replacement", "[ModelPreviewPuzzle]") {
    indexed_triangle_set mesh;
    mesh.vertices={{0,0,0},{1,0,0},{0,1,0},{1,1,0}};
    mesh.indices={{0,1,2},{2,1,3}};
    std::vector<std::array<float,4>> base(mesh.vertices.size(),{1,0,0,.5f});
    auto surface=BeautySurface::build(mesh,base);
    BeautyPuzzle puzzle;puzzle.geometry_id=surface->geometry_id;puzzle.face_piece={1,2};puzzle.next_id=3;
    Slic3r::GUI::ModelPreviewPuzzle preview;
    preview.update(mesh,base,*surface,puzzle,UINT32_MAX,true);
    puzzle.paint(1,{0,1,0,1});
    preview.update(mesh,base,*surface,puzzle,UINT32_MAX,true);
    REQUIRE(preview.fill);
    CHECK(preview.fill->get_geometry().indices==std::vector<unsigned>{0,1,2,3,4,5});
    CHECK(preview.fill->get_geometry().vertices[6]==float(0x00FF00));
    CHECK_THAT(preview.fill->get_geometry().vertices[7],Catch::Matchers::WithinAbs(.5f,1e-6f));
    // Repartitioning preserves topology; replacing the mesh changes it.
    puzzle.face_piece={1,1};
    preview.update(mesh,base,*surface,puzzle,UINT32_MAX,true);
    CHECK(preview.fill->get_geometry().indices==std::vector<unsigned>{0,1,2,3,4,5});
    mesh.vertices[2]={0,2,0};mesh.indices={{2,0,1}};
    surface=BeautySurface::build(mesh,base);
    puzzle.geometry_id=surface->geometry_id;puzzle.face_piece={1};
    preview.update(mesh,base,*surface,puzzle,UINT32_MAX,true);
    REQUIRE(preview.fill);
    const auto& geometry=preview.fill->get_geometry();
    CHECK(geometry.indices==std::vector<unsigned>{0,1,2});
    REQUIRE(geometry.vertices.size()==24);
    for(size_t corner=0;corner<3;++corner) {
        const auto& position=mesh.vertices[mesh.indices[0][corner]];
        for(size_t axis=0;axis<3;++axis) {
            CHECK_THAT(geometry.vertices[corner*8+axis],Catch::Matchers::WithinAbs(position[axis],1e-6f));
            CHECK_THAT(geometry.vertices[corner*8+3+axis],Catch::Matchers::WithinAbs(float(surface->normals[0][axis]),1e-6f));
        }
        CHECK(geometry.vertices[corner*8+6]==float(0x00FF00));
        CHECK_THAT(geometry.vertices[corner*8+7],Catch::Matchers::WithinAbs(.5f,1e-6f));
    }
}

TEST_CASE("Puzzle preview republishes complete triangles when an attribute update is rejected", "[ModelPreviewPuzzle]") {
    indexed_triangle_set mesh;
    mesh.vertices={{0,0,0},{1,0,0},{0,1,0}};mesh.indices={{0,1,2}};
    std::vector<std::array<float,4>> base(3,{1,0,0,.7f});
    const auto surface=BeautySurface::build(mesh,base);
    BeautyPuzzle puzzle;puzzle.geometry_id=surface->geometry_id;puzzle.face_piece={1};puzzle.next_id=2;
    Slic3r::GUI::ModelPreviewPuzzle preview;
    preview.update(mesh,base,*surface,puzzle,UINT32_MAX,true);
    // Equal counts permit reuse, but a different vertex stride rejects the update.
    Slic3r::GUI::GLModel::Geometry incompatible;
    incompatible.format={Slic3r::GUI::GLModel::Geometry::EPrimitiveType::Triangles,
                         Slic3r::GUI::GLModel::Geometry::EVertexLayout::P3};
    for(const auto& vertex:mesh.vertices)incompatible.add_vertex(vertex);
    incompatible.add_triangle(0,1,2);
    preview.fill=std::make_unique<Slic3r::GUI::GLModel>();
    preview.fill->init_from(std::move(incompatible));
    puzzle.paint(1,{0,0,1,1});
    preview.update(mesh,base,*surface,puzzle,UINT32_MAX,true);
    REQUIRE(preview.fill);
    const auto& geometry=preview.fill->get_geometry();
    CHECK(geometry.indices==std::vector<unsigned>{0,1,2});
    REQUIRE(geometry.vertices.size()==24);
    for(size_t corner=0;corner<3;++corner) {
        CHECK(geometry.vertices[corner*8+6]==float(0x0000FF));
        CHECK_THAT(geometry.vertices[corner*8+7],Catch::Matchers::WithinAbs(.7f,1e-6f));
        for(size_t axis=0;axis<3;++axis)
            CHECK_THAT(geometry.vertices[corner*8+axis],Catch::Matchers::WithinAbs(mesh.vertices[corner][axis],1e-6f));
    }
}

TEST_CASE("Background preview data preserves the display and subsequent edits", "[ModelPreviewPuzzle][BeautyWorkbench]") {
    const bool custom=GENERATE(false,true);
    const uint32_t selected=GENERATE(UINT32_MAX,1u,17u);
    indexed_triangle_set mesh;
    mesh.vertices={{0,0,0},{1,0,0},{0,1,0},{1,1,0}};mesh.indices={{0,1,2},{2,1,3}};
    std::vector<std::array<float,4>> base(4,{.2f,.4f,.6f,.7f});
    const auto surface=BeautySurface::build(mesh,base);
    BeautyPuzzle puzzle;puzzle.geometry_id=surface->geometry_id;puzzle.face_piece={1,2};puzzle.next_id=3;
    puzzle.paint(1,{1,0,0,1});
    std::vector<uint32_t> regions{17,17};const auto* partition=custom?&regions:nullptr;
    using Preview=Slic3r::GUI::ModelPreviewPuzzle;
    std::optional<Preview::Prepared> prepared;
    std::exception_ptr error;
    std::thread worker([&] {
        try {prepared=Preview::prepare_initial(mesh,base,*surface,puzzle,selected,partition);}
        catch(...) {error=std::current_exception();}
    });
    worker.join();REQUIRE_FALSE(error);REQUIRE(prepared);
    Preview reference,actual;
    reference.update(mesh,base,*surface,puzzle,selected,true,partition);
    REQUIRE(actual.install(*prepared,mesh,base,*surface,puzzle,selected,partition));
    CHECK_FALSE(actual.install(*prepared,mesh,base,*surface,puzzle,selected,partition));
    const auto compare=[](const Slic3r::GUI::GLModel* a,const Slic3r::GUI::GLModel* b) {
        REQUIRE(bool(a)==bool(b));if(!a)return;
        const auto& a_bounds=a->get_bounding_box();const auto& b_bounds=b->get_bounding_box();
        CHECK(a_bounds.defined==b_bounds.defined);
        CHECK(std::memcmp(a_bounds.min.data(),b_bounds.min.data(),3*sizeof(double))==0);
        CHECK(std::memcmp(a_bounds.max.data(),b_bounds.max.data(),3*sizeof(double))==0);
        const auto& left=a->get_geometry();const auto& right=b->get_geometry();
        REQUIRE(left.vertices.size()==right.vertices.size());CHECK(left.indices==right.indices);
        // Rendering attributes must retain their original ordered float bits.
        if(!left.vertices.empty())CHECK(std::memcmp(left.vertices.data(),right.vertices.data(),left.vertices.size()*sizeof(float))==0);
    };
    compare(reference.fill.get(),actual.fill.get());compare(reference.borders.get(),actual.borders.get());
    compare(reference.active.get(),actual.active.get());
    REQUIRE(reference.cpu.contours.size()==actual.cpu.contours.size());
    for(size_t i=0;i<reference.cpu.contours.size();++i) {
        const auto& a=reference.cpu.contours[i];const auto& b=actual.cpu.contours[i];
        CHECK(a.face==b.face);CHECK(a.neighbor==b.neighbor);CHECK(a.left==b.left);CHECK(a.right==b.right);
        CHECK(std::memcmp(&a.tolerance,&b.tolerance,sizeof(float))==0);
    }
    puzzle.paint(1,{0,1,0,1});
    reference.update(mesh,base,*surface,puzzle,selected,true,partition);
    actual.update(mesh,base,*surface,puzzle,selected,true,partition);
    compare(reference.fill.get(),actual.fill.get());compare(reference.active.get(),actual.active.get());
    CHECK(actual.fill->get_geometry().vertices[6]==float(0x00FF00));
}

TEST_CASE("Obsolete or canceled preview preparation leaves the installed display intact", "[ModelPreviewPuzzle][BeautyWorkbench]") {
    indexed_triangle_set mesh;
    mesh.vertices={{0,0,0},{1,0,0},{0,1,0}};mesh.indices={{0,1,2}};
    std::vector<std::array<float,4>> base(3,{1,0,0,1});
    const auto surface=BeautySurface::build(mesh,base);
    BeautyPuzzle puzzle;puzzle.geometry_id=surface->geometry_id;puzzle.face_piece={1};puzzle.next_id=2;
    using Preview=Slic3r::GUI::ModelPreviewPuzzle;
    Preview actual;actual.update(mesh,base,*surface,puzzle,UINT32_MAX,true);
    auto* installed=actual.fill.get();const auto before=installed->get_geometry().vertices;
    auto prepared=Preview::prepare_initial(mesh,base,*surface,puzzle,UINT32_MAX);
    auto replacement=mesh;
    CHECK_FALSE(actual.install(prepared,replacement,base,*surface,puzzle,UINT32_MAX));
    auto colors=base;
    CHECK_FALSE(actual.install(prepared,mesh,colors,*surface,puzzle,UINT32_MAX));
    auto changed=puzzle;changed.paint(1,{0,0,1,1});
    CHECK_FALSE(actual.install(prepared,mesh,base,*surface,changed,UINT32_MAX));
    changed=puzzle;changed.face_piece={2};changed.next_id=3;
    CHECK_FALSE(actual.install(prepared,mesh,base,*surface,changed,UINT32_MAX));
    CHECK_FALSE(actual.install(prepared,mesh,base,*surface,puzzle,1));
    std::vector<uint32_t> regions{17};
    auto layered=Preview::prepare_initial(mesh,base,*surface,puzzle,UINT32_MAX,&regions);
    CHECK_FALSE(actual.install(layered,mesh,base,*surface,changed,UINT32_MAX,&regions));
    regions[0]=18;
    CHECK_FALSE(actual.install(layered,mesh,base,*surface,puzzle,UINT32_MAX,&regions));
    CHECK_FALSE(actual.install(layered,mesh,base,*surface,puzzle,UINT32_MAX));
    size_t checkpoints=0;
    std::exception_ptr error;
    std::thread canceled_worker([&] {
        try {Preview::prepare_initial(mesh,base,*surface,puzzle,UINT32_MAX,nullptr,
            [&]{return ++checkpoints>=4;});}
        catch(...) {error=std::current_exception();}
    });
    canceled_worker.join();REQUIRE(error);
    CHECK(checkpoints==4);CHECK(actual.fill.get()==installed);
    CHECK(actual.fill->get_geometry().vertices==before);
    CHECK(prepared.valid);
    REQUIRE(actual.install(prepared,mesh,base,*surface,puzzle,UINT32_MAX));
}

// Local performance fixture only: requires a user-supplied historical model
// and its matching saved draft, or explicit fresh regions; never runs by default.
TEST_CASE("Historical puzzle preview keeps its fill bytes across repaint runs", "[.ModelPreviewPuzzleProbe]") {
    const auto env=[](const char* key){const auto value=boost::nowide::getenv(key);return value?std::string(value):std::string{};};
    const auto source=env("ORCA_PREVIEW_SOURCE"),draft=env("ORCA_PREVIEW_DRAFT"),report=env("ORCA_PREVIEW_REPORT");
    const bool fresh=env("ORCA_PREVIEW_CREATE")=="1";
    if(source.empty() || (!fresh && draft.empty()) || report.empty())SKIP("Set model, matching draft or explicit fresh regions, and a fresh report path.");
    REQUIRE(boost::filesystem::is_regular_file(source));
    if(!fresh)REQUIRE(boost::filesystem::is_regular_file(draft));
    REQUIRE_FALSE(boost::filesystem::exists(report));
    const auto source_hash=model_artifact_sha256(source);
    TriangleMesh mesh;ObjInfo colors;std::string error;
    REQUIRE(load_model_artifact(source,mesh,colors,error));
    const size_t faces=mesh.its.indices.size();
    BeautyDocument document;
    nlohmann::json record;
    if(!fresh) {
        boost::filesystem::ifstream input(draft);nlohmann::json wrapper;input>>wrapper;
        record=wrapper.at("beauty_puzzle_draft");
        document=BeautyDocument::decode(record,record.at("geometry_id").get<std::string>(),faces);
    }
    const auto surface=BeautySurface::build(mesh.its,colors.vertex_colors,document.face_patch);
    auto puzzle=fresh?BeautyPuzzle::create_regions(*surface):BeautyPuzzle::decode(record.at("puzzle"),surface->geometry_id,faces);
    puzzle.validate(*surface);
    Slic3r::GUI::ModelPreviewPuzzle preview;
    std::atomic<size_t> cancellation_checks{0};
    std::function<bool()> keep_running;
    if(env("ORCA_PREVIEW_CHECK_CANCEL")=="1")keep_running=[&] {++cancellation_checks;return false;};
    double background_ms=0.,install_ms=0.;
    const auto initial_start=std::chrono::steady_clock::now();
    if(env("ORCA_PREVIEW_STAGED")=="1") {
        const auto started=std::chrono::steady_clock::now();
        std::optional<Slic3r::GUI::ModelPreviewPuzzle::Prepared> prepared;
        std::exception_ptr worker_error;
        std::thread worker([&] {
            try {prepared=Slic3r::GUI::ModelPreviewPuzzle::prepare_initial(mesh.its,colors.vertex_colors,*surface,puzzle,UINT32_MAX,nullptr,keep_running);}
            catch(...) {worker_error=std::current_exception();}
        });
        worker.join();REQUIRE_FALSE(worker_error);REQUIRE(prepared);
        background_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
        const auto install_start=std::chrono::steady_clock::now();
        REQUIRE(preview.install(*prepared,mesh.its,colors.vertex_colors,*surface,puzzle,UINT32_MAX));
        install_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-install_start).count();
    } else preview.update(mesh.its,colors.vertex_colors,*surface,puzzle,UINT32_MAX,true);
    const double initial_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-initial_start).count();
    // Complete initial CPU display output, beyond the fill-only repaint check.
    const auto hash_initial = [&] {
        uint64_t hash = 14695981039346656037ull;
        const auto bytes = [&](const void* data, size_t size) {
            for (size_t i = 0; i < size; ++i)
                hash = (hash ^ static_cast<const unsigned char*>(data)[i]) * 1099511628211ull;
        };
        for (const auto* model : {preview.fill.get(), preview.borders.get(), preview.active.get()}) {
            const bool present = model != nullptr;
            bytes(&present, sizeof(present));
            if (!present) continue;
            const auto& geometry = model->get_geometry();
            const size_t vertices = geometry.vertices.size(), indices = geometry.indices.size();
            bytes(&vertices, sizeof(vertices)); bytes(&indices, sizeof(indices));
            bytes(geometry.vertices.data(), vertices * sizeof(float));
            bytes(geometry.indices.data(), indices * sizeof(unsigned));
        }
        for (const auto& edge : preview.cpu.contours) {
            for (int axis = 0; axis < 3; ++axis) {
                bytes(&edge.a[axis], sizeof(float)); bytes(&edge.b[axis], sizeof(float));
            }
            bytes(&edge.face, sizeof(edge.face)); bytes(&edge.neighbor, sizeof(edge.neighbor));
            bytes(&edge.left, sizeof(edge.left)); bytes(&edge.right, sizeof(edge.right));
            bytes(&edge.tolerance, sizeof(edge.tolerance));
        }
        return hash;
    };
    const uint64_t initial_frame_hash = hash_initial();
    // Bounds must match the original ordered scan, including ribbon vertices.
    for(const auto* model:{preview.fill.get(),preview.borders.get(),preview.active.get()}) {
        if(!model)continue;
        const auto& geometry=model->get_geometry();
        BoundingBoxf3 expected;
        for(size_t i=0;i<geometry.vertices_count();++i)
            expected.merge(geometry.extract_position_3(i).cast<double>());
        const auto& actual=model->get_bounding_box();
        CHECK(actual.defined==expected.defined);
        CHECK(std::memcmp(actual.min.data(),expected.min.data(),3*sizeof(double))==0);
        CHECK(std::memcmp(actual.max.data(),expected.max.data(),3*sizeof(double))==0);
    }
    puzzle.paint(puzzle.face_piece.front(),{1.f,1.f,1.f,1.f});
    nlohmann::json samples=nlohmann::json::array();
    for(int iteration=0;iteration<6;++iteration) {
        const auto start=std::chrono::steady_clock::now();
        preview.update(mesh.its,colors.vertex_colors,*surface,puzzle,UINT32_MAX,true);
        samples.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
    }
    REQUIRE(preview.fill);
    const auto& vertices=preview.fill->get_geometry().vertices;
    REQUIRE(vertices.size()==faces*3*8);
    std::vector<float> reference(vertices.size());
    for(size_t f=0;f<faces;++f) {
        const auto& face=mesh.its.indices[f];
        const auto painted=puzzle.colors.find(puzzle.face_piece[f]);
        const auto normal=surface->normals[f].cast<float>();
        for(int corner=0;corner<3;++corner) {
            const auto base_color=size_t(face[corner])<colors.vertex_colors.size()?colors.vertex_colors[face[corner]]:
                std::array<float,4>{.7f,.7f,.7f,1};
            auto shown=painted!=puzzle.colors.end()?painted->second:base_color;shown[3]=base_color[3];
            const uint32_t rgb=(uint32_t(std::lround(shown[0]*255))<<16) |
                (uint32_t(std::lround(shown[1]*255))<<8) | uint32_t(std::lround(shown[2]*255));
            const auto& position=mesh.its.vertices[face[corner]];
            float* vertex=reference.data()+(f*3+corner)*8;
            vertex[0]=position.x();vertex[1]=position.y();vertex[2]=position.z();
            vertex[3]=normal.x();vertex[4]=normal.y();vertex[5]=normal.z();
            vertex[6]=float(rgb);vertex[7]=shown[3];
        }
    }
    CHECK(std::memcmp(reference.data(),vertices.data(),vertices.size()*sizeof(float))==0);
    const auto& indices=preview.fill->get_geometry().indices;
    REQUIRE(indices.size()==faces*3);
    bool linear_indices=true;
    for(size_t i=0;i<indices.size();++i)if(indices[i]!=i){linear_indices=false;break;}
    CHECK(linear_indices);
    uint64_t digest=14695981039346656037ull;
    for(const unsigned char byte:std::string_view(reinterpret_cast<const char*>(vertices.data()),vertices.size()*sizeof(float)))
        digest=(digest^byte)*1099511628211ull;
    boost::filesystem::ofstream output(report);
    output<<nlohmann::json{{"source_sha256",source_hash},{"faces",faces},{"vertices_bytes",vertices.size()*sizeof(float)},
        {"source_vertices",mesh.its.vertices.size()},{"packed_base_bytes",preview.cpu.packed_base_colors.size()*sizeof(uint32_t)},
        {"fresh_regions",fresh},{"pieces",puzzle.piece_count()},{"cancellation_checks",cancellation_checks.load()},
        {"initial_ms",initial_ms},{"background_ms",background_ms},{"install_ms",install_ms},
        {"initial_frame_fnv64",initial_frame_hash},{"fill_fnv64",digest},{"repaint_ms",samples},
        {"scope","headless CPU geometry and GLModel update before GPU upload"}}.dump(2);
    output.close();REQUIRE(output.good());
    REQUIRE(model_artifact_sha256(source)==source_hash);
}

// Opt-in local timing for the existing save button's value-only preparation.
// The historical draft is read from a caller-owned copy and never rewritten.
TEST_CASE("Historical puzzle save record measures UI-side encoding and copying", "[.BeautySaveRecordProbe]") {
    const auto env=[](const char* key){const auto value=boost::nowide::getenv(key);return value?std::string(value):std::string{};};
    const auto draft=env("ORCA_SAVE_RECORD_DRAFT"),report=env("ORCA_SAVE_RECORD_REPORT");
    if(draft.empty() || report.empty())SKIP("Set a draft copy and a fresh report path.");
    REQUIRE(boost::filesystem::is_regular_file(draft));
    REQUIRE_FALSE(boost::filesystem::exists(report));
    const auto source_hash=model_artifact_sha256(draft);
    boost::filesystem::ifstream input(draft);nlohmann::json wrapper;input>>wrapper;
    const auto& saved=wrapper.at("beauty_puzzle_draft");
    REQUIRE_FALSE(saved.contains("edit_regions"));
    REQUIRE_FALSE(saved.contains("guidance"));
    const auto geometry=saved.at("geometry_id").get<std::string>();
    const size_t faces=saved.at("face_count").get<size_t>();
    const auto document=BeautyDocument::decode(saved,geometry,faces);
    const auto puzzle=BeautyPuzzle::decode(saved.at("puzzle"),geometry,faces);
    nlohmann::json encode_samples=nlohmann::json::array(),copy_samples=nlohmann::json::array();
    nlohmann::json shared_samples=nlohmann::json::array();
    nlohmann::json accepted_copy_samples=nlohmann::json::array(),accepted_dump_samples=nlohmann::json::array();
    nlohmann::json accepted_write_samples=nlohmann::json::array();
    nlohmann::json preencode_samples=nlohmann::json::array(),preencoded_write_samples=nlohmann::json::array();
    size_t accepted_bytes=0;
    for(int iteration=0;iteration<6;++iteration) {
        const auto started=std::chrono::steady_clock::now();
        auto encoded=document.encode();encoded["puzzle"]=puzzle.encode();
        encoded["puzzle_base_file"]=saved.at("puzzle_base_file");
        encoded["puzzle_base_sha256"]=saved.at("puzzle_base_sha256");
        const auto encoded_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
        REQUIRE(encoded==saved);
        ModelFinishingOptions options;options.beauty_puzzle=true;options.beauty_document=std::move(encoded);
        const auto copy_started=std::chrono::steady_clock::now();
        auto worker_options=options;
        const auto copy_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-copy_started).count();
        REQUIRE(worker_options.beauty_document==saved);
        const auto shared_started=std::chrono::steady_clock::now();
        auto snapshot=std::make_shared<const ModelFinishingOptions>(std::move(options));
        auto worker_snapshot=snapshot;
        const auto shared_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-shared_started).count();
        snapshot.reset();
        REQUIRE(worker_snapshot->beauty_document==saved);
        const auto accepted_started=std::chrono::steady_clock::now();
        auto accepted=worker_snapshot->beauty_document;
        accepted["geometry_id"]=geometry;
        accepted["edits"]=nlohmann::json::array({{{"kind","puzzle"},{"source_geometry",geometry}}});
        const auto accepted_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-accepted_started).count();
        REQUIRE(accepted.at("puzzle")==saved.at("puzzle"));
        const auto preencode_started=std::chrono::steady_clock::now();
        const auto preencoded=accepted.dump();
        const auto preencode_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-preencode_started).count();
        nlohmann::json full_metadata=nlohmann::json::object();
        full_metadata["beauty_workbench"]=std::move(accepted);
        const auto dump_started=std::chrono::steady_clock::now();
        const auto serialized=full_metadata.dump();
        const auto dump_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-dump_started).count();
        accepted_bytes=serialized.size();
        const boost::filesystem::path output_path=report+".history-probe.json";
        REQUIRE_FALSE(boost::filesystem::exists(output_path));
        const auto write_started=std::chrono::steady_clock::now();
        boost::filesystem::ofstream history(output_path);
        history<<serialized;history.close();
        const auto write_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-write_started).count();
        REQUIRE(history.good());
        REQUIRE(boost::filesystem::file_size(output_path)==accepted_bytes);
        REQUIRE(boost::filesystem::remove(output_path));
        const auto preencoded_write_started=std::chrono::steady_clock::now();
        REQUIRE(GUI::ModelGenerationPresentation::write_json_with_preencoded_field(
            output_path,nlohmann::json::object(),"beauty_workbench",preencoded));
        const auto preencoded_write_ms=std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now()-preencoded_write_started).count();
        boost::filesystem::ifstream persisted(output_path);
        const std::string actual((std::istreambuf_iterator<char>(persisted)),std::istreambuf_iterator<char>());
        REQUIRE(actual==serialized);
        persisted.close();
        REQUIRE(boost::filesystem::remove(output_path));
        encode_samples.push_back(encoded_ms);copy_samples.push_back(copy_ms);shared_samples.push_back(shared_ms);
        accepted_copy_samples.push_back(accepted_ms);accepted_dump_samples.push_back(dump_ms);
        accepted_write_samples.push_back(write_ms);
        preencode_samples.push_back(preencode_ms);preencoded_write_samples.push_back(preencoded_write_ms);
    }
    boost::filesystem::ofstream output(report);
    output<<nlohmann::json{{"draft_sha256",source_hash},{"faces",faces},
        {"draft_bytes",boost::filesystem::file_size(draft)},
        {"encode_ms",encode_samples},{"options_copy_ms",copy_samples},
        {"shared_capture_ms",shared_samples},{"accepted_record_copy_ms",accepted_copy_samples},
        {"accepted_metadata_dump_ms",accepted_dump_samples},{"accepted_file_write_ms",accepted_write_samples},
        {"worker_preencode_ms",preencode_samples},{"preencoded_file_write_ms",preencoded_write_samples},
        {"accepted_metadata_bytes",accepted_bytes},
        {"scope","headless C++ equivalent of save-button record encoding, worker capture and history acceptance; no GUI"}}.dump(2);
    output.close();REQUIRE(output.good());
    REQUIRE(model_artifact_sha256(draft)==source_hash);
}

TEST_CASE("Palette changes match the original target instead of the previous approximation", "[BeautyTarget]") {
    const auto surface=BeautySurface::build(its_make_cube(10,10,10),{});
    auto p=BeautyPuzzle::create(*surface,1);
    const std::vector<RGBA> source(surface->areas.size(),RGBA{1,0,0,1});
    p.match_filaments(*surface,{{0,"#808080","PLA",true}}, {},source);
    auto restored=BeautyPuzzle::decode(p.encode(),p.geometry_id,p.face_piece.size());
    restored.match_filaments(*surface,{{0,"#808080","PLA",true},{1,"#FF0000","PLA",true}}, {},source);
    for(const auto& slot:restored.filament_slots)CHECK(slot.second==1);
}

TEST_CASE("Custom targets survive edits and reject corrupt persisted records", "[BeautyTarget]") {
    const auto surface=BeautySurface::build(its_make_cube(10,10,10),{});
    auto p=BeautyPuzzle::create(*surface,1);const auto id=p.face_piece.front();
    p.match_filaments(*surface,{{0,"#808080","PLA",true}});
    p.paint(id,{1,0,0,1});
    const auto snapshot=p;const auto saved=p.encode();
    REQUIRE(saved.at("schema")=="orca.beauty-puzzle/v4");
    auto restored=BeautyPuzzle::decode(saved,p.geometry_id,p.face_piece.size());
    CHECK(restored.same_edit(p));
    const auto split=restored.split(id,{0},*surface);
    CHECK(restored.target_colors.at(split)==p.target_colors.at(id));
    restored.match_filaments(*surface,{{0,"#808080","PLA",true},{1,"#FF0000","PLA",true}});
    CHECK(restored.filament_slots.at(split)==1);
    restored.paint_filament(split,0);CHECK_FALSE(restored.target_colors.count(split));
    const auto explicit_paint=restored;
    restored.match_filaments(*surface,restored.palette);CHECK(restored.same_edit(explicit_paint));
    restored.clear_color(split);CHECK_FALSE(restored.target_colors.count(split));
    restored=snapshot;CHECK(restored.same_edit(p)); // Undo restores both intent and output.
    restored.merge(id,restored.split(id,{0},*surface),*surface);
    CHECK(restored.target_colors.size()==1);REQUIRE_NOTHROW(restored.validate(*surface));
    auto invalid=saved;invalid["target_colors"][0]["id"]=999;
    CHECK_THROWS(BeautyPuzzle::decode(invalid,p.geometry_id,p.face_piece.size()));
    invalid=saved;invalid["target_colors"][0]["rgba"][0]=1.1;
    CHECK_THROWS(BeautyPuzzle::decode(invalid,p.geometry_id,p.face_piece.size()));
    invalid=saved;invalid["target_colors"].push_back(invalid["target_colors"][0]);
    CHECK_THROWS(BeautyPuzzle::decode(invalid,p.geometry_id,p.face_piece.size()));
    invalid=saved;invalid.erase("target_colors");
    CHECK_THROWS(BeautyPuzzle::decode(invalid,p.geometry_id,p.face_piece.size()));
    auto legacy=saved;legacy["schema"]="orca.beauty-puzzle/v2";legacy.erase("target_colors");legacy.erase("mixed_recipes");
    restored=BeautyPuzzle::decode(legacy,p.geometry_id,p.face_piece.size());
    CHECK(restored.target_colors.empty());
    restored.match_filaments(*surface,{{0,"#808080","PLA",true},{1,"#FF0000","PLA",true}});
    CHECK(restored.filament_slots.at(id)==0); // Old gray paint has no recoverable red intent.
}

TEST_CASE("Explicit same color material slots survive unrelated palette changes", "[BeautyTarget]") {
    const auto surface=BeautySurface::build(its_make_cube(10,10,10),{});
    auto p=BeautyPuzzle::create(*surface,1);const auto id=p.face_piece.front();
    p.match_filaments(*surface,{{0,"#808080","PLA",true},{1,"#808080","PLA",true}});
    p.paint_filament(id,1);
    p=BeautyPuzzle::decode(p.encode(),p.geometry_id,p.face_piece.size());
    p.match_filaments(*surface,{{0,"#808080","PLA",true},{1,"#808080","PLA",true},{2,"#FF0000","PLA",true}});
    CHECK(p.filament_slots.at(id)==1);
    p.match_filaments(*surface,{{0,"#808080","PLA",true},{1,"#808080","PLA",false},{2,"#FF0000","PLA",true}});
    CHECK(p.filament_slots.at(id)==0);
}

TEST_CASE("A selected custom target survives reload without repainting other faces", "[BeautyTarget]") {
    const auto surface=BeautySurface::build(its_make_cube(10,10,10),{});
    auto p=BeautyPuzzle::create(*surface,1);
    p.match_filaments(*surface,{{0,"#808080","PLA",true}});
    p.paint_faces_target({0},*surface,{1,0,0,1});
    p=BeautyPuzzle::decode(p.encode(),p.geometry_id,p.face_piece.size());
    p.match_filaments(*surface,{{0,"#808080","PLA",true},{1,"#FF0000","PLA",true}});
    for(size_t f=0;f<p.face_piece.size();++f)CHECK(p.filament_slots.at(p.face_piece[f])==(f==0?1:0));
    const auto before=p;
    CHECK_THROWS(p.paint_faces_target({999},*surface,{1,0,0,1}));CHECK(p.same_edit(before));
    CHECK_THROWS(p.paint_faces_target({0},*surface,{2,0,0,1}));CHECK(p.same_edit(before));
}

namespace {
// Explicit-instantiation access is confined to this test translation unit. It
// observes private stages without changing production visibility or its ODR.
template<class Tag, typename Tag::Type Member> struct PuzzleStageAccess {
    friend typename Tag::Type stage_member(Tag) { return Member; }
};
struct RegularizeStage {
    using Type = void (BeautyPuzzle::*)(const BeautySurface&, const std::function<bool()>&);
    friend Type stage_member(RegularizeStage);
};
struct SmoothStage {
    using Type = void (BeautyPuzzle::*)(const BeautySurface&, const std::vector<uint8_t>&,
        const std::vector<int32_t>&, const std::function<bool()>&,
        const std::vector<uint32_t>*, const std::vector<int32_t>*, const std::vector<uint8_t>*);
    friend Type stage_member(SmoothStage);
};
template struct PuzzleStageAccess<RegularizeStage, &BeautyPuzzle::regularize_boundaries>;
template struct PuzzleStageAccess<SmoothStage, &BeautyPuzzle::smooth_partition>;
template<class Tag, auto Member> struct AutoPuzzleStageAccess {
    friend auto stage_member(Tag) { return Member; }
};
struct BoundaryFieldStage { friend auto stage_member(BoundaryFieldStage); };
template struct AutoPuzzleStageAccess<BoundaryFieldStage, &BeautyPuzzle::boundary_field>;
struct PuzzleLab {
    using Type = std::array<double,3> (*)(const std::array<double,3>&);
    friend Type stage_member(PuzzleLab);
};
template struct PuzzleStageAccess<PuzzleLab, &BeautyPuzzle::to_lab>;
struct PuzzleStateCheck {
    using Type = void (BeautyPuzzle::*)() const;
    friend Type stage_member(PuzzleStateCheck);
};
struct PuzzleSurfaceCheck {
    using Type = void (*)(const BeautySurface&);
    friend Type stage_member(PuzzleSurfaceCheck);
};
template struct PuzzleStageAccess<PuzzleStateCheck, &BeautyPuzzle::check_state>;
template struct PuzzleStageAccess<PuzzleSurfaceCheck, &BeautyPuzzle::check_surface_data>;


// Shared bounded topology proof for test-only transfer candidates.
size_t restore_candidate_faces(const BeautyPuzzle& before, BeautyPuzzle& proposed,
                               const BeautySurface& surface, const std::vector<size_t>& risky,
                               size_t* unresolved, const std::function<bool()>& canceled) {
    const auto checkpoint=[&] {if(canceled && canceled())throw std::runtime_error("Candidate preparation cancelled.");};
    std::vector<uint32_t> visited(before.face_piece.size(),0);uint32_t stamp=0;
    std::vector<size_t> queue;queue.reserve(128);
    const auto donor_connected=[&](size_t removed,uint32_t owner) {
        std::vector<size_t> same;
        for(int32_t n:surface.face_neighbors[removed])if(n>=0 && proposed.face_piece[n]==owner &&
            std::find(same.begin(),same.end(),size_t(n))==same.end())same.push_back(size_t(n));
        if(same.empty())return false;
        if(same.size()==1)return true;
        if(++stamp==0){std::fill(visited.begin(),visited.end(),0);++stamp;}
        queue.assign(1,same[0]);visited[same[0]]=stamp;size_t reached=1;
        for(size_t at=0;at<queue.size() && queue.size()<128;++at)
            for(int32_t n:surface.face_neighbors[queue[at]])
                if(n>=0 && size_t(n)!=removed && proposed.face_piece[n]==owner && visited[n]!=stamp) {
                    visited[n]=stamp;queue.push_back(size_t(n));
                    if(std::find(same.begin()+1,same.end(),size_t(n))!=same.end())++reached;
                    if(reached==same.size())return true;
                }
        return false;
    };
    size_t reverted=0;
    for(unsigned pass=0;pass<8;++pass) {
        checkpoint();size_t changed=0;
        for(size_t at=0;at<risky.size();++at) {
            if((at&4095)==0)checkpoint();
            const size_t f=risky[pass%2?risky.size()-1-at:at];
            const uint32_t target=before.face_piece[f],owner=proposed.face_piece[f];
            if(target==owner)continue;
            const auto& neighbors=surface.face_neighbors[f];
            if(std::none_of(neighbors.begin(),neighbors.end(),[&](int32_t n){return n>=0 && proposed.face_piece[n]==target;}))continue;
            if(!donor_connected(f,owner))continue;
            proposed.face_piece[f]=target;++changed;
        }
        reverted+=changed;if(!changed)break;
    }
    if(unresolved)*unresolved=risky.size()-reverted;
    return reverted;
}

// TEST CANDIDATE ONLY. Restore a strong-color face only when it touches its
// original region and its removal provably leaves the donor connected. The
// bounded connectivity proof follows the existing smooth_partition approach.
size_t guard_color_transfers(const BeautyPuzzle& before, BeautyPuzzle& proposed,
                             const BeautySurface& surface, size_t* unresolved=nullptr,
                             const std::function<bool()>& canceled={}) {
    const auto checkpoint=[&] {if(canceled && canceled())throw std::runtime_error("Candidate preparation cancelled.");};
    std::map<uint32_t,size_t> slots;
    for(uint32_t id:before.face_piece)slots.emplace(id,slots.size());
    std::vector<std::array<double,3>> mean(slots.size()),lab;
    std::vector<double> area(slots.size(),0.);
    for(const auto& patch:surface.patches)lab.push_back(stage_member(PuzzleLab{})(patch.mean_color));
    for(size_t f=0;f<before.face_piece.size();++f) {
        if((f&4095)==0)checkpoint();
        const size_t a=slots.at(before.face_piece[f]);
        const double mass=std::max(surface.areas[f],1e-15);area[a]+=mass;
        for(size_t c=0;c<3;++c)mean[a][c]+=lab[surface.face_patch[f]][c]*mass;
    }
    for(size_t i=0;i<area.size();++i)for(double& c:mean[i])c/=area[i];
    std::vector<size_t> risky;
    for(size_t f=0;f<before.face_piece.size();++f)if(before.face_piece[f]!=proposed.face_piece[f]) {
        if((f&4095)==0)checkpoint();
        const size_t a=slots.at(before.face_piece[f]),b=slots.at(proposed.face_piece[f]);
        double old_fit=0.,new_fit=0.;
        for(size_t c=0;c<3;++c) {
            const double color=lab[surface.face_patch[f]][c];
            old_fit+=std::pow(color-mean[a][c],2);new_fit+=std::pow(color-mean[b][c],2);
        }
        // Initial scale comes from existing teeth trimming, not sample tuning.
        if(std::sqrt(new_fit)>std::sqrt(old_fit)+12.)risky.push_back(f);
    }
    return restore_candidate_faces(before,proposed,surface,risky,unresolved,canceled);
}


size_t guarded_regularize(BeautyPuzzle& puzzle,const BeautySurface& surface,
                         const std::function<bool()>& canceled={},size_t* unresolved=nullptr) {
    auto proposed=puzzle;
    (proposed.*stage_member(RegularizeStage{}))(surface,canceled);
    const size_t reverted=guard_color_transfers(puzzle,proposed,surface,unresolved,canceled);
    if(canceled && canceled())throw std::runtime_error("Candidate preparation cancelled.");
    proposed.validate(surface);puzzle=std::move(proposed);
    return reverted;
}


// Automatic-only test candidate, not a persistence/manual-paint policy.
nlohmann::json constrain_stationary_palette(const BeautyPuzzle& baseline,BeautyPuzzle& candidate,
                                           const BeautySurface& surface,const std::vector<RGBA>& source) {
    if(!baseline.same_palette(candidate.palette) || baseline.geometry_id!=candidate.geometry_id ||
       baseline.face_piece.size()!=candidate.face_piece.size() || source.size()!=candidate.face_piece.size() ||
       !baseline.mixed_recipes.empty() || !candidate.mixed_recipes.empty())
        throw std::invalid_argument("Joint candidate requires identical physical palette and geometry.");
    baseline.validate(surface);candidate.validate(surface);
    const size_t k=candidate.palette.size();
    if(!k || k>6 || std::any_of(candidate.palette.begin(),candidate.palette.end(),[](const auto& c){return !c.compatible;}))
        throw std::invalid_argument("Joint candidate requires compatible physical channels.");
    std::map<size_t,size_t> index;
    std::vector<std::array<float,4>> palette;
    for(size_t j=0;j<k;++j){index[candidate.palette[j].slot]=j;palette.push_back(BeautyPuzzle::filament_color(candidate.palette[j]));}
    struct Region {std::array<double,6> cost{};std::array<bool,6> allowed,all_allowed;Region(){allowed.fill(true);all_allowed.fill(true);}};
    std::map<uint32_t,Region> regions;
    std::map<std::array<float,3>,std::array<double,6>> cache;
    for(size_t f=0;f<source.size();++f) {
        const std::array<float,3> rgb{source[f][0],source[f][1],source[f][2]};
        for(float c:rgb)if(!std::isfinite(c) || c<0 || c>1)throw std::invalid_argument("Invalid joint source color.");
        auto entry=cache.try_emplace(rgb);
        if(entry.second)for(size_t j=0;j<k;++j)
            entry.first->second[j]=tex2color::color_utils::calc_rgb_color_difference_by_ciede2000_srgb01(
                {rgb[0],rgb[1],rgb[2]},{palette[j][0],palette[j][1],palette[j][2]});
        const auto& distances=entry.first->second;
        const auto original=baseline.face_piece[f],id=candidate.face_piece[f];
        const double old_error=distances[index.at(baseline.filament_slots.at(original))];
        auto& region=regions[id];
        for(size_t j=0;j<k;++j) {
            region.cost[j]+=surface.areas[f]*distances[j];
            if(surface.areas[f]>0 && distances[j]>old_error+1e-7) {
                region.all_allowed[j]=false;if(original==id)region.allowed[j]=false;
            }
        }
    }
    auto result=candidate;size_t changed=0;
    nlohmann::json infeasible=nlohmann::json::array();
    for(const auto& item:regions) {
        const auto id=item.first;const auto& region=item.second;
        const size_t previous=index.at(candidate.filament_slots.at(id));
        size_t best=k;
        for(size_t j=0;j<k;++j)if(region.allowed[j] && (best==k || region.cost[j]<region.cost[best]))best=j;
        if(best==k)throw std::runtime_error("Stationary-region baseline must provide a feasible color.");
        if(region.allowed[previous] && region.cost[previous]==region.cost[best])best=previous;
        bool feasible=false;for(size_t j=0;j<k;++j)feasible|=region.all_allowed[j];
        if(!feasible)infeasible.push_back(id);
        changed+=best!=previous;
        // Preserve automatic target RGB; do not turn this into explicit paint.
        result.colors[id]=palette[best];result.filament_slots[id]=candidate.palette[best].slot;
    }
    result.validate(surface);candidate=std::move(result);
    return {{"changed_region_colors",changed},{"all_faces_infeasible_regions",infeasible},
            {"status","test_candidate_only; source-error constraint, not manual intent or visual acceptance"}};
}

// Fixed-output diagnostic: test whether residual transfers can be undone without
// any other face changing color. Targets are intentionally frozen; this is not
// a rematch-safe or manual-edit production operation.
nlohmann::json restore_residual_transfers(const BeautyPuzzle& baseline,BeautyPuzzle& candidate,
                                         const BeautySurface& surface,const std::vector<RGBA>& source,
                                         const std::function<bool()>& canceled={}) {
    const auto checkpoint=[&] {if(canceled && canceled())throw std::runtime_error("Residual candidate cancelled.");};
    checkpoint();baseline.validate(surface);candidate.validate(surface);
    if(!baseline.same_palette(candidate.palette) || baseline.geometry_id!=candidate.geometry_id ||
       source.size()!=candidate.face_piece.size() || baseline.face_piece.size()!=source.size() ||
       !baseline.mixed_recipes.empty() || !candidate.mixed_recipes.empty())
        throw std::invalid_argument("Residual candidate requires identical physical palette and geometry.");
    const auto error=[](const RGBA& source,const RGBA& output) {
        return tex2color::color_utils::calc_rgb_color_difference_by_ciede2000_srgb01(
            {source[0],source[1],source[2]},{output[0],output[1],output[2]});
    };
    std::vector<size_t> risky;size_t rejected_color=0;
    for(size_t f=0;f<source.size();++f) {
        if((f&4095)==0)checkpoint();
        for(float c:source[f])if(!std::isfinite(c) || c<0 || c>1)throw std::invalid_argument("Invalid residual source color.");
        const auto original=baseline.face_piece[f],current=candidate.face_piece[f];
        if(original==current || surface.areas[f]<=0)continue;
        const double old_error=error(source[f],baseline.colors.at(original));
        const double current_error=error(source[f],candidate.colors.at(current));
        if(current_error<=old_error+1e-7)continue;
        const auto target=candidate.colors.find(original);
        if(target==candidate.colors.end() || error(source[f],target->second)>old_error+1e-7) {
            ++rejected_color;continue;
        }
        risky.push_back(f);
    }
    auto result=candidate;size_t unresolved=0;
    const size_t restored=restore_candidate_faces(baseline,result,surface,risky,&unresolved,canceled);
    checkpoint();result.validate(surface);candidate=std::move(result);
    return {{"restored_faces",restored},{"unresolved_topology_or_pass_limit",unresolved},
            {"rejected_destination_color",rejected_color},
            {"status","fixed-output diagnostic only; target refresh and rematch not validated"}};
}

}

TEST_CASE("Historical workbench partition separates grouping regularization and smoothing", "[.BeautyPartitionProbe]") {
    const auto env=[](const char* key){const auto value=boost::nowide::getenv(key);return value?std::string(value):std::string{};};
    const auto source=env("ORCA_BEAUTY_PREP_SOURCE"),report=env("ORCA_BEAUTY_PARTITION_REPORT");
    if(source.empty() || report.empty())SKIP("Set a local model and a fresh partition report path.");
    REQUIRE(boost::filesystem::exists(source));
    REQUIRE_FALSE(boost::filesystem::exists(report));
    const auto source_hash=model_artifact_sha256(source);
    REQUIRE_FALSE(source_hash.empty());
    TriangleMesh mesh;ObjInfo colors;std::string error;
    REQUIRE(load_model_artifact(source,mesh,colors,error));
    auto surface=BeautySurface::build(mesh.its,colors.vertex_colors);
    nlohmann::json samples=nlohmann::json::array();
    std::string expected;
    for(int iteration=0;iteration<5;++iteration) {
        auto previous=std::chrono::steady_clock::now();const auto started=previous;
        nlohmann::json stages=nlohmann::json::object();
        const auto mark=[&](const char* name) {
            const auto now=std::chrono::steady_clock::now();
            stages[name]=std::chrono::duration<double,std::milli>(now-previous).count();previous=now;
        };
        auto puzzle=BeautyPuzzle::create(*surface,28);
        mark("create_28");
        (puzzle.*stage_member(RegularizeStage{}))(*surface,{});
        mark("regularize_boundaries");
        (puzzle.*stage_member(SmoothStage{}))(*surface,{},{},{},nullptr,nullptr,nullptr);
        mark("smooth_partition");
        const double operation_ms=std::chrono::duration<double,std::milli>(previous-started).count();
        puzzle.validate(*surface);
        const auto encoded=puzzle.encode().dump();
        if(iteration==0) {
            expected=encoded;
            CHECK(encoded==BeautyPuzzle::create_regions(*surface).encode().dump());
        } else CHECK(encoded==expected);
        samples.push_back({{"phase",iteration==0?"first":iteration==1?"warmup":"warm"},
            {"stages_ms",stages},{"operation_ms",operation_ms},{"pieces",puzzle.piece_count()}});
    }
    REQUIRE(model_artifact_sha256(source)==source_hash);
    const auto puzzle_digest=Slic3r::GUI::LocalSemanticGeometry::detail::digest(expected.data(),expected.size(),nullptr);
    std::string puzzle_sha256;
    for(unsigned char byte:puzzle_digest){puzzle_sha256+="0123456789abcdef"[byte>>4];puzzle_sha256+="0123456789abcdef"[byte&15];}
    boost::filesystem::ofstream output(report);
    output<<nlohmann::json{{"source_sha256",source_hash},{"faces",mesh.its.indices.size()},
        {"puzzle_sha256",puzzle_sha256},{"samples",samples},
        {"scope","offline no-recognition partition; surface and model loading excluded"}}.dump(2);
    output.close();
    REQUIRE(output.good());
}

TEST_CASE("Historical workbench boundary field stays stable across replays", "[.BeautyBoundaryFieldProbe]") {
    const auto env=[](const char* key){const auto value=boost::nowide::getenv(key);return value?std::string(value):std::string{};};
    const auto source=env("ORCA_BEAUTY_PREP_SOURCE"),report=env("ORCA_BEAUTY_FIELD_REPORT");
    if(source.empty() || report.empty())SKIP("Set a local model and a fresh field report path.");
    REQUIRE(boost::filesystem::exists(source));
    REQUIRE_FALSE(boost::filesystem::exists(report));
    const auto source_hash=model_artifact_sha256(source);
    TriangleMesh mesh;ObjInfo colors;std::string error;
    REQUIRE(load_model_artifact(source,mesh,colors,error));
    auto surface=BeautySurface::build(mesh.its,colors.vertex_colors);
    auto puzzle=BeautyPuzzle::create(*surface,28);
    (puzzle.*stage_member(RegularizeStage{}))(*surface,{});
    std::unordered_map<uint32_t,size_t> slot;
    std::vector<double> area;
    std::vector<size_t> members;
    for(size_t f=0;f<puzzle.face_piece.size();++f) {
        const auto entry=slot.emplace(puzzle.face_piece[f],slot.size());
        if(entry.second) {area.push_back(0.);members.push_back(0);}
        area[entry.first->second]+=std::max(surface->areas[f],1e-15);
        ++members[entry.first->second];
    }
    const std::vector<uint8_t> no_scope;
    const std::vector<int32_t> no_labels;
    const std::function<bool()> no_cancel;
    nlohmann::json samples=nlohmann::json::array();
    std::vector<uint32_t> expected_targets;
    std::vector<float> expected_confidence;
    for(int iteration=0;iteration<5;++iteration) {
        const auto started=std::chrono::steady_clock::now();
        auto field=(puzzle.*stage_member(BoundaryFieldStage{}))(*surface,no_scope,no_labels,
            slot,area,members,no_cancel,nullptr,nullptr,nullptr);
        const double elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
        if(iteration==0) {expected_targets=field.target;expected_confidence=field.confidence;}
        else {CHECK(field.target==expected_targets);CHECK(field.confidence==expected_confidence);}
        samples.push_back({{"phase",iteration==0?"first":iteration==1?"warmup":"warm"},
            {"elapsed_ms",elapsed},{"targeted_faces",std::count_if(field.target.begin(),field.target.end(),
                [](uint32_t target){return target!=0;})}});
    }
    if (puzzle.face_piece.size() >= 200000 && std::thread::hardware_concurrency() > 1) {
        const auto caller = std::this_thread::get_id();
        std::atomic<bool> worker_seen{false};
        std::atomic<unsigned> worker_checks{0};
        // Optional delayed cancellation reaches work inside the solve, while
        // the default replay still cancels at its first worker checkpoint.
        const auto delay_text = env("ORCA_BEAUTY_FIELD_CANCEL_CHECKS");
        const unsigned cancel_after = delay_text.empty() ? 1u : unsigned(std::stoul(delay_text));
        REQUIRE(cancel_after > 0);
        REQUIRE(cancel_after <= 4096);
        const std::function<bool()> cancel_on_worker = [&] {
            if (std::this_thread::get_id() != caller) {
                worker_seen = true;
                return ++worker_checks >= cancel_after;
            }
            return false;
        };
        const auto original_partition = puzzle.encode();
        REQUIRE_THROWS_AS((puzzle.*stage_member(BoundaryFieldStage{}))(*surface,no_scope,no_labels,
            slot,area,members,cancel_on_worker,nullptr,nullptr,nullptr), std::runtime_error);
        CHECK(worker_seen.load());
        CHECK(worker_checks.load() >= cancel_after);
        CHECK(puzzle.encode() == original_partition);
    }
    REQUIRE(model_artifact_sha256(source)==source_hash);
    std::string field_bytes;
    field_bytes.reserve(expected_targets.size()*sizeof(uint32_t)+expected_confidence.size()*sizeof(float));
    field_bytes.append(reinterpret_cast<const char*>(expected_targets.data()),expected_targets.size()*sizeof(uint32_t));
    field_bytes.append(reinterpret_cast<const char*>(expected_confidence.data()),expected_confidence.size()*sizeof(float));
    const auto field_digest=Slic3r::GUI::LocalSemanticGeometry::detail::digest(field_bytes.data(),field_bytes.size(),nullptr);
    std::string field_sha256;
    for(unsigned char byte:field_digest){field_sha256+="0123456789abcdef"[byte>>4];field_sha256+="0123456789abcdef"[byte&15];}
    boost::filesystem::ofstream output(report);
    output<<nlohmann::json{{"source_sha256",source_hash},{"faces",mesh.its.indices.size()},
        {"field_sha256",field_sha256},{"samples",samples},
        {"scope","offline no-recognition boundary field only"}}.dump(2);
    output.close();
    REQUIRE(output.good());
}

// Explicit, read-only replay on user supplied model copies. Not GUI acceptance.
TEST_CASE("Historical models retain direct matching after a restrictive palette round trip", "[.][BeautyTargetModelProbe]") {
    const auto env=[](const char* key){const auto p=boost::nowide::getenv(key);return p?std::string(p):std::string{};};
    const auto source=env("ORCA_TARGET_SOURCE"),output=env("ORCA_TARGET_OUTPUT");
    if(source.empty() || output.empty())SKIP("Explicit model copy and fresh output required.");
    REQUIRE_FALSE(boost::filesystem::exists(output));
    const auto hash=model_artifact_sha256(source);
    const auto repeat_text=env("ORCA_TARGET_REPEATS");
    const int repeats=repeat_text.empty()?0:std::stoi(repeat_text);
    REQUIRE(repeats>=0);REQUIRE(repeats<=20);
    const auto signature=[](const std::string& bytes) {
        const auto raw=Slic3r::GUI::LocalSemanticGeometry::detail::digest(bytes.data(),bytes.size(),nullptr);
        std::string result;for(unsigned char c:raw){result+="0123456789abcdef"[c>>4];result+="0123456789abcdef"[c&15];}
        return result;
    };
    nlohmann::json measurements=nlohmann::json::array(), summary;
    std::string expected_signature, expected_loaded_signature;
    for(int iteration=0;iteration<(repeats?repeats+2:1);++iteration) {
    nlohmann::json stages=nlohmann::json::object();
    auto previous=std::chrono::steady_clock::now();const auto started=previous;
    const auto mark=[&](const char* name) {
        const auto now=std::chrono::steady_clock::now();
        stages[name]=std::chrono::duration<double>(now-previous).count();previous=now;
    };
    TriangleMesh mesh;ObjInfo colors;std::string error;
    const bool loaded=load_model_artifact(source,mesh,colors,error);INFO(error);REQUIRE(loaded);
    mark("load_decode");
    const auto surface=BeautySurface::build(mesh.its,colors.vertex_colors);
    mark("surface_adjacency");
    const auto faces=beauty_source_face_colors(mesh.its,colors.vertex_colors);
    mark("source_face_colors");
    auto original=BeautyPuzzle::create(*surface,180);
    mark("partition");
    const std::vector<PhysicalFilamentChannel> palette{{0,"#F7E2DA","PLA",true},{1,"#282629","PLA",true},
        {2,"#F6F7F9","PLA",true},{3,"#EA9A92","PLA",true},{4,"#668CB6","PLA",true},{5,"#958B86","PLA",true}};
    auto direct=original;auto roundtrip=original;mark("puzzle_copies");
    direct.match_filaments(*surface,palette,{},faces);mark("direct_match");
    roundtrip.match_filaments(*surface,{{0,"#808080","PLA",true}}, {},faces);mark("restrictive_match");
    const auto serialized=roundtrip.encode().dump();mark("json_encode");
    roundtrip=BeautyPuzzle::decode(nlohmann::json::parse(serialized),roundtrip.geometry_id,roundtrip.face_piece.size());
    mark("json_decode");
    roundtrip.match_filaments(*surface,palette,{},faces);
    mark("rematch");
    const double operation_seconds=std::chrono::duration<double>(previous-started).count();
    size_t changed=0;for(size_t f=0;f<faces.size();++f)
        changed+=direct.filament_slots.at(direct.face_piece[f])!=roundtrip.filament_slots.at(roundtrip.face_piece[f]);
    std::string loaded_bytes;
    loaded_bytes.reserve(mesh.its.vertices.size()*3*sizeof(float)+mesh.its.indices.size()*3*sizeof(int)+colors.vertex_colors.size()*4*sizeof(float));
    const auto append=[&](const auto& value) { loaded_bytes.append(reinterpret_cast<const char*>(&value),sizeof(value)); };
    for(const auto& vertex:mesh.its.vertices)for(int c=0;c<3;++c)append(vertex[c]);
    for(const auto& face:mesh.its.indices)for(int c=0;c<3;++c)append(face[c]);
    for(const auto& color:colors.vertex_colors)for(int c=0;c<4;++c)append(color[c]);
    const auto loaded_signature=signature(loaded_bytes);
    if(iteration==0)expected_loaded_signature=loaded_signature;
    CHECK(loaded_signature==expected_loaded_signature);
    const auto output_signature=signature(direct.encode().dump());
    if(iteration==0)expected_signature=output_signature;
    CHECK(output_signature==expected_signature);CHECK(changed==0);
    nlohmann::json memory=nullptr;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};counters.cb=sizeof(counters);
    if(K32GetProcessMemoryInfo(GetCurrentProcess(),&counters,sizeof(counters)))
        memory={{"working_set_bytes",uint64_t(counters.WorkingSetSize)},
                {"process_lifetime_peak_working_set_bytes",uint64_t(counters.PeakWorkingSetSize)}};
#endif
    measurements.push_back({{"phase",iteration==0?"first_process_pass":iteration==1?"warmup":"warm"},
        {"stages_seconds",stages},{"operation_seconds",operation_seconds},{"memory",memory},
        {"output_sha256",output_signature},{"changed_faces",changed}});
    summary={{"source",source},{"sha256",hash},{"faces",faces.size()},
             {"loaded_mesh_colors_sha256",loaded_signature},{"pieces",original.piece_count()},{"changed_faces",changed},{"source_unchanged",true}};
    if(iteration==0 && env("ORCA_TARGET_EDIT_PROFILE")=="1") {
        REQUIRE(repeats==0); // Isolated diagnostic, never mixed with stage timings.
        auto current=BeautyPuzzle::create_regions(*surface);
        beauty_match_feature_filaments(current,*surface,BeautyGuidance{},faces,palette,{});
        BeautyDocument document;document.geometry_id=surface->geometry_id;
        document.face_count=faces.size();document.face_patch=surface->face_patch;
        const auto expected=current.encode();const auto expected_document=document.encode();
        nlohmann::json samples=nlohmann::json::array();
        for(unsigned sample=0;sample<7;++sample) {
            nlohmann::json times;
            auto start=std::chrono::steady_clock::now();
            const auto mark_edit=[&](const char* name) {
                auto end=std::chrono::steady_clock::now();
                times[name]=std::chrono::duration<double>(end-start).count();start=end;
            };
            auto next=current;mark_edit("snapshot_copy");
            next.match_filaments(*surface,palette);mark_edit("same_palette_match");
            (next.*stage_member(PuzzleStateCheck{}))();mark_edit("state_validation");
            stage_member(PuzzleSurfaceCheck{})(*surface);mark_edit("surface_validation");

            const bool same=next.same_edit(current);mark_edit("same_edit");
            auto record=document.encode();mark_edit("document_encode");
            record["puzzle"]=next.encode();mark_edit("puzzle_encode_and_attach");
            record["puzzle_base_file"]="input.glb";record["puzzle_base_sha256"]=hash;
            mark_edit("base_metadata_attach");
            const auto serialized=record.dump();mark_edit("json_dump");
            const auto parsed=nlohmann::json::parse(serialized);mark_edit("json_parse");
            REQUIRE(same);REQUIRE(record.at("puzzle")==expected);
            REQUIRE(parsed==record);
            REQUIRE(BeautyPuzzle::decode(parsed.at("puzzle"),current.geometry_id,faces.size()).same_edit(current));
            REQUIRE(BeautyDocument::decode(parsed,current.geometry_id,faces.size()).encode()==expected_document);
            samples.push_back({{"phase",sample==0?"first":sample==1?"warmup":"warm"},
                {"seconds",times},{"record_bytes",serialized.size()},
                {"patch_runs",record.at("patch_runs").size()},
                {"puzzle_runs",record.at("puzzle").at("piece_runs").size()},
                {"record_sha256",signature(serialized)}});
        }
        summary["edit_profile"]={{"samples",samples},{"pieces",current.piece_count()},
            {"scope","offline replay of actual methods; no GUI/event-loop/disk/render timing"},
            {"state","no-recognition automatic fallback; no edit_regions, semantic guidance or user groups"}};
    }
    const auto quality_dir=env("ORCA_TARGET_QUALITY_DIR");
    if(iteration==0 && !quality_dir.empty()) {
        REQUIRE(repeats==0); // Exports are not part of the performance protocol.
        const boost::filesystem::path directory(quality_dir);
        REQUIRE_FALSE(boost::filesystem::exists(directory));
        REQUIRE(boost::filesystem::create_directory(directory));
        auto quality_puzzle=BeautyPuzzle::create_regions(*surface);
        beauty_match_feature_filaments(quality_puzzle,*surface,BeautyGuidance{},faces,palette,{});
        REQUIRE_NOTHROW(quality_puzzle.validate(*surface));
        const auto export_quality=[&](const BeautyPuzzle& quality_puzzle,
                                      const boost::filesystem::path& directory,
                                      const std::string& method) {
        nlohmann::json files=nlohmann::json::object();
        const auto write=[&](const char* name,const auto& values,const char* dtype,size_t columns) {
            const auto path=directory/name;
            boost::filesystem::ofstream out(path,std::ios::binary);
            const size_t bytes=values.size()*sizeof(values[0]);
            out.write(reinterpret_cast<const char*>(values.data()),std::streamsize(bytes));out.close();
            REQUIRE(out.good());
            files[name]={{"sha256",model_artifact_sha256(path)},{"bytes",bytes},{"dtype",dtype},
                         {"shape",{values.size()/columns,columns}}};
        };
        std::vector<float> vertices,source_rgb,automatic_rgb;
        std::vector<int32_t> triangles;
        vertices.reserve(mesh.its.vertices.size()*3);triangles.reserve(mesh.its.indices.size()*3);
        for(const auto& v:mesh.its.vertices)for(int c=0;c<3;++c)vertices.push_back(v[c]);
        for(const auto& f:mesh.its.indices)for(int c=0;c<3;++c)triangles.push_back(f[c]);
        source_rgb.reserve(faces.size()*3);automatic_rgb.reserve(faces.size()*3);
        for(size_t f=0;f<faces.size();++f)for(int c=0;c<3;++c) {
            source_rgb.push_back(faces[f][c]);automatic_rgb.push_back(quality_puzzle.colors.at(quality_puzzle.face_piece[f])[c]);
        }
        const uint32_t endian=1;REQUIRE(*reinterpret_cast<const unsigned char*>(&endian)==1);
        write("vertices.f32",vertices,"<f4",3);write("triangles.i32",triangles,"<i4",3);
        write("source.f32",source_rgb,"<f4",3);write("automatic.f32",automatic_rgb,"<f4",3);
        write("areas.f64",surface->areas,"<f8",1);write("pieces.u32",quality_puzzle.face_piece,"<u4",1);
        boost::filesystem::ofstream metadata(directory/"manifest.json");
        metadata<<nlohmann::json({{"schema",1},{"source_sha256",hash},{"geometry_id",surface->geometry_id},
            {"loaded_mesh_colors_sha256",loaded_signature},{"output_sha256",signature(quality_puzzle.encode().dump())},
            {"palette",quality_puzzle.encode().at("palette")},{"files",files},
            {"source_representation","native loader vertex colors averaged per face; not original texture pixels"},
            {"automatic_method",method}}).dump(2);
        metadata.close();REQUIRE(metadata.good());
        };
        export_quality(quality_puzzle,directory,"workbench no-recognition fallback: create_regions + beauty_match_feature_filaments; no manual edits");
        if(env("ORCA_TARGET_QUALITY_STAGES")=="1") {
            auto staged=BeautyPuzzle::create(*surface,28);
            const auto export_stage=[&](const char* name) {
                auto matched=staged;
                beauty_match_feature_filaments(matched,*surface,BeautyGuidance{},faces,palette,{});
                REQUIRE_NOTHROW(matched.validate(*surface));
                const auto folder=directory/name;
                REQUIRE(boost::filesystem::create_directory(folder));
                export_quality(matched,folder,std::string("stage replay with identical matching: ")+name);
                return matched;
            };
            export_stage("01-create28");
            (staged.*stage_member(RegularizeStage{}))(*surface,{});
            export_stage("02-regularize");
            (staged.*stage_member(SmoothStage{}))(*surface,{},{},{},nullptr,nullptr,nullptr);
            const auto final=export_stage("03-smooth");
            REQUIRE(final.encode()==quality_puzzle.encode());
            // Ablation only: keep the same initial budget and matcher, omit
            // regularization, and retain smoothing. Never a production route.
            staged=BeautyPuzzle::create(*surface,28);
            (staged.*stage_member(SmoothStage{}))(*surface,{},{},{},nullptr,nullptr,nullptr);
            export_stage("04-ablation-no-regularize");
            staged=BeautyPuzzle::create(*surface,28);
            const auto before_guard=staged.face_piece;
            const auto guard_started=std::chrono::steady_clock::now();
            size_t unresolved=0;
            const size_t reverted=guarded_regularize(staged,*surface,{},&unresolved);
            const double guard_seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-guard_started).count();
            size_t retained=0;double retained_area=0.;
            for(size_t f=0;f<before_guard.size();++f)if(before_guard[f]!=staged.face_piece[f]) {
                ++retained;retained_area+=surface->areas[f];
            }
            export_stage("05-guard-regularize");
            (staged.*stage_member(SmoothStage{}))(*surface,{},{},{},nullptr,nullptr,nullptr);
            auto joint=export_stage("06-guard-smooth");
            const auto target_before=joint.target_colors;const auto partition_before=joint.face_piece;
            const auto joint_stats=constrain_stationary_palette(quality_puzzle,joint,*surface,faces);
            REQUIRE(joint.target_colors==target_before);REQUIRE(joint.face_piece==partition_before);
            const auto joint_dir=directory/"07-joint-palette";
            REQUIRE(boost::filesystem::create_directory(joint_dir));
            export_quality(joint,joint_dir,"native test candidate: local color guard + stationary source-error constrained physical palette");
            boost::filesystem::ofstream joint_report(directory/"joint.json");joint_report<<joint_stats.dump(2);
            joint_report.close();REQUIRE(joint_report.good());
            auto residual=joint;
            const auto residual_stats=restore_residual_transfers(quality_puzzle,residual,*surface,faces);
            REQUIRE(residual.colors==joint.colors);REQUIRE(residual.filament_slots==joint.filament_slots);
            const auto residual_dir=directory/"08-residual-transfer";
            REQUIRE(boost::filesystem::create_directory(residual_dir));
            export_quality(residual,residual_dir,"fixed-output residual transfer diagnostic; no production or rematch acceptance");
            boost::filesystem::ofstream residual_report(directory/"residual.json");
            residual_report<<residual_stats.dump(2);residual_report.close();REQUIRE(residual_report.good());
            boost::filesystem::ofstream guard_report(directory/"guard.json");
            guard_report<<nlohmann::json({{"reverted_faces",reverted},{"unresolved_risky_faces",unresolved},{"retained_transfer_faces",retained},
                {"retained_transfer_area",retained_area},{"regularize_plus_guard_seconds",guard_seconds},
                {"status","test_candidate_only; single diagnostic timing, not performance acceptance"}}).dump(2);
            guard_report.close();REQUIRE(guard_report.good());
            std::vector<uint32_t> component(surface->areas.size(),0);
            uint32_t component_count=0;
            std::vector<size_t> queue;
            for(size_t seed=0;seed<component.size();++seed)if(!component[seed]) {
                ++component_count;component[seed]=component_count;queue.assign(1,seed);
                for(size_t at=0;at<queue.size();++at)
                    for(int32_t n:surface->face_neighbors[queue[at]])if(n>=0 && !component[n]) {
                        component[n]=component_count;queue.push_back(size_t(n));
                    }
            }
            boost::filesystem::ofstream graph(directory/"components.u32",std::ios::binary);
            graph.write(reinterpret_cast<const char*>(component.data()),std::streamsize(component.size()*sizeof(uint32_t)));
            graph.close();REQUIRE(graph.good());
            boost::filesystem::ofstream topology(directory/"topology.json");
            topology<<nlohmann::json({{"source_sha256",hash},{"surface_patches",surface->patches.size()},
                {"connected_components",component_count},{"faces",component.size()},
                {"components_sha256",model_artifact_sha256(directory/"components.u32")}}).dump(2);
            topology.close();REQUIRE(topology.good());
        }
    }
    } // Each iteration releases its model data; allocator/OS caches are not cleared.
    REQUIRE(model_artifact_sha256(source)==hash);
    summary["measurements"]=measurements;
    summary["memory_scope"]="OS process lifetime peak (includes test harness and validation), not per-stage allocation";
    summary["timing_scope"]="native stages exclude source hashes, output signature, comparison and report write; no GUI or disk-save timing";
    boost::filesystem::ofstream result(output);
    result<<summary.dump(2);
    REQUIRE(result.good());
}

TEST_CASE("Region mix targets use area weighted original faces regardless of saved paint", "[BeautyPuzzle]") {
    const auto mesh=its_make_cube(10,10,10);const auto surface=BeautySurface::build(mesh,{});
    BeautyPuzzle p;p.geometry_id=surface->geometry_id;p.next_id=3;p.face_piece.assign(mesh.indices.size(),1);p.face_piece[0]=p.face_piece[1]=2;
    surface->areas[0]=1;surface->areas[1]=3;
    std::vector<RGBA> source(mesh.indices.size(),RGBA{0,1,0,1});source[0]={1,0,0,1};source[1]={0,0,1,1};
    p.colors[2]={0,0,0,1};const auto before=p.encode();
    const auto target=p.source_region_color(2,*surface,source);
    CHECK_THAT(target[0],Catch::Matchers::WithinAbs(.25,1e-6));CHECK_THAT(target[2],Catch::Matchers::WithinAbs(.75,1e-6));
    CHECK(target[1]==0);CHECK(p.encode()==before);
    CHECK_THROWS(p.source_region_color(3,*surface,source));source.pop_back();CHECK_THROWS(p.source_region_color(2,*surface,source));
}

TEST_CASE("Unmatched edit-region paint preserves untouched original texture until explicit filament matching", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface=BeautySurface::build(its_make_cube(10,10,10),{});
    BeautyPuzzle puzzle;puzzle.geometry_id=surface->geometry_id;
    puzzle.face_piece.assign(surface->areas.size(),1);puzzle.next_id=2;
    const auto before=puzzle;
    puzzle.paint_faces_color({0,1},*surface,{1.f,1.f,1.f,1.f});
    CHECK(puzzle.palette.empty());CHECK(puzzle.filament_slots.empty());
    for(size_t face=0;face<puzzle.face_piece.size();++face) {
        const bool painted=puzzle.colors.count(puzzle.face_piece[face])!=0;
        CHECK(painted==(face<2));
    }
    CHECK(puzzle.face_piece!=before.face_piece);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    const auto restored=BeautyPuzzle::decode(puzzle.encode(),puzzle.geometry_id,puzzle.face_piece.size());
    CHECK(restored.same_edit(puzzle));
    auto matched=restored;
    matched.match_filaments(*surface,{{0,"#FFFFFF","PLA",true},{1,"#000000","PLA",true}});
    CHECK(matched.filament_slots.size()==matched.piece_count());
    CHECK(before.colors.empty());
}

TEST_CASE("Native region proposals improve available colors without mixing material types", "[BeautyPuzzle]") {
    const std::vector<std::string> colors{"#F7E2DA","#282629","#F6F7F9","#EA9A92","#668CB6","#958B86"};
    auto types=std::vector<std::string>(6,"PLA");
    const auto result=GUI::suggest_native_mixed_filament("#BB6A65",colors,{},types);
    REQUIRE(result.valid);REQUIRE(result.components.size()>=2);REQUIRE(result.components.size()<=3);
    int total=0;for(const auto& c:result.components){CHECK(c.filament_index>=1);CHECK(c.filament_index<=6);CHECK(c.ratio>0);total+=c.ratio;}
    CHECK(total==100);
    CHECK_FALSE(GUI::suggest_native_mixed_filament("#BB6A65",colors,{},types,result.matched_color_hex).valid);
    CHECK_FALSE(GUI::suggest_native_mixed_filament("#926247",colors,{},types,"#907273").valid);
    CHECK_FALSE(GUI::suggest_native_mixed_filament(colors[0],colors,{},types).valid);
    CHECK_FALSE(GUI::suggest_native_mixed_filament("invalid",colors,{},types).valid);
    CHECK_FALSE(GUI::suggest_native_mixed_filament("#BB6A65",colors,{},{}).valid);
    for(size_t i=0;i<types.size();++i)types[i]="material-"+std::to_string(i);
    CHECK_FALSE(GUI::suggest_native_mixed_filament("#BB6A65",colors,{},types).valid);
    types[2]=types[5]="PLA";
    const auto same=GUI::suggest_native_mixed_filament("#BB6A65",colors,{},types);
    for(const auto& c:same.components)CHECK((c.filament_index==3 || c.filament_index==6));
}

TEST_CASE("Explicit region colors receive native mix proposals without creating slots", "[.][NativeRegionMixProbe]") {
    const auto env=[](const char* key){const auto p=boost::nowide::getenv(key);return p?std::string(p):std::string{};};
    const auto input=env("ORCA_REGION_MIX_INPUT"),output=env("ORCA_REGION_MIX_OUTPUT");
    if(input.empty() || output.empty())SKIP("Explicit input and fresh output required.");
    REQUIRE_FALSE(boost::filesystem::exists(output));
    boost::filesystem::ifstream file(input);nlohmann::json cases;file>>cases;nlohmann::json results=nlohmann::json::array();
    for(const auto& item:cases) {
        const auto result=GUI::suggest_native_mixed_filament(item.at("target"),item.at("colors"),{},item.at("types"),item.value("current",std::string{}));
        nlohmann::json components=nlohmann::json::array();
        for(const auto& c:result.components)components.push_back({c.filament_index,c.ratio});
        results.push_back({{"sample",item.at("sample")},{"label",item.at("label")},{"target",item.at("target")},
            {"valid",result.valid},{"color",result.matched_color_hex},{"components",components}});
    }
    boost::filesystem::ofstream result(output);result<<results.dump(2);
}

TEST_CASE("Recognition distinguishes a completed empty result from a missing or failed attempt", "[BeautyWorkbench][BeautyGuidance]") {
    BeautyGuidance hints;CHECK_FALSE(hints.completed(3));
    hints.labels={-1,-1,-1};CHECK(hints.completed(3));CHECK_FALSE(hints.has_features());
    auto saved=hints.encode("geometry","source");
    CHECK(BeautyGuidance::decode(saved,"geometry","source",3).completed(3));
    saved["schema"]="orca.beauty-guidance/v1";saved.erase("names");
    CHECK(BeautyGuidance::decode(saved,"geometry","source",3).completed(3));
    const nlohmann::json failed={{"geometry","g"},{"source","s"},{"state","failed"},{"time",1000}};
    CHECK_FALSE(beauty_recognition_due(failed,"g","s",1100));
    CHECK(beauty_recognition_due(failed,"g","s",1300));
    CHECK(beauty_recognition_due(failed,"g","other",1100));
    CHECK(beauty_recognition_due(nlohmann::json{},"g","s",1100));
}

TEST_CASE("Only unchanged automatic partitions and exact filament assignments qualify for upgrade", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface=BeautySurface::build(its_make_cube(10,10,10),{});
    auto automatic=BeautyPuzzle::create(*surface,4);
    automatic.match_filaments(*surface,{{0,"#FFFFFF","PLA",true},{1,"#000000","PLA",true}});
    auto saved=automatic;saved.colors.clear();saved.filament_slots.clear();saved.target_colors.clear();
    for(auto& id:saved.face_piece)id+=100;
    for(const auto& c:automatic.colors)saved.colors[c.first+100]=c.second;
    for(const auto& s:automatic.filament_slots)saved.filament_slots[s.first+100]=s.second;
    CHECK(same_automatic_puzzle(saved,automatic));
    saved.filament_slots.begin()->second=1-saved.filament_slots.begin()->second;
    CHECK_FALSE(same_automatic_puzzle(saved,automatic));
    saved=automatic;saved.face_piece.front()=saved.next_id++;
    CHECK_FALSE(same_automatic_puzzle(saved,automatic));
}

TEST_CASE("Small semantic pieces match their own source colors instead of a surrounding patch average", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto mesh=its_make_cube(10,10,10);const auto surface=BeautySurface::build(mesh,{});
    BeautyPuzzle p;p.geometry_id=surface->geometry_id;p.next_id=3;p.face_piece.assign(mesh.indices.size(),1);
    p.face_piece.back()=2;std::vector<RGBA> colors(mesh.indices.size(),RGBA{1,1,1,1});colors.back()={0,0,0,1};
    p.match_filaments(*surface,{{0,"#FFFFFF","PLA",true},{1,"#000000","PLA",true}}, {},colors);
    CHECK(p.filament_slots.at(1)==0);CHECK(p.filament_slots.at(2)==1);
    p.paint_filament(2,0);p.match_filaments(*surface,p.palette,{},colors);
    CHECK(p.filament_slots.at(2)==0); // New evidence cannot overwrite deliberate paint.
}

TEST_CASE("Dark source colors retain lightness when a matching native mixture is available", "[BeautyWorkbench][BeautyPuzzle]") {
    BeautyPuzzle p;p.palette={{0,"#F7E2DA","PLA",true},{1,"#958B86","PLA",true},{2,"#EA9A92","PLA",true}};
    const MixedColorRecipe brown{"#684331",{{0,.3},{1,.7}},6,std::string(64,'a'),true};
    const auto source=BeautyPuzzle::filament_color({0,brown.target_color,{},true});
    CHECK(p.nearest_filament(source,{brown})==6);
    CHECK(p.nearest_filament(BeautyPuzzle::filament_color(p.palette[2]),{brown})==2);
    // A physical match wins a tie, preventing gratuitous mixed-slot changes.
    auto duplicate=brown;duplicate.target_color=p.palette[1].display_color;
    CHECK(p.nearest_filament(BeautyPuzzle::filament_color(p.palette[1]),{duplicate})==1);
    auto invalid=brown;invalid.components[0].slot=99;
    CHECK(p.nearest_filament(source,{invalid})==p.nearest_filament(source));
    CHECK(p.nearest_filament(source,{brown,brown})==p.nearest_filament(source));
    auto gradient=brown;gradient.uniform_color=false;
    CHECK(p.nearest_filament(source,{gradient})==p.nearest_filament(source));
}

TEST_CASE("Fresh automatic color uses existing mixtures while saved and manual slots remain unchanged", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface=BeautySurface::build(its_make_cube(10,10,10),{});
    const std::vector<PhysicalFilamentChannel> physical{{0,"#FFFFFF","PLA",true},{1,"#000000","PLA",true}};
    const MixedColorRecipe mix{"#808080",{{0,.4},{1,.6}},2,std::string(64,'a'),true};
    auto p=BeautyPuzzle::create(*surface,1);
    const std::vector<RGBA> source(surface->areas.size(),BeautyPuzzle::filament_color({0,mix.target_color,{},true}));
    p.match_filaments(*surface,physical,{mix},source);
    REQUIRE(p.mixed_recipes.size()==1);CHECK(same_native_mixed_recipe(p.mixed_recipes.front(),mix));
    for(const auto& slot:p.filament_slots)CHECK(slot.second==2);
    const auto saved=p.encode();auto restored=BeautyPuzzle::decode(saved,p.geometry_id,p.face_piece.size());
    restored.match_filaments(*surface,physical,{mix},source);CHECK(restored.same_edit(p));
    restored.paint_filament(restored.face_piece.front(),0);const auto manual=restored;
    restored.match_filaments(*surface,physical,{mix},source);CHECK(restored.same_edit(manual));
    restored=p;auto changed=mix;changed.native_settings_fingerprint=std::string(64,'b');
    restored.match_filaments(*surface,physical,{changed},source);
    for(const auto& slot:restored.filament_slots)CHECK(slot.second!=2);
    CHECK(restored.mixed_recipes.empty());
    auto bad=mix;bad.existing_virtual_slot=0;
    auto fresh=BeautyPuzzle::create(*surface,1);
    CHECK_NOTHROW(fresh.match_filaments(*surface,physical,{bad},source));
    CHECK(fresh.mixed_recipes.empty());
}

TEST_CASE("Automatic facial color keeps source contrast without changing saved paint or disconnected parts", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto mesh=its_make_cube(10,10,10);const auto surface=BeautySurface::build(mesh,{});
    BeautyPuzzle p;p.geometry_id=surface->geometry_id;p.next_id=3;p.face_piece.assign(mesh.indices.size(),1);p.face_piece[0]=2;
    BeautyGuidance g;g.names={"face","lb"};g.labels.assign(mesh.indices.size(),0);g.labels[0]=1;
    std::vector<RGBA> source(mesh.indices.size(),RGBA{.6f,.6f,.6f,1});source[0]={.4f,.4f,.4f,1};
    const std::vector<PhysicalFilamentChannel> palette{{0,"#808080","PLA",true},{1,"#404040","PLA",true}};
    SECTION("adjacent darker eyebrow stays darker with bounded color deviation") {
        const auto faces=p.face_piece;
        CHECK(beauty_match_feature_filaments(p,*surface,g,source,palette)==1);
        CHECK(p.filament_slots.at(1)==0);CHECK(p.filament_slots.at(2)==1);CHECK(p.face_piece==faces);
        auto saved=BeautyPuzzle::decode(p.encode(),p.geometry_id,p.face_piece.size());const auto before=saved;
        CHECK(beauty_match_feature_filaments(saved,*surface,g,source,palette)==0);CHECK(saved.same_edit(before));
    }
    SECTION("manual color is never adjusted") {
        p.match_filaments(*surface,palette,{},source);p.paint_filament(2,0);const auto before=p;
        CHECK(beauty_match_feature_filaments(p,*surface,g,source,palette)==0);CHECK(p.same_edit(before));
    }
    SECTION("no original contrast does not invent a dark eyebrow") {
        source[0]=source[1];CHECK(beauty_match_feature_filaments(p,*surface,g,source,palette)==0);
        CHECK(p.filament_slots.at(2)==p.filament_slots.at(1));
    }
    SECTION("already readable contrast is not washed out to fit an exact difference") {
        source[0]={.3f,.3f,.3f,1};
        CHECK(beauty_match_feature_filaments(p,*surface,g,source,palette)==0);CHECK(p.filament_slots.at(2)==1);
    }
    SECTION("an unlabeled contour rim does not hide an otherwise unambiguous feature") {
        p.face_piece[1]=2;source[1]=source[0];g.labels[1]=-1;
        CHECK(beauty_match_feature_filaments(p,*surface,g,source,palette)==1);CHECK(p.filament_slots.at(2)==1);
    }
    SECTION("a disconnected feature cannot borrow another face color") {
        for(auto& neighbors:surface->face_neighbors)for(auto& n:neighbors)if(n==0)n=-1;
        surface->face_neighbors[0]={-1,-1,-1};CHECK(beauty_match_feature_filaments(p,*surface,g,source,palette)==0);
        CHECK(p.filament_slots.at(2)==0);
    }
    SECTION("missing recognition retains ordinary matching") {
        g.labels.clear();CHECK(beauty_match_feature_filaments(p,*surface,g,source,palette)==0);CHECK(p.filament_slots.at(2)==0);
    }
    SECTION("invalid native slot collision cannot override a physical candidate") {
        const MixedColorRecipe bad{"#404040",{{0,.5},{1,.5}},1,std::string(64,'a'),true};
        CHECK_NOTHROW(beauty_match_feature_filaments(p,*surface,g,source,palette,{bad}));CHECK(p.mixed_recipes.empty());
    }
}

TEST_CASE("Facial contrast follows new automatic colour regions and keeps saved paint", "[BeautyColorRegions][BeautyPuzzle]") {
    BeautySurface surface;surface.geometry_id="split-feature";surface.areas.assign(11,1.);
    std::vector<RGBA> source(11,{1,1,1,1});
    source[0]={.6f,.6f,.6f,1};source[1]=source[2]={.4f,.4f,.4f,1};
    for(int f=0;f<11;++f) {
        surface.face_patch.push_back(uint32_t(f));surface.patches.emplace_back();
        for(size_t c=0;c<3;++c)surface.patches.back().mean_color[c]=source[size_t(f)][c];
        surface.centers.emplace_back(f,0,0);surface.normals.emplace_back(0,0,1);
        surface.face_neighbors.push_back({f>0?f-1:-1,f<10?f+1:-1,-1});
    }
    BeautyPuzzle puzzle;puzzle.geometry_id=surface.geometry_id;puzzle.face_piece.assign(11,1);
    puzzle.face_piece[0]=2;puzzle.next_id=3;
    BeautyGuidance guidance;guidance.names={"face","lb"};guidance.labels.assign(11,-1);
    guidance.labels[0]=0;guidance.labels[1]=guidance.labels[2]=1;
    const std::vector<PhysicalFilamentChannel> palette{{0,"#FFFFFF","PLA",true},{1,"#808080","PLA",true},{2,"#404040","PLA",true}};
    CHECK(beauty_match_feature_filaments(puzzle,surface,guidance,source,palette)==1);
    CHECK(puzzle.face_piece[1]!=1);
    CHECK(puzzle.filament_slots.at(puzzle.face_piece[0])==1);
    CHECK(puzzle.filament_slots.at(puzzle.face_piece[1])==2);
    CHECK(puzzle.filament_slots.at(puzzle.face_piece[10])==0);
    REQUIRE_NOTHROW(puzzle.validate(surface));
    auto saved=BeautyPuzzle::decode(puzzle.encode(),surface.geometry_id,source.size());
    saved.paint_filament(saved.face_piece[1],0);const auto manual=saved;
    CHECK(beauty_match_feature_filaments(saved,surface,guidance,source,palette)==0);
    CHECK(saved.same_edit(manual));
}

// Read-only experiment: rank native mixer colors for supplied local evidence.
// Proposals never create native slots or represent calibrated printed colors.
TEST_CASE("Explicit portrait evidence ranks existing native mixer combinations", "[.][NativePortraitMixProbe]") {
    const auto env=[](const char* key){const auto value=boost::nowide::getenv(key);return value?std::string(value):std::string();};
    const auto source=env("ORCA_PORTRAIT_GROUPS"),output=env("ORCA_PORTRAIT_MIX_OUTPUT");
    if(source.empty() || output.empty())SKIP("Explicit groups and new output required.");
    REQUIRE_FALSE(boost::filesystem::exists(output));
    boost::filesystem::ifstream input(source);nlohmann::json root;input>>root;
    const auto& saved=root.at("puzzle");
    auto puzzle=BeautyPuzzle::decode(saved,saved.at("geometry_id"),saved.at("face_count"));
    std::map<std::string,nlohmann::json> largest;
    for(const auto& group:root.at("groups")) {
        const std::string label=group.at("label");
        if(!largest.count(label) || group.at("area").get<double>()>largest[label].at("area").get<double>())largest[label]=group;
    }
    nlohmann::json results=nlohmann::json::array();
    for(const auto& entry:largest) {
        const auto target=entry.second.at("rgb").get<std::array<double,3>>();
        std::vector<std::pair<double,nlohmann::json>> ranked;
        auto add=[&](std::vector<size_t> slots,std::vector<int> weights) {
            std::vector<std::string> colors;for(size_t i:slots)colors.push_back(puzzle.palette[i].display_color);
            const auto hex=blend_color_multi(colors,weights);const auto rgb=BeautyPuzzle::filament_color({0,hex,{},true});
            const double error=tex2color::color_utils::calc_rgb_color_difference_by_ciede2000_srgb01(target,{rgb[0],rgb[1],rgb[2]});
            for(auto& i:slots)i=puzzle.palette[i].slot+1;
            ranked.push_back({error,{{"components_1based",slots},{"ratios_percent",weights},{"color",hex},{"delta_e",error}}});
        };
        for(size_t a=0;a<puzzle.palette.size();++a)for(size_t b=a+1;b<puzzle.palette.size();++b) {
            for(int x=10;x<=90;x+=10)add({a,b},{x,100-x});
            for(size_t c=b+1;c<puzzle.palette.size();++c)
                for(int x=10;x<=80;x+=10)for(int y=10;x+y<=90;y+=10)add({a,b,c},{x,y,100-x-y});
        }
        std::stable_sort(ranked.begin(),ranked.end(),[](const auto& a,const auto& b){return a.first<b.first;});
        nlohmann::json candidates=nlohmann::json::array();
        for(size_t i=0;i<std::min(size_t(5),ranked.size());++i)candidates.push_back(ranked[i].second);
        results.push_back({{"label",entry.first},{"source_rgb",target},{"candidates",candidates}});
    }
    boost::filesystem::ofstream result(output);result<<results.dump(2);CHECK_FALSE(results.empty());
}

TEST_CASE("Supplementing recognized features preserves paint outside the requested feature faces", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto mesh=its_make_cube(10,10,10);const auto surface=BeautySurface::build(mesh,{});
    auto p=BeautyPuzzle::create(*surface,1);p.match_filaments(*surface,{{0,"#FFFFFF","PLA",true},{1,"#000000","PLA",true}});
    p.paint_filament(p.face_piece.back(),1);
    BeautyGuidance g;g.labels.assign(mesh.indices.size(),-1);g.labels[0]=0;g.names={"llip"};
    auto next=beauty_supplement_features(p,*surface,g);
    for(size_t f=1;f<p.face_piece.size();++f)CHECK(next.colors.at(next.face_piece[f])==p.colors.at(p.face_piece[f]));
    CHECK_FALSE(next.colors.count(next.face_piece[0]));next.validate(*surface);
    CHECK(p.face_piece[0]!=next.face_piece[0]);
}

TEST_CASE("History beauty records win over stale adjacent drafts without capturing external models", "[BeautyWorkbench][BeautyMetadata]") {
    const auto root=boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("beauty-record-%%%%-%%%%");
    boost::filesystem::create_directories(root/"library"/"downloads");
    boost::filesystem::create_directory(root/"library"/"job");
    boost::filesystem::create_directory(root/"external");
    struct Cleanup {boost::filesystem::path path;~Cleanup(){boost::system::error_code ec;boost::filesystem::remove_all(path,ec);}} cleanup{root};
    const auto model=root/"library"/"job"/"orcaslicer-ai-finish-test.glb";
    boost::filesystem::ofstream(model)<<"model";
    auto adjacent=model;adjacent.replace_extension(".json");
    boost::filesystem::ofstream(adjacent)<<"stale draft";
    CHECK(beauty_metadata_path(model,root/"library")==adjacent);
    const auto saved=root/"library"/"downloads"/adjacent.filename();
    boost::filesystem::ofstream(saved)<<"saved workbench";
    CHECK(beauty_metadata_path(model,root/"library")==saved);
    const auto external=root/"external"/model.filename();
    boost::filesystem::ofstream(external)<<"another model";
    auto external_metadata=external;external_metadata.replace_extension(".json");
    CHECK(beauty_metadata_path(external,root/"library")==external_metadata);
}

TEST_CASE("Saved recognition aids restore without inference and reject foreign or invalid surfaces", "[BeautyWorkbench][BeautyGuidance]") {
    BeautyGuidance hints;hints.labels={-1,0,0,1,1,1};hints.details={{1,2}};
    hints.eyes.push_back({"face-1","le",{0,1,2},{1,2}});
    const auto saved=hints.encode("geometry","original");
    const auto restored=BeautyGuidance::decode(saved,"geometry","original",6);
    CHECK(restored.labels==hints.labels);CHECK(restored.details==hints.details);
    REQUIRE(restored.eyes.size()==1);CHECK(restored.eyes.front().iris_faces==hints.eyes.front().iris_faces);
    CHECK_THROWS(BeautyGuidance::decode(saved,"other","original",6));
    CHECK_THROWS(BeautyGuidance::decode(saved,"geometry","changed-texture",6));
    auto invalid=saved;invalid["labels"][0][1]=7;
    CHECK_THROWS(BeautyGuidance::decode(invalid,"geometry","original",6));
    invalid=saved;invalid["eyes"][0]["iris"]={4};
    CHECK_THROWS(BeautyGuidance::decode(invalid,"geometry","original",6));
}

TEST_CASE("Native mixed puzzle assignments survive reopen and decline changed recipes", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface=BeautySurface::build(its_make_cube(10,10,10),{});
    auto puzzle=BeautyPuzzle::create(*surface,4);
    const std::vector<PhysicalFilamentChannel> palette{{0,"#FFFFFF","PLA",true},{1,"#000000","PLA",true}};
    const MixedColorRecipe mix{"#808080",{{0,.4},{1,.6}},2,std::string(64,'a')};
    puzzle.match_filaments(*surface,palette,{mix});
    const auto id=puzzle.face_piece.front();const auto partition=puzzle.face_piece;
    puzzle.paint_mixed(id,mix);
    puzzle.target_colors.clear(); // Exercise the pre-target v3 format too.
    const auto saved=puzzle.encode();REQUIRE(saved["schema"]=="orca.beauty-puzzle/v3");
    auto restored=BeautyPuzzle::decode(saved,puzzle.geometry_id,partition.size());
    restored.match_filaments(*surface,palette,{mix});
    CHECK(restored.same_edit(puzzle));CHECK(restored.filament_slots.at(id)==2);
    const auto faces=restored.faces(id);
    if(faces.size()>1) {
        auto copy=restored;const auto split=copy.split(id,{faces.front()},*surface);
        copy.match_filaments(*surface,palette,{mix});CHECK(copy.filament_slots.at(split)==2);copy.validate(*surface);
    }
    auto changed=mix;changed.native_settings_fingerprint=std::string(64,'b');
    restored.match_filaments(*surface,palette,{changed});
    CHECK(restored.face_piece==partition);CHECK(restored.filament_slots.at(id)!=2);CHECK(restored.mixed_recipes.empty());
    auto invalid=saved;invalid["mixed_recipes"][0]["components"][0][0]=99;
    CHECK_THROWS(BeautyPuzzle::decode(invalid,puzzle.geometry_id,partition.size()));
    invalid=saved;invalid["mixed_recipes"][0]["slot"]=0;
    CHECK_THROWS(BeautyPuzzle::decode(invalid,puzzle.geometry_id,partition.size()));
}

TEST_CASE("Eye texture detail stays inside recognized eyes and declines flat or weak evidence", "[BeautyWorkbench][BeautyPuzzle]") {
    indexed_triangle_set mesh;std::vector<RGBA> colors;std::vector<size_t> eye;
    std::vector<size_t> expected;
    for(int y=0;y<8;++y)for(int x=0;x<10;++x)for(int side=0;side<2;++side) {
        const int first=int(mesh.vertices.size());const size_t f=mesh.indices.size();
        if(side==0) {
            mesh.vertices.emplace_back(float(x),float(y),0);
            mesh.vertices.emplace_back(float(x+1),float(y),0);
            mesh.vertices.emplace_back(float(x),float(y+1),0);
        } else {
            mesh.vertices.emplace_back(float(x+1),float(y+1),0);
            mesh.vertices.emplace_back(float(x),float(y+1),0);
            mesh.vertices.emplace_back(float(x+1),float(y),0);
        }
        mesh.indices.emplace_back(first,first+1,first+2);
        const bool inside=x>=2 && x<8 && y>=2 && y<6;
        const bool pupil=x>=4 && x<6 && y>=3 && y<5;
        const float intensity=inside && !pupil?.9f:.1f;
        for(int i=0;i<3;++i)colors.push_back({intensity,intensity,intensity,1});
        if(inside)eye.push_back(f);
        if(pupil)expected.push_back(f);
    }
    const auto surface=BeautySurface::build(mesh,colors);
    const auto selected=BeautyEyeDetail::dark_region(mesh,colors,*surface,eye);
    CHECK(selected==expected);
    // Black hair outside the eye cannot become a candidate.
    for(size_t f:selected)CHECK(std::find(eye.begin(),eye.end(),f)!=eye.end());
    auto flat=colors;for(auto& c:flat)c={.5f,.5f,.5f,1};
    CHECK(BeautyEyeDetail::dark_region(mesh,flat,*surface,eye).empty());
    auto weak=colors;for(auto& c:weak)if(c[0]<.5f)c={.85f,.85f,.85f,1};
    CHECK(BeautyEyeDetail::dark_region(mesh,weak,*surface,eye).empty());
    auto bad=eye;bad.push_back(mesh.indices.size());
    CHECK(BeautyEyeDetail::dark_region(mesh,colors,*surface,bad).empty());
}

TEST_CASE("Matched puzzle colors preserve separate regions and exact physical slots through edits and persistence", "[BeautyWorkbench][BeautyPuzzle]") {
    indexed_triangle_set mesh=its_make_cube(10,10,10);
    const auto surface=BeautySurface::build(mesh,{});
    auto puzzle=BeautyPuzzle::create(*surface,4);
    const auto partition=puzzle.face_piece;
    const std::vector<PhysicalFilamentChannel> palette {{0,"#FFFFFF","PLA",true},{2,"#222222","PLA",true},{4,"#222222","PLA",true}};
    puzzle.match_filaments(*surface,palette);
    REQUIRE(puzzle.face_piece==partition);
    REQUIRE(puzzle.filament_slots.size()==puzzle.piece_count());
    const uint32_t id=puzzle.face_piece.front();
    puzzle.paint_filament(id,4);
    puzzle.target_colors.clear(); // Exercise the pre-target v2 format too.
    const auto saved=puzzle.encode();
    REQUIRE(saved["schema"]=="orca.beauty-puzzle/v2");
    auto restored=BeautyPuzzle::decode(saved,puzzle.geometry_id,partition.size());
    REQUIRE(restored.same_edit(puzzle));
    restored.match_filaments(*surface,palette);
    CHECK(restored.filament_slots.at(id)==4);
    const auto selected=restored.faces(id);
    if(selected.size()>1) {
        const uint32_t created=restored.split(id,{selected.front()},*surface);
        CHECK(restored.filament_slots.at(created)==4);
        restored.validate(*surface);
    }
    auto invalid=saved;invalid["filament_slots"][0][1]=99;
    CHECK_THROWS(BeautyPuzzle::decode(invalid,puzzle.geometry_id,partition.size()));
    invalid=saved;invalid["palette"][0]["compatible"]=false;
    // Force this unavailable channel to be referenced, independent of matching.
    invalid["filament_slots"][0][1]=0;
    CHECK_THROWS(BeautyPuzzle::decode(invalid,puzzle.geometry_id,partition.size()));
}

TEST_CASE("A saved full color puzzle migrates to current filaments without changing the original surface", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto mesh=its_make_cube(10,10,10);
    const auto surface=BeautySurface::build(mesh,std::vector<std::array<float,4>>(mesh.vertices.size(),{1,1,1,1}));
    auto original=BeautyPuzzle::create(*surface,4);
    original.paint(original.face_piece.front(),{1,0,0,1});
    const auto old_record=original.encode();
    REQUIRE(old_record["schema"]=="orca.beauty-puzzle/v1");
    auto matched=BeautyPuzzle::decode(old_record,original.geometry_id,original.face_piece.size());
    matched.match_filaments(*surface,{{0,"#FFFFFF","PLA",true},{1,"#FF0000","PLA",true},{2,"#000000","PLA",false}});
    CHECK(matched.face_piece==original.face_piece);
    CHECK(matched.filament_slots.at(original.face_piece.front())==1);
    CHECK_FALSE(matched.same_edit(original));
    CHECK(original.encode()==old_record);
    for(const auto& item:matched.filament_slots)CHECK(item.second!=2);
    matched.restore_source_color(original.face_piece.front(),*surface);
    CHECK(matched.filament_slots.at(original.face_piece.front())==0);
}

namespace {
std::shared_ptr<BeautySurface> puzzle_grid(int width = 6, int height = 4) {
    indexed_triangle_set mesh;
    for (int y = 0; y <= height; ++y)
        for (int x = 0; x <= width; ++x) mesh.vertices.emplace_back(float(x), float(y), 0.f);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const int a = y * (width + 1) + x;
        mesh.indices.emplace_back(a, a + 1, a + width + 2);
        mesh.indices.emplace_back(a, a + width + 2, a + width + 1);
    }
    return BeautySurface::build(mesh, {});
}

BeautyPuzzle columns(const BeautySurface& surface, double boundary) {
    BeautyPuzzle result;
    result.geometry_id = surface.geometry_id;
    result.next_id = 3;
    for (const auto& center : surface.centers) result.face_piece.push_back(center.x() < boundary ? 1 : 2);
    return result;
}

template<class Predicate>
std::vector<size_t> matching(const BeautySurface& surface, Predicate predicate) {
    std::vector<size_t> result;
    for (size_t f = 0; f < surface.centers.size(); ++f) if (predicate(surface.centers[f])) result.push_back(f);
    return result;
}

const std::array<float, 4> blue {.1f, .2f, .8f, 1.f};
const std::array<float, 4> red {.8f, .2f, .1f, 1.f};

// A connected strip with distinct source-color halves is small enough to make
// the intended color boundary unambiguous, independently of triangulation.
BeautySurface color_strip() {
    BeautySurface surface;
    surface.geometry_id = "six-face-color-strip";
    for (size_t f = 0; f < 6; ++f) {
        surface.face_patch.push_back(uint32_t(f));
        surface.face_neighbors.push_back({f == 0 ? -1 : int32_t(f - 1), f == 5 ? -1 : int32_t(f + 1), -1});
        surface.centers.emplace_back(double(f), 0., 0.);
        surface.normals.emplace_back(0., 0., 1.);
        surface.areas.push_back(1.);
        BeautyPatch patch;
        patch.faces = {f};
        patch.area = 1.;
        patch.center = surface.centers.back();
        patch.normal = surface.normals.back();
        patch.mean_color = f < 3 ? std::array<double, 3>{.8, .2, .1} : std::array<double, 3>{.1, .2, .8};
        surface.patches.push_back(patch);
    }
    return surface;
}

size_t boundary_edges(const BeautyPuzzle& puzzle, const BeautySurface& surface) {
    size_t result = 0;
    for (size_t f = 0; f < puzzle.face_piece.size(); ++f)
        for (int32_t n : surface.face_neighbors[f])
            if (n >= 0 && size_t(n) > f && puzzle.face_piece[f] != puzzle.face_piece[n]) ++result;
    return result;
}
}

TEST_CASE("Candidate color guard rejects contrasting transfers without undoing independent uniform edits", "[BeautyColorGuard]") {
    auto surface=color_strip();
    // Two independent six-face chains: red/blue contrast and uniform gray.
    for(size_t f=6;f<12;++f) {
        surface.face_patch.push_back(uint32_t(f));surface.areas.push_back(1.);
        surface.centers.emplace_back(double(f),0.,0.);surface.normals.emplace_back(0.,0.,1.);
        surface.face_neighbors.push_back({f==6?-1:int32_t(f-1),f==11?-1:int32_t(f+1),-1});
        BeautyPatch patch;patch.faces={f};patch.area=1.;patch.mean_color={.5,.5,.5};
        patch.center=surface.centers.back();patch.normal=surface.normals.back();surface.patches.push_back(patch);
    }
    BeautyPuzzle before;before.geometry_id=surface.geometry_id;before.next_id=5;
    before.face_piece={1,1,1,2,2,2,3,3,3,4,4,4};
    auto proposed=before;proposed.face_piece[2]=2;proposed.face_piece[8]=4;
    REQUIRE_NOTHROW(before.validate(surface));REQUIRE_NOTHROW(proposed.validate(surface));
    CHECK(guard_color_transfers(before,proposed,surface)==1);
    CHECK(proposed.face_piece[2]==1);CHECK(proposed.face_piece[8]==4);
    REQUIRE_NOTHROW(proposed.validate(surface));
    CHECK(proposed.piece_count()==before.piece_count());
}

TEST_CASE("Candidate color guard restores a harmful transfer while retaining a beneficial one", "[BeautyColorGuard]") {
    auto surface=color_strip();
    // The center region sends to one neighbor and receives from the other.
    // A beneficial transfer remains; both regions must stay connected.
    BeautyPuzzle before;before.geometry_id=surface.geometry_id;before.next_id=4;
    before.face_piece={1,1,2,2,3,3};
    auto proposed=before;proposed.face_piece[1]=2;proposed.face_piece[3]=3;
    REQUIRE_NOTHROW(proposed.validate(surface));
    CHECK(guard_color_transfers(before,proposed,surface)==1);
    CHECK(proposed.face_piece[1]==1);CHECK(proposed.face_piece[3]==3);
    REQUIRE_NOTHROW(proposed.validate(surface));
}

TEST_CASE("Candidate color guard refuses to cut a donor bridge", "[BeautyColorGuard]") {
    auto surface=color_strip();
    surface.face_neighbors={{{1,2,3}},{{0,5,-1}},{{0,4,-1}},{{0,4,-1}},{{2,3,5}},{{1,4,-1}}};
    for(size_t f=0;f<6;++f) {
        surface.patches[f].mean_color=(f==2 || f==3)?std::array<double,3>{.1,.2,.8}:std::array<double,3>{.8,.2,.1};
        surface.areas[f]=(f==2 || f==3)?10.:1.;surface.patches[f].area=surface.areas[f];
    }
    BeautyPuzzle before;before.geometry_id=surface.geometry_id;before.next_id=3;
    before.face_piece={1,1,2,2,2,1};
    auto proposed=before;proposed.face_piece[0]=2;proposed.face_piece[4]=1;
    REQUIRE_NOTHROW(before.validate(surface));REQUIRE_NOTHROW(proposed.validate(surface));
    size_t unresolved=0;
    CHECK(guard_color_transfers(before,proposed,surface,&unresolved)==0);
    CHECK(unresolved==1);CHECK(proposed.face_piece[0]==2);
    REQUIRE_NOTHROW(proposed.validate(surface));
}

TEST_CASE("Joint palette candidate protects stationary faces without erasing automatic targets or sparse slots", "[BeautyColorGuard]") {
    auto surface=color_strip();surface.areas[2]=100.;surface.patches[2].area=100.;
    BeautyPuzzle before;before.geometry_id=surface.geometry_id;before.next_id=3;before.face_piece={1,1,1,2,2,2};
    const std::vector<PhysicalFilamentChannel> palette{{5,"#FF0000","PLA",true},{9,"#0000FF","PLA",true}};
    std::vector<RGBA> source(6,{1,0,0,1});for(size_t f=3;f<6;++f)source[f]={0,0,1,1};
    auto candidate=before;candidate.face_piece[2]=2;
    before.match_filaments(surface,palette,{},source);candidate.match_filaments(surface,palette,{},source);
    REQUIRE(candidate.filament_slots.at(2)==5);
    const auto target=candidate.target_colors;const auto partition=candidate.face_piece;
    const auto stats=constrain_stationary_palette(before,candidate,surface,source);
    CHECK(candidate.filament_slots.at(1)==5);CHECK(candidate.filament_slots.at(2)==9);
    CHECK(candidate.target_colors==target);CHECK(candidate.face_piece==partition);
    CHECK(stats.at("all_faces_infeasible_regions")==nlohmann::json::array({2}));
    CHECK(BeautyPuzzle::decode(candidate.encode(),candidate.geometry_id,6).same_edit(candidate));
}

TEST_CASE("Joint palette candidate rejects mismatched palettes without changing its input", "[BeautyColorGuard]") {
    const auto surface=color_strip();auto before=BeautyPuzzle::create(surface,2);
    const std::vector<RGBA> source(6,{1,0,0,1});before.match_filaments(surface,{{0,"#FF0000","PLA",true}}, {},source);
    auto candidate=before;candidate.match_filaments(surface,{{0,"#0000FF","PLA",true}}, {},source);
    const auto saved=candidate.encode();
    CHECK_THROWS(constrain_stationary_palette(before,candidate,surface,source));CHECK(candidate.encode()==saved);
}

TEST_CASE("Residual transfer rollback preserves colors and cancels transactionally", "[BeautyColorGuard]") {
    const auto surface=color_strip();
    BeautyPuzzle baseline;baseline.geometry_id=surface.geometry_id;baseline.next_id=3;baseline.face_piece={1,1,1,2,2,2};
    std::vector<RGBA> source(6,{1,0,0,1});for(size_t f=3;f<6;++f)source[f]={0,0,1,1};
    baseline.match_filaments(surface,{{5,"#FF0000","PLA",true},{9,"#0000FF","PLA",true}}, {},source);
    auto candidate=baseline;candidate.face_piece[2]=2;
    const auto saved=candidate.encode();
    CHECK_THROWS(restore_residual_transfers(baseline,candidate,surface,source,[]{return true;}));
    CHECK(candidate.encode()==saved);
    const auto stats=restore_residual_transfers(baseline,candidate,surface,source);
    CHECK(stats.at("restored_faces")==1);CHECK(candidate.face_piece==baseline.face_piece);
    CHECK(candidate.colors==baseline.colors);CHECK(candidate.filament_slots==baseline.filament_slots);
    CHECK(candidate.target_colors==baseline.target_colors);
}

TEST_CASE("Residual transfer rollback rejects a destination whose color has become worse", "[BeautyColorGuard]") {
    const auto surface=color_strip();
    BeautyPuzzle baseline;baseline.geometry_id=surface.geometry_id;baseline.next_id=3;baseline.face_piece={1,1,1,2,2,2};
    std::vector<RGBA> source(6,{1,0,0,1});for(size_t f=3;f<6;++f)source[f]={0,0,1,1};
    baseline.match_filaments(surface,{{5,"#FF0000","PLA",true},{9,"#0000FF","PLA",true}}, {},source);
    auto candidate=baseline;candidate.face_piece[2]=2;candidate.paint_filament(1,9);
    const auto saved=candidate.encode();
    const auto stats=restore_residual_transfers(baseline,candidate,surface,source);
    CHECK(stats.at("restored_faces")==0);CHECK(stats.at("rejected_destination_color")==1);
    CHECK(candidate.encode()==saved);
}

TEST_CASE("Candidate regularization cancellation preserves its input", "[BeautyColorGuard]") {
    const auto surface=puzzle_grid();auto puzzle=BeautyPuzzle::create(*surface,4);
    const auto saved=puzzle.encode();
    CHECK_THROWS(guarded_regularize(puzzle,*surface,[]{return true;}));
    CHECK(puzzle.encode()==saved);
}

TEST_CASE("Mouth texture separates coherent light detail and refuses weak or unrelated evidence", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface=puzzle_grid(12,8);BeautyGuidance g;g.names={"face","imouth"};
    g.labels.assign(surface->areas.size(),0);std::vector<RGBA> colors(surface->areas.size(),{.95f,.95f,.95f,1});
    std::vector<size_t> expected;
    for(size_t f=0;f<colors.size();++f) {
        const auto& p=surface->centers[f];
        if(p.x()>=2 && p.x()<10 && p.y()>=2 && p.y()<6) {
            g.labels[f]=1;colors[f]={.2f,.2f,.2f,1};
            if(p.x()>=3 && p.x()<9 && p.y()>=3 && p.y()<5) {colors[f]={.9f,.88f,.85f,1};expected.push_back(f);}
        }
    }
    const auto original=g;
    SECTION("only coherent source light area is separated and persistence retains it") {
        CHECK(beauty_refine_mouth_guidance(*surface,colors,g)==expected.size());
        for(size_t f=0;f<colors.size();++f)CHECK(g.labels[f]==(std::find(expected.begin(),expected.end(),f)!=expected.end()?2:original.labels[f]));
        auto restored=BeautyGuidance::decode(g.encode(surface->geometry_id,"source"),surface->geometry_id,"source",colors.size());
        CHECK(restored.labels==g.labels);CHECK(restored.names==g.names);
        CHECK(beauty_refine_mouth_guidance(*surface,colors,restored)==0);
    }
    SECTION("closed mouth and missing recognition do not infer a light region") {
        g.names[1]="ulip";CHECK(beauty_refine_mouth_guidance(*surface,colors,g)==0);
        g.labels.clear();CHECK(beauty_refine_mouth_guidance(*surface,colors,g)==0);
    }
    SECTION("dim, flat and saturated textures remain untouched") {
        for(const auto value:std::vector<RGBA>{{.4f,.4f,.4f,1},{.21f,.21f,.21f,1},{1.f,.65f,.2f,1}}) {
            auto variant=colors;for(size_t f:expected)variant[f]=value;
            auto next=original;CHECK(beauty_refine_mouth_guidance(*surface,variant,next)==0);CHECK(next.labels==original.labels);
        }
    }
    SECTION("isolated highlights do not form one fabricated region") {
        for(size_t f:expected)colors[f]={.2f,.2f,.2f,1};
        for(size_t f=0;f<colors.size();++f)if(g.labels[f]==1 && f%4==0)colors[f]={.95f,.95f,.95f,1};
        CHECK(beauty_refine_mouth_guidance(*surface,colors,g)==0);
    }
    SECTION("illumination and scale variation preserve the same visible split") {
        for(float gain:{.9f,1.f,1.05f}) {
            auto variant=colors;for(auto& c:variant)for(size_t k=0;k<3;++k)c[k]*=gain;
            auto next=original;auto scaled=*surface;for(auto& a:scaled.areas)a*=100.;
            CHECK(beauty_refine_mouth_guidance(scaled,variant,next)==expected.size());
            for(size_t f:expected)CHECK(next.labels[f]==2);
        }
    }
}

TEST_CASE("Puzzle initialization covers the unchanged surface with deterministic connected pieces", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface = puzzle_grid(18, 12);
    const auto original_patches = surface->face_patch;
    const auto original_neighbors = surface->face_neighbors;
    const auto a = BeautyPuzzle::create(*surface, 12);
    const auto b = BeautyPuzzle::create(*surface, 12);
    REQUIRE(a.face_piece == b.face_piece);
    REQUIRE(a.face_piece.size() == surface->face_patch.size());
    REQUIRE(a.piece_count() == 12);
    REQUIRE(a.colors.empty());
    REQUIRE_NOTHROW(a.validate(*surface));
    REQUIRE(surface->face_patch == original_patches);
    REQUIRE(surface->face_neighbors == original_neighbors);
    size_t covered = 0;
    for (uint32_t id = 1; id < a.next_id; ++id) covered += a.faces(id).size();
    REQUIRE(covered == surface->face_patch.size());
    REQUIRE_THROWS(BeautyPuzzle::create(*surface, 0));
    REQUIRE_THROWS(BeautyPuzzle::create(*surface, 12, [] { return true; }));
    size_t checkpoints = 0;
    REQUIRE_THROWS(BeautyPuzzle::create(*surface, 12, [&] { return ++checkpoints >= 5; }));
    REQUIRE(checkpoints >= 5);
}

TEST_CASE("Boundary drags ignore disconnected visible samples and keep the shared boundary connected", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface = color_strip();
    const bool outward = GENERATE(true, false);
    auto puzzle = columns(surface, 3.);
    puzzle.paint(1, red); puzzle.paint(2, blue);
    const auto selected = outward ? std::vector<size_t>{3, 5} : std::vector<size_t>{2, 0};
    puzzle.resize_boundary(1, selected, outward, surface);
    REQUIRE_NOTHROW(puzzle.validate(surface));
    CHECK(puzzle.face_piece == (outward ? std::vector<uint32_t>{1, 1, 1, 1, 2, 2}
                                      : std::vector<uint32_t>{1, 1, 2, 2, 2, 2}));
    CHECK(puzzle.colors.at(1) == red);
    CHECK(puzzle.colors.at(2) == blue);
    const auto saved = puzzle.encode();
    REQUIRE_THROWS(puzzle.resize_boundary(1, {outward ? size_t(5) : size_t(0)}, outward, surface));
    CHECK(puzzle.encode() == saved);
}

TEST_CASE("Saved colored regions lose staircase teeth without erasing small pieces or color intent", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface = puzzle_grid(40, 24);
    auto puzzle = columns(*surface, 20.);
    puzzle.next_id = 4;
    for (size_t f = 0; f < puzzle.face_piece.size(); ++f) {
        const auto& c = surface->centers[f];
        if (c.x() < 22. && int(c.y()) % 4 < 2) puzzle.face_piece[f] = 1;
        if (c.x() > 4. && c.x() < 6. && c.y() > 4. && c.y() < 5.) puzzle.face_piece[f] = 3;
    }
    puzzle.paint(1, blue); puzzle.paint(2, red); puzzle.paint(3, red);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    const auto small_eye = puzzle.faces(3);
    const auto old_colors = puzzle.colors;
    const auto old_patches = surface->face_patch;
    const auto old_centers = surface->centers;
    const size_t original_boundary = boundary_edges(puzzle, *surface);
    puzzle = BeautyPuzzle::decode(puzzle.encode(), surface->geometry_id, surface->face_patch.size());
    puzzle.smooth_boundaries(*surface);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    CHECK(boundary_edges(puzzle, *surface) < original_boundary);
    CHECK(puzzle.faces(3) == small_eye);
    CHECK(puzzle.piece_count() == 3);
    CHECK(puzzle.next_id == 4);
    CHECK(puzzle.colors == old_colors);
    CHECK(puzzle.face_piece.size() == surface->face_patch.size());
    CHECK(surface->face_patch == old_patches);
    CHECK(surface->centers == old_centers);
    const auto saved = puzzle.encode();
    auto foreign = *surface; foreign.geometry_id = "different-surface";
    REQUIRE_THROWS(puzzle.smooth_boundaries(foreign));
    CHECK(puzzle.encode() == saved);
}

TEST_CASE("Smoothing preserves saved colors and partitions when region identities become sparse", "[BeautyWorkbench][BeautyPuzzle]") {
    const bool sparse_ids = GENERATE(false, true);
    const auto surface = puzzle_grid(40, 24);
    auto puzzle = columns(*surface, 20.);
    puzzle.next_id = 4;
    for (size_t f = 0; f < puzzle.face_piece.size(); ++f) {
        const auto& c = surface->centers[f];
        if (c.x() < 22. && int(c.y()) % 4 < 2) puzzle.face_piece[f] = 1;
        if (c.x() > 4. && c.x() < 6. && c.y() > 4. && c.y() < 5.) puzzle.face_piece[f] = 3;
    }
    puzzle.paint(1, blue); puzzle.paint(2, red); puzzle.paint(3, red);
    puzzle.palette = {{7, "#FF0000", "PLA", true}};
    puzzle.paint_filament(2, 7);
    puzzle.target_colors[2] = {.25f, .5f, .75f, 1.f};
    const std::array<uint32_t, 4> renamed = sparse_ids
        ? std::array<uint32_t, 4>{0, 17, 4096, BeautyPuzzle::max_id - 1}
        : std::array<uint32_t, 4>{0, 2, 4, 6};
    auto restored = puzzle;
    restored.next_id = sparse_ids ? BeautyPuzzle::max_id : 7;
    for (auto& id : restored.face_piece) id = renamed[id];
    restored.colors.clear(); restored.target_colors.clear(); restored.filament_slots.clear();
    for (const auto& entry : puzzle.colors) restored.colors[renamed[entry.first]] = entry.second;
    for (const auto& entry : puzzle.target_colors) restored.target_colors[renamed[entry.first]] = entry.second;
    for (const auto& entry : puzzle.filament_slots) restored.filament_slots[renamed[entry.first]] = entry.second;
    restored = BeautyPuzzle::decode(restored.encode(), surface->geometry_id, surface->face_patch.size());
    const auto initial = puzzle.face_piece;
    const auto old_patches = surface->face_patch;
    const auto old_neighbors = surface->face_neighbors;
    puzzle.smooth_boundaries(*surface);
    restored.smooth_boundaries(*surface);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    REQUIRE_NOTHROW(restored.validate(*surface));
    REQUIRE(puzzle.face_piece != initial);
    std::vector<uint32_t> expected;
    for (uint32_t id : puzzle.face_piece) expected.push_back(renamed[id]);
    CHECK(restored.face_piece == expected);
    CHECK(restored.next_id == (sparse_ids ? BeautyPuzzle::max_id : 7));
    for (const auto& entry : puzzle.colors) CHECK(restored.colors.at(renamed[entry.first]) == entry.second);
    for (const auto& entry : puzzle.target_colors) CHECK(restored.target_colors.at(renamed[entry.first]) == entry.second);
    for (const auto& entry : puzzle.filament_slots) CHECK(restored.filament_slots.at(renamed[entry.first]) == entry.second);
    CHECK(surface->face_patch == old_patches);
    CHECK(surface->face_neighbors == old_neighbors);
}

TEST_CASE("Smoothing preserves narrow connecting necks and sparse saved identities", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface = puzzle_grid(30, 16);
    auto puzzle = columns(*surface, 15.);
    puzzle.next_id = BeautyPuzzle::max_id;
    for (size_t f = 0; f < puzzle.face_piece.size(); ++f) {
        const auto& c = surface->centers[f];
        const bool left = c.x() < 8.;
        const bool right = c.x() > 20.;
        const bool neck = c.y() > 7. && c.y() < 8.;
        puzzle.face_piece[f] = left || right || neck ? BeautyPuzzle::max_id - 1 : (c.y() < 8. ? 2 : 3);
    }
    puzzle.paint(BeautyPuzzle::max_id - 1, blue);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    const size_t original_boundary = boundary_edges(puzzle, *surface);
    puzzle.smooth_boundaries(*surface);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    CHECK(puzzle.piece_count() == 3);
    CHECK(puzzle.next_id == BeautyPuzzle::max_id);
    CHECK(puzzle.colors.at(BeautyPuzzle::max_id - 1) == blue);
    CHECK(boundary_edges(puzzle, *surface) <= original_boundary);
}

TEST_CASE("Physical boundary diffusion rounds long stairs beyond single triangle teeth", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface = puzzle_grid(160, 96);
    auto puzzle = columns(*surface, 80.);
    for (size_t f = 0; f < puzzle.face_piece.size(); ++f) {
        const auto& c = surface->centers[f];
        puzzle.face_piece[f] = c.x() < (int(c.y() / 12.) % 2 ? 86. : 74.) ? 1 : 2;
    }
    puzzle.paint(1, blue); puzzle.paint(2, red);
    const auto original = puzzle.face_piece;
    const size_t original_boundary = boundary_edges(puzzle, *surface);
    std::vector<int> depth(original.size(), -1);
    std::vector<size_t> queue;
    for (size_t f = 0; f < original.size(); ++f)
        for (int32_t n : surface->face_neighbors[f])
            if (n >= 0 && original[f] != original[n]) { depth[f] = 0; queue.push_back(f); break; }
    for (size_t at = 0; at < queue.size(); ++at)
        for (int32_t n : surface->face_neighbors[queue[at]])
            if (n >= 0 && depth[n] < 0) { depth[n] = depth[queue[at]] + 1; queue.push_back(size_t(n)); }
    puzzle.smooth_boundaries(*surface);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    CHECK(boundary_edges(puzzle, *surface) < original_boundary * .85);
    CHECK(puzzle.piece_count() == 2);
    CHECK(puzzle.colors.at(1) == blue);
    CHECK(puzzle.colors.at(2) == red);
    int deepest_change = 0;
    for (size_t f = 0; f < original.size(); ++f)
        if (puzzle.face_piece[f] != original[f]) deepest_change = std::max(deepest_change, depth[f]);
    CHECK(deepest_change >= 3);
}

TEST_CASE("Physical boundary smoothing is invariant to a uniform change of model units", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface = puzzle_grid(72, 48);
    auto puzzle = columns(*surface, 36.);
    for (size_t f = 0; f < puzzle.face_piece.size(); ++f) {
        const auto& c = surface->centers[f];
        puzzle.face_piece[f] = c.x() < (int(c.y() / 6.) % 2 ? 39. : 33.) ? 1 : 2;
    }
    auto scaled_surface = *surface;
    for (auto& center : scaled_surface.centers) center *= 1024.;
    for (auto& area : scaled_surface.areas) area *= 1024. * 1024.;
    for (auto& patch : scaled_surface.patches) { patch.center *= 1024.; patch.area *= 1024. * 1024.; }
    auto scaled = puzzle;
    puzzle.smooth_boundaries(*surface);
    scaled.smooth_boundaries(scaled_surface);
    CHECK(puzzle.face_piece == scaled.face_piece);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    REQUIRE_NOTHROW(scaled.validate(scaled_surface));
}

TEST_CASE("A rejected boundary fragment does not roll back useful smoothing on a distant shell", "[BeautyWorkbench][BeautyPuzzle]") {
    auto surface = *puzzle_grid(64, 48);
    const size_t first_count = surface.face_patch.size();
    BeautyPuzzle puzzle;
    puzzle.geometry_id = surface.geometry_id;
    puzzle.next_id = 5;
    for (const auto& c : surface.centers)
        puzzle.face_piece.push_back(c.x() < (int(c.y() / 8.) % 2 ? 36. : 28.) ? 1 : 2);
    // A separate, larger shell has a strong texture boundary. A gray notch
    // permits one diffusion transfer that lengthens its outline, while the
    // adjacent contrasting texture blocks the rest of that local proposal.
    // Its rejected energy must not cancel smoothing on the first shell.
    for (size_t f = 0; f < first_count; ++f) {
        surface.centers.push_back((surface.centers[f] * 1000. + Vec3d(200000., 0., 0.)).eval());
        surface.normals.push_back(Vec3d(0., 0., 1.));
        surface.areas.push_back(surface.areas[f] * 1000000.);
        auto neighbors = surface.face_neighbors[f];
        for (auto& n : neighbors) if (n >= 0) n += int32_t(first_count);
        surface.face_neighbors.push_back(neighbors);
        puzzle.face_piece.push_back(puzzle.face_piece[f] + 2);
    }
    surface.face_patch.resize(puzzle.face_piece.size());
    std::iota(surface.face_patch.begin(), surface.face_patch.end(), 0);
    surface.patches.assign(puzzle.face_piece.size(), {});
    for (size_t f = 0; f < puzzle.face_piece.size(); ++f) {
        auto& patch = surface.patches[f];
        patch.faces = {f}; patch.area = surface.areas[f]; patch.center = surface.centers[f];
        patch.normal = surface.normals[f];
        patch.mean_color = f < first_count ? std::array<double, 3>{.5, .5, .5} :
            puzzle.face_piece[f] == 3 ? std::array<double, 3>{1., 0., 0.} : std::array<double, 3>{0., 0., 1.};
    }
    const size_t bad_tip = first_count + 1990, neighbor = first_count + 1993;
    surface.patches[bad_tip].mean_color = surface.patches[neighbor].mean_color = {.5, .5, .5};
    puzzle.paint(1, blue); puzzle.paint(3, red);
    const auto original = puzzle.face_piece;
    const auto colors = puzzle.colors;
    puzzle.smooth_boundaries(surface);
    REQUIRE_NOTHROW(puzzle.validate(surface));
    CHECK(puzzle.face_piece[bad_tip] == original[bad_tip]);
    CHECK(puzzle.face_piece[neighbor] == original[neighbor]);
    CHECK(puzzle.piece_count() == 4);
    CHECK(puzzle.colors == colors);
    size_t useful_changes = 0;
    for (size_t f = 0; f < first_count; ++f) if (puzzle.face_piece[f] != original[f]) ++useful_changes;
    CHECK(useful_changes > 50);
}

TEST_CASE("A boundary reshape grows and shrinks one piece while ignoring distant visible samples", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface = puzzle_grid(40, 24);
    auto puzzle = columns(*surface, 20.);
    puzzle.paint(1, blue); puzzle.paint(2, red);
    auto added = matching(*surface, [](const Vec3d& c) {
        return c.x() > 20. && c.x() < 23. && c.y() > 2. && c.y() < 10.;
    });
    const auto remote = matching(*surface, [](const Vec3d& c) { return c.x() > 38. && c.y() > 22.; });
    added.insert(added.end(), remote.begin(), remote.end());
    const auto removed = matching(*surface, [](const Vec3d& c) {
        return c.x() > 17. && c.x() < 20. && c.y() > 14. && c.y() < 22.;
    });
    puzzle.reshape_region(1, added, removed, *surface);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    CHECK(puzzle.piece_count() == 2);
    CHECK(puzzle.colors.at(1) == blue);
    CHECK(puzzle.colors.at(2) == red);
    for (size_t f : remote) CHECK(puzzle.face_piece[f] == 2);
    for (size_t f : matching(*surface, [](const Vec3d& c) {
             return c.x() > 20. && c.x() < 22. && c.y() > 4. && c.y() < 8.;
         })) CHECK(puzzle.face_piece[f] == 1);
    for (size_t f : matching(*surface, [](const Vec3d& c) {
             return c.x() > 18. && c.x() < 20. && c.y() > 16. && c.y() < 20.;
         })) CHECK(puzzle.face_piece[f] == 2);
    const auto saved = puzzle.encode();
    puzzle.reshape_region(1, remote, {}, *surface);
    CHECK(puzzle.encode() == saved);
    puzzle.reshape_region(1, {}, {}, *surface);
    CHECK(puzzle.encode() == saved);
    REQUIRE_THROWS(puzzle.reshape_region(1, {}, {surface->face_patch.size()}, *surface));
    CHECK(puzzle.encode() == saved);
}

TEST_CASE("Boundary reshaping closes single missing samples without flooding unsampled areas", "[BeautyWorkbench][BeautyPuzzle]") {
    auto surface = color_strip();
    auto puzzle = columns(surface, 1.);
    puzzle.paint(1, blue); puzzle.paint(2, red);
    // Faces 1 and 3 are stroke samples separated by exactly one missed face.
    puzzle.reshape_region(1, {1, 3}, {}, surface);
    REQUIRE_NOTHROW(puzzle.validate(surface));
    CHECK(puzzle.face_piece == (std::vector<uint32_t>{1, 1, 1, 1, 2, 2}));
    CHECK(puzzle.colors.at(1) == blue);
    CHECK(puzzle.colors.at(2) == red);
}

TEST_CASE("Recognized boundaries guide local fairing without blocking an explicit correction", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface=puzzle_grid(40,24);
    const auto original=columns(*surface,20.);
    const auto added=matching(*surface,[](const Vec3d& p){return p.x()>=20. && p.x()<23. && p.y()>5. && p.y()<15.;});
    std::vector<int32_t> labels;
    for(uint32_t id:original.face_piece)labels.push_back(int32_t(id));
    auto guided=original;
    guided.reshape_region(1,added,{},*surface,labels);
    REQUIRE_NOTHROW(guided.validate(*surface));
    // The face parser calls the entire new area a different part. A deliberate
    // center movement must still win, so recognition never locks a wrong mask.
    for(size_t f:matching(*surface,[](const Vec3d& p){return p.x()>20. && p.x()<22. && p.y()>8. && p.y()<12.;}))CHECK(guided.face_piece[f]==1);
    auto unknown=original,plain=original;
    plain.reshape_region(1,added,{},*surface);
    unknown.reshape_region(1,added,{},*surface,std::vector<int32_t>(labels.size(),-1));
    CHECK(unknown.same_edit(plain));
    CHECK(guided.face_piece!=plain.face_piece);
    const auto saved=guided.encode();
    CHECK_THROWS(guided.reshape_region(1,added,{},*surface,{0}));
    CHECK(guided.encode()==saved);
    for(size_t f:matching(*surface,[](const Vec3d& p){return p.y()>20.;}))CHECK(guided.face_piece[f]==original.face_piece[f]);
}

TEST_CASE("A reshape keeps donor island colors and does not smooth unrelated boundaries", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface = puzzle_grid(40, 24);
    auto puzzle = columns(*surface, 3.);
    puzzle.paint(1, blue); puzzle.paint(2, red);
    const auto band = matching(*surface, [](const Vec3d& c) { return c.y() > 10. && c.y() < 13.; });
    const auto before = puzzle.face_piece;
    puzzle.reshape_region(1, band, {}, *surface);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    CHECK(puzzle.piece_count() == 3);
    CHECK(puzzle.colors.at(1) == blue);
    CHECK(puzzle.colors.at(2) == red);
    CHECK(puzzle.colors.at(3) == red);
    for (size_t f = 0; f < puzzle.face_piece.size(); ++f) {
        const auto& c = surface->centers[f];
        if (c.y() > 11. && c.y() < 12.) CHECK(puzzle.face_piece[f] == 1);
        if (c.y() < 6.) CHECK(puzzle.colors.at(puzzle.face_piece[f]) == (before[f] == 1 ? blue : red));
    }
}

TEST_CASE("Dragging a boundary fills tiny new remnants without swallowing existing small regions", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface = puzzle_grid(80, 40);
    const bool outward = GENERATE(true, false);
    const double boundary = outward ? 20. : 60.;
    auto puzzle = columns(*surface, boundary);
    puzzle.next_id = 4;
    const double start = outward ? 20. : 54.;
    for (size_t f = 0; f < puzzle.face_piece.size(); ++f) {
        const auto& c = surface->centers[f];
        if (c.x() > start + 2. && c.x() < start + 4. && c.y() > 13. && c.y() < 15.) puzzle.face_piece[f] = 3;
    }
    puzzle.paint(1, blue); puzzle.paint(2, red); puzzle.paint(3, red);
    const auto existing_small_piece = puzzle.faces(3);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    std::vector<size_t> stroke, missed;
    for (size_t f = 0; f < puzzle.face_piece.size(); ++f) {
        const auto& c = surface->centers[f];
        if (c.x() <= start || c.x() >= start + 6. || c.y() <= 8. || c.y() >= 16.) continue;
        if (puzzle.face_piece[f] == 3) continue;
        if (c.x() > start + 2. && c.x() < start + 4. && c.y() > 10. && c.y() < 12.) missed.push_back(f);
        else stroke.push_back(f);
    }
    if (outward) puzzle.reshape_region(1, stroke, {}, *surface);
    else puzzle.reshape_region(1, {}, stroke, *surface);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    CHECK(puzzle.piece_count() == 3);
    CHECK(puzzle.next_id == 4);
    // Growing piece 1 must not sample or consume the untouched piece 3.
    // When shrinking piece 1, however, its explicitly removed faces touch
    // both 2 and 3; each is a valid geodesic recipient. Protect the original
    // small piece without incorrectly freezing its receiving boundary.
    if (outward) CHECK(puzzle.faces(3) == existing_small_piece);
    for (size_t f : existing_small_piece) CHECK(puzzle.face_piece[f] == 3);
    CHECK(puzzle.colors.at(1) == blue);
    CHECK(puzzle.colors.at(2) == red);
    CHECK(puzzle.colors.at(3) == red);
    const std::set<uint32_t> shrink_recipients {2, 3};
    for (size_t f : missed) {
        if (outward) CHECK(puzzle.face_piece[f] == 1);
        else CHECK(shrink_recipients.count(puzzle.face_piece[f]) == 1);
    }
}

TEST_CASE("Puzzle aggregation follows adjacent color regions and preserves disconnected shells", "[BeautyWorkbench][BeautyPuzzle]") {
    auto surface = color_strip();
    auto puzzle = BeautyPuzzle::create(surface, 2);
    REQUIRE_NOTHROW(puzzle.validate(surface));
    CHECK(puzzle.face_piece[0] == puzzle.face_piece[2]);
    CHECK(puzzle.face_piece[3] == puzzle.face_piece[5]);
    CHECK(puzzle.face_piece[2] != puzzle.face_piece[3]);
    surface.face_neighbors[2][1] = -1;
    surface.face_neighbors[3][0] = -1;
    puzzle = BeautyPuzzle::create(surface, 1);
    CHECK(puzzle.piece_count() == 2);
    REQUIRE_NOTHROW(puzzle.validate(surface));
    // Even a saved micro-patch label crossing disconnected shells is separated.
    surface.face_patch.assign(6, 0);
    puzzle = BeautyPuzzle::create(surface, 1);
    CHECK(puzzle.piece_count() == 2);
    REQUIRE_NOTHROW(puzzle.validate(surface));
}

TEST_CASE("Growing a puzzle piece transfers a shared boundary and preserves donor island colors", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface = puzzle_grid();
    auto puzzle = columns(*surface, 1.);
    puzzle.paint(1, blue);
    puzzle.paint(2, red);
    const auto band = matching(*surface, [](const Vec3d& c) { return c.y() > 1. && c.y() < 2.; });
    puzzle.grow(1, band, *surface);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    CHECK(puzzle.piece_count() == 3);
    for (size_t f = 0; f < puzzle.face_piece.size(); ++f) {
        const auto& c = surface->centers[f];
        if (c.x() < 1. || (c.y() > 1. && c.y() < 2.)) CHECK(puzzle.face_piece[f] == 1);
        else if (c.y() > 2.) CHECK(puzzle.face_piece[f] == 2);
        else CHECK(puzzle.face_piece[f] == 3);
    }
    CHECK(puzzle.colors.at(1) == blue);
    CHECK(puzzle.colors.at(2) == red);
    CHECK(puzzle.colors.at(3) == red);

    auto unchanged = columns(*surface, 1.);
    const auto before = unchanged.encode();
    const auto remote = matching(*surface, [](const Vec3d& c) { return c.x() > 4. && c.y() > 2.; });
    REQUIRE_THROWS(unchanged.grow(1, remote, *surface));
    CHECK(unchanged.encode() == before);
    REQUIRE_THROWS(unchanged.grow(1, {surface->face_patch.size()}, *surface));
    CHECK(unchanged.encode() == before);
}

TEST_CASE("Shrinking a puzzle piece hands faces to adjacent pieces and rejects enclosed holes atomically", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface = puzzle_grid();
    auto puzzle = columns(*surface, 3.);
    puzzle.paint(1, blue);
    puzzle.paint(2, red);
    const auto enclosed = matching(*surface, [](const Vec3d& c) { return c.x() > 1. && c.x() < 2. && c.y() > 1. && c.y() < 2.; });
    const auto before = puzzle.encode();
    REQUIRE_THROWS(puzzle.shrink(1, enclosed, *surface));
    CHECK(puzzle.encode() == before);

    const auto border = matching(*surface, [](const Vec3d& c) { return c.x() > 2. && c.x() < 3.; });
    puzzle.shrink(1, border, *surface);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    for (size_t f = 0; f < puzzle.face_piece.size(); ++f)
        CHECK(puzzle.face_piece[f] == (surface->centers[f].x() < 2. ? 1 : 2));
    CHECK(puzzle.colors.at(1) == blue);
    CHECK(puzzle.colors.at(2) == red);
    CHECK(puzzle.piece_count() == 2);
    puzzle.shrink(1, puzzle.faces(1), *surface);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    CHECK(puzzle.piece_count() == 1);
    CHECK(puzzle.colors.count(1) == 0);
    CHECK(puzzle.colors.at(2) == red);
}

TEST_CASE("Splitting and merging puzzle pieces preserve connected IDs and explicit color intent", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface = puzzle_grid();
    auto puzzle = BeautyPuzzle::create(*surface, 1);
    puzzle.paint(1, red);
    const auto band = matching(*surface, [](const Vec3d& c) { return c.x() > 2. && c.x() < 3.; });
    const uint32_t created = puzzle.split(1, band, *surface);
    REQUIRE(created == 2);
    REQUIRE(puzzle.piece_count() == 3);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    for (uint32_t id = 1; id < puzzle.next_id; ++id) CHECK(puzzle.colors.at(id) == red);
    const auto before = puzzle.encode();
    REQUIRE_THROWS(puzzle.merge(1, 3, *surface));
    CHECK(puzzle.encode() == before);
    puzzle.paint(created, blue);
    puzzle.merge(created, 1, *surface);
    CHECK(puzzle.faces(1).empty());
    CHECK(puzzle.colors.count(1) == 0);
    CHECK(puzzle.colors.at(created) == blue);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    puzzle.clear_color(3);
    puzzle.merge(3, created, *surface);
    CHECK(puzzle.colors.empty());
    CHECK(puzzle.piece_count() == 1);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    REQUIRE_THROWS(puzzle.split(3, puzzle.faces(3), *surface));
}

TEST_CASE("Painting a cross-color edit group changes only its selected faces", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface = puzzle_grid();
    auto puzzle = columns(*surface, 3.);
    puzzle.palette = {{0, "#FF0000", "PLA", true}, {1, "#0000FF", "PLA", true}, {2, "#00FF00", "PLA", true}};
    puzzle.paint_filament(1, 0);
    puzzle.paint_filament(2, 1);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    const auto before = puzzle;
    const auto selected = matching(*surface, [](const Vec3d& c) { return c.y() >= 1. && c.y() < 3.; });
    std::vector<uint8_t> mask(puzzle.face_piece.size(), 0);
    for (size_t f : selected) mask[f] = 1;

    puzzle.paint_faces_filament(selected, *surface, 2);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    std::set<uint32_t> painted_ids;
    for (size_t f : selected) painted_ids.insert(puzzle.face_piece[f]);
    CHECK(painted_ids.size() == 1);
    for (size_t f = 0; f < puzzle.face_piece.size(); ++f) {
        const auto slot = puzzle.filament_slots.at(puzzle.face_piece[f]);
        CHECK(slot == (mask[f] ? 2 : before.filament_slots.at(before.face_piece[f])));
        if (!mask[f]) CHECK(puzzle.colors.at(puzzle.face_piece[f]) == before.colors.at(before.face_piece[f]));
    }
    const auto reopened = BeautyPuzzle::decode(puzzle.encode(), puzzle.geometry_id, puzzle.face_piece.size());
    REQUIRE_NOTHROW(reopened.validate(*surface));
    CHECK(reopened.same_edit(puzzle));
    const auto painted = puzzle.encode();
    REQUIRE_THROWS(puzzle.paint_faces_filament({}, *surface, 2));
    REQUIRE_THROWS(puzzle.paint_faces_filament({surface->face_patch.size()}, *surface, 2));
    REQUIRE_THROWS(puzzle.paint_faces_filament(selected, *surface, 99));
    CHECK(puzzle.encode() == painted);
    puzzle = before; // The workbench stores the pre-edit puzzle as one undo snapshot.
    CHECK(puzzle.same_edit(before));
}

TEST_CASE("Puzzle persistence rejects invalid identity coverage capacity and color records", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface = puzzle_grid();
    auto puzzle = columns(*surface, 3.);
    puzzle.paint(2, red);
    const auto json = puzzle.encode();
    const auto decode = [&](const nlohmann::json& value) { return BeautyPuzzle::decode(value, surface->geometry_id, surface->face_patch.size()); };
    const auto loaded = decode(json);
    CHECK(loaded.face_piece == puzzle.face_piece);
    CHECK(loaded.colors == puzzle.colors);
    CHECK(loaded.next_id == puzzle.next_id);
    REQUIRE_NOTHROW(loaded.validate(*surface));
    REQUIRE_THROWS(BeautyPuzzle::decode(json, "unrelated", surface->face_patch.size()));
    REQUIRE_THROWS(BeautyPuzzle::decode(json, surface->geometry_id, surface->face_patch.size() - 1));
    REQUIRE_THROWS(BeautyPuzzle::decode(json, surface->geometry_id, BeautyPuzzle::max_faces + 1));

    auto corrupt = json;
    corrupt["piece_runs"][0][1] = surface->face_patch.size() + 1;
    REQUIRE_THROWS(decode(corrupt));
    corrupt = json; corrupt["piece_runs"] = nlohmann::json::array({{1, 1}});
    REQUIRE_THROWS(decode(corrupt));
    corrupt = json; corrupt["piece_runs"][0][0] = 0;
    REQUIRE_THROWS(decode(corrupt));
    corrupt = json; corrupt["piece_runs"][0][0] = 1.5;
    REQUIRE_THROWS(decode(corrupt));
    corrupt = json; corrupt["piece_runs"][0][0] = -1;
    REQUIRE_THROWS(decode(corrupt));
    corrupt = json; corrupt["next_id"] = 2;
    REQUIRE_THROWS(decode(corrupt));
    corrupt = json; corrupt["next_id"] = std::numeric_limits<uint64_t>::max();
    REQUIRE_THROWS(decode(corrupt));
    corrupt = json; corrupt["colors"][0]["id"] = 42;
    REQUIRE_THROWS(decode(corrupt));
    corrupt = json; corrupt["colors"].push_back(corrupt["colors"][0]);
    REQUIRE_THROWS(decode(corrupt));
    corrupt = json; corrupt["colors"][0]["rgba"][0] = 1.2;
    REQUIRE_THROWS(decode(corrupt));
    corrupt = json; corrupt["colors"][0]["rgba"][0] = std::numeric_limits<double>::quiet_NaN();
    REQUIRE_THROWS(decode(corrupt));
    corrupt = json; corrupt["colors"][0]["rgba"] = {0, 1, 0};
    REQUIRE_THROWS(decode(corrupt));

    auto disconnected = loaded;
    const auto far_corner = matching(*surface, [](const Vec3d& c) { return c.x() > 5. && c.y() > 3.; });
    for (size_t f : far_corner) disconnected.face_piece[f] = 1;
    const auto structurally_valid = decode(disconnected.encode());
    REQUIRE_THROWS(structurally_valid.validate(*surface));
    REQUIRE_THROWS(puzzle.paint(1, {1.f, 0.f, std::numeric_limits<float>::infinity(), 1.f}));
    REQUIRE_THROWS(puzzle.paint(999, blue));
    CHECK(puzzle.encode() == json);
}

TEST_CASE("Unpainted puzzle colors use surface area and painted colors remain exact", "[BeautyWorkbench][BeautyPuzzle]") {
    auto surface = color_strip();
    surface.areas[0] = 4.;
    const auto puzzle = BeautyPuzzle::create(surface, 1);
    const auto color = puzzle.representative_color(1, surface);
    CHECK(puzzle.representative_color_on_validated_surface(1, surface) == color);
    // Red half has area 6; blue half has area 3.
    CHECK_THAT(color[0], Catch::Matchers::WithinAbs((.8 * 6 + .1 * 3) / 9., 1e-6));
    CHECK_THAT(color[2], Catch::Matchers::WithinAbs((.1 * 6 + .8 * 3) / 9., 1e-6));
    auto edited = puzzle;
    edited.paint(1, blue);
    CHECK(edited.representative_color(1, surface) == blue);
    CHECK(edited.representative_color_on_validated_surface(1, surface) == blue);
    edited.clear_color(1);
    CHECK(edited.representative_color(1, surface) == color);
    CHECK(edited.representative_color_on_validated_surface(1, surface) == color);
    auto wrong_surface = surface;
    wrong_surface.geometry_id = "other";
    REQUIRE_THROWS(edited.representative_color(1, wrong_surface));
    REQUIRE_THROWS(edited.representative_color_on_validated_surface(1, wrong_surface));
    wrong_surface = surface;
    wrong_surface.face_neighbors[0][1] = -1;
    REQUIRE_THROWS(BeautyPuzzle::create(wrong_surface, 2));
}

TEST_CASE("Coarse editing regions cover the surface with fewer connected areas and unchanged source data", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface = puzzle_grid(40, 30);
    const auto original_partition = surface->face_patch;
    const auto original_centers = surface->centers;
    const auto detailed = BeautyPuzzle::create(*surface);
    const auto coarse = BeautyPuzzle::create_regions(*surface);
    REQUIRE_NOTHROW(coarse.validate(*surface));
    CHECK(coarse.face_piece.size() == surface->face_patch.size());
    CHECK(coarse.piece_count() <= 35);
    CHECK(coarse.piece_count() > 1);
    CHECK(coarse.piece_count() < detailed.piece_count());
    CHECK(coarse.colors.empty());
    CHECK(coarse.face_piece == BeautyPuzzle::create_regions(*surface, std::vector<int32_t>(surface->face_patch.size(), -1)).face_piece);
    CHECK(surface->face_patch == original_partition);
    CHECK(surface->centers == original_centers);
    REQUIRE_THROWS(BeautyPuzzle::create_regions(*surface, {1}));
    auto invalid = std::vector<int32_t>(surface->face_patch.size(), -1); invalid.back() = -2;
    REQUIRE_THROWS(BeautyPuzzle::create_regions(*surface, invalid));
    REQUIRE_THROWS(BeautyPuzzle::create_regions(*surface, {}, [] { return true; }));
}

TEST_CASE("Boundary regularization changes the real partition and removes uniform-color staircase edges", "[BeautyWorkbench][BeautyPuzzle]") {
    auto surface = puzzle_grid(56, 20);
    surface->patches.assign(28, {});
    for (size_t f = 0; f < surface->face_patch.size(); ++f) {
        const auto& c = surface->centers[f];
        const int shift = int(c.y()) % 4 < 2 ? 0 : 1;
        const uint32_t id = uint32_t(std::min(27, (int(c.x()) + shift) / 2));
        surface->face_patch[f] = id;
        auto& patch = surface->patches[id];
        patch.faces.push_back(f); patch.area += surface->areas[f];
        patch.center += surface->centers[f] * surface->areas[f];
        patch.normal = Vec3d(0., 0., 1.);
        patch.mean_color = {.5, .5, .5};
    }
    for (auto& patch : surface->patches) patch.center /= patch.area;
    const auto jagged = BeautyPuzzle::create(*surface, 28);
    const auto smooth = BeautyPuzzle::create_regions(*surface);
    REQUIRE_NOTHROW(smooth.validate(*surface));
    CHECK(smooth.face_piece != jagged.face_piece);
    CHECK(boundary_edges(smooth, *surface) < boundary_edges(jagged, *surface));
    CHECK(smooth.piece_count() <= jagged.piece_count());
    auto guarded=jagged;
    CHECK(guarded_regularize(guarded,*surface)==0);
    (guarded.*stage_member(SmoothStage{}))(*surface,{},{},{},nullptr,nullptr,nullptr);
    CHECK(guarded.face_piece==smooth.face_piece);
    CHECK(boundary_edges(guarded,*surface)<boundary_edges(jagged,*surface));
}

TEST_CASE("Sparse semantic hints preserve separate regions and cannot flood an unrelated coarse block", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface = puzzle_grid(40, 30);
    const auto coarse = BeautyPuzzle::create_regions(*surface);
    uint32_t largest = 1;
    for (uint32_t id = 2; id < coarse.next_id; ++id)
        if (coarse.faces(id).size() > coarse.faces(largest).size()) largest = id;
    const auto region = coarse.faces(largest);
    REQUIRE(region.size() > 10);
    const auto first = *std::min_element(region.begin(), region.end(), [&](size_t a, size_t b) {
        return surface->centers[a].x() < surface->centers[b].x();
    });
    const auto second = *std::max_element(region.begin(), region.end(), [&](size_t a, size_t b) {
        return (surface->centers[a] - surface->centers[first]).squaredNorm() <
               (surface->centers[b] - surface->centers[first]).squaredNorm();
    });
    std::vector<int32_t> labels(surface->face_patch.size(), -1);
    labels[first] = 4; labels[second] = 9;
    const auto result = BeautyPuzzle::create_regions(*surface, labels);
    REQUIRE_NOTHROW(result.validate(*surface));
    const auto eye = result.face_piece[first], nose = result.face_piece[second];
    CHECK(eye != nose);
    CHECK(result.faces(eye).size() > 1);
    CHECK(result.faces(eye).size() < region.size());
    for (size_t f : result.faces(eye)) CHECK(coarse.face_piece[f] == largest);
    for (size_t f : result.faces(nose)) CHECK(coarse.face_piece[f] == largest);
    CHECK(result.colors.empty());
}

TEST_CASE("One semantic part preserves substantial contrasting source pigments", "[BeautyWorkbench][BeautyPuzzle]") {
    auto surface=color_strip();std::vector<int32_t> labels(surface.areas.size(),0);
    const auto result=BeautyPuzzle::create_regions(surface,labels);
    CHECK(result.face_piece.front()!=result.face_piece.back());
    for(size_t f=0;f<3;++f)CHECK(result.face_piece[f]==result.face_piece[0]);
    for(size_t f=3;f<6;++f)CHECK(result.face_piece[f]==result.face_piece[3]);
    REQUIRE_NOTHROW(result.validate(surface));
    for(auto& patch:surface.patches)patch.mean_color={.5,.4,.3};
    const auto uniform=BeautyPuzzle::create_regions(surface,labels);
    CHECK(uniform.piece_count()==1);
}

TEST_CASE("Unknown feature growth spends less reach across a fold and preserves reliable seeds", "[BeautyWorkbench][BeautyPuzzle]") {
    BeautySurface flat;flat.geometry_id="fold-growth";flat.patches.resize(1);
    auto& patch=flat.patches.front();patch.mean_color={.5,.5,.5};patch.normal=Vec3d(0,0,1);
    for(size_t f=0;f<100;++f) {
        flat.face_patch.push_back(0);flat.areas.push_back(.01);patch.area+=.01;patch.faces.push_back(f);
        flat.centers.emplace_back(.1*double(f),0,0);flat.normals.emplace_back(0,0,1);
        flat.face_neighbors.push_back({f?int32_t(f-1):-1,f<99?int32_t(f+1):-1,-1});
    }
    auto folded=flat;
    for(size_t f=50;f<100;++f) {
        const double x=.1*double(f-50);
        folded.centers[f]=Vec3d(5.+.5*x,0.,std::sqrt(.75)*x);
        folded.normals[f]=Vec3d(-std::sqrt(.75),0.,.5);
    }
    std::vector<int32_t> labels(100,-1);for(size_t f=40;f<49;++f)labels[f]=0;
    const auto plain=BeautyPuzzle::create_regions(flat,labels,{}, {"nose"}), bent=BeautyPuzzle::create_regions(folded,labels,{}, {"nose"});
    auto gentle=flat;
    for(size_t f=0;f<100;++f)gentle.normals[f]=Vec3d(-std::sin(.01*f),0.,std::cos(.01*f));
    CHECK(BeautyPuzzle::create_regions(gentle,labels,{}, {"nose"}).face_piece==plain.face_piece);
    const auto legacy=BeautyPuzzle::create_regions(folded,labels);
    for(const auto& name:{"hair","face","neck","clothing"})
        CHECK(BeautyPuzzle::create_regions(folded,labels,{}, {name}).face_piece==legacy.face_piece);
    const auto extent=[](const BeautyPuzzle& p){size_t last=40;for(size_t f=40;f<100;++f)if(p.face_piece[f]==p.face_piece[40])last=f;return last;};
    CHECK(extent(bent)<extent(plain));
    for(size_t f=40;f<49;++f)CHECK(bent.face_piece[f]==bent.face_piece[40]);
    CHECK(bent.face_piece.back()!=bent.face_piece[40]);
    // Rigid rotation and a change of length units must not change membership.
    auto scaled=folded;const Eigen::AngleAxisd rotation(.7,Vec3d(1,2,3).normalized());
    for(auto& c:scaled.centers)c=rotation*c*10.;
    for(auto& n:scaled.normals)n=rotation*n;
    for(auto& a:scaled.areas)a*=100.;
    for(auto& p:scaled.patches){p.area*=100.;p.center=rotation*p.center*10.;p.normal=rotation*p.normal;}
    CHECK(BeautyPuzzle::create_regions(scaled,labels,{}, {"nose"}).face_piece==bent.face_piece);
    REQUIRE_NOTHROW(bent.validate(folded));
}

TEST_CASE("Semantic region interiors fill unknown holes while retaining seed boundaries", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface = puzzle_grid(24, 20);
    const auto coarse = BeautyPuzzle::create_regions(*surface);
    size_t hole = surface->face_patch.size();
    for (size_t f = 0; f < coarse.face_piece.size(); ++f) {
        bool interior = true;
        for (int32_t n : surface->face_neighbors[f])
            if (n < 0 || coarse.face_piece[n] != coarse.face_piece[f]) interior = false;
        if (interior) { hole = f; break; }
    }
    REQUIRE(hole < surface->face_patch.size());
    const auto region = coarse.faces(coarse.face_piece[hole]);
    std::vector<int32_t> labels(surface->face_patch.size(), -1);
    for (size_t f : region) labels[f] = 3;
    labels[hole] = -1;
    const auto result = BeautyPuzzle::create_regions(*surface, labels);
    REQUIRE_NOTHROW(result.validate(*surface));
    const uint32_t id = result.face_piece[hole];
    for (size_t f : region) CHECK(result.face_piece[f] == id);
    CHECK(result.faces(id).size() == region.size());
}

TEST_CASE("Eye detail refinement keeps the existing piece and removes only newly stranded fragments", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface = puzzle_grid(24, 20);
    auto puzzle = columns(*surface, 18.);
    const auto eye = matching(*surface, [](const Vec3d& c) { return c.x()>6. && c.x()<9. && c.y()>7. && c.y()<10.; });
    const auto owner = puzzle.assign_region(eye, *surface);
    puzzle.paint(owner, blue); puzzle.paint(2, red);
    const auto before = puzzle;
    auto detail = matching(*surface, [](const Vec3d& c) { return c.x()>5. && c.x()<11. && c.y()>6. && c.y()<11.; });
    const auto hole = *std::find_if(detail.begin(), detail.end(), [&](size_t f) { return surface->centers[f].x()>9. && surface->centers[f].x()<10. && surface->centers[f].y()>8. && surface->centers[f].y()<9.; });
    detail.erase(std::find(detail.begin(), detail.end(), hole));
    CHECK(puzzle.refine_detail_region(detail, *surface) == owner);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    CHECK(puzzle.piece_count() == before.piece_count());
    CHECK(puzzle.face_piece[hole] == owner);
    CHECK(puzzle.colors.at(owner) == blue);
    CHECK(puzzle.faces(2) == before.faces(2));
    CHECK(puzzle.colors.at(2) == red);
    for (size_t f : detail) CHECK(puzzle.face_piece[f] == owner);
    const auto refined = puzzle.encode();
    CHECK(puzzle.refine_detail_region(detail, *surface) == owner);
    CHECK(puzzle.encode() == refined);
    REQUIRE_THROWS(puzzle.refine_detail_region({}, *surface));
}

TEST_CASE("Facial outlines shrink old eye seeds and recover missing lips without rewriting evidence", "[BeautyWorkbench][BeautyPuzzle]") {
    GUI::LocalSemanticEvidence::Evidence evidence;
    AI::PrintColorRegion eye,skin;
    eye.subject_id=skin.subject_id="person";eye.label="re";skin.label="face";
    eye.faces={0,1,2};skin.faces={3,4};
    evidence.regions={eye,skin};evidence.face_regions={0,0,0,1,1,-1};
    evidence.feature_details={{"person","re",{1,2},{2},2},{"person","ulip",{4,5},{},2}};
    const auto guidance=beauty_feature_guidance(evidence);
    CHECK(guidance.names.at(guidance.labels[0])=="face");
    CHECK(guidance.names.at(guidance.labels[1])=="re");
    CHECK(guidance.names.at(guidance.labels[4])=="ulip");
    CHECK(guidance.names.at(guidance.labels[5])=="ulip");
    REQUIRE(guidance.eyes.size()==1);
    CHECK(guidance.eyes[0].aperture_faces==std::vector<size_t>{1,2});
    CHECK(guidance.eyes[0].iris_faces==std::vector<size_t>{2});
    CHECK(evidence.face_regions==std::vector<int32_t>{0,0,0,1,1,-1});
    const auto hash=std::string(64,'a');
    CHECK(BeautyGuidance::decode(guidance.encode(hash,hash),hash,hash,6).labels==guidance.labels);
}

TEST_CASE("Facial seed cleanup fills local pinholes without erasing a small feature or distant labels", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface=puzzle_grid(24,20);
    std::vector<int32_t> labels(surface->centers.size(),0);
    std::vector<uint8_t> band(labels.size(),0);
    for(size_t f=0;f<labels.size();++f) {
        const auto& p=surface->centers[f];
        if(p.x()>6 && p.x()<16 && p.y()>5 && p.y()<15)labels[f]=1;
        band[f]=p.x()>3 && p.x()<19 && p.y()>3 && p.y()<17;
    }
    const auto hole=matching(*surface,[](const Vec3d& p){return p.x()>9 && p.x()<10 && p.y()>8 && p.y()<9;});
    const auto speck=matching(*surface,[](const Vec3d& p){return p.x()>4 && p.x()<5 && p.y()>8 && p.y()<9;});
    const auto remote=matching(*surface,[](const Vec3d& p){return p.x()<1 && p.y()<1;});
    const auto small=matching(*surface,[](const Vec3d& p){return p.x()>17 && p.x()<18 && p.y()>8 && p.y()<9;});
    for(size_t f:hole)labels[f]=0;for(size_t f:speck)labels[f]=1;
    for(size_t f:remote)labels[f]=1;for(size_t f:small)labels[f]=2;
    const auto before=labels;
    beauty_clean_feature_seeds(labels,*surface,band,{0,1,2});
    for(size_t f:hole)CHECK(labels[f]==1);
    for(size_t f:speck)CHECK(labels[f]==0);
    for(size_t f:remote)CHECK(labels[f]==1);
    for(size_t f:small)CHECK(labels[f]==2);
    for(size_t f=0;f<labels.size();++f)if(!band[f])CHECK(labels[f]==before[f]);
}

TEST_CASE("Automatic same-filament specks lose seams while facial parts and different slots survive", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface=puzzle_grid(24,20);
    BeautyPuzzle p;p.geometry_id=surface->geometry_id;p.next_id=2;p.face_piece.assign(surface->areas.size(),1);
    p.match_filaments(*surface,{{0,"#282629","PLA",true},{1,"#282629","PLA",true}});
    BeautyGuidance g;g.labels.assign(p.face_piece.size(),0);g.names={"hair","iris"};
    const size_t seed=200;
    REQUIRE(std::all_of(surface->face_neighbors[seed].begin(),surface->face_neighbors[seed].end(),[](int n){return n>=0;}));
    const auto id=p.assign_region({seed},*surface);p.paint_filament(id,0);
    surface->areas[seed]=1e-6; // A tiny triangle inside a much larger surface.
    const auto before=p;
    SECTION("equal physical color preserves every triangle assignment") {
        CHECK(beauty_coalesce_matched_specks(p,*surface,g)==1);
        CHECK(p.piece_count()==1);
        REQUIRE_NOTHROW(p.validate(*surface));
        for(size_t f=0;f<p.face_piece.size();++f) {
            CHECK(p.filament_slots.at(p.face_piece[f])==before.filament_slots.at(before.face_piece[f]));
            CHECK(p.colors.at(p.face_piece[f])==before.colors.at(before.face_piece[f]));
        }
        CHECK(beauty_coalesce_matched_specks(p,*surface,g)==0);
    }
    SECTION("a small facial feature is retained even with the same color") {
        g.labels[seed]=1;CHECK(beauty_coalesce_matched_specks(p,*surface,g)==0);CHECK(p.same_edit(before));
    }
    SECTION("duplicate RGB does not merge distinct physical slots") {
        p.paint_filament(id,1);CHECK(beauty_coalesce_matched_specks(p,*surface,g)==0);CHECK(p.piece_count()==2);
    }
    SECTION("an open or disconnected fragment is not joined across missing edges") {
        surface->face_neighbors[seed]={-1,-1,-1};CHECK(beauty_coalesce_matched_specks(p,*surface,g)==0);
    }
    SECTION("a substantial region is retained") {
        surface->areas[seed]=1.;CHECK(beauty_coalesce_matched_specks(p,*surface,g)==0);
    }
}

TEST_CASE("Automatic garment seams disappear without changing paint or crossing protected boundaries", "[BeautyPuzzle]") {
    const auto surface=puzzle_grid(24,20);auto p=columns(*surface,12.);
    p.match_filaments(*surface,{{0,"#FFFFFF","PLA",true},{1,"#FFFFFF","PLA",true}});
    BeautyGuidance g;g.labels.assign(p.face_piece.size(),-1);g.names={"cloth","hair","iris"};
    for(size_t f=0;f<g.labels.size();++f)if(p.face_piece[f]==1)g.labels[f]=0;
    std::vector<RGBA> source(g.labels.size(),RGBA{.8,.8,.8,1});const auto before=p;
    SECTION("connected same pigment becomes one piece and round trips unchanged") {
        CHECK(beauty_coalesce_body_regions(p,*surface,g,source)==1);CHECK(p.piece_count()==1);
        for(size_t f=0;f<source.size();++f) {
            CHECK(p.colors.at(p.face_piece[f])==before.colors.at(before.face_piece[f]));
            CHECK(p.filament_slots.at(p.face_piece[f])==before.filament_slots.at(before.face_piece[f]));
        }
        REQUIRE_NOTHROW(p.validate(*surface));
        CHECK(BeautyPuzzle::decode(p.encode(),p.geometry_id,source.size()).same_edit(p));
        CHECK(beauty_coalesce_body_regions(p,*surface,g,source)==0);
    }
    SECTION("hair and clothing remain independently editable") {
        for(size_t f=0;f<source.size();++f)if(p.face_piece[f]!=1)g.labels[f]=1;
        CHECK(beauty_coalesce_body_regions(p,*surface,g,source)==0);
    }
    SECTION("a facial feature remains protected") {
        g.labels.back()=2;CHECK(beauty_coalesce_body_regions(p,*surface,g,source)==0);
    }
    SECTION("a true source color boundary survives a collapsed palette") {
        for(size_t f=0;f<source.size();++f)if(p.face_piece[f]!=1)source[f]={.15,.15,.15,1};
        CHECK(beauty_coalesce_body_regions(p,*surface,g,source)==0);
    }
    SECTION("equal RGB with distinct physical slots remains separate") {
        p.paint_filament(2,1);CHECK(beauty_coalesce_body_regions(p,*surface,g,source)==0);
    }
    SECTION("disconnected surfaces remain separate") {
        for(size_t f=0;f<source.size();++f)for(auto& n:surface->face_neighbors[f])if(n>=0 && p.face_piece[f]!=p.face_piece[size_t(n)])n=-1;
        CHECK(beauty_coalesce_body_regions(p,*surface,g,source)==0);
    }
    SECTION("a sharp junction retains its seam") {
        for(size_t f=0;f<source.size();++f)if(p.face_piece[f]!=1)surface->normals[f]=-surface->normals[f];
        CHECK(beauty_coalesce_body_regions(p,*surface,g,source)==0);
    }
    SECTION("unrecognized surfaces do not invent clothing") {
        std::fill(g.labels.begin(),g.labels.end(),-1);CHECK(beauty_coalesce_body_regions(p,*surface,g,source)==0);
    }
}

// Explicit local files only; reads a saved result without changing its history.
// Experimental mask-confirmed simplification. Kept in this probe until visual
// and performance gates show that it should enter the normal workbench path.
static size_t probe_coalesce_mask_regions(BeautyPuzzle& puzzle,const BeautySurface& surface,
    const BeautyGuidance& guidance,const std::vector<RGBA>& source,const std::vector<int32_t>& mask_region) {
    const size_t count=puzzle.face_piece.size();
    if(mask_region.size()!=count || source.size()!=count || !guidance.completed(count))throw std::runtime_error("Mask candidate belongs to another surface.");
    struct Part {double area=0,covered=0;std::array<double,3> rgb{};std::map<int32_t,double> mask;std::set<std::string> names;bool protected_part=false;};
    std::map<uint32_t,Part> parts;
    std::set<std::pair<uint32_t,uint32_t>> adjacency;
    for(size_t f=0;f<count;++f) {
        const auto id=puzzle.face_piece[f];auto& part=parts[id];
        const double area=surface.areas[f];part.area+=area;
        for(size_t c=0;c<3;++c)part.rgb[c]+=area*source[f][c];
        if(mask_region[f]>=0){part.mask[mask_region[f]]+=area;part.covered+=area;}
        if(guidance.labels[f]>=0) {
            const auto& name=guidance.names.at(size_t(guidance.labels[f]));
            if(name=="hair" || name=="cloth")part.names.insert(name);else part.protected_part=true;
        }
        for(int32_t n:surface.face_neighbors[f])if(n>=0 && puzzle.face_piece[size_t(n)]!=id)
            adjacency.emplace(std::minmax(id,puzzle.face_piece[size_t(n)]));
    }
    std::map<uint32_t,int32_t> dominant;
    for(auto& entry:parts) {
        auto& part=entry.second;
        if(part.area<=0 || part.protected_part || part.mask.empty())continue;
        for(auto& value:part.rgb)value/=part.area;
        const auto best=std::max_element(part.mask.begin(),part.mask.end(),[](const auto& a,const auto& b){return a.second<b.second;});
        if(best->second>=.8*part.area)dominant[entry.first]=best->first;
    }
    struct Edge {uint32_t a,b;double distance;};std::vector<Edge> edges;
    for(const auto& [a,b]:adjacency) {
        if(!dominant.count(a) || !dominant.count(b) || dominant.at(a)!=dominant.at(b))continue;
        if(parts[a].names.size()>1 || parts[b].names.size()>1)continue;
        auto names=parts[a].names;names.insert(parts[b].names.begin(),parts[b].names.end());
        if(names.size()!=1)continue;
        if(puzzle.filament_slots.at(a)!=puzzle.filament_slots.at(b) || puzzle.colors.at(a)!=puzzle.colors.at(b))continue;
        const double distance=tex2color::color_utils::calc_rgb_color_difference_by_ciede2000_srgb01(parts[a].rgb,parts[b].rgb);
        if(distance<=12.)edges.push_back({a,b,distance});
    }
    std::sort(edges.begin(),edges.end(),[](const auto& a,const auto& b){return std::tie(a.distance,a.a,a.b)<std::tie(b.distance,b.a,b.b);});
    std::map<uint32_t,uint32_t> parent;std::map<uint32_t,std::vector<uint32_t>> members;
    for(const auto& [id,part]:parts){parent[id]=id;members[id]={id};}
    const auto root=[&](uint32_t id){while(parent[id]!=id){parent[id]=parent[parent[id]];id=parent[id];}return id;};
    size_t merged=0;
    for(const auto& edge:edges) {
        auto a=root(edge.a),b=root(edge.b);if(a==b)continue;
        auto names=parts[a].names;names.insert(parts[b].names.begin(),parts[b].names.end());
        if(names.size()!=1 || dominant.at(a)!=dominant.at(b))continue;
        bool similar=true;
        for(uint32_t x:members[a])for(uint32_t y:members[b])if(
            tex2color::color_utils::calc_rgb_color_difference_by_ciede2000_srgb01(parts[x].rgb,parts[y].rgb)>12.)similar=false;
        if(!similar)continue;
        if(a>b)std::swap(a,b);
        parent[b]=a;parts[a].names=std::move(names);
        members[a].insert(members[a].end(),members[b].begin(),members[b].end());members[b].clear();
        puzzle.colors.erase(b);puzzle.filament_slots.erase(b);puzzle.target_colors.erase(b);++merged;
    }
    for(auto& id:puzzle.face_piece)id=root(id);
    return merged;
}

TEST_CASE("A saved portrait measures seam simplification with unchanged per-face printing colors", "[.][BeautyPortraitProbe]") {
    const auto env=[](const char* key){const auto p=boost::nowide::getenv(key);return p?std::string(p):std::string();};
    const auto source=env("ORCA_PORTRAIT_SOURCE"),record=env("ORCA_PORTRAIT_RECORD"),output=env("ORCA_PORTRAIT_OUTPUT");
    if(source.empty() || record.empty() || output.empty())SKIP("Explicit source, saved record and new output required.");
    REQUIRE_FALSE(boost::filesystem::exists(output));
    TriangleMesh mesh;ObjInfo colors;std::string error;
    REQUIRE(load_model_artifact(source,mesh,colors,error));
    const auto surface=BeautySurface::build(mesh.its,colors.vertex_colors);
    boost::filesystem::ifstream input(record);nlohmann::json root;input>>root;
    const auto& w=root.contains("beauty_workbench")?root.at("beauty_workbench"):root.at("beauty_puzzle_draft");
    const auto& saved=w.at("guidance");
    auto g=BeautyGuidance::decode(saved,surface->geometry_id,saved.at("source"),mesh.its.indices.size());
    // Offline replay of complete worker evidence through the actual native
    // geometry proof and guidance consumer. This never changes an asset record.
    const auto replay=env("ORCA_PORTRAIT_REPLAY");
    if(!replay.empty()) {
        namespace E=Slic3r::GUI::LocalSemanticEvidence;
        namespace G=Slic3r::GUI::LocalSemanticGeometry;
        const auto read=[&](const std::string& name){boost::filesystem::ifstream file(boost::filesystem::path(replay)/name,std::ios::binary);
            REQUIRE(file.good());return std::string(std::istreambuf_iterator<char>(file),{});};
        const auto hash=[](const std::string& value){const auto raw=G::detail::digest(value.data(),value.size(),nullptr);
            std::string result;for(unsigned char c:raw){result+="0123456789abcdef"[c>>4];result+="0123456789abcdef"[c&15];}return result;};
        const auto spec=nlohmann::json::parse(read("request.json")),response=nlohmann::json::parse(read("result.json"));
        const auto source_hash=model_artifact_sha256(source);
        REQUIRE(response.at("status")=="ok");REQUIRE(spec.at("source_sha256")==source_hash);
        REQUIRE(spec.at("runtime_fingerprint")==hash(response.at("identity").dump()));
        REQUIRE(spec.at("policy_sha256")==response.at("policy_sha256"));
        const auto packet_bytes=read("rendered.bin"),evidence_bytes=read("evidence.json");
        REQUIRE(response.at("files").at("rendered.bin").at("sha256")==hash(packet_bytes));
        REQUIRE(response.at("files").at("evidence.json").at("sha256")==hash(evidence_bytes));
        G::Packet packet;REQUIRE(G::decode(packet_bytes,source_hash,packet,error));
        E::VerifiedFaceBinding binding;REQUIRE(E::prove_ordered_faces(source_hash,mesh.its,packet.mesh,binding,error));
        E::ExpectedIdentity expected;expected.request_id=spec.at("request_id");expected.source_sha256=source_hash;
        expected.geometry_id=surface->geometry_id;expected.face_count=mesh.its.indices.size();
        expected.weights_sha256=hash(response.at("identity").at("probe_identity").at("weights").dump());
        expected.runtime_sha256=spec.at("runtime_fingerprint");expected.policy_sha256=spec.at("policy_sha256");
        E::Evidence evidence;const bool decoded=E::decode(evidence_bytes,expected,binding,evidence,error);INFO(error);REQUIRE(decoded);
        g=beauty_feature_guidance(evidence,surface.get());
    }
    const auto mouth_faces=beauty_refine_mouth_guidance(*surface,beauty_source_face_colors(mesh.its,colors.vertex_colors),g);
    std::cout<<"Mouth light faces: "<<mouth_faces<<"\n";
    auto p=BeautyPuzzle::decode(w.at("puzzle"),surface->geometry_id,mesh.its.indices.size());const auto before=p;
    const auto removed=beauty_coalesce_matched_specks(p,*surface,g);
    REQUIRE_NOTHROW(p.validate(*surface));
    size_t changed_slots=0,changed_colors=0;
    for(size_t f=0;f<p.face_piece.size();++f) {
        changed_slots+=p.filament_slots.at(p.face_piece[f])!=before.filament_slots.at(before.face_piece[f]);
        changed_colors+=p.colors.at(p.face_piece[f])!=before.colors.at(before.face_piece[f]);
    }
    CHECK(changed_slots==0);CHECK(changed_colors==0);
    const auto coarse=BeautyPuzzle::create_regions(*surface);
    nlohmann::json groups=nlohmann::json::array();
    struct Mean {size_t count=0;double area=0;std::array<double,3> rgb{};};
    std::map<std::pair<int32_t,uint32_t>,Mean> means;
    const auto source_faces=beauty_source_face_colors(mesh.its,colors.vertex_colors);
    if(!env("ORCA_PORTRAIT_EXPORT_SURFACE").empty()) {
        const auto write_binary=[&](const std::string& suffix,const auto& values){
            const auto path=output+suffix;REQUIRE_FALSE(boost::filesystem::exists(path));
            boost::filesystem::ofstream file(path,std::ios::binary);
            file.write(reinterpret_cast<const char*>(values.data()),std::streamsize(values.size()*sizeof(values[0])));REQUIRE(file.good());};
        write_binary(".patch.u32",surface->face_patch);write_binary(".neighbors.i32",surface->face_neighbors);
        write_binary(".areas.f64",surface->areas);
        std::vector<float> normals,rgb;normals.reserve(source_faces.size()*3);rgb.reserve(source_faces.size()*3);
        for(size_t f=0;f<source_faces.size();++f)for(size_t c=0;c<3;++c){normals.push_back(float(surface->normals[f][c]));rgb.push_back(source_faces[f][c]);}
        write_binary(".normals.f32",normals);write_binary(".rgb.f32",rgb);
    }
    {auto updated=root;auto& record=updated.contains("beauty_workbench")?updated["beauty_workbench"]:updated["beauty_puzzle_draft"];
        record["guidance"]=g.encode(surface->geometry_id,saved.at("source"));
        boost::filesystem::ofstream file(output+".guidance.json");file<<updated.dump();}
    for(size_t f=0;f<source_faces.size();++f)if(g.labels[f]>=0) {
        auto& mean=means[{g.labels[f],coarse.face_piece[f]}];++mean.count;mean.area+=surface->areas[f];
        for(size_t c=0;c<3;++c)mean.rgb[c]+=surface->areas[f]*source_faces[f][c];
    }
    for(auto& entry:means) {
        for(auto& c:entry.second.rgb)c/=entry.second.area;
        groups.push_back({{"label",g.names.at(size_t(entry.first.first))},{"coarse",entry.first.second},
            {"area",entry.second.area},{"faces",entry.second.count},{"rgb",entry.second.rgb}});
    }
    boost::filesystem::ofstream result(output);
    result<<nlohmann::json({{"before",before.piece_count()},{"after",p.piece_count()},{"removed",removed},
        {"changed_slots",changed_slots},{"changed_colors",changed_colors},{"puzzle",p.encode()},
        {"coarse",coarse.encode()},{"groups",groups}}).dump();
    auto palette=before.palette;auto mixtures=before.mixed_recipes;
    const auto palette_file=env("ORCA_PORTRAIT_PALETTE");
    if(!palette_file.empty()) {
        boost::filesystem::ifstream file(palette_file);nlohmann::json values;file>>values;
        const auto saved_palette=BeautyPuzzle::decode(values.at("puzzle"),surface->geometry_id,mesh.its.indices.size());
        palette=saved_palette.palette;mixtures=saved_palette.mixed_recipes;
        const auto uniform=values.at("uniform_slots").get<std::set<size_t>>();
        for(auto& mix:mixtures)mix.uniform_color=uniform.count(*mix.existing_virtual_slot)!=0;
    }
    auto automatic=BeautyPuzzle::create_regions(*surface,g.labels,{},g.names);auto nearest=automatic;
    nearest.match_filaments(*surface,palette,mixtures,source_faces);
    boost::filesystem::ofstream baseline(output+".nearest.json");baseline<<nearest.encode().dump();
    nlohmann::json audit=nlohmann::json::array();
    std::map<uint32_t,Mean> piece_means;
    std::map<uint32_t,std::map<std::string,double>> piece_labels;
    std::map<uint32_t,std::set<uint32_t>> piece_neighbors;
    for(size_t f=0;f<source_faces.size();++f) {
        const auto id=nearest.face_piece[f];auto& mean=piece_means[id];++mean.count;mean.area+=surface->areas[f];
        for(size_t c=0;c<3;++c)mean.rgb[c]+=surface->areas[f]*source_faces[f][c];
        if(g.labels[f]>=0)piece_labels[id][g.names.at(size_t(g.labels[f]))]+=surface->areas[f];
        for(int32_t n:surface->face_neighbors[f])if(n>=0 && nearest.face_piece[size_t(n)]!=id)piece_neighbors[id].insert(nearest.face_piece[size_t(n)]);
    }
    for(auto& entry:piece_means) {
        for(auto& c:entry.second.rgb)c/=entry.second.area;
        audit.push_back({{"id",entry.first},{"area",entry.second.area},{"rgb",entry.second.rgb},
            {"labels",piece_labels[entry.first]},{"neighbors",piece_neighbors[entry.first]}});
    }
    boost::filesystem::ofstream audit_file(output+".regions.json");audit_file<<audit.dump();
    const auto adjusted=beauty_match_feature_filaments(automatic,*surface,g,source_faces,palette,mixtures);
    std::cout<<"Contrast-adjusted regions: "<<adjusted<<"\n";
    beauty_coalesce_matched_specks(automatic,*surface,g);
    beauty_coalesce_body_regions(automatic,*surface,g,source_faces);REQUIRE_NOTHROW(automatic.validate(*surface));
    const auto mask_file=env("ORCA_PORTRAIT_MASK_REGIONS");
    if(!mask_file.empty()) {
        namespace G=Slic3r::GUI::LocalSemanticGeometry;
        const auto digest=[](const char* data,size_t size){const auto raw=G::detail::digest(data,size,nullptr);
            std::string result;for(unsigned char c:raw){result+="0123456789abcdef"[c>>4];result+="0123456789abcdef"[c&15];}return result;};
        boost::filesystem::ifstream file(mask_file,std::ios::binary);
        REQUIRE(file.good());const std::string bytes(std::istreambuf_iterator<char>(file),{});
        REQUIRE(bytes.size()==surface->patches.size()*sizeof(int32_t));
        REQUIRE(digest(bytes.data(),bytes.size())==env("ORCA_PORTRAIT_MASK_SHA256"));
        const auto& patch=surface->face_patch;
        REQUIRE(digest(reinterpret_cast<const char*>(patch.data()),patch.size()*sizeof(uint32_t))==env("ORCA_PORTRAIT_PATCH_SHA256"));
        std::vector<int32_t> patch_region(surface->patches.size());
        std::memcpy(patch_region.data(),bytes.data(),bytes.size());
        std::vector<int32_t> face_region(patch.size());
        for(size_t f=0;f<patch.size();++f)face_region[f]=patch_region[patch[f]];
        const auto before_mask=automatic;
        const size_t joined=probe_coalesce_mask_regions(automatic,*surface,g,source_faces,face_region);
        REQUIRE_NOTHROW(automatic.validate(*surface));
        size_t slot_changes=0,color_changes=0;
        for(size_t f=0;f<automatic.face_piece.size();++f) {
            slot_changes+=automatic.filament_slots.at(automatic.face_piece[f])!=before_mask.filament_slots.at(before_mask.face_piece[f]);
            color_changes+=automatic.colors.at(automatic.face_piece[f])!=before_mask.colors.at(before_mask.face_piece[f]);
        }
        CHECK(slot_changes==0);CHECK(color_changes==0);
        std::cout<<"Mask-confirmed merged regions: "<<joined<<"\n";
    }
    boost::filesystem::ofstream fresh(output+".automatic.json");fresh<<automatic.encode().dump();
}

TEST_CASE("A frozen portrait edit group paints atomically without changing other print faces", "[.][BeautyEditGroupProbe]") {
    const auto env=[](const char* key){const auto p=boost::nowide::getenv(key);return p?std::string(p):std::string();};
    const auto source=env("ORCA_EDIT_SOURCE"),puzzle_file=env("ORCA_EDIT_PUZZLE"),group_file=env("ORCA_EDIT_GROUPS"),output=env("ORCA_EDIT_OUTPUT");
    if(source.empty() || puzzle_file.empty() || group_file.empty() || output.empty())SKIP("Explicit frozen inputs and fresh output required.");
    REQUIRE_FALSE(boost::filesystem::exists(output));
    TriangleMesh mesh;ObjInfo colors;std::string error;
    REQUIRE(load_model_artifact(source,mesh,colors,error));
    const auto surface=BeautySurface::build(mesh.its,colors.vertex_colors);
    namespace G=Slic3r::GUI::LocalSemanticGeometry;
    const auto digest=[](const char* data,size_t size){const auto raw=G::detail::digest(data,size,nullptr);
        std::string result;for(unsigned char c:raw){result+="0123456789abcdef"[c>>4];result+="0123456789abcdef"[c&15];}return result;};
    REQUIRE(model_artifact_sha256(source)==env("ORCA_EDIT_SOURCE_SHA256"));
    REQUIRE(digest(reinterpret_cast<const char*>(surface->face_patch.data()),surface->face_patch.size()*sizeof(uint32_t))==env("ORCA_EDIT_PATCH_SHA256"));
    boost::filesystem::ifstream saved(puzzle_file);nlohmann::json encoded;saved>>encoded;
    auto puzzle=BeautyPuzzle::decode(encoded,surface->geometry_id,mesh.its.indices.size());
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    boost::filesystem::ifstream mask_input(group_file,std::ios::binary);
    REQUIRE(mask_input.good());
    const std::string bytes(std::istreambuf_iterator<char>(mask_input),{});
    REQUIRE(bytes.size()==surface->patches.size()*sizeof(int32_t));
    REQUIRE(digest(bytes.data(),bytes.size())==env("ORCA_EDIT_GROUP_SHA256"));
    std::vector<int32_t> patch_groups(surface->patches.size());
    std::memcpy(patch_groups.data(),bytes.data(),bytes.size());
    std::map<int32_t,double> group_area;
    for(size_t f=0;f<puzzle.face_piece.size();++f) {
        const int32_t group=patch_groups[surface->face_patch[f]];
        if(group>=0)group_area[group]+=surface->areas[f];
    }
    REQUIRE_FALSE(group_area.empty());
    const auto largest=std::max_element(group_area.begin(),group_area.end(),[](const auto& a,const auto& b){return a.second<b.second;});
    std::vector<size_t> selected;
    for(size_t f=0;f<puzzle.face_piece.size();++f)
        if(patch_groups[surface->face_patch[f]]==largest->first)selected.push_back(f);
    std::map<size_t,size_t> original_slots;
    for(size_t f:selected)++original_slots[puzzle.filament_slots.at(puzzle.face_piece[f])];
    const auto common=std::max_element(original_slots.begin(),original_slots.end(),[](const auto& a,const auto& b){return a.second<b.second;});
    const std::string requested_slot=env("ORCA_EDIT_TARGET_SLOT");
    auto target=std::find_if(puzzle.palette.begin(),puzzle.palette.end(),[&](const auto& channel){
        return channel.compatible && channel.slot!=common->first &&
            (requested_slot.empty() || std::to_string(channel.slot)==requested_slot);
    });
    REQUIRE(target!=puzzle.palette.end());
    const size_t target_slot=target->slot;
    const auto before=puzzle;
    const auto start=std::chrono::steady_clock::now();
    puzzle.paint_faces_filament(selected,*surface,target_slot);
    const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    std::vector<uint8_t> chosen(puzzle.face_piece.size(),0);
    for(size_t f:selected)chosen[f]=1;
    size_t untouched_changes=0,selected_changes=0;
    for(size_t f=0;f<puzzle.face_piece.size();++f) {
        const auto old_id=before.face_piece[f],new_id=puzzle.face_piece[f];
        const auto old_slot=before.filament_slots.at(old_id),new_slot=puzzle.filament_slots.at(new_id);
        if(chosen[f]) {CHECK(new_slot==target_slot);selected_changes+=new_slot!=old_slot;}
        else untouched_changes+=new_slot!=old_slot || puzzle.colors.at(new_id)!=before.colors.at(old_id);
    }
    CHECK(untouched_changes==0);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    CHECK(BeautyPuzzle::decode(puzzle.encode(),puzzle.geometry_id,puzzle.face_piece.size()).same_edit(puzzle));
    boost::filesystem::ofstream result(output);
    result<<nlohmann::json({{"group",largest->first},{"selected_faces",selected.size()},{"original_slots",original_slots},
        {"target_slot",target_slot},{"selected_slot_changes",selected_changes},{"untouched_changes",untouched_changes},
        {"before_pieces",before.piece_count()},{"after_pieces",puzzle.piece_count()},{"paint_seconds",seconds}}).dump(2);
    boost::filesystem::ofstream painted(output+".puzzle.json");painted<<puzzle.encode().dump();
}

TEST_CASE("An unseen portrait builds a native baseline from verified worker evidence", "[.][BeautyHoldoutProbe]") {
    const auto env=[](const char* key){const auto p=boost::nowide::getenv(key);return p?std::string(p):std::string();};
    const auto source=env("ORCA_HOLDOUT_SOURCE"),replay=env("ORCA_HOLDOUT_REPLAY"),palette_file=env("ORCA_HOLDOUT_PALETTE"),output=env("ORCA_HOLDOUT_OUTPUT");
    if(source.empty() || replay.empty() || palette_file.empty() || output.empty())SKIP("Explicit unseen source, worker result, palette and fresh output required.");
    REQUIRE_FALSE(boost::filesystem::exists(output));
    TriangleMesh mesh;ObjInfo colors;std::string error;
    REQUIRE(load_model_artifact(source,mesh,colors,error));
    const auto surface=BeautySurface::build(mesh.its,colors.vertex_colors);
    namespace E=Slic3r::GUI::LocalSemanticEvidence;
    namespace G=Slic3r::GUI::LocalSemanticGeometry;
    const auto digest=[](const char* data,size_t size){const auto raw=G::detail::digest(data,size,nullptr);
        std::string result;for(unsigned char c:raw){result+="0123456789abcdef"[c>>4];result+="0123456789abcdef"[c&15];}return result;};
    const auto read=[&](const std::string& name){boost::filesystem::ifstream file(boost::filesystem::path(replay)/name,std::ios::binary);
        REQUIRE(file.good());return std::string(std::istreambuf_iterator<char>(file),{});};
    const auto source_hash=model_artifact_sha256(source);
    REQUIRE(source_hash==env("ORCA_HOLDOUT_SOURCE_SHA256"));
    const auto spec=nlohmann::json::parse(read("request.json")),response=nlohmann::json::parse(read("result.json"));
    REQUIRE(response.at("status")=="ok");REQUIRE(spec.at("source_sha256")==source_hash);
    const auto identity_text=response.at("identity").dump();
    REQUIRE(spec.at("runtime_fingerprint")==digest(identity_text.data(),identity_text.size()));
    REQUIRE(spec.at("policy_sha256")==response.at("policy_sha256"));
    const auto packet_bytes=read("rendered.bin"),evidence_bytes=read("evidence.json");
    REQUIRE(response.at("files").at("rendered.bin").at("sha256")==digest(packet_bytes.data(),packet_bytes.size()));
    REQUIRE(response.at("files").at("evidence.json").at("sha256")==digest(evidence_bytes.data(),evidence_bytes.size()));
    G::Packet packet;REQUIRE(G::decode(packet_bytes,source_hash,packet,error));
    E::VerifiedFaceBinding binding;REQUIRE(E::prove_ordered_faces(source_hash,mesh.its,packet.mesh,binding,error));
    E::ExpectedIdentity expected;expected.request_id=spec.at("request_id");expected.source_sha256=source_hash;
    expected.geometry_id=surface->geometry_id;expected.face_count=mesh.its.indices.size();
    const auto weights_text=response.at("identity").at("probe_identity").at("weights").dump();
    expected.weights_sha256=digest(weights_text.data(),weights_text.size());
    expected.runtime_sha256=spec.at("runtime_fingerprint");expected.policy_sha256=spec.at("policy_sha256");
    E::Evidence evidence;const bool decoded=E::decode(evidence_bytes,expected,binding,evidence,error);INFO(error);REQUIRE(decoded);
    auto guidance=beauty_feature_guidance(evidence,surface.get());
    const auto source_faces=beauty_source_face_colors(mesh.its,colors.vertex_colors);
    beauty_refine_mouth_guidance(*surface,source_faces,guidance);
    boost::filesystem::ifstream palette_input(palette_file);REQUIRE(palette_input.good());
    const std::string palette_bytes(std::istreambuf_iterator<char>(palette_input),{});
    REQUIRE(digest(palette_bytes.data(),palette_bytes.size())==env("ORCA_HOLDOUT_PALETTE_SHA256"));
    const auto palette_json=nlohmann::json::parse(palette_bytes).at("palette");
    std::vector<PhysicalFilamentChannel> palette;
    for(const auto& entry:palette_json)palette.push_back({entry.at("slot"),entry.at("color"),entry.at("material"),entry.at("compatible")});
    REQUIRE(is_valid_physical_channel_set(palette));
    auto puzzle=BeautyPuzzle::create_regions(*surface,guidance.labels,{},guidance.names);
    beauty_match_feature_filaments(puzzle,*surface,guidance,source_faces,palette,{});
    beauty_coalesce_matched_specks(puzzle,*surface,guidance);
    beauty_coalesce_body_regions(puzzle,*surface,guidance,source_faces);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    CHECK(BeautyPuzzle::decode(puzzle.encode(),puzzle.geometry_id,puzzle.face_piece.size()).same_edit(puzzle));
    const auto write_binary=[&](const std::string& suffix,const auto& values){
        const auto path=output+suffix;REQUIRE_FALSE(boost::filesystem::exists(path));
        boost::filesystem::ofstream file(path,std::ios::binary);
        file.write(reinterpret_cast<const char*>(values.data()),std::streamsize(values.size()*sizeof(values[0])));REQUIRE(file.good());};
    write_binary(".patch.u32",surface->face_patch);write_binary(".neighbors.i32",surface->face_neighbors);
    write_binary(".areas.f64",surface->areas);
    std::vector<float> normals,rgb;normals.reserve(source_faces.size()*3);rgb.reserve(source_faces.size()*3);
    for(size_t f=0;f<source_faces.size();++f)for(size_t c=0;c<3;++c){normals.push_back(float(surface->normals[f][c]));rgb.push_back(source_faces[f][c]);}
    write_binary(".normals.f32",normals);write_binary(".rgb.f32",rgb);
    boost::filesystem::ofstream saved(output+".guidance.json");saved<<nlohmann::json({{"beauty_workbench",{{"guidance",guidance.encode(surface->geometry_id,source_hash)}}}}).dump();
    boost::filesystem::ofstream automatic(output+".automatic.json");automatic<<puzzle.encode().dump();
    boost::filesystem::ofstream result(output);result<<nlohmann::json({{"face_count",puzzle.face_piece.size()},{"piece_count",puzzle.piece_count()},
        {"geometry_id",surface->geometry_id},{"source_sha256",source_hash},{"palette_sha256",env("ORCA_HOLDOUT_PALETTE_SHA256")}}).dump(2);
}

TEST_CASE("Drawing a region spans old pieces and keeps original texture with independent disconnected selections", "[BeautyWorkbench][BeautyPuzzle]") {
    const auto surface = puzzle_grid();
    auto puzzle = columns(*surface, 3.);
    puzzle.paint(1, blue); puzzle.paint(2, red);
    const auto crossing = matching(*surface, [](const Vec3d& c) { return c.y() > 1. && c.y() < 3.; });
    const uint32_t created = puzzle.assign_region(crossing, *surface);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    CHECK(puzzle.faces(created).size() == crossing.size());
    CHECK(puzzle.colors.count(created) == 0);
    for (size_t f : crossing) CHECK(puzzle.face_piece[f] == created);
    for (size_t f = 0; f < puzzle.face_piece.size(); ++f)
        if (surface->centers[f].y() < 1. || surface->centers[f].y() > 3.)
            CHECK(puzzle.colors.at(puzzle.face_piece[f]) == (surface->centers[f].x() < 3. ? blue : red));

    const auto before = puzzle.encode();
    REQUIRE_THROWS(puzzle.assign_region({}, *surface));
    CHECK(puzzle.encode() == before);
    REQUIRE_THROWS(puzzle.assign_region({0, surface->face_patch.size()}, *surface));
    CHECK(puzzle.encode() == before);
    const auto corners = matching(*surface, [](const Vec3d& c) {
        return (c.x() < 1. && c.y() < 1.) || (c.x() > 4. && c.y() > 3.);
    });
    const uint32_t drawn = puzzle.assign_region(corners, *surface);
    REQUIRE_NOTHROW(puzzle.validate(*surface));
    CHECK(puzzle.faces(drawn).size() == 4);
    std::set<uint32_t> new_ids;
    for (size_t f : corners) { new_ids.insert(puzzle.face_piece[f]); CHECK(puzzle.colors.count(puzzle.face_piece[f]) == 0); }
    CHECK(new_ids.size() == 2);
    auto exhausted = puzzle; exhausted.next_id = BeautyPuzzle::max_id;
    const auto full = exhausted.encode();
    REQUIRE_THROWS(exhausted.assign_region(crossing, *surface));
    CHECK(exhausted.encode() == full);
}
