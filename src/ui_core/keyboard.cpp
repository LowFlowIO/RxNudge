#include "ui_core/keyboard.h"

#include "ui_core/assets/fonts.h"
#include "ui_core/text.h"
#include "ui_core/widgets.h"

namespace ui {
namespace {
// US QWERTY
const char* const kLower[3] = {"q w e r t y u i o p", "a s d f g h j k l", "z x c v b n m , ."};
const char* const kUpper[3] = {"Q W E R T Y U I O P", "A S D F G H J K L", "Z X C V B N M , ."};
const char* const kSym[3] = {"1 2 3 4 5 6 7 8 9 0", "- / : ; ( ) & @ \" '", ". , ? ! + * # % = _"};
// Accented letters (Latin-1, covered by the Atkinson fonts)
const char* const kAcc[3] = {"á à â ä ã å æ ç é è", "ê ë í ì î ï ñ ó ò ô", "ö õ ø ú ù û ü ß ý ÿ"};
const char* const kAccUp[3] = {"Á À Â Ä Ã Å Æ Ç É È", "Ê Ë Í Ì Î Ï Ñ Ó Ò Ô", "Ö Õ Ø Ú Ù Û Ü ß Ý Ÿ"};

std::vector<std::string> split(const char* s) {
  std::vector<std::string> out;
  std::string cur;
  for (const char* p = s; *p; p++) {
    if (*p == ' ') {
      out.push_back(cur);
      cur.clear();
    } else {
      cur += *p;
    }
  }
  out.push_back(cur);
  return out;
}
}  // namespace

const Rect& kbFieldRect() {
  static const Rect r(16, 60, Canvas::kW - 32, 78);  // between the 56 px title bar and the keys at kKbTop
  return r;
}
const Rect& kbKeysRect() {
  static const Rect r(0, kKbTop, Canvas::kW, Canvas::kH - kKbTop);
  return r;
}

void utf8PopBack(std::string& s) {
  if (s.empty()) return;
  size_t i = s.size() - 1;
  while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) i--;
  s.erase(i);
}

KeyboardModel::KeyboardModel(const std::string& initial, size_t maxBytes, bool numeric, bool password)
    : text_(initial), max_(maxBytes), numeric_(numeric), password_(password) {
  if (numeric_) layer_ = KbLayer::Numeric;
  if (text_.size() > max_) text_.resize(max_);
  relayout();
}

void KeyboardModel::relayout() {
  keys_.clear();
  if (layer_ == KbLayer::Numeric) {
    const char* digits = "123456789";
    const int w = 200, h = kKbRowH, x0 = (Canvas::kW - 3 * w) / 2;
    for (int i = 0; i < 9; i++)
      keys_.push_back({KeyKind::Char, std::string(1, digits[i]), std::string(1, digits[i]),
                       Rect(x0 + (i % 3) * w, kKbTop + (i / 3) * h, w, h)});
    keys_.push_back({KeyKind::Backspace, "Del", "", Rect(x0, kKbTop + 3 * h, w, h)});
    keys_.push_back({KeyKind::Char, "0", "0", Rect(x0 + w, kKbTop + 3 * h, w, h)});
    keys_.push_back({KeyKind::Done, "OK", "", Rect(x0 + 2 * w, kKbTop + 3 * h, w, h)});
    keys_.push_back({KeyKind::Cancel, "Cancel", "", Rect(0, kKbTop + 3 * h, x0, h)});
  } else {
    const char* const* rows = kLower;
    switch (layer_) {
      case KbLayer::Upper: rows = kUpper; break;
      case KbLayer::Symbols: rows = kSym; break;
      case KbLayer::Accents: rows = kAcc; break;
      case KbLayer::AccentsUpper: rows = kAccUp; break;
      default: break;
    }
    for (int r = 0; r < 3; r++) {
      const std::vector<std::string> ks = split(rows[r]);
      const int x0 = (Canvas::kW - static_cast<int>(ks.size()) * 80) / 2;  // QWERTY rows 2-3 are staggered
      for (size_t i = 0; i < ks.size(); i++)
        keys_.push_back({KeyKind::Char, ks[i], ks[i], Rect(x0 + static_cast<int>(i) * 80, kKbTop + r * kKbRowH, 80, kKbRowH)});
    }
    const int y = kKbTop + 3 * kKbRowH;
    const bool upper = layer_ == KbLayer::Upper || layer_ == KbLayer::AccentsUpper;
    if (layer_ == KbLayer::Symbols) {
      keys_.push_back({KeyKind::Layer, "abc", "abc", Rect(0, y, 120, kKbRowH)});
      keys_.push_back({KeyKind::Layer, "\xC3\xA0\xC3\xA9", "acc", Rect(120, y, 120, kKbRowH)});  // "àé"
    } else {
      keys_.push_back({KeyKind::Shift, upper ? "SHIFT" : "Shift", "", Rect(0, y, 120, kKbRowH)});
      keys_.push_back({KeyKind::Layer, "?123", "sym", Rect(120, y, 120, kKbRowH)});
    }
    keys_.push_back({KeyKind::Space, "Space", " ", Rect(240, y, 320, kKbRowH)});
    keys_.push_back({KeyKind::Backspace, "Del", "", Rect(560, y, 120, kKbRowH)});
    keys_.push_back({KeyKind::Done, "OK", "", Rect(680, y, 120, kKbRowH)});
  }
  if (focus_ >= static_cast<int>(keys_.size())) focus_ = 0;
}

size_t KeyboardModel::codepoints() const {
  size_t n = 0;
  const char* p = text_.c_str();
  while (utf8Next(p)) n++;
  return n;
}

