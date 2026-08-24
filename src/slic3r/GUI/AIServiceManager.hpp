#ifndef slic3r_GUI_AIServiceManager_hpp_
#define slic3r_GUI_AIServiceManager_hpp_

#include <wx/weakref.h>
#include <wx/window.h>

#include <functional>
#include <memory>
#include <string>

namespace Slic3r {
class Http;

namespace GUI {

struct AIServiceAvailability
{
    bool        compatible { false };
    bool        transient { false };
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

    void discover_async(wxWindow* target, CompleteFn on_complete);
    void shutdown();

private:
    struct SidecarProcess;

    void cancel_discovery();
    void try_autostart_local_sidecar();
    void stop_owned_sidecar();

    std::string           m_endpoint;
    std::shared_ptr<Http> m_active_request;
    std::shared_ptr<int>  m_lifetime;
    std::unique_ptr<SidecarProcess> m_sidecar_process;
    unsigned              m_autostart_attempts { 0 };
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_AIServiceManager_hpp_
