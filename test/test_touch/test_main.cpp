// Touch debounce / click-through guard (ui_core/touch_filter.h) with simulated GT911 report streams.
#include <unity.h>

#include <cstdint>
#include <string>

#include "ui_core/touch_filter.h"

using namespace ui;

void setUp() {}
void tearDown() {}

namespace {

// One char per poll (kTouchPollMs apart): 'D' down report, 'S' stale (buffer bit clear), 'U' up report,
// '2' two-finger down, 'M' down moved far away.
struct Sim {
  TouchFilter f;
  uint32_t t = 1000;
  int taps = 0;
  int16_t lastX = -1, lastY = -1;
  int run(const std::string& seq, int16_t x = 200, int16_t y = 300) {
    int n = 0;
    for (char c : seq) {
      TouchReport r;
      r.x = x;
      r.y = y;
      r.trackId = 0;
      if (c == 'D' || c == '2' || c == 'M') {
        r.kind = TouchReport::Down;
        r.count = c == '2' ? 2 : 1;
        if (c == 'M') r.x = static_cast<int16_t>(x + kTouchSlopPx + 20);
      } else if (c == 'U') {
        r.kind = TouchReport::Up;
      }
      const TouchTap tap = f.feed(r, t);
      if (tap.tap) {
        ++n;
        lastX = tap.x;
        lastY = tap.y;
      }
      t += kTouchPollMs;
    }
    taps += n;
    return n;
  }
  void idle(uint32_t ms) { run(std::string(ms / kTouchPollMs, 'S')); }
};

}  // namespace

void test_clean_tap_emits_on_release_only() {
  Sim s;
  TEST_ASSERT_EQUAL(0, s.run("DDDDD"));
  TEST_ASSERT_TRUE(s.f.down());
  TEST_ASSERT_EQUAL(1, s.run("U"));
  TEST_ASSERT_EQUAL(200, s.lastX);
  TEST_ASSERT_EQUAL(300, s.lastY);
  TEST_ASSERT_EQUAL(0, s.run("UUUSSS"));  // duplicate up reports
}

// The v1.0.2 bug: "buffer not ready" between reports was read as a release -> 3 taps for one touch.
void test_stale_reports_are_not_releases() {
  Sim s;
  TEST_ASSERT_EQUAL(0, s.run("DSSDSDSSSDSDSS"));
  TEST_ASSERT_EQUAL(1, s.run("U"));
}

void test_duplicate_down_reports_dedupe() {
  Sim s;
  TEST_ASSERT_EQUAL(1, s.run("DDDDDDDDDDDDDDDDDDDDU"));
}

void test_bounce_within_min_up_time_is_same_touch() {
  Sim s;
  TEST_ASSERT_EQUAL(1, s.run("DDDDU"));
  // finger bounces: up 30 ms, down 40 ms, up
  TEST_ASSERT_EQUAL(0, s.run("SSDDDDU"));
  TEST_ASSERT_EQUAL(0, s.run("SDDU"));  // repeated bounces keep being ignored
  s.idle(kTouchMinUpMs + 20);
  TEST_ASSERT_EQUAL(1, s.run("DDDDU"));  // a real second tap
}

void test_min_press_time() {
  Sim s;
  TEST_ASSERT_EQUAL(0, s.run("DU"));  // 10 ms spike
  s.idle(200);
  TEST_ASSERT_EQUAL(0, s.run("DDU"));  // 20 ms < kTouchMinPressMs
  s.idle(200);
  TEST_ASSERT_EQUAL(1, s.run("DDDDU"));
}

void test_missed_up_report_times_out_once() {
  Sim s;
  TEST_ASSERT_EQUAL(0, s.run("DDDD"));
  int n = s.run(std::string(kTouchStaleReleaseMs / kTouchPollMs + 2, 'S'));
  TEST_ASSERT_EQUAL(1, n);
  TEST_ASSERT_FALSE(s.f.down());
  TEST_ASSERT_EQUAL(0, s.run("SSSSSSSSSSSU"));
}

void test_long_hold_is_one_tap_no_repeat() {
  Sim s;
  std::string hold;
  for (int i = 0; i < 200; ++i) hold += (i % 3) ? 'S' : 'D';  // 2 s, chip reporting every 30 ms
  TEST_ASSERT_EQUAL(0, s.run(hold));
  TEST_ASSERT_EQUAL(1, s.run("DU"));
}

