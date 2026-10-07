#pragma once

#include "ModelFinishing.hpp"

namespace Slic3r::AI::AppearanceEditing {
enum class FinishingOperation { Obj, Artifact, Beauty };

// Synchronous processing of an application-owned immutable snapshot. References
// live until process() returns; avoid copying potentially large edit arrays.
struct Request {
    FinishingOperation operation;
    const boost::filesystem::path& source;
    const boost::filesystem::path& destination;
    const ModelFinishingOptions& options;
    const std::function<bool()>& canceled;
};

class IAppearanceEngine {
public:
    virtual ~IAppearanceEngine() = default;
    virtual const char* algorithm_id() const noexcept = 0;
    virtual const char* algorithm_version() const noexcept = 0;
    // Writes a new candidate only; source, history and project remain owned by
    // the application. Report whether source face order is still meaningful.
    virtual ModelFinishingResult process(const Request& request) const = 0;
};

std::shared_ptr<const IAppearanceEngine> baseline_engine();
ModelFinishingResult process(const Request& request);
}
