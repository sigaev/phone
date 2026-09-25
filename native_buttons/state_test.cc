#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
    std::int32_t legacy = 73;
    auto restored = decode_state({reinterpret_cast<const std::byte*>(&legacy), sizeof(legacy)});
    if (!restored || restored->count != 73 || restored->maximum || restored->paused ||
        restored->zoom != 1 || restored->bird != Bird::kPelican)
        return 2;
    auto bytes = encode_state(SessionState{.count = 20});
    struct LegacyRecord {
        std::uint32_t magic, version;
        std::int32_t count;
        std::uint32_t flags;
        float yaw, time, zoom;
    };
    for (unsigned version : {1u, 2u}) {
        LegacyRecord previous{0x50454c49, version, 20, 3, -1.25f, 524288.f, 1.75f};
        restored =
            decode_state({reinterpret_cast<const std::byte*>(&previous), version == 1 ? 24u : 28u});
        if (!restored || restored->count != 20 || !restored->maximum || !restored->paused ||
            restored->yaw != previous.yaw || restored->time != previous.time ||
            restored->zoom != (version == 1 ? 1.f : previous.zoom) ||
            restored->bird != Bird::kPelican)
            return 6;
    }
    // Version 3 used the same layout, with no bird flag. Preserve its pelican.
    auto previous = encode_state(SessionState{.count = 20, .bird = Bird::kPelican});
    previous[4] = std::byte{3};
    restored = decode_state(previous);
    if (!restored || restored->bird != Bird::kPelican)
        return 8;
    previous[12] |= std::byte{4};
    if (decode_state(previous))
        return 9;
    // Preserve sub-frame precision across recreation, beyond float's useful range.
    for (double time : {65536. + 1. / 60, 524288. + 1. / 60, 31536000. + 1. / 120}) {
        auto original = SessionState{.count = 20, .time = time};
        restored = decode_state(encode_state(original));
        if (!restored || restored->time != time)
            return 7;
    }
    for (std::size_t length = 0; length < bytes.size(); ++length)
        if (length != sizeof(legacy) && decode_state({bytes.data(), length}))
            return 3;
    // Unknown versions and flag bits are rejected rather than reinterpreted.
    for (int offset : {0, 4, 15}) {
        auto invalid = bytes;
        invalid[offset] = std::byte{0xff};
        if (decode_state(invalid))
            return 4;
    }
    for (auto invalid :
         {SessionState{.count = 1, .bird = static_cast<Bird>(2)}, SessionState{.count = -1},
          SessionState{.count = 1000000},
          SessionState{.count = 1, .yaw = std::numeric_limits<float>::infinity()},
          SessionState{.count = 1, .time = std::numeric_limits<double>::quiet_NaN()},
          SessionState{.count = 1, .time = std::numeric_limits<double>::infinity()},
          SessionState{.count = 1, .time = -1}, SessionState{.count = 1, .zoom = .25f},
          SessionState{.count = 1, .zoom = 3},
          SessionState{.count = 1, .zoom = std::numeric_limits<float>::quiet_NaN()}})
        if (decode_state(encode_state(invalid)))
            return 5;
    std::puts("Versioned Activity state, legacy count state and invalid records passed");
}
