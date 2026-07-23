#ifdef APP_BACKEND_OPENGL
#include "opengl_backend.h"

#include "model.h"
#include "paths.h"
#include "projection.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

#include <GLFW/glfw3.h>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Reads a shader source file located relative to the repo root, e.g.
// "assets/shaders/opengl/model.vert.glsl".
static std::string readShaderFile(std::string_view relative) {
    const std::filesystem::path path = assetPath(relative);
    std::ifstream file(path);
    if (!file) {
        std::cerr << "Failed to open shader file: " << path << "\n";
        return {};
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

static GLuint compileProgram(std::string_view vertPath, std::string_view fragPath, const char *label) {
    const std::string vertSrc = readShaderFile(vertPath);
    const std::string fragSrc = readShaderFile(fragPath);
    const char *vert = vertSrc.c_str();
    const char *frag = fragSrc.c_str();
    auto compile = [](GLenum type, const char *src) {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[512];
            glGetShaderInfoLog(s, sizeof(log), nullptr, log);
            std::cerr << "Shader error: " << log << "\n";
        }
        return s;
    };
    GLuint vs = compile(GL_VERTEX_SHADER, vert);
    GLuint fs = compile(GL_FRAGMENT_SHADER, frag);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        std::cerr << "Program link error (" << label << "): " << log << "\n";
    }

    glObjectLabel(GL_PROGRAM, prog, -1, label);
    return prog;
}

// ---------------------------------------------------------------------------
// Init / shutdown
// ---------------------------------------------------------------------------

bool OpenGLBackend::init(GLFWwindow *window) {
    m_window = window;
    if (!gladLoadGL(glfwGetProcAddress)) {
        std::cerr << "Failed to initialize OpenGL via Glad2\n";
        return false;
    }
    std::cout << "OpenGL " << glGetString(GL_VERSION) << "\n";

    glEnable(GL_DEBUG_OUTPUT);
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    glDebugMessageCallback([](GLenum, GLenum type, GLuint, GLenum severity,
                              GLsizei, const GLchar *message, const void *) {
        if (severity == GL_DEBUG_SEVERITY_NOTIFICATION)
            return;
        std::cerr << "GL [" << (type == GL_DEBUG_TYPE_ERROR ? "error" : "info") << "]: "
                  << message << "\n";
    },
                           nullptr);

    return initImGui() && initModel();
}

