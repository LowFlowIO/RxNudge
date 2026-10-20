#pragma once
// Visual metrics for the 3.97" 800x480 panel (235 ppi => 9.25 px/mm).
#include <cstdint>

#include "ui_core/text.h"

namespace ui {

constexpr int kPanelPpi = 235;
constexpr int mmToPx(double mm) { return static_cast<int>(mm * kPanelPpi / 25.4 + 0.5); }
constexpr int kMinTouchPx = mmToPx(9.0);  // 83 px

// Touch timing (GT911 polled every kTouchPollMs by the input task; see touch_filter.h).
constexpr uint32_t kTouchPollMs = 10;
constexpr uint32_t kTouchMinPressMs = 30;       // shorter contacts are noise, not taps
constexpr uint32_t kTouchMinUpMs = 70;          // a new touch needs the finger up this long (bounce = same touch)
constexpr uint32_t kTouchGuardMs = 280;         // after a screen/form change: finger up + this before touch counts
constexpr uint32_t kTouchStaleReleaseMs = 250;  // no fresh report while down this long = finger lifted
constexpr int kTouchSlopPx = 60;                // moved further than this while down = not a tap

struct Theme {
  bool large = false;
  const Font* small;     // secondary text (14 pt normal, 18 pt large)
  const Font* body;      // 18 pt normal, 24 pt large
  const Font* bodyBold;
  const Font* title;     // 24 pt bold
  const Font* huge;      // 40 pt bold
  int margin = 16;
  int gap = 10;
  int touch = kMinTouchPx;  // minimum touch target edge
  int headerH = 84;         // sub-screen header (back button is a full touch target)
  int homeHeaderH = 64;
  int border = 3;
  int focusBorder = 6;
  int radius = 14;
};

const Theme& normalTheme();
const Theme& largeTheme();
inline const Theme& themeFor(bool large) { return large ? largeTheme() : normalTheme(); }

}  // namespace ui
