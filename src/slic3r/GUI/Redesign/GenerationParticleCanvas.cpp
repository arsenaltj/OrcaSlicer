#include "GenerationParticleCanvas.hpp"
#include "RedesignTheme.hpp"
#include "../GUI_App.hpp"
#include "../OpenGLManager.hpp"
#include "../GLShader.hpp"
#include <boost/log/trivial.hpp>
#include <glad/gl.h>
#include <wx/glcanvas.h>
#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/log.h>
#include <array>
#include <chrono>

namespace Slic3r::GUI {
namespace {
using Clock = std::chrono::steady_clock;
// The app lends one context to multiple canvases. Never leak particle state
// into the model/workbench canvas, including framebuffer and blending state.
struct GlState {
    GLint program, vao, buffer, draw_fbo, read_fbo, viewport[4], src_rgb, dst_rgb, src_alpha, dst_alpha, eq_rgb, eq_alpha;
    GLfloat clear[4]; GLboolean blend, depth, scissor, cull, stencil, discard, logic, srgb, point_size, color_mask[4], point_sprite{false};
    bool compatibility {false};
    GlState() {
        glGetIntegerv(GL_CURRENT_PROGRAM, &program); glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &buffer);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo); glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
        glGetIntegerv(GL_VIEWPORT, viewport); glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
        glGetIntegerv(GL_BLEND_SRC_RGB, &src_rgb); glGetIntegerv(GL_BLEND_DST_RGB, &dst_rgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &src_alpha); glGetIntegerv(GL_BLEND_DST_ALPHA, &dst_alpha);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &eq_rgb); glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &eq_alpha);
        blend = glIsEnabled(GL_BLEND); depth = glIsEnabled(GL_DEPTH_TEST);
        scissor = glIsEnabled(GL_SCISSOR_TEST); cull = glIsEnabled(GL_CULL_FACE);
        point_size = glIsEnabled(GL_PROGRAM_POINT_SIZE);
        stencil=glIsEnabled(GL_STENCIL_TEST); discard=glIsEnabled(GL_RASTERIZER_DISCARD);
        logic=glIsEnabled(GL_COLOR_LOGIC_OP); srgb=glIsEnabled(GL_FRAMEBUFFER_SRGB);
        glGetBooleanv(GL_COLOR_WRITEMASK,color_mask);
        compatibility = !OpenGLManager::get_gl_info().is_core_profile();
        if (compatibility) point_sprite=glIsEnabled(GL_POINT_SPRITE);
    }
    ~GlState() {
        glUseProgram(program); glBindVertexArray(vao); glBindBuffer(GL_ARRAY_BUFFER, buffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo); glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        glClearColor(clear[0], clear[1], clear[2], clear[3]);
        glBlendFuncSeparate(src_rgb, dst_rgb, src_alpha, dst_alpha);
        glBlendEquationSeparate(eq_rgb,eq_alpha); glColorMask(color_mask[0],color_mask[1],color_mask[2],color_mask[3]);
        const auto restore = [](GLenum cap, bool enabled) { if (enabled) glEnable(cap); else glDisable(cap); };
        restore(GL_BLEND, blend); restore(GL_DEPTH_TEST, depth); restore(GL_SCISSOR_TEST, scissor);
        restore(GL_CULL_FACE, cull); restore(GL_PROGRAM_POINT_SIZE, point_size);
        restore(GL_STENCIL_TEST,stencil); restore(GL_RASTERIZER_DISCARD,discard);
        restore(GL_COLOR_LOGIC_OP,logic); restore(GL_FRAMEBUFFER_SRGB,srgb);
        if (compatibility) restore(GL_POINT_SPRITE,point_sprite);
    }
};
}
struct GenerationParticleCanvas::Impl {
    GenerationParticleCanvas& owner;
    wxGLCanvas* canvas {nullptr};
    wxGLContext* context {nullptr}; // Borrowed from GUI_App.
    std::unique_ptr<GLShaderProgram> shader;
    GLuint vao {0}, buffers[2] {};
    std::shared_ptr<const GenerationParticles::Shape> uploaded[2];
    GenerationParticles::Frame frame, completed;
    wxBitmap static_bitmap;
    bool failed {false}, released {false};
    GenerationParticles::QualityPolicy quality;
    std::size_t frames {0};
    std::size_t completed_cpu_limit {3000};
    std::vector<double> intervals, costs;
    Clock::time_point last {};

