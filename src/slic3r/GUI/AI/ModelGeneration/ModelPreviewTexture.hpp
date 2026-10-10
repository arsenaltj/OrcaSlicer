#pragma once
#include "slic3r/GUI/AI/Model/ModelArtifact.hpp"
#include "slic3r/GUI/AI/Model/SurfaceSelectionState.hpp"
#include "ModelPreviewTextureColor.hpp"
#include "slic3r/GUI/GLShader.hpp"
#include "slic3r/GUI/OpenGLManager.hpp"
#include <glad/gl.h>
#include <stdexcept>
#include <algorithm>
#include <unordered_map>
#include <boost/log/trivial.hpp>

namespace Slic3r::GUI {
// A texture draw owned by the existing preview context. Original face order and
// CPU editing topology stay unchanged; no texture resampling into vertex RGB.
class ModelPreviewTexture {
    using AlphaMode = AI::ModelArtifactTextureSurface::AlphaMode;
    struct Batch {
        GLint first{0}; GLsizei count{0};
        int image{-1}, wrap_s{10497}, wrap_t{10497}, min_filter{9729}, mag_filter{9729};
        AlphaMode alpha_mode{AlphaMode::Opaque}; float alpha_cutoff{0.5f};
        bool color_locked{false};
    };
    struct BlendFace { GLint first; size_t batch; Vec3d center; };
    GLuint m_vbo{0}, m_vao{0}, m_blend_indices{0};
    std::vector<GLuint> m_images;
    std::vector<Batch> m_batches;
    std::vector<BlendFace> m_blend_faces;
    mutable std::vector<Batch> m_blend_draws;
    mutable Transform3d m_sorted_view{Transform3d::Identity()};
    mutable bool m_sorted{false};
    size_t m_bytes{0};
    mutable bool m_sampler_diagnostics_logged{false};
public:
    ModelPreviewTexture(const AI::ModelArtifactTextureSurface& surface,
                        const indexed_triangle_set& mesh, const std::vector<Vec3f>& normals,
                        const AI::SurfaceSelectionPersistence::FaceColorOverrides& overrides = {}) {
        if (surface.faces.size() != mesh.indices.size() || normals.size() != mesh.indices.size()*3)
            throw std::runtime_error("Preview texture does not match the model faces.");
        if (!GLAD_GL_VERSION_2_0 || ::glGetIntegerv == nullptr)
            throw std::runtime_error("OpenGL texture preview is not initialized.");
        GLint maximum=0; ::glGetIntegerv(GL_MAX_TEXTURE_SIZE,&maximum);
        for (const auto& image : surface.images)
            if (image.width<=0 || image.height<=0 || image.width>maximum || image.height>maximum ||
                image.rgba.size()!=size_t(image.width)*image.height*4)
                throw std::runtime_error("The color texture exceeds this OpenGL preview capability.");
        std::vector<float> vertices; vertices.reserve(surface.faces.size()*3*12);
        std::unordered_map<size_t, std::array<float, 3>> locked_colors;
        for (const auto& entry : overrides) {
            if (entry.first >= surface.faces.size())
                throw std::runtime_error("Preview color override does not match the model faces.");
            locked_colors[entry.first] = entry.second;
        }
        for (size_t f=0; f<surface.faces.size(); ++f) {
            const auto lock = locked_colors.find(f);
            const bool color_locked = lock != locked_colors.end();
            const auto face = preview_texture_face_color(surface.faces[f], color_locked ? &lock->second : nullptr);
            if (face.image>=0 && size_t(face.image)>=surface.images.size())
                throw std::runtime_error("Preview texture reference is invalid.");
            if (m_batches.empty() || m_batches.back().image!=face.image ||
                m_batches.back().wrap_s!=face.wrap_s || m_batches.back().wrap_t!=face.wrap_t ||
                m_batches.back().min_filter!=face.min_filter || m_batches.back().mag_filter!=face.mag_filter ||
                m_batches.back().alpha_mode!=face.alpha_mode || m_batches.back().alpha_cutoff!=face.alpha_cutoff ||
                m_batches.back().color_locked!=color_locked)
                m_batches.push_back({GLint(f*3),0,face.image,face.wrap_s,face.wrap_t,
                    face.min_filter,face.mag_filter,face.alpha_mode,face.alpha_cutoff,color_locked});
            m_batches.back().count+=3;
            if (face.alpha_mode == AlphaMode::Blend) {
                Vec3d center = Vec3d::Zero();
                for (int c=0; c<3; ++c) center += mesh.vertices[mesh.indices[f][c]].cast<double>();
                m_blend_faces.push_back({GLint(f*3),m_batches.size()-1,center/3.0});
            }
            for (size_t c=0; c<3; ++c) {
                const auto& position=mesh.vertices[mesh.indices[f][c]]; const auto& normal=normals[f*3+c];
                for (int ch=0; ch<3; ++ch) vertices.push_back(position[ch]);
                for (int ch=0; ch<3; ++ch) vertices.push_back(normal[ch]);
                vertices.insert(vertices.end(),face.corners[c].uv.begin(),face.corners[c].uv.end());
                vertices.insert(vertices.end(),face.corners[c].multiplier.begin(),face.corners[c].multiplier.end());
            }
        }
        GLint array=0,active=0,texture=0,alignment=0;
        ::glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&array); ::glGetIntegerv(GL_ACTIVE_TEXTURE,&active);
        ::glActiveTexture(GL_TEXTURE0); ::glGetIntegerv(GL_TEXTURE_BINDING_2D,&texture);
        ::glGetIntegerv(GL_UNPACK_ALIGNMENT,&alignment); ::glPixelStorei(GL_UNPACK_ALIGNMENT,1);
        ::glGenBuffers(1,&m_vbo); ::glBindBuffer(GL_ARRAY_BUFFER,m_vbo);
        ::glBufferData(GL_ARRAY_BUFFER,vertices.size()*sizeof(float),vertices.data(),GL_STATIC_DRAW);
        m_bytes=vertices.size()*sizeof(float);
        if (OpenGLManager::get_gl_info().is_core_profile()) ::glGenVertexArrays(1,&m_vao);
        if (!m_blend_faces.empty()) ::glGenBuffers(1,&m_blend_indices);
        m_images.resize(surface.images.size()); ::glGenTextures(GLsizei(m_images.size()),m_images.data());
        for (size_t i=0; i<surface.images.size(); ++i) {
            const auto& image=surface.images[i]; ::glBindTexture(GL_TEXTURE_2D,m_images[i]);
            ::glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
            ::glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
            ::glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,image.width,image.height,0,GL_RGBA,GL_UNSIGNED_BYTE,image.rgba.data());
            m_bytes+=image.rgba.size();
            GLint last_level=0;
            if (std::any_of(m_batches.begin(),m_batches.end(),[i](const Batch& b) { return b.image==int(i) && b.min_filter>=9984; })) {
                const auto levels=AI::model_texture_mipmaps(image);
                last_level=GLint(levels.size()-1);
                for (size_t level=1; level<levels.size(); ++level) {
                    const auto& mip=levels[level];
                    ::glTexImage2D(GL_TEXTURE_2D,GLint(level),GL_RGBA,mip.width,mip.height,0,GL_RGBA,GL_UNSIGNED_BYTE,mip.rgba.data());
                    m_bytes+=mip.rgba.size();
                }
            }
            ::glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_BASE_LEVEL,0);
            ::glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAX_LEVEL,last_level);
            GLint uploaded_width=0;
            ::glGetTexLevelParameteriv(GL_TEXTURE_2D,last_level,GL_TEXTURE_WIDTH,&uploaded_width);
            BOOST_LOG_TRIVIAL(info) << "[preview-sampler] image=" << i << " size=" << image.width << "x" << image.height << " last_level=" << last_level << " uploaded_width=" << uploaded_width;
        }
        for (const auto& batch:m_batches)
            BOOST_LOG_TRIVIAL(info) << "[preview-sampler] first=" << batch.first << " min=" << batch.min_filter << " mag=" << batch.mag_filter;
        ::glBindBuffer(GL_ARRAY_BUFFER,array); ::glBindTexture(GL_TEXTURE_2D,texture);
        ::glPixelStorei(GL_UNPACK_ALIGNMENT,alignment); ::glActiveTexture(active);
    }
    ~ModelPreviewTexture() {
        if (m_vbo) ::glDeleteBuffers(1,&m_vbo);
        if (m_blend_indices) ::glDeleteBuffers(1,&m_blend_indices);
        if (m_vao) ::glDeleteVertexArrays(1,&m_vao);
        if (!m_images.empty()) ::glDeleteTextures(GLsizei(m_images.size()),m_images.data());
    }
    ModelPreviewTexture(const ModelPreviewTexture&)=delete;
    ModelPreviewTexture& operator=(const ModelPreviewTexture&)=delete;
    size_t memory_used() const {
        return m_bytes + m_blend_faces.capacity()*sizeof(BlendFace) +
            m_blend_draws.capacity()*sizeof(Batch) + m_blend_faces.size()*3*sizeof(GLuint);
    }
    void render(GLShaderProgram* shader, const Transform3d& view_model) const {
        GLint array=0,active=0,texture=0,vao=0;
        ::glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&array); ::glGetIntegerv(GL_ACTIVE_TEXTURE,&active);
        ::glActiveTexture(GL_TEXTURE0); ::glGetIntegerv(GL_TEXTURE_BINDING_2D,&texture);
        if (m_vao) { ::glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&vao); ::glBindVertexArray(m_vao); }
        ::glBindBuffer(GL_ARRAY_BUFFER,m_vbo);
        std::array<GLint,4> attributes{shader->get_attrib_location("v_position"),shader->get_attrib_location("v_normal"),
            shader->get_attrib_location("v_tex_coord"),shader->get_attrib_location("v_texture_multiplier")};
        const int sizes[4]={3,3,2,4}; const size_t offsets[4]={0,3,6,8};
        for (size_t i=0; i<attributes.size(); ++i) if(attributes[i]>=0) {
            ::glVertexAttribPointer(attributes[i],sizes[i],GL_FLOAT,GL_FALSE,12*sizeof(float),reinterpret_cast<const void*>(offsets[i]*sizeof(float)));
            ::glEnableVertexAttribArray(attributes[i]);
        }
        shader->set_uniform("preview_texture_enabled",true); shader->set_uniform("preview_texture",0);
        GLboolean old_depth_write = GL_TRUE;
        ::glGetBooleanv(GL_DEPTH_WRITEMASK,&old_depth_write);
        const bool old_blend = ::glIsEnabled(GL_BLEND);
        GLint src_rgb=0,dst_rgb=0,src_alpha=0,dst_alpha=0,equation_rgb=0,equation_alpha=0;
        ::glGetIntegerv(GL_BLEND_SRC_RGB,&src_rgb); ::glGetIntegerv(GL_BLEND_DST_RGB,&dst_rgb);
        ::glGetIntegerv(GL_BLEND_SRC_ALPHA,&src_alpha); ::glGetIntegerv(GL_BLEND_DST_ALPHA,&dst_alpha);
        ::glGetIntegerv(GL_BLEND_EQUATION_RGB,&equation_rgb); ::glGetIntegerv(GL_BLEND_EQUATION_ALPHA,&equation_alpha);
        const auto bind_batch = [&](const Batch& batch) {
            shader->set_uniform("preview_texture_color_lock",batch.color_locked);
            shader->set_uniform("preview_alpha_mode",int(batch.alpha_mode));
            shader->set_uniform("preview_alpha_cutoff",batch.alpha_cutoff);
            const bool textured=batch.image>=0; shader->set_uniform("preview_texture_has_image",textured);
            if (textured) {
                ::glBindTexture(GL_TEXTURE_2D,m_images[batch.image]);
                ::glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,batch.wrap_s);
                ::glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,batch.wrap_t);
                ::glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,batch.min_filter);
                ::glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,batch.mag_filter);
                if (!m_sampler_diagnostics_logged) {
                    GLint min_filter=0,mag_filter=0,last_level=0;
                    ::glGetTexParameteriv(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,&min_filter);
                    ::glGetTexParameteriv(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,&mag_filter);
                    ::glGetTexParameteriv(GL_TEXTURE_2D,GL_TEXTURE_MAX_LEVEL,&last_level);
                    BOOST_LOG_TRIVIAL(info) << "[preview-sampler-draw] first=" << batch.first
                        << " actual_min=" << min_filter << " actual_mag=" << mag_filter
                        << " max_level=" << last_level;
                }
            }
        };
        // OPAQUE ignores native alpha; MASK discards before writing depth.
        // Blend triangles are sorted in view space without reordering the
        // original CPU mesh or editing face IDs.
        ::glDisable(GL_BLEND); ::glDepthMask(GL_TRUE);
        for (const auto& batch:m_batches) if (batch.alpha_mode != AlphaMode::Blend) {
            bind_batch(batch);
            ::glDrawArrays(GL_TRIANGLES,batch.first,batch.count);
        }
        if (m_blend_indices) {
            GLint element=0; ::glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING,&element);
            ::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,m_blend_indices);
            if (!m_sorted || !(m_sorted_view.matrix().array() == view_model.matrix().array()).all()) {
                std::vector<std::pair<double,size_t>> order; order.reserve(m_blend_faces.size());
                for (size_t i=0; i<m_blend_faces.size(); ++i)
                    order.emplace_back((view_model*m_blend_faces[i].center).z(),i);
                std::sort(order.begin(),order.end());
                std::vector<GLuint> indices; indices.reserve(order.size()*3);
                m_blend_draws.clear();
                size_t previous_batch = size_t(-1);
                for (const auto& entry:order) {
                    const auto& face=m_blend_faces[entry.second];
                    if (face.batch != previous_batch) {
                        Batch batch=m_batches[face.batch]; batch.first=GLint(indices.size()); batch.count=0;
                        m_blend_draws.push_back(batch); previous_batch=face.batch;
                    }
                    m_blend_draws.back().count+=3;
                    for (int c=0; c<3; ++c) indices.push_back(GLuint(face.first+c));
                }
                ::glBufferData(GL_ELEMENT_ARRAY_BUFFER,indices.size()*sizeof(GLuint),indices.data(),GL_DYNAMIC_DRAW);
                m_sorted_view=view_model; m_sorted=true;
            }
            ::glEnable(GL_BLEND); ::glDepthMask(GL_FALSE);
            ::glBlendEquationSeparate(GL_FUNC_ADD,GL_FUNC_ADD);
            ::glBlendFuncSeparate(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
            for (const auto& batch:m_blend_draws) {
                bind_batch(batch);
                ::glDrawElements(GL_TRIANGLES,batch.count,GL_UNSIGNED_INT,
                    reinterpret_cast<const void*>(size_t(batch.first)*sizeof(GLuint)));
            }
            ::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,element);
        }
        ::glDepthMask(old_depth_write);
        ::glBlendFuncSeparate(src_rgb,dst_rgb,src_alpha,dst_alpha);
        ::glBlendEquationSeparate(equation_rgb,equation_alpha);
        if (old_blend) ::glEnable(GL_BLEND); else ::glDisable(GL_BLEND);
        m_sampler_diagnostics_logged=true;
        shader->set_uniform("preview_texture_enabled",false);
        shader->set_uniform("preview_texture_color_lock",false);
        for (GLint attribute:attributes) if(attribute>=0) ::glDisableVertexAttribArray(attribute);
        if(m_vao) ::glBindVertexArray(vao);
        ::glBindBuffer(GL_ARRAY_BUFFER,array); ::glBindTexture(GL_TEXTURE_2D,texture); ::glActiveTexture(active);
    }
};
} // namespace Slic3r::GUI
