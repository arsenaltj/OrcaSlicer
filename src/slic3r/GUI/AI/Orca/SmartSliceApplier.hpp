#pragma once

#include "slic3r/AI/SmartSlicing/Domain/SmartSliceTypes.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <functional>
#include <string>

namespace Slic3r {
class Model;
class Print;

namespace GUI {

// Translates the UI-neutral recommendation into native Orca options and then
// invokes the already existing background slicing scheduler.  The applier is
// deliberately small: it does not own a second Print pipeline or estimate
// native statistics.  The caller supplies the current model/config snapshot
// and the scheduler callback used by Plater.
class SmartSliceApplier final
{
public:
    using StartSliceFn = std::function<bool()>;

    SmartSliceApplier(const Model* model, DynamicPrintConfig base_config,
                      StartSliceFn start_slice = {});

    bool ApplyAndExecute(Print* current_print,
                         const AI::SmartSlicing::SliceRecommendation& selected_plan,
                         std::string* diagnostic = nullptr) const;

    static bool BuildConfig(const DynamicPrintConfig& base_config,
                            const AI::SmartSlicing::SliceRecommendation& selected_plan,
                            DynamicPrintConfig& output_config,
                            std::string* diagnostic = nullptr);

private:
    const Model* m_model{nullptr};
    DynamicPrintConfig m_base_config;
    StartSliceFn m_start_slice;
};

} // namespace GUI
} // namespace Slic3r