bool OpenGLBackend::initImGui() {
    m_imguiShader = compileProgram("assets/shaders/opengl/imgui.vert.glsl",
                                   "assets/shaders/opengl/imgui.frag.glsl", "ImGui Program");

    glCreateVertexArrays(1, &m_imguiVao);
    glObjectLabel(GL_VERTEX_ARRAY, m_imguiVao, -1, "ImGui VAO");

    glVertexArrayAttribFormat(m_imguiVao, 0, 2, GL_FLOAT, GL_FALSE, offsetof(ImDrawVert, pos));
    glVertexArrayAttribFormat(m_imguiVao, 1, 2, GL_FLOAT, GL_FALSE, offsetof(ImDrawVert, uv));
    glVertexArrayAttribFormat(m_imguiVao, 2, 4, GL_UNSIGNED_BYTE, GL_TRUE, offsetof(ImDrawVert, col));

    glVertexArrayAttribBinding(m_imguiVao, 0, 0);
    glVertexArrayAttribBinding(m_imguiVao, 1, 0);
    glVertexArrayAttribBinding(m_imguiVao, 2, 0);

    glEnableVertexArrayAttrib(m_imguiVao, 0);
    glEnableVertexArrayAttrib(m_imguiVao, 1);
    glEnableVertexArrayAttrib(m_imguiVao, 2);

    glCreateBuffers(1, &m_imguiVbo);
    glObjectLabel(GL_BUFFER, m_imguiVbo, -1, "ImGui VBO");
    glCreateBuffers(1, &m_imguiIbo);
    glObjectLabel(GL_BUFFER, m_imguiIbo, -1, "ImGui IBO");

    glVertexArrayVertexBuffer(m_imguiVao, 0, m_imguiVbo, 0, sizeof(ImDrawVert));
    glVertexArrayElementBuffer(m_imguiVao, m_imguiIbo);

    glCreateSamplers(1, &m_fontSampler);
    glObjectLabel(GL_SAMPLER, m_fontSampler, -1, "ImGui Font Sampler");
    glSamplerParameteri(m_fontSampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(m_fontSampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // Font atlas
    unsigned char *pixels;
    int w, h;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    glCreateTextures(GL_TEXTURE_2D, 1, &m_fontTex);
    glObjectLabel(GL_TEXTURE, m_fontTex, -1, "ImGui Font Atlas");
    glTextureStorage2D(m_fontTex, 1, GL_RGBA8, w, h);
    glTextureSubImage2D(m_fontTex, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    ImGui::GetIO().Fonts->SetTexID((ImTextureID)(intptr_t)m_fontTex);

    return true;
}

bool OpenGLBackend::initModel() {
    m_modelShader = compileProgram("assets/shaders/opengl/model.vert.glsl",
                                   "assets/shaders/opengl/model.frag.glsl", "Model Program");

    glCreateSamplers(1, &m_modelSampler);
    glObjectLabel(GL_SAMPLER, m_modelSampler, -1, "Model Sampler");
    glSamplerParameteri(m_modelSampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glSamplerParameteri(m_modelSampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(m_modelSampler, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glSamplerParameteri(m_modelSampler, GL_TEXTURE_WRAP_T, GL_REPEAT);

    auto model = loadModel(assetPath("assets/models/DamagedHelmet.glb"));
    if (!model) {
        std::cerr << "OpenGLBackend: failed to load sample model, skipping model rendering\n";
        return true; // non-fatal: backend still works without a model
    }

    m_modelTextures.reserve(model->images.size());
    for (const ImageData &image : model->images) {
        GLuint texture = 0;
        glCreateTextures(GL_TEXTURE_2D, 1, &texture);
        char label[32];
        std::snprintf(label, sizeof(label), "Model Texture %zu", m_modelTextures.size());
        glObjectLabel(GL_TEXTURE, texture, -1, label);
        if (image.width > 0 && image.height > 0) {
            const auto levels =
                static_cast<GLsizei>(1 + std::floor(std::log2(std::max(image.width, image.height))));
            glTextureStorage2D(texture, levels, GL_RGBA8, image.width, image.height);
            glTextureSubImage2D(texture, 0, 0, 0, image.width, image.height, GL_RGBA, GL_UNSIGNED_BYTE,
                                image.pixels.data());
            glGenerateTextureMipmap(texture);
        }
        m_modelTextures.push_back(texture);
    }

    for (const MeshData &mesh : model->meshes) {
        for (const Primitive &primitive : mesh.primitives) {
            if (primitive.indices.empty())
                continue;

            GLPrimitive glPrimitive;
            glCreateVertexArrays(1, &glPrimitive.vao);
            glCreateBuffers(1, &glPrimitive.vbo);
            glCreateBuffers(1, &glPrimitive.ebo);

            char label[32];
            std::snprintf(label, sizeof(label), "Model Primitive %zu", m_modelPrimitives.size());
            glObjectLabel(GL_VERTEX_ARRAY, glPrimitive.vao, -1, label);
            std::snprintf(label, sizeof(label), "Model Primitive %zu VBO", m_modelPrimitives.size());
            glObjectLabel(GL_BUFFER, glPrimitive.vbo, -1, label);
            std::snprintf(label, sizeof(label), "Model Primitive %zu EBO", m_modelPrimitives.size());
            glObjectLabel(GL_BUFFER, glPrimitive.ebo, -1, label);

            glNamedBufferData(glPrimitive.vbo,
                              static_cast<GLsizeiptr>(primitive.vertices.size() * sizeof(Vertex)),
                              primitive.vertices.data(), GL_STATIC_DRAW);
            glNamedBufferData(glPrimitive.ebo,
                              static_cast<GLsizeiptr>(primitive.indices.size() * sizeof(std::uint32_t)),
                              primitive.indices.data(), GL_STATIC_DRAW);

            glVertexArrayVertexBuffer(glPrimitive.vao, 0, glPrimitive.vbo, 0, sizeof(Vertex));
            glVertexArrayElementBuffer(glPrimitive.vao, glPrimitive.ebo);

            glEnableVertexArrayAttrib(glPrimitive.vao, 0);
            glVertexArrayAttribFormat(glPrimitive.vao, 0, 3, GL_FLOAT, GL_FALSE, offsetof(Vertex, position));
            glVertexArrayAttribBinding(glPrimitive.vao, 0, 0);

            glEnableVertexArrayAttrib(glPrimitive.vao, 1);
            glVertexArrayAttribFormat(glPrimitive.vao, 1, 3, GL_FLOAT, GL_FALSE, offsetof(Vertex, normal));
            glVertexArrayAttribBinding(glPrimitive.vao, 1, 0);

            glEnableVertexArrayAttrib(glPrimitive.vao, 2);
            glVertexArrayAttribFormat(glPrimitive.vao, 2, 2, GL_FLOAT, GL_FALSE, offsetof(Vertex, texCoord));
            glVertexArrayAttribBinding(glPrimitive.vao, 2, 0);

            glPrimitive.indexCount = static_cast<GLsizei>(primitive.indices.size());

            if (primitive.materialIndex.has_value()) {
                const MaterialData &material = model->materials[*primitive.materialIndex];
                glPrimitive.baseColorFactor = material.baseColorFactor;
                if (material.baseColorTextureIndex.has_value())
                    glPrimitive.albedoTexture = m_modelTextures[*material.baseColorTextureIndex];
            }

            m_modelPrimitives.push_back(glPrimitive);
        }
    }

    return true;
}

void OpenGLBackend::shutdown() {
    glDeleteVertexArrays(1, &m_imguiVao);
    glDeleteBuffers(1, &m_imguiVbo);
    glDeleteBuffers(1, &m_imguiIbo);
    glDeleteProgram(m_imguiShader);
    glDeleteTextures(1, &m_fontTex);
    glDeleteSamplers(1, &m_fontSampler);

    for (const GLPrimitive &primitive : m_modelPrimitives) {
        glDeleteVertexArrays(1, &primitive.vao);
        glDeleteBuffers(1, &primitive.vbo);
        glDeleteBuffers(1, &primitive.ebo);
    }
    glDeleteTextures(static_cast<GLsizei>(m_modelTextures.size()), m_modelTextures.data());
    glDeleteProgram(m_modelShader);
    glDeleteSamplers(1, &m_modelSampler);
}

void OpenGLBackend::resize(int width, int height) {
    glViewport(0, 0, width, height);
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void OpenGLBackend::render(ImDrawData *drawData, const ImVec4 &clearColor) {
    int w, h;
    glfwGetFramebufferSize(m_window, &w, &h);
    glViewport(0, 0, w, h);

    const float cc[4] = {clearColor.x, clearColor.y, clearColor.z, clearColor.w};
    const float depth = 1.0f;
    glClearNamedFramebufferfv(0, GL_COLOR, 0, cc);
    glClearNamedFramebufferfv(0, GL_DEPTH, 0, &depth);

    renderModel();
    renderImGui(drawData);

    glfwSwapBuffers(m_window);
}

void OpenGLBackend::renderModel() {
    if (m_modelPrimitives.empty())
        return;

    int w, h;
    glfwGetFramebufferSize(m_window, &w, &h);
    if (w == 0 || h == 0)
        return;
    const float aspect = static_cast<float>(w) / static_cast<float>(h);

    // No keyboard/mouse plumbing runs from main.cpp into the Backend interface yet, so
    // this just orbits the camera around the model to prove the loader/camera/projection
    // pipeline end-to-end.
    const auto time = static_cast<float>(glfwGetTime());
    constexpr float orbitRadius = 4.0f;
    m_camera.Position = glm::vec3(std::sin(time * 0.5f) * orbitRadius, 1.5f, std::cos(time * 0.5f) * orbitRadius);
    m_camera.Front = glm::normalize(-m_camera.Position);

    const glm::mat4 view = m_camera.GetViewMatrix();
    const glm::mat4 projection = perspective(Backend::OpenGL, glm::radians(45.0f), aspect, 0.1f, 100.0f);

    glEnable(GL_DEPTH_TEST);
    glUseProgram(m_modelShader);
    glProgramUniformMatrix4fv(m_modelShader, 0, 1, GL_FALSE, glm::value_ptr(view));
    glProgramUniformMatrix4fv(m_modelShader, 1, 1, GL_FALSE, glm::value_ptr(projection));
    glBindSampler(0, m_modelSampler);

    for (const GLPrimitive &primitive : m_modelPrimitives) {
        glProgramUniform4fv(m_modelShader, 2, 1, glm::value_ptr(primitive.baseColorFactor));
        glProgramUniform1i(m_modelShader, 3, primitive.albedoTexture != 0 ? 1 : 0);
        glBindTextureUnit(0, primitive.albedoTexture);
        glBindVertexArray(primitive.vao);
        glDrawElements(GL_TRIANGLES, primitive.indexCount, GL_UNSIGNED_INT, nullptr);
    }

    glBindVertexArray(0);
    glDisable(GL_DEPTH_TEST);
}

void OpenGLBackend::renderImGui(ImDrawData *data) {
    int fbW = (int)(data->DisplaySize.x * data->FramebufferScale.x);
    int fbH = (int)(data->DisplaySize.y * data->FramebufferScale.y);
    if (fbW == 0 || fbH == 0)
        return;

    // Save state
    GLboolean blendWasOn = glIsEnabled(GL_BLEND);
    GLboolean depthWasOn = glIsEnabled(GL_DEPTH_TEST);
    GLboolean cullWasOn = glIsEnabled(GL_CULL_FACE);

    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_SCISSOR_TEST);

    float L = data->DisplayPos.x, R = data->DisplayPos.x + data->DisplaySize.x;
    float T = data->DisplayPos.y, B = data->DisplayPos.y + data->DisplaySize.y;
    const float proj[4][4] = {
        {2.0f / (R - L), 0.0f, 0.0f, 0.0f},
        {0.0f, 2.0f / (T - B), 0.0f, 0.0f},
        {0.0f, 0.0f, -1.0f, 0.0f},
        {(R + L) / (L - R), (T + B) / (B - T), 0.0f, 1.0f},
    };

    glUseProgram(m_imguiShader);
    glProgramUniformMatrix4fv(m_imguiShader, 0, 1, GL_FALSE, &proj[0][0]);
    glBindSampler(0, m_fontSampler);
    glBindVertexArray(m_imguiVao);

    ImVec2 clipOff = data->DisplayPos;
    ImVec2 clipScale = data->FramebufferScale;

    for (int n = 0; n < data->CmdListsCount; ++n) {
        const ImDrawList *cl = data->CmdLists[n];
        const auto vtxBytes = (GLsizeiptr)cl->VtxBuffer.Size * sizeof(ImDrawVert);
        const auto idxBytes = (GLsizeiptr)cl->IdxBuffer.Size * sizeof(ImDrawIdx);

        glNamedBufferData(m_imguiVbo, vtxBytes, cl->VtxBuffer.Data, GL_STREAM_DRAW);
        glNamedBufferData(m_imguiIbo, idxBytes, cl->IdxBuffer.Data, GL_STREAM_DRAW);

        for (int i = 0; i < cl->CmdBuffer.Size; ++i) {
            const ImDrawCmd &cmd = cl->CmdBuffer[i];
            ImVec2 clipMin{(cmd.ClipRect.x - clipOff.x) * clipScale.x,
                           (cmd.ClipRect.y - clipOff.y) * clipScale.y};
            ImVec2 clipMax{(cmd.ClipRect.z - clipOff.x) * clipScale.x,
                           (cmd.ClipRect.w - clipOff.y) * clipScale.y};
            if (clipMax.x <= clipMin.x || clipMax.y <= clipMin.y)
                continue;
            glScissor((GLint)clipMin.x, fbH - (GLint)clipMax.y,
                      (GLsizei)(clipMax.x - clipMin.x), (GLsizei)(clipMax.y - clipMin.y));
            glBindTextureUnit(0, (GLuint)(intptr_t)cmd.GetTexID());
            glDrawElementsBaseVertex(
                GL_TRIANGLES, (GLsizei)cmd.ElemCount,
                sizeof(ImDrawIdx) == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
                (void *)(intptr_t)(cmd.IdxOffset * sizeof(ImDrawIdx)),
                (GLint)cmd.VtxOffset);
        }
    }

    // Restore state
    glDisable(GL_SCISSOR_TEST);
    if (!blendWasOn)
        glDisable(GL_BLEND);
    if (depthWasOn)
        glEnable(GL_DEPTH_TEST);
    if (cullWasOn)
        glEnable(GL_CULL_FACE);
}
#endif
