#pragma once

#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace Slic3r::GUI {

// One writer owns all draft mutations. Switching models and shutdown drain it
// before a reader can restore the old file. Only pending writes for the same
// model are coalesced; confirmed cleanup stays ordered with subsequent edits.
class BeautyDraftQueue {
public:
    struct Request {
        boost::filesystem::path model;
        nlohmann::json record;
        bool remove = false;
        bool clear_legacy = false;
    };
    using Writer = std::function<void(const Request&)>;

    explicit BeautyDraftQueue(Writer writer) : writer_(std::move(writer)), worker_([this] { run(); }) {}
    ~BeautyDraftQueue() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        changed_.notify_one();
        worker_.join();
    }
    BeautyDraftQueue(const BeautyDraftQueue&) = delete;
    BeautyDraftQueue& operator=(const BeautyDraftQueue&) = delete;

    void submit(Request request) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!pending_.empty() && pending_.back().model == request.model &&
                !pending_.back().remove && !request.remove)
                pending_.back() = std::move(request);
            else
                pending_.push_back(std::move(request));
        }
        changed_.notify_one();
    }
    void flush() {
        std::unique_lock<std::mutex> lock(mutex_);
        // A later explicit retry must keep the failed snapshot, even after the
        // UI has shown its error. Never retry continuously on a full disk.
        if (failed_ && pending_.empty() && !active_) {
            pending_.push_back(*failed_);
            changed_.notify_one();
        }
        drained_.wait(lock, [this] { return pending_.empty() && !active_; });
    }
    std::string take_error() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string result;
        result.swap(error_);
        return result;
    }

private:
    Writer writer_;
    std::mutex mutex_;
    std::condition_variable changed_, drained_;
    std::deque<Request> pending_;
    std::optional<Request> failed_;
    std::string error_;
    bool active_ = false, stopping_ = false;
    // Start only after all state used by run() has been initialized.
    std::thread worker_;

    void run() {
        for (;;) {
            Request request;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                changed_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
                if (pending_.empty()) return;
                request = std::move(pending_.front());
                pending_.pop_front();
                active_ = true;
            }
            std::string error;
            try { writer_(request); }
            catch (const std::exception& failure) { error = failure.what(); }
            catch (...) { error = "Unknown beauty draft persistence failure."; }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!error.empty()) {
                    failed_ = request;
                    error_ = request.model.filename().string() + ": " + error;
                } else if (failed_ && failed_->model == request.model) {
                    failed_.reset();
                    error_.clear();
                }
                active_ = false;
            }
            drained_.notify_all();
        }
    }
};

} // namespace Slic3r::GUI
