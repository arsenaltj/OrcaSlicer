#pragma once

#include "ModelLibraryEntry.hpp"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <deque>
#include <limits>

namespace Slic3r::GUI {

// One owner, one worker; all UI-thread publication is pulled by the consumer.
// A stopped/replaced request can never publish into a newer list or thumbnail.
class ModelLibraryLoader final {
public:
    struct Request {
        uint64_t revision {0};
        bool scan {false}, for_shell {false};
        boost::filesystem::path root;
        std::vector<ModelLibraryEntry> entries;
        // Optional destination indices when only changed thumbnails are decoded.
        std::vector<size_t> indices;
        int edge {96};
        std::atomic<bool> cancelled {false};
    };
    struct Snapshot {
        uint64_t revision;
        std::vector<ModelLibraryEntry> entries;
        bool failed {false}, for_shell {false};
    };
    struct Thumbnail {
        uint64_t revision;
        size_t index;
        bool for_shell {false};
        int width {0}, height {0};
        std::vector<unsigned char> rgb, alpha;
    };
    struct Ports {
        std::function<std::vector<ModelLibraryEntry>(const boost::filesystem::path&, const std::atomic<bool>&)> read;
        std::function<Thumbnail(const boost::filesystem::path&, const ModelLibraryEntry&, int,
                                const std::atomic<bool>&)> thumbnail;
    };
    explicit ModelLibraryLoader(Ports ports) : m_ports(std::move(ports)), m_worker([this] { run(); }) {}
    ~ModelLibraryLoader() { stop(); }
    ModelLibraryLoader(const ModelLibraryLoader&) = delete;
    ModelLibraryLoader& operator=(const ModelLibraryLoader&) = delete;

    bool submit(std::shared_ptr<Request> request) {
        if (!request) return false;
        if (!request->indices.empty() && request->indices.size() != request->entries.size()) return false;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping) return false;
        cancel_locked();
        m_revision = request->revision;
        m_accept_deferred = true;
        m_pending = std::move(request);
        m_wake.notify_one();
        return true;
    }
    void cancel() { std::lock_guard<std::mutex> lock(m_mutex); cancel_locked(); }
    std::thread begin_shutdown() {
        { std::lock_guard<std::mutex> lock(m_mutex); cancel_locked(); m_stopping = true; }
        m_wake.notify_one();
        return std::move(m_worker);
    }
    void stop() {
        auto thread = begin_shutdown();
        if (thread.joinable()) thread.join();
    }
    bool loading() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return bool(m_pending) || (m_active && !m_active->cancelled) || m_snapshot.has_value() || !m_thumbnails.empty();
    }
    void take(std::optional<Snapshot>& snapshot, std::vector<Thumbnail>& thumbnails,
              size_t limit = std::numeric_limits<size_t>::max()) {
        std::lock_guard<std::mutex> lock(m_mutex);
        snapshot = std::move(m_snapshot); m_snapshot.reset();
        thumbnails.clear();
        const auto count = std::min(limit, m_thumbnails.size());
        thumbnails.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            thumbnails.push_back(std::move(m_thumbnails.front())); m_thumbnails.pop_front();
        }
        m_wake.notify_all();
    }
    bool defer(const Thumbnail& thumbnail) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping || !m_accept_deferred || thumbnail.revision != m_revision ||
            m_thumbnails.size() >= max_thumbnails) return false;
        m_thumbnails.push_back(thumbnail); return true;
    }
private:
    void cancel_locked() {
        m_accept_deferred = false;
        if (m_active) m_active->cancelled = true;
        if (m_pending) m_pending->cancelled = true;
        m_pending.reset(); m_snapshot.reset(); m_thumbnails.clear();
        m_wake.notify_all();
    }
    void run() {
        for (;;) {
            std::shared_ptr<Request> request;
            { std::unique_lock<std::mutex> lock(m_mutex);
              m_wake.wait(lock, [this] { return m_stopping || m_pending; });
              if (m_stopping) return;
              request = std::move(m_pending); m_active = request; }
            if (request->scan) {
                Snapshot snapshot {request->revision, {}, false, request->for_shell};
                try { snapshot.entries = m_ports.read(request->root, request->cancelled); }
                catch (...) { snapshot.failed = true; }
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_stopping && !request->cancelled) m_snapshot = std::move(snapshot);
            } else {
                for (size_t index = 0; index < request->entries.size() && !request->cancelled; ++index) {
                    {
                        std::unique_lock<std::mutex> lock(m_mutex);
                        m_wake.wait(lock, [&] { return m_stopping || request->cancelled ||
                            m_thumbnails.size() < max_thumbnails; });
                        if (m_stopping || request->cancelled) break;
                    }
                    const size_t destination = request->indices.empty() ? index : request->indices.at(index);
                    Thumbnail thumbnail {request->revision, destination, request->for_shell};
                    try {
                        if (m_ports.thumbnail)
                            thumbnail = m_ports.thumbnail(request->root, request->entries[index],
                                                          request->edge, request->cancelled);
                    } catch (...) {} // A bad thumbnail never removes an asset.
                    thumbnail.revision = request->revision; thumbnail.index = destination; thumbnail.for_shell = request->for_shell;
                    std::unique_lock<std::mutex> lock(m_mutex);
                    m_wake.wait(lock, [&] { return m_stopping || request->cancelled ||
                        m_thumbnails.size() < max_thumbnails; });
                    if (m_stopping || request->cancelled) break;
                    m_thumbnails.push_back(std::move(thumbnail));
                }
            }
            { std::lock_guard<std::mutex> lock(m_mutex);
              if (m_active == request) m_active.reset(); }
        }
    }
    Ports m_ports;
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    bool m_stopping {false};
    bool m_accept_deferred {false};
    uint64_t m_revision {0};
    std::shared_ptr<Request> m_pending, m_active;
    std::optional<Snapshot> m_snapshot;
    static constexpr size_t max_thumbnails = 64;
    std::deque<Thumbnail> m_thumbnails;
    std::thread m_worker;
};

} // namespace Slic3r::GUI
