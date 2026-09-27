#if defined(NATIVE_BUTTONS_VULKAN_VALIDATION)
#include <dlfcn.h>
#endif
#include <poll.h>
#include <stdlib.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "common/gpu/renderer.h"
#include "sudoku/app.h"
#include "sudoku/puzzle.h"

#if defined(NATIVE_BUTTONS_VULKAN_VALIDATION)
namespace gpu {
unsigned validation_error_count();
}
#endif

namespace {
using namespace sudoku;
using gpu::Rect;

#if defined(NATIVE_BUTTONS_VULKAN_VALIDATION)
// The CLI has no Activity to configure Android's layer search path. This
// test-only bootstrap uses the platform GraphicsEnv, as //native_buttons:gpu_probe does.
bool prepare_validation() {
  const char* path = std::getenv("NATIVE_BUTTONS_VULKAN_LAYER_PATH");
  using GetEnvironment = void* (*)();
  using SetPaths = void (*)(void*, void*, const std::string&);
  auto get_environment = reinterpret_cast<GetEnvironment>(
      dlsym(RTLD_DEFAULT, "_ZN7android11GraphicsEnv11getInstanceEv"));
  auto set_paths = reinterpret_cast<SetPaths>(
      dlsym(RTLD_DEFAULT,
            "_ZN7android11GraphicsEnv13setLayerPathsEPNS_21NativeLoaderNamespaceERKNSt3__"
            "112basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEE"));
  if (!path || !get_environment || !set_paths) return false;
  set_paths(get_environment(), nullptr, std::string(path));
  return true;
}
#endif

int failures = 0;

void check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAILED: %s\n", message);
    ++failures;
  }
}

// Adjacent rectangles may share an edge up to rounding.
bool overlaps(Rect a, Rect b) {
  constexpr float kEpsilon = .01f;
  return a.x + kEpsilon < b.x + b.w && b.x + kEpsilon < a.x + a.w && a.y + kEpsilon < b.y + b.h &&
         b.y + kEpsilon < a.y + a.h;
}

bool within(Rect inner, Rect outer) {
  return inner.x >= outer.x - .5f && inner.y >= outer.y - .5f &&
         inner.x + inner.w <= outer.x + outer.w + .5f &&
         inner.y + inner.h <= outer.y + outer.h + .5f;
}

float center_x(Rect r) { return r.x + r.w * .5f; }

float center_y(Rect r) { return r.y + r.h * .5f; }

void check_layout(const char* name, int width, int height, Rect content, float density,
                  bool landscape, float minimum_key_dp) {
  Layout l = layout_view(width, height, content, density);
  std::printf("%s: %s, board %.0f px, cell %.0f px, keys %.0fx%.0f dp\n", name,
              l.landscape ? "landscape" : "portrait", l.board.w, l.cell, l.digits[0].w / density,
              l.digits[0].h / density);
  check(l.landscape == landscape, name);
  std::vector<Rect> controls = {l.new_game, l.pause, l.board};
  controls.insert(controls.end(), l.actions.begin(), l.actions.end());
  controls.insert(controls.end(), l.digits.begin(), l.digits.end());
  for (std::size_t i = 0; i < controls.size(); ++i) {
    check(within(controls[i], l.safe) && controls[i].w > 0 && controls[i].h > 0,
          "controls stay inside the safe area");
    for (std::size_t j = i + 1; j < controls.size(); ++j)
      if (overlaps(controls[i], controls[j])) {
        std::fprintf(stderr, "%s: controls %zu and %zu overlap\n", name, i, j);
        check(false, "controls do not overlap");
      }
  }
  check(l.board.w == l.board.h && l.board.w == 9 * l.cell + 6 * l.thin + 4 * l.thick,
        "the board is a whole-pixel square grid");
  check(l.board.x == std::floor(l.board.x) && l.board.y == std::floor(l.board.y),
        "the board is pixel aligned");
  check(l.digits[0].w >= minimum_key_dp * density, "digit keys stay large enough to tap");
  check(within(l.dialog, l.safe), "dialogs fit the safe area");
  Board board;
  ViewState view;
  for (int cell = 0; cell < kCells; ++cell) {
    Rect bounds = cell_bounds(l, cell);
    Hit hit = hit_test(l, &board, view, center_x(bounds), center_y(bounds));
    check(within(bounds, l.board) && hit.target == Target::kCell && hit.index == cell,
          "cells are hit where they are drawn");
  }
  for (int digit = 1; digit <= 9; ++digit) {
    Rect key = l.digits[digit - 1];
    Hit hit = hit_test(l, &board, view, center_x(key), center_y(key));
    check(hit.target == Target::kDigit && hit.index == digit, "digit keys are hit");
  }
  Target actions[] = {Target::kUndo, Target::kErase, Target::kNotes};
  for (int i = 0; i < 3; ++i)
    check(hit_test(l, &board, view, center_x(l.actions[i]), center_y(l.actions[i])).target ==
              actions[i],
          "actions are hit");
  view.dialog = Dialog::kConfirmNewGame;
  check(hit_test(l, &board, view, center_x(l.primary), center_y(l.primary)).target ==
                Target::kPrimary &&
            hit_test(l, &board, view, center_x(l.board), center_y(l.board)).target == Target::kNone,
        "dialogs are modal");
}

