#pragma once
// Minimal JSON DOM, parser and writer (UTF-8). Pure C++, no exceptions.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace core {

class JsonValue {
 public:
  enum class Type : uint8_t { Null, Bool, Int, Double, String, Array, Object };

  JsonValue() = default;
  static JsonValue boolean(bool v);
  static JsonValue integer(int64_t v);
  static JsonValue number(double v);
  static JsonValue string(const std::string& v);
  static JsonValue array();
  static JsonValue object();

  Type type() const { return type_; }
  bool isNull() const { return type_ == Type::Null; }
  bool isObject() const { return type_ == Type::Object; }
  bool isArray() const { return type_ == Type::Array; }
  bool isString() const { return type_ == Type::String; }
  bool isNumber() const { return type_ == Type::Int || type_ == Type::Double; }

  bool asBool(bool def = false) const;
  int64_t asInt(int64_t def = 0) const;
  double asDouble(double def = 0) const;
  const std::string& asString() const;  // empty string if not a string

  // Array
  size_t size() const;
  const JsonValue& at(size_t i) const;
  JsonValue& push(const JsonValue& v);
  // Object
  bool has(const std::string& key) const;
  const JsonValue& operator[](const std::string& key) const;  // Null if missing
  JsonValue& set(const std::string& key, const JsonValue& v);
  bool remove(const std::string& key);
  const std::vector<std::pair<std::string, JsonValue>>& members() const { return obj_; }

  std::string dump(bool pretty = false) const;

 private:
  void dumpTo(std::string& out, bool pretty, int indent) const;
  Type type_ = Type::Null;
  bool b_ = false;
  int64_t i_ = 0;
  double d_ = 0;
  std::string s_;
  std::vector<JsonValue> arr_;
  std::vector<std::pair<std::string, JsonValue>> obj_;
};

bool parseJson(const std::string& text, JsonValue& out, std::string* error = nullptr);

}  // namespace core
