#pragma once
// Phase 3 firmware runtime: clock bring-up, AppModel, device IAppPort and the
// FreeRTOS tasks (input, ui, engine, storage, alerts). See ARCHITECTURE.md.
#include "board/sleep_wake.h"

#ifndef MT_DEMO_DATA
#define MT_DEMO_DATA 0
#endif
#ifndef MT_IDLE_SLEEP_MS
#define MT_IDLE_SLEEP_MS (5UL * 60UL * 1000UL)  // battery only, home screen, nothing due
#endif

#ifndef MT_TIMER_WAKE_IDLE_MS
#define MT_TIMER_WAKE_IDLE_MS (20UL * 1000UL)  // after a timer wake with no input: back to sleep quickly
#endif

namespace app {

// Call from setup() after board::begin(), storageBegin(), alertInit() and the first FULL refresh.
void deviceAppStart(board::WakeCause cause);

}  // namespace app
