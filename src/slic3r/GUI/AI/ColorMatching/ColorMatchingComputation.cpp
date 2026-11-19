#include "ColorMatchingComputation.hpp"
#include "slic3r/GUI/AI/ModelGeneration/LocalPrintColorBoundaryRefinement.hpp"
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include <boost/filesystem.hpp>

namespace Slic3r::GUI {
namespace Matching = LocalPrintColorMatching;
namespace fs = boost::filesystem;
ColorMatchingComputationResult compute_color_matching(ColorMatchingComputationRequest request, std::atomic<bool>& cancel)
{
    auto task = std::move(request.input);
    auto recipe_input = std::move(request.recipes);
    task.cancelled = [&cancel] { return cancel.load(); };
    recipe_input.cancelled = task.cancelled;
    const auto& topology = request.topology;
    const auto& source = request.source;
    const auto expected_source = task.identity.source_sha256;
    if (!topology) return {{ {}, "Missing frozen color topology", false }};
    Matching::Computation result;
    try {
        if (cancel.load()) result.cancelled = true;
        if (!result.cancelled && AI::print_color_mode(task.identity.requested_color_count) == AI::PrintColorMode::Layered) {
            auto catalog = LocalPrintColorRecipes::enumerate(recipe_input);
            if (catalog.cancelled) result.cancelled = true;
            task.recipe_catalog = std::make_shared<const LocalPrintColorRecipes::Catalog>(std::move(catalog));
        }
        if (!result.cancelled)
            result = LocalPrintColorBoundaryRefinement::compute_guarded(task, *topology);
        if (result.ok() && AI::model_artifact_sha256(source) != expected_source)
            result.error = "Original model changed during color matching. Reopen the model.";
    } catch (const std::exception& error) { result.error = error.what(); }
    return {std::move(result)};
}
}
