// Phase 3: pure UI logic, view models, refresh policy, RTC codec and controller flows.
#include <unity.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "../common/fixtures.h"
#include "app_core/app_port.h"
#include "app_core/format.h"
#include "core/demo_data.h"
#include "core/rtc.h"
#include "ui_core/canvas.h"
#include "ui_core/controller.h"
#include "ui_core/input.h"
#include "ui_core/refresh.h"
#include "ui_core/text.h"
#include "ui_core/theme.h"
#include "ui_core/assets/fonts.h"
#include "ui_core/icons.h"
#include "ui_core/strings.h"
#include "ui_core/widgets.h"
#include "ui_core/value_entry.h"

using namespace ui;

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------- rig
struct Rig {
  core::FakeClock clk;
  core::Database db;
  core::MemoryDoseLog persist;
  std::unique_ptr<app::AppModel> model;
  std::unique_ptr<app::ModelPort> port;
  std::unique_ptr<UiController> ui;
  uint8_t buf[Canvas::kBytes];
  Canvas canvas{buf};
  uint32_t ms = 1000;

  static bool setClock(void* ctx, int64_t utc) {
    Rig* r = static_cast<Rig*>(ctx);
    r->clk.set(utc);
    r->model->status.clockUnset = false;
    return true;
  }

  explicit Rig(int64_t now, bool history = true) : clk(now) {
    const core::LocalTime lt = core::utcToLocal(fx::berlin(), now);
    db = core::makeDemoDatabase(lt.days - 20);
    model.reset(new app::AppModel(clk, db, &persist));
    if (history) model->setRecentLog(core::makeDemoLog(db, model->tz(), now - 3600, 14));
    clk.set(now - 3600);  // engine starts an hour earlier so doses become due naturally
    model->startEngine(now - 3600);
    model->process();
    clk.set(now);
    model->process();
    port.reset(new app::ModelPort(*model, &Rig::setClock, this));
    ui.reset(new UiController(*port));
  }
  void draw() { ui->render(canvas); }
  void step(uint32_t dt = 100) {
    ms += dt;
    model->process();
    ui->tick(ms);
    draw();
  }
  bool tapId(int id) {
    draw();
    const Rect* r = ui->hits().rectOf(id);
    if (!r) return false;
    InputEvent e;
    e.type = InputType::Tap;
    e.x = static_cast<int16_t>(r->cx());
    e.y = static_cast<int16_t>(r->cy());
    ms += 300;
    ui->handle(e, ms);
    model->process();
    draw();
    return true;
  }
  void key(InputType t, uint16_t permille = 0) {
    InputEvent e;
    e.type = t;
    e.permille = permille;
    ms += 50;
    ui->handle(e, ms);
    model->process();
    draw();
  }
  int focusedId() const { return ui->hits().focusableId(ui->focusIndex()); }
};

static int64_t berlin(int d, int h, int m = 0) { return fx::berlinLocal(2026, 10, d, h, m); }

// ---------------------------------------------------------------- text / theme / canvas
void test_text_metrics() {
  TEST_ASSERT_TRUE(textWidth(atk18r, "Hello") > 30);
  TEST_ASSERT_TRUE(textWidth(atk24b, "Hello") > textWidth(atk18r, "Hello"));
  const std::string f = fitText(atk18r, "A very long medication name that cannot fit", 150);
  TEST_ASSERT_TRUE(textWidth(atk18r, f) <= 150);
  std::vector<std::string> lines;
  wrapText(atk18r, "Take with food and a full glass of water please", 200, 5, lines);
  TEST_ASSERT_TRUE(lines.size() >= 2);
  for (const auto& l : lines) TEST_ASSERT_TRUE(textWidth(atk18r, l) <= 200);
}

void test_theme_sizes() {
  TEST_ASSERT_TRUE(kMinTouchPx >= 83);
  TEST_ASSERT_TRUE(normalTheme().touch >= kMinTouchPx);
  // body at least 18 pt: cap height of 'H' >= ~24 px at 235 ppi
  TEST_ASSERT_TRUE(normalTheme().body->ascent >= largeTheme().small->ascent - 1);
  TEST_ASSERT_TRUE(largeTheme().body->ascent > normalTheme().body->ascent);
}

void test_canvas_basics() {
  uint8_t b[Canvas::kBytes];
  Canvas c(b);
  c.clear(Color::White);
  TEST_ASSERT_FALSE(c.isBlack(10, 10));
  c.fillRect(Rect(8, 8, 16, 4), Color::Black);
  TEST_ASSERT_TRUE(c.isBlack(10, 10));
  TEST_ASSERT_FALSE(c.isBlack(24, 10));
  c.pixel(-1, 5000, Color::Black);  // clipped, no crash
  c.text(atk18r, 100, 100, "Ab", Color::Black);
  int black = 0;
  for (int y = 70; y < 110; y++)
    for (int x = 100; x < 140; x++) black += c.isBlack(x, y);
  TEST_ASSERT_TRUE(black > 20);
}

// ---------------------------------------------------------------- input
void test_button_click_and_hold() {
  ButtonDecoder d;
  InputEvent ev[8];
  uint32_t t = 0;
  int n = 0;
  for (; t < 100; t += 10) n += d.update(t, false, false, true, ev, 8);
  n = d.update(t, false, false, false, ev, 8);
  TEST_ASSERT_EQUAL(1, n);
  TEST_ASSERT_EQUAL(static_cast<int>(InputType::OkClick), static_cast<int>(ev[0].type));
  // hold for 1.1 s -> progress, then HoldDone, no click on release
  bool done = false, progress = false;
  for (t += 100; t < 1400; t += 10) {
    n = d.update(t, false, false, true, ev, 8);
    for (int i = 0; i < n; i++) {
      if (ev[i].type == InputType::OkHold) progress = true;
      if (ev[i].type == InputType::OkHoldDone) done = true;
      TEST_ASSERT_TRUE(ev[i].type != InputType::OkClick);
    }
  }
  TEST_ASSERT_TRUE(progress);
  TEST_ASSERT_TRUE(done);
  n = d.update(t, false, false, false, ev, 8);
  for (int i = 0; i < n; i++) TEST_ASSERT_TRUE(ev[i].type != InputType::OkClick);
}

