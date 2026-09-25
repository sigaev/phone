#pragma once

#include <string>

#include "common/owner.h"
#include "common/result.h"
#include "native_buttons/controls.h"
#include "native_buttons/state.h"

struct ANativeWindow;

namespace native_buttons {
struct Runtime;
struct RuntimeState : SessionState {
    Control pressed = Control::kNone;
    Control focused = Control::kNone;
    bool saved = true;
    unsigned frames = 0;
    gpu::Rect safe;
    std::string error, save_error;
};
enum class Key { kNext, kPrevious, kActivate };
enum class Touch { kDown, kMove, kUp, kCancel };

common::Result<common::Owner<Runtime>> create_runtime(const char* directory,
                                                      SessionState restored = {});
void destroy(Runtime* runtime) noexcept;
// A null window with positive dimensions selects offscreen rendering for tests.
common::Result<void> set_surface(Runtime& runtime, ANativeWindow* window, int width = 0,
                                 int height = 0);
// NativeActivity requires drawing to stop before detach returns. If the worker
// cannot release its surface within three seconds, terminate the process rather
// than return with live window users. Destruction uses the same bounded policy.
common::Result<void> detach_surface(Runtime& runtime);
// Fail after three seconds if a required redraw cannot complete.
common::Result<void> redraw(Runtime& runtime);
// A paused Activity can remain visible in split screen. Stop monitoring only
// when it is hidden (onStop), not when it loses foreground interaction (onPause).
void set_visible(Runtime& runtime, bool visible);
void set_resumed(Runtime& runtime, bool resumed);
void set_content(Runtime& runtime, gpu::Rect content);
void activate(Runtime& runtime, Control control);
void focus_control(Runtime& runtime, Control control);
void key(Runtime& runtime, Key action);
void touch(Runtime& runtime, Touch action, float x, float y);
// Relative finger-span change: values above one zoom in; below one zoom out.
void pinch(Runtime& runtime, float scale);
// Pass ViewConfiguration.getScaledTouchSlop() in window pixels.
void set_touch_slop(Runtime& runtime, float pixels);
void set_density(Runtime& runtime, float pixels_per_dp);
RuntimeState get_state(Runtime& runtime);
// Lifecycle-only barrier: include startup loading and all previously queued actions.
common::Result<RuntimeState> capture_state(Runtime& runtime);
int notification_fd(const Runtime& runtime);
void acknowledge_notifications(Runtime& runtime);
}  // namespace native_buttons
