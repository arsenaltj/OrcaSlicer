#include <catch2/catch_all.hpp>

#include "slic3r/AI/Contracts/GeneratedModelArtifact.hpp"
#include "slic3r/AI/Contracts/IModelArtifactConsumer.hpp"
#include "slic3r/AI/Contracts/IPrintablePaletteProvider.hpp"
#include "slic3r/AI/ModelGeneration/GeneratedModelArtifact.hpp"
#include "slic3r/AI/ModelGeneration/IPrintablePaletteProvider.hpp"
#include "slic3r/AI/SmartSlicing/IModelArtifactConsumer.hpp"

#include <type_traits>

using namespace Slic3r::AI;

namespace {

class RecordingConsumer final : public IModelArtifactConsumer
{
public:
    ModelImportResult import_artifact(const ModelImportRequest& request) override
    {
        ModelImportResult result;
        if (request.artifact.job_id.empty())
            result.outcome = ModelImportOutcome::InvalidArtifact;
        else
            result.outcome = ModelImportOutcome::Imported;
        result.color_mode         = request.color_mode;
        result.slice_after_import = request.auto_slice_after_import;
        return result;
    }
};

class FixedPaletteProvider final : public IPrintablePaletteProvider
{
public:
    PrintablePaletteSnapshot printable_palette() const override
    {
        return {{"#112233"}, {0}, {0}, {"#112233"}};
    }
};

} // namespace

TEST_CASE("neutral AI contracts preserve accepted defaults and legacy includes", "[AIContracts]")
{
    static_assert(std::is_abstract_v<IModelArtifactConsumer>);
    static_assert(std::is_abstract_v<IPrintablePaletteProvider>);

    ModelImportRequest request;
    CHECK(request.color_mode == ImportColorMode::ManualMatch);
    CHECK(request.auto_slice_after_import);
    CHECK_FALSE(request.artifact.used_printable_colors);

    request.artifact.job_id = "accepted-job";
    request.color_mode      = ImportColorMode::AutoMap;
    RecordingConsumer consumer;
    const ModelImportResult result = consumer.import_artifact(request);
    CHECK(result.imported());
    CHECK(result.color_mode == ImportColorMode::AutoMap);
    CHECK(result.slice_after_import);

    const PrintablePaletteSnapshot palette = FixedPaletteProvider().printable_palette();
    REQUIRE(palette.compatible_colors.size() == 1);
    CHECK(palette.compatible_colors.front() == "#112233");
}
