#pragma once

#include <array>
#include <cstdint>

#include "common/gpu/math.h"
#include "sudoku/game.h"

namespace gpu {
struct Renderer;
}

namespace sudoku {
enum class Target {
  kNone,
  kCell,
  kDigit,
  kUndo,
  kErase,
  kNotes,
  kPause,
  kNewGame,
  kResume,
  kPrimary,
  kSecondary
};
enum class Dialog { kNone, kConfirmNewGame, kLost, kWon };

struct Hit {
  Target target = Target::kNone;
  int index = -1;
};

struct Layout {
  gpu::Rect screen, safe, title, new_game, info, pause, board, dialog, primary, secondary;
  // Undo, erase, and notes.
  std::array<gpu::Rect, 3> actions;
  std::array<gpu::Rect, 9> digits;
  // Pixels per dp after fitting, and whole-pixel cell and grid-line sizes.
  float scale = 1, cell = 0, thin = 1, thick = 2;
  bool landscape = false;
};

// Everything shown besides the game itself.
struct ViewState {
  Hit pressed;
  Dialog dialog = Dialog::kNone;
  bool paused = false;
  // Units completed by the last correct digit flash outward from its cell.
  std::uint32_t flash_units = 0;
  int flash_cell = -1;
  float flash_age = -1;
};

inline constexpr float kFlashSeconds = .75f;

// Lay out a width x height window. Controls stay inside the content rectangle,
// which excludes system bars and cutouts; an empty one selects the window.
Layout layout_view(int width, int height, gpu::Rect content, float density);
gpu::Rect cell_bounds(const Layout& layout, int cell);
Hit hit_test(const Layout& layout, const Board* board, const ViewState& state, float x, float y);
void draw_view(gpu::Renderer& renderer, const Layout& layout, const Board* board,
               const ViewState& state);
}
