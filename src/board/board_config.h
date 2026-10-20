#pragma once
// RxNudge board configuration for Seeed reTerminal Sticky.
//
// Pin numbers come from two places only:
//   [REF]   lib/hal/board_pins.h, copied unchanged from LowFlowIO/sticky-micronotes
//   [SEEED] Seeed's official docs and demo code:
//           https://www.seeedstudio.com/sticky/docs/en/device-guide/hardware-overview/
//           https://files.seeedstudio.com/wiki/reterminal_sticky/res/Sticky_dashboard_demo.zip
//             (main/pin_config.h, devices/*.cpp)
// Anything not proven by those sources is tagged ASSUMPTION and must be
// checked with the hidden self-test on real hardware.

#include "board_pins.h"  // [REF] EPD_*, SD_*, BTN_*, PWR_*, GAUGE_*, TOUCH_*, BUZZER_PIN

// ---------------------------------------------------------------- display
// [SEEED] 3.97", 800 x 480, 235 ppi, 4-level gray, SSD1677 (Hardware Overview).
#define MT_DISPLAY_W 800
#define MT_DISPLAY_H 480

// ------------------------------------------------------------------ touch
// GT911 at TOUCH_I2C_ADDR, polled every MT_TOUCH_POLL_MS by the input task (no IRQ use).
// Read: 0x814E status (bit7 buffer ready, bits0..3 count), 0x814F track id, 0x8150 x, 0x8152 y;
// 0 is written back to 0x814E after every ready report (as Seeed's gt911.cpp does).
// Debounce/guard timings live in ui_core/theme.h so host tests use the same values:
// kTouchMinPressMs, kTouchMinUpMs, kTouchGuardMs, kTouchStaleReleaseMs, kTouchSlopPx.
#define MT_TOUCH_POLL_MS 10

// ------------------------------------------------- external power / charger
// [SEEED] pin_config.h: PIN_EXTERNAL_POWER 9 = "USB/external power detect, high when present".
#define MT_PIN_EXT_POWER 9
// [REF] board_pins.h CHARGE_STATE 40. Seeed's demo never uses GPIO40.
// ASSUMPTION: GPIO40 is a BQ25616 status signal; we only read and log it raw. It is not used for decisions.
#define MT_PIN_CHARGE_STATE_RAW CHARGE_STATE
// [SEEED]+[REF] EN_BAT_CHGn on GPIO39, active low (LOW = charging enabled).
#define MT_PIN_CHG_EN_N EN_BAT_CHG

// ---------------------------------------------------------------- microSD
// [SEEED] PIN_SD_DETECT 11, input with pull-up, card present == LOW (sticky_sdcard.cpp).
#define MT_PIN_SD_DETECT SD_DET
#define MT_SD_DETECT_ACTIVE_LEVEL 0

// ----------------------------------------------------------------- buzzer
// [SEEED] Buzzer "PWM: GPIO48". Seeed drives it with LEDC, 10-bit, 2400 Hz at
// duty 256 (dashboard) and 4000 Hz at duty 512 (peripheral demo); duty 0 = silent.
// ASSUMPTION: passive (externally driven) transducer behind a transistor, active-high.
// The variable-frequency LEDC drive in Seeed's code implies this, but no schematic is published.
#define MT_PIN_BUZZER BUZZER_PIN
#define MT_BUZZER_DEFAULT_HZ 2400
#define MT_BUZZER_DUTY_10BIT 256
// Compile-time master switch. Runtime fallback: if self-test records
// "not heard", alerts drop to visual-only (NVS key alerts/buzzer_ok).
#ifndef MT_BUZZER_ENABLED
#define MT_BUZZER_ENABLED 1
#endif

// ------------------------------------------------- other outputs / sensors
// [SEEED] PIN_MIC_EN 38. Held LOW in deep sleep (sticky_power.cpp).
#define MT_PIN_MIC_EN 38
// [SEEED] PCF8563 RTC at 0x51 on the sensor bus (GPIO0/GPIO1).
// ASSUMPTION: the PCF8563 INT/alarm output is not routed to an ESP32 wake pin
// (no pin documented). Phase 1 therefore wakes with the ESP32 RTC timer only.
#define MT_PCF8563_ADDR 0x51
// [SEEED] Charging indicator is a hardware red/green LED next to USB-C.
// ASSUMPTION: this LED is driven by the charger and is NOT firmware controllable.
// [SEEED] No vibration motor and no user LED are documented.

// ------------------------------------------------------------- power hold
// [SEEED] sticky_power.cpp: before deep sleep, PWR_HOLD/PWR_LOCK are latched HIGH with
// gpio_hold_en + gpio_deep_sleep_hold_en. board.cpp preloads them HIGH before
// releasing the holds after wake.

// ---------------------------------------------------------------- timing
#define MT_POWER_LONG_PRESS_MS 2000   // [SEEED] "Hold AI for about two seconds" -> deep sleep
#define MT_SELFTEST_COMBO_MS 3000     // Hidden self-test entry: hold UP+DOWN for 3 s
