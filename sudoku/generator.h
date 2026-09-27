#pragma once

#include <cstdint>
#include <optional>

#include "common/owner.h"
#include "common/result.h"
#include "sudoku/puzzle.h"

namespace sudoku {
struct Generator;
// Generate Extreme puzzles on a background thread, keeping one ready.
common::Result<common::Owner<Generator>> create_generator(std::uint64_t seed);
// Cancels any generation in progress and joins the thread.
void destroy(Generator* generator) noexcept;
// Readable whenever a puzzle becomes ready.
int generator_fd(const Generator& generator);
// Clear the notification and report whether a puzzle is ready.
bool puzzle_ready(Generator& generator);
// Take the ready puzzle without blocking, and start generating the next.
std::optional<Puzzle> take_puzzle(Generator& generator);
}
