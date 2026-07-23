#pragma once
#ifdef APP_BACKEND_OPENGL

#include "backend.h"

#include <glad/gl.h>
#include <glm/glm.hpp>
#include <vector>

class OpenGLBackend : public RendererBackend {
  public:
    bool init(GLFWwindow *window) override;
    void render(ImDrawData *drawData, const ImVec4 &clearColor) override;
    void shutdown() override;
    void resize(int width, int height) override;

  private:
    struct GLPrimitive {
        GLuint vao = 0;
        GLuint vbo = 0;
        GLuint ebo = 0;
        GLsizei indexCount = 0;
        glm::vec4 baseColorFactor{1.0f};
        GLuint albedoTexture = 0; // 0 == none
    };

    GLuint m_imguiVao = 0;
    GLuint m_imguiVbo = 0;
    GLuint m_imguiIbo = 0;
    GLuint m_imguiShader = 0;
    GLuint m_fontTex = 0;
    GLuint m_fontSampler = 0;

    GLuint m_modelShader = 0;
    GLuint m_modelSampler = 0;
    std::vector<GLPrimitive> m_modelPrimitives;
    std::vector<GLuint> m_modelTextures;

    bool initImGui();
    bool initModel();
    void renderImGui(ImDrawData *data);
    void renderModel();
};
#endif
