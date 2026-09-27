#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include "common/result.h"

namespace sudoku {
inline constexpr int kCells = 81;
// Digits 1-9 in row-major order; zero marks an empty cell.
using Grid = std::array<std::uint8_t, kCells>;

struct Puzzle {
  Grid givens, solution;
};

// Technique levels: 1 singles, 2 locked candidates, 3 naked and hidden
// subsets, 4 fish and wings, 5 alternating inference chains.
struct Rating {
  bool solved = false;
  // False if a deduction contradicted the reference solution passed to rate().
  bool consistent = true;
  int level = 0, advanced_steps = 0, chain_steps = 0;
};

// Count solutions, stopping at limit. The first solution is copied to solution.
int count_solutions(const Grid& grid, int limit, Grid* solution = nullptr);
// Solve with human techniques, always applying the easiest one that progresses.
Rating rate(const Grid& givens, const Grid* reference = nullptr);
bool is_extreme(const Rating& rating);
// Generate a minimal, uniquely solvable Extreme puzzle. Fails only when cancelled.
common::Result<Puzzle> generate_puzzle(std::uint64_t seed, const std::atomic<bool>& cancelled);
}
