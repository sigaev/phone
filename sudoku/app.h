#pragma once

#include <cstdint>
#include <string>

#include "common/gpu/math.h"
#include "common/owner.h"
#include "common/result.h"
#include "sudoku/view.h"

struct ANativeWindow;

namespace gpu {
struct Renderer;
}

namespace sudoku {
struct App;
enum class Touch { kDown, kMove, kUp, kCancel };
enum class Key {
  kDigit,
  kErase,
  kUndo,
  kNotes,
  kPause,
  kNewGame,
  kLeft,
  kRight,
  kUp,
  kDown,
  kEnter
};

struct AppState {
  bool has_game = false, paused = false, pending = false;
  Board board;
  Dialog dialog = Dialog::kNone;
};

// Load the game saved in directory, or start generating a new one. Times are
// monotonic seconds supplied by the caller.
common::Result<common::Owner<App>> create_app(const char* directory, std::uint64_t seed);
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
// The clock runs only while the Activity is resumed. Pausing saves the game.
void set_resumed(App& app, bool resumed, double now);
void touch(App& app, Touch action, float x, float y, double now);
// Keyboard and D-pad input; digit is used by Key::kDigit.
void press(App& app, Key key, int digit, double now);
// Dismiss an open dialog as its secondary button would. Returns false if none was open.
bool dismiss(App& app, double now);
// Advance the clock and animations; true when a frame should be drawn.
bool update(App& app, double now);
// False when the frame was deferred and must be retried on a later frame.
common::Result<bool> draw(App& app, double now);
// Draw and wait for the frame, retrying transient deferrals a few times.
common::Result<void> redraw(App& app, double now);
// Poll for window size and rotation changes that arrive without a callback.
// Returns whether the window changed.
common::Result<bool> check_surface(App& app);
// Readable when the background generator has a puzzle ready.
int puzzle_fd(const App& app);
void on_puzzle_ready(App& app, double now);
// A persistence problem to show the user once; empty when there is none.
std::string take_notice(App& app);
AppState get_state(const App& app);
// Layout of the last presented frame, which input is tested against.
const Layout& get_layout(const App& app);
}
