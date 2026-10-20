#include <unity.h>

#include <set>

#include "../common/fixtures.h"
#include "core/codec.h"
#include "core/dose_engine.h"
#include "core/stats.h"

using namespace core;
using fx::at;
using fx::day;

void setUp() {}
void tearDown() {}

namespace {
const int64_t kT0 = fx::utc(2026, 6, 10, 0, 0);
const int64_t kDose = fx::utc(2026, 6, 10, 6, 0);  // 08:00 CEST

struct Rig {
  FakeClock clock{kT0};
  Database db;
  MemoryDoseLog log;
  DoseEngine* eng = nullptr;
  explicit Rig(Database d) : db(std::move(d)) {
    eng = new DoseEngine(clock, db, &log);
    eng->begin(kT0);
    eng->process();
  }
  ~Rig() { delete eng; }
  std::vector<EngineEvent> at(int64_t t) {
    clock.set(t);
    return eng->process();
  }
  uint64_t id() const { return makeEventId(1, 1, kDose); }
  DoseState state() const { return eng->find(id())->state; }
};

Database baseDb() {
  Database db = fx::singleMedDb(fx::fixedRule({at(8)}), day(2026, 6, 1));
  db.meds[0].supply = 10;
  return db;
}

size_t count(const std::vector<EngineEvent>& ev, EngineEventKind k) {
  size_t n = 0;
  for (const auto& e : ev)
    if (e.kind == k) n++;
  return n;
}
}  // namespace

static void test_pending_to_due_and_alarm_ladder() {
  Rig r(baseDb());
  TEST_ASSERT_EQUAL(DoseState::Pending, r.state());
  TEST_ASSERT_EQUAL(kDose, r.eng->nextWakeUtc());
  TEST_ASSERT_EQUAL(0, r.at(kDose - 1).size());
  auto ev = r.at(kDose);
  TEST_ASSERT_EQUAL(1, ev.size());
  TEST_ASSERT_EQUAL(EngineEventKind::BecameDue, ev[0].kind);
  TEST_ASSERT_TRUE(ev[0].audible);
  TEST_ASSERT_FALSE(ev[0].stale);
  TEST_ASSERT_EQUAL(DoseState::Due, r.state());
  TEST_ASSERT_EQUAL(kDose + 300, r.eng->nextWakeUtc());
  // repeat every 5 minutes, 6 times
  size_t ticks = 0;
  for (int64_t t = kDose + 60; t <= kDose + 59 * 60; t += 60) {
    auto e = r.at(t);
    for (const auto& x : e) {
      TEST_ASSERT_EQUAL(EngineEventKind::AlarmTick, x.kind);
      TEST_ASSERT_EQUAL(kDose + static_cast<int64_t>(x.alarmIndex) * 300, x.atUtc);
    }
    ticks += e.size();
  }
  TEST_ASSERT_EQUAL(6, ticks);
  TEST_ASSERT_EQUAL(DoseState::Due, r.state());
}

static void test_due_to_missed_after_grace() {
  Rig r(baseDb());
  r.at(kDose + 59 * 60 + 59);
  TEST_ASSERT_EQUAL(DoseState::Due, r.state());
  auto ev = r.at(kDose + 3600);
  TEST_ASSERT_EQUAL(1, count(ev, EngineEventKind::BecameMissed));
  TEST_ASSERT_EQUAL(DoseState::Missed, r.state());
  TEST_ASSERT_EQUAL(1, r.log.entries.size());
  TEST_ASSERT_EQUAL(DoseAction::Missed, r.log.entries[0].action);
  TEST_ASSERT_EQUAL(ActionSource::System, r.log.entries[0].source);
  TEST_ASSERT_EQUAL(kDose + 3600, r.log.entries[0].actionUtc);
  TEST_ASSERT_EQUAL(10, r.db.meds[0].supply);
  // custom grace
  Database db = baseDb();
  db.meds[0].graceMin = 20;
  Rig r2(db);
  r2.at(kDose + 20 * 60);
  TEST_ASSERT_EQUAL(DoseState::Missed, r2.state());
}

