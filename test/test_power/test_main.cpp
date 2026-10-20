// Phase 4: alarm ladder, quiet hours, multi-person queue, next wake, catch-up across sleep, battery policy.
#include <unity.h>

#include "../common/fixtures.h"
#include "core/alarm.h"
#include "core/alert_pending.h"
#include "core/codec.h"
#include "core/dose_engine.h"
#include "core/power.h"

using namespace core;
using fx::at;
using fx::day;

void setUp() {}
void tearDown() {}

namespace {
// 2026-06-10, CEST (UTC+2). 08:00 local = 06:00 UTC.
const int64_t kT0 = fx::utc(2026, 6, 10, 0, 0);
const int64_t kDose = fx::utc(2026, 6, 10, 6, 0);

struct Rig {
  FakeClock clock{kT0};
  Database db;
  MemoryDoseLog log;
  DoseEngine* eng = nullptr;
  explicit Rig(Database d, int64_t start = kT0) : db(std::move(d)) {
    clock.set(start);
    eng = new DoseEngine(clock, db, &log);
    eng->begin(start);
    eng->process();
  }
  ~Rig() { delete eng; }
  std::vector<EngineEvent> at(int64_t t) {
    clock.set(t);
    return eng->process();
  }
  AlertPlan plan(const std::vector<EngineEvent>& ev) { return planAlerts(ev, *eng, db, fx::berlin(), clock.nowUtc()); }
};

Database twoPeople(int hA, int hB, bool critB = false) {
  Database db;
  Profile a = fx::profile(1);
  a.name = "Anna";
  a.quietStartMin = 22 * 60;
  a.quietEndMin = 7 * 60;
  Profile b = fx::profile(2);
  b.name = "Ben";
  b.initials = "BK";
  b.quietStartMin = 22 * 60;
  b.quietEndMin = 7 * 60;
  db.profiles = {a, b};
  db.meds.push_back(fx::med(1, 1, day(2026, 6, 1), fx::fixedRule({at(hA)})));
  db.meds.push_back(fx::med(2, 2, day(2026, 6, 1), fx::fixedRule({at(hB)})));
  db.meds[1].critical = critB;
  return db;
}

size_t count(const std::vector<EngineEvent>& ev, EngineEventKind k) {
  size_t n = 0;
  for (const auto& e : ev) n += e.kind == k;
  return n;
}

struct FakeOut : AlertOutput {
  const char* n;
  bool aud, ok;
  int plays = 0;
  FakeOut(const char* name, bool audible, bool working) : n(name), aud(audible), ok(working) {}
  const char* name() const override { return n; }
  bool audible() const override { return aud; }
  bool available() const override { return ok; }
  void play(const PersonAlert&) override { plays++; }
};
}  // namespace

// ---------------------------------------------------------------- ladder
static void test_default_ladder_six_repeats_then_missed() {
  Rig r(fx::singleMedDb(fx::fixedRule({at(8)}), day(2026, 6, 1)));
  std::vector<EngineEvent> all;
  for (int64_t t = kDose; t <= kDose + 61 * 60; t += 30) {
    auto ev = r.at(t);
    all.insert(all.end(), ev.begin(), ev.end());
  }
  TEST_ASSERT_EQUAL(1, count(all, EngineEventKind::BecameDue));
  TEST_ASSERT_EQUAL(6, count(all, EngineEventKind::AlarmTick));
  TEST_ASSERT_EQUAL(1, count(all, EngineEventKind::BecameMissed));
  int64_t prev = kDose;
  for (const auto& e : all) {
    if (e.kind != EngineEventKind::AlarmTick) continue;
    TEST_ASSERT_EQUAL_INT64(prev + 5 * 60, e.atUtc);  // every 5 minutes
    prev = e.atUtc;
  }
  TEST_ASSERT_EQUAL(static_cast<int>(DoseState::Missed), static_cast<int>(r.eng->find(makeEventId(1, 1, kDose))->state));
}

static void test_configurable_ladder() {
  Database db = fx::singleMedDb(fx::fixedRule({at(8)}), day(2026, 6, 1));
  db.meds[0].ladderIntervalMin = 10;
  db.meds[0].ladderCount = 2;
  Rig r(db);
  std::vector<EngineEvent> all;
  for (int64_t t = kDose; t <= kDose + 59 * 60; t += 60) {
    auto ev = r.at(t);
    all.insert(all.end(), ev.begin(), ev.end());
  }
  TEST_ASSERT_EQUAL(2, count(all, EngineEventKind::AlarmTick));
}

