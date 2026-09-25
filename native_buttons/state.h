#pragma once

#include <array>
#include <cstddef>
#include <span>

#include "common/result.h"

namespace native_buttons {
struct SessionState {
    // -1 selects the count on disk when no Activity state was restored.
    int count = -1;
    bool maximum = false, paused = false;
    float yaw = .34f;
    double time = 0;
    float zoom = 1;
};
using SavedState = std::array<std::byte, 32>;
SavedState encode_state(const SessionState& state);
common::Result<SessionState> decode_state(std::span<const std::byte> bytes);
}  // namespace native_buttons
