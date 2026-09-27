#include <cstdio>
#include <cstring>

#include "sudoku/game.h"

namespace {
using namespace sudoku;

int failures = 0;

void check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAILED: %s\n", message);
    ++failures;
  }
}

Grid parse(const char* text) {
  Grid grid{};
  for (int i = 0; i < kCells; ++i) grid[i] = text[i] == '.' ? 0 : text[i] - '0';
  return grid;
}

Puzzle sample() {
  return {
      parse("53..7....6..195....98....6.8...6...34..8.3..17...2...6.6....28....419..5....8..79"),
      parse("534678912672195348198342567859761423426853791713924856961537284287419635345286179")};
}

int wrong_digit(const Puzzle& puzzle, int cell) { return puzzle.solution[cell] % 9 + 1; }

void solve_all(Game& game, const Puzzle& puzzle) {
  for (int cell = 0; cell < kCells; ++cell)
    if (!puzzle.givens[cell]) {
      select_cell(game, cell);
      enter_digit(game, puzzle.solution[cell]);
    }
}

bool same(const Board& a, const Board& b) {
  for (int cell = 0; cell < kCells; ++cell)
    if (a.cells[cell].value != b.cells[cell].value || a.cells[cell].notes != b.cells[cell].notes ||
        a.cells[cell].given != b.cells[cell].given)
      return false;
  return a.selected == b.selected && a.mistakes == b.mistakes && a.notes_mode == b.notes_mode &&
         a.can_undo == b.can_undo && a.elapsed == b.elapsed && a.status == b.status;
}
}

int main() {
  Puzzle puzzle = sample();
  Puzzle invalid = puzzle;
  invalid.givens[2] = 5;
  check(!create_game(invalid), "an inconsistent puzzle is rejected");
  auto created = create_game(puzzle);
  if (!created) return 1;
  Game& game = **created;

  select_cell(game, 0);
  check(!enter_digit(game, 1).changed, "given cells cannot change");
  check(get_board(game).selected == 0, "given cells can be selected");

  // Empty cell 2 has empty peers 3 (row), 11 and 29 (column), and 10 (box).
  int cell = 2;
  select_cell(game, cell);
  Move move = enter_digit(game, wrong_digit(puzzle, cell));
  Board board = get_board(game);
  check(move.changed && move.mistake && board.mistakes == 1, "a wrong digit counts a mistake");
  check(board.cells[cell].wrong && board.cells[cell].value, "the wrong digit stays visible");
  check(!enter_digit(game, wrong_digit(puzzle, cell)).changed, "repeating it is ignored");
  check(get_board(game).mistakes == 1, "repeating a wrong digit is not another mistake");
  check(erase_cell(game).changed && !get_board(game).cells[cell].value, "erase clears it");
  check(undo(game) && get_board(game).cells[cell].wrong, "undo restores the erased digit");
  check(undo(game) && !get_board(game).cells[cell].value, "undo removes the wrong digit");
  check(get_board(game).mistakes == 1, "undo keeps mistakes counted");

  // Pencil marks, and their removal from peers by a correct digit.
  toggle_notes(game);
  int digit = puzzle.solution[cell];
  for (int peer : {3, 11, 29, 10, 40}) {
    select_cell(game, peer);
    enter_digit(game, digit);
  }
  select_cell(game, cell);
  enter_digit(game, digit);
  enter_digit(game, digit % 9 + 1);
  board = get_board(game);
  check(board.notes_mode && board.cells[cell].notes == ((1u << (digit - 1)) | (1u << (digit % 9))),
        "notes mode toggles pencil marks");
  check(!board.cells[cell].value && board.mistakes == 1, "notes are not entries or mistakes");
  toggle_notes(game);
  Board before = get_board(game);
  move = enter_digit(game, digit);
  board = get_board(game);
  check(move.changed && !move.mistake && board.cells[cell].value == digit, "correct digit placed");
  check(!board.cells[cell].notes, "a placed digit hides its cell's notes");
  for (int peer : {3, 11, 29, 10})
    check(!(board.cells[peer].notes & (1u << (digit - 1))), "peers lose the placed digit's note");
  check(board.cells[40].notes == 1u << (digit - 1), "unrelated notes remain");
  check(!erase_cell(game).changed && !enter_digit(game, digit % 9 + 1).changed,
        "correct digits are final");
  check(undo(game), "placing can be undone");
  board = get_board(game);
  check(same(board, before), "undo restores the cell and its peers' notes");
  check(enter_digit(game, digit).changed, "digit placed again");

  // Round trip through the saved format.
  add_time(game, 12.5);
  auto bytes = encode_game(game);
  auto decoded = decode_game(bytes);
  check(decoded && same(get_board(**decoded), get_board(game)), "saved games round trip");
  check(decoded && undo(**decoded) && undo(game) && same(get_board(**decoded), get_board(game)),
        "undo history survives saving");
  enter_digit(game, digit);
  auto corrupt = [&](std::size_t offset) {
    auto copy = encode_game(game);
    copy[offset] = std::byte(0xff);
    return !decode_game(copy);
  };
  check(corrupt(0), "a bad signature is rejected");
  check(corrupt(8 + 3), "an inconsistent given is rejected");
  check(corrupt(8 + 81 + 5), "an invalid solution is rejected");
  auto truncated = encode_game(game);
  truncated.pop_back();
  check(!decode_game(truncated), "a truncated game is rejected");
  check(!decode_game({}), "an empty file is rejected");

  // Completing the puzzle wins and reports completed units.
  auto second = create_game(puzzle);
  if (!second) return 1;
  solve_all(**second, puzzle);
  board = get_board(**second);
  check(board.status == Status::kWon && !board.can_undo, "solving every cell wins");
  for (int value = 1; value <= 9; ++value)
    check(board.placed[value] == 9, "every digit is fully placed");
  auto third = create_game(puzzle);
  if (!third) return 1;
  std::uint32_t completed = 0;
  for (int c = 0; c < kCells; ++c)
    if (!puzzle.givens[c]) {
      select_cell(**third, c);
      completed |= enter_digit(**third, puzzle.solution[c]).completed;
    }
  check(completed == (1u << 27) - 1, "each row, column, and box completes once");
  add_time(**second, 5);
  check(get_board(**second).elapsed == 0, "the clock stops after winning");

  // Three mistakes lose the game and freeze it.
  auto fourth = create_game(puzzle);
  if (!fourth) return 1;
  Game& lost = **fourth;
  for (int empty : {2, 3, 5}) {
    select_cell(lost, empty);
    enter_digit(lost, wrong_digit(puzzle, empty));
  }
  board = get_board(lost);
  check(board.status == Status::kLost && board.mistakes == 3, "three mistakes lose");
  select_cell(lost, 6);
  check(!enter_digit(lost, puzzle.solution[6]).changed && !undo(lost), "a lost game is frozen");
  auto restored = decode_game(encode_game(lost));
  check(restored && get_board(**restored).status == Status::kLost, "a lost game stays lost");
  if (failures) return 1;
  std::printf("game_test passed\n");
  return 0;
}
