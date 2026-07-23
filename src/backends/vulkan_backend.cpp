#ifdef APP_BACKEND_VULKAN

// The Vulkan Memory Allocator C implementation is compiled into this translation unit.
// Must be defined before vk_mem_alloc.h (pulled in via the header below) is included.
#define VMA_IMPLEMENTATION

#include "vulkan_backend.h"

#include "paths.h"
#include "projection.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <imgui.h>

#include <VkBootstrap.h>

#include <slang/slang-com-helper.h>
#include <slang/slang-com-ptr.h>
#include <slang/slang.h>

namespace {

struct ImGuiPushConstants {
    float scale[2];
    float translate[2];
};

struct alignas(16) ShaderData {
    glm::mat4 view;
    glm::mat4 projection;
    glm::vec4 baseColorFactor;
    int32_t textureIndex;
};

void logDiagnostics(slang::IBlob *diagnostics) {
    if (diagnostics && diagnostics->getBufferSize() > 0)
        std::cerr << "Slang: " << static_cast<const char *>(diagnostics->getBufferPointer()) << "\n";
}

// Compiles a Slang module (both entry points) to SPIR-V and wraps it in a shader module.
bool compileSlangModule(const vk::raii::Device &device, const char *moduleName,
                        const char *sourcePath, vk::raii::ShaderModule &out) {
    using Slang::ComPtr;

    ComPtr<slang::IGlobalSession> globalSession;
    if (SLANG_FAILED(slang::createGlobalSession(globalSession.writeRef()))) {
        std::cerr << "Slang: failed to create global session\n";
        return false;
    }

    slang::TargetDesc targetDesc{
        .format = SLANG_SPIRV,
        .profile = globalSession->findProfile("spirv_1_5"),
    };

    // Emit SPIR-V directly, and keep the original entry-point names so the pipeline can
    // reference "vertexMain"/"fragmentMain" (Slang otherwise renames the entry point to "main").
    std::array<slang::CompilerOptionEntry, 2> options = {
        {
            {
                .name = slang::CompilerOptionName::EmitSpirvDirectly,
                .value{
                    .kind = slang::CompilerOptionValueKind::Int,
                    .intValue0 = 1,
                },
            },
            {
                .name = slang::CompilerOptionName::VulkanUseEntryPointName,
                .value{
                    .kind = slang::CompilerOptionValueKind::Int,
                    .intValue0 = 1,
                },
            },
        }};

    // Column-major so glm matrices (also column-major) can be uploaded to push constants as-is;
    // harmless for shaders (like ImGui's) that use no matrices.
    slang::SessionDesc sessionDesc{
        .targets = &targetDesc,
        .targetCount = 1,
        .defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR,
        .compilerOptionEntries = options.data(),
        .compilerOptionEntryCount = 2,
    };

    ComPtr<slang::ISession> session;
    if (SLANG_FAILED(globalSession->createSession(sessionDesc, session.writeRef()))) {
        std::cerr << "Slang: failed to create session\n";
        return false;
    }

    ComPtr<slang::IBlob> diagnostics;
    slang::IModule *module =
        session->loadModuleFromSource(moduleName, sourcePath, nullptr, diagnostics.writeRef());
    logDiagnostics(diagnostics);
    if (!module)
        return false;

    ComPtr<slang::IBlob> spirv;
    if (SLANG_FAILED(module->getTargetCode(0, spirv.writeRef(), diagnostics.writeRef()))) {
        logDiagnostics(diagnostics);
        return false;
    }

    vk::ShaderModuleCreateInfo moduleInfo{
        .codeSize = spirv->getBufferSize(),
        .pCode = static_cast<const uint32_t *>(spirv->getBufferPointer()),
    };
    out = device.createShaderModule(moduleInfo);

    return true;
}

// Records a sync2 image layout transition into an already-begun command buffer.
void transitionImage(vk::CommandBuffer cmd, vk::Image image, vk::ImageLayout oldLayout,
                     vk::ImageLayout newLayout, vk::PipelineStageFlags2 srcStage,
                     vk::AccessFlags2 srcAccess, vk::PipelineStageFlags2 dstStage,
                     vk::AccessFlags2 dstAccess, vk::ImageAspectFlags aspect) {
    vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = srcStage,
        .srcAccessMask = srcAccess,
        .dstStageMask = dstStage,
        .dstAccessMask = dstAccess,
        .oldLayout = oldLayout,
        .newLayout = newLayout,
        .image = image,
        .subresourceRange = vk::ImageSubresourceRange{aspect, 0, 1, 0, 1},
    };

    vk::DependencyInfo dependency{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier,
    };
    cmd.pipelineBarrier2(dependency);
}

} // namespace

// ---------------------------------------------------------------------------
// Init / shutdown
// ---------------------------------------------------------------------------

bool VulkanBackend::init(GLFWwindow *window) {
    m_window = window;
    try {
        return initVulkan() && createSwapchain() && initFrames() && initImGui() && initModel();
    } catch (const vk::SystemError &error) {
        std::cerr << "VulkanBackend: init failed: " << error.what() << "\n";
        return false;
    }
}

