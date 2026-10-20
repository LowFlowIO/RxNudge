#include "app_core/app_port.h"

#include "core/civil.h"

namespace app {

bool localToUtcChecked(const core::PosixTz& tz, int y, int mo, int d, int h, int mi, int64_t& utc) {
  if (y < 2025 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > core::daysInMonth(y, mo) || h < 0 || h > 23 || mi < 0 || mi > 59)
    return false;
  utc = core::localToUtc(tz, core::daysFromCivil(y, mo, d), h * 3600 + mi * 60);
  return true;
}

bool ModelPort::setClockLocal(int y, int mo, int d, int h, int mi) {
  int64_t utc;
  if (!localToUtcChecked(m_.tz(), y, mo, d, h, mi, utc)) return false;
  if (setter_ && !setter_(ctx_, utc)) return false;
  m_.status.clockUnset = false;
  m_.status.clockUntrusted = false;
  if (!m_.engineRunning()) m_.startEngine(utc);
  return true;
}

}  // namespace app
