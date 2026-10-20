#include "board/sleep_wake.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <sys/time.h>

#include "alerts/buzzer.h"
#include "board/board.h"
#include "board/log.h"
#include "core/selftest_logic.h"

namespace board {
namespace {

constexpr uint32_t kMagic = 0x4D454454u;  // "MEDT"
RTC_DATA_ATTR uint32_t s_magic = 0;
RTC_DATA_ATTR uint32_t s_expectedSeconds = 0;
RTC_DATA_ATTR int64_t s_sleptAtSec = 0;

void holdOutput(int pin, int level) {
  const gpio_num_t g = static_cast<gpio_num_t>(pin);
  gpio_hold_dis(g);
  gpio_set_direction(g, GPIO_MODE_OUTPUT);
  gpio_set_level(g, level);
  gpio_hold_en(g);
}

}  // namespace

WakeCause wakeCause() {
  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_UNDEFINED: return WakeCause::PowerOn;
    case ESP_SLEEP_WAKEUP_TIMER: return WakeCause::Timer;
    case ESP_SLEEP_WAKEUP_EXT0:
    case ESP_SLEEP_WAKEUP_EXT1:
    case ESP_SLEEP_WAKEUP_GPIO: return WakeCause::PowerButton;
    default: return WakeCause::Other;
  }
}

const char* wakeCauseName(WakeCause c) {
  switch (c) {
    case WakeCause::PowerOn: return "power-on/reset";
    case WakeCause::Timer: return "RTC timer";
    case WakeCause::PowerButton: return "AI/power button";
    default: return "other";
  }
}

int64_t nowSeconds() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return static_cast<int64_t>(tv.tv_sec);
}

WakeTestRecord takeWakeTestRecord() {
  WakeTestRecord r{false, 0, 0};
  if (s_magic == kMagic) {
    r.pending = true;
    r.expectedSeconds = s_expectedSeconds;
    r.sleptAtSec = s_sleptAtSec;
  }
  s_magic = 0;
  return r;
}

WakePin wakePin() {
  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_TIMER: return WakePin::Timer;
    case ESP_SLEEP_WAKEUP_EXT0: return WakePin::Usb;
    case ESP_SLEEP_WAKEUP_EXT1: {
      const uint64_t m = esp_sleep_get_ext1_wakeup_status();
      if (m & (1ULL << BTN_PIN_POWER)) return WakePin::Ai;
      if (m & (1ULL << BTN_PIN_UP)) return WakePin::Up;
      if (m & (1ULL << BTN_PIN_DOWN)) return WakePin::Down;
      if (m & (1ULL << TOUCH_INT)) return WakePin::Touch;
      return WakePin::Ai;
    }
    default: return WakePin::None;
  }
}

const char* wakePinName(WakePin p) {
  static const char* const n[] = {"none", "ai", "up", "down", "touch", "usb", "timer"};
  return n[static_cast<int>(p)];
}

void enterDeepSleep(uint32_t timerSeconds, bool selfTest) {
  SleepConfig c;
  c.timerSeconds = timerSeconds;
  c.selfTest = selfTest;
  c.upDown = false;
  c.usbEdge = false;
  enterDeepSleepEx(c);
}

void enterDeepSleepEx(const SleepConfig& cfg) {
  const uint32_t timerSeconds = cfg.timerSeconds;
  const bool selfTest = cfg.selfTest;
  const bool touchWake = MT_TOUCH_WAKE && cfg.touch;
  // [REF] HalGPIO::startDeepSleep: wait for the power button release, or the
  // still-low GPIO4 would wake the chip immediately.
  while (io().isPressed(HalGPIO::BTN_POWER) || digitalRead(BTN_PIN_POWER) == LOW ||
         (cfg.upDown && (digitalRead(BTN_PIN_UP) == LOW || digitalRead(BTN_PIN_DOWN) == LOW))) {
    delay(20);
    io().update();
  }
  delay(60);  // [SEEED] kReleaseDebounceTime

  if (selfTest) {
    s_magic = kMagic;
    s_expectedSeconds = timerSeconds;
    s_sleptAtSec = nowSeconds();
  }

  alerts::buzzerOff();
  // [REF] display.deepSleep() waits for BUSY before DEEP_SLEEP 0x01.
  display().deepSleep();
  MT_LOGF("sleep: timer=%lus selfTest=%d upDown=%d touch=%d usb=%d", static_cast<unsigned long>(timerSeconds), selfTest,
          cfg.upDown, touchWake, cfg.usbEdge);
  Serial.flush();

  // [SEEED] sticky_power_enter_deep_sleep(): keep the board latched, park
  // peripheral power at their disabled levels.
  holdOutput(PWR_HOLD, 1);
  holdOutput(PWR_LOCK, 1);
  holdOutput(EPD_PWR_EN, 0);
  if (touchWake) {
    holdOutput(TOUCH_EN, 1);  // keep the GT911 powered so its INT can pull GPIO21 low
    holdOutput(TOUCH_RST, 1);
  } else {
    holdOutput(TOUCH_EN, 0);
    holdOutput(TOUCH_RST, 0);
  }
  holdOutput(MT_PIN_MIC_EN, 0);
  holdOutput(SD_PWR_EN, 0);
  holdOutput(MT_PIN_BUZZER, 0);

  uint64_t mask = 1ULL << BTN_PIN_POWER;
  if (cfg.upDown) mask |= (1ULL << BTN_PIN_UP) | (1ULL << BTN_PIN_DOWN);
  if (touchWake) mask |= 1ULL << TOUCH_INT;
  for (int pin = 0; pin < 22; pin++) {
    if (!(mask & (1ULL << pin))) continue;
    const gpio_num_t g = static_cast<gpio_num_t>(pin);
    gpio_hold_dis(g);
    gpio_set_direction(g, GPIO_MODE_INPUT);
    gpio_pullup_en(g);
    gpio_pulldown_dis(g);
    gpio_hold_en(g);
  }
  // [SEEED] EXT1, any-low on GPIO4 (plus UP/DOWN/touch INT when enabled).
  esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);
  if (cfg.usbEdge) {
    // GPIO9 HIGH = USB present [SEEED]; wake on the opposite level.
    const int level = digitalRead(MT_PIN_EXT_POWER) == HIGH ? 0 : 1;
    esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(MT_PIN_EXT_POWER), level);
  }
  if (timerSeconds > 0) {
    esp_sleep_enable_timer_wakeup(core::sleepMicros(timerSeconds));
  }
  gpio_deep_sleep_hold_en();
  delay(50);
  esp_deep_sleep_start();
  for (;;) {
  }
}

}  // namespace board