bool VulkanBackend::initVulkan() {
    vkb::InstanceBuilder builder;
    builder.set_app_name("Renderer")
        .require_api_version(1, 3)
#if !NDEBUG
        .request_validation_layers(true)
        .use_default_debug_messenger()
#endif
        ;

    // Instance extensions GLFW needs to present to this platform's window system.
    uint32_t glfwExtensionCount = 0;
    const char **glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
    for (uint32_t i = 0; i < glfwExtensionCount; ++i)
        builder.enable_extension(glfwExtensions[i]);

    auto instanceResult = builder.build();
    if (!instanceResult) {
        std::cerr << "VulkanBackend: instance creation failed: " << instanceResult.error().message() << "\n";
        return false;
    }
    vkb::Instance vkbInstance = instanceResult.value();
    // vk-bootstrap hands back plain C handles; adopt them into vk::raii wrappers so the rest
    // of the backend gets automatic, exception-safe teardown.
    m_instance = vk::raii::Instance(m_context, vkbInstance.instance);
#if !NDEBUG
    m_debugMessenger = vk::raii::DebugUtilsMessengerEXT(m_instance, vkbInstance.debug_messenger);
#endif

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (glfwCreateWindowSurface(*m_instance, m_window, nullptr, &surface) != VK_SUCCESS) {
        std::cerr << "VulkanBackend: failed to create window surface\n";
        return false;
    }
    m_surface = vk::raii::SurfaceKHR(m_instance, surface);

    VkPhysicalDeviceVulkan12Features features12{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .descriptorIndexing = VK_TRUE,
        .shaderSampledImageArrayNonUniformIndexing = VK_TRUE,
        .descriptorBindingVariableDescriptorCount = VK_TRUE,
        .runtimeDescriptorArray = VK_TRUE,
        .bufferDeviceAddress = VK_TRUE,
    };
    VkPhysicalDeviceVulkan13Features features13{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .synchronization2 = VK_TRUE,
        .dynamicRendering = VK_TRUE,
    };
    VkPhysicalDeviceFeatures features10{
        .samplerAnisotropy = VK_TRUE,
    };

    vkb::PhysicalDeviceSelector selector{vkbInstance};
    auto physicalResult = selector.set_surface(*m_surface)
                              .set_minimum_version(1, 3)
                              .set_required_features(features10)
                              .set_required_features_12(features12)
                              .set_required_features_13(features13)
                              .select();
    if (!physicalResult) {
        std::cerr << "VulkanBackend: no suitable GPU: " << physicalResult.error().message() << "\n";
        return false;
    }
    const vkb::PhysicalDevice &vkbPhysical = physicalResult.value();

    vkb::DeviceBuilder deviceBuilder{vkbPhysical};
    auto deviceResult = deviceBuilder.build();
    if (!deviceResult) {
        std::cerr << "VulkanBackend: device creation failed: " << deviceResult.error().message() << "\n";
        return false;
    }
    const vkb::Device &vkbDevice = deviceResult.value();

    m_physicalDevice = vk::raii::PhysicalDevice(m_instance, vkbPhysical.physical_device);
    m_device = vk::raii::Device(m_physicalDevice, vkbDevice.device);

    auto graphicsQueue = vkbDevice.get_queue(vkb::QueueType::graphics);
    auto presentQueue = vkbDevice.get_queue(vkb::QueueType::present);
    auto graphicsFamily = vkbDevice.get_queue_index(vkb::QueueType::graphics);
    if (!graphicsQueue || !presentQueue || !graphicsFamily) {
        std::cerr << "VulkanBackend: failed to obtain queues\n";
        return false;
    }
    m_graphicsQueue = vk::raii::Queue(m_device, graphicsQueue.value());
    m_presentQueue = vk::raii::Queue(m_device, presentQueue.value());
    m_graphicsQueueFamily = graphicsFamily.value();

    VmaVulkanFunctions vulkanFunctions{
        .vkGetInstanceProcAddr = vkGetInstanceProcAddr,
        .vkGetDeviceProcAddr = vkGetDeviceProcAddr,
        .vkCreateImage = vkCreateImage,
    };

    VmaAllocatorCreateInfo allocatorInfo{
        .flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
        .physicalDevice = *m_physicalDevice,
        .device = *m_device,
        .pVulkanFunctions = &vulkanFunctions,
        .instance = *m_instance,
        .vulkanApiVersion = VK_API_VERSION_1_3,
    };
    if (vmaCreateAllocator(&allocatorInfo, &m_allocator) != VK_SUCCESS) {
        std::cerr << "VulkanBackend: failed to create VMA allocator\n";
        return false;
    }

    std::cout << "Vulkan device: " << vkbPhysical.name << "\n";
    return true;
}

