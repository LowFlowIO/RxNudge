#include <unity.h>

#include <set>

#include "../common/fixtures.h"
#include "core/schedule.h"

using namespace core;
using fx::at;
using fx::berlinLocal;
using fx::day;

void setUp() {}
void tearDown() {}

static std::vector<Occurrence> expand(const Database& db, int64_t from, int64_t to) {
  std::vector<Occurrence> out;
  expandOccurrences(db, fx::berlin(), from, to, out);
  return out;
}
// Occurrences on local days [d0, d1]
static std::vector<Occurrence> expandDays(const Database& db, int32_t d0, int32_t d1) {
  return expand(db, localToUtc(fx::berlin(), d0, 0) - 1, localToUtc(fx::berlin(), d1 + 1, 0) - 1);
}

static void test_fixed_times_daily() {
  Database db = fx::singleMedDb(fx::fixedRule({at(8), at(20), at(14)}), day(2026, 6, 1));
  auto o = expandDays(db, day(2026, 6, 1), day(2026, 6, 7));
  TEST_ASSERT_EQUAL(21, o.size());
  TEST_ASSERT_EQUAL(berlinLocal(2026, 6, 1, 8), o[0].scheduledUtc);
  TEST_ASSERT_EQUAL(berlinLocal(2026, 6, 1, 14), o[1].scheduledUtc);
  TEST_ASSERT_EQUAL(berlinLocal(2026, 6, 7, 20), o[20].scheduledUtc);
  for (size_t i = 1; i < o.size(); i++) TEST_ASSERT_TRUE(o[i - 1].scheduledUtc < o[i].scheduledUtc);
  // nothing before start
  TEST_ASSERT_EQUAL(0, expandDays(db, day(2026, 5, 20), day(2026, 5, 31)).size());
}

static void test_routine_anchored() {
  ScheduleRule r;
  r.type = ScheduleType::Routine;
  DoseTime t1;
  t1.routine = Routine::Breakfast;
  t1.offsetMin = -30;
  DoseTime t2;
  t2.routine = Routine::Bed;
  r.times = {t1, t2};
  Database db = fx::singleMedDb(r, day(2026, 6, 1));
  db.profiles[0].routineMin[1] = 8 * 60;   // breakfast 08:00
  db.profiles[0].routineMin[4] = 23 * 60;  // bed 23:00
  auto o = expandDays(db, day(2026, 6, 2), day(2026, 6, 2));
  TEST_ASSERT_EQUAL(2, o.size());
  TEST_ASSERT_EQUAL(berlinLocal(2026, 6, 2, 7, 30), o[0].scheduledUtc);
  TEST_ASSERT_EQUAL(berlinLocal(2026, 6, 2, 23), o[1].scheduledUtc);
  // routine change moves the doses
  db.profiles[0].routineMin[1] = 9 * 60;
  o = expandDays(db, day(2026, 6, 2), day(2026, 6, 2));
  TEST_ASSERT_EQUAL(berlinLocal(2026, 6, 2, 8, 30), o[0].scheduledUtc);
  // clamped to the same day
  t2.offsetMin = 120;
  TEST_ASSERT_EQUAL(1439, resolveMinute(t2, db.profiles[0]));
}

static void test_weekdays() {
  ScheduleRule r = fx::fixedRule({at(9)});
  r.type = ScheduleType::Weekdays;
  r.weekdayMask = (1 << 1) | (1 << 3) | (1 << 5);  // Mon Wed Fri
  Database db = fx::singleMedDb(r, day(2026, 10, 1));
  auto o = expandDays(db, day(2026, 10, 12), day(2026, 10, 25));  // Mon 12th .. Sun 25th
  TEST_ASSERT_EQUAL(6, o.size());
  for (const auto& x : o) {
    const int wd = weekdayFromDays(x.localDay);
    TEST_ASSERT_TRUE(wd == 1 || wd == 3 || wd == 5);
  }
}

