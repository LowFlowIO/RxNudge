#include "board/power_stats.h"

#include <Arduino.h>
#include <Preferences.h>

namespace board {
namespace {
RTC_DATA_ATTR core::PowerCounters s_rtc;
RTC_DATA_ATTR int64_t s_target = 0;
bool s_loaded = false;
}  // namespace

core::PowerCounters& powerCounters() {
  if (!s_loaded) {
    s_loaded = true;
    if (!s_rtc.valid()) {
      Preferences p;
      core::PowerCounters nv;
      if (p.begin("power", true)) {
        if (p.getBytesLength("counters") == sizeof(nv)) p.getBytes("counters", &nv, sizeof(nv));
        p.end();
      }
      if (nv.valid()) s_rtc = nv;
      else s_rtc.init();
    }
  }
  return s_rtc;
}

void powerCountersSave() {
  Preferences p;
  if (p.begin("power", false)) {
    p.putBytes("counters", &powerCounters(), sizeof(core::PowerCounters));
    p.end();
  }
}

void powerCountersReset() {
  powerCounters().init();
  powerCountersSave();
}

int64_t lastSleepTargetUtc() { return s_target; }
void setLastSleepTargetUtc(int64_t t) { s_target = t; }

}  // namespace board
