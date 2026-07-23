#pragma once
#include "camera.h"

#include "imgui.h"
#ifdef APP_BACKEND_VULKAN
#define GLFW_INCLUDE_VULKAN
#endif
#include <GLFW/glfw3.h>

// ---------------------------------------------------------------------------
// Backend registry
// ---------------------------------------------------------------------------
enum class Backend {
#ifdef APP_BACKEND_OPENGL
    OpenGL,
#endif
#ifdef APP_BACKEND_VULKAN
    Vulkan,
#endif
#ifdef APP_BACKEND_DX11
    DX11,
#endif
#ifdef APP_BACKEND_DX12
    DX12,
#endif
#ifdef APP_BACKEND_METAL
    Metal,
#endif
};

class RendererBackend {
  public:
    virtual ~RendererBackend() = default;
    virtual bool init(GLFWwindow *window) = 0;
    virtual void render(ImDrawData *drawData, const ImVec4 &clearColor) = 0;
    virtual void shutdown() = 0;
    virtual void resize(int width, int height) = 0;

  protected:
    GLFWwindow *m_window = nullptr;
    Camera m_camera{glm::vec3(0.0f, 0.0f, 4.0f)};
};