static void test_due_to_taken_on_time_and_supply() {
  Rig r(baseDb());
  r.at(kDose + 10 * 60);
  TEST_ASSERT_EQUAL(ActionResult::Ok, r.eng->take(r.id(), ActionSource::Button));
  TEST_ASSERT_EQUAL(DoseState::Taken, r.state());
  TEST_ASSERT_EQUAL(9, r.db.meds[0].supply);
  TEST_ASSERT_EQUAL(kDose + 600, r.eng->lastTaken(1));
  TEST_ASSERT_EQUAL(DoseAction::Taken, r.log.entries.back().action);
  TEST_ASSERT_EQUAL(ActionSource::Button, r.log.entries.back().source);
  // no double dose
  TEST_ASSERT_EQUAL(ActionResult::InvalidState, r.eng->take(r.id(), ActionSource::Button));
  TEST_ASSERT_EQUAL(9, r.db.meds[0].supply);
  // no Missed afterwards, no more alarms
  TEST_ASSERT_EQUAL(0, r.at(kDose + 2 * 3600).size());
  TEST_ASSERT_EQUAL(DoseState::Taken, r.state());
}

static void test_due_to_taken_late() {
  Rig r(baseDb());
  r.at(kDose + 16 * 60);
  TEST_ASSERT_EQUAL(ActionResult::Ok, r.eng->take(r.id(), ActionSource::Touch));
  TEST_ASSERT_EQUAL(DoseState::TakenLate, r.state());
  TEST_ASSERT_EQUAL(DoseAction::TakenLate, r.log.entries.back().action);
}

static void test_pending_early_take_and_too_early() {
  Rig r(baseDb());
  r.clock.set(kDose - 61 * 60);
  TEST_ASSERT_EQUAL(ActionResult::TooEarly, r.eng->take(r.id(), ActionSource::Button));
  r.clock.set(kDose - 30 * 60);
  TEST_ASSERT_EQUAL(ActionResult::Ok, r.eng->take(r.id(), ActionSource::Button));
  TEST_ASSERT_EQUAL(DoseState::Taken, r.state());
  TEST_ASSERT_EQUAL(0, r.at(kDose + 5 * 3600).size());  // never becomes due
}

static void test_skip_from_pending_and_due() {
  Rig r(baseDb());
  r.at(kDose + 60);
  TEST_ASSERT_EQUAL(ActionResult::Ok, r.eng->skip(r.id(), ActionSource::Button));
  TEST_ASSERT_EQUAL(DoseState::Skipped, r.state());
  TEST_ASSERT_EQUAL(10, r.db.meds[0].supply);
  TEST_ASSERT_EQUAL(ActionResult::InvalidState, r.eng->skip(r.id(), ActionSource::Button));
  TEST_ASSERT_EQUAL(ActionResult::InvalidState, r.eng->snooze(r.id(), 5, ActionSource::Button));
  Rig r2(baseDb());
  r2.clock.set(kDose - 3600);
  TEST_ASSERT_EQUAL(ActionResult::Ok, r2.eng->skip(r2.id(), ActionSource::Button));
  TEST_ASSERT_EQUAL(0, r2.at(kDose + 3 * 3600).size());
}

static void test_snooze_cycle_and_limits() {
  Rig r(baseDb());
  r.at(kDose);
  TEST_ASSERT_EQUAL(ActionResult::InvalidSnooze, r.eng->snooze(r.id(), 7, ActionSource::Button));
  TEST_ASSERT_EQUAL(ActionResult::Ok, r.eng->snooze(r.id(), 10, ActionSource::Button));
  TEST_ASSERT_EQUAL(DoseState::Snoozed, r.state());
  TEST_ASSERT_EQUAL(0, r.at(kDose + 9 * 60).size());  // ladder silenced while snoozed
  auto ev = r.at(kDose + 10 * 60);
  TEST_ASSERT_EQUAL(1, count(ev, EngineEventKind::SnoozeEnded));
  TEST_ASSERT_EQUAL(DoseState::Due, r.state());
  TEST_ASSERT_EQUAL(ActionResult::Ok, r.eng->snooze(r.id(), 5, ActionSource::Button));
  r.at(kDose + 15 * 60);
  TEST_ASSERT_EQUAL(ActionResult::Ok, r.eng->snooze(r.id(), 15, ActionSource::Button));
  r.at(kDose + 30 * 60);
  TEST_ASSERT_EQUAL(DoseState::Due, r.state());
  TEST_ASSERT_EQUAL(ActionResult::SnoozeLimit, r.eng->snooze(r.id(), 5, ActionSource::Button));
  TEST_ASSERT_EQUAL(3, r.eng->find(r.id())->snoozeCount);
  // a new ladder started at snooze end
  ev = r.at(kDose + 35 * 60);
  TEST_ASSERT_EQUAL(1, count(ev, EngineEventKind::AlarmTick));
}

