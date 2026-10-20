#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <nlohmann/json.hpp>
namespace Slic3r { class Print; }
namespace orca::slicing {
using Json=nlohmann::json;
using EventSink=std::function<void(const Json&)>;
class Engine {
public:
 explicit Engine(std::string workspace);
 ~Engine();
 Json execute(const Json& request,const EventSink& sink);
 void cancel() noexcept;
private:
 std::string workspace_;
 std::atomic<bool> canceled_{false};
 std::mutex printMutex_;
 Slic3r::Print* activePrint_{nullptr};
 void checkCanceled() const;
};
}
