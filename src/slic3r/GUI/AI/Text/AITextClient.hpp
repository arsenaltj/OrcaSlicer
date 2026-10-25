#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>

namespace Slic3r {
class Http;

namespace GUI {

class AITextClient
{
public:
    struct Availability
    {
        bool available { false };
        std::string reason;
        std::string diagnostic_id;
    };

    struct PolishRequest
    {
        bool has_reference_image { false };
        boost::filesystem::path reference_image_path;
        std::string user_description;
    };

    struct PolishResult
    {
        std::string status;
        std::string optimized_prompt;
        std::vector<std::string> warnings;
        std::string diagnostic_id;
    };

    struct Error
    {
        std::string code;
        std::string message;
        std::string diagnostic_id;
    };

    class RequestHandle final
    {
    public:
        void cancel();
        bool active() const { return m_request != nullptr; }

    private:
        friend class AITextClient;
        explicit RequestHandle(std::shared_ptr<Http> request) : m_request(std::move(request)) {}
        std::shared_ptr<Http> m_request;
    };

    using AvailabilityFn = std::function<void(Availability)>;
    using ResultFn = std::function<void(PolishResult)>;
    using ErrorFn = std::function<void(Error)>;

    explicit AITextClient(std::string endpoint);

    std::shared_ptr<RequestHandle> get_image_prompt_polish_availability(
        AvailabilityFn on_complete, ErrorFn on_error) const;
    std::shared_ptr<RequestHandle> polish_image_prompt(
        const PolishRequest& request, ResultFn on_complete, ErrorFn on_error) const;

    static bool parse_availability(const std::string& body, Availability& result);
    static bool parse_polish_result(const std::string& body, PolishResult& result);
    static Error parse_error(const std::string& body, const std::string& transport_error, unsigned status);

private:
    std::string url(const char* path) const;
    std::string m_endpoint;
};

} // namespace GUI
} // namespace Slic3r
