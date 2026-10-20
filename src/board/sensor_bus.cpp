#include "board/sensor_bus.h"

#include <Wire.h>

#include "board/board.h"

namespace board {
namespace {
bool s_ready = false;
}
bool sensorBusBegin() {
  if (!s_ready) s_ready = Wire1.begin(GAUGE_SDA, GAUGE_SCL, 400000);
  return s_ready;
}
}  // namespace board
