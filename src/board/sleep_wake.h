#pragma once
#include <cstdint>

namespace board {

enum class WakeCause : uint8_t { PowerOn, Timer, PowerButton, Other };

#ifndef MT_TOUCH_WAKE
#define MT_TOUCH_WAKE 0
#endif

WakeCause wakeCause();
const char* wakeCauseName(WakeCause c);

// Self-test bookkeeping kept in RTC slow memory across deep sleep.
struct WakeTestRecord {
  bool pending;              // a self-test sleep was started
  uint32_t expectedSeconds;  // requested timer
  int64_t sleptAtSec;        // gettimeofday() seconds when sleep started
};
// Returns the record and clears it, so it is reported once only.
WakeTestRecord takeWakeTestRecord();
// Seconds of system time (the RTC timer keeps counting during deep sleep).
int64_t nowSeconds();

// Shuts down the display and peripherals, latches the power rails, arms
// GPIO4 (AI/power button, active low) and, if timerSeconds > 0, the RTC
// timer, then enters deep sleep. Never returns.
[[noreturn]] void enterDeepSleep(uint32_t timerSeconds, bool selfTest);

// Wake sources on the ESP32-S3: only RTC GPIOs 0..21 can wake from deep sleep.
//  - AI/OK GPIO4, UP GPIO5, DOWN GPIO6: RTC-capable, active low -> EXT1 any-low.
//  - GT911 INT GPIO21: RTC-capable, but the touch controller is unpowered in sleep
//    (TOUCH_EN held low, as Seeed's sleep code does). Touch wake therefore needs
//    -DMT_TOUCH_WAKE=1, which keeps the GT911 powered (extra current, unmeasured) and
//    adds GPIO21 to the any-low mask.
//  - USB present GPIO9: RTC-capable -> EXT0, armed for the opposite of the current level.
//  - PCF8563 INT: not routed to any GPIO in Seeed's pin map, so the RTC cannot wake us;
//    the ESP32 RTC timer is used and the PCF8563 corrects its drift at every wake.
struct SleepConfig {
  uint32_t timerSeconds = 0;  // 0 = no timer
  bool buttons = true;        // AI only when false is not allowed: AI is always armed
  bool upDown = true;         // UP/DOWN also wake
  bool touch = false;         // honoured only with MT_TOUCH_WAKE
  bool usbEdge = true;        // wake when USB power is plugged in / removed
  bool selfTest = false;
};
[[noreturn]] void enterDeepSleepEx(const SleepConfig& c);

// Detailed wake source of this boot (EXT1 status decoded per pin).
enum class WakePin : uint8_t { None, Ai, Up, Down, Touch, Usb, Timer };
WakePin wakePin();
const char* wakePinName(WakePin p);

}  // namespace board
