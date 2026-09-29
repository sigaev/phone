// Every GEMM kernel variant on shapes that exercise its edges: unaligned M and
// N, batches, single rows, a B too tall for one image row per matrix row, K
// split into slices, and narrow rasters. Each run is checked on exact data and on
// random data.
#include <cstdio>

#include "gemm/bench.h"
#include "gemm/device.h"

namespace {
constexpr gemm::Shape kShapes[] = {
    {"aligned", 1, 64, 64, 64},
    {"unaligned", 1, 77, 111, 128},
    {"batched", 3, 33, 50, 96},
    {"batched, image-aligned", 3, 33, 50, 128},
    {"single row", 1, 1, 200, 256},
    {"few rows", 2, 4, 100, 64},
    // 71,680 rows of B exceed the 65,536-row image limit, so rows are folded.
    {"folded", 70, 1, 1024, 64},
};

// Shapes run with K split into slices, by variants that can split K, and with
// fewer block rows swept together than usual, including a partial last band.
constexpr struct {
  gemm::Shape shape;
  gemm::Launch launch;
} kLaunchShapes[] = {
    {{"unaligned, K in three slices, raster 2", 1, 77, 111, 384}, {3, 2}},
    {{"batched, K in two slices, raster 1", 3, 33, 50, 256}, {2, 1}},
};
}

int main() {
  auto device = gemm::create_device();
  if (!device) return std::fprintf(stderr, "%s\n", device.error().message.c_str()), 1;
  unsigned runs = 0, failures = 0;
  auto verify = [&](const gemm::Variant& variant, const gemm::Shape& shape, gemm::Launch launch) {
    auto run = gemm::verify_gemm(**device, variant, shape, launch);
    ++runs;
    if (run && run->passed()) return;
    ++failures;
    std::printf("FAILED: %.*s %.*s on %.*s: %s\n", int(gemm::get_name(variant.precision).size()),
                gemm::get_name(variant.precision).data(), int(variant.name.size()),
                variant.name.data(), int(shape.name.size()), shape.name.data(),
                !run                  ? run.error().message.c_str()
                : !run->exact.passed  ? run->exact.failure.c_str()
                : !run->random.passed ? run->random.failure.c_str()
                                      : "rotating sets disagree");
  };
  for (const auto& variant : gemm::get_variants()) {
    for (const auto& shape : kShapes)
      if (gemm::supports(variant, shape)) verify(variant, shape, {1});
    for (const auto& [shape, launch] : kLaunchShapes)
      if (variant.split && gemm::supports(variant, shape)) verify(variant, shape, launch);
  }
  std::printf("%u of %u kernel runs verified\n", runs - failures, runs);
  return failures ? 1 : 0;
}
