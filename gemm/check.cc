#include "gemm/check.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace gemm {

namespace {
struct Random {
  std::uint64_t state;

  std::uint64_t next() {
    std::uint64_t z = (state += 0x9e3779b97f4a7c15);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
    z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
    return z ^ (z >> 31);
  }

  double uniform() { return (next() >> 11) * 0x1p-53 * 2 - 1; }  // [-1, 1)
};

void store(Precision p, std::span<std::byte> data, std::size_t i, double value) {
  switch (p) {
    case Precision::kFp32: {
      float v = float(value);
      std::memcpy(&data[4 * i], &v, 4);
      return;
    }
    case Precision::kFp16:
    case Precision::kFp16Fp32: {
      _Float16 v = _Float16(value);
      std::memcpy(&data[2 * i], &v, 2);
      return;
    }
    case Precision::kInt8:
      data[i] = std::byte(std::int8_t(value));
      return;
  }
}

double accumulator_unit(Precision p) {
  return p == Precision::kFp16 ? 0x1p-11 : p == Precision::kInt8 ? 0 : 0x1p-24;
}

// Half a unit in the last place of the output's rounding of the value.
double output_rounding(Precision p, double value) {
  if (p != Precision::kFp16 && p != Precision::kFp16Fp32) return 0;
  double magnitude = std::max(std::abs(value), 0x1p-14);
  return std::ldexp(1., std::ilogb(magnitude) - 11);
}

// A probabilistic bound on accumulated rounding error for a dot product whose
// absolute terms sum to at most `magnitude` (Higham and Mary, 2019).
double accumulation_bound(Precision p, Data data, unsigned k, double magnitude) {
  // Integer partial sums up to 2048 are exact in fp16, and in fp32 and int32.
  if (p == Precision::kInt8 || (data == Data::kExact && magnitude <= 2048)) return 0;
  return 4 * std::sqrt(double(k)) * accumulator_unit(p) * magnitude;
}
}

void fill_operands(Precision p, const Shape& s, Data data, std::uint64_t seed,
                   std::span<std::byte> a, std::span<std::byte> b) {
  Random random{seed};
  for (auto [bytes, count] : {std::pair{a, std::size_t(s.batch) * s.m * s.k},
                              std::pair{b, std::size_t(s.batch) * s.n * s.k}})
    for (std::size_t i = 0; i < count; ++i) {
      double value;
      if (data == Data::kExact) {
        // -1 and 1 each with probability 1/8, otherwise 0.
        unsigned draw = random.next() >> 61;
        value = draw == 0 ? -1 : draw == 1 ? 1 : 0;
      } else if (p == Precision::kInt8) {
        value = double(std::int8_t(random.next() >> 56));
      } else {
        value = random.uniform();
      }
      store(p, bytes, i, value);
    }
}

