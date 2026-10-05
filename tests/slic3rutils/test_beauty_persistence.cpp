#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "slic3r/GUI/AI/ModelGeneration/BeautyDraftQueue.hpp"
#include "slic3r/GUI/AI/ModelGeneration/BeautyPreparationTicket.hpp"
#include "slic3r/GUI/AI/ModelGeneration/BeautyWorkbenchControls.hpp"
#include "slic3r/GUI/AI/Model/BeautyPrintColorHandoff.hpp"
#include "slic3r/GUI/AI/ModelGeneration/LocalPrintColorMatching.hpp"
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <atomic>
#include <future>

using namespace Slic3r;
using namespace Slic3r::GUI;

TEST_CASE("Late preparation cannot publish a draft after switching away and back", "[BeautyWorkbench][BeautyPersistence]") {
    const auto switched = GENERATE(std::make_pair("second.glb", "geometry-A"),
        std::make_pair("first.glb", "geometry-B"), std::make_pair("second.glb", "geometry-B"));
    INFO("Switched asset " << switched.first << " / " << switched.second);
    BeautyPreparationTicket old{"first.glb","geometry-A"};
    std::vector<std::string> published;
    BeautyDraftQueue queue([&](const auto& request) { published.push_back(request.model.string()); });
    std::promise<void> entered, release;
    auto started=entered.get_future();
    auto gate=release.get_future().share();
    std::thread worker([&] {
        entered.set_value();
        gate.wait();
        if(old.accepts("first.glb","geometry-A","geometry-A"))
            queue.submit({"first.glb",1});
    });
    const bool running=started.wait_for(std::chrono::seconds(5))==std::future_status::ready;
    old.cancel_if_changed(switched.first, switched.second);
    old.cancel_if_changed("first.glb","geometry-A");
    release.set_value();
    worker.join();
    queue.flush();
    CHECK(running);
    CHECK_FALSE(old.accepts("first.glb","geometry-A","geometry-A"));
    CHECK(published.empty());

    BeautyPreparationTicket current{"second.glb","geometry-B"};
    CHECK_FALSE(current.accepts("first.glb","geometry-B","geometry-B"));
    CHECK_FALSE(current.accepts("second.glb","geometry-A","geometry-B"));
    CHECK_FALSE(current.accepts("second.glb","geometry-B","geometry-A"));
    REQUIRE(current.accepts("second.glb","geometry-B","geometry-B"));
    queue.submit({"second.glb",2});
    queue.flush();
    CHECK(published==std::vector<std::string>{"second.glb"});
}

TEST_CASE("Closing the draft writer preserves the latest unsaved edit", "[BeautyWorkbench][BeautyPersistence]") {
    std::vector<int> persisted;
    std::promise<void> entered, release;
    auto started=entered.get_future();
    auto gate=release.get_future().share();
    bool running=false;
    {
        BeautyDraftQueue queue([&](const auto& request) {
            if(request.record==1) {entered.set_value();gate.wait();}
            persisted.push_back(request.record.template get<int>());
        });
        queue.submit({"first.glb",1});
        running=started.wait_for(std::chrono::seconds(5))==std::future_status::ready;
        queue.submit({"first.glb",2});
        queue.submit({"first.glb",3});
        release.set_value();
        // Destruction must drain without requiring an explicit flush.
    }
    REQUIRE(running);
    CHECK(persisted==std::vector<int>{1,3});
}

TEST_CASE("Confirmed cleanup stays ordered through switching and later edits", "[BeautyWorkbench][BeautyPersistence]") {
    std::vector<std::string> persisted;
    {
        BeautyDraftQueue queue([&](const auto& request) {
            persisted.push_back(request.model.string()+(request.remove?":remove":":write"));
        });
        queue.submit({"first.glb",1});
        queue.submit({"first.glb",{},true,true});
        queue.flush();
        CHECK(persisted==std::vector<std::string>{"first.glb:write","first.glb:remove"});
        queue.submit({"second.glb",2});
        queue.flush();
        queue.submit({"first.glb",3});
    }
    CHECK(persisted==std::vector<std::string>{"first.glb:write","first.glb:remove","second.glb:write","first.glb:write"});
}

