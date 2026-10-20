#pragma once
#include <cstdint>

namespace board {

struct BatteryReading {
  bool gaugeOk = false;       // BQ27220 answered on I2C
  int socPercent = -1;        // RelativeStateOfCharge 0x2C
  int millivolts = -1;        // Voltage 0x08
  int currentMa = 0;          // Current 0x0C (signed, + = charging)
  int tempDeciC = 0;          // Temperature 0x06 (0.1 K -> 0.1 C)
  bool externalPower = false; // GPIO9 HIGH [SEEED]
  int chargeStateRaw = -1;    // GPIO40 raw, diagnostic only (ASSUMPTION)
};

// Reads the BQ27220 on Wire1 (SDA GPIO1, SCL GPIO0, 0x55), the same bus and
// address as the reference Bq27220.h.
BatteryReading readBattery();

}  // namespace board
