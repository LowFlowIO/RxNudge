#include <unity.h>

#include "../common/fixtures.h"
#include "core/civil.h"
#include "core/tz.h"

using namespace core;
using fx::utc;

void setUp() {}
void tearDown() {}

static void test_civil_roundtrip_and_weekday() {
  TEST_ASSERT_EQUAL(0, daysFromCivil(1970, 1, 1));
  TEST_ASSERT_EQUAL(4, weekdayFromDays(0));  // Thursday
  TEST_ASSERT_EQUAL(0, weekdayFromDays(daysFromCivil(2026, 10, 18)));  // Sunday
  for (int32_t d = -800000; d < 800000; d += 997) {
    const Ymd y = civilFromDays(d);
    TEST_ASSERT_EQUAL(d, daysFromCivil(y.y, y.m, y.d));
  }
  TEST_ASSERT_EQUAL(-1, floorDiv(-1, 86400));
  TEST_ASSERT_EQUAL(86399, floorMod(-1, 86400));
}

static void test_leap_years_and_month_lengths() {
  TEST_ASSERT_TRUE(isLeapYear(2028));
  TEST_ASSERT_TRUE(isLeapYear(2000));
  TEST_ASSERT_FALSE(isLeapYear(2100));
  TEST_ASSERT_FALSE(isLeapYear(2026));
  TEST_ASSERT_EQUAL(29, daysInMonth(2028, 2));
  TEST_ASSERT_EQUAL(28, daysInMonth(2026, 2));
  TEST_ASSERT_EQUAL(31, daysInMonth(2026, 12));
  TEST_ASSERT_EQUAL(daysFromCivil(2028, 3, 1) - 1, daysFromCivil(2028, 2, 29));
  TEST_ASSERT_EQUAL(daysFromCivil(2027, 1, 1) - 1, daysFromCivil(2026, 12, 31));
}

static void test_parse_berlin() {
  PosixTz tz;
  TEST_ASSERT_TRUE(parsePosixTz(kDefaultTz, tz));
  TEST_ASSERT_EQUAL_STRING("CET", tz.stdName.c_str());
  TEST_ASSERT_EQUAL_STRING("CEST", tz.dstName.c_str());
  TEST_ASSERT_EQUAL(3600, tz.stdOffset);
  TEST_ASSERT_EQUAL(7200, tz.dstOffset);
  TEST_ASSERT_TRUE(tz.hasDst);
  TEST_ASSERT_EQUAL(3, tz.start.month);
  TEST_ASSERT_EQUAL(5, tz.start.week);
  TEST_ASSERT_EQUAL(0, tz.start.wday);
  TEST_ASSERT_EQUAL(7200, tz.start.timeSec);
  TEST_ASSERT_EQUAL(10, tz.end.month);
  TEST_ASSERT_EQUAL(10800, tz.end.timeSec);
}

static void test_parse_variants_and_errors() {
  PosixTz tz;
  TEST_ASSERT_TRUE(parsePosixTz("UTC0", tz));
  TEST_ASSERT_EQUAL(0, tz.stdOffset);
  TEST_ASSERT_FALSE(tz.hasDst);
  TEST_ASSERT_TRUE(parsePosixTz("<+0530>-5:30", tz));
  TEST_ASSERT_EQUAL(5 * 3600 + 1800, tz.stdOffset);
  TEST_ASSERT_TRUE(parsePosixTz("EST5EDT", tz));  // no rules -> US default
  TEST_ASSERT_EQUAL(-5 * 3600, tz.stdOffset);
  TEST_ASSERT_EQUAL(-4 * 3600, tz.dstOffset);
  TEST_ASSERT_EQUAL(utc(2026, 3, 8, 7), dstStartUtc(tz, 2026));  // 2nd Sunday of March 02:00 EST
  TEST_ASSERT_TRUE(parsePosixTz("XXX3YYY,J60/1,300", tz));
  TEST_ASSERT_EQUAL(TzRule::Kind::Julian1, tz.start.kind);
  TEST_ASSERT_EQUAL(TzRule::Kind::Julian0, tz.end.kind);
  TEST_ASSERT_FALSE(parsePosixTz("", tz));
  TEST_ASSERT_FALSE(parsePosixTz("CET", tz));
  TEST_ASSERT_FALSE(parsePosixTz("CET-1CEST,M13.5.0,M10.5.0", tz));
  TEST_ASSERT_FALSE(parsePosixTz("CET-1CEST,M3.5.0", tz));
  TEST_ASSERT_FALSE(parsePosixTz("CET-1CEST,M3.5.0,M10.5.0/3garbage", tz));
}

