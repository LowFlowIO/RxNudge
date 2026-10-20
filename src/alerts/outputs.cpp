#include "alerts/outputs.h"

#include <Arduino.h>

#include "alerts/alert.h"
#include "alerts/buzzer.h"

namespace alerts {

bool BuzzerOutput::available() const { return alertMode() == AlertMode::AudibleAndVisual; }

void BuzzerOutput::play(const core::PersonAlert& a) {
  const core::AlertPattern& p = core::patternFor(a.pattern);
  const uint16_t duty = core::volumeToDuty(a.volume);
  // Later ladder steps and critical doses play the pattern once more (escalation).
  const int extra = (a.critical ? 1 : 0) + (a.alarmIndex >= 3 ? 1 : 0);
  for (int r = 0; r < p.repeats + extra; r++) {
    for (int i = 0; i < p.count; i++) {
      const core::ToneStep& s = p.steps[i];
      if (s.hz && duty) buzzerToneDuty(s.hz, s.onMs, duty);
      else delay(s.onMs);
      delay(s.offMs);
    }
  }
  buzzerOff();
}

void ScreenFlashOutput::play(const core::PersonAlert& a) {
  // one inversion flash; two for critical doses and the battery alert
  const int n = a.critical ? 2 : 1;
  int cur = pending_.load();
  while (!pending_.compare_exchange_weak(cur, cur > n ? cur : n)) {
  }
}

}  // namespace alerts
