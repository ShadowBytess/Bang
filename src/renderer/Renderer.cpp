#include "bang/render/Renderer.hpp"

#include "quad_fragment.spv.hpp"
#include "quad_vertex.spv.hpp"

#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace bang::render {

namespace {

constexpr int atlasSize = 2048;
constexpr std::size_t initialInstanceCapacity = 4096;

VkShaderModule createShaderModule(VkDevice device, const unsigned char* data,
    std::size_t size)
{
    VkShaderModuleCreateInfo info { .sType =
            VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    info.codeSize = size;
    info.pCode = reinterpret_cast<const std::uint32_t*>(data);
    VkShaderModule module = nullptr;
    if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS) {
        throw std::runtime_error("cannot create shader module");
    }
    return module;
}

} // namespace

struct Renderer::Impl {
    wl_display* display = nullptr;
    wl_surface* surface = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    VkInstance instance = nullptr;
    VkPhysicalDevice physicalDevice = nullptr;
    VkDevice device = nullptr;
    std::uint32_t queueFamily = 0;
    VkQueue queue = nullptr;
    VkSurfaceKHR surfaceHandle = nullptr;
    VkSwapchainKHR swapchain = nullptr;
    VkFormat swapchainFormat = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D extent { 0, 0 };
    std::vector<VkImage> swapchainImages;
    std::vector<VkImageView> swapchainViews;

    VkCommandPool commandPool = nullptr;
    VkCommandBuffer commandBuffer = nullptr;
    VkSemaphore imageAvailable = nullptr;
    VkSemaphore renderFinished = nullptr;
    VkFence frameFence = nullptr;

    VkBuffer cornerBuffer = nullptr;
    VkDeviceMemory cornerMemory = nullptr;
    VkBuffer indexBuffer = nullptr;
    VkDeviceMemory indexMemory = nullptr;
    VkBuffer instanceBuffer = nullptr;
    VkDeviceMemory instanceMemory = nullptr;
    std::size_t instanceCapacity = 0;

    VkDescriptorSetLayout setLayout = nullptr;
    VkPipelineLayout pipelineLayout = nullptr;
    VkPipeline pipeline = nullptr;
    VkDescriptorPool descriptorPool = nullptr;
    VkDescriptorSet descriptorSet = nullptr;

    VkImage atlasImage = nullptr;
    VkDeviceMemory atlasMemory = nullptr;
    VkImageView atlasView = nullptr;
    VkSampler atlasSampler = nullptr;
    bool atlasInitialized = false;
    int atlasCursorX = 0;
    int atlasCursorY = 0;
    int atlasRowHeight = 0;

    struct PendingUpload {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> pixels;
    };
    std::vector<PendingUpload> pendingUploads;

    bool swapchainDirty = true;
    VkDebugUtilsMessengerEXT debugMessenger_ = nullptr;

    [[noreturn]] void fail(const char* what) const
    {
        throw std::runtime_error(std::string(what) + " failed");
    }

    [[nodiscard]] std::uint32_t findMemoryType(
        std::uint32_t typeBits, VkMemoryPropertyFlags properties) const
    {
        VkPhysicalDeviceMemoryProperties memoryProperties {};
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
        for (std::uint32_t index = 0; index < memoryProperties.memoryTypeCount;
            ++index) {
            const bool allowed = (typeBits & (1u << index)) != 0;
            const bool suitable =
                (memoryProperties.memoryTypes[index].propertyFlags & properties)
                == properties;
            if (allowed && suitable) {
                return index;
            }
        }
        fail("no suitable Vulkan memory type");
    }

    struct BufferAllocation {
        VkBuffer buffer = nullptr;
        VkDeviceMemory memory = nullptr;
    };

