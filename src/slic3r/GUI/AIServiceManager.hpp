#ifndef slic3r_GUI_AIServiceManager_hpp_
#define slic3r_GUI_AIServiceManager_hpp_

#include <functional>
#include <memory>
#include <string>

namespace Slic3r {
class Http;

namespace GUI {

struct AIServiceAvailability
{
    bool        compatible { false };
    bool        config_proposal_available { false };
    bool        model_generation_available { false };
    std::string sidecar_version;
    std::string error;
};

class AIServiceManager
{
public:
    using CompleteFn = std::function<void(AIServiceAvailability)>;

    explicit AIServiceManager(std::string endpoint);
    ~AIServiceManager();

    void discover_async(CompleteFn on_complete);
    void shutdown();

private:
    std::string           m_endpoint;
    std::shared_ptr<Http> m_active_request;
    std::shared_ptr<int>  m_lifetime;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_AIServiceManager_hpp_
