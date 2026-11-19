#pragma once
#include "GenerationParticleModel.hpp"
#include <wx/panel.h>
#include <wx/bitmap.h>
#include <memory>

namespace Slic3r::GUI {
// A presentation surface only. It never reads task state, files or providers.
class GenerationParticleCanvas final : public wxPanel {
public:
    explicit GenerationParticleCanvas(wxWindow* parent);
    ~GenerationParticleCanvas() override;
    void present(GenerationParticles::Frame frame);
    wxBitmap retain_frame();
    void release();
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
