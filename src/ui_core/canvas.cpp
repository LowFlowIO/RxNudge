#include "ui_core/canvas.h"

#include <cstdlib>
#include <cstring>

namespace ui {

void Canvas::clear(Color c) { std::memset(buf_, c == Color::White ? 0xFF : 0x00, kBytes); }

void Canvas::pixel(int x, int y, Color c) {
  if (!clip_.contains(x, y)) return;
  uint8_t& b = buf_[y * kStride + (x >> 3)];
  const uint8_t m = static_cast<uint8_t>(0x80 >> (x & 7));
  if (c == Color::Black) b &= static_cast<uint8_t>(~m);
  else b |= m;
}

bool Canvas::isBlack(int x, int y) const {
  if (x < 0 || y < 0 || x >= kW || y >= kH) return false;
  return (buf_[y * kStride + (x >> 3)] & (0x80 >> (x & 7))) == 0;
}

void Canvas::fillRect(const Rect& r0, Color c) {
  const Rect r = r0.intersect(clip_);
  if (r.empty()) return;
  for (int y = r.y; y < r.bottom(); y++) {
    uint8_t* row = buf_ + y * kStride;
    int x = r.x;
    while (x < r.right() && (x & 7)) pixel(x++, y, c);
    const int fullEnd = r.right() & ~7;
    if (x < fullEnd) {
      std::memset(row + (x >> 3), c == Color::White ? 0xFF : 0x00, static_cast<size_t>((fullEnd - x) >> 3));
      x = fullEnd;
    }
    while (x < r.right()) pixel(x++, y, c);
  }
}

void Canvas::rect(const Rect& r, int t, Color c) {
  fillRect(Rect(r.x, r.y, r.w, t), c);
  fillRect(Rect(r.x, r.bottom() - t, r.w, t), c);
  fillRect(Rect(r.x, r.y, t, r.h), c);
  fillRect(Rect(r.right() - t, r.y, t, r.h), c);
}

void Canvas::hline(int x0, int x1, int y, int t, Color c) {
  if (x1 < x0) std::swap(x0, x1);
  fillRect(Rect(x0, y, x1 - x0 + 1, t), c);
}

void Canvas::vline(int x, int y0, int y1, int t, Color c) {
  if (y1 < y0) std::swap(y0, y1);
  fillRect(Rect(x, y0, t, y1 - y0 + 1), c);
}

void Canvas::line(int x0, int y0, int x1, int y1, int t, Color c) {
  const int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  const int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  const int h = t / 2;
  for (;;) {
    fillRect(Rect(x0 - h, y0 - h, t, t), c);
    if (x0 == x1 && y0 == y1) break;
    const int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

void Canvas::fillCircle(int cx, int cy, int r, Color c) {
  for (int dy = -r; dy <= r; dy++) {
    int dx = 0;
    while ((dx + 1) * (dx + 1) + dy * dy <= r * r) dx++;
    fillRect(Rect(cx - dx, cy + dy, 2 * dx + 1, 1), c);
  }
}

void Canvas::circle(int cx, int cy, int r, int t, Color c) {
  for (int y = -r; y <= r; y++)
    for (int x = -r; x <= r; x++) {
      const int d = x * x + y * y;
      if (d <= r * r && d > (r - t) * (r - t)) pixel(cx + x, cy + y, c);
    }
}

void Canvas::fillRoundRect(const Rect& r, int rad, Color c) {
  rad = std::min(rad, std::min(r.w, r.h) / 2);
  fillRect(Rect(r.x, r.y + rad, r.w, r.h - 2 * rad), c);
  for (int i = 0; i < rad; i++) {
    const int dy = rad - i;
    int dx = 0;
    while ((dx + 1) * (dx + 1) + dy * dy <= rad * rad) dx++;
    const int inset = rad - dx;
    fillRect(Rect(r.x + inset, r.y + i, r.w - 2 * inset, 1), c);
    fillRect(Rect(r.x + inset, r.bottom() - 1 - i, r.w - 2 * inset, 1), c);
  }
}

void Canvas::roundRect(const Rect& r, int rad, int t, Color c) {
  rad = std::min(rad, std::min(r.w, r.h) / 2);
  // edges
  fillRect(Rect(r.x + rad, r.y, r.w - 2 * rad, t), c);
  fillRect(Rect(r.x + rad, r.bottom() - t, r.w - 2 * rad, t), c);
  fillRect(Rect(r.x, r.y + rad, t, r.h - 2 * rad), c);
  fillRect(Rect(r.right() - t, r.y + rad, t, r.h - 2 * rad), c);
  // corners
  const int cxs[2] = {r.x + rad, r.right() - 1 - rad};
  const int cys[2] = {r.y + rad, r.bottom() - 1 - rad};
  for (int yy = 0; yy < 2; yy++)
    for (int xx = 0; xx < 2; xx++)
      for (int dy = 0; dy <= rad; dy++)
        for (int dx = 0; dx <= rad; dx++) {
          const int d = dx * dx + dy * dy;
          if (d <= rad * rad && d > (rad - t) * (rad - t))
            pixel(cxs[xx] + (xx ? dx : -dx), cys[yy] + (yy ? dy : -dy), c);
        }
}

void Canvas::invert(const Rect& r0) {
  const Rect r = r0.intersect(clip_);
  for (int y = r.y; y < r.bottom(); y++)
    for (int x = r.x; x < r.right(); x++) buf_[y * kStride + (x >> 3)] ^= static_cast<uint8_t>(0x80 >> (x & 7));
}

void Canvas::bitmap(int x, int y, const uint8_t* bits, int w, int h, Color c) {
  const int stride = (w + 7) / 8;
  for (int yy = 0; yy < h; yy++)
    for (int xx = 0; xx < w; xx++)
      if (bits[yy * stride + xx / 8] & (0x80 >> (xx & 7))) pixel(x + xx, y + yy, c);
}

int Canvas::text(const Font& f, int x, int baseline, const char* s, Color c) {
  uint32_t cp;
  while ((cp = utf8Next(s)) != 0) {
    const Glyph* g = findGlyph(f, cp);
    if (!g) continue;
    const uint8_t* bits = f.bits + g->offset;
    const int gx = x + g->left, gy = baseline - g->top;
    for (int i = 0, n = g->w * g->h; i < n; i++)
      if (bits[i >> 3] & (0x80 >> (i & 7))) pixel(gx + i % g->w, gy + i / g->w, c);
    x += g->adv;
  }
  return x;
}

void Canvas::textIn(const Font& f, const Rect& box, const std::string& s, Align a, Color c) {
  const std::string t = fitText(f, s, box.w);
  const int w = textWidth(f, t);
  int x = box.x;
  if (a == Align::Center) x = box.x + (box.w - w) / 2;
  else if (a == Align::Right) x = box.right() - w;
  // centre the region from cap height (~0.72 ascent) to baseline
  const int cap = f.ascent * 72 / 100;
  const int baseline = box.y + (box.h + cap) / 2;
  text(f, x, baseline, t.c_str(), c);
}

}  // namespace ui
