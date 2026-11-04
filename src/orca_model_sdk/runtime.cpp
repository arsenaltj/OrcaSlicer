#include "model_internal.hpp"
namespace orca_model_detail {
std::mutex registry_mutex;
std::unordered_map<uintptr_t, std::shared_ptr<Context>> registry;
std::atomic<uintptr_t> sequence{1};
std::shared_ptr<Context> acquire(orca_model_context* p)
{
    std::lock_guard<std::mutex> lock(registry_mutex);
    auto i = registry.find(reinterpret_cast<uintptr_t>(p));
    return i == registry.end() ? nullptr : i->second;
}
char* allocated(const std::string& s)
{
    auto* p = static_cast<char*>(std::malloc(s.size() + 1));
    if (p)
        std::memcpy(p, s.c_str(), s.size() + 1);
    return p;
}
void check_cancel(Context& c)
{
    if (c.cancel)
        throw Error("CANCELED", "Operation canceled");
}
}
