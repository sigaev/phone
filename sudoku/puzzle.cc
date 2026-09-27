#include "sudoku/puzzle.h"

#include <algorithm>
#include <bit>
#include <utility>

namespace sudoku {
namespace {
constexpr std::uint16_t kAll = 0x1ff;
constexpr int kNodes = kCells * 9;
// Longer chains are impractical to find by hand.
constexpr int kMaximumChainLinks = 12;

constexpr std::uint16_t bit(int digit) { return std::uint16_t(1u << (digit - 1)); }

constexpr int row(int cell) { return cell / 9; }

constexpr int column(int cell) { return cell % 9; }

constexpr int box(int cell) { return cell / 27 * 3 + cell % 9 / 3; }

constexpr bool sees(int a, int b) {
  return a != b && (row(a) == row(b) || column(a) == column(b) || box(a) == box(b));
}

int lowest_digit(std::uint16_t mask) { return std::countr_zero(mask) + 1; }

struct Tables {
  // Rows are units 0-8, columns 9-17, and boxes 18-26.
  std::uint8_t units[27][9];
  std::uint8_t cell_units[kCells][3];
  std::uint8_t peers[kCells][20];
};

constexpr Tables make_tables() {
  Tables tables{};
  int filled[27]{};
  for (int cell = 0; cell < kCells; ++cell) {
    int ids[3] = {row(cell), 9 + column(cell), 18 + box(cell)};
    for (int k = 0; k < 3; ++k) {
      tables.cell_units[cell][k] = ids[k];
      tables.units[ids[k]][filled[ids[k]]++] = cell;
    }
    int count = 0;
    for (int other = 0; other < kCells; ++other)
      if (sees(cell, other)) tables.peers[cell][count++] = other;
  }
  return tables;
}

constexpr Tables kTables = make_tables();

struct Random {
  std::uint64_t state;

  std::uint64_t next() {
    std::uint64_t z = state += 0x9e3779b97f4a7c15ull;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
  }

