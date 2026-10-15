#include "SmartSliceApplier.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <algorithm>
#include <cmath>
#include <memory>

namespace Slic3r::GUI {
namespace {

template<class Option>
bool set_if_present(DynamicPrintConfig& config, const DynamicPrintConfig& base,
                    const char* key, std::unique_ptr<Option> value, std::string* diagnostic)
{
    if (base.option(key) == nullptr)
        return true; // A profile may legitimately omit an optional capability.
    config.set_key_value(key, value.release());
    return true;
}

template<class Enum>
bool enum_value(const std::string& id, int& value)
{
    const auto& values = ConfigOptionEnum<Enum>::get_enum_values();
    const auto found = values.find(id);
    if (found == values.end())
        return false;
    value = found->second;
    return true;
}

bool finite_positive(double value)
{
    return std::isfinite(value) && value > 0.0;
}

void set_diagnostic(std::string* diagnostic, const char* value)
{
    if (diagnostic != nullptr)
        *diagnostic = value;
}

} // namespace

SmartSliceApplier::SmartSliceApplier(const Model* model, DynamicPrintConfig base_config,
                                     StartSliceFn start_slice)
    : m_model(model), m_base_config(std::move(base_config)), m_start_slice(std::move(start_slice))
{}

bool SmartSliceApplier::BuildConfig(
    const DynamicPrintConfig& base_config,
    const AI::SmartSlicing::SliceRecommendation& selected_plan,
    DynamicPrintConfig& output_config,
    std::string* diagnostic)
{
    output_config = base_config;
    if (!finite_positive(selected_plan.layer_height_mm) ||
        selected_plan.layer_height_mm > 0.8 || selected_plan.wall_loops < 1 ||
        selected_plan.wall_loops > 12 || selected_plan.sparse_infill_density_percent < 0.0 ||
        selected_plan.sparse_infill_density_percent > 100.0 ||
        !finite_positive(selected_plan.outer_wall_speed_mm_s) ||
        !finite_positive(selected_plan.line_width_mm)) {
        set_diagnostic(diagnostic, "smart_slice_plan_out_of_range");
        return false;
    }

    // Orca's process window is bounded by the installed nozzle.  The engine
    // already clamps its own output; this second guard protects the native
    // boundary when a plan arrives from a stale/serialized UI state.
    if (const auto* nozzles = base_config.option<ConfigOptionFloats>("nozzle_diameter");
        nozzles != nullptr && !nozzles->values.empty()) {
        const double nozzle = nozzles->values.front();
        if (!finite_positive(nozzle) ||
            selected_plan.layer_height_mm > std::min(0.8, nozzle * 0.75) ||
            selected_plan.line_width_mm < nozzle * 0.90 ||
            selected_plan.line_width_mm > nozzle * 1.25) {
            set_diagnostic(diagnostic, "smart_slice_plan_physical_bounds");
            return false;
        }
    }

    set_if_present(output_config, base_config, "layer_height",
                   std::make_unique<ConfigOptionFloat>(selected_plan.layer_height_mm), diagnostic);
    set_if_present(output_config, base_config, "wall_loops",
                   std::make_unique<ConfigOptionInt>(selected_plan.wall_loops), diagnostic);
    set_if_present(output_config, base_config, "sparse_infill_density",
                   std::make_unique<ConfigOptionPercent>(selected_plan.sparse_infill_density_percent), diagnostic);
    set_if_present(output_config, base_config, "outer_wall_line_width",
                   std::make_unique<ConfigOptionFloatOrPercent>(selected_plan.line_width_mm, false), diagnostic);
    set_if_present(output_config, base_config, "outer_wall_speed",
                   std::make_unique<ConfigOptionFloatsNullable>(1, selected_plan.outer_wall_speed_mm_s), diagnostic);
    set_if_present(output_config, base_config, "enable_support",
                   std::make_unique<ConfigOptionBool>(selected_plan.enable_support), diagnostic);
    set_if_present(output_config, base_config, "support_threshold_angle",
                   std::make_unique<ConfigOptionInt>(static_cast<int>(std::lround(
                       std::clamp(selected_plan.support_threshold_angle_deg, 25.0, 75.0)))), diagnostic);

    int pattern = 0;
    if (!enum_value<InfillPattern>(selected_plan.sparse_infill_pattern, pattern)) {
        set_diagnostic(diagnostic, "smart_slice_infill_pattern_unavailable");
        return false;
    }
    set_if_present(output_config, base_config, "sparse_infill_pattern",
                   std::make_unique<ConfigOptionEnum<InfillPattern>>(static_cast<InfillPattern>(pattern)), diagnostic);

    const std::string support_id = selected_plan.enable_support ? selected_plan.support_type : "none";
    int support = 0;
    if (!enum_value<SupportType>(support_id, support)) {
        // Native profiles use "tree(auto)"/"tree" and "none" is represented
        // by enable_support=false; only reject a requested enabled type.
        if (selected_plan.enable_support) {
            set_diagnostic(diagnostic, "smart_slice_support_type_unavailable");
            return false;
        }
    } else {
        set_if_present(output_config, base_config, "support_type",
                       std::make_unique<ConfigOptionEnum<SupportType>>(static_cast<SupportType>(support)), diagnostic);
    }

    if (base_config.option("wall_generator") != nullptr) {
        const auto generator = selected_plan.arachne_wall_generator ?
            PerimeterGeneratorType::Arachne : PerimeterGeneratorType::Classic;
        output_config.set_key_value("wall_generator", new ConfigOptionEnum<PerimeterGeneratorType>(generator));
    }
    // adaptive_layer_height is intentionally capability-gated.  This branch
    // is compiled only when a profile exposes the option; current Orca builds
    // commonly keep adaptive layer height as a UI-generated schedule.
    if (base_config.option("adaptive_layer_height") != nullptr)
        output_config.set_key_value("adaptive_layer_height", new ConfigOptionBool(selected_plan.adaptive_layer_height));

    return true;
}

bool SmartSliceApplier::ApplyAndExecute(
    Print* current_print, const AI::SmartSlicing::SliceRecommendation& selected_plan,
    std::string* diagnostic) const
{
    if (current_print == nullptr || m_model == nullptr) {
        set_diagnostic(diagnostic, "smart_slice_print_or_model_unavailable");
        return false;
    }
    DynamicPrintConfig effective;
    if (!BuildConfig(m_base_config, selected_plan, effective, diagnostic))
        return false;
    bool apply_attempted = false;
    const auto rollback = [&]() {
        if (!apply_attempted)
            return;
        try {
            // Print::apply is the native invalidation boundary. Reapplying the
            // captured base config restores that boundary when scheduling the
            // official slice cannot be started.
            current_print->apply(*m_model, m_base_config);
        } catch (...) {
            // Preserve the original diagnostic; rollback failure is recorded by
            // the owning workflow's normal Print validation path.
        }
    };
    try {
        apply_attempted = true;
        current_print->apply(*m_model, std::move(effective));
        if (current_print->empty()) {
            rollback();
            set_diagnostic(diagnostic, "smart_slice_print_apply_empty");
            return false;
        }
        if (!m_start_slice || !m_start_slice()) {
            rollback();
            set_diagnostic(diagnostic, "smart_slice_scheduler_not_started");
            return false;
        }
    } catch (...) {
        rollback();
        set_diagnostic(diagnostic, "smart_slice_apply_exception");
        return false;
    }
    if (diagnostic != nullptr)
        diagnostic->clear();
    return true;
}

} // namespace Slic3r::GUI