static void test_berlin_transitions_2026() {
  const PosixTz& tz = fx::berlin();
  TEST_ASSERT_EQUAL(utc(2026, 3, 29, 1), dstStartUtc(tz, 2026));
  TEST_ASSERT_EQUAL(utc(2026, 10, 25, 1), dstEndUtc(tz, 2026));
  TEST_ASSERT_EQUAL(utc(2027, 3, 28, 1), dstStartUtc(tz, 2027));
  TEST_ASSERT_EQUAL(utc(2028, 10, 29, 1), dstEndUtc(tz, 2028));
  TEST_ASSERT_EQUAL(3600, utcOffsetAt(tz, utc(2026, 3, 29, 0, 59, 59)));
  TEST_ASSERT_EQUAL(7200, utcOffsetAt(tz, utc(2026, 3, 29, 1)));
  TEST_ASSERT_EQUAL(7200, utcOffsetAt(tz, utc(2026, 10, 25, 0, 59, 59)));
  TEST_ASSERT_EQUAL(3600, utcOffsetAt(tz, utc(2026, 10, 25, 1)));
  const LocalTime lt = utcToLocal(tz, utc(2026, 7, 1, 22, 30));
  TEST_ASSERT_EQUAL(daysFromCivil(2026, 7, 2), lt.days);
  TEST_ASSERT_EQUAL(30 * 60, lt.secOfDay);
  TEST_ASSERT_TRUE(lt.dst);
}

static void test_spring_forward_gap() {
  const PosixTz& tz = fx::berlin();
  LocalResolution r;
  const int64_t t = localToUtc(tz, daysFromCivil(2026, 3, 29), 2 * 3600 + 30 * 60, &r);
  TEST_ASSERT_EQUAL(LocalResolution::Gap, r);
  TEST_ASSERT_EQUAL(utc(2026, 3, 29, 1), t);  // 03:00 CEST, first valid minute after the gap
  const LocalTime lt = utcToLocal(tz, t);
  TEST_ASSERT_EQUAL(3 * 3600, lt.secOfDay);
  localToUtc(tz, daysFromCivil(2026, 3, 29), 3 * 3600, &r);
  TEST_ASSERT_EQUAL(LocalResolution::Unique, r);
  TEST_ASSERT_EQUAL(utc(2026, 3, 29, 0, 59), localToUtc(tz, daysFromCivil(2026, 3, 29), 1 * 3600 + 59 * 60, &r));
  TEST_ASSERT_EQUAL(LocalResolution::Unique, r);
}

static void test_fall_back_ambiguous() {
  const PosixTz& tz = fx::berlin();
  LocalResolution r;
  const int64_t t = localToUtc(tz, daysFromCivil(2026, 10, 25), 2 * 3600 + 30 * 60, &r);
  TEST_ASSERT_EQUAL(LocalResolution::Ambiguous, r);
  TEST_ASSERT_EQUAL(utc(2026, 10, 25, 0, 30), t);  // first occurrence (CEST)
  TEST_ASSERT_EQUAL(utc(2026, 10, 25, 2), localToUtc(tz, daysFromCivil(2026, 10, 25), 3 * 3600, &r));
  TEST_ASSERT_EQUAL(LocalResolution::Unique, r);
}

static void test_southern_hemisphere() {
  PosixTz tz;
  TEST_ASSERT_TRUE(parsePosixTz("NZST-12NZDT,M9.5.0,M4.1.0/3", tz));
  TEST_ASSERT_EQUAL(13 * 3600, utcOffsetAt(tz, utc(2026, 1, 15)));
  TEST_ASSERT_EQUAL(12 * 3600, utcOffsetAt(tz, utc(2026, 7, 15)));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_civil_roundtrip_and_weekday);
  RUN_TEST(test_leap_years_and_month_lengths);
  RUN_TEST(test_parse_berlin);
  RUN_TEST(test_parse_variants_and_errors);
  RUN_TEST(test_berlin_transitions_2026);
  RUN_TEST(test_spring_forward_gap);
  RUN_TEST(test_fall_back_ambiguous);
  RUN_TEST(test_southern_hemisphere);
  return UNITY_END();
}