  int below(int limit) { return int(next() % std::uint64_t(limit)); }
};

// Backtracking search over row, column, and box masks, choosing the most constrained cell.
struct Search {
  std::uint16_t rows[9]{}, columns[9]{}, boxes[9]{};
  Grid cells{};
  int count = 0, limit = 1;
  Grid* solution = nullptr;
  Random* random = nullptr;
};

bool load(Search& s, const Grid& grid) {
  for (int cell = 0; cell < kCells; ++cell) {
    int digit = grid[cell];
    if (!digit) continue;
    if (digit > 9) return false;
    std::uint16_t mask = bit(digit);
    if ((s.rows[row(cell)] | s.columns[column(cell)] | s.boxes[box(cell)]) & mask) return false;
    s.rows[row(cell)] |= mask;
    s.columns[column(cell)] |= mask;
    s.boxes[box(cell)] |= mask;
    s.cells[cell] = digit;
  }
  return true;
}

// Returns true when the search should stop.
bool search(Search& s) {
  int best = -1, best_count = 10;
  std::uint16_t best_mask = 0;
  for (int cell = 0; cell < kCells; ++cell) {
    if (s.cells[cell]) continue;
    std::uint16_t mask = kAll & ~(s.rows[row(cell)] | s.columns[column(cell)] | s.boxes[box(cell)]);
    int count = std::popcount(mask);
    if (count < best_count) {
      best = cell;
      best_count = count;
      best_mask = mask;
      if (count <= 1) break;
    }
  }
  if (best < 0) {
    if (s.solution && s.count == 0) *s.solution = s.cells;
    return ++s.count >= s.limit;
  }
  int digits[9], count = 0;
  for (int digit = 1; digit <= 9; ++digit)
    if (best_mask & bit(digit)) digits[count++] = digit;
  if (s.random)
    for (int i = count - 1; i > 0; --i) std::swap(digits[i], digits[s.random->below(i + 1)]);
  int r = row(best), c = column(best), b = box(best);
  for (int i = 0; i < count; ++i) {
    std::uint16_t mask = bit(digits[i]);
    s.rows[r] |= mask;
    s.columns[c] |= mask;
    s.boxes[b] |= mask;
    s.cells[best] = digits[i];
    bool stop = search(s);
    s.rows[r] &= ~mask;
    s.columns[c] &= ~mask;
    s.boxes[b] &= ~mask;
    s.cells[best] = 0;
    if (stop) return true;
  }
  return false;
}

Grid random_solution(Random& random) {
  Search s;
  Grid solution{};
  s.solution = &solution;
  s.random = &random;
  search(s);
  return solution;
}

// Remove clues in random order while the solution stays unique. The result is
// minimal: removing a clue from a smaller puzzle cannot restore uniqueness.
Grid minimize(const Grid& solution, Random& random) {
  Grid puzzle = solution;
  int order[kCells];
  for (int i = 0; i < kCells; ++i) order[i] = i;
  for (int i = kCells - 1; i > 0; --i) std::swap(order[i], order[random.below(i + 1)]);
  for (int cell : order) {
    std::uint8_t digit = puzzle[cell];
    puzzle[cell] = 0;
    if (count_solutions(puzzle, 2) != 1) puzzle[cell] = digit;
  }
  return puzzle;
}

// Pencil-mark state for the human-style solver.
struct Logic {
  Grid values{};
  std::uint16_t candidates[kCells]{};
  const Grid* reference = nullptr;
  bool consistent = true;
};

void place(Logic& s, int cell, int digit) {
  if (s.reference && (*s.reference)[cell] != digit) s.consistent = false;
  s.values[cell] = digit;
  s.candidates[cell] = 0;
  for (int peer : kTables.peers[cell]) s.candidates[peer] &= ~bit(digit);
}

bool eliminate(Logic& s, int cell, std::uint16_t mask) {
  mask &= s.candidates[cell];
  if (!mask) return false;
  if (s.reference && (*s.reference)[cell] && (mask & bit((*s.reference)[cell])))
    s.consistent = false;
  s.candidates[cell] &= ~mask;
  return true;
}

bool naked_single(Logic& s) {
  for (int cell = 0; cell < kCells; ++cell)
    if (std::popcount(s.candidates[cell]) == 1) {
      place(s, cell, lowest_digit(s.candidates[cell]));
      return true;
    }
  return false;
}

bool hidden_single(Logic& s) {
  for (const auto& unit : kTables.units)
    for (int digit = 1; digit <= 9; ++digit) {
      int count = 0, found = -1;
      for (int cell : unit)
        if (s.candidates[cell] & bit(digit)) {
          ++count;
          found = cell;
        }
      if (count == 1) {
        place(s, found, digit);
        return true;
      }
    }
  return false;
}

// Pointing: a box's candidates on one line clear that line outside the box.
// Claiming: a line's candidates in one box clear the rest of that box.
bool locked_candidates(Logic& s) {
  for (int unit = 0; unit < 27; ++unit)
    for (int digit = 1; digit <= 9; ++digit) {
      std::uint16_t mask = bit(digit);
      int rows = 0, columns = 0, boxes = 0;
      for (int cell : kTables.units[unit])
        if (s.candidates[cell] & mask) {
          rows |= 1 << row(cell);
          columns |= 1 << column(cell);
          boxes |= 1 << box(cell);
        }
      if (!rows) continue;
      int target = -1;
      if (unit >= 18 && std::popcount(unsigned(rows)) == 1)
        target = std::countr_zero(unsigned(rows));
      else if (unit >= 18 && std::popcount(unsigned(columns)) == 1)
        target = 9 + std::countr_zero(unsigned(columns));
      else if (unit < 18 && std::popcount(unsigned(boxes)) == 1)
        target = 18 + std::countr_zero(unsigned(boxes));
      if (target < 0) continue;
      bool progress = false;
      for (int cell : kTables.units[target]) {
        bool inside = unit < 9    ? row(cell) == unit
                      : unit < 18 ? column(cell) == unit - 9
                                  : box(cell) == unit - 18;
        if (!inside) progress |= eliminate(s, cell, mask);
      }
      if (progress) return true;
    }
  return false;
}

bool subsets(Logic& s) {
  for (int size = 2; size <= 4; ++size)
    for (const auto& unit : kTables.units) {
      // Naked: size cells whose candidates contain only size digits.
      for (unsigned combination = 0; combination < 512; ++combination) {
        if (std::popcount(combination) != size) continue;
        std::uint16_t digits = 0;
        bool valid = true;
        for (int i = 0; i < 9 && valid; ++i)
          if (combination & (1u << i)) {
            valid = s.candidates[unit[i]] != 0;
            digits |= s.candidates[unit[i]];
          }
        if (!valid || std::popcount(digits) != size) continue;
        bool progress = false;
        for (int i = 0; i < 9; ++i)
          if (!(combination & (1u << i))) progress |= eliminate(s, unit[i], digits);
        if (progress) return true;
      }
      // Hidden: size digits confined to size cells exclude other digits there.
      unsigned positions[10]{};
      for (int i = 0; i < 9; ++i)
        for (int digit = 1; digit <= 9; ++digit)
          if (s.candidates[unit[i]] & bit(digit)) positions[digit] |= 1u << i;
      for (unsigned combination = 0; combination < 512; ++combination) {
        if (std::popcount(combination) != size) continue;
        unsigned cells = 0;
        bool valid = true;
        for (int digit = 1; digit <= 9 && valid; ++digit)
          if (combination & bit(digit)) {
            valid = positions[digit] != 0;
            cells |= positions[digit];
          }
        if (!valid || std::popcount(cells) != size) continue;
        bool progress = false;
        for (int i = 0; i < 9; ++i)
          if (cells & (1u << i)) progress |= eliminate(s, unit[i], kAll & ~combination);
        if (progress) return true;
      }
    }
  return false;
}

// X-wing, swordfish, and jellyfish in rows and columns.
bool fish(Logic& s) {
  for (int size = 2; size <= 4; ++size)
    for (int digit = 1; digit <= 9; ++digit)
      for (bool by_rows : {true, false}) {
        unsigned lines[9]{};
        for (int line = 0; line < 9; ++line)
          for (int position = 0; position < 9; ++position) {
            int cell = by_rows ? line * 9 + position : position * 9 + line;
            if (s.candidates[cell] & bit(digit)) lines[line] |= 1u << position;
          }
        for (unsigned combination = 0; combination < 512; ++combination) {
          if (std::popcount(combination) != size) continue;
          unsigned cover = 0;
          bool valid = true;
          for (int line = 0; line < 9 && valid; ++line)
            if (combination & (1u << line)) {
              valid = lines[line] != 0;
              cover |= lines[line];
            }
          if (!valid || std::popcount(cover) != size) continue;
          bool progress = false;
          for (int line = 0; line < 9; ++line)
            if (!(combination & (1u << line)))
              for (int position = 0; position < 9; ++position)
                if (cover & (1u << position))
                  progress |=
                      eliminate(s, by_rows ? line * 9 + position : position * 9 + line, bit(digit));
          if (progress) return true;
        }
      }
  return false;
}

bool xy_wing(Logic& s) {
  for (int pivot = 0; pivot < kCells; ++pivot) {
    std::uint16_t pair = s.candidates[pivot];
    if (std::popcount(pair) != 2) continue;
    for (int a : kTables.peers[pivot]) {
      std::uint16_t first = s.candidates[a];
      if (std::popcount(first) != 2 || std::popcount(std::uint16_t(first & pair)) != 1) continue;
      std::uint16_t z = first & ~pair;
      std::uint16_t second = (pair & ~first) | z;
      for (int b : kTables.peers[pivot]) {
        if (b == a || s.candidates[b] != second) continue;
        bool progress = false;
        for (int cell : kTables.peers[a])
          if (cell != b && sees(cell, b)) progress |= eliminate(s, cell, z);
        if (progress) return true;
      }
    }
  }
  return false;
}

bool xyz_wing(Logic& s) {
  for (int pivot = 0; pivot < kCells; ++pivot) {
    std::uint16_t triple = s.candidates[pivot];
    if (std::popcount(triple) != 3) continue;
    for (int a : kTables.peers[pivot]) {
      std::uint16_t first = s.candidates[a];
      if (std::popcount(first) != 2 || (first & ~triple)) continue;
      for (int b : kTables.peers[pivot]) {
        std::uint16_t second = s.candidates[b];
        if (b == a || std::popcount(second) != 2 || (second & ~triple) || first == second ||
            (first | second) != triple)
          continue;
        std::uint16_t z = first & second;
        bool progress = false;
        for (int cell : kTables.peers[pivot])
          if (cell != a && cell != b && sees(cell, a) && sees(cell, b))
            progress |= eliminate(s, cell, z);
        if (progress) return true;
      }
    }
  }
  return false;
}

// Alternating inference chains over bivalue cells and bilocal units. Assuming
// a candidate is false, strong links force candidates true and weak links then
// force their peers false. Every candidate that sees the start and is forced
// false by the chain is false either way; a chain that forces the start true
// proves it.
bool chains(Logic& s) {
  int bilocal[27][10];
  for (int unit = 0; unit < 27; ++unit)
    for (int digit = 1; digit <= 9; ++digit) {
      int count = 0, cells = 0;
      for (int cell : kTables.units[unit])
        if (s.candidates[cell] & bit(digit)) {
          ++count;
          cells += cell;
        }
      // Store the sum so either cell can find its partner.
      bilocal[unit][digit] = count == 2 ? cells : -1;
    }
  short on[kNodes], off[kNodes];
  short queue[kNodes * 2];
  for (int start = 0; start < kNodes; ++start) {
    int start_cell = start / 9, start_digit = start % 9 + 1;
    if (!(s.candidates[start_cell] & bit(start_digit))) continue;
    std::fill(std::begin(on), std::end(on), -1);
    std::fill(std::begin(off), std::end(off), -1);
    off[start] = 0;
    int head = 0, tail = 0;
    queue[tail++] = short(start * 2);
    bool proven = false;
    while (head < tail && !proven) {
      int entry = queue[head++], node = entry / 2, cell = node / 9, digit = node % 9 + 1;
      bool is_on = entry & 1;
      int distance = is_on ? on[node] : off[node];
      if (distance >= kMaximumChainLinks) continue;
      auto visit = [&](int next_cell, int next_digit, bool next_on) {
        int next = next_cell * 9 + next_digit - 1;
        short* seen = next_on ? on : off;
        if (seen[next] >= 0) return;
        seen[next] = short(distance + 1);
        queue[tail++] = short(next * 2 + next_on);
        if (next_on && next == start) proven = true;
      };
      std::uint16_t candidates = s.candidates[cell];
      if (!is_on) {
        if (std::popcount(candidates) == 2)
          visit(cell, lowest_digit(candidates & ~bit(digit)), true);
        for (int unit : kTables.cell_units[cell])
          if (bilocal[unit][digit] >= 0) visit(bilocal[unit][digit] - cell, digit, true);
      } else {
        for (int other = 1; other <= 9; ++other)
          if (other != digit && (candidates & bit(other))) visit(cell, other, false);
        for (int peer : kTables.peers[cell])
          if (s.candidates[peer] & bit(digit)) visit(peer, digit, false);
      }
    }
    if (proven) {
      place(s, start_cell, start_digit);
      return true;
    }
    bool progress = false;
    for (int node = 0; node < kNodes; ++node) {
      if (node == start || off[node] < 0) continue;
      int cell = node / 9, digit = node % 9 + 1;
      bool linked = cell == start_cell ? digit != start_digit
                                       : digit == start_digit && sees(cell, start_cell);
      if (linked) progress |= eliminate(s, cell, bit(digit));
    }
    if (progress) return true;
  }
  return false;
}
}

int count_solutions(const Grid& grid, int limit, Grid* solution) {
  Search s;
  if (!load(s, grid)) return 0;
  s.limit = std::max(1, limit);
  s.solution = solution;
  search(s);
  return s.count;
}

Rating rate(const Grid& givens, const Grid* reference) {
  Logic s;
  s.reference = reference;
  std::fill(std::begin(s.candidates), std::end(s.candidates), kAll);
  Rating rating;
  for (int cell = 0; cell < kCells; ++cell)
    if (givens[cell]) {
      if (givens[cell] > 9 || !(s.candidates[cell] & bit(givens[cell]))) return rating;
      place(s, cell, givens[cell]);
    }
  while (true) {
    bool complete = true;
    for (int cell = 0; cell < kCells; ++cell) {
      if (s.values[cell]) continue;
      complete = false;
      if (!s.candidates[cell]) {
        rating.consistent = s.consistent;
        return rating;
      }
    }
    if (complete) {
      rating.solved = true;
      break;
    }
    int level = 0;
    if (naked_single(s) || hidden_single(s)) level = 1;
    else if (locked_candidates(s)) level = 2;
    else if (subsets(s)) level = 3;
    else if (fish(s) || xy_wing(s) || xyz_wing(s)) level = 4;
    else if (chains(s)) level = 5;
    else break;
    rating.level = std::max(rating.level, level);
    rating.advanced_steps += level >= 4;
    rating.chain_steps += level == 5;
  }
  rating.consistent = s.consistent;
  return rating;
}

// About one in ten random minimal puzzles needs four or more chain deductions.
bool is_extreme(const Rating& rating) {
  return rating.solved && rating.level == 5 && rating.chain_steps >= 4;
}

common::Result<Puzzle> generate_puzzle(std::uint64_t seed, const std::atomic<bool>& cancelled) {
  Random random{seed};
  while (!cancelled.load(std::memory_order_relaxed)) {
    Puzzle puzzle;
    puzzle.solution = random_solution(random);
    puzzle.givens = minimize(puzzle.solution, random);
    if (is_extreme(rate(puzzle.givens))) return puzzle;
  }
  return std::unexpected(common::Error{"Puzzle generation was cancelled"});
}
}