    BufferAllocation allocateBuffer(std::size_t bytes,
        VkBufferUsageFlags usage, VkMemoryPropertyFlags properties) const
    {
        VkBufferCreateInfo info { .sType =
                VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size = bytes;
        info.usage = usage;
        BufferAllocation allocation;
        if (vkCreateBuffer(device, &info, nullptr, &allocation.buffer)
            != VK_SUCCESS) {
            fail("vkCreateBuffer");
        }
        VkMemoryRequirements requirements {};
        vkGetBufferMemoryRequirements(device, allocation.buffer, &requirements);
        VkMemoryAllocateInfo allocate { .sType =
                VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex =
            findMemoryType(requirements.memoryTypeBits, properties);
        if (vkAllocateMemory(device, &allocate, nullptr, &allocation.memory)
            != VK_SUCCESS) {
            fail("vkAllocateMemory");
        }
        if (vkBindBufferMemory(device, allocation.buffer, allocation.memory, 0)
            != VK_SUCCESS) {
            fail("vkBindBufferMemory");
        }
        return allocation;
    }

    void writeBuffer(VkDeviceMemory memory, std::size_t size,
        const void* data) const
    {
        void* mapped = nullptr;
        if (vkMapMemory(device, memory, 0, size, 0, &mapped) != VK_SUCCESS) {
            fail("vkMapMemory");
        }
        std::memcpy(mapped, data, size);
        vkUnmapMemory(device, memory);
    }

    void createInstance();
    void pickDevice();
    void createSwapchain();
    void destroySwapchainObjects();
    void createAtlas();
    void updateAtlasDescriptor();
    void createFixedBuffers();
    void createPipeline();
    void ensureInstanceCapacity(std::size_t count);
    void flushUploads();
};

void Renderer::Impl::createInstance()
{
    VkApplicationInfo appInfo { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO };
    appInfo.pApplicationName = "Bang";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    static constexpr const char* extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
        VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
    };
    static constexpr const char* validationLayers[] = {
        "VK_LAYER_KHRONOS_validation",
    };

    const bool validate = std::getenv("BANG_VULKAN_VALIDATION") != nullptr;

    VkInstanceCreateInfo info { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    info.pApplicationInfo = &appInfo;
    info.enabledExtensionCount = validate ? 3 : 2;
    info.ppEnabledExtensionNames = extensions;
    if (validate) {
        info.enabledLayerCount = 1;
        info.ppEnabledLayerNames = validationLayers;
    }
    if (vkCreateInstance(&info, nullptr, &instance) != VK_SUCCESS) {
        fail("vkCreateInstance");
    }

    if (validate) {
        VkDebugUtilsMessengerCreateInfoEXT debugInfo {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT
        };
        debugInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
            | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debugInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
            | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
            | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debugInfo.pfnUserCallback = [](VkDebugUtilsMessageSeverityFlagBitsEXT,
                                        VkDebugUtilsMessageTypeFlagsEXT,
                                        const VkDebugUtilsMessengerCallbackDataEXT*
                                            data,
                                        void*) -> VkBool32 {
            std::fprintf(stderr, "vulkan: %s\n", data->pMessage);
            return VK_FALSE;
        };
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance,
                "vkCreateDebugUtilsMessengerEXT"));
        if (create != nullptr) {
            create(instance, &debugInfo, nullptr, &debugMessenger_);
        }
    }
}

void Renderer::Impl::pickDevice()
{
    VkWaylandSurfaceCreateInfoKHR surfaceInfo {
        .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR
    };
    surfaceInfo.display = display;
    surfaceInfo.surface = surface;
    if (vkCreateWaylandSurfaceKHR(instance, &surfaceInfo, nullptr,
            &surfaceHandle)
        != VK_SUCCESS) {
        fail("vkCreateWaylandSurfaceKHR");
    }

    std::uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        fail("no Vulkan physical devices");
    }
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

    for (const VkPhysicalDevice candidate : devices) {
        std::uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(
            candidate, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(
            candidate, &familyCount, families.data());
        for (std::uint32_t family = 0; family < familyCount; ++family) {
            if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
                continue;
            }
            VkBool32 presentable = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(
                candidate, family, surfaceHandle, &presentable);
            if (!presentable) {
                continue;
            }
            VkPhysicalDeviceProperties properties {};
            vkGetPhysicalDeviceProperties(candidate, &properties);
            if (properties.apiVersion < VK_API_VERSION_1_3) {
                continue;
            }
            physicalDevice = candidate;
            queueFamily = family;

            const float priority = 1.0f;
            VkDeviceQueueCreateInfo queueInfo {
                .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO
            };
            queueInfo.queueFamilyIndex = family;
            queueInfo.queueCount = 1;
            queueInfo.pQueuePriorities = &priority;

            static constexpr const char* deviceExtensions[] = {
                VK_KHR_SWAPCHAIN_EXTENSION_NAME,
            };
            VkPhysicalDeviceDynamicRenderingFeatures dynamicRendering {
                .sType =
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES
            };
            dynamicRendering.dynamicRendering = VK_TRUE;
            VkDeviceCreateInfo deviceInfo {
                .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO
            };
            deviceInfo.pNext = &dynamicRendering;
            deviceInfo.queueCreateInfoCount = 1;
            deviceInfo.pQueueCreateInfos = &queueInfo;
            deviceInfo.enabledExtensionCount = 1;
            deviceInfo.ppEnabledExtensionNames = deviceExtensions;

            if (vkCreateDevice(candidate, &deviceInfo, nullptr, &device)
                == VK_SUCCESS) {
                vkGetDeviceQueue(device, family, 0, &queue);
                return;
            }
            physicalDevice = nullptr;
        }
    }
    fail("no usable Vulkan 1.3 device with Wayland present support");
}