static void test_snooze_beyond_grace_then_missed() {
  Rig r(baseDb());
  r.at(kDose + 50 * 60);
  TEST_ASSERT_EQUAL(ActionResult::SnoozeBeyondGrace, r.eng->snooze(r.id(), 10, ActionSource::Button));
  TEST_ASSERT_EQUAL(ActionResult::Ok, r.eng->snooze(r.id(), 5, ActionSource::Button));
  r.at(kDose + 55 * 60);
  TEST_ASSERT_EQUAL(DoseState::Due, r.state());
  TEST_ASSERT_EQUAL(ActionResult::SnoozeBeyondGrace, r.eng->snooze(r.id(), 5, ActionSource::Button));
  r.at(kDose + 60 * 60);
  TEST_ASSERT_EQUAL(DoseState::Missed, r.state());
}

static void test_snoozed_to_taken_and_skipped() {
  Rig r(baseDb());
  r.at(kDose + 60);
  r.eng->snooze(r.id(), 15, ActionSource::Button);
  r.clock.set(kDose + 5 * 60);
  TEST_ASSERT_EQUAL(ActionResult::Ok, r.eng->take(r.id(), ActionSource::Button));
  TEST_ASSERT_EQUAL(DoseState::Taken, r.state());
  TEST_ASSERT_EQUAL(0, r.at(kDose + 20 * 60).size());
  Rig r2(baseDb());
  r2.at(kDose + 60);
  r2.eng->snooze(r2.id(), 5, ActionSource::Button);
  TEST_ASSERT_EQUAL(ActionResult::Ok, r2.eng->skip(r2.id(), ActionSource::Button));
  TEST_ASSERT_EQUAL(DoseState::Skipped, r2.state());
}

static void test_undo_window() {
  Rig r(baseDb());
  r.at(kDose + 60);
  TEST_ASSERT_EQUAL(ActionResult::NothingToUndo, r.eng->undo(r.id(), ActionSource::Button));
  r.eng->take(r.id(), ActionSource::Button);
  TEST_ASSERT_EQUAL(9, r.db.meds[0].supply);
  r.clock.set(kDose + 60 + 60);
  TEST_ASSERT_EQUAL(ActionResult::Ok, r.eng->undo(r.id(), ActionSource::Button));
  TEST_ASSERT_EQUAL(DoseState::Due, r.state());
  TEST_ASSERT_EQUAL(10, r.db.meds[0].supply);
  TEST_ASSERT_EQUAL(0, r.eng->lastTaken(1));
  TEST_ASSERT_EQUAL(DoseAction::Undo, r.log.entries.back().action);
  TEST_ASSERT_EQUAL(ActionResult::NothingToUndo, r.eng->undo(r.id(), ActionSource::Button));
  // undo restores the snooze state too
  r.eng->snooze(r.id(), 5, ActionSource::Button);
  TEST_ASSERT_EQUAL(ActionResult::Ok, r.eng->undo(r.id(), ActionSource::Button));
  TEST_ASSERT_EQUAL(DoseState::Due, r.state());
  TEST_ASSERT_EQUAL(0, r.eng->find(r.id())->snoozeCount);
  // expired after 60 s
  r.eng->skip(r.id(), ActionSource::Button);
  r.clock.advance(61);
  TEST_ASSERT_EQUAL(ActionResult::UndoExpired, r.eng->undo(r.id(), ActionSource::Button));
  TEST_ASSERT_EQUAL(DoseState::Skipped, r.state());
  // log resolves to the last effective outcome
  auto res = resolveLog(r.log.entries);
  TEST_ASSERT_TRUE(res[r.id()].resolved);
  TEST_ASSERT_EQUAL(DoseAction::Skipped, res[r.id()].action);
}

