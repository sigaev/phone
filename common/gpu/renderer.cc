#include "common/gpu/renderer.h"

#define VK_USE_PLATFORM_ANDROID_KHR
#include <android/log.h>
#include <android/native_window.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#if defined(NATIVE_BUTTONS_VULKAN_VALIDATION)
#include <atomic>
#endif
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <vector>
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

namespace gpu {
using common::Error;
using common::Owner;
using common::Result;

namespace {
#if defined(NATIVE_BUTTONS_VULKAN_VALIDATION)
std::atomic<unsigned> validation_errors{0};
VKAPI_ATTR VkBool32 VKAPI_CALL validation_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                  VkDebugUtilsMessageTypeFlagsEXT,
                                                  const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                  void*) {
    std::fprintf(stderr, "Vulkan validation: %s\n", data->pMessage);
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        ++validation_errors;
    return VK_FALSE;
}
#endif
constexpr int kAtlasSize = 1024;
constexpr unsigned kFrameCount = 2;
constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr std::uint64_t kWaitForever = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint64_t kFrameTimeout = 1000000000;
constexpr std::uint32_t kFullVertex[] =
#include "common/gpu/full_vert.inc"
    ;
constexpr std::uint32_t kBlurFragment[] =
#include "common/gpu/blur_frag.inc"
    ;
constexpr std::uint32_t kPostFragment[] =
#include "common/gpu/post_frag.inc"
    ;
constexpr std::uint32_t kParticlesCompute[] =
#include "common/gpu/particles_comp.inc"
    ;
constexpr std::uint32_t kParticleVertex[] =
#include "common/gpu/particle_vert.inc"
    ;
constexpr std::uint32_t kParticleFragment[] =
#include "common/gpu/particle_frag.inc"
    ;
constexpr std::uint32_t kUiVertex[] =
#include "common/gpu/ui_vert.inc"
    ;
constexpr std::uint32_t kUiFragment[] =
#include "common/gpu/ui_frag.inc"
    ;

struct Buffer {
    VkDevice device = VK_NULL_HANDLE;
    VkBuffer handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
};
void destroy(Buffer* buffer) noexcept {
    if (!buffer)
        return;
    if (buffer->mapped)
        vkUnmapMemory(buffer->device, buffer->memory);
    vkDestroyBuffer(buffer->device, buffer->handle, nullptr);
    vkFreeMemory(buffer->device, buffer->memory, nullptr);
    delete buffer;
}
struct Image {
    VkDevice device = VK_NULL_HANDLE;
    VkImage handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};
void destroy(Image* image) noexcept {
    if (!image)
        return;
    vkDestroyImageView(image->device, image->view, nullptr);
    vkDestroyImage(image->device, image->handle, nullptr);
    vkFreeMemory(image->device, image->memory, nullptr);
    delete image;
}
struct Instance {
    Mat4 model;
    Color color;
    float material[4];
};
struct Mesh {
    Owner<Buffer> vertices, indices;
    unsigned index_count = 0;
    VkDeviceSize instance_offset = 0;
    std::vector<Instance> items;
};
struct UiVertex {
    float x, y, u, v;
    Color color;
};
struct Glyph {
    float x0, y0, x1, y1, xoff, yoff, advance;
};
struct Globals {
    Mat4 view, light;
    Color eye_time, size, animation_clock;
};
enum class Set : unsigned { kScene, kHdr, kBloom0, kBloom1, kPost, kUi, kCount };
struct Frame {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkFence acquisition = VK_NULL_HANDLE;
    Owner<Buffer> globals, instances, ui;
    VkDescriptorSet sets[static_cast<unsigned>(Set::kCount)]{};
    bool submitted = false, in_flight = false, acquiring = false;
};
struct SurfaceImage {
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkSemaphore ready = VK_NULL_HANDLE;
    VkFence presented = VK_NULL_HANDLE;
    bool present_pending = false;
};
}  // namespace

struct Renderer {
#if defined(NATIVE_BUTTONS_VULKAN_VALIDATION)
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
#endif
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory{};
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    unsigned queue_family = 0, timestamp_bits = 0;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    ANativeWindow* window = nullptr;
    VkFormat output_format = VK_FORMAT_R8G8B8A8_UNORM, depth_format = VK_FORMAT_D24_UNORM_S8_UINT;
    VkColorSpaceKHR color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkSurfaceTransformFlagBitsKHR surface_transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptors = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE, shadow_sampler = VK_NULL_HANDLE;
    VkFilter shadow_filter = VK_FILTER_NEAREST;
    VkRenderPass shadow_pass = VK_NULL_HANDLE, scene_pass = VK_NULL_HANDLE,
                 bloom_pass = VK_NULL_HANDLE, output_pass = VK_NULL_HANDLE;
    VkPipeline mesh_pipeline = VK_NULL_HANDLE, shadow_pipeline = VK_NULL_HANDLE,
               sky_pipeline = VK_NULL_HANDLE, particle_pipeline = VK_NULL_HANDLE,
               compute_pipeline = VK_NULL_HANDLE, blur_pipeline = VK_NULL_HANDLE,
               post_pipeline = VK_NULL_HANDLE, ui_pipeline = VK_NULL_HANDLE;
    Owner<Image> hdr, ms_color, ms_depth, shadow, bloom[2], output, font;
    Owner<Buffer> particles;
    VkFramebuffer hdr_framebuffer = VK_NULL_HANDLE, shadow_framebuffer = VK_NULL_HANDLE,
                  bloom_framebuffers[2]{}, output_framebuffer = VK_NULL_HANDLE;
    VkQueryPool queries = VK_NULL_HANDLE;
    std::array<Frame, kFrameCount> frames;
    std::vector<SurfaceImage> surface_images;
    unsigned frame_index = 0, image_index = 0, last_frame = 0, triangle_count = 0;
    int width = 0, height = 0, window_width = 0, window_height = 0, render_width = 0,
        render_height = 0, shadow_size = 0, particle_count = 0;
    int observed_window_width = 0, observed_window_height = 0;
    bool maximum = false, recreate_surface = false, targets_ready = false, has_frame = false;
    float gpu_millis = 0;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_4_BIT;
    std::vector<Mesh> meshes;
    std::vector<UiVertex> ui;
    Glyph glyphs[96]{};
    Mat4 model_transform;
};

