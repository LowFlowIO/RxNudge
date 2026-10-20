#include "core/json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace core {
namespace {
const JsonValue kNull;
const std::string kEmpty;

struct P {
  const std::string& s;
  size_t i;
  std::string err;
  int depth = 0;
};

void skipWs(P& p) {
  while (p.i < p.s.size() && (p.s[p.i] == ' ' || p.s[p.i] == '\n' || p.s[p.i] == '\r' || p.s[p.i] == '\t')) p.i++;
}

void appendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) out += static_cast<char>(cp);
  else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

bool hex4(P& p, uint32_t& v) {
  if (p.i + 4 > p.s.size()) return false;
  v = 0;
  for (int k = 0; k < 4; k++) {
    const char c = p.s[p.i++];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
    else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
    else return false;
  }
  return true;
}

bool parseString(P& p, std::string& out) {
  if (p.s[p.i] != '"') return false;
  p.i++;
  out.clear();
  while (p.i < p.s.size()) {
    const char c = p.s[p.i++];
    if (c == '"') return true;
    if (static_cast<unsigned char>(c) < 0x20) return false;
    if (c != '\\') {
      out += c;
      continue;
    }
    if (p.i >= p.s.size()) return false;
    const char e = p.s[p.i++];
    switch (e) {
      case '"': out += '"'; break;
      case '\\': out += '\\'; break;
      case '/': out += '/'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 't': out += '\t'; break;
      case 'u': {
        uint32_t cp;
        if (!hex4(p, cp)) return false;
        if (cp >= 0xD800 && cp <= 0xDBFF) {
          uint32_t lo;
          if (p.i + 2 > p.s.size() || p.s[p.i] != '\\' || p.s[p.i + 1] != 'u') return false;
          p.i += 2;
          if (!hex4(p, lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
          cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
          return false;
        }
        appendUtf8(out, cp);
        break;
      }
      default:
        return false;
    }
  }
  return false;
}

bool parseValue(P& p, JsonValue& v);

bool parseNumber(P& p, JsonValue& v) {
  const size_t start = p.i;
  if (p.s[p.i] == '-') p.i++;
  bool isInt = true;
  while (p.i < p.s.size()) {
    const char c = p.s[p.i];
    if (c >= '0' && c <= '9') p.i++;
    else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
      isInt = false;
      p.i++;
    } else break;
  }
  const std::string num = p.s.substr(start, p.i - start);
  if (num.empty() || num == "-") return false;
  char* end = nullptr;
  if (isInt && num.size() < 19) {
    const long long x = std::strtoll(num.c_str(), &end, 10);
    if (*end != '\0') return false;
    v = JsonValue::integer(x);
  } else {
    const double d = std::strtod(num.c_str(), &end);
    if (*end != '\0') return false;
    v = JsonValue::number(d);
  }
  return true;
}

bool parseValue(P& p, JsonValue& v) {
  skipWs(p);
  if (p.i >= p.s.size()) return false;
  if (++p.depth > 64) return false;
  const char c = p.s[p.i];
  bool ok = false;
  if (c == '{') {
    p.i++;
    v = JsonValue::object();
    skipWs(p);
    if (p.i < p.s.size() && p.s[p.i] == '}') {
      p.i++;
      ok = true;
    } else {
      for (;;) {
        skipWs(p);
        std::string key;
        if (p.i >= p.s.size() || !parseString(p, key)) break;
        skipWs(p);
        if (p.i >= p.s.size() || p.s[p.i] != ':') break;
        p.i++;
        JsonValue child;
        if (!parseValue(p, child)) break;
        v.set(key, child);
        skipWs(p);
        if (p.i < p.s.size() && p.s[p.i] == ',') {
          p.i++;
          continue;
        }
        if (p.i < p.s.size() && p.s[p.i] == '}') {
          p.i++;
          ok = true;
        }
        break;
      }
    }
  } else if (c == '[') {
    p.i++;
    v = JsonValue::array();
    skipWs(p);
    if (p.i < p.s.size() && p.s[p.i] == ']') {
      p.i++;
      ok = true;
    } else {
      for (;;) {
        JsonValue child;
        if (!parseValue(p, child)) break;
        v.push(child);
        skipWs(p);
        if (p.i < p.s.size() && p.s[p.i] == ',') {
          p.i++;
          continue;
        }
        if (p.i < p.s.size() && p.s[p.i] == ']') {
          p.i++;
          ok = true;
        }
        break;
      }
    }
  } else if (c == '"') {
    std::string s;
    ok = parseString(p, s);
    if (ok) v = JsonValue::string(s);
  } else if (p.s.compare(p.i, 4, "true") == 0) {
    p.i += 4;
    v = JsonValue::boolean(true);
    ok = true;
  } else if (p.s.compare(p.i, 5, "false") == 0) {
    p.i += 5;
    v = JsonValue::boolean(false);
    ok = true;
  } else if (p.s.compare(p.i, 4, "null") == 0) {
    p.i += 4;
    v = JsonValue();
    ok = true;
  } else if (c == '-' || (c >= '0' && c <= '9')) {
    ok = parseNumber(p, v);
  }
  p.depth--;
  return ok;
}

void escape(std::string& out, const std::string& s) {
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  out += '"';
}

}  // namespace