static void test_missed_backfill_and_double_dose_guard() {
  Rig r(baseDb());
  r.at(kDose + 2 * 3600);
  TEST_ASSERT_EQUAL(DoseState::Missed, r.state());
  TEST_ASSERT_EQUAL(ActionResult::TakenAtInFuture, r.eng->take(r.id(), ActionSource::Touch, kDose + 3 * 3600));
  TEST_ASSERT_EQUAL(ActionResult::TooEarly, r.eng->take(r.id(), ActionSource::Touch, kDose - 2 * 3600));
  // next dose is tomorrow 08:00: a taken-at within 1 h of it would be a double dose
  r.clock.set(kDose + 86400 - 30 * 60);
  TEST_ASSERT_EQUAL(ActionResult::WouldDoubleDose, r.eng->take(r.id(), ActionSource::Touch, kDose + 86400 - 40 * 60));
  TEST_ASSERT_EQUAL(ActionResult::Ok, r.eng->take(r.id(), ActionSource::Touch, kDose + 5 * 60));
  TEST_ASSERT_EQUAL(DoseState::Taken, r.state());  // backfilled time was on time
  const DoseLogEntry& e = r.log.entries.back();
  TEST_ASSERT_EQUAL(ActionSource::Backfill, e.source);
  TEST_ASSERT_EQUAL(kDose + 5 * 60, e.actionUtc);
  TEST_ASSERT_TRUE(e.recordedUtc > e.actionUtc);
  TEST_ASSERT_EQUAL(kDose + 5 * 60, r.eng->lastTaken(1));
  // undo of the correction returns to Missed; both entries remain in the log
  TEST_ASSERT_EQUAL(ActionResult::Ok, r.eng->undo(r.id(), ActionSource::Touch));
  TEST_ASSERT_EQUAL(DoseState::Missed, r.state());
  auto res = resolveLog(r.log.entries);
  TEST_ASSERT_EQUAL(DoseAction::Missed, res[r.id()].action);
}

static void test_quiet_hours_and_critical() {
  Database db = fx::singleMedDb(fx::fixedRule({at(23)}), day(2026, 6, 1));
  db.meds.push_back(fx::med(2, 1, day(2026, 6, 1), fx::fixedRule({at(23)})));
  db.meds[1].critical = true;
  Rig r(db);
  auto ev = r.at(fx::utc(2026, 6, 10, 21, 0));  // 23:00 CEST
  TEST_ASSERT_EQUAL(2, ev.size());
  for (const auto& e : ev) TEST_ASSERT_EQUAL(e.medId == 2, e.audible);
}

