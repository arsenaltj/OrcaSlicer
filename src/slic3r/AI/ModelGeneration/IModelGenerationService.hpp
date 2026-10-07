#pragma once
#include "ModelGenerationTypes.hpp"

namespace Slic3r::AI::ModelGeneration {
// Transport owns asynchronous requests. The facade adds no submission retries,
// confirmation, persistence, UI dispatch, or second request sequence.
class IModelGenerationService : public ModelGenerationTypes {
public:
    virtual ~IModelGenerationService() = default;
    virtual const char* implementation_id() const noexcept = 0;
    virtual const char* implementation_version() const noexcept = 0;
    virtual void preprocess_text(const std::string& request_id, const std::string& prompt,
                          const std::vector<std::string>& palette, const PaletteRoles& palette_roles,
                          bool palette_recommendation_confirmed,
                          const std::string& style, const std::string& custom_style,
                          const ImagePrintSettings& print_settings,
                          StatusFn on_complete, ErrorFn on_error, const GenerationOptions& options) = 0;
    virtual void preprocess_image(const std::string& request_id, const std::string& instruction,
                           const boost::filesystem::path& image_path, const std::vector<std::string>& palette,
                           const PaletteRoles& palette_roles, bool palette_recommendation_confirmed,
                           const std::string& style, const std::string& custom_style,
                           const ImagePrintSettings& print_settings,
                           StatusFn on_complete, ErrorFn on_error, const GenerationOptions& options) = 0;
    virtual void recommend_text_palette(const std::string& request_id, const std::string& prompt,
                                const std::string& style, const std::string& custom_style,
                                size_t palette_color_count,
                                const ImagePrintSettings& print_settings,
                                StatusFn on_complete, ErrorFn on_error, bool generate_image,
                                const GenerationOptions& options) = 0;
    virtual void recommend_image_palette(const std::string& request_id, const std::string& instruction,
                                 const boost::filesystem::path& image_path,
                                 const std::string& style, const std::string& custom_style,
                                 size_t palette_color_count,
                                 const ImagePrintSettings& print_settings,
                                 StatusFn on_complete, ErrorFn on_error, bool generate_image,
                                 const GenerationOptions& options) = 0;
    virtual void recommend_image_style(const std::string& prompt,
                               const boost::filesystem::path& image_path,
                               StyleRecommendationFn on_complete, ErrorFn on_error) = 0;
    virtual void confirm_palette(const std::string& job_id, const std::vector<std::string>& palette,
                         const PaletteRoles& palette_roles, StatusFn on_complete, ErrorFn on_error) = 0;
    virtual void generate(const std::string& job_id, const std::string& prepared_prompt,
                  const std::vector<std::string>& palette, const GenerationOptions& options,
                  StatusFn on_complete, ErrorFn on_error) = 0;
    virtual void update_generation_options(const std::string& job_id, const GenerationOptions& options,
                                   StatusFn on_complete, ErrorFn on_error) = 0;
    virtual void retexture(const std::string& reference_job_id, const std::string& geometry_job_id,
                   StatusFn on_complete, ErrorFn on_error) = 0;
    virtual void get_status(const std::string& job_id, StatusFn on_complete, ErrorFn on_error) = 0;
    virtual void get_latest(LatestFn on_complete, ErrorFn on_error) = 0;
    virtual void recheck(const std::string& job_id, StatusFn on_complete, ErrorFn on_error) = 0;
    virtual void visual_review(const std::string& job_id, StatusFn on_complete, ErrorFn on_error) = 0;
    virtual void stop(const std::string& job_id, StatusFn on_complete, ErrorFn on_error) = 0;
    virtual void remove(const std::string& job_id, CompleteFn on_complete, ErrorFn on_error) = 0;
    virtual void download_preview(const std::string& job_id, const boost::filesystem::path& path, PathFn on_complete, ErrorFn on_error) = 0;
    virtual void download_image_output(const std::string& job_id, const std::string& output,
                               const boost::filesystem::path& path, PathFn on_complete, ErrorFn on_error) = 0;
    virtual void download_input(const std::string& job_id, const boost::filesystem::path& path, PathFn on_complete, ErrorFn on_error) = 0;
    virtual void download_artifact(const std::string& job_id, const std::string& format,
                           const boost::filesystem::path& path, PathFn on_complete, ErrorFn on_error) = 0;
    virtual void download_color_intent(const std::string& job_id, const std::string& schema,
                               const std::string& sha256, const boost::filesystem::path& artifact_path,
                               const boost::filesystem::path& path, PathFn on_complete, ErrorFn on_error) = 0;
    virtual void record_journey_event(const std::string& event, const std::string& job_id = {}) = 0;
    virtual void cancel_current() = 0;
};
} // namespace Slic3r::AI::ModelGeneration
