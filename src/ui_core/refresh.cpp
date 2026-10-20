#include "ui_core/refresh.h"

#include <algorithm>

#include "ui_core/canvas.h"

namespace ui {

const char* refreshKindName(RefreshKind k) {
  switch (k) {
    case RefreshKind::None: return "none";
    case RefreshKind::Partial: return "partial";
    case RefreshKind::Half: return "half";
    case RefreshKind::Full: return "full";
  }
  return "?";
}

Rect alignToBytes(const Rect& r) {
  const Rect c = r.intersect(Rect(0, 0, Canvas::kW, Canvas::kH));
  if (c.empty()) return Rect();
  const int x0 = c.x & ~7;
  const int x1 = (c.right() + 7) & ~7;
  return Rect(x0, c.y, std::min(x1, Canvas::kW) - x0, c.h);
}

int area(const std::vector<Rect>& rs) {
  int a = 0;
  for (const auto& r : rs) a += r.w * r.h;
  return a;
}

void diffFrames(const uint8_t* prev, const uint8_t* next, std::vector<Rect>& out, int bandH, size_t maxRects) {
  out.clear();
  for (int y0 = 0; y0 < Canvas::kH; y0 += bandH) {
    const int y1 = std::min(Canvas::kH, y0 + bandH);
    int minB = Canvas::kStride, maxB = -1, minY = y1, maxY = -1;
    for (int y = y0; y < y1; y++) {
      const uint8_t* a = prev + y * Canvas::kStride;
      const uint8_t* b = next + y * Canvas::kStride;
      for (int i = 0; i < Canvas::kStride; i++) {
        if (a[i] != b[i]) {
          minB = std::min(minB, i);
          maxB = std::max(maxB, i);
          minY = std::min(minY, y);
          maxY = std::max(maxY, y);
        }
      }
    }
    if (maxB < 0) continue;
    const Rect r(minB * 8, minY, (maxB - minB + 1) * 8, maxY - minY + 1);
    // merge with the previous rect if it touches this band and overlaps in x
    if (!out.empty()) {
      Rect& p = out.back();
      const bool touches = p.bottom() >= y0;
      const bool overlapX = r.x < p.right() && p.x < r.right();
      if (touches && overlapX) {
        p = p.unite(r);
        continue;
      }
    }
    out.push_back(r);
  }
  while (out.size() > maxRects) {
    // unite the pair with the smallest combined growth
    size_t bi = 0;
    int best = INT32_MAX;
    for (size_t i = 0; i + 1 < out.size(); i++) {
      const Rect u = out[i].unite(out[i + 1]);
      const int growth = u.w * u.h - out[i].w * out[i].h - out[i + 1].w * out[i + 1].h;
      if (growth < best) {
        best = growth;
        bi = i;
      }
    }
    out[bi] = out[bi].unite(out[bi + 1]);
    out.erase(out.begin() + static_cast<long>(bi) + 1);
  }
}

RefreshKind RefreshPolicy::decide(bool changed, int screenType, int32_t localDay, int minuteOfDay, bool force) {
  // The "nightly epoch" advances at nightlyMinute_ local time each day.
  const int32_t epoch = minuteOfDay >= nightlyMinute_ ? localDay : localDay - 1;
  if (lastNightlyDay_ == INT32_MIN) lastNightlyDay_ = epoch;  // first call never counts as nightly
  const bool nightlyDue = nightly_ && epoch > lastNightlyDay_;
  if (epoch > lastNightlyDay_) lastNightlyDay_ = epoch;
  RefreshKind k = RefreshKind::None;
  if (first_ || force || nightlyDue) {
    k = RefreshKind::Full;
  } else if (screenType != lastScreen_) {
    k = RefreshKind::Half;
  } else if (changed) {
    k = partials_ + 1 >= fullEvery_ ? RefreshKind::Full : RefreshKind::Partial;
  }
  first_ = false;
  lastScreen_ = screenType;
  if (k == RefreshKind::Partial) partials_++;
  else if (k != RefreshKind::None) partials_ = 0;
  return k;
}

void LatencyStats::add(uint32_t ms) {
  ring_[n_ % 32] = ms;
  n_++;
  sum_ += ms;
  last_ = ms;
  min_ = std::min(min_, ms);
  max_ = std::max(max_, ms);
}

uint32_t LatencyStats::p90Ms() const {
  const uint32_t k = std::min<uint32_t>(n_, 32);
  if (!k) return 0;
  uint32_t tmp[32];
  std::copy(ring_, ring_ + k, tmp);
  std::sort(tmp, tmp + k);
  return tmp[(k * 9 + 9) / 10 - 1];
}

}  // namespace ui
