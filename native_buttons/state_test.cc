#include <cstdio>
#include <limits>

#include "native_buttons/state.h"

int main() {
  using namespace native_buttons;
  for (int count : {0, 12, 999999})
    for (bool maximum : {false, true})
      for (bool paused : {false, true})
        for (Bird bird : {Bird::kPelican, Bird::kFlamingo}) {
          SessionState original{count, maximum, paused, -2.25f, 12345.5f, 1.75f, bird};
          auto restored = decode_state(encode_state(original));
          if (!restored || restored->count != count || restored->maximum != maximum ||
              restored->paused != paused || restored->yaw != original.yaw ||
              restored->time != original.time || restored->zoom != original.zoom ||
              restored->bird != bird)
            return 1;
        }
  auto bytes = encode_state(SessionState{.count = 20});
  // Preserve sub-frame precision across recreation, beyond float's useful range.
  for (double time : {65536. + 1. / 60, 524288. + 1. / 60, 31536000. + 1. / 120}) {
    auto original = SessionState{.count = 20, .time = time};
    auto restored = decode_state(encode_state(original));
    if (!restored || restored->time != time) return 7;
  }
  // Only the current record is accepted; shorter or earlier formats are rejected.
  for (std::size_t length = 0; length < bytes.size(); ++length)
    if (decode_state({bytes.data(), length})) return 3;
  auto previous = bytes;
  previous[4] = std::byte{3};
  if (decode_state(previous)) return 8;
  // Unknown versions and flag bits are rejected rather than reinterpreted.
  for (int offset : {0, 4, 15}) {
    auto invalid = bytes;
    invalid[offset] = std::byte{0xff};
    if (decode_state(invalid)) return 4;
  }
  for (auto invalid : {SessionState{.count = 1, .bird = static_cast<Bird>(2)},
                       SessionState{.count = -1}, SessionState{.count = 1000000},
                       SessionState{.count = 1, .yaw = std::numeric_limits<float>::infinity()},
                       SessionState{.count = 1, .time = std::numeric_limits<double>::quiet_NaN()},
                       SessionState{.count = 1, .time = std::numeric_limits<double>::infinity()},
                       SessionState{.count = 1, .time = -1}, SessionState{.count = 1, .zoom = .25f},
                       SessionState{.count = 1, .zoom = 3},
                       SessionState{.count = 1, .zoom = std::numeric_limits<float>::quiet_NaN()}})
    if (decode_state(encode_state(invalid))) return 5;
  std::puts("Versioned Activity state and invalid records passed");
}
