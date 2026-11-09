#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r::GUI::AdvancedCreation {

enum class ViewId : std::size_t { Front, Left, Back, Right };
inline constexpr std::array<ViewId, 4> AllViews {ViewId::Front, ViewId::Left, ViewId::Back, ViewId::Right};
inline constexpr std::size_t index(ViewId view) { return static_cast<std::size_t>(view); }
const char* view_name(ViewId view);
std::optional<ViewId> parse_view(const std::string& name);

struct DesignDraft {
    std::string prompt;
    std::string style {"realistic"};
    std::string custom_style;
    std::string provider {"gpt"};
    std::string model;
    bool operator==(const DesignDraft& other) const;
    bool operator!=(const DesignDraft& other) const { return !(*this == other); }
};

struct ModelOptions {
    std::string provider {"tripo"};
    std::string model;
    int face_limit {1000000};
    std::string geometry_quality {"standard"};
    std::string texture_quality {"standard"};
    std::string output_format {"glb"};
};

struct InputDraft {
    std::array<std::string, 4> uploads;
    DesignDraft design;
    ModelOptions geometry;
};

struct DesignView {
    std::string version_id;
    std::string sha256;
    std::string image_path;
    bool usable {false};
    std::string operation_id;
    std::string attempt_state;
    std::string error_code;
};

struct GroupSnapshot {
    std::string id;
    std::string display_name;
    std::string input_epoch;
    std::uint64_t revision {0};
    std::array<std::string, 4> uploads;
    std::array<bool, 4> pending_views {};
    DesignDraft initial_input;
    DesignDraft saved_draft;
    std::optional<DesignDraft> latest_submitted_draft;
    std::array<DesignView, 4> views;
    std::string active_operation_id;
    bool stop_intent {false};
    bool published {false};
    bool model_busy {false};
    std::string model_job_id;
    std::string model_state;
    std::string model_message;
    bool model_resumable {false};
};

struct Capabilities {
    bool service_available {false};
    bool initial_text {false};
    bool initial_references {false};
    bool edit_current {false};
    bool four_view_model {false};
};

struct HostGuards {
    // Supplied by the original workbench/application seam, never inferred from four images.
    bool can_start {true};
    bool model_options_valid {true};
    bool model_service_available {false};
};

struct Eligibility {
    bool generate_design {false};
    bool generate_model {false};
    bool stop {false};
    bool retry {false};
    bool recover {false};
    std::string design_reason;
    std::string model_reason;
};

struct CallbackContext {
    std::uint64_t epoch {0};
    std::string group_id;
};

struct CommandContext {
    CallbackContext callback;
    std::uint64_t draft_revision {0};
    std::uint64_t selection_revision {0};
    std::uint64_t group_revision {0};
};

struct LayoutRect { int x {0}, y {0}, width {0}, height {0}; };
struct GridLayout { std::array<LayoutRect, 4> cards; std::array<LayoutRect, 4> images; };

GridLayout grid_layout(int width, int height, int margin, int gap, int caption_height);
LayoutRect contain_image(LayoutRect available, int image_width, int image_height);
bool design_attempt_generating(const std::string& state);

} // namespace Slic3r::GUI::AdvancedCreation
