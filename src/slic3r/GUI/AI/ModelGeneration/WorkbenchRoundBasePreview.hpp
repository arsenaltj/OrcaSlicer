#pragma once

#include "slic3r/GUI/AI/Orca/OrcaModelPreparation.hpp"

#include <string>

namespace Slic3r::GUI {

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
        return !m_geometry_id.empty() && workbench_round_base_layout(bounds, reason).has_value();
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
        if (!workbench_round_base_layout(bounds, &error)) {
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
        return enabled(geometry_id) ? workbench_round_base_layout(bounds) : std::nullopt;
    }

private:
    std::string m_geometry_id;
    bool m_enabled {false};
};

} // namespace Slic3r::GUI
