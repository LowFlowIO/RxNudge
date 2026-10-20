#include "core/selftest_logic.h"

namespace core {

MenuModel::MenuModel(int itemCount, int rowTop, int rowHeight)
    : count_(itemCount > 0 ? itemCount : 1), rowTop_(rowTop), rowHeight_(rowHeight > 0 ? rowHeight : 1), rowsPerCol_(count_) {}

MenuModel::MenuModel(int itemCount, int rowTop, int rowHeight, int rowsPerColumn, int left, int columnWidth)
    : MenuModel(itemCount, rowTop, rowHeight) {
  rowsPerCol_ = rowsPerColumn > 0 ? rowsPerColumn : count_;
  left_ = left;
  colWidth_ = columnWidth > 0 ? columnWidth : 1;
}

int MenuModel::hitTest(int x, int y) const {
  if (y < rowTop_ || x < left_) return -1;
  const int row = (y - rowTop_) / rowHeight_;
  if (row >= rowsPerCol_) return -1;
  const int col = colWidth_ > 0 ? (x - left_) / colWidth_ : 0;
  const int idx = col * rowsPerCol_ + row;
  return idx < count_ ? idx : -1;
}

void MenuModel::next() { selected_ = (selected_ + 1) % count_; }

void MenuModel::previous() { selected_ = (selected_ + count_ - 1) % count_; }

void MenuModel::select(int index) {
  if (index >= 0 && index < count_) selected_ = index;
}

int MenuModel::hitTest(int y) const {
  if (y < rowTop_) return -1;
  const int idx = (y - rowTop_) / rowHeight_;
  return idx < count_ && idx < rowsPerCol_ ? idx : -1;
}

bool ComboGate::update(uint32_t nowMs, bool upHeld, bool downHeld) {
  if (!(upHeld && downHeld)) {
    holding_ = false;
    fired_ = false;
    return false;
  }
  if (!holding_) {
    holding_ = true;
    startMs_ = nowMs;
    return false;
  }
  if (!fired_ && (nowMs - startMs_) >= holdMs_) {
    fired_ = true;
    return true;
  }
  return false;
}

void ComboGate::reset() {
  holding_ = false;
  fired_ = false;
  startMs_ = 0;
}

PressKind classifyPress(uint32_t durationMs, uint32_t longMs) {
  if (durationMs < 50) return PressKind::None;
  return durationMs >= longMs ? PressKind::Long : PressKind::Short;
}

uint64_t sleepMicros(uint32_t seconds) {
  if (seconds < 1) seconds = 1;
  if (seconds > kMaxSleepSeconds) seconds = kMaxSleepSeconds;
  return static_cast<uint64_t>(seconds) * 1000000ULL;
}

WakeVerdict judgeWake(bool causeIsTimer, uint32_t expectedSeconds, int64_t measuredSeconds) {
  WakeVerdict v{};
  v.causeOk = causeIsTimer;
  v.errorSeconds = static_cast<int32_t>(measuredSeconds - static_cast<int64_t>(expectedSeconds));
  int32_t upper = static_cast<int32_t>(expectedSeconds / 20);  // 5 %
  if (upper < 5) upper = 5;
  v.timingOk = v.errorSeconds >= -2 && v.errorSeconds <= upper;
  v.pass = v.causeOk && v.timingOk;
  return v;
}

uint32_t crc32(const uint8_t* data, size_t len, uint32_t seed) {
  uint32_t crc = ~seed;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

void fillPattern(uint8_t* buf, size_t len, uint32_t seed) {
  uint32_t x = seed ? seed : 0x9E3779B9u;
  for (size_t i = 0; i < len; i++) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    buf[i] = static_cast<uint8_t>(x & 0xFF);
  }
}

BatteryHealth classifyBattery(int socPercent, int millivolts) {
  if (socPercent < 0 || socPercent > 100) return BatteryHealth::Unknown;
  if (millivolts > 0 && millivolts < 3400) return BatteryHealth::Critical;
  if (socPercent <= 5) return BatteryHealth::Critical;
  if (socPercent <= 20) return BatteryHealth::Low;
  if (socPercent >= 98) return BatteryHealth::Full;
  return BatteryHealth::Ok;
}

const char* batteryHealthName(BatteryHealth h) {
  switch (h) {
    case BatteryHealth::Critical: return "CRITICAL";
    case BatteryHealth::Low: return "LOW";
    case BatteryHealth::Ok: return "OK";
    case BatteryHealth::Full: return "FULL";
    default: return "UNKNOWN";
  }
}

int bq27220TempToDeciC(uint16_t raw) { return static_cast<int>(raw) - 2732; }

void cornerTargets(int screenW, int screenH, int s, int m, Rect out[4]) {
  out[0] = {m, m, s, s};
  out[1] = {screenW - m - s, m, s, s};
  out[2] = {m, screenH - m - s, s, s};
  out[3] = {screenW - m - s, screenH - m - s, s, s};
}

}  // namespace core
