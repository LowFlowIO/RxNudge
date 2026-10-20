#pragma once
// English date/time formatting for the UI (24-hour clock).
#include <cstdint>
#include <string>

namespace app {

std::string fmtHm(int minuteOfDay);                // "08:05"
std::string fmtLongDate(int32_t day);              // "Sunday 18 October"
std::string fmtShortDate(int32_t day);             // "Sun 18 Oct"
std::string fmtMonthYear(int32_t day);             // "October 2026"
const char* weekdayShort(int wd);                  // 0 = Sunday
const char* monthShort(int m);                     // 1..12
std::string fmt(const char* format, ...);          // printf into std::string

}  // namespace app
