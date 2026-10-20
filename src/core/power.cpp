#include "core/power.h"

#include <algorithm>
#include <cstdlib>

namespace core {

const char* wakeReasonName(WakeReason r) {
  switch (r) {
    case WakeReason::DoseEvent: return "dose_event";
    case WakeReason::NightlyRefresh: return "nightly_refresh";
    case WakeReason::NtpSync: return "ntp_sync";
    case WakeReason::MaxSleep: return "max_sleep";
    default: return "none";
  }
}

int64_t nextLocalMinute(const PosixTz& tz, int64_t nowUtc, int minuteOfDay) {
  const LocalTime lt = utcToLocal(tz, nowUtc);
  for (int add = 0; add < 3; add++) {
    const int64_t t = localToUtc(tz, lt.days + add, minuteOfDay * 60);
    if (t > nowUtc) return t;
  }
  return nowUtc + 86400;
}

WakePlan planWake(const PosixTz& tz, const WakeInputs& in) {
  WakePlan p;
  p.targetUtc = in.nowUtc + in.maxSleepSec;
  p.reason = WakeReason::MaxSleep;
  auto consider = [&](int64_t t, WakeReason r) {
    if (t < p.targetUtc) {
      p.targetUtc = t;
      p.reason = r;
    }
  };
  if (in.engineRunning && in.engineNextUtc != INT64_MAX) consider(in.engineNextUtc, WakeReason::DoseEvent);
  consider(nextLocalMinute(tz, in.nowUtc, in.nightlyRefreshMinute), WakeReason::NightlyRefresh);
  if (in.ntpEnabled) consider(nextLocalMinute(tz, in.nowUtc, in.ntpMinute), WakeReason::NtpSync);
  const int64_t span = std::max<int64_t>(0, p.targetUtc - in.nowUtc);
  const int64_t margin = in.baseMarginSec + span * in.driftPermille / 1000;
  p.wakeUtc = std::max(p.targetUtc - margin, in.nowUtc + kMinSleepSec);
  if (p.targetUtc <= in.nowUtc) p.wakeUtc = in.nowUtc;  // overdue: do not sleep at all
  p.sleepSec = p.wakeUtc - in.nowUtc;
  return p;
}

const char* sleepBlockerName(SleepBlocker b) {
  static const char* const n[] = {"none", "refreshing", "alarm_active", "usb_powered", "pending_writes", "clock_unset",
                                  "not_idle", "not_home", "network_busy"};
  return n[static_cast<int>(b)];
}

SleepBlocker sleepBlocker(const SleepInputs& in) {
  // Safety rules first: these also block a user-requested sleep.
  if (in.refreshing) return SleepBlocker::Refreshing;
  if (in.alarmActive) return SleepBlocker::AlarmActive;
  if (in.pendingWrites) return SleepBlocker::PendingWrites;
  if (in.clockUnset) return SleepBlocker::ClockUnset;
  if (in.networkBusy) return SleepBlocker::NetworkBusy;
  if (in.userRequested) return SleepBlocker::None;
  if (in.usbPowered && !in.sleepOnUsb) return SleepBlocker::UsbPowered;
  if (!in.onHomeScreen) return SleepBlocker::NotHome;
  if (in.idleMs < in.idleLimitMs) return SleepBlocker::NotIdle;
  return SleepBlocker::None;
}

const char* batteryLevelName(BatteryLevel l) {
  static const char* const n[] = {"unknown", "ok", "low", "very_low", "critical"};
  return n[static_cast<int>(l)];
}

BatteryUpdate BatteryMonitor::update(bool gaugeOk, int pct, bool usb) {
  BatteryUpdate u;
  const BatteryLevel before = level_;
  if (!gaugeOk || pct < 0) {
    // Keep the last known level; an unreadable gauge never triggers a shutdown.
    criticalReadings_ = 0;
    u.level = level_;
    return u;
  }
  auto raw = [](int v) {
    if (v <= kBatteryCriticalPct) return BatteryLevel::Critical;
    if (v <= kBatteryVeryLowPct) return BatteryLevel::VeryLow;
    if (v <= kBatteryLowPct) return BatteryLevel::Low;
    return BatteryLevel::Ok;
  };
  BatteryLevel next = raw(pct);
  if (usb) {
    // Charging: warnings clear once above the low threshold; never shut down on USB.
    next = pct > kBatteryLowPct ? BatteryLevel::Ok : (next == BatteryLevel::Critical ? BatteryLevel::VeryLow : next);
  }
  // Hysteresis: going back up requires threshold + hysteresis.
  if (level_ != BatteryLevel::Unknown && next < level_) {
    const int thr = level_ == BatteryLevel::Critical ? kBatteryCriticalPct
                    : level_ == BatteryLevel::VeryLow ? kBatteryVeryLowPct
                                                       : kBatteryLowPct;
    if (pct <= thr + kBatteryHysteresisPct && !(usb && level_ == BatteryLevel::Critical)) next = level_;
  }
  if (next == BatteryLevel::Critical) {
    criticalReadings_++;
    if (criticalReadings_ < kCriticalConfirmReadings && level_ != BatteryLevel::Critical)
      next = std::max(BatteryLevel::VeryLow, level_ == BatteryLevel::Unknown ? BatteryLevel::VeryLow : level_);
  } else {
    criticalReadings_ = 0;
  }
  if (next < BatteryLevel::VeryLow && next != BatteryLevel::Unknown) veryLowAlerted_ = false;
  level_ = next;
  u.level = level_;
  u.changed = level_ != before;
  if (level_ >= BatteryLevel::VeryLow && !veryLowAlerted_) {
    veryLowAlerted_ = true;
    u.alertNow = true;
  }
  u.shutdownNow = u.changed && level_ == BatteryLevel::Critical && !usb;
  return u;
}

const char* wakeSourceName(WakeSource s) {
  static const char* const n[] = {"power_on", "timer", "button", "touch", "usb", "other"};
  return n[static_cast<int>(s)];
}

void PowerCounters::init() {
  *this = PowerCounters();
  magic = kMagic;
}

void PowerCounters::recordWake(WakeSource s, int32_t driftSec) {
  if (!valid()) init();
  boots++;
  wakes[static_cast<int>(s)]++;
  lastDriftSec = driftSec;
  if (std::abs(driftSec) > std::abs(maxDriftSec)) maxDriftSec = driftSec;
}

void PowerCounters::recordSleep(int64_t sec, uint32_t awakeMsBefore) {
  if (!valid()) init();
  sleeps++;
  sleepSec += static_cast<uint64_t>(sec > 0 ? sec : 0);
  awakeMs += awakeMsBefore;
}

}  // namespace core
