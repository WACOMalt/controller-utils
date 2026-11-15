#pragma once
#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES 1
#endif
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include "button_animation.h"
#include "touchpad_glow.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ct::ui {
inline GLuint model_shader(GLenum type, const char *source) {
    const GLuint result = glCreateShader(type);
    glShaderSource(result, 1, &source, nullptr);
    glCompileShader(result);
    GLint status = 0;
    glGetShaderiv(result, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[2048]{};
        glGetShaderInfoLog(result, sizeof(log), nullptr, log);
        glDeleteShader(result);
        throw std::runtime_error(std::string("shader compilation: ") + log);
    }
    return result;
}

struct ModelRenderer {
    static constexpr int Size = 1024;
    ModelRenderer() = default;
    ModelRenderer(const ModelRenderer &) = delete;
    ModelRenderer &operator=(const ModelRenderer &) = delete;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    GLuint program = 0, vao = 0, buffer = 0, depth = 0;
    GLuint textures[2]{}, framebuffers[2]{};
    GLuint appearance_textures[2]{};
    GLsizei count = 0;
    bool current = false;
    bool stereo_canvas = false;
    ct::Point mesh_center{};
    float mesh_radius = 0;
    ct::ButtonAnimation buttons;
    TouchpadGlow touchpads;
    GLint button_pose_uniform = -1;
    GLint pad_contact_uniform = -1;

    ~ModelRenderer() {
        if (current) {
            glFinish();
            glDeleteFramebuffers(2, framebuffers);
            glDeleteRenderbuffers(1, &depth);
            glDeleteTextures(2, textures);
            glDeleteTextures(2, appearance_textures);
            glDeleteBuffers(1, &buffer);
            glDeleteVertexArrays(1, &vao);
            glDeleteProgram(program);
            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }
        if (context != EGL_NO_CONTEXT)
            eglDestroyContext(display, context);
        if (surface != EGL_NO_SURFACE)
            eglDestroySurface(display, surface);
        if (display != EGL_NO_DISPLAY)
            eglTerminate(display);
        eglReleaseThread();
    }

