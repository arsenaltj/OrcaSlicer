#pragma once

#include <algorithm>
#include <chrono>
#include <string>

namespace Slic3r::GUI {

// Local intent is deliberately separate from recovered job state. Merely
// displaying a saved job must never authorize another paid stage.
class SmartGenerationFlow
{
public:
    enum class Phase { Preparation, Geometry, Local, Complete };
    void begin(std::string id) {
        flow_id = std::move(id); phase = Phase::Preparation; progress = 0;
        armed = true; submitted = false; stopped = false;
        elapsed_base = 0; started = std::chrono::steady_clock::now();
    }
    void stop() { elapsed_base = elapsed(); started = {}; armed = false; stopped = true; }
    void restore(int checkpoint, int seconds, bool geometry) {
        progress = std::max(progress, std::min(99, checkpoint)); elapsed_base = std::max(elapsed_base, seconds);
        started = {}; armed = false; stopped = true; submitted = geometry;
        phase = geometry ? Phase::Geometry : Phase::Preparation;
    }
    void resume(bool geometry) {
        armed = true; stopped = false; submitted = geometry;
        if (geometry && phase == Phase::Preparation) phase = Phase::Geometry;
        started = std::chrono::steady_clock::now();
    }
    bool claim_geometry(bool eligible) {
        if (!armed || stopped || submitted || !eligible) return false;
        submitted = true; phase = Phase::Geometry; return true;
    }
    int observe(int stage_progress, bool local, bool loaded) {
        if (loaded) { phase = Phase::Complete; stop(); return progress = 100; }
        if (stopped) return progress;
        if (local) phase = Phase::Local;
        int mapped = phase == Phase::Preparation ? std::clamp(stage_progress, 0, 100) * 30 / 100 :
            phase == Phase::Local ? 90 + std::clamp(stage_progress - 90, 0, 9) :
            30 + std::clamp(stage_progress, 0, 100) * 60 / 100;
        return progress = std::max(progress, std::min(mapped, 99));
    }
    int elapsed() const {
        return elapsed_base + (started.time_since_epoch().count() == 0 ? 0 :
            int(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count()));
    }
    std::string flow_id;
    Phase phase {Phase::Preparation};
    int progress {0};
    bool armed {false}, submitted {false}, stopped {false};
private:
    std::chrono::steady_clock::time_point started {};
    int elapsed_base {0};
};

} // namespace Slic3r::GUI