void test_button_repeat_and_combo() {
  ButtonDecoder d;
  InputEvent ev[8];
  int downs = 0;
  uint32_t t = 0;
  for (; t <= 1500; t += 10) {
    const int n = d.update(t, false, true, false, ev, 8);
    for (int i = 0; i < n; i++) downs += ev[i].type == InputType::Down;
  }
  TEST_ASSERT_TRUE(downs >= 4);  // first + repeats after 600 ms every 200 ms
  d.update(t += 10, false, false, false, ev, 8);
  bool combo = false;
  for (uint32_t s = t + 10; s < t + 3300; s += 10) {
    const int n = d.update(s, true, true, false, ev, 8);
    for (int i = 0; i < n; i++) combo |= ev[i].type == InputType::Combo;
  }
  TEST_ASSERT_TRUE(combo);
}

void test_hitmap() {
  HitMap h;
  h.add(1, Rect(0, 0, 100, 100));
  h.add(2, Rect(300, 300, 40, 40));
  h.add(3, Rect(0, 0, 50, 50), false);
  TEST_ASSERT_EQUAL(3, h.hit(10, 10));  // top-most exact
  TEST_ASSERT_EQUAL(1, h.hit(80, 80));
  TEST_ASSERT_EQUAL(2, h.hit(310, 290));  // small target grown to the minimum touch size
  TEST_ASSERT_EQUAL(-1, h.hit(700, 50));
  TEST_ASSERT_EQUAL(2, h.focusableCount());
  TEST_ASSERT_EQUAL(2, h.focusableId(1));
  TEST_ASSERT_EQUAL(-1, h.focusIndexOf(3));
}

// ---------------------------------------------------------------- refresh
void test_diff_frames() {
  static uint8_t a[Canvas::kBytes], b[Canvas::kBytes];
  memset(a, 0xFF, sizeof(a));
  memcpy(b, a, sizeof(b));
  std::vector<Rect> rs;
  diffFrames(a, b, rs);
  TEST_ASSERT_EQUAL(0, static_cast<int>(rs.size()));
  Canvas c(b);
  c.fillRect(Rect(13, 20, 5, 5), Color::Black);
  c.fillRect(Rect(600, 400, 30, 30), Color::Black);
  diffFrames(a, b, rs);
  TEST_ASSERT_EQUAL(2, static_cast<int>(rs.size()));
  for (const Rect& r : rs) {
    TEST_ASSERT_EQUAL(0, r.x % 8);
    TEST_ASSERT_EQUAL(0, r.w % 8);
  }
  TEST_ASSERT_TRUE(rs[0].contains(13, 20) || rs[1].contains(13, 20));
  const Rect al = alignToBytes(Rect(3, 0, 6, 10));
  TEST_ASSERT_EQUAL(0, al.x);
  TEST_ASSERT_EQUAL(16, al.w);
}

void test_refresh_policy() {
  RefreshPolicy p(3, 180);
  const int32_t d = fx::day(2026, 10, 18);
  TEST_ASSERT_EQUAL(static_cast<int>(RefreshKind::Full), static_cast<int>(p.decide(true, 0, d, 600)));
  TEST_ASSERT_EQUAL(static_cast<int>(RefreshKind::None), static_cast<int>(p.decide(false, 0, d, 601)));
  TEST_ASSERT_EQUAL(static_cast<int>(RefreshKind::Partial), static_cast<int>(p.decide(true, 0, d, 602)));
  TEST_ASSERT_EQUAL(static_cast<int>(RefreshKind::Half), static_cast<int>(p.decide(true, 1, d, 603)));
  p.decide(true, 1, d, 604);
  p.decide(true, 1, d, 605);
  TEST_ASSERT_EQUAL(static_cast<int>(RefreshKind::Full), static_cast<int>(p.decide(true, 1, d, 606)));
  // nightly: first decision at/after 03:00 the next day is a full refresh, once
  TEST_ASSERT_EQUAL(static_cast<int>(RefreshKind::Full), static_cast<int>(p.decide(true, 1, d + 1, 185)));
  TEST_ASSERT_EQUAL(static_cast<int>(RefreshKind::Partial), static_cast<int>(p.decide(true, 1, d + 1, 186)));
  TEST_ASSERT_EQUAL(static_cast<int>(RefreshKind::Full), static_cast<int>(p.decide(true, 1, d + 1, 187, true)));
}

void test_latency_stats() {
  LatencyStats s;
  for (uint32_t i = 1; i <= 100; i++) s.add(i * 10);
  TEST_ASSERT_EQUAL(100u, s.count());
  TEST_ASSERT_EQUAL(10u, s.minMs());
  TEST_ASSERT_EQUAL(1000u, s.maxMs());
  TEST_ASSERT_EQUAL(505u, s.avgMs());
  TEST_ASSERT_TRUE(s.p90Ms() >= 960 && s.p90Ms() <= 1000);
}

// ---------------------------------------------------------------- rtc / clocks
void test_pcf8563_codec() {
  uint8_t raw[7];
  const int64_t t = fx::utc(2026, 10, 18, 21, 7, 42);
  core::encodePcf8563(t, raw);
  int64_t back = 0;
  bool vl = true;
  TEST_ASSERT_TRUE(core::decodePcf8563(raw, back, vl));
  TEST_ASSERT_FALSE(vl);
  TEST_ASSERT_EQUAL_INT64(t, back);
  raw[0] |= 0x80;
  TEST_ASSERT_TRUE(core::decodePcf8563(raw, back, vl));
  TEST_ASSERT_TRUE(vl);
  raw[2] = 0x25;  // hour 25
  TEST_ASSERT_FALSE(core::decodePcf8563(raw, back, vl));
}

