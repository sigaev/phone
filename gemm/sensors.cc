#include "gemm/sensors.h"

#include <android/thermal.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <new>

namespace gemm {
using common::Error;
using common::Owner;
using common::Result;

namespace {
// The Mali driver's current DVFS frequency in kHz. Its directory is not listable
// from apps, but this file is readable.
constexpr char kClockPath[] = "/sys/devices/platform/1f000000.mali/cur_freq";

double read_mhz(int fd) {
  char text[32];
  ssize_t length = pread(fd, text, sizeof(text) - 1, 0);
  if (length <= 0) return 0;
  text[length] = 0;
  return std::strtod(text, nullptr) / 1000;
}

// The runtime lacks libc++abi's guarded statics, so initialize once explicitly.
AThermalManager* manager = nullptr;
pthread_once_t manager_once = PTHREAD_ONCE_INIT;

AThermalManager* thermal_manager() {
  pthread_once(&manager_once, [] { manager = AThermal_acquireManager(); });
  return manager;
}
}

struct ClockSampler {
  int fd = -1;
  unsigned interval = 0;
  pthread_t thread{};
  bool started = false;
  std::atomic<bool> stop{false};
  pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  double sum = 0, minimum = 0, maximum = 0;
  unsigned samples = 0;
};

namespace {
void* sample(void* data) {
  auto& s = *static_cast<ClockSampler*>(data);
  timespec next;
  clock_gettime(CLOCK_MONOTONIC, &next);
  while (!s.stop.load(std::memory_order_relaxed)) {
    double mhz = read_mhz(s.fd);
    pthread_mutex_lock(&s.mutex);
    if (mhz > 0) {
      s.minimum = s.samples ? std::min(s.minimum, mhz) : mhz;
      s.maximum = s.samples ? std::max(s.maximum, mhz) : mhz;
      s.sum += mhz;
      ++s.samples;
    }
    pthread_mutex_unlock(&s.mutex);
    next.tv_nsec += long(s.interval) * 1000;
    while (next.tv_nsec >= 1000000000) {
      next.tv_nsec -= 1000000000;
      ++next.tv_sec;
    }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr);
  }
  return nullptr;
}
}

void destroy(ClockSampler* s) noexcept {
  if (!s) return;
  s->stop = true;
  if (s->started) pthread_join(s->thread, nullptr);
  if (s->fd >= 0) close(s->fd);
  delete s;
}

Result<Owner<ClockSampler>> start_clock_sampler(unsigned interval_microseconds) {
  Owner<ClockSampler> owner(new (std::nothrow) ClockSampler);
  if (!owner) return std::unexpected(Error{"Cannot allocate the clock sampler"});
  owner->interval = std::max(interval_microseconds, 100u);
  owner->fd = open(kClockPath, O_RDONLY | O_CLOEXEC);
  if (owner->fd < 0 || read_mhz(owner->fd) <= 0)
    return std::unexpected(Error{"Cannot read the GPU clock"});
  if (pthread_create(&owner->thread, nullptr, sample, owner.get()))
    return std::unexpected(Error{"Cannot start the clock sampler"});
  owner->started = true;
  return owner;
}

ClockStats take_clock_stats(ClockSampler& s) {
  pthread_mutex_lock(&s.mutex);
  ClockStats stats{s.samples ? s.sum / s.samples : 0, s.minimum, s.maximum, s.samples};
  s.sum = s.minimum = s.maximum = 0;
  s.samples = 0;
  pthread_mutex_unlock(&s.mutex);
  return stats;
}

double read_gpu_mhz() {
  int fd = open(kClockPath, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return 0;
  double mhz = read_mhz(fd);
  close(fd);
  return mhz;
}

int thermal_status() {
  AThermalManager* manager = thermal_manager();
  return manager ? int(AThermal_getCurrentThermalStatus(manager)) : -1;
}

float thermal_headroom(int forecast_seconds) {
  AThermalManager* manager = thermal_manager();
  return manager ? AThermal_getThermalHeadroom(manager, forecast_seconds) : NAN;
}
}
