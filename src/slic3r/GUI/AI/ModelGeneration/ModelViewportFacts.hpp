#pragma once

#include <wx/clntdata.h>
#include <wx/stattext.h>
#include <cstddef>

namespace Slic3r::GUI {

// The existing statistics label owns the scalar facts of its displayed artifact.
// Keep the viewport projection typed; do not parse translated/wrapped labels or
// read a saved report that may describe a different preview candidate.
struct ModelViewportFacts final : wxClientData {
    ModelViewportFacts(size_t faces, size_t values) : triangles(faces), colors(values) {}
    size_t triangles;
    size_t colors;
};

inline void set_model_viewport_facts(wxStaticText* owner, size_t triangles, size_t colors)
{
    owner->SetClientObject(new ModelViewportFacts(triangles, colors));
}

} // namespace Slic3r::GUI
