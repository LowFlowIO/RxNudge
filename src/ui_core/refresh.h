#pragma once
// Dirty-rectangle detection and the e-paper refresh policy.
#include <cstdint>
#include <vector>

#include "ui_core/geometry.h"

namespace ui {

// Compares two frame buffers in bands of `bandH` rows and returns byte-aligned
// rectangles covering every changed pixel. Vertically adjacent bands with
// overlapping columns are merged; when more than maxRects remain they are united.
void diffFrames(const uint8_t* prev, const uint8_t* next, std::vector<Rect>& out, int bandH = 16, size_t maxRects = 6);
// Grows a rect to x/width multiples of 8 inside the panel (SSD1677 window guard).
Rect alignToBytes(const Rect& r);
int area(const std::vector<Rect>& rs);

enum class RefreshKind : uint8_t {
  None,     // nothing changed
  Partial,  // FAST (differential) refresh
  Half,     // screen type changed: clears ghosting without the full black/white flash
  Full      // every N partials, nightly, after power-on, or when forced
};
const char* refreshKindName(RefreshKind k);

class RefreshPolicy {
 public:
  explicit RefreshPolicy(int fullEvery = 20, int nightlyMinute = 3 * 60) : fullEvery_(fullEvery), nightlyMinute_(nightlyMinute) {}
  // localDay/minuteOfDay are the current local date and time (for the nightly full refresh).
  RefreshKind decide(bool changed, int screenType, int32_t localDay, int minuteOfDay, bool force = false);
  int partialsSinceFull() const { return partials_; }
  // Settings > Display: full refresh after N fast refreshes (5..50) and the optional nightly full refresh.
  void configure(int fullEvery, bool nightly) {
    fullEvery_ = fullEvery < 5 ? 5 : (fullEvery > 50 ? 50 : fullEvery);
    nightly_ = nightly;
  }
  void reset() { first_ = true; }

 private:
  int fullEvery_;
  int nightlyMinute_;
  bool nightly_ = true;
  int partials_ = 0;
  int lastScreen_ = -1;
  int32_t lastNightlyDay_ = INT32_MIN;
  bool first_ = true;
};

// Rolling latency statistics (input event -> refresh finished).
class LatencyStats {
 public:
  void add(uint32_t ms);
  uint32_t count() const { return n_; }
  uint32_t minMs() const { return min_; }
  uint32_t maxMs() const { return max_; }
  uint32_t avgMs() const { return n_ ? static_cast<uint32_t>(sum_ / n_) : 0; }
  uint32_t lastMs() const { return last_; }
  uint32_t p90Ms() const;  // over the last 32 samples

 private:
  uint32_t n_ = 0, min_ = UINT32_MAX, max_ = 0, last_ = 0;
  uint64_t sum_ = 0;
  uint32_t ring_[32] = {};
};

}  // namespace ui
