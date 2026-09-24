#include "native_buttons/controls.h"

#include <algorithm>

namespace native_buttons {
Controls layout_controls(gpu::Rect safe) {
    float s = std::max(0.f, std::min(safe.w / 400.f, safe.h / 720.f));
    float cx = safe.x + safe.w * .5f, top = safe.y + 22 * s;
    float panel_y = safe.y + safe.h - 176 * s;
    return {{cx - 160 * s, panel_y + 88 * s, 220 * s, 48 * s},
            {cx + 72 * s, panel_y + 88 * s, 88 * s, 48 * s},
            {cx - 170 * s, top + 119 * s, 114 * s, 33 * s},
            {cx - 46 * s, top + 119 * s, 84 * s, 33 * s}};
}
Control hit_test(Controls c, float x, float y) {
    if (gpu::contains(c.add, x, y))
        return Control::kAdd;
    if (gpu::contains(c.reset, x, y))
        return Control::kReset;
    if (gpu::contains(c.quality, x, y))
        return Control::kQuality;
    if (gpu::contains(c.pause, x, y))
        return Control::kPause;
    return Control::kNone;
}
gpu::Rect safe_area(gpu::Rect content, int width, int height) {
    if (content.w <= 0 || content.h <= 0)
        return {0, 0, float(width), float(height)};
    float left = std::clamp(content.x, 0.f, float(width));
    float top = std::clamp(content.y, 0.f, float(height));
    return {left, top, std::clamp(content.x + content.w, left, float(width)) - left,
            std::clamp(content.y + content.h, top, float(height)) - top};
}
}  // namespace native_buttons
