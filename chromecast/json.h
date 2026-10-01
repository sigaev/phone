#pragma once

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chromecast {
// A parsed JSON value. Objects keep their keys in order, parallel to items.
struct Json {
  enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };
  Type type = Type::kNull;
  bool boolean = false;
  double number = 0;
  std::string string;
  std::vector<std::string> keys;
  std::vector<Json> items;
};

// Nesting is limited, so hostile input cannot exhaust the stack.
std::optional<Json> parse_json(std::string_view text);
// Follow object keys; null when any step is missing or not an object.
const Json* find(const Json* value, std::initializer_list<std::string_view> path);
std::optional<std::int64_t> as_integer(const Json* value);
std::optional<std::string> as_string(const Json* value);
std::optional<bool> as_bool(const Json* value);
// A quoted JSON string literal.
std::string quote(std::string_view value);
}