static void test_prn_lockout_and_daily_cap() {
  ScheduleRule rule;
  rule.type = ScheduleType::Prn;
  rule.prnMinIntervalMin = 240;
  rule.prnMaxPer24h = 3;
  Database db = fx::singleMedDb(rule, day(2026, 6, 1));
  db.meds.push_back(fx::med(2, 1, day(2026, 6, 1), fx::fixedRule({at(8)})));
  Rig r(db);
  TEST_ASSERT_EQUAL(ActionResult::NotPrn, r.eng->takePrn(2, ActionSource::Button));
  const int64_t t = fx::utc(2026, 6, 10, 6);
  r.at(t);
  PrnStatus s = r.eng->prnStatus(1);
  TEST_ASSERT_TRUE(s.isPrn && s.allowed);
  uint64_t id1 = 0;
  TEST_ASSERT_EQUAL(ActionResult::Ok, r.eng->takePrn(1, ActionSource::Button, &id1));
  TEST_ASSERT_TRUE(eventIdIsPrn(id1));
  s = r.eng->prnStatus(1);
  TEST_ASSERT_FALSE(s.allowed);
  TEST_ASSERT_EQUAL(t + 4 * 3600, s.nextAllowedUtc);
  r.at(t + 4 * 3600 - 60);
  TEST_ASSERT_EQUAL(ActionResult::PrnTooSoon, r.eng->takePrn(1, ActionSource::Button));
  r.at(t + 4 * 3600);
  TEST_ASSERT_EQUAL(ActionResult::Ok, r.eng->takePrn(1, ActionSource::Button));
  r.at(t + 8 * 3600);
  TEST_ASSERT_EQUAL(ActionResult::Ok, r.eng->takePrn(1, ActionSource::Button));
  r.at(t + 12 * 3600);
  s = r.eng->prnStatus(1);
  TEST_ASSERT_EQUAL(3, s.takenLast24h);
  TEST_ASSERT_EQUAL(t + 24 * 3600, s.nextAllowedUtc);  // cap frees when the first take is 24 h old
  TEST_ASSERT_EQUAL(ActionResult::PrnDailyCap, r.eng->takePrn(1, ActionSource::Button));
  r.at(t + 24 * 3600);
  TEST_ASSERT_EQUAL(ActionResult::Ok, r.eng->takePrn(1, ActionSource::Button));
  // undo a PRN take: removed and the lockout lifts back
  uint64_t id5 = 0;
  r.at(t + 36 * 3600);
  TEST_ASSERT_EQUAL(ActionResult::Ok, r.eng->takePrn(1, ActionSource::Button, &id5));
  TEST_ASSERT_EQUAL(ActionResult::Ok, r.eng->undo(id5, ActionSource::Button));
  TEST_ASSERT_EQUAL(nullptr, r.eng->find(id5));
  TEST_ASSERT_TRUE(r.eng->prnStatus(1).allowed);
  TEST_ASSERT_EQUAL(t + 24 * 3600, r.eng->lastTaken(1));
}

static Database catchupDb() {
  Database db = fx::singleMedDb(fx::fixedRule({at(8), at(13), at(21)}), day(2026, 6, 1));
  db.meds.push_back(fx::med(2, 1, day(2026, 6, 1), fx::fixedRule({at(8)})));
  db.profiles.push_back(fx::profile(2));
  db.meds.push_back(fx::med(3, 2, day(2026, 6, 1), fx::fixedRule({at(8), at(20)})));
  return db;
}

static void test_catch_up_after_long_sleep() {
  Rig r(catchupDb());
  const int64_t wake = kT0 + 3 * 86400 + 6 * 3600 + 30 * 60;  // 3 days later, 08:30 CEST
  auto ev = r.at(wake);
  // 3 days * 6 doses = 18 missed; day 4: three 08:00 doses are Due (not missed)
  TEST_ASSERT_EQUAL(18, count(ev, EngineEventKind::BecameMissed));
  TEST_ASSERT_EQUAL(18, r.log.entries.size());
  std::set<uint64_t> ids;
  for (size_t i = 0; i < r.log.entries.size(); i++) {
    TEST_ASSERT_EQUAL(DoseAction::Missed, r.log.entries[i].action);
    ids.insert(r.log.entries[i].eventId);
    if (i) TEST_ASSERT_TRUE(r.log.entries[i - 1].actionUtc <= r.log.entries[i].actionUtc);
  }
  TEST_ASSERT_EQUAL(18, ids.size());  // nothing duplicated
  for (size_t i = 1; i < ev.size(); i++) TEST_ASSERT_TRUE(ev[i - 1].atUtc <= ev[i].atUtc);  // chronological
  for (const auto& e : ev)
    if (e.atUtc < wake - 60) TEST_ASSERT_TRUE(e.stale);  // stale alarms are never rung
  size_t due = 0;
  for (const auto& d : r.eng->instances())
    if (d.state == DoseState::Due) due++;
  TEST_ASSERT_EQUAL(3, due);
  // processing again changes nothing
  TEST_ASSERT_EQUAL(0, count(r.at(wake), EngineEventKind::BecameMissed));
  TEST_ASSERT_EQUAL(18, r.log.entries.size());
}

