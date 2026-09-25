#pragma once

#include <expected>
#include <string>

namespace common {

struct Error {
  std::string message;
};

template <typename T>
using Result = std::expected<T, Error>;

}