void Renderer::Impl::destroySwapchainObjects()
{
    for (const VkImageView view : swapchainViews) {
        vkDestroyImageView(device, view, nullptr);
    }
    swapchainViews.clear();
    swapchainImages.clear();
    if (swapchain != nullptr) {
        vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = nullptr;
    }
}

void Renderer::Impl::createSwapchain()
{
    VkSurfaceCapabilitiesKHR capabilities {};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
            physicalDevice, surfaceHandle, &capabilities)
        != VK_SUCCESS) {
        fail("vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    }

    extent = capabilities.currentExtent;
    if (extent.width == 0xFFFFFFFFu || extent.height == 0xFFFFFFFFu
        || extent.width == 0 || extent.height == 0) {
        extent.width = width;
        extent.height = height;
    }

    std::uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(
        physicalDevice, surfaceHandle, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(
        physicalDevice, surfaceHandle, &formatCount, formats.data());
    swapchainFormat = formats.empty() ? VK_FORMAT_B8G8R8A8_UNORM
                                      : formats.front().format;
    for (const auto& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_UNORM
            && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            swapchainFormat = format.format;
            break;
        }
    }

    std::uint32_t imageCount = capabilities.minImageCount + 1;
    if (imageCount < 2u) {
        imageCount = 2u;
    }
    if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount) {
        imageCount = capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR info {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR
    };
    info.surface = surfaceHandle;
    info.minImageCount = imageCount;
    info.imageFormat = swapchainFormat;
    info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = capabilities.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    const VkResult swapchainResult =
        vkCreateSwapchainKHR(device, &info, nullptr, &swapchain);
    if (swapchainResult != VK_SUCCESS) {
        std::fprintf(stderr, "bang: swapchain extent=%ux%u formats=%zu"
                             " minImages=%u maxImages=%u result=%d\n",
            info.imageExtent.width, info.imageExtent.height, formats.size(),
            imageCount, capabilities.maxImageCount,
            static_cast<int>(swapchainResult));
        fail("vkCreateSwapchainKHR");
    }

    std::uint32_t actualCount = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &actualCount, nullptr);
    swapchainImages.resize(actualCount);
    vkGetSwapchainImagesKHR(device, swapchain, &actualCount, swapchainImages.data());

    for (const VkImage image : swapchainImages) {
        VkImageViewCreateInfo viewInfo {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO
        };
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = swapchainFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        VkImageView view = nullptr;
        if (vkCreateImageView(device, &viewInfo, nullptr, &view) != VK_SUCCESS) {
            fail("vkCreateImageView");
        }
        swapchainViews.push_back(view);
    }
    swapchainDirty = false;
}

void Renderer::Impl::createAtlas()
{
    VkImageCreateInfo imageInfo { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8_UNORM;
    imageInfo.extent = { atlasSize, atlasSize, 1 };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage =
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &imageInfo, nullptr, &atlasImage) != VK_SUCCESS) {
        fail("vkCreateImage");
    }

    VkMemoryRequirements requirements {};
    vkGetImageMemoryRequirements(device, atlasImage, &requirements);
    VkMemoryAllocateInfo allocate { .sType =
            VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = findMemoryType(requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device, &allocate, nullptr, &atlasMemory)
        != VK_SUCCESS) {
        fail("vkAllocateMemory(atlas)");
    }
    if (vkBindImageMemory(device, atlasImage, atlasMemory, 0) != VK_SUCCESS) {
        fail("vkBindImageMemory");
    }

    VkImageViewCreateInfo viewInfo { .sType =
            VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    viewInfo.image = atlasImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device, &viewInfo, nullptr, &atlasView)
        != VK_SUCCESS) {
        fail("vkCreateImageView(atlas)");
    }

    VkSamplerCreateInfo samplerInfo { .sType =
            VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxAnisotropy = 1.0f;
    if (vkCreateSampler(device, &samplerInfo, nullptr, &atlasSampler)
        != VK_SUCCESS) {
        fail("vkCreateSampler");
    }
}

