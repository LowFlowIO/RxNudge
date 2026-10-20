#include "ui_core/touch_filter.h"

#include <cstdlib>

namespace ui {

static bool before(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) < 0; }

TouchTap TouchFilter::release(uint32_t nowMs) {
  TouchTap t;
  down_ = false;
  upAt_ = nowMs;
  everUp_ = true;
  if (guardOnRelease_) {
    guardUntil_ = nowMs + kTouchGuardMs;
    guardOnRelease_ = false;
  }
  if (!ignore_ && nowMs - downAt_ >= kTouchMinPressMs) {
    t.tap = true;
    t.x = x0_;
    t.y = y0_;
    ++taps_;
  } else {
    ++rejected_;
  }
  ignore_ = false;
  return t;
}

TouchTap TouchFilter::feed(const TouchReport& r, uint32_t nowMs) {
  switch (r.kind) {
    case TouchReport::Stale:
      // Nothing new from the chip. Only a long silence while down counts as a lift (missed up report).
      if (down_ && nowMs - lastReportAt_ >= kTouchStaleReleaseMs) return release(nowMs);
      return TouchTap();
    case TouchReport::Up:
      lastReportAt_ = nowMs;
      if (down_) return release(nowMs);
      return TouchTap();  // duplicate up
    case TouchReport::Down:
      break;
  }
  lastReportAt_ = nowMs;
  if (down_) {
    // Same touch (duplicate or moved report). Two fingers or a slide is not a tap.
    if (r.count > 1) ignore_ = true;
    if (std::abs(r.x - x0_) > kTouchSlopPx || std::abs(r.y - y0_) > kTouchSlopPx) ignore_ = true;
    track_ = r.trackId;
    return TouchTap();
  }
  // A new touch.
  down_ = true;
  downAt_ = nowMs;
  track_ = r.trackId;
  x0_ = r.x;
  y0_ = r.y;
  ignore_ = r.count > 1;
  if (everUp_ && nowMs - upAt_ < kTouchMinUpMs) ignore_ = true;  // bounce: lifted too briefly
  if (before(nowMs, guardUntil_)) ignore_ = true;                 // click-through after a view change
  return TouchTap();
}

void TouchFilter::guard(uint32_t nowMs) {
  if (down_) {
    ignore_ = true;
    guardOnRelease_ = true;
    guardUntil_ = nowMs + 0x7FFFFFF;  // until the release sets the real end
  } else {
    guardUntil_ = nowMs + kTouchGuardMs;
  }
}

}  // namespace ui