static void test_every_n_days_from_start() {
  ScheduleRule r = fx::fixedRule({at(8)});
  r.type = ScheduleType::EveryN;
  r.intervalN = 3;
  Database db = fx::singleMedDb(r, day(2026, 1, 30));
  // window starts long after start: still anchored at Jan 30 -> Feb 2, 5, 8 ...
  auto o = expandDays(db, day(2026, 2, 3), day(2026, 2, 12));
  TEST_ASSERT_EQUAL(3, o.size());
  TEST_ASSERT_EQUAL(day(2026, 2, 5), o[0].localDay);
  TEST_ASSERT_EQUAL(day(2026, 2, 8), o[1].localDay);
  TEST_ASSERT_EQUAL(day(2026, 2, 11), o[2].localDay);
  // across a leap day and year boundary (every 2 days from 2027-12-30)
  db.meds[0].startDay = day(2027, 12, 30);
  db.meds[0].phases[0].rule.intervalN = 2;
  o = expandDays(db, day(2027, 12, 30), day(2028, 3, 2));
  std::set<int32_t> days;
  for (auto& x : o) days.insert(x.localDay);
  TEST_ASSERT_TRUE(days.count(day(2028, 1, 1)));
  TEST_ASSERT_TRUE(days.count(day(2028, 2, 28)));
  TEST_ASSERT_FALSE(days.count(day(2028, 2, 29)));
  TEST_ASSERT_TRUE(days.count(day(2028, 3, 1)));
}

static void test_every_n_weeks() {
  ScheduleRule r = fx::fixedRule({at(10)});
  r.type = ScheduleType::EveryN;
  r.intervalN = 2;
  r.intervalWeeks = true;
  Database db = fx::singleMedDb(r, day(2026, 10, 5));
  auto o = expandDays(db, day(2026, 10, 5), day(2026, 11, 30));
  TEST_ASSERT_EQUAL(5, o.size());  // Oct 5, 19, Nov 2, 16, 30
  TEST_ASSERT_EQUAL(day(2026, 11, 30), o[4].localDay);
  // Oct 19 is before DST end, Nov 2 after: still 10:00 local
  TEST_ASSERT_EQUAL(berlinLocal(2026, 11, 2, 10), o[2].scheduledUtc);
  TEST_ASSERT_EQUAL(o[1].scheduledUtc + 14 * 86400 + 3600, o[2].scheduledUtc);
}

static void test_specific_dates_including_leap_day() {
  ScheduleRule r = fx::fixedRule({at(7, 15)});
  r.type = ScheduleType::SpecificDates;
  r.dates = {day(2028, 2, 29), day(2027, 12, 31), day(2028, 1, 1)};
  Database db = fx::singleMedDb(r, day(2027, 12, 1));
  auto o = expandDays(db, day(2027, 12, 1), day(2028, 3, 31));
  TEST_ASSERT_EQUAL(3, o.size());
  TEST_ASSERT_EQUAL(day(2027, 12, 31), o[0].localDay);
  TEST_ASSERT_EQUAL(day(2028, 1, 1), o[1].localDay);
  TEST_ASSERT_EQUAL(berlinLocal(2028, 2, 29, 7, 15), o[2].scheduledUtc);
}

static void test_cycle_on_off() {
  ScheduleRule r = fx::fixedRule({at(21)});
  r.type = ScheduleType::Cycle;
  r.cycleOn = 21;
  r.cycleOff = 7;
  Database db = fx::singleMedDb(r, day(2026, 1, 1));
  auto o = expandDays(db, day(2026, 1, 1), day(2026, 3, 25));  // 84 days = 3 cycles
  TEST_ASSERT_EQUAL(63, o.size());
  std::set<int32_t> days;
  for (auto& x : o) days.insert(x.localDay);
  TEST_ASSERT_TRUE(days.count(day(2026, 1, 21)));
  TEST_ASSERT_FALSE(days.count(day(2026, 1, 22)));
  TEST_ASSERT_FALSE(days.count(day(2026, 1, 28)));
  TEST_ASSERT_TRUE(days.count(day(2026, 1, 29)));
}

static void test_prn_has_no_scheduled_occurrences() {
  ScheduleRule r;
  r.type = ScheduleType::Prn;
  Database db = fx::singleMedDb(r, day(2026, 1, 1));
  TEST_ASSERT_EQUAL(0, expandDays(db, day(2026, 1, 1), day(2026, 2, 1)).size());
  TEST_ASSERT_TRUE(eventIdIsPrn(makeEventId(1, 1, 1000, true)));
  TEST_ASSERT_FALSE(eventIdIsPrn(makeEventId(1, 1, 1000, false)));
}

