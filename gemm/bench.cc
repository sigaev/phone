#include "gemm/bench.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace gemm {
using common::Error;
using common::Owner;
using common::Result;

namespace {
#include "gemm/ablations_table.inc"
#include "gemm/variants_table.inc"

// Adds split-K partial sums: fp32 into fp32 or fp16 outputs, int32 into int32.
constexpr std::uint32_t kReduceFp32[] =
#include "gemm/reduce_fp32.inc"
    ;
constexpr std::uint32_t kReduceFp16[] =
#include "gemm/reduce_fp16.inc"
    ;
constexpr std::uint32_t kReduceInt32[] =
#include "gemm/reduce_int32.inc"
    ;

// Rotating sets make repeated GEMMs read at least this much input, beyond the
// GPU L2 and system cache, like consecutive layers of a model.
constexpr std::size_t kRotationBytes = std::size_t(64) << 20;
constexpr unsigned kMaximumSets = 64;

struct Set {
  Owner<Buffer> a, b, c;
  Owner<Image> a_image, b_image;
};

// Rows of B per image row, as a power of two, so the image fits the device's
// dimension limit; the extra rows of the last image row fall in B's padding.
Result<unsigned> fold_shift(const Device& device, std::size_t rows, std::size_t texels,
                            unsigned padding_rows) {
  const unsigned limit = get_info(device).image_dimension;
  for (unsigned shift = 0; (1u << shift) <= padding_rows + 1; ++shift)
    if ((rows + (1u << shift) - 1) >> shift <= limit && (texels << shift) <= limit) return shift;
  return std::unexpected(Error{"B does not fit in an image"});
}
}

std::span<const Variant> get_variants() { return kVariants; }

std::span<const Variant> get_ablations() { return kAblations; }

bool supports(const Variant& v, const Shape& s) {
  // Mali's linear images need 64-byte row pitches.
  constexpr std::size_t kImageRowAlignment = 64;
  constexpr unsigned kImageRows = 65536;
  return s.k % v.k_multiple == 0 && (!v.max_m || s.m <= v.max_m) &&
         (!(v.b_image || v.a_image) || s.k * input_bytes(v.precision) % kImageRowAlignment == 0) &&
         (!v.a_image || std::size_t(s.batch) * s.m + v.block_m <= kImageRows);
}

namespace {
// Defaults measured on the large shapes with GEMM_SPLIT and GEMM_RASTER:
// - A grid of few workgroups leaves the cores short of subgroups to hide load
//   latency with, so K is split until the grid has kMinimumSubgroups, keeping
//   slices of at least kMinimumSlice values of K.
// - The rows of A for the block rows swept together are reused across every
//   block column only while they stay in the system cache, so the sweep narrows
//   from 8 block rows until that panel, over one slice of K, is at most
//   kPanelBytes: 4 rows instead of 8 doubled fp32 on the K = 14,336 shapes.
constexpr std::size_t kMinimumSubgroups = 1536, kMinimumSlice = 1024;
constexpr std::size_t kPanelBytes = std::size_t(8) << 20;

unsigned from_environment(const char* name) {
  const char* value = std::getenv(name);
  return value ? unsigned(std::atoi(value)) : 0;
}
}

Launch choose_launch(const Variant& v, const Shape& s, Launch launch) {
  if (!launch.split && v.split) {
    unsigned forced = from_environment("GEMM_SPLIT");
    if (forced && s.k % (forced * v.k_multiple) == 0) launch.split = forced;
  }
  if (!launch.split) {
    launch.split = 1;
    std::size_t subgroups = std::size_t((s.m + v.block_m - 1) / v.block_m) *
                            ((s.n + v.block_n - 1) / v.block_n) * s.batch * v.subgroups;
    while (v.split && subgroups * launch.split < kMinimumSubgroups &&
           s.k % (2 * launch.split * v.k_multiple) == 0 &&
           s.k / (2 * launch.split) >= kMinimumSlice)
      launch.split *= 2;
  }
  if (!launch.raster_rows) {
    launch.raster_rows = from_environment("GEMM_RASTER");
    if (!launch.raster_rows) {
      launch.raster_rows = 8;
      std::size_t row = std::size_t(v.block_m) * (s.k / launch.split) * input_bytes(v.precision);
      while (launch.raster_rows > 1 && launch.raster_rows * row > kPanelBytes)
        launch.raster_rows /= 2;
    }
  }
  return launch;
}

