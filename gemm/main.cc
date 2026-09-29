#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "gemm/bench.h"
#include "gemm/device.h"
#include "gemm/measure.h"
#include "gemm/problem.h"
#include "gemm/sensors.h"

namespace {
using common::Owner;
using common::Result;
using gemm::Device;
using gemm::Dispatch;

constexpr std::uint32_t kPeakFp32Runtime[] =
#include "gemm/peak_fp32_runtime.inc"
    ;
constexpr std::uint32_t kPeakFp32[] =
#include "gemm/peak_fp32.inc"
    ;
constexpr std::uint32_t kPeakFp16x2[] =
#include "gemm/peak_fp16x2.inc"
    ;
constexpr std::uint32_t kPeakInt8[] =
#include "gemm/peak_int8.inc"
    ;
constexpr std::uint32_t kMixShuffleXor[] =
#include "gemm/peak_mix_fp16x2_shuffle_xor.inc"
    ;
constexpr std::uint32_t kMixBroadcast[] =
#include "gemm/peak_mix_fp16x2_broadcast.inc"
    ;
constexpr std::uint32_t kMixQuadBroadcast[] =
#include "gemm/peak_mix_fp16x2_quad_broadcast.inc"
    ;
constexpr std::uint32_t kMatrixFp32_16[] =
#include "gemm/peak_coop_fp32_small_16.inc"
    ;
constexpr std::uint32_t kMatrixFp32_24[] =
#include "gemm/peak_coop_fp32_small_24.inc"
    ;
constexpr std::uint32_t kMatrixFp32_32[] =
#include "gemm/peak_coop_fp32_small_32.inc"
    ;
constexpr std::uint32_t kMatrixFp16_16[] =
#include "gemm/peak_coop_fp16_small_16.inc"
    ;
constexpr std::uint32_t kMatrixFp16_24[] =
#include "gemm/peak_coop_fp16_small_24.inc"
    ;
constexpr std::uint32_t kMatrixInt8_12[] =
#include "gemm/peak_coop_int8_12.inc"
    ;
constexpr std::uint32_t kMatrixInt8_14[] =
#include "gemm/peak_coop_int8_14.inc"
    ;
constexpr std::uint32_t kOrderChained[] =
#include "gemm/peak_coop_build_chained.inc"
    ;
constexpr std::uint32_t kOrderInterleaved[] =
#include "gemm/peak_coop_build_interleaved.inc"
    ;

constexpr std::uint32_t kRead1[] =
#include "gemm/bandwidth_read_1.inc"
    ;
constexpr std::uint32_t kRead4[] =
#include "gemm/bandwidth_read_4.inc"
    ;
constexpr std::uint32_t kRead16[] =
#include "gemm/bandwidth_read_16.inc"
    ;
constexpr std::uint32_t kCopy4[] =
#include "gemm/bandwidth_copy_4.inc"
    ;
constexpr std::uint32_t kWrite4[] =
#include "gemm/bandwidth_write_4.inc"
    ;

constexpr std::uint32_t kLoadVec4[] =
#include "gemm/load_global_vec4.inc"
    ;
constexpr std::uint32_t kLoadVec4Rows[] =
#include "gemm/load_global_vec4_rows.inc"
    ;
constexpr std::uint32_t kLoadVec2[] =
#include "gemm/load_global_vec2.inc"
    ;
constexpr std::uint32_t kLoadVec2HalfLines[] =
#include "gemm/load_global_vec2_half_lines.inc"
    ;
constexpr std::uint32_t kLoadScalar[] =
#include "gemm/load_global_scalar.inc"
    ;
constexpr std::uint32_t kLoadScalarLine[] =
#include "gemm/load_global_scalar_line.inc"
    ;
constexpr std::uint32_t kLoadUniform[] =
#include "gemm/load_global_uniform.inc"
    ;
constexpr std::uint32_t kLoadShared[] =
#include "gemm/load_shared_vec4.inc"
    ;
constexpr std::uint32_t kLoadTexel[] =
#include "gemm/load_texel_vec4.inc"
    ;
constexpr std::uint32_t kLoadImage[] =
#include "gemm/load_image_vec4.inc"
    ;
constexpr std::uint32_t kLoadImageRows[] =
#include "gemm/load_image_vec4_rows.inc"
    ;
constexpr std::uint32_t kLoadImageVec2[] =
#include "gemm/load_image_vec2.inc"
    ;
constexpr std::uint32_t kLoadImageScalar[] =
#include "gemm/load_image_scalar.inc"
    ;
constexpr std::uint32_t kLoadCoopGlobal[] =
#include "gemm/load_coop_global.inc"
    ;
constexpr std::uint32_t kLoadCoopShared[] =
#include "gemm/load_coop_shared.inc"
    ;

constexpr std::uint32_t kLayout_fp32_a_16x16[] =
#include "gemm/layout_fp32_a_16x16.inc"
    ;
constexpr std::uint32_t kLayout_fp32_b_16x16[] =
#include "gemm/layout_fp32_b_16x16.inc"
    ;
constexpr std::uint32_t kLayout_fp32_accumulator_16x16[] =
#include "gemm/layout_fp32_accumulator_16x16.inc"
    ;
constexpr std::uint32_t kLayout_fp32_a_4x4[] =
#include "gemm/layout_fp32_a_4x4.inc"
    ;
constexpr std::uint32_t kLayout_fp32_b_4x4[] =
#include "gemm/layout_fp32_b_4x4.inc"
    ;
constexpr std::uint32_t kLayout_fp32_accumulator_4x4[] =
#include "gemm/layout_fp32_accumulator_4x4.inc"
    ;
constexpr std::uint32_t kLayout_fp16_a_16x32[] =
#include "gemm/layout_fp16_a_16x32.inc"
    ;
constexpr std::uint32_t kLayout_fp16_b_32x32[] =
#include "gemm/layout_fp16_b_32x32.inc"
    ;
constexpr std::uint32_t kLayout_fp32_accumulator_16x32[] =
#include "gemm/layout_fp32_accumulator_16x32.inc"
    ;
constexpr std::uint32_t kLayout_fp16_a_4x8[] =
#include "gemm/layout_fp16_a_4x8.inc"
    ;
constexpr std::uint32_t kLayout_fp16_b_8x8[] =
#include "gemm/layout_fp16_b_8x8.inc"
    ;
constexpr std::uint32_t kLayout_fp32_accumulator_4x8[] =
#include "gemm/layout_fp32_accumulator_4x8.inc"
    ;
constexpr std::uint32_t kLayout_int8_a_4x16[] =
#include "gemm/layout_int8_a_4x16.inc"
    ;
constexpr std::uint32_t kLayout_int8_b_16x16[] =
#include "gemm/layout_int8_b_16x16.inc"
    ;
constexpr std::uint32_t kLayout_int32_accumulator_4x16[] =
#include "gemm/layout_int32_accumulator_4x16.inc"
    ;

constexpr std::uint32_t kEmpty[] =
#include "gemm/empty.inc"
    ;

// Tensor G3's published Mali-G715 MC7 maximum clock.
constexpr double kPublishedMhz = 890;

void print_steady(const gemm::Steady& s) {
  std::printf(
      "  %s after %.2f s: %u windows of %u repeats, spread %.2f%%, trend %+.2f%%, drift %+.2f%%, "
      "clock %.0f MHz (spread %.1f%%)\n",
      s.steady ? "steady" : "NOT STEADY", s.elapsed, unsigned(s.history.size()), s.repeats,
      s.spread * 100, s.trend * 100, s.drift * 100, s.clock_mhz, s.clock_spread * 100);
}

// Wait until Android reports no thermal throttling and the headroom has fallen
// to the target, so each measurement starts from the same thermal state.
void cool_down(float target, double maximum_seconds) {
  timespec start, now;
  clock_gettime(CLOCK_MONOTONIC, &start);
  for (;;) {
    int status = gemm::thermal_status();
    float headroom = gemm::thermal_headroom(0);
    clock_gettime(CLOCK_MONOTONIC, &now);
    double waited = double(now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) * 1e-9;
    if ((status == 0 && headroom <= target) || std::isnan(headroom) || waited >= maximum_seconds) {
      if (waited > 0)
        std::printf("Thermal status %d, headroom %.2f after %.0f s of cooling\n", status, headroom,
                    waited);
      return;
    }
    sleep(2);
  }
}

int run_thermal(int seconds) {
  for (int i = 0; i < seconds; i += 2) {
    std::printf("%3d s: status %d, headroom %.3f, GPU %.0f MHz\n", i, gemm::thermal_status(),
                gemm::thermal_headroom(0), gemm::read_gpu_mhz());
    std::fflush(stdout);
    sleep(2);
  }
  return 0;
}

void print_stats(const gemm::Kernel& kernel) {
  auto s = gemm::get_stats(kernel);
  if (!s.known) return;
  std::printf(
      "  compiler: %.0f registers, %.0f spill bytes, %.0f shared bytes; cycles FMA %.3f CVT %.3f "
      "SFU %.3f LS %.3f TEX %.3f\n",
      s.registers, s.spill_bytes, s.workgroup_bytes, s.fma_cycles, s.cvt_cycles, s.sfu_cycles,
      s.load_store_cycles, s.texture_cycles);
}

// Measured with `gemm peak`: the fastest instruction stream per precision, in
// operations per core per clock at a steady 890 MHz, and Arm's published peaks
// (256 fp32 and 512 fp16 operations per core per clock). Loops need constant trip
// counts to reach them. fp16 with fp32 accumulation runs at the fp32 rate. Arm
// publishes no int8 rate; the matrix multiplier's is about four times fp32's.
struct Ceiling {
  double measured, published;
};

Ceiling get_ceiling(gemm::Precision precision) {
  switch (precision) {
    case gemm::Precision::kFp32:
      return {246, 256};  // 4x4x4 cooperative matrices, 32 chains
    case gemm::Precision::kFp16:
      return {483, 512};  // f16vec2 FMAs
    case gemm::Precision::kFp16Fp32:
      return {250, 256};  // 4x8x8 cooperative matrices, 24 chains
    case gemm::Precision::kInt8:
      return {944, 0};  // 4x16x16 cooperative matrices, 14 chains
  }
  return {};
}

// Measured with `gemm bandwidth`: reads of 256 MiB by the GPU alone.
constexpr double kMeasuredBandwidth = 33e9;

// A variant's label, with how its run was launched where that differs from the
// usual: K split into slices, or fewer block rows swept together.
std::string label(const gemm::Variant& variant, const gemm::GemmRun& run) {
  std::string label(variant.name);
  if (run.launch.split > 1) label += ", split " + std::to_string(run.launch.split);
  if (variant.raster && run.launch.raster_rows != 8)
    label += ", raster " + std::to_string(run.launch.raster_rows);
  return label;
}

// Measures every matching kernel on every matching shape, `repeats` times in
// interleaved rounds so slow changes in the phone's state affect all of them
// alike, then summarizes each by its median run. Quick runs, for iterating on
// kernels, skip steady state: each kernel runs for a fixed 0.4 s and is judged
// per clock (%ceil) at the GPU clock sampled meanwhile, which discounts
// throttled GPU clocks.
int run_gemm(Device& device, gemm::ClockSampler& clock, float cooling,
             std::string_view precision_filter, std::string_view shape_filter,
             std::string_view kernel_filter, unsigned repeats, bool quick = false) {
  const auto& info = gemm::get_info(device);

  struct Entry {
    const gemm::Shape* shape;
    const gemm::Variant* variant;
    std::vector<gemm::GemmRun> runs;
    bool failed = false;
  };

  std::vector<Entry> entries;
  for (const auto& shape : gemm::get_benchmark_shapes()) {
    if (!shape_filter.empty() && shape.name.find(shape_filter) == std::string_view::npos) continue;
    for (const auto& variant : gemm::get_variants())
      if ((precision_filter.empty() || gemm::get_name(variant.precision) == precision_filter) &&
          (kernel_filter.empty() || variant.name.find(kernel_filter) != std::string_view::npos) &&
          gemm::supports(variant, shape))
        entries.push_back({&shape, &variant});
  }
  auto print = [&](const Entry& e, const gemm::GemmRun& run, const char* note) {
    double rate = gemm::operations(*e.shape) / run.seconds;
    double clock_hz = run.steady.clock_mhz * 1e6;
    auto ceiling = get_ceiling(e.variant->precision);
    double compute = ceiling.measured * info.cores * clock_hz;
    double bytes = double(gemm::a_bytes(e.variant->precision, *e.shape) +
                          gemm::b_bytes(e.variant->precision, *e.shape) +
                          gemm::c_bytes(e.variant->precision, *e.shape));
    double roof = std::min(compute, gemm::operations(*e.shape) / (bytes / kMeasuredBandwidth));
    char published[16] = "   n/a";
    if (ceiling.published)
      std::snprintf(published, sizeof(published), "%6.1f",
                    100 * rate / (ceiling.published * info.cores * kPublishedMhz * 1e6));
    auto name = gemm::get_name(e.variant->precision);
    std::printf("%-34.*s %-13.*s %-38s %8.3fms %8.3f %6.1f %6.1f %s %5.0f %s\n",
                int(e.shape->name.size()), e.shape->name.data(), int(name.size()), name.data(),
                label(*e.variant, run).c_str(), run.seconds * 1e3, rate * 1e-12,
                100 * rate / compute, 100 * rate / roof, published, run.steady.clock_mhz, note);
    std::fflush(stdout);
  };
  std::printf("%-34s %-13s %-38s %10s %8s %6s %6s %6s %5s\n", "shape", "precision", "kernel",
              "time", "TFLOPS", "%ceil", "%roof", "%pub", "MHz");
  unsigned failures = 0;
  for (unsigned round = 0; round < repeats; ++round)
    for (auto& e : entries) {
      // Every run starts from the same thermal state: throttling can slow memory
      // before it lowers the GPU clock, which per-clock rates do not discount.
      cool_down(cooling, 90);
      gemm::SteadyOptions options;
      if (quick) {
        options.duration = .4;
        options.windows = 3;
      }
      auto run = gemm::run_gemm(device, clock, *e.variant, *e.shape, options);
      if (!run) {
        std::printf("%.*s %.*s: %s\n", int(e.shape->name.size()), e.shape->name.data(),
                    int(e.variant->name.size()), e.variant->name.data(),
                    run.error().message.c_str());
        continue;
      }
      if (!run->passed()) {
        e.failed = true;
        ++failures;
        std::printf("FAILED %.*s %.*s: exact %s; random %s; sets agree %d\n",
                    int(e.shape->name.size()), e.shape->name.data(), int(e.variant->name.size()),
                    e.variant->name.data(), run->exact.failure.c_str(), run->random.failure.c_str(),
                    run->sets_agree);
        continue;
      }
      char note[64];
      std::snprintf(note, sizeof(note), "%s, verified, thermal status %d",
                    quick                ? "fixed 0.4 s"
                    : run->steady.steady ? "steady"
                                         : "UNSTEADY",
                    gemm::thermal_status());
      print(e, *run, note);
      if (!quick && (!run->steady.steady || std::getenv("GEMM_WINDOWS"))) print_steady(run->steady);
      if (std::getenv("GEMM_WINDOWS"))
        for (const auto& w : run->steady.history)
          std::printf("  %6.2f s %8.3f ms %4.0f MHz\n", w.elapsed, w.seconds * 1e3,
                      w.clock.mean_mhz);
      e.runs.push_back(std::move(*run));
    }
  if (repeats > 1) {
    std::printf("\nMedians of %u rounds (spread is the range over the fastest run):\n", repeats);
    for (auto& e : entries) {
      if (e.runs.empty()) continue;
      std::sort(e.runs.begin(), e.runs.end(),
                [](const auto& x, const auto& y) { return x.seconds < y.seconds; });
      unsigned steady = 0;
      for (const auto& run : e.runs) steady += run.steady.steady;
      char note[64];
      std::snprintf(note, sizeof(note), "spread %.1f%%, %u/%zu steady%s",
                    100 * (e.runs.back().seconds / e.runs.front().seconds - 1), steady,
                    e.runs.size(), e.failed ? ", FAILED once" : "");
      print(e, e.runs[e.runs.size() / 2], note);
    }
  }
  return failures ? 1 : 0;
}

// The fastest verified, steady kernel for every shape and precision. Every
// variant is screened with a fixed-length run, also with K unsplit where its
// default launch splits K, and any that fails verification is reported; the
// fastest three are measured to steady state, each from the same thermal state.
int run_matrix(Device& device, gemm::ClockSampler& clock, float cooling,
               std::string_view shape_filter) {
  const auto& info = gemm::get_info(device);
  const gemm::Precision precisions[] = {gemm::Precision::kFp32, gemm::Precision::kFp16,
                                        gemm::Precision::kFp16Fp32, gemm::Precision::kInt8};
  std::printf("%-34s %-13s %-34s %10s %8s %8s %6s %6s %6s %5s %7s %s\n", "shape", "precision",
              "kernel", "median", "TFLOPS", "GB/s", "%ceil", "%roof", "%pub", "MHz", "spread",
              "headroom");
  unsigned failures = 0;
  for (const auto& shape : gemm::get_benchmark_shapes()) {
    if (!shape_filter.empty() && shape.name.find(shape_filter) == std::string_view::npos) continue;
    cool_down(cooling, 300);
    for (auto precision : precisions) {
      // Screen every kernel with a fixed-length run, then measure the fastest
      // three to steady state.
      gemm::SteadyOptions screen;
      screen.duration = .4;
      screen.windows = 3;

      struct Candidate {
        double seconds;
        const gemm::Variant* variant;
        gemm::Launch launch;
      };

      std::vector<Candidate> screened;
      for (const auto& variant : gemm::get_variants()) {
        if (variant.precision != precision || !gemm::supports(variant, shape)) continue;
        std::vector<gemm::Launch> launches = {gemm::choose_launch(variant, shape)};
        if (launches[0].split > 1) launches.push_back({1});
        for (const auto& launch : launches) {
          auto run = gemm::run_gemm(device, clock, variant, shape, screen, launch);
          if (!run) continue;
          if (!run->passed()) {
            ++failures;
            std::printf("FAILED %.*s %s: exact %s; random %s; sets agree %d\n",
                        int(shape.name.size()), shape.name.data(), label(variant, *run).c_str(),
                        run->exact.failure.c_str(), run->random.failure.c_str(), run->sets_agree);
            continue;
          }
          screened.push_back({run->seconds, &variant, run->launch});
        }
      }
      std::sort(screened.begin(), screened.end(),
                [](const auto& x, const auto& y) { return x.seconds < y.seconds; });
      if (screened.size() > 3) screened.resize(3);
      const gemm::Variant* best = nullptr;
      gemm::Launch best_launch;
      gemm::GemmRun best_run;
      for (const auto& [seconds, variant, launch] : screened) {
        cool_down(cooling, 90);
        auto run = gemm::run_gemm(device, clock, *variant, shape, {}, launch);
        if (!run) continue;
        if (!run->passed()) {
          ++failures;
          continue;
        }
        if (run->steady.steady && (!best || run->seconds < best_run.seconds)) {
          best = variant;
          best_launch = launch;
          best_run = std::move(*run);
        }
      }
      if (!best) {
        std::printf("%-34.*s %-13.*s no steady, verified result\n", int(shape.name.size()),
                    shape.name.data(), int(gemm::get_name(precision).size()),
                    gemm::get_name(precision).data());
        continue;
      }
      // The fastest of many variants is biased toward a lucky run, so measure it
      // twice more from the same thermal state and report the median of three.
      std::vector<gemm::GemmRun> repeats;
      repeats.push_back(std::move(best_run));
      for (int again = 0; again < 2; ++again) {
        cool_down(cooling, 90);
        auto run = gemm::run_gemm(device, clock, *best, shape, {}, best_launch);
        if (run && run->passed() && run->steady.steady) repeats.push_back(std::move(*run));
        else if (run && !run->passed()) ++failures;
      }
      std::sort(repeats.begin(), repeats.end(),
                [](const auto& x, const auto& y) { return x.seconds < y.seconds; });
      double spread = (repeats.back().seconds - repeats.front().seconds) / repeats.front().seconds;
      best_run = std::move(repeats[repeats.size() / 2]);
      double rate = gemm::operations(shape) / best_run.seconds;
      double bytes = double(gemm::a_bytes(precision, shape) + gemm::b_bytes(precision, shape) +
                            gemm::c_bytes(precision, shape));
      auto ceiling = get_ceiling(precision);
      double compute = ceiling.measured * info.cores * best_run.steady.clock_mhz * 1e6;
      double roof = std::min(compute, gemm::operations(shape) / (bytes / kMeasuredBandwidth));
      char published[16] = "   n/a";
      if (ceiling.published)
        std::snprintf(published, sizeof(published), "%6.1f",
                      100 * rate / (ceiling.published * info.cores * kPublishedMhz * 1e6));
      std::printf("%-34.*s %-13.*s %-34s %8.3fms %8.3f %8.1f %6.1f %6.1f %s %5.0f %6.1f%% %.2f\n",
                  int(shape.name.size()), shape.name.data(), int(gemm::get_name(precision).size()),
                  gemm::get_name(precision).data(), label(*best, best_run).c_str(),
                  best_run.seconds * 1e3, rate * 1e-12, bytes / best_run.seconds * 1e-9,
                  100 * rate / compute, 100 * rate / roof, published, best_run.steady.clock_mhz,
                  100 * spread, gemm::thermal_headroom(0));
      std::fflush(stdout);
    }
  }
  std::printf("%u kernel runs failed verification\n", failures);
  return failures ? 1 : 0;
}

// Times each tiled kernel with loads or multiplies removed beside its full
// version, from the same thermal state, to show what limits it. Only the full
// versions compute correct results.
int run_ablate(Device& device, gemm::ClockSampler& clock, float cooling,
               std::string_view shape_name) {
  const auto& info = gemm::get_info(device);
  const gemm::Shape* shape = nullptr;
  for (const auto& s : gemm::get_benchmark_shapes())
    if (s.name == shape_name) shape = &s;
  if (!shape) return std::fprintf(stderr, "Unknown shape\n"), 2;
  std::printf("%-40s %10s %8s %6s %5s\n", "kernel", "time", "TFLOPS", "%ceil", "MHz");
  // Fixed-length runs judged per clock, like `gemm quick`.
  gemm::SteadyOptions options;
  options.duration = .4;
  options.windows = 3;
  for (const auto& v : gemm::get_ablations()) {
    if (!gemm::supports(v, *shape)) continue;
    cool_down(cooling, 30);
    auto run = gemm::run_gemm(device, clock, v, *shape, options);
    if (!run) {
      std::printf("%.*s: %s\n", int(v.name.size()), v.name.data(), run.error().message.c_str());
      continue;
    }
    double rate = gemm::operations(*shape) / run->seconds;
    double compute = get_ceiling(v.precision).measured * info.cores * run->steady.clock_mhz * 1e6;
    std::printf("%-40.*s %8.3fms %8.3f %6.1f %5.0f %s, %s\n", int(v.name.size()), v.name.data(),
                run->seconds * 1e3, rate * 1e-12, 100 * rate / compute, run->steady.clock_mhz,
                "fixed 0.4 s", run->passed() ? "verified" : "wrong results");
    std::fflush(stdout);
  }
  return 0;
}

// Samples for a soak: every window's time and clock, and Android's thermal state
// once a second (it rate-limits headroom queries).
struct SoakLog {
  double operations = 0, last_thermal = -1, last_print = 0;
  int status = -1;
  float headroom = NAN;

