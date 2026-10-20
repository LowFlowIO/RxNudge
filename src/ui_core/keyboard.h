#pragma once
// On-screen keyboard: pure model (layouts, UTF-8 editing, focus) plus rendering.
// Layout: title bar 0..72, text field 84..176, keys 192..480 (4 rows x 72 px, 80 px
// wide letter keys). A keypress changes only the field and, when the layer changes,
// the key area, so the diff-based refresh updates just those regions (FAST refresh).
#include <cstdint>
#include <string>
#include <vector>

#include "ui_core/canvas.h"
#include "ui_core/geometry.h"
#include "ui_core/theme.h"

namespace ui {

// US QWERTY letters; accented letters (UTF-8) live on the Accents layers reached from Symbols.
enum class KbLayer : uint8_t { Lower, Upper, Symbols, Numeric, Accents, AccentsUpper };
enum class KeyKind : uint8_t { Char, Shift, Layer, Space, Backspace, Done, Cancel };
enum class KbAction : uint8_t { None, Edited, LayerChanged, Done, Cancel, Rejected };

struct Key {
  KeyKind kind;
  std::string label;
  std::string text;  // for Char
  Rect r;
};

constexpr int kKbTop = 144;  // 4 rows x 84 px (9.1 mm tall); 10 keys x 80 px fill the 800 px width (8.65 mm)
constexpr int kKbRowH = 84;
const Rect& kbFieldRect();  // the text field
const Rect& kbKeysRect();   // the whole key area

class KeyboardModel {
 public:
  KeyboardModel() { relayout(); }
  KeyboardModel(const std::string& initial, size_t maxBytes, bool numeric, bool password);
  KbAction press(size_t keyIndex);
  KbAction typeText(const std::string& utf8);  // test helper: types each character
  KbAction backspace();
  const std::vector<Key>& keys() const { return keys_; }
  int hit(int x, int y) const;  // key index or -1
  const std::string& text() const { return text_; }
  std::string display() const;  // masked for passwords
  KbLayer layer() const { return layer_; }
  bool numeric() const { return numeric_; }
  size_t maxBytes() const { return max_; }
  size_t codepoints() const;
  // Button navigation
  int focus() const { return focus_; }
  void moveFocus(int delta);
  KbAction pressFocused() { return press(static_cast<size_t>(focus_)); }

 private:
  void relayout();
  KbAction insert(const std::string& s);
  std::string text_;
  size_t max_ = 48;
  bool numeric_ = false, password_ = false, shiftOnce_ = false;
  KbLayer layer_ = KbLayer::Lower;
  std::vector<Key> keys_;
  int focus_ = 0;
};

// Removes the last UTF-8 code point.
void utf8PopBack(std::string& s);

void drawKeyboard(Canvas& c, const Theme& t, const KeyboardModel& kb, const std::string& title, const std::string& hint,
                  bool showFocus);

}  // namespace ui
