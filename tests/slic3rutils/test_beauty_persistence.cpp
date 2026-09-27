#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "slic3r/GUI/AI/ModelGeneration/BeautyDraftQueue.hpp"
#include "slic3r/GUI/AI/ModelGeneration/BeautyWorkbenchControls.hpp"
#include "slic3r/GUI/AI/Model/BeautyPrintColorHandoff.hpp"
#include "slic3r/GUI/AI/ModelGeneration/LocalPrintColorMatching.hpp"
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <future>

using namespace Slic3r;
using namespace Slic3r::GUI;

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
