#pragma once
// Device time: system time (kept by the ESP32 RTC timer across deep sleep), seeded
// from the PCF8563 at boot, optionally accelerated for development
// (-DMT_DEV_CLOCK_SPEED=N, default 1 = real time).
#include "core/clock.h"
#include "core/rtc.h"

#ifndef MT_DEV_CLOCK_SPEED
#define MT_DEV_CLOCK_SPEED 1
#endif

namespace app {

class DeviceClock : public core::IClock {
 public:
  DeviceClock();
  int64_t nowUtc() const override { return scaled_.nowUtc(); }
  // Reads the RTC, evaluates trust and seeds the system time. Call once at boot.
  core::ClockTrust begin();
  // User-entered time: writes the RTC and the system time, becomes Trusted.
  bool set(int64_t utc);
  core::ClockTrust trust() const { return trust_; }
  // System time minus RTC time at begin() (ESP32 RC drift since the last sync); 0 if unknown.
  int32_t bootDriftSec() const { return drift_; }
  // Real (unscaled) seconds until the device clock reaches utc (deep-sleep timers).
  int64_t realSecondsUntil(int64_t utc) const { return scaled_.sourceSecondsUntil(utc); }
  int speed() const { return scaled_.speed(); }

 private:
  class Sys : public core::IClock {
   public:
    int64_t nowUtc() const override;
  } sys_;
  core::ScaledClock scaled_;
  core::ClockTrust trust_ = core::ClockTrust::Unset;
  int32_t drift_ = 0;
};

}  // namespace app
