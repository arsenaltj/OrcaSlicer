#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace Slic3r::GUI::ModelGenerationPresentation {

struct SubmissionContext
{
    std::string job_id;
    std::string provider;
    uint64_t sequence { 0 };

    bool matches(const std::string& job, const std::string& selected_provider, uint64_t generation) const
    {
        return !job_id.empty() && job_id == job && provider == selected_provider && sequence == generation;
    }
};

// A rejected POST and an unchanged preview GET describe different facts. Keep
// the rejection until an explicit new submission or authoritative advancement.
// This state only controls presentation; it never requests a provider retry.
class SubmissionState
{
public:
    void begin(SubmissionContext context)
    {
        m_context = std::move(context);
        m_error.clear();
    }

    void clear() { begin({}); }

    bool retain_error(const SubmissionContext& context, const std::string& error)
    {
        if (!matches(context)) return false;
        if (m_error.empty()) m_error = error;
        return true;
    }

    void reconcile(const SubmissionContext& context, const std::string& state)
    {
        if (!matches(context)) return;
        if (state == "queued" || state == "running" || state == "ready" ||
            state == "failed" || state == "stopping" || state == "stopped")
            m_error.clear();
    }

    std::string error(const SubmissionContext& context) const
    {
        return matches(context) ? m_error : std::string();
    }

private:
    bool matches(const SubmissionContext& context) const
    {
        return m_context.matches(context.job_id, context.provider, context.sequence);
    }

    SubmissionContext m_context;
    std::string m_error;
};

inline bool is_model_provider_not_configured(const std::string& error)
{
    // The client currently exposes the sidecar's message, not its error code.
    return error == "Model generation is not configured.";
}

} // namespace Slic3r::GUI::ModelGenerationPresentation
