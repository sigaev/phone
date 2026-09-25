#include "native_buttons/controls.h"

#include <algorithm>
#include <cmath>

namespace native_buttons {
OverlayLayout layout_overlay(gpu::Rect safe, float density) {
    float s = std::isfinite(density) && density > 0 ? density : 1;
    OverlayLayout layout{{}, {}, s, OverlayMode::kPortrait};
    // Preserve physical target sizes. A short window gets a horizontal toolbar,
    // and landscape gets a side panel instead of shrinking the portrait design.
    bool landscape = safe.w > safe.h && safe.w >= 480 * s;
    if (safe.w < 300 * s || safe.h < (landscape ? 280 : 420) * s) {
        layout.mode = OverlayMode::kCompact;
        bool single_row = safe.w >= 264 * s;
        float height = (single_row ? 96 : 156) * s;
        layout.panel = {safe.x + 8 * s, safe.y + safe.h - height - 8 * s, safe.w - 16 * s, height};
        float width = single_row ? (layout.panel.w - 40 * s) / 4 : (layout.panel.w - 24 * s) / 2;
        float x = layout.panel.x + 8 * s, y = layout.panel.y + 40 * s;
        float add_x = single_row ? x + 2 * (width + 8 * s) : x;
        float add_y = single_row ? y : y + 60 * s;
        layout.controls = {{add_x, add_y, width, 48 * s},
                           {add_x + width + 8 * s, add_y, width, 48 * s},
                           {x, y, width, 48 * s},
                           {x + width + 8 * s, y, width, 48 * s}};
    } else if (landscape) {
        layout.mode = OverlayMode::kLandscape;
        layout.panel = {safe.x + safe.w - 264 * s, safe.y + (safe.h - 256 * s) * .5f, 248 * s,
                        256 * s};
        float x = layout.panel.x + 16 * s, y = layout.panel.y;
        layout.controls = {{x, y + 192 * s, 102 * s, 48 * s},
                           {x + 114 * s, y + 192 * s, 102 * s, 48 * s},
                           {x, y + 132 * s, 102 * s, 48 * s},
                           {x + 114 * s, y + 132 * s, 102 * s, 48 * s}};
    } else {
        float width = std::min(360 * s, std::max(0.f, safe.w - 32 * s));
        float x = safe.x + (safe.w - width) * .5f;
        layout.panel = {x, safe.y + safe.h - 176 * s, width, 156 * s};
        float gap = 12 * s, inner = std::max(0.f, width - 40 * s);
        float reset_width = std::max(48 * s, inner * .275f);
        float add_width = inner - gap - reset_width;
        float y = layout.panel.y + 88 * s;
        float quality_width = std::min(114 * s, (width - 32 * s) * .5f);
        layout.controls = {{x + 20 * s, y, add_width, 48 * s},
                           {x + 20 * s + add_width + gap, y, reset_width, 48 * s},
                           {x + 10 * s, safe.y + 141 * s, quality_width, 48 * s},
                           {x + 22 * s + quality_width, safe.y + 141 * s,
                            std::min(84 * s, quality_width), 48 * s}};
    }
    return layout;
}
Controls layout_controls(gpu::Rect safe, float density) {
    return layout_overlay(safe, density).controls;
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
