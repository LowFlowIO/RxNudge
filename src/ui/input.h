#pragma once
#include <cstdint>

namespace ui {

enum class Ev : uint8_t { None, Up, Down, Ok, OkLong, Tap };

struct InputEvent {
  Ev ev = Ev::None;
  int x = 0;
  int y = 0;
};

// Call every loop iteration (~10 ms). Polls buttons and GT911 through HalGPIO.
// OK short fires on release (50 ms .. long threshold); OkLong fires once while held.
InputEvent pollInput();
bool upHeld();
bool downHeld();
bool okHeld();
// Drains pending events (used after blocking screens and refreshes).
void flushInput();

}  // namespace ui
