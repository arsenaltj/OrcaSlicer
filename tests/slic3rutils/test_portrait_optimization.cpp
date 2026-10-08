#include <catch2/catch_test_macros.hpp>
#include "slic3r/GUI/AI/ModelGeneration/PortraitOptimization.hpp"
#include "slic3r/GUI/AI/ModelGeneration/PortraitWorkerProgress.hpp"
#include "slic3r/GUI/AI/Model/BakedPortraitAppearance.hpp"
using namespace Slic3r::GUI;
namespace AI = Slic3r::AI;

TEST_CASE("Baked portrait imports reject changed files and unsaved local colors", "[PortraitOptimization]") {
    const std::string hash(64,'a');
    nlohmann::json semantic={{"geometry_id","mesh"},{"faces",{{1,{.8,.6,.4}}}},{"subfaces",nlohmann::json::array()}};
    std::map<std::string,AI::SemanticColoring::Color> cells={{"cell-1",{.8f,.6f,.4f}}};
    const auto baked=AI::baked_portrait_appearance(hash,semantic,cells,"partition");
    REQUIRE(AI::same_baked_portrait_appearance(baked,AI::baked_portrait_appearance(hash,semantic,cells,"partition")));
    REQUIRE_FALSE(AI::same_baked_portrait_appearance(nullptr,baked));
    REQUIRE_FALSE(AI::same_baked_portrait_appearance(baked,AI::baked_portrait_appearance(hash,semantic,cells,"changed-boundary")));
    REQUIRE_FALSE(AI::same_baked_portrait_appearance(baked,AI::baked_portrait_appearance(std::string(64,'b'),semantic,cells,"partition")));
    auto changed=cells;changed["cell-1"]={1.f,0.f,0.f};
    REQUIRE_FALSE(AI::same_baked_portrait_appearance(baked,AI::baked_portrait_appearance(hash,semantic,changed,"partition")));
    semantic["subfaces"].push_back({1,1,0,{1.,0.,0.}});
    REQUIRE_FALSE(AI::same_baked_portrait_appearance(baked,AI::baked_portrait_appearance(hash,semantic,cells,"partition")));
}

TEST_CASE("Portrait worker telemetry rejects stale identities and malformed counters", "[PortraitOptimization]") {
    nlohmann::json record={{"schema","orca.portrait-progress/v1"},{"request_id","current"},
        {"source_sha256","source"},{"geometry_id","mesh"},{"sequence",2},{"completed",2},{"total",8},
        {"stage","recognizing"},{"detail","front"}};
    uint64_t sequence=0; PortraitProgress progress;
    auto read=[&](const nlohmann::json& value) { return decode_portrait_worker_progress(value.dump(),"current","source","mesh",sequence,progress); };
    REQUIRE(read(record));
    REQUIRE(sequence==2);
    REQUIRE(progress.completed==2);
    REQUIRE_FALSE(read(record));
    record["sequence"]=3;
    for (const auto* field : {"request_id","source_sha256","geometry_id"}) {
        auto other=record; other[field]="previous";
        REQUIRE_FALSE(read(other));
        REQUIRE(sequence==2);
    }
    for (const auto* field : {"sequence","completed","total"}) {
        auto other=record; other[field]=-1;
        REQUIRE_FALSE(read(other));
        other[field]=1.5;
        REQUIRE_FALSE(read(other));
    }
    record["completed"]=9;
    REQUIRE_FALSE(read(record));
    record["completed"]=3;
    REQUIRE(read(record));
    REQUIRE_FALSE(decode_portrait_worker_progress("{partial","current","source","mesh",sequence,progress));
    REQUIRE(sequence==3);
    REQUIRE(progress.completed==3);
}

TEST_CASE("Portrait recognition starts only on an eligible first explicit entry", "[PortraitOptimization]") {
    REQUIRE(portrait_should_start_on_enter(true,false,true));
    REQUIRE_FALSE(portrait_should_start_on_enter(false,false,true));
    REQUIRE_FALSE(portrait_should_start_on_enter(true,true,true));
    REQUIRE_FALSE(portrait_should_start_on_enter(true,false,false));
}
TEST_CASE("Portrait progress preserves stages and waits for rendered candidate completion", "[PortraitOptimization]") {
    PortraitOptimizationTask task("request","asset",7);
    REQUIRE(task.snapshot().percent == -1);
    REQUIRE(task.snapshot().remaining_seconds == -1);
    task.report({PortraitStage::Ownership,"voting",2,3});
    const int progress = task.snapshot().percent;
    REQUIRE(progress > 0);
    task.report({PortraitStage::Recognizing,"late view",9,10});
    REQUIRE(task.snapshot().progress.stage == PortraitStage::Ownership);
    task.report({PortraitStage::Saving,"writing",1,1});
    REQUIRE(task.snapshot().percent < 100);
    task.report({PortraitStage::Preview,"waiting for GL",1,1});
    REQUIRE(task.snapshot().percent < 100);
    task.evidence("Unconfirmed regions retain appearance",true);
    task.finish(PortraitOutcome::Ready);
    REQUIRE(task.snapshot().outcome == PortraitOutcome::Partial);
    REQUIRE(task.snapshot().percent == 100);
}
TEST_CASE("Cancelled and unsaved portrait results cannot become successful through late updates", "[PortraitOptimization]") {
    PortraitOptimizationTask task("request","asset",8);
    task.finish(PortraitOutcome::DraftOnly,"shared UV conflict");
    task.report({PortraitStage::Preview,"late",1,1});
    task.finish(PortraitOutcome::Ready);
    REQUIRE(task.snapshot().outcome == PortraitOutcome::DraftOnly);
    REQUIRE(task.snapshot().percent != 100);
    task.finish(PortraitOutcome::Cancelled,"discarded");
    REQUIRE(task.snapshot().outcome == PortraitOutcome::Cancelled);
    task.finish(PortraitOutcome::Ready);
    REQUIRE(task.snapshot().outcome == PortraitOutcome::Cancelled);
}
TEST_CASE("Portrait estimates require complete matching phase samples", "[PortraitOptimization]") {
    PortraitOptimizationTask task("request","asset",9);
    task.history("current-source-runtime-policy-cold",{{1,2,3,4,5,0}});
    REQUIRE(task.snapshot().remaining_seconds == -1);
    task.history("current-source-runtime-policy-cold",{{1,2,3,4,5,6}});
    REQUIRE(task.snapshot().remaining_seconds >= 0);
    REQUIRE(task.snapshot().percent == -1);
    REQUIRE(task.history_key() == "current-source-runtime-policy-cold");
}