void tap(App& app, Rect bounds, double now) {
  touch(app, Touch::kDown, center_x(bounds), center_y(bounds), now);
  touch(app, Touch::kUp, center_x(bounds), center_y(bounds), now);
}

bool wait_for_game(App& app) {
  for (int attempt = 0; attempt < 100 && !get_state(app).has_game; ++attempt) {
    pollfd descriptor{puzzle_fd(app), POLLIN, 0};
    poll(&descriptor, 1, 100);
    on_puzzle_ready(app, 0);
  }
  return get_state(app).has_game;
}

int find_empty(const Board& board) {
  for (int cell = 0; cell < kCells; ++cell)
    if (!board.cells[cell].value) return cell;
  return -1;
}

std::string output_directory;

void capture(App& app, const char* name) {
  if (output_directory.empty()) return;
  std::string path = output_directory + "/" + name + ".ppm";
  if (auto result = gpu::capture_frame(*get_renderer(app), path.c_str()); !result)
    check(false, "screenshot is written");
}

bool render(App& app, double now) {
  auto result = redraw(app, now);
  if (!result) std::fprintf(stderr, "%s\n", result.error().message.c_str());
  return bool(result);
}
}

int main(int argc, char** argv) {
#if defined(NATIVE_BUTTONS_VULKAN_VALIDATION)
  if (!prepare_validation()) {
    std::fprintf(stderr, "Use //tools:vulkan_validation_runner\n");
    return 1;
  }
#endif
  if (argc > 1) output_directory = argv[1];
  // Pixel 8 Pro portrait and landscape, a tablet, split screen, and a small window.
  check_layout("phone", 1344, 2992, {0, 145, 1344, 2992 - 145 - 72}, 3, false, 36);
  check_layout("phone landscape", 2992, 1344, {145, 72, 2992 - 145 - 72, 1344 - 72}, 3, true, 36);
  check_layout("tablet", 2560, 1600, {0, 72, 2560, 1600 - 72 - 96}, 2, true, 48);
  check_layout("tablet portrait", 1600, 2560, {0, 72, 1600, 2560 - 72 - 96}, 2, false, 48);
  check_layout("split screen", 1344, 1380, {0, 0, 1344, 1380}, 3, false, 36);
  check_layout("small window", 520, 420, {}, 1.5f, false, 10);

  const char* root = getenv("TEST_TMPDIR");
  if (!root) root = getenv("TMPDIR");
  std::string directory = std::string(root ? root : "/tmp") + "/sudoku.XXXXXX";
  if (!mkdtemp(directory.data())) return 1;
  auto created = create_app(directory.c_str(), 42);
  if (!created) return 1;
  App& app = **created;
  check(wait_for_game(app), "a puzzle is generated");
  if (auto result = attach_window(app, nullptr, 1344, 2992); !result) {
    std::fprintf(stderr, "%s\n", result.error().message.c_str());
    return 1;
  }
  set_density(app, 3);
  set_touch_slop(app, 24);
  set_content(app, {0, 145, 1344, 2992 - 145 - 72});
  set_resumed(app, true, 0);
  check(render(app, 0), "the first frame renders");
  std::vector<unsigned char> pixels(std::size_t(1344) * 2992 * 4);
  check(bool(gpu::read_pixels(*get_renderer(app), pixels)), "pixels read back");
  check(pixels[0] == 255 && pixels[1] == 255 && pixels[2] == 255, "the background is white");
  Layout layout = get_layout(app);
  std::size_t line = (std::size_t(layout.board.y + layout.board.h * .5f) * 1344 +
                      std::size_t(layout.board.x + 1)) *
                     4;
  check(pixels[line] < 80 && pixels[line + 2] < 120, "the board has a dark border");

  Board board = get_state(app).board;
  Grid givens{}, answer{};
  for (int c = 0; c < kCells; ++c) givens[c] = board.cells[c].given ? board.cells[c].value : 0;
  check(count_solutions(givens, 1, &answer) == 1, "the board's givens are solvable");
  int cell = find_empty(board);
  check(cell >= 0, "the puzzle has empty cells");
  tap(app, cell_bounds(layout, cell), 1);
  check(get_state(app).board.selected == cell, "tapping a cell selects it");
  tap(app, layout.digits[answer[cell] % 9], 2);
  auto state = get_state(app);
  check(state.board.cells[cell].wrong && state.board.mistakes == 1, "a wrong digit is a mistake");
  check(render(app, 2), "a mistake renders");
  tap(app, layout.actions[1], 2);
  check(!get_state(app).board.cells[cell].value, "Erase clears the wrong digit");
  tap(app, layout.digits[answer[cell] - 1], 2);
  state = get_state(app);
  check(state.board.cells[cell].value == answer[cell] && !state.board.cells[cell].wrong,
        "a correct digit is placed");
  check(state.board.status == Status::kPlaying, "the game continues");
  int solution = answer[cell];
  int mistakes = state.board.mistakes;

  int second = find_empty(state.board);
  tap(app, cell_bounds(layout, second), 3);
  tap(app, layout.actions[2], 3);
  check(get_state(app).board.notes_mode, "the notes button enables notes");
  tap(app, layout.digits[0], 3);
  tap(app, layout.digits[4], 3);
  state = get_state(app);
  check(state.board.cells[second].notes == 0x11 && !state.board.cells[second].value,
        "digits become pencil marks in notes mode");
  tap(app, layout.actions[2], 3);
  check(!get_state(app).board.notes_mode, "the notes button disables notes");
  check(render(app, 3), "notes render");
  capture(app, "portrait");

  double elapsed = get_state(app).board.elapsed;
  update(app, 8);
  check(std::fabs(get_state(app).board.elapsed - elapsed - 5) < .01, "the clock runs");
  tap(app, layout.pause, 8);
  check(get_state(app).paused, "pause pauses");
  update(app, 20);
  check(std::fabs(get_state(app).board.elapsed - elapsed - 5) < .01, "the clock stops");
  check(render(app, 20), "the paused board renders");
  capture(app, "paused");
  tap(app, layout.board, 21);
  check(!get_state(app).paused, "tapping the board resumes");
  set_resumed(app, false, 22);
  update(app, 40);
  set_resumed(app, true, 40);
  check(std::fabs(get_state(app).board.elapsed - elapsed - 6) < .01,
        "the clock stops while the Activity is paused");

  touch(app, Touch::kDown, center_x(layout.new_game), center_y(layout.new_game), 41);
  touch(app, Touch::kUp, center_x(layout.new_game) + 30, center_y(layout.new_game), 41);
  check(get_state(app).dialog == Dialog::kNone, "a release beyond the touch slop is ignored");
  tap(app, layout.new_game, 41);
  check(get_state(app).dialog == Dialog::kConfirmNewGame, "New Game asks for confirmation");
  check(render(app, 41), "the dialog renders");
  capture(app, "confirm");
  tap(app, layout.secondary, 41);
  check(get_state(app).dialog == Dialog::kNone, "Cancel keeps the game");

  // The game is saved on every move and restored on recreation.
  set_resumed(app, false, 42);
  Board saved = get_state(app).board;
  created->reset();
  auto reopened = create_app(directory.c_str(), 43);
  if (!reopened) return 1;
  App& again = **reopened;
  state = get_state(again);
  bool restored = state.has_game && state.board.mistakes == mistakes &&
                  state.board.cells[cell].value == solution &&
                  state.board.cells[second].notes == saved.cells[second].notes &&
                  std::fabs(state.board.elapsed - saved.elapsed) < .01;
  check(restored, "the saved game is restored");

  // Landscape uses the side panel; three mistakes end the game.
  if (auto result = attach_window(again, nullptr, 2992, 1344); !result) return 1;
  set_density(again, 3);
  set_content(again, {145, 72, 2992 - 145 - 72, 1344 - 72});
  set_resumed(again, true, 50);
  check(render(again, 50), "the landscape frame renders");
  layout = get_layout(again);
  check(layout.landscape, "wide windows use the landscape layout");
  press(again, Key::kRight, 0, 50);
  tap(again, cell_bounds(layout, second), 51);
  check(get_state(again).board.selected == second, "landscape cells are hit");
  capture(again, "landscape");
  for (int attempt = 0; attempt < 5 && get_state(again).board.status == Status::kPlaying;
       ++attempt) {
    int target = find_empty(get_state(again).board);
    tap(again, cell_bounds(layout, target), 52);
    tap(again, layout.digits[answer[target] % 9], 52);
  }
  update(again, 53);
  check(get_state(again).board.status == Status::kLost, "three mistakes lose");
  check(get_state(again).dialog == Dialog::kLost, "losing shows Game Over");
  check(render(again, 53), "Game Over renders");
  capture(again, "lost");
  tap(again, layout.primary, 54);
  if (!get_state(again).has_game) wait_for_game(again);
  state = get_state(again);
  check(state.has_game && state.board.status == Status::kPlaying && !state.board.mistakes &&
            state.dialog == Dialog::kNone,
        "New Game starts a fresh puzzle");
  // A tablet-sized landscape window uses the three-by-three keypad.
  if (auto result = attach_window(again, nullptr, 2560, 1600); !result) return 1;
  set_density(again, 2);
  set_content(again, {0, 72, 2560, 1600 - 72 - 96});
  check(render(again, 55), "the tablet frame renders");
  capture(again, "tablet");
  detach_window(again);
  reopened->reset();
#if defined(NATIVE_BUTTONS_VULKAN_VALIDATION)
  check(!gpu::validation_error_count(), "Vulkan validation reports no errors");
#endif
  if (failures) return 1;
  std::printf("app_test passed\n");
  return 0;
}
