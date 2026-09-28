// Real Vulkan rendering with a deterministic display boundary. The mock delays
// window-size reports and Vulkan capabilities independently, and tracks
// presentation completion independently of the rendering fences.
#define VK_USE_PLATFORM_ANDROID_KHR
#include <android/native_window.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>
#if defined(NATIVE_BUTTONS_VULKAN_VALIDATION)
#include <dlfcn.h>
#endif

#include "common/gpu/renderer.h"
#include "native_buttons/controls.h"
#include "native_buttons/scene.h"

namespace {
int window_width = 320, window_height = 720, buffer_width = 320, buffer_height = 720;
VkSurfaceTransformFlagBitsKHR transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
VkResult acquire_result = VK_SUCCESS, present_result = VK_SUCCESS, creation_result = VK_SUCCESS;
unsigned creations = 0, presentation_waits = 0;
// The desired presentation time of the last present, or zero without one.
uint64_t desired_present_time = 0;
// Display timing reports that the next query returns, for a 120 Hz display.
constexpr uint64_t kRefresh = 8333333;
std::vector<VkPastPresentationTimingGOOGLE> reported_timings;

VKAPI_ATTR VkResult VKAPI_CALL refresh_cycle(VkDevice, VkSwapchainKHR,
                                             VkRefreshCycleDurationGOOGLE* duration) {
  duration->refreshDuration = kRefresh;
  return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL past_presentations(VkDevice, VkSwapchainKHR, uint32_t* count,
                                                  VkPastPresentationTimingGOOGLE* timings) {
  if (timings) {
    *count = std::min<uint32_t>(*count, reported_timings.size());
    std::copy_n(reported_timings.begin(), *count, timings);
    reported_timings.erase(reported_timings.begin(), reported_timings.begin() + *count);
  } else {
    *count = reported_timings.size();
  }
  return VK_SUCCESS;
}

bool hide_maintenance = false;
bool resize_on_acquire = false, resize_on_present = false;
bool cache_capabilities = false;
int cached_width = 0, cached_height = 0;
VkSurfaceTransformFlagBitsKHR cached_transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
VkResult capabilities_result = VK_SUCCESS;

void resize_window() {
  std::swap(window_width, window_height);
  transform = window_width > window_height ? VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR
                                           : VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
}

struct Image {
  VkImage handle = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
};

struct Swapchain {
  VkDevice device;
  std::vector<Image> images;
  unsigned next_image = 0;
  bool retired = false;
};

struct Presentation {
  VkFence fence;
  VkSemaphore semaphore;
  VkSwapchainKHR swapchain;
  bool observed = false;
};

std::vector<Presentation> presentations;

void require(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "%s\n", message);
    std::abort();
  }
}

void destroy(Swapchain* swapchain) noexcept {
  for (auto& image : swapchain->images) {
    vkDestroyImage(swapchain->device, image.handle, nullptr);
    vkFreeMemory(swapchain->device, image.memory, nullptr);
  }
  delete swapchain;
}

VkQueue queue = VK_NULL_HANDLE;
unsigned queue_family = 0;
}