    void initialize(const std::vector<ct::Vertex> &vertices, bool stereo = false,
                    const ct::Surface *appearance = nullptr) {
        stereo_canvas = stereo;
        ct::Point lower{vertices.front().position[0], vertices.front().position[1],
                        vertices.front().position[2]},
            upper = lower;
        for (const auto &vertex : vertices)
            for (int i = 0; i < 3; ++i) {
                lower[i] = std::min(lower[i], vertex.position[i]);
                upper[i] = std::max(upper[i], vertex.position[i]);
            }
        for (int i = 0; i < 3; ++i)
            mesh_center[i] = (lower[i] + upper[i]) / 2;
        for (const auto &vertex : vertices) {
            float squared = 0;
            for (int i = 0; i < 3; ++i)
                squared += std::pow(vertex.position[i] - mesh_center[i], 2);
            mesh_radius = std::max(mesh_radius, std::sqrt(squared));
        }
        display =
            eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
        if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr) ||
            !eglBindAPI(EGL_OPENGL_API))
            throw std::runtime_error("surfaceless EGL OpenGL is unavailable");
        const EGLint attributes[] = {EGL_SURFACE_TYPE,
                                     EGL_PBUFFER_BIT,
                                     EGL_RENDERABLE_TYPE,
                                     EGL_OPENGL_BIT,
                                     EGL_RED_SIZE,
                                     8,
                                     EGL_GREEN_SIZE,
                                     8,
                                     EGL_BLUE_SIZE,
                                     8,
                                     EGL_ALPHA_SIZE,
                                     8,
                                     EGL_NONE};
        EGLConfig config{};
        EGLint found = 0;
        if (!eglChooseConfig(display, attributes, &config, 1, &found) || found != 1)
            throw std::runtime_error("no EGL RGBA configuration");
        const EGLint surface_attributes[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        surface = eglCreatePbufferSurface(display, config, surface_attributes);
        const EGLint context_attributes[] = {EGL_CONTEXT_MAJOR_VERSION,
                                             3,
                                             EGL_CONTEXT_MINOR_VERSION,
                                             3,
                                             EGL_CONTEXT_OPENGL_PROFILE_MASK,
                                             EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                                             EGL_NONE};
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
        if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
            !eglMakeCurrent(display, surface, surface, context))
            throw std::runtime_error("cannot create the off-screen OpenGL context");
        current = true;
        const char *vertex_source = R"(#version 330 core
            layout(location=0) in vec3 position;
            layout(location=1) in vec3 normal;
            layout(location=2) in vec2 coordinates;
            layout(location=3) in float part;
            layout(location=4) in float touchpad;
            uniform mat4 mvp;
            uniform mat4 model;
            uniform mat4 button_pose[20];
            out vec3 n;
            out vec3 p;
            out vec2 uv;
            out vec2 pad_surface;
            flat out int pad;
            void main() {
                mat4 component = button_pose[int(part)];
                vec4 vertex = component * vec4(position,1);
                gl_Position = mvp * vertex; n = mat3(model) * mat3(component) * normal;
                p = (model * vertex).xyz; uv = coordinates;
                pad_surface = position.xz; pad = int(touchpad);
            })";
        const char *fragment_source = R"(#version 330 core
            in vec3 n;
            in vec3 p;
            in vec2 uv;
            in vec2 pad_surface;
            flat in int pad;
            uniform bool has_appearance;
            uniform sampler2D color_roughness;
            uniform sampler2D normal_metalness;
            uniform mat3x2 pad_mapping[2];
            uniform vec4 pad_contact[2];
            out vec4 color;
            void main() {
                vec3 normal = normalize(n);
                if (!has_appearance) {
                    float light = .3+.7*max(dot(normal,normalize(vec3(.4,.8,1))),0);
                    color = vec4(vec3(.40,.45,.50)*light,1);
                } else {
                    vec4 surface = texture(color_roughness,uv), detail = texture(normal_metalness,uv);
                    vec3 dp1=dFdx(p), dp2=dFdy(p); vec2 duv1=dFdx(uv), duv2=dFdy(uv);
                    float determinant=duv1.x*duv2.y-duv1.y*duv2.x;
                    if (abs(determinant)>1e-12) {
                        vec3 tangent=(dp1*duv2.y-dp2*duv1.y)/determinant;
                        tangent=normalize(tangent-normal*dot(normal,tangent));
                        vec3 bitangent=normalize(cross(normal,tangent));
                        if (dot(bitangent,(-dp1*duv2.x+dp2*duv1.x)/determinant)<0) bitangent=-bitangent;
                        normal=normalize(mat3(tangent,bitangent,normal)*(detail.rgb*2-1));
                    }
                    vec3 base=surface.rgb;
                    float light=.48+.28*max(dot(normal,normalize(vec3(-.3,.85,.4))),0)
                                   +.08*max(dot(normal,normalize(vec3(.8,.3,-.5))),0);
                    vec3 result=base*light;
                    color=vec4(pow(max(result,vec3(0)),vec3(1/2.2)),1);
                }
                if (pad > 0) {
                    vec4 contact = pad_contact[pad-1];
                    if (contact.z > 0) {
                        vec2 center = pad_mapping[pad-1]*vec3(1,contact.xy);
                        vec2 delta = pad_surface-center;
                        float spot = exp(-3*dot(delta,delta)/(.006*.006));
                        vec3 blue = pow(vec3(26./255.,159./255.,1),vec3(2.2));
                        vec3 emission = blue*.10*smoothstep(0.,1.,contact.z)*spot;
                        color.rgb = pow(pow(color.rgb,vec3(2.2))+emission,vec3(1/2.2));
                    }
                }
            })";
        const GLuint vertex_shader = model_shader(GL_VERTEX_SHADER, vertex_source);
        GLuint fragment_shader = 0;
        try {
            fragment_shader = model_shader(GL_FRAGMENT_SHADER, fragment_source);
        } catch (...) {
            glDeleteShader(vertex_shader);
            throw;
        }
        program = glCreateProgram();
        glAttachShader(program, vertex_shader);
        glAttachShader(program, fragment_shader);
        glLinkProgram(program);
        glDeleteShader(vertex_shader);
        glDeleteShader(fragment_shader);
        GLint status = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &status);
        if (!status)
            throw std::runtime_error("model shader link failed");
        button_pose_uniform = glGetUniformLocation(program, "button_pose[0]");
        if (button_pose_uniform < 0)
            throw std::runtime_error("button animation uniform is unavailable");
        pad_contact_uniform = glGetUniformLocation(program, "pad_contact[0]");
        const auto pad_mapping_uniform = glGetUniformLocation(program, "pad_mapping[0]");
        if (pad_contact_uniform < 0 || pad_mapping_uniform < 0)
            throw std::runtime_error("touchpad glow uniforms are unavailable");
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenBuffers(1, &buffer);
        glBindBuffer(GL_ARRAY_BUFFER, buffer);
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(ct::Vertex), vertices.data(),
                     GL_STATIC_DRAW);
        count = static_cast<GLsizei>(vertices.size());
        for (GLuint i = 0; i < 2; ++i) {
            glEnableVertexAttribArray(i);
            glVertexAttribPointer(i, 3, GL_FLOAT, GL_FALSE, sizeof(ct::Vertex),
                                  reinterpret_cast<void *>(i == 0 ? offsetof(ct::Vertex, position)
                                                                  : offsetof(ct::Vertex, normal)));
        }
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(ct::Vertex),
                              reinterpret_cast<void *>(offsetof(ct::Vertex, uv)));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(ct::Vertex),
                              reinterpret_cast<void *>(offsetof(ct::Vertex, part)));
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(ct::Vertex),
                              reinterpret_cast<void *>(offsetof(ct::Vertex, touchpad)));
        glUseProgram(program);
        glUniformMatrix3x2fv(pad_mapping_uniform, buttons.pad_mapping.size(), GL_FALSE,
                             buttons.pad_mapping[0].data());
        glUniform1i(glGetUniformLocation(program, "has_appearance"), appearance != nullptr);
        if (appearance) {
            glGenTextures(2, appearance_textures);
            const std::vector<unsigned char> *images[2] = {&appearance->color_roughness,
                                                           &appearance->normal_metalness};
            for (int i = 0; i < 2; ++i) {
                glActiveTexture(GL_TEXTURE0 + i);
                glBindTexture(GL_TEXTURE_2D, appearance_textures[i]);
                glTexImage2D(GL_TEXTURE_2D, 0, i ? GL_RGBA8 : GL_SRGB8_ALPHA8, appearance->width,
                             appearance->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, images[i]->data());
                glGenerateMipmap(GL_TEXTURE_2D);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glUniform1i(
                    glGetUniformLocation(program, i ? "normal_metalness" : "color_roughness"), i);
            }
            glActiveTexture(GL_TEXTURE0);
        }
        glGenRenderbuffers(1, &depth);
        glBindRenderbuffer(GL_RENDERBUFFER, depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, stereo ? 2 * Size : Size,
                              Size);
        glGenTextures(2, textures);
        glGenFramebuffers(2, framebuffers);
        for (int eye = 0; eye < 2; ++eye) {
            glBindTexture(GL_TEXTURE_2D, textures[eye]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, stereo && eye == 0 ? 2 * Size : Size, Size, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindFramebuffer(GL_FRAMEBUFFER, framebuffers[eye]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                   textures[eye], 0);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE ||
                glGetError() != GL_NO_ERROR)
                throw std::runtime_error("model framebuffer creation failed");
        }
    }

    void render(const ct::Matrix &projection, const ct::Matrix &view, const ct::Matrix &model,
                int eye = 0) {
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffers[stereo_canvas ? 0 : eye]);
        glViewport(stereo_canvas ? eye * Size : 0, 0, Size, Size);
        if (stereo_canvas) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(eye * Size, 0, Size, Size);
        }
        glEnable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glUseProgram(program);
        glBindVertexArray(vao);
        for (int i = 0; i < 2; ++i)
            if (appearance_textures[i]) {
                glActiveTexture(GL_TEXTURE0 + i);
                glBindTexture(GL_TEXTURE_2D, appearance_textures[i]);
            }
        glActiveTexture(GL_TEXTURE0);
        const auto mvp = ct::multiply(projection, ct::multiply(view, model));
        glUniformMatrix4fv(glGetUniformLocation(program, "mvp"), 1, GL_FALSE, mvp.data());
        glUniformMatrix4fv(glGetUniformLocation(program, "model"), 1, GL_FALSE, model.data());
        glUniformMatrix4fv(button_pose_uniform, buttons.poses.size(), GL_FALSE,
                           buttons.poses[0].data());
        glUniform4fv(pad_contact_uniform, touchpads.contacts.size(), touchpads.contacts[0].data());
        glDrawArrays(GL_TRIANGLES, 0, count);
        glDisable(GL_SCISSOR_TEST);
        glFinish();
        if (glGetError() != GL_NO_ERROR)
            throw std::runtime_error("model drawing failed");
    }
};

} // namespace ct::ui
