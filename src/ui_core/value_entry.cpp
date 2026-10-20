#include "ui_core/value_entry.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "app_core/format.h"
#include "core/civil.h"
#include "ui_core/strings.h"
#include "ui_core/text.h"
#include "ui_core/widgets.h"

namespace ui {
namespace {

int digitsFor(int v) {
  v = v < 0 ? -v : v;
  int d = 1;
  while (v >= 10) {
    v /= 10;
    ++d;
  }
  return d;
}

int clampInt(int v, int lo, int hi) { return std::min(std::max(v, lo), hi); }

EntryField makeField(int v, int mn, int mx, int digits, bool wrap, const std::string& label, int step = 1) {
  EntryField f;
  f.min = mn;
  f.max = mx;
  f.value = clampInt(v, mn, mx);
  f.digits = digits;
  f.wrap = wrap;
  f.label = label;
  f.step = std::max(1, step);
  return f;
}

// Filled triangle pointing up (dir > 0) or down, centred in r.
void drawArrow(Canvas& c, const Rect& r, int dir, Color col) {
  const int h = std::min(r.h - 28, 36), cx = r.cx(), cy = r.cy();
  for (int i = 0; i < h; ++i) {
    const int half = i * 5 / 4;
    const int y = dir > 0 ? cy - h / 2 + i : cy + h / 2 - i;
    c.hline(cx - half, cx + half, y, 1, col);
  }
}

}  // namespace

const char* firmwareBuildDate() { return __DATE__; }
const char* firmwareBuildTime() { return __TIME__; }

bool parseBuildDateTime(const char* date, const char* time, DateTimeValue& out) {
  static const char* const kMon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  if (!date || !time) return false;
  char mon[4] = {0};
  int d = 0, y = 0, hh = 0, mm = 0, ss = 0;
  if (std::sscanf(date, "%3s %d %d", mon, &d, &y) != 3) return false;
  if (std::sscanf(time, "%d:%d:%d", &hh, &mm, &ss) < 2) return false;
  int m = 0;
  for (int i = 0; i < 12; ++i)
    if (std::strcmp(mon, kMon[i]) == 0) m = i + 1;
  if (m == 0 || y < 2000 || y > 2099 || d < 1 || d > core::daysInMonth(y, m) || hh < 0 || hh > 23 || mm < 0 || mm > 59)
    return false;
  out.day = core::daysFromCivil(y, m, d);
  out.minute = hh * 60 + mm;
  return true;
}

DateTimeValue defaultClockValue(bool rtcValid, const core::LocalTime& now, const char* buildDate, const char* buildTime) {
  DateTimeValue v;
  if (rtcValid && now.days >= core::daysFromCivil(2025, 1, 1)) {
    v.day = now.days;
    v.minute = now.secOfDay / 60;
    return v;
  }
  if (parseBuildDateTime(buildDate, buildTime, v)) return v;
  v.day = core::daysFromCivil(2026, 1, 1);
  v.minute = 8 * 60;
  return v;
}

// ------------------------------------------------------------------ model
ValueEntry ValueEntry::number(int value, int mn, int mx, int step, const std::string& unit) {
  ValueEntry e;
  e.kind_ = EntryKind::Number;
  e.unit_ = unit;
  e.f_.push_back(makeField(value, mn, mx, std::max(digitsFor(mn), digitsFor(mx)), false, unit, step));
  return e;
}

ValueEntry ValueEntry::date(int32_t day, int minYear, int maxYear) {
  ValueEntry e;
  e.kind_ = EntryKind::Date;
  const core::Ymd y = core::civilFromDays(day);
  e.iD_ = 0;
  e.iM_ = 1;
  e.iY_ = 2;
  e.f_.push_back(makeField(y.d, 1, 31, 2, true, tr(S::FieldDay)));
  e.f_.push_back(makeField(y.m, 1, 12, 2, true, tr(S::FieldMonth)));
  e.f_.push_back(makeField(y.y, minYear, maxYear, 4, false, tr(S::FieldYear)));
  e.wantedDay_ = y.d;
  e.clampDay();
  return e;
}

ValueEntry ValueEntry::time(int minute, int minuteStep) {
  ValueEntry e;
  e.kind_ = EntryKind::Time;
  minute = ((minute % 1440) + 1440) % 1440;
  e.iH_ = 0;
  e.iMin_ = 1;
  e.f_.push_back(makeField(minute / 60, 0, 23, 2, true, tr(S::FieldHour)));
  e.f_.push_back(makeField(minute % 60, 0, 59, 2, true, tr(S::FieldMinute), minuteStep));
  return e;
}

ValueEntry ValueEntry::dateTime(int32_t day, int minute, int minYear, int maxYear) {
  ValueEntry e = date(day, minYear, maxYear);
  const ValueEntry t = time(minute);
  e.kind_ = EntryKind::DateTime;
  e.iH_ = 3;
  e.iMin_ = 4;
  e.f_.push_back(t.f_[0]);
  e.f_.push_back(t.f_[1]);
  return e;
}

void ValueEntry::setFocus(int i) {
  finishTyping();
  focus_ = clampInt(i, 0, count());
}

void ValueEntry::clampDay() {
  if (iD_ < 0) return;
  EntryField& d = f_[static_cast<size_t>(iD_)];
  d.max = core::daysInMonth(f_[static_cast<size_t>(iY_)].value, f_[static_cast<size_t>(iM_)].value);
  d.value = clampInt(wantedDay_, 1, d.max);
}

void ValueEntry::commit(int i, int v) {
  EntryField& f = f_[static_cast<size_t>(i)];
  if (i == iD_) {
    wantedDay_ = clampInt(v, 1, 31);  // the month may still change; clampDay() applies the real length
    clampDay();
    return;
  }
  f.value = clampInt(v, f.min, f.max);
  clampDay();
}

bool ValueEntry::next() {
  if (typing()) {
    const int i = typingField_;
    finishTyping();
    focus_ = i;
  }
  if (focus_ < count()) focus_++;
  return focus_ >= count();
}

void ValueEntry::stepFocused(int dir) {
  if (focus_ < count()) step(focus_, dir);
}

void ValueEntry::step(int i, int dir) {
  if (i < 0 || i >= count()) return;
  finishTyping();
  focus_ = i;
  EntryField& f = f_[static_cast<size_t>(i)];
  int v = f.value;
  if (dir > 0) v = (v / f.step + 1) * f.step;
  else v = ((v + f.step - 1) / f.step - 1) * f.step;
  if (f.wrap) {
    if (v > f.max) v = f.min;
    if (v < f.min) v = (f.max / f.step) * f.step;
  }
  f.value = clampInt(v, f.min, f.max);
  if (i == iD_) wantedDay_ = f.value;
  clampDay();
}

void ValueEntry::beginTyping(int i) {
  if (i < 0 || i >= count()) return;
  finishTyping();
  focus_ = i;
  typingField_ = i;
  buf_.clear();
  neg_ = false;
}

void ValueEntry::advanceAfterCommit(int i) {
  typingField_ = -1;
  buf_.clear();
  neg_ = false;
  if (i + 1 < count()) beginTyping(i + 1);
  else focus_ = count();
}

void ValueEntry::typeDigit(int d) {
  if (!typing() || d < 0 || d > 9) return;
  const int i = typingField_;
  const EntryField& f = f_[static_cast<size_t>(i)];
  if (static_cast<int>(buf_.size()) >= f.digits) return;
  buf_.push_back(static_cast<char>('0' + d));
  const int v = std::atoi(buf_.c_str());
  const bool full = static_cast<int>(buf_.size()) >= f.digits;
  // Auto-advance early when no further digit could still give a valid value (e.g. month "4").
  const int limit = i == iD_ ? 31 : f.max;
  const bool early = kind_ != EntryKind::Number && i != iY_ && v * 10 > limit;
  if (full || early) {
    commit(i, neg_ ? -v : v);
    advanceAfterCommit(i);
  }
}

void ValueEntry::backspace() {
  if (typing() && !buf_.empty()) buf_.pop_back();
}

void ValueEntry::toggleSign() {
  if (typing() && signedNumber()) neg_ = !neg_;
}

void ValueEntry::finishTyping() {
  if (!typing()) return;
  const int i = typingField_;
  if (!buf_.empty()) {
    const int v = std::atoi(buf_.c_str());
    commit(i, neg_ ? -v : v);
  }
  typingField_ = -1;
  buf_.clear();
  neg_ = false;
}

int32_t ValueEntry::day() const {
  if (iD_ < 0) return 0;
  return core::daysFromCivil(f_[static_cast<size_t>(iY_)].value, f_[static_cast<size_t>(iM_)].value,
                             f_[static_cast<size_t>(iD_)].value);
}

int ValueEntry::minute() const {
  if (iH_ < 0) return 0;
  return f_[static_cast<size_t>(iH_)].value * 60 + f_[static_cast<size_t>(iMin_)].value;
}

int ValueEntry::encoded() const {
  switch (kind_) {
    case EntryKind::Number: return number();
    case EntryKind::Date: return day();
    case EntryKind::Time: return minute();
    case EntryKind::DateTime: return day() * 1440 + minute();
  }
  return 0;
}

std::string ValueEntry::fieldText(int i) const {
  const EntryField& f = f_[static_cast<size_t>(i)];
  if (i == typingField_) {
    std::string s = (neg_ ? "-" : "") + buf_;
    for (int k = static_cast<int>(buf_.size()); k < f.digits; ++k) s += "_";
    return s;
  }
  if (kind_ == EntryKind::Number) return app::fmt("%d", f.value);
  return f.digits == 4 ? app::fmt("%04d", f.value) : app::fmt("%02d", f.value);
}

std::string ValueEntry::summary() const {
  auto dateText = [&]() { return app::fmtShortDate(day()) + app::fmt(" %d", f_[static_cast<size_t>(iY_)].value); };
  switch (kind_) {
    case EntryKind::Number: return unit_.empty() ? app::fmt("%d", number()) : app::fmt("%d %s", number(), unit_.c_str());
    case EntryKind::Date: return dateText();
    case EntryKind::Time: return app::fmtHm(minute());
    case EntryKind::DateTime: return dateText() + ", " + app::fmtHm(minute());
  }
  return "";
}

// ------------------------------------------------------------------ layout / drawing
EntryLayout layoutValueEntry(const ValueEntry& e, int top) {
  EntryLayout L;
  const int n = e.count();
  int widths[5] = {0}, gaps[5] = {0};
  int total = 0;
  for (int i = 0; i < n; ++i) {
    widths[i] = e.kind() == EntryKind::Number ? 260 : e.field(i).digits == 4 ? 224 : 116;  // 40 pt digits: "00" 108 px, "0000" 216 px
    if (i > 0) {
      const bool timeStart = e.kind() == EntryKind::DateTime && i == 3;
      const bool colon = (e.kind() == EntryKind::Time && i == 1) || (e.kind() == EntryKind::DateTime && i == 4);
      gaps[i] = timeStart ? 40 : colon ? 32 : 12;
    }
    total += widths[i] + gaps[i];
  }
  int x = (Canvas::kW - total) / 2;
  const bool typing = e.typing();
  const int boxY = typing ? top : top + kEntryArrowH + 4;
  for (int i = 0; i < n; ++i) {
    x += gaps[i];
    L.box[i] = Rect(x, boxY, widths[i], kEntryBoxH);
    L.up[i] = Rect(x, top, widths[i], kEntryArrowH);
    L.down[i] = Rect(x, boxY + kEntryBoxH + 4, widths[i], kEntryArrowH);
    x += widths[i];
  }
  if (!typing) {
    L.summary = Rect(16, top + 2 * kEntryArrowH + kEntryBoxH + 10, Canvas::kW - 32, 40);
    return L;
  }
  L.summary = Rect(16, top + kEntryBoxH + 4, Canvas::kW - 32, 40);
  std::vector<int> row1 = {1, 2, 3, 4, 5, 6}, row2 = {7, 8, 9, 0};
  std::vector<int> ids1, ids2;
  for (int d : row1) ids1.push_back(kEntryKey0 + d);
  for (int d : row2) ids2.push_back(kEntryKey0 + d);
  ids2.push_back(kEntryDel);
  ids2.push_back(kEntryOk);
  if (e.signedNumber()) ids1.push_back(kEntrySign);
  const int perRow = static_cast<int>(std::max(ids1.size(), ids2.size()));
  const int gap = 8, kw = (Canvas::kW - 32 - (perRow - 1) * gap) / perRow;
  const int y1 = top + kEntryBoxH + 48, y2 = y1 + kEntryArrowH + 8;
  for (size_t k = 0; k < ids1.size(); ++k)
    L.keys.push_back({ids1[k], Rect(16 + static_cast<int>(k) * (kw + gap), y1, kw, kEntryArrowH)});
  for (size_t k = 0; k < ids2.size(); ++k)
    L.keys.push_back({ids2[k], Rect(16 + static_cast<int>(k) * (kw + gap), y2, kw, kEntryArrowH)});
  return L;
}

void drawValueEntry(Canvas& c, const Theme& t, const ValueEntry& e, int top, HitMap& hm, int hitBase) {
  const EntryLayout L = layoutValueEntry(e, top);
  const int n = e.count();
  for (int i = 0; i < n; ++i) {
    const bool f = e.focus() == i;
    const Rect& b = L.box[i];
    hm.add(hitBase + kEntryBox0 + i, b, false);
    if (f) c.fillRoundRect(b, t.radius, Color::Black);
    else c.roundRect(b, t.radius, t.border, Color::Black);
    const Color fg = f ? Color::White : Color::Black;
    const std::string txt = e.fieldText(i);
    const Font* vf = textWidth(*t.huge, txt) <= b.w - 6 ? t.huge : t.title;  // large-text theme: keep it inside the box
    c.textIn(*vf, Rect(b.x, b.y + 4, b.w, 64), txt, Align::Center, fg);
    c.textIn(*t.small, Rect(b.x, b.y + 66, b.w, 30), e.field(i).label, Align::Center, fg);
    const bool colon = (e.kind() == EntryKind::Time && i == 1) || (e.kind() == EntryKind::DateTime && i == 4);
    if (colon) c.textIn(*t.huge, Rect(b.x - 32, b.y + 4, 32, 64), ":", Align::Center, Color::Black);
    if (!e.typing()) {
      hm.add(hitBase + kEntryUp0 + i, L.up[i], false);
      hm.add(hitBase + kEntryDown0 + i, L.down[i], false);
      c.roundRect(L.up[i], t.radius, 2, Color::Black);
      c.roundRect(L.down[i], t.radius, 2, Color::Black);
      drawArrow(c, L.up[i], 1, Color::Black);
      drawArrow(c, L.down[i], -1, Color::Black);
    }
  }
  c.textIn(*t.bodyBold, L.summary, e.summary(), Align::Center, Color::Black);
  for (const auto& k : L.keys) {
    hm.add(hitBase + k.first, k.second, false);
    std::string label;
    if (k.first == kEntryDel) label = tr(S::KeyDelete);
    else if (k.first == kEntryOk) label = tr(S::KeyOk);
    else if (k.first == kEntrySign) label = "+/-";
    else label = app::fmt("%d", k.first - kEntryKey0);
    drawButton(c, t, k.second, label, IconId::None, k.first == kEntryOk ? BtnStyle::Primary : BtnStyle::Normal, false, 0,
               t.title);
  }
}

bool handleEntryHit(ValueEntry& e, int id) {
  if (id >= kEntryUp0 && id < kEntryUp0 + e.count()) {
    e.step(id - kEntryUp0, 1);
    return true;
  }
  if (id >= kEntryDown0 && id < kEntryDown0 + e.count()) {
    e.step(id - kEntryDown0, -1);
    return true;
  }
  if (id >= kEntryBox0 && id < kEntryBox0 + e.count()) {
    e.beginTyping(id - kEntryBox0);
    return true;
  }
  if (id >= kEntryKey0 && id <= kEntryKey0 + 9) {
    e.typeDigit(id - kEntryKey0);
    return true;
  }
  if (id == kEntryDel) {
    e.backspace();
    return true;
  }
  if (id == kEntrySign) {
    e.toggleSign();
    return true;
  }
  if (id == kEntryOk) {
    const int i = e.typingField();
    e.finishTyping();
    e.setFocus(i + 1);
    return true;
  }
  return false;
}

}  // namespace ui
