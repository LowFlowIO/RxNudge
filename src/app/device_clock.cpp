#include "app/device_clock.h"

#include <sys/time.h>

#include <ctime>

#include "board/log.h"
#include "board/rtc_pcf8563.h"

namespace app {
namespace {
void setSystem(int64_t utc) {
  timeval tv;
  tv.tv_sec = static_cast<time_t>(utc);
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
}
}  // namespace

int64_t DeviceClock::Sys::nowUtc() const { return static_cast<int64_t>(time(nullptr)); }

DeviceClock::DeviceClock() : scaled_(sys_, MT_DEV_CLOCK_SPEED, 0) {}

core::ClockTrust DeviceClock::begin() {
  const board::RtcRead r = board::rtcRead();
  const int64_t sysBefore = sys_.nowUtc();
  if (r.ok && !r.voltageLow && sysBefore >= core::kMinPlausibleUtc) drift_ = static_cast<int32_t>(sysBefore - r.utc);
  const core::ClockDecision d = core::evaluateClock(r.ok, r.voltageLow, r.utc, sysBefore);
  if (d.setSystemTime) setSystem(d.utc);
  trust_ = d.trust;
  scaled_.rebase(sys_.nowUtc());
  MT_LOGF("clock: rtc ok=%d vl=%d trust=%s speed=%d utc=%lld drift=%ld s", r.ok, r.voltageLow, core::clockTrustName(trust_),
          speed(), static_cast<long long>(sys_.nowUtc()), static_cast<long>(drift_));
  return trust_;
}

bool DeviceClock::set(int64_t utc) {
  setSystem(utc);
  scaled_.rebase(utc);
  const bool ok = board::rtcWrite(utc);
  trust_ = core::ClockTrust::Trusted;  // the user vouched for it; RTC failure is reported separately
  MT_LOGF("clock: set by user, rtc write %s", ok ? "ok" : "FAILED");
  return ok;
}

}  // namespace app
