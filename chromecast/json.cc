#include "chromecast/json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace chromecast {
namespace {
constexpr int kMaximumDepth = 32;

struct Parser {
  std::string_view text;
  std::size_t at = 0;

  void space() {
    while (at < text.size() &&
           (text[at] == ' ' || text[at] == '\t' || text[at] == '\n' || text[at] == '\r'))
      ++at;
  }

  bool literal(std::string_view word) {
    if (text.substr(at, word.size()) != word) return false;
    at += word.size();
    return true;
  }

  static void append_utf8(std::string& out, std::uint32_t code) {
    if (code < 0x80) out += char(code);
    else if (code < 0x800) {
      out += char(0xc0 | code >> 6);
      out += char(0x80 | (code & 0x3f));
    } else if (code < 0x10000) {
      out += char(0xe0 | code >> 12);
      out += char(0x80 | (code >> 6 & 0x3f));
      out += char(0x80 | (code & 0x3f));
    } else {
      out += char(0xf0 | code >> 18);
      out += char(0x80 | (code >> 12 & 0x3f));
      out += char(0x80 | (code >> 6 & 0x3f));
      out += char(0x80 | (code & 0x3f));
    }
  }

  std::optional<std::uint32_t> hex4() {
    if (at + 4 > text.size()) return std::nullopt;
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      char c = text[at++];
      value <<= 4;
      if (c >= '0' && c <= '9') value |= c - '0';
      else if (c >= 'a' && c <= 'f') value |= c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') value |= c - 'A' + 10;
      else return std::nullopt;
    }
    return value;
  }

  std::optional<std::string> string() {
    if (at >= text.size() || text[at] != '"') return std::nullopt;
    ++at;
    std::string out;
    while (at < text.size()) {
      char c = text[at++];
      if (c == '"') return out;
      if (static_cast<unsigned char>(c) < 0x20) return std::nullopt;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (at >= text.size()) return std::nullopt;
      switch (char e = text[at++]) {
        case '"':
        case '\\':
        case '/':
          out += e;
          break;
        case 'b':
          out += '\b';
          break;
        case 'f':
          out += '\f';
          break;
        case 'n':
          out += '\n';
          break;
        case 'r':
          out += '\r';
          break;
        case 't':
          out += '\t';
          break;
        case 'u': {
          auto code = hex4();
          if (!code) return std::nullopt;
          if (*code >= 0xd800 && *code < 0xdc00) {
            if (!literal("\\u")) return std::nullopt;
            auto low = hex4();
            if (!low || *low < 0xdc00 || *low >= 0xe000) return std::nullopt;
            *code = 0x10000 + ((*code - 0xd800) << 10) + (*low - 0xdc00);
          } else if (*code >= 0xdc00 && *code < 0xe000) {
            return std::nullopt;
          }
          append_utf8(out, *code);
          break;
        }
        default:
          return std::nullopt;
      }
    }
    return std::nullopt;
  }

  std::optional<double> number() {
    std::size_t start = at;
    if (at < text.size() && text[at] == '-') ++at;
    // No leading zeros.
    if (at + 1 < text.size() && text[at] == '0' && text[at + 1] >= '0' && text[at + 1] <= '9')
      return std::nullopt;
    auto digits = [&] {
      std::size_t first = at;
      while (at < text.size() && text[at] >= '0' && text[at] <= '9') ++at;
      return at > first;
    };
    if (!digits()) return std::nullopt;
    if (at < text.size() && text[at] == '.') {
      ++at;
      if (!digits()) return std::nullopt;
    }
    if (at < text.size() && (text[at] == 'e' || text[at] == 'E')) {
      ++at;
      if (at < text.size() && (text[at] == '+' || text[at] == '-')) ++at;
      if (!digits()) return std::nullopt;
    }
    std::string token(text.substr(start, at - start));
    double value = std::strtod(token.c_str(), nullptr);
    if (!std::isfinite(value)) return std::nullopt;
    return value;
  }

  bool value(Json& out, int depth) {
    if (depth > kMaximumDepth) return false;
    space();
    if (at >= text.size()) return false;
    char c = text[at];
    if (c == '{') {
      ++at;
      out.type = Json::Type::kObject;
      space();
      if (at < text.size() && text[at] == '}') {
        ++at;
        return true;
      }
      while (true) {
        space();
        auto key = string();
        if (!key) return false;
        space();
        if (at >= text.size() || text[at++] != ':') return false;
        out.keys.push_back(std::move(*key));
        out.items.emplace_back();
        if (!value(out.items.back(), depth + 1)) return false;
        space();
        if (at >= text.size()) return false;
        if (text[at] == ',') {
          ++at;
          continue;
        }
        if (text[at++] != '}') return false;
        return true;
      }
    }
    if (c == '[') {
      ++at;
      out.type = Json::Type::kArray;
      space();
      if (at < text.size() && text[at] == ']') {
        ++at;
        return true;
      }
      while (true) {
        out.items.emplace_back();
        if (!value(out.items.back(), depth + 1)) return false;
        space();
        if (at >= text.size()) return false;
        if (text[at] == ',') {
          ++at;
          continue;
        }
        if (text[at++] != ']') return false;
        return true;
      }
    }
    if (c == '"') {
      auto parsed = string();
      if (!parsed) return false;
      out.type = Json::Type::kString;
      out.string = std::move(*parsed);
      return true;
    }
    if (literal("true")) {
      out.type = Json::Type::kBool;
      out.boolean = true;
      return true;
    }
    if (literal("false")) {
      out.type = Json::Type::kBool;
      return true;
    }
    if (literal("null")) return true;
    auto parsed = number();
    if (!parsed) return false;
    out.type = Json::Type::kNumber;
    out.number = *parsed;
    return true;
  }
};
}

std::optional<Json> parse_json(std::string_view text) {
  Parser parser{text};
  Json result;
  if (!parser.value(result, 0)) return std::nullopt;
  parser.space();
  if (parser.at != text.size()) return std::nullopt;
  return result;
}

const Json* find(const Json* value, std::initializer_list<std::string_view> path) {
  for (std::string_view key : path) {
    if (!value || value->type != Json::Type::kObject) return nullptr;
    const Json* next = nullptr;
    for (std::size_t i = 0; i < value->keys.size(); ++i)
      if (value->keys[i] == key) {
        next = &value->items[i];
        break;
      }
    value = next;
  }
  return value;
}

std::optional<std::int64_t> as_integer(const Json* value) {
  if (!value || value->type != Json::Type::kNumber) return std::nullopt;
  double number = value->number;
  if (number != std::floor(number) || std::fabs(number) > 9e15) return std::nullopt;
  return std::int64_t(number);
}

std::optional<std::string> as_string(const Json* value) {
  if (!value || value->type != Json::Type::kString) return std::nullopt;
  return value->string;
}

std::optional<bool> as_bool(const Json* value) {
  if (!value || value->type != Json::Type::kBool) return std::nullopt;
  return value->boolean;
}

std::string quote(std::string_view value) {
  std::string out = "\"";
  for (char c : value) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char escape[8];
          std::snprintf(escape, sizeof(escape), "\\u%04x", unsigned(c));
          out += escape;
        } else {
          out += c;
        }
    }
  }
  out += '"';
  return out;
}
}
