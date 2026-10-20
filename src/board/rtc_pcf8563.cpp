#include "board/rtc_pcf8563.h"

#include <Wire.h>

#include "board/bus_locks.h"
#include "board/sensor_bus.h"
#include "core/rtc.h"

namespace board {

RtcRead rtcRead() {
  RtcRead r;
  BusLock lock(i2cMutex());
  if (!sensorBusBegin()) return r;
  Wire1.beginTransmission(kPcf8563Addr);
  Wire1.write(0x02);  // seconds register
  if (Wire1.endTransmission(false) != 0) return r;
  if (Wire1.requestFrom(static_cast<int>(kPcf8563Addr), 7) != 7) return r;
  uint8_t raw[7];
  for (uint8_t& b : raw) {
    const int v = Wire1.read();
    if (v < 0) return r;
    b = static_cast<uint8_t>(v);
  }
  r.ok = core::decodePcf8563(raw, r.utc, r.voltageLow);
  return r;
}

bool rtcWrite(int64_t utc) {
  uint8_t raw[7];
  core::encodePcf8563(utc, raw);
  BusLock lock(i2cMutex());
  if (!sensorBusBegin()) return false;
  Wire1.beginTransmission(kPcf8563Addr);
  Wire1.write(0x00);  // control/status 1: clock running (STOP=0), normal mode
  Wire1.write(0x00);
  if (Wire1.endTransmission() != 0) return false;
  Wire1.beginTransmission(kPcf8563Addr);
  Wire1.write(0x02);
  Wire1.write(raw, sizeof(raw));
  return Wire1.endTransmission() == 0;
}

}  // namespace board
