#include <iostream>
#include <memory>

using namespace std::literals;

#include <GLFW/glfw3.h>

#if defined(APP_BACKEND_DX11) || defined(APP_BACKEND_DX12)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif
#ifdef APP_BACKEND_METAL
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>
#endif

#include "imgui.h"
#include "imgui_impl_glfw.h"

#include "backend.h"
#ifdef APP_BACKEND_OPENGL
#include "backends/opengl_backend.h"
#endif

static constexpr std::string_view backendName(Backend b) {
    switch (b) {
#ifdef APP_BACKEND_OPENGL
    case Backend::OpenGL:
        return "OpenGL 4.6";
#endif
#ifdef APP_BACKEND_VULKAN
    case Backend::Vulkan:
        return "Vulkan 1.4";
#endif
#ifdef APP_BACKEND_DX11
    case Backend::DX11:
        return "DirectX 11";
#endif
#ifdef APP_BACKEND_DX12
    case Backend::DX12:
        return "DirectX 12";
#endif
#ifdef APP_BACKEND_METAL
    case Backend::Metal:
        return "Metal 4";
#endif
    }
    return "Unknown";
}

static Backend g_Backend =
#if defined(APP_BACKEND_OPENGL)
    Backend::OpenGL;
#elif defined(APP_BACKEND_VULKAN)
    Backend::Vulkan;
#elif defined(APP_BACKEND_DX11)
    Backend::DX11;
#elif defined(APP_BACKEND_DX12)
    Backend::DX12;
#elif defined(APP_BACKEND_METAL)
    Backend::Metal;
#endif

