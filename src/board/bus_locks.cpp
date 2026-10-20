#include "board/bus_locks.h"

namespace board {
namespace {
SemaphoreHandle_t s_spi = nullptr;
SemaphoreHandle_t s_i2c = nullptr;
}  // namespace

void busLocksInit() {
  if (!s_spi) s_spi = xSemaphoreCreateRecursiveMutex();
  if (!s_i2c) s_i2c = xSemaphoreCreateRecursiveMutex();
}
SemaphoreHandle_t spiMutex() { return s_spi; }
SemaphoreHandle_t i2cMutex() { return s_i2c; }

}  // namespace board