void test_clock_trust() {
  const int64_t good = fx::utc(2026, 10, 18, 12);
  core::ClockDecision d = core::evaluateClock(true, false, good, 0);
  TEST_ASSERT_EQUAL(static_cast<int>(core::ClockTrust::Trusted), static_cast<int>(d.trust));
  TEST_ASSERT_TRUE(d.setSystemTime);
  d = core::evaluateClock(true, true, good, 0);  // VL set
  TEST_ASSERT_NOT_EQUAL(static_cast<int>(core::ClockTrust::Trusted), static_cast<int>(d.trust));
  d = core::evaluateClock(false, false, 0, 0);
  TEST_ASSERT_EQUAL(static_cast<int>(core::ClockTrust::Unset), static_cast<int>(d.trust));
  d = core::evaluateClock(false, false, 0, good);  // system time survived sleep
  TEST_ASSERT_EQUAL(static_cast<int>(core::ClockTrust::Untrusted), static_cast<int>(d.trust));
  TEST_ASSERT_EQUAL_INT64(good, d.utc);
}

void test_scaled_clock() {
  core::FakeClock src(1000);
  core::ScaledClock sc(src, 60, 1000);
  TEST_ASSERT_EQUAL_INT64(1000, sc.nowUtc());
  src.advance(10);
  TEST_ASSERT_EQUAL_INT64(1600, sc.nowUtc());
  TEST_ASSERT_EQUAL_INT64(1, sc.sourceSecondsUntil(1601));
  TEST_ASSERT_EQUAL_INT64(0, sc.sourceSecondsUntil(1500));
}

void test_local_to_utc_checked() {
  int64_t u = 0;
  TEST_ASSERT_TRUE(app::localToUtcChecked(fx::berlin(), 2026, 10, 18, 12, 30, u));
  TEST_ASSERT_EQUAL_INT64(fx::berlinLocal(2026, 10, 18, 12, 30), u);
  TEST_ASSERT_FALSE(app::localToUtcChecked(fx::berlin(), 2026, 2, 30, 12, 0, u));
  TEST_ASSERT_FALSE(app::localToUtcChecked(fx::berlin(), 2026, 1, 1, 24, 0, u));
}

// ---------------------------------------------------------------- view models
void test_demo_data_valid() {
  const core::Database db = core::makeDemoDatabase(fx::day(2026, 10, 1));
  TEST_ASSERT_EQUAL(2, static_cast<int>(db.profiles.size()));
  TEST_ASSERT_TRUE(db.meds.size() >= 8);
  const auto log = core::makeDemoLog(db, fx::berlin(), berlin(18, 7), 14);
  TEST_ASSERT_TRUE(log.size() > 20);
  const auto log2 = core::makeDemoLog(db, fx::berlin(), berlin(18, 7), 14);
  TEST_ASSERT_EQUAL(static_cast<int>(log.size()), static_cast<int>(log2.size()));  // deterministic
}

void test_banner_priority() {
  app::StatusFlags s;
  s.sdPresent = true;
  TEST_ASSERT_EQUAL(static_cast<int>(app::BannerKind::None), static_cast<int>(app::topBanner(s)));
  s.lowBattery = true;
  s.sdError = true;
  s.clockUnset = true;
  int more = 0;
  TEST_ASSERT_EQUAL(static_cast<int>(app::BannerKind::ClockUnset), static_cast<int>(app::topBanner(s, &more)));
  TEST_ASSERT_EQUAL(2, more);
}

void test_home_vm_privacy() {
  Rig r(berlin(14, 12));
  const app::HomeVM vm = r.model->home(0, 4);
  TEST_ASSERT_EQUAL(2, static_cast<int>(vm.lanes.size()));
  const app::LaneVM& anna = vm.lanes[0];
  const app::LaneVM& ben = vm.lanes[1];
  TEST_ASSERT_EQUAL_STRING("Anna", anna.title.c_str());
  TEST_ASSERT_FALSE(anna.privacy);
  TEST_ASSERT_TRUE(ben.privacy);
  TEST_ASSERT_TRUE(ben.title.size() <= 3);  // initials
  TEST_ASSERT_TRUE(ben.nextLabel.empty());
  TEST_ASSERT_TRUE(anna.total >= 2);
  TEST_ASSERT_EQUAL(1, vm.pageCount);
}

void test_layout_helpers() {
  TEST_ASSERT_EQUAL(4, homeLanesPerPage(false, false));
  TEST_ASSERT_EQUAL(3, homeLanesPerPage(true, false));
  TEST_ASSERT_EQUAL(2, homeLanesPerPage(true, true));
  TEST_ASSERT_TRUE(listRowsPerPage(84, 104) >= 2);
  TEST_ASSERT_EQUAL_STRING("08:05", app::fmtHm(8 * 60 + 5).c_str());
}

// ---------------------------------------------------------------- controller flows
static bool screenTouchTargetsOk(Rig& r) {
  for (int i = 0; i < r.ui->hits().count(); i++) {
    const Rect* rc = r.ui->hits().rectOf(r.ui->hits().idAt(i));
    if (rc->w < 60 || rc->h < 60) return false;
  }
  return true;
}

