#pragma once

#include "../AI/SmartSlicing/SmartSlicingWorkbenchState.hpp"
#include "../I18N.hpp"
#include "../Presentation/UiLifecycle.hpp"

#include <cmath>
#include <vector>
#include "../AI/Orca/OrcaTrialSliceExecutor.hpp"
#include <wx/sizer.h>
#include <wx/window.h>

namespace Slic3r::GUI {

inline bool set_slicing_detail_visible(wxSizerItem& item, bool visible)
{
    if (item.IsShown() == visible) return false;
    item.Show(visible);
    return true;
}

inline bool set_slicing_detail_label(wxWindow& control, const wxString& text)
{
    if (control.GetLabel() == text) return false;
    const auto before = control.GetBestSize();
    control.SetLabel(text);
    control.Refresh(false);
    return before != control.GetBestSize();
}

struct SliceDetailRow
{
    wxString label;
    wxString value;
    wxString section;
    bool operator==(const SliceDetailRow& rhs) const {
        return label == rhs.label && value == rhs.value && section == rhs.section;
    }
};

inline bool slicing_surface_paint_required(Presentation::Dirty flags) {
    using Presentation::Dirty;
    return Presentation::has(flags, Dirty::Paint) || Presentation::has(flags, Dirty::Layout) ||
        Presentation::has(flags, Dirty::Visibility) || Presentation::has(flags, Dirty::Barrier);
}

struct SliceParameterGroup {
    std::array<wxString, 3> layers;
    std::vector<SliceDetailRow> details;
    bool operator==(const SliceParameterGroup& rhs) const { return layers == rhs.layers && details == rhs.details; }
};

// Display values only. Session remains the owner of recommendations, estimates
// and command eligibility, including while this last complete group is retained.
class SlicingParameterPresentation {
public:
    bool submit(const SmartSlicingWorkbenchState& state, SliceParameterGroup next, bool complete, bool terminal) {
        const auto goal = state.has_selected_goal ? state.selected_goal : state.pending_slice_goal.value_or(
            state.last_sliced_goal.value_or(AI::SmartSlicing::RECOMMENDATION_GOALS.front()));
        const auto identity = std::make_tuple(state.session.workspace_revision, state.recommendation_source, goal,
            state.has_selected_goal || state.pending_slice_goal.has_value() || state.last_sliced_goal.has_value());
        const bool same = m_identity && *m_identity == identity;
        if (same && m_complete && !complete && !terminal) return false;
        const bool changed = !same || !(m_group == next);
        m_identity = identity;
        m_complete = complete;
        m_group = std::move(next);
        return changed;
    }
    const SliceParameterGroup& group() const { return m_group; }
private:
    using Identity = std::tuple<AI::SmartSlicing::WorkspaceRevision, SlicingRecommendationSource,
        AI::SmartSlicing::RecommendationGoal, bool>;
    std::optional<Identity> m_identity;
    SliceParameterGroup m_group;
    bool m_complete {false};
};

inline bool slicing_parameter_group_complete(const SmartSlicingWorkbenchState& state,
    AI::SmartSlicing::RecommendationGoal goal, bool official_ready)
{
    if (official_ready) return true;
    if (state.analyzing || state.lightweight_analyzing) return false;
    const auto selected = static_cast<size_t>(goal);
    if (state.recommendation_source == SlicingRecommendationSource::Local && state.candidates[selected])
        return state.metrics[selected].has_value();
    if (!workbench_recommendation_parameters_ready(state) || !state.lightweight_analysis) return false;
    const auto mode = goal == AI::SmartSlicing::RecommendationGoal::Speed ? AI::SmartSlicing::SliceMode::SPEED :
        goal == AI::SmartSlicing::RecommendationGoal::Quality ? AI::SmartSlicing::SliceMode::QUALITY : AI::SmartSlicing::SliceMode::BALANCED;
    const auto* parameters = state.lightweight_analysis->find(mode);
    if (parameters && state.slice_estimate && state.slice_estimate->status == ExactEstimateStatus::Ready &&
        state.slice_estimate->goal == goal && state.slice_estimate->parameters &&
        same_slice_parameters(*state.slice_estimate->parameters, *parameters)) return true;
    return parameters && state.exact_estimate.status == ExactEstimateStatus::Ready &&
        exact_estimate_matches(state.exact_estimate, state.session.workspace_revision, goal, *parameters);
}

inline bool slicing_parameter_group_terminal(const SmartSlicingWorkbenchState& state,
    AI::SmartSlicing::RecommendationGoal goal)
{
    using AI::SmartSlicing::RecommendationSessionState;
    return state.session.state == RecommendationSessionState::Canceled ||
        state.session.state == RecommendationSessionState::Stale ||
        state.cloud_recommendation.status == CloudRecommendationStatus::Failed ||
        (!state.analyzing && !state.diagnostic.empty()) ||
        (state.exact_estimate.status == ExactEstimateStatus::Failed && state.exact_estimate.goal == goal &&
            state.exact_estimate.revision == state.session.workspace_revision);
}

inline ExactEstimateView slicing_display_estimate(const SmartSlicingWorkbenchState& state,
    AI::SmartSlicing::RecommendationGoal goal, const AI::SmartSlicing::SliceRecommendation& parameters)
{
    auto estimate = state.exact_estimate;
    if (!exact_estimate_matches(estimate, state.session.workspace_revision, goal, parameters))
        estimate.status = ExactEstimateStatus::None;
    if (estimate.status != ExactEstimateStatus::Ready && state.slice_estimate &&
        state.slice_estimate->goal == goal && state.slice_estimate->parameters &&
        same_slice_parameters(*state.slice_estimate->parameters, parameters)) estimate = *state.slice_estimate;
    // A canceled or unavailable estimate is terminal, never perpetual "calculating".
    if (estimate.status != ExactEstimateStatus::Ready && estimate.status != ExactEstimateStatus::PositionChanged &&
        slicing_parameter_group_terminal(state, goal))
        estimate.status = ExactEstimateStatus::Failed;
    return estimate;
}

inline wxString format_estimated_time(double seconds)
{
    const auto minutes = static_cast<long long>(std::llround(std::max(0.0, seconds) / 60.0));
    if (minutes >= 60)
        return wxString::Format(_L("%lld 小时 %lld 分钟"), minutes / 60, minutes % 60);
    return wxString::Format(_L("%lld 分钟"), minutes);
}

inline std::vector<SliceDetailRow> exact_estimate_detail_rows(
    const AI::SmartSlicing::SliceRecommendation& recommendation, const ExactEstimateView& estimate,
    const std::vector<SliceDetailRow>& official = {}, bool previous_input = false, bool slicing = false)
{
    const auto& details = estimate.native_details;
    auto support = details ? (details->support_type == "tree" ? _L("树状") :
        details->support_type == "normal" ? _L("普通") :
        details->support_type == "mixed" ? _L("按对象设置") : _L("关闭")) :
        recommendation.enable_support ? _L("待试切确认") : _L("关闭");
    if (details && details->plate_only_support) support += _L(" · 仅底板");
    const auto adaptive = details && details->variable_layers ? _L("启用") : _L("关闭");
    const auto wall_generator = recommendation.arachne_wall_generator ? _L("Arachne") : _L("经典");
    std::vector<SliceDetailRow> rows = official;
    if (official.empty() && estimate.status == ExactEstimateStatus::Ready) {
        rows.push_back({_L("预计打印时间"), estimate.estimated_time_seconds ?
            format_estimated_time(*estimate.estimated_time_seconds) : _L("暂无"), _L("原生模拟结果")});
        rows.push_back({_L("预计耗材"), estimate.material_grams ?
            wxString::Format(_L("%.2f g"), *estimate.material_grams) : _L("暂无"), _L("原生模拟结果")});
    } else if (official.empty() && estimate.status == ExactEstimateStatus::PositionChanged) {
        rows.push_back({_L("预计打印时间"), _L("待更新"), _L("原生模拟结果")});
        rows.push_back({_L("预计耗材"), _L("待更新"), _L("原生模拟结果")});
    } else if (official.empty()) {
        rows.push_back({_L("预计打印时间"), estimate.status == ExactEstimateStatus::Failed ?
            _L("暂无准确时间") : slicing ? _L("等待切片结果") : _L("计算中..."), _L("原生模拟结果")});
        if (estimate.status == ExactEstimateStatus::Failed)
            rows.push_back({_L("AI快速参考"),
                wxString::Format(_L("约 %.0f 分钟（可能存在较大偏差）"), recommendation.estimated_time_seconds / 60.0),
                _L("辅助参考")});
        rows.push_back({_L("预计耗材"), estimate.status == ExactEstimateStatus::Failed ?
            wxString::Format(_L("%.1f g（AI估算）"), recommendation.estimated_material_grams) :
            slicing ? _L("等待切片结果") : _L("计算中..."), _L("辅助参考")});
    }
    if (official.empty() && previous_input && estimate.status == ExactEstimateStatus::Ready) {
        rows[0].label = _L("切片前预计时间");
        rows[1].label = _L("切片前预计耗材");
        for (auto& row : rows) row.section = _L("切片前估算，正式切片完成后更新");
    }
    rows.push_back({_L("表面质量"), wxString::Format(_L("%s (%.0f/100)"),
                                         wxString::FromUTF8(recommendation.surface_quality_rating),
                                         recommendation.surface_quality_score), _L("预估指标")});
    const bool reliable_normals = std::find(recommendation.warnings.begin(), recommendation.warnings.end(),
        "open_or_non_manifold_mesh") == recommendation.warnings.end();
    rows.push_back({_L("悬垂风险"), reliable_normals ? wxString::Format(_L("%.0f/100"),
        recommendation.overhang_risk_score * 100.0) : _L("无法可靠评估"), _L("预估指标")});
    auto layer = wxString::Format(_L("%.2f mm"), details ? details->effective_layer_height : recommendation.layer_height_mm);
    if (details && (details->variable_layers || details->mixed_nominal_layer_heights) && details->layer_range)
        layer = wxString::Format(_L("%.2f–%.2f mm"), details->layer_range->first, details->layer_range->second);
    rows.push_back({_L("层高"), layer, details ? _L("有效参数") : _L("推荐参数")});
    rows.push_back({_L("墙数"), wxString::Format("%d", recommendation.wall_loops), _L("推荐参数")});
    rows.push_back({_L("填充率"), wxString::Format(_L("%.0f%% · %s"), recommendation.sparse_infill_density_percent,
                                      wxString::FromUTF8(recommendation.sparse_infill_pattern)), _L("推荐参数")});
    rows.push_back({_L("外壁速度"), wxString::Format(_L("%.0f mm/s"), recommendation.outer_wall_speed_mm_s), _L("推荐参数")});
    rows.push_back({_L("支撑"), support, _L("推荐参数")});
    const auto angle = details ? (details->support_angle ? wxString::Format(_L("%d°"), *details->support_angle) : _L("—")) :
        wxString::Format(_L("%.0f°"), recommendation.support_threshold_angle_deg);
    rows.push_back({_L("支撑角度"), angle, details ? _L("有效参数") : _L("推荐参数")});
    rows.push_back({_L("自适应层高"), adaptive, _L("推荐参数")});
    rows.push_back({_L("墙生成器"), wall_generator, _L("推荐参数")});
    if (details) {
        if (details->preserves_manual_settings)
            rows.push_back({_L("人工设置"), _L("沿用工程中的支撑或层高设置"), _L("有效参数")});
        if (details->tower_position && !estimate.moved_tower_position)
            rows.push_back({_L("擦料塔坐标"), wxString::Format(_L("X %.2f / Y %.2f mm"),
                details->tower_position->x(), details->tower_position->y()), _L("原生试切")});
        if (details->orientation_degrees > 0.)
            rows.push_back({_L("备选摆放"), wxString::Format(_L("旋转 %.1f°"), details->orientation_degrees), _L("待应用")});
    }
    if (estimate.moved_tower_position) {
        const auto& [x, y] = *estimate.moved_tower_position;
        rows.push_back({_L("擦料塔坐标"), wxString::Format(_L("X %.2f / Y %.2f mm"), x, y), _L("有效参数")});
    }
    return rows;
}

} // namespace Slic3r::GUI
