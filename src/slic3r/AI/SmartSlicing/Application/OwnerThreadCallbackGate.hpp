#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

namespace Slic3r::AI::SmartSlicing {

class OwnerThreadCallbackGate
{
public:
    OwnerThreadCallbackGate() : m_state(std::make_shared<State>(std::this_thread::get_id())) {}

    std::function<void()> guard(std::function<void()> callback) const
    {
        const std::weak_ptr<State> state = m_state;
        return [state, callback = std::move(callback)] {
            const std::shared_ptr<State> locked = state.lock();
            if (!locked || !locked->alive.load(std::memory_order_acquire))
                return;
            if (std::this_thread::get_id() != locked->owner_thread)
                throw std::logic_error("owner_thread_callback_required");
            callback();
        };
    }

    void close()
    {
        assert_owner_thread();
        m_state->alive.store(false, std::memory_order_release);
    }

    bool alive() const { return m_state->alive.load(std::memory_order_acquire); }

    void assert_owner_thread() const
    {
        if (std::this_thread::get_id() != m_state->owner_thread)
            throw std::logic_error("owner_thread_required");
    }

private:
    struct State
    {
        explicit State(std::thread::id owner) : owner_thread(owner) {}

        const std::thread::id owner_thread;
        std::atomic<bool> alive{true};
    };

    std::shared_ptr<State> m_state;
};

} // namespace Slic3r::AI::SmartSlicing
