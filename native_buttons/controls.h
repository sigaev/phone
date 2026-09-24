#pragma once

#include "common/gpu/math.h"

namespace native_buttons {
enum class Control : int { kNone, kAdd, kReset, kQuality, kPause };
struct Controls {
    gpu::Rect add, reset, quality, pause;
};
Controls layout_controls(gpu::Rect safe);
Control hit_test(Controls controls, float x, float y);
gpu::Rect safe_area(gpu::Rect content, int width, int height);
}  // namespace native_buttons