namespace {
std::unexpected<Error> fail(const char* message, VkResult result = VK_SUCCESS) {
    char text[512];
    if (result == VK_SUCCESS)
        std::snprintf(text, sizeof(text), "%s", message);
    else
        std::snprintf(text, sizeof(text), "%s (VkResult %d)", message, static_cast<int>(result));
    __android_log_print(ANDROID_LOG_ERROR, "native_buttons", "%s", text);
    return std::unexpected(Error{text});
}
#define VK_CHECK(call)                  \
    do {                                \
        const VkResult result = (call); \
        if (result != VK_SUCCESS)       \
            return fail(#call, result); \
    } while (false)

Result<unsigned> memory_type(const Renderer& r, unsigned bits, VkMemoryPropertyFlags flags) {
    for (unsigned i = 0; i < r.memory.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (r.memory.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    return fail("No compatible Vulkan memory type");
}
Result<Owner<Buffer>> create_buffer(Renderer& r, VkDeviceSize size, VkBufferUsageFlags usage,
                                    bool host = true) {
    Owner<Buffer> buffer(new (std::nothrow) Buffer);
    if (!buffer)
        return fail("Cannot allocate buffer state");
    buffer->device = r.device;
    buffer->size = size;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    VK_CHECK(vkCreateBuffer(r.device, &info, nullptr, &buffer->handle));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(r.device, buffer->handle, &requirements);
    auto type = memory_type(
        r, requirements.memoryTypeBits,
        host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
             : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!type)
        return std::unexpected(type.error());
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = *type;
    VK_CHECK(vkAllocateMemory(r.device, &allocate, nullptr, &buffer->memory));
    VK_CHECK(vkBindBufferMemory(r.device, buffer->handle, buffer->memory, 0));
    if (host)
        VK_CHECK(vkMapMemory(r.device, buffer->memory, 0, VK_WHOLE_SIZE, 0, &buffer->mapped));
    return buffer;
}
Result<void> reserve_buffer(Renderer& r, Owner<Buffer>& buffer, VkDeviceSize size,
                            VkBufferUsageFlags usage) {
    if (buffer && buffer->size >= size)
        return {};
    auto created = create_buffer(r, std::max<VkDeviceSize>(size, 4096), usage);
    if (!created)
        return std::unexpected(created.error());
    buffer = std::move(*created);
    return {};
}
Result<Owner<Image>> create_image(Renderer& r, int width, int height, VkFormat format,
                                  VkImageUsageFlags usage, VkImageAspectFlags aspect,
                                  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT) {
    Owner<Image> image(new (std::nothrow) Image);
    if (!image)
        return fail("Cannot allocate image state");
    image->device = r.device;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {static_cast<unsigned>(width), static_cast<unsigned>(height), 1};
    info.mipLevels = info.arrayLayers = 1;
    info.samples = samples;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    VK_CHECK(vkCreateImage(r.device, &info, nullptr, &image->handle));
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(r.device, image->handle, &requirements);
    auto type = memory_type(r, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!type)
        return std::unexpected(type.error());
    if (usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT)
        for (unsigned i = 0; i < r.memory.memoryTypeCount; ++i)
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (r.memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)) {
                type = i;
                break;
            }
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = *type;
    VK_CHECK(vkAllocateMemory(r.device, &allocate, nullptr, &image->memory));
    VK_CHECK(vkBindImageMemory(r.device, image->handle, image->memory, 0));
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = image->handle;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {aspect, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(r.device, &view, nullptr, &image->view));
    return image;
}
Result<void> begin_commands(Renderer& r, Frame& frame) {
    VK_CHECK(vkResetCommandPool(r.device, frame.pool, 0));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(frame.command, &begin));
    return {};
}
// Every queue submission and acquisition has a fence. Present fences cover the
// presentation engine too; device/queue idle alone cannot release its resources.
VkResult wait_for_work(Renderer& r) {
    std::vector<VkFence> fences;
    for (auto& frame : r.frames) {
        if (frame.in_flight)
            fences.push_back(frame.fence);
        if (frame.acquiring)
            fences.push_back(frame.acquisition);
    }
    for (auto& image : r.surface_images)
        if (image.present_pending)
            fences.push_back(image.presented);
    VkResult result = fences.empty() ? VK_SUCCESS
                                     : vkWaitForFences(r.device, fences.size(), fences.data(),
                                                       VK_TRUE, kFrameTimeout);
    if (result == VK_SUCCESS || result == VK_ERROR_DEVICE_LOST) {
        for (auto& frame : r.frames)
            frame.in_flight = frame.acquiring = false;
        for (auto& image : r.surface_images)
            image.present_pending = false;
    }
    return result;
}
Result<void> submit_immediate(Renderer& r, Frame& frame) {
    VK_CHECK(vkEndCommandBuffer(frame.command));
    VK_CHECK(vkResetFences(r.device, 1, &frame.fence));
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &frame.command;
    VK_CHECK(vkQueueSubmit(r.queue, 1, &submit, frame.fence));
    frame.in_flight = true;
    auto result = vkWaitForFences(r.device, 1, &frame.fence, VK_TRUE, kFrameTimeout);
    if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST) {
        // Callers own temporary staging buffers. They cannot unwind into cleanup
        // while this submission still uses those buffers.
        (void)fail("GPU transfer did not finish before its deadline", result);
        std::abort();
    }
    frame.in_flight = false;
    if (result != VK_SUCCESS)
        return fail("GPU transfer failed", result);
    return {};
}
void image_barrier(VkCommandBuffer command, VkImage image, VkImageLayout old_layout,
                   VkImageLayout new_layout, VkAccessFlags source, VkAccessFlags destination,
                   VkPipelineStageFlags source_stage, VkPipelineStageFlags destination_stage) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = source;
    barrier.dstAccessMask = destination;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(command, source_stage, destination_stage, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);
}
Result<void> create_context(Renderer& r) {
    std::vector<const char*> instance_extensions;
    if (r.window) {
        unsigned count = 0;
        VK_CHECK(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr));
        std::vector<VkExtensionProperties> available(count);
        VK_CHECK(vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data()));
        for (const char* name :
             {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
              VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
              VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME}) {
            if (std::none_of(available.begin(), available.end(), [&](const auto& extension) {
                    return std::strcmp(extension.extensionName, name) == 0;
                }))
                return fail("Safe window rendering requires Vulkan surface maintenance support");
            instance_extensions.push_back(name);
        }
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "native_buttons";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instance{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance.pApplicationInfo = &app;
#if defined(NATIVE_BUTTONS_VULKAN_VALIDATION)
    const char* layer = "VK_LAYER_KHRONOS_validation";
    instance_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    instance.enabledLayerCount = 1;
    instance.ppEnabledLayerNames = &layer;
    VkDebugUtilsMessengerCreateInfoEXT debug{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debug.pfnUserCallback = validation_message;
    VkValidationFeatureEnableEXT feature =
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validation{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    validation.enabledValidationFeatureCount = 1;
    validation.pEnabledValidationFeatures = &feature;
    debug.pNext = &validation;
    instance.pNext = &debug;
#endif
    instance.enabledExtensionCount = instance_extensions.size();
    instance.ppEnabledExtensionNames = instance_extensions.data();
    VK_CHECK(vkCreateInstance(&instance, nullptr, &r.instance));
#if defined(NATIVE_BUTTONS_VULKAN_VALIDATION)
    debug.pNext = nullptr;
    auto create_messenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(r.instance, "vkCreateDebugUtilsMessengerEXT"));
    if (!create_messenger)
        return fail("Vulkan validation messenger is unavailable");
    VK_CHECK(create_messenger(r.instance, &debug, nullptr, &r.messenger));
#endif
    if (r.window) {
        VkAndroidSurfaceCreateInfoKHR surface{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
        surface.window = r.window;
        VK_CHECK(vkCreateAndroidSurfaceKHR(r.instance, &surface, nullptr, &r.surface));
    }
    unsigned count = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(r.instance, &count, nullptr));
    std::vector<VkPhysicalDevice> devices(count);
    VK_CHECK(vkEnumeratePhysicalDevices(r.instance, &count, devices.data()));
    VkPhysicalDeviceFeatures features{};
    for (auto physical : devices) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(physical, &properties);
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ||
            properties.apiVersion < VK_API_VERSION_1_1)
            continue;
        unsigned family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families.data());
        for (unsigned i = 0; i < family_count; ++i) {
            constexpr auto kFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
            if ((families[i].queueFlags & kFlags) != kFlags)
                continue;
            VkBool32 present = VK_TRUE;
            if (r.surface)
                VK_CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, r.surface, &present));
            if (!present)
                continue;
            r.physical = physical;
            r.queue_family = i;
            r.timestamp_bits = families[i].timestampValidBits;
            r.properties = properties;
            break;
        }
        if (r.physical)
            break;
    }
    if (!r.physical)
        return fail("A hardware Vulkan 1.1 graphics/compute device is required");
    vkGetPhysicalDeviceFeatures(r.physical, &features);
    if (!features.largePoints)
        return fail("The particle renderer requires large points");
    vkGetPhysicalDeviceMemoryProperties(r.physical, &r.memory);
    VkImageFormatProperties hdr_properties{};
    VK_CHECK(vkGetPhysicalDeviceImageFormatProperties(
        r.physical, kHdrFormat, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0, &hdr_properties));
    if (!(hdr_properties.sampleCounts & VK_SAMPLE_COUNT_4_BIT))
        return fail("4x HDR multisampling is required");
    bool depth_found = false;
    for (auto format : {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM,
                        VK_FORMAT_D32_SFLOAT_S8_UINT}) {
        VkImageFormatProperties attachment{}, shadow{};
        auto attachment_result = vkGetPhysicalDeviceImageFormatProperties(
            r.physical, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
            0, &attachment);
        auto shadow_result = vkGetPhysicalDeviceImageFormatProperties(
            r.physical, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, &shadow);
        if (attachment_result != VK_SUCCESS || shadow_result != VK_SUCCESS ||
            !(attachment.sampleCounts & VK_SAMPLE_COUNT_4_BIT) ||
            !(shadow.sampleCounts & VK_SAMPLE_COUNT_1_BIT))
            continue;
        VkFormatProperties properties;
        vkGetPhysicalDeviceFormatProperties(r.physical, format, &properties);
        r.depth_format = format;
        r.shadow_filter =
            properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT
                ? VK_FILTER_LINEAR
                : VK_FILTER_NEAREST;
        depth_found = true;
        break;
    }
    if (!depth_found)
        return fail("A sampled depth format with 4x attachment support is required");
    VkFormatProperties format_properties;
    vkGetPhysicalDeviceFormatProperties(r.physical, kHdrFormat, &format_properties);
    constexpr auto kHdrFeatures = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
                                  VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if ((format_properties.optimalTilingFeatures & kHdrFeatures) != kHdrFeatures)
        return fail("HDR blending and filtering are required");
    float priority = 1;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = r.queue_family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkPhysicalDeviceFeatures enabled{};
    enabled.largePoints = VK_TRUE;
    const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                                VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME};
    VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};
    VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device.queueCreateInfoCount = 1;
    device.pQueueCreateInfos = &queue;
    device.pEnabledFeatures = &enabled;
    if (r.surface) {
        VK_CHECK(vkEnumerateDeviceExtensionProperties(r.physical, nullptr, &count, nullptr));
        std::vector<VkExtensionProperties> available(count);
        VK_CHECK(
            vkEnumerateDeviceExtensionProperties(r.physical, nullptr, &count, available.data()));
        for (const char* name : extensions)
            if (std::none_of(available.begin(), available.end(), [&](const auto& extension) {
                    return std::strcmp(extension.extensionName, name) == 0;
                }))
                return fail("Safe window rendering requires Vulkan swapchain presentation fences");
        VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        supported.pNext = &maintenance;
        auto get_features = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
            vkGetInstanceProcAddr(r.instance, "vkGetPhysicalDeviceFeatures2"));
        if (!get_features)
            return fail("Vulkan 1.1 feature queries are unavailable");
        get_features(r.physical, &supported);
        if (!maintenance.swapchainMaintenance1)
            return fail("The Vulkan driver does not support swapchain presentation fences");
        device.pNext = &maintenance;
        device.enabledExtensionCount = std::size(extensions);
        device.ppEnabledExtensionNames = extensions;
    }
    VK_CHECK(vkCreateDevice(r.physical, &device, nullptr, &r.device));
    vkGetDeviceQueue(r.device, r.queue_family, 0, &r.queue);
    if (r.surface) {
        VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(r.physical, r.surface, &count, nullptr));
        std::vector<VkSurfaceFormatKHR> formats(count);
        VK_CHECK(
            vkGetPhysicalDeviceSurfaceFormatsKHR(r.physical, r.surface, &count, formats.data()));
        bool found = false;
        for (auto format : formats)
            if ((format.format == VK_FORMAT_R8G8B8A8_UNORM ||
                 format.format == VK_FORMAT_B8G8R8A8_UNORM) &&
                format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                r.output_format = format.format;
                r.color_space = format.colorSpace;
                found = true;
                break;
            }
        if (!found)
            return fail("An eight-bit UNORM Vulkan surface is required");
    }
    for (auto& frame : r.frames) {
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.queueFamilyIndex = r.queue_family;
        VK_CHECK(vkCreateCommandPool(r.device, &pool, nullptr, &frame.pool));
        VkCommandBufferAllocateInfo commands{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commands.commandPool = frame.pool;
        commands.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commands.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(r.device, &commands, &frame.command));
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(r.device, &fence, nullptr, &frame.fence));
        fence.flags = 0;
        VK_CHECK(vkCreateFence(r.device, &fence, nullptr, &frame.acquisition));
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(r.device, &semaphore, nullptr, &frame.acquired));
        if (auto result = reserve_buffer(r, frame.globals, sizeof(Globals),
                                         VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
            !result)
            return result;
    }
    if (r.timestamp_bits && r.properties.limits.timestampComputeAndGraphics) {
        VkQueryPoolCreateInfo queries{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        queries.queryType = VK_QUERY_TYPE_TIMESTAMP;
        queries.queryCount = kFrameCount * 2;
        VK_CHECK(vkCreateQueryPool(r.device, &queries, nullptr, &r.queries));
    }
    __android_log_print(ANDROID_LOG_INFO, "native_buttons", "Vulkan %u.%u on %s",
                        VK_VERSION_MAJOR(r.properties.apiVersion),
                        VK_VERSION_MINOR(r.properties.apiVersion), r.properties.deviceName);
    return {};
}

