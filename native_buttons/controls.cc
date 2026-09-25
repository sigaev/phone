#include "native_buttons/controls.h"

#include <algorithm>
#include <cmath>

namespace native_buttons {
OverlayLayout layout_overlay(gpu::Rect safe, float density) {
  float s = std::isfinite(density) && density > 0 ? density : 1;
  OverlayLayout layout{{}, {}, s, OverlayMode::kPortrait};
  // Preserve physical target sizes. A short window gets a horizontal toolbar,
  // and landscape gets a side panel instead of shrinking the portrait design.
  bool landscape = safe.w > safe.h && safe.w >= 560 * s;
  if (safe.w < 300 * s || safe.h < (landscape ? 280 : 420) * s) {
    layout.mode = OverlayMode::kCompact;
    bool single_row = safe.w >= 424 * s || (safe.h < 172 * s && safe.w >= 304 * s);
    bool three_rows =
        (safe.w < 196 * s && safe.h >= 200 * s) || (safe.w < 300 * s && safe.h >= 232 * s);
    bool tight_two = !three_rows && safe.w < 196 * s;
    bool tight_rows = three_rows && safe.h < 232 * s;
    float row_step = (tight_rows ? 52 : tight_two ? 56 : 60) * s;
    float header = (tight_rows || tight_two ? 24 : 40) * s;
    float height = header + 56 * s + (single_row ? 0 : three_rows ? 2 : 1) * row_step;
    layout.panel = {safe.x + 8 * s, safe.y + safe.h - height - 8 * s, safe.w - 16 * s, height};
    // Three 48-dp targets also fit a 160-dp-wide short window by using
    // the panel's full width; rounded corners still separate the buttons.
    float gap = tight_two ? 0 : 8 * s;
    float x = layout.panel.x + gap, y = layout.panel.y + header;
    float row_width = layout.panel.w - 2 * gap;
    int columns = single_row ? 5 : three_rows ? 2 : 3;
    float width = (row_width - (columns - 1) * gap) / columns;
    float stride = width + gap;
    float add_width = single_row ? width : (row_width - 8 * s) / 2;
    float add_x = single_row ? x + 3 * stride : x;
    float add_y = single_row ? y : y + (three_rows ? 2 : 1) * row_step;
    layout.controls = {{add_x, add_y, add_width, 48 * s},
                       {add_x + add_width + 8 * s, add_y, add_width, 48 * s},
                       {x, y, width, 48 * s},
                       {x + stride, y, width, 48 * s},
                       {three_rows ? x : x + 2 * stride, three_rows ? y + row_step : y,
                        three_rows ? row_width : width, 48 * s}};
  } else if (landscape) {
    layout.mode = OverlayMode::kLandscape;
    layout.panel = {safe.x + safe.w - 296 * s, safe.y + (safe.h - 256 * s) * .5f, 280 * s, 256 * s};
    float x = layout.panel.x + 16 * s, y = layout.panel.y;
    layout.controls = {{x, y + 192 * s, 118 * s, 48 * s},
                       {x + 130 * s, y + 192 * s, 118 * s, 48 * s},
                       {x, y + 132 * s, 77 * s, 48 * s},
                       {x + 85 * s, y + 132 * s, 77 * s, 48 * s},
                       {x + 170 * s, y + 132 * s, 78 * s, 48 * s}};
  } else {
    float width = std::min(360 * s, std::max(0.f, safe.w - 32 * s));
    float x = safe.x + (safe.w - width) * .5f;
    layout.panel = {x, safe.y + safe.h - 176 * s, width, 156 * s};
    float gap = 12 * s, inner = std::max(0.f, width - 40 * s);
    float reset_width = std::max(48 * s, inner * .275f);
    float add_width = inner - gap - reset_width;
    float y = layout.panel.y + 88 * s;
    float toggle_width = (width - 36 * s) / 3;
    layout.controls = {{x + 20 * s, y, add_width, 48 * s},
                       {x + 20 * s + add_width + gap, y, reset_width, 48 * s},
                       {x + 10 * s, safe.y + 141 * s, toggle_width, 48 * s},
                       {x + 18 * s + toggle_width, safe.y + 141 * s, toggle_width, 48 * s},
                       {x + 26 * s + 2 * toggle_width, safe.y + 141 * s, toggle_width, 48 * s}};
  }
  return layout;
}

Controls layout_controls(gpu::Rect safe, float density) {
  return layout_overlay(safe, density).controls;
}

Control hit_test(Controls c, float x, float y) {
  if (gpu::contains(c.add, x, y)) return Control::kAdd;
  if (gpu::contains(c.reset, x, y)) return Control::kReset;
  if (gpu::contains(c.quality, x, y)) return Control::kQuality;
  if (gpu::contains(c.pause, x, y)) return Control::kPause;
  if (gpu::contains(c.bird, x, y)) return Control::kBird;
  return Control::kNone;
}

gpu::Rect safe_area(gpu::Rect content, int width, int height) {
  if (content.w <= 0 || content.h <= 0) return {0, 0, float(width), float(height)};
  float left = std::clamp(content.x, 0.f, float(width));
  float top = std::clamp(content.y, 0.f, float(height));
  return {left, top, std::clamp(content.x + content.w, left, float(width)) - left,
          std::clamp(content.y + content.h, top, float(height)) - top};
}
}
