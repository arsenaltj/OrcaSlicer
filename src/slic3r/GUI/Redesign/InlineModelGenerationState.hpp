#pragma once

#include "../AI/ModelGeneration/ModelGenerationHost.hpp"
#include "../AI/ModelGeneration/WorkbenchOpenRequest.hpp"
#include <optional>
#include <set>
#include <tuple>

namespace Slic3r::GUI {

enum class CreationSource { Smart, Advanced };

struct GenerationBinding {
    CreationSource source {CreationSource::Smart};
    std::string context;
    std::uint64_t epoch {0};

    bool operator==(const GenerationBinding& other) const {
        return std::tie(source, context, epoch) == std::tie(other.source, other.context, other.epoch);
    }
};

struct InlineModelArtifact {
    WorkbenchOpenRequest request;
    bool operator==(const InlineModelArtifact& other) const {
        return request.entry.job_id == other.request.entry.job_id &&
            request.entry.model_path.lexically_normal() == other.request.entry.model_path.lexically_normal() &&
            request.sha256 == other.request.sha256 &&
            request.entry.palette == other.request.entry.palette &&
            request.entry.palette_roles == other.request.entry.palette_roles &&
            request.entry.use_printable_colors == other.request.entry.use_printable_colors &&
            request.entry.color_intent_path.lexically_normal() == other.request.entry.color_intent_path.lexically_normal() &&
            request.entry.color_intent_schema == other.request.entry.color_intent_schema &&
            request.entry.color_intent_sha256 == other.request.entry.color_intent_sha256;
    }
};

enum class InlinePreviewPhase { Empty, Preparing, AwaitingFirstFrame, Ready, Failed };

inline bool inline_generation_active(const ModelGenerationUIState& state) {
    return state.stage == ModelGenerationUIStage::GeneratingDesign ||
        state.stage == ModelGenerationUIStage::GeneratingModel ||
        state.stage == ModelGenerationUIStage::LoadingModel ||
        state.stage == ModelGenerationUIStage::Stopping;
}

inline bool inline_generation_handoff(const ModelGenerationUIState& state) {
    // Smart preparation publishes its confirmed design before dispatching 3D.
    // Retain the feedback through that display handoff without changing the
    // host's busy flag, command eligibility or submission sequence.
    return state.smart_mode && state.model_generation_context && !state.smart_flow_id.empty() &&
        (state.stage == ModelGenerationUIStage::DesignReady || state.stage == ModelGenerationUIStage::Saving3DOptions);
}

inline std::string inline_generation_task(const ModelGenerationUIState& state) {
    return state.smart_mode && !state.smart_flow_id.empty() ? state.smart_flow_id : state.job_id;
}

// Local presentation only. Hosts retain task eligibility, persistence and commands.
class InlineModelGenerationState {
public:
    bool apply(const GenerationBinding& binding, const ModelGenerationUIState& state,
               std::optional<InlineModelArtifact> artifact) {
        const bool same_context = m_bound && binding == m_binding;
        if (same_context && (state.revision < m_state.revision ||
            state.model_generation_session < m_state.model_generation_session)) return false;
        const auto task = inline_generation_task(state);
        const bool new_task = !same_context ||
            state.model_generation_session != m_state.model_generation_session ||
            (!task.empty() && !m_task.empty() && task != m_task);
        if (state.stage != ModelGenerationUIStage::ModelReady || !state.model_ready || state.busy ||
            (artifact && artifact->request.entry.job_id != state.model_asset_id))
            artifact.reset();
        const bool same_artifact = artifact && m_artifact && *artifact == *m_artifact;
        if (new_task || !same_artifact) {
            // Repeated progress snapshots without an artifact must not restart workers.
            if (new_task || artifact || m_artifact) {
                ++m_generation;
                m_phase = InlinePreviewPhase::Empty;
            }
        }
        m_bound = true; m_binding = binding; m_state = state; m_task = task;
        m_artifact = std::move(artifact);
        return true;
    }

    void invalidate() {
        ++m_generation; m_bound = false; m_artifact.reset();
        m_phase = InlinePreviewPhase::Empty; m_state = {}; m_task.clear();
    }
    std::uint64_t retry() { m_phase = InlinePreviewPhase::Empty; return ++m_generation; }
    // Only an accepted generation command arms a success notice. Observing or
    // downloading an existing history job must not masquerade as a new result.
    void generation_started() {
        if (m_bound && !m_task.empty() && m_state.model_generation_context && inline_generation_active(m_state))
            m_generated.emplace(m_binding.source, m_binding.context, m_task);
    }
    bool advance(std::uint64_t generation, InlinePreviewPhase phase) {
        if (generation != m_generation || !m_artifact) return false;
        const bool allowed =
            (phase == InlinePreviewPhase::Preparing && m_phase == InlinePreviewPhase::Empty) ||
            (phase == InlinePreviewPhase::AwaitingFirstFrame && m_phase == InlinePreviewPhase::Preparing) ||
            (phase == InlinePreviewPhase::Ready && m_phase == InlinePreviewPhase::AwaitingFirstFrame) ||
            (phase == InlinePreviewPhase::Failed && (m_phase == InlinePreviewPhase::Preparing || m_phase == InlinePreviewPhase::AwaitingFirstFrame));
        if (!allowed) return false;
        m_phase = phase; return true;
    }
    bool ready() const { return m_artifact && m_phase == InlinePreviewPhase::Ready; }
    bool consume_completion(bool visible) {
        if (!visible || !ready() || !m_generated.count({m_binding.source, m_binding.context, m_task})) return false;
        const auto& request = m_artifact->request;
        return m_notified.emplace(m_binding.source, m_binding.context, m_task, request.entry.job_id,
            request.entry.model_path.lexically_normal().generic_string(), request.sha256).second;
    }
    bool has_content() const {
        return m_artifact || inline_generation_active(m_state) || inline_generation_handoff(m_state) ||
            (m_state.model_generation_context && !m_state.model_path.empty()) ||
            (m_state.model_generation_context && (m_state.stage == ModelGenerationUIStage::Failed ||
                m_state.stage == ModelGenerationUIStage::Stopped));
    }
    std::uint64_t generation() const { return m_generation; }
    InlinePreviewPhase phase() const { return m_phase; }
    const ModelGenerationUIState& snapshot() const { return m_state; }
    const GenerationBinding& binding() const { return m_binding; }
    const std::optional<InlineModelArtifact>& artifact() const { return m_artifact; }

private:
    GenerationBinding m_binding;
    ModelGenerationUIState m_state;
    std::string m_task;
    std::optional<InlineModelArtifact> m_artifact;
    InlinePreviewPhase m_phase {InlinePreviewPhase::Empty};
    std::uint64_t m_generation {0};
    bool m_bound {false};
    std::set<std::tuple<CreationSource, std::string, std::string>> m_generated;
    std::set<std::tuple<CreationSource, std::string, std::string, std::string,
        std::string, std::string>> m_notified;
};

} // namespace Slic3r::GUI
