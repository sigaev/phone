#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "common/result.h"
#include "gemm/check.h"
#include "gemm/device.h"
#include "gemm/measure.h"
#include "gemm/problem.h"
#include "gemm/sensors.h"

namespace gemm {

// One compiled GEMM kernel: each workgroup computes a block_m x block_n block
// of one batch item's C, and K must be a multiple of k_multiple.
struct Variant {
  std::string_view name;
  Precision precision;
  std::span<const std::uint32_t> spirv;
  unsigned block_m, block_n, k_multiple;
  // B is sampled through the texture unit from a linear image over its buffer,
  // in texels of this many bytes.
  bool b_image = false;
  unsigned texel_bytes = 16;
  unsigned max_m = 0;  // the most rows of A the kernel handles, or 0 for any
  // A is also sampled through the texture unit, from a linear image over its
  // buffer bound at binding 3, with the same texel size as B.
  bool a_image = false;
  // Workgroups are dispatched in one dimension and rastered over blocks of C.
  bool raster = false;
  // K can be split into slices summed by separate workgroups into partial sums,
  // which a second dispatch adds in a fixed order.
  bool split = false;
  unsigned subgroups = 1;  // per workgroup
};

// How a run dispatches a variant: the slices of K, and the block rows its
// workgroups sweep together (raster.glsl). Zero means the default.
struct Launch {
  unsigned split = 0, raster_rows = 0;
};

std::span<const Variant> get_variants();
// Variants of the tiled kernels with loads or multiplies removed. They compute
// wrong results; `gemm ablate` times them to show what limits each kernel.
std::span<const Variant> get_ablations();
// Whether the kernel handles the shape: its K step, its rows of A, and, for B
// read as an image, rows whose byte length the driver accepts as a linear
// image's row pitch.
bool supports(const Variant& variant, const Shape& shape);

struct GemmRun {
  Steady steady;
  Check exact, random;
  bool sets_agree = false;  // every rotating buffer set produced identical output
  unsigned sets = 0;
  Launch launch;
  double seconds = 0;  // per GEMM, at steady state

  bool passed() const { return exact.passed && random.passed && sets_agree; }
};

// Fills in the launch's defaults for the shape: see bench.cc. The GEMM_SPLIT and
// GEMM_RASTER environment variables override them, for experiments.
Launch choose_launch(const Variant& variant, const Shape& shape, Launch launch = {});

// Verifies the kernel on exact data, then measures it to steady state on random
// data rotated through enough buffer sets to exceed the GPU's caches, and
// verifies the timed output.
common::Result<GemmRun> run_gemm(Device& device, ClockSampler& clock, const Variant& variant,
                                 const Shape& shape, const SteadyOptions& options = {},
                                 Launch launch = {});
// The same checks on one run of the kernel, without measuring it.
common::Result<GemmRun> verify_gemm(Device& device, const Variant& variant, const Shape& shape,
                                    Launch launch = {});
}