static void test_multi_phase_dose_change() {
  Database db = fx::singleMedDb(fx::fixedRule({at(8), at(20)}), day(2026, 5, 1));
  Medication& m = db.meds[0];
  m.phases[0].toDay = day(2026, 5, 7);
  m.phases[0].doseText = "2 tablets";
  Phase p2;
  p2.fromDay = day(2026, 5, 8);
  p2.toDay = day(2026, 5, 14);
  p2.doseText = "1 tablet";
  p2.rule = fx::fixedRule({at(8)});
  Phase p3;  // taper: every other day from May 15
  p3.fromDay = day(2026, 5, 15);
  p3.rule = fx::fixedRule({at(8)});
  p3.rule.type = ScheduleType::EveryN;
  p3.rule.intervalN = 2;
  m.phases.push_back(p2);
  m.phases.push_back(p3);
  TEST_ASSERT_EQUAL_STRING("", validateMedication(m).c_str());
  auto o = expandDays(db, day(2026, 5, 1), day(2026, 5, 20));
  TEST_ASSERT_EQUAL(14 + 7 + 3, o.size());  // phase 3: 15, 17, 19
  TEST_ASSERT_EQUAL(0, o[0].phaseIndex);
  TEST_ASSERT_EQUAL(1, o[14].phaseIndex);
  TEST_ASSERT_EQUAL(2, o[21].phaseIndex);
  TEST_ASSERT_EQUAL(day(2026, 5, 17), o[22].localDay);
  // overlapping phases are rejected
  m.phases[1].fromDay = day(2026, 5, 6);
  TEST_ASSERT_TRUE(validateMedication(m).size() > 0);
}

static void test_course_length_and_end_date() {
  Database db = fx::singleMedDb(fx::fixedRule({at(8)}), day(2026, 2, 26));
  db.meds[0].courseDays = 5;  // Feb 26 .. Mar 2
  auto o = expandDays(db, day(2026, 2, 1), day(2026, 3, 31));
  TEST_ASSERT_EQUAL(5, o.size());
  TEST_ASSERT_EQUAL(day(2026, 3, 2), o.back().localDay);
  db.meds[0].endDay = day(2026, 2, 27);  // explicit end wins
  TEST_ASSERT_EQUAL(2, expandDays(db, day(2026, 2, 1), day(2026, 3, 31)).size());
  db.meds[0].active = false;
  TEST_ASSERT_EQUAL(0, expandDays(db, day(2026, 2, 1), day(2026, 3, 31)).size());
}

static void test_dst_spring_forward_daily_dose() {
  Database db = fx::singleMedDb(fx::fixedRule({at(2, 30), at(3, 0)}), day(2026, 3, 27));
  auto o = expandDays(db, day(2026, 3, 28), day(2026, 3, 30));
  // Mar 29: 02:30 does not exist -> 03:00; collides with the 03:00 dose -> one event (never two)
  TEST_ASSERT_EQUAL(5, o.size());
  TEST_ASSERT_EQUAL(fx::utc(2026, 3, 28, 1, 30), o[0].scheduledUtc);
  TEST_ASSERT_EQUAL(fx::utc(2026, 3, 29, 1, 0), o[2].scheduledUtc);
  TEST_ASSERT_EQUAL(fx::utc(2026, 3, 30, 0, 30), o[3].scheduledUtc);
  // only the 02:30 dose: fires at 03:00 CEST exactly once
  db.meds[0].phases[0].rule = fx::fixedRule({at(2, 30)});
  o = expandDays(db, day(2026, 3, 29), day(2026, 3, 29));
  TEST_ASSERT_EQUAL(1, o.size());
  TEST_ASSERT_EQUAL(fx::utc(2026, 3, 29, 1, 0), o[0].scheduledUtc);
}

static void test_dst_fall_back_daily_dose() {
  Database db = fx::singleMedDb(fx::fixedRule({at(2, 30)}), day(2026, 10, 20));
  auto o = expandDays(db, day(2026, 10, 24), day(2026, 10, 26));
  TEST_ASSERT_EQUAL(3, o.size());  // once per day, the ambiguous day fires once
  TEST_ASSERT_EQUAL(fx::utc(2026, 10, 24, 0, 30), o[0].scheduledUtc);
  TEST_ASSERT_EQUAL(fx::utc(2026, 10, 25, 0, 30), o[1].scheduledUtc);  // first 02:30 (CEST)
  TEST_ASSERT_EQUAL(fx::utc(2026, 10, 26, 1, 30), o[2].scheduledUtc);
  // the day is 25 h long for an 08:00 dose
  db.meds[0].phases[0].rule = fx::fixedRule({at(8)});
  o = expandDays(db, day(2026, 10, 24), day(2026, 10, 25));
  TEST_ASSERT_EQUAL(25 * 3600, o[1].scheduledUtc - o[0].scheduledUtc);
}

