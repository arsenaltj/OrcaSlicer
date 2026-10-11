#pragma once
#include "ModelLibraryLoader.hpp"

namespace Slic3r::GUI {
// Native image decoder adapter. No windows are created by this factory.
std::unique_ptr<ModelLibraryLoader> make_model_library_loader();
}
