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
#include <initializer_list>
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
  if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++validation_errors;
  return VK_FALSE;
}
#endif
constexpr int kAtlasSize = 1024;
constexpr int kAtlasLevels = 11;
// Glyphs are rasterized at this pixel height and filtered through mipmaps
// when drawn smaller. Padding keeps neighbors apart through the fifth level.
constexpr int kFontHeight = 128;
constexpr int kFontPadding = 16;
// Rows above the packed glyphs hold white texels for solid geometry.
constexpr int kAtlasReserved = 4;
constexpr unsigned kFrameCount = 2;
constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkSampleCountFlagBits kSamples = VK_SAMPLE_COUNT_4_BIT;
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
  if (!buffer) return;
  if (buffer->mapped) vkUnmapMemory(buffer->device, buffer->memory);
  vkDestroyBuffer(buffer->device, buffer->handle, nullptr);
  vkFreeMemory(buffer->device, buffer->memory, nullptr);
  delete buffer;
}

// Swapchain images use the same description without owning their memory.
struct Image {
  VkDevice device = VK_NULL_HANDLE;
  VkImage handle = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
  VkExtent2D extent{};
  unsigned levels = 1;
};

void destroy(Image* image) noexcept {
  if (!image) return;
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

// Vulkan 1.4 guarantees 256 bytes of push constants, enough for all per-frame values.
// Each pass replaces only the leading parameters.
struct Constants {
  Color parameters;
  Mat4 view, light;
  Color eye_time, size, animation_clock;
};

struct Frame {
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer command = VK_NULL_HANDLE;
  VkSemaphore acquired = VK_NULL_HANDLE;
  VkFence acquisition = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  Owner<Buffer> instances, ui;
  bool in_flight = false, timed = false, acquiring = false;
};

struct SurfaceImage {
  Image target;
  VkSemaphore ready = VK_NULL_HANDLE;
  VkFence presented = VK_NULL_HANDLE;
  bool present_pending = false;
};

// Android's API 26 stub library exports only Vulkan 1.0; the driver supplies newer commands.
struct Commands {
  PFN_vkCmdBeginRendering begin_rendering = nullptr;
  PFN_vkCmdEndRendering end_rendering = nullptr;
  PFN_vkCmdPipelineBarrier2 pipeline_barrier = nullptr;
  PFN_vkCmdPushDescriptorSet push_descriptor_set = nullptr;
  PFN_vkQueueSubmit2 queue_submit = nullptr;
};
}

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
  Commands vk;
  unsigned queue_family = 0, timestamp_bits = 0;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  ANativeWindow* window = nullptr;
  VkFormat output_format = VK_FORMAT_R8G8B8A8_UNORM, depth_format = VK_FORMAT_X8_D24_UNORM_PACK32;
  VkColorSpaceKHR color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  VkSurfaceTransformFlagBitsKHR surface_transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkSampler sampler = VK_NULL_HANDLE, shadow_sampler = VK_NULL_HANDLE;
  VkFilter shadow_filter = VK_FILTER_NEAREST;
  VkPipeline mesh_pipeline = VK_NULL_HANDLE, shadow_pipeline = VK_NULL_HANDLE,
             sky_pipeline = VK_NULL_HANDLE, particle_pipeline = VK_NULL_HANDLE,
             compute_pipeline = VK_NULL_HANDLE, blur_pipeline = VK_NULL_HANDLE,
             post_pipeline = VK_NULL_HANDLE, ui_pipeline = VK_NULL_HANDLE;
  Owner<Image> hdr, ms_color, ms_depth, shadow, bloom[2], output, font;
  Owner<Buffer> particles;
  VkQueryPool queries = VK_NULL_HANDLE;
  std::array<Frame, kFrameCount> frames;
  std::vector<SurfaceImage> surface_images;
  unsigned frame_index = 0, image_index = 0, last_frame = 0, triangle_count = 0;
  int width = 0, height = 0, window_width = 0, window_height = 0, render_width = 0,
      render_height = 0, shadow_size = 0, particle_count = 0;
  int observed_window_width = 0, observed_window_height = 0;
  bool maximum = false, recreate_surface = false, targets_ready = false, has_frame = false;
  // Overlay renderers draw only multisampled UI geometry to the output.
  bool overlay = false;
  float gpu_millis = 0;
  std::vector<Mesh> meshes;
  std::vector<UiVertex> ui;
  Glyph glyphs[96]{};
  Mat4 model_transform;
};

