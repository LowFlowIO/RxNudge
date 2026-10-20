// RxNudge - Phase 3 (RTOS runtime, clock, dose engine on device, full UI) for Seeed reTerminal Sticky.
#include <Arduino.h>

#include "alerts/alert.h"
#include "app/app_storage.h"
#include "app/device_app.h"
#include "board/board.h"
#include "board/bus_locks.h"
#include "board/log.h"
#include "board/sleep_wake.h"
#include "net/radio.h"
#include "ui/selftest.h"

void setup() {
  MT_LOG_BEGIN();
  const board::WakeCause cause = board::wakeCause();
  MT_LOGF("RxNudge " MT_FW_VERSION " boot, wake cause: %s", board::wakeCauseName(cause));
  board::busLocksInit();
  board::begin();  // power rails first, then GPIO/touch/SPI, then display (reference order)
  net::radiosOff();
  alerts::alertInit();
  app::storageBegin();        // LittleFS + SD, database load/migration/recovery
  board::firstFullRefresh();  // required before any FAST refresh
  ui::showWakeTestResult();   // only shows something after a self-test sleep
  app::deviceAppStart(cause);
}

void loop() {
  // All work happens in the FreeRTOS tasks started by deviceAppStart().
  vTaskDelete(nullptr);
}
