#include "gemm/measure.h"

#include <time.h>

#include <algorithm>
#include <cmath>

namespace gemm {
using common::Error;
using common::Owner;
using common::Result;

namespace {
double now() {
  timespec time;
  clock_gettime(CLOCK_MONOTONIC, &time);
  return double(time.tv_sec) + time.tv_nsec * 1e-9;
}

double median(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  size_t n = values.size();
  return n % 2 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2;
}

// Spread, least-squares trend across the span, drift between the halves, and
// clock agreement of the last windows.
void assess(Steady& s, unsigned windows) {
  auto last = std::span(s.history).last(windows);
  std::vector<double> times, clocks;
  for (const auto& w : last) {
    times.push_back(w.seconds);
    clocks.push_back(w.clock.mean_mhz);
  }
  s.seconds = median(times);
  auto [low, high] = std::minmax_element(times.begin(), times.end());
  s.spread = (*high - *low) / s.seconds;
  // A single window, as fixed-length runs of long GEMMs can have, shows no trend.
  s.drift = s.trend = 0;
  if (windows >= 2) {
    unsigned half = windows / 2;
    std::vector<double> earlier(times.begin(), times.begin() + half),
        later(times.end() - half, times.end());
    s.drift = (median(later) - median(earlier)) / s.seconds;
    double mean_x = (windows - 1) / 2., mean_y = 0, covariance = 0, variance = 0;
    for (double t : times) mean_y += t / windows;
    for (unsigned i = 0; i < windows; ++i) {
      covariance += (i - mean_x) * (times[i] - mean_y);
      variance += (i - mean_x) * (i - mean_x);
    }
    s.trend = covariance / variance * (windows - 1) / s.seconds;
  }
  s.clock_mhz = median(clocks);
  auto [slow, fast] = std::minmax_element(clocks.begin(), clocks.end());
  s.clock_spread = s.clock_mhz > 0 ? (*fast - *slow) / s.clock_mhz : 0;
}

// No drift across the windows, by their trend and their halves' medians, noise
// within bounds, and a steady GPU clock.
bool is_steady(const Steady& s, const SteadyOptions& options) {
  return std::abs(s.trend) <= options.tolerance / 2 && std::abs(s.drift) <= options.tolerance / 2 &&
         s.spread <= options.noise && s.clock_spread <= .01;
}
}

Result<Steady> measure_steady(Device& device, ClockSampler& clock,
                              std::span<const Dispatch> dispatches, const SteadyOptions& options) {
  Owner<Batch> batches[2];
  for (auto& batch : batches) {
    auto created = create_batch(device);
    if (!created) return std::unexpected(created.error());
    batch = std::move(*created);
  }
  // Find a repeat count that fills a window, doubling from one.
  Steady s;
  unsigned repeats = 1;
  double per_repeat = 0;
  for (;;) {
    if (auto r = record(*batches[0], dispatches, repeats); !r) return std::unexpected(r.error());
    if (auto r = submit(*batches[0]); !r) return std::unexpected(r.error());
    auto times = wait(*batches[0]);
    if (!times) return std::unexpected(times.error());
    double time = (times->end_ns - times->start_ns) * 1e-9;
    per_repeat = time / repeats;
    if (time >= options.window_seconds / 4 || repeats >= (1u << 20)) break;
    repeats *= 2;
  }
  s.repeats =
      std::max(options.minimum_repeats, unsigned(std::lround(options.window_seconds / per_repeat)));
  for (auto& batch : batches)
    if (auto r = record(*batch, dispatches, s.repeats); !r) return std::unexpected(r.error());
  take_clock_stats(clock);
  double start = now();
  for (auto& batch : batches)
    if (auto r = submit(*batch); !r) return std::unexpected(r.error());
  // Each window is timed from the end of the one before, since two batches in
  // flight keep the GPU busy: consecutive end times tile the GPU's time exactly,
  // while a batch's start time can be taken late, after its work has begun.
  double previous_end = -1;
  for (unsigned next = 0;; next ^= 1) {
    auto times = wait(*batches[next]);
    if (!times) return std::unexpected(times.error());
    double elapsed = now() - start;
    double time = times->end_ns - (previous_end < 0 ? times->start_ns : previous_end);
    previous_end = times->end_ns;
    s.history.push_back({time * 1e-9 / s.repeats, elapsed, take_clock_stats(clock)});
    if (options.on_window) options.on_window(s.history.back(), options.context);
    if (options.duration > 0) {
      if (elapsed < options.duration) {
        if (auto r = submit(*batches[next]); !r) return std::unexpected(r.error());
        continue;
      }
      s.elapsed = elapsed;
      if (auto rest = wait(*batches[next ^ 1]); !rest) return std::unexpected(rest.error());
      assess(s, std::min<unsigned>(options.windows, unsigned(s.history.size())));
      s.steady = is_steady(s, options);
      return s;
    }
    if (s.history.size() >= options.windows && elapsed >= options.minimum_seconds) {
      assess(s, options.windows);
      s.steady = is_steady(s, options);
    }
    if (s.steady || elapsed >= options.maximum_seconds) {
      s.elapsed = elapsed;
      if (auto rest = wait(*batches[next ^ 1]); !rest) return std::unexpected(rest.error());
      if (s.history.size() >= options.windows && !s.steady) assess(s, options.windows);
      return s;
    }
    if (auto r = submit(*batches[next]); !r) return std::unexpected(r.error());
  }
}
}
