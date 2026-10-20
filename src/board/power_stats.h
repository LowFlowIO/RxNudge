#pragma once
// Wake/sleep counters: kept in RTC slow memory across deep sleep and mirrored to
// NVS ("power"/"counters") before every sleep, so they also survive resets and battery swaps.
#include "core/power.h"

namespace board {

core::PowerCounters& powerCounters();  // loads from RTC memory, else NVS, else fresh
void powerCountersSave();               // NVS mirror
void powerCountersReset();
// Target of the last timer sleep (to detect early wakes); 0 = none.
int64_t lastSleepTargetUtc();
void setLastSleepTargetUtc(int64_t t);

}  // namespace board
