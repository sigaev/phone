#include "sudoku/app.h"

#include <android/log.h>

#include <cmath>
#include <new>
#include <optional>

#include "common/gpu/renderer.h"
#include "sudoku/generator.h"
#include "sudoku/storage.h"

namespace sudoku {
using common::Error;
using common::Owner;
using common::Result;

namespace {
constexpr char kSaveName[] = "game.bin";
constexpr gpu::Color kBackground{1, 1, 1};
}

struct App {
  std::string directory;
  Owner<Generator> generator;
  Owner<Game> game;
  Owner<gpu::Renderer> renderer;
  gpu::Rect content;
  float density = 1, touch_slop = 8;
  Layout layout;
  bool laid_out = false;
  ViewState view;
  bool resumed = false, dirty = true, pending = false, moved = false;
  // Whether the finished game's result dialog has been shown.
  bool result_shown = false;
  // Time of the last clock update while running; negative when stopped.
  double clock = -1, flash_start = -1;
  int shown_second = -1;
  float down_x = 0, down_y = 0;
  std::string notice;
};

namespace {
std::optional<Board> board(const App& a) {
  if (!a.game) return std::nullopt;
  return get_board(*a.game);
}

bool playing(const App& a) { return a.game && get_board(*a.game).status == Status::kPlaying; }

bool running(const App& a) {
  return playing(a) && a.resumed && a.renderer && !a.view.paused && a.view.dialog == Dialog::kNone;
}

bool interactive(const App& a) {
  return playing(a) && !a.view.paused && a.view.dialog == Dialog::kNone;
}

void save(App& a) {
  if (!a.game) return;
  auto bytes = encode_game(*a.game);
  if (auto result = save_file(a.directory.c_str(), kSaveName, bytes); !result) {
    __android_log_print(ANDROID_LOG_ERROR, "sudoku", "%s", result.error().message.c_str());
    a.notice = result.error().message;
  }
}

void start_game(App& a) {
  auto puzzle = take_puzzle(*a.generator);
  if (!puzzle) {
    // The board shows progress until the generator notifies puzzle_fd().
    a.pending = true;
    a.game.reset();
    a.view = {};
    return;
  }
  auto game = create_game(*puzzle);
  if (!game) {
    // Wait for the generator's next puzzle.
    a.pending = true;
    a.notice = game.error().message;
    return;
  }
  a.game = std::move(*game);
  a.pending = false;
  a.result_shown = false;
  a.view = {};
  a.flash_start = -1;
  save(a);
}

void request_new_game(App& a) {
  auto current = board(a);
  if (current && current->status == Status::kPlaying && (current->can_undo || current->mistakes))
    a.view.dialog = Dialog::kConfirmNewGame;
  else start_game(a);
}

void enter(App& a, int digit, double now) {
  int cell = get_board(*a.game).selected;
  Move move = enter_digit(*a.game, digit);
  if (!move.changed) return;
  if (move.completed) {
    a.view.flash_units = move.completed;
    a.view.flash_cell = cell;
    a.view.flash_age = 0;
    a.flash_start = now;
  }
  save(a);
}

void activate(App& a, Hit hit, double now) {
  bool can_play = interactive(a);
  switch (hit.target) {
    case Target::kDigit:
      if (can_play) enter(a, hit.index, now);
      break;
    case Target::kUndo:
      if (can_play && undo(*a.game)) save(a);
      break;
    case Target::kErase:
      if (can_play && erase_cell(*a.game).changed) save(a);
      break;
    case Target::kNotes:
      if (can_play) {
        toggle_notes(*a.game);
        save(a);
      }
      break;
    case Target::kPause:
      if (playing(a) && a.view.dialog == Dialog::kNone) a.view.paused = !a.view.paused;
      break;
    case Target::kResume:
      a.view.paused = false;
      break;
    case Target::kNewGame:
      if (a.view.dialog == Dialog::kNone) request_new_game(a);
      break;
    case Target::kPrimary:
      if (a.view.dialog != Dialog::kNone) {
        a.view.dialog = Dialog::kNone;
        start_game(a);
      }
      break;
    case Target::kSecondary:
      a.view.dialog = Dialog::kNone;
      break;
    default:
      break;
  }
  a.view.pressed = {};
  a.dirty = true;
}

bool same(Hit a, Hit b) { return a.target == b.target && a.index == b.index; }

void move_selection(App& a, int dx, int dy) {
  if (!a.game || a.view.paused) return;
  int cell = get_board(*a.game).selected;
  if (cell < 0) cell = 40;
  else cell = (cell / 9 + dy + 9) % 9 * 9 + (cell % 9 + dx + 9) % 9;
  select_cell(*a.game, cell);
}
}

Result<Owner<App>> create_app(const char* directory, std::uint64_t seed) {
  Owner<App> app(new (std::nothrow) App);
  if (!app) return std::unexpected(Error{"Cannot allocate the application"});
  app->directory = directory;
  auto generator = create_generator(seed);
  if (!generator) return std::unexpected(generator.error());
  app->generator = std::move(*generator);
  auto saved = load_file(directory, kSaveName);
  if (!saved) app->notice = saved.error().message;
  if (saved && !saved->empty()) {
    auto game = decode_game(*saved);
    if (game) app->game = std::move(*game);
    else app->notice = "The saved game could not be restored";
  }
  if (!app->game) start_game(*app);
  return app;
}

void destroy(App* app) noexcept {
  app->renderer.reset();
  delete app;
}

Result<void> attach_window(App& a, ANativeWindow* window, int width, int height) {
  a.renderer.reset();
  a.laid_out = false;
  auto renderer = gpu::create_overlay_renderer(window, width, height);
  if (!renderer) return std::unexpected(renderer.error());
  a.renderer = std::move(*renderer);
  a.dirty = true;
  return {};
}

void detach_window(App& a) {
  a.renderer.reset();
  a.laid_out = false;
  a.view.pressed = {};
  a.clock = -1;
}

gpu::Renderer* get_renderer(App& a) { return a.renderer.get(); }

void set_content(App& a, gpu::Rect content) {
  if (content.x == a.content.x && content.y == a.content.y && content.w == a.content.w &&
      content.h == a.content.h)
    return;
  a.content = content;
  a.view.pressed = {};
  a.dirty = true;
}

void set_density(App& a, float pixels_per_dp) {
  if (!std::isfinite(pixels_per_dp) || pixels_per_dp <= 0 || pixels_per_dp == a.density) return;
  a.density = pixels_per_dp;
  a.view.pressed = {};
  a.dirty = true;
}

void set_touch_slop(App& a, float pixels) {
  if (std::isfinite(pixels) && pixels > 0) a.touch_slop = pixels;
}

void set_resumed(App& a, bool resumed, double now) {
  update(a, now);
  a.resumed = resumed;
  a.view.pressed = {};
  if (!resumed) save(a);
  update(a, now);
  a.dirty = true;
}

void touch(App& a, Touch action, float x, float y, double now) {
  update(a, now);
  if (!a.laid_out) return;
  auto current = board(a);
  Hit hit = hit_test(a.layout, current ? &*current : nullptr, a.view, x, y);
  float dx = x - a.down_x, dy = y - a.down_y;
  // Check the release too; Android need not send a move first.
  if (action != Touch::kDown && dx * dx + dy * dy > a.touch_slop * a.touch_slop) a.moved = true;
  switch (action) {
    case Touch::kDown:
      a.down_x = x;
      a.down_y = y;
      a.moved = false;
      a.view.pressed = {};
      // Cells respond immediately, like sudoku.com; buttons act on release.
      if (hit.target == Target::kCell) select_cell(*a.game, hit.index);
      else a.view.pressed = hit;
      break;
    case Touch::kMove:
      if (a.moved || !same(hit, a.view.pressed)) a.view.pressed = {};
      break;
    case Touch::kUp:
      if (a.view.pressed.target != Target::kNone && same(hit, a.view.pressed) && !a.moved)
        activate(a, hit, now);
      a.view.pressed = {};
      break;
    case Touch::kCancel:
      a.view.pressed = {};
      break;
  }
  a.dirty = true;
  update(a, now);
}

void press(App& a, Key key, int digit, double now) {
  update(a, now);
  a.view.pressed = {};
  if (a.view.dialog != Dialog::kNone) {
    if (key == Key::kEnter) activate(a, {Target::kPrimary}, now);
  } else {
    switch (key) {
      case Key::kDigit:
        activate(a, {Target::kDigit, digit}, now);
        break;
      case Key::kErase:
        activate(a, {Target::kErase}, now);
        break;
      case Key::kUndo:
        activate(a, {Target::kUndo}, now);
        break;
      case Key::kNotes:
        activate(a, {Target::kNotes}, now);
        break;
      case Key::kPause:
        activate(a, {Target::kPause}, now);
        break;
      case Key::kNewGame:
        activate(a, {Target::kNewGame}, now);
        break;
      case Key::kEnter:
        if (a.view.paused) activate(a, {Target::kResume}, now);
        break;
      case Key::kLeft:
        move_selection(a, -1, 0);
        break;
      case Key::kRight:
        move_selection(a, 1, 0);
        break;
      case Key::kUp:
        move_selection(a, 0, -1);
        break;
      case Key::kDown:
        move_selection(a, 0, 1);
        break;
    }
  }
  a.dirty = true;
  update(a, now);
}

bool dismiss(App& a, double now) {
  if (a.view.dialog == Dialog::kNone) return false;
  update(a, now);
  activate(a, {Target::kSecondary}, now);
  update(a, now);
  return true;
}

bool update(App& a, double now) {
  bool run = running(a);
  if (run && a.clock >= 0) add_time(*a.game, now - a.clock);
  a.clock = run ? now : -1;
  if (a.flash_start >= 0) {
    double age = now - a.flash_start;
    a.view.flash_age = age < kFlashSeconds ? float(std::max(0., age)) : -1;
    if (age >= kFlashSeconds) a.flash_start = -1;
    a.dirty = true;
  }
  if (a.game) {
    Board current = get_board(*a.game);
    // Show the result once the completion wave has finished.
    if (current.status != Status::kPlaying && !a.result_shown && a.flash_start < 0) {
      a.result_shown = true;
      a.view.paused = false;
      a.view.dialog = current.status == Status::kWon ? Dialog::kWon : Dialog::kLost;
      a.dirty = true;
    }
    if (int(current.elapsed) != a.shown_second) a.dirty = true;
  }
  return a.dirty && a.renderer;
}

Result<bool> draw(App& a, double now) {
  if (!a.renderer) return false;
  update(a, now);
  auto& r = *a.renderer;
  auto prepared = gpu::prepare_frame(r, false);
  if (!prepared || !*prepared) return prepared;
  auto stats = gpu::get_stats(r);
  Layout layout = layout_view(stats.width, stats.height, a.content, a.density);
  auto begun = gpu::render_overlay(r, kBackground);
  if (!begun || !*begun) return begun;
  auto current = board(a);
  draw_view(r, layout, current ? &*current : nullptr, a.view);
  auto presented = gpu::present(r);
  if (presented && *presented) {
    a.layout = layout;
    a.laid_out = true;
    a.dirty = a.flash_start >= 0;
    a.shown_second = current ? int(current->elapsed) : -1;
  }
  return presented;
}

Result<void> redraw(App& a, double now) {
  for (int attempt = 0; attempt < 4; ++attempt) {
    auto drawn = draw(a, now);
    if (!drawn) return std::unexpected(drawn.error());
    if (*drawn) return gpu::wait_frame(*a.renderer);
    if (!a.renderer) return {};
  }
  return {};
}

Result<bool> check_surface(App& a) {
  if (!a.renderer) return false;
  auto changed = gpu::surface_changed(*a.renderer);
  if (!changed) return std::unexpected(changed.error());
  if (*changed) {
    a.view.pressed = {};
    a.dirty = true;
  }
  return *changed;
}

int puzzle_fd(const App& a) { return generator_fd(*a.generator); }

void on_puzzle_ready(App& a, double now) {
  if (!a.pending) {
    (void)puzzle_ready(*a.generator);
    return;
  }
  update(a, now);
  start_game(a);
  a.dirty = true;
  update(a, now);
}

std::string take_notice(App& a) {
  std::string notice;
  notice.swap(a.notice);
  return notice;
}

AppState get_state(const App& a) {
  AppState state;
  state.has_game = a.game != nullptr;
  if (a.game) state.board = get_board(*a.game);
  state.paused = a.view.paused;
  state.pending = a.pending;
  state.dialog = a.view.dialog;
  return state;
}

const Layout& get_layout(const App& a) { return a.layout; }
}
