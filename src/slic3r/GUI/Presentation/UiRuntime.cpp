#include "UiRuntime.hpp"
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <stdexcept>
#include <vector>
#include <map>
#include <cstdlib>
#include <tbb/task_arena.h>
#include <boost/log/trivial.hpp>
#include <wx/app.h>
#include <wx/thread.h>
#include <wx/utils.h>

namespace Slic3r::GUI::Presentation {
namespace {
bool latency_trace_enabled() {
    const char* value = std::getenv("ORCASLICER_UI_LATENCY_TRACE");
    return value && std::string(value) == "1";
}
double milliseconds_since(std::chrono::steady_clock::time_point from) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - from).count();
}
}
void assert_gui_thread() {
    if (!wxIsMainThread()) throw std::logic_error("Presentation must run on the GUI thread");
}
void post_gui(std::function<void()> callback) {
    if (wxTheApp) wxTheApp->CallAfter([callback = std::move(callback)] {
        assert_gui_thread(); callback();
    });
}

struct RefreshScheduler::State {
    RefreshState dirty;
    bool posted {false}, stopped {false}, committing {false};
    std::optional<std::chrono::steady_clock::time_point> invalidated;
    std::function<bool()> visible;
    Commit commit;
    const char* name {"surface"};
    Dispatcher post;
};
RefreshScheduler::RefreshScheduler(Dispatcher dispatcher) : m_state(std::make_shared<State>()) { m_state->post = std::move(dispatcher); }
RefreshScheduler::~RefreshScheduler() { stop(); }
void RefreshScheduler::configure(std::function<bool()> visible, Commit commit, const char* name) {
    assert_gui_thread(); m_state->visible = std::move(visible); m_state->commit = std::move(commit); m_state->name = name;
}
void RefreshScheduler::stop() {
    m_state->stopped = true; m_state->visible = {}; m_state->commit = {};
}
void RefreshScheduler::request(Dirty flags) {
    assert_gui_thread(); if (m_state->stopped) return;
    if (!m_state->invalidated) m_state->invalidated = std::chrono::steady_clock::now();
    m_state->dirty.invalidate(flags); schedule(m_state);
}
void RefreshScheduler::resume() { assert_gui_thread(); schedule(m_state); }
void RefreshScheduler::flush(Dirty flags) {
    assert_gui_thread();
    if (m_state->stopped) return;
    if (!m_state->invalidated) m_state->invalidated = std::chrono::steady_clock::now();
    m_state->dirty.invalidate(flags | Dirty::Barrier);
    commit_pending(m_state);
}
void RefreshScheduler::schedule(const std::shared_ptr<State>& state) {
    if (state->stopped || state->posted || state->committing || !state->commit || !state->dirty.dirty()) return;
    if (state->visible && !state->visible()) return;
    state->posted = true;
    state->post([state] {
        state->posted = false;
        commit_pending(state);
    });
}
void RefreshScheduler::commit_pending(const std::shared_ptr<State>& state) {
        if (state->stopped || state->committing || !state->commit || !state->dirty.dirty() ||
            (state->visible && !state->visible())) return;
        state->committing = true;
        const auto flags = state->dirty.take();
        const auto started = std::chrono::steady_clock::now();
        const auto invalidated = state->invalidated.value_or(started);
        state->invalidated.reset();
        // Copy: disposal during the commit must not destroy the active callable.
        const auto commit = state->commit;
        try { commit(flags); }
        catch (...) {
            state->committing = false;
            state->dirty.invalidate(flags);
            throw;
        }
        const double elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        wxString trace;
        if (wxGetEnv("ORCASLICER_UI_LATENCY_TRACE", &trace) && trace == "1")
            BOOST_LOG_TRIVIAL(info) << "[UiCommit] surface=" << state->name << " flags=" << unsigned(flags)
                << " queue_ms=" << std::chrono::duration<double, std::milli>(started - invalidated).count()
                << " elapsed_ms=" << elapsed << " slow=" << (elapsed > 50.);
        state->committing = false;
        if (!state->stopped && state->dirty.dirty()) schedule(state);
}

