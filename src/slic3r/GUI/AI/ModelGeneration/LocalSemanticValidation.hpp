#pragma once

#include <functional>
class wxWindow;
class wxPanel;

namespace Slic3r::GUI {
class Plater;
bool local_semantic_validation_requested();
wxPanel* create_local_semantic_validation(wxWindow* parent, Plater*, std::function<void()> on_import);
}
