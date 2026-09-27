#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "common/owner.h"
#include "common/result.h"
#include "sudoku/puzzle.h"

namespace sudoku {
struct Game;
inline constexpr int kMaximumMistakes = 3;
enum class Status { kPlaying, kWon, kLost };

struct Cell {
  // Given or entered digit; zero when empty.
  std::uint8_t value = 0;
  bool given = false, wrong = false;
  // Bit n - 1 marks pencilled digit n.
  std::uint16_t notes = 0;
};

struct Board {
  std::array<Cell, kCells> cells;
  // Correctly placed copies of each digit, indexed by digit.
  std::array<std::uint8_t, 10> placed{};
  int selected = -1, mistakes = 0;
  bool notes_mode = false, can_undo = false;
  Status status = Status::kPlaying;
  double elapsed = 0;
};

struct Move {
  bool changed = false, mistake = false;
  // Units completed by this move: rows in bits 0-8, columns 9-17, boxes 18-26.
  std::uint32_t completed = 0;
};

common::Result<common::Owner<Game>> create_game(const Puzzle& puzzle);
void destroy(Game* game) noexcept;
Board get_board(const Game& game);
// Given cells can be selected to highlight their digit but not changed.
void select_cell(Game& game, int cell);
// Enter a digit or, in notes mode, toggle a pencil mark in the selected cell.
// Correct digits are final; wrong digits count as mistakes until erased.
Move enter_digit(Game& game, int digit);
Move erase_cell(Game& game);
// Mistakes stay counted after their moves are undone.
bool undo(Game& game);
void toggle_notes(Game& game);
void add_time(Game& game, double seconds);
std::vector<std::byte> encode_game(const Game& game);
common::Result<common::Owner<Game>> decode_game(std::span<const std::byte> bytes);
}
