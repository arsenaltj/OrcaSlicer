#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <nlohmann/json.hpp>
namespace orca::mobile::slicing {
using Json=nlohmann::json;
using EventSink=std::function<void(const Json&)>;
class Engine {
public:
 explicit Engine(std::string workspace);
 ~Engine();
 Json execute(const Json& request,const EventSink& sink);
 // Signal only: safe from progress callbacks and other threads; never takes the
 // execute/Print lock. The budget monitor forwards it to Print::cancel().
 void cancel() noexcept;
private:
 std::string workspace_;
 std::atomic<bool> canceled_{false};
};
}
