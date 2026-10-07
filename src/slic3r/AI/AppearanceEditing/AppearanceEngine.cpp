#include "AppearanceEngine.hpp"
#include "BaselineFinishing.hpp"

namespace Slic3r::AI::AppearanceEditing {
namespace {
class BaselineAppearanceEngine final : public IAppearanceEngine {
public:
    const char* algorithm_id() const noexcept override { return "appearance-baseline"; }
    const char* algorithm_version() const noexcept override { return "appearance-baseline-v1"; }
    ModelFinishingResult process(const Request& request) const override
    {
        ModelFinishingResult result;
        switch (request.operation) {
        case FinishingOperation::Obj:
            result = finish_model_obj_baseline(request.source, request.destination, request.options, request.canceled);
            break;
        case FinishingOperation::Artifact:
            result = finish_model_artifact_baseline(request.source, request.destination, request.options, request.canceled);
            break;
        case FinishingOperation::Beauty:
            result = finish_beauty_artifact_baseline(request.source, request.destination, request.options, request.canceled);
            break;
        }
        result.preserves_face_order = result.success && result.faces_before == result.faces_after
            && result.removed_degenerate_faces == 0 && result.removed_duplicate_faces == 0;
        return result;
    }
};
}

std::shared_ptr<const IAppearanceEngine> baseline_engine()
{
    static const auto engine = std::make_shared<const BaselineAppearanceEngine>();
    return engine;
}

ModelFinishingResult process(const Request& request)
{
    const auto engine = request.options.engine ? request.options.engine : baseline_engine();
    auto result = engine->process(request);
    result.algorithm_id = engine->algorithm_id();
    result.algorithm_version = engine->algorithm_version();
    return result;
}
}

namespace Slic3r::AI {
ModelFinishingResult finish_model_obj(const boost::filesystem::path& source,
    const boost::filesystem::path& destination, const ModelFinishingOptions& options,
    const std::function<bool()>& canceled)
{
    return AppearanceEditing::process({AppearanceEditing::FinishingOperation::Obj, source, destination, options, canceled});
}

ModelFinishingResult finish_model_artifact(const boost::filesystem::path& source,
    const boost::filesystem::path& destination, const ModelFinishingOptions& options,
    const std::function<bool()>& canceled)
{
    return AppearanceEditing::process({AppearanceEditing::FinishingOperation::Artifact, source, destination, options, canceled});
}

ModelFinishingResult finish_beauty_artifact(const boost::filesystem::path& source,
    const boost::filesystem::path& destination, const ModelFinishingOptions& options,
    const std::function<bool()>& canceled)
{
    return AppearanceEditing::process({AppearanceEditing::FinishingOperation::Beauty, source, destination, options, canceled});
}
}
