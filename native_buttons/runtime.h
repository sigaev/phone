#pragma once

#include <string>

#include "common/owner.h"
#include "common/result.h"
#include "native_buttons/controls.h"

struct ANativeWindow;

namespace native_buttons {
struct Runtime;
struct RuntimeState {
    int count = 0;
    Control pressed = Control::kNone;
    Control focused = Control::kNone;
    bool maximum = false, paused = false, saved = true;
    float yaw = .34f, time = 0;
    unsigned frames = 0;
    gpu::Rect safe;
    std::string error, save_error;
};
enum class Key { kNext, kPrevious, kActivate };
enum class Touch { kDown, kMove, kUp, kCancel };

common::Result<common::Owner<Runtime>> create_runtime(const char* directory, int restored = -1);
void destroy(Runtime* runtime) noexcept;
// A null window with positive dimensions selects offscreen rendering for tests.
common::Result<void> set_surface(Runtime& runtime, ANativeWindow* window, int width = 0,
                                 int height = 0);
// These two NativeActivity handshakes wait for the worker to finish using/drawing
// the surface. Ordinary state/input updates below never wait for GPU work.
common::Result<void> detach_surface(Runtime& runtime);
common::Result<void> redraw(Runtime& runtime);
void set_resumed(Runtime& runtime, bool resumed);
void set_content(Runtime& runtime, gpu::Rect content);
void activate(Runtime& runtime, Control control);
void focus_control(Runtime& runtime, Control control);
void key(Runtime& runtime, Key action);
void touch(Runtime& runtime, Touch action, float x, float y);
RuntimeState get_state(Runtime& runtime);
// Lifecycle-only barrier: include startup loading and all previously queued actions.
RuntimeState capture_state(Runtime& runtime);
int notification_fd(const Runtime& runtime);
void acknowledge_notifications(Runtime& runtime);
}  // namespace native_buttons
