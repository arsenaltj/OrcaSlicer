#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace Slic3r::GUI {
enum class SemanticMode { General, Portrait };
enum class PortraitStage { Preparing, Recognizing, Ownership, Coloring, Saving, Preview };
enum class PortraitOutcome { Idle, Running, Ready, Partial, DraftOnly, Failed, Cancelled };
struct PortraitProgress {
    PortraitStage stage {PortraitStage::Preparing};
    std::string detail;
    uint64_t completed {0}, total {0};
};
using PortraitProgressCallback = std::function<void(const PortraitProgress&)>;
struct PortraitOptimizationState {
    std::string request_id, asset_id, evidence, reason;
    uint64_t revision {0};
    PortraitOutcome outcome {PortraitOutcome::Idle};
    PortraitProgress progress;
    double elapsed_seconds {0}, remaining_seconds {-1};
    int percent {-1};
    bool running() const { return outcome == PortraitOutcome::Running; }
};

// Shared by recognition/export workers; UI ownership and publication stay in the host.
// Stage counters describe completed work. Time never advances a percentage.
class PortraitOptimizationTask {
public:
    using Clock = std::chrono::steady_clock;
    using Durations = std::array<double, 6>;
    PortraitOptimizationTask(std::string request, std::string asset, uint64_t revision)
        : m_started(Clock::now()), m_stage_started(m_started) {
        m_state.request_id = std::move(request); m_state.asset_id = std::move(asset);
        m_state.revision = revision; m_state.outcome = PortraitOutcome::Running;
    }
    void report(const PortraitProgress& value) {
        std::lock_guard<std::mutex> guard(m_mutex);
        if (!m_state.running() || int(value.stage) < int(m_state.progress.stage)) return;
        const auto now = Clock::now();
        if (value.stage != m_state.progress.stage) {
            m_durations[size_t(m_state.progress.stage)] += seconds(now - m_stage_started);
            m_stage_started = now;
        }
        m_state.progress = value;
        if (value.total && value.completed > value.total) m_state.progress.completed = value.total;
    }
    void evidence(std::string text, bool partial = false) {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_state.evidence = std::move(text); m_partial = m_partial || partial;
    }
    void history(std::string key, std::vector<Durations> samples) {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_history_key = std::move(key); m_samples = std::move(samples);
        if (m_samples.size() > 5) m_samples.erase(m_samples.begin(), m_samples.end() - 5);
    }
    std::string history_key() const { std::lock_guard<std::mutex> guard(m_mutex); return m_history_key; }
    Durations durations() const { std::lock_guard<std::mutex> guard(m_mutex); return m_durations; }
    void finish(PortraitOutcome outcome, std::string reason = {}) {
        std::lock_guard<std::mutex> guard(m_mutex);
        if (!m_state.running()) {
            if (m_state.outcome == PortraitOutcome::DraftOnly && outcome == PortraitOutcome::Cancelled) {
                m_state.outcome = outcome; m_state.reason = std::move(reason);
            }
            return;
        }
        const auto now = Clock::now();
        m_durations[size_t(m_state.progress.stage)] += seconds(now - m_stage_started);
        m_state.elapsed_seconds = seconds(now - m_started);
        m_state.outcome = outcome == PortraitOutcome::Ready && m_partial ? PortraitOutcome::Partial : outcome;
        m_state.reason = std::move(reason);
    }
    PortraitOptimizationState snapshot() const {
        std::lock_guard<std::mutex> guard(m_mutex);
        auto state = m_state;
        const bool ready = state.outcome == PortraitOutcome::Ready || state.outcome == PortraitOutcome::Partial;
        state.percent = ready ? 100 : -1;
        if (!state.running()) return state;
        const auto now = Clock::now();
        state.elapsed_seconds = seconds(now - m_started);
        const auto& progress = state.progress;
        if (progress.total)
            state.percent = std::min(99, int((int(progress.stage) * 100.0 + 100.0 * progress.completed / progress.total) / 6.0));
        if (!m_samples.empty()) {
            double remaining = 0;
            for (size_t stage = size_t(progress.stage); stage < 6; ++stage) {
                std::vector<double> values;
                for (const auto& sample : m_samples) if (sample[stage] > 0 && std::isfinite(sample[stage])) values.push_back(sample[stage]);
                if (values.empty()) { remaining = -1; break; }
                std::sort(values.begin(), values.end());
                double duration = values[values.size()/2];
                if (stage == size_t(progress.stage)) {
                    const auto elapsed = seconds(now - m_stage_started);
                    duration = progress.total && progress.completed > 0
                        ? elapsed * double(progress.total - progress.completed) / double(progress.completed)
                        : std::max(0.0, duration - elapsed);
                }
                remaining += duration;
            }
            state.remaining_seconds = remaining;
        }
        return state;
    }
private:
    static double seconds(Clock::duration duration) { return std::chrono::duration<double>(duration).count(); }
    mutable std::mutex m_mutex;
    PortraitOptimizationState m_state;
    Clock::time_point m_started, m_stage_started;
    Durations m_durations {};
    std::vector<Durations> m_samples;
    std::string m_history_key;
    bool m_partial {false};
};

inline bool portrait_should_start_on_enter(bool first_entry, bool restored, bool available) {
    return first_entry && !restored && available;
}
} // namespace Slic3r::GUI