static void test_snooze_limit() {
  Rig r(fx::singleMedDb(fx::fixedRule({at(8)}), day(2026, 6, 1)));
  const uint64_t id = makeEventId(1, 1, kDose);
  int64_t t = kDose;
  r.at(t);
  for (int i = 0; i < 3; i++) {
    TEST_ASSERT_EQUAL(static_cast<int>(ActionResult::Ok), static_cast<int>(r.eng->snooze(id, 5, ActionSource::Button)));
    t += 5 * 60;
    auto ev = r.at(t);
    TEST_ASSERT_EQUAL(1, count(ev, EngineEventKind::SnoozeEnded));
  }
  TEST_ASSERT_EQUAL(static_cast<int>(ActionResult::SnoozeLimit), static_cast<int>(r.eng->snooze(id, 5, ActionSource::Button)));
}

// ---------------------------------------------------------------- quiet hours / critical
static void test_quiet_hours_silent_unless_critical() {
  // 23:00 local: inside quiet hours 22:00-07:00
  const int64_t dose = fx::berlinLocal(2026, 6, 10, 23, 0);
  Rig r(twoPeople(23, 23, true));
  auto ev = r.at(dose);
  AlertPlan p = r.plan(ev);
  TEST_ASSERT_EQUAL(2, static_cast<int>(p.alerts.size()));
  TEST_ASSERT_EQUAL(2, p.alerts[0].profileId);  // critical first
  TEST_ASSERT_TRUE(p.alerts[0].audible);
  TEST_ASSERT_TRUE(p.alerts[0].critical);
  TEST_ASSERT_EQUAL(1, p.alerts[1].profileId);
  TEST_ASSERT_FALSE(p.alerts[1].audible);  // downgraded to silent visual
  TEST_ASSERT_TRUE(alertAudible(&r.db.profiles[0], false, 12 * 60));
  TEST_ASSERT_FALSE(alertAudible(&r.db.profiles[0], false, 6 * 60 + 59));
  TEST_ASSERT_TRUE(alertAudible(&r.db.profiles[0], true, 23 * 60));
}

static void test_alarm_manager_outputs_and_fallback() {
  FakeOut buzzer("buzzer", true, true), screen("screen", false, true);
  AlarmManager m;
  m.addOutput(&buzzer);
  m.addOutput(&screen);
  PersonAlert a;
  a.audible = true;
  AlarmManager::Result r = m.deliver(a);
  TEST_ASSERT_TRUE(r.sounded && r.shown);
  a.audible = false;  // quiet hours
  r = m.deliver(a);
  TEST_ASSERT_FALSE(r.sounded);
  TEST_ASSERT_TRUE(r.shown);
  TEST_ASSERT_EQUAL(1, buzzer.plays);
  TEST_ASSERT_EQUAL(2, screen.plays);
  TEST_ASSERT_FALSE(m.visualOnly());
  buzzer.ok = false;  // failed self-test
  TEST_ASSERT_TRUE(m.visualOnly());
  a.audible = true;
  r = m.deliver(a);
  TEST_ASSERT_FALSE(r.sounded);
  TEST_ASSERT_TRUE(r.shown);
}