void Renderer::Impl::updateAtlasDescriptor()
{
    VkDescriptorImageInfo imageInfo {};
    imageInfo.sampler = atlasSampler;
    imageInfo.imageView = atlasView;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    write.dstSet = descriptorSet;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}

void Renderer::Impl::createFixedBuffers()
{
    const float corners[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    const std::uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };

    const auto cornerAllocation = allocateBuffer(sizeof(corners),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
            | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    cornerBuffer = cornerAllocation.buffer;
    cornerMemory = cornerAllocation.memory;
    writeBuffer(cornerMemory, sizeof(corners), corners);

    const auto indexAllocation = allocateBuffer(sizeof(indices),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
            | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    indexBuffer = indexAllocation.buffer;
    indexMemory = indexAllocation.memory;
    writeBuffer(indexMemory, sizeof(indices), indices);

    ensureInstanceCapacity(initialInstanceCapacity);
}

void Renderer::Impl::ensureInstanceCapacity(std::size_t count)
{
    if (count <= instanceCapacity) {
        return;
    }
    std::size_t capacity =
        instanceCapacity == 0 ? initialInstanceCapacity : instanceCapacity * 2;
    while (capacity < count) {
        capacity *= 2;
    }
    if (instanceBuffer != nullptr) {
        vkDestroyBuffer(device, instanceBuffer, nullptr);
        vkFreeMemory(device, instanceMemory, nullptr);
    }
    const auto allocation = allocateBuffer(capacity * sizeof(Instance),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
            | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    instanceBuffer = allocation.buffer;
    instanceMemory = allocation.memory;
    instanceCapacity = capacity;
}

void Renderer::Impl::createPipeline()
{
    VkDescriptorSetLayoutBinding samplerBinding {};
    samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerBinding.descriptorCount = 1;
    samplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo setInfo {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };
    setInfo.bindingCount = 1;
    setInfo.pBindings = &samplerBinding;
    if (vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &setLayout)
        != VK_SUCCESS) {
        fail("vkCreateDescriptorSetLayout");
    }

    VkPushConstantRange pushRange {};
    pushRange.offset = 0;
    pushRange.size = sizeof(float) * 2;
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkPipelineLayoutCreateInfo layoutInfo {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
    };
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout)
        != VK_SUCCESS) {
        fail("vkCreatePipelineLayout");
    }

    const VkShaderModule vertexModule = createShaderModule(
        device, quad_vertex_spirv_bytes(), quad_vertex_spirv_size());
    const VkShaderModule fragmentModule = createShaderModule(
        device, quad_fragment_spirv_bytes(), quad_fragment_spirv_size());

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertexModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragmentModule;
    stages[1].pName = "main";

    VkVertexInputBindingDescription bindings[2] = {};
    bindings[0] = { 0, sizeof(float) * 2, VK_VERTEX_INPUT_RATE_VERTEX };
    bindings[1] = { 1, static_cast<std::uint32_t>(sizeof(Instance)),
        VK_VERTEX_INPUT_RATE_INSTANCE };

    VkVertexInputAttributeDescription attributes[6] = {};
    attributes[0] = { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 };
    for (int index = 0; index < 5; ++index) {
        attributes[index + 1] = { static_cast<std::uint32_t>(index + 1), 1,
            VK_FORMAT_R32G32B32A32_SFLOAT,
            static_cast<std::uint32_t>(index * 16) };
    }

    VkPipelineVertexInputStateCreateInfo vertexInput {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
    };
    vertexInput.vertexBindingDescriptionCount = 2;
    vertexInput.pVertexBindingDescriptions = bindings;
    vertexInput.vertexAttributeDescriptionCount = 6;
    vertexInput.pVertexAttributeDescriptions = attributes;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO
    };
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO
    };
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterization {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO
    };
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO
    };
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO
    };

    VkPipelineColorBlendAttachmentState blendAttachment {};
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT
        | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT
        | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo colorBlend {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO
    };
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;

    const VkDynamicState dynamicStates[2] = { VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO
    };
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    VkPipelineRenderingCreateInfo renderingInfo {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO
    };
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachmentFormats = &swapchainFormat;
    VkGraphicsPipelineCreateInfo pipelineInfo {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO
    };
    pipelineInfo.pNext = &renderingInfo;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterization;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipelineLayout;
    if (vkCreateGraphicsPipelines(
            device, nullptr, 1, &pipelineInfo, nullptr, &pipeline)
        != VK_SUCCESS) {
        fail("vkCreateGraphicsPipelines");
    }

    vkDestroyShaderModule(device, vertexModule, nullptr);
    vkDestroyShaderModule(device, fragmentModule, nullptr);

    VkDescriptorPoolSize poolSize {};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 1;
    VkDescriptorPoolCreateInfo poolInfo {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO
    };
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool)
        != VK_SUCCESS) {
        fail("vkCreateDescriptorPool");
    }
    VkDescriptorSetAllocateInfo setAllocate {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
    };
    setAllocate.descriptorPool = descriptorPool;
    setAllocate.descriptorSetCount = 1;
    setAllocate.pSetLayouts = &setLayout;
    if (vkAllocateDescriptorSets(device, &setAllocate, &descriptorSet)
        != VK_SUCCESS) {
        fail("vkAllocateDescriptorSets");
    }
    updateAtlasDescriptor();

    VkCommandPoolCreateInfo poolCreateInfo {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO
    };
    poolCreateInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolCreateInfo.queueFamilyIndex = queueFamily;
    if (vkCreateCommandPool(device, &poolCreateInfo, nullptr, &commandPool)
        != VK_SUCCESS) {
        fail("vkCreateCommandPool");
    }
    VkCommandBufferAllocateInfo commandAllocate {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO
    };
    commandAllocate.commandPool = commandPool;
    commandAllocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandAllocate.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device, &commandAllocate, &commandBuffer)
        != VK_SUCCESS) {
        fail("vkAllocateCommandBuffers");
    }

    VkSemaphoreCreateInfo semaphoreInfo {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
    };
    if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &imageAvailable)
            != VK_SUCCESS
        || vkCreateSemaphore(device, &semaphoreInfo, nullptr, &renderFinished)
            != VK_SUCCESS) {
        fail("vkCreateSemaphore");
    }
    VkFenceCreateInfo fenceInfo { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateFence(device, &fenceInfo, nullptr, &frameFence) != VK_SUCCESS) {
        fail("vkCreateFence");
    }
}

