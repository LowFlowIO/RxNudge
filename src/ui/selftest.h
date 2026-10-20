#pragma once

namespace ui {

// Hidden hardware self-test menu. Entered by holding UP+DOWN for 3 s on the
// home screen. Returns when the user picks "Exit". The deep-sleep test never
// returns; its result is shown by showWakeTestResult() on the next boot.
void runSelfTestMenu();

// Called from setup() when the RTC record says a self-test sleep was pending.
void showWakeTestResult();

}  // namespace ui
