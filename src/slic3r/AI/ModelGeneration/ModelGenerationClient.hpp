#pragma once
#include "IModelGenerationService.hpp"
#include <memory>
#include <stdexcept>
#include <utility>

namespace Slic3r::AI::ModelGeneration {
// One service is captured for this client's lifetime. Changing a provider is an
// explicit GenerationOptions choice; failed/unknown submissions are never retried here.
class ModelGenerationClient : public IModelGenerationService {
public:
    explicit ModelGenerationClient(std::unique_ptr<IModelGenerationService> service)
        : m_service(std::move(service))
    {
        if (!m_service) throw std::invalid_argument("A model generation service is required.");
    }
    const char* implementation_id() const noexcept override { return m_service->implementation_id(); }
    const char* implementation_version() const noexcept override { return m_service->implementation_version(); }
    void preprocess_text(const std::string& request_id, const std::string& prompt,
                          const std::vector<std::string>& palette, const PaletteRoles& palette_roles,
                          bool palette_recommendation_confirmed,
                          const std::string& style, const std::string& custom_style,
                          const ImagePrintSettings& print_settings,
                          StatusFn on_complete, ErrorFn on_error, const GenerationOptions& options) override
    { m_service->preprocess_text(request_id, prompt, palette, palette_roles, palette_recommendation_confirmed, style, custom_style, print_settings, status_callback(std::move(on_complete)), std::move(on_error), options); }

    void preprocess_image(const std::string& request_id, const std::string& instruction,
                           const boost::filesystem::path& image_path, const std::vector<std::string>& palette,
                           const PaletteRoles& palette_roles, bool palette_recommendation_confirmed,
                           const std::string& style, const std::string& custom_style,
                           const ImagePrintSettings& print_settings,
                           StatusFn on_complete, ErrorFn on_error, const GenerationOptions& options) override
    { m_service->preprocess_image(request_id, instruction, image_path, palette, palette_roles, palette_recommendation_confirmed, style, custom_style, print_settings, status_callback(std::move(on_complete)), std::move(on_error), options); }

    void recommend_text_palette(const std::string& request_id, const std::string& prompt,
                                const std::string& style, const std::string& custom_style,
                                size_t palette_color_count,
                                const ImagePrintSettings& print_settings,
                                StatusFn on_complete, ErrorFn on_error, bool generate_image,
                                const GenerationOptions& options) override
    { m_service->recommend_text_palette(request_id, prompt, style, custom_style, palette_color_count, print_settings, status_callback(std::move(on_complete)), std::move(on_error), generate_image, options); }

    void recommend_image_palette(const std::string& request_id, const std::string& instruction,
                                 const boost::filesystem::path& image_path,
                                 const std::string& style, const std::string& custom_style,
                                 size_t palette_color_count,
                                 const ImagePrintSettings& print_settings,
                                 StatusFn on_complete, ErrorFn on_error, bool generate_image,
                                 const GenerationOptions& options) override
    { m_service->recommend_image_palette(request_id, instruction, image_path, style, custom_style, palette_color_count, print_settings, status_callback(std::move(on_complete)), std::move(on_error), generate_image, options); }

    void recommend_image_style(const std::string& prompt,
                               const boost::filesystem::path& image_path,
                               StyleRecommendationFn on_complete, ErrorFn on_error) override
    { m_service->recommend_image_style(prompt, image_path, std::move(on_complete), std::move(on_error)); }

    void confirm_palette(const std::string& job_id, const std::vector<std::string>& palette,
                         const PaletteRoles& palette_roles, StatusFn on_complete, ErrorFn on_error) override
    { m_service->confirm_palette(job_id, palette, palette_roles, status_callback(std::move(on_complete)), std::move(on_error)); }

    void generate(const std::string& job_id, const std::string& prepared_prompt,
                  const std::vector<std::string>& palette, const GenerationOptions& options,
                  StatusFn on_complete, ErrorFn on_error) override
    { m_service->generate(job_id, prepared_prompt, palette, options, status_callback(std::move(on_complete)), std::move(on_error)); }

