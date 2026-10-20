#pragma once
// Power policy (pure): next timer wake, sleep gating, battery thresholds and
// wake/sleep counters. The firmware feeds it readings and acts on the results.
#include <cstdint>

#include "core/tz.h"

namespace core {

// ---------------------------------------------------------------- next wake
enum class WakeReason : uint8_t { DoseEvent = 0, NightlyRefresh, NtpSync, MaxSleep, None };
const char* wakeReasonName(WakeReason r);

struct WakeInputs {
  int64_t nowUtc = 0;
  int64_t engineNextUtc = INT64_MAX;  // DoseEngine::nextWakeUtc(): due, ladder repeat, snooze end, grace expiry, regeneration
  bool engineRunning = true;
  int nightlyRefreshMinute = 3 * 60;  // local minute of the nightly FULL refresh
  int ntpMinute = 4 * 60;             // local minute of the daily NTP sync (04:00)
  bool ntpEnabled = false;
  int64_t maxSleepSec = 6 * 3600;     // periodic status/battery check
  int64_t baseMarginSec = 20;         // boot + catch-up time
  int driftPermille = 50;             // worst-case ESP32 RC slow-clock error (ASSUMPTION 5 %), corrected at wake via the PCF8563
};

struct WakePlan {
  int64_t targetUtc = 0;  // the event we wake for
  int64_t wakeUtc = 0;    // target minus margin (never before now + minSleep)
  int64_t sleepSec = 0;   // seconds of device time to sleep
  WakeReason reason = WakeReason::None;
};

constexpr int64_t kMinSleepSec = 15;
// Next local occurrence (strictly after nowUtc) of a minute-of-day.
int64_t nextLocalMinute(const PosixTz& tz, int64_t nowUtc, int minuteOfDay);
WakePlan planWake(const PosixTz& tz, const WakeInputs& in);

// ---------------------------------------------------------------- sleep gate
struct SleepInputs {
  bool refreshing = false;      // e-paper refresh in progress
  bool alarmActive = false;     // a dose is due (alarm screen) or an alert is playing
  bool usbPowered = false;
  bool sleepOnUsb = false;      // Settings::sleepOnUsb
  bool pendingWrites = false;   // log/snapshot not yet on flash
  bool clockUnset = false;      // must show the clock screen, never sleep
  bool onHomeScreen = true;
  uint32_t idleMs = 0;
  uint32_t idleLimitMs = 5UL * 60UL * 1000UL;
  bool userRequested = false;   // OK long press on Home (bypasses idle/screen, not safety rules)
  bool networkBusy = false;     // radio job in flight (NTP, notification, portal, OTA)
};

enum class SleepBlocker : uint8_t { None = 0, Refreshing, AlarmActive, UsbPowered, PendingWrites, ClockUnset, NotIdle, NotHome,
                                    NetworkBusy };
const char* sleepBlockerName(SleepBlocker b);
SleepBlocker sleepBlocker(const SleepInputs& in);

// ---------------------------------------------------------------- battery
enum class BatteryLevel : uint8_t { Unknown = 0, Ok, Low, VeryLow, Critical };
const char* batteryLevelName(BatteryLevel l);

constexpr int kBatteryLowPct = 20;       // banner
constexpr int kBatteryVeryLowPct = 10;   // persistent banner + one alert
constexpr int kBatteryCriticalPct = 5;   // final screen, deep sleep, button wake only
constexpr int kBatteryHysteresisPct = 3; // must rise this far above a threshold to clear it

struct BatteryUpdate {
  BatteryLevel level = BatteryLevel::Unknown;
  bool changed = false;
  bool alertNow = false;     // entered VeryLow: sound/show one alert
  bool shutdownNow = false;  // entered Critical (confirmed by consecutive readings) and not on USB
};

class BatteryMonitor {
 public:
  // pct = BQ27220 state of charge; usb = external power present.
  BatteryUpdate update(bool gaugeOk, int pct, bool usb);
  BatteryLevel level() const { return level_; }
  void reset() { *this = BatteryMonitor(); }

 private:
  BatteryLevel level_ = BatteryLevel::Unknown;
  int criticalReadings_ = 0;
  bool veryLowAlerted_ = false;
};
constexpr int kCriticalConfirmReadings = 2;

// ---------------------------------------------------------------- counters
enum class WakeSource : uint8_t { PowerOn = 0, Timer, Button, Touch, Usb, Other };
const char* wakeSourceName(WakeSource s);

struct PowerCounters {
  uint32_t magic = 0;
  uint32_t boots = 0;
  uint32_t wakes[6] = {};      // by WakeSource
  uint32_t sleeps = 0;
  uint32_t earlyWakes = 0;     // woke before the target (margin) and went back to sleep
  uint32_t catchUpAlerts = 0;
  uint32_t missedWhileAsleep = 0;
  uint32_t shutdowns = 0;      // critical battery
  uint64_t sleepSec = 0;       // total requested sleep
  uint64_t awakeMs = 0;        // total awake time (approx.)
  int32_t lastDriftSec = 0;    // system time minus RTC at the last wake (+ = ESP32 ran fast)
  int32_t maxDriftSec = 0;
  void recordWake(WakeSource s, int32_t driftSec);
  void recordSleep(int64_t sec, uint32_t awakeMsBefore);
  bool valid() const { return magic == kMagic; }
  void init();
  static constexpr uint32_t kMagic = 0x50575231u;  // "PWR1"
};

}  // namespace core