Result<void> create_descriptors(Renderer& r) {
    VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout.bindingCount = std::size(bindings);
    layout.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(r.device, &layout, nullptr, &r.set_layout));
    VkPushConstantRange push{VK_SHADER_STAGE_ALL, 0, sizeof(Color)};
    VkPipelineLayoutCreateInfo pipeline{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipeline.setLayoutCount = 1;
    pipeline.pSetLayouts = &r.set_layout;
    pipeline.pushConstantRangeCount = 1;
    pipeline.pPushConstantRanges = &push;
    VK_CHECK(vkCreatePipelineLayout(r.device, &pipeline, nullptr, &r.pipeline_layout));
    constexpr unsigned kSetCount = static_cast<unsigned>(Set::kCount);
    constexpr unsigned kTotal = kFrameCount * kSetCount;
    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kTotal},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kTotal * 2},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kTotal},
    };
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = kTotal;
    pool.poolSizeCount = std::size(sizes);
    pool.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(r.device, &pool, nullptr, &r.descriptors));
    std::array<VkDescriptorSetLayout, kSetCount> layouts;
    layouts.fill(r.set_layout);
    for (auto& frame : r.frames) {
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = r.descriptors;
        allocate.descriptorSetCount = kSetCount;
        allocate.pSetLayouts = layouts.data();
        VK_CHECK(vkAllocateDescriptorSets(r.device, &allocate, frame.sets));
    }
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(r.device, &sampler, nullptr, &r.sampler));
    sampler.compareEnable = VK_TRUE;
    sampler.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    sampler.magFilter = sampler.minFilter = r.shadow_filter;
    VK_CHECK(vkCreateSampler(r.device, &sampler, nullptr, &r.shadow_sampler));
    auto particles = create_buffer(
        r, 65536 * sizeof(Color),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, false);
    if (!particles)
        return std::unexpected(particles.error());
    r.particles = std::move(*particles);
    return {};
}