bool VulkanBackend::createSwapchain() {
    int width = 0, height = 0;
    glfwGetFramebufferSize(m_window, &width, &height);

    vkb::SwapchainBuilder swapchainBuilder{*m_physicalDevice, *m_device, *m_surface};
    auto swapchainResult =
        swapchainBuilder
            .set_desired_format({VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
            .set_desired_present_mode(VK_PRESENT_MODE_IMMEDIATE_KHR) // vsync off; falls back to FIFO if absent
            .set_desired_extent(static_cast<uint32_t>(width), static_cast<uint32_t>(height))
            .add_image_usage_flags(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
            .build();
    if (!swapchainResult) {
        std::cerr << "VulkanBackend: swapchain creation failed: " << swapchainResult.error().message()
                  << "\n";
        return false;
    }
    vkb::Swapchain vkbSwapchain = swapchainResult.value();

    m_swapchain = vk::raii::SwapchainKHR(m_device, vkbSwapchain.swapchain);
    m_swapchainFormat = static_cast<vk::Format>(vkbSwapchain.image_format);
    m_swapchainExtent = vk::Extent2D{vkbSwapchain.extent.width, vkbSwapchain.extent.height};

    for (VkImage image : vkbSwapchain.get_images().value())
        m_swapchainImages.emplace_back(image);
    for (VkImageView view : vkbSwapchain.get_image_views().value())
        m_swapchainImageViews.emplace_back(m_device, view);

    // One present-signalling semaphore per swapchain image (see header comment).
    for (size_t i = 0; i < m_swapchainImages.size(); ++i)
        m_renderFinished.emplace_back(m_device.createSemaphore({}));

    return createDepthResources();
}

bool VulkanBackend::createDepthResources() {
    vk::ImageCreateInfo imageInfo{
        .imageType = vk::ImageType::e2D,
        .format = m_depthFormat,
        .extent = vk::Extent3D{m_swapchainExtent.width, m_swapchainExtent.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eDepthStencilAttachment,
    };

    VmaAllocationCreateInfo allocInfo{
        .flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO,
    };

    VkImage image = VK_NULL_HANDLE;
    if (vmaCreateImage(m_allocator, reinterpret_cast<const VkImageCreateInfo *>(&imageInfo),
                       &allocInfo, &image, &m_depthAllocation, nullptr) != VK_SUCCESS) {
        std::cerr << "VulkanBackend: failed to create depth image\n";
        return false;
    }
    m_depthImage = image;

    vk::ImageViewCreateInfo viewInfo{
        .image = m_depthImage,
        .viewType = vk::ImageViewType::e2D,
        .format = m_depthFormat,
        .subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1},
    };
    m_depthView = m_device.createImageView(viewInfo);
    return true;
}

void VulkanBackend::destroySwapchain() {
    if (!*m_device)
        return;
    m_depthView = nullptr;
    if (m_depthImage) {
        vmaDestroyImage(m_allocator, m_depthImage, m_depthAllocation);
        m_depthImage = nullptr;
        m_depthAllocation = nullptr;
    }
    m_renderFinished.clear();
    m_swapchainImageViews.clear();
    m_swapchainImages.clear();
    m_swapchain = nullptr;
}

bool VulkanBackend::recreateSwapchain() {
    int width = 0, height = 0;
    glfwGetFramebufferSize(m_window, &width, &height);
    if (width == 0 || height == 0)
        return true; // minimized: skip until the window has area again

    m_device.waitIdle();
    destroySwapchain();
    return createSwapchain();
}

bool VulkanBackend::initFrames() {
    vk::CommandPoolCreateInfo poolInfo{
        .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
        .queueFamilyIndex = m_graphicsQueueFamily,
    };
    commandPool = m_device.createCommandPool(poolInfo);

    for (FrameData &frame : m_frames) {
        vk::CommandBufferAllocateInfo allocInfo{
            .commandPool = *commandPool,
            .level = vk::CommandBufferLevel::ePrimary,
            .commandBufferCount = 1,
        };
        frame.commandBuffer = std::move(m_device.allocateCommandBuffers(allocInfo).front());

        frame.imageAvailable = m_device.createSemaphore({});
        frame.inFlight = m_device.createFence({.flags = vk::FenceCreateFlagBits::eSignaled});
    }
    return true;
}

bool VulkanBackend::initImGui() {
    return createImGuiPipeline() && createFontTexture();
}

bool VulkanBackend::createImGuiPipeline() {
    vk::raii::ShaderModule module = nullptr;
    const std::string source = assetPath("assets/shaders/vulkan/imgui.slang").string();
    if (!compileSlangModule(m_device, "imgui", source.c_str(), module))
        return false;

    // Combined image sampler at set 0, binding 0 (the ImGui-supplied texture).
    vk::DescriptorSetLayoutBinding samplerBinding{
        .binding = 0,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
    };

    vk::DescriptorSetLayoutCreateInfo layoutInfo{
        .bindingCount = 1,
        .pBindings = &samplerBinding,
    };
    m_imguiDescriptorSetLayout = m_device.createDescriptorSetLayout(layoutInfo);

    vk::PushConstantRange pushConstant{
        .stageFlags = vk::ShaderStageFlagBits::eVertex,
        .offset = 0,
        .size = sizeof(ImGuiPushConstants),
    };

    vk::PipelineLayoutCreateInfo pipelineLayoutInfo{
        .setLayoutCount = 1,
        .pSetLayouts = &*m_imguiDescriptorSetLayout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pushConstant,
    };
    m_imguiPipelineLayout = m_device.createPipelineLayout(pipelineLayoutInfo);

    vk::PipelineShaderStageCreateInfo stages[2] = {
        {.stage = vk::ShaderStageFlagBits::eVertex, .module = *module, .pName = "main"},
        {.stage = vk::ShaderStageFlagBits::eFragment, .module = *module, .pName = "main"},
    };

    vk::VertexInputBindingDescription binding{
        .binding = 0,
        .stride = sizeof(ImDrawVert),
        .inputRate = vk::VertexInputRate::eVertex,
    };

    vk::VertexInputAttributeDescription attributes[3] = {
        {.location = 0, .binding = 0, .format = vk::Format::eR32G32Sfloat, .offset = offsetof(ImDrawVert, pos)},
        {.location = 1, .binding = 0, .format = vk::Format::eR32G32Sfloat, .offset = offsetof(ImDrawVert, uv)},
        {.location = 2, .binding = 0, .format = vk::Format::eR8G8B8A8Unorm, .offset = offsetof(ImDrawVert, col)},
    };

    vk::PipelineVertexInputStateCreateInfo vertexInput{
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = 3,
        .pVertexAttributeDescriptions = attributes,
    };

    vk::PipelineInputAssemblyStateCreateInfo inputAssembly{.topology = vk::PrimitiveTopology::eTriangleList};

    vk::PipelineViewportStateCreateInfo viewportState{
        .viewportCount = 1,
        .scissorCount = 1,
    };

    vk::PipelineRasterizationStateCreateInfo rasterization{
        .polygonMode = vk::PolygonMode::eFill,
        .cullMode = vk::CullModeFlagBits::eNone,
        .frontFace = vk::FrontFace::eCounterClockwise,
        .lineWidth = 1.0f,
    };

    vk::PipelineMultisampleStateCreateInfo multisample{.rasterizationSamples = vk::SampleCountFlagBits::e1};

    // ImGui draws as a transparent overlay: alpha-blend, no depth test/write.
    vk::PipelineDepthStencilStateCreateInfo depthStencil{
        .depthTestEnable = VK_FALSE,
        .depthWriteEnable = VK_FALSE,
    };

    vk::PipelineColorBlendAttachmentState blendAttachment{
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = vk::BlendFactor::eSrcAlpha,
        .dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
        .colorBlendOp = vk::BlendOp::eAdd,
        .srcAlphaBlendFactor = vk::BlendFactor::eOne,
        .dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
        .alphaBlendOp = vk::BlendOp::eAdd,
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
    };

    vk::PipelineColorBlendStateCreateInfo colorBlend{
        .attachmentCount = 1,
        .pAttachments = &blendAttachment,
    };

    vk::DynamicState dynamicStates[2] = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    vk::PipelineDynamicStateCreateInfo dynamicState{
        .dynamicStateCount = 2,
        .pDynamicStates = dynamicStates,
    };

    // Dynamic rendering: describe the attachment formats in lieu of a render pass.
    vk::PipelineRenderingCreateInfo renderingInfo{
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &m_swapchainFormat,
        .depthAttachmentFormat = m_depthFormat,
    };

    vk::GraphicsPipelineCreateInfo pipelineInfo{
        .pNext = &renderingInfo,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vertexInput,
        .pInputAssemblyState = &inputAssembly,
        .pViewportState = &viewportState,
        .pRasterizationState = &rasterization,
        .pMultisampleState = &multisample,
        .pDepthStencilState = &depthStencil,
        .pColorBlendState = &colorBlend,
        .pDynamicState = &dynamicState,
        .layout = *m_imguiPipelineLayout,
    };

    m_imguiPipeline = m_device.createGraphicsPipeline(nullptr, pipelineInfo);

    // shader module frees itself (vkDestroyShaderModule) at the end of this scope;
    // that's safe once pipeline creation above has completed.

    // A single combined-image-sampler descriptor for the font atlas.
    vk::DescriptorPoolSize poolSize{.type = vk::DescriptorType::eCombinedImageSampler, .descriptorCount = 1};
    // Individual free is needed so ~DescriptorSet (m_fontDescriptor) can outlive/precede pool
    // teardown independently rather than relying solely on whole-pool destruction.
    vk::DescriptorPoolCreateInfo poolInfo{
        .flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &poolSize,
    };
    m_imguiDescriptorPool = m_device.createDescriptorPool(poolInfo);

    return true;
}

bool VulkanBackend::createFontTexture() {
    unsigned char *pixels = nullptr;
    int width = 0, height = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    const vk::DeviceSize uploadSize =
        static_cast<vk::DeviceSize>(width) * static_cast<vk::DeviceSize>(height) * 4;

    // Host-visible staging buffer, mapped so we can memcpy the atlas straight in.
    vk::BufferCreateInfo stagingInfo{
        .size = uploadSize,
        .usage = vk::BufferUsageFlagBits::eTransferSrc,
    };

    VmaAllocationCreateInfo stagingAlloc{
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO,
    };

    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VmaAllocation stagingAllocation = nullptr;
    VmaAllocationInfo stagingAllocInfo{};
    vmaCreateBuffer(m_allocator, reinterpret_cast<const VkBufferCreateInfo *>(&stagingInfo),
                    &stagingAlloc, &stagingBuffer, &stagingAllocation, &stagingAllocInfo);
    std::memcpy(stagingAllocInfo.pMappedData, pixels, static_cast<size_t>(uploadSize));

    // Device-local sampled image.
    vk::ImageCreateInfo imageInfo{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR8G8B8A8Unorm,
        .extent = vk::Extent3D{static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
    };

    VmaAllocationCreateInfo imageAlloc{.usage = VMA_MEMORY_USAGE_AUTO};
    VkImage fontImage = VK_NULL_HANDLE;
    vmaCreateImage(m_allocator, reinterpret_cast<const VkImageCreateInfo *>(&imageInfo), &imageAlloc,
                   &fontImage, &m_fontAllocation, nullptr);
    m_fontImage = fontImage;

    // One-time upload: transition, copy, transition to shader-read.
    vk::CommandBufferAllocateInfo cmdAllocInfo{
        .commandPool = *commandPool,
        .level = vk::CommandBufferLevel::ePrimary,
        .commandBufferCount = 1,
    };
    vk::raii::CommandBuffer cmd = std::move(m_device.allocateCommandBuffers(cmdAllocInfo).front());

    cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

    transitionImage(*cmd, m_fontImage, vk::ImageLayout::eUndefined,
                    vk::ImageLayout::eTransferDstOptimal, vk::PipelineStageFlagBits2::eTopOfPipe, {},
                    vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
                    vk::ImageAspectFlagBits::eColor);

    vk::BufferImageCopy copy{
        .imageSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .imageExtent = imageInfo.extent,
    };
    cmd.copyBufferToImage(vk::Buffer{stagingBuffer}, m_fontImage, vk::ImageLayout::eTransferDstOptimal,
                          copy);

    transitionImage(*cmd, m_fontImage, vk::ImageLayout::eTransferDstOptimal,
                    vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits2::eTransfer,
                    vk::AccessFlagBits2::eTransferWrite, vk::PipelineStageFlagBits2::eFragmentShader,
                    vk::AccessFlagBits2::eShaderRead, vk::ImageAspectFlagBits::eColor);

    cmd.end();

    vk::SubmitInfo submit{
        .commandBufferCount = 1,
        .pCommandBuffers = &*cmd,
    };
    m_graphicsQueue.submit(submit);
    m_graphicsQueue.waitIdle();

    // cmd frees itself (vkFreeCommandBuffers) when it goes out of scope below.
    vmaDestroyBuffer(m_allocator, stagingBuffer, stagingAllocation);

    vk::ImageViewCreateInfo viewInfo{
        .image = m_fontImage,
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR8G8B8A8Unorm,
        .subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    m_fontView = m_device.createImageView(viewInfo);

    vk::SamplerCreateInfo samplerInfo{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eLinear,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
    };
    m_fontSampler = m_device.createSampler(samplerInfo);

    vk::DescriptorSetAllocateInfo descriptorAlloc{
        .descriptorPool = *m_imguiDescriptorPool,
        .descriptorSetCount = 1,
        .pSetLayouts = &*m_imguiDescriptorSetLayout,
    };
    m_fontDescriptor = std::move(m_device.allocateDescriptorSets(descriptorAlloc).front());

    vk::DescriptorImageInfo descriptorImage{
        .sampler = *m_fontSampler,
        .imageView = *m_fontView,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };

    vk::WriteDescriptorSet write{
        .dstSet = *m_fontDescriptor,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .pImageInfo = &descriptorImage,
    };
    m_device.updateDescriptorSets(write, {});

    // ImGui carries the bound texture as an ImTextureID per draw command; use the font's set.
    ImGui::GetIO().Fonts->SetTexID(
        static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(static_cast<VkDescriptorSet>(*m_fontDescriptor))));
    return true;
}

void VulkanBackend::immediateSubmit(const std::function<void(vk::CommandBuffer)> &record) const {
    vk::CommandBufferAllocateInfo allocInfo{
        .commandPool = *commandPool,
        .level = vk::CommandBufferLevel::ePrimary,
        .commandBufferCount = 1,
    };
    vk::raii::CommandBuffer cmd = std::move(m_device.allocateCommandBuffers(allocInfo).front());

    cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    record(*cmd);
    cmd.end();

    vk::SubmitInfo submit{.commandBufferCount = 1, .pCommandBuffers = &*cmd};
    m_graphicsQueue.submit(submit);
    m_graphicsQueue.waitIdle();
    // cmd frees itself (vkFreeCommandBuffers) when it goes out of scope.
}

VulkanBackend::ModelTexture VulkanBackend::createModelTexture(const ImageData &image) const {
    ModelTexture texture;
    const auto width = static_cast<uint32_t>(image.width);
    const auto height = static_cast<uint32_t>(image.height);
    const uint32_t mipLevels =
        static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1;
    const vk::DeviceSize uploadSize = static_cast<vk::DeviceSize>(width) * height * 4;

    // Host-visible staging buffer holding the base level.
    vk::BufferCreateInfo stagingInfo{.size = uploadSize, .usage = vk::BufferUsageFlagBits::eTransferSrc};
    VmaAllocationCreateInfo stagingAlloc{
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO,
    };
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VmaAllocation stagingAllocation = nullptr;
    VmaAllocationInfo stagingAllocInfo{};
    vmaCreateBuffer(m_allocator, reinterpret_cast<const VkBufferCreateInfo *>(&stagingInfo), &stagingAlloc,
                    &stagingBuffer, &stagingAllocation, &stagingAllocInfo);
    std::memcpy(stagingAllocInfo.pMappedData, image.pixels.data(), static_cast<size_t>(uploadSize));

    // Device-local sampled image with room for the full mip chain. TransferSrc is needed too:
    // each mip is produced by blitting down from the previous one.
    vk::ImageCreateInfo imageInfo{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR8G8B8A8Unorm,
        .extent = vk::Extent3D{width, height, 1},
        .mipLevels = mipLevels,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst |
                 vk::ImageUsageFlagBits::eSampled,
    };
    VmaAllocationCreateInfo imageAlloc{.usage = VMA_MEMORY_USAGE_AUTO};
    VkImage handle = VK_NULL_HANDLE;
    vmaCreateImage(m_allocator, reinterpret_cast<const VkImageCreateInfo *>(&imageInfo), &imageAlloc,
                   &handle, &texture.allocation, nullptr);
    texture.image = handle;

    immediateSubmit([&](vk::CommandBuffer cmd) {
        // Per-mip layout transition helper (transitionImage covers a single subresource range,
        // but mip generation flips levels independently).
        auto barrier = [&](uint32_t level, vk::ImageLayout oldLayout, vk::ImageLayout newLayout,
                           vk::PipelineStageFlags2 srcStage, vk::AccessFlags2 srcAccess,
                           vk::PipelineStageFlags2 dstStage, vk::AccessFlags2 dstAccess) {
            vk::ImageMemoryBarrier2 b{
                .srcStageMask = srcStage,
                .srcAccessMask = srcAccess,
                .dstStageMask = dstStage,
                .dstAccessMask = dstAccess,
                .oldLayout = oldLayout,
                .newLayout = newLayout,
                .image = texture.image,
                .subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, level, 1, 0, 1},
            };
            cmd.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b});
        };

        // All levels -> TransferDst, then upload the base level.
        vk::ImageMemoryBarrier2 toDst{
            .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .image = texture.image,
            .subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, mipLevels, 0, 1},
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &toDst});

        vk::BufferImageCopy copy{
            .imageSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .imageExtent = imageInfo.extent,
        };
        cmd.copyBufferToImage(vk::Buffer{stagingBuffer}, texture.image, vk::ImageLayout::eTransferDstOptimal,
                              copy);

        // Blit chain: level i-1 (TransferSrc) down into level i (TransferDst); once level i-1 has
        // been read from, transition it to shader-read.
        int32_t mipW = static_cast<int32_t>(width);
        int32_t mipH = static_cast<int32_t>(height);
        for (uint32_t i = 1; i < mipLevels; ++i) {
            barrier(i - 1, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferSrcOptimal,
                    vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
                    vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);

            const int32_t dstW = mipW > 1 ? mipW / 2 : 1;
            const int32_t dstH = mipH > 1 ? mipH / 2 : 1;
            vk::ImageBlit2 blit{
                .srcSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, i - 1, 0, 1},
                .srcOffsets = std::array{vk::Offset3D{0, 0, 0}, vk::Offset3D{mipW, mipH, 1}},
                .dstSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, i, 0, 1},
                .dstOffsets = std::array{vk::Offset3D{0, 0, 0}, vk::Offset3D{dstW, dstH, 1}},
            };
            vk::BlitImageInfo2 blitInfo{
                .srcImage = texture.image,
                .srcImageLayout = vk::ImageLayout::eTransferSrcOptimal,
                .dstImage = texture.image,
                .dstImageLayout = vk::ImageLayout::eTransferDstOptimal,
                .regionCount = 1,
                .pRegions = &blit,
                .filter = vk::Filter::eLinear,
            };
            cmd.blitImage2(blitInfo);

            barrier(i - 1, vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                    vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead,
                    vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderRead);

            mipW = dstW;
            mipH = dstH;
        }

        // The last level was never blitted from, so it is still in TransferDst.
        barrier(mipLevels - 1, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
                vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderRead);
    });

    vmaDestroyBuffer(m_allocator, stagingBuffer, stagingAllocation);

    vk::ImageViewCreateInfo viewInfo{
        .image = texture.image,
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR8G8B8A8Unorm,
        .subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, mipLevels, 0, 1},
    };
    texture.view = m_device.createImageView(viewInfo);
    return texture;
}

