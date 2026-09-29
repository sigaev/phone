#include "gemm/problem.h"

namespace gemm {

std::string_view get_name(Precision precision) {
  switch (precision) {
    case Precision::kFp32:
      return "fp32";
    case Precision::kFp16:
      return "fp16";
    case Precision::kFp16Fp32:
      return "fp16/fp32acc";
    case Precision::kInt8:
      return "int8";
  }
  return "?";
}

std::size_t input_bytes(Precision precision) {
  return precision == Precision::kFp32 ? 4 : precision == Precision::kInt8 ? 1 : 2;
}

std::size_t output_bytes(Precision precision) {
  return precision == Precision::kFp32 || precision == Precision::kInt8 ? 4 : 2;
}

std::size_t a_bytes(Precision precision, const Shape& s) {
  return std::size_t(s.batch) * s.m * s.k * input_bytes(precision);
}

std::size_t b_bytes(Precision precision, const Shape& s) {
  return std::size_t(s.batch) * s.n * s.k * input_bytes(precision);
}

std::size_t c_bytes(Precision precision, const Shape& s) {
  return std::size_t(s.batch) * s.m * s.n * output_bytes(precision);
}

double operations(const Shape& s) { return 2. * s.batch * s.m * s.n * s.k; }

namespace {
// Gate and up projections are fused, so their N is twice the intermediate size.
constexpr Shape kShapes[] = {
    {"square 256", 1, 256, 256, 256},
    {"square 512", 1, 512, 512, 512},
    {"square 1024", 1, 1024, 1024, 1024},
    {"square 2048", 1, 2048, 2048, 2048},
    {"square 4096", 1, 4096, 4096, 4096},
    // Llama 3 8B: hidden 4096, fused QKV 6144, intermediate 14336.
    {"llama3-8b decode qkv", 1, 1, 6144, 4096},
    {"llama3-8b decode gate+up", 1, 1, 28672, 4096},
    {"llama3-8b decode down", 1, 1, 4096, 14336},
    {"llama3-8b prefill qkv", 1, 512, 6144, 4096},
    {"llama3-8b prefill gate+up", 1, 512, 28672, 4096},
    {"llama3-8b prefill down", 1, 512, 4096, 14336},
    // Qwen3-30B-A3B: hidden 2048, expert intermediate 768, 8 of 128 experts per
    // token; 512 prefill tokens give 32 per expert.
    {"qwen3-30b-a3b decode gate+up", 8, 1, 1536, 2048},
    {"qwen3-30b-a3b decode down", 8, 1, 2048, 768},
    {"qwen3-30b-a3b prefill gate+up", 128, 32, 1536, 2048},
    {"qwen3-30b-a3b prefill down", 128, 32, 2048, 768},
    // gpt-oss-20b: hidden 2880, expert intermediate 2880, 4 of 32 experts; 64
    // tokens per expert from 512.
    {"gpt-oss-20b decode gate+up", 4, 1, 5760, 2880},
    {"gpt-oss-20b decode down", 4, 1, 2880, 2880},
    {"gpt-oss-20b prefill gate+up", 32, 64, 5760, 2880},
    {"gpt-oss-20b prefill down", 32, 64, 2880, 2880},
    // DeepSeek-V2-Lite: hidden 2048, expert intermediate 1408, 6 of 64 experts;
    // 48 tokens per expert from 512.
    {"deepseek-v2-lite prefill gate+up", 64, 48, 2816, 2048},
    {"deepseek-v2-lite prefill down", 64, 48, 2048, 1408},
    // Mixtral 8x7B: hidden 4096, expert intermediate 14336, 2 of 8 experts. At
    // prefill each expert sees 128 of 512 tokens; one expert is shown.
    {"mixtral-8x7b decode gate+up", 2, 1, 28672, 4096},
    {"mixtral-8x7b decode down", 2, 1, 4096, 14336},
    {"mixtral-8x7b prefill gate+up", 1, 128, 28672, 4096},
    {"mixtral-8x7b prefill down", 1, 128, 4096, 14336},
    // Skinny and unaligned problems.
    {"tall 4096x64x4096", 1, 4096, 64, 4096},
    {"wide 64x4096x4096", 1, 64, 4096, 4096},
    {"deep 256x256x16384", 1, 256, 256, 16384},
    {"unaligned 777x1111x1024", 1, 777, 1111, 1024},
};
}

std::span<const Shape> get_benchmark_shapes() { return kShapes; }
}