void test_alarm_one_person_and_two_step() {
  Rig r(berlin(14, 8, 1), false);
  r.step();
  TEST_ASSERT_EQUAL(static_cast<int>(ScreenId::Alarm), static_cast<int>(r.ui->screen()));
  const app::AlarmVM vm = r.model->alarm();
  TEST_ASSERT_TRUE(vm.active);
  TEST_ASSERT_TRUE(screenTouchTargetsOk(r));
  const size_t before = r.persist.entries.size();
  // single tap only arms
  TEST_ASSERT_TRUE(r.tapId(kHitTaken));
  TEST_ASSERT_EQUAL(static_cast<int>(before), static_cast<int>(r.persist.entries.size()));
  TEST_ASSERT_TRUE(r.ui->armed());
  // second tap confirms the whole group for this one person
  TEST_ASSERT_TRUE(r.tapId(kHitTaken));
  TEST_ASSERT_EQUAL(static_cast<int>(before + vm.meds.size()), static_cast<int>(r.persist.entries.size()));
  for (size_t i = before; i < r.persist.entries.size(); i++)
  {
    TEST_ASSERT_EQUAL(static_cast<int>(core::DoseAction::Taken), static_cast<int>(r.persist.entries[i].action));
    TEST_ASSERT_EQUAL(vm.profileId, r.db.med(r.persist.entries[i].medId)->profileId);  // never mixes people
  }
  TEST_ASSERT_TRUE(r.ui->toastVisible());
}

void test_arm_times_out() {
  Rig r(berlin(14, 8, 1), false);
  r.step();
  r.tapId(kHitTaken);
  TEST_ASSERT_TRUE(r.ui->armed());
  r.step(6000);
  TEST_ASSERT_FALSE(r.ui->armed());
  const size_t before = r.persist.entries.size();
  r.tapId(kHitTaken);  // arms again, does not confirm
  TEST_ASSERT_EQUAL(static_cast<int>(before), static_cast<int>(r.persist.entries.size()));
}

void test_hold_confirm_with_button() {
  Rig r(berlin(14, 8, 1), false);
  r.step();
  TEST_ASSERT_EQUAL(kHitTaken, r.focusedId());
  const size_t before = r.persist.entries.size();
  r.key(InputType::OkClick);  // a click alone is not enough
  TEST_ASSERT_EQUAL(static_cast<int>(before), static_cast<int>(r.persist.entries.size()));
  r.key(InputType::OkHold, 500);
  TEST_ASSERT_EQUAL(static_cast<int>(before), static_cast<int>(r.persist.entries.size()));
  r.key(InputType::OkHoldDone);
  TEST_ASSERT_TRUE(r.persist.entries.size() > before);
}

void test_snooze_and_undo() {
  Rig r(berlin(14, 8, 1), false);
  r.step();
  const app::AlarmVM vm = r.model->alarm();
  r.tapId(kHitSnooze);
  TEST_ASSERT_TRUE(r.tapId(kHitSnooze10));
  const core::DoseLogEntry& last = r.persist.entries.back();
  TEST_ASSERT_EQUAL(static_cast<int>(core::DoseAction::Snoozed), static_cast<int>(last.action));
  // the snoozed person disappears from the alarm (another person may take over)
  const app::AlarmVM after = r.model->alarm();
  TEST_ASSERT_TRUE(!after.active || after.profileId != vm.profileId || after.meds.size() < vm.meds.size());
  // skip then undo from the toast
  Rig s(berlin(14, 8, 1), false);
  s.step();
  s.tapId(kHitSkip);
  TEST_ASSERT_TRUE(s.ui->toastVisible());
  const size_t n = s.persist.entries.size();
  TEST_ASSERT_TRUE(s.tapId(kHitToastUndo));
  TEST_ASSERT_TRUE(s.persist.entries.size() > n);
  TEST_ASSERT_EQUAL(static_cast<int>(core::DoseAction::Undo), static_cast<int>(s.persist.entries.back().action));
}

void test_backfill_taken_earlier() {
  Rig r(berlin(14, 8, 40), false);
  r.step();
  TEST_ASSERT_EQUAL(static_cast<int>(ScreenId::Alarm), static_cast<int>(r.ui->screen()));
  r.tapId(kHitEarlier);
  TEST_ASSERT_EQUAL(static_cast<int>(ScreenId::Backfill), static_cast<int>(r.ui->screen()));
  for (int i = 0; i < 15; ++i) r.tapId(kHitEntry0 + kEntryDown0 + 1);  // minutes -15
  r.tapId(kHitSave);
  r.tapId(kHitSave);
  const core::DoseLogEntry& e = r.persist.entries.back();
  TEST_ASSERT_TRUE(e.action == core::DoseAction::Taken || e.action == core::DoseAction::TakenLate);
  TEST_ASSERT_TRUE(e.actionUtc < berlin(14, 8, 40));
}

void test_prn_lockout() {
  Rig r(berlin(14, 12, 0), false);
  r.step();
  if (r.ui->screen() == ScreenId::Alarm) r.tapId(kHitSkip), r.step();
  while (r.ui->screen() == ScreenId::Alarm) r.tapId(kHitSkip), r.step();
  r.ui->openPrn(1);
  r.draw();
  TEST_ASSERT_EQUAL(static_cast<int>(ScreenId::Prn), static_cast<int>(r.ui->screen()));
  r.tapId(kHitLog0);
  r.tapId(kHitLog0);
  const app::PrnVM vm = r.model->prn(1);
  TEST_ASSERT_TRUE(vm.rows.size() >= 1);
  TEST_ASSERT_FALSE(vm.rows[0].allowed);
  TEST_ASSERT_NOT_NULL(strstr(vm.rows[0].status.c_str(), ":"));  // "Next allowed HH:MM"
  const size_t n = r.persist.entries.size();
  r.tapId(kHitLog0);
  r.tapId(kHitLog0);
  TEST_ASSERT_EQUAL(static_cast<int>(n), static_cast<int>(r.persist.entries.size()));
}


