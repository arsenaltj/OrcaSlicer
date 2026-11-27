#pragma once
#include "LibraryClient.hpp"
#include <algorithm>
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
    void cancel(const std::string& kind, const std::string& key) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_active && m_active_kind == kind && m_active_key == key) *m_active = true;
        const auto matches = [&](const auto& item) { return item.kind == kind && item.key == key; };
        m_jobs.erase(std::remove_if(m_jobs.begin(), m_jobs.end(), matches), m_jobs.end());
        m_results.erase(std::remove_if(m_results.begin(), m_results.end(), matches), m_results.end());
        m_ready.notify_all();
    }
    bool publish(std::uint64_t epoch,std::string kind,std::string key,Value value,const Cancellation& cancel) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if(m_stopped || !cancel || m_active!=cancel || *cancel) return false;
        // Intermediate snapshots supersede each other while the GUI
        // is paused; terminal results are always delivered separately.
        for(auto& result:m_results) if(result.epoch==epoch && result.kind==kind && result.key==key) {
            result.value=std::move(value);return true;
        }
        if(m_results.size()>=16) return false;
        m_results.push_back(Result{epoch,std::move(kind),std::move(key),std::move(value)});
        return true;
    }
    std::thread begin_shutdown() {
        { std::lock_guard<std::mutex> lock(m_mutex);m_stopped=true;cancel_locked(); }
        m_ready.notify_one();return std::move(m_thread);
    }
    void stop() {
        auto thread = begin_shutdown(); if (thread.joinable()) thread.join();
    }
    std::deque<Result> take(std::size_t limit=4) {
        std::lock_guard<std::mutex> lock(m_mutex);std::deque<Result> out;
        while(!m_results.empty() && out.size()<limit) {out.push_back(std::move(m_results.front()));m_results.pop_front();}
        m_ready.notify_all();return out;
    }
private:
    struct Job {std::uint64_t epoch;std::string kind,key;Work work;Cancellation cancel;};
    void cancel_locked() {if(m_active) *m_active=true;for(auto& job:m_jobs) *job.cancel=true;m_jobs.clear();m_results.clear();m_ready.notify_all();}
    void run() {
        for(;;) {
            Job job;
            {std::unique_lock<std::mutex> lock(m_mutex);m_ready.wait(lock,[this]{return m_stopped || (!m_jobs.empty() && m_results.size()<16);});
             if(m_stopped) return;job=std::move(m_jobs.front());m_jobs.pop_front();m_active=job.cancel;
             m_active_kind=job.kind;m_active_key=job.key;}
            Result result{job.epoch,job.kind,job.key,{}};
            try {result.value=job.work(job.cancel);} catch(const Error& e) {result.status=e.status;result.error=e.what();}
            catch(...) {result.error="无法完成模型库操作，请重试";}
            {std::unique_lock<std::mutex> lock(m_mutex);
             m_ready.wait(lock,[&]{return m_stopped || *job.cancel || m_results.size()<16;});
             if(!m_stopped && !*job.cancel) m_results.push_back(std::move(result));
             if(m_active==job.cancel) m_active.reset();}
        }
    }
    mutable std::mutex m_mutex;
    std::condition_variable m_ready;
    std::deque<Job> m_jobs;
    std::deque<Result> m_results;
    Cancellation m_active;
    std::string m_active_kind, m_active_key;
    bool m_stopped {false};
    std::thread m_thread;
};
}
