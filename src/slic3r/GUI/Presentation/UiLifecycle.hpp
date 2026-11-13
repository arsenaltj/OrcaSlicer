#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace Slic3r::GUI::Presentation {

enum class Dirty : unsigned { None = 0, Content = 1, Visibility = 2, Layout = 4,
    Overlay = 8, Viewport = 16, Paint = 32, Barrier = 64 };
inline Dirty operator|(Dirty a, Dirty b) { return Dirty(unsigned(a) | unsigned(b)); }
inline bool has(Dirty flags, Dirty bit) { return (unsigned(flags) & unsigned(bit)) != 0; }

// GUI-thread state. Taking work clears only the sampled changes, so an
// invalidation raised during a commit is never lost by its pending guard.
class RefreshState {
public:
    void invalidate(Dirty flags) { m_dirty = m_dirty | flags; }
    bool dirty() const { return m_dirty != Dirty::None; }
    Dirty take() { const auto flags = m_dirty; m_dirty = Dirty::None; return flags; }
private:
    Dirty m_dirty {Dirty::None};
};

enum class TaskLifetime { View, Session, Transaction };
struct TaskContext {
    TaskLifetime lifetime {TaskLifetime::View};
    std::uint64_t generation {0};
    std::uint64_t account_epoch {0};
    std::string identity;
    std::shared_ptr<std::atomic_bool> cancellation;
    std::shared_ptr<const char> owner;
    std::shared_ptr<std::function<void()>> wake;
    // Opt in only for replaceable data snapshots (for example one draft's
    // save/clear intent), never for native publication or generation receipts.
    bool coalesce_pending {false};
    bool canceled() const { return cancellation && cancellation->load(); }
};

// A view scope must not own a generation or publication transaction. Owners
// choose the lifetime explicitly; changing a route invalidates only its view.
class TaskScope {
public:
    TaskScope() = default;
    TaskScope(const TaskScope&) = delete;
    TaskScope& operator=(const TaskScope&) = delete;
    ~TaskScope() { cancel(); }
    TaskContext begin(std::string identity, TaskLifetime lifetime = TaskLifetime::View,
                      std::uint64_t account_epoch = 0) {
        cancel();
        m_current = {lifetime, ++m_generation, account_epoch, std::move(identity),
                     std::make_shared<std::atomic_bool>(false), m_owner, std::make_shared<std::function<void()>>()};
        return m_current;
    }
    void cancel() {
        if (m_current.cancellation) *m_current.cancellation = true;
        if (m_current.wake && *m_current.wake) (*m_current.wake)();
    }
    bool owns(const TaskContext& context) const {
        return context.cancellation && context.cancellation == m_current.cancellation &&
            context.generation == m_generation;
    }
    bool matches(const TaskContext& context) const { return owns(context) && !context.canceled(); }
private:
    std::uint64_t m_generation {0};
    TaskContext m_current;
    std::shared_ptr<const char> m_owner {std::make_shared<const char>(0)};
};

struct PresentationStamp {
    std::uint64_t page {0}, model {0}, layout {0};
    int width {0}, height {0}, dpi {0};
    bool operator==(const PresentationStamp& rhs) const {
        return page == rhs.page && model == rhs.model && layout == rhs.layout &&
            width == rhs.width && height == rhs.height && dpi == rhs.dpi;
    }
};

class ProgressGate {
public:
    using Clock = std::chrono::steady_clock;
    bool accept(int phase, bool terminal, Clock::time_point now = Clock::now()) {
        if (terminal || phase != m_phase || now - m_last >= std::chrono::milliseconds(100)) {
            m_phase = phase; m_last = now; return true;
        }
        return false;
    }
private:
    int m_phase {-1};
    Clock::time_point m_last {};
};

// A stale restore cannot take a shared native window from its newer host.
class MountGeneration {
public:
    using Token = std::uint64_t;
    Token acquire() { m_active = ++m_generation; return m_active; }
    bool current(Token token) const { return token != 0 && token == m_active; }
    bool release(Token token) {
        if (!current(token)) return false;
        m_active = 0; return true;
    }
private:
    Token m_generation {0}, m_active {0};
};

} // namespace Slic3r::GUI::Presentation