static void test_catch_up_is_deterministic_vs_stepwise() {
  Rig big(catchupDb()), step(catchupDb());
  const int64_t end = kT0 + 5 * 86400;
  big.at(end);
  for (int64_t t = kT0; t <= end; t += 1777) step.at(t);
  step.at(end);
  TEST_ASSERT_EQUAL(big.log.entries.size(), step.log.entries.size());
  for (size_t i = 0; i < big.log.entries.size(); i++) {
    TEST_ASSERT_TRUE(big.log.entries[i].eventId == step.log.entries[i].eventId);
    TEST_ASSERT_EQUAL(big.log.entries[i].actionUtc, step.log.entries[i].actionUtc);
  }
}

static void test_snapshot_roundtrip_continues() {
  Rig r(baseDb());
  r.at(kDose + 60);
  r.eng->snooze(r.id(), 10, ActionSource::Button);
  JsonValue j = encodeSnapshot(r.eng->snapshot());
  JsonValue parsed;
  TEST_ASSERT_TRUE(parseJson(j.dump(), parsed));
  EngineSnapshot s;
  TEST_ASSERT_TRUE(decodeSnapshot(parsed, s));
  DoseEngine e2(r.clock, r.db, &r.log);
  e2.restore(s);
  TEST_ASSERT_EQUAL(DoseState::Snoozed, e2.find(r.id())->state);
  r.clock.set(kDose + 11 * 60);
  auto ev = e2.process();
  TEST_ASSERT_EQUAL(1, count(ev, EngineEventKind::SnoozeEnded));
  TEST_ASSERT_EQUAL(1, e2.find(r.id())->snoozeCount);
}

static void test_regenerate_after_edit() {
  Rig r(baseDb());
  r.db.meds[0].phases[0].rule = fx::fixedRule({at(9)});
  r.eng->regenerateFuture();
  r.eng->process();
  TEST_ASSERT_EQUAL(nullptr, r.eng->find(r.id()));
  TEST_ASSERT_NOT_NULL(r.eng->find(makeEventId(1, 1, kDose + 3600)));
}

static void test_adherence_and_streaks() {
  MemoryDoseLog log;
  auto add = [&](int d, DoseAction a, int64_t extra = 0) {
    DoseLogEntry e;
    e.profileId = 1;
    e.medId = 1;
    e.scheduledUtc = fx::utc(2026, 6, d, 6) + extra;
    e.eventId = makeEventId(1, 1, e.scheduledUtc);
    e.action = a;
    e.actionUtc = e.scheduledUtc;
    e.recordedUtc = e.scheduledUtc;
    log.append(e);
  };
  add(1, DoseAction::Taken);
  add(2, DoseAction::Taken);
  add(3, DoseAction::Missed);
  add(4, DoseAction::Taken);
  add(5, DoseAction::TakenLate);
  add(6, DoseAction::Skipped);
  add(7, DoseAction::Taken);
  add(8, DoseAction::Taken);
  add(8, DoseAction::Taken, 3600);
  // a corrected miss: missed then backfilled taken -> counts as taken
  add(9, DoseAction::Missed);
  add(9, DoseAction::TakenLate);
  AdherenceStats s = computeAdherence(log.entries, fx::berlin(), day(2026, 6, 1), day(2026, 6, 9));
  TEST_ASSERT_EQUAL(6, s.onTime);
  TEST_ASSERT_EQUAL(2, s.late);
  TEST_ASSERT_EQUAL(1, s.skipped);
  TEST_ASSERT_EQUAL(1, s.missed);
  TEST_ASSERT_EQUAL(3, s.currentStreakDays);  // 7, 8, 9
  TEST_ASSERT_EQUAL(3, s.longestStreakDays);
  TEST_ASSERT_TRUE(s.adherencePercent > 79.99 && s.adherencePercent < 80.01);
  WeeklySummary w = weeklySummary(log.entries, fx::berlin(), day(2026, 6, 3));
  TEST_ASSERT_EQUAL(day(2026, 6, 1), w.weekStartDay);  // Monday
  TEST_ASSERT_EQUAL(1, w.missed);
  TEST_ASSERT_EQUAL(1, w.skipped);
}