    void update_generation_options(const std::string& job_id, const GenerationOptions& options,
                                   StatusFn on_complete, ErrorFn on_error) override
    { m_service->update_generation_options(job_id, options, status_callback(std::move(on_complete)), std::move(on_error)); }

    void retexture(const std::string& reference_job_id, const std::string& geometry_job_id,
                   StatusFn on_complete, ErrorFn on_error) override
    { m_service->retexture(reference_job_id, geometry_job_id, status_callback(std::move(on_complete)), std::move(on_error)); }

    void get_status(const std::string& job_id, StatusFn on_complete, ErrorFn on_error) override
    { m_service->get_status(job_id, status_callback(std::move(on_complete)), std::move(on_error)); }

    void get_latest(LatestFn on_complete, ErrorFn on_error) override
    { m_service->get_latest(latest_callback(std::move(on_complete)), std::move(on_error)); }

    void recheck(const std::string& job_id, StatusFn on_complete, ErrorFn on_error) override
    { m_service->recheck(job_id, status_callback(std::move(on_complete)), std::move(on_error)); }

    void visual_review(const std::string& job_id, StatusFn on_complete, ErrorFn on_error) override
    { m_service->visual_review(job_id, status_callback(std::move(on_complete)), std::move(on_error)); }

    void stop(const std::string& job_id, StatusFn on_complete, ErrorFn on_error) override
    { m_service->stop(job_id, status_callback(std::move(on_complete)), std::move(on_error)); }

    void remove(const std::string& job_id, CompleteFn on_complete, ErrorFn on_error) override
    { m_service->remove(job_id, std::move(on_complete), std::move(on_error)); }

    void download_preview(const std::string& job_id, const boost::filesystem::path& path, PathFn on_complete, ErrorFn on_error) override
    { m_service->download_preview(job_id, path, std::move(on_complete), std::move(on_error)); }

    void download_image_output(const std::string& job_id, const std::string& output,
                               const boost::filesystem::path& path, PathFn on_complete, ErrorFn on_error) override
    { m_service->download_image_output(job_id, output, path, std::move(on_complete), std::move(on_error)); }

    void download_input(const std::string& job_id, const boost::filesystem::path& path, PathFn on_complete, ErrorFn on_error) override
    { m_service->download_input(job_id, path, std::move(on_complete), std::move(on_error)); }

    void download_artifact(const std::string& job_id, const std::string& format,
                           const boost::filesystem::path& path, PathFn on_complete, ErrorFn on_error) override
    { m_service->download_artifact(job_id, format, path, std::move(on_complete), std::move(on_error)); }

    void download_color_intent(const std::string& job_id, const std::string& schema,
                               const std::string& sha256, const boost::filesystem::path& artifact_path,
                               const boost::filesystem::path& path, PathFn on_complete, ErrorFn on_error) override
    { m_service->download_color_intent(job_id, schema, sha256, artifact_path, path, std::move(on_complete), std::move(on_error)); }

    void record_journey_event(const std::string& event, const std::string& job_id = {}) override
    { m_service->record_journey_event(event, job_id); }

    void cancel_current() override
    { m_service->cancel_current(); }

private:
    StatusFn status_callback(StatusFn callback) const
    {
        if (!callback) return {};
        const std::string id = implementation_id(), version = implementation_version();
        return [callback = std::move(callback), id, version](JobStatus status) mutable {
            status.service_implementation_id = id;
            status.service_implementation_version = version;
            callback(std::move(status));
        };
    }
    LatestFn latest_callback(LatestFn callback) const
    {
        if (!callback) return {};
        const std::string id = implementation_id(), version = implementation_version();
        return [callback = std::move(callback), id, version](std::optional<JobStatus> status) mutable {
            if (status) {
                status->service_implementation_id = id;
                status->service_implementation_version = version;
            }
            callback(std::move(status));
        };
    }
    std::unique_ptr<IModelGenerationService> m_service;
};
} // namespace Slic3r::AI::ModelGeneration
