#include "sudoku/game.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

namespace sudoku {
using common::Error;
using common::Owner;
using common::Result;

namespace {
constexpr std::uint32_t kMagic = 0x554b4453;
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kMaximumMoves = 2000;
constexpr std::uint16_t kAllNotes = 0x1ff;

// A cell's state before a move changed it.
struct Change {
  std::uint8_t cell, value;
  std::uint16_t notes;
};
}

struct Game {
  Puzzle puzzle;
  Grid values{};
  std::array<std::uint16_t, kCells> notes{};
  int selected = -1, mistakes = 0;
  bool notes_mode = false;
  double elapsed = 0;
  // Undo history: changes grouped into moves, oldest first.
  std::vector<Change> changes;
  std::vector<std::uint16_t> moves;
};

namespace {
int row(int cell) { return cell / 9; }

int column(int cell) { return cell % 9; }

int box(int cell) { return cell / 27 * 3 + cell % 9 / 3; }

bool related(int a, int b) {
  return a != b && (row(a) == row(b) || column(a) == column(b) || box(a) == box(b));
}

std::uint16_t note(int digit) { return std::uint16_t(1u << (digit - 1)); }

Status status(const Game& g) {
  if (g.mistakes >= kMaximumMistakes) return Status::kLost;
  return g.values == g.puzzle.solution ? Status::kWon : Status::kPlaying;
}

bool editable(const Game& g, int cell) {
  return cell >= 0 && cell < kCells && status(g) == Status::kPlaying && !g.puzzle.givens[cell] &&
         g.values[cell] != g.puzzle.solution[cell];
}

void record(Game& g, int cell) {
  g.changes.push_back({std::uint8_t(cell), g.values[cell], g.notes[cell]});
}

void finish_move(Game& g, std::size_t first) {
  if (g.changes.size() == first) return;
  g.moves.push_back(std::uint16_t(g.changes.size() - first));
  if (g.moves.size() > kMaximumMoves) {
    g.changes.erase(g.changes.begin(), g.changes.begin() + g.moves.front());
    g.moves.erase(g.moves.begin());
  }
}

bool unit_complete(const Game& g, int unit) {
  for (int i = 0; i < 9; ++i) {
    int cell = unit < 9    ? unit * 9 + i
               : unit < 18 ? i * 9 + unit - 9
                           : (unit - 18) / 3 * 27 + (unit - 18) % 3 * 3 + i / 3 * 9 + i % 3;
    if (g.values[cell] != g.puzzle.solution[cell]) return false;
  }
  return true;
}

bool valid_solution(const Grid& grid) {
  for (int unit = 0; unit < 27; ++unit) {
    unsigned seen = 0;
    for (int i = 0; i < 9; ++i) {
      int cell = unit < 9    ? unit * 9 + i
                 : unit < 18 ? i * 9 + unit - 9
                             : (unit - 18) / 3 * 27 + (unit - 18) % 3 * 3 + i / 3 * 9 + i % 3;
      if (grid[cell] < 1 || grid[cell] > 9) return false;
      seen |= 1u << grid[cell];
    }
    if (seen != 0x3fe) return false;
  }
  return true;
}

bool valid_puzzle(const Puzzle& puzzle) {
  if (!valid_solution(puzzle.solution)) return false;
  for (int cell = 0; cell < kCells; ++cell)
    if (puzzle.givens[cell] && puzzle.givens[cell] != puzzle.solution[cell]) return false;
  Grid solution;
  return count_solutions(puzzle.givens, 2, &solution) == 1 && solution == puzzle.solution;
}

struct Writer {
  std::vector<std::byte>& bytes;

  template <typename T>
  void put(const T& value) {
    auto* data = reinterpret_cast<const std::byte*>(&value);
    bytes.insert(bytes.end(), data, data + sizeof(T));
  }
};

struct Reader {
  std::span<const std::byte> bytes;
  std::size_t offset = 0;