bool VulkanBackend::createModelPipeline() {
    vk::raii::ShaderModule module = nullptr;
    const std::string source = assetPath("assets/shaders/vulkan/model.slang").string();
    if (!compileSlangModule(m_device, "model", source.c_str(), module))
        return false;

    vk::PushConstantRange pushConstant{
        .stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
        .offset = 0,
        .size = sizeof(vk::DeviceAddress),
    };

    vk::PipelineLayoutCreateInfo pipelineLayoutInfo{
        .setLayoutCount = 1,
        .pSetLayouts = &*m_modelDescriptorSetLayout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pushConstant,
    };
    m_modelPipelineLayout = m_device.createPipelineLayout(pipelineLayoutInfo);

    vk::PipelineShaderStageCreateInfo stages[2] = {
        {.stage = vk::ShaderStageFlagBits::eVertex, .module = *module, .pName = "main"},
        {.stage = vk::ShaderStageFlagBits::eFragment, .module = *module, .pName = "main"},
    };

    vk::VertexInputBindingDescription binding{
        .binding = 0,
        .stride = sizeof(Vertex),
        .inputRate = vk::VertexInputRate::eVertex,
    };

    vk::VertexInputAttributeDescription attributes[3] = {
        {.location = 0, .binding = 0, .format = vk::Format::eR32G32B32Sfloat, .offset = offsetof(Vertex, position)},
        {.location = 1, .binding = 0, .format = vk::Format::eR32G32B32Sfloat, .offset = offsetof(Vertex, normal)},
        {.location = 2, .binding = 0, .format = vk::Format::eR32G32Sfloat, .offset = offsetof(Vertex, texCoord)},
    };

    vk::PipelineVertexInputStateCreateInfo vertexInput{
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = 3,
        .pVertexAttributeDescriptions = attributes,
    };

    vk::PipelineInputAssemblyStateCreateInfo inputAssembly{.topology = vk::PrimitiveTopology::eTriangleList};

    vk::PipelineViewportStateCreateInfo viewportState{.viewportCount = 1, .scissorCount = 1};

    // glTF is authored counter-clockwise; the projection's Y flip (see projection.h) would
    // reverse the apparent winding, so leave culling off rather than track that here.
    vk::PipelineRasterizationStateCreateInfo rasterization{
        .polygonMode = vk::PolygonMode::eFill,
        .cullMode = vk::CullModeFlagBits::eNone,
        .frontFace = vk::FrontFace::eCounterClockwise,
        .lineWidth = 1.0f,
    };

    vk::PipelineMultisampleStateCreateInfo multisample{.rasterizationSamples = vk::SampleCountFlagBits::e1};

    vk::PipelineDepthStencilStateCreateInfo depthStencil{
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = vk::CompareOp::eLessOrEqual,
    };

    // Opaque: the model draws before the ImGui overlay, no blending.
    vk::PipelineColorBlendAttachmentState blendAttachment{
        .blendEnable = VK_FALSE,
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
    };

    vk::PipelineColorBlendStateCreateInfo colorBlend{
        .attachmentCount = 1,
        .pAttachments = &blendAttachment,
    };

    vk::DynamicState dynamicStates[2] = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    vk::PipelineDynamicStateCreateInfo dynamicState{
        .dynamicStateCount = 2,
        .pDynamicStates = dynamicStates,
    };

    vk::PipelineRenderingCreateInfo renderingInfo{
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &m_swapchainFormat,
        .depthAttachmentFormat = m_depthFormat,
    };

    vk::GraphicsPipelineCreateInfo pipelineInfo{
        .pNext = &renderingInfo,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vertexInput,
        .pInputAssemblyState = &inputAssembly,
        .pViewportState = &viewportState,
        .pRasterizationState = &rasterization,
        .pMultisampleState = &multisample,
        .pDepthStencilState = &depthStencil,
        .pColorBlendState = &colorBlend,
        .pDynamicState = &dynamicState,
        .layout = *m_modelPipelineLayout,
    };
    m_modelPipeline = m_device.createGraphicsPipeline(nullptr, pipelineInfo);
    return true;
}

