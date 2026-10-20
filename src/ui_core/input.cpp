#include "ui_core/input.h"

#include <climits>

namespace ui {

int ButtonDecoder::update(uint32_t now, bool up, bool down, bool ok, InputEvent* out, int maxOut) {
  int n = 0;
  auto emit = [&](InputType t, uint16_t permille = 0) {
    if (n < maxOut) {
      out[n].type = t;
      out[n].x = out[n].y = 0;
      out[n].permille = permille;
      out[n].atMs = now;
      n++;
    }
  };
  // ---- UP / DOWN with auto-repeat and the UP+DOWN combo
  const bool upEdge = up && !up_, downEdge = down && !down_;
  if (up && down) {
    if (!combo_) {
      combo_ = true;
      comboFired_ = false;
      comboSince_ = now;
    } else if (!comboFired_ && now - comboSince_ >= kComboMs) {
      comboFired_ = true;
      emit(InputType::Combo);
    }
  } else {
    if (!up && !down) combo_ = false;
    if (!combo_) {
      if (upEdge) {
        upSince_ = now;
        nextRepeat_ = now + kRepeatDelayMs;
        emit(InputType::Up);
      } else if (downEdge) {
        downSince_ = now;
        nextRepeat_ = now + kRepeatDelayMs;
        emit(InputType::Down);
      } else if ((up || down) && static_cast<int32_t>(now - nextRepeat_) >= 0) {
        nextRepeat_ = now + kRepeatEveryMs;
        emit(up ? InputType::Up : InputType::Down);
      }
    }
  }
  up_ = up;
  down_ = down;
  // ---- OK: click, hold-to-confirm with progress, long press
  if (ok && !ok_) {
    okSince_ = now;
    holdDone_ = longDone_ = false;
    lastQuarter_ = 0;
  } else if (ok && ok_) {
    const uint32_t held = now - okSince_;
    const int quarter = static_cast<int>(held >= kHoldConfirmMs ? 4 : held * 4 / kHoldConfirmMs);
    if (quarter != lastQuarter_ && quarter < 4) {
      lastQuarter_ = quarter;
      emit(InputType::OkHold, static_cast<uint16_t>(quarter * 250));
    }
    if (!holdDone_ && held >= kHoldConfirmMs) {
      holdDone_ = true;
      lastQuarter_ = 4;
      emit(InputType::OkHoldDone, 1000);
    }
    if (!longDone_ && held >= kLongMs) {
      longDone_ = true;
      emit(InputType::OkLong);
    }
  } else if (!ok && ok_) {
    const uint32_t held = now - okSince_;
    if (!holdDone_ && held >= kClickMinMs) emit(InputType::OkClick);
    if (lastQuarter_ > 0) emit(InputType::OkRelease);
    lastQuarter_ = 0;
  }
  ok_ = ok;
  return n;
}

void HitMap::add(int id, const Rect& r, bool focusable) {
  if (n_ >= kMax) return;
  items_[n_++] = Item{id, r, focusable};
}

int HitMap::hit(int x, int y) const {
  for (int i = n_ - 1; i >= 0; i--)
    if (items_[i].r.contains(x, y)) return items_[i].id;
  int best = -1;
  long bestD = LONG_MAX;
  for (int i = 0; i < n_; i++) {
    const Rect& r = items_[i].r;
    const int gw = r.w < kMinTouchPx ? (kMinTouchPx - r.w + 1) / 2 : 0;
    const int gh = r.h < kMinTouchPx ? (kMinTouchPx - r.h + 1) / 2 : 0;
    const Rect g(r.x - gw, r.y - gh, r.w + 2 * gw, r.h + 2 * gh);
    if (!g.contains(x, y)) continue;
    const long dx = x - r.cx(), dy = y - r.cy();
    const long d = dx * dx + dy * dy;
    if (d < bestD) {
      bestD = d;
      best = items_[i].id;
    }
  }
  return best;
}

uint32_t HitMap::layoutKey() const {
  uint32_t h = 2166136261u;  // FNV-1a
  auto mix = [&h](int v) {
    for (int i = 0; i < 4; i++) {
      h ^= static_cast<uint32_t>(v >> (8 * i)) & 0xFFu;
      h *= 16777619u;
    }
  };
  mix(n_);
  for (int i = 0; i < n_; i++) {
    mix(items_[i].id);
    mix(items_[i].r.x);
    mix(items_[i].r.y);
    mix(items_[i].r.w);
    mix(items_[i].r.h);
  }
  return h;
}

const Rect* HitMap::rectOf(int id) const {
  for (int i = 0; i < n_; i++)
    if (items_[i].id == id) return &items_[i].r;
  return nullptr;
}

int HitMap::focusableCount() const {
  int c = 0;
  for (int i = 0; i < n_; i++) c += items_[i].focusable ? 1 : 0;
  return c;
}

int HitMap::focusableId(int index) const {
  for (int i = 0; i < n_; i++)
    if (items_[i].focusable && index-- == 0) return items_[i].id;
  return -1;
}

int HitMap::focusIndexOf(int id) const {
  int k = 0;
  for (int i = 0; i < n_; i++) {
    if (!items_[i].focusable) continue;
    if (items_[i].id == id) return k;
    k++;
  }
  return -1;
}

}  // namespace ui