// ---------------------------------------------------------------- value entry (dates, times, numbers)
void test_entry_clamp_and_leap_year() {
  ValueEntry e = ValueEntry::date(core::daysFromCivil(2026, 1, 31));
  TEST_ASSERT_EQUAL_STRING("Sat 31 Jan 2026", e.summary().c_str());
  e.step(1, 1);  // month up: 31 Feb -> 28 Feb
  TEST_ASSERT_EQUAL(core::daysFromCivil(2026, 2, 28), e.day());
  e.step(2, 1);
  e.step(2, 1);  // 2028 is a leap year: the chosen 31 now gives 29 Feb
  TEST_ASSERT_EQUAL(core::daysFromCivil(2028, 2, 29), e.day());
  e.step(1, 1);  // March: back to the chosen 31
  TEST_ASSERT_EQUAL(core::daysFromCivil(2028, 3, 31), e.day());
  e.step(1, -1);
  TEST_ASSERT_EQUAL(core::daysFromCivil(2028, 2, 29), e.day());
  e.step(0, 1);  // wraps to 1
  TEST_ASSERT_EQUAL(1, e.field(0).value);
  e.step(0, -1);  // and back to the last day of the month
  TEST_ASSERT_EQUAL(29, e.field(0).value);
  e.step(2, 1);  // 2029: 29 Feb -> 28 Feb
  TEST_ASSERT_EQUAL(core::daysFromCivil(2029, 2, 28), e.day());
  TEST_ASSERT_EQUAL_STRING("Wed 28 Feb 2029", e.summary().c_str());
  // 2100 is not a leap year (century rule), 2000 is
  TEST_ASSERT_EQUAL(28, core::daysInMonth(2100, 2));
  TEST_ASSERT_EQUAL(29, core::daysInMonth(2000, 2));
  // typed day 31 in April clamps to 30
  ValueEntry a = ValueEntry::date(core::daysFromCivil(2026, 4, 10));
  a.beginTyping(0);
  a.typeDigit(3);
  a.typeDigit(1);
  TEST_ASSERT_EQUAL(30, a.field(0).value);
  // typed day first, then a shorter month: clamped when the month is committed
  a.beginTyping(0);
  a.typeDigit(3);
  a.typeDigit(1);
  a.typeDigit(0);
  a.typeDigit(2);  // February 2026
  TEST_ASSERT_EQUAL(core::daysFromCivil(2026, 2, 28), a.day());
  // year outside the range clamps; minutes wrap; month never 0
  a.beginTyping(2);
  for (int d : {1, 9, 9, 9}) a.typeDigit(d);
  TEST_ASSERT_EQUAL(2020, a.field(2).value);
  ValueEntry t = ValueEntry::time(23 * 60 + 59);
  t.step(1, 1);
  TEST_ASSERT_EQUAL(23 * 60, t.minute());  // minutes wrap without carrying into the hour
  t.step(0, 1);
  TEST_ASSERT_EQUAL(0, t.minute());
  a.beginTyping(1);
  a.typeDigit(0);
  a.typeDigit(0);
  TEST_ASSERT_EQUAL(1, a.field(1).value);
}

void test_entry_numpad_and_auto_advance() {
  ValueEntry e = ValueEntry::dateTime(core::daysFromCivil(2026, 10, 19), 0);
  TEST_ASSERT_EQUAL_STRING("Mon 19 Oct 2026, 00:00", e.summary().c_str());
  e.beginTyping(0);
  TEST_ASSERT_EQUAL_STRING("__", e.fieldText(0).c_str());
  e.typeDigit(0);
  TEST_ASSERT_EQUAL_STRING("0_", e.fieldText(0).c_str());
  e.typeDigit(5);  // 2 digits: next field
  TEST_ASSERT_EQUAL(1, e.typingField());
  e.typeDigit(1);
  e.typeDigit(1);
  TEST_ASSERT_EQUAL(2, e.typingField());
  for (int d : {2, 0, 2, 7}) e.typeDigit(d);  // the year needs 4 digits
  TEST_ASSERT_EQUAL(3, e.typingField());
  e.typeDigit(7);  // hour "7": no second digit can follow (70 > 23) -> advances at once
  TEST_ASSERT_EQUAL(4, e.typingField());
  e.typeDigit(4);
  e.typeDigit(5);
  TEST_ASSERT_FALSE(e.typing());
  TEST_ASSERT_TRUE(e.onDone());
  TEST_ASSERT_EQUAL_STRING("Fri 5 Nov 2027, 07:45", e.summary().c_str());
  TEST_ASSERT_EQUAL(core::daysFromCivil(2027, 11, 5) * 1440 + 7 * 60 + 45, e.encoded());
  // early advance for months 2..9, backspace, partial entry committed by OK
  ValueEntry d = ValueEntry::date(core::daysFromCivil(2026, 1, 1));
  d.beginTyping(1);
  d.typeDigit(4);
  TEST_ASSERT_EQUAL(4, d.field(1).value);
  TEST_ASSERT_EQUAL(2, d.typingField());
  d.typeDigit(2);
  d.typeDigit(9);
  d.backspace();
  TEST_ASSERT_EQUAL_STRING("2___", d.fieldText(2).c_str());
  TEST_ASSERT_TRUE(handleEntryHit(d, kEntryOk));  // "2" -> clamped to 2020, focus moves past the year
  TEST_ASSERT_EQUAL(2020, d.field(2).value);
  TEST_ASSERT_TRUE(d.onDone());
  // buttons: OK walks the fields, UP/DOWN change the focused one
  ValueEntry b = ValueEntry::time(8 * 60);
  b.stepFocused(1);
  TEST_ASSERT_EQUAL(9 * 60, b.minute());
  TEST_ASSERT_FALSE(b.next());
  b.stepFocused(-1);
  TEST_ASSERT_EQUAL(9 * 60 + 59, b.minute());
  TEST_ASSERT_TRUE(b.next());
  // numbers: one field, digits from the range, sign key when negative values are allowed
  ValueEntry n = ValueEntry::number(5, -120, 120, 5, "min");
  TEST_ASSERT_TRUE(n.signedNumber());
  n.beginTyping(0);
  n.toggleSign();
  n.typeDigit(3);
  n.typeDigit(0);
  n.finishTyping();
  TEST_ASSERT_EQUAL(-30, n.number());
  TEST_ASSERT_EQUAL_STRING("-30 min", n.summary().c_str());
  n.step(0, 1);
  TEST_ASSERT_EQUAL(-25, n.number());
  ValueEntry big = ValueEntry::number(10, 5, 50, 1, "");
  big.beginTyping(0);
  big.typeDigit(9);
  big.typeDigit(9);  // 2 digits typed: committed and clamped
  TEST_ASSERT_EQUAL(50, big.number());
  TEST_ASSERT_TRUE(big.onDone());
}

