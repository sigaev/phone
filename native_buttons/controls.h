#pragma once

#include "common/gpu/math.h"

namespace native_buttons {
enum class Control : int { kNone, kAdd, kReset, kQuality, kPause, kBird };

struct Controls {
  gpu::Rect add, reset, quality, pause, bird;
};
enum class OverlayMode { kPortrait, kLandscape, kCompact };

struct OverlayLayout {
  Controls controls;
  gpu::Rect panel;
  float density;
  OverlayMode mode;
};

OverlayLayout layout_overlay(gpu::Rect safe, float density = 1);
Controls layout_controls(gpu::Rect safe, float density = 1);
Control hit_test(Controls controls, float x, float y);
gpu::Rect safe_area(gpu::Rect content, int width, int height);
}
