#pragma once
#include "GenerationParticleModel.hpp"
#include <boost/filesystem/path.hpp>

namespace Slic3r::GUI::GenerationParticles {
// Display-only CPU inference. No job state, network, provider or disk output.
// The bounded worker owns cancellation; only the installed, pinned runtime is
// used. Missing/invalid runtime returns a reason and leaves generation alone.
MaskResult infer_foreground(const Image& image, const boost::filesystem::path& runtime,
    const std::function<bool()>& canceled);
// Release the cached CPU session while the native runtime is still alive.
// Call after presentation workers drain, before DLL/static teardown.
void release_foreground_runtime();
}