namespace {
std::unexpected<Error> fail(const char* message, VkResult result = VK_SUCCESS) {
  char text[512];
  if (result == VK_SUCCESS) std::snprintf(text, sizeof(text), "%s", message);
  else std::snprintf(text, sizeof(text), "%s (VkResult %d)", message, static_cast<int>(result));
  __android_log_print(ANDROID_LOG_ERROR, "native_buttons", "%s", text);
  return std::unexpected(Error{text});
}

#define VK_CHECK(call)                                    \
  do {                                                    \
    const VkResult result = (call);                       \
    if (result != VK_SUCCESS) return fail(#call, result); \
  } while (false)

Result<unsigned> memory_type(const Renderer& r, unsigned bits, VkMemoryPropertyFlags flags) {
  for (unsigned i = 0; i < r.memory.memoryTypeCount; ++i)
    if ((bits & (1u << i)) && (r.memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
  return fail("No compatible Vulkan memory type");
}

Result<Owner<Buffer>> create_buffer(Renderer& r, VkDeviceSize size, VkBufferUsageFlags usage,
                                    bool host = true) {
  Owner<Buffer> buffer(new (std::nothrow) Buffer);
  if (!buffer) return fail("Cannot allocate buffer state");
  buffer->device = r.device;
  buffer->size = size;
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = size;
  info.usage = usage;
  VK_CHECK(vkCreateBuffer(r.device, &info, nullptr, &buffer->handle));
  VkMemoryRequirements requirements;
  vkGetBufferMemoryRequirements(r.device, buffer->handle, &requirements);
  auto type =
      memory_type(r, requirements.memoryTypeBits,
                  host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                       : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (!type) return std::unexpected(type.error());
  VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocate.allocationSize = requirements.size;
  allocate.memoryTypeIndex = *type;
  VK_CHECK(vkAllocateMemory(r.device, &allocate, nullptr, &buffer->memory));
  VK_CHECK(vkBindBufferMemory(r.device, buffer->handle, buffer->memory, 0));
  if (host) VK_CHECK(vkMapMemory(r.device, buffer->memory, 0, VK_WHOLE_SIZE, 0, &buffer->mapped));
  return buffer;
}

Result<void> reserve_buffer(Renderer& r, Owner<Buffer>& buffer, VkDeviceSize size,
                            VkBufferUsageFlags usage) {
  if (buffer && buffer->size >= size) return {};
  auto created = create_buffer(r, std::max<VkDeviceSize>(size, 4096), usage);
  if (!created) return std::unexpected(created.error());
  buffer = std::move(*created);
  return {};
}

Result<void> create_view(Renderer& r, Image& image, VkFormat format) {
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = image.handle;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = format;
  view.subresourceRange = {image.aspect, 0, image.levels, 0, 1};
  VK_CHECK(vkCreateImageView(r.device, &view, nullptr, &image.view));
  return {};
}

Result<Owner<Image>> create_image(Renderer& r, int width, int height, VkFormat format,
                                  VkImageUsageFlags usage,
                                  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT,
                                  unsigned levels = 1) {
  Owner<Image> image(new (std::nothrow) Image);
  if (!image) return fail("Cannot allocate image state");
  image->device = r.device;
  image->extent = {static_cast<unsigned>(width), static_cast<unsigned>(height)};
  image->levels = levels;
  if (format == r.depth_format) image->aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = {image->extent.width, image->extent.height, 1};
  info.mipLevels = levels;
  info.arrayLayers = 1;
  info.samples = samples;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = usage;
  VK_CHECK(vkCreateImage(r.device, &info, nullptr, &image->handle));
  VkMemoryRequirements requirements;
  vkGetImageMemoryRequirements(r.device, image->handle, &requirements);
  auto type = memory_type(r, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (!type) return std::unexpected(type.error());
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
  if (auto result = create_view(r, *image, format); !result) return std::unexpected(result.error());
  return image;
}

Result<void> begin_commands(Renderer& r, Frame& frame) {
  VK_CHECK(vkResetCommandPool(r.device, frame.pool, 0));
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VK_CHECK(vkBeginCommandBuffer(frame.command, &begin));
  return {};
}

// This phone's driver wakes from fence waits sooner than from timeline-semaphore waits.
VkResult wait_for(Renderer& r, Frame& frame) {
  if (!frame.in_flight) return VK_SUCCESS;
  auto result = vkWaitForFences(r.device, 1, &frame.fence, VK_TRUE, kFrameTimeout);
  if (result == VK_SUCCESS) frame.in_flight = false;
  return result;
}

// Every queue submission and acquisition has a fence. Present fences cover the
// presentation engine too; device/queue idle alone cannot release its resources.
VkResult wait_for_work(Renderer& r) {
  std::vector<VkFence> fences;
  for (auto& frame : r.frames) {
    if (frame.in_flight) fences.push_back(frame.fence);
    if (frame.acquiring) fences.push_back(frame.acquisition);
  }
  for (auto& image : r.surface_images)
    if (image.present_pending) fences.push_back(image.presented);
  VkResult result = fences.empty() ? VK_SUCCESS
                                   : vkWaitForFences(r.device, fences.size(), fences.data(),
                                                     VK_TRUE, kFrameTimeout);
  if (result == VK_SUCCESS || result == VK_ERROR_DEVICE_LOST) {
    for (auto& frame : r.frames) frame.in_flight = frame.acquiring = false;
    for (auto& image : r.surface_images) image.present_pending = false;
  }
  return result;
}

// Presentation waits for the acquired image at color output and signals its ready semaphore.
Result<void> submit(Renderer& r, Frame& frame, VkSemaphore acquired = VK_NULL_HANDLE,
                    VkSemaphore ready = VK_NULL_HANDLE) {
  VK_CHECK(vkEndCommandBuffer(frame.command));
  VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  wait.semaphore = acquired;
  wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  signal.semaphore = ready;
  signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  VkCommandBufferSubmitInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
  command.commandBuffer = frame.command;
  VkSubmitInfo2 info{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
  info.waitSemaphoreInfoCount = acquired ? 1 : 0;
  info.pWaitSemaphoreInfos = &wait;
  info.commandBufferInfoCount = 1;
  info.pCommandBufferInfos = &command;
  info.signalSemaphoreInfoCount = ready ? 1 : 0;
  info.pSignalSemaphoreInfos = &signal;
  VK_CHECK(vkResetFences(r.device, 1, &frame.fence));
  VK_CHECK(r.vk.queue_submit(r.queue, 1, &info, frame.fence));
  frame.in_flight = true;
  return {};
}

Result<void> submit_immediate(Renderer& r, Frame& frame) {
  if (auto result = submit(r, frame); !result) return result;
  auto result = wait_for(r, frame);
  if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST) {
    // Callers own temporary staging buffers. They cannot unwind into cleanup
    // while this submission still uses those buffers.
    (void)fail("GPU transfer did not finish before its deadline", result);
    std::abort();
  }
  if (result != VK_SUCCESS) return fail("GPU transfer failed", result);
  return {};
}

struct Scope {
  VkPipelineStageFlags2 stages;
  VkAccessFlags2 access;
};

constexpr VkPipelineStageFlags2 kAttachmentStages =
    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
    VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
constexpr VkAccessFlags2 kAttachmentWrites =
    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

// Images are rendered, sampled by fragment shaders, copied, or presented.
Scope scope(VkImageLayout layout) {
  switch (layout) {
    // Discarded contents still follow every earlier use of the image.
    case VK_IMAGE_LAYOUT_UNDEFINED:
      return {kAttachmentStages | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                  VK_PIPELINE_STAGE_2_COPY_BIT,
              kAttachmentWrites};
    case VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL:
      return {kAttachmentStages, kAttachmentWrites};
    case VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL:
      return {VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
      return {VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT};
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
      return {VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT};
    // Presentation is ordered by the submission's semaphore signal.
    default:
      return {VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE};
  }
}

void transition(Renderer& r, std::initializer_list<const Image*> images, VkImageLayout from,
                VkImageLayout to) {
  auto source = scope(from), destination = scope(to);
  std::array<VkImageMemoryBarrier2, 3> barriers{};
  unsigned count = 0;
  for (const Image* image : images) {
    if (!image) continue;
    auto& barrier = barriers[count++];
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = source.stages;
    barrier.srcAccessMask = source.access;
    barrier.dstStageMask = destination.stages;
    barrier.dstAccessMask = destination.access;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image->handle;
    barrier.subresourceRange = {image->aspect, 0, image->levels, 0, 1};
  }
  VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.imageMemoryBarrierCount = count;
  dependency.pImageMemoryBarriers = barriers.data();
  r.vk.pipeline_barrier(r.frames[r.frame_index].command, &dependency);
}

void memory_barrier(Renderer& r, Scope source, Scope destination) {
  VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  barrier.srcStageMask = source.stages;
  barrier.srcAccessMask = source.access;
  barrier.dstStageMask = destination.stages;
  barrier.dstAccessMask = destination.access;
  VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.memoryBarrierCount = 1;
  dependency.pMemoryBarriers = &barrier;
  r.vk.pipeline_barrier(r.frames[r.frame_index].command, &dependency);
}

template <typename T>
bool load(VkDevice device, T& command, const char* name) {
  command = reinterpret_cast<T>(vkGetDeviceProcAddr(device, name));
  return command != nullptr;
}

Result<void> create_context(Renderer& r) {
  std::vector<const char*> instance_extensions;
  if (r.window)
    instance_extensions = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
                           VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
                           VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME};
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "native_buttons";
  app.apiVersion = VK_API_VERSION_1_4;
  VkInstanceCreateInfo instance{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  instance.pApplicationInfo = &app;
#if defined(NATIVE_BUTTONS_VULKAN_VALIDATION)
  const char* layer = "VK_LAYER_KHRONOS_validation";
  instance_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  instance.enabledLayerCount = 1;
  instance.ppEnabledLayerNames = &layer;
  VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
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
  auto created = vkCreateInstance(&instance, nullptr, &r.instance);
  if (created == VK_ERROR_EXTENSION_NOT_PRESENT)
    return fail("Safe window rendering requires Vulkan surface maintenance support", created);
  VK_CHECK(created);
#if defined(NATIVE_BUTTONS_VULKAN_VALIDATION)
  debug.pNext = nullptr;
  auto create_messenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
      vkGetInstanceProcAddr(r.instance, "vkCreateDebugUtilsMessengerEXT"));
  if (!create_messenger) return fail("Vulkan validation messenger is unavailable");
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
  for (auto physical : devices) {
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ||
        properties.apiVersion < VK_API_VERSION_1_4)
      continue;
    unsigned family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families.data());
    for (unsigned i = 0; i < family_count; ++i) {
      constexpr auto kFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
      if ((families[i].queueFlags & kFlags) != kFlags) continue;
      VkBool32 present = VK_TRUE;
      if (r.surface)
        VK_CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, r.surface, &present));
      if (!present) continue;
      r.physical = physical;
      r.queue_family = i;
      r.timestamp_bits = families[i].timestampValidBits;
      r.properties = properties;
      break;
    }
    if (r.physical) break;
  }
  if (!r.physical) return fail("A hardware Vulkan 1.4 graphics/compute device is required");
  vkGetPhysicalDeviceMemoryProperties(r.physical, &r.memory);
  // Without a stencil aspect, dynamic rendering has no stencil contents to preserve.
  bool depth_found = r.overlay;
  for (auto format : {VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM}) {
    if (r.overlay) break;
    VkImageFormatProperties attachment{}, shadow{};
    auto attachment_result = vkGetPhysicalDeviceImageFormatProperties(
        r.physical, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, 0,
        &attachment);
    auto shadow_result = vkGetPhysicalDeviceImageFormatProperties(
        r.physical, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, &shadow);
    if (attachment_result != VK_SUCCESS || shadow_result != VK_SUCCESS ||
        !(attachment.sampleCounts & kSamples) || !(shadow.sampleCounts & VK_SAMPLE_COUNT_1_BIT))
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
  if (!depth_found) return fail("A sampled depth format with 4x attachment support is required");
  // Vulkan guarantees 4x multisampling, blending, and filtering for the HDR format.
  // Device creation reports any missing optional feature or extension.
  float priority = 1;
  VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue.queueFamilyIndex = r.queue_family;
  queue.queueCount = 1;
  queue.pQueuePriorities = &priority;
  VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};
  maintenance.swapchainMaintenance1 = VK_TRUE;
  VkPhysicalDeviceVulkan14Features vulkan14{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
  if (r.surface) vulkan14.pNext = &maintenance;
  vulkan14.maintenance5 = vulkan14.pushDescriptor = VK_TRUE;
  VkPhysicalDeviceVulkan13Features vulkan13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  vulkan13.pNext = &vulkan14;
  // SPIR-V 1.6 compute shaders declare their workgroup size with maintenance4.
  vulkan13.synchronization2 = vulkan13.dynamicRendering = vulkan13.maintenance4 = VK_TRUE;
  VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  features.pNext = &vulkan13;
  features.features.largePoints = !r.overlay;
  const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                              VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME};
  VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  device.pNext = &features;
  device.queueCreateInfoCount = 1;
  device.pQueueCreateInfos = &queue;
  if (r.surface) {
    device.enabledExtensionCount = std::size(extensions);
    device.ppEnabledExtensionNames = extensions;
  }
  created = vkCreateDevice(r.physical, &device, nullptr, &r.device);
  if (created == VK_ERROR_EXTENSION_NOT_PRESENT || created == VK_ERROR_FEATURE_NOT_PRESENT)
    return fail("The GPU lacks large points or swapchain presentation fences", created);
  VK_CHECK(created);
  vkGetDeviceQueue(r.device, r.queue_family, 0, &r.queue);
  auto& vk = r.vk;
  if (!load(r.device, vk.begin_rendering, "vkCmdBeginRendering") ||
      !load(r.device, vk.end_rendering, "vkCmdEndRendering") ||
      !load(r.device, vk.pipeline_barrier, "vkCmdPipelineBarrier2") ||
      !load(r.device, vk.push_descriptor_set, "vkCmdPushDescriptorSet") ||
      !load(r.device, vk.queue_submit, "vkQueueSubmit2"))
    return fail("The Vulkan driver does not provide its 1.4 commands");
  if (r.surface) {
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(r.physical, r.surface, &count, nullptr));
    std::vector<VkSurfaceFormatKHR> formats(count);
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(r.physical, r.surface, &count, formats.data()));
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
    if (!found) return fail("An eight-bit UNORM Vulkan surface is required");
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
    VK_CHECK(vkCreateFence(r.device, &fence, nullptr, &frame.fence));
    VK_CHECK(vkCreateFence(r.device, &fence, nullptr, &frame.acquisition));
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VK_CHECK(vkCreateSemaphore(r.device, &semaphore, nullptr, &frame.acquired));
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

Result<void> create_layout(Renderer& r) {
  // Passes push their sampled images and the particle storage into the command buffer.
  VkDescriptorSetLayoutBinding bindings[] = {
      {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
  };
  VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  layout.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT;
  layout.bindingCount = std::size(bindings);
  layout.pBindings = bindings;
  VK_CHECK(vkCreateDescriptorSetLayout(r.device, &layout, nullptr, &r.set_layout));
  VkPushConstantRange push{VK_SHADER_STAGE_ALL, 0, sizeof(Constants)};
  VkPipelineLayoutCreateInfo pipeline{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pipeline.setLayoutCount = 1;
  pipeline.pSetLayouts = &r.set_layout;
  pipeline.pushConstantRangeCount = 1;
  pipeline.pPushConstantRanges = &push;
  VK_CHECK(vkCreatePipelineLayout(r.device, &pipeline, nullptr, &r.pipeline_layout));
  VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
  // Only the font atlas has mipmaps; other sampled images have one level.
  sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  sampler.maxLod = VK_LOD_CLAMP_NONE;
  sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VK_CHECK(vkCreateSampler(r.device, &sampler, nullptr, &r.sampler));
  sampler.compareEnable = VK_TRUE;
  sampler.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
  sampler.magFilter = sampler.minFilter = r.shadow_filter;
  VK_CHECK(vkCreateSampler(r.device, &sampler, nullptr, &r.shadow_sampler));
  return {};
}

// Maintenance5 builds pipelines directly from SPIR-V, without shader modules.
VkShaderModuleCreateInfo shader_code(std::span<const std::uint32_t> code) {
  VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize = code.size_bytes();
  info.pCode = code.data();
  return info;
}

VkPipelineShaderStageCreateInfo shader_stage(VkShaderStageFlagBits stage,
                                             const VkShaderModuleCreateInfo& code) {
  VkPipelineShaderStageCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  info.pNext = &code;
  info.stage = stage;
  info.pName = "main";
  return info;
}

enum class Pipeline { kMesh, kShadow, kSky, kParticle, kBlur, kPost, kUi };

Result<VkPipeline> create_pipeline(Renderer& r, std::span<const std::uint32_t> vertex,
                                   std::span<const std::uint32_t> fragment, Pipeline kind) {
  VkShaderModuleCreateInfo code[] = {shader_code(vertex), shader_code(fragment)};
  VkPipelineShaderStageCreateInfo stages[] = {
      shader_stage(VK_SHADER_STAGE_VERTEX_BIT, code[0]),
      shader_stage(VK_SHADER_STAGE_FRAGMENT_BIT, code[1]),
  };
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
  VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
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
  multisample.rasterizationSamples = hdr || (ui && r.overlay) ? kSamples : VK_SAMPLE_COUNT_1_BIT;
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
  // Dynamic rendering declares attachment formats instead of render passes.
  VkFormat color = hdr || kind == Pipeline::kBlur ? kHdrFormat : r.output_format;
  VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount = shadow ? 0 : 1;
  rendering.pColorAttachmentFormats = &color;
  if (shadow || hdr) rendering.depthAttachmentFormat = r.depth_format;
  VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  info.pNext = &rendering;
  info.stageCount = fragment.empty() ? 1 : 2;
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
  VkPipeline pipeline;
  VK_CHECK(vkCreateGraphicsPipelines(r.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline));
  return pipeline;
}

Result<void> create_scene_pipelines(Renderer& r, SceneShaders shaders) {
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
  };
  for (auto spec : specs) {
    auto pipeline = create_pipeline(r, spec.vertex, spec.fragment, spec.kind);
    if (!pipeline) return std::unexpected(pipeline.error());
    *spec.output = *pipeline;
  }
  auto code = shader_code(kParticlesCompute);
  VkComputePipelineCreateInfo compute{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  compute.stage = shader_stage(VK_SHADER_STAGE_COMPUTE_BIT, code);
  compute.layout = r.pipeline_layout;
  VK_CHECK(vkCreateComputePipelines(r.device, VK_NULL_HANDLE, 1, &compute, nullptr,
                                    &r.compute_pipeline));
  return {};
}

void destroy_targets(Renderer& r) {
  r.targets_ready = false;
  r.has_frame = false;
  for (auto& image : r.surface_images) {
    vkDestroyImageView(r.device, image.target.view, nullptr);
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

VkExtent2D surface_extent(const Renderer& r, const VkSurfaceCapabilitiesKHR& capabilities) {
  if (capabilities.currentExtent.width != std::numeric_limits<unsigned>::max())
    return capabilities.currentExtent;
  int width = ANativeWindow_getWidth(r.window), height = ANativeWindow_getHeight(r.window);
  if (width <= 0 || height <= 0) return {};
  return {std::clamp<unsigned>(width, capabilities.minImageExtent.width,
                               capabilities.maxImageExtent.width),
          std::clamp<unsigned>(height, capabilities.minImageExtent.height,
                               capabilities.maxImageExtent.height)};
}

Result<bool> create_swapchain(Renderer& r, const VkSurfaceCapabilitiesKHR& capabilities) {
  if (r.width <= 0 || r.height <= 0) return false;
  if (!(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
    return fail("Vulkan surface cannot be rendered to");
  unsigned count = capabilities.minImageCount + 1;
  if (capabilities.maxImageCount) count = std::min(count, capabilities.maxImageCount);
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
  if (result == VK_ERROR_OUT_OF_DATE_KHR) return false;
  if (result != VK_SUCCESS) return fail("Cannot create the Vulkan swapchain", result);
  VK_CHECK(vkGetSwapchainImagesKHR(r.device, r.swapchain, &count, nullptr));
  std::vector<VkImage> images(count);
  VK_CHECK(vkGetSwapchainImagesKHR(r.device, r.swapchain, &count, images.data()));
  r.surface_images.resize(count);
  for (unsigned i = 0; i < count; ++i) {
    auto& image = r.surface_images[i];
    image.target.device = r.device;
    image.target.handle = images[i];
    image.target.extent = info.imageExtent;
    if (auto view = create_view(r, image.target, r.output_format); !view)
      return std::unexpected(view.error());
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VK_CHECK(vkCreateSemaphore(r.device, &semaphore, nullptr, &image.ready));
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_CHECK(vkCreateFence(r.device, &fence, nullptr, &image.presented));
  }
  return true;
}

Result<bool> ensure_targets(Renderer& r, bool maximum) {
  if (r.overlay) maximum = false;
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
    if (auto result = create_swapchain(r, capabilities); !result || !*result) return result;
  }
  float scale = maximum ? 1.30f : 1.f;
  r.render_width = int(r.width * scale);
  r.render_height = int(r.height * scale);
  r.shadow_size = r.overlay ? 0 : maximum ? 4096 : 2048;
  r.particle_count = r.overlay ? 0 : maximum ? 65536 : 16384;
  if (std::max({r.render_width, r.render_height, r.shadow_size}) >
      static_cast<int>(r.properties.limits.maxImageDimension2D))
    return fail("Requested image size exceeds the GPU limit");

  struct ImageSpec {
    Owner<Image>* target;
    int width, height;
    VkFormat format;
    VkImageUsageFlags usage;
    VkSampleCountFlagBits samples;
  };

  constexpr auto kColorUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  constexpr auto kDepthUsage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  constexpr auto kTransient = VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
  int bw = std::max(1, r.render_width / 4), bh = std::max(1, r.render_height / 4);
  // Overlay frames resolve their multisampled color directly into the output.
  const ImageSpec overlay_specs[] = {
      {&r.ms_color, r.render_width, r.render_height, r.output_format,
       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | kTransient, kSamples},
  };
  const ImageSpec scene_specs[] = {
      {&r.hdr, r.render_width, r.render_height, kHdrFormat, kColorUsage, VK_SAMPLE_COUNT_1_BIT},
      {&r.ms_color, r.render_width, r.render_height, kHdrFormat,
       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | kTransient, kSamples},
      {&r.ms_depth, r.render_width, r.render_height, r.depth_format, kDepthUsage | kTransient,
       kSamples},
      {&r.shadow, r.shadow_size, r.shadow_size, r.depth_format,
       kDepthUsage | VK_IMAGE_USAGE_SAMPLED_BIT, VK_SAMPLE_COUNT_1_BIT},
      {&r.bloom[0], bw, bh, kHdrFormat, kColorUsage, VK_SAMPLE_COUNT_1_BIT},
      {&r.bloom[1], bw, bh, kHdrFormat, kColorUsage, VK_SAMPLE_COUNT_1_BIT},
  };
  std::span<const ImageSpec> specs = r.overlay ? std::span<const ImageSpec>(overlay_specs)
                                               : std::span<const ImageSpec>(scene_specs);
  for (auto spec : specs) {
    auto image = create_image(r, spec.width, spec.height, spec.format, spec.usage, spec.samples);
    if (!image) return std::unexpected(image.error());
    *spec.target = std::move(*image);
  }
  if (!r.window) {
    auto image =
        create_image(r, r.width, r.height, r.output_format,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (!image) return std::unexpected(image.error());
    r.output = std::move(*image);
  }
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
    if (!f) continue;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::rewind(f);
    if (n > 0 && n < 16000000) {
      data.resize(n);
      if (std::fread(data.data(), 1, n, f) != size_t(n)) data.clear();
    }
    std::fclose(f);
    if (!data.empty()) break;
  }
  if (data.empty()) return fail("Cannot load the system font");
  size_t total = 0;
  for (int level = 0; level < kAtlasLevels; ++level) {
    size_t size = kAtlasSize >> level;
    total += size * size;
  }
  std::vector<unsigned char> pixels(total);
  stbtt_pack_context pack;
  stbtt_packedchar packed[96];
  if (!stbtt_PackBegin(&pack, pixels.data() + kAtlasReserved * kAtlasSize, kAtlasSize,
                       kAtlasSize - kAtlasReserved, kAtlasSize, kFontPadding, nullptr))
    return fail("Cannot allocate the font atlas packer");
  int packed_all = stbtt_PackFontRange(&pack, data.data(), 0, kFontHeight, 32, 96, packed);
  stbtt_PackEnd(&pack);
  if (!packed_all) return fail("Font atlas overflow");
  for (int i = 0; i < 96; ++i) {
    auto& p = packed[i];
    renderer.glyphs[i] = {float(p.x0), float(p.y0 + kAtlasReserved),
                          float(p.x1), float(p.y1 + kAtlasReserved),
                          p.xoff,      p.yoff,
                          p.xadvance};
  }
  for (int y = 0; y < kAtlasReserved; ++y)
    for (int x = 0; x < kAtlasReserved; ++x) pixels[y * kAtlasSize + x] = 255;
  // Box-filter each level from the previous one; offsets stay multiples of four.
  VkBufferImageCopy copies[kAtlasLevels]{};
  size_t offset = 0;
  for (int level = 0; level < kAtlasLevels; ++level) {
    unsigned size = kAtlasSize >> level;
    if (level) {
      const unsigned char* source = pixels.data() + copies[level - 1].bufferOffset;
      unsigned char* target = pixels.data() + offset;
      unsigned wide = size * 2;
      for (unsigned y = 0; y < size; ++y)
        for (unsigned x = 0; x < size; ++x) {
          const unsigned char* p = source + (y * 2) * wide + x * 2;
          target[y * size + x] = (p[0] + p[1] + p[wide] + p[wide + 1] + 2) / 4;
        }
    }
    copies[level].bufferOffset = offset;
    copies[level].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, unsigned(level), 0, 1};
    copies[level].imageExtent = {size, size, 1};
    offset += size_t(size) * size;
  }
  auto image = create_image(renderer, kAtlasSize, kAtlasSize, VK_FORMAT_R8_UNORM,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VK_SAMPLE_COUNT_1_BIT, kAtlasLevels);
  if (!image) return std::unexpected(image.error());
  renderer.font = std::move(*image);
  auto staging = create_buffer(renderer, pixels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
  if (!staging) return std::unexpected(staging.error());
  std::memcpy((*staging)->mapped, pixels.data(), pixels.size());
  auto& frame = renderer.frames[renderer.frame_index];
  if (auto result = begin_commands(renderer, frame); !result) return result;
  transition(renderer, {renderer.font.get()}, VK_IMAGE_LAYOUT_UNDEFINED,
             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  vkCmdCopyBufferToImage(frame.command, (*staging)->handle, renderer.font->handle,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kAtlasLevels, copies);
  transition(renderer, {renderer.font.get()}, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
             VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL);
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
    if (shape == Shape::kTorus) rows = 20;
    if (shape == Shape::kCylinder || shape == Shape::kCone) rows = 1;
    if (shape == Shape::kPlane) {
      rows = 1;
      cols = 1;
    }
    for (int i = 0; i <= rows; ++i)
      for (int j = 0; j <= cols; ++j) {
        float u = float(j) / cols, v = float(i) / rows, a = u * 2 * kPi, b = v * kPi;
        Vertex vert;
        if (shape == Shape::kSphere || shape == Shape::kLowSphere) {
          vert.normal = {std::sin(b) * std::cos(a), std::cos(b), std::sin(b) * std::sin(a)};
          vert.position = vert.normal;
        } else if (shape == Shape::kFeather) {
          float x = (u * 2 - 1), profile = std::pow(std::max(0.f, std::sin(v * kPi)), .72f);
          vert.position = {x * profile, v * 2 - 1, .16f * (1 - x * x) * profile + .20f * v * v};
          vert.normal = unit({x * .32f, .08f - v * .20f, 1});
        } else if (shape == Shape::kTorus) {
          b = v * 2 * kPi;
          vert.normal = {std::cos(b) * std::cos(a), std::cos(b) * std::sin(a), std::sin(b)};
          vert.position = {std::cos(a) * (1 + .075f * std::cos(b)),
                           std::sin(a) * (1 + .075f * std::cos(b)), .075f * std::sin(b)};
        } else if (shape == Shape::kPlane) {
          vert.position = {u * 2 - 1, 0, v * 2 - 1};
          vert.normal = {0, 1, 0};
        } else {
          float radius = shape == Shape::kCone ? 1 - v : 1;
          vert.position = {std::cos(a) * radius, v - .5f, std::sin(a) * radius};
          vert.normal = unit({std::cos(a), shape == Shape::kCone ? 1.f : 0.f, std::sin(a)});
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
        if (shape == Shape::kCone && end == 1) continue;
        unsigned center = vertices.size();
        vertices.push_back({{0, end - .5f, 0}, {0, end ? 1.f : -1.f, 0}});
        for (int j = 0; j <= cols; ++j) {
          float a = float(j) / cols * 2 * kPi;
          vertices.push_back({{std::cos(a), end - .5f, std::sin(a)}, {0, end ? 1.f : -1.f, 0}});
        }
        for (int j = 0; j < cols; ++j)
          indices.insert(indices.end(),
                         {center, center + 1 + unsigned(j), center + 2 + unsigned(j)});
      }
    auto uploaded = create_mesh(renderer, vertices, indices);
    if (!uploaded) return std::unexpected(uploaded.error());
  }
  return {};
}

void collect_timing(Renderer& r, unsigned index) {
  auto& frame = r.frames[index];
  if (!r.queries || !frame.timed) return;
  std::uint64_t timestamps[2]{};
  if (vkGetQueryPoolResults(r.device, r.queries, index * 2, 2, sizeof(timestamps), timestamps,
                            sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
    std::uint64_t mask = r.timestamp_bits == 64 ? kWaitForever : (1ull << r.timestamp_bits) - 1;
    float elapsed = float((timestamps[1] - timestamps[0]) & mask) *
                    r.properties.limits.timestampPeriod / 1000000.f;
    r.gpu_millis = r.gpu_millis == 0 ? elapsed : r.gpu_millis * .85f + elapsed * .15f;
  }
  frame.timed = false;
}

const Image& output(const Renderer& r) {
  return r.window ? r.surface_images[r.image_index].target : *r.output;
}

// Discard and clear the targets, then render to them. Multisampled attachments
// are resolved and discarded; other color targets and the shadow depth are stored.
void begin_pass(Renderer& r, const Image* color, const Image* depth = nullptr,
                const Image* resolve = nullptr, Color clear = {0, 0, 0, 1}) {
  transition(r, {color, depth, resolve}, VK_IMAGE_LAYOUT_UNDEFINED,
             VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL);
  VkRenderingAttachmentInfo attachments[2]{};
  for (auto& attachment : attachments) {
    attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    attachment.imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = resolve ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
  }
  attachments[0].clearValue.color = {{clear.r, clear.g, clear.b, clear.a}};
  attachments[1].clearValue.depthStencil = {1, 0};
  VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
  info.renderArea.extent = (color ? color : depth)->extent;
  info.layerCount = 1;
  if (color) {
    attachments[0].imageView = color->view;
    info.colorAttachmentCount = 1;
    info.pColorAttachments = &attachments[0];
  }
  if (depth) {
    attachments[1].imageView = depth->view;
    info.pDepthAttachment = &attachments[1];
  }
  if (resolve) {
    attachments[0].resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
    attachments[0].resolveImageView = resolve->view;
    attachments[0].resolveImageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
  }
  auto command = r.frames[r.frame_index].command;
  r.vk.begin_rendering(command, &info);
  VkViewport viewport{
      0, 0, float(info.renderArea.extent.width), float(info.renderArea.extent.height), 0, 1};
  vkCmdSetViewport(command, 0, 1, &viewport);
  vkCmdSetScissor(command, 0, 1, &info.renderArea);
}

void end_pass(Renderer& r, const Image& target, VkImageLayout layout) {
  r.vk.end_rendering(r.frames[r.frame_index].command);
  transition(r, {&target}, VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL, layout);
}

VkDescriptorImageInfo sampled(VkSampler sampler, const Image& image) {
  return {sampler, image.view, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL};
}

// Push the fragment shader's images to consecutive bindings along with the pipeline.
void bind(Renderer& r, VkPipeline pipeline,
          std::initializer_list<VkDescriptorImageInfo> images = {}) {
  auto command = r.frames[r.frame_index].command;
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  std::array<VkWriteDescriptorSet, 2> writes{};
  unsigned count = 0;
  for (const auto& image : images) {
    auto& write = writes[count];
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstBinding = count++;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
  }
  if (count)
    r.vk.push_descriptor_set(command, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipeline_layout, 0, count,
                             writes.data());
}

void push(Renderer& r, Color parameters) {
  vkCmdPushConstants(r.frames[r.frame_index].command, r.pipeline_layout, VK_SHADER_STAGE_ALL, 0,
                     sizeof(parameters), &parameters);
}

void draw_meshes(Renderer& r, bool shadow) {
  if (shadow) bind(r, r.shadow_pipeline);
  else bind(r, r.mesh_pipeline, {sampled(r.shadow_sampler, *r.shadow)});
  push(r, {shadow ? 1.f : 0.f, 0, 0, 0});
  auto& frame = r.frames[r.frame_index];
  for (auto& mesh : r.meshes)
    if (!mesh.items.empty()) {
      VkBuffer buffers[] = {mesh.vertices->handle, frame.instances->handle};
      VkDeviceSize offsets[] = {0, mesh.instance_offset};
      vkCmdBindVertexBuffers(frame.command, 0, 2, buffers, offsets);
      vkCmdBindIndexBuffer(frame.command, mesh.indices->handle, 0, VK_INDEX_TYPE_UINT32);
      vkCmdDrawIndexed(frame.command, mesh.index_count, mesh.items.size(), 0, 0, 0);
      if (!shadow) r.triangle_count += mesh.index_count / 3 * mesh.items.size();
    }
}
}

void clear_instances(Renderer& renderer) {
  renderer.model_transform = Mat4{};
  for (auto& m : renderer.meshes) m.items.clear();
  renderer.ui.clear();
  renderer.triangle_count = 0;
}

void add(Renderer& renderer, Shape shape, Mat4 model, Color color, float roughness, float metal,
         float emission, float kind) {
  renderer.meshes[int(shape)].items.push_back(
      {renderer.model_transform * model, color, {roughness, metal, emission, kind}});
}

void draw_triangle(Renderer& renderer, float x0, float y0, float x1, float y1, float x2, float y2,
                   Color color) {
  constexpr float kWhite = .5f / kAtlasSize;
  renderer.ui.insert(renderer.ui.end(), {{x0, y0, kWhite, kWhite, color},
                                         {x1, y1, kWhite, kWhite, color},
                                         {x2, y2, kWhite, kWhite, color}});
}

void draw_line(Renderer& renderer, float x0, float y0, float x1, float y1, float width,
               Color color) {
  float dx = x1 - x0, dy = y1 - y0, length = std::sqrt(dx * dx + dy * dy);
  if (!(length > 0)) return;
  float nx = -dy / length * width * .5f, ny = dx / length * width * .5f;
  draw_triangle(renderer, x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, color);
  draw_triangle(renderer, x0 + nx, y0 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny, color);
}

void draw_rect(Renderer& renderer, Rect r, float radius, Color color) {
  radius = std::min(radius, std::min(r.w, r.h) * .5f);
  if (!(radius > 0)) {
    draw_triangle(renderer, r.x, r.y, r.x + r.w, r.y, r.x + r.w, r.y + r.h, color);
    draw_triangle(renderer, r.x, r.y, r.x + r.w, r.y + r.h, r.x, r.y + r.h, color);
    return;
  }
  UiVertex center{r.x + r.w * .5f, r.y + r.h * .5f, .5f / kAtlasSize, .5f / kAtlasSize, color};
  std::vector<UiVertex> perimeter;
  perimeter.reserve(36);
  for (int corner = 0; corner < 4; ++corner)
    for (int j = 0; j <= 8; ++j) {
      float a = (-kPi * .5f + corner * kPi * .5f) + j * kPi / 16;
      float cx = corner < 2 ? r.x + r.w - radius : r.x + radius;
      float cy = corner == 0 || corner == 3 ? r.y + radius : r.y + r.h - radius;
      perimeter.push_back({cx + std::cos(a) * radius, cy + std::sin(a) * radius, .5f / kAtlasSize,
                           .5f / kAtlasSize, color});
    }
  for (size_t i = 0; i < perimeter.size(); ++i) {
    renderer.ui.push_back(center);
    renderer.ui.push_back(perimeter[i]);
    renderer.ui.push_back(perimeter[(i + 1) % perimeter.size()]);
  }
}

void draw_text(Renderer& renderer, const char* value, float x, float baseline, float height,
               Color color, bool centered) {
  float scale = height / kFontHeight;
  if (centered) {
    float width = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; ++p)
      if (*p >= 32 && *p < 128) width += renderer.glyphs[*p - 32].advance;
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

Rect measure_text(const Renderer& renderer, const char* value, float height) {
  float scale = height / kFontHeight, width = 0, top = 0, bottom = 0;
  bool inked = false;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; ++p)
    if (*p >= 32 && *p < 128) {
      const Glyph& g = renderer.glyphs[*p - 32];
      if (g.y1 > g.y0) {
        float glyph_top = g.yoff * scale, glyph_bottom = glyph_top + (g.y1 - g.y0) * scale;
        top = inked ? std::min(top, glyph_top) : glyph_top;
        bottom = inked ? std::max(bottom, glyph_bottom) : glyph_bottom;
        inked = true;
      }
      width += g.advance * scale;
    }
  return {0, top, width, bottom - top};
}

void add(Renderer& renderer, MeshId mesh, Mat4 model, Color color, float roughness, float metal,
         float emission, float kind) {
  renderer.meshes[mesh].items.push_back(
      {renderer.model_transform * model, color, {roughness, metal, emission, kind}});
}

void set_transform(Renderer& renderer, Mat4 transform) { renderer.model_transform = transform; }

Result<MeshId> create_mesh(Renderer& r, std::span<const Vertex> vertices,
                           std::span<const unsigned> indices) {
  if (vertices.empty() || indices.empty()) return fail("A mesh must contain vertices and indices");
  auto vertex_buffer = create_buffer(r, vertices.size_bytes(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
  if (!vertex_buffer) return std::unexpected(vertex_buffer.error());
  auto index_buffer = create_buffer(r, indices.size_bytes(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
  if (!index_buffer) return std::unexpected(index_buffer.error());
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
  if (!r.targets_ready || r.recreate_surface) return true;
  if (!r.window) return false;
  VkSurfaceCapabilitiesKHR capabilities;
  VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(r.physical, r.surface, &capabilities));
  auto extent = surface_extent(r, capabilities);
  return int(extent.width) != r.window_width || int(extent.height) != r.window_height ||
         capabilities.currentTransform != r.surface_transform ||
         ANativeWindow_getWidth(r.window) != r.observed_window_width ||
         ANativeWindow_getHeight(r.window) != r.observed_window_height;
}

namespace {
// Wait for the frame's resources and acquire its output. False defers the frame.
Result<bool> begin_frame(Renderer& r, bool maximum) {
  // Camera and UI coordinates refer to the targets prepared by the caller.
  // Never rebuild them here after that layout has been computed.
  if (!r.targets_ready || r.maximum != maximum || r.recreate_surface) return false;
  auto changed = surface_changed(r);
  if (!changed) return std::unexpected(changed.error());
  if (*changed) {
    r.recreate_surface = true;
    return false;
  }
  auto& frame = r.frames[r.frame_index];
  VK_CHECK(wait_for(r, frame));
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
    if (acquired == VK_TIMEOUT || acquired == VK_NOT_READY) return false;
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
  return true;
}
}

Result<bool> render(Renderer& r, Vec3 eye, Vec3 target, double time, bool maximum) {
  if (r.overlay) return fail("Scene rendering requires scene shaders");
  auto begun = begin_frame(r, maximum);
  if (!begun || !*begun) return begun;
  auto& frame = r.frames[r.frame_index];
  // Periodic shader motion gets bounded clocks. Seed-dependent particle
  // velocities use both halves of the elapsed time, reduced in the shader.
  float time_high = static_cast<float>(time);
  Constants constants{
      {0, 0, 0, 0},
      perspective(42 * kPi / 180, float(r.width) / r.height, .15f, 80) * look_at(eye, target),
      ortho(-8, 8, -8, 8, .1f, 32) * look_at({-8, 13, 8}, {0, 0, 0}),
      {eye.x, eye.y, eye.z, oscillation_time(time)},
      {r.render_height / 1000.f, 1.f / r.shadow_size, float(r.width), float(r.height)},
      {time_high, static_cast<float>(time - time_high), static_cast<float>(wrap(time, 100.)), 0},
  };
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
  if (auto result = begin_commands(r, frame); !result) return std::unexpected(result.error());
  auto command = frame.command;
  if (r.queries) {
    vkCmdResetQueryPool(command, r.queries, r.frame_index * 2, 2);
    vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, r.queries, r.frame_index * 2);
  }
  // Every pipeline shares this layout, so the constants persist across the frame.
  vkCmdPushConstants(command, r.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(constants),
                     &constants);
  // The previous frame reads this shared storage buffer as vertex data.
  memory_barrier(r, {VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT, VK_ACCESS_2_NONE},
                 {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT});
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, r.compute_pipeline);
  VkDescriptorBufferInfo particles{r.particles->handle, 0, VK_WHOLE_SIZE};
  VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstBinding = 2;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  write.pBufferInfo = &particles;
  r.vk.push_descriptor_set(command, VK_PIPELINE_BIND_POINT_COMPUTE, r.pipeline_layout, 0, 1,
                           &write);
  vkCmdDispatch(command, r.particle_count / 128, 1, 1);
  memory_barrier(
      r, {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT},
      {VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT});
  begin_pass(r, nullptr, r.shadow.get());
  draw_meshes(r, true);
  end_pass(r, *r.shadow, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL);
  begin_pass(r, r.ms_color.get(), r.ms_depth.get(), r.hdr.get());
  bind(r, r.sky_pipeline);
  vkCmdDraw(command, 3, 1, 0, 0);
  draw_meshes(r, false);
  bind(r, r.particle_pipeline);
  VkDeviceSize offset = 0;
  vkCmdBindVertexBuffers(command, 0, 1, &r.particles->handle, &offset);
  vkCmdDraw(command, r.particle_count, 1, 0, 0);
  end_pass(r, *r.hdr, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL);
  auto bloom = r.bloom[0]->extent;
  for (int i = 0; i < 6; ++i) {
    const Image& source = i == 0 ? *r.hdr : *r.bloom[(i + 1) % 2];
    begin_pass(r, r.bloom[i % 2].get());
    bind(r, r.blur_pipeline, {sampled(r.sampler, source)});
    push(r, {i % 2 == 0 ? 1.f / bloom.width : 0, i % 2 ? 1.f / bloom.height : 0, i == 0 ? 1.f : 0.f,
             0});
    vkCmdDraw(command, 3, 1, 0, 0);
    end_pass(r, *r.bloom[i % 2], VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL);
  }
  begin_pass(r, &output(r));
  bind(r, r.post_pipeline, {sampled(r.sampler, *r.hdr), sampled(r.sampler, *r.bloom[1])});
  vkCmdDraw(command, 3, 1, 0, 0);
  // UI is appended to this pass by present(), without another image store/load.
  return true;
}

Result<bool> render_overlay(Renderer& r, Color background) {
  if (!r.overlay) return fail("Overlay frames require an overlay renderer");
  // Overlay frames have no scene instances; discard the previous frame's UI.
  r.ui.clear();
  auto begun = begin_frame(r, false);
  if (!begun || !*begun) return begun;
  auto& frame = r.frames[r.frame_index];
  Constants constants{};
  constants.size = {1, 0, float(r.width), float(r.height)};
  if (auto result = begin_commands(r, frame); !result) return std::unexpected(result.error());
  if (r.queries) {
    vkCmdResetQueryPool(frame.command, r.queries, r.frame_index * 2, 2);
    vkCmdWriteTimestamp(frame.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, r.queries,
                        r.frame_index * 2);
  }
  vkCmdPushConstants(frame.command, r.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(constants),
                     &constants);
  // present() draws the UI and resolves the multisampled color into the output.
  begin_pass(r, r.ms_color.get(), nullptr, &output(r), background);
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
    bind(r, r.ui_pipeline, {sampled(r.sampler, *r.font)});
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(frame.command, 0, 1, &frame.ui->handle, &offset);
    vkCmdDraw(frame.command, r.ui.size(), 1, 0, 0);
  }
  end_pass(r, output(r),
           r.window ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  if (r.queries)
    vkCmdWriteTimestamp(frame.command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, r.queries,
                        r.frame_index * 2 + 1);
  auto* image = r.window ? &r.surface_images[r.image_index] : nullptr;
  if (auto result = submit(r, frame, image ? frame.acquired : VK_NULL_HANDLE,
                           image ? image->ready : VK_NULL_HANDLE);
      !result)
    return std::unexpected(result.error());
  frame.timed = true;
  r.last_frame = r.frame_index;
  r.has_frame = true;
  r.frame_index = (r.frame_index + 1) % kFrameCount;
  if (image) {
    VkSwapchainPresentFenceInfoEXT completion{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT};
    completion.swapchainCount = 1;
    completion.pFences = &image->presented;
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.pNext = &completion;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &image->ready;
    present.swapchainCount = 1;
    present.pSwapchains = &r.swapchain;
    present.pImageIndices = &r.image_index;
    auto result = vkQueuePresentKHR(r.queue, &present);
    // OUT_OF_DATE and SURFACE_LOST still enqueue the presentation operations.
    // Allocation failures leave the fence untouched and must not be waited on.
    image->present_pending =
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
    if (!changed) return std::unexpected(changed.error());
    if (*changed) {
      r.recreate_surface = true;
      return false;
    }
  }
  return true;
}

Result<void> wait_frame(Renderer& r) {
  if (!r.has_frame) return fail("No Vulkan frame has been submitted");
  VK_CHECK(wait_for(r, r.frames[r.last_frame]));
  collect_timing(r, r.last_frame);
  return {};
}

Result<void> read_pixels(Renderer& r, std::span<unsigned char> rgba) {
  if (r.window || !r.has_frame || rgba.size() != size_t(r.width) * r.height * 4)
    return fail("Readback requires a rendered offscreen image and a correctly sized RGBA buffer");
  if (auto result = wait_frame(r); !result) return result;
  auto staging = create_buffer(r, rgba.size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  if (!staging) return std::unexpected(staging.error());
  auto& frame = r.frames[r.frame_index];
  VK_CHECK(wait_for(r, frame));
  collect_timing(r, r.frame_index);
  if (auto result = begin_commands(r, frame); !result) return result;
  VkBufferImageCopy copy{};
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageExtent = {static_cast<unsigned>(r.width), static_cast<unsigned>(r.height), 1};
  vkCmdCopyImageToBuffer(frame.command, r.output->handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         (*staging)->handle, 1, &copy);
  memory_barrier(r, {VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT},
                 {VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT});
  if (auto result = submit_immediate(r, frame); !result) return result;
  std::memcpy(rgba.data(), (*staging)->mapped, rgba.size());
  return {};
}

Result<void> capture_frame(Renderer& r, const char* path) {
  std::vector<unsigned char> rgba(size_t(r.width) * r.height * 4);
  if (auto result = read_pixels(r, rgba); !result) return result;
  FILE* file = std::fopen(path, "wb");
  if (!file) return fail("Cannot open the screenshot output");
  bool ok = std::fprintf(file, "P6\n%d %d\n255\n", r.width, r.height) > 0;
  std::vector<unsigned char> row(size_t(r.width) * 3);
  for (int y = 0; y < r.height && ok; ++y) {
    for (int x = 0; x < r.width; ++x)
      std::memcpy(row.data() + x * 3, rgba.data() + (size_t(y) * r.width + x) * 4, 3);
    ok = std::fwrite(row.data(), 1, row.size(), file) == row.size();
  }
  if (std::fclose(file) != 0 || !ok) return fail("Cannot write the screenshot");
  return {};
}

namespace {
// Only create_renderer references the scene resources, so linking an overlay
// renderer alone omits their pipelines, shaders, and meshes.
Result<void> create_scene_resources(Renderer& r, SceneShaders shaders) {
  auto particles =
      create_buffer(r, 65536 * sizeof(Color),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, false);
  if (!particles) return std::unexpected(particles.error());
  r.particles = std::move(*particles);
  if (auto result = create_scene_pipelines(r, shaders); !result) return result;
  return create_geometry(r);
}

using SceneSetup = Result<void> (*)(Renderer&, SceneShaders);

Result<Owner<Renderer>> create(ANativeWindow* window, int offscreen_width, int offscreen_height,
                               SceneSetup scene, SceneShaders shaders) {
  Owner<Renderer> renderer(new (std::nothrow) Renderer);
  if (!renderer) return fail("Cannot allocate renderer state");
  auto& r = *renderer;
  r.overlay = !scene;
  r.window = window;
  r.width = offscreen_width;
  r.height = offscreen_height;
  if (auto result = create_context(r); !result) return std::unexpected(result.error());
  if (auto result = create_layout(r); !result) return std::unexpected(result.error());
  if (scene)
    if (auto result = scene(r, shaders); !result) return std::unexpected(result.error());
  auto ui = create_pipeline(r, kUiVertex, kUiFragment, Pipeline::kUi);
  if (!ui) return std::unexpected(ui.error());
  r.ui_pipeline = *ui;
  if (auto result = create_font(r); !result) return std::unexpected(result.error());
  r.ui.reserve(12000);
  if (auto result = ensure_targets(r, false); !result) return std::unexpected(result.error());
  return renderer;
}
}

Result<Owner<Renderer>> create_renderer(ANativeWindow* window, SceneShaders shaders,
                                        int offscreen_width, int offscreen_height) {
  return create(window, offscreen_width, offscreen_height, create_scene_resources, shaders);
}

Result<Owner<Renderer>> create_overlay_renderer(ANativeWindow* window, int offscreen_width,
                                                int offscreen_height) {
  return create(window, offscreen_width, offscreen_height, nullptr, {});
}

void destroy(Renderer* renderer) noexcept {
  if (!renderer) return;
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
      frame.instances.reset();
      frame.ui.reset();
      vkDestroySemaphore(r.device, frame.acquired, nullptr);
      vkDestroyFence(r.device, frame.fence, nullptr);
      vkDestroyFence(r.device, frame.acquisition, nullptr);
      vkDestroyCommandPool(r.device, frame.pool, nullptr);
    }
    vkDestroyQueryPool(r.device, r.queries, nullptr);
    for (auto pipeline : {r.mesh_pipeline, r.shadow_pipeline, r.sky_pipeline, r.particle_pipeline,
                          r.compute_pipeline, r.blur_pipeline, r.post_pipeline, r.ui_pipeline})
      vkDestroyPipeline(r.device, pipeline, nullptr);
    vkDestroySampler(r.device, r.sampler, nullptr);
    vkDestroySampler(r.device, r.shadow_sampler, nullptr);
    vkDestroyPipelineLayout(r.device, r.pipeline_layout, nullptr);
    vkDestroyDescriptorSetLayout(r.device, r.set_layout, nullptr);
    if (r.swapchain) vkDestroySwapchainKHR(r.device, r.swapchain, nullptr);
    vkDestroyDevice(r.device, nullptr);
  }
  if (r.surface) vkDestroySurfaceKHR(r.instance, r.surface, nullptr);
#if defined(NATIVE_BUTTONS_VULKAN_VALIDATION)
  if (r.messenger) {
    auto destroy_messenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(r.instance, "vkDestroyDebugUtilsMessengerEXT"));
    destroy_messenger(r.instance, r.messenger, nullptr);
  }
#endif
  if (r.instance) vkDestroyInstance(r.instance, nullptr);
  delete renderer;
}

RenderStats get_stats(const Renderer& r) {
  return {r.width,
          r.height,
          r.render_width,
          r.render_height,
          static_cast<int>(kSamples),
          r.particle_count,
          r.triangle_count,
          r.gpu_millis,
          r.queries != VK_NULL_HANDLE};
}

std::string_view get_device(const Renderer& r) { return r.properties.deviceName; }
#if defined(NATIVE_BUTTONS_VULKAN_VALIDATION)
unsigned validation_error_count() { return validation_errors.load(); }
#endif
#undef VK_CHECK
}