void test_guard_while_finger_down_blocks_click_through() {
  Sim s;
  s.run("DDDD");
  s.f.guard(s.t);  // the screen changed while the finger is down
  TEST_ASSERT_EQUAL(0, s.run("DDDU"));
  TEST_ASSERT_EQUAL(0, s.run("SSSSSSSSDDDDU"));  // 80 ms after the lift: still guarded
  s.idle(kTouchGuardMs);
  TEST_ASSERT_EQUAL(1, s.run("DDDDU"));
}

void test_guard_after_release() {
  Sim s;
  TEST_ASSERT_EQUAL(1, s.run("DDDDU"));
  s.f.guard(s.t);  // the tap changed the screen
  s.idle(100);
  TEST_ASSERT_EQUAL(0, s.run("DDDDU"));  // a second report burst of the same finger
  s.idle(kTouchGuardMs);
  TEST_ASSERT_EQUAL(1, s.run("DDDDU"));
}

void test_two_fingers_and_slides_are_not_taps() {
  Sim s;
  TEST_ASSERT_EQUAL(0, s.run("DD2DDU"));
  s.idle(200);
  TEST_ASSERT_EQUAL(0, s.run("DDMMU"));
  s.idle(200);
  TEST_ASSERT_EQUAL(1, s.run("DDDU"));
}

// Pseudo-random noisy streams: every touch has stale gaps, duplicate reports, release bounces
// and duplicate up reports; the number of taps must equal the number of touches.
void test_noisy_streams_one_tap_per_touch() {
  uint32_t seed = 12345;
  auto rnd = [&seed](int n) {
    seed = seed * 1103515245u + 12345u;
    return static_cast<int>((seed >> 16) % static_cast<uint32_t>(n));
  };
  Sim s;
  const int touches = 200;
  for (int k = 0; k < touches; ++k) {
    std::string seq = "D";
    const int len = 4 + rnd(30);
    for (int i = 0; i < len; ++i) seq += rnd(3) == 0 ? 'D' : 'S';
    seq += 'U';
    if (rnd(2)) seq += "SDU";          // release bounce (lift 10 ms, touch 10 ms)
    if (rnd(2)) seq += "UU";           // duplicate up reports
    seq += std::string(static_cast<size_t>(10 + rnd(30)), 'S');  // >= 100 ms up between taps
    s.run(seq, static_cast<int16_t>(100 + rnd(300)), static_cast<int16_t>(100 + rnd(600)));
  }
  TEST_ASSERT_EQUAL(touches, s.taps);
}

void test_tap_gate_drops_taps_from_before_the_change() {
  TapGate g;
  TEST_ASSERT_TRUE(g.rendered(111, 5000));   // first screen
  TEST_ASSERT_FALSE(g.rendered(111, 5200));  // same layout (a key press, a clock tick)
  TEST_ASSERT_TRUE(g.accept(5100));
  TEST_ASSERT_TRUE(g.rendered(222, 6000));   // new screen on the panel at 6000
  TEST_ASSERT_FALSE(g.accept(5600));         // queued during the refresh: aimed at the old screen
  TEST_ASSERT_TRUE(g.accept(6000));
  TEST_ASSERT_TRUE(g.accept(6500));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_clean_tap_emits_on_release_only);
  RUN_TEST(test_stale_reports_are_not_releases);
  RUN_TEST(test_duplicate_down_reports_dedupe);
  RUN_TEST(test_bounce_within_min_up_time_is_same_touch);
  RUN_TEST(test_min_press_time);
  RUN_TEST(test_missed_up_report_times_out_once);
  RUN_TEST(test_long_hold_is_one_tap_no_repeat);
  RUN_TEST(test_guard_while_finger_down_blocks_click_through);
  RUN_TEST(test_guard_after_release);
  RUN_TEST(test_two_fingers_and_slides_are_not_taps);
  RUN_TEST(test_noisy_streams_one_tap_per_touch);
  RUN_TEST(test_tap_gate_drops_taps_from_before_the_change);
  return UNITY_END();
}
