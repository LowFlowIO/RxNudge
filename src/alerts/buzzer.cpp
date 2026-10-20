#include "alerts/buzzer.h"

#include <Arduino.h>
#include <driver/ledc.h>

#include "board/board_config.h"
#include "board/log.h"

namespace alerts {
namespace {

constexpr ledc_mode_t kMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t kTimer = LEDC_TIMER_3;
constexpr ledc_channel_t kChannel = LEDC_CHANNEL_7;
bool s_ready = false;

}  // namespace

bool buzzerCompiledIn() { return MT_BUZZER_ENABLED != 0; }

bool buzzerInit() {
  if (!buzzerCompiledIn()) return false;
  if (s_ready) return true;
  ledc_timer_config_t t = {};
  t.speed_mode = kMode;
  t.timer_num = kTimer;
  t.duty_resolution = LEDC_TIMER_10_BIT;
  t.freq_hz = MT_BUZZER_DEFAULT_HZ;
  t.clk_cfg = LEDC_AUTO_CLK;
  if (ledc_timer_config(&t) != ESP_OK) {
    MT_LOGF("buzzer: timer config failed");
    return false;
  }
  ledc_channel_config_t c = {};
  c.gpio_num = MT_PIN_BUZZER;
  c.speed_mode = kMode;
  c.channel = kChannel;
  c.intr_type = LEDC_INTR_DISABLE;
  c.timer_sel = kTimer;
  c.duty = 0;
  c.hpoint = 0;
  if (ledc_channel_config(&c) != ESP_OK) {
    MT_LOGF("buzzer: channel config failed");
    return false;
  }
  s_ready = true;
  return true;
}

void buzzerOff() {
  if (!s_ready) return;
  ledc_set_duty(kMode, kChannel, 0);
  ledc_update_duty(kMode, kChannel);
}

void buzzerTone(uint32_t hz, uint32_t ms) { buzzerToneDuty(hz, ms, MT_BUZZER_DUTY_10BIT); }

void buzzerToneDuty(uint32_t hz, uint32_t ms, uint16_t duty) {
  if (!buzzerInit()) return;
  if (ledc_set_freq(kMode, kTimer, hz) != ESP_OK) {
    MT_LOGF("buzzer: %lu Hz rejected", static_cast<unsigned long>(hz));
    return;
  }
  if (duty > 512) duty = 512;
  ledc_set_duty(kMode, kChannel, duty);
  ledc_update_duty(kMode, kChannel);
  delay(ms);
  buzzerOff();
}

}  // namespace alerts
