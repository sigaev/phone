#pragma once

#include "common/owner.h"
#include "common/result.h"

namespace gemm {
struct ClockSampler;

struct ClockStats {
  double mean_mhz = 0, min_mhz = 0, max_mhz = 0;
  unsigned samples = 0;
};

// Samples the Mali GPU clock from sysfs on a background thread.
common::Result<common::Owner<ClockSampler>> start_clock_sampler(unsigned interval_microseconds);
void destroy(ClockSampler* sampler) noexcept;
// Statistics since the previous call.
ClockStats take_clock_stats(ClockSampler& sampler);
// The current GPU clock, or 0 when it cannot be read.
double read_gpu_mhz();

// Android's thermal status (AThermalStatus), or -1 when unavailable.
int thermal_status();
// Android's forecast thermal headroom, where 1 means severe throttling, or NaN
// when unavailable. Android rate-limits it to about one call per second.
float thermal_headroom(int forecast_seconds);
}
