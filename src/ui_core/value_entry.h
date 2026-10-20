#pragma once
// Number / date / time entry shared by the form host (Number, Time, Date, DateTime fields),
// the forced clock screen and the "Taken at" backfill screen.
//
// Each value is split into fields (DD MM YYYY, HH : MM, or one number). Every field is a big
// box with large up/down arrows above and below it (84 px = 9.1 mm). Buttons: UP/DOWN change
// the focused field, OK moves to the next field. Tapping a box opens a number pad; typing
// auto-advances after 2 digits (4 for the year, or earlier when no further digit can fit).
// The day is clamped to the month length (leap years included).
#include <cstdint>
#include <string>
#include <vector>

#include "core/tz.h"
#include "ui_core/canvas.h"
#include "ui_core/input.h"
#include "ui_core/theme.h"

namespace ui {

enum class EntryKind : uint8_t { Number, Date, Time, DateTime };

struct EntryField {
  int value = 0;
  int min = 0, max = 0;
  int step = 1;
  int digits = 2;
  bool wrap = false;
  std::string label;
};

struct DateTimeValue {
  int32_t day = 0;   // local day number
  int minute = 0;    // minute of day
};

// "Oct 19 2026" / "00:22:11" (the __DATE__ / __TIME__ formats). Returns false if unparsable.
bool parseBuildDateTime(const char* date, const char* time, DateTimeValue& out);
// Default for clock entry: the RTC value when it is valid, otherwise the firmware build time.
DateTimeValue defaultClockValue(bool rtcValid, const core::LocalTime& now, const char* buildDate, const char* buildTime);
// The firmware's own build date/time (__DATE__ / __TIME__ of this module).
const char* firmwareBuildDate();
const char* firmwareBuildTime();

class ValueEntry {
 public:
  static ValueEntry number(int value, int min, int max, int step, const std::string& unit);
  static ValueEntry date(int32_t day, int minYear = 2020, int maxYear = 2099);
  static ValueEntry time(int minute, int minuteStep = 1);
  static ValueEntry dateTime(int32_t day, int minute, int minYear = 2025, int maxYear = 2099);
  ValueEntry() = default;

  EntryKind kind() const { return kind_; }
  int count() const { return static_cast<int>(f_.size()); }
  const EntryField& field(int i) const { return f_[static_cast<size_t>(i)]; }
  int focus() const { return focus_; }
  bool onDone() const { return focus_ >= count(); }  // focus is on the Save/Done button
  void setFocus(int i);

  // OK: finishes typing and moves to the next field. Returns true when focus moved past the last field.
  bool next();
  void stepFocused(int dir);  // dir +1 = up
  void step(int i, int dir);

  void beginTyping(int i);
  bool typing() const { return typingField_ >= 0; }
  int typingField() const { return typingField_; }
  const std::string& buffer() const { return buf_; }
  void typeDigit(int d);
  void backspace();
  void toggleSign();          // numbers with a negative minimum
  void finishTyping();        // commits a partial entry
  bool signedNumber() const { return kind_ == EntryKind::Number && f_[0].min < 0; }

  int32_t day() const;
  int minute() const;
  int number() const { return f_.empty() ? 0 : f_[0].value; }
  // Number: value; Date: day; Time: minute; DateTime: day * 1440 + minute.
  int encoded() const;
  std::string fieldText(int i) const;  // what the box shows (typing buffer with "_" for missing digits)
  std::string summary() const;         // "Mon 19 Oct 2026", "08:05", "Mon 19 Oct 2026, 08:05", "20 min"

 private:
  void clampDay();
  void commit(int i, int v);
  void advanceAfterCommit(int i);
  EntryKind kind_ = EntryKind::Number;
  std::vector<EntryField> f_;
  int focus_ = 0;
  int typingField_ = -1;
  std::string buf_;
  bool neg_ = false;
  std::string unit_;
  int iD_ = -1, iM_ = -1, iY_ = -1, iH_ = -1, iMin_ = -1;
  int wantedDay_ = 1;  // the day the user chose; re-applied (clamped) when the month or year changes
};

// Hit ids relative to the caller's base.
constexpr int kEntryUp0 = 0, kEntryDown0 = 10, kEntryBox0 = 20, kEntryKey0 = 30 /* +digit */, kEntryDel = 40,
              kEntryOk = 41, kEntrySign = 42, kEntrySpan = 50;
constexpr int kEntryArrowH = 84;  // 9.1 mm at 235 ppi
constexpr int kEntryBoxH = 100;
constexpr int kEntryHeight = 330; // vertical space drawValueEntry needs below `top`

struct EntryLayout {
  Rect box[5], up[5], down[5];
  Rect summary;
  std::vector<std::pair<int, Rect>> keys;  // (local id, rect) of the number pad (typing only)
};
EntryLayout layoutValueEntry(const ValueEntry& e, int top);
void drawValueEntry(Canvas& c, const Theme& t, const ValueEntry& e, int top, HitMap& hm, int hitBase);
// Applies a tap on one of the entry's targets (id relative to the base). Returns false if not an entry id.
bool handleEntryHit(ValueEntry& e, int localId);

}  // namespace ui
