#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "common/result.h"

namespace sudoku {
// A missing file loads as empty.
common::Result<std::vector<std::byte>> load_file(const char* directory, const char* name);
// Replace the file atomically after flushing a temporary copy, so a failed
// write preserves the previous contents.
common::Result<void> save_file(const char* directory, const char* name,
                               std::span<const std::byte> bytes);
}