  struct Sample {
    double elapsed, rate, mhz;
    int status;
    float headroom;
  };

  std::vector<Sample> samples;
};

void on_soak_window(const gemm::Window& w, void* context) {
  auto& log = *static_cast<SoakLog*>(context);
  if (w.elapsed - log.last_thermal >= 1) {
    log.status = gemm::thermal_status();
    log.headroom = gemm::thermal_headroom(0);
    log.last_thermal = w.elapsed;
  }
  log.samples.push_back(
      {w.elapsed, log.operations / w.seconds, w.clock.mean_mhz, log.status, log.headroom});
  if (w.elapsed - log.last_print >= 5) {
    log.last_print = w.elapsed;
    std::printf(
        "%6.0f s  %7.3f TFLOPS  GPU %4.0f MHz (%4.0f-%4.0f)  thermal status %d, headroom %.2f\n",
        w.elapsed, log.operations / w.seconds * 1e-12, w.clock.mean_mhz, w.clock.min_mhz,
        w.clock.max_mhz, log.status, log.headroom);
    std::fflush(stdout);
  }
}

double median_of(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  return values.empty() ? 0 : values[values.size() / 2];
}

// Runs one GEMM continuously and reports when throttling begins and when a
// throttled steady state is reached. Throttled phones oscillate as the thermal
// governor steps clocks, so steady state is judged on time averages: the
// earliest time after which every 60-second mean stays within 3% of the final
// two minutes' mean, confirmed for at least three further minutes.
int run_soak(Device& device, gemm::ClockSampler& clock, std::string_view precision,
             std::string_view shape_name, std::string_view kernel_name, double seconds) {
  const gemm::Variant* variant = nullptr;
  for (const auto& v : gemm::get_variants())
    if (gemm::get_name(v.precision) == precision && v.name == kernel_name) variant = &v;
  const gemm::Shape* shape = nullptr;
  for (const auto& s : gemm::get_benchmark_shapes())
    if (s.name == shape_name) shape = &s;
  if (!variant || !shape) return std::fprintf(stderr, "Unknown kernel or shape\n"), 2;
  SoakLog log;
  gemm::SteadyOptions options;
  options.window_seconds = .25;
  options.duration = seconds;
  options.on_window = on_soak_window;
  options.context = &log;
  std::printf("Soaking %.*s %.*s with %.*s for %.0f s\n", int(precision.size()), precision.data(),
              int(shape_name.size()), shape_name.data(), int(kernel_name.size()),
              kernel_name.data(), seconds);
  // Rotating sets divide each window's time; the callback needs operations per repeat.
  log.operations = gemm::operations(*shape) *
                   std::clamp<double>(std::ceil(double(64 << 20) /
                                                double(gemm::a_bytes(variant->precision, *shape) +
                                                       gemm::b_bytes(variant->precision, *shape))),
                                      1, 64);
  auto run = gemm::run_gemm(device, clock, *variant, *shape, options);
  if (!run) return std::fprintf(stderr, "%s\n", run.error().message.c_str()), 1;
  const auto& samples = log.samples;
  auto window_median = [&](double from, double to, double SoakLog::Sample::* field) {
    std::vector<double> values;
    for (const auto& s : samples)
      if (s.elapsed >= from && s.elapsed < to) values.push_back(s.*field);
    return median_of(values);
  };
  // Operations over GPU time within a span: the time average the phone sustains.
  auto window_mean = [&](double from, double to, double SoakLog::Sample::* field) {
    double weighted = 0, total = 0;
    for (const auto& s : samples)
      if (s.elapsed >= from && s.elapsed < to) {
        double seconds = log.operations / s.rate;
        weighted += s.*field * (field == &SoakLog::Sample::rate ? seconds : 1);
        total += field == &SoakLog::Sample::rate ? seconds : 1;
      }
    return total ? weighted / total : 0;
  };
  double end = samples.back().elapsed;
  double initial = window_median(1, 6, &SoakLog::Sample::rate);
  double initial_mhz = window_median(1, 6, &SoakLog::Sample::mhz);
  double final_rate = window_mean(end - 120, end + 1, &SoakLog::Sample::rate);
  double final_mhz = window_mean(end - 120, end + 1, &SoakLog::Sample::mhz);
  double onset = -1, capped = -1, settled = -1;
  for (double t = 1; t + 5 <= end; t += 1)
    if (onset < 0 && window_median(t, t + 5, &SoakLog::Sample::rate) < .97 * initial) onset = t;
  // From the first second on, as the clock is still ramping up from idle before.
  for (const auto& s : samples)
    if (capped < 0 && s.elapsed >= 1 && s.mhz < initial_mhz - 1) capped = s.elapsed;
  for (double t = 0; t + 60 <= end; t += 1) {
    bool stays = true;
    for (double u = t; u + 60 <= end && stays; u += 5)
      stays = std::abs(window_mean(u, u + 60, &SoakLog::Sample::rate) / final_rate - 1) <= .03;
    if (stays) {
      settled = t;
      break;
    }
  }
  double low = INFINITY, high = 0;
  for (double t = settled < 0 ? end - 120 : settled; t + 5 <= end; t += 5) {
    low = std::min(low, window_median(t, t + 5, &SoakLog::Sample::rate));
    high = std::max(high, window_median(t, t + 5, &SoakLog::Sample::rate));
  }
  std::printf("\nUnthrottled: %.3f TFLOPS at %.0f MHz (1-6 s)\n", initial * 1e-12, initial_mhz);
  if (onset >= 0)
    std::printf("Throttling began at %.0f s (5-second median below 97%% of unthrottled)\n", onset);
  else std::printf("No throttling within %.0f s\n", end);
  if (capped >= 0) std::printf("The GPU clock was first lowered at %.0f s\n", capped);
  std::printf("Final two minutes: %.3f TFLOPS at a mean %.0f MHz = %.1f%% of unthrottled\n",
              final_rate * 1e-12, final_mhz, 100 * final_rate / initial);
  if (settled >= 0 && end - settled >= 180)
    std::printf(
        "Throttled steady state from %.0f s: every later 60-second mean within 3%% of the "
        "final two minutes, confirmed for %.0f s; 5-second medians ranged %.3f-%.3f TFLOPS\n",
        settled, end - settled, low * 1e-12, high * 1e-12);
  else
    std::printf("Steady state NOT confirmed: it needs three minutes of stable 60-second means\n");
  std::printf("Output after %.0f s: %s\n", end, run->passed() ? "verified" : "FAILED");
  return run->passed() ? 0 : 1;
}

// The fixed cost of a dispatch and the barrier after it, which bounds how fast
// small GEMMs can be: batches of empty dispatches of increasing workgroup counts.
int run_dispatch(Device& device, gemm::ClockSampler& clock) {
  auto sink = gemm::create_buffer(device, 1 << 20);
  auto kernel = gemm::create_kernel(device, kEmpty, 1, 4);
  if (!sink || !kernel) return 1;
  for (unsigned groups : {1u, 64u, 1024u, 16384u}) {
    Dispatch dispatch;
    dispatch.kernel = kernel->get();
    dispatch.bindings[0] = {sink->get()};
    dispatch.groups = {groups, 1, 1};
    auto steady = gemm::measure_steady(device, clock, std::span(&dispatch, 1));
    if (!steady) return std::fprintf(stderr, "%s\n", steady.error().message.c_str()), 1;
    std::printf("%5u empty workgroups: %.2f us per dispatch and barrier\n", groups,
                steady->seconds * 1e6);
    print_steady(*steady);
  }
  return 0;
}

int run_info(Device& device) {
  const auto& info = gemm::get_info(device);
  std::printf("GPU: %s (%s)\n", info.name.c_str(), info.driver.c_str());
  std::printf("Cores: %u, FP32 FMA per core per clock: %u, subgroup: %u, shared memory: %u bytes\n",
              info.cores, info.fma_per_core_clock, info.subgroup_size, info.shared_memory);
  std::printf("Timestamp period: %.3f ns; image dimension %u; texel buffer elements %u\n",
              info.timestamp_ns, info.image_dimension, info.texel_buffer_elements);
  for (const auto& shape : info.matrix_shapes)
    std::printf("Cooperative matrix: %ux%ux%u, A/B type %d, C type %d\n", shape.m, shape.n, shape.k,
                shape.a_type, shape.c_type);
  std::printf("GPU clock now: %.0f MHz; thermal status %d, headroom %.2f\n", gemm::read_gpu_mhz(),
              gemm::thermal_status(), gemm::thermal_headroom(0));
  return 0;
}

struct PeakKernel {
  const char* name;
  std::span<const std::uint32_t> spirv;
  bool matrix;
  // Per invocation per iteration for arithmetic kernels, per subgroup for matrices.
  double operations, instructions;
  double peak_per_core_clock;  // published operations per core per clock, 0 when unknown
  unsigned group_size;
};

int run_peak(Device& device, gemm::ClockSampler& clock, const char* filter) {
  const auto& info = gemm::get_info(device);
  double fma = info.fma_per_core_clock;
  const PeakKernel kernels[] = {
      {"fp32 FMA", kPeakFp32, false, 16 * 2, 16, 2 * fma, 256},
      {"fp32 FMA, runtime trip count", kPeakFp32Runtime, false, 16 * 2, 16, 2 * fma, 256},
      {"fp16 FMA vec2", kPeakFp16x2, false, 16 * 2 * 2, 16, 4 * fma, 256},
      {"int8 dot", kPeakInt8, false, 16 * 8, 16, 0, 256},
      {"fp16 FMA vec2 + shuffle xor", kMixShuffleXor, false, 16 * 2 * 2, 32, 4 * fma, 256},
      {"fp16 FMA vec2 + broadcast", kMixBroadcast, false, 16 * 2 * 2, 32, 4 * fma, 256},
      {"fp16 FMA vec2 + quad broadcast", kMixQuadBroadcast, false, 16 * 2 * 2, 32, 4 * fma, 256},
      {"fp32 matrix 4x4x4, 16 chains", kMatrixFp32_16, true, 16 * 4 * 4 * 4 * 2, 0, 2 * fma, 64},
      {"fp32 matrix 4x4x4, 24 chains", kMatrixFp32_24, true, 24 * 4 * 4 * 4 * 2, 0, 2 * fma, 64},
      {"fp32 matrix 4x4x4, 32 chains", kMatrixFp32_32, true, 32 * 4 * 4 * 4 * 2, 0, 2 * fma, 64},
      {"fp16 matrix 4x8x8, 16 chains", kMatrixFp16_16, true, 16 * 4 * 8 * 8 * 2, 0, 0, 64},
      {"fp16 matrix 4x8x8, 24 chains", kMatrixFp16_24, true, 24 * 4 * 8 * 8 * 2, 0, 0, 64},
      {"int8 matrix 4x16x16, 12 chains", kMatrixInt8_12, true, 12 * 4 * 16 * 16 * 2, 0, 0, 64},
      {"int8 matrix 4x16x16, 14 chains", kMatrixInt8_14, true, 14 * 4 * 16 * 16 * 2, 0, 0, 64},
      {"int8 matrix, dependent multiplies in sequence", kOrderChained, true, 8 * 4 * 16 * 16 * 2, 0,
       0, 64},
      {"int8 matrix, dependent multiplies interleaved", kOrderInterleaved, true,
       8 * 4 * 16 * 16 * 2, 0, 0, 64},
  };
  auto sink = gemm::create_buffer(device, 64 << 20);
  if (!sink) return std::fprintf(stderr, "%s\n", sink.error().message.c_str()), 1;
  // Nonzero operands for every precision: fp16 ones, small fp32 values, int8 0 and 60.
  auto words = gemm::get_data(**sink);
  for (std::size_t i = 0; i + 4 <= words.size(); i += 4) {
    std::uint32_t word = 0x3c003c00;
    std::memcpy(&words[i], &word, 4);
  }
  if (auto flushed = gemm::flush(**sink); !flushed) return 1;
  for (const auto& k : kernels) {
    if (filter && !std::strstr(k.name, filter)) continue;
    unsigned iterations = k.matrix ? 256 : 1024;
    std::uint32_t specialization[] = {k.group_size, iterations};
    auto kernel = gemm::create_kernel(device, k.spirv, 1, 8, specialization);
    if (!kernel) {
      std::printf("%s: %s\n", k.name, kernel.error().message.c_str());
      continue;
    }
    // Enough invocations to fill every core several times over.
    unsigned groups = info.cores * (k.matrix ? 4096 : 8192) / k.group_size;
    Dispatch dispatch;
    dispatch.kernel = kernel->get();
    dispatch.bindings[0] = {sink->get()};
    dispatch.push[0] = iterations;
    float seed = 1e-4f;
    std::memcpy(&dispatch.push[1], &seed, 4);
    dispatch.groups = {groups, 1, 1};
    if (std::getenv("GEMM_DESCRIBE")) std::printf("%s", gemm::describe(**kernel).c_str());
    auto steady = gemm::measure_steady(device, clock, std::span(&dispatch, 1));
    if (!steady) {
      std::printf("%s: %s\n", k.name, steady.error().message.c_str());
      continue;
    }
    double units = double(groups) * k.group_size / (k.matrix ? info.subgroup_size : 1);
    double per_second = units * iterations / steady->seconds;
    double core_clocks = info.cores * steady->clock_mhz * 1e6;
    double per_core_clock = per_second * k.operations / core_clocks;
    std::printf("%-30s %7.1f G ops/s = %6.1f ops", k.name, per_second * k.operations * 1e-9,
                per_core_clock);
    if (k.instructions)
      std::printf(", %5.1f lane-instructions", per_second * k.instructions / core_clocks);
    std::printf(" per core per clock");
    if (k.peak_per_core_clock > 0)
      std::printf(" (%.1f%% of published %.0f)", 100 * per_core_clock / k.peak_per_core_clock,
                  k.peak_per_core_clock);
    std::printf("\n");
    print_steady(*steady);
    print_stats(**kernel);
  }
  return 0;
}

int run_loads(Device& device, gemm::ClockSampler& clock) {
  const auto& info = gemm::get_info(device);
  auto source = gemm::create_buffer(device, 64 << 20);
  auto sink = gemm::create_buffer(device, 16 << 20);
  if (!source || !sink) return 1;
  auto* values = reinterpret_cast<float*>(gemm::get_data(**source).data());
  for (std::size_t i = 0; i < (64 << 20) / 4; ++i) values[i] = float(i % 1000) * 1e-3f;
  if (!gemm::flush(**source)) return 1;
  // The same data as a 64-texel-wide image: one workgroup's region is four rows.
  auto image = gemm::create_image(device, 64, (64 << 20) / 16 / 64 / 4);
  if (!image) return std::fprintf(stderr, "%s\n", image.error().message.c_str()), 1;
  if (auto uploaded = gemm::upload(**image, **source); !uploaded)
    return std::fprintf(stderr, "%s\n", uploaded.error().message.c_str()), 1;
  // Linear images over the buffer, with 1 KiB rows of 16-, 8-, and 4-byte texels.
  auto linear = gemm::create_image(**source, 0, 64, (64 << 20) / 16 / 64 / 4);
  auto linear8 = gemm::create_image(**source, 0, 128, (64 << 20) / 16 / 64 / 4, 8);
  auto linear4 = gemm::create_image(**source, 0, 256, (64 << 20) / 16 / 64 / 4, 4);
  if (!linear || !linear8 || !linear4) return std::fprintf(stderr, "Cannot create images\n"), 1;

  struct Load {
    const char* name;
    std::span<const std::uint32_t> spirv;
    double bytes;  // per invocation per iteration, as seen by each lane
    unsigned texel_mask = 0, image_mask = 0;
    gemm::Image* image = nullptr;  // the optimally tiled image when null
  } loads[] = {
      {"global vec4 (L1)", kLoadVec4, 8 * 16},
      {"global vec4, a row per lane", kLoadVec4Rows, 8 * 16},
      {"global vec2", kLoadVec2, 8 * 8},
      {"global vec2, half lines", kLoadVec2HalfLines, 8 * 8},
      {"global scalar, 16-byte stride", kLoadScalar, 8 * 4},
      {"global scalar, consecutive", kLoadScalarLine, 8 * 4},
      {"global vec4, same address", kLoadUniform, 8 * 16},
      {"shared vec4", kLoadShared, 8 * 16},
      {"texel buffer RGBA32UI", kLoadTexel, 8 * 16, 1},
      {"sampled image, optimal tiling", kLoadImage, 8 * 16, 0, 1},
      {"linear image RGBA32UI", kLoadImage, 8 * 16, 0, 1, linear->get()},
      {"linear image RGBA32UI, a row per lane", kLoadImageRows, 8 * 16, 0, 1, linear->get()},
      {"linear image RG32UI", kLoadImageVec2, 8 * 8, 0, 1, linear8->get()},
      {"linear image R32UI", kLoadImageScalar, 8 * 4, 0, 1, linear4->get()},
      {"coopMatLoad 16x16 fp32, global", kLoadCoopGlobal, 1024 / 16},
      {"coopMatLoad 16x16 fp32, shared", kLoadCoopShared, 1024 / 16},
  };

  for (const auto& load : loads) {
    std::uint32_t specialization[] = {0, 256};
    auto kernel = gemm::create_kernel(device, load.spirv, 2, 8, specialization, load.texel_mask,
                                      load.image_mask);
    if (!kernel) return std::fprintf(stderr, "%s\n", kernel.error().message.c_str()), 1;
    unsigned groups = info.cores * 64, iterations = 256;
    Dispatch dispatch;
    dispatch.kernel = kernel->get();
    dispatch.bindings[0] = {source->get(), load.image ? load.image : image->get()};
    dispatch.bindings[1] = {sink->get()};
    dispatch.push[0] = iterations;
    dispatch.push[1] = 256;  // 4 KiB per workgroup
    dispatch.groups = {groups, 1, 1};
    auto steady = gemm::measure_steady(device, clock, std::span(&dispatch, 1));
    if (!steady) return std::fprintf(stderr, "%s\n", steady.error().message.c_str()), 1;
    double bytes = double(groups) * 256 * iterations * load.bytes / steady->seconds;
    std::printf("%-42s %7.1f GB/s = %6.1f bytes per core per clock\n", load.name, bytes * 1e-9,
                bytes / (info.cores * steady->clock_mhz * 1e6));
    print_steady(*steady);
    print_stats(**kernel);
  }
  return 0;
}

int run_layouts(Device& device) {
  struct Layout {
    const char* name;
    std::span<const std::uint32_t> spirv;
    std::string_view type;
    unsigned rows, cols;
  } layouts[] = {
      {"fp32 a 16x16", kLayout_fp32_a_16x16, "fp32", 16, 16},
      {"fp32 b 16x16", kLayout_fp32_b_16x16, "fp32", 16, 16},
      {"fp32 accumulator 16x16", kLayout_fp32_accumulator_16x16, "fp32", 16, 16},
      {"fp32 a 4x4", kLayout_fp32_a_4x4, "fp32", 4, 4},
      {"fp32 b 4x4", kLayout_fp32_b_4x4, "fp32", 4, 4},
      {"fp32 accumulator 4x4", kLayout_fp32_accumulator_4x4, "fp32", 4, 4},
      {"fp16 a 16x32", kLayout_fp16_a_16x32, "fp16", 16, 32},
      {"fp16 b 32x32", kLayout_fp16_b_32x32, "fp16", 32, 32},
      {"fp32 accumulator 16x32", kLayout_fp32_accumulator_16x32, "fp32", 16, 32},
      {"fp16 a 4x8", kLayout_fp16_a_4x8, "fp16", 4, 8},
      {"fp16 b 8x8", kLayout_fp16_b_8x8, "fp16", 8, 8},
      {"fp32 accumulator 4x8", kLayout_fp32_accumulator_4x8, "fp32", 4, 8},
      {"int8 a 4x16", kLayout_int8_a_4x16, "int8", 4, 16},
      {"int8 b 16x16", kLayout_int8_b_16x16, "int8", 16, 16},
      {"int32 accumulator 4x16", kLayout_int32_accumulator_4x16, "int32", 4, 16},
  };

  auto values = gemm::create_buffer(device, 1 << 16);
  auto lanes = gemm::create_buffer(device, 1 << 16);
  auto batch = gemm::create_batch(device);
  if (!values || !lanes || !batch) return 1;
  for (const auto& layout : layouts) {
    auto kernel = gemm::create_kernel(device, layout.spirv, 2, 0);
    if (!kernel) {
      std::printf("%s: %s\n", layout.name, kernel.error().message.c_str());
      continue;
    }
    unsigned elements = layout.rows * layout.cols;
    std::vector<unsigned> index(elements * 2, 0);
    // int8 holds indices below 128, so it takes two passes.
    for (unsigned pass = 0; pass < (layout.type == "int8" ? 2u : 1u); ++pass) {
      auto data = gemm::get_data(**values);
      for (unsigned i = 0; i < elements; ++i) {
        unsigned value = layout.type == "int8" ? (pass ? i >> 7 : i & 127) : i;
        if (layout.type == "fp32") {
          float v = float(value);
          std::memcpy(&data[4 * i], &v, 4);
        } else if (layout.type == "fp16") {
          _Float16 v = _Float16(value);
          std::memcpy(&data[2 * i], &v, 2);
        } else if (layout.type == "int32") {
          std::int32_t v = std::int32_t(value);
          std::memcpy(&data[4 * i], &v, 4);
        } else {
          data[i] = std::byte(value);
        }
      }
      Dispatch dispatch;
      dispatch.kernel = kernel->get();
      dispatch.bindings[0] = {values->get()};
      dispatch.bindings[1] = {lanes->get()};
      if (!gemm::flush(**values) || !gemm::record(**batch, std::span(&dispatch, 1), 1) ||
          !gemm::submit(**batch) || !gemm::wait(**batch) || !gemm::invalidate(**lanes))
        return 1;
      auto* out = reinterpret_cast<const std::uint32_t*>(gemm::get_data(**lanes).data());
      unsigned length = out[16 * (elements / 16)];
      for (unsigned i = 0; i < 16 * length; ++i) index[i] += pass ? out[i] << 7 : out[i];
    }
    unsigned length = elements / 16;
    std::printf("%s: %u elements per lane\n", layout.name, length);
    for (unsigned lane : {0u, 1u, 2u, 15u}) {
      std::printf("  lane %2u:", lane);
      for (unsigned e = 0; e < length; ++e)
        std::printf(" %u,%u", index[lane * length + e] / layout.cols,
                    index[lane * length + e] % layout.cols);
      std::printf("\n");
    }
  }
  return 0;
}

struct CopyJob {
  std::byte* to;
  const std::byte* from;
  std::size_t bytes;
  unsigned rounds;
  double seconds = 0;
};

void* run_copy(void* data) {
  auto& job = *static_cast<CopyJob*>(data);
  timespec begin, end;
  clock_gettime(CLOCK_MONOTONIC, &begin);
  for (unsigned round = 0; round < job.rounds; ++round) std::memcpy(job.to, job.from, job.bytes);
  clock_gettime(CLOCK_MONOTONIC, &end);
  job.seconds = double(end.tv_sec - begin.tv_sec) + (end.tv_nsec - begin.tv_nsec) * 1e-9;
  return nullptr;
}

int run_bandwidth(Device& device, gemm::ClockSampler& clock) {
  constexpr std::size_t kBytes = std::size_t(256) << 20;
  auto source = gemm::create_buffer(device, kBytes);
  auto destination = gemm::create_buffer(device, kBytes);
  auto coherent = gemm::create_buffer(device, kBytes, gemm::Memory::kCoherent);
  if (!source || !destination || !coherent) return std::fprintf(stderr, "Cannot allocate\n"), 1;
  auto* values = reinterpret_cast<float*>(gemm::get_data(**source).data());
  for (std::size_t i = 0; i < kBytes / 4; ++i) values[i] = float(i % 1000) * 1e-3f;
  std::memcpy(gemm::get_data(**coherent).data(), values, kBytes);
  if (!gemm::flush(**source)) return 1;

  // Grids are powers of two, so every subgroup takes the same number of steps.
  struct Stream {
    const char* name;
    std::span<const std::uint32_t> spirv;
    gemm::Buffer* from;
    std::size_t bytes;
    double traffic;   // bytes moved per byte of buffer
    unsigned unroll;  // vec4 elements per invocation per step
    unsigned groups;  // 0: one invocation per vec4 for cache-sized buffers
  } streams[] = {
      {"read, 1 vec4 per step", kRead1, source->get(), kBytes, 1, 1, 256},
      {"read, 4 vec4 per step", kRead4, source->get(), kBytes, 1, 4, 256},
      {"read, 16 vec4 per step", kRead16, source->get(), kBytes, 1, 16, 256},
      {"read, 4 vec4, 64 groups", kRead4, source->get(), kBytes, 1, 4, 64},
      {"read, 4 vec4, 1024 groups", kRead4, source->get(), kBytes, 1, 4, 1024},
      {"read, host-coherent memory", kRead4, coherent->get(), kBytes, 1, 4, 256},
      {"copy (read + write)", kCopy4, source->get(), kBytes, 2, 4, 256},
      {"write", kWrite4, source->get(), kBytes, 1, 4, 256},
      {"read 256 MiB repeatedly", kRead1, source->get(), 256 << 20, 1, 1, 0},
      {"read 128 MiB repeatedly", kRead1, source->get(), 128 << 20, 1, 1, 0},
      {"read 64 MiB repeatedly", kRead1, source->get(), 64 << 20, 1, 1, 0},
      {"read 32 MiB repeatedly", kRead1, source->get(), 32 << 20, 1, 1, 0},
      {"read 16 MiB repeatedly", kRead1, source->get(), 16 << 20, 1, 1, 0},
      {"read 8 MiB repeatedly", kRead1, source->get(), 8 << 20, 1, 1, 0},
      {"read 4 MiB repeatedly", kRead1, source->get(), 4 << 20, 1, 1, 0},
      {"read 1 MiB repeatedly", kRead1, source->get(), 1 << 20, 1, 1, 0},
      {"read 256 KiB repeatedly", kRead1, source->get(), 256 << 10, 1, 1, 0},
      {"read 64 KiB repeatedly", kRead1, source->get(), 64 << 10, 1, 1, 0},
  };

  for (const auto& stream : streams) {
    unsigned count = unsigned(stream.bytes / 16);
    unsigned passes = unsigned(std::max<std::size_t>(1, (64 << 20) / stream.bytes));
    unsigned groups = stream.groups ? stream.groups : count / 256;
    std::uint32_t specialization[] = {256, count, passes, groups * 16};
    auto kernel = gemm::create_kernel(device, stream.spirv, 2, 0, specialization);
    if (!kernel) return std::fprintf(stderr, "%s\n", kernel.error().message.c_str()), 1;
    Dispatch dispatch;
    dispatch.kernel = kernel->get();
    dispatch.bindings[0] = {stream.from};
    dispatch.bindings[1] = {destination->get()};
    dispatch.groups = {groups, 1, 1};
    auto steady = gemm::measure_steady(device, clock, std::span(&dispatch, 1));
    if (!steady) return std::fprintf(stderr, "%s\n", steady.error().message.c_str()), 1;
    double bytes = double(stream.bytes) * passes * stream.traffic;
    std::printf("%-36s %6.1f GB/s = %5.1f bytes per core per clock\n", stream.name,
                bytes / steady->seconds * 1e-9,
                bytes / steady->seconds / (gemm::get_info(device).cores * steady->clock_mhz * 1e6));
    print_steady(*steady);
  }
  // The CPU's view of the same DRAM, as a reference for its capability, alone
  // and alongside the GPU stream.
  // Ordinary cached heap memory, since host-coherent GPU memory is uncached for the CPU.
  std::vector<std::byte> heap_from(kBytes, std::byte{1}), heap_to(kBytes);
  auto copy_bandwidth = [&](unsigned threads) {
    CopyJob jobs[8];
    pthread_t handles[8];
    std::size_t chunk = kBytes / threads;
    for (unsigned t = 0; t < threads; ++t) {
      jobs[t] = {heap_to.data() + t * chunk, heap_from.data() + t * chunk, chunk, 4};
      pthread_create(&handles[t], nullptr, run_copy, &jobs[t]);
    }
    double seconds = 0;
    for (unsigned t = 0; t < threads; ++t) {
      pthread_join(handles[t], nullptr);
      seconds = std::max(seconds, jobs[t].seconds);
    }
    return 2. * 4 * kBytes / seconds * 1e-9;
  };
  for (unsigned threads : {1u, 2u, 4u, 8u})
    std::printf("CPU memcpy, %u threads: %.1f GB/s (read + write)\n", threads,
                copy_bandwidth(threads));
  {
    std::uint32_t specialization[] = {256, unsigned(kBytes / 16), 1, 256 * 16};
    auto kernel = gemm::create_kernel(device, kRead4, 2, 0, specialization);
    if (!kernel) return 1;
    Dispatch dispatch;
    dispatch.kernel = kernel->get();
    dispatch.bindings[0] = {source->get()};
    dispatch.bindings[1] = {destination->get()};
    dispatch.groups = {256, 1, 1};
    auto batch = gemm::create_batch(device);
    if (!batch || !gemm::record(**batch, std::span(&dispatch, 1), 40)) return 1;
    CopyJob background{heap_to.data(), heap_from.data(), kBytes, 12};
    pthread_t handle;
    pthread_create(&handle, nullptr, run_copy, &background);
    if (!gemm::submit(**batch)) return 1;
    auto gpu = gemm::wait(**batch);
    pthread_join(handle, nullptr);
    if (!gpu) return 1;
    std::printf("GPU read during CPU memcpy: %.1f GB/s; CPU memcpy alongside: %.1f GB/s\n",
                40. * kBytes / (gpu->end_ns - gpu->start_ns),
                2. * 12 * kBytes / background.seconds * 1e-9);
  }
  return 0;
}
}