bool VulkanBackend::initModel() {
    vk::SamplerCreateInfo samplerInfo{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eLinear,
        .addressModeU = vk::SamplerAddressMode::eRepeat,
        .addressModeV = vk::SamplerAddressMode::eRepeat,
        .addressModeW = vk::SamplerAddressMode::eRepeat,
        .anisotropyEnable = VK_TRUE,
        .maxAnisotropy = 8.0f,
        .maxLod = VK_LOD_CLAMP_NONE,
    };
    m_modelSampler = m_device.createSampler(samplerInfo);

    auto model = loadModel(assetPath("assets/models/DamagedHelmet.glb"));
    if (!model) {
        std::cerr << "VulkanBackend: failed to load sample model, skipping model rendering\n";
        return true; // non-fatal: backend still works without a model
    }

    m_modelTextures.reserve(model->images.size());
    for (const ImageData &image : model->images) {
        if (image.width > 0 && image.height > 0)
            m_modelTextures.push_back(createModelTexture(image));
        else
            m_modelTextures.emplace_back(); // keep indices aligned with model->images
    }

    // Bindless combined-image-sampler array. The descriptor count is only known now (after the
    // model loads), and the last binding uses a variable count so the layout matches exactly.
    const auto textureCount = std::max<uint32_t>(1, static_cast<uint32_t>(m_modelTextures.size()));

    vk::DescriptorSetLayoutBinding textureBinding{
        .binding = 0,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = textureCount,
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
    };
    vk::DescriptorBindingFlags bindingFlags = vk::DescriptorBindingFlagBits::eVariableDescriptorCount;
    vk::DescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsInfo{
        .bindingCount = 1,
        .pBindingFlags = &bindingFlags,
    };
    vk::DescriptorSetLayoutCreateInfo layoutInfo{
        .pNext = &bindingFlagsInfo,
        .bindingCount = 1,
        .pBindings = &textureBinding,
    };
    m_modelDescriptorSetLayout = m_device.createDescriptorSetLayout(layoutInfo);

    vk::DescriptorPoolSize poolSize{
        .type = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = textureCount,
    };
    vk::DescriptorPoolCreateInfo poolInfo{
        .flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &poolSize,
    };
    m_modelDescriptorPool = m_device.createDescriptorPool(poolInfo);

    vk::DescriptorSetVariableDescriptorCountAllocateInfo variableCount{
        .descriptorSetCount = 1,
        .pDescriptorCounts = &textureCount,
    };
    vk::DescriptorSetAllocateInfo setAlloc{
        .pNext = &variableCount,
        .descriptorPool = *m_modelDescriptorPool,
        .descriptorSetCount = 1,
        .pSetLayouts = &*m_modelDescriptorSetLayout,
    };
    m_modelTextureSet = std::move(m_device.allocateDescriptorSets(setAlloc).front());

    std::vector<vk::DescriptorImageInfo> imageInfos;
    imageInfos.reserve(m_modelTextures.size());
    for (const ModelTexture &texture : m_modelTextures) {
        if (!*texture.view)
            continue;
        imageInfos.push_back(vk::DescriptorImageInfo{
            .sampler = *m_modelSampler,
            .imageView = *texture.view,
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        });
    }
    if (!imageInfos.empty()) {
        vk::WriteDescriptorSet write{
            .dstSet = *m_modelTextureSet,
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = static_cast<uint32_t>(imageInfos.size()),
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = imageInfos.data(),
        };
        m_device.updateDescriptorSets(write, {});
    }

    if (!createModelPipeline())
        return false;

    for (const MeshData &mesh : model->meshes) {
        for (const Primitive &primitive : mesh.primitives) {
            if (primitive.indices.empty())
                continue;

            ModelPrimitive gpuPrimitive;
            gpuPrimitive.vertexBuffer = createStaticBuffer(
                primitive.vertices.data(), primitive.vertices.size() * sizeof(Vertex),
                vk::BufferUsageFlagBits::eVertexBuffer);
            gpuPrimitive.indexBuffer = createStaticBuffer(
                primitive.indices.data(), primitive.indices.size() * sizeof(std::uint32_t),
                vk::BufferUsageFlagBits::eIndexBuffer);
            gpuPrimitive.indexCount = static_cast<uint32_t>(primitive.indices.size());

            if (primitive.materialIndex.has_value()) {
                const MaterialData &material = model->materials[*primitive.materialIndex];
                gpuPrimitive.baseColorFactor = material.baseColorFactor;
                if (material.baseColorTextureIndex.has_value())
                    gpuPrimitive.textureIndex = static_cast<int32_t>(*material.baseColorTextureIndex);
            }

            m_modelPrimitives.push_back(std::move(gpuPrimitive));
        }
    }

    return true;
}

