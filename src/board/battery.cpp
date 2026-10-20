#include "board/battery.h"

#include <Arduino.h>
#include <Wire.h>

#include "board/board.h"
#include "board/bus_locks.h"
#include "board/sensor_bus.h"
#include "core/selftest_logic.h"

namespace board {
namespace {

bool readWord(uint8_t reg, uint16_t& out) {
  Wire1.beginTransmission(static_cast<uint8_t>(GAUGE_I2C_ADDR));
  Wire1.write(reg);
  if (Wire1.endTransmission(false) != 0) return false;
  if (Wire1.requestFrom(static_cast<int>(GAUGE_I2C_ADDR), 2) != 2) return false;
  const int lo = Wire1.read();
  const int hi = Wire1.read();
  if (lo < 0 || hi < 0) return false;
  out = static_cast<uint16_t>(lo | (hi << 8));
  return true;
}

}  // namespace

BatteryReading readBattery() {
  BatteryReading r;
  {
  BusLock lock(i2cMutex());
  uint16_t v = 0;
  if (sensorBusBegin() && readWord(0x2C, v)) {
    r.gaugeOk = true;
    r.socPercent = v > 100 ? 100 : static_cast<int>(v);
    if (readWord(0x08, v)) r.millivolts = v;
    if (readWord(0x0C, v)) r.currentMa = static_cast<int16_t>(v);
    if (readWord(0x06, v)) r.tempDeciC = core::bq27220TempToDeciC(v);
  }
  }
  r.externalPower = externalPowerPresent();
  r.chargeStateRaw = chargeStateRaw();
  return r;
}

}  // namespace board
