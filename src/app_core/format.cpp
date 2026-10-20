#include "app_core/format.h"

#include <cstdarg>
#include <cstdio>

#include "core/civil.h"

namespace app {

static const char* const kWd[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char* const kWdS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char* const kMo[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
static const char* const kMoS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

std::string fmt(const char* f, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, f);
  vsnprintf(buf, sizeof(buf), f, ap);
  va_end(ap);
  return buf;
}

std::string fmtHm(int m) {
  if (m < 0) m = 0;
  return fmt("%02d:%02d", (m / 60) % 24, m % 60);
}

std::string fmtLongDate(int32_t day) {
  const core::Ymd d = core::civilFromDays(day);
  return fmt("%s %d %s", kWd[core::weekdayFromDays(day)], d.d, kMo[d.m - 1]);
}

std::string fmtShortDate(int32_t day) {
  const core::Ymd d = core::civilFromDays(day);
  return fmt("%s %d %s", kWdS[core::weekdayFromDays(day)], d.d, kMoS[d.m - 1]);
}

std::string fmtMonthYear(int32_t day) {
  const core::Ymd d = core::civilFromDays(day);
  return fmt("%s %d", kMo[d.m - 1], d.y);
}

const char* weekdayShort(int wd) { return kWdS[((wd % 7) + 7) % 7]; }
const char* monthShort(int m) { return kMoS[(m - 1 + 12) % 12]; }

}  // namespace app
