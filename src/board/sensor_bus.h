#pragma once
// Wire1 = sensor I2C bus (SDA GPIO1, SCL GPIO0), shared by the BQ27220 (0x55) and
// the PCF8563 RTC (0x51) as in the Seeed peripheral demo (I2C_NUM_1). Callers hold board::i2cMutex().
namespace board {
bool sensorBusBegin();  // idempotent
}