void VulkanBackend::ensureBufferCapacity(Buffer &buffer, vk::DeviceSize size,
                                         vk::BufferUsageFlags usage) const {
    if (buffer.handle && buffer.capacity >= size)
        return;

    // Grow with headroom so steady-state frames stop reallocating.
    vk::DeviceSize newCapacity =
        std::max<vk::DeviceSize>(size, buffer.capacity ? buffer.capacity * 2 : 64 * 1024);

    if (buffer.handle)
        m_device.waitIdle(); // old buffer may still be read by an in-flight frame
    destroyBuffer(buffer);

    vk::BufferCreateInfo bufferInfo{
        .size = newCapacity,
        .usage = usage,
    };

    VmaAllocationCreateInfo allocInfo{
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO,
    };

    VkBuffer handle = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VmaAllocationInfo mappedInfo{};
    vmaCreateBuffer(m_allocator, reinterpret_cast<const VkBufferCreateInfo *>(&bufferInfo), &allocInfo,
                    &handle, &allocation, &mappedInfo);
    buffer.handle = vk::Buffer{handle};
    buffer.allocation = allocation;
    buffer.mapped = mappedInfo.pMappedData;
    buffer.capacity = newCapacity;
    if (usage & vk::BufferUsageFlagBits::eShaderDeviceAddress)
        buffer.deviceAddress = m_device.getBufferAddress(vk::BufferDeviceAddressInfo{.buffer = buffer.handle});
}

VulkanBackend::Buffer VulkanBackend::createStaticBuffer(const void *data, vk::DeviceSize size,
                                                        vk::BufferUsageFlags usage) const {
    vk::BufferCreateInfo bufferInfo{.size = size, .usage = usage};
    VmaAllocationCreateInfo allocInfo{
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO,
    };

    VkBuffer handle = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VmaAllocationInfo mappedInfo{};
    vmaCreateBuffer(m_allocator, reinterpret_cast<const VkBufferCreateInfo *>(&bufferInfo), &allocInfo,
                    &handle, &allocation, &mappedInfo);
    std::memcpy(mappedInfo.pMappedData, data, static_cast<size_t>(size));

    Buffer buffer;
    buffer.handle = vk::Buffer{handle};
    buffer.allocation = allocation;
    buffer.mapped = mappedInfo.pMappedData;
    buffer.capacity = size;
    return buffer;
}

void VulkanBackend::destroyBuffer(Buffer &buffer) const {
    if (buffer.handle)
        vmaDestroyBuffer(m_allocator, buffer.handle, buffer.allocation);
    buffer = {};
}

