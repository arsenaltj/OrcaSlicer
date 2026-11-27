#pragma once

#include "PostGenerationWorkbenchState.hpp"

#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::GUI {

struct WorkbenchTopologyDetails {
    std::string summary {"暂无拓扑记录"};
    std::vector<std::string> rows;
};

inline WorkbenchTopologyDetails workbench_topology_details(const WorkbenchCheckResult& check,
    const std::string& preview_sha256)
{
    WorkbenchTopologyDetails details;
    if (!workbench_check_matches_preview(check, preview_sha256)) return details;

    struct Metric {
        const char* label;
        size_t count;
        const char* unit;
    };
    const auto add_row = [&](std::initializer_list<Metric> metrics) {
        std::string row;
        for (const auto& metric : metrics) {
            if (!metric.count) continue;
            if (!row.empty()) row += " · ";
            row += std::string(metric.label) + " " + std::to_string(metric.count) + " " + metric.unit;
        }
        if (!row.empty()) details.rows.push_back(std::move(row));
    };
    add_row({{"开放边", check.boundary_edges, "条"}, {"边界环", check.boundary_loops, "个"}});
    add_row({{"非流形边", check.nonmanifold_edges, "条"}, {"非流形顶点", check.nonmanifold_vertices, "个"}});
    add_row({{"退化面", check.degenerate_faces, "个"}, {"重复面", check.duplicate_faces, "个"}});
    add_row({{"方向不一致", check.inconsistent_edges, "条边"}});

    const bool inspected = check.status == WorkbenchCheckStatus::Normal ||
        check.status == WorkbenchCheckStatus::Attention || check.status == WorkbenchCheckStatus::Invalid;
    const bool has_anomalies = !details.rows.empty();
    if (!inspected && !has_anomalies && !check.isolated_components && !check.self_intersections_checked)
        return details;

    const bool intersects = check.self_intersections_checked && check.self_intersections != 0;
    const char* self_check = !check.self_intersections_checked ? "自交检测未完成" :
        intersects ? "发现自交" : "未发现自交";
    if (has_anomalies) {
        details.summary = "发现拓扑异常";
    } else if (intersects || !check.self_intersections_checked) {
        details.summary = self_check;
    } else {
        details.summary = "未发现拓扑异常";
    }

    if (!has_anomalies && !intersects && (inspected || check.self_intersections_checked))
        details.rows.emplace_back("未发现拓扑异常");
    add_row({{"连通组件", check.isolated_components, "个"}});
    details.rows.emplace_back(self_check);
    return details;
}

} // namespace Slic3r::GUI
