#pragma once
// Input decoding (3 buttons + touch taps) and touch hit testing. Pure; the
// device input task feeds raw button levels and taps, the UI consumes events.
#include <cstdint>

#include "ui_core/geometry.h"
#include "ui_core/theme.h"

namespace ui {

enum class InputType : uint8_t {
  None,
  Up,          // press or auto-repeat
  Down,
  OkClick,     // OK released before the hold-confirm time
  OkHold,      // OK still held; permille = progress towards hold-confirm (quantized to quarters)
  OkHoldDone,  // OK held for kHoldConfirmMs (deliberate confirm)
  OkLong,      // OK held for kLongMs (sleep on the home screen)
  OkRelease,   // OK released after hold feedback was shown
  Tap,         // touch released at x,y
  Combo        // UP+DOWN held for kComboMs (hidden self-test)
};

struct InputEvent {
  InputType type = InputType::None;
  int16_t x = 0, y = 0;
  uint16_t permille = 0;
  uint32_t atMs = 0;  // when the input was detected (for latency measurement)
};

class ButtonDecoder {
 public:
  static constexpr uint32_t kHoldConfirmMs = 1000;
  static constexpr uint32_t kLongMs = 2000;
  static constexpr uint32_t kRepeatDelayMs = 600;
  static constexpr uint32_t kRepeatEveryMs = 200;
  static constexpr uint32_t kComboMs = 3000;
  static constexpr uint32_t kClickMinMs = 30;

  // Call every ~10 ms with the debounced levels (true = pressed). Returns the number of events written.
  int update(uint32_t nowMs, bool up, bool down, bool ok, InputEvent* out, int maxOut);

 private:
  bool up_ = false, down_ = false, ok_ = false;
  uint32_t upSince_ = 0, downSince_ = 0, okSince_ = 0, nextRepeat_ = 0, comboSince_ = 0;
  bool combo_ = false, comboFired_ = false;
  bool holdDone_ = false, longDone_ = false;
  int lastQuarter_ = 0;
};

// Touch targets registered while a screen renders. Hits outside a visual rect
// still count when they fall inside the target grown to the minimum touch size.
class HitMap {
 public:
  static constexpr int kMax = 56;
  void clear() { n_ = 0; }
  void add(int id, const Rect& r, bool focusable = true);
  int hit(int x, int y) const;  // -1 = none
  const Rect* rectOf(int id) const;
  int count() const { return n_; }
  int idAt(int i) const { return items_[i].id; }
  // Focus order = registration order of focusable targets.
  int focusableCount() const;
  int focusableId(int index) const;  // -1 if out of range
  int focusIndexOf(int id) const;
  // Hash of all targets (ids and rects): changes when what is under the finger changes.
  uint32_t layoutKey() const;

 private:
  struct Item {
    int id;
    Rect r;
    bool focusable;
  };
  Item items_[kMax];
  int n_ = 0;
};

}  // namespace ui
