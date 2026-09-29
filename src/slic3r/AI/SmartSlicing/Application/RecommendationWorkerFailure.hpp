#pragma once

#include "CandidateSearchSessionPlanner.hpp"

#include <atomic>
#include <cstddef>

namespace Slic3r::AI::SmartSlicing {

inline constexpr const char* RECOMMENDATION_WORKER_EXCEPTION_CODE = "trial_worker_exception";

class RecommendationWorkerState
{
public:
    void mark_started() noexcept
    {
        m_failed.store(false, std::memory_order_release);
        m_running.store(true, std::memory_order_release);
    }

    void mark_completed() noexcept { m_running.store(false, std::memory_order_release); }

    void mark_failed() noexcept
    {
        m_failed.store(true, std::memory_order_release);
        m_running.store(false, std::memory_order_release);
    }

    bool running() const noexcept { return m_running.load(std::memory_order_acquire); }
    bool consume_failure() noexcept { return m_failed.exchange(false, std::memory_order_acq_rel); }

private:
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_failed{false};
};

size_t settle_recommendation_worker_exception(RecommendationSessionCoordinator& coordinator,
                                               const CandidateSearchSessionPlan& plan);

} // namespace Slic3r::AI::SmartSlicing
