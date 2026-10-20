#include "ui/draw.h"

#include <Arduino.h>

#include "board/board.h"
#include "ui/input.h"

namespace ui {

void beginFrame(const char* title) {
  GfxRenderer& g = board::gfx();
  g.clearScreen();
  g.drawText(board::FONT_TITLE, 24, 16, title, true);
  g.drawLine(16, 62, MT_DISPLAY_W - 16, 62, 2, true);
}

void line(int y, const char* text, bool bold) {
  board::gfx().drawText(board::FONT_BODY, 32, y, text, true, bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
}

void small(int y, const char* text) { board::gfx().drawText(board::FONT_SMALL, 32, y, text, true); }

void footer(const char* text) {
  GfxRenderer& g = board::gfx();
  g.drawLine(16, MT_DISPLAY_H - 44, MT_DISPLAY_W - 16, MT_DISPLAY_H - 44, true);
  g.drawText(board::FONT_SMALL, 24, MT_DISPLAY_H - 34, text, true);
}

void present(HalDisplay::RefreshMode mode) {
  board::gfx().displayBuffer(mode);
  flushInput();
}

bool askYesNo(const char* title, const char* question, uint32_t timeoutMs) {
  beginFrame(title);
  line(140, question, true);
  line(220, "UP   = yes");
  line(270, "DOWN = no");
  footer("No answer within the timeout counts as NO.");
  present(HalDisplay::HALF_REFRESH);
  const uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    const InputEvent e = pollInput();
    if (e.ev == Ev::Up) return true;
    if (e.ev == Ev::Down) return false;
    delay(10);
  }
  return false;
}

}  // namespace ui
