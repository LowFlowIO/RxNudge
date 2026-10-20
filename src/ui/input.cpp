#include "ui/input.h"

#include <Arduino.h>

#include "board/board.h"
#include "core/selftest_logic.h"

namespace ui {
namespace {

uint32_t s_okDownMs = 0;
bool s_okDown = false;
bool s_okLongFired = false;

}  // namespace

InputEvent pollInput() {
  HalGPIO& io = board::io();
  io.update();
  InputEvent e;
  const uint32_t now = millis();

  if (io.wasPressed(HalGPIO::BTN_POWER)) {
    s_okDown = true;
    s_okDownMs = now;
    s_okLongFired = false;
  }
  if (s_okDown && !s_okLongFired && io.isPressed(HalGPIO::BTN_POWER) &&
      core::classifyPress(now - s_okDownMs, MT_POWER_LONG_PRESS_MS) == core::PressKind::Long) {
    s_okLongFired = true;
    e.ev = Ev::OkLong;
    return e;
  }
  if (s_okDown && io.wasReleased(HalGPIO::BTN_POWER)) {
    s_okDown = false;
    if (!s_okLongFired &&
        core::classifyPress(now - s_okDownMs, MT_POWER_LONG_PRESS_MS) == core::PressKind::Short) {
      e.ev = Ev::Ok;
      return e;
    }
  }
  // UP/DOWN are reported on press, but not while both are held (self-test combo).
  if (io.wasPressed(HalGPIO::BTN_UP) && !io.isPressed(HalGPIO::BTN_DOWN)) {
    e.ev = Ev::Up;
    return e;
  }
  if (io.wasPressed(HalGPIO::BTN_DOWN) && !io.isPressed(HalGPIO::BTN_UP)) {
    e.ev = Ev::Down;
    return e;
  }
  int x = 0, y = 0;
  if (io.consumeTap(x, y)) {
    e.ev = Ev::Tap;
    e.x = x;
    e.y = y;
  }
  return e;
}

bool upHeld() { return board::io().isPressed(HalGPIO::BTN_UP); }
bool downHeld() { return board::io().isPressed(HalGPIO::BTN_DOWN); }
bool okHeld() { return board::io().isPressed(HalGPIO::BTN_POWER); }

void flushInput() {
  int x, y;
  board::io().touchGuard();  // a finger still down from the previous screen must not tap the next one
  for (int i = 0; i < 5; i++) {
    board::io().update();
    delay(10);
  }
  while (board::io().consumeTap(x, y)) {
  }
  s_okDown = okHeld();
  s_okLongFired = s_okDown;  // a still-held OK must not fire again
}

}  // namespace ui
