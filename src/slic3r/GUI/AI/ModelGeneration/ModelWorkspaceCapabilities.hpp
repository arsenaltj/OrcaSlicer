#pragma once
#include "ModelLibraryApplication.hpp"
#include "BeautyWorkbenchSession.hpp"
#include "slic3r/GUI/AI/ColorMatching/ColorMatchingSession.hpp"

namespace Slic3r::GUI {
// Minimal composition needs only the root and explicit action ports. Preview
// rendering and native project commits are selected by the caller separately.
struct ModelWorkspaceCapabilities {
    explicit ModelWorkspaceCapabilities(boost::filesystem::path root, ModelLibraryApplication::Ports ports = {})
        : library(std::make_shared<ModelLibraryApplication>(std::move(root), std::move(ports))), colors(std::make_shared<ColorMatchingSession>()) {}
    std::shared_ptr<ModelLibraryApplication> library;
    std::shared_ptr<ColorMatchingSession> colors;
};
}