void Renderer::Impl::flushUploads()
{
    if (pendingUploads.empty()) {
        return;
    }
    VkDeviceSize stagingSize = 0;
    for (const auto& upload : pendingUploads) {
        stagingSize += static_cast<VkDeviceSize>(upload.pixels.size());
    }

    const auto staging =
        allocateBuffer(static_cast<std::size_t>(stagingSize),
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    void* mapped = nullptr;
    if (vkMapMemory(device, staging.memory, 0, stagingSize, 0, &mapped)
        != VK_SUCCESS) {
        fail("vkMapMemory(staging)");
    }
    VkDeviceSize offset = 0;
    for (const auto& upload : pendingUploads) {
        std::memcpy(static_cast<std::byte*>(mapped) + offset,
            upload.pixels.data(), upload.pixels.size());
        offset += static_cast<VkDeviceSize>(upload.pixels.size());
    }
    vkUnmapMemory(device, staging.memory);

    vkResetCommandBuffer(commandBuffer, 0);
    VkCommandBufferBeginInfo beginInfo {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO
    };
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
        fail("vkBeginCommandBuffer");
    }

    VkImageMemoryBarrier toTransfer { .sType =
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    toTransfer.srcAccessMask = 0;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toTransfer.oldLayout = atlasInitialized
        ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        : VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = atlasImage;
    toTransfer.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
        &toTransfer);

    offset = 0;
    for (const auto& upload : pendingUploads) {
        VkBufferImageCopy copy {};
        copy.bufferOffset = offset;
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageOffset = { upload.x, upload.y, 0 };
        copy.imageExtent = { static_cast<std::uint32_t>(upload.width),
            static_cast<std::uint32_t>(upload.height), 1 };
        vkCmdCopyBufferToImage(commandBuffer, staging.buffer, atlasImage,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        offset += static_cast<VkDeviceSize>(upload.pixels.size());
    }

    VkImageMemoryBarrier toShaderRead = toTransfer;
    toShaderRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toShaderRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toShaderRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toShaderRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
        &toShaderRead);

    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
        fail("vkEndCommandBuffer");
    }
    VkSubmitInfo submit { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commandBuffer;

    // vkQueueWaitIdle has no timeout and cannot fail with VK_TIMEOUT - if the
    // queue is stalled (e.g. present blocked because the window isn't
    // currently visible to the compositor) it blocks forever, freezing the
    // whole single-threaded app. Submit with a dedicated fence and wait on
    // that with a bounded timeout instead, so a stall here just delays this
    // glyph upload rather than hanging the app.
    VkFenceCreateInfo uploadFenceInfo {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
    };
    VkFence uploadFence = VK_NULL_HANDLE;
    if (vkCreateFence(device, &uploadFenceInfo, nullptr, &uploadFence)
        != VK_SUCCESS) {
        fail("vkCreateFence(upload)");
    }

    if (vkQueueSubmit(queue, 1, &submit, uploadFence) != VK_SUCCESS) {
        vkDestroyFence(device, uploadFence, nullptr);
        fail("vkQueueSubmit(upload)");
    }

    constexpr std::uint64_t uploadTimeoutNs = 1'000'000'000ULL; // 1 second
    const VkResult uploadWait =
        vkWaitForFences(device, 1, &uploadFence, VK_TRUE, uploadTimeoutNs);
    vkDestroyFence(device, uploadFence, nullptr);
    if (uploadWait != VK_SUCCESS) {
        // Timed out or failed: the submitted commands may still be in
        // flight on the GPU, so it's not safe to destroy the staging buffer
        // or reset commandBuffer here (that's undefined behavior against a
        // resource still in use). Intentionally leak the staging buffer
        // rather than risk corrupting a live GPU submission; this path
        // should only be hit when the compositor is genuinely not
        // presenting the window, which is rare. Leave pendingUploads
        // intact so the glyph data isn't lost - it'll be retried once the
        // queue is unstuck (subsequent flushUploads() calls will build a
        // fresh staging buffer and commandBuffer state).
        return;
    }

    vkDestroyBuffer(device, staging.buffer, nullptr);
    vkFreeMemory(device, staging.memory, nullptr);
    pendingUploads.clear();
    atlasInitialized = true;
}

