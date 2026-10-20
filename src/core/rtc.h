#pragma once
// PCF8563 register codec and clock-trust evaluation. Pure (no I2C here).
// The RTC holds UTC. Registers 0x02..0x08: sec(VL bit7), min, hour, day, weekday, month(century bit7), year.
#include <cstdint>

namespace core {

constexpr int64_t kMinPlausibleUtc = 1735689600;  // 2025-01-01T00:00:00Z

// Returns false for impossible field values. voltageLow = VL flag (clock integrity not guaranteed).
bool decodePcf8563(const uint8_t raw[7], int64_t& utc, bool& voltageLow);
void encodePcf8563(int64_t utc, uint8_t raw[7]);  // VL cleared, century bit set for years < 2000

enum class ClockTrust : uint8_t {
  Unset,      // no usable time: schedules must not run, the user must set the clock
  Untrusted,  // usable but unverified (RTC unreadable or lost power, system time survived deep sleep)
  Trusted     // RTC read OK with integrity flag clear, or set by the user
};
const char* clockTrustName(ClockTrust t);

struct ClockDecision {
  ClockTrust trust = ClockTrust::Unset;
  int64_t utc = 0;          // time to use (0 when Unset)
  bool setSystemTime = false;  // copy `utc` into the system clock
};
ClockDecision evaluateClock(bool rtcReadOk, bool rtcVoltageLow, int64_t rtcUtc, int64_t systemUtc);

}  // namespace core
