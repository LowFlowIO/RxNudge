#include "board/board.h"

#include <builtinFonts/notosans_14_bold.h>
#include <builtinFonts/notosans_14_regular.h>
#include <builtinFonts/notosans_18_bold.h>
#include <builtinFonts/ubuntu_10_regular.h>
#include <driver/gpio.h>
#include <esp_sleep.h>

#include "board/log.h"

namespace board {
namespace {

HalDisplay s_display;
GfxRenderer s_gfx(s_display);
HalGPIO s_io;

EpdFont s_title(&notosans_18_bold);
EpdFontFamily s_titleFamily(&s_title);
EpdFont s_body(&notosans_14_regular);
EpdFont s_bodyBold(&notosans_14_bold);
EpdFontFamily s_bodyFamily(&s_body, &s_bodyBold);
EpdFont s_small(&ubuntu_10_regular);
EpdFontFamily s_smallFamily(&s_small);

// Pins that sleep_wake.cpp may have latched with gpio_hold_en before deep sleep.
constexpr gpio_num_t kHeldPins[] = {
    static_cast<gpio_num_t>(PWR_HOLD),    static_cast<gpio_num_t>(PWR_LOCK),
    static_cast<gpio_num_t>(BTN_PIN_POWER), static_cast<gpio_num_t>(EPD_PWR_EN),
    static_cast<gpio_num_t>(TOUCH_EN),    static_cast<gpio_num_t>(TOUCH_RST),
    static_cast<gpio_num_t>(MT_PIN_MIC_EN), static_cast<gpio_num_t>(SD_PWR_EN),
    static_cast<gpio_num_t>(MT_PIN_BUZZER), static_cast<gpio_num_t>(BTN_PIN_UP),
    static_cast<gpio_num_t>(BTN_PIN_DOWN),  static_cast<gpio_num_t>(TOUCH_INT),
};

void releaseSleepHolds() {
  if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_UNDEFINED) {
    // [SEEED] board_init(): preload HIGH before releasing the holds so the
    // board never drops its own power latch.
    gpio_set_direction(static_cast<gpio_num_t>(PWR_HOLD), GPIO_MODE_OUTPUT);
    gpio_set_level(static_cast<gpio_num_t>(PWR_HOLD), 1);
    gpio_set_direction(static_cast<gpio_num_t>(PWR_LOCK), GPIO_MODE_OUTPUT);
    gpio_set_level(static_cast<gpio_num_t>(PWR_LOCK), 1);
  }
  gpio_deep_sleep_hold_dis();
  for (gpio_num_t pin : kHeldPins) gpio_hold_dis(pin);
}

}  // namespace

HalDisplay& display() { return s_display; }
GfxRenderer& gfx() { return s_gfx; }
HalGPIO& io() { return s_io; }

void begin() {
  releaseSleepHolds();
  setCpuFrequencyMhz(80);
  s_io.begin();
  s_display.begin();

  s_gfx.setFadingFix(true);
  s_gfx.insertFont(FONT_TITLE, s_titleFamily);
  s_gfx.insertFont(FONT_BODY, s_bodyFamily);
  s_gfx.insertFont(FONT_SMALL, s_smallFamily);
  // Orientation 3 = LandscapeCounterClockwise = native 800x480 panel orientation;
  // the same index drives InputManager::panelToLogical (default branch, identity).
  s_gfx.setOrientation(GfxRenderer::LandscapeCounterClockwise);
  s_io.setUiOrientation(3);

  pinMode(MT_PIN_EXT_POWER, INPUT);
  pinMode(MT_PIN_SD_DETECT, INPUT_PULLUP);
  MT_LOGF("board: up, PSRAM %u bytes, ext power %d, GPIO40 %d, SD detect %d",
          static_cast<unsigned>(ESP.getPsramSize()), externalPowerPresent(), chargeStateRaw(),
          sdCardDetected());
}

void firstFullRefresh() {
  s_gfx.clearScreen();
  s_gfx.displayBuffer(HalDisplay::FULL_REFRESH);
}

bool externalPowerPresent() { return digitalRead(MT_PIN_EXT_POWER) == HIGH; }

int chargeStateRaw() { return digitalRead(MT_PIN_CHARGE_STATE_RAW); }

bool sdCardDetected() { return digitalRead(MT_PIN_SD_DETECT) == MT_SD_DETECT_ACTIVE_LEVEL; }

}  // namespace board