KbAction KeyboardModel::insert(const std::string& s) {
  if (text_.size() + s.size() > max_) return KbAction::Rejected;
  if (numeric_ && (s.size() != 1 || s[0] < '0' || s[0] > '9')) return KbAction::Rejected;
  text_ += s;
  if (shiftOnce_ && (layer_ == KbLayer::Upper || layer_ == KbLayer::AccentsUpper)) {
    shiftOnce_ = false;
    layer_ = layer_ == KbLayer::Upper ? KbLayer::Lower : KbLayer::Accents;
    relayout();
    return KbAction::LayerChanged;
  }
  return KbAction::Edited;
}

KbAction KeyboardModel::backspace() {
  if (text_.empty()) return KbAction::None;
  utf8PopBack(text_);
  return KbAction::Edited;
}

KbAction KeyboardModel::press(size_t i) {
  if (i >= keys_.size()) return KbAction::None;
  const Key k = keys_[i];
  switch (k.kind) {
    case KeyKind::Char:
    case KeyKind::Space: return insert(k.text);
    case KeyKind::Backspace: return backspace();
    case KeyKind::Done: return KbAction::Done;
    case KeyKind::Cancel: return KbAction::Cancel;
    case KeyKind::Shift:
      if (layer_ == KbLayer::Lower || layer_ == KbLayer::Accents) {
        layer_ = layer_ == KbLayer::Lower ? KbLayer::Upper : KbLayer::AccentsUpper;
        shiftOnce_ = true;
      } else if (shiftOnce_) {
        shiftOnce_ = false;  // second press: caps lock
      } else {
        layer_ = layer_ == KbLayer::Upper ? KbLayer::Lower : KbLayer::Accents;
      }
      relayout();
      return KbAction::LayerChanged;
    case KeyKind::Layer:
      layer_ = k.text == "sym" ? KbLayer::Symbols : k.text == "acc" ? KbLayer::Accents : KbLayer::Lower;
      shiftOnce_ = false;
      relayout();
      return KbAction::LayerChanged;
  }
  return KbAction::None;
}

KbAction KeyboardModel::typeText(const std::string& s) {
  KbAction last = KbAction::None;
  const char* p = s.c_str();
  while (*p) {
    const char* start = p;
    utf8Next(p);
    last = insert(std::string(start, p));
    if (last == KbAction::Rejected) return last;
  }
  return last;
}

int KeyboardModel::hit(int x, int y) const {
  for (size_t i = 0; i < keys_.size(); i++)
    if (keys_[i].r.contains(x, y)) return static_cast<int>(i);
  return -1;
}

std::string KeyboardModel::display() const {
  if (!password_) return text_;
  return std::string(codepoints(), '*');
}

void KeyboardModel::moveFocus(int d) {
  const int n = static_cast<int>(keys_.size());
  focus_ = ((focus_ + d) % n + n) % n;
}

void drawKeyboard(Canvas& c, const Theme& t, const KeyboardModel& kb, const std::string& title, const std::string& hint,
                  bool showFocus) {
  c.textIn(*t.title, Rect(t.margin, 0, Canvas::kW - 2 * t.margin - 200, 54), title, Align::Left, Color::Black);
  c.fillRect(Rect(0, 53, Canvas::kW, 3), Color::Black);
  const Rect& f = kbFieldRect();
  c.roundRect(f, 10, 3, Color::Black);
  std::string shown = kb.display();
  const Font& ff = *t.title;
  const int infoW = 190;  // hint + byte counter on the right of the field
  const int maxW = f.w - 40 - infoW;
  // show the end of long text (cursor is always at the end)
  while (!shown.empty() && textWidth(ff, shown) > maxW) {
    const char* p = shown.c_str();
    utf8Next(p);
    shown.erase(0, static_cast<size_t>(p - shown.c_str()));
  }
  const int tw = textWidth(ff, shown);
  c.textIn(ff, Rect(f.x + 16, f.y, maxW, f.h), shown, Align::Left, Color::Black);
  c.fillRect(Rect(f.x + 18 + tw, f.y + 16, 4, f.h - 32), Color::Black);  // cursor
  char cnt[24];
  snprintf(cnt, sizeof(cnt), "%u/%u", static_cast<unsigned>(kb.text().size()), static_cast<unsigned>(kb.maxBytes()));
  const Rect info(f.right() - infoW - 12, f.y + 6, infoW, f.h - 12);
  c.textIn(*t.small, Rect(info.x, info.y, info.w, info.h / 2), hint, Align::Right, Color::Black);
  c.textIn(*t.small, Rect(info.x, info.y + info.h / 2, info.w, info.h / 2), cnt, Align::Right, Color::Black);
  for (size_t i = 0; i < kb.keys().size(); i++) {
    const Key& k = kb.keys()[i];
    const Rect r = k.r.inset(3);
    const bool special = k.kind != KeyKind::Char && k.kind != KeyKind::Space;
    const bool active = (k.kind == KeyKind::Shift && (kb.layer() == KbLayer::Upper || kb.layer() == KbLayer::AccentsUpper)) || k.kind == KeyKind::Done;
    if (active) c.fillRoundRect(r, 8, Color::Black);
    else c.roundRect(r, 8, special ? 3 : 2, Color::Black);
    c.textIn(special ? *t.bodyBold : *t.title, r, k.label, Align::Center, active ? Color::White : Color::Black);
    if (showFocus && static_cast<int>(i) == kb.focus()) c.rect(k.r.inset(0), 4, active ? Color::Black : Color::Black);
  }
}

}  // namespace ui
