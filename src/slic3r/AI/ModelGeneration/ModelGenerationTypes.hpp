#pragma once
#include "slic3r/AI/Contracts/ColorIntent.hpp"
#include <boost/filesystem/path.hpp>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r::AI::ModelGeneration {
struct ModelGenerationTypes {
    using PaletteRoles = std::map<std::string, std::string>;

    struct ImagePrintSettings
    {
        double      width_mm { 160.0 };
        double      nozzle_mm { 0.4 };
        double      line_width_mm { 0.4 };
        double      minimum_feature_mm { 0.8 };
        std::string color_distance { "ciede2000" };
        std::string print_mode { "solid_regions" };
        std::string shadow_color { "blue" };
    };

    struct ModelQuality
    {
        struct ThinLocalRegion
        {
            size_t              sample_count { 0 };
            double              sampled_area_mm2 { 0.0 };
            double              minimum_thickness_mm { 0.0 };
            size_t              representative_face_index { 0 };
            std::vector<size_t> face_indices;
        };

        struct TargetPaletteUsage
        {
            std::string color;
            double      surface_ratio { 0.0 };
            bool        meaningful { false };
        };

        bool                     available { false };
        std::string              artifact_sha256;
        std::string              units;
        std::string              gate_version;
        std::map<std::string, double> report_metrics;
        std::map<std::string, double> report_thresholds;
        std::string              status;
        std::vector<std::string> errors;
        std::vector<std::string> warnings;
        size_t                   vertex_count { 0 };
        size_t                   face_count { 0 };
        size_t                   component_count { 0 };
        size_t                   tiny_component_count { 0 };
        double                   largest_component_face_ratio { 0.0 };
        double                   contact_span_ratio { 0.0 };
        bool                     bed_contact_area_available { false };
        double                   bed_contact_area_ratio { 0.0 };
        double                   downward_surface_ratio { 0.0 };
        bool                     elevated_downward_surface_ratio_available { false };
        double                   elevated_downward_surface_ratio { 0.0 };
        bool                     overhang_region_metrics_available { false };
        size_t                   significant_overhang_region_count { 0 };
        bool                     component_thickness_available { false };
        size_t                   thin_component_count { 0 };
        double                   minimum_component_thickness_mm { 0.0 };
        bool                     local_thickness_available { false };
        bool                     local_wall_thickness_threshold_available { false };
        double                   minimum_local_wall_thickness_mm { 0.0 };
        size_t                   local_thickness_sample_count { 0 };
        size_t                   thin_local_surface_sample_count { 0 };
        double                   minimum_sampled_local_thickness_mm { 0.0 };
        size_t                   thin_local_region_count { 0 };
        size_t                   reported_thin_local_region_count { 0 };
        std::vector<size_t>      thin_local_face_indices;
        std::vector<ThinLocalRegion> thin_local_regions;
        bool                     target_palette_metrics_available { false };
        size_t                   target_palette_color_count { 0 };
        size_t                   used_target_palette_color_count { 0 };
        size_t                   meaningful_target_palette_color_count { 0 };
        size_t                   required_meaningful_target_palette_color_count { 0 };
        double                   target_palette_surface_coverage_ratio { 0.0 };
        bool                     target_palette_diversity_ok { false };
        std::vector<TargetPaletteUsage> target_palette_surface_usage;
        bool                     repairable_topology { false };
    };

    struct VisualQuality
    {
        bool                     available { false };
        bool                     import_recommended { true };
        std::string              status;
        int                      score { 0 };
        double                   confidence { 0.0 };
        std::string              summary;
        std::vector<std::string> errors;
        std::vector<std::string> warnings;
        std::vector<std::string> blocking_warnings;
        std::map<std::string, std::string> check_reasons;
    };

    struct ModelRefinementAdvice
    {
        struct Issue
        {
            std::string code;
            std::string category;
            std::string title;
            std::string instruction;
        };

        bool               available { false };
        std::string        summary;
        std::string        prompt_suffix;
        std::vector<Issue> issues;
    };

    struct PaletteRecommendationColor
    {
        std::string hex;
        std::string name;
        std::string role;
        std::string usage;
        std::string reason;
    };

    struct PaletteRecommendation
    {
        bool                                    available { false };
        bool                                    confirmed { false };
        std::string                             summary;
        std::vector<PaletteRecommendationColor> colors;
    };

    struct StyleRecommendation
    {
        std::string              primary;
        std::vector<std::string> alternatives;
        std::string              reason;
        std::string              confidence;
    };

    struct GenerationOptions
    {
        std::string provider { "tripo" };
        int face_limit { 1000000 };
        std::string geometry_quality { "standard" };
        std::string texture_quality { "standard" };
        std::string output_format { "glb" };
    };

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
        int         face_limit { 1000000 };
        std::string generation_profile { "quality" };
        GenerationOptions generation_options;
        std::string style;
        std::string custom_style;
        size_t      palette_color_count { Slic3r::AI::kLegacyDefaultTargetPaletteColors };
        std::vector<std::string> palette;
        PaletteRoles palette_roles;
        ImagePrintSettings print_settings;
        double      updated_at { 0.0 };
        bool        input_ready { false };
        bool        preview_ready { false };
        bool        raw_preview_ready { false };
        bool        model_views_ready { false };
        bool        strict_preview_ready { false };
        bool        model_reference_ready { false };
        bool        heatmap_ready { false };
        bool        metadata_ready { false };
        double      image_score { 0.0 };
        double      mean_color_error { 0.0 };
        double      small_region_ratio { 0.0 };
        double      changed_pixel_ratio { 0.0 };
        double      boundary_complexity { 0.0 };
        int         minimum_feature_px { 0 };
        int         meaningful_palette_count { 0 };
        int         meaningful_subject_color_count { 0 };
        double      printable_subject_area_ratio { 0.0 };
        double      largest_subject_component_ratio { 0.0 };
        double      largest_detached_subject_diagonal_ratio { 0.0 };
        bool        palette_quality_ok { true };
        bool        material_fragmentation_ok { true };
        double      model_input_score { 0.0 };
        bool        model_input_eligible { true };
        std::vector<std::string> model_input_blockers;
        std::vector<std::string> model_input_warnings;
        std::string provider_error_code;
        std::string provider_error_category;
        bool        provider_error_retryable { false };
        bool        provider_error_ambiguous { false };
        std::string provider_name;
        std::string provider_task_id;
        std::string provider_conversion_task_id;
        bool        artifact_ready { false };
        std::string artifact_format;
        std::string artifact_color_encoding;
        size_t      artifact_size { 0 };
        bool        color_intent_ready { false };
        std::string color_intent_schema;
        std::string color_intent_sha256;
        size_t      color_intent_size { 0 };
        // Identifies the client service implementation; provider/task identity stays separate.
        std::string service_implementation_id;
        std::string service_implementation_version;
        ModelQuality model_quality;
        VisualQuality visual_quality;
        ModelRefinementAdvice refinement;
        PaletteRecommendation palette_recommendation;
    };

    using StatusFn = std::function<void(JobStatus)>;
    using PathFn = std::function<void(boost::filesystem::path)>;
    using CompleteFn = std::function<void()>;
    using ErrorFn = std::function<void(std::string)>;
    using LatestFn = std::function<void(std::optional<JobStatus>)>;
    using StyleRecommendationFn = std::function<void(StyleRecommendation)>;
};
} // namespace Slic3r::AI::ModelGeneration