static void test_patterns_and_volume() {
  Database db = twoPeople(8, 8);
  db.profiles[0].alertPattern = static_cast<uint8_t>(PatternId::Gentle);
  db.profiles[0].alertVolume = 30;
  db.meds[0].alertPattern = static_cast<uint8_t>(PatternId::Urgent);
  db.meds[0].alertVolume = 90;
  Rig r(db);
  AlertPlan p = r.plan(r.at(kDose));
  TEST_ASSERT_EQUAL(2, static_cast<int>(p.alerts.size()));
  const PersonAlert& anna = p.alerts[0].profileId == 1 ? p.alerts[0] : p.alerts[1];
  const PersonAlert& ben = p.alerts[0].profileId == 2 ? p.alerts[0] : p.alerts[1];
  TEST_ASSERT_EQUAL(static_cast<int>(PatternId::Urgent), static_cast<int>(anna.pattern));  // medication overrides
  TEST_ASSERT_EQUAL(90, anna.volume);
  TEST_ASSERT_EQUAL(static_cast<int>(PatternId::Chime), static_cast<int>(ben.pattern));  // profile default
  TEST_ASSERT_EQUAL(0, volumeToDuty(0));
  TEST_ASSERT_TRUE(volumeToDuty(10) < volumeToDuty(50));
  TEST_ASSERT_EQUAL(512, volumeToDuty(100));
  TEST_ASSERT_TRUE(patternDurationMs(patternFor(PatternId::Urgent)) > 1000);
  // codec keeps the new fields
  Database back;
  std::string err;
  TEST_ASSERT_TRUE(decodeDatabase(encodeDatabase(db), back, &err));
  TEST_ASSERT_EQUAL(static_cast<int>(PatternId::Gentle), back.profiles[0].alertPattern);
  TEST_ASSERT_EQUAL(90, back.meds[0].alertVolume);
  TEST_ASSERT_EQUAL(255, back.meds[1].alertPattern);
  TEST_ASSERT_EQUAL_STRING("", validateDatabase(back).c_str());
}

// ---------------------------------------------------------------- multi-person queue
static void test_multi_person_queue_one_alert_each() {
  Database db = twoPeople(8, 8);
  db.meds.push_back(fx::med(3, 1, day(2026, 6, 1), fx::fixedRule({at(8)})));  // Anna has two meds
  Rig r(db);
  AlertPlan p = r.plan(r.at(kDose));
  TEST_ASSERT_EQUAL(2, static_cast<int>(p.alerts.size()));  // one per person, not per med
  const PersonAlert& anna = p.alerts[0].profileId == 1 ? p.alerts[0] : p.alerts[1];
  TEST_ASSERT_EQUAL(2, anna.doseCount);
  TEST_ASSERT_EQUAL(1, p.alerts[0].profileId);  // same time, no critical: by profile id
  // Anna takes hers; the repeat only alerts Ben
  r.eng->take(makeEventId(1, 1, kDose), ActionSource::Touch);
  r.eng->take(makeEventId(1, 3, kDose), ActionSource::Touch);
  p = r.plan(r.at(kDose + 5 * 60));
  TEST_ASSERT_EQUAL(1, static_cast<int>(p.alerts.size()));
  TEST_ASSERT_EQUAL(2, p.alerts[0].profileId);
  TEST_ASSERT_EQUAL(1, p.alerts[0].alarmIndex);
}

static void test_people_due_at_different_times_ordered() {
  Database db = twoPeople(9, 8);
  db.meds[1].graceMin = 120;
  Rig r(db);
  std::vector<EngineEvent> ev = r.at(fx::utc(2026, 6, 10, 7, 0));  // 09:00 local: both due, Ben since 08:00
  AlertPlan p = r.plan(ev);
  TEST_ASSERT_EQUAL(2, static_cast<int>(p.alerts.size()));
  TEST_ASSERT_EQUAL(2, p.alerts[0].profileId);  // earliest scheduled first
}

// ---------------------------------------------------------------- catch-up across sleep
static void test_catch_up_across_sleep() {
  Database db = twoPeople(8, 6);  // Ben 06:00, Anna 08:00
  db.meds[1].graceMin = 60;
  db.meds[0].graceMin = 180;
  Rig r(db, fx::utc(2026, 6, 10, 2, 0));  // 04:00 local, then "sleep" until 09:30
  auto ev = r.at(fx::utc(2026, 6, 10, 7, 30));
  // chronological order
  for (size_t i = 1; i < ev.size(); i++) TEST_ASSERT_TRUE(ev[i - 1].atUtc <= ev[i].atUtc);
  for (const auto& e : ev) TEST_ASSERT_TRUE(e.stale);
  AlertPlan p = r.plan(ev);
  TEST_ASSERT_EQUAL(1, p.missed);  // Ben's 06:00 dose passed its grace while asleep: recorded, reported
  TEST_ASSERT_EQUAL(1, static_cast<int>(p.alerts.size()));  // Anna is still due: never silently dropped
  TEST_ASSERT_EQUAL(1, p.alerts[0].profileId);
  TEST_ASSERT_TRUE(p.alerts[0].catchUp);
  TEST_ASSERT_TRUE(p.alerts[0].audible);  // 09:30 is outside quiet hours
  TEST_ASSERT_EQUAL(1, p.catchUpDoses);
  bool missedLogged = false;
  for (const auto& e : r.log.entries) missedLogged = missedLogged || e.action == DoseAction::Missed;
  TEST_ASSERT_TRUE(missedLogged);
}