void test_entry_defaults() {
  DateTimeValue v;
  TEST_ASSERT_TRUE(parseBuildDateTime("Oct 19 2026", "00:22:11", v));
  TEST_ASSERT_EQUAL(core::daysFromCivil(2026, 10, 19), v.day);
  TEST_ASSERT_EQUAL(22, v.minute);
  TEST_ASSERT_TRUE(parseBuildDateTime("Feb  9 2027", "23:05:00", v));  // __DATE__ pads the day with a space
  TEST_ASSERT_EQUAL(core::daysFromCivil(2027, 2, 9), v.day);
  TEST_ASSERT_EQUAL(23 * 60 + 5, v.minute);
  TEST_ASSERT_FALSE(parseBuildDateTime("Foo 1 2026", "00:00:00", v));
  TEST_ASSERT_FALSE(parseBuildDateTime("Feb 30 2026", "00:00:00", v));
  // a valid RTC wins
  core::LocalTime now{core::daysFromCivil(2026, 12, 24), 18 * 3600 + 30 * 60, 3600, false};
  DateTimeValue d = defaultClockValue(true, now, "Oct 19 2026", "00:22:11");
  TEST_ASSERT_EQUAL(now.days, d.day);
  TEST_ASSERT_EQUAL(18 * 60 + 30, d.minute);
  // RTC not valid (or reset to 1970/2000): the firmware build time
  d = defaultClockValue(false, now, "Oct 19 2026", "00:22:11");
  TEST_ASSERT_EQUAL(core::daysFromCivil(2026, 10, 19), d.day);
  core::LocalTime old{core::daysFromCivil(2000, 1, 1), 0, 0, false};
  d = defaultClockValue(true, old, "Oct 19 2026", "00:22:11");
  TEST_ASSERT_EQUAL(core::daysFromCivil(2026, 10, 19), d.day);
  // the real build date parses
  TEST_ASSERT_TRUE(parseBuildDateTime(firmwareBuildDate(), firmwareBuildTime(), v));
  // the forced clock screen starts from the build date when the clock is unset
  Rig r(berlin(14, 12, 0), false);
  r.model->status.clockUnset = true;
  r.step();
  TEST_ASSERT_TRUE(parseBuildDateTime(firmwareBuildDate(), firmwareBuildTime(), v));
  const core::Ymd y = core::civilFromDays(v.day);
  r.tapId(kHitSave);
  TEST_ASSERT_EQUAL(fx::berlinLocal(y.y, y.m, y.d, v.minute / 60, v.minute % 60), r.clk.nowUtc());
}

void test_entry_targets_and_partial_refresh() {
  ValueEntry e = ValueEntry::dateTime(core::daysFromCivil(2026, 10, 19), 8 * 60);
  const EntryLayout L = layoutValueEntry(e, 60);
  for (int i = 0; i < e.count(); ++i) {
    TEST_ASSERT_TRUE(L.up[i].h >= mmToPx(9.0f) && L.up[i].w >= mmToPx(9.0f));
    TEST_ASSERT_TRUE(L.down[i].h >= mmToPx(9.0f));
    TEST_ASSERT_TRUE(L.box[i].x >= 0 && L.box[i].right() <= Canvas::kW);
    if (i) TEST_ASSERT_TRUE(L.box[i].x >= L.box[i - 1].right());
  }
  TEST_ASSERT_TRUE(L.down[0].bottom() <= 396 && L.summary.bottom() <= 396);
  e.beginTyping(0);
  const EntryLayout K = layoutValueEntry(e, 60);
  TEST_ASSERT_EQUAL(12, static_cast<int>(K.keys.size()));
  for (const auto& k : K.keys) TEST_ASSERT_TRUE(k.second.h >= mmToPx(9.0f) && k.second.w >= mmToPx(9.0f) && k.second.bottom() <= 396);

  // Clock screen: tapping the day's up arrow only redraws the day column and the summary line.
  Rig r(berlin(14, 12, 0), false);
  r.model->status.clockUnset = true;
  r.step();
  std::vector<uint8_t> before(r.buf, r.buf + Canvas::kBytes);
  r.tapId(kHitEntry0 + kEntryUp0 + 0);
  std::vector<Rect> dirty;
  diffFrames(before.data(), r.buf, dirty);
  TEST_ASSERT_FALSE(dirty.empty());
  const EntryLayout C = layoutValueEntry(ValueEntry::dateTime(0, 0), 60);
  for (const Rect& d : dirty) {
    const bool inDay = d.x >= alignToBytes(C.box[0]).x && d.right() <= alignToBytes(C.box[0]).right();
    const bool inSummary = d.y >= C.summary.y - 1 && d.bottom() <= C.summary.bottom() + 1;
    TEST_ASSERT_TRUE_MESSAGE(inDay || inSummary, "only the changed field and the summary may change");
  }
}

