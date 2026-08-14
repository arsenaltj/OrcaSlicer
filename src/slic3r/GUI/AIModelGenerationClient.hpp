#pragma once

#include "slic3r/Utils/Http.hpp"

#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>

#include <functional>
#include <optional>
#include <string>
#include <vector>

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
        std::string user_prompt;
        int         progress { 0 };
        int         face_limit { 300000 };
        std::string style;
        std::vector<std::string> palette;
        double      updated_at { 0.0 };
        bool        input_ready { false };
        bool        preview_ready { false };
        bool        artifact_ready { false };
        std::string artifact_format;
        std::string artifact_color_encoding;
        size_t      artifact_size { 0 };
    };

    using StatusFn = std::function<void(JobStatus)>;
    using PathFn = std::function<void(boost::filesystem::path)>;
    using CompleteFn = std::function<void()>;
    using ErrorFn = std::function<void(std::string)>;
    using LatestFn = std::function<void(std::optional<JobStatus>)>;

    explicit AIModelGenerationClient(std::string endpoint);
    ~AIModelGenerationClient();

    void preprocess_text(const std::string& request_id, const std::string& prompt,
                         const std::vector<std::string>& palette, const std::string& style,
                         StatusFn on_complete, ErrorFn on_error);
    void preprocess_image(const std::string& request_id, const std::string& instruction,
                          const boost::filesystem::path& image_path, const std::vector<std::string>& palette,
                          const std::string& style,
                          StatusFn on_complete, ErrorFn on_error);
    void generate(const std::string& job_id, const std::string& prepared_prompt,
                  const std::vector<std::string>& palette, int face_limit,
                  StatusFn on_complete, ErrorFn on_error);
    void get_status(const std::string& job_id, StatusFn on_complete, ErrorFn on_error);
    void get_latest(LatestFn on_complete, ErrorFn on_error);
    void stop(const std::string& job_id, StatusFn on_complete, ErrorFn on_error);
    void remove(const std::string& job_id, CompleteFn on_complete, ErrorFn on_error);
    void download_preview(const std::string& job_id, const boost::filesystem::path& path, PathFn on_complete, ErrorFn on_error);
    void download_input(const std::string& job_id, const boost::filesystem::path& path, PathFn on_complete, ErrorFn on_error);
    void download_artifact(const std::string& job_id, const std::string& format,
                           const boost::filesystem::path& path, PathFn on_complete, ErrorFn on_error);
    void cancel_current();

    static bool is_loopback_endpoint(const std::string& endpoint);

private:
    using json = nlohmann::json;

    std::string url(const std::string& path) const;
    void post_json(const std::string& path, const json& body, StatusFn on_complete, ErrorFn on_error);
    void parse_status_response(std::string body, StatusFn on_complete, ErrorFn on_error);
    static std::optional<JobStatus> parse_job(const json& job);
    void download(const std::string& path, const boost::filesystem::path& destination, size_t size_limit,
                  PathFn on_complete, ErrorFn on_error);

    std::string           m_endpoint;
    std::shared_ptr<Http> m_active_request;
};

} // namespace Slic3r::GUI