int main(int argc, char** argv) {
  // Line by line, so progress shows when the output goes to a file.
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  std::string_view mode = argc > 1 ? argv[1] : "info";
  auto device = gemm::create_device();
  if (!device) return std::fprintf(stderr, "%s\n", device.error().message.c_str()), 1;
  auto clock = gemm::start_clock_sampler(2000);
  if (!clock) return std::fprintf(stderr, "%s\n", clock.error().message.c_str()), 1;
  if (mode == "info") return run_info(**device);
  if (mode == "kernels") {
    // Statistics for a 2048-cubed problem, as the benchmark specializes it.
    std::uint32_t specialization[] = {0, 2048, 2048, 2048};
    std::vector<gemm::Variant> variants(gemm::get_variants().begin(), gemm::get_variants().end());
    variants.insert(variants.end(), gemm::get_ablations().begin(), gemm::get_ablations().end());
    for (const auto& variant : variants) {
      auto kernel =
          gemm::create_kernel(**device, variant.spirv, variant.a_image ? 4 : 3, 0, specialization,
                              0, (variant.b_image ? 2 : 0) | (variant.a_image ? 8 : 0));
      std::printf("%.*s %.*s\n", int(gemm::get_name(variant.precision).size()),
                  gemm::get_name(variant.precision).data(), int(variant.name.size()),
                  variant.name.data());
      if (!kernel) std::printf("  %s\n", kernel.error().message.c_str());
      // GEMM_DESCRIBE=<label> prints what the driver reports for matching kernels.
      else if (const char* label = std::getenv("GEMM_DESCRIBE");
               label && variant.name.find(label) != std::string_view::npos)
        std::printf("%s", gemm::describe(**kernel).c_str());
      else print_stats(**kernel);
    }
    return 0;
  }
  if (mode == "thermal") return run_thermal(argc > 2 ? std::atoi(argv[2]) : 60);
  if (mode != "matrix" && mode != "gemm" && mode != "quick" && mode != "ablate")
    cool_down(argc > 2 ? std::strtof(argv[2], nullptr) : .5f, 600);
  if (mode == "peak") return run_peak(**device, **clock, argc > 3 ? argv[3] : nullptr);
  if (mode == "bandwidth") return run_bandwidth(**device, **clock);
  if (mode == "loads") return run_loads(**device, **clock);
  if (mode == "dispatch") return run_dispatch(**device, **clock);
  if (mode == "layouts") return run_layouts(**device);
  if (mode == "matrix")
    return run_matrix(**device, **clock, argc > 2 ? std::strtof(argv[2], nullptr) : .5f,
                      argc > 3 ? argv[3] : "");
  if (mode == "quick")
    return run_gemm(**device, **clock, .75f, argc > 2 ? argv[2] : "", argc > 3 ? argv[3] : "",
                    argc > 4 ? argv[4] : "", 1, true);
  if (mode == "ablate")
    return run_ablate(**device, **clock, argc > 2 ? std::strtof(argv[2], nullptr) : .5f,
                      argc > 3 ? argv[3] : "square 2048");
  if (mode == "soak")
    return argc > 6 ? run_soak(**device, **clock, argv[3], argv[4], argv[5],
                               argc > 6 ? std::strtod(argv[6], nullptr) : 600)
                    : (std::fprintf(
                           stderr,
                           "Usage: gemm soak <cooling> <precision> <shape> <kernel> <seconds>\n"),
                       2);
  if (mode == "gemm")
    return run_gemm(**device, **clock, argc > 2 ? std::strtof(argv[2], nullptr) : .5f,
                    argc > 3 ? argv[3] : "", argc > 4 ? argv[4] : "", argc > 5 ? argv[5] : "",
                    argc > 6 ? unsigned(std::atoi(argv[6])) : 1);
  std::fprintf(stderr,
               "Usage: gemm "
               "[info|thermal|kernels|peak|bandwidth|loads|dispatch|layouts|gemm|quick|matrix|"
               "ablate|soak]\n");
  return 2;
}
