#pragma once
// 800x480 1-bit drawing surface in the SSD1677 frame-buffer layout
// (100 bytes per row, MSB = leftmost pixel, bit 1 = white, bit 0 = black).
#include <cstdint>

#include "ui_core/geometry.h"
#include "ui_core/text.h"

namespace ui {

enum class Color : uint8_t { Black, White };
enum class Align : uint8_t { Left, Center, Right };

class Canvas {
 public:
  static constexpr int kW = 800;
  static constexpr int kH = 480;
  static constexpr int kStride = kW / 8;
  static constexpr int kBytes = kStride * kH;

  explicit Canvas(uint8_t* buffer) : buf_(buffer) {}
  uint8_t* data() { return buf_; }
  const uint8_t* data() const { return buf_; }

  void clear(Color c = Color::White);
  void pixel(int x, int y, Color c);
  bool isBlack(int x, int y) const;
  void fillRect(const Rect& r, Color c);
  void rect(const Rect& r, int thickness, Color c);
  void hline(int x0, int x1, int y, int thickness, Color c);
  void vline(int x, int y0, int y1, int thickness, Color c);
  void line(int x0, int y0, int x1, int y1, int thickness, Color c);
  void fillRoundRect(const Rect& r, int radius, Color c);
  void roundRect(const Rect& r, int radius, int thickness, Color c);
  void fillCircle(int cx, int cy, int r, Color c);
  void circle(int cx, int cy, int r, int thickness, Color c);
  void invert(const Rect& r);
  // 1-bit bitmap, rows padded to whole bytes, set bits drawn in `c`.
  void bitmap(int x, int y, const uint8_t* bits, int w, int h, Color c);
  // Text with the pen at (x, baseline). Returns the pen x after the text.
  int text(const Font& f, int x, int baseline, const char* s, Color c);
  // Text inside a box: horizontally aligned, vertically centred on the font's cap area, ellipsized to fit.
  void textIn(const Font& f, const Rect& box, const std::string& s, Align a, Color c);

  void setClip(const Rect& r) { clip_ = r.intersect(Rect(0, 0, kW, kH)); }
  void resetClip() { clip_ = Rect(0, 0, kW, kH); }

 private:
  uint8_t* buf_;
  Rect clip_{0, 0, kW, kH};
};

}  // namespace ui