TEST_CASE("Recognition writes stay ordered between draft edits", "[BeautyWorkbench][BeautyPersistence]") {
    std::vector<std::string> persisted;
    std::promise<void> entered, release;
    auto started=entered.get_future();
    auto gate=release.get_future().share();
    BeautyDraftQueue queue([&](const auto& request) {
        if(request.record==1) {entered.set_value();gate.wait();}
        persisted.push_back(request.recognition?"recognition":request.record.dump());
    });
    queue.submit({"first.glb",1});
    REQUIRE(started.wait_for(std::chrono::seconds(5))==std::future_status::ready);
    BeautyDraftQueue::Request recognition;
    recognition.model="first.glb";
    recognition.record={{"beauty_recognition",{{"state","ready"}}}};
    recognition.recognition=true;
    queue.submit(std::move(recognition));
    queue.submit({"first.glb",2});
    queue.submit({"first.glb",3});
    release.set_value();
    queue.flush();
    CHECK(persisted==std::vector<std::string>{"1","recognition","3"});
}

TEST_CASE("Failed drafts can be retried after the storage error is resolved", "[BeautyWorkbench][BeautyPersistence]") {
    std::atomic<bool> writable{false};
    std::vector<int> persisted;
    BeautyDraftQueue queue([&](const auto& request) {
        if(!writable)throw std::runtime_error("disk full");
        persisted.push_back(request.record.template get<int>());
    });
    queue.submit({"first.glb",7});
    queue.flush();
    CHECK(queue.take_error().find("disk full")!=std::string::npos);
    CHECK(persisted.empty());
    writable=true;
    queue.flush();
    CHECK(queue.take_error().empty());
    CHECK(persisted==std::vector<int>{7});
}

TEST_CASE("Deferred draft encoding keeps the latest pending edit in writer order", "[BeautyWorkbench][BeautyPersistence]") {
    std::promise<void> entered, release;
    auto started=entered.get_future();
    auto gate=release.get_future().share();
    std::vector<int> persisted;
    std::atomic<int> prepared{0};
    std::thread::id encoder_thread;
    BeautyDraftQueue queue([&](const auto& request) {
        if(request.record==1) {entered.set_value();gate.wait();}
        persisted.push_back(request.record.template get<int>());
    });
    queue.submit({"first.glb",1});
    REQUIRE(started.wait_for(std::chrono::seconds(5))==std::future_status::ready);
    BeautyDraftQueue::Request second;second.model="first.glb";
    second.prepare_record=[&] {++prepared;return nlohmann::json(2);};
    queue.submit(std::move(second));
    BeautyDraftQueue::Request latest;latest.model="first.glb";
    latest.prepare_record=[&] {++prepared;encoder_thread=std::this_thread::get_id();return nlohmann::json(3);};
    queue.submit(std::move(latest));
    CHECK(prepared==0);
    release.set_value();
    queue.flush();
    CHECK(persisted==std::vector<int>{1,3});
    CHECK(prepared==1);
    CHECK(encoder_thread!=std::this_thread::get_id());
}

TEST_CASE("Deferred draft encoding failure remains retryable before a model switch", "[BeautyWorkbench][BeautyPersistence]") {
    std::atomic<bool> valid{false};
    std::vector<int> persisted;
    BeautyDraftQueue queue([&](const auto& request) {
        persisted.push_back(request.record.template get<int>());
    });
    BeautyDraftQueue::Request request;request.model="first.glb";
    request.prepare_record=[&] {
        if(!valid)throw std::runtime_error("draft encoding failed");
        return nlohmann::json(7);
    };
    queue.submit(std::move(request));
    queue.flush();
    CHECK(queue.take_error().find("draft encoding failed")!=std::string::npos);
    CHECK(persisted.empty());
    valid=true;
    queue.flush();
    CHECK(queue.take_error().empty());
    CHECK(persisted==std::vector<int>{7});
}

namespace {
struct HistoryFixture {
    boost::filesystem::path directory=boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("beauty-history-%%%%-%%%%");
    boost::filesystem::path model=directory/"model.glb";
    AI::BeautyPuzzle puzzle;
    HistoryFixture() {
        boost::filesystem::create_directory(directory);
        puzzle.geometry_id=std::string(64,'a');puzzle.face_piece={1,1};puzzle.next_id=2;
        puzzle.colors[1]={1.f,0.f,0.f,1.f};
    }
    ~HistoryFixture() {boost::system::error_code ignored;boost::filesystem::remove_all(directory,ignored);}
    void save() {
        boost::filesystem::ofstream output(directory/"model.json");
        output<<nlohmann::json{{"model_sha256",std::string(64,'b')},
            {"beauty_workbench",{{"geometry_id",puzzle.geometry_id},{"puzzle",puzzle.encode()}}}};
    }
};
}