void VulkanBackend::shutdown() {
    if (!*m_device)
        return;
    m_device.waitIdle();

    for (FrameData &frame : m_frames) {
        destroyBuffer(frame.vertexBuffer);
        destroyBuffer(frame.indexBuffer);
        destroyBuffer(frame.shaderDataBuffer);
        frame.inFlight = nullptr;
        frame.imageAvailable = nullptr;
        frame.commandBuffer = nullptr; // must free before the pool it came from
    }

    for (ModelPrimitive &primitive : m_modelPrimitives) {
        destroyBuffer(primitive.vertexBuffer);
        destroyBuffer(primitive.indexBuffer);
    }
    m_modelPrimitives.clear();
    for (ModelTexture &texture : m_modelTextures) {
        texture.view = nullptr;
        if (texture.image)
            vmaDestroyImage(m_allocator, texture.image, texture.allocation);
    }
    m_modelTextures.clear();
    m_modelSampler = nullptr;
    m_modelTextureSet = nullptr; // must free before the pool it came from
    m_modelDescriptorPool = nullptr;
    m_modelPipeline = nullptr;
    m_modelPipelineLayout = nullptr;
    m_modelDescriptorSetLayout = nullptr;

    m_fontSampler = nullptr;
    m_fontView = nullptr;
    if (m_fontImage)
        vmaDestroyImage(m_allocator, m_fontImage, m_fontAllocation);
    m_fontDescriptor = nullptr; // must free before the pool it came from
    m_imguiDescriptorPool = nullptr;
    m_imguiPipeline = nullptr;
    m_imguiPipelineLayout = nullptr;
    m_imguiDescriptorSetLayout = nullptr;

    destroySwapchain();

    commandPool = nullptr;
    if (m_allocator)
        vmaDestroyAllocator(m_allocator);
    m_device = nullptr;
    m_surface = nullptr;
    m_debugMessenger = nullptr;
    m_instance = nullptr;
}