enum class Pass { kShadow, kScene, kBloom, kOutput };
Result<VkRenderPass> create_pass(Renderer& r, Pass type) {
    bool shadow = type == Pass::kShadow, scene = type == Pass::kScene,
         output = type == Pass::kOutput;
    std::array<VkAttachmentDescription, 3> attachments{};
    attachments[0].format = shadow ? r.depth_format : output ? r.output_format : kHdrFormat;
    attachments[0].samples = scene ? r.samples : VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp =
        scene ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = shadow   ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                 : scene  ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                 : output ? (r.window ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                                                      : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
                                          : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    attachments[1] = attachments[0];
    attachments[1].format = r.depth_format;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachments[2] = attachments[0];
    attachments[2].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[2].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth{shadow ? 0u : 1u, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkAttachmentReference resolve{2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    if (!shadow) {
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color;
    }
    if (shadow || scene)
        subpass.pDepthStencilAttachment = &depth;
    if (scene)
        subpass.pResolveAttachments = &resolve;
    constexpr auto kAttachmentStages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                       VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    constexpr auto kAttachmentAccess =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkSubpassDependency dependencies[2]{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask =
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | kAttachmentStages | VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependencies[0].srcAccessMask =
        VK_ACCESS_SHADER_READ_BIT | kAttachmentAccess | VK_ACCESS_TRANSFER_READ_BIT;
    dependencies[0].dstStageMask = kAttachmentStages;
    dependencies[0].dstAccessMask = kAttachmentAccess | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = kAttachmentStages;
    dependencies[1].srcAccessMask = kAttachmentAccess;
    dependencies[1].dstStageMask =
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = scene ? 3 : 1;
    info.pAttachments = attachments.data();
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 2;
    info.pDependencies = dependencies;
    VkRenderPass pass;
    VK_CHECK(vkCreateRenderPass(r.device, &info, nullptr, &pass));
    return pass;
}
Result<VkShaderModule> create_shader(Renderer& r, std::span<const std::uint32_t> code) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = code.size_bytes();
    info.pCode = code.data();
    VkShaderModule module;
    VK_CHECK(vkCreateShaderModule(r.device, &info, nullptr, &module));
    return module;
}
enum class Pipeline { kMesh, kShadow, kSky, kParticle, kBlur, kPost, kUi };
Result<VkPipeline> create_pipeline(Renderer& r, std::span<const std::uint32_t> vertex,
                                   std::span<const std::uint32_t> fragment, Pipeline kind) {
    auto vs = create_shader(r, vertex);
    if (!vs)
        return std::unexpected(vs.error());
    VkShaderModule fs = VK_NULL_HANDLE;
    if (!fragment.empty()) {
        auto result = create_shader(r, fragment);
        if (!result) {
            vkDestroyShaderModule(r.device, *vs, nullptr);
            return std::unexpected(result.error());
        }
        fs = *result;
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = *vs;
    stages[0].pName = stages[1].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    bool mesh = kind == Pipeline::kMesh || kind == Pipeline::kShadow;
    bool shadow = kind == Pipeline::kShadow, particle = kind == Pipeline::kParticle,
         ui = kind == Pipeline::kUi;
    VkVertexInputBindingDescription bindings[2]{};
    VkVertexInputAttributeDescription attributes[8]{};
    VkPipelineVertexInputStateCreateInfo input{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    input.pVertexBindingDescriptions = bindings;
    input.pVertexAttributeDescriptions = attributes;
    if (mesh) {
        bindings[0] = {0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
        bindings[1] = {1, sizeof(Instance), VK_VERTEX_INPUT_RATE_INSTANCE};
        attributes[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
        attributes[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(Vec3)};
        for (unsigned i = 0; i < 6; ++i)
            attributes[i + 2] = {i + 2, 1, VK_FORMAT_R32G32B32A32_SFLOAT, i * 16};
        input.vertexBindingDescriptionCount = 2;
        input.vertexAttributeDescriptionCount = 8;
    } else if (particle) {
        bindings[0] = {0, sizeof(Color), VK_VERTEX_INPUT_RATE_VERTEX};
        attributes[0] = {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
        input.vertexBindingDescriptionCount = input.vertexAttributeDescriptionCount = 1;
    } else if (ui) {
        bindings[0] = {0, sizeof(UiVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        attributes[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
        attributes[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, 8};
        attributes[2] = {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16};
        input.vertexBindingDescriptionCount = 1;
        input.vertexAttributeDescriptionCount = 3;
    }
    VkPipelineInputAssemblyStateCreateInfo assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology =
        particle ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1;
    raster.depthBiasEnable = shadow;
    raster.depthBiasConstantFactor = 3;
    raster.depthBiasSlopeFactor = 2;
    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    bool hdr = kind == Pipeline::kMesh || kind == Pipeline::kSky || particle;
    multisample.rasterizationSamples = hdr ? r.samples : VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = mesh || particle;
    depth.depthWriteEnable = mesh;
    depth.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend.blendEnable = ui || particle;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    // Source-over UI coverage preserves opaque alpha in the final image.
    blend.srcAlphaBlendFactor = ui ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = blend.dstAlphaBlendFactor =
        particle ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = blend.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo blending{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blending.attachmentCount = shadow ? 0 : 1;
    blending.pAttachments = &blend;
    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = fs ? 2 : 1;
    info.pStages = stages;
    info.pVertexInputState = &input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blending;
    info.pDynamicState = &dynamic;
    info.layout = r.pipeline_layout;
    info.renderPass = shadow                    ? r.shadow_pass
                      : hdr                     ? r.scene_pass
                      : kind == Pipeline::kBlur ? r.bloom_pass
                                                : r.output_pass;
    VkPipeline pipeline;
    auto result = vkCreateGraphicsPipelines(r.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
    vkDestroyShaderModule(r.device, *vs, nullptr);
    vkDestroyShaderModule(r.device, fs, nullptr);
    if (result != VK_SUCCESS)
        return fail("Cannot create Vulkan graphics pipeline", result);
    return pipeline;
}
Result<void> create_pipelines(Renderer& r, SceneShaders shaders) {
    struct RenderPassSpec {
        Pass type;
        VkRenderPass* output;
    };
    for (auto spec : {RenderPassSpec{Pass::kShadow, &r.shadow_pass},
                      {Pass::kScene, &r.scene_pass},
                      {Pass::kBloom, &r.bloom_pass},
                      {Pass::kOutput, &r.output_pass}}) {
        auto pass = create_pass(r, spec.type);
        if (!pass)
            return std::unexpected(pass.error());
        *spec.output = *pass;
    }
    struct PipelineSpec {
        Pipeline kind;
        std::span<const std::uint32_t> vertex, fragment;
        VkPipeline* output;
    };
    const PipelineSpec specs[] = {
        {Pipeline::kMesh, shaders.vertex, shaders.fragment, &r.mesh_pipeline},
        {Pipeline::kShadow, shaders.vertex, {}, &r.shadow_pipeline},
        {Pipeline::kSky, kFullVertex, shaders.sky, &r.sky_pipeline},
        {Pipeline::kParticle, kParticleVertex, kParticleFragment, &r.particle_pipeline},
        {Pipeline::kBlur, kFullVertex, kBlurFragment, &r.blur_pipeline},
        {Pipeline::kPost, kFullVertex, kPostFragment, &r.post_pipeline},
        {Pipeline::kUi, kUiVertex, kUiFragment, &r.ui_pipeline},
    };
    for (auto spec : specs) {
        auto pipeline = create_pipeline(r, spec.vertex, spec.fragment, spec.kind);
        if (!pipeline)
            return std::unexpected(pipeline.error());
        *spec.output = *pipeline;
    }
    auto shader = create_shader(r, kParticlesCompute);
    if (!shader)
        return std::unexpected(shader.error());
    VkComputePipelineCreateInfo compute{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    compute.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    compute.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    compute.stage.module = *shader;
    compute.stage.pName = "main";
    compute.layout = r.pipeline_layout;
    auto result = vkCreateComputePipelines(r.device, VK_NULL_HANDLE, 1, &compute, nullptr,
                                           &r.compute_pipeline);
    vkDestroyShaderModule(r.device, *shader, nullptr);
    if (result != VK_SUCCESS)
        return fail("Cannot create Vulkan compute pipeline", result);
    return {};
}

void destroy_targets(Renderer& r) {
    r.targets_ready = false;
    r.has_frame = false;
    vkDestroyFramebuffer(r.device, r.hdr_framebuffer, nullptr);
    vkDestroyFramebuffer(r.device, r.shadow_framebuffer, nullptr);
    vkDestroyFramebuffer(r.device, r.output_framebuffer, nullptr);
    r.hdr_framebuffer = r.shadow_framebuffer = r.output_framebuffer = VK_NULL_HANDLE;
    for (auto& framebuffer : r.bloom_framebuffers) {
        vkDestroyFramebuffer(r.device, framebuffer, nullptr);
        framebuffer = VK_NULL_HANDLE;
    }
    for (auto& image : r.surface_images) {
        vkDestroyFramebuffer(r.device, image.framebuffer, nullptr);
        vkDestroyImageView(r.device, image.view, nullptr);
        vkDestroySemaphore(r.device, image.ready, nullptr);
        vkDestroyFence(r.device, image.presented, nullptr);
    }
    r.surface_images.clear();
    r.hdr.reset();
    r.ms_color.reset();
    r.ms_depth.reset();
    r.shadow.reset();
    r.bloom[0].reset();
    r.bloom[1].reset();
    r.output.reset();
}
Result<VkFramebuffer> create_framebuffer(Renderer& r, VkRenderPass pass,
                                         std::span<const VkImageView> views, int width,
                                         int height) {
    VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    info.renderPass = pass;
    info.attachmentCount = views.size();
    info.pAttachments = views.data();
    info.width = width;
    info.height = height;
    info.layers = 1;
    VkFramebuffer framebuffer;
    VK_CHECK(vkCreateFramebuffer(r.device, &info, nullptr, &framebuffer));
    return framebuffer;
}
VkExtent2D surface_extent(const Renderer& r, const VkSurfaceCapabilitiesKHR& capabilities) {
    if (capabilities.currentExtent.width != std::numeric_limits<unsigned>::max())
        return capabilities.currentExtent;
    int width = ANativeWindow_getWidth(r.window), height = ANativeWindow_getHeight(r.window);
    if (width <= 0 || height <= 0)
        return {};
    return {std::clamp<unsigned>(width, capabilities.minImageExtent.width,
                                 capabilities.maxImageExtent.width),
            std::clamp<unsigned>(height, capabilities.minImageExtent.height,
                                 capabilities.maxImageExtent.height)};
}
Result<bool> create_swapchain(Renderer& r, const VkSurfaceCapabilitiesKHR& capabilities) {
    if (r.width <= 0 || r.height <= 0)
        return false;
    if (!(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
        return fail("Vulkan surface cannot be rendered to");
    unsigned count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount)
        count = std::min(count, capabilities.maxImageCount);
    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = r.surface;
    info.minImageCount = count;
    info.imageFormat = r.output_format;
    info.imageColorSpace = r.color_space;
    info.imageExtent = {static_cast<unsigned>(r.width), static_cast<unsigned>(r.height)};
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    // Render in window coordinates. Android's compositor applies display rotation;
    // claiming currentTransform here would require rotating every output/UI vertex.
    if (!(capabilities.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR))
        return fail("The Vulkan surface does not support window-coordinate presentation");
    info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    for (auto alpha :
         {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
          VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR})
        if (capabilities.supportedCompositeAlpha & alpha) {
            info.compositeAlpha = alpha;
            break;
        }
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    info.oldSwapchain = r.swapchain;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    auto result = vkCreateSwapchainKHR(r.device, &info, nullptr, &swapchain);
    // Passing oldSwapchain retires it even when creation fails. Never retry
    // with that retired handle, and keep partially built targets unready.
    vkDestroySwapchainKHR(r.device, r.swapchain, nullptr);
    r.swapchain = result == VK_SUCCESS ? swapchain : VK_NULL_HANDLE;
    if (result == VK_ERROR_OUT_OF_DATE_KHR)
        return false;
    if (result != VK_SUCCESS)
        return fail("Cannot create the Vulkan swapchain", result);
    VK_CHECK(vkGetSwapchainImagesKHR(r.device, r.swapchain, &count, nullptr));
    std::vector<VkImage> images(count);
    VK_CHECK(vkGetSwapchainImagesKHR(r.device, r.swapchain, &count, images.data()));
    r.surface_images.resize(count);
    for (unsigned i = 0; i < count; ++i) {
        auto& image = r.surface_images[i];
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = images[i];
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = r.output_format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(r.device, &view, nullptr, &image.view));
        auto framebuffer =
            create_framebuffer(r, r.output_pass, {&image.view, 1}, r.width, r.height);
        if (!framebuffer)
            return std::unexpected(framebuffer.error());
        image.framebuffer = *framebuffer;
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(r.device, &semaphore, nullptr, &image.ready));
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VK_CHECK(vkCreateFence(r.device, &fence, nullptr, &image.presented));
    }
    return true;
}
void update_descriptors(Renderer& r) {
    for (auto& frame : r.frames)
        for (unsigned i = 0; i < static_cast<unsigned>(Set::kCount); ++i) {
            auto set = static_cast<Set>(i);
            Image* image = set == Set::kScene    ? r.shadow.get()
                           : set == Set::kBloom0 ? r.bloom[0].get()
                           : set == Set::kBloom1 ? r.bloom[1].get()
                           : set == Set::kUi     ? r.font.get()
                                                 : r.hdr.get();
            VkDescriptorBufferInfo uniform{frame.globals->handle, 0, sizeof(Globals)};
            VkDescriptorBufferInfo particles{r.particles->handle, 0, VK_WHOLE_SIZE};
            VkDescriptorImageInfo first{
                set == Set::kScene ? r.shadow_sampler : r.sampler, image->view,
                set == Set::kScene ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                   : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkDescriptorImageInfo second{r.sampler, r.bloom[1]->view,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet writes[4]{};
            for (unsigned j = 0; j < 4; ++j) {
                writes[j].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[j].dstSet = frame.sets[i];
                writes[j].dstBinding = j;
                writes[j].descriptorCount = 1;
            }
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[0].pBufferInfo = &uniform;
            writes[1].descriptorType = writes[2].descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[1].pImageInfo = &first;
            writes[2].pImageInfo = &second;
            writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[3].pBufferInfo = &particles;
            vkUpdateDescriptorSets(r.device, 4, writes, 0, nullptr);
        }
}
Result<bool> ensure_targets(Renderer& r, bool maximum) {
    int width = r.width, height = r.height;
    VkSurfaceCapabilitiesKHR capabilities{};
    if (r.window) {
        // Allocate to the extent advertised by Vulkan. Android can cache this
        // until presentation; prepare_frame also observes the independent native
        // window size so an idle resize can trigger the frame that refreshes it.
        VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(r.physical, r.surface, &capabilities));
        auto extent = surface_extent(r, capabilities);
        width = extent.width;
        height = extent.height;
    }
    if (width <= 0 || height <= 0) {
        if (r.window) {
            r.recreate_surface = true;
            return false;
        }
        return fail("Vulkan surface has no size");
    }
    if (r.targets_ready && r.maximum == maximum && !r.recreate_surface && width == r.window_width &&
        height == r.window_height &&
        (!r.window || r.surface_transform == capabilities.currentTransform))
        return true;
    VK_CHECK(wait_for_work(r));
    destroy_targets(r);
    r.width = r.window_width = width;
    r.height = r.window_height = height;
    r.maximum = maximum;
    if (r.window) {
        r.surface_transform = capabilities.currentTransform;
        if (auto result = create_swapchain(r, capabilities); !result || !*result)
            return result;
    }
    float scale = maximum ? 1.30f : 1.f;
    r.render_width = int(r.width * scale);
    r.render_height = int(r.height * scale);
    r.shadow_size = maximum ? 4096 : 2048;
    r.particle_count = maximum ? 65536 : 16384;
    if (std::max({r.render_width, r.render_height, r.shadow_size}) >
        static_cast<int>(r.properties.limits.maxImageDimension2D))
        return fail("Requested image size exceeds the GPU limit");
    struct ImageSpec {
        Owner<Image>* target;
        int width, height;
        VkFormat format;
        VkImageUsageFlags usage;
        VkImageAspectFlags aspect;
        VkSampleCountFlagBits samples;
    };
    constexpr auto kColorUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    constexpr auto kDepthUsage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    constexpr auto kTransient = VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    int bw = std::max(1, r.render_width / 4), bh = std::max(1, r.render_height / 4);
    const ImageSpec specs[] = {
        {&r.hdr, r.render_width, r.render_height, kHdrFormat, kColorUsage,
         VK_IMAGE_ASPECT_COLOR_BIT, VK_SAMPLE_COUNT_1_BIT},
        {&r.ms_color, r.render_width, r.render_height, kHdrFormat,
         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | kTransient, VK_IMAGE_ASPECT_COLOR_BIT, r.samples},
        {&r.ms_depth, r.render_width, r.render_height, r.depth_format, kDepthUsage | kTransient,
         VK_IMAGE_ASPECT_DEPTH_BIT, r.samples},
        {&r.shadow, r.shadow_size, r.shadow_size, r.depth_format,
         kDepthUsage | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
         VK_SAMPLE_COUNT_1_BIT},
        {&r.bloom[0], bw, bh, kHdrFormat, kColorUsage, VK_IMAGE_ASPECT_COLOR_BIT,
         VK_SAMPLE_COUNT_1_BIT},
        {&r.bloom[1], bw, bh, kHdrFormat, kColorUsage, VK_IMAGE_ASPECT_COLOR_BIT,
         VK_SAMPLE_COUNT_1_BIT},
    };
    for (auto spec : specs) {
        auto image = create_image(r, spec.width, spec.height, spec.format, spec.usage, spec.aspect,
                                  spec.samples);
        if (!image)
            return std::unexpected(image.error());
        *spec.target = std::move(*image);
    }
    VkImageView scene_views[] = {r.ms_color->view, r.ms_depth->view, r.hdr->view};
    auto scene = create_framebuffer(r, r.scene_pass, scene_views, r.render_width, r.render_height);
    if (!scene)
        return std::unexpected(scene.error());
    r.hdr_framebuffer = *scene;
    auto shadow =
        create_framebuffer(r, r.shadow_pass, {&r.shadow->view, 1}, r.shadow_size, r.shadow_size);
    if (!shadow)
        return std::unexpected(shadow.error());
    r.shadow_framebuffer = *shadow;
    for (int i = 0; i < 2; ++i) {
        auto framebuffer = create_framebuffer(r, r.bloom_pass, {&r.bloom[i]->view, 1}, bw, bh);
        if (!framebuffer)
            return std::unexpected(framebuffer.error());
        r.bloom_framebuffers[i] = *framebuffer;
    }
    if (!r.window) {
        auto image =
            create_image(r, r.width, r.height, r.output_format,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT);
        if (!image)
            return std::unexpected(image.error());
        r.output = std::move(*image);
        auto framebuffer =
            create_framebuffer(r, r.output_pass, {&r.output->view, 1}, r.width, r.height);
        if (!framebuffer)
            return std::unexpected(framebuffer.error());
        r.output_framebuffer = *framebuffer;
    }
    update_descriptors(r);
    r.targets_ready = true;
    r.recreate_surface = false;
    return true;
}

Result<void> create_font(Renderer& renderer) {
    const char* paths[] = {"/system/fonts/RobotoStatic-Regular.ttf",
                           "/system/fonts/Roboto-Regular.ttf"};
    std::vector<unsigned char> data;
    for (const char* path : paths) {
        FILE* f = std::fopen(path, "rb");
        if (!f)
            continue;
        std::fseek(f, 0, SEEK_END);
        long n = std::ftell(f);
        std::rewind(f);
        if (n > 0 && n < 16000000) {
            data.resize(n);
            if (std::fread(data.data(), 1, n, f) != size_t(n))
                data.clear();
        }
        std::fclose(f);
        if (!data.empty())
            break;
    }
    if (data.empty())
        return fail("Cannot load the system font");
    std::vector<unsigned char> bitmap(kAtlasSize * kAtlasSize);
    stbtt_bakedchar baked[96];
    if (stbtt_BakeFontBitmap(data.data(), 0, 48, bitmap.data(), kAtlasSize, kAtlasSize, 32, 96,
                             baked) <= 0)
        return fail("Font atlas overflow");
    for (int i = 0; i < 96; ++i) {
        auto& b = baked[i];
        renderer.glyphs[i] = {float(b.x0), float(b.y0), float(b.x1), float(b.y1),
                              b.xoff,      b.yoff,      b.xadvance};
    }
    // A white texel is shared by the solid UI geometry.
    bitmap[0] = bitmap[1] = bitmap[kAtlasSize] = bitmap[kAtlasSize + 1] = 255;
    auto image = create_image(renderer, kAtlasSize, kAtlasSize, VK_FORMAT_R8_UNORM,
                              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              VK_IMAGE_ASPECT_COLOR_BIT);
    if (!image)
        return std::unexpected(image.error());
    renderer.font = std::move(*image);
    auto staging = create_buffer(renderer, bitmap.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    if (!staging)
        return std::unexpected(staging.error());
    std::memcpy((*staging)->mapped, bitmap.data(), bitmap.size());
    auto& frame = renderer.frames[0];
    if (auto result = begin_commands(renderer, frame); !result)
        return result;
    image_barrier(frame.command, renderer.font->handle, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {kAtlasSize, kAtlasSize, 1};
    vkCmdCopyBufferToImage(frame.command, (*staging)->handle, renderer.font->handle,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    image_barrier(frame.command, renderer.font->handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    return submit_immediate(renderer, frame);
}

Result<void> create_geometry(Renderer& renderer) {
    for (int type = 0; type < static_cast<int>(Shape::kCount); ++type) {
        std::vector<Vertex> vertices;
        std::vector<unsigned> indices;
        int rows = 40, cols = 64;
        Shape shape = static_cast<Shape>(type);
        if (shape == Shape::kLowSphere) {
            rows = 12;
            cols = 16;
        }
        if (shape == Shape::kFeather) {
            rows = 16;
            cols = 8;
        }
        if (shape == Shape::kTorus)
            rows = 20;
        if (shape == Shape::kCylinder || shape == Shape::kCone)
            rows = 1;
        if (shape == Shape::kPlane) {
            rows = 1;
            cols = 1;
        }
        for (int i = 0; i <= rows; ++i)
            for (int j = 0; j <= cols; ++j) {
                float u = float(j) / cols, v = float(i) / rows, a = u * 2 * kPi, b = v * kPi;
                Vertex vert;
                if (shape == Shape::kSphere || shape == Shape::kLowSphere) {
                    vert.normal = {std::sin(b) * std::cos(a), std::cos(b),
                                   std::sin(b) * std::sin(a)};
                    vert.position = vert.normal;
                } else if (shape == Shape::kFeather) {
                    float x = (u * 2 - 1),
                          profile = std::pow(std::max(0.f, std::sin(v * kPi)), .72f);
                    vert.position = {x * profile, v * 2 - 1,
                                     .16f * (1 - x * x) * profile + .20f * v * v};
                    vert.normal = unit({x * .32f, .08f - v * .20f, 1});
                } else if (shape == Shape::kTorus) {
                    b = v * 2 * kPi;
                    vert.normal = {std::cos(b) * std::cos(a), std::cos(b) * std::sin(a),
                                   std::sin(b)};
                    vert.position = {std::cos(a) * (1 + .075f * std::cos(b)),
                                     std::sin(a) * (1 + .075f * std::cos(b)), .075f * std::sin(b)};
                } else if (shape == Shape::kPlane) {
                    vert.position = {u * 2 - 1, 0, v * 2 - 1};
                    vert.normal = {0, 1, 0};
                } else {
                    float radius = shape == Shape::kCone ? 1 - v : 1;
                    vert.position = {std::cos(a) * radius, v - .5f, std::sin(a) * radius};
                    vert.normal =
                        unit({std::cos(a), shape == Shape::kCone ? 1.f : 0.f, std::sin(a)});
                }
                vertices.push_back(vert);
            }
        for (int i = 0; i < rows; ++i)
            for (int j = 0; j < cols; ++j) {
                unsigned k = i * (cols + 1) + j;
                indices.insert(indices.end(), {k, k + unsigned(cols) + 1, k + 1, k + 1,
                                               k + unsigned(cols) + 1, k + unsigned(cols) + 2});
            }
        if (shape == Shape::kCylinder || shape == Shape::kCone)
            for (int end = 0; end < 2; ++end) {
                if (shape == Shape::kCone && end == 1)
                    continue;
                unsigned center = vertices.size();
                vertices.push_back({{0, end - .5f, 0}, {0, end ? 1.f : -1.f, 0}});
                for (int j = 0; j <= cols; ++j) {
                    float a = float(j) / cols * 2 * kPi;
                    vertices.push_back(
                        {{std::cos(a), end - .5f, std::sin(a)}, {0, end ? 1.f : -1.f, 0}});
                }
                for (int j = 0; j < cols; ++j)
                    indices.insert(indices.end(),
                                   {center, center + 1 + unsigned(j), center + 2 + unsigned(j)});
            }
        auto uploaded = create_mesh(renderer, vertices, indices);
        if (!uploaded)
            return std::unexpected(uploaded.error());
    }
    return {};
}
void collect_timing(Renderer& r, unsigned index) {
    auto& frame = r.frames[index];
    if (!r.queries || !frame.submitted)
        return;
    std::uint64_t timestamps[2]{};
    if (vkGetQueryPoolResults(r.device, r.queries, index * 2, 2, sizeof(timestamps), timestamps,
                              sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        std::uint64_t mask = r.timestamp_bits == 64 ? kWaitForever : (1ull << r.timestamp_bits) - 1;
        float elapsed = float((timestamps[1] - timestamps[0]) & mask) *
                        r.properties.limits.timestampPeriod / 1000000.f;
        r.gpu_millis = r.gpu_millis == 0 ? elapsed : r.gpu_millis * .85f + elapsed * .15f;
    }
    frame.submitted = false;
}
void begin_pass(Renderer& r, VkRenderPass pass, VkFramebuffer framebuffer, int width, int height,
                bool shadow = false, bool scene = false) {
    auto command = r.frames[r.frame_index].command;
    VkClearValue clear[3]{};
    clear[0].color = {{0, 0, 0, 1}};
    clear[1].depthStencil = {1, 0};
    if (shadow)
        clear[0].depthStencil = {1, 0};
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = pass;
    begin.framebuffer = framebuffer;
    begin.renderArea.extent = {static_cast<unsigned>(width), static_cast<unsigned>(height)};
    begin.clearValueCount = scene ? 3 : 1;
    begin.pClearValues = clear;
    vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport{0, 0, float(width), float(height), 0, 1};
    VkRect2D scissor{{0, 0}, {static_cast<unsigned>(width), static_cast<unsigned>(height)}};
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
}
void bind(Renderer& r, VkPipeline pipeline, Set set,
          VkPipelineBindPoint point = VK_PIPELINE_BIND_POINT_GRAPHICS) {
    auto& frame = r.frames[r.frame_index];
    vkCmdBindPipeline(frame.command, point, pipeline);
    vkCmdBindDescriptorSets(frame.command, point, r.pipeline_layout, 0, 1,
                            &frame.sets[static_cast<unsigned>(set)], 0, nullptr);
}
void push(Renderer& r, Color parameters) {
    vkCmdPushConstants(r.frames[r.frame_index].command, r.pipeline_layout, VK_SHADER_STAGE_ALL, 0,
                       sizeof(parameters), &parameters);
}
void draw_meshes(Renderer& r, bool shadow) {
    bind(r, shadow ? r.shadow_pipeline : r.mesh_pipeline, Set::kScene);
    push(r, {shadow ? 1.f : 0.f, 0, 0, 0});
    auto& frame = r.frames[r.frame_index];
    for (auto& mesh : r.meshes)
        if (!mesh.items.empty()) {
            VkBuffer buffers[] = {mesh.vertices->handle, frame.instances->handle};
            VkDeviceSize offsets[] = {0, mesh.instance_offset};
            vkCmdBindVertexBuffers(frame.command, 0, 2, buffers, offsets);
            vkCmdBindIndexBuffer(frame.command, mesh.indices->handle, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(frame.command, mesh.index_count, mesh.items.size(), 0, 0, 0);
            if (!shadow)
                r.triangle_count += mesh.index_count / 3 * mesh.items.size();
        }
}
}  // namespace

void clear_instances(Renderer& renderer) {
    renderer.model_transform = Mat4{};
    for (auto& m : renderer.meshes)
        m.items.clear();
    renderer.ui.clear();
    renderer.triangle_count = 0;
}
void add(Renderer& renderer, Shape shape, Mat4 model, Color color, float roughness, float metal,
         float emission, float kind) {
    renderer.meshes[int(shape)].items.push_back(
        {renderer.model_transform * model, color, {roughness, metal, emission, kind}});
}
void draw_rect(Renderer& renderer, Rect r, float radius, Color color) {
    radius = std::min(radius, std::min(r.w, r.h) * .5f);
    UiVertex center{r.x + r.w * .5f, r.y + r.h * .5f, .5f / kAtlasSize, .5f / kAtlasSize, color};
    std::vector<UiVertex> perimeter;
    perimeter.reserve(36);
    for (int corner = 0; corner < 4; ++corner)
        for (int j = 0; j <= 8; ++j) {
            float a = (-kPi * .5f + corner * kPi * .5f) + j * kPi / 16;
            float cx = corner < 2 ? r.x + r.w - radius : r.x + radius;
            float cy = corner == 0 || corner == 3 ? r.y + radius : r.y + r.h - radius;
            perimeter.push_back({cx + std::cos(a) * radius, cy + std::sin(a) * radius,
                                 .5f / kAtlasSize, .5f / kAtlasSize, color});
        }
    for (size_t i = 0; i < perimeter.size(); ++i) {
        renderer.ui.push_back(center);
        renderer.ui.push_back(perimeter[i]);
        renderer.ui.push_back(perimeter[(i + 1) % perimeter.size()]);
    }
}
void draw_text(Renderer& renderer, const char* value, float x, float baseline, float height,
               Color color, bool centered) {
    float scale = height / 48;
    if (centered) {
        float width = 0;
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; ++p)
            if (*p >= 32 && *p < 128)
                width += renderer.glyphs[*p - 32].advance;
        x -= width * scale * .5f;
    }
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; ++p)
        if (*p >= 32 && *p < 128) {
            const Glyph& g = renderer.glyphs[*p - 32];
            float left = x + g.xoff * scale, top = baseline + g.yoff * scale,
                  right = left + (g.x1 - g.x0) * scale, bottom = top + (g.y1 - g.y0) * scale;
            UiVertex a{left, top, g.x0 / kAtlasSize, g.y0 / kAtlasSize, color},
                b{right, top, g.x1 / kAtlasSize, g.y0 / kAtlasSize, color},
                c{right, bottom, g.x1 / kAtlasSize, g.y1 / kAtlasSize, color},
                d{left, bottom, g.x0 / kAtlasSize, g.y1 / kAtlasSize, color};
            renderer.ui.insert(renderer.ui.end(), {a, b, c, a, c, d});
            x += g.advance * scale;
        }
}
void add(Renderer& renderer, MeshId mesh, Mat4 model, Color color, float roughness, float metal,
         float emission, float kind) {
    renderer.meshes[mesh].items.push_back(
        {renderer.model_transform * model, color, {roughness, metal, emission, kind}});
}
void set_transform(Renderer& renderer, Mat4 transform) {
    renderer.model_transform = transform;
}

Result<MeshId> create_mesh(Renderer& r, std::span<const Vertex> vertices,
                           std::span<const unsigned> indices) {
    if (vertices.empty() || indices.empty())
        return fail("A mesh must contain vertices and indices");
    auto vertex_buffer = create_buffer(r, vertices.size_bytes(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    if (!vertex_buffer)
        return std::unexpected(vertex_buffer.error());
    auto index_buffer = create_buffer(r, indices.size_bytes(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    if (!index_buffer)
        return std::unexpected(index_buffer.error());
    std::memcpy((*vertex_buffer)->mapped, vertices.data(), vertices.size_bytes());
    std::memcpy((*index_buffer)->mapped, indices.data(), indices.size_bytes());
    r.meshes.push_back({std::move(*vertex_buffer),
                        std::move(*index_buffer),
                        static_cast<unsigned>(indices.size()),
                        0,
                        {}});
    r.meshes.back().items.reserve(1024);
    return static_cast<MeshId>(r.meshes.size() - 1);
}
Result<bool> prepare_frame(Renderer& r, bool maximum) {
    auto result = ensure_targets(r, maximum);
    if (result && *result && r.window) {
        // Compare each size source against its own last observation. They can
        // disagree until queueBuffer refreshes Android's capability cache. Do
        // not wait for agreement before submitting that cache-refresh frame.
        r.observed_window_width = ANativeWindow_getWidth(r.window);
        r.observed_window_height = ANativeWindow_getHeight(r.window);
    }
    return result;
}
Result<bool> surface_changed(const Renderer& r) {
    if (!r.targets_ready || r.recreate_surface)
        return true;
    if (!r.window)
        return false;
    VkSurfaceCapabilitiesKHR capabilities;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(r.physical, r.surface, &capabilities));
    auto extent = surface_extent(r, capabilities);
    return int(extent.width) != r.window_width || int(extent.height) != r.window_height ||
           capabilities.currentTransform != r.surface_transform ||
           ANativeWindow_getWidth(r.window) != r.observed_window_width ||
           ANativeWindow_getHeight(r.window) != r.observed_window_height;
}
Result<bool> render(Renderer& r, Vec3 eye, Vec3 target, double time, bool maximum) {
    // Camera and UI coordinates refer to the targets prepared by the caller.
    // Never rebuild them here after that layout has been computed.
    if (!r.targets_ready || r.maximum != maximum || r.recreate_surface)
        return false;
    auto changed = surface_changed(r);
    if (!changed)
        return std::unexpected(changed.error());
    if (*changed) {
        r.recreate_surface = true;
        return false;
    }
    auto& frame = r.frames[r.frame_index];
    VK_CHECK(vkWaitForFences(r.device, 1, &frame.fence, VK_TRUE, kFrameTimeout));
    frame.in_flight = false;
    collect_timing(r, r.frame_index);
    if (r.window) {
        if (frame.acquiring) {
            VK_CHECK(vkWaitForFences(r.device, 1, &frame.acquisition, VK_TRUE, kFrameTimeout));
            frame.acquiring = false;
        }
        VK_CHECK(vkResetFences(r.device, 1, &frame.acquisition));
        auto acquired = vkAcquireNextImageKHR(r.device, r.swapchain, kFrameTimeout, frame.acquired,
                                              frame.acquisition, &r.image_index);
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
            r.recreate_surface = true;
            // Rebuild before the next frame's camera and UI layout are computed.
            return false;
        }
        if (acquired == VK_TIMEOUT || acquired == VK_NOT_READY)
            return false;
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR)
            return fail("Cannot acquire a Vulkan surface image", acquired);
        frame.acquiring = true;
        auto& image = r.surface_images[r.image_index];
        if (image.present_pending) {
            VK_CHECK(vkWaitForFences(r.device, 1, &image.presented, VK_TRUE, kFrameTimeout));
            image.present_pending = false;
            VK_CHECK(vkResetFences(r.device, 1, &image.presented));
        }
        // With compositor rotation, SUBOPTIMAL alone need not mean the window
        // dimensions changed. Recreating on every such frame would churn targets.
    }
    // Periodic shader motion gets bounded clocks. Seed-dependent particle
    // velocities use both halves of the elapsed time, reduced in the shader.
    float time_high = static_cast<float>(time);
    Globals globals{
        perspective(42 * kPi / 180, float(r.width) / r.height, .15f, 80) * look_at(eye, target),
        ortho(-8, 8, -8, 8, .1f, 32) * look_at({-8, 13, 8}, {0, 0, 0}),
        {eye.x, eye.y, eye.z, oscillation_time(time)},
        {r.render_height / 1000.f, 1.f / r.shadow_size, float(r.width), float(r.height)},
        {time_high, static_cast<float>(time - time_high), static_cast<float>(wrap(time, 100.)), 0},
    };
    std::memcpy(frame.globals->mapped, &globals, sizeof(globals));
    VkDeviceSize size = 0;
    for (auto& mesh : r.meshes) {
        mesh.instance_offset = size;
        size += mesh.items.size() * sizeof(Instance);
    }
    if (auto result = reserve_buffer(r, frame.instances, size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        !result)
        return std::unexpected(result.error());
    for (auto& mesh : r.meshes)
        if (!mesh.items.empty())
            std::memcpy(static_cast<char*>(frame.instances->mapped) + mesh.instance_offset,
                        mesh.items.data(), mesh.items.size() * sizeof(Instance));
    if (auto result = begin_commands(r, frame); !result)
        return std::unexpected(result.error());
    auto command = frame.command;
    if (r.queries) {
        vkCmdResetQueryPool(command, r.queries, r.frame_index * 2, 2);
        vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, r.queries,
                            r.frame_index * 2);
    }
    // The previous frame reads this shared storage buffer as vertex data.
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0,
                         nullptr);
    bind(r, r.compute_pipeline, Set::kScene, VK_PIPELINE_BIND_POINT_COMPUTE);
    vkCmdDispatch(command, r.particle_count / 128, 1, 1);
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &barrier, 0, nullptr, 0,
                         nullptr);
    begin_pass(r, r.shadow_pass, r.shadow_framebuffer, r.shadow_size, r.shadow_size, true);
    draw_meshes(r, true);
    vkCmdEndRenderPass(command);
    begin_pass(r, r.scene_pass, r.hdr_framebuffer, r.render_width, r.render_height, false, true);
    bind(r, r.sky_pipeline, Set::kScene);
    vkCmdDraw(command, 3, 1, 0, 0);
    draw_meshes(r, false);
    bind(r, r.particle_pipeline, Set::kScene);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(command, 0, 1, &r.particles->handle, &offset);
    vkCmdDraw(command, r.particle_count, 1, 0, 0);
    vkCmdEndRenderPass(command);
    int bw = std::max(1, r.render_width / 4), bh = std::max(1, r.render_height / 4);
    for (int i = 0; i < 6; ++i) {
        begin_pass(r, r.bloom_pass, r.bloom_framebuffers[i % 2], bw, bh);
        bind(r, r.blur_pipeline, i == 0 ? Set::kHdr : i % 2 ? Set::kBloom0 : Set::kBloom1);
        push(r, {i % 2 == 0 ? 1.f / bw : 0, i % 2 ? 1.f / bh : 0, i == 0 ? 1.f : 0.f, 0});
        vkCmdDraw(command, 3, 1, 0, 0);
        vkCmdEndRenderPass(command);
    }
    begin_pass(r, r.output_pass,
               r.window ? r.surface_images[r.image_index].framebuffer : r.output_framebuffer,
               r.width, r.height);
    bind(r, r.post_pipeline, Set::kPost);
    vkCmdDraw(command, 3, 1, 0, 0);
    // UI is appended to this render pass by present(), without another image store/load.
    return true;
}
Result<bool> present(Renderer& r) {
    auto& frame = r.frames[r.frame_index];
    if (auto result = reserve_buffer(r, frame.ui, r.ui.size() * sizeof(UiVertex),
                                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        !result)
        return std::unexpected(result.error());
    if (!r.ui.empty()) {
        std::memcpy(frame.ui->mapped, r.ui.data(), r.ui.size() * sizeof(UiVertex));
        bind(r, r.ui_pipeline, Set::kUi);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(frame.command, 0, 1, &frame.ui->handle, &offset);
        vkCmdDraw(frame.command, r.ui.size(), 1, 0, 0);
    }
    vkCmdEndRenderPass(frame.command);
    if (r.queries)
        vkCmdWriteTimestamp(frame.command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, r.queries,
                            r.frame_index * 2 + 1);
    VK_CHECK(vkEndCommandBuffer(frame.command));
    VK_CHECK(vkResetFences(r.device, 1, &frame.fence));
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &frame.command;
    if (r.window) {
        submit.waitSemaphoreCount = submit.signalSemaphoreCount = 1;
        submit.pWaitSemaphores = &frame.acquired;
        submit.pWaitDstStageMask = &wait_stage;
        submit.pSignalSemaphores = &r.surface_images[r.image_index].ready;
    }
    VK_CHECK(vkQueueSubmit(r.queue, 1, &submit, frame.fence));
    frame.in_flight = true;
    frame.submitted = true;
    r.last_frame = r.frame_index;
    r.has_frame = true;
    r.frame_index = (r.frame_index + 1) % kFrameCount;
    if (r.window) {
        auto& image = r.surface_images[r.image_index];
        VkSwapchainPresentFenceInfoEXT completion{
            VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT};
        completion.swapchainCount = 1;
        completion.pFences = &image.presented;
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.pNext = &completion;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &r.surface_images[r.image_index].ready;
        present.swapchainCount = 1;
        present.pSwapchains = &r.swapchain;
        present.pImageIndices = &r.image_index;
        auto result = vkQueuePresentKHR(r.queue, &present);
        // OUT_OF_DATE and SURFACE_LOST still enqueue the presentation operations.
        // Allocation failures leave the fence untouched and must not be waited on.
        image.present_pending =
            result != VK_ERROR_OUT_OF_HOST_MEMORY && result != VK_ERROR_OUT_OF_DEVICE_MEMORY;
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            r.recreate_surface = true;
            return false;
        } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
            return fail("Cannot present the Vulkan frame", result);
        // Acquisition/presentation can block across a resize without reporting
        // OUT_OF_DATE. Consume the acquired image and its semaphores first, then
        // request another frame if its geometry has already become stale.
        auto changed = surface_changed(r);
        if (!changed)
            return std::unexpected(changed.error());
        if (*changed) {
            r.recreate_surface = true;
            return false;
        }
    }
    return true;
}
Result<void> wait_frame(Renderer& r) {
    if (!r.has_frame)
        return fail("No Vulkan frame has been submitted");
    VK_CHECK(vkWaitForFences(r.device, 1, &r.frames[r.last_frame].fence, VK_TRUE, kFrameTimeout));
    collect_timing(r, r.last_frame);
    return {};
}
Result<void> read_pixels(Renderer& r, std::span<unsigned char> rgba) {
    if (r.window || !r.has_frame || rgba.size() != size_t(r.width) * r.height * 4)
        return fail(
            "Readback requires a rendered offscreen image and a correctly sized RGBA buffer");
    if (auto result = wait_frame(r); !result)
        return result;
    auto staging = create_buffer(r, rgba.size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    if (!staging)
        return std::unexpected(staging.error());
    auto& frame = r.frames[r.frame_index];
    VK_CHECK(vkWaitForFences(r.device, 1, &frame.fence, VK_TRUE, kFrameTimeout));
    frame.in_flight = false;
    collect_timing(r, r.frame_index);
    if (auto result = begin_commands(r, frame); !result)
        return result;
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {static_cast<unsigned>(r.width), static_cast<unsigned>(r.height), 1};
    vkCmdCopyImageToBuffer(frame.command, r.output->handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           (*staging)->handle, 1, &copy);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(frame.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);
    if (auto result = submit_immediate(r, frame); !result)
        return result;
    std::memcpy(rgba.data(), (*staging)->mapped, rgba.size());
    return {};
}
Result<void> capture_frame(Renderer& r, const char* path) {
    std::vector<unsigned char> rgba(size_t(r.width) * r.height * 4);
    if (auto result = read_pixels(r, rgba); !result)
        return result;
    FILE* file = std::fopen(path, "wb");
    if (!file)
        return fail("Cannot open the screenshot output");
    bool ok = std::fprintf(file, "P6\n%d %d\n255\n", r.width, r.height) > 0;
    std::vector<unsigned char> row(size_t(r.width) * 3);
    for (int y = 0; y < r.height && ok; ++y) {
        for (int x = 0; x < r.width; ++x)
            std::memcpy(row.data() + x * 3, rgba.data() + (size_t(y) * r.width + x) * 4, 3);
        ok = std::fwrite(row.data(), 1, row.size(), file) == row.size();
    }
    if (std::fclose(file) != 0 || !ok)
        return fail("Cannot write the screenshot");
    return {};
}
Result<Owner<Renderer>> create_renderer(ANativeWindow* window, SceneShaders shaders,
                                        int offscreen_width, int offscreen_height) {
    Owner<Renderer> renderer(new (std::nothrow) Renderer);
    if (!renderer)
        return fail("Cannot allocate renderer state");
    auto& r = *renderer;
    r.window = window;
    r.width = offscreen_width;
    r.height = offscreen_height;
    if (auto result = create_context(r); !result)
        return std::unexpected(result.error());
    if (auto result = create_descriptors(r); !result)
        return std::unexpected(result.error());
    if (auto result = create_pipelines(r, shaders); !result)
        return std::unexpected(result.error());
    if (auto result = create_font(r); !result)
        return std::unexpected(result.error());
    if (auto result = create_geometry(r); !result)
        return std::unexpected(result.error());
    r.ui.reserve(12000);
    if (auto result = ensure_targets(r, false); !result)
        return std::unexpected(result.error());
    return renderer;
}
void destroy(Renderer* renderer) noexcept {
    if (!renderer)
        return;
    auto& r = *renderer;
    if (r.device) {
        auto result = wait_for_work(r);
        if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST) {
            (void)fail("Cannot safely release GPU resources before their deadline", result);
            std::abort();
        }
        destroy_targets(r);
        r.meshes.clear();
        r.font.reset();
        r.particles.reset();
        for (auto& frame : r.frames) {
            frame.globals.reset();
            frame.instances.reset();
            frame.ui.reset();
            vkDestroySemaphore(r.device, frame.acquired, nullptr);
            vkDestroyFence(r.device, frame.fence, nullptr);
            vkDestroyFence(r.device, frame.acquisition, nullptr);
            vkDestroyCommandPool(r.device, frame.pool, nullptr);
        }
        vkDestroyQueryPool(r.device, r.queries, nullptr);
        for (auto pipeline :
             {r.mesh_pipeline, r.shadow_pipeline, r.sky_pipeline, r.particle_pipeline,
              r.compute_pipeline, r.blur_pipeline, r.post_pipeline, r.ui_pipeline})
            vkDestroyPipeline(r.device, pipeline, nullptr);
        for (auto pass : {r.shadow_pass, r.scene_pass, r.bloom_pass, r.output_pass})
            vkDestroyRenderPass(r.device, pass, nullptr);
        vkDestroySampler(r.device, r.sampler, nullptr);
        vkDestroySampler(r.device, r.shadow_sampler, nullptr);
        vkDestroyDescriptorPool(r.device, r.descriptors, nullptr);
        vkDestroyPipelineLayout(r.device, r.pipeline_layout, nullptr);
        vkDestroyDescriptorSetLayout(r.device, r.set_layout, nullptr);
        if (r.swapchain)
            vkDestroySwapchainKHR(r.device, r.swapchain, nullptr);
        vkDestroyDevice(r.device, nullptr);
    }
    if (r.surface)
        vkDestroySurfaceKHR(r.instance, r.surface, nullptr);
#if defined(NATIVE_BUTTONS_VULKAN_VALIDATION)
    if (r.messenger) {
        auto destroy_messenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(r.instance, "vkDestroyDebugUtilsMessengerEXT"));
        destroy_messenger(r.instance, r.messenger, nullptr);
    }
#endif
    if (r.instance)
        vkDestroyInstance(r.instance, nullptr);
    delete renderer;
}
RenderStats get_stats(const Renderer& r) {
    return {r.width,
            r.height,
            r.render_width,
            r.render_height,
            static_cast<int>(r.samples),
            r.particle_count,
            r.triangle_count,
            r.gpu_millis,
            r.queries != VK_NULL_HANDLE};
}
std::string_view get_device(const Renderer& r) {
    return r.properties.deviceName;
}
#if defined(NATIVE_BUTTONS_VULKAN_VALIDATION)
unsigned validation_error_count() {
    return validation_errors.load();
}
#endif
#undef VK_CHECK
}  // namespace gpu