static void test_month_year_leap_boundaries() {
  Database db = fx::singleMedDb(fx::fixedRule({at(0, 0), at(23, 59)}), day(2027, 12, 30));
  auto o = expandDays(db, day(2027, 12, 31), day(2028, 1, 1));
  TEST_ASSERT_EQUAL(4, o.size());
  TEST_ASSERT_EQUAL(fx::utc(2027, 12, 30, 23, 0), o[0].scheduledUtc);  // 00:00 CET = 23:00Z previous day
  TEST_ASSERT_EQUAL(day(2027, 12, 31), o[0].localDay);
  TEST_ASSERT_EQUAL(day(2028, 1, 1), o[2].localDay);
  o = expandDays(db, day(2028, 2, 28), day(2028, 3, 1));
  TEST_ASSERT_EQUAL(6, o.size());
  TEST_ASSERT_EQUAL(day(2028, 2, 29), o[2].localDay);
  db.meds[0].startDay = day(2026, 1, 1);
  o = expandDays(db, day(2026, 4, 30), day(2026, 5, 1));
  TEST_ASSERT_EQUAL(4, o.size());
}

static void test_ids_deterministic_and_windows_compose() {
  Database db = fx::singleMedDb(fx::fixedRule({at(8), at(12), at(20)}), day(2026, 1, 1));
  db.meds.push_back(fx::med(2, 1, day(2026, 1, 1), fx::fixedRule({at(8)})));
  const int64_t a = fx::utc(2026, 3, 1), b = fx::utc(2026, 4, 15);
  auto whole = expand(db, a, b);
  std::vector<Occurrence> parts;
  for (int64_t t = a; t < b; t += 7 * 3600 + 13) {
    auto p = expand(db, t, std::min(b, t + 7 * 3600 + 13));
    parts.insert(parts.end(), p.begin(), p.end());
  }
  TEST_ASSERT_EQUAL(whole.size(), parts.size());
  std::set<uint64_t> ids;
  for (size_t i = 0; i < whole.size(); i++) {
    TEST_ASSERT_TRUE(whole[i].eventId == parts[i].eventId);
    ids.insert(whole[i].eventId);
  }
  TEST_ASSERT_EQUAL(whole.size(), ids.size());
  // same input -> same ids
  auto again = expand(db, a, b);
  TEST_ASSERT_TRUE(again[5].eventId == whole[5].eventId);
}

static void test_doses_per_day() {
  Database db = fx::singleMedDb(fx::fixedRule({at(8), at(20)}), day(2026, 1, 1));
  TEST_ASSERT_TRUE(dosesPerDay(db.meds[0], db.profiles[0], day(2026, 1, 1), 28) == 2.0);
  db.meds[0].phases[0].rule.type = ScheduleType::EveryN;
  db.meds[0].phases[0].rule.intervalN = 2;
  TEST_ASSERT_TRUE(dosesPerDay(db.meds[0], db.profiles[0], day(2026, 1, 1), 28) == 1.0);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_fixed_times_daily);
  RUN_TEST(test_routine_anchored);
  RUN_TEST(test_weekdays);
  RUN_TEST(test_every_n_days_from_start);
  RUN_TEST(test_every_n_weeks);
  RUN_TEST(test_specific_dates_including_leap_day);
  RUN_TEST(test_cycle_on_off);
  RUN_TEST(test_prn_has_no_scheduled_occurrences);
  RUN_TEST(test_multi_phase_dose_change);
  RUN_TEST(test_course_length_and_end_date);
  RUN_TEST(test_dst_spring_forward_daily_dose);
  RUN_TEST(test_dst_fall_back_daily_dose);
  RUN_TEST(test_month_year_leap_boundaries);
  RUN_TEST(test_ids_deterministic_and_windows_compose);
  RUN_TEST(test_doses_per_day);
  return UNITY_END();
}