void test_clock_set_forced() {
  Rig r(berlin(14, 12, 0), false);
  r.model->status.clockUnset = true;
  r.step();
  TEST_ASSERT_EQUAL(static_cast<int>(ScreenId::ClockSet), static_cast<int>(r.ui->screen()));
  r.tapId(kHitBack);  // no way out
  r.step();
  TEST_ASSERT_EQUAL(static_cast<int>(ScreenId::ClockSet), static_cast<int>(r.ui->screen()));
  // Buttons: UP changes the focused field (day first), OK moves on; past the minutes Save has the focus.
  r.key(InputType::Up);
  for (int i = 0; i < 5; i++) r.key(InputType::OkClick);
  TEST_ASSERT_EQUAL(kHitSave, r.focusedId());
  // Touch: tap the day box and type everything on the number pad (auto-advance), 29 Feb 2028 09:30.
  r.tapId(kHitEntry0 + kEntryBox0 + 0);
  for (int d : {2, 9, 0, 2, 2, 0, 2, 8, 0, 9, 3, 0}) TEST_ASSERT_TRUE(r.tapId(kHitEntry0 + kEntryKey0 + d));
  r.tapId(kHitSave);
  TEST_ASSERT_FALSE(r.model->status.clockUnset);
  TEST_ASSERT_EQUAL(fx::berlinLocal(2028, 2, 29, 9, 30), r.clk.nowUtc());
}

void test_navigation_and_render_all() {
  Rig r(berlin(14, 12, 0));
  r.step();
  while (r.ui->screen() == ScreenId::Alarm) r.tapId(kHitSkip), r.step();
  TEST_ASSERT_EQUAL(static_cast<int>(ScreenId::Home), static_cast<int>(r.ui->screen()));
  TEST_ASSERT_TRUE(screenTouchTargetsOk(r));
  r.tapId(kHitLane0);
  TEST_ASSERT_EQUAL(static_cast<int>(ScreenId::Today), static_cast<int>(r.ui->screen()));
  r.tapId(kHitRow0);
  TEST_ASSERT_EQUAL(static_cast<int>(ScreenId::Detail), static_cast<int>(r.ui->screen()));
  r.tapId(kHitBack);
  r.tapId(kHitHistory);
  TEST_ASSERT_EQUAL(static_cast<int>(ScreenId::History), static_cast<int>(r.ui->screen()));
  for (int i = 0; i < 3; i++) r.tapId(kHitMode);
  r.tapId(kHitPerMed);
  r.tapId(kHitPrev);
  r.tapId(kHitBack);
  r.tapId(kHitBack);
  TEST_ASSERT_EQUAL(static_cast<int>(ScreenId::Home), static_cast<int>(r.ui->screen()));
  // button focus navigation reaches every focusable target
  const int n = r.ui->hits().focusableCount();
  for (int i = 0; i < n; i++) r.key(InputType::Down);
  TEST_ASSERT_EQUAL(0, r.ui->focusIndex());
  r.tapId(kHitLargeText);
  TEST_ASSERT_TRUE(r.ui->largeText());
}

void test_quiet_alarm_and_battery_screen() {
  Rig r(berlin(14, 20, 2), false);
  r.db.profiles[0].quietStartMin = 19 * 60;
  r.db.profiles[0].quietEndMin = 23 * 60;
  r.step();
  TEST_ASSERT_EQUAL(static_cast<int>(ScreenId::Alarm), static_cast<int>(r.ui->screen()));
  const app::AlarmVM vm = r.model->alarm();
  TEST_ASSERT_EQUAL(1, vm.profileId);
  TEST_ASSERT_TRUE(vm.silent);
  r.model->status.veryLowBattery = true;
  TEST_ASSERT_EQUAL(static_cast<int>(app::BannerKind::VeryLowBattery), static_cast<int>(app::topBanner(r.model->status)));
  r.ui->showBatteryEmpty();
  r.draw();
  TEST_ASSERT_EQUAL(0, r.ui->hits().count());  // nothing to press: it stays on the e-paper
  r.step(5000);
  TEST_ASSERT_EQUAL(static_cast<int>(ScreenId::BatteryEmpty), static_cast<int>(r.ui->screen()));
}

// ---------------------------------------------------------------- previews (PBM, when MT_SCREENS_DIR is set)
static void writePbm(const char* dir, const char* name, const uint8_t* buf) {
  char path[512];
  snprintf(path, sizeof(path), "%s/%s.pbm", dir, name);
  FILE* f = fopen(path, "wb");
  if (!f) return;
  fprintf(f, "P4\n%d %d\n", Canvas::kW, Canvas::kH);
  for (int i = 0; i < Canvas::kBytes; i++) fputc(static_cast<uint8_t>(~buf[i]), f);  // PBM: 1 = black
  fclose(f);
}

void test_button_labels_fit() {
  // every footer label must fit its button in both text sizes (Today: 280 px "As needed", 230 px "History")
  for (bool large : {false, true}) {
    const Theme& t = themeFor(large);
    for (const auto& c : {std::make_pair(280, S::PrnTitle), std::make_pair(230, S::History)}) {
      const ButtonFit f = fitButton(t, t.bodyBold, c.first, tr(c.second), true);
      TEST_ASSERT_TRUE_MESSAGE(textWidth(*f.font, tr(c.second)) + (f.icon ? kIconSize + 8 : 0) <= c.first - 16, tr(c.second));
    }
  }
  const Theme& t = themeFor(true);
  const ButtonFit wide = fitButton(t, t.bodyBold, 600, "OK", true);
  TEST_ASSERT_TRUE(wide.icon);
  TEST_ASSERT_TRUE(wide.font == t.bodyBold);
}