struct TaskRunner::Impl {
    struct Job {
        TaskContext context; std::function<void()> work, complete; TaskPriority priority;
        std::chrono::steady_clock::time_point queued {std::chrono::steady_clock::now()};
    };
    mutable std::mutex mutex;
    std::condition_variable ready;
    std::deque<Job> queue;
    std::vector<TaskContext> running;
    std::vector<std::thread> threads;
    std::function<void()> drained;
    std::size_t pending_commits {0};
    bool accepting {true}, stopping {false}, native_busy {false};
    Dispatcher post;
    std::map<const void*, std::function<void()>> shutdown_hooks;
    bool shutdown_started {false}, flushing_finalizers {false};
    bool can_start(const Job& job) const {
        if (job.context.canceled()) return true;
        if (running.size() >= 2) return false;
        if (native_busy && accepting && job.priority == TaskPriority::Low) return false;
        return std::none_of(running.begin(), running.end(), [&](const auto& context) {
            return context.owner && context.owner == job.context.owner;
        });
    }
    bool idle_locked() const { return queue.empty() && running.empty() && pending_commits == 0; }
};
TaskRunner::TaskRunner(Dispatcher dispatcher) : m_impl(std::make_shared<Impl>()) {
    m_impl->post = std::move(dispatcher);
    for (int i = 0; i < 2; ++i) m_impl->threads.emplace_back([state = m_impl] {
        for (;;) {
            Impl::Job job;
            {
                std::unique_lock<std::mutex> lock(state->mutex);
                state->ready.wait(lock, [&] { return state->stopping ||
                    std::any_of(state->queue.begin(), state->queue.end(), [&](const auto& j) { return state->can_start(j); }); });
                if (state->stopping && state->queue.empty()) return;
                const auto next = state->stopping ? state->queue.begin() :
                    std::find_if(state->queue.begin(), state->queue.end(), [&](const auto& j) { return state->can_start(j); });
                if (next == state->queue.end()) continue;
                job = std::move(*next); state->queue.erase(next); state->running.push_back(job.context);
            }
            // Bound nested TBB algorithms as well as the two outer workers.
            // Native slicing retains its own executor and concurrency policy.
            const auto started = std::chrono::steady_clock::now();
            const double queue_ms = milliseconds_since(job.queued);
            tbb::task_arena arena(1);
            arena.execute([&] { job.work(); });
            const double work_ms = milliseconds_since(started);
            const auto finished = std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                ++state->pending_commits;
            }
            state->post([state, context = job.context, complete = std::move(job.complete),
                         queued = job.queued, queue_ms, work_ms, finished] {
                const auto commit_started = std::chrono::steady_clock::now();
                const double dispatch_ms = milliseconds_since(finished);
                try { complete(); }
                catch (const std::exception& e) { BOOST_LOG_TRIVIAL(error) << "[UiTask] commit failed: " << e.what(); }
                catch (...) { BOOST_LOG_TRIVIAL(error) << "[UiTask] commit failed"; }
                if (latency_trace_enabled())
                    BOOST_LOG_TRIVIAL(info) << "[UiTask] scope=" << int(context.lifetime)
                        << " generation=" << context.generation << " canceled=" << context.canceled()
                        << " queue_ms=" << queue_ms << " work_ms=" << work_ms
                        << " dispatch_ms=" << dispatch_ms << " commit_ms=" << milliseconds_since(commit_started)
                        << " total_ms=" << milliseconds_since(queued);
                std::function<void()> drained;
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    --state->pending_commits;
                    const auto current = std::find_if(state->running.begin(), state->running.end(), [&](const auto& c) {
                        return c.cancellation == context.cancellation;
                    });
                    if (current != state->running.end()) state->running.erase(current);
                    state->ready.notify_all();
                    if (!state->accepting && state->idle_locked()) drained = std::move(state->drained);
                }
                if (drained) drained();
            });
        }
    });
}
TaskRunner::~TaskRunner() { stop(); }
bool TaskRunner::enqueue(TaskContext context, std::function<void()> work,
                         std::function<void()> complete, TaskPriority priority, Admission admission) {
    assert_gui_thread();
    std::vector<std::function<void()>> superseded;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        const bool draining = admission == Admission::Drain;
        const bool finalizing = admission == Admission::Finalizer && m_impl->flushing_finalizers;
        if (m_impl->stopping || (!m_impl->accepting && !draining && !finalizing)) return false;
        const auto replaceable = [&](const TaskContext& prior) {
            return context.owner && context.owner == prior.owner &&
                ((context.lifetime == TaskLifetime::View && prior.lifetime == TaskLifetime::View) ||
                 (context.coalesce_pending && prior.coalesce_pending));
        };
        const auto replacements = std::count_if(m_impl->queue.begin(), m_impl->queue.end(), [&](const auto& job) {
            return replaceable(job.context);
        });
        // Admission is atomic: a full queue must not remove requests whose
        // cancellation completions would otherwise never be dispatched.
        if (!draining && !finalizing &&
            (m_impl->queue.size() - replacements >= 64 || m_impl->pending_commits + replacements >= 64)) return false;
        // At most one latest pending view for an owner. Deliver cancellation
        // cleanup for superseded requests, but release their meshes/work now.
        for (auto it = m_impl->queue.begin(); it != m_impl->queue.end();) {
            if (replaceable(it->context)) {
                if (it->context.cancellation) *it->context.cancellation = true;
                superseded.push_back(std::move(it->complete));
                it = m_impl->queue.erase(it); ++m_impl->pending_commits;
            } else ++it;
        }
        if (!context.cancellation) context.cancellation = std::make_shared<std::atomic_bool>(false);
        if (context.wake) *context.wake = [weak = std::weak_ptr<Impl>(m_impl)] {
            if (auto state = weak.lock()) { std::lock_guard<std::mutex> lock(state->mutex); state->ready.notify_all(); }
        };
        auto where = std::find_if(m_impl->queue.begin(), m_impl->queue.end(), [priority](const auto& j) { return j.priority > priority; });
        m_impl->queue.insert(where, {std::move(context), std::move(work), std::move(complete), priority});
    }
    for (auto& callback : superseded) m_impl->post([state = m_impl, callback = std::move(callback)] {
        try { callback(); }
        catch (...) { BOOST_LOG_TRIVIAL(error) << "[UiTask] canceled commit failed"; }
        std::function<void()> drained;
        { std::lock_guard<std::mutex> lock(state->mutex);
          --state->pending_commits;
          if (!state->accepting && state->idle_locked()) drained = std::move(state->drained); }
        if (drained) drained();
    });
    m_impl->ready.notify_all(); return true;
}
void TaskRunner::set_native_busy(bool busy) {
    std::lock_guard<std::mutex> lock(m_impl->mutex); m_impl->native_busy = busy; m_impl->ready.notify_all();
}
bool TaskRunner::idle() const { std::lock_guard<std::mutex> lock(m_impl->mutex); return m_impl->idle_locked(); }
bool TaskRunner::request_shutdown(std::function<void()> drained) {
    assert_gui_thread();
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->accepting = false;
    }
    if (!m_impl->shutdown_started) {
        m_impl->shutdown_started = true;
        auto hooks = std::move(m_impl->shutdown_hooks);
        { std::lock_guard<std::mutex> lock(m_impl->mutex); m_impl->flushing_finalizers = true; }
        for (auto& entry : hooks) {
            try { entry.second(); }
            catch (...) { BOOST_LOG_TRIVIAL(error) << "[UiTask] shutdown hook failed"; }
        }
        { std::lock_guard<std::mutex> lock(m_impl->mutex); m_impl->flushing_finalizers = false; }
    }
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    for (auto& job : m_impl->queue) if (job.context.lifetime == TaskLifetime::View && job.context.cancellation) *job.context.cancellation = true;
    for (auto& context : m_impl->running) if (context.lifetime == TaskLifetime::View && context.cancellation) *context.cancellation = true;
    if (m_impl->idle_locked()) return true;
    m_impl->drained = std::move(drained); m_impl->ready.notify_all(); return false;
}
void TaskRunner::on_shutdown(const void* owner, std::function<void()> hook) {
    assert_gui_thread();
    if (!m_impl->shutdown_started) m_impl->shutdown_hooks[owner] = std::move(hook);
}
void TaskRunner::remove_shutdown_hook(const void* owner) {
    assert_gui_thread(); m_impl->shutdown_hooks.erase(owner);
}
bool TaskRunner::retire(std::thread& thread, std::function<void()> complete) {
    assert_gui_thread();
    if (!thread.joinable()) { if (complete) complete(); return true; }
    auto retiring = std::make_shared<std::thread>(std::move(thread));
    TaskContext context;
    context.lifetime = TaskLifetime::Transaction;
    context.identity = "legacy-drain";
    // Draining is unconditional, including during stop(). A normal task's
    // cancellation wrapper would skip join and destroy a joinable thread.
    const bool accepted = enqueue(context, [retiring] {
        if (retiring->joinable()) retiring->join();
    }, [complete = std::move(complete)] { if (complete) complete(); }, TaskPriority::Normal, Admission::Drain);
    if (!accepted) thread = std::move(*retiring);
    return accepted;
}
void TaskRunner::stop() {
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->accepting = false; m_impl->stopping = true;
        for (auto& job : m_impl->queue) if (job.context.cancellation) *job.context.cancellation = true;
        for (auto& context : m_impl->running) if (context.cancellation) *context.cancellation = true;
    }
    m_impl->ready.notify_all();
    for (auto& thread : m_impl->threads) if (thread.joinable()) thread.join();
}
TaskRunner& background_tasks() { static TaskRunner runner; return runner; }
} // namespace Slic3r::GUI::Presentation