TEST_CASE("Appearance-only history continues through normal import matching", "[BeautyWorkbench][BeautyPersistence]") {
    HistoryFixture fixture;fixture.save();
    AI::ModelImportRequest request;
    request.face_color_overrides.push_back({0,{1.f,0.f,0.f}});
    REQUIRE_NOTHROW(BeautyWorkbenchControls::prepare_import(fixture.model,request));
    CHECK_FALSE(request.matched_colors.has_value());
    CHECK(request.face_color_overrides.size()==1);
}

TEST_CASE("Matched history preserves explicit physical slots including identical colors", "[BeautyWorkbench][BeautyPersistence]") {
    HistoryFixture fixture;
    fixture.puzzle.palette={{0,"#FF0000","PLA",true},{1,"#FF0000","PLA",true}};
    fixture.puzzle.filament_slots[1]=1;fixture.save();
    if(GENERATE(false,true)) {
        fixture.puzzle.target_colors[1]={.8f,.2f,.1f,1.f};fixture.save();
    }
    AI::ModelImportRequest request;
    request.face_color_overrides.push_back({0,{0.f,1.f,0.f}});
    REQUIRE_NOTHROW(BeautyWorkbenchControls::prepare_import(fixture.model,request));
    REQUIRE(request.matched_colors.has_value());
    CHECK(request.matched_colors->valid());
    CHECK(request.matched_colors->face_slots==std::vector<size_t>{1,1});
    CHECK(request.face_color_overrides.empty());
    LocalPrintColorMatching::Input input;
    input.identity.source_sha256=request.matched_colors->source_sha256;
    input.identity.geometry_id=request.matched_colors->geometry_id;
    input.identity.face_count=2;
    input.identity.material_fingerprint="current-materials";
    input.identity.process_fingerprint="current-process";
    input.identity.physical_channels=fixture.puzzle.palette;
    input.faces={{{1.f,0.f,0.f},1},{{1.f,0.f,0.f},1}};
    BeautyPrintColorHandoff::seed(*request.matched_colors,input.identity);
    const auto matched=LocalPrintColorMatching::compute(input);
    INFO(matched.error);
    REQUIRE(matched.ok());
    for(size_t target:matched.result.face_targets)CHECK(matched.result.targets[target].physical_slot==1);
}

TEST_CASE("Incomplete matched history is rejected without replacing the import request", "[BeautyWorkbench][BeautyPersistence]") {
    HistoryFixture fixture;
    fixture.puzzle.palette={{0,"#FF0000","PLA",true}};fixture.save();
    AI::ModelImportRequest request;
    REQUIRE_THROWS(BeautyWorkbenchControls::prepare_import(fixture.model,request));
    CHECK_FALSE(request.matched_colors.has_value());
}

#include "slic3r/GUI/AI/ModelGeneration/BeautySourceSnapshot.hpp"
#include "slic3r/GUI/AI/Model/VertexColorRegionEditor.hpp"
#include <type_traits>

namespace {
struct SharedBeautySource {
    indexed_triangle_set mesh;
    std::vector<RGBA> colors;
    std::shared_ptr<std::atomic<bool>> destroyed;
    ~SharedBeautySource() { if(destroyed)destroyed->store(true); }
};
std::shared_ptr<SharedBeautySource> shared_beauty_source() {
    auto owner=std::make_shared<SharedBeautySource>();
    owner->mesh.vertices={{0,0,0},{1,0,0},{0,1,0},{1,1,0}};
    owner->mesh.indices={{0,1,2},{2,1,3}};
    owner->colors.assign(4,{.2f,.4f,.6f,1.f});
    return owner;
}
}