static void test_supply_forecast() {
  Database db = baseDb();
  db.meds[0].phases[0].rule = fx::fixedRule({at(8), at(20)});
  db.meds[0].supply = 20;
  db.meds[0].lowStockThreshold = 7;
  SupplyForecast f = forecastSupply(db.meds[0], db.profiles[0], day(2026, 6, 10));
  TEST_ASSERT_TRUE(f.tracked);
  TEST_ASSERT_FALSE(f.lowStock);
  TEST_ASSERT_EQUAL(day(2026, 6, 20), f.runOutDay);
  TEST_ASSERT_EQUAL(day(2026, 6, 17), f.refillByDay);
  db.meds[0].supply = 6;
  TEST_ASSERT_TRUE(forecastSupply(db.meds[0], db.profiles[0], day(2026, 6, 10)).lowStock);
  db.meds[0].supply = -1;
  TEST_ASSERT_FALSE(forecastSupply(db.meds[0], db.profiles[0], day(2026, 6, 10)).tracked);
}

static void test_model_validation_and_pin() {
  Database db = baseDb();
  TEST_ASSERT_EQUAL_STRING("", validateDatabase(db).c_str());
  for (int i = 2; i <= 9; i++) db.profiles.push_back(fx::profile(static_cast<uint8_t>(i)));
  TEST_ASSERT_TRUE(validateDatabase(db).size() > 0);  // 9 profiles
  db = baseDb();
  for (int i = 2; i <= 31; i++) db.meds.push_back(fx::med(static_cast<uint16_t>(i), 1, day(2026, 6, 1), fx::fixedRule({at(8)})));
  TEST_ASSERT_TRUE(validateDatabase(db).size() > 0);  // 31 meds
  db = baseDb();
  db.meds[0].phases[0].rule.times.clear();
  for (int i = 0; i < 13; i++) db.meds[0].phases[0].rule.times.push_back(at(i));
  TEST_ASSERT_TRUE(validateMedication(db.meds[0]).size() > 0);  // 13 times
  Profile p = fx::profile();
  TEST_ASSERT_TRUE(p.privacy);
  p.pinSalt = "a1b2c3d4";
  p.pinHash = hashPin("4711", p.pinSalt);
  TEST_ASSERT_TRUE(checkPin(p, "4711"));
  TEST_ASSERT_FALSE(checkPin(p, "4712"));
  TEST_ASSERT_TRUE(inQuietHours(p, 23 * 60));
  TEST_ASSERT_FALSE(inQuietHours(p, 12 * 60));
  TEST_ASSERT_EQUAL_STRING("Ibu", utf8Truncate("Ibu\xC3\xBC", 4).c_str());
  TEST_ASSERT_FALSE(utf8Valid("\xC3"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_pending_to_due_and_alarm_ladder);
  RUN_TEST(test_due_to_missed_after_grace);
  RUN_TEST(test_due_to_taken_on_time_and_supply);
  RUN_TEST(test_due_to_taken_late);
  RUN_TEST(test_pending_early_take_and_too_early);
  RUN_TEST(test_skip_from_pending_and_due);
  RUN_TEST(test_snooze_cycle_and_limits);
  RUN_TEST(test_snooze_beyond_grace_then_missed);
  RUN_TEST(test_snoozed_to_taken_and_skipped);
  RUN_TEST(test_undo_window);
  RUN_TEST(test_missed_backfill_and_double_dose_guard);
  RUN_TEST(test_quiet_hours_and_critical);
  RUN_TEST(test_prn_lockout_and_daily_cap);
  RUN_TEST(test_catch_up_after_long_sleep);
  RUN_TEST(test_catch_up_is_deterministic_vs_stepwise);
  RUN_TEST(test_snapshot_roundtrip_continues);
  RUN_TEST(test_regenerate_after_edit);
  RUN_TEST(test_adherence_and_streaks);
  RUN_TEST(test_supply_forecast);
  RUN_TEST(test_model_validation_and_pin);
  return UNITY_END();
}
