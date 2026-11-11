#pragma once
namespace Slic3r::GUI {
enum class ModelGenerationSubmissionResult { Submitted, Pending, Cancelled, Rejected };
inline bool submission_accepted(ModelGenerationSubmissionResult value) {
    return value == ModelGenerationSubmissionResult::Submitted || value == ModelGenerationSubmissionResult::Pending;
}
}
