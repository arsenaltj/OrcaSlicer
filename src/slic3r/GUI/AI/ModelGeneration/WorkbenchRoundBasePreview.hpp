#pragma once

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Geometry.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

namespace Slic3r::GUI {

struct WorkbenchRoundBaseLayout
{
    Transform3d body_transform {Transform3d::Identity()};
    Transform3d base_transform {Transform3d::Identity()};
    double body_scale {1.0};
    double radius {0.0};
    double height {3.0};
    double overlap {0.2};
};

class WorkbenchRoundBasePreviewState
{
public:
    void reset(std::string geometry_id)
    {
        m_geometry_id = std::move(geometry_id);
        m_enabled = false;
    }

    void clear() { reset({}); }

    bool available(const BoundingBoxf3& bounds, std::string* reason = nullptr) const
    {
        return !m_geometry_id.empty() && layout_for(bounds, reason).has_value();
    }

    bool set_enabled(bool enabled, const BoundingBoxf3& bounds, std::string& error)
    {
        error.clear();
        if (!enabled) {
            m_enabled = false;
            return true;
        }
        if (m_geometry_id.empty()) {
            error = "当前模型尚未加载，无法增加底座。";
            m_enabled = false;
            return false;
        }
        if (!layout_for(bounds, &error)) {
            m_enabled = false;
            return false;
        }
        m_enabled = true;
        return true;
    }

    bool enabled(const std::string& geometry_id) const
    {
        return m_enabled && !geometry_id.empty() && geometry_id == m_geometry_id;
    }

    std::optional<WorkbenchRoundBaseLayout> layout(
        const std::string& geometry_id, const BoundingBoxf3& bounds) const
    {
        return enabled(geometry_id) ? layout_for(bounds) : std::nullopt;
    }

    static std::optional<WorkbenchRoundBaseLayout> layout_for(
        const BoundingBoxf3& bounds, std::string* reason = nullptr)
    {
        auto fail = [&](const char* message) -> std::optional<WorkbenchRoundBaseLayout> {
            if (reason) *reason = message;
            return std::nullopt;
        };
        if (!bounds.defined || !bounds.min.allFinite() || !bounds.max.allFinite())
            return fail("当前模型边界无效，无法安全生成底座预览。");
        const Vec3d size = bounds.size().cast<double>();
        constexpr double base_height = 3.0;
        constexpr double overlap = 0.2;
        if (!size.allFinite() || size.x() <= 1e-6 || size.y() <= 1e-6 ||
            size.z() <= base_height)
            return fail("当前模型尺寸不足，无法安全生成 3 mm 底座。");

        WorkbenchRoundBaseLayout result;
        result.height = base_height;
        result.overlap = overlap;
        result.body_scale = (size.z() - (base_height - overlap)) / size.z();
        if (!std::isfinite(result.body_scale) || result.body_scale <= 0.0 || result.body_scale >= 1.0)
            return fail("当前模型缩放结果无效，无法安全生成底座预览。");

        const Vec3d center = bounds.center().cast<double>();
        result.body_transform.linear() *= result.body_scale;
        result.body_transform.translation() = Vec3d(
            (1.0 - result.body_scale) * center.x(),
            (1.0 - result.body_scale) * center.y(),
            double(bounds.min.z()) + base_height - overlap -
                result.body_scale * double(bounds.min.z()));

        const double width = std::max(8.0, size.x() * result.body_scale + 4.0);
        const double depth = std::max(8.0, size.y() * result.body_scale + 4.0);
        // A circular base must cover the longest XY span and extend 2 mm
        // beyond the transformed body on every radial side.
        result.radius = std::max(4.0, std::max(width, depth) / 2.0);
        result.base_transform.translation() = Vec3d(
            center.x(), center.y(), double(bounds.min.z()));
        return result;
    }

private:
    std::string m_geometry_id;
    bool m_enabled {false};
};

} // namespace Slic3r::GUI