namespace {
Result<GemmRun> execute(Device& device, ClockSampler* clock, const Variant& v, const Shape& s,
                        const SteadyOptions& options, Launch launch) {
  if (!supports(v, s)) return std::unexpected(Error{"The kernel does not handle this shape"});
  launch = choose_launch(v, s, launch);
  const unsigned split = launch.split;
  if (split > 1 && (!v.split || s.k % (split * v.k_multiple)))
    return std::unexpected(Error{"The kernel cannot split this K"});
  // Rastered kernels dispatch every block of every slice in one dimension, which
  // Vulkan guarantees to 65,535 workgroups.
  std::size_t blocks =
      std::size_t((s.m + v.block_m - 1) / v.block_m) * ((s.n + v.block_n - 1) / v.block_n);
  if (v.raster && split * blocks > 65535)
    return std::unexpected(Error{"Too many workgroups for one dispatch"});
  const Precision p = v.precision;
  // Padding rows let edge tiles load whole tiles.
  std::size_t a = a_bytes(p, s), b = b_bytes(p, s), c = c_bytes(p, s);
  std::size_t a_padding = std::size_t(v.block_m) * s.k * input_bytes(p);
  std::size_t b_padding = std::size_t(v.block_n) * s.k * input_bytes(p);
  GemmRun run;
  run.launch = launch;
  run.sets =
      unsigned(std::clamp<std::size_t>((kRotationBytes + a + b - 1) / (a + b), 1, kMaximumSets));

  std::vector<Set> sets(run.sets);
  for (auto& set : sets) {
    auto created_a = create_buffer(device, a + a_padding);
    auto created_b = create_buffer(device, b + b_padding);
    auto created_c = create_buffer(device, c);
    if (!created_a) return std::unexpected(created_a.error());
    if (!created_b) return std::unexpected(created_b.error());
    if (!created_c) return std::unexpected(created_c.error());
    set.a = std::move(*created_a);
    set.b = std::move(*created_b);
    set.c = std::move(*created_c);
    std::memset(get_data(*set.a).data() + a, 0, a_padding);
    std::memset(get_data(*set.b).data() + b, 0, b_padding);
  }
  unsigned shift = 0;
  if (v.b_image) {
    std::size_t rows = std::size_t(s.batch) * s.n, texels = s.k * input_bytes(p) / v.texel_bytes;
    auto folded = fold_shift(device, rows, texels, v.block_n);
    if (!folded) return std::unexpected(folded.error());
    shift = *folded;
    for (auto& set : sets) {
      auto image = create_image(*set.b, 0, unsigned(texels << shift),
                                unsigned((rows + (1u << shift) - 1) >> shift), v.texel_bytes);
      if (!image) return std::unexpected(image.error());
      set.b_image = std::move(*image);
    }
  }
  if (v.a_image)
    for (auto& set : sets) {
      unsigned texels = unsigned(s.k * input_bytes(p) / v.texel_bytes);
      auto image = create_image(*set.a, 0, texels, s.batch * s.m + v.block_m, v.texel_bytes);
      if (!image) return std::unexpected(image.error());
      set.a_image = std::move(*image);
    }
  std::uint32_t specialization[] = {shift, s.m, s.n, s.k, split, launch.raster_rows};
  auto kernel = create_kernel(device, v.spirv, v.a_image ? 4 : 3, 0, specialization, 0,
                              (v.b_image ? 2 : 0) | (v.a_image ? 8 : 0));
  if (!kernel) return std::unexpected(kernel.error());
  // Split K: each workgroup sums one slice of K into that slice's copy of C in
  // 32-bit partial sums, and a second dispatch adds the copies into C. One buffer of
  // partial sums serves every set, as dispatches run one after another.
  const unsigned count = s.batch * s.m * s.n;
  Owner<Buffer> partials;
  Owner<Kernel> reduce;
  if (split > 1) {
    auto created = create_buffer(device, std::size_t(split) * count * 4);
    if (!created) return std::unexpected(created.error());
    partials = std::move(*created);
    std::uint32_t constants[] = {count, split};
    auto spirv = p == Precision::kInt8   ? std::span<const std::uint32_t>(kReduceInt32)
                 : p == Precision::kFp32 ? std::span<const std::uint32_t>(kReduceFp32)
                                         : std::span<const std::uint32_t>(kReduceFp16);
    auto created_reduce = create_kernel(device, spirv, 2, 0, constants);
    if (!created_reduce) return std::unexpected(created_reduce.error());
    reduce = std::move(*created_reduce);
  }
  const unsigned per_set = split > 1 ? 2 : 1;
  std::vector<Dispatch> dispatches;
  for (unsigned i = 0; i < run.sets; ++i) {
    Dispatch d;
    d.kernel = kernel->get();
    d.bindings[0] = {sets[i].a.get()};
    d.bindings[1] = {sets[i].b.get(), sets[i].b_image.get()};
    d.bindings[2] = {split > 1 ? partials.get() : sets[i].c.get()};
    if (v.a_image) d.bindings[3] = {sets[i].a.get(), sets[i].a_image.get()};

    unsigned blocks_n = (s.n + v.block_n - 1) / v.block_n,
             blocks_m = (s.m + v.block_m - 1) / v.block_m;
    d.groups = v.raster ? std::array<unsigned, 3>{split * blocks_n * blocks_m, 1, s.batch}
                        : std::array<unsigned, 3>{blocks_n, blocks_m, s.batch};
    dispatches.push_back(d);
    if (split > 1) {
      Dispatch r;
      r.kernel = reduce.get();
      r.bindings[0] = {partials.get()};
      r.bindings[1] = {sets[i].c.get()};
      unsigned groups = (count + 255) / 256, x = std::min(groups, 32768u);
      r.groups = {x, (groups + x - 1) / x, 1};
      dispatches.push_back(r);
    }
  }
  auto operands = [&](const Set& set) {
    return std::pair{get_data(*set.a).first(a), get_data(*set.b).first(b)};
  };
  auto output = [&](const Set& set) {
    return std::span<const std::byte>(get_data(*set.c).first(c));
  };

  // Exact data first: any wrong index or missed term changes the result.
  auto [exact_a, exact_b] = operands(sets[0]);
  fill_operands(p, s, Data::kExact, 1, exact_a, exact_b);
  if (auto r = flush(*sets[0].a); !r) return std::unexpected(r.error());
  if (auto r = flush(*sets[0].b); !r) return std::unexpected(r.error());
  auto batch = create_batch(device);
  if (!batch) return std::unexpected(batch.error());
  if (auto r = record(**batch, std::span(dispatches).first(per_set), 1); !r)
    return std::unexpected(r.error());
  if (auto r = submit(**batch); !r) return std::unexpected(r.error());
  if (auto r = wait(**batch); !r) return std::unexpected(r.error());
  if (auto r = invalidate(*sets[0].c); !r) return std::unexpected(r.error());
  run.exact = check_result(p, s, Data::kExact, exact_a, exact_b, output(sets[0]), 1);

  // Full-entropy data for the measurement, the same in every set.
  auto [random_a, random_b] = operands(sets[0]);
  fill_operands(p, s, Data::kRandom, 2, random_a, random_b);
  for (auto& set : sets) {
    if (&set != &sets[0]) {
      std::memcpy(get_data(*set.a).data(), random_a.data(), a);
      std::memcpy(get_data(*set.b).data(), random_b.data(), b);
    }
    std::memset(get_data(*set.c).data(), 0xff, c);
    for (Buffer* buffer : {set.a.get(), set.b.get(), set.c.get()})
      if (auto r = flush(*buffer); !r) return std::unexpected(r.error());
  }
  if (clock) {
    auto steady = measure_steady(device, *clock, dispatches, options);
    if (!steady) return std::unexpected(steady.error());
    run.steady = std::move(*steady);
    run.seconds = run.steady.seconds / run.sets;
  } else {
    if (auto r = record(**batch, dispatches, 1); !r) return std::unexpected(r.error());
    if (auto r = submit(**batch); !r) return std::unexpected(r.error());
    if (auto r = wait(**batch); !r) return std::unexpected(r.error());
  }

  // The timed dispatches' output: set 0 against references, the rest against set 0.
  for (auto& set : sets)
    if (auto r = invalidate(*set.c); !r) return std::unexpected(r.error());
  run.random = check_result(p, s, Data::kRandom, random_a, random_b, output(sets[0]), 2);
  run.sets_agree = true;
  for (auto& set : sets)
    run.sets_agree = run.sets_agree && !std::memcmp(output(set).data(), output(sets[0]).data(), c);
  return run;
}
}

Result<GemmRun> run_gemm(Device& device, ClockSampler& clock, const Variant& variant,
                         const Shape& shape, const SteadyOptions& options, Launch launch) {
  return execute(device, &clock, variant, shape, options, launch);
}

Result<GemmRun> verify_gemm(Device& device, const Variant& variant, const Shape& shape,
                            Launch launch) {
  return execute(device, nullptr, variant, shape, {}, launch);
}
}
