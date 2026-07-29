#pragma once

#include "slic3r/Utils/Http.hpp"

#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>

#include <functional>
#include <string>

namespace Slic3r::GUI {

class AIModelGenerationClient
{
public:
    struct JobStatus
    {
        std::string id;
        std::string source;
        std::string state;
        std::string phase;
        std::string message;
        std::string prepared_prompt;
        int         progress { 0 };
        bool        preview_ready { false };
        bool        artifact_ready { false };
        std::string artifact_format;
        size_t      artifact_size { 0 };
    };

    using StatusFn = std::function<void(JobStatus)>;
    using PathFn = std::function<void(boost::filesystem::path)>;
    using CompleteFn = std::function<void()>;
    using ErrorFn = std::function<void(std::string)>;

    explicit AIModelGenerationClient(std::string endpoint);
    ~AIModelGenerationClient();

    void preprocess_text(const std::string& request_id, const std::string& prompt, StatusFn on_complete, ErrorFn on_error);
    void preprocess_image(const std::string& request_id, const std::string& instruction,
                          const boost::filesystem::path& image_path, StatusFn on_complete, ErrorFn on_error);
    void generate(const std::string& job_id, const std::string& prepared_prompt, StatusFn on_complete, ErrorFn on_error);
    void get_status(const std::string& job_id, StatusFn on_complete, ErrorFn on_error);
    void stop(const std::string& job_id, StatusFn on_complete, ErrorFn on_error);
    void remove(const std::string& job_id, CompleteFn on_complete, ErrorFn on_error);
    void download_preview(const std::string& job_id, const boost::filesystem::path& path, PathFn on_complete, ErrorFn on_error);
    void download_artifact(const std::string& job_id, const std::string& format,
                           const boost::filesystem::path& path, PathFn on_complete, ErrorFn on_error);
    void cancel_current();

    static bool is_loopback_endpoint(const std::string& endpoint);

private:
    using json = nlohmann::json;

    std::string url(const std::string& path) const;
    void post_json(const std::string& path, const json& body, StatusFn on_complete, ErrorFn on_error);
    void parse_status_response(std::string body, StatusFn on_complete, ErrorFn on_error);
    void download(const std::string& path, const boost::filesystem::path& destination, size_t size_limit,
                  PathFn on_complete, ErrorFn on_error);

    std::string           m_endpoint;
    std::shared_ptr<Http> m_active_request;
};

} // namespace Slic3r::GUI
