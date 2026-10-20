#pragma once
// Turns raw GT911 reports into taps: exactly one tap per touch, emitted on release.
// Pure (no Arduino); the device input task feeds it every kTouchPollMs, host tests feed
// simulated noisy sequences.
//
// GT911 status register 0x814E: bit7 = buffer ready (a new report), bits0..3 = point count.
// When bit7 is clear the chip has nothing new; that is NOT a release (the v1.0.2 driver
// treated it as one, so one touch became several taps between the chip's reports).
#include <cstdint>

#include "ui_core/theme.h"

namespace ui {

struct TouchReport {
  enum Kind : uint8_t { Stale, Up, Down } kind = Stale;  // Stale: buffer not ready / read failed
  uint8_t count = 0;    // points in this report
  uint8_t trackId = 0;  // GT911 track id of the first point
  int16_t x = 0, y = 0; // logical coordinates
};

struct TouchTap {
  bool tap = false;
  int16_t x = 0, y = 0;
};

class TouchFilter {
 public:
  // One poll. Returns a tap at most once per touch, on release.
  TouchTap feed(const TouchReport& r, uint32_t nowMs);
  // The view changed (screen, form page, mode): ignore the current touch, and any touch that
  // starts before the finger has been up for kTouchGuardMs.
  void guard(uint32_t nowMs);
  bool down() const { return down_; }
  // counters for the self-test / logs
  uint32_t taps() const { return taps_; }
  uint32_t rejected() const { return rejected_; }

 private:
  TouchTap release(uint32_t nowMs);
  bool down_ = false;
  bool ignore_ = false;     // current touch will not produce a tap
  uint32_t downAt_ = 0;
  uint32_t lastReportAt_ = 0;
  uint32_t upAt_ = 0;
  bool everUp_ = false;
  uint32_t guardUntil_ = 0; // touches starting before this are ignored
  bool guardOnRelease_ = false;
  uint8_t track_ = 0;
  int16_t x0_ = 0, y0_ = 0;
  uint32_t taps_ = 0, rejected_ = 0;
};

// UI side of the guard: after a render whose touch layout changed, taps detected before that
// render reached the panel were aimed at the old screen and are dropped (they may have queued
// up during the refresh).
class TapGate {
 public:
  // Call after each render with HitMap::layoutKey(); returns true when the layout changed
  // (then also call TouchFilter::guard via the input task).
  bool rendered(uint32_t layoutKey, uint32_t nowMs) {
    if (layoutKey == key_) return false;
    key_ = layoutKey;
    changedAt_ = nowMs;
    return true;
  }
  bool accept(uint32_t tapAtMs) const { return static_cast<int32_t>(tapAtMs - changedAt_) >= 0; }

 private:
  uint32_t key_ = 0;
  uint32_t changedAt_ = 0;
};

}  // namespace ui