Renderer::Renderer(wl_display* display, wl_surface* surface,
    std::uint32_t width, std::uint32_t height)
{
    impl_ = new Impl;
    impl_->display = display;
    impl_->surface = surface;
    impl_->width = width;
    impl_->height = height;

    impl_->createInstance();
    impl_->pickDevice();
    impl_->createAtlas();
    impl_->createSwapchain();
    impl_->createPipeline();
    impl_->createFixedBuffers();
}

Renderer::~Renderer()
{
    if (impl_ == nullptr) {
        return;
    }
    vkDeviceWaitIdle(impl_->device);
    impl_->destroySwapchainObjects();
    vkDestroyFence(impl_->device, impl_->frameFence, nullptr);
    vkDestroySemaphore(impl_->device, impl_->renderFinished, nullptr);
    vkDestroySemaphore(impl_->device, impl_->imageAvailable, nullptr);
    vkDestroyCommandPool(impl_->device, impl_->commandPool, nullptr);
    vkDestroyDescriptorPool(impl_->device, impl_->descriptorPool, nullptr);
    vkDestroyPipeline(impl_->device, impl_->pipeline, nullptr);
    vkDestroyPipelineLayout(impl_->device, impl_->pipelineLayout, nullptr);
    vkDestroyDescriptorSetLayout(impl_->device, impl_->setLayout, nullptr);
    vkDestroyBuffer(impl_->device, impl_->instanceBuffer, nullptr);
    vkFreeMemory(impl_->device, impl_->instanceMemory, nullptr);
    vkDestroyBuffer(impl_->device, impl_->indexBuffer, nullptr);
    vkFreeMemory(impl_->device, impl_->indexMemory, nullptr);
    vkDestroyBuffer(impl_->device, impl_->cornerBuffer, nullptr);
    vkFreeMemory(impl_->device, impl_->cornerMemory, nullptr);
    vkDestroySampler(impl_->device, impl_->atlasSampler, nullptr);
    vkDestroyImageView(impl_->device, impl_->atlasView, nullptr);
    vkFreeMemory(impl_->device, impl_->atlasMemory, nullptr);
    vkDestroyImage(impl_->device, impl_->atlasImage, nullptr);
    vkDestroyDevice(impl_->device, nullptr);
    vkDestroySurfaceKHR(impl_->instance, impl_->surfaceHandle, nullptr);
    vkDestroyInstance(impl_->instance, nullptr);
    delete impl_;
}

