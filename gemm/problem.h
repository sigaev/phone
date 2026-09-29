#pragma once

#include <cstddef>
#include <span>
#include <string_view>

namespace gemm {

// C = A B^T for each batch item: A is M x K and B is N x K, both K-contiguous like
// a linear layer's activations and weights, and C is M x N, N-contiguous.
enum class Precision {
  kFp32,      // fp32 inputs, accumulation, and output
  kFp16,      // fp16 inputs, accumulation, and output
  kFp16Fp32,  // fp16 inputs and output, fp32 accumulation
  kInt8,      // int8 inputs, int32 accumulation and output
};

struct Shape {
  std::string_view name;
  unsigned batch, m, n, k;
};

std::string_view get_name(Precision precision);
std::size_t input_bytes(Precision precision);
std::size_t output_bytes(Precision precision);
std::size_t a_bytes(Precision precision, const Shape& shape);
std::size_t b_bytes(Precision precision, const Shape& shape);
std::size_t c_bytes(Precision precision, const Shape& shape);
double operations(const Shape& shape);  // 2 * batch * M * N * K

// Square problems, dense LLM layers at decode (one token) and prefill (512
// tokens), mixture-of-experts layers as batches of experts with their tokens,
// and skinny or unaligned problems.
std::span<const Shape> get_benchmark_shapes();
}
