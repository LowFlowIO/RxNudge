#pragma once
// PCF8563 RTC at 0x51 on Wire1 (the Seeed peripheral demo: I2C_NUM_1, SDA 1 / SCL 0,
// PCF8563_I2C_ADDR 0x51). Stores UTC. Takes the i2c bus lock itself.
#include <cstdint>

namespace board {

constexpr uint8_t kPcf8563Addr = 0x51;

struct RtcRead {
  bool ok = false;          // device answered and fields are valid
  bool voltageLow = false;  // VL flag: integrity not guaranteed
  int64_t utc = 0;
};

RtcRead rtcRead();
bool rtcWrite(int64_t utc);  // also clears VL and stops no alarms/timers (CLKOUT/alarm untouched)

}  // namespace board
