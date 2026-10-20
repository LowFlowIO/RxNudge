#include "alerts/alert.h"

#include <Preferences.h>

#include "alerts/buzzer.h"
#include "board/board.h"
#include "board/log.h"

namespace alerts {
namespace {

bool s_buzzerOk = true;
bool s_tested = false;

}  // namespace

void alertInit() {
  Preferences p;
  if (p.begin("alerts", true)) {
    s_tested = p.isKey("buzzer_ok");
    s_buzzerOk = p.getBool("buzzer_ok", true);
    p.end();
  }
  if (buzzerCompiledIn()) buzzerInit();
  MT_LOGF("alerts: mode=%s (tested=%d)", alertMode() == AlertMode::AudibleAndVisual ? "audible+visual" : "visual-only",
          s_tested);
}

AlertMode alertMode() {
  return (buzzerCompiledIn() && s_buzzerOk) ? AlertMode::AudibleAndVisual : AlertMode::VisualOnly;
}

void setBuzzerConfirmed(bool heard) {
  s_buzzerOk = heard;
  s_tested = true;
  Preferences p;
  if (p.begin("alerts", false)) {
    p.putBool("buzzer_ok", heard);
    p.end();
  }
}

bool buzzerTested() { return s_tested; }

void visualFlash(int flashes) {
  // Inverting twice restores the original frame bit-exactly, so no copy is needed.
  GfxRenderer& g = board::gfx();
  for (int i = 0; i < flashes; i++) {
    g.invertScreen();
    g.displayBuffer(HalDisplay::FULL_REFRESH);
    g.invertScreen();
    g.displayBuffer(HalDisplay::FULL_REFRESH);
  }
}

void raiseAlert() {
  if (alertMode() == AlertMode::AudibleAndVisual) {
    for (int i = 0; i < 3; i++) {
      buzzerTone(MT_BUZZER_DEFAULT_HZ, 150);
      delay(120);
    }
  }
  visualFlash(1);
}

}  // namespace alerts
