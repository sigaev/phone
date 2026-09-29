#pragma once

#include <span>
#include <vector>

#include "common/result.h"
#include "gemm/device.h"
#include "gemm/sensors.h"

namespace gemm {

struct Window {
  double seconds = 0;  // GPU time per repeat of the dispatches
  double elapsed = 0;  // wall time from the first window
  ClockStats clock;
};

// Steady state means no drift: across the last `windows` windows, the
// least-squares trend and the difference between the halves' medians are both
// within tolerance / 2, their range within `noise`, and the GPU clock within 1%.
// Windows average at least `minimum_repeats` runs, as single long GEMMs vary by a
// few percent from run to run.
struct SteadyOptions {
  double window_seconds = .1;  // GPU time per measured window
  unsigned minimum_repeats = 4;
  unsigned windows = 8;
  double tolerance = .02;
  double noise = .05;
  double minimum_seconds = 1;   // load before steady state may be declared
  double maximum_seconds = 30;  // give up and report an unsteady result
  // A soak runs for exactly this long instead of stopping at steady state.
  double duration = 0;
  void (*on_window)(const Window& window, void* context) = nullptr;
  void* context = nullptr;
};

struct Steady {
  bool steady = false;
  double seconds = 0;  // median GPU time per repeat over the steady windows
  // Relative to the median over the steady windows: range, trend from first to
  // last, and the later half's median less the earlier half's.
  double spread = 0, trend = 0, drift = 0;
  double clock_mhz = 0, clock_spread = 0;
  double elapsed = 0;    // wall seconds of load until steady state
  unsigned repeats = 0;  // repeats per window
  std::vector<Window> history;
};

// Runs the dispatches back to back, two batches in flight, until the last
// `windows` windows agree in time and GPU clock.
common::Result<Steady> measure_steady(Device& device, ClockSampler& clock,
                                      std::span<const Dispatch> dispatches,
                                      const SteadyOptions& options = {});

}
