#pragma once
class wxWindow;
namespace Slic3r::GUI {
enum class DiagnosticLogAppearance { Native, Settings };
void export_diagnostic_logs(wxWindow* parent, DiagnosticLogAppearance appearance = DiagnosticLogAppearance::Native);
}