void Renderer::resize(std::uint32_t width, std::uint32_t height)
{
    if (impl_->width != width || impl_->height != height) {
        impl_->width = width;
        impl_->height = height;
        impl_->swapchainDirty = true;
    }
}

Renderer::AtlasRegion Renderer::allocateAtlas(int glyphWidth, int glyphHeight)
{
    const int paddedWidth = glyphWidth + 2;
    const int paddedHeight = glyphHeight + 2;
    if (impl_->atlasCursorX + paddedWidth > atlasSize) {
        impl_->atlasCursorX = 0;
        impl_->atlasCursorY += impl_->atlasRowHeight + 2;
        impl_->atlasRowHeight = 0;
    }
    if (impl_->atlasCursorY + paddedHeight > atlasSize) {
        throw std::runtime_error("glyph atlas exhausted");
    }
    AtlasRegion region;
    region.x = impl_->atlasCursorX + 1;
    region.y = impl_->atlasCursorY + 1;
    region.u0 = static_cast<float>(region.x) / atlasSize;
    region.v0 = static_cast<float>(region.y) / atlasSize;
    region.u1 = static_cast<float>(region.x + glyphWidth) / atlasSize;
    region.v1 = static_cast<float>(region.y + glyphHeight) / atlasSize;
    impl_->atlasCursorX += paddedWidth;
    impl_->atlasRowHeight = std::max(impl_->atlasRowHeight, paddedHeight);
    return region;
}

void Renderer::uploadAtlas(const AtlasRegion& region, int width, int height,
    const std::uint8_t* alphaPixels)
{
    Impl::PendingUpload upload;
    upload.x = region.x;
    upload.y = region.y;
    upload.width = width;
    upload.height = height;
    upload.pixels.assign(alphaPixels,
        alphaPixels + static_cast<std::size_t>(width) * height);
    impl_->pendingUploads.push_back(std::move(upload));
}

