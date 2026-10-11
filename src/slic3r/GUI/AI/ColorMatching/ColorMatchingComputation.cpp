#include "ColorMatchingComputation.hpp"
#include "slic3r/GUI/AI/ModelGeneration/LocalPrintColorBoundaryRefinement.hpp"
#include "slic3r/GUI/AI/ModelGeneration/LocalSemanticWorkerClient.hpp"
#include "slic3r/GUI/AI/Model/LocalSemanticDraft.hpp"
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
    const auto& config_path = request.config;
    const auto& module_directory = request.modules;
    const auto& temporary_parent = request.temporary_parent;
    const auto& semantic_cache = request.semantic_cache;
    const auto recognize_details = request.recognize_details;
    const auto expected_source = task.identity.source_sha256;
    if (!topology) return {{ {}, "Missing frozen color topology", false }, {}, {}, {}};
    Matching::Computation result;
    std::shared_ptr<const LocalSemanticEvidence::Evidence> evidence;
    std::string notice;
    fs::path owned_request;
    try {
        if (recognize_details) {
            try {
                LocalSemanticWorker::Configuration config; std::string reason;
                if (!fs::exists(config_path)) notice = "未启用本地细节识别，按原色匹配。\n";
                else if (!LocalSemanticWorker::read_configuration(config_path, config, reason))
                    notice = "本地识别配置不可用，按原色匹配。\n";
                else if (!config.enabled) notice = "本地细节识别已关闭，按原色匹配。\n";
                else if (AI::model_artifact_format(source) != "glb")
                    notice = "此文件暂不支持本地细节识别，按原色匹配。\n";
                else {
                    fs::create_directories(temporary_parent);
                    const auto request = temporary_parent / fs::unique_path("page-%%%%-%%%%-%%%%");
                    if (!fs::create_directory(request)) throw std::runtime_error("Unable to create an owned local request.");
                    owned_request = request;
                    auto analyzed = LocalSemanticWorker::analyze(config, module_directory, owned_request, source, *topology, cancel, semantic_cache);
                    if (analyzed.process.status == LocalSemanticWorker::Status::Cancelled) result.cancelled = true;
                    else if (analyzed.process.status == LocalSemanticWorker::Status::Ready) {
                        std::vector<AI::PrintColorRegion> merged;
                        if (LocalSemanticDraft::merge_regions(task.identity, analyzed.evidence, merged, reason)) {
                            task.identity.regions = std::move(merged);
                            evidence = std::make_shared<const LocalSemanticEvidence::Evidence>(std::move(analyzed.evidence));
                            if (analyzed.cache_hit) notice = "已复用通过校验的本地识别结果，本次未运行识别模型。\n";
                        } else notice = "识别结果与当前局部设置不兼容，保留现有设置匹配。\n";
                    } else if (analyzed.process.status == LocalSemanticWorker::Status::TimedOut)
                        notice = "本地细节识别超时，按现有原色和局部设置匹配。\n";
                    else notice = "本地细节识别未完成，按现有原色和局部设置匹配。\n";
                }
            } catch (const std::exception&) {
                notice = "本地细节识别不可用，按现有原色和局部设置匹配。\n";
            }
        }
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
    // These are disposable files from this one owned invocation. Original
    // assets and version records never live beneath this request root.
    std::string cleanup_notice;
    if (!owned_request.empty()) {
        std::string reason;
        if (!LocalSemanticWorker::cleanup_request(owned_request, temporary_parent, reason))
            cleanup_notice = "本次识别的临时文件未能完全清理。\n";
    }
    return {std::move(result), std::move(evidence), std::move(notice), std::move(cleanup_notice)};
}
}
