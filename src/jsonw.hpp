// jsonw.hpp - Tiny JSON writer. Responses are built here (version-independent);
// request bodies are parsed with Crow's own crow::json::load in main.cpp.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace jsonw {

inline std::string quote(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));  // UTF-8 bytes pass through unchanged
        }
    }
  }
  out.push_back('"');
  return out;
}

class Obj {
 public:
  Obj& str(const std::string& k, const std::string& v) { return raw(k, quote(v)); }
  Obj& num(const std::string& k, int64_t v) { return raw(k, std::to_string(v)); }
  Obj& real(const std::string& k, double v) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.2f", v);
    return raw(k, buf);
  }
  Obj& boolean(const std::string& k, bool v) { return raw(k, v ? "true" : "false"); }
  Obj& null(const std::string& k) { return raw(k, "null"); }
  Obj& raw(const std::string& k, const std::string& json) {
    if (!first_) s_ += ',';
    first_ = false;
    s_ += quote(k);
    s_ += ':';
    s_ += json;
    return *this;
  }
  std::string done() const { return s_ + "}"; }

 private:
  std::string s_ = "{";
  bool first_ = true;
};

inline std::string arr(const std::vector<std::string>& items) {
  std::string s = "[";
  for (size_t i = 0; i < items.size(); ++i) {
    if (i) s += ',';
    s += items[i];
  }
  return s + "]";
}

}  // namespace jsonw
