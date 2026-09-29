#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "gemm/problem.h"

namespace gemm {

// kRandom draws full-precision values in [-1, 1), or all of int8. kExact draws
// sparse -1, 0, and 1, so every partial sum is a small integer and even fp16
// accumulation must be exact in any order.
enum class Data { kRandom, kExact };

void fill_operands(Precision precision, const Shape& shape, Data data, std::uint64_t seed,
                   std::span<std::byte> a, std::span<std::byte> b);

struct Check {
  bool passed = false;
  double worst = 0;  // largest error relative to its tolerance
  unsigned elements = 0, rows = 0, columns = 0;
  std::string failure;
};

// Compares sampled elements and randomized checksums of every row and column
// of C with double-precision references. Tolerances follow probabilistic
// rounding-error bounds for the precision's accumulator and output.
Check check_result(Precision precision, const Shape& shape, Data data, std::span<const std::byte> a,
                   std::span<const std::byte> b, std::span<const std::byte> c, std::uint64_t seed);
}
