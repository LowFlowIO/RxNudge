#pragma once
// Self-contained POSIX TZ parser and UTC<->local conversion (runs on host).
// Supports: std offset [dst [offset] [,rule,rule]] with rules Jn, n, Mm.w.d
// and optional /time (hours may be negative or > 24, POSIX 2024 extension).
#include <cstdint>
#include <string>

namespace core {

constexpr const char* kDefaultTz = "CET-1CEST,M3.5.0,M10.5.0/3";  // Europe/Berlin

struct TzRule {
  enum class Kind : uint8_t { Julian1, Julian0, MonthWeekDay } kind = Kind::MonthWeekDay;
  int month = 0;  // 1..12 (M)
  int week = 0;   // 1..5  (M), 5 = last
  int wday = 0;   // 0..6  (M), 0 = Sunday
  int day = 0;    // J: 1..365, n: 0..365
  int32_t timeSec = 7200;  // local wall time of the transition, default 02:00
};

struct PosixTz {
  std::string stdName;
  std::string dstName;
  int32_t stdOffset = 0;  // seconds EAST of UTC (local = utc + offset); POSIX sign inverted
  int32_t dstOffset = 0;
  bool hasDst = false;
  TzRule start;  // std -> dst
  TzRule end;    // dst -> std
};

bool parsePosixTz(const std::string& spec, PosixTz& out, std::string* error = nullptr);

struct LocalTime {
  int32_t days;      // local day number since 1970-01-01
  int32_t secOfDay;  // 0..86399
  int32_t offset;    // seconds east of UTC in effect
  bool dst;
};

int32_t utcOffsetAt(const PosixTz& tz, int64_t utc, bool* isDst = nullptr);
LocalTime utcToLocal(const PosixTz& tz, int64_t utc);

enum class LocalResolution : uint8_t { Unique, Gap, Ambiguous };
// Converts a local wall-clock time to UTC.
// Gap (nonexistent time): returns the first valid instant after the gap (the transition).
// Ambiguous (repeated hour): returns the FIRST occurrence (earlier UTC instant).
int64_t localToUtc(const PosixTz& tz, int32_t days, int32_t secOfDay, LocalResolution* res = nullptr);

// UTC instants of the two transitions in a given year (only valid if hasDst).
int64_t dstStartUtc(const PosixTz& tz, int year);
int64_t dstEndUtc(const PosixTz& tz, int year);

}  // namespace core