static void test_catch_up_in_quiet_hours_is_silent() {
  Database db = twoPeople(21, 21);
  db.meds[0].graceMin = db.meds[1].graceMin = 180;
  Rig r(db, fx::utc(2026, 6, 10, 18, 0));  // 20:00 local
  auto ev = r.at(fx::utc(2026, 6, 10, 20, 30));             // woke 22:30 local (quiet), dose 21:00 still due
  AlertPlan p = r.plan(ev);
  TEST_ASSERT_EQUAL(2, static_cast<int>(p.alerts.size()));
  for (const auto& a : p.alerts) {
    TEST_ASSERT_TRUE(a.catchUp);
    TEST_ASSERT_FALSE(a.audible);
  }
}

// ---------------------------------------------------------------- next wake
static void test_next_wake_dose_and_margin() {
  Rig r(fx::singleMedDb(fx::fixedRule({at(8)}), day(2026, 6, 1)), fx::utc(2026, 6, 10, 5, 0));  // 07:00 local
  WakeInputs in;
  in.nowUtc = r.clock.nowUtc();
  in.engineNextUtc = r.eng->nextWakeUtc();
  TEST_ASSERT_EQUAL_INT64(kDose, in.engineNextUtc);
  WakePlan p = planWake(fx::berlin(), in);
  TEST_ASSERT_EQUAL(static_cast<int>(WakeReason::DoseEvent), static_cast<int>(p.reason));
  TEST_ASSERT_EQUAL_INT64(kDose, p.targetUtc);
  TEST_ASSERT_EQUAL_INT64(kDose - (20 + 3600 * 50 / 1000), p.wakeUtc);  // base 20 s + 5 % of 1 h
  // while due: the next ladder repeat
  r.at(kDose);
  in.nowUtc = kDose;
  in.engineNextUtc = r.eng->nextWakeUtc();
  p = planWake(fx::berlin(), in);
  TEST_ASSERT_EQUAL_INT64(kDose + 300, p.targetUtc);
  TEST_ASSERT_EQUAL_INT64(kDose + 300 - (20 + 15), p.wakeUtc);
  in.nowUtc = kDose + 290;  // 10 s before the repeat: margin clamped to the minimum sleep
  p = planWake(fx::berlin(), in);
  TEST_ASSERT_EQUAL_INT64(kDose + 290 + kMinSleepSec, p.wakeUtc);
}

static void test_next_wake_nightly_ntp_and_max() {
  WakeInputs in;
  in.nowUtc = fx::berlinLocal(2026, 6, 10, 1, 0);
  in.engineNextUtc = INT64_MAX;
  WakePlan p = planWake(fx::berlin(), in);
  TEST_ASSERT_EQUAL(static_cast<int>(WakeReason::NightlyRefresh), static_cast<int>(p.reason));
  TEST_ASSERT_EQUAL_INT64(fx::berlinLocal(2026, 6, 10, 3, 0), p.targetUtc);
  in.nowUtc = fx::berlinLocal(2026, 6, 10, 3, 30);
  in.ntpEnabled = true;
  p = planWake(fx::berlin(), in);
  TEST_ASSERT_EQUAL(static_cast<int>(WakeReason::NtpSync), static_cast<int>(p.reason));
  in.ntpEnabled = false;
  in.nowUtc = fx::berlinLocal(2026, 6, 10, 12, 0);
  p = planWake(fx::berlin(), in);
  TEST_ASSERT_EQUAL(static_cast<int>(WakeReason::MaxSleep), static_cast<int>(p.reason));
  TEST_ASSERT_TRUE(p.sleepSec < 6 * 3600);
  // overdue target: no sleep
  in.engineNextUtc = in.nowUtc - 5;
  p = planWake(fx::berlin(), in);
  TEST_ASSERT_EQUAL_INT64(0, p.sleepSec);
  // DST: nightly 03:00 on the fall-back day is still found
  TEST_ASSERT_EQUAL_INT64(fx::berlinLocal(2026, 10, 25, 3, 0),
                          nextLocalMinute(fx::berlin(), fx::berlinLocal(2026, 10, 25, 1, 0), 180));
}

