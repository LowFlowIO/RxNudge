#pragma once
// Hardware-free logic used by the board layer and the hidden self-test menu.
// Everything here compiles on the host (env:native) and is unit tested.

#include <cstddef>
#include <cstdint>

namespace core {

// ---------------------------------------------------------------- menu model
class MenuModel {
 public:
  MenuModel(int itemCount, int rowTop, int rowHeight);
  // Grid: items fill column by column (rowsPerColumn each) starting at x = left.
  MenuModel(int itemCount, int rowTop, int rowHeight, int rowsPerColumn, int left, int columnWidth);
  int count() const { return count_; }
  int selected() const { return selected_; }
  void next();
  void previous();
  void select(int index);
  // Maps a logical tap Y coordinate to a row index; returns -1 outside the list.
  int hitTest(int y) const;
  int hitTest(int x, int y) const;  // grid aware
  int rowTop(int index) const { return rowTop_ + (index % rowsPerCol_) * rowHeight_; }
  int columnLeft(int index) const { return left_ + (index / rowsPerCol_) * colWidth_; }
  int columnWidth() const { return colWidth_; }
  int rowHeight() const { return rowHeight_; }

 private:
  int count_;
  int rowTop_;
  int rowHeight_;
  int rowsPerCol_;
  int left_ = 0;
  int colWidth_ = 0;
  int selected_ = 0;
};

// ------------------------------------------------- hidden self-test entry gate
// Fires once when UP and DOWN are held together for holdMs.
class ComboGate {
 public:
  explicit ComboGate(uint32_t holdMs) : holdMs_(holdMs) {}
  // Returns true exactly once per continuous hold.
  bool update(uint32_t nowMs, bool upHeld, bool downHeld);
  void reset();

 private:
  uint32_t holdMs_;
  bool holding_ = false;
  bool fired_ = false;
  uint32_t startMs_ = 0;
};

// --------------------------------------------------------- press classifier
enum class PressKind : uint8_t { None, Short, Long };
// Short: 50 ms <= d < longMs, Long: d >= longMs. Below 50 ms is treated as bounce.
PressKind classifyPress(uint32_t durationMs, uint32_t longMs);

// ------------------------------------------------------------ wake schedule
// Converts seconds to microseconds for esp_sleep_enable_timer_wakeup, clamped
// to [1 s, 7 days] so a corrupt value can never brick the wake path.
uint64_t sleepMicros(uint32_t seconds);
constexpr uint32_t kSelfTestWakeSeconds = 120;
constexpr uint32_t kMaxSleepSeconds = 7u * 24u * 3600u;

struct WakeVerdict {
  bool pass;
  bool causeOk;
  bool timingOk;
  int32_t errorSeconds;  // measured - expected
};
// causeIsTimer: esp_sleep_get_wakeup_cause() == TIMER.
// Tolerance: -2 s .. +max(5 s, 5 %) (RTC RC oscillator drift + boot time).
WakeVerdict judgeWake(bool causeIsTimer, uint32_t expectedSeconds, int64_t measuredSeconds);

// --------------------------------------------------------------- SD pattern
uint32_t crc32(const uint8_t* data, size_t len, uint32_t seed = 0);
// Deterministic pseudo-random pattern (xorshift32) for SD write/read checks.
void fillPattern(uint8_t* buf, size_t len, uint32_t seed);

// ------------------------------------------------------------------ battery
enum class BatteryHealth : uint8_t { Unknown, Critical, Low, Ok, Full };
BatteryHealth classifyBattery(int socPercent, int millivolts);
const char* batteryHealthName(BatteryHealth h);
// BQ27220 Temperature() is in 0.1 K; returns tenths of a degree Celsius.
int bq27220TempToDeciC(uint16_t raw);

// ------------------------------------------------------------- touch target
struct Rect {
  int x, y, w, h;
  bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};
// Four corner targets of size s inset by m on a w x h screen: TL, TR, BL, BR.
void cornerTargets(int screenW, int screenH, int s, int m, Rect out[4]);

}  // namespace core
