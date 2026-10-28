// A small JSON reader for the GitHub API's answers: enough of the format to walk a release list.
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kameo::launcher::json {

struct Value {
  enum class Type { Null, Bool, Number, String, Array, Object };
  Type type = Type::Null;
  bool boolean = false;
  double number = 0.0;
  std::string string;
  std::vector<Value> array;
  std::vector<std::pair<std::string, Value>> object;

  // The member named `key`, or a shared null value when there is none or this is not an object.
  const Value& operator[](std::string_view key) const;
  bool IsNull() const { return type == Type::Null; }
  std::string Str() const { return type == Type::String ? string : std::string(); }
  bool Bool() const { return type == Type::Bool && boolean; }
  double Num() const { return type == Type::Number ? number : 0.0; }
};

// Parses a whole document; false (with `error` set) on malformed input.
bool Parse(std::string_view text, Value& out, std::string& error);

}  // namespace kameo::launcher::json