static void test_sleep_gate() {
  SleepInputs s;
  s.idleMs = 10 * 60 * 1000;
  TEST_ASSERT_EQUAL(static_cast<int>(SleepBlocker::None), static_cast<int>(sleepBlocker(s)));
  {
    SleepInputs n = s;
    n.networkBusy = true;
    n.userRequested = true;  // a radio job in flight is never cut off, even on request
    TEST_ASSERT_EQUAL(static_cast<int>(SleepBlocker::NetworkBusy), static_cast<int>(sleepBlocker(n)));
    TEST_ASSERT_EQUAL_STRING("network_busy", sleepBlockerName(SleepBlocker::NetworkBusy));
  }
  s.refreshing = true;
  s.userRequested = true;
  TEST_ASSERT_EQUAL(static_cast<int>(SleepBlocker::Refreshing), static_cast<int>(sleepBlocker(s)));
  s.refreshing = false;
  s.alarmActive = true;
  TEST_ASSERT_EQUAL(static_cast<int>(SleepBlocker::AlarmActive), static_cast<int>(sleepBlocker(s)));
  s.alarmActive = false;
  s.userRequested = false;
  s.usbPowered = true;
  TEST_ASSERT_EQUAL(static_cast<int>(SleepBlocker::UsbPowered), static_cast<int>(sleepBlocker(s)));
  s.sleepOnUsb = true;
  TEST_ASSERT_EQUAL(static_cast<int>(SleepBlocker::None), static_cast<int>(sleepBlocker(s)));
  s.idleMs = 1000;
  TEST_ASSERT_EQUAL(static_cast<int>(SleepBlocker::NotIdle), static_cast<int>(sleepBlocker(s)));
  s.userRequested = true;
  TEST_ASSERT_EQUAL(static_cast<int>(SleepBlocker::None), static_cast<int>(sleepBlocker(s)));
}

// ---------------------------------------------------------------- battery
static void test_battery_thresholds() {
  BatteryMonitor m;
  BatteryUpdate u = m.update(true, 80, false);
  TEST_ASSERT_EQUAL(static_cast<int>(BatteryLevel::Ok), static_cast<int>(u.level));
  u = m.update(true, 20, false);
  TEST_ASSERT_EQUAL(static_cast<int>(BatteryLevel::Low), static_cast<int>(u.level));
  TEST_ASSERT_TRUE(u.changed);
  TEST_ASSERT_FALSE(u.alertNow);
  u = m.update(true, 22, false);  // hysteresis
  TEST_ASSERT_EQUAL(static_cast<int>(BatteryLevel::Low), static_cast<int>(u.level));
  u = m.update(true, 10, false);
  TEST_ASSERT_EQUAL(static_cast<int>(BatteryLevel::VeryLow), static_cast<int>(u.level));
  TEST_ASSERT_TRUE(u.alertNow);
  u = m.update(true, 9, false);
  TEST_ASSERT_FALSE(u.alertNow);  // once only
  u = m.update(true, 5, false);   // first critical reading: not yet
  TEST_ASSERT_FALSE(u.shutdownNow);
  TEST_ASSERT_EQUAL(static_cast<int>(BatteryLevel::VeryLow), static_cast<int>(u.level));
  u = m.update(true, 4, false);   // confirmed
  TEST_ASSERT_EQUAL(static_cast<int>(BatteryLevel::Critical), static_cast<int>(u.level));
  TEST_ASSERT_TRUE(u.shutdownNow);
  u = m.update(true, 4, false);
  TEST_ASSERT_FALSE(u.shutdownNow);  // edge only
}

static void test_battery_usb_and_gauge_errors() {
  BatteryMonitor m;
  m.update(true, 30, false);
  BatteryUpdate u = m.update(false, -1, false);  // gauge unreadable: keep level, never shut down
  TEST_ASSERT_EQUAL(static_cast<int>(BatteryLevel::Ok), static_cast<int>(u.level));
  for (int i = 0; i < 4; i++) u = m.update(true, 3, true);  // on USB: no shutdown
  TEST_ASSERT_FALSE(u.shutdownNow);
  TEST_ASSERT_TRUE(u.level != BatteryLevel::Critical);
  u = m.update(true, 25, true);  // charging above low clears the warning
  TEST_ASSERT_EQUAL(static_cast<int>(BatteryLevel::Ok), static_cast<int>(u.level));
  u = m.update(true, 9, false);  // unplugged and low again: alert again
  TEST_ASSERT_TRUE(u.alertNow);
}