JsonValue JsonValue::boolean(bool v) {
  JsonValue j;
  j.type_ = Type::Bool;
  j.b_ = v;
  return j;
}
JsonValue JsonValue::integer(int64_t v) {
  JsonValue j;
  j.type_ = Type::Int;
  j.i_ = v;
  return j;
}
JsonValue JsonValue::number(double v) {
  JsonValue j;
  j.type_ = Type::Double;
  j.d_ = v;
  return j;
}
JsonValue JsonValue::string(const std::string& v) {
  JsonValue j;
  j.type_ = Type::String;
  j.s_ = v;
  return j;
}
JsonValue JsonValue::array() {
  JsonValue j;
  j.type_ = Type::Array;
  return j;
}
JsonValue JsonValue::object() {
  JsonValue j;
  j.type_ = Type::Object;
  return j;
}

bool JsonValue::asBool(bool def) const { return type_ == Type::Bool ? b_ : def; }
int64_t JsonValue::asInt(int64_t def) const {
  if (type_ == Type::Int) return i_;
  if (type_ == Type::Double) return static_cast<int64_t>(std::llround(d_));
  return def;
}
double JsonValue::asDouble(double def) const {
  if (type_ == Type::Double) return d_;
  if (type_ == Type::Int) return static_cast<double>(i_);
  return def;
}
const std::string& JsonValue::asString() const { return type_ == Type::String ? s_ : kEmpty; }

size_t JsonValue::size() const {
  if (type_ == Type::Array) return arr_.size();
  if (type_ == Type::Object) return obj_.size();
  return 0;
}
const JsonValue& JsonValue::at(size_t i) const { return (type_ == Type::Array && i < arr_.size()) ? arr_[i] : kNull; }
JsonValue& JsonValue::push(const JsonValue& v) {
  if (type_ != Type::Array) *this = array();
  arr_.push_back(v);
  return arr_.back();
}
bool JsonValue::has(const std::string& key) const {
  for (const auto& kv : obj_)
    if (kv.first == key) return true;
  return false;
}
const JsonValue& JsonValue::operator[](const std::string& key) const {
  for (const auto& kv : obj_)
    if (kv.first == key) return kv.second;
  return kNull;
}
JsonValue& JsonValue::set(const std::string& key, const JsonValue& v) {
  if (type_ != Type::Object) *this = object();
  for (auto& kv : obj_)
    if (kv.first == key) {
      kv.second = v;
      return kv.second;
    }
  obj_.emplace_back(key, v);
  return obj_.back().second;
}
bool JsonValue::remove(const std::string& key) {
  for (auto it = obj_.begin(); it != obj_.end(); ++it)
    if (it->first == key) {
      obj_.erase(it);
      return true;
    }
  return false;
}

void JsonValue::dumpTo(std::string& out, bool pretty, int indent) const {
  auto nl = [&](int ind) {
    if (!pretty) return;
    out += '\n';
    out.append(static_cast<size_t>(ind) * 2, ' ');
  };
  char buf[40];
  switch (type_) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += b_ ? "true" : "false"; break;
    case Type::Int:
      std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(i_));
      out += buf;
      break;
    case Type::Double:
      if (!std::isfinite(d_)) {
        out += "null";
      } else {
        std::snprintf(buf, sizeof(buf), "%.17g", d_);
        out += buf;
      }
      break;
    case Type::String: escape(out, s_); break;
    case Type::Array:
      out += '[';
      for (size_t i = 0; i < arr_.size(); i++) {
        if (i) out += ',';
        nl(indent + 1);
        arr_[i].dumpTo(out, pretty, indent + 1);
      }
      if (!arr_.empty()) nl(indent);
      out += ']';
      break;
    case Type::Object:
      out += '{';
      for (size_t i = 0; i < obj_.size(); i++) {
        if (i) out += ',';
        nl(indent + 1);
        escape(out, obj_[i].first);
        out += pretty ? ": " : ":";
        obj_[i].second.dumpTo(out, pretty, indent + 1);
      }
      if (!obj_.empty()) nl(indent);
      out += '}';
      break;
  }
}

std::string JsonValue::dump(bool pretty) const {
  std::string out;
  dumpTo(out, pretty, 0);
  return out;
}

bool parseJson(const std::string& text, JsonValue& out, std::string* error) {
  P p{text, 0, {}};
  JsonValue v;
  if (!parseValue(p, v)) {
    if (error) *error = "JSON syntax error near byte " + std::to_string(p.i);
    return false;
  }
  skipWs(p);
  if (p.i != text.size()) {
    if (error) *error = "trailing data after JSON value";
    return false;
  }
  out = v;
  return true;
}

}  // namespace core
