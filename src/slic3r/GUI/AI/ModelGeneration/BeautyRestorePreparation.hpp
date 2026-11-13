#pragma once
#include "ModelPreview3D.hpp"

namespace Slic3r::GUI {
// One worker result, consumed at the existing atomic GUI commit boundary.
// Read leases end at installation; they are never retained in undo history.
struct BeautyRestorePreparation {
    ModelPreview3D::PreparedModel model;
    std::string error;
};
}
