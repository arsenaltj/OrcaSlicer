#pragma once
#include "UiLifecycle.hpp"
#include <exception>
#include <functional>
#include <optional>
#include <type_traits>
#include <thread>
#include <wx/event.h>

namespace Slic3r::GUI::Presentation {
using Dispatcher = std::function<void(std::function<void()>)>;

void assert_gui_thread();
void post_gui(std::function<void()> callback);

// One queued commit per surface. Hidden/minimized surfaces retain dirty state
// and resume explicitly; no polling timer or recursive synchronous repaint.
class RefreshScheduler {
public:
    using Commit = std::function<void(Dirty)>;
    explicit RefreshScheduler(Dispatcher dispatcher = post_gui);
    ~RefreshScheduler();
    RefreshScheduler(const RefreshScheduler&) = delete;
    RefreshScheduler& operator=(const RefreshScheduler&) = delete;
    void configure(std::function<bool()> visible, Commit commit, const char* name);
    void request(Dirty flags = Dirty::Paint);
    // Explicit first-frame/restore barrier. Consumes queued changes once;
    // the already-posted callback cannot repaint the same batch again.
    void flush(Dirty flags = Dirty::Barrier | Dirty::Paint);
    void resume();
    void stop();
private:
    struct State;
    std::shared_ptr<State> m_state;
    static void schedule(const std::shared_ptr<State>& state);
    static void commit_pending(const std::shared_ptr<State>& state);
};

enum class TaskPriority { Visible, Normal, Low };
template<class T> struct TaskResult {
    std::optional<T> value;
    std::exception_ptr error;
    bool canceled {false};
};

// Execution only, never a second business state owner. Existing workers,
// network clients and native slicing keep their own transaction contracts.
class TaskRunner {
public:
    explicit TaskRunner(Dispatcher dispatcher = post_gui);
    ~TaskRunner();
    TaskRunner(const TaskRunner&) = delete;
    TaskRunner& operator=(const TaskRunner&) = delete;
    template<class Work, class Complete>
    bool submit(TaskContext context, Work work, Complete complete,
                TaskPriority priority = TaskPriority::Normal) {
        return submit_impl(std::move(context), std::move(work), std::move(complete), priority, Admission::Normal);
    }
    // Only retained transaction snapshots may be flushed from a shutdown hook.
    // Ordinary submissions remain closed while owners prepare their last save.
    template<class Work, class Complete>
    bool submit_finalizer(TaskContext context, Work work, Complete complete) {
        if (context.lifetime != TaskLifetime::Transaction) return false;
        return submit_impl(std::move(context), std::move(work), std::move(complete), TaskPriority::Normal, Admission::Finalizer);
    }
private:
    enum class Admission { Normal, Finalizer, Drain };
    template<class Work, class Complete>
    bool submit_impl(TaskContext context, Work work, Complete complete, TaskPriority priority, Admission admission) {
        using Value = std::invoke_result_t<Work, const TaskContext&>;
        // Work, cancellation and completion must observe the same token even
        // when a legacy caller supplies an unscoped context.
        if (!context.cancellation) context.cancellation = std::make_shared<std::atomic_bool>(false);
        auto result = std::make_shared<TaskResult<Value>>();
        return enqueue(context, [context, result, work = std::move(work)]() mutable {
            if (!context.canceled()) {
                try { result->value.emplace(work(context)); }
                catch (...) { result->error = std::current_exception(); }
            }
            result->canceled = context.canceled();
        }, [context, result, complete = std::move(complete)]() mutable {
            result->canceled = context.canceled();
            complete(std::move(*result));
        }, priority, admission);
    }
public:
    void set_native_busy(bool busy);
    bool idle() const;
    // Returns immediately. The callback runs on the GUI thread after workers
    // AND their queued completion handlers have drained. View work is canceled.
    bool request_shutdown(std::function<void()> drained);
    // Adapt retained legacy executors without replacing their business state.
    // Hooks run on GUI after ordinary admission closes; owners outlive drain.
    void on_shutdown(const void* owner, std::function<void()> cancel_and_drain);
    void remove_shutdown_hook(const void* owner);
    bool retire(std::thread& thread, std::function<void()> complete = {});
    void stop();
private:
    struct Impl;
    std::shared_ptr<Impl> m_impl;
    bool enqueue(TaskContext, std::function<void()>, std::function<void()>, TaskPriority, Admission = Admission::Normal);
};
TaskRunner& background_tasks();

} // namespace Slic3r::GUI::Presentation
