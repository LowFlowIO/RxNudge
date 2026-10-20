#include "ui/selftest.h"

#include <Arduino.h>

#include <cstdio>
#include <cstring>

#include "alerts/alert.h"
#include "alerts/buzzer.h"
#include "board/battery.h"
#include "board/board.h"
#include "board/log.h"
#include "board/power_stats.h"
#include "board/sleep_wake.h"
#include "core/selftest_logic.h"
#include "storage/sd_selftest.h"
#include "ui/draw.h"
#include "ui/input.h"

namespace ui {
namespace {

enum Item { kDisplay, kTouch, kButtons, kAlerts, kSd, kBattery, kSleep, kPower, kSleepCurrent, kExit, kItemCount };
const char* const kNames[kItemCount] = {
    "1 Display",     "2 Touch",          "3 Buttons",       "4 Alerts",        "5 microSD",
    "6 Battery",     "7 Sleep wake 2m",  "8 Counters",      "9 Sleep current", "10 Exit",
};
enum class Result : uint8_t { NotRun, Pass, Fail };
Result s_results[kItemCount] = {};

// Two columns of five rows; 84 px = 9.1 mm on the 235 ppi panel (touch target >= 9 mm).
constexpr int kRowTop = 56;
constexpr int kRowH = 84;
constexpr int kRowsPerCol = 5;
constexpr int kColLeft = 16;
constexpr int kColW = (MT_DISPLAY_W - 32) / 2;

const char* resultText(Result r) {
  switch (r) {
    case Result::Pass: return "PASS";
    case Result::Fail: return "FAIL";
    default: return "--";
  }
}

void record(Item i, bool pass) {
  s_results[i] = pass ? Result::Pass : Result::Fail;
  MT_LOGF("selftest: %s -> %s", kNames[i], resultText(s_results[i]));
}

// Waits for OK (short) or a tap. Returns false on OK long press (abort) or timeout.
bool waitOk(uint32_t timeoutMs) {
  const uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    const InputEvent e = pollInput();
    if (e.ev == Ev::Ok || e.ev == Ev::Tap) return true;
    if (e.ev == Ev::OkLong) return false;
    delay(10);
  }
  return false;
}

void drawMenu(const core::MenuModel& m) {
  GfxRenderer& g = board::gfx();
  g.clearScreen();
  g.drawText(board::FONT_SMALL, 24, 12, "Hardware self-test   UP/DOWN move, AI = run, tap a row = run", true);
  g.drawLine(16, kRowTop - 6, MT_DISPLAY_W - 16, kRowTop - 6, 2, true);
  for (int i = 0; i < m.count(); i++) {
    const int x = m.columnLeft(i), y = m.rowTop(i);
    const bool sel = i == m.selected();
    if (sel) g.fillRect(x + 4, y + 2, kColW - 8, kRowH - 4, true);
    else g.drawRect(x + 4, y + 2, kColW - 8, kRowH - 4, true);
    g.drawText(board::FONT_BODY, x + 20, y + kRowH / 2 - 14, kNames[i], !sel);
    if (i != kExit) g.drawText(board::FONT_BODY, x + kColW - 100, y + kRowH / 2 - 14, resultText(s_results[i]), !sel);
  }
}

// ------------------------------------------------------------------ tests
void testDisplay() {
  GfxRenderer& g = board::gfx();
  bool ok = true;

  g.clearScreen(0x00);
  g.displayBuffer(HalDisplay::FULL_REFRESH);
  ok = ok && waitOk(60000);

  if (ok) {
    g.clearScreen(0xFF);
    g.displayBuffer(HalDisplay::FULL_REFRESH);
    ok = waitOk(60000);
  }
  if (ok) {
    uint8_t* fb = g.getFrameBuffer();
    for (int y = 0; y < MT_DISPLAY_H; y++) {
      for (int bx = 0; bx < MT_DISPLAY_W / 8; bx++) {
        fb[y * (MT_DISPLAY_W / 8) + bx] = (((y / 8) + bx) & 1) ? 0x00 : 0xFF;
      }
    }
    g.displayBuffer(HalDisplay::FULL_REFRESH);
    ok = waitOk(60000);
  }
  if (ok) {
    g.clearScreen();
    const Color bands[4] = {White, LightGray, DarkGray, Black};
    const int bw = (MT_DISPLAY_W - 40) / 4;
    for (int i = 0; i < 4; i++) g.fillRectDither(20 + i * bw, 120, bw, 200, bands[i]);
    g.drawRect(0, 0, MT_DISPLAY_W, MT_DISPLAY_H, 3, true);
    g.fillRect(0, 0, 24, 24, true);
    g.fillRect(MT_DISPLAY_W - 24, MT_DISPLAY_H - 24, 24, 24, true);
    g.drawText(board::FONT_TITLE, 40, 30, "Top-left marker = filled square", true);
    g.drawText(board::FONT_BODY, 40, 340, "Bands: white / light / dark / black (dithered)", true);
    g.drawText(board::FONT_BODY, 40, 380, "Frame must touch all four edges. AI = next", true);
    g.displayBuffer(HalDisplay::HALF_REFRESH);
    ok = waitOk(60000);
  }
  if (ok) {
    // FAST (partial) refresh: a bar walks across the screen.
    g.clearScreen();
    g.displayBuffer(HalDisplay::FULL_REFRESH);
    for (int step = 0; step < 6; step++) {
      g.clearScreen();
      g.fillRect(40 + step * 120, 180, 100, 120, true);
      char buf[48];
      snprintf(buf, sizeof(buf), "FAST refresh %d/6", step + 1);
      g.drawText(board::FONT_BODY, 40, 60, buf, true);
      g.displayBuffer(HalDisplay::FAST_REFRESH);
    }
  }
  flushInput();
  const bool looked = ok && askYesNo("Display pattern", "Were all patterns clean and complete?", 60000);
  record(kDisplay, looked);
}

void testTouch() {
  GfxRenderer& g = board::gfx();
  core::Rect t[5];
  core::cornerTargets(MT_DISPLAY_W, MT_DISPLAY_H, 90, 20, t);
  t[4] = {MT_DISPLAY_W / 2 - 45, MT_DISPLAY_H / 2 - 45, 90, 90};
  bool hit[5] = {};
  int lastX = -1, lastY = -1;
  bool first = true;
  const uint32_t start = millis();
  bool dirty = true;
  while (millis() - start < 90000) {
    if (dirty) {
      g.clearScreen();
      for (int i = 0; i < 5; i++) {
        if (hit[i]) g.fillRect(t[i].x, t[i].y, t[i].w, t[i].h, true);
        else g.drawRect(t[i].x, t[i].y, t[i].w, t[i].h, 3, true);
      }
      char buf[64];
      snprintf(buf, sizeof(buf), "Tap all 5 boxes. Last tap: %d,%d", lastX, lastY);
      g.drawText(board::FONT_BODY, 140, 130, buf, true);
      g.drawText(board::FONT_SMALL, 140, 330, "Hold AI 2 s to abort", true);
      g.displayBuffer(first ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
      first = false;
      dirty = false;
    }
    const InputEvent e = pollInput();
    if (e.ev == Ev::OkLong) break;
    if (e.ev == Ev::Tap) {
      lastX = e.x;
      lastY = e.y;
      MT_LOGF("touch: tap %d,%d", e.x, e.y);
      for (int i = 0; i < 5; i++)
        if (t[i].contains(e.x, e.y)) hit[i] = true;
      dirty = true;
      bool all = true;
      for (bool h : hit) all = all && h;
      if (all) break;
    }
    delay(10);
  }
  bool all = true;
  for (bool h : hit) all = all && h;
  record(kTouch, all);
}

void testButtons() {
  GfxRenderer& g = board::gfx();
  bool seen[3] = {};
  const char* names[3] = {"UP (GPIO5)", "DOWN (GPIO6)", "AI/POWER (GPIO4)"};
  bool first = true, dirty = true;
  const uint32_t start = millis();
  while (millis() - start < 60000) {
    if (dirty) {
      g.clearScreen();
      g.drawText(board::FONT_TITLE, 24, 16, "Press each button once", true);
      for (int i = 0; i < 3; i++) {
        const int y = 110 + i * 100;
        if (seen[i]) g.fillRect(40, y, 80, 80, true);
        else g.drawRect(40, y, 80, 80, 3, true);
        g.drawText(board::FONT_BODY, 150, y + 24, names[i], true);
      }
      g.displayBuffer(first ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
      first = false;
      dirty = false;
      if (seen[0] && seen[1] && seen[2]) break;
    }
    HalGPIO& io = board::io();
    io.update();
    if (io.wasPressed(HalGPIO::BTN_UP) && !seen[0]) seen[0] = dirty = true;
    if (io.wasPressed(HalGPIO::BTN_DOWN) && !seen[1]) seen[1] = dirty = true;
    if (io.wasPressed(HalGPIO::BTN_POWER) && !seen[2]) seen[2] = dirty = true;
    delay(10);
  }
  flushInput();
  record(kButtons, seen[0] && seen[1] && seen[2]);
}

void testAlerts() {
  bool heard = false;
  if (alerts::buzzerCompiledIn()) {
    beginFrame("Alert outputs: buzzer (GPIO48)");
    line(110, "Playing: 3 beeps at 2400 Hz, then 1000 / 2000 / 4000 Hz.");
    line(160, "Listen now.");
    small(230, "Outputs present: buzzer only. The charge LED is charger-driven.");
    small(260, "There is no vibration motor or user LED on this device.");
    present(HalDisplay::HALF_REFRESH);
    for (int i = 0; i < 3; i++) {
      alerts::buzzerTone(MT_BUZZER_DEFAULT_HZ, 120);
      delay(150);
    }
    const uint32_t tones[3] = {1000, 2000, 4000};
    for (uint32_t hz : tones) {
      alerts::buzzerTone(hz, 300);
      delay(200);
    }
    heard = askYesNo("Alert outputs: buzzer", "Did you hear the beeps and three tones?", 60000);
    alerts::setBuzzerConfirmed(heard);
  }
  beginFrame("Alert outputs: visual");
  line(140, "The screen will now invert twice (visual alert).");
  present(HalDisplay::HALF_REFRESH);
  delay(800);
  alerts::visualFlash(2);
  const bool seen = askYesNo("Alert outputs: visual", "Did the screen flash black twice?", 60000);
  beginFrame("Alert outputs: result");
  char buf[96];
  snprintf(buf, sizeof(buf), "Buzzer: %s", !alerts::buzzerCompiledIn() ? "disabled at build time" : (heard ? "heard" : "NOT heard"));
  line(110, buf);
  snprintf(buf, sizeof(buf), "Visual flash: %s", seen ? "seen" : "NOT seen");
  line(160, buf);
  snprintf(buf, sizeof(buf), "Alert mode now: %s",
           alerts::alertMode() == alerts::AlertMode::AudibleAndVisual ? "AUDIBLE + VISUAL" : "VISUAL ONLY");
  line(210, buf, true);
  footer("AI = back");
  present(HalDisplay::FAST_REFRESH);
  waitOk(30000);
  record(kAlerts, seen && (heard || !alerts::buzzerCompiledIn()));
}

void testSd() {
  beginFrame("microSD read/write");
  line(120, "Testing... (16 KiB write, read back, CRC32, delete)");
  present(HalDisplay::HALF_REFRESH);
  const storage::SdTestResult r = storage::runSdSelfTest();
  beginFrame("microSD read/write");
  char buf[96];
  snprintf(buf, sizeof(buf), "Card-detect GPIO11: %s", r.detectPin ? "card present (LOW)" : "no card (HIGH)");
  line(90, buf);
  snprintf(buf, sizeof(buf), "Mount (SdFat, CS GPIO8): %s", r.mounted ? "OK" : "FAILED");
  line(130, buf);
  snprintf(buf, sizeof(buf), "Write %lu B: %s in %lu ms", static_cast<unsigned long>(r.bytes), r.written ? "OK" : "FAILED",
           static_cast<unsigned long>(r.writeMs));
  line(170, buf);
  snprintf(buf, sizeof(buf), "Read back: %s in %lu ms", r.readBack ? "OK" : "FAILED", static_cast<unsigned long>(r.readMs));
  line(210, buf);
  snprintf(buf, sizeof(buf), "CRC32 %08lX match: %s", static_cast<unsigned long>(r.crc), r.crcMatch ? "YES" : "NO");
  line(250, buf);
  snprintf(buf, sizeof(buf), "Delete test file: %s", r.removed ? "OK" : "FAILED");
  line(290, buf);
  line(350, r.pass() ? "RESULT: PASS" : "RESULT: FAIL", true);
  footer("AI = back");
  present(HalDisplay::HALF_REFRESH);
  waitOk(60000);
  record(kSd, r.pass());
}

void testBattery() {
  bool pass = false;
  bool first = true;
  const uint32_t start = millis();
  uint32_t lastRead = 0;
  while (millis() - start < 120000) {
    if (first || millis() - lastRead >= 3000) {
      lastRead = millis();
      const board::BatteryReading b = board::readBattery();
      pass = b.gaugeOk && b.millivolts >= 3000 && b.millivolts <= 4500;
      beginFrame("Battery & power");
      char buf[96];
      snprintf(buf, sizeof(buf), "BQ27220 (0x55 on GPIO1/GPIO0): %s", b.gaugeOk ? "OK" : "NO RESPONSE");
      line(80, buf);
      snprintf(buf, sizeof(buf), "State of charge: %d %%   (%s)", b.socPercent,
               core::batteryHealthName(core::classifyBattery(b.socPercent, b.millivolts)));
      line(120, buf);
      snprintf(buf, sizeof(buf), "Voltage: %d mV    Current: %d mA", b.millivolts, b.currentMa);
      line(160, buf);
      snprintf(buf, sizeof(buf), "Gauge temperature: %d.%d C", b.tempDeciC / 10, abs(b.tempDeciC % 10));
      line(200, buf);
      snprintf(buf, sizeof(buf), "External power (GPIO9): %s", b.externalPower ? "PRESENT" : "absent");
      line(240, buf);
      snprintf(buf, sizeof(buf), "GPIO40 raw (unverified): %d", b.chargeStateRaw);
      line(280, buf);
      line(340, pass ? "RESULT: PASS" : "RESULT: FAIL", true);
      small(380, "Plug and unplug USB-C: external power and current sign must follow.");
      footer("Updates every 3 s.  AI = back");
      board::gfx().displayBuffer(first ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
      first = false;
      MT_LOGF("battery: ok=%d soc=%d mv=%d ma=%d t=%d ext=%d gpio40=%d", b.gaugeOk, b.socPercent, b.millivolts,
              b.currentMa, b.tempDeciC, b.externalPower, b.chargeStateRaw);
    }
    const InputEvent e = pollInput();
    if (e.ev == Ev::Ok || e.ev == Ev::Tap || e.ev == Ev::OkLong) break;
    delay(10);
  }
  record(kBattery, pass);
}

void testSleep() {
  beginFrame("Deep-sleep wake test");
  line(100, "The device will deep-sleep and wake itself");
  line(140, "after 120 s using the ESP32 RTC timer.");
  line(200, "Do NOT press any button while it sleeps:");
  line(240, "an AI-button wake counts as FAIL.");
  line(300, "AI = start      UP = cancel", true);
  present(HalDisplay::HALF_REFRESH);
  const uint32_t start = millis();
  while (millis() - start < 60000) {
    const InputEvent e = pollInput();
    if (e.ev == Ev::Up || e.ev == Ev::OkLong) return;
    if (e.ev == Ev::Ok || e.ev == Ev::Tap) {
      beginFrame("Deep-sleep wake test");
      char buf[80];
      snprintf(buf, sizeof(buf), "Sleeping for %lu s...", static_cast<unsigned long>(core::kSelfTestWakeSeconds));
      line(140, buf, true);
      line(200, "Expect the result screen at wake-up.");
      board::gfx().displayBuffer(HalDisplay::FULL_REFRESH);
      board::enterDeepSleep(core::kSelfTestWakeSeconds, true);
    }
    delay(10);
  }
}

void testPower() {
  bool first = true;
  for (;;) {
    const core::PowerCounters& c = board::powerCounters();
    beginFrame("Power counters");
    char buf[112];
    snprintf(buf, sizeof(buf), "Boots %lu   Sleeps %lu   Early wakes %lu", static_cast<unsigned long>(c.boots),
             static_cast<unsigned long>(c.sleeps), static_cast<unsigned long>(c.earlyWakes));
    line(90, buf);
    snprintf(buf, sizeof(buf), "Wakes: timer %lu  button %lu  touch %lu  USB %lu  power-on %lu",
             static_cast<unsigned long>(c.wakes[static_cast<int>(core::WakeSource::Timer)]),
             static_cast<unsigned long>(c.wakes[static_cast<int>(core::WakeSource::Button)]),
             static_cast<unsigned long>(c.wakes[static_cast<int>(core::WakeSource::Touch)]),
             static_cast<unsigned long>(c.wakes[static_cast<int>(core::WakeSource::Usb)]),
             static_cast<unsigned long>(c.wakes[static_cast<int>(core::WakeSource::PowerOn)]));
    small(130, buf);
    snprintf(buf, sizeof(buf), "Slept %llu h %02llu min   Awake %llu min", static_cast<unsigned long long>(c.sleepSec / 3600),
             static_cast<unsigned long long>((c.sleepSec / 60) % 60), static_cast<unsigned long long>(c.awakeMs / 60000));
    line(170, buf);
    snprintf(buf, sizeof(buf), "RTC drift at last wake %+ld s (max %+ld s)", static_cast<long>(c.lastDriftSec),
             static_cast<long>(c.maxDriftSec));
    line(210, buf);
    snprintf(buf, sizeof(buf), "Catch-up alarms %lu   Missed while asleep %lu   Battery shutdowns %lu",
             static_cast<unsigned long>(c.catchUpAlerts), static_cast<unsigned long>(c.missedWhileAsleep),
             static_cast<unsigned long>(c.shutdowns));
    small(250, buf);
    const uint64_t total = c.sleepSec * 1000ULL + c.awakeMs;
    snprintf(buf, sizeof(buf), "Awake share: %llu.%01llu %%", static_cast<unsigned long long>(total ? c.awakeMs * 100ULL / total : 0),
             static_cast<unsigned long long>(total ? (c.awakeMs * 1000ULL / total) % 10 : 0));
    line(290, buf);
    small(340, "Counters live in RTC memory and are mirrored to NVS before each sleep.");
    footer("AI = back     UP+DOWN = reset counters");
    present(first ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
    first = false;
    const uint32_t start = millis();
    bool redraw = false;
    while (!redraw) {
      const InputEvent e = pollInput();
      if (e.ev == Ev::Ok || e.ev == Ev::Tap || e.ev == Ev::OkLong) return;
      if (upHeld() && downHeld() && millis() - start > 500) {
        board::powerCountersReset();
        redraw = true;
      }
      if (millis() - start > 120000) return;
      delay(10);
    }
  }
}

void testSleepCurrent() {
  beginFrame("Sleep-current test");
  line(90, "Measure the battery current with a meter in series");
  line(130, "(or a USB power meter with the battery disconnected).");
  line(180, "The device enters deep sleep with every peripheral");
  line(220, "rail off and NO timer. Only the AI button wakes it.");
  small(270, "Expected: ESP32-S3 deep sleep plus board quiescent current (see docs/POWER.md).");
  small(300, "Counters are saved first; the result is entered by you in the docs.");
  line(350, "AI = sleep now      UP = cancel", true);
  present(HalDisplay::HALF_REFRESH);
  const uint32_t start = millis();
  while (millis() - start < 60000) {
    const InputEvent e = pollInput();
    if (e.ev == Ev::Up || e.ev == Ev::OkLong) return;
    if (e.ev == Ev::Ok || e.ev == Ev::Tap) {
      beginFrame("Sleep-current test");
      line(160, "Sleeping. Measure now.", true);
      line(220, "Press AI to wake.");
      board::gfx().displayBuffer(HalDisplay::FULL_REFRESH);
      board::powerCounters().recordSleep(0, millis());
      board::powerCountersSave();
      record(kSleepCurrent, true);
      board::SleepConfig c;
      c.timerSeconds = 0;
      c.upDown = false;
      c.usbEdge = false;
      board::enterDeepSleepEx(c);
    }
    delay(10);
  }
}

}  // namespace

void showWakeTestResult() {
  const board::WakeTestRecord rec = board::takeWakeTestRecord();
  if (!rec.pending) return;
  const board::WakeCause cause = board::wakeCause();
  const int64_t measured = board::nowSeconds() - rec.sleptAtSec;
  const core::WakeVerdict v = core::judgeWake(cause == board::WakeCause::Timer, rec.expectedSeconds, measured);
  s_results[kSleep] = v.pass ? Result::Pass : Result::Fail;
  MT_LOGF("wake test: cause=%s expected=%lu measured=%lld pass=%d", board::wakeCauseName(cause),
          static_cast<unsigned long>(rec.expectedSeconds), static_cast<long long>(measured), v.pass);
  beginFrame("Deep-sleep wake test: result");
  char buf[96];
  snprintf(buf, sizeof(buf), "Wake cause: %s (%s)", board::wakeCauseName(cause), v.causeOk ? "OK" : "WRONG");
  line(100, buf);
  snprintf(buf, sizeof(buf), "Expected %lu s, measured %lld s (%+ld s)", static_cast<unsigned long>(rec.expectedSeconds),
           static_cast<long long>(measured), static_cast<long>(v.errorSeconds));
  line(150, buf);
  line(200, v.timingOk ? "Timing within tolerance" : "Timing OUT of tolerance");
  line(270, v.pass ? "RESULT: PASS" : "RESULT: FAIL", true);
  small(330, "The time is measured on the ESP32 RTC clock, which also drives the wake timer.");
  small(360, "Check the real elapsed time with a stopwatch; it should also be about 2 minutes.");
  footer("AI = continue to self-test menu");
  present(HalDisplay::HALF_REFRESH);
  waitOk(120000);
  runSelfTestMenu();
}

void runSelfTestMenu() {
  MT_LOGF("selftest: enter");
  core::MenuModel menu(kItemCount, kRowTop, kRowH, kRowsPerCol, kColLeft, kColW);
  bool full = true;
  for (;;) {
    drawMenu(menu);
    present(full ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
    full = false;
    int run = -1;
    while (run < 0) {
      const InputEvent e = pollInput();
      if (e.ev == Ev::Up) {
        menu.previous();
        break;
      }
      if (e.ev == Ev::Down) {
        menu.next();
        break;
      }
      if (e.ev == Ev::Ok) run = menu.selected();
      if (e.ev == Ev::Tap) {
        const int idx = menu.hitTest(e.x, e.y);
        if (idx >= 0) {
          menu.select(idx);
          run = idx;
        }
      }
      delay(10);
    }
    if (run < 0) continue;
    switch (run) {
      case kDisplay: testDisplay(); break;
      case kTouch: testTouch(); break;
      case kButtons: testButtons(); break;
      case kAlerts: testAlerts(); break;
      case kSd: testSd(); break;
      case kBattery: testBattery(); break;
      case kSleep: testSleep(); break;
      case kPower: testPower(); break;
      case kSleepCurrent: testSleepCurrent(); break;
      case kExit:
        MT_LOGF("selftest: exit");
        flushInput();
        return;
      default: break;
    }
    full = true;  // tests leave arbitrary content on screen; repaint the menu cleanly
  }
}

}  // namespace ui
