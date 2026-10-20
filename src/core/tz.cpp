#include "core/tz.h"

#include <cctype>

#include "core/civil.h"

namespace core {
namespace {

struct Parser {
  const std::string& s;
  size_t i = 0;
  bool eof() const { return i >= s.size(); }
  char peek() const { return eof() ? '\0' : s[i]; }
};

bool parseName(Parser& p, std::string& out) {
  out.clear();
  if (p.peek() == '<') {
    p.i++;
    while (!p.eof() && p.peek() != '>') out += p.s[p.i++];
    if (p.peek() != '>') return false;
    p.i++;
  } else {
    while (!p.eof() && std::isalpha(static_cast<unsigned char>(p.peek()))) out += p.s[p.i++];
  }
  return out.size() >= 3;
}

bool parseNum(Parser& p, int& v, int maxDigits) {
  int n = 0;
  v = 0;
  while (!p.eof() && std::isdigit(static_cast<unsigned char>(p.peek())) && n < maxDigits) {
    v = v * 10 + (p.s[p.i++] - '0');
    n++;
  }
  return n > 0;
}

// [+-]hh[:mm[:ss]] -> seconds (sign as written)
bool parseHms(Parser& p, int32_t& sec, int maxHours) {
  int sign = 1;
  if (p.peek() == '+' || p.peek() == '-') sign = p.s[p.i++] == '-' ? -1 : 1;
  int h = 0, m = 0, sc = 0;
  if (!parseNum(p, h, 3) || h > maxHours) return false;
  if (p.peek() == ':') {
    p.i++;
    if (!parseNum(p, m, 2) || m > 59) return false;
    if (p.peek() == ':') {
      p.i++;
      if (!parseNum(p, sc, 2) || sc > 59) return false;
    }
  }
  sec = sign * (h * 3600 + m * 60 + sc);
  return true;
}

bool parseRule(Parser& p, TzRule& r) {
  if (p.peek() == 'M') {
    p.i++;
    r.kind = TzRule::Kind::MonthWeekDay;
    if (!parseNum(p, r.month, 2) || r.month < 1 || r.month > 12 || p.peek() != '.') return false;
    p.i++;
    if (!parseNum(p, r.week, 1) || r.week < 1 || r.week > 5 || p.peek() != '.') return false;
    p.i++;
    if (!parseNum(p, r.wday, 1) || r.wday > 6) return false;
  } else if (p.peek() == 'J') {
    p.i++;
    r.kind = TzRule::Kind::Julian1;
    if (!parseNum(p, r.day, 3) || r.day < 1 || r.day > 365) return false;
  } else {
    r.kind = TzRule::Kind::Julian0;
    if (!parseNum(p, r.day, 3) || r.day > 365) return false;
  }
  r.timeSec = 7200;
  if (p.peek() == '/') {
    p.i++;
    if (!parseHms(p, r.timeSec, 167)) return false;
  }
  return true;
}

int32_t ruleDay(const TzRule& r, int year) {
  switch (r.kind) {
    case TzRule::Kind::Julian1: {
      // 1..365, Feb 29 never counted
      int32_t d = daysFromCivil(year, 1, 1) + r.day - 1;
      if (isLeapYear(year) && r.day >= 60) d += 1;
      return d;
    }
    case TzRule::Kind::Julian0:
      return daysFromCivil(year, 1, 1) + r.day;
    case TzRule::Kind::MonthWeekDay:
    default: {
      const int32_t first = daysFromCivil(year, r.month, 1);
      const int wdFirst = weekdayFromDays(first);
      int32_t d = first + ((r.wday - wdFirst + 7) % 7) + (r.week - 1) * 7;
      const int32_t last = first + daysInMonth(year, r.month) - 1;
      while (d > last) d -= 7;
      return d;
    }
  }
}

}  // namespace

bool parsePosixTz(const std::string& spec, PosixTz& out, std::string* error) {
  auto fail = [&](const char* msg) {
    if (error) *error = msg;
    return false;
  };
  PosixTz tz;
  Parser p{spec};
  if (p.peek() == ':') return fail("implementation-defined ':' form not supported");
  if (!parseName(p, tz.stdName)) return fail("bad std name");
  int32_t off = 0;
  if (!parseHms(p, off, 24)) return fail("bad std offset");
  tz.stdOffset = -off;
  if (p.eof()) {
    out = tz;
    return true;
  }
  if (!parseName(p, tz.dstName)) return fail("bad dst name");
  tz.hasDst = true;
  tz.dstOffset = tz.stdOffset + 3600;
  if (!p.eof() && p.peek() != ',') {
    if (!parseHms(p, off, 24)) return fail("bad dst offset");
    tz.dstOffset = -off;
  }
  if (p.eof()) {
    // No rules: POSIX leaves this implementation-defined; use US rules like glibc.
    tz.start = TzRule{TzRule::Kind::MonthWeekDay, 3, 2, 0, 0, 7200};
    tz.end = TzRule{TzRule::Kind::MonthWeekDay, 11, 1, 0, 0, 7200};
    out = tz;
    return true;
  }
  if (p.peek() != ',') return fail("expected ',' before start rule");
  p.i++;
  if (!parseRule(p, tz.start)) return fail("bad start rule");
  if (p.peek() != ',') return fail("expected ',' before end rule");
  p.i++;
  if (!parseRule(p, tz.end)) return fail("bad end rule");
  if (!p.eof()) return fail("trailing characters");
  out = tz;
  return true;
}

int64_t dstStartUtc(const PosixTz& tz, int year) {
  // Transition happens at local standard time.
  return static_cast<int64_t>(ruleDay(tz.start, year)) * 86400 + tz.start.timeSec - tz.stdOffset;
}

int64_t dstEndUtc(const PosixTz& tz, int year) {
  // Transition happens at local daylight time.
  return static_cast<int64_t>(ruleDay(tz.end, year)) * 86400 + tz.end.timeSec - tz.dstOffset;
}

int32_t utcOffsetAt(const PosixTz& tz, int64_t utc, bool* isDst) {
  bool dst = false;
  if (tz.hasDst) {
    const int year = civilFromDays(static_cast<int32_t>(floorDiv(utc + tz.stdOffset, 86400))).y;
    const int64_t s = dstStartUtc(tz, year);
    const int64_t e = dstEndUtc(tz, year);
    if (s < e) dst = utc >= s && utc < e;
    else dst = !(utc >= e && utc < s);  // southern hemisphere
  }
  if (isDst) *isDst = dst;
  return dst ? tz.dstOffset : tz.stdOffset;
}

LocalTime utcToLocal(const PosixTz& tz, int64_t utc) {
  bool dst = false;
  const int32_t off = utcOffsetAt(tz, utc, &dst);
  const int64_t local = utc + off;
  LocalTime lt;
  lt.days = static_cast<int32_t>(floorDiv(local, 86400));
  lt.secOfDay = static_cast<int32_t>(floorMod(local, 86400));
  lt.offset = off;
  lt.dst = dst;
  return lt;
}

int64_t localToUtc(const PosixTz& tz, int32_t days, int32_t secOfDay, LocalResolution* res) {
  const int64_t local = static_cast<int64_t>(days) * 86400 + secOfDay;
  if (!tz.hasDst) {
    if (res) *res = LocalResolution::Unique;
    return local - tz.stdOffset;
  }
  const int64_t a = local - tz.stdOffset;
  const int64_t b = local - tz.dstOffset;
  const bool aOk = utcOffsetAt(tz, a) == tz.stdOffset;
  const bool bOk = utcOffsetAt(tz, b) == tz.dstOffset;
  if (aOk && bOk && a != b) {
    if (res) *res = LocalResolution::Ambiguous;
    return a < b ? a : b;
  }
  if (aOk || bOk) {
    if (res) *res = LocalResolution::Unique;
    return aOk ? a : b;
  }
  // Gap: find the transition whose wall-clock jump skips over `local`.
  if (res) *res = LocalResolution::Gap;
  const int year = civilFromDays(days).y;
  for (int y = year - 1; y <= year + 1; y++) {
    const int64_t ts[2] = {dstStartUtc(tz, y), dstEndUtc(tz, y)};
    for (int64_t t : ts) {
      const int32_t before = utcOffsetAt(tz, t - 1);
      const int32_t after = utcOffsetAt(tz, t);
      if (after > before && t + before <= local && local < t + after) {
        return t;  // first valid instant (minute-aligned for all real zones)
      }
    }
  }
  return a;  // unreachable for consistent rules
}

}  // namespace core
