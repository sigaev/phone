#pragma once

#include "common/result.h"

namespace native_buttons {
common::Result<int> load_count(const char* directory);
common::Result<void> save_count(const char* directory, int count);
}  // namespace native_buttons
