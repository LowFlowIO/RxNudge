#pragma once
// Integer rectangle helpers shared by the pure UI code.
#include <algorithm>

namespace ui {

struct Rect {
  int x = 0, y = 0, w = 0, h = 0;
  constexpr Rect() = default;
  constexpr Rect(int x_, int y_, int w_, int h_) : x(x_), y(y_), w(w_), h(h_) {}
  bool empty() const { return w <= 0 || h <= 0; }
  int right() const { return x + w; }
  int bottom() const { return y + h; }
  int cx() const { return x + w / 2; }
  int cy() const { return y + h / 2; }
  bool contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
  Rect intersect(const Rect& o) const {
    const int l = std::max(x, o.x), t = std::max(y, o.y), r = std::min(right(), o.right()), b = std::min(bottom(), o.bottom());
    return (r > l && b > t) ? Rect(l, t, r - l, b - t) : Rect();
  }
  Rect unite(const Rect& o) const {
    if (empty()) return o;
    if (o.empty()) return *this;
    const int l = std::min(x, o.x), t = std::min(y, o.y), r = std::max(right(), o.right()), b = std::max(bottom(), o.bottom());
    return Rect(l, t, r - l, b - t);
  }
  Rect inset(int d) const { return Rect(x + d, y + d, w - 2 * d, h - 2 * d); }
  bool operator==(const Rect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
};

}  // namespace ui
