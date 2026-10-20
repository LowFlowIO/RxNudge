#pragma once
// Shared-bus mutexes (FreeRTOS). Lock order, always: db -> spi -> i2c (see ARCHITECTURE.md).
//  spi: SSD1677 display and SD card share one SPI bus.
//  i2c: Wire1 (sensor bus, GPIO1/0) carries the BQ27220 gauge and the PCF8563 RTC.
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace board {

void busLocksInit();  // idempotent; call before any task starts
SemaphoreHandle_t spiMutex();
SemaphoreHandle_t i2cMutex();

class BusLock {
 public:
  explicit BusLock(SemaphoreHandle_t m) : m_(m) {
    if (m_) xSemaphoreTakeRecursive(m_, portMAX_DELAY);
  }
  ~BusLock() {
    if (m_) xSemaphoreGiveRecursive(m_);
  }
  BusLock(const BusLock&) = delete;
  BusLock& operator=(const BusLock&) = delete;

 private:
  SemaphoreHandle_t m_;
};

}  // namespace board
