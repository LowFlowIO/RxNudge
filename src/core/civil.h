#pragma once
// Proleptic Gregorian calendar helpers (H. Hinnant's algorithms). Pure.
#include <cstdint>

namespace core {

constexpr int32_t kNoDate = INT32_MIN;  // "unset" day number

struct Ymd {
  int y;
  int m;  // 1..12
  int d;  // 1..31
};

int32_t daysFromCivil(int y, int m, int d);  // days since 1970-01-01
Ymd civilFromDays(int32_t days);
int weekdayFromDays(int32_t days);  // 0 = Sunday .. 6 = Saturday
bool isLeapYear(int y);
int daysInMonth(int y, int m);
int64_t floorDiv(int64_t a, int64_t b);
int64_t floorMod(int64_t a, int64_t b);

}  // namespace core
