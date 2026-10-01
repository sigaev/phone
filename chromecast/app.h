#pragma once

#include <string>

#include "chromecast/session.h"
#include "chromecast/view.h"
#include "common/gpu/math.h"
#include "common/owner.h"
#include "common/result.h"

struct ANativeWindow;

namespace gpu {
struct Renderer;
}

namespace chromecast {
struct App;
struct Platform;
enum class Touch { kDown, kMove, kUp, kCancel };

// Starts a session that searches the phone's Wi-Fi networks right away.
common::Result<common::Owner<App>> create_app(Platform& platform, const SessionConfig& config);
void destroy(App* app) noexcept;
// A null window with positive dimensions renders offscreen for tests.
common::Result<void> attach_window(App& app, ANativeWindow* window, int width = 0, int height = 0);
// Waits for the GPU; the window may be released afterwards.
void detach_window(App& app);
gpu::Renderer* get_renderer(App& app);
// Content is the window area outside system bars and cutouts.
void set_content(App& app, gpu::Rect content);
void set_density(App& app, float pixels_per_dp);
// Pass ViewConfiguration.getScaledTouchSlop() in window pixels.
void set_touch_slop(App& app, float pixels);
void touch(App& app, Touch action, float x, float y, double now);
// Back goes up one screen; false when the app is on its first screen.
bool back(App& app);
// Enter connects from the password screen.
void enter(App& app);
// Paste was tapped; the caller reads the clipboard and calls paste().
bool take_paste_request(App& app);
void paste(App& app, const std::string& text);
// Readable when the session has news; then call on_session().
int app_fd(const App& app);
void on_session(App& app);
// Advance animations; true when a frame should be drawn.
bool update(App& app, double now);
// False when the frame was deferred and must be retried on a later frame.
common::Result<bool> draw(App& app, double now);
// Draw and wait for the frame, retrying transient deferrals a few times.
common::Result<void> redraw(App& app, double now);
// Poll for window size and rotation changes that arrive without a callback.
common::Result<bool> check_surface(App& app);
Screen get_screen(const App& app);
const Snapshot& get_snapshot(const App& app);
const std::string& get_password(const App& app);
// Layout of the last presented frame, which input is tested against.
const Layout& get_layout(const App& app);
}