    explicit Impl(GenerationParticleCanvas& panel) : owner(panel) {
        owner.SetBackgroundColour(RedesignTheme::background_colour()); owner.SetBackgroundStyle(wxBG_STYLE_PAINT);
        owner.SetWindowStyleFlag(owner.GetWindowStyleFlag() | wxCLIP_CHILDREN);
        owner.Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint_cpu(); });
        owner.Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
            if (canvas && canvas->GetRect() != owner.GetClientRect()) canvas->SetSize(owner.GetClientRect());
            if (canvas) canvas->Refresh(false); else owner.Refresh(false); event.Skip();
        });
    }
    bool initialize() {
        if (failed || released) return false;
        if (shader) return true;
        wxString cpu_only;
        if (wxGetEnv("ORCASLICER_GENERATION_PARTICLES_CPU",&cpu_only) && cpu_only == "1") { fail(); return false; }
        try {
            wxLogNull quiet;
            if (!canvas) {
                canvas = OpenGLManager::create_wxglcanvas(owner);
                canvas->SetName("generation-particle-canvas"); canvas->SetSize(owner.GetClientRect());
                canvas->Bind(wxEVT_PAINT, [this](wxPaintEvent&) { wxPaintDC dc(canvas); paint_gl(); });
            }
            context = wxGetApp().init_glcontext(*canvas);
            if (!context || !context->IsOK() || !canvas->SetCurrent(*context) || !wxGetApp().init_opengl() ||
                !OpenGLManager::get_gl_info().is_version_greater_or_equal_to(3, 1)) { fail(); return false; }
            GlState saved;
            GLShaderProgram::ShaderSources sources;
            sources[static_cast<std::size_t>(GLShaderProgram::EShaderType::Vertex)] = R"(#version 140
in vec4 particle;
in float intensity;
uniform vec2 viewport;
uniform float seconds, opacity, pixel_scale;
out float alpha;
void main() {
    float t=mod(seconds,10000.0), phase=particle.w;
    // Keep motion in sync with GenerationParticles::sample (CPU fallback).
    float wave=sin(t*2.2-particle.y*7.0+phase*0.4);
    vec2 p=particle.xy+vec2(0.010*sin(t*1.4+phase)+0.003*sin(t*2.5+phase*1.7)+particle.z*0.08*sin(t*0.55),
        0.011*cos(t*1.2+phase)+0.003*sin(t*2.1+phase*1.3));
    float depth=particle.z+0.025*sin(t*0.9+phase);
    float side=min(viewport.x,viewport.y)*0.87;
    gl_Position=vec4(p.x*side*2.0/viewport.x,-p.y*side*2.0/viewport.y,0.0,1.0);
    gl_PointSize=pixel_scale*(1.35+intensity*0.85)*(1.0+depth*2.2);
    alpha=opacity*intensity*(0.55+0.45*(0.5+0.5*wave));
})";
            sources[static_cast<std::size_t>(GLShaderProgram::EShaderType::Fragment)] = R"(#version 140
in float alpha;
out vec4 fragment;
void main() {
    float r=length(gl_PointCoord-vec2(0.5));
    if(r>0.5) discard;
    fragment=vec4(1.0,1.0,1.0,alpha*(1.0-smoothstep(0.1,0.5,r)));
})";
            shader = std::make_unique<GLShaderProgram>();
            if (!shader->init_from_texts("generation_particles", sources)) { fail(); return false; }
            glGenVertexArrays(1, &vao); glGenBuffers(2, buffers);
            return true;
        } catch (...) { fail(); return false; }
    }
    void fail() {
        failed = true;
        quality.require_cpu();
        if (canvas) canvas->Hide();
        owner.Refresh(false);
        BOOST_LOG_TRIVIAL(warning) << "[GenerationParticles] using bounded CPU fallback";
    }
    void release() {
        if (released) return;
        released = true;
        if (canvas && context && context->IsOK() && canvas->SetCurrent(*context)) {
            if (GLAD_GL_VERSION_3_0) {
                GlState saved;
                if (vao) glDeleteVertexArrays(1, &vao);
                glDeleteBuffers(2, buffers); shader.reset();
            } else shader.reset();
        } else {
            // The application owns the shared context and its final teardown.
            // Buffer names disappear with it if it can no longer be made current.
            shader.reset();
        }
        frame = {}; completed = {}; static_bitmap = wxNullBitmap;
        uploaded[0].reset(); uploaded[1].reset();
    }
    void record(double cost) {
        const auto now = Clock::now();
        if (last != Clock::time_point()) {
            const double gap = std::chrono::duration<double,std::milli>(now-last).count();
            intervals.push_back(gap);
        }
        last = now; costs.push_back(cost); ++frames;
        if (costs.size() < 120) return;
        auto gaps = intervals, cpu = costs; std::sort(gaps.begin(),gaps.end()); std::sort(cpu.begin(),cpu.end());
        const double interval_p95 = gaps.empty()?0:gaps[std::min(gaps.size()-1,gaps.size()*95/100)];
        const double cpu_p95 = cpu[std::min(cpu.size()-1,cpu.size()*95/100)];
        const auto tier = quality.quality();
        wxString trace;
        if (wxGetEnv("ORCASLICER_UI_LATENCY_TRACE", &trace) && trace == "1") {
            BOOST_LOG_TRIVIAL(info) << "[GenerationParticles] frames=" << frames << " tier="
                << (tier == GenerationParticles::Quality::Normal ? "normal" : tier == GenerationParticles::Quality::Low ? "low" :
                    tier == GenerationParticles::Quality::Cpu ? "cpu" : "static")
                << " dip100_px=" << owner.FromDIP(100) << " viewport=" << owner.GetClientSize().x << "x" << owner.GetClientSize().y
                << " interval_p95_ms=" << interval_p95
                << " interval_max_ms=" << (gaps.empty()?0:gaps.back())
                << " cpu_p95_ms=" << cpu_p95
                << " cpu_max_ms=" << cpu.back();
        }
        costs.clear(); intervals.clear();
        if (quality.observe(interval_p95,cpu_p95)) {
            BOOST_LOG_TRIVIAL(info) << "[GenerationParticles] quality adjusted using frame latency and drawing cost tier="
                << int(quality.quality());
        }
    }
    void draw_gl(const std::shared_ptr<const GenerationParticles::Shape>& shape, int slot, float opacity) {
        if (!shape || opacity <= 0) return;
        glBindBuffer(GL_ARRAY_BUFFER, buffers[slot]);
        if (uploaded[slot] != shape) {
            glBufferData(GL_ARRAY_BUFFER, shape->points.size()*sizeof(GenerationParticles::Point), shape->points.data(), GL_STATIC_DRAW);
            uploaded[slot] = shape;
        }
        const auto a = shader->get_attrib_location("particle"), b = shader->get_attrib_location("intensity");
        if (a < 0 || b < 0) return;
        glEnableVertexAttribArray(a); glEnableVertexAttribArray(b);
        glVertexAttribPointer(a, 4, GL_FLOAT, GL_FALSE, sizeof(GenerationParticles::Point), nullptr);
        glVertexAttribPointer(b, 1, GL_FLOAT, GL_FALSE, sizeof(GenerationParticles::Point), reinterpret_cast<void*>(4*sizeof(float)));
        shader->set_uniform("opacity", opacity);
        glDrawArrays(GL_POINTS, 0, GLsizei(std::min<std::size_t>(shape->points.size(),
            quality.quality() == GenerationParticles::Quality::Normal ? 12000 :
            quality.quality() == GenerationParticles::Quality::Static ? 512 : 3000)));
    }
    void paint_gl() {
        if (released || !owner.IsShownOnScreen()) { last = {}; return; }
        if (!initialize()) { owner.Refresh(false); return; }
        if (!canvas->SetCurrent(*context)) { fail(); return; }
        const auto begin = Clock::now();
        const auto size = canvas->GetClientSize(); const double scale = canvas->GetContentScaleFactor();
        const int width = std::max(1,int(size.x*scale)), height = std::max(1,int(size.y*scale));
        {
            GlState saved;
            glBindFramebuffer(GL_FRAMEBUFFER, 0); glViewport(0,0,width,height);
            glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST); glDisable(GL_CULL_FACE);
            glDisable(GL_STENCIL_TEST); glDisable(GL_RASTERIZER_DISCARD); glDisable(GL_COLOR_LOGIC_OP);
            glDisable(GL_FRAMEBUFFER_SRGB); glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
            glBlendEquation(GL_FUNC_ADD);
            glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glEnable(GL_PROGRAM_POINT_SIZE);
            if (!OpenGLManager::get_gl_info().is_core_profile()) glEnable(GL_POINT_SPRITE);
            const auto bg = RedesignTheme::background_colour();
            glClearColor(bg.Red()/255.f,bg.Green()/255.f,bg.Blue()/255.f,1); glClear(GL_COLOR_BUFFER_BIT);
            shader->start_using(); glBindVertexArray(vao);
            shader->set_uniform("viewport",std::array<float,2>{float(width),float(height)});
            shader->set_uniform("seconds", float(frame.seconds));
            shader->set_uniform("pixel_scale",float(scale*owner.FromDIP(100)/100.0));
            draw_gl(frame.logo,0,float(1-frame.subject_weight)); draw_gl(frame.subject,1,float(frame.subject_weight));
        }
        const double cpu_cost=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
        if (canvas->SwapBuffers()) { completed = frame; record(cpu_cost); }
        else { fail(); last = {}; }
    }
    template<class DC> void draw_cpu(DC& dc, const GenerationParticles::Frame& value, wxSize size, std::size_t limit) {
        dc.SetBackground(wxBrush(RedesignTheme::background_colour())); dc.Clear();
        auto gc = std::unique_ptr<wxGraphicsContext>(wxGraphicsContext::Create(dc));
        if (!gc) return;
        gc->SetPen(*wxTRANSPARENT_PEN);
        const double side = std::min(size.x,size.y)*0.87;
        const auto draw = [&](const auto& shape, double weight) {
            if (!shape || weight <= 0) return;
            const auto count = std::min(shape->points.size(),limit);
            for (std::size_t i = 0; i < count; ++i) {
                const auto p = GenerationParticles::sample(shape->points[i], value.seconds);
                const int alpha = int(std::clamp(weight*p.intensity,0.0,1.0)*255);
                gc->SetBrush(wxBrush(wxColour(255,255,255,alpha)));
                const double radius = std::max(0.75,double(owner.FromDIP(1))*0.9)*(1.0+p.z*2.2);
                gc->DrawEllipse(size.x*0.5+p.x*side-radius,size.y*0.5+p.y*side-radius,radius*2,radius*2);
            }
        };
        draw(value.logo,1-value.subject_weight); draw(value.subject,value.subject_weight);
    }
    void paint_cpu() {
        wxAutoBufferedPaintDC dc(&owner);
        if (released) return;
        if (canvas && !failed) {
            dc.SetBackground(wxBrush(RedesignTheme::background_colour())); dc.Clear();
            return; // The child owns the GPU surface.
        }
        const auto begin = Clock::now();
        const auto size = owner.GetClientSize();
        if (size.x <= 0 || size.y <= 0) return;
        const bool stationary = quality.quality() == GenerationParticles::Quality::Static;
        completed_cpu_limit=stationary ? 512 : 3000;
        if (stationary) {
            // Native parent invalidations can reach the child even when
            // present() requests no animation. Rasterize this shape once.
            if (static_bitmap.IsOk() && static_bitmap.GetSize() == size) {
                dc.DrawBitmap(static_bitmap,0,0); return;
            }
            static_bitmap = wxBitmap(size.x,size.y,32);
            wxMemoryDC memory(static_bitmap);
            draw_cpu(memory,frame,size,completed_cpu_limit);
            memory.SelectObject(wxNullBitmap);
            dc.DrawBitmap(static_bitmap,0,0);
        } else draw_cpu(dc,frame,size,completed_cpu_limit);
        completed=frame;
        record(std::chrono::duration<double,std::milli>(Clock::now()-begin).count());
    }
    wxBitmap retain() {
        if (released || completed.task!=frame.task || (!completed.logo && !completed.subject)) return {};
        const auto size=owner.GetClientSize();
        if (size.x<=0 || size.y<=0) return {};
        if (!failed && canvas && context && context->IsOK() && canvas->SetCurrent(*context) && shader) {
            GlState saved;
            const double scale=canvas->GetContentScaleFactor();
            const int w=std::max(1,int(size.x*scale)), h=std::max(1,int(size.y*scale));
            wxImage image(w,h); GLint pack=4,read=0,pack_buffer=0,row_length=0,skip_rows=0,skip_pixels=0;
            glGetIntegerv(GL_PACK_ALIGNMENT,&pack); glGetIntegerv(GL_READ_BUFFER,&read);
            glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING,&pack_buffer); glGetIntegerv(GL_PACK_ROW_LENGTH,&row_length);
            glGetIntegerv(GL_PACK_SKIP_ROWS,&skip_rows); glGetIntegerv(GL_PACK_SKIP_PIXELS,&skip_pixels);
            glBindFramebuffer(GL_FRAMEBUFFER,0); glReadBuffer(GL_FRONT); glPixelStorei(GL_PACK_ALIGNMENT,1);
            glBindBuffer(GL_PIXEL_PACK_BUFFER,0); glPixelStorei(GL_PACK_ROW_LENGTH,0);
            glPixelStorei(GL_PACK_SKIP_ROWS,0); glPixelStorei(GL_PACK_SKIP_PIXELS,0);
            glReadPixels(0,0,w,h,GL_RGB,GL_UNSIGNED_BYTE,image.GetData());
            glPixelStorei(GL_PACK_ALIGNMENT,pack); glBindBuffer(GL_PIXEL_PACK_BUFFER,pack_buffer);
            glPixelStorei(GL_PACK_ROW_LENGTH,row_length); glPixelStorei(GL_PACK_SKIP_ROWS,skip_rows);
            glPixelStorei(GL_PACK_SKIP_PIXELS,skip_pixels);
            glBindFramebuffer(GL_READ_FRAMEBUFFER,saved.read_fbo); glReadBuffer(read);
            image=image.Mirror(false);
            wxBitmap bitmap(image); bitmap.SetScaleFactor(scale); return bitmap;
        }
        wxBitmap bitmap(size.x,size.y,32); wxMemoryDC dc(bitmap);
        draw_cpu(dc,completed,size,completed_cpu_limit); dc.SelectObject(wxNullBitmap); return bitmap;
    }
};
GenerationParticleCanvas::GenerationParticleCanvas(wxWindow* parent) : wxPanel(parent), m_impl(std::make_unique<Impl>(*this)) {}
GenerationParticleCanvas::~GenerationParticleCanvas() { m_impl->release(); }
void GenerationParticleCanvas::release() { m_impl->release(); }
void GenerationParticleCanvas::present(GenerationParticles::Frame frame) {
    auto& self=*m_impl;
    if (self.released) return;
    const bool changed = self.frame.task!=frame.task || self.frame.logo!=frame.logo || self.frame.subject!=frame.subject || self.frame.subject_weight!=frame.subject_weight;
    if (self.quality.quality() == GenerationParticles::Quality::Static && !changed) return;
    if (changed) self.static_bitmap=wxNullBitmap;
    if (self.frame.task!=frame.task) {
        self.completed={};
        self.quality.reset(self.failed);
        self.costs.clear(); self.intervals.clear(); self.frames=0; self.last={};
    }
    self.frame=std::move(frame);
    if (!IsShownOnScreen()) { self.last={}; return; }
    if (!self.canvas && !self.failed) self.initialize();
    if (self.canvas && !self.failed) self.canvas->Refresh(false); else Refresh(false);
}
wxBitmap GenerationParticleCanvas::retain_frame() { return m_impl->retain(); }
} // namespace Slic3r::GUI
