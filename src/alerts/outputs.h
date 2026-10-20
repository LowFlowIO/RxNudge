#pragma once
// Real alert outputs for core::AlarmManager.
#include <atomic>

#include "core/alarm.h"

namespace alerts {

// Buzzer: LEDC PWM on GPIO48; volume = duty cycle. Unavailable when compiled out or
// when the user said they could not hear it in the self-test (visual-only fallback).
class BuzzerOutput : public core::AlertOutput {
 public:
  const char* name() const override { return "buzzer"; }
  bool audible() const override { return true; }
  bool available() const override;
  void play(const core::PersonAlert& a) override;  // blocking for the pattern duration
};

// Screen flash: inversion done by the UI task (it owns the display and the spi lock).
// play() only posts the request; the UI task calls takeFlashes().
class ScreenFlashOutput : public core::AlertOutput {
 public:
  const char* name() const override { return "screen"; }
  bool audible() const override { return false; }
  bool available() const override { return true; }
  void play(const core::PersonAlert& a) override;
  int takeFlashes() { return pending_.exchange(0); }

 private:
  std::atomic<int> pending_{0};
};

}  // namespace alerts