static std::unique_ptr<RendererBackend> createBackend(Backend b) {
    switch (b) {
#ifdef APP_BACKEND_OPENGL
    case Backend::OpenGL:
        return std::make_unique<OpenGLBackend>();
#endif
    default:
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
// Window creation
// ---------------------------------------------------------------------------
static GLFWwindow *createWindow(Backend backend, int w, int h) {
    glfwDefaultWindowHints();
#ifdef APP_BACKEND_OPENGL
    if (backend == Backend::OpenGL) {
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    } else
#endif
    {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    }
    std::string title = std::string("Renderer — ") + backendName(backend).data();
    return glfwCreateWindow(w, h, title.c_str(), nullptr, nullptr);
}

// Platform-level setup before renderer backend init (context, swap interval, etc.)
static bool platformInit(Backend backend, GLFWwindow *window) {
    switch (backend) {
#ifdef APP_BACKEND_OPENGL
    case Backend::OpenGL:
        glfwMakeContextCurrent(window);
        glfwSwapInterval(0);
        return true;
#endif
#ifdef APP_BACKEND_VULKAN
    case Backend::Vulkan:
        return true; // TODO
#endif
#ifdef APP_BACKEND_DX11
    case Backend::DX11:
        return true; // TODO
#endif
#ifdef APP_BACKEND_DX12
    case Backend::DX12:
        return true; // TODO
#endif
#ifdef APP_BACKEND_METAL
    case Backend::Metal:
        return true; // TODO
#endif
    }
    return false;
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------
static Backend drawUI(Backend current, ImVec4 &clearColor, bool &switchRequested) {
    Backend pending = current;
    ImGui::Begin("Renderer");

    const ImGuiIO &io = ImGui::GetIO();
    ImGui::Text("%.1f FPS  (%.2f ms)", io.Framerate, 1000.0f / io.Framerate);
    ImGui::Separator();
    ImGui::ColorEdit3("Background", reinterpret_cast<float *>(&clearColor));
    ImGui::Separator();

    ImGui::Text("Backend:");
#ifdef APP_BACKEND_OPENGL
    if (ImGui::RadioButton("OpenGL 4.6", current == Backend::OpenGL)) {
        pending = Backend::OpenGL;
        switchRequested = true;
    }
#endif
#ifdef APP_BACKEND_VULKAN
    if (ImGui::RadioButton("Vulkan 1.4", current == Backend::Vulkan)) {
        pending = Backend::Vulkan;
        switchRequested = true;
    }
#endif
#ifdef APP_BACKEND_DX11
    if (ImGui::RadioButton("DirectX 11", current == Backend::DX11)) {
        pending = Backend::DX11;
        switchRequested = true;
    }
#endif
#ifdef APP_BACKEND_DX12
    if (ImGui::RadioButton("DirectX 12", current == Backend::DX12)) {
        pending = Backend::DX12;
        switchRequested = true;
    }
#endif
#ifdef APP_BACKEND_METAL
    if (ImGui::RadioButton("Metal 4", current == Backend::Metal)) {
        pending = Backend::Metal;
        switchRequested = true;
    }
#endif

    ImGui::End();
    return pending;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
static void glfwErrorCallback(int error, const char *description) {
    std::cerr << "GLFW error " << error << ": " << description << "\n";
}

static void glfwFramebufferSizeCallback(GLFWwindow *window, int width, int height) {
    if (auto *renderer = static_cast<RendererBackend *>(glfwGetWindowUserPointer(window)))
        renderer->resize(width, height);
}

int main(int argc, char *argv[]) {
    // Set initial backend from arg
    if (argc > 1) {
#if defined(APP_BACKEND_OPENGL)
        if (argv[1] == "opengl"sv)
            g_Backend = Backend::OpenGL;
#endif
#if defined(APP_BACKEND_VULKAN)
        if (argv[1] == "vulkan"sv)
            g_Backend = Backend::Vulkan;
#endif
#if defined(APP_BACKEND_DX11)
        if (argv[1] == "dx11"sv)
            g_Backend = Backend::DX11;
#endif
#if defined(APP_BACKEND_DX12)
        if (argv[1] == "dx12"sv)
            g_Backend = Backend::DX12;
#endif
#if defined(APP_BACKEND_METAL)
        if (argv[1] == "metal"sv)
            g_Backend = Backend::Metal;
#endif
    }

    glfwSetErrorCallback(glfwErrorCallback);
#ifdef __linux__
    glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
#endif
    if (!glfwInit())
        return -1;

    ImVec4 clearColor = {0.15f, 0.15f, 0.18f, 1.0f};

    while (true) {
        GLFWwindow *window = createWindow(g_Backend, 1280, 720);
        if (!window) {
            glfwTerminate();
            return -1;
        }

        if (!platformInit(g_Backend, window)) {
            glfwDestroyWindow(window);
            glfwTerminate();
            return -1;
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::StyleColorsDark();

        // HiDPI: ImGui draws glyphs in logical points, but on a Retina/scaled display each
        // logical point spans `contentScale` physical pixels. Bake the atlas at that higher
        // texel density and shrink the rendered glyph size back down by the same factor, so
        // text keeps its on-screen size but is sampled 1:1 with physical pixels (sharp).
        float xScale = 1.0f, yScale = 1.0f;
        glfwGetWindowContentScale(window, &xScale, &yScale);
        const float dpiScale = xScale > 0.0f ? xScale : 1.0f;
        ImFontConfig fontConfig;
        fontConfig.SizePixels = 13.0f * dpiScale;
        io.Fonts->AddFontDefault(&fontConfig);
        io.FontGlobalScale = 1.0f / dpiScale;

        ImGui_ImplGlfw_InitForOther(window, true);

        auto renderer = createBackend(g_Backend);
        if (!renderer || !renderer->init(window)) {
            ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext();
            glfwDestroyWindow(window);
            glfwTerminate();
            return -1;
        }

        glfwSetWindowUserPointer(window, renderer.get());
        glfwSetFramebufferSizeCallback(window, glfwFramebufferSizeCallback);

        bool switchRequested = false;
        Backend pendingBackend = g_Backend;

        while (!glfwWindowShouldClose(window) && !switchRequested) {
            glfwPollEvents();

            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            pendingBackend = drawUI(g_Backend, clearColor, switchRequested);

            ImGui::Render();
            renderer->render(ImGui::GetDrawData(), clearColor);
        }

        renderer->shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);

        if (!switchRequested)
            break;
        g_Backend = pendingBackend;
    }

    glfwTerminate();
    return 0;
}