  template <typename T>
  bool get(T& value) {
    if (bytes.size() - offset < sizeof(T)) return false;
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    offset += sizeof(T);
    return true;
  }
};
}

Result<Owner<Game>> create_game(const Puzzle& puzzle) {
  if (!valid_puzzle(puzzle)) return std::unexpected(Error{"Invalid puzzle"});
  Owner<Game> game(new (std::nothrow) Game);
  if (!game) return std::unexpected(Error{"Cannot allocate the game"});
  game->puzzle = puzzle;
  game->values = puzzle.givens;
  return game;
}

void destroy(Game* game) noexcept { delete game; }

Board get_board(const Game& g) {
  Board board;
  for (int cell = 0; cell < kCells; ++cell) {
    auto& c = board.cells[cell];
    c.value = g.values[cell];
    c.given = g.puzzle.givens[cell] != 0;
    c.wrong = c.value && c.value != g.puzzle.solution[cell];
    c.notes = c.value ? 0 : g.notes[cell];
    if (c.value && !c.wrong) ++board.placed[c.value];
  }
  board.selected = g.selected;
  board.mistakes = g.mistakes;
  board.notes_mode = g.notes_mode;
  board.status = status(g);
  board.can_undo = !g.moves.empty() && board.status == Status::kPlaying;
  board.elapsed = g.elapsed;
  return board;
}

void select_cell(Game& g, int cell) {
  if (cell >= -1 && cell < kCells) g.selected = cell;
}

Move enter_digit(Game& g, int digit) {
  Move move;
  int cell = g.selected;
  if (digit < 1 || digit > 9 || !editable(g, cell)) return move;
  std::size_t first = g.changes.size();
  if (g.notes_mode) {
    // Pencil marks belong to empty cells; erase a wrong digit first.
    if (g.values[cell]) return move;
    record(g, cell);
    g.notes[cell] ^= note(digit);
    move.changed = true;
  } else if (g.values[cell] != digit) {
    record(g, cell);
    g.values[cell] = digit;
    g.notes[cell] = 0;
    move.changed = true;
    if (digit == g.puzzle.solution[cell]) {
      for (int other = 0; other < kCells; ++other)
        if (related(cell, other) && !g.values[other] && (g.notes[other] & note(digit))) {
          record(g, other);
          g.notes[other] &= ~note(digit);
        }
      int units[3] = {row(cell), 9 + column(cell), 18 + box(cell)};
      for (int unit : units)
        if (unit_complete(g, unit)) move.completed |= 1u << unit;
    } else {
      move.mistake = true;
      ++g.mistakes;
    }
  }
  finish_move(g, first);
  return move;
}

Move erase_cell(Game& g) {
  Move move;
  int cell = g.selected;
  if (!editable(g, cell) || (!g.values[cell] && !g.notes[cell])) return move;
  std::size_t first = g.changes.size();
  record(g, cell);
  g.values[cell] = 0;
  g.notes[cell] = 0;
  finish_move(g, first);
  move.changed = true;
  return move;
}

bool undo(Game& g) {
  if (g.moves.empty() || status(g) != Status::kPlaying) return false;
  std::size_t count = g.moves.back();
  g.moves.pop_back();
  int primary = g.changes[g.changes.size() - count].cell;
  for (std::size_t i = 0; i < count; ++i) {
    Change change = g.changes.back();
    g.changes.pop_back();
    g.values[change.cell] = change.value;
    g.notes[change.cell] = change.notes;
  }
  g.selected = primary;
  return true;
}

void toggle_notes(Game& g) { g.notes_mode = !g.notes_mode; }

void add_time(Game& g, double seconds) {
  if (std::isfinite(seconds) && seconds > 0 && status(g) == Status::kPlaying)
    g.elapsed = std::min(g.elapsed + seconds, 359999.);
}

std::vector<std::byte> encode_game(const Game& g) {
  std::vector<std::byte> bytes;
  Writer out{bytes};
  out.put(kMagic);
  out.put(kVersion);
  out.put(g.puzzle.givens);
  out.put(g.puzzle.solution);
  out.put(g.values);
  out.put(g.notes);
  out.put(std::int8_t(g.selected));
  out.put(std::uint8_t(g.mistakes));
  out.put(std::uint8_t(g.notes_mode));
  out.put(std::uint8_t(0));
  out.put(g.elapsed);
  out.put(std::uint32_t(g.moves.size()));
  out.put(std::uint32_t(g.changes.size()));
  for (auto size : g.moves) out.put(size);
  for (auto change : g.changes) out.put(change);
  return bytes;
}

Result<Owner<Game>> decode_game(std::span<const std::byte> bytes) {
  auto invalid = [] { return std::unexpected(Error{"The saved game is invalid"}); };
  Reader in{bytes};
  std::uint32_t magic = 0, version = 0, move_count = 0, change_count = 0;
  Puzzle puzzle;
  Owner<Game> game(new (std::nothrow) Game);
  if (!game) return std::unexpected(Error{"Cannot allocate the game"});
  auto& g = *game;
  std::int8_t selected = 0;
  std::uint8_t mistakes = 0, notes_mode = 0, reserved = 0;
  if (!in.get(magic) || !in.get(version) || magic != kMagic || version != kVersion ||
      !in.get(puzzle.givens) || !in.get(puzzle.solution) || !in.get(g.values) || !in.get(g.notes) ||
      !in.get(selected) || !in.get(mistakes) || !in.get(notes_mode) || !in.get(reserved) ||
      !in.get(g.elapsed) || !in.get(move_count) || !in.get(change_count))
    return invalid();
  if (move_count > kMaximumMoves + 1 || change_count > (kMaximumMoves + 1) * kCells ||
      bytes.size() - in.offset !=
          move_count * sizeof(std::uint16_t) + change_count * sizeof(Change))
    return invalid();
  g.moves.resize(move_count);
  g.changes.resize(change_count);
  std::size_t total = 0;
  for (auto& size : g.moves) {
    if (!in.get(size) || !size) return invalid();
    total += size;
  }
  for (auto& change : g.changes)
    if (!in.get(change) || change.cell >= kCells || change.value > 9 ||
        (change.notes & ~kAllNotes) || puzzle.givens[change.cell])
      return invalid();
  if (total != change_count || selected < -1 || selected >= kCells || mistakes > kMaximumMistakes ||
      notes_mode > 1 || reserved || !std::isfinite(g.elapsed) || g.elapsed < 0 ||
      !valid_puzzle(puzzle))
    return invalid();
  for (int cell = 0; cell < kCells; ++cell)
    if (g.values[cell] > 9 || (g.notes[cell] & ~kAllNotes) ||
        (puzzle.givens[cell] && g.values[cell] != puzzle.givens[cell]))
      return invalid();
  g.puzzle = puzzle;
  g.selected = selected;
  g.mistakes = mistakes;
  g.notes_mode = notes_mode;
  return game;
}
}
