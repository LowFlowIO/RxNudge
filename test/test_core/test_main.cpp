#include <unity.h>

#include <cstring>

#include "core/selftest_logic.h"

using namespace core;

void setUp() {}
void tearDown() {}

static void test_menu_wraps_and_hits() {
  MenuModel m(8, 100, 40);
  TEST_ASSERT_EQUAL(0, m.selected());
  m.previous();
  TEST_ASSERT_EQUAL(7, m.selected());
  m.next();
  TEST_ASSERT_EQUAL(0, m.selected());
  TEST_ASSERT_EQUAL(-1, m.hitTest(99));
  TEST_ASSERT_EQUAL(0, m.hitTest(100));
  TEST_ASSERT_EQUAL(1, m.hitTest(140));
  TEST_ASSERT_EQUAL(7, m.hitTest(100 + 8 * 40 - 1));
  TEST_ASSERT_EQUAL(-1, m.hitTest(100 + 8 * 40));
  m.select(5);
  TEST_ASSERT_EQUAL(5, m.selected());
  m.select(42);
  TEST_ASSERT_EQUAL(5, m.selected());
}

static void test_menu_grid_rows_9mm() {
  // self-test menu: 10 items in 2 columns of 5, rows of at least 9 mm on the 235 ppi panel
  const int rowH = 84;
  TEST_ASSERT_TRUE(rowH * 25.4 / 235.0 >= 9.0);
  MenuModel m(10, 56, rowH, 5, 16, 384);
  TEST_ASSERT_TRUE(m.rowTop(4) + rowH <= 480);
  TEST_ASSERT_EQUAL(56, m.rowTop(5));
  TEST_ASSERT_EQUAL(16 + 384, m.columnLeft(5));
  TEST_ASSERT_EQUAL(0, m.hitTest(20, 60));
  TEST_ASSERT_EQUAL(4, m.hitTest(20, 56 + 4 * rowH + 1));
  TEST_ASSERT_EQUAL(5, m.hitTest(16 + 384 + 5, 60));
  TEST_ASSERT_EQUAL(9, m.hitTest(16 + 384 + 5, 56 + 4 * rowH + 1));
  TEST_ASSERT_EQUAL(-1, m.hitTest(20, 56 + 5 * rowH));
  TEST_ASSERT_EQUAL(-1, m.hitTest(20, 40));
}

static void test_combo_gate_fires_once() {
  ComboGate g(3000);
  TEST_ASSERT_FALSE(g.update(0, true, true));
  TEST_ASSERT_FALSE(g.update(2999, true, true));
  TEST_ASSERT_TRUE(g.update(3000, true, true));
  TEST_ASSERT_FALSE(g.update(5000, true, true));
  TEST_ASSERT_FALSE(g.update(5001, true, false));
  TEST_ASSERT_FALSE(g.update(6000, true, true));
  TEST_ASSERT_TRUE(g.update(9000, true, true));
}

static void test_combo_gate_requires_both() {
  ComboGate g(100);
  TEST_ASSERT_FALSE(g.update(0, true, false));
  TEST_ASSERT_FALSE(g.update(1000, true, false));
  TEST_ASSERT_FALSE(g.update(1000, false, true));
}

static void test_press_classifier() {
  TEST_ASSERT_EQUAL(PressKind::None, classifyPress(10, 1000));
  TEST_ASSERT_EQUAL(PressKind::Short, classifyPress(50, 1000));
  TEST_ASSERT_EQUAL(PressKind::Short, classifyPress(999, 1000));
  TEST_ASSERT_EQUAL(PressKind::Long, classifyPress(1000, 1000));
}

static void test_sleep_micros_clamps() {
  TEST_ASSERT_EQUAL_UINT64(1000000ULL, sleepMicros(0));
  TEST_ASSERT_EQUAL_UINT64(120000000ULL, sleepMicros(kSelfTestWakeSeconds));
  TEST_ASSERT_EQUAL_UINT64(static_cast<uint64_t>(kMaxSleepSeconds) * 1000000ULL, sleepMicros(0xFFFFFFFFu));
}

static void test_judge_wake() {
  WakeVerdict v = judgeWake(true, 120, 121);
  TEST_ASSERT_TRUE(v.pass);
  TEST_ASSERT_EQUAL(1, v.errorSeconds);
  v = judgeWake(true, 120, 127);
  TEST_ASSERT_FALSE(v.timingOk);
  v = judgeWake(true, 120, 117);
  TEST_ASSERT_FALSE(v.pass);
  v = judgeWake(false, 120, 120);
  TEST_ASSERT_FALSE(v.pass);
  TEST_ASSERT_TRUE(v.timingOk);
  v = judgeWake(true, 3600, 3770);  // 5 % of 3600 = 180
  TEST_ASSERT_TRUE(v.pass);
}

static void test_crc_and_pattern() {
  const char* s = "123456789";
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, crc32(reinterpret_cast<const uint8_t*>(s), 9));
  uint8_t a[256], b[256];
  fillPattern(a, sizeof(a), 42);
  fillPattern(b, sizeof(b), 42);
  TEST_ASSERT_EQUAL_MEMORY(a, b, sizeof(a));
  fillPattern(b, sizeof(b), 43);
  TEST_ASSERT_TRUE(std::memcmp(a, b, sizeof(a)) != 0);
}

static void test_battery() {
  TEST_ASSERT_EQUAL(BatteryHealth::Unknown, classifyBattery(-1, 3900));
  TEST_ASSERT_EQUAL(BatteryHealth::Critical, classifyBattery(50, 3300));
  TEST_ASSERT_EQUAL(BatteryHealth::Critical, classifyBattery(4, 3700));
  TEST_ASSERT_EQUAL(BatteryHealth::Low, classifyBattery(15, 3700));
  TEST_ASSERT_EQUAL(BatteryHealth::Ok, classifyBattery(60, 3900));
  TEST_ASSERT_EQUAL(BatteryHealth::Full, classifyBattery(100, 4200));
  TEST_ASSERT_EQUAL_STRING("LOW", batteryHealthName(BatteryHealth::Low));
  TEST_ASSERT_EQUAL(250, bq27220TempToDeciC(2982));
}

static void test_corner_targets() {
  Rect r[4];
  cornerTargets(800, 480, 80, 20, r);
  TEST_ASSERT_TRUE(r[0].contains(20, 20));
  TEST_ASSERT_TRUE(r[1].contains(779, 20));
  TEST_ASSERT_FALSE(r[1].contains(780, 20));
  TEST_ASSERT_TRUE(r[2].contains(20, 459));
  TEST_ASSERT_TRUE(r[3].contains(700, 380));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_menu_wraps_and_hits);
  RUN_TEST(test_menu_grid_rows_9mm);
  RUN_TEST(test_combo_gate_fires_once);
  RUN_TEST(test_combo_gate_requires_both);
  RUN_TEST(test_press_classifier);
  RUN_TEST(test_sleep_micros_clamps);
  RUN_TEST(test_judge_wake);
  RUN_TEST(test_crc_and_pattern);
  RUN_TEST(test_battery);
  RUN_TEST(test_corner_targets);
  return UNITY_END();
}
