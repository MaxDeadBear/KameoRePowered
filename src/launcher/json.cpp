#include "launcher/json.h"

#include <cstdint>
#include <cstdlib>

namespace kameo::launcher::json {
namespace {

const Value kNull;

class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  bool Document(Value& out, std::string& error) {
    if (!ParseValue(out, 0)) {
      error = error_.empty() ? "malformed JSON" : error_;
      return false;
    }
    SkipSpace();
    if (at_ != text_.size()) {
      error = "unexpected text after the JSON document";
      return false;
    }
    return true;
  }

 private:
  static constexpr int kMaxDepth = 64;

  void SkipSpace() {
    while (at_ < text_.size() && (text_[at_] == ' ' || text_[at_] == '\t' || text_[at_] == '\n' || text_[at_] == '\r')) ++at_;
  }

  bool Fail(const char* message) {
    if (error_.empty()) error_ = std::string(message) + " at offset " + std::to_string(at_);
    return false;
  }

  bool Literal(std::string_view word) {
    if (text_.substr(at_, word.size()) != word) return Fail("unknown literal");
    at_ += word.size();
    return true;
  }

  static void AppendUtf8(std::string& out, uint32_t point) {
    if (point < 0x80) {
      out.push_back(char(point));
    } else if (point < 0x800) {
      out.push_back(char(0xC0 | (point >> 6)));
      out.push_back(char(0x80 | (point & 0x3F)));
    } else if (point < 0x10000) {
      out.push_back(char(0xE0 | (point >> 12)));
      out.push_back(char(0x80 | ((point >> 6) & 0x3F)));
      out.push_back(char(0x80 | (point & 0x3F)));
    } else {
      out.push_back(char(0xF0 | (point >> 18)));
      out.push_back(char(0x80 | ((point >> 12) & 0x3F)));
      out.push_back(char(0x80 | ((point >> 6) & 0x3F)));
      out.push_back(char(0x80 | (point & 0x3F)));
    }
  }

  bool Hex4(uint32_t& out) {
    if (at_ + 4 > text_.size()) return Fail("short \\u escape");
    out = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[at_++];
      out <<= 4;
      if (c >= '0' && c <= '9') out |= uint32_t(c - '0');
      else if (c >= 'a' && c <= 'f') out |= uint32_t(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') out |= uint32_t(c - 'A' + 10);
      else return Fail("bad \\u escape");
    }
    return true;
  }

  bool ParseString(std::string& out) {
    ++at_;  // the opening quote
    while (at_ < text_.size()) {
      const char c = text_[at_++];
      if (c == '"') return true;
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (at_ >= text_.size()) break;
      const char escape = text_[at_++];
      switch (escape) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          uint32_t point = 0;
          if (!Hex4(point)) return false;
          // A surrogate pair spells one code point above the basic plane.
          if (point >= 0xD800 && point <= 0xDBFF && text_.substr(at_, 2) == "\\u") {
            at_ += 2;
            uint32_t low = 0;
            if (!Hex4(low)) return false;
            if (low >= 0xDC00 && low <= 0xDFFF) point = 0x10000 + ((point - 0xD800) << 10) + (low - 0xDC00);
          }
          AppendUtf8(out, point);
          break;
        }
        default: return Fail("bad escape");
      }
    }
    return Fail("unterminated string");
  }

  bool ParseNumber(Value& out) {
    const size_t start = at_;
    while (at_ < text_.size() && (text_[at_] == '-' || text_[at_] == '+' || text_[at_] == '.' || text_[at_] == 'e' || text_[at_] == 'E' || (text_[at_] >= '0' && text_[at_] <= '9'))) ++at_;
    if (at_ == start) return Fail("expected a value");
    out.type = Value::Type::Number;
    out.number = std::strtod(std::string(text_.substr(start, at_ - start)).c_str(), nullptr);
    return true;
  }

  bool ParseValue(Value& out, int depth) {
    if (depth > kMaxDepth) return Fail("nested too deeply");
    SkipSpace();
    if (at_ >= text_.size()) return Fail("unexpected end");
    const char c = text_[at_];
    if (c == '"') {
      out.type = Value::Type::String;
      return ParseString(out.string);
    }
    if (c == '{') {
      out.type = Value::Type::Object;
      ++at_;
      SkipSpace();
      if (at_ < text_.size() && text_[at_] == '}') {
        ++at_;
        return true;
      }
      while (true) {
        SkipSpace();
        if (at_ >= text_.size() || text_[at_] != '"') return Fail("expected a member name");
        std::pair<std::string, Value> member;
        if (!ParseString(member.first)) return false;
        SkipSpace();
        if (at_ >= text_.size() || text_[at_] != ':') return Fail("expected ':'");
        ++at_;
        if (!ParseValue(member.second, depth + 1)) return false;
        out.object.push_back(std::move(member));
        SkipSpace();
        if (at_ < text_.size() && text_[at_] == ',') {
          ++at_;
          continue;
        }
        if (at_ < text_.size() && text_[at_] == '}') {
          ++at_;
          return true;
        }
        return Fail("expected ',' or '}'");
      }
    }
    if (c == '[') {
      out.type = Value::Type::Array;
      ++at_;
      SkipSpace();
      if (at_ < text_.size() && text_[at_] == ']') {
        ++at_;
        return true;
      }
      while (true) {
        Value item;
        if (!ParseValue(item, depth + 1)) return false;
        out.array.push_back(std::move(item));
        SkipSpace();
        if (at_ < text_.size() && text_[at_] == ',') {
          ++at_;
          continue;
        }
        if (at_ < text_.size() && text_[at_] == ']') {
          ++at_;
          return true;
        }
        return Fail("expected ',' or ']'");
      }
    }
    if (c == 't') {
      out.type = Value::Type::Bool;
      out.boolean = true;
      return Literal("true");
    }
    if (c == 'f') {
      out.type = Value::Type::Bool;
      out.boolean = false;
      return Literal("false");
    }
    if (c == 'n') {
      out.type = Value::Type::Null;
      return Literal("null");
    }
    return ParseNumber(out);
  }

  std::string_view text_;
  size_t at_ = 0;
  std::string error_;
};

}  // namespace

const Value& Value::operator[](std::string_view key) const {
  if (type != Type::Object) return kNull;
  for (const auto& member : object) {
    if (member.first == key) return member.second;
  }
  return kNull;
}

bool Parse(std::string_view text, Value& out, std::string& error) {
  out = Value();
  Parser parser(text);
  return parser.Document(out, error);
}

}  // namespace kameo::launcher::json