static void test_power_counters() {
  PowerCounters c;
  TEST_ASSERT_FALSE(c.valid());
  c.recordWake(WakeSource::Timer, 3);
  c.recordWake(WakeSource::Button, -7);
  c.recordSleep(600, 2500);
  TEST_ASSERT_TRUE(c.valid());
  TEST_ASSERT_EQUAL(2u, c.boots);
  TEST_ASSERT_EQUAL(1u, c.wakes[static_cast<int>(WakeSource::Timer)]);
  TEST_ASSERT_EQUAL(-7, c.maxDriftSec);
  TEST_ASSERT_EQUAL(600u, static_cast<unsigned>(c.sleepSec));
  TEST_ASSERT_EQUAL_STRING("usb", wakeSourceName(WakeSource::Usb));
}

static void test_alert_pending_never_drops() {
  AlertPending q;
  // 50 alerts for 9 slots (8 people + system): nothing is lost, everything is coalesced per person
  int pushed[AlertPending::kSlots] = {};
  for (int i = 0; i < 50; i++) {
    PersonAlert a;
    a.profileId = static_cast<uint8_t>(i % AlertPending::kSlots);
    a.earliestScheduledUtc = 1000 + i;
    a.alarmIndex = static_cast<uint8_t>(i / AlertPending::kSlots);
    a.audible = (i == 13);  // one audible alert for profile 4 must survive the merge
    a.critical = (i == 20);  // profile 2 becomes critical
    a.catchUp = true;
    q.push(a);
    pushed[a.profileId]++;
  }
  TEST_ASSERT_EQUAL(AlertPending::kSlots, q.size());
  TEST_ASSERT_EQUAL(50u - AlertPending::kSlots, q.coalesced());
  PersonAlert out;
  TEST_ASSERT_TRUE(q.pop(out));
  TEST_ASSERT_EQUAL(2, out.profileId);  // critical first
  TEST_ASSERT_TRUE(out.critical);
  int seen = 1;
  bool audible4 = false;
  int64_t prev = 0;
  while (q.pop(out)) {
    seen++;
    TEST_ASSERT_TRUE(out.earliestScheduledUtc >= prev);  // then earliest first
    prev = out.earliestScheduledUtc;
    if (out.profileId == 4) {
      audible4 = out.audible;
      TEST_ASSERT_EQUAL(5, out.alarmIndex);  // highest ladder step kept
    }
  }
  TEST_ASSERT_EQUAL(AlertPending::kSlots, seen);
  TEST_ASSERT_TRUE(audible4);
  TEST_ASSERT_TRUE(q.empty());
  // a fresh alert merged into a catch-up one makes it fresh
  PersonAlert c;
  c.profileId = 1;
  c.catchUp = true;
  PersonAlert f = c;
  f.catchUp = false;
  TEST_ASSERT_FALSE(mergeAlerts(c, f).catchUp);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_default_ladder_six_repeats_then_missed);
  RUN_TEST(test_configurable_ladder);
  RUN_TEST(test_snooze_limit);
  RUN_TEST(test_quiet_hours_silent_unless_critical);
  RUN_TEST(test_alarm_manager_outputs_and_fallback);
  RUN_TEST(test_patterns_and_volume);
  RUN_TEST(test_multi_person_queue_one_alert_each);
  RUN_TEST(test_people_due_at_different_times_ordered);
  RUN_TEST(test_catch_up_across_sleep);
  RUN_TEST(test_catch_up_in_quiet_hours_is_silent);
  RUN_TEST(test_next_wake_dose_and_margin);
  RUN_TEST(test_next_wake_nightly_ntp_and_max);
  RUN_TEST(test_sleep_gate);
  RUN_TEST(test_battery_thresholds);
  RUN_TEST(test_battery_usb_and_gauge_errors);
  RUN_TEST(test_power_counters);
  RUN_TEST(test_alert_pending_never_drops);
  return UNITY_END();
}
