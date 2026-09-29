#include "gemm/device.h"

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstring>
#include <new>

namespace gemm {
using common::Error;
using common::Owner;
using common::Result;

namespace {
// Batches are sized to tens of milliseconds; a minute means the GPU is stuck.
constexpr std::uint64_t kWaitNanoseconds = 60'000'000'000;

std::unexpected<Error> fail(const char* message, VkResult result = VK_SUCCESS) {
  char text[512];
  if (result == VK_SUCCESS) std::snprintf(text, sizeof(text), "%s", message);
  else std::snprintf(text, sizeof(text), "%s (VkResult %d)", message, static_cast<int>(result));
  return std::unexpected(Error{text});
}

#define VK_CHECK(call)                                    \
  do {                                                    \
    const VkResult result = (call);                       \
    if (result != VK_SUCCESS) return fail(#call, result); \
  } while (false)

bool has_extension(std::span<const VkExtensionProperties> extensions, const char* name) {
  for (const auto& extension : extensions)
    if (!std::strcmp(extension.extensionName, name)) return true;
  return false;
}
}

struct Device {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkPhysicalDeviceMemoryProperties memory{};
  PFN_vkCmdPushDescriptorSet push_descriptor_set = nullptr;
  VkSampler sampler = VK_NULL_HANDLE;
  PFN_vkGetPipelineExecutablePropertiesKHR executable_properties = nullptr;
  PFN_vkGetPipelineExecutableStatisticsKHR executable_statistics = nullptr;
  PFN_vkGetPipelineExecutableInternalRepresentationsKHR executable_representations = nullptr;
  DeviceInfo info;
};

struct View {
  std::size_t offset, size;
  VkBufferView handle;
};

struct Buffer {
  Device* device = nullptr;
  std::vector<View> views;
  VkBuffer handle = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  std::byte* mapped = nullptr;
  std::size_t size = 0;
  bool coherent = false;
};

struct Image {
  Device* device = nullptr;
  VkImage handle = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  unsigned width = 0, height = 0;
  bool owns_memory = true;
};

struct Kernel {
  Device* device = nullptr;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  unsigned buffers = 0, push_bytes = 0, texel_mask = 0, image_mask = 0;
};

struct Batch {
  Device* device = nullptr;
  VkCommandBuffer commands = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  VkQueryPool queries = VK_NULL_HANDLE;
  bool recorded = false, pending = false;
};

void destroy(Device* d) noexcept {
  if (!d) return;
  if (d->device) {
    vkDeviceWaitIdle(d->device);
    vkDestroySampler(d->device, d->sampler, nullptr);
    vkDestroyCommandPool(d->device, d->pool, nullptr);
    vkDestroyDevice(d->device, nullptr);
  }
  if (d->instance) vkDestroyInstance(d->instance, nullptr);
  delete d;
}

Result<Owner<Device>> create_device() {
  Owner<Device> owner(new (std::nothrow) Device);
  if (!owner) return fail("Cannot allocate the device");
  auto& d = *owner;
  VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  application.pApplicationName = "gemm";
  application.apiVersion = VK_API_VERSION_1_4;
  VkInstanceCreateInfo instance{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  instance.pApplicationInfo = &application;
  VK_CHECK(vkCreateInstance(&instance, nullptr, &d.instance));
  unsigned count = 1;
  VkResult enumerated = vkEnumeratePhysicalDevices(d.instance, &count, &d.physical);
  if ((enumerated != VK_SUCCESS && enumerated != VK_INCOMPLETE) || !count)
    return fail("No Vulkan GPU", enumerated);

  VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  VkPhysicalDeviceVulkan11Properties vulkan11{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
  VkPhysicalDeviceVulkan12Properties vulkan12{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
  VkPhysicalDeviceShaderCorePropertiesARM core{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CORE_PROPERTIES_ARM};
  VkPhysicalDeviceShaderCoreBuiltinsPropertiesARM builtins{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CORE_BUILTINS_PROPERTIES_ARM};
  unsigned extension_count = 0;
  VK_CHECK(vkEnumerateDeviceExtensionProperties(d.physical, nullptr, &extension_count, nullptr));
  std::vector<VkExtensionProperties> extensions(extension_count);
  VK_CHECK(vkEnumerateDeviceExtensionProperties(d.physical, nullptr, &extension_count,
                                                extensions.data()));
  bool arm_core = has_extension(extensions, VK_ARM_SHADER_CORE_PROPERTIES_EXTENSION_NAME) &&
                  has_extension(extensions, VK_ARM_SHADER_CORE_BUILTINS_EXTENSION_NAME);
  bool matrices = has_extension(extensions, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
  bool executables =
      has_extension(extensions, VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
  properties.pNext = &vulkan11;
  vulkan11.pNext = &vulkan12;
  if (arm_core) {
    vulkan12.pNext = &core;
    core.pNext = &builtins;
  }
  vkGetPhysicalDeviceProperties2(d.physical, &properties);
  vkGetPhysicalDeviceMemoryProperties(d.physical, &d.memory);
  d.info.name = properties.properties.deviceName;
  d.info.driver = vulkan12.driverInfo;
  d.info.subgroup_size = vulkan11.subgroupSize;
  d.info.shared_memory = properties.properties.limits.maxComputeSharedMemorySize;
  d.info.timestamp_ns = properties.properties.limits.timestampPeriod;
  d.info.texel_buffer_elements = properties.properties.limits.maxTexelBufferElements;
  d.info.image_dimension = properties.properties.limits.maxImageDimension2D;
  if (arm_core) {
    d.info.cores = builtins.shaderCoreCount;
    d.info.fma_per_core_clock = core.fmaRate;
  }

  VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  VkPhysicalDeviceVulkan11Features supported11{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
  VkPhysicalDeviceVulkan12Features supported12{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  VkPhysicalDeviceVulkan13Features supported13{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceVulkan14Features supported14{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
  VkPhysicalDeviceCooperativeMatrixFeaturesKHR supported_matrix{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
  VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR supported_executables{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
  supported.pNext = &supported11;
  supported11.pNext = &supported12;
  supported12.pNext = &supported13;
  supported13.pNext = &supported14;
  supported14.pNext = &supported_matrix;
  supported_matrix.pNext = &supported_executables;
  vkGetPhysicalDeviceFeatures2(d.physical, &supported);
  if (!supported.features.shaderInt16 || !supported11.storageBuffer16BitAccess ||
      !supported12.shaderFloat16 || !supported12.shaderInt8 ||
      !supported12.storageBuffer8BitAccess || !supported12.vulkanMemoryModel ||
      !supported13.shaderIntegerDotProduct || !supported13.subgroupSizeControl ||
      !supported13.computeFullSubgroups || !supported13.synchronization2 ||
      !supported14.maintenance5)
    return fail("The GPU lacks a required Vulkan feature");
  matrices = matrices && supported_matrix.cooperativeMatrix;
  executables = executables && supported_executables.pipelineExecutableInfo;

  VkPhysicalDeviceFeatures2 enabled{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  VkPhysicalDeviceVulkan11Features enabled11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
  VkPhysicalDeviceVulkan12Features enabled12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  VkPhysicalDeviceVulkan13Features enabled13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceVulkan14Features enabled14{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
  VkPhysicalDeviceCooperativeMatrixFeaturesKHR enabled_matrix{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
  VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR enabled_executables{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
  enabled_executables.pipelineExecutableInfo = VK_TRUE;
  enabled.features.shaderInt16 = VK_TRUE;
  enabled.features.shaderInt64 = supported.features.shaderInt64;
  enabled11.storageBuffer16BitAccess = VK_TRUE;
  enabled12.shaderFloat16 = enabled12.shaderInt8 = enabled12.storageBuffer8BitAccess = VK_TRUE;
  enabled12.vulkanMemoryModel = VK_TRUE;
  enabled12.vulkanMemoryModelDeviceScope = supported12.vulkanMemoryModelDeviceScope;
  enabled13.shaderIntegerDotProduct = enabled13.subgroupSizeControl = VK_TRUE;
  enabled13.computeFullSubgroups = enabled13.synchronization2 = VK_TRUE;
  enabled14.maintenance5 = VK_TRUE;
  enabled_matrix.cooperativeMatrix = VK_TRUE;
  enabled.pNext = &enabled11;
  enabled11.pNext = &enabled12;
  enabled12.pNext = &enabled13;
  enabled13.pNext = &enabled14;
  std::vector<const char*> enabled_extensions;
  void** tail = &enabled14.pNext;
  if (matrices) {
    enabled_extensions.push_back(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
    *tail = &enabled_matrix;
    tail = &enabled_matrix.pNext;
  }
  if (executables) {
    enabled_extensions.push_back(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
    *tail = &enabled_executables;
  }

  unsigned families = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(d.physical, &families, nullptr);
  std::vector<VkQueueFamilyProperties> family_properties(families);
  vkGetPhysicalDeviceQueueFamilyProperties(d.physical, &families, family_properties.data());
  unsigned family = families;
  for (unsigned i = 0; i < families && family == families; ++i)
    if ((family_properties[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
        family_properties[i].timestampValidBits >= 64)
      family = i;
  if (family == families) return fail("No compute queue with 64-bit timestamps");
  float priority = 1;
  VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue.queueFamilyIndex = family;
  queue.queueCount = 1;
  queue.pQueuePriorities = &priority;
  VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  device.pNext = &enabled;
  device.queueCreateInfoCount = 1;
  device.pQueueCreateInfos = &queue;
  device.enabledExtensionCount = unsigned(enabled_extensions.size());
  device.ppEnabledExtensionNames = enabled_extensions.data();
  VK_CHECK(vkCreateDevice(d.physical, &device, nullptr, &d.device));
  vkGetDeviceQueue(d.device, family, 0, &d.queue);
  VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool.queueFamilyIndex = family;
  VK_CHECK(vkCreateCommandPool(d.device, &pool, nullptr, &d.pool));
  // NDK r29's API 35 stub library predates Vulkan 1.4.
  d.push_descriptor_set = reinterpret_cast<PFN_vkCmdPushDescriptorSet>(
      vkGetDeviceProcAddr(d.device, "vkCmdPushDescriptorSet"));
  if (!d.push_descriptor_set) return fail("The driver lacks vkCmdPushDescriptorSet");
  // Images are read with texelFetch, so filtering does not matter.
  VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VK_CHECK(vkCreateSampler(d.device, &sampler, nullptr, &d.sampler));
  if (executables) {
    d.executable_properties = reinterpret_cast<PFN_vkGetPipelineExecutablePropertiesKHR>(
        vkGetDeviceProcAddr(d.device, "vkGetPipelineExecutablePropertiesKHR"));
    d.executable_statistics = reinterpret_cast<PFN_vkGetPipelineExecutableStatisticsKHR>(
        vkGetDeviceProcAddr(d.device, "vkGetPipelineExecutableStatisticsKHR"));
    d.executable_representations =
        reinterpret_cast<PFN_vkGetPipelineExecutableInternalRepresentationsKHR>(
            vkGetDeviceProcAddr(d.device, "vkGetPipelineExecutableInternalRepresentationsKHR"));
  }

  if (matrices) {
    auto get_shapes = reinterpret_cast<PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR>(
        vkGetInstanceProcAddr(d.instance, "vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR"));
    unsigned shapes = 0;
    if (get_shapes && get_shapes(d.physical, &shapes, nullptr) == VK_SUCCESS) {
      std::vector<VkCooperativeMatrixPropertiesKHR> list(
          shapes, {VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR});
      if (get_shapes(d.physical, &shapes, list.data()) == VK_SUCCESS)
        for (const auto& shape : list)
          if (shape.scope == VK_SCOPE_SUBGROUP_KHR && shape.AType == shape.BType &&
              shape.CType == shape.ResultType)
            d.info.matrix_shapes.push_back(
                {shape.MSize, shape.NSize, shape.KSize, shape.AType, shape.CType});
    }
  }
  return owner;
}

const DeviceInfo& get_info(const Device& device) { return device.info; }

void destroy(Buffer* b) noexcept {
  if (!b) return;
  if (b->device) {
    for (const auto& view : b->views) vkDestroyBufferView(b->device->device, view.handle, nullptr);
    vkDestroyBuffer(b->device->device, b->handle, nullptr);
    vkFreeMemory(b->device->device, b->memory, nullptr);
  }
  delete b;
}

Result<Owner<Buffer>> create_buffer(Device& d, std::size_t bytes, Memory memory) {
  Owner<Buffer> owner(new (std::nothrow) Buffer);
  if (!owner) return fail("Cannot allocate a buffer");
  auto& b = *owner;
  b.device = &d;
  b.size = bytes;
  b.coherent = memory == Memory::kCoherent;
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = bytes;
  info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
               VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VK_CHECK(vkCreateBuffer(d.device, &info, nullptr, &b.handle));
  VkMemoryRequirements requirements;
  vkGetBufferMemoryRequirements(d.device, b.handle, &requirements);
  VkMemoryPropertyFlags flags =
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      (b.coherent ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
  unsigned type = d.memory.memoryTypeCount;
  for (unsigned i = 0; i < d.memory.memoryTypeCount && type == d.memory.memoryTypeCount; ++i)
    if ((requirements.memoryTypeBits & (1u << i)) &&
        (d.memory.memoryTypes[i].propertyFlags & flags) == flags)
      type = i;
  if (type == d.memory.memoryTypeCount) return fail("No host-visible device memory");
  b.coherent = d.memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocate.allocationSize = requirements.size;
  allocate.memoryTypeIndex = type;
  VK_CHECK(vkAllocateMemory(d.device, &allocate, nullptr, &b.memory));
  VK_CHECK(vkBindBufferMemory(d.device, b.handle, b.memory, 0));
  void* mapped = nullptr;
  VK_CHECK(vkMapMemory(d.device, b.memory, 0, VK_WHOLE_SIZE, 0, &mapped));
  b.mapped = static_cast<std::byte*>(mapped);
  return owner;
}

std::span<std::byte> get_data(Buffer& buffer) { return {buffer.mapped, buffer.size}; }

Result<void> flush(Buffer& b) {
  if (b.coherent) return {};
  VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
  range.memory = b.memory;
  range.size = VK_WHOLE_SIZE;
  VK_CHECK(vkFlushMappedMemoryRanges(b.device->device, 1, &range));
  return {};
}

Result<void> invalidate(Buffer& b) {
  if (b.coherent) return {};
  VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
  range.memory = b.memory;
  range.size = VK_WHOLE_SIZE;
  VK_CHECK(vkInvalidateMappedMemoryRanges(b.device->device, 1, &range));
  return {};
}

void destroy(Image* image) noexcept {
  if (!image) return;
  if (image->device) {
    VkDevice device = image->device->device;
    vkDestroyImageView(device, image->view, nullptr);
    vkDestroyImage(device, image->handle, nullptr);
    if (image->owns_memory) vkFreeMemory(device, image->memory, nullptr);
  }
  delete image;
}

Result<Owner<Image>> create_image(Device& d, unsigned width, unsigned height) {
  if (!width || !height || width > d.info.image_dimension || height > d.info.image_dimension)
    return fail("Unsupported image size");
  Owner<Image> owner(new (std::nothrow) Image);
  if (!owner) return fail("Cannot allocate an image");
  auto& image = *owner;
  image.device = &d;
  image.width = width;
  image.height = height;
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = VK_FORMAT_R32G32B32A32_UINT;
  info.extent = {width, height, 1};
  info.mipLevels = info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  VK_CHECK(vkCreateImage(d.device, &info, nullptr, &image.handle));
  VkMemoryRequirements requirements;
  vkGetImageMemoryRequirements(d.device, image.handle, &requirements);
  unsigned type = d.memory.memoryTypeCount;
  for (unsigned i = 0; i < d.memory.memoryTypeCount && type == d.memory.memoryTypeCount; ++i)
    if ((requirements.memoryTypeBits & (1u << i)) &&
        (d.memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
      type = i;
  if (type == d.memory.memoryTypeCount) return fail("No device memory for the image");
  VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocate.allocationSize = requirements.size;
  allocate.memoryTypeIndex = type;
  VK_CHECK(vkAllocateMemory(d.device, &allocate, nullptr, &image.memory));
  VK_CHECK(vkBindImageMemory(d.device, image.handle, image.memory, 0));
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = image.handle;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = info.format;
  view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VK_CHECK(vkCreateImageView(d.device, &view, nullptr, &image.view));
  return owner;
}

Result<Owner<Image>> create_image(Buffer& buffer, std::size_t offset, unsigned width,
                                  unsigned height, unsigned texel_bytes) {
  Device& d = *buffer.device;
  VkFormat texel = texel_bytes == 4   ? VK_FORMAT_R32_UINT
                   : texel_bytes == 8 ? VK_FORMAT_R32G32_UINT
                                      : VK_FORMAT_R32G32B32A32_UINT;
  if (!width || !height || width > d.info.image_dimension || height > d.info.image_dimension ||
      (texel_bytes != 4 && texel_bytes != 8 && texel_bytes != 16) ||
      offset + std::size_t(width) * height * texel_bytes > buffer.size)
    return fail("Unsupported linear image");
  VkFormatProperties format;
  vkGetPhysicalDeviceFormatProperties(d.physical, texel, &format);
  if (!(format.linearTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
    return fail("The GPU cannot sample linear images of this format");
  Owner<Image> owner(new (std::nothrow) Image);
  if (!owner) return fail("Cannot allocate an image");
  auto& image = *owner;
  image.device = &d;
  image.width = width;
  image.height = height;
  image.owns_memory = false;
  image.memory = buffer.memory;
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = texel;
  info.extent = {width, height, 1};
  info.mipLevels = info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_LINEAR;
  info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
  info.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
  VK_CHECK(vkCreateImage(d.device, &info, nullptr, &image.handle));
  VkImageSubresource subresource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
  VkSubresourceLayout layout;
  vkGetImageSubresourceLayout(d.device, image.handle, &subresource, &layout);
  VkMemoryRequirements requirements;
  vkGetImageMemoryRequirements(d.device, image.handle, &requirements);
  if (layout.offset || layout.rowPitch != std::size_t(width) * texel_bytes ||
      offset % requirements.alignment || !(requirements.memoryTypeBits & (1u << 1 | 1u << 0)))
    return fail("The linear image's layout does not match the buffer's rows");
  VK_CHECK(vkBindImageMemory(d.device, image.handle, buffer.memory, offset));
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = image.handle;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = info.format;
  view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VK_CHECK(vkCreateImageView(d.device, &view, nullptr, &image.view));
  // Move the image to its sampled layout once; the contents are preserved from
  // the preinitialized layout.
  VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  allocate.commandPool = d.pool;
  allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocate.commandBufferCount = 1;
  VkCommandBuffer commands;
  VK_CHECK(vkAllocateCommandBuffers(d.device, &allocate, &commands));
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(commands, &begin);
  VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  barrier.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
  barrier.srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT;
  barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
  barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  barrier.image = image.handle;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.imageMemoryBarrierCount = 1;
  dependency.pImageMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(commands, &dependency);
  vkEndCommandBuffer(commands);
  VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence;
  VK_CHECK(vkCreateFence(d.device, &fence_info, nullptr, &fence));
  VkCommandBufferSubmitInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
  command_info.commandBuffer = commands;
  VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
  submit.commandBufferInfoCount = 1;
  submit.pCommandBufferInfos = &command_info;
  VkResult result = vkQueueSubmit2(d.queue, 1, &submit, fence);
  if (result == VK_SUCCESS)
    result = vkWaitForFences(d.device, 1, &fence, VK_TRUE, kWaitNanoseconds);
  vkDestroyFence(d.device, fence, nullptr);
  vkFreeCommandBuffers(d.device, d.pool, 1, &commands);
  if (result != VK_SUCCESS) return fail("Cannot prepare the linear image", result);
  return owner;
}

Result<void> upload(Image& image, Buffer& source) {
  if (source.size < std::size_t(image.width) * image.height * 16)
    return fail("The source buffer is smaller than the image");
  Device& d = *image.device;
  VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  allocate.commandPool = d.pool;
  allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocate.commandBufferCount = 1;
  VkCommandBuffer commands;
  VK_CHECK(vkAllocateCommandBuffers(d.device, &allocate, &commands));
  VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence;
  VK_CHECK(vkCreateFence(d.device, &fence_info, nullptr, &fence));
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(commands, &begin);
  VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
  barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.image = image.handle;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.imageMemoryBarrierCount = 1;
  dependency.pImageMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(commands, &dependency);
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {image.width, image.height, 1};
  vkCmdCopyBufferToImage(commands, source.handle, image.handle,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
  barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  vkCmdPipelineBarrier2(commands, &dependency);
  vkEndCommandBuffer(commands);
  VkCommandBufferSubmitInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
  command_info.commandBuffer = commands;
  VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
  submit.commandBufferInfoCount = 1;
  submit.pCommandBufferInfos = &command_info;
  VkResult result = vkQueueSubmit2(d.queue, 1, &submit, fence);
  if (result == VK_SUCCESS)
    result = vkWaitForFences(d.device, 1, &fence, VK_TRUE, kWaitNanoseconds);
  vkDestroyFence(d.device, fence, nullptr);
  vkFreeCommandBuffers(d.device, d.pool, 1, &commands);
  if (result != VK_SUCCESS) return fail("Cannot upload the image", result);
  return {};
}

void destroy(Kernel* k) noexcept {
  if (!k) return;
  if (k->device) {
    VkDevice device = k->device->device;
    vkDestroyPipeline(device, k->pipeline, nullptr);
    vkDestroyPipelineLayout(device, k->layout, nullptr);
    vkDestroyDescriptorSetLayout(device, k->set_layout, nullptr);
  }
  delete k;
}

Result<Owner<Kernel>> create_kernel(Device& d, std::span<const std::uint32_t> spirv,
                                    unsigned buffers, unsigned push_bytes,
                                    std::span<const std::uint32_t> specialization,
                                    unsigned texel_mask, unsigned image_mask) {
  if (buffers > std::tuple_size_v<decltype(Dispatch::bindings)> ||
      push_bytes > sizeof(Dispatch::push) || push_bytes % 4)
    return fail("Unsupported kernel interface");
  Owner<Kernel> owner(new (std::nothrow) Kernel);
  if (!owner) return fail("Cannot allocate a kernel");
  auto& k = *owner;
  k.device = &d;
  k.buffers = buffers;
  k.push_bytes = push_bytes;
  k.texel_mask = texel_mask;
  k.image_mask = image_mask;
  VkDescriptorSetLayoutBinding bindings[4]{};
  for (unsigned i = 0; i < buffers; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType = image_mask & (1u << i) ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                 : texel_mask & (1u << i) ? VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER
                                                          : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT;
  set.bindingCount = buffers;
  set.pBindings = bindings;
  VK_CHECK(vkCreateDescriptorSetLayout(d.device, &set, nullptr, &k.set_layout));
  VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes};
  VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout.setLayoutCount = 1;
  layout.pSetLayouts = &k.set_layout;
  layout.pushConstantRangeCount = push_bytes ? 1 : 0;
  layout.pPushConstantRanges = &range;
  VK_CHECK(vkCreatePipelineLayout(d.device, &layout, nullptr, &k.layout));

  std::vector<VkSpecializationMapEntry> entries(specialization.size());
  for (unsigned i = 0; i < entries.size(); ++i) entries[i] = {i, 4 * i, 4};
  VkSpecializationInfo values{static_cast<unsigned>(entries.size()), entries.data(),
                              specialization.size_bytes(), specialization.data()};
  VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  module.codeSize = spirv.size_bytes();
  module.pCode = spirv.data();
  VkPipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup{
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
  subgroup.pNext = &module;
  subgroup.requiredSubgroupSize = d.info.subgroup_size;
  VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  if (d.executable_statistics)
    pipeline.flags = VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR |
                     VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR;
  pipeline.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  pipeline.stage.pNext = &subgroup;
  pipeline.stage.flags = VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;
  pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  pipeline.stage.pName = "main";
  pipeline.stage.pSpecializationInfo = entries.empty() ? nullptr : &values;
  pipeline.layout = k.layout;
  VK_CHECK(vkCreateComputePipelines(d.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &k.pipeline));
  return owner;
}

std::string describe(const Kernel& k) {
  const Device& d = *k.device;
  if (!d.executable_statistics) return "The driver reports no compiler statistics\n";
  std::string text;
  char line[512];
  VkPipelineInfoKHR pipeline{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};
  pipeline.pipeline = k.pipeline;
  unsigned count = 0;
  if (d.executable_properties(d.device, &pipeline, &count, nullptr) != VK_SUCCESS) return text;
  std::vector<VkPipelineExecutablePropertiesKHR> executables(
      count, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR});
  d.executable_properties(d.device, &pipeline, &count, executables.data());
  for (unsigned i = 0; i < count; ++i) {
    std::snprintf(line, sizeof(line), "%s (%s), subgroup %u\n", executables[i].name,
                  executables[i].description, executables[i].subgroupSize);
    text += line;
    VkPipelineExecutableInfoKHR executable{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
    executable.pipeline = k.pipeline;
    executable.executableIndex = i;
    unsigned statistics = 0;
    d.executable_statistics(d.device, &executable, &statistics, nullptr);
    std::vector<VkPipelineExecutableStatisticKHR> values(
        statistics, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
    d.executable_statistics(d.device, &executable, &statistics, values.data());
    for (const auto& v : values) {
      double value =
          v.format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR  ? v.value.f64
          : v.format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR  ? double(v.value.i64)
          : v.format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR ? double(v.value.u64)
                                                                           : double(v.value.b32);
      std::snprintf(line, sizeof(line), "  %s: %g (%s)\n", v.name, value, v.description);
      text += line;
    }
    unsigned representations = 0;
    if (!d.executable_representations ||
        d.executable_representations(d.device, &executable, &representations, nullptr) !=
            VK_SUCCESS)
      continue;
    std::vector<VkPipelineExecutableInternalRepresentationKHR> list(
        representations, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR});
    d.executable_representations(d.device, &executable, &representations, list.data());
    std::vector<std::vector<char>> storage(representations);
    for (unsigned j = 0; j < representations; ++j) {
      storage[j].resize(list[j].dataSize + 1);
      list[j].pData = storage[j].data();
    }
    d.executable_representations(d.device, &executable, &representations, list.data());
    for (unsigned j = 0; j < representations; ++j) {
      std::snprintf(line, sizeof(line), "  representation %s (%s), %zu bytes:\n", list[j].name,
                    list[j].description, list[j].dataSize);
      text += line;
      if (list[j].isText) text += storage[j].data();
    }
  }
  return text;
}

KernelStats get_stats(const Kernel& k) {
  KernelStats stats;
  const Device& d = *k.device;
  if (!d.executable_statistics) return stats;
  VkPipelineExecutableInfoKHR executable{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
  executable.pipeline = k.pipeline;
  unsigned count = 0;
  if (d.executable_statistics(d.device, &executable, &count, nullptr) != VK_SUCCESS) return stats;
  std::vector<VkPipelineExecutableStatisticKHR> values(
      count, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
  d.executable_statistics(d.device, &executable, &count, values.data());
  const std::pair<const char*, double KernelStats::*> names[] = {
      {"Registers used", &KernelStats::registers},
      {"Spill size", &KernelStats::spill_bytes},
      {"WLS size", &KernelStats::workgroup_bytes},
      {"Arithmetic FMA (Valhall only)", &KernelStats::fma_cycles},
      {"Arithmetic CVT (Valhall only)", &KernelStats::cvt_cycles},
      {"Arithmetic SFU (Valhall only)", &KernelStats::sfu_cycles},
      {"Load Store", &KernelStats::load_store_cycles},
      {"Texture", &KernelStats::texture_cycles},
  };
  for (const auto& v : values)
    for (const auto& [name, field] : names)
      if (!std::strcmp(v.name, name)) {
        stats.*field = v.format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR
                           ? v.value.f64
                           : double(v.value.u64);
        stats.known = true;
      }
  return stats;
}

namespace {
// A cached RGBA32UI view of a buffer range, for texel-buffer bindings.
Result<VkBufferView> get_view(Buffer& b, std::size_t offset, std::size_t size) {
  if (!size) size = b.size - offset;
  for (const auto& view : b.views)
    if (view.offset == offset && view.size == size) return view.handle;
  VkBufferViewCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};
  info.buffer = b.handle;
  info.format = VK_FORMAT_R32G32B32A32_UINT;
  info.offset = offset;
  info.range = size;
  VkBufferView view;
  VK_CHECK(vkCreateBufferView(b.device->device, &info, nullptr, &view));
  b.views.push_back({offset, size, view});
  return view;
}
}

void destroy(Batch* b) noexcept {
  if (!b) return;
  if (b->device) {
    VkDevice device = b->device->device;
    if (b->pending) vkWaitForFences(device, 1, &b->fence, VK_TRUE, kWaitNanoseconds);
    vkDestroyQueryPool(device, b->queries, nullptr);
    vkDestroyFence(device, b->fence, nullptr);
    vkFreeCommandBuffers(device, b->device->pool, 1, &b->commands);
  }
  delete b;
}

Result<Owner<Batch>> create_batch(Device& d) {
  Owner<Batch> owner(new (std::nothrow) Batch);
  if (!owner) return fail("Cannot allocate a batch");
  auto& b = *owner;
  b.device = &d;
  VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  allocate.commandPool = d.pool;
  allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocate.commandBufferCount = 1;
  VK_CHECK(vkAllocateCommandBuffers(d.device, &allocate, &b.commands));
  VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VK_CHECK(vkCreateFence(d.device, &fence, nullptr, &b.fence));
  VkQueryPoolCreateInfo queries{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  queries.queryType = VK_QUERY_TYPE_TIMESTAMP;
  queries.queryCount = 2;
  VK_CHECK(vkCreateQueryPool(d.device, &queries, nullptr, &b.queries));
  return owner;
}

Result<void> record(Batch& b, std::span<const Dispatch> dispatches, unsigned repeats) {
  if (b.pending) return fail("Cannot record a pending batch");
  const Device& d = *b.device;
  VK_CHECK(vkResetCommandBuffer(b.commands, 0));
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  VK_CHECK(vkBeginCommandBuffer(b.commands, &begin));
  vkCmdResetQueryPool(b.commands, b.queries, 0, 2);
  VkMemoryBarrier2 memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  memory.srcStageMask = memory.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  memory.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  memory.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.memoryBarrierCount = 1;
  dependency.pMemoryBarriers = &memory;
  // Earlier batches finish before this one starts and before its start time is
  // taken, so batches in flight together time disjoint, back-to-back intervals.
  vkCmdPipelineBarrier2(b.commands, &dependency);
  vkCmdWriteTimestamp2(b.commands, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, b.queries, 0);
  const Kernel* bound = nullptr;
  bool first = true;
  for (unsigned repeat = 0; repeat < repeats; ++repeat)
    for (const auto& dispatch : dispatches) {
      const Kernel& k = *dispatch.kernel;
      if (!first) vkCmdPipelineBarrier2(b.commands, &dependency);
      first = false;
      if (bound != &k) vkCmdBindPipeline(b.commands, VK_PIPELINE_BIND_POINT_COMPUTE, k.pipeline);
      bound = &k;
      VkDescriptorBufferInfo buffers[4];
      VkDescriptorImageInfo images[4];
      VkBufferView views[4];
      VkWriteDescriptorSet writes[4];
      for (unsigned i = 0; i < k.buffers; ++i) {
        const Binding& binding = dispatch.bindings[i];
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        if (k.image_mask & (1u << i)) {
          if (!binding.image) return fail("A dispatch lacks an image");
          images[i] = {d.sampler, binding.image->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
          writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
          writes[i].pImageInfo = &images[i];
          continue;
        }
        if (!binding.buffer) return fail("A dispatch lacks a buffer");
        if (k.texel_mask & (1u << i)) {
          auto view = get_view(*binding.buffer, binding.offset, binding.size);
          if (!view) return std::unexpected(view.error());
          views[i] = *view;
          writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
          writes[i].pTexelBufferView = &views[i];
        } else {
          buffers[i] = {binding.buffer->handle, binding.offset,
                        binding.size ? binding.size : VK_WHOLE_SIZE};
          writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
          writes[i].pBufferInfo = &buffers[i];
        }
      }
      if (k.buffers)
        d.push_descriptor_set(b.commands, VK_PIPELINE_BIND_POINT_COMPUTE, k.layout, 0, k.buffers,
                              writes);
      if (k.push_bytes)
        vkCmdPushConstants(b.commands, k.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, k.push_bytes,
                           dispatch.push.data());
      vkCmdDispatch(b.commands, dispatch.groups[0], dispatch.groups[1], dispatch.groups[2]);
    }
  vkCmdWriteTimestamp2(b.commands, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, b.queries, 1);
  VK_CHECK(vkEndCommandBuffer(b.commands));
  b.recorded = true;
  return {};
}

Result<void> submit(Batch& b) {
  if (!b.recorded || b.pending) return fail("The batch is not ready to submit");
  VK_CHECK(vkResetFences(b.device->device, 1, &b.fence));
  VkCommandBufferSubmitInfo commands{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
  commands.commandBuffer = b.commands;
  VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
  submit.commandBufferInfoCount = 1;
  submit.pCommandBufferInfos = &commands;
  VK_CHECK(vkQueueSubmit2(b.device->queue, 1, &submit, b.fence));
  b.pending = true;
  return {};
}

Result<BatchTimes> wait(Batch& b) {
  if (!b.pending) return fail("The batch was not submitted");
  VK_CHECK(vkWaitForFences(b.device->device, 1, &b.fence, VK_TRUE, kWaitNanoseconds));
  b.pending = false;
  std::uint64_t times[2];
  VK_CHECK(vkGetQueryPoolResults(b.device->device, b.queries, 0, 2, sizeof(times), times,
                                 sizeof(times[0]), VK_QUERY_RESULT_64_BIT));
  if (times[1] < times[0]) return fail("GPU timestamps ran backwards");
  double period = b.device->info.timestamp_ns;
  return BatchTimes{double(times[0]) * period, double(times[1]) * period};
}
}