void VulkanBackend::resize(int width, int height) {
    recreateSwapchain();
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void VulkanBackend::render(ImDrawData *drawData, const ImVec4 &clearColor) {
    if (!*m_swapchain)
        return;

    int width = 0, height = 0;
    glfwGetFramebufferSize(m_window, &width, &height);
    if (width == 0 || height == 0)
        return; // minimized

    FrameData &frame = m_frames[m_frameIndex];
    (void)m_device.waitForFences(*frame.inFlight, VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    bool needsRecreate = false;
    try {
        auto acquired = m_swapchain.acquireNextImage(UINT64_MAX, *frame.imageAvailable, nullptr);
        imageIndex = acquired.value;
        if (acquired.result == vk::Result::eSuboptimalKHR)
            needsRecreate = true;
    } catch (const vk::OutOfDateKHRError &) {
        recreateSwapchain();
        return;
    }

    m_device.resetFences(*frame.inFlight);

    vk::CommandBuffer cmd = frame.commandBuffer;
    cmd.reset();
    cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

    transitionImage(cmd, m_swapchainImages[imageIndex], vk::ImageLayout::eUndefined,
                    vk::ImageLayout::eColorAttachmentOptimal, vk::PipelineStageFlagBits2::eTopOfPipe,
                    {}, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                    vk::AccessFlagBits2::eColorAttachmentWrite, vk::ImageAspectFlagBits::eColor);

    // Depth is cleared every frame, so transitioning from UNDEFINED (discarding contents) is fine.
    transitionImage(cmd, m_depthImage, vk::ImageLayout::eUndefined,
                    vk::ImageLayout::eDepthAttachmentOptimal, vk::PipelineStageFlagBits2::eTopOfPipe,
                    {},
                    vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                        vk::PipelineStageFlagBits2::eLateFragmentTests,
                    vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
                    vk::ImageAspectFlagBits::eDepth);

    vk::RenderingAttachmentInfo colorAttachment{
        .imageView = *m_swapchainImageViews[imageIndex],
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = vk::ClearColorValue{std::array<float, 4>{clearColor.x, clearColor.y, clearColor.z, clearColor.w}},
    };

    vk::RenderingAttachmentInfo depthAttachment{
        .imageView = *m_depthView,
        .imageLayout = vk::ImageLayout::eDepthAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eDontCare,
        .clearValue = vk::ClearDepthStencilValue{1.0f, 0},
    };

    vk::RenderingInfo renderingInfo{
        .renderArea = vk::Rect2D{{0, 0}, m_swapchainExtent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &colorAttachment,
        .pDepthAttachment = &depthAttachment,
    };

    cmd.beginRendering(renderingInfo);

    // The model draws into the depth-tested scene first; the ImGui overlay composites on top.
    recordModel(cmd, frame);
    recordImGui(cmd, drawData, frame);

    cmd.endRendering();

    transitionImage(cmd, m_swapchainImages[imageIndex], vk::ImageLayout::eColorAttachmentOptimal,
                    vk::ImageLayout::ePresentSrcKHR,
                    vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                    vk::AccessFlagBits2::eColorAttachmentWrite,
                    vk::PipelineStageFlagBits2::eBottomOfPipe, {}, vk::ImageAspectFlagBits::eColor);

    cmd.end();

    vk::PipelineStageFlags waitStage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
    vk::SubmitInfo submit{
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &*frame.imageAvailable,
        .pWaitDstStageMask = &waitStage,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &*m_renderFinished[imageIndex],
    };
    m_graphicsQueue.submit(submit, *frame.inFlight);

    vk::PresentInfoKHR presentInfo{
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &*m_renderFinished[imageIndex],
        .swapchainCount = 1,
        .pSwapchains = &*m_swapchain,
        .pImageIndices = &imageIndex,
    };
    try {
        if (m_presentQueue.presentKHR(presentInfo) == vk::Result::eSuboptimalKHR)
            needsRecreate = true;
    } catch (const vk::OutOfDateKHRError &) {
        needsRecreate = true;
    }

    m_frameIndex = (m_frameIndex + 1) % kFramesInFlight;

    if (needsRecreate)
        recreateSwapchain();
}

void VulkanBackend::recordModel(vk::CommandBuffer cmd, FrameData &frame) {
    if (m_modelPrimitives.empty())
        return;

    int width = 0, height = 0;
    glfwGetFramebufferSize(m_window, &width, &height);
    if (width == 0 || height == 0)
        return;
    const float aspect = static_cast<float>(width) / static_cast<float>(height);

    // No input plumbing reaches the Backend interface yet, so orbit the camera around the model
    // to exercise the loader/camera/projection pipeline end-to-end (mirrors the OpenGL backend).
    const auto time = static_cast<float>(glfwGetTime());
    constexpr float orbitRadius = 4.0f;
    m_camera.Position = glm::vec3(std::sin(time * 0.5f) * orbitRadius, 1.5f, std::cos(time * 0.5f) * orbitRadius);
    m_camera.Front = glm::normalize(-m_camera.Position);

    const glm::mat4 view = m_camera.GetViewMatrix();
    const glm::mat4 proj = perspective(Backend::Vulkan, glm::radians(45.0f), aspect, 0.1f, 100.0f);

    const vk::DeviceSize slotStride = sizeof(ShaderData); // alignas(16) keeps this a multiple of 16
    ensureBufferCapacity(frame.shaderDataBuffer, m_modelPrimitives.size() * slotStride,
                         vk::BufferUsageFlagBits::eShaderDeviceAddress);

    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *m_modelPipeline);

    vk::Viewport viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    cmd.setViewport(0, viewport);
    cmd.setScissor(0, vk::Rect2D{{0, 0}, m_swapchainExtent});

    if (*m_modelTextureSet)
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_modelPipelineLayout, 0,
                               *m_modelTextureSet, {});

    for (size_t i = 0; i < m_modelPrimitives.size(); ++i) {
        const ModelPrimitive &primitive = m_modelPrimitives[i];

        const ShaderData shaderData{
            .view = view,
            .projection = proj,
            .baseColorFactor = primitive.baseColorFactor,
            .textureIndex = primitive.textureIndex,
        };
        auto *slot = static_cast<std::byte *>(frame.shaderDataBuffer.mapped) + i * slotStride;
        std::memcpy(slot, &shaderData, sizeof(ShaderData));

        const vk::DeviceAddress address = frame.shaderDataBuffer.deviceAddress + i * slotStride;
        cmd.pushConstants<vk::DeviceAddress>(
            *m_modelPipelineLayout, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0,
            address);

        cmd.bindVertexBuffers(0, primitive.vertexBuffer.handle, {vk::DeviceSize{0}});
        cmd.bindIndexBuffer(primitive.indexBuffer.handle, 0, vk::IndexType::eUint32);
        cmd.drawIndexed(primitive.indexCount, 1, 0, 0, 0);
    }
}

void VulkanBackend::recordImGui(vk::CommandBuffer cmd, ImDrawData *drawData, FrameData &frame) const {
    const int fbWidth = static_cast<int>(drawData->DisplaySize.x * drawData->FramebufferScale.x);
    const int fbHeight = static_cast<int>(drawData->DisplaySize.y * drawData->FramebufferScale.y);
    if (fbWidth <= 0 || fbHeight <= 0 || drawData->TotalVtxCount == 0)
        return;

    const vk::DeviceSize vertexSize = drawData->TotalVtxCount * sizeof(ImDrawVert);
    const vk::DeviceSize indexSize = drawData->TotalIdxCount * sizeof(ImDrawIdx);
    ensureBufferCapacity(frame.vertexBuffer, vertexSize, vk::BufferUsageFlagBits::eVertexBuffer);
    ensureBufferCapacity(frame.indexBuffer, indexSize, vk::BufferUsageFlagBits::eIndexBuffer);

    auto *vertexDst = static_cast<ImDrawVert *>(frame.vertexBuffer.mapped);
    auto *indexDst = static_cast<ImDrawIdx *>(frame.indexBuffer.mapped);
    for (int n = 0; n < drawData->CmdListsCount; ++n) {
        const ImDrawList *cmdList = drawData->CmdLists[n];
        std::memcpy(vertexDst, cmdList->VtxBuffer.Data, cmdList->VtxBuffer.Size * sizeof(ImDrawVert));
        std::memcpy(indexDst, cmdList->IdxBuffer.Data, cmdList->IdxBuffer.Size * sizeof(ImDrawIdx));
        vertexDst += cmdList->VtxBuffer.Size;
        indexDst += cmdList->IdxBuffer.Size;
    }

    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *m_imguiPipeline);
    cmd.bindVertexBuffers(0, frame.vertexBuffer.handle, {vk::DeviceSize{0}});
    cmd.bindIndexBuffer(frame.indexBuffer.handle, 0,
                        sizeof(ImDrawIdx) == 2 ? vk::IndexType::eUint16 : vk::IndexType::eUint32);

    vk::Viewport viewport{0.0f, 0.0f, static_cast<float>(fbWidth), static_cast<float>(fbHeight),
                          0.0f, 1.0f};
    cmd.setViewport(0, viewport);

    // Map ImGui's display-space coordinates to clip space. Vulkan's clip-space Y points down,
    // which matches ImGui's top-left origin, so no Y flip is needed here.
    ImGuiPushConstants pushConstants{};
    pushConstants.scale[0] = 2.0f / drawData->DisplaySize.x;
    pushConstants.scale[1] = 2.0f / drawData->DisplaySize.y;
    pushConstants.translate[0] = -1.0f - drawData->DisplayPos.x * pushConstants.scale[0];
    pushConstants.translate[1] = -1.0f - drawData->DisplayPos.y * pushConstants.scale[1];
    cmd.pushConstants<ImGuiPushConstants>(*m_imguiPipelineLayout, vk::ShaderStageFlagBits::eVertex, 0,
                                          pushConstants);

    const ImVec2 clipOffset = drawData->DisplayPos;
    const ImVec2 clipScale = drawData->FramebufferScale;

    int globalVtxOffset = 0;
    int globalIdxOffset = 0;
    for (int n = 0; n < drawData->CmdListsCount; ++n) {
        const ImDrawList *cmdList = drawData->CmdLists[n];
        for (int i = 0; i < cmdList->CmdBuffer.Size; ++i) {
            const ImDrawCmd &drawCmd = cmdList->CmdBuffer[i];

            ImVec2 clipMin{(drawCmd.ClipRect.x - clipOffset.x) * clipScale.x,
                           (drawCmd.ClipRect.y - clipOffset.y) * clipScale.y};
            ImVec2 clipMax{(drawCmd.ClipRect.z - clipOffset.x) * clipScale.x,
                           (drawCmd.ClipRect.w - clipOffset.y) * clipScale.y};
            clipMin.x = std::max(clipMin.x, 0.0f);
            clipMin.y = std::max(clipMin.y, 0.0f);
            clipMax.x = std::min(clipMax.x, static_cast<float>(fbWidth));
            clipMax.y = std::min(clipMax.y, static_cast<float>(fbHeight));
            if (clipMax.x <= clipMin.x || clipMax.y <= clipMin.y)
                continue;

            vk::Rect2D scissor{
                .offset = vk::Offset2D{static_cast<int32_t>(clipMin.x), static_cast<int32_t>(clipMin.y)},
                .extent = vk::Extent2D{static_cast<uint32_t>(clipMax.x - clipMin.x),
                                       static_cast<uint32_t>(clipMax.y - clipMin.y)},
            };
            cmd.setScissor(0, scissor);

            vk::DescriptorSet descriptor{
                reinterpret_cast<VkDescriptorSet>(static_cast<uintptr_t>(drawCmd.GetTexID()))};
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_imguiPipelineLayout, 0, descriptor,
                                   {});

            cmd.drawIndexed(drawCmd.ElemCount, 1, drawCmd.IdxOffset + globalIdxOffset,
                            static_cast<int32_t>(drawCmd.VtxOffset) + globalVtxOffset, 0);
        }
        globalIdxOffset += cmdList->IdxBuffer.Size;
        globalVtxOffset += cmdList->VtxBuffer.Size;
    }
}

#endif
