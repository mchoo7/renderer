#pragma once
#ifdef APP_BACKEND_VULKAN

#include "backend.h"
#include "model.h"

#include <array>
#include <functional>
#include <vector>

#include <vma/vk_mem_alloc.h>

#define VULKAN_HPP_NO_STRUCT_CONSTRUCTORS
#include <vulkan/vulkan_raii.hpp>

class VulkanBackend : public RendererBackend {
  public:
    bool init(GLFWwindow *window) override;
    void render(ImDrawData *drawData, const ImVec4 &clearColor) override;
    void shutdown() override;
    void resize(int width, int height) override;

  private:
    // Double-buffered CPU/GPU overlap: while the GPU consumes frame N the CPU records N+1.
    static constexpr int kFramesInFlight = 2;

    // VMA owns the memory and the image/buffer handle lifetime (vmaDestroy*), so these stay
    // plain non-owning vk:: handles rather than vk::raii wrappers.
    struct Buffer {
        vk::Buffer handle;
        VmaAllocation allocation = nullptr;
        void *mapped = nullptr;              // persistently mapped (host-visible)
        vk::DeviceSize capacity = 0;         // bytes
        vk::DeviceAddress deviceAddress = 0; // set only when created with eShaderDeviceAddress usage
    };

    // Everything recorded/consumed per in-flight frame. ImGui vertex/index data is streamed
    // into host-visible buffers owned here so frame N's upload never races frame N-1's draw.
    struct FrameData {
        vk::raii::CommandBuffer commandBuffer = nullptr;
        vk::raii::Semaphore imageAvailable = nullptr; // signalled by vkAcquireNextImageKHR
        vk::raii::Fence inFlight = nullptr;           // CPU waits on this before reusing the frame
        Buffer vertexBuffer;
        Buffer indexBuffer;
        // View/projection matrices for the model pass, read by the shader through a buffer
        // device address passed in the vertex push constant (see recordModel).
        Buffer shaderDataBuffer;
    };

    // Core objects (created via vk-bootstrap, then adopted into vk::raii wrappers for
    // automatic, exception-safe teardown).
    vk::raii::Context m_context;
    vk::raii::Instance m_instance = nullptr;
    vk::raii::DebugUtilsMessengerEXT m_debugMessenger = nullptr;
    vk::raii::SurfaceKHR m_surface = nullptr;
    vk::raii::PhysicalDevice m_physicalDevice = nullptr;
    vk::raii::Device m_device = nullptr;
    vk::raii::Queue m_graphicsQueue = nullptr;
    vk::raii::Queue m_presentQueue = nullptr;
    uint32_t m_graphicsQueueFamily = 0;
    VmaAllocator m_allocator = nullptr;
    vk::raii::CommandPool commandPool = nullptr;

    // Swapchain + its per-image resources.
    vk::raii::SwapchainKHR m_swapchain = nullptr;
    vk::Format m_swapchainFormat = vk::Format::eUndefined;
    vk::Extent2D m_swapchainExtent;
    std::vector<vk::Image> m_swapchainImages; // non-owning: lifetime tied to m_swapchain
    std::vector<vk::raii::ImageView> m_swapchainImageViews;
    // Present-wait semaphores are per swapchain image, not per frame-in-flight: acquire can
    // hand back any image, so the semaphore a present waits on must track the image.
    std::vector<vk::raii::Semaphore> m_renderFinished;

    // Depth buffer (dynamic rendering). The model pass depth-tests against it; the ImGui
    // overlay ignores it but render() still clears it each frame.
    vk::Image m_depthImage; // VMA-owned
    VmaAllocation m_depthAllocation = nullptr;
    vk::raii::ImageView m_depthView = nullptr;
    vk::Format m_depthFormat = vk::Format::eD32Sfloat;

    // ImGui pipeline + font resources.
    vk::raii::DescriptorSetLayout m_imguiDescriptorSetLayout = nullptr;
    vk::raii::PipelineLayout m_imguiPipelineLayout = nullptr;
    vk::raii::Pipeline m_imguiPipeline = nullptr;
    vk::raii::DescriptorPool m_imguiDescriptorPool = nullptr;
    vk::raii::Sampler m_fontSampler = nullptr;
    vk::Image m_fontImage; // VMA-owned
    VmaAllocation m_fontAllocation = nullptr;
    vk::raii::ImageView m_fontView = nullptr;
    vk::raii::DescriptorSet m_fontDescriptor = nullptr; // also used as the ImGui texture id

    // Model pipeline. Textures live in a single runtime-sized descriptor array (bindless);
    // each primitive picks one by index through a push constant. Geometry is a host-visible
    // vertex/index buffer pair per primitive, uploaded once (loadModel bakes node transforms
    // into the vertices, so the shader only needs the camera view-projection).
    struct ModelTexture {
        vk::Image image; // VMA-owned
        VmaAllocation allocation = nullptr;
        vk::raii::ImageView view = nullptr;
    };

    struct ModelPrimitive {
        Buffer vertexBuffer;
        Buffer indexBuffer;
        uint32_t indexCount = 0;
        glm::vec4 baseColorFactor{1.0f};
        int32_t textureIndex = -1; // index into m_modelTextures, -1 == untextured
    };

    vk::raii::DescriptorSetLayout m_modelDescriptorSetLayout = nullptr;
    vk::raii::PipelineLayout m_modelPipelineLayout = nullptr;
    vk::raii::Pipeline m_modelPipeline = nullptr;
    vk::raii::DescriptorPool m_modelDescriptorPool = nullptr;
    vk::raii::DescriptorSet m_modelTextureSet = nullptr; // the bindless texture array
    vk::raii::Sampler m_modelSampler = nullptr;
    std::vector<ModelTexture> m_modelTextures;
    std::vector<ModelPrimitive> m_modelPrimitives;

    std::array<FrameData, kFramesInFlight> m_frames;
    uint32_t m_frameIndex = 0;

    bool initVulkan();
    bool createSwapchain();
    bool createDepthResources();
    void destroySwapchain();
    bool recreateSwapchain();

    bool initFrames();
    bool initImGui();
    bool initModel();
    bool createImGuiPipeline();
    bool createModelPipeline();
    bool createFontTexture();

    // Grows `buffer` to at least `size` bytes if needed, (re)mapping it. Waits idle before
    // freeing the old allocation since it may still be in flight.
    void ensureBufferCapacity(Buffer &buffer, vk::DeviceSize size, vk::BufferUsageFlags usage) const;
    // Creates a host-visible, persistently-mapped buffer preloaded with `size` bytes from
    // `data` — used for static model geometry that is uploaded exactly once.
    Buffer createStaticBuffer(const void *data, vk::DeviceSize size, vk::BufferUsageFlags usage) const;
    void destroyBuffer(Buffer &buffer) const;

    // Records `record` into a throwaway command buffer, submits it, and blocks until the GPU
    // is done. Used for one-off transfers (texture uploads) outside the frame loop.
    void immediateSubmit(const std::function<void(vk::CommandBuffer)> &record) const;
    // Uploads an RGBA8 image to a device-local sampled image, generating a full mip chain.
    ModelTexture createModelTexture(const ImageData &image) const;

    void recordImGui(vk::CommandBuffer cmd, ImDrawData *drawData, FrameData &frame) const;
    void recordModel(vk::CommandBuffer cmd, FrameData &frame);
};
#endif