namespace {
// The checks, specialized for the element types so the inner loops read them directly.
template <typename In, typename Out>
Check check_typed(Precision p, const Shape& s, Data data, const In* a, const In* b, const Out* c,
                  std::uint64_t seed) {
  Check result;
  Random random{seed ^ 0x5eed};
  const std::size_t M = s.m, N = s.n, K = s.k;
  char text[256];
  auto fail = [&](const char* what, unsigned item, std::size_t i, std::size_t j, double got,
                  double expected, double tolerance) {
    if (!result.failure.empty()) return;
    std::snprintf(text, sizeof(text), "%s %u (%zu, %zu): got %.9g, expected %.9g, tolerance %.3g",
                  what, item, i, j, got, expected, tolerance);
    result.failure = text;
  };
  auto judge = [&](double error, double tolerance) {
    if (!std::isfinite(error)) return false;
    result.worst = std::max(result.worst, tolerance > 0 ? error / tolerance
                                          : error > 0   ? INFINITY
                                                        : 0);
    return error <= tolerance;
  };
  std::vector<double> a_norm(M), b_norm(N), weights(std::max(M, N)), projected(K);
  for (unsigned item = 0; item < s.batch; ++item) {
    auto A = [&](std::size_t i, std::size_t k) { return double(a[(item * M + i) * K + k]); };
    auto B = [&](std::size_t j, std::size_t k) { return double(b[(item * N + j) * K + k]); };
    auto C = [&](std::size_t i, std::size_t j) { return double(c[(item * M + i) * N + j]); };
    for (std::size_t i = 0; i < M; ++i) {
      double sum = 0;
      for (std::size_t k = 0; k < K; ++k) sum += A(i, k) * A(i, k);
      a_norm[i] = std::sqrt(sum);
    }
    for (std::size_t j = 0; j < N; ++j) {
      double sum = 0;
      for (std::size_t k = 0; k < K; ++k) sum += B(j, k) * B(j, k);
      b_norm[j] = std::sqrt(sum);
    }
    // Individual elements: corners, the first row and column, and random ones.
    auto element = [&](std::size_t i, std::size_t j) {
      double expected = 0, magnitude = 0;
      for (std::size_t k = 0; k < K; ++k) {
        double term = A(i, k) * B(j, k);
        expected += term;
        magnitude += std::abs(term);
      }
      double got = C(i, j);
      double tolerance =
          accumulation_bound(p, data, K, magnitude) + output_rounding(p, expected) * 1.01;
      if (!judge(std::abs(got - expected), tolerance))
        fail("element of item", item, i, j, got, expected, tolerance);
      ++result.elements;
    };
    for (std::size_t j = 0; j < N; ++j) element(0, j);
    for (std::size_t i = 1; i < M; ++i) element(i, 0);
    element(M - 1, N - 1);
    for (unsigned n = 0; n < 256; ++n) element(random.next() % M, random.next() % N);
    // Every row: C r against A (B^T r) for random signs r, with the rounding of
    // each element bounded through Cauchy-Schwarz and summed in quadrature.
    double b_squares = 0;
    for (std::size_t j = 0; j < N; ++j) {
      weights[j] = random.next() >> 63 ? 1 : -1;
      b_squares += b_norm[j] * b_norm[j];
    }
    std::fill(projected.begin(), projected.end(), 0);
    for (std::size_t j = 0; j < N; ++j)
      for (std::size_t k = 0; k < K; ++k) projected[k] += weights[j] * B(j, k);
    for (std::size_t i = 0; i < M; ++i) {
      double expected = 0, got = 0, rounding = 0;
      for (std::size_t k = 0; k < K; ++k) expected += A(i, k) * projected[k];
      for (std::size_t j = 0; j < N; ++j) {
        double value = C(i, j);
        got += weights[j] * value;
        rounding += std::pow(output_rounding(p, value), 2);
      }
      double tolerance = 6 * (accumulation_bound(p, data, K, a_norm[i] * std::sqrt(b_squares)) +
                              std::sqrt(rounding));
      if (!judge(std::abs(got - expected), tolerance))
        fail("row checksum of item", item, i, 0, got, expected, tolerance);
      ++result.rows;
    }
    // Every column: s^T C against (s^T A) B^T.
    double a_squares = 0;
    for (std::size_t i = 0; i < M; ++i) {
      weights[i] = random.next() >> 63 ? 1 : -1;
      a_squares += a_norm[i] * a_norm[i];
    }
    std::fill(projected.begin(), projected.end(), 0);
    for (std::size_t i = 0; i < M; ++i)
      for (std::size_t k = 0; k < K; ++k) projected[k] += weights[i] * A(i, k);
    for (std::size_t j = 0; j < N; ++j) {
      double expected = 0, got = 0, rounding = 0;
      for (std::size_t k = 0; k < K; ++k) expected += projected[k] * B(j, k);
      for (std::size_t i = 0; i < M; ++i) {
        double value = C(i, j);
        got += weights[i] * value;
        rounding += std::pow(output_rounding(p, value), 2);
      }
      double tolerance = 6 * (accumulation_bound(p, data, K, b_norm[j] * std::sqrt(a_squares)) +
                              std::sqrt(rounding));
      if (!judge(std::abs(got - expected), tolerance))
        fail("column checksum of item", item, 0, j, got, expected, tolerance);
      ++result.columns;
    }
  }
  result.passed = result.failure.empty();
  return result;
}
}

Check check_result(Precision p, const Shape& s, Data data, std::span<const std::byte> a,
                   std::span<const std::byte> b, std::span<const std::byte> c, std::uint64_t seed) {
  // Mapped GPU memory holds the elements as their types.
  switch (p) {
    case Precision::kFp32:
      return check_typed(p, s, data, reinterpret_cast<const float*>(a.data()),
                         reinterpret_cast<const float*>(b.data()),
                         reinterpret_cast<const float*>(c.data()), seed);
    case Precision::kFp16:
    case Precision::kFp16Fp32:
      return check_typed(p, s, data, reinterpret_cast<const _Float16*>(a.data()),
                         reinterpret_cast<const _Float16*>(b.data()),
                         reinterpret_cast<const _Float16*>(c.data()), seed);
    case Precision::kInt8:
      return check_typed(p, s, data, reinterpret_cast<const std::int8_t*>(a.data()),
                         reinterpret_cast<const std::int8_t*>(b.data()),
                         reinterpret_cast<const std::int32_t*>(c.data()), seed);
  }
  return {};
}
}
