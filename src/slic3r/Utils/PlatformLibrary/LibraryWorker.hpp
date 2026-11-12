#pragma once
#include "LibraryClient.hpp"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace Slic3r::PlatformLibrary {
// One bounded worker. Results are pulled on the GUI thread; no window pointers cross it.
class Worker final {
public:
    struct Value { Json json; std::shared_ptr<StagedDownload> download; std::shared_ptr<const std::string> geometry; };
    struct Result { std::uint64_t epoch; std::string kind, key; Value value; unsigned status {0}; std::string error; };
    using Work=std::function<Value(Cancellation)>;
    Worker() : m_thread([this]{run();}) {}
    ~Worker() { stop(); }
    bool submit(std::uint64_t epoch,std::string kind,std::string key,Work work,bool priority=false) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if(m_stopped || m_jobs.size()>=128) return false;
        Job job{epoch,std::move(kind),std::move(key),std::move(work),std::make_shared<std::atomic_bool>(false)};
        if(priority) m_jobs.push_front(std::move(job));else m_jobs.push_back(std::move(job));
        m_ready.notify_one();return true;
    }
    void cancel() {
        std::lock_guard<std::mutex> lock(m_mutex);cancel_locked();
    }
    void stop() {
        { std::lock_guard<std::mutex> lock(m_mutex);m_stopped=true;cancel_locked(); }
        m_ready.notify_one();if(m_thread.joinable()) m_thread.join();
    }
    std::deque<Result> take() {
        std::lock_guard<std::mutex> lock(m_mutex);std::deque<Result> out;out.swap(m_results);return out;
    }
    std::pair<std::size_t,std::size_t> bytes() const {return {m_done.load(),m_total.load()};}
    void progress(std::size_t done,std::size_t total) {m_done=done;m_total=total;}
private:
    struct Job {std::uint64_t epoch;std::string kind,key;Work work;Cancellation cancel;};
    void cancel_locked() {if(m_active) *m_active=true;for(auto& job:m_jobs) *job.cancel=true;m_jobs.clear();m_results.clear();m_done=0;m_total=0;}
    void run() {
        for(;;) {
            Job job;
            {std::unique_lock<std::mutex> lock(m_mutex);m_ready.wait(lock,[this]{return m_stopped || !m_jobs.empty();});
             if(m_stopped) return;job=std::move(m_jobs.front());m_jobs.pop_front();m_active=job.cancel;}
            Result result{job.epoch,job.kind,job.key,{}};
            try {result.value=job.work(job.cancel);} catch(const Error& e) {result.status=e.status;result.error=e.what();}
            catch(...) {result.error="无法完成模型库操作，请重试";}
            {std::lock_guard<std::mutex> lock(m_mutex);if(!m_stopped && !*job.cancel) m_results.push_back(std::move(result));
             if(m_active==job.cancel) m_active.reset();}
        }
    }
    mutable std::mutex m_mutex;
    std::condition_variable m_ready;
    std::deque<Job> m_jobs;
    std::deque<Result> m_results;
    Cancellation m_active;
    bool m_stopped {false};
    std::atomic<std::size_t> m_done {0},m_total {0};
    std::thread m_thread;
};
}