bool Renderer::render(std::vector<Instance> instances)
{
    if ((impl_->extent.width == 0 || impl_->extent.height == 0
            || impl_->swapchainDirty)
        && impl_->swapchain != nullptr) {
        impl_->destroySwapchainObjects();
        impl_->extent = { 0, 0 };
    }
    if (impl_->swapchain == nullptr) {
        impl_->createSwapchain();
        if (impl_->swapchain == nullptr || impl_->extent.width == 0
            || impl_->extent.height == 0) {
            return false;
        }
    }

    impl_->flushUploads();

    // Bounded timeout instead of UINT64_MAX: under FIFO present mode these
    // calls only get signaled once the compositor actually consumes a
    // presented frame. If the window is minimized, occluded, or the
    // compositor briefly stalls, an infinite wait here blocks the whole
    // single-threaded app - including the Wayland dispatch loop that answers
    // the compositor's responsiveness ping - which is what produces the
    // "not responding" prompt. Timing out just skips this frame; we retry
    // on the next loop iteration once dispatch has had a chance to run.
    constexpr std::uint64_t frameTimeoutNs = 1'000'000'000ULL; // 1 second

    const VkResult waitResult = vkWaitForFences(
        impl_->device, 1, &impl_->frameFence, VK_TRUE, frameTimeoutNs);
    if (waitResult == VK_TIMEOUT) {
        return false;
    }
    if (waitResult != VK_SUCCESS) {
        return false;
    }

    std::uint32_t imageIndex = 0;
    const VkResult acquire = vkAcquireNextImageKHR(impl_->device,
        impl_->swapchain, frameTimeoutNs, impl_->imageAvailable, nullptr,
        &imageIndex);
    if (acquire == VK_TIMEOUT) {
        return false;
    }
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
        std::fprintf(stderr, "bang: vkAcquireNextImageKHR=%d\n", (int)acquire);
        impl_->swapchainDirty = true;
        return false;
    }

    vkResetCommandBuffer(impl_->commandBuffer, 0);
    VkCommandBufferBeginInfo beginInfo {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO
    };
    if (vkBeginCommandBuffer(impl_->commandBuffer, &beginInfo) != VK_SUCCESS) {
        return false;
    }

    VkClearValue clearValue {};
    clearValue.color.float32[0] = 0.055f;
    clearValue.color.float32[1] = 0.063f;
    clearValue.color.float32[2] = 0.086f;
    clearValue.color.float32[3] = 1.0f;

    VkRenderingAttachmentInfo attachment {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO
    };
    attachment.imageView = impl_->swapchainViews[imageIndex];
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.clearValue = clearValue;

    const VkRect2D renderArea { .offset = { 0, 0 }, .extent = impl_->extent };

    VkRenderingInfo renderingInfo { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO };
    renderingInfo.renderArea = renderArea;
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &attachment;

    VkImageMemoryBarrier toAttachment { .sType =
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    toAttachment.srcAccessMask = 0;
    toAttachment.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toAttachment.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toAttachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toAttachment.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toAttachment.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toAttachment.image = impl_->swapchainImages[imageIndex];
    toAttachment.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(impl_->commandBuffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0,
        nullptr, 1, &toAttachment);

    vkCmdBeginRendering(impl_->commandBuffer, &renderingInfo);

    const VkViewport viewport { 0.0f, 0.0f,
        static_cast<float>(impl_->extent.width),
        static_cast<float>(impl_->extent.height), 0.0f, 1.0f };
    vkCmdSetViewport(impl_->commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(impl_->commandBuffer, 0, 1, &renderArea);

    const float pushConstants[2] = {
        static_cast<float>(impl_->extent.width),
        static_cast<float>(impl_->extent.height),
    };
    vkCmdPushConstants(impl_->commandBuffer, impl_->pipelineLayout,
        VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pushConstants), pushConstants);

    vkCmdBindPipeline(impl_->commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
        impl_->pipeline);
    vkCmdBindDescriptorSets(impl_->commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
        impl_->pipelineLayout, 0, 1, &impl_->descriptorSet, 0, nullptr);

    if (!instances.empty()) {
        impl_->ensureInstanceCapacity(instances.size());
        void* target = nullptr;
        if (vkMapMemory(impl_->device, impl_->instanceMemory, 0,
                instances.size() * sizeof(Instance), 0, &target)
            == VK_SUCCESS) {
            std::memcpy(target, instances.data(),
                instances.size() * sizeof(Instance));
            vkUnmapMemory(impl_->device, impl_->instanceMemory);
        }

        const VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(impl_->commandBuffer, 0, 1, &impl_->cornerBuffer,
            &zero);
        vkCmdBindVertexBuffers(impl_->commandBuffer, 1, 1,
            &impl_->instanceBuffer, &zero);
        vkCmdBindIndexBuffer(impl_->commandBuffer, impl_->indexBuffer, 0,
            VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(impl_->commandBuffer, 6,
            static_cast<std::uint32_t>(instances.size()), 0, 0, 0);
    }

    vkCmdEndRendering(impl_->commandBuffer);

    VkImageMemoryBarrier toPresent { .sType =
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    toPresent.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toPresent.dstAccessMask = 0;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toPresent.image = impl_->swapchainImages[imageIndex];
    toPresent.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(impl_->commandBuffer,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
        &toPresent);

    const VkResult endResult = vkEndCommandBuffer(impl_->commandBuffer);
    if (endResult != VK_SUCCESS) {
        std::fprintf(stderr, "bang: vkEndCommandBuffer=%d\n", (int)endResult);
        return false;
    }
    const VkResult resetResult = vkResetFences(impl_->device, 1, &impl_->frameFence);
    if (resetResult != VK_SUCCESS) {
        std::fprintf(stderr, "bang: vkResetFences=%d\n", (int)resetResult);
        return false;
    }

    VkSubmitInfo submit { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
    const VkPipelineStageFlags waitStage =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &impl_->imageAvailable;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &impl_->commandBuffer;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &impl_->renderFinished;
    const VkResult submitResult = vkQueueSubmit(impl_->queue, 1, &submit, impl_->frameFence);
    if (submitResult != VK_SUCCESS) {
        std::fprintf(stderr, "bang: vkQueueSubmit=%d\n", (int)submitResult);
        return false;
    }

    VkPresentInfoKHR present { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &impl_->renderFinished;
    present.swapchainCount = 1;
    present.pSwapchains = &impl_->swapchain;
    present.pImageIndices = &imageIndex;
    const VkResult presentResult = vkQueuePresentKHR(impl_->queue, &present);
    if (presentResult != VK_SUCCESS) {
        std::fprintf(stderr, "bang: vkQueuePresentKHR=%d\n",
            static_cast<int>(presentResult));
    }
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR) {
        impl_->swapchainDirty = true;
    }
    return presentResult == VK_SUCCESS || presentResult == VK_SUBOPTIMAL_KHR;
}

} // namespace bang::render
