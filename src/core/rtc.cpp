#include "core/rtc.h"

#include "core/civil.h"

namespace core {

static uint8_t bcd2dec(uint8_t v) { return static_cast<uint8_t>((v >> 4) * 10 + (v & 0x0F)); }
static uint8_t dec2bcd(int v) { return static_cast<uint8_t>(((v / 10) << 4) | (v % 10)); }
static bool bcdValid(uint8_t v) { return (v & 0x0F) <= 9 && (v >> 4) <= 9; }

bool decodePcf8563(const uint8_t raw[7], int64_t& utc, bool& voltageLow) {
  voltageLow = (raw[0] & 0x80) != 0;
  const uint8_t s = raw[0] & 0x7F, mi = raw[1] & 0x7F, h = raw[2] & 0x3F, d = raw[3] & 0x3F, mo = raw[5] & 0x1F, y = raw[6];
  if (!bcdValid(s) || !bcdValid(mi) || !bcdValid(h) || !bcdValid(d) || !bcdValid(mo) || !bcdValid(y)) return false;
  const int sec = bcd2dec(s), min = bcd2dec(mi), hour = bcd2dec(h), day = bcd2dec(d), mon = bcd2dec(mo);
  const int year = ((raw[5] & 0x80) ? 1900 : 2000) + bcd2dec(y);
  if (sec > 59 || min > 59 || hour > 23 || mon < 1 || mon > 12 || day < 1 || day > daysInMonth(year, mon)) return false;
  utc = static_cast<int64_t>(daysFromCivil(year, mon, day)) * 86400 + hour * 3600 + min * 60 + sec;
  return true;
}

void encodePcf8563(int64_t utc, uint8_t raw[7]) {
  const int32_t days = static_cast<int32_t>(floorDiv(utc, 86400));
  const int64_t sod = floorMod(utc, 86400);
  const Ymd ymd = civilFromDays(days);
  raw[0] = dec2bcd(static_cast<int>(sod % 60));
  raw[1] = dec2bcd(static_cast<int>((sod / 60) % 60));
  raw[2] = dec2bcd(static_cast<int>(sod / 3600));
  raw[3] = dec2bcd(ymd.d);
  raw[4] = static_cast<uint8_t>(weekdayFromDays(days));
  raw[5] = static_cast<uint8_t>(dec2bcd(ymd.m) | (ymd.y < 2000 ? 0x80 : 0x00));
  raw[6] = dec2bcd(ymd.y % 100);
}

const char* clockTrustName(ClockTrust t) {
  switch (t) {
    case ClockTrust::Unset: return "unset";
    case ClockTrust::Untrusted: return "untrusted";
    case ClockTrust::Trusted: return "trusted";
  }
  return "?";
}

ClockDecision evaluateClock(bool rtcOk, bool vl, int64_t rtcUtc, int64_t sysUtc) {
  ClockDecision d;
  if (rtcOk && !vl && rtcUtc >= kMinPlausibleUtc) {
    d.trust = ClockTrust::Trusted;
    d.utc = rtcUtc;
    d.setSystemTime = true;
  } else if (sysUtc >= kMinPlausibleUtc) {
    d.trust = ClockTrust::Untrusted;
    d.utc = sysUtc;
  } else if (rtcOk && rtcUtc >= kMinPlausibleUtc) {
    // RTC lost power at some point but still holds a plausible time: usable, not trusted
    d.trust = ClockTrust::Untrusted;
    d.utc = rtcUtc;
    d.setSystemTime = true;
  }
  return d;
}

}  // namespace core
