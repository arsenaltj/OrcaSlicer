#pragma once
#include "ModelLibraryEntry.hpp"
#include "slic3r/AI/Contracts/GeneratedModelArtifact.hpp"

namespace Slic3r::GUI {
// A GUI open request, separate from restoring a generation session.
struct WorkbenchOpenRequest {
    AI::GeneratedModelArtifact artifact;
    ModelLibraryEntry entry;
    std::string expected_sha256;
};
}