void test_render_timing() {
  // Host timing of a full frame render (model -> 800x480 1-bit canvas). Device timing differs
  // (ESP32-S3 at 80-240 MHz, PSRAM) and is listed as "to be measured on device" in the docs.
  Rig r(berlin(14, 12, 30));
  r.step();
  while (r.ui->screen() == ScreenId::Alarm) r.tapId(kHitSkip), r.step(70000);
  r.step(70000);
  struct Case {
    const char* name;
    int hit;
  };
  const Case cases[] = {{"home", -1}, {"today", kHitLane0}, {"history", kHitHistory}};
  char msg[160];
  for (const Case& c : cases) {
    if (c.hit >= 0) r.tapId(c.hit);
    const int n = 50;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; i++) r.draw();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / n;
    snprintf(msg, sizeof msg, "render %s: %.3f ms/frame (host)", c.name, ms);
    printf("%s\n", msg);
    TEST_ASSERT_TRUE_MESSAGE(ms < 100.0, msg);
  }
}

void test_previews() {
  const char* dir = getenv("MT_SCREENS_DIR");
  if (!dir) {
    TEST_PASS_MESSAGE("MT_SCREENS_DIR not set");
    return;
  }
  {
    Rig r(berlin(14, 12, 30));
    r.step();
    while (r.ui->screen() == ScreenId::Alarm) r.tapId(kHitSkip), r.step(70000);
    r.step(70000);
    writePbm(dir, "home", r.buf);
    r.tapId(kHitLane0);
    writePbm(dir, "today", r.buf);
    r.tapId(kHitRow0);
    writePbm(dir, "detail", r.buf);
    r.tapId(kHitBack);
    r.tapId(kHitPrn);
    writePbm(dir, "prn", r.buf);
    r.tapId(kHitBack);
    r.tapId(kHitHistory);
    writePbm(dir, "history", r.buf);
    r.tapId(kHitMode);
    r.tapId(kHitMode);
    writePbm(dir, "history_day", r.buf);
    r.tapId(kHitMode);
    r.tapId(kHitMode);
    writePbm(dir, "history_month", r.buf);
  }
  {
    Rig r(berlin(14, 8, 2));
    r.step();
    writePbm(dir, "alarm", r.buf);
    r.tapId(kHitTaken);
    writePbm(dir, "alarm_armed", r.buf);
  }
  {
    Rig r(berlin(14, 12, 0), false);
    r.model->status.clockUnset = true;
    r.step();
    writePbm(dir, "clockset", r.buf);
    r.tapId(kHitEntry0 + kEntryBox0 + 2);  // year: number pad
    r.tapId(kHitEntry0 + kEntryKey0 + 2);
    r.tapId(kHitEntry0 + kEntryKey0 + 0);
    writePbm(dir, "clockset_numpad", r.buf);
  }
  {
    Rig r(berlin(14, 8, 40), false);
    r.step();
    r.tapId(kHitEarlier);
    writePbm(dir, "backfill", r.buf);
  }
  {
    Rig r(berlin(14, 20, 2), false);
    r.db.profiles[0].quietStartMin = 19 * 60;
    r.db.profiles[0].quietEndMin = 23 * 60;
    r.step();
    writePbm(dir, "alarm_quiet", r.buf);
  }
  {
    Rig r(berlin(14, 12, 30));
    r.model->status.lowBattery = r.model->status.veryLowBattery = true;
    r.model->status.batteryPct = 9;
    r.step();
    while (r.ui->screen() == ScreenId::Alarm) r.tapId(kHitSkip), r.step(70000);
    r.step(70000);
    writePbm(dir, "home_battery10", r.buf);
    r.ui->showBatteryEmpty();
    r.draw();
    writePbm(dir, "battery_empty", r.buf);
  }
  {
    Rig r(berlin(14, 12, 30));
    r.model->status.lowBattery = true;
    r.step();
    while (r.ui->screen() == ScreenId::Alarm) r.tapId(kHitSkip), r.step(70000);
    r.step(70000);
    r.ui->setLargeText(true);
    r.draw();
    writePbm(dir, "home_large", r.buf);
    r.tapId(kHitLane0);
    r.draw();
    writePbm(dir, "today_large", r.buf);
  }
  TEST_PASS();
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_text_metrics);
  RUN_TEST(test_theme_sizes);
  RUN_TEST(test_canvas_basics);
  RUN_TEST(test_button_click_and_hold);
  RUN_TEST(test_button_repeat_and_combo);
  RUN_TEST(test_hitmap);
  RUN_TEST(test_button_labels_fit);
  RUN_TEST(test_render_timing);
  RUN_TEST(test_diff_frames);
  RUN_TEST(test_refresh_policy);
  RUN_TEST(test_latency_stats);
  RUN_TEST(test_pcf8563_codec);
  RUN_TEST(test_clock_trust);
  RUN_TEST(test_scaled_clock);
  RUN_TEST(test_local_to_utc_checked);
  RUN_TEST(test_demo_data_valid);
  RUN_TEST(test_banner_priority);
  RUN_TEST(test_home_vm_privacy);
  RUN_TEST(test_layout_helpers);
  RUN_TEST(test_alarm_one_person_and_two_step);
  RUN_TEST(test_arm_times_out);
  RUN_TEST(test_hold_confirm_with_button);
  RUN_TEST(test_snooze_and_undo);
  RUN_TEST(test_backfill_taken_earlier);
  RUN_TEST(test_prn_lockout);
  RUN_TEST(test_clock_set_forced);
  RUN_TEST(test_entry_clamp_and_leap_year);
  RUN_TEST(test_entry_numpad_and_auto_advance);
  RUN_TEST(test_entry_defaults);
  RUN_TEST(test_entry_targets_and_partial_refresh);
  RUN_TEST(test_navigation_and_render_all);
  RUN_TEST(test_quiet_alarm_and_battery_screen);
  RUN_TEST(test_previews);
  return UNITY_END();
}
