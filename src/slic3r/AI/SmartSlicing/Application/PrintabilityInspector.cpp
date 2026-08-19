#include "PrintabilityInspector.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace Slic3r::AI::SmartSlicing {
namespace {

void add_issue(PrintabilityReport& report,
               IssueCode code,
               Severity severity,
               IssueScope scope,
               std::string evidence,
               bool blocks,
               bool decision,
               uint64_t object_id                   = 0,
               std::vector<std::string> resolutions = {})
{
    report.issues.push_back({code, severity, scope, object_id, std::move(evidence), std::move(resolutions), blocks, decision});
}

} // namespace

PrintabilityReport PrintabilityInspector::inspect(const WorkspaceContext& context) const
{
    PrintabilityReport report;
    report.revision = context.revision;

    if (context.objects.empty())
        add_issue(report, IssueCode::EmptyPlate, Severity::Error, IssueScope::Plate, "No printable object on plate.", true, false);
    if (context.printer_preset_id.empty())
        add_issue(report, IssueCode::MissingPrinter, Severity::Error, IssueScope::Configuration, "No printer preset is selected.", true,
                  false);
    if (context.process_preset_id.empty())
        add_issue(report, IssueCode::MissingProcess, Severity::Error, IssueScope::Configuration, "No process preset is selected.", true,
                  false);
    const bool has_material = std::any_of(context.materials.begin(), context.materials.end(),
                                          [](const MaterialSnapshot& material) { return !material.preset_id.empty(); });
    if (!has_material)
        add_issue(report, IssueCode::MissingMaterial, Severity::Error, IssueScope::Material, "No material preset is selected.", true, false);
    if (!context.native_validation_available)
        add_issue(report, IssueCode::NativeValidationUnavailable, Severity::Warning, IssueScope::Configuration,
                  "Native configuration validation is unavailable until the current plate has a valid slice.", false, false, 0,
                  {"run_standard_slice"});

    for (const WorkspaceObjectSnapshot& object : context.objects) {
        if (object.open_edge_count > 0)
            add_issue(report, IssueCode::OpenMesh, Severity::Error, IssueScope::Object,
                      std::to_string(object.open_edge_count) + " open mesh edges.", true, true, object.object_id,
                      {"repair_mesh", "keep_current_mesh"});
        if (object.outside_build_volume)
            add_issue(report, IssueCode::OutsideBuildVolume, Severity::Error, IssueScope::Object,
                      "Object extends outside the current build volume.", true, false, object.object_id, {"arrange_on_plate"});
    }
    for (const std::string& error : context.validation_errors)
        add_issue(report, IssueCode::ConfigurationValidationError, Severity::Error, IssueScope::Configuration, error, true, false);
    for (const std::string& warning : context.validation_warnings)
        add_issue(report, IssueCode::ConfigurationValidationWarning, Severity::Warning, IssueScope::Configuration, warning, false, false);

    if (report.has_blocking_issue())
        report.readiness = Readiness::Blocked;
    else if (!report.issues.empty())
        report.readiness = Readiness::NeedsAttention;
    return report;
}

} // namespace Slic3r::AI::SmartSlicing
