#ifndef slic3r_GUI_AISidecarClient_hpp_
#define slic3r_GUI_AISidecarClient_hpp_

#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

namespace Slic3r {
class Http;

namespace GUI {

class AISidecarClient
{
public:
    using json = nlohmann::json;

    struct Response
    {
        std::string request_id;
        std::string assistant_text;
        json        proposal;
        json        raw;
    };

    using CompleteFn = std::function<void(Response)>;
    using ErrorFn = std::function<void(std::string)>;

    explicit AISidecarClient(std::string endpoint = default_endpoint());
    ~AISidecarClient();

    void propose_config_changes(const json& request, CompleteFn on_complete, ErrorFn on_error);
    void cancel_current();

    const std::string& endpoint() const { return m_endpoint; }
    void set_endpoint(std::string endpoint) { m_endpoint = std::move(endpoint); }

    static std::string default_endpoint();

private:
    std::string           m_endpoint;
    std::shared_ptr<Http> m_active_request;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_AISidecarClient_hpp_
