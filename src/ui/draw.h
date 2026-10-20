#pragma once
#include <HalDisplay.h>

namespace ui {

void beginFrame(const char* title);
void line(int y, const char* text, bool bold = false);
void small(int y, const char* text);
void footer(const char* text);
void present(HalDisplay::RefreshMode mode);
// Shows a yes/no question; returns true for UP (yes), false for DOWN (no) or timeout.
bool askYesNo(const char* title, const char* question, uint32_t timeoutMs);

}  // namespace ui
