#pragma once
#include "slic3r/AI/ModelGeneration/IModelGenerationService.hpp"
#include "slic3r/Utils/Http.hpp"
#include <nlohmann/json.hpp>
#include <memory>

namespace Slic3r::GUI {
// Existing HTTP adapter, isolated from the neutral client/DTO boundary.
class SidecarModelGenerationService final : public AI::ModelGeneration::IModelGenerationService {
public:
    explicit SidecarModelGenerationService(std::string endpoint);
    ~SidecarModelGenerationService() override;
    const char* implementation_id() const noexcept override { return "sidecar-http"; }
    const char* implementation_version() const noexcept override { return "sidecar-http-v1"; }
    void preprocess_text(const std::string& request_id, const std::string& prompt,
                          const std::vector<std::string>& palette, const PaletteRoles& palette_roles,
                          bool palette_recommendation_confirmed,
                          const std::string& style, const std::string& custom_style,
                          const ImagePrintSettings& print_settings,
                          StatusFn on_complete, ErrorFn on_error, const GenerationOptions& options) override;
    void preprocess_image(const std::string& request_id, const std::string& instruction,
                           const boost::filesystem::path& image_path, const std::vector<std::string>& palette,
                           const PaletteRoles& palette_roles, bool palette_recommendation_confirmed,
                           const std::string& style, const std::string& custom_style,
                           const ImagePrintSettings& print_settings,
                           StatusFn on_complete, ErrorFn on_error, const GenerationOptions& options) override;
    void recommend_text_palette(const std::string& request_id, const std::string& prompt,
                                const std::string& style, const std::string& custom_style,
                                size_t palette_color_count,
                                const ImagePrintSettings& print_settings,
                                StatusFn on_complete, ErrorFn on_error, bool generate_image,
                                const GenerationOptions& options) override;
    void recommend_image_palette(const std::string& request_id, const std::string& instruction,
                                 const boost::filesystem::path& image_path,
                                 const std::string& style, const std::string& custom_style,
                                 size_t palette_color_count,
                                 const ImagePrintSettings& print_settings,
                                 StatusFn on_complete, ErrorFn on_error, bool generate_image,
                                 const GenerationOptions& options) override;
    void recommend_image_style(const std::string& prompt,
                               const boost::filesystem::path& image_path,
                               StyleRecommendationFn on_complete, ErrorFn on_error) override;
    void confirm_palette(const std::string& job_id, const std::vector<std::string>& palette,
                         const PaletteRoles& palette_roles, StatusFn on_complete, ErrorFn on_error) override;
    void generate(const std::string& job_id, const std::string& prepared_prompt,
                  const std::vector<std::string>& palette, const GenerationOptions& options,
                  StatusFn on_complete, ErrorFn on_error) override;
    void update_generation_options(const std::string& job_id, const GenerationOptions& options,
                                   StatusFn on_complete, ErrorFn on_error) override;
    void retexture(const std::string& reference_job_id, const std::string& geometry_job_id,
                   StatusFn on_complete, ErrorFn on_error) override;
    void get_status(const std::string& job_id, StatusFn on_complete, ErrorFn on_error) override;
    void get_latest(LatestFn on_complete, ErrorFn on_error) override;
    void recheck(const std::string& job_id, StatusFn on_complete, ErrorFn on_error) override;
    void visual_review(const std::string& job_id, StatusFn on_complete, ErrorFn on_error) override;
    void stop(const std::string& job_id, StatusFn on_complete, ErrorFn on_error) override;
    void remove(const std::string& job_id, CompleteFn on_complete, ErrorFn on_error) override;
    void download_preview(const std::string& job_id, const boost::filesystem::path& path, PathFn on_complete, ErrorFn on_error) override;
    void download_image_output(const std::string& job_id, const std::string& output,
                               const boost::filesystem::path& path, PathFn on_complete, ErrorFn on_error) override;
    void download_input(const std::string& job_id, const boost::filesystem::path& path, PathFn on_complete, ErrorFn on_error) override;
    void download_artifact(const std::string& job_id, const std::string& format,
                           const boost::filesystem::path& path, PathFn on_complete, ErrorFn on_error) override;
    void download_color_intent(const std::string& job_id, const std::string& schema,
                               const std::string& sha256, const boost::filesystem::path& artifact_path,
                               const boost::filesystem::path& path, PathFn on_complete, ErrorFn on_error) override;
    void record_journey_event(const std::string& event, const std::string& job_id = {}) override;
    void cancel_current() override;
    static bool is_loopback_endpoint(const std::string& endpoint);
    // Restores editable choices without changing the historical job or silently
    // upgrading pre-option Tripo jobs to a costlier geometry tier.
    static GenerationOptions restore_generation_options(const nlohmann::json& saved);
    static bool validate_color_intent_manifest_file(const boost::filesystem::path& manifest_path,
                                                    const std::string& schema, const std::string& sha256,
                                                    const boost::filesystem::path& artifact_path);

private:
    using json = nlohmann::json;
    using DownloadValidator = std::function<std::optional<std::string>(const std::string&)>;

    std::string url(const std::string& path) const;
    void post_json(const std::string& path, const json& body, StatusFn on_complete, ErrorFn on_error,
                   long timeout_seconds = 130, bool preserve_downloads = false);
    void parse_status_response(std::string body, StatusFn on_complete, ErrorFn on_error);
    static std::optional<JobStatus> parse_job(const json& job);
    static json serialize_print_settings(const ImagePrintSettings& settings);
    void download(const std::string& path, const boost::filesystem::path& destination, size_t size_limit,
                  PathFn on_complete, ErrorFn on_error, DownloadValidator validator = {});
    void cancel_active_request();

    std::string           m_endpoint;
    std::shared_ptr<Http> m_active_request;
    std::vector<std::shared_ptr<Http>> m_download_requests;
    std::vector<std::shared_ptr<Http>> m_background_requests;
};


} // namespace Slic3r::GUI
