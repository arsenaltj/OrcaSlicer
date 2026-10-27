#pragma once

#include "../AI/SmartSlicing/SmartSlicingWorkbenchState.hpp"
#include "../I18N.hpp"

#include <cmath>
#include <vector>

namespace Slic3r::GUI {

struct SliceDetailRow
{
    wxString label;
    wxString value;
    wxString section;
};

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
    const auto support = recommendation.enable_support ? _L("树状") : _L("关闭");
    const auto adaptive = recommendation.adaptive_layer_height ? _L("启用") : _L("关闭");
    const auto wall_generator = recommendation.arachne_wall_generator ? _L("Arachne") : _L("经典");
    std::vector<SliceDetailRow> rows = official;
    if (official.empty() && estimate.status == ExactEstimateStatus::Ready) {
        rows.push_back({_L("预计打印时间"), estimate.estimated_time_seconds ?
            format_estimated_time(*estimate.estimated_time_seconds) : _L("暂无"), _L("原生模拟结果")});
        rows.push_back({_L("预计耗材"), estimate.material_grams ?
            wxString::Format(_L("%.2f g"), *estimate.material_grams) : _L("暂无"), _L("原生模拟结果")});
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
    rows.push_back({_L("悬垂风险"), wxString::Format(_L("%.0f/100"), recommendation.overhang_risk_score * 100.0), _L("预估指标")});
    rows.push_back({_L("层高"), wxString::Format(_L("%.2f mm"), recommendation.layer_height_mm), _L("推荐参数")});
    rows.push_back({_L("墙数"), wxString::Format("%d", recommendation.wall_loops), _L("推荐参数")});
    rows.push_back({_L("填充率"), wxString::Format(_L("%.0f%% · %s"), recommendation.sparse_infill_density_percent,
                                      wxString::FromUTF8(recommendation.sparse_infill_pattern)), _L("推荐参数")});
    rows.push_back({_L("外壁速度"), wxString::Format(_L("%.0f mm/s"), recommendation.outer_wall_speed_mm_s), _L("推荐参数")});
    rows.push_back({_L("支撑"), support, _L("推荐参数")});
    rows.push_back({_L("支撑角度"), wxString::Format(_L("%.0f°"), recommendation.support_threshold_angle_deg), _L("推荐参数")});
    rows.push_back({_L("自适应层高"), adaptive, _L("推荐参数")});
    rows.push_back({_L("墙生成器"), wall_generator, _L("推荐参数")});
    return rows;
}

} // namespace Slic3r::GUI
