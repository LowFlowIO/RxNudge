#include "ui_core/text.h"

namespace ui {

uint32_t utf8Next(const char*& s) {
  const auto* p = reinterpret_cast<const uint8_t*>(s);
  if (*p == 0) return 0;
  uint32_t cp;
  int extra;
  if (p[0] < 0x80) {
    cp = p[0];
    extra = 0;
  } else if ((p[0] & 0xE0) == 0xC0) {
    cp = p[0] & 0x1F;
    extra = 1;
  } else if ((p[0] & 0xF0) == 0xE0) {
    cp = p[0] & 0x0F;
    extra = 2;
  } else if ((p[0] & 0xF8) == 0xF0) {
    cp = p[0] & 0x07;
    extra = 3;
  } else {
    s++;
    return 0xFFFD;
  }
  for (int i = 1; i <= extra; i++) {
    if ((p[i] & 0xC0) != 0x80) {
      s += i;
      return 0xFFFD;
    }
    cp = (cp << 6) | (p[i] & 0x3F);
  }
  s += extra + 1;
  return cp;
}

const Glyph* findGlyph(const Font& f, uint32_t cp) {
  int lo = 0, hi = f.rangeCount - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    const GlyphRange& r = f.ranges[mid];
    if (cp < r.first) hi = mid - 1;
    else if (cp > r.last) lo = mid + 1;
    else return &f.glyphs[r.index + (cp - r.first)];
  }
  return cp == '?' ? nullptr : findGlyph(f, '?');
}

int textWidth(const Font& f, const char* s) {
  int w = 0;
  uint32_t cp;
  while ((cp = utf8Next(s)) != 0) {
    const Glyph* g = findGlyph(f, cp);
    if (g) w += g->adv;
  }
  return w;
}

std::string fitText(const Font& f, const std::string& s, int maxWidth) {
  if (textWidth(f, s) <= maxWidth) return s;
  static const char* kEllipsis = "\xE2\x80\xA6";
  const int ew = textWidth(f, kEllipsis);
  std::string out;
  int w = 0;
  const char* p = s.c_str();
  for (;;) {
    const char* start = p;
    const uint32_t cp = utf8Next(p);
    if (cp == 0) break;
    const Glyph* g = findGlyph(f, cp);
    const int gw = g ? g->adv : 0;
    if (w + gw + ew > maxWidth) break;
    out.append(start, static_cast<size_t>(p - start));
    w += gw;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out + kEllipsis;
}

int wrapText(const Font& f, const std::string& s, int maxWidth, int maxLines, std::vector<std::string>& out) {
  out.clear();
  std::vector<std::string> words;
  std::string cur;
  for (char c : s) {
    if (c == ' ' || c == '\n') {
      if (!cur.empty()) words.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) words.push_back(cur);
  std::string line;
  size_t i = 0;
  for (; i < words.size(); i++) {
    const std::string cand = line.empty() ? words[i] : line + " " + words[i];
    if (textWidth(f, cand) <= maxWidth || line.empty()) {
      line = cand;
      continue;
    }
    if (static_cast<int>(out.size()) + 1 == maxLines) break;
    out.push_back(fitText(f, line, maxWidth));
    line = words[i];
  }
  if (!line.empty()) {
    if (i < words.size()) {
      for (; i < words.size(); i++) line += " " + words[i];
      out.push_back(fitText(f, line + "\xE2\x80\xA6", maxWidth));  // force the ellipsis
    } else {
      out.push_back(fitText(f, line, maxWidth));
    }
  }
  return static_cast<int>(out.size());
}

}  // namespace ui
