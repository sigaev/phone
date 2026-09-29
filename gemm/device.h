#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "common/owner.h"
#include "common/result.h"

namespace gemm {
struct Device;
struct Buffer;
struct Kernel;
struct Batch;
struct Image;

// A cooperative matrix multiply the driver supports, C(MxN) += A(MxK) B(KxN).
struct MatrixShape {
  unsigned m, n, k;
  int a_type, c_type;  // VkComponentTypeKHR
};

struct DeviceInfo {
  std::string name, driver;
  unsigned cores = 0, fma_per_core_clock = 0, subgroup_size = 0, shared_memory = 0;
  unsigned texel_buffer_elements = 0, image_dimension = 0;
  double timestamp_ns = 0;
  std::vector<MatrixShape> matrix_shapes;
};

common::Result<common::Owner<Device>> create_device();
void destroy(Device* device) noexcept;
const DeviceInfo& get_info(const Device& device);

// Host-cached buffers read quickly on the CPU; they need flush after CPU writes and
// invalidate before CPU reads. Coherent buffers need neither.
enum class Memory { kCached, kCoherent };
common::Result<common::Owner<Buffer>> create_buffer(Device& device, std::size_t bytes,
                                                    Memory memory = Memory::kCached);
void destroy(Buffer* buffer) noexcept;
std::span<std::byte> get_data(Buffer& buffer);
common::Result<void> flush(Buffer& buffer);
common::Result<void> invalidate(Buffer& buffer);

// An RGBA32UI 2D image sampled through the texture unit, filled from a buffer's
// tightly packed rows of 16-byte texels. An image created over a buffer instead
// uses linear tiling in the buffer's own memory: its rows are the buffer's
// width * texel_bytes rows from `offset`, with no copy, and its texels are
// R32UI, RG32UI, or RGBA32UI for 4, 8, or 16 bytes.
common::Result<common::Owner<Image>> create_image(Device& device, unsigned width, unsigned height);
common::Result<common::Owner<Image>> create_image(Buffer& buffer, std::size_t offset,
                                                  unsigned width, unsigned height,
                                                  unsigned texel_bytes = 16);
void destroy(Image* image) noexcept;
common::Result<void> upload(Image& image, Buffer& source);

// Resources bind in order from binding 0: as storage buffers by default; where
// `texel_mask` has their bit, as RGBA32UI uniform texel buffers; where
// `image_mask` has it, as sampled images. Specialization values set constant_id
// 0, 1, ... The subgroup size is the device's, with full subgroups.
common::Result<common::Owner<Kernel>> create_kernel(
    Device& device, std::span<const std::uint32_t> spirv, unsigned buffers, unsigned push_bytes,
    std::span<const std::uint32_t> specialization = {}, unsigned texel_mask = 0,
    unsigned image_mask = 0);
void destroy(Kernel* kernel) noexcept;
// The driver compiler's statistics and internal representations, when it reports them.
std::string describe(const Kernel& kernel);

// The Mali compiler's per-thread estimates for the main compute executable.
struct KernelStats {
  bool known = false;
  double registers = 0, spill_bytes = 0, workgroup_bytes = 0;
  double fma_cycles = 0, cvt_cycles = 0, sfu_cycles = 0, load_store_cycles = 0, texture_cycles = 0;
};

KernelStats get_stats(const Kernel& kernel);

struct Binding {
  Buffer* buffer = nullptr;
  Image* image = nullptr;
  std::size_t offset = 0, size = 0;  // A size of 0 binds the rest of the buffer.
};

struct Dispatch {
  const Kernel* kernel = nullptr;
  std::array<Binding, 4> bindings{};
  std::array<std::uint32_t, 32> push{};
  std::array<unsigned, 3> groups{1, 1, 1};
};

// A recorded command buffer. Dispatches run in order, like kernels on one CUDA
// stream: each waits for the previous one's memory writes.
common::Result<common::Owner<Batch>> create_batch(Device& device);
void destroy(Batch* batch) noexcept;
common::Result<void> record(Batch& batch, std::span<const Dispatch> dispatches, unsigned repeats);
common::Result<void> submit(Batch& batch);

// A completed batch's GPU timestamps in nanoseconds: after all earlier work,
// when its first dispatch may start, and at the end of its last dispatch.
struct BatchTimes {
  double start_ns = 0, end_ns = 0;
};

common::Result<BatchTimes> wait(Batch& batch);
}