TEST_CASE("Selection and surface readers retain the original immutable source after navigation", "[BeautySourceSnapshot][BeautyPersistence]") {
    auto owner=shared_beauty_source();
    auto destroyed=std::make_shared<std::atomic<bool>>(false);owner->destroyed=destroyed;
    std::weak_ptr<SharedBeautySource> weak=owner;
    auto source=BeautySourceSnapshot::share(owner,owner->mesh,owner->colors);
    static_assert(std::is_same_v<decltype(source.mesh()),const indexed_triangle_set&>);
    static_assert(std::is_same_v<decltype(source.colors()),const std::vector<RGBA>&>);
    REQUIRE(source);
    CHECK(&source.mesh()==&owner->mesh);CHECK(source.colors().data()==owner->colors.data());
    AI::VertexColorRegionEditor editor;std::shared_ptr<AI::BeautySurface> surface;
    std::promise<void> selected_entered,beauty_entered,release;
    auto selected_started=selected_entered.get_future(),beauty_started=beauty_entered.get_future();
    auto gate=release.get_future().share();
    auto selection=std::async(std::launch::async,[&,source] {
        selected_entered.set_value();gate.wait();std::string error;
        if(!editor.initialize(source.mesh(),source.colors(),error))throw std::runtime_error(error);
    });
    auto beauty=std::async(std::launch::async,[&,source] {
        beauty_entered.set_value();gate.wait();surface=AI::BeautySurface::build(source.mesh(),source.colors());
    });
    const bool selected_running=selected_started.wait_for(std::chrono::seconds(5))==std::future_status::ready;
    const bool beauty_running=beauty_started.wait_for(std::chrono::seconds(5))==std::future_status::ready;
    owner.reset();CHECK_FALSE(weak.expired());CHECK_FALSE(destroyed->load());
    release.set_value();selection.get();beauty.get();
    REQUIRE(selected_running);REQUIRE(beauty_running);REQUIRE(surface);
    CHECK(editor.mesh().vertices==source.mesh().vertices);CHECK(editor.mesh().indices==source.mesh().indices);
    CHECK(editor.vertex_colors()==source.colors());
    const auto reference=AI::BeautySurface::build(editor.mesh(),editor.vertex_colors());
    CHECK(surface->geometry_id==reference->geometry_id);CHECK(surface->face_neighbors==reference->face_neighbors);
    CHECK(surface->face_patch==reference->face_patch);CHECK(surface->areas==reference->areas);
    source={};selection={};beauty={};
    CHECK(weak.expired());CHECK(destroyed->load());
}

TEST_CASE("Incomplete source arrays are never exposed to beauty workers", "[BeautySourceSnapshot]") {
    auto owner=shared_beauty_source();
    CHECK_FALSE(BeautySourceSnapshot::share(std::shared_ptr<SharedBeautySource>{},owner->mesh,owner->colors));
    auto missing=owner->colors;missing.pop_back();
    CHECK_FALSE(BeautySourceSnapshot::share(owner,owner->mesh,missing));
    CHECK_FALSE(BeautySourceSnapshot::share(owner,indexed_triangle_set{},owner->colors));
}

TEST_CASE("Completed beauty preparation waits for selection and retires obsolete tasks", "[BeautyPersistence][BeautySourceSnapshot]") {
    BeautyPreparationTicket ticket{"first.glb","geometry-A"};
    CHECK(ticket.waits_for_selection(false,false,"first.glb","geometry-A","geometry-A"));
    CHECK_FALSE(ticket.accepts("first.glb","geometry-A","geometry-A",false));
    CHECK_FALSE(ticket.waits_for_selection(true,false,"first.glb","geometry-A","geometry-A"));
    CHECK(ticket.accepts("first.glb","geometry-A","geometry-A",true));
    CHECK_FALSE(ticket.waits_for_selection(false,true,"first.glb","geometry-A","geometry-A"));
    CHECK_FALSE(ticket.waits_for_selection(false,false,"first.glb","geometry-A","geometry-B"));
    ticket.cancel_if_changed("second.glb","geometry-B");
    CHECK_FALSE(ticket.waits_for_selection(false,false,"first.glb","geometry-A","geometry-A"));
    CHECK_FALSE(ticket.accepts("first.glb","geometry-A","geometry-A",true));
}

TEST_CASE("Prepared display rebinds only bitwise identical source geometry", "[ModelPreviewPuzzle][BeautySourceSnapshot]") {
    const auto owner=shared_beauty_source();const auto& source=owner->mesh;
    const auto surface=AI::BeautySurface::build(source,owner->colors);
    const auto puzzle=AI::BeautyPuzzle::create_regions(*surface);
    auto prepared=ModelPreviewPuzzle::prepare_initial(source,owner->colors,*surface,puzzle,UINT32_MAX);
    auto target=source;target.vertices[0].x()=.25f;
    CHECK_FALSE(prepared.rebind_identical_mesh(source,target));
    target=source;std::swap(target.indices[0][1],target.indices[0][2]);
    CHECK_FALSE(prepared.rebind_identical_mesh(source,target));
    target=source;target.vertices[0].x()=-0.f;
    CHECK_FALSE(prepared.rebind_identical_mesh(source,target));
    CHECK(prepared.matches(source,owner->colors,*surface,puzzle,UINT32_MAX,nullptr));
    target=source;
    REQUIRE(prepared.rebind_identical_mesh(source,target));
    CHECK(prepared.matches(target,owner->colors,*surface,puzzle,UINT32_MAX,nullptr));
    CHECK_FALSE(prepared.matches(source,owner->colors,*surface,puzzle,UINT32_MAX,nullptr));
}