extern "C" {
VKAPI_ATTR VkResult VKAPI_CALL __real_vkCreateInstance(const VkInstanceCreateInfo*,
                                                       const VkAllocationCallbacks*, VkInstance*);

VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateInstance(const VkInstanceCreateInfo* info,
                                                       const VkAllocationCallbacks* allocator,
                                                       VkInstance* instance) {
  if (hide_maintenance &&
      std::any_of(info->ppEnabledExtensionNames,
                  info->ppEnabledExtensionNames + info->enabledExtensionCount,
                  [](const char* name) {
                    return std::strcmp(name, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME) == 0;
                  }))
    return VK_ERROR_EXTENSION_NOT_PRESENT;
  return __real_vkCreateInstance(info, allocator, instance);
}

int32_t __wrap_ANativeWindow_getWidth(ANativeWindow*) {
  return cache_capabilities ? window_width : buffer_width;
}

int32_t __wrap_ANativeWindow_getHeight(ANativeWindow*) {
  return cache_capabilities ? window_height : buffer_height;
}

VKAPI_ATTR VkResult VKAPI_CALL
__wrap_vkCreateAndroidSurfaceKHR(VkInstance, const VkAndroidSurfaceCreateInfoKHR*,
                                 const VkAllocationCallbacks*, VkSurfaceKHR* surface) {
  *surface = reinterpret_cast<VkSurfaceKHR>(1);
  return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL __wrap_vkDestroySurfaceKHR(VkInstance, VkSurfaceKHR,
                                                      const VkAllocationCallbacks*) {}

VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice,
                                                                           unsigned family,
                                                                           VkSurfaceKHR,
                                                                           VkBool32* supported) {
  queue_family = family;
  *supported = VK_TRUE;
  return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR* capabilities) {
  if (capabilities_result != VK_SUCCESS) return capabilities_result;
  *capabilities = {};
  capabilities->minImageCount = 2;
  capabilities->maxImageCount = 3;
  capabilities->currentExtent = {unsigned(cache_capabilities ? cached_width : window_width),
                                 unsigned(cache_capabilities ? cached_height : window_height)};
  capabilities->minImageExtent = {1, 1};
  capabilities->maxImageExtent = {4096, 4096};
  capabilities->maxImageArrayLayers = 1;
  capabilities->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  capabilities->supportedTransforms =
      VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR |
      VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR;
  capabilities->currentTransform = cache_capabilities ? cached_transform : transform;
  capabilities->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateSwapchainKHR(VkDevice device,
                                                           const VkSwapchainCreateInfoKHR* info,
                                                           const VkAllocationCallbacks*,
                                                           VkSwapchainKHR* output) {
  if (info->oldSwapchain) reinterpret_cast<Swapchain*>(info->oldSwapchain)->retired = true;
  auto result = creation_result;
  creation_result = VK_SUCCESS;
  if (result != VK_SUCCESS) return result;
  require(
      info->imageExtent.width == unsigned(cache_capabilities ? cached_width : window_width) &&
          info->imageExtent.height == unsigned(cache_capabilities ? cached_height : window_height),
      "Swapchain ignored the advertised surface extent");
  require(info->preTransform == VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
          "Window-coordinate rendering declared an unimplemented pre-rotation");
  common::Owner<Swapchain> swapchain(new Swapchain{device, {}});
  swapchain->images.resize(3);
  for (auto& image : swapchain->images) {
    VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    create.imageType = VK_IMAGE_TYPE_2D;
    create.format = info->imageFormat;
    create.extent = {info->imageExtent.width, info->imageExtent.height, 1};
    create.mipLevels = create.arrayLayers = 1;
    create.samples = VK_SAMPLE_COUNT_1_BIT;
    create.tiling = VK_IMAGE_TILING_OPTIMAL;
    create.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    result = vkCreateImage(device, &create, nullptr, &image.handle);
    if (result != VK_SUCCESS) return result;
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, image.handle, &requirements);
    unsigned type = 0;
    while (!(requirements.memoryTypeBits & (1u << type))) ++type;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = type;
    result = vkAllocateMemory(device, &allocation, nullptr, &image.memory);
    if (result != VK_SUCCESS) return result;
    result = vkBindImageMemory(device, image.handle, image.memory, 0);
    if (result != VK_SUCCESS) return result;
  }
  vkGetDeviceQueue(device, queue_family, 0, &queue);
  buffer_width = window_width;
  buffer_height = window_height;
  ++creations;
  *output = reinterpret_cast<VkSwapchainKHR>(swapchain.release());
  return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL __wrap_vkDestroySwapchainKHR(VkDevice, VkSwapchainKHR handle,
                                                        const VkAllocationCallbacks*) {
  if (!handle) return;
  for (const auto& p : presentations)
    require(p.swapchain != handle || p.observed,
            "Destroyed swapchain before presentation completion");
  common::Owner<Swapchain> swapchain(reinterpret_cast<Swapchain*>(handle));
}

VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkGetSwapchainImagesKHR(VkDevice, VkSwapchainKHR handle,
                                                              unsigned* count, VkImage* images) {
  auto& swapchain = *reinterpret_cast<Swapchain*>(handle);
  if (images)
    for (unsigned i = 0; i < swapchain.images.size(); ++i) images[i] = swapchain.images[i].handle;
  *count = swapchain.images.size();
  return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkAcquireNextImageKHR(VkDevice, VkSwapchainKHR handle,
                                                            uint64_t, VkSemaphore semaphore,
                                                            VkFence fence, unsigned* index) {
  auto& swapchain = *reinterpret_cast<Swapchain*>(handle);
  require(!swapchain.retired, "Acquired from a retired swapchain");
  auto result = acquire_result;
  acquire_result = VK_SUCCESS;
  if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) return result;
  if (resize_on_acquire) {
    resize_on_acquire = false;
    resize_window();
  }
  *index = swapchain.next_image++ % swapchain.images.size();
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.signalSemaphoreCount = 1;
  submit.pSignalSemaphores = &semaphore;
  auto submitted = vkQueueSubmit(queue, 1, &submit, fence);
  return submitted == VK_SUCCESS ? result : submitted;
}

VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkQueuePresentKHR(VkQueue q, const VkPresentInfoKHR* info) {
  if (resize_on_present) {
    resize_on_present = false;
    resize_window();
  }
  const auto* completion = static_cast<const VkSwapchainPresentFenceInfoEXT*>(info->pNext);
  require(completion && completion->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT &&
              completion->swapchainCount == 1,
          "Presentation has no completion fence");
  const auto* timing = static_cast<const VkPresentTimesInfoGOOGLE*>(completion->pNext);
  require(!timing || (timing->sType == VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE &&
                      timing->swapchainCount == 1),
          "Presentation has invalid display timing");
  desired_present_time = timing ? timing->pTimes[0].desiredPresentTime : 0;
  auto result = present_result;
  present_result = VK_SUCCESS;
  if (result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY)
    return result;
  auto fence = completion->pFences[0];
  auto found = std::find_if(presentations.begin(), presentations.end(),
                            [&](const auto& p) { return p.fence == fence; });
  if (found == presentations.end())
    presentations.push_back({fence, info->pWaitSemaphores[0], info->pSwapchains[0]});
  else {
    require(found->observed, "Reused presentation fence before observing completion");
    found->observed = false;
  }
  VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.waitSemaphoreCount = 1;
  submit.pWaitSemaphores = info->pWaitSemaphores;
  submit.pWaitDstStageMask = &stage;
  auto submitted = vkQueueSubmit(q, 1, &submit, fence);
  if (cache_capabilities) {
    cached_width = window_width;
    cached_height = window_height;
    cached_transform = transform;
  }
  return submitted == VK_SUCCESS ? result : submitted;
}

VKAPI_ATTR VkResult VKAPI_CALL __real_vkWaitForFences(VkDevice, unsigned, const VkFence*, VkBool32,
                                                      uint64_t);

VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkWaitForFences(VkDevice device, unsigned count,
                                                      const VkFence* fences, VkBool32 all,
                                                      uint64_t timeout) {
  require(timeout != UINT64_MAX, "GPU wait has no deadline");
  auto result = __real_vkWaitForFences(device, count, fences, all, timeout);
  if (result == VK_SUCCESS)
    for (unsigned i = 0; i < count; ++i)
      for (auto& p : presentations)
        if (p.fence == fences[i]) {
          p.observed = true;
          ++presentation_waits;
        }
  return result;
}

VKAPI_ATTR void VKAPI_CALL __real_vkDestroySemaphore(VkDevice, VkSemaphore,
                                                     const VkAllocationCallbacks*);

VKAPI_ATTR void VKAPI_CALL __wrap_vkDestroySemaphore(VkDevice device, VkSemaphore semaphore,
                                                     const VkAllocationCallbacks* allocator) {
  for (const auto& p : presentations)
    require(p.semaphore != semaphore || p.observed,
            "Destroyed a semaphore still used by presentation");
  __real_vkDestroySemaphore(device, semaphore, allocator);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL __real_vkGetDeviceProcAddr(VkDevice, const char*);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL __wrap_vkGetDeviceProcAddr(VkDevice device,
                                                                    const char* name) {
  if (!std::strcmp(name, "vkGetRefreshCycleDurationGOOGLE"))
    return reinterpret_cast<PFN_vkVoidFunction>(refresh_cycle);
  if (!std::strcmp(name, "vkGetPastPresentationTimingGOOGLE"))
    return reinterpret_cast<PFN_vkVoidFunction>(past_presentations);
  return __real_vkGetDeviceProcAddr(device, name);
}

VKAPI_ATTR void VKAPI_CALL __real_vkDestroyFence(VkDevice, VkFence, const VkAllocationCallbacks*);

VKAPI_ATTR void VKAPI_CALL __wrap_vkDestroyFence(VkDevice device, VkFence fence,
                                                 const VkAllocationCallbacks* allocator) {
  for (const auto& p : presentations)
    require(p.fence != fence || p.observed, "Destroyed an incomplete presentation fence");
  std::erase_if(presentations, [&](const auto& p) { return p.fence == fence; });
  __real_vkDestroyFence(device, fence, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkDeviceWaitIdle(VkDevice) {
  require(false, "Device idle is not presentation completion");
  return VK_ERROR_UNKNOWN;
}
}  // extern "C"

#if defined(NATIVE_BUTTONS_VULKAN_VALIDATION)
namespace gpu {
unsigned validation_error_count();
}
#endif

int main() {
#if defined(NATIVE_BUTTONS_VULKAN_VALIDATION)
  using GetEnvironment = void* (*)();
  using SetPaths = void (*)(void*, void*, const std::string&);
  auto get_environment = reinterpret_cast<GetEnvironment>(
      dlsym(RTLD_DEFAULT, "_ZN7android11GraphicsEnv11getInstanceEv"));
  auto set_paths = reinterpret_cast<SetPaths>(
      dlsym(RTLD_DEFAULT,
            "_ZN7android11GraphicsEnv13setLayerPathsEPNS_21NativeLoaderNamespaceERKNSt3__"
            "112basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEE"));
  const char* path = std::getenv("NATIVE_BUTTONS_VULKAN_LAYER_PATH");
  require(get_environment && set_paths && path, "Validation bootstrap is unavailable");
  set_paths(get_environment(), nullptr, std::string(path));
#endif
  using namespace native_buttons;
  auto* window = reinterpret_cast<ANativeWindow*>(1);
  hide_maintenance = true;
  require(!gpu::create_renderer(window, get_scene_shaders()),
          "Missing presentation support was accepted");
  hide_maintenance = false;
  {
    auto renderer = gpu::create_renderer(window, get_scene_shaders());
    if (!renderer) {
      std::fprintf(stderr, "%s\n", renderer.error().message.c_str());
      return 1;
    }
    auto scene = create_scene(**renderer);
    require(bool(scene), "Cannot create presentation test scene");
    auto draw = [&](bool maximum = false) {
      return render_scene(**scene, 12, .34f, maximum, 7, 0, 60, true, {}, true, true, 1);
    };
    for (int i = 0; i < 5; ++i) {
      auto result = draw();
      require(result && *result, "Initial window frame failed");
    }
    for (auto orientation :
         {VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR, VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR,
          VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR, VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR}) {
      transform = orientation;
      bool sideways = orientation == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
                      orientation == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR;
      window_width = sideways ? 720 : 320;
      window_height = sideways ? 320 : 720;
      unsigned before = creations;
      auto changed = gpu::surface_changed(**renderer);
      require(changed && *changed && creations == before,
              "Idle surface check missed changed extent/transform or allocated targets");
      acquire_result = VK_SUBOPTIMAL_KHR;
      auto result = draw();
      require(result && *result && creations == before + 1,
              "Rotation did not replace the old targets");
      auto stats = gpu::get_stats(**renderer);
      require(stats.width == window_width && stats.height == window_height,
              "Rotation kept portrait dimensions");
      auto controls = layout_controls({0, 0, float(window_width), float(window_height)});
      require(hit_test(**scene, controls.add.x + controls.add.w * .5f,
                       controls.add.y + controls.add.h * .5f) == int(Control::kAdd),
              "Rotated control does not match its visible location");
      changed = gpu::surface_changed(**renderer);
      require(changed && !*changed, "Repaired surface still reports a geometry change");
      acquire_result = present_result = VK_SUBOPTIMAL_KHR;
      auto stable = draw();
      require(stable && *stable && creations == before + 1,
              "Stable compositor rotation caused continuous swapchain replacement");
    }
    // Android's Surface caches the DEFAULT_WIDTH/HEIGHT used by Vulkan until
    // queueBuffer. The independent native-window query changes immediately.
    // Repeated capabilities queries alone must not leave a paused view stale.
    cache_capabilities = true;
    cached_width = window_width;
    cached_height = window_height;
    cached_transform = transform;
    resize_window();
    auto pending = gpu::surface_changed(**renderer);
    require(pending && *pending, "Cached capabilities hid a late idle resize");
    auto refresh = draw();
    require(refresh && !*refresh, "The cache-refresh presentation acknowledged stale geometry");
    refresh = draw();
    require(refresh && *refresh, "Cached surface extent did not converge after presentation");
    pending = gpu::surface_changed(**renderer);
    require(pending && !*pending, "Refreshed geometry kept requesting frames");
    cache_capabilities = false;

    auto prepared = gpu::prepare_frame(**renderer, false);
    require(prepared && *prepared, "Cannot prepare the resize race");
    resize_window();
    auto raced = gpu::render(**renderer, {0, 3, 8}, {0, 1, 0}, 0, false);
    require(raced && !*raced, "A resize changed dimensions after layout");
    auto recovered = draw();
    require(recovered && *recovered, "Resize race did not recover");
    for (auto* resize : {&resize_on_acquire, &resize_on_present}) {
      auto old_controls = layout_controls({0, 0, float(window_width), float(window_height)});
      unsigned before = creations;
      *resize = true;
      auto deferred = draw();
      require(deferred && !*deferred && creations == before,
              "Resize during acquisition/presentation acknowledged a stale frame");
      require(hit_test(**scene, old_controls.add.x + old_controls.add.w * .5f,
                       old_controls.add.y + old_controls.add.h * .5f) == int(Control::kAdd),
              "Deferred presentation replaced the last completed touch layout");
      recovered = draw();
      auto stats = gpu::get_stats(**renderer);
      require(recovered && *recovered && creations == before + 1 && stats.width == window_width &&
                  stats.height == window_height,
              "Late presentation resize did not rebuild camera and render targets");
    }
    int previous_width = window_width;
    window_width = 0;
    auto changed = gpu::surface_changed(**renderer);
    auto unavailable = draw();
    require(changed && *changed && unavailable && !*unavailable,
            "Zero extent was not reported as a deferred surface change");
    window_width = previous_width;
    recovered = draw();
    require(recovered && *recovered, "Temporarily zero-sized surface did not recover");
    gpu::set_present_time(**renderer, 123456789);
    auto timed = draw();
    require(timed && *timed && desired_present_time == 123456789,
            "Presentation ignored its desired time");
    auto untimed = draw();
    require(untimed && *untimed && !desired_present_time, "A desired presentation time was reused");
    // Frames shown a refresh after their requested times keep a steady 60 Hz
    // cadence. Only a frame shown later than that, relative to its request, slipped.
    auto report = [](uint64_t frame, uint64_t delay) {
      uint64_t desired = 1000000000 + frame * 2 * kRefresh;
      reported_timings.push_back({unsigned(frame), desired, desired + delay, desired, 0});
    };
    for (uint64_t frame = 0; frame < 5; ++frame) report(frame, kRefresh + 2000000);
    require(gpu::take_late_frames(**renderer) == 0, "A steady presentation delay was late");
    report(5, 2 * kRefresh + 2000000);
    report(6, 2 * kRefresh + 2000000);
    require(gpu::take_late_frames(**renderer) == 1, "A slipped presentation was not late");
    capabilities_result = VK_ERROR_SURFACE_LOST_KHR;
    require(!gpu::surface_changed(**renderer), "Idle surface query error was ignored");
    capabilities_result = VK_SUCCESS;
    for (auto* fault : {&acquire_result, &present_result, &creation_result}) {
      *fault = VK_ERROR_OUT_OF_DATE_KHR;
      bool maximum = fault == &creation_result;
      auto deferred = draw(maximum);
      require(deferred && !*deferred, "Out-of-date surface was not deferred");
      recovered = draw(maximum);
      require(recovered && *recovered, "Out-of-date surface did not recover");
    }
    present_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    require(!draw(true), "Presentation allocation failure was ignored");
    // Destruction must not wait for the unsignaled fence of that rejected present.
  }
  require(presentations.empty() && presentation_waits >= 5,
          "Presentation resources were not retired safely");
#if defined(NATIVE_BUTTONS_VULKAN_VALIDATION)
  require(gpu::validation_error_count() == 0, "Vulkan validation failed");
#endif
  std::puts(
      "Idle geometry checks, cached capabilities, four rotations, late resize races, zero extent "
      "and presentation "
      "retirement passed");
}
