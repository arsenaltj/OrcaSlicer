#include <catch2/catch_test_macros.hpp>
#include "slic3r/AI/AppearanceEditing/AppearanceEngine.hpp"
#include "slic3r/AI/ModelArtifacts/ModelArtifact.hpp"
#include "slic3r/GUI/AI/Model/ModelFinishing.hpp"
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>

using namespace Slic3r;
namespace Appearance = AI::AppearanceEditing;

namespace {
struct ModelFixture {
    boost::filesystem::path directory = boost::filesystem::temp_directory_path()
        / boost::filesystem::unique_path("orca-appearance-module-%%%%-%%%%-%%%%");
    boost::filesystem::path source = directory / "source.obj", output = directory / "candidate.obj";
    explicit ModelFixture(bool duplicate = false)
    {
        boost::filesystem::create_directory(directory);
        boost::filesystem::ofstream file(source);
        file << "v 0 0 0\nv 10 0 0\nv 0 10 0\nf 1 2 3\n";
        if (duplicate) file << "f 1 2 3\n";
    }
    ~ModelFixture()
    {
        boost::system::error_code error;
        boost::filesystem::remove_all(directory, error);
    }
};
class RecordingAppearanceEngine final : public Appearance::IAppearanceEngine {
public:
    mutable size_t calls {0};
    mutable Appearance::FinishingOperation operation {Appearance::FinishingOperation::Beauty};
    const char* algorithm_id() const noexcept override { return "recording-appearance"; }
    const char* algorithm_version() const noexcept override { return "recording-appearance-v1"; }
    AI::ModelFinishingResult process(const Appearance::Request& request) const override
    {
        ++calls;
        operation = request.operation;
        return Appearance::baseline_engine()->process(request);
    }
};
}

TEST_CASE("The legacy artifact entry uses one captured appearance strategy and preserves the source", "[AppearanceEngine]")
{
    ModelFixture fixture(true);
    const auto source_hash = AI::model_artifact_sha256(fixture.source);
    AI::ModelFinishingOptions options;
    options.smooth_surface = false;
    const auto engine = std::make_shared<RecordingAppearanceEngine>();
    options.engine = engine;
    const auto result = AI::finish_model_artifact(fixture.source, fixture.output, options);
    REQUIRE(result.success);
    CHECK(engine->calls == 1);
    CHECK(engine->operation == Appearance::FinishingOperation::Artifact);
    CHECK(result.algorithm_id == "recording-appearance");
    CHECK(result.algorithm_version == "recording-appearance-v1");
    CHECK(result.removed_duplicate_faces == 1);
    CHECK_FALSE(result.preserves_face_order);
    CHECK(AI::model_artifact_sha256(fixture.source) == source_hash);
    CHECK(boost::filesystem::exists(fixture.output));
}

TEST_CASE("Baseline appearance reports preserved face correspondence for unchanged topology", "[AppearanceEngine]")
{
    ModelFixture fixture;
    AI::ModelFinishingOptions options;
    options.smooth_surface = false;
    const auto result = AI::finish_model_obj(fixture.source, fixture.output, options);
    REQUIRE(result.success);
    CHECK(result.preserves_face_order);
    CHECK(result.algorithm_id == "appearance-baseline");
    CHECK(result.algorithm_version == "appearance-baseline-v1");
    CHECK(result.faces_before == result.faces_after);
}

TEST_CASE("Canceled appearance processing leaves the source and destination untouched", "[AppearanceEngine]")
{
    ModelFixture fixture;
    const auto source_hash = AI::model_artifact_sha256(fixture.source);
    AI::ModelFinishingOptions options;
    const auto result = AI::finish_model_artifact(fixture.source, fixture.output, options, [] { return true; });
    CHECK_FALSE(result.success);
    CHECK(result.canceled);
    CHECK_FALSE(result.preserves_face_order);
    CHECK_FALSE(boost::filesystem::exists(fixture.output));
    CHECK(AI::model_artifact_sha256(fixture.source) == source_hash);
}
