#include <cstdio>

#include "sudoku/puzzle.h"

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

bool valid_solution(const Grid& grid) {
  for (int i = 0; i < 9; ++i) {
    unsigned rows = 0, columns = 0, boxes = 0;
    for (int j = 0; j < 9; ++j) {
      rows |= 1u << grid[i * 9 + j];
      columns |= 1u << grid[j * 9 + i];
      boxes |= 1u << grid[(i / 3 * 3 + j / 3) * 9 + i % 3 * 3 + j % 3];
    }
    if (rows != 0x3fe || columns != 0x3fe || boxes != 0x3fe) return false;
  }
  return true;
}
}

int main() {
  // A singles-only newspaper puzzle and its solution.
  Grid easy =
      parse("53..7....6..195....98....6.8...6...34..8.3..17...2...6.6....28....419..5....8..79");
  Grid expected =
      parse("534678912672195348198342567859761423426853791713924856961537284287419635345286179");
  Grid solution{};
  check(count_solutions(easy, 2, &solution) == 1, "easy puzzle has one solution");
  check(solution == expected, "solver finds the known solution");
  Rating rating = rate(easy, &expected);
  check(rating.solved && rating.level == 1 && rating.consistent, "easy puzzle needs only singles");
  check(!is_extreme(rating), "easy puzzle is not Extreme");
  Grid empty{};
  check(count_solutions(empty, 2) == 2, "empty grid has many solutions");
  Grid conflict = easy;
  conflict[2] = 5;
  check(count_solutions(conflict, 2) == 0, "conflicting givens have no solution");
  // A notoriously hard puzzle exercises the advanced techniques without guessing.
  Grid hard =
      parse("8..........36......7..9.2...5...7.......457.....1...3...1....68..85...1..9....4..");
  check(count_solutions(hard, 2, &solution) == 1, "hard puzzle has one solution");
  Rating hard_rating = rate(hard, &solution);
  check(hard_rating.consistent, "advanced deductions agree with the solution");
  check(!hard_rating.solved || hard_rating.level >= 4, "hard puzzle needs advanced techniques");

  std::atomic<bool> cancelled{false};
  for (std::uint64_t seed = 1; seed <= 6; ++seed) {
    auto puzzle = generate_puzzle(seed * 7919, cancelled);
    if (!puzzle) {
      check(false, "generation succeeds");
      continue;
    }
    check(valid_solution(puzzle->solution), "generated solution is a valid grid");
    int clues = 0;
    for (int cell = 0; cell < kCells; ++cell) {
      if (!puzzle->givens[cell]) continue;
      ++clues;
      check(puzzle->givens[cell] == puzzle->solution[cell], "givens match the solution");
    }
    check(clues >= 17 && clues <= 32, "generated puzzle has a sparse set of clues");
    check(count_solutions(puzzle->givens, 2, &solution) == 1 && solution == puzzle->solution,
          "generated puzzle is uniquely solvable");
    Rating generated = rate(puzzle->givens, &puzzle->solution);
    check(generated.consistent, "generated puzzle's deductions agree with the solution");
    check(is_extreme(generated), "generated puzzle is Extreme");
    for (int cell = 0; cell < kCells; ++cell) {
      if (!puzzle->givens[cell]) continue;
      Grid reduced = puzzle->givens;
      reduced[cell] = 0;
      if (count_solutions(reduced, 2) != 2) {
        check(false, "every clue of a generated puzzle is necessary");
        break;
      }
    }
    std::printf("seed %llu: %d clues, %d chain steps\n", static_cast<unsigned long long>(seed),
                clues, generated.chain_steps);
  }
  cancelled = true;
  check(!generate_puzzle(1, cancelled), "cancelled generation stops");
  if (failures) return 1;
  std::printf("puzzle_test passed\n");
  return 0;
}
