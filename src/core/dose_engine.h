#pragma once
// Dose state machine and event processor. All time comes from IClock.
//
//   Pending --(scheduled time)--> Due (alerting, ladder)
//   Due/Snoozed --take--> Taken | TakenLate
//   Due/Snoozed/Pending --skip--> Skipped
//   Due --snooze 5/10/15 (max 3)--> Snoozed --(snooze end)--> Due
//   Pending/Due/Snoozed --(scheduled + grace)--> Missed
//   Missed --take(backfill takenAt)--> Taken | TakenLate (correction entry)
//   any user action --undo within 60 s--> previous state
#include <cstdint>
#include <string>
#include <vector>

#include "core/clock.h"
#include "core/dose_log.h"
#include "core/model.h"
#include "core/schedule.h"
#include "core/tz.h"

namespace core {

enum class DoseState : uint8_t { Pending = 0, Due, Snoozed, Taken, TakenLate, Skipped, Missed };
const char* doseStateName(DoseState s);
bool isFinalState(DoseState s);

constexpr int64_t kUndoWindowSec = 60;
constexpr int kMaxSnoozes = 3;
constexpr int64_t kEarlyTakeWindowSec = 60 * 60;  // a dose can be taken up to 60 min early
constexpr int64_t kLookaheadSec = 24 * 3600;      // Pending instances are generated this far ahead
constexpr int64_t kRetainFinalSec = 48 * 3600;    // final instances kept in memory this long
constexpr int64_t kNow = INT64_MIN;               // "use the clock" marker for takenAt

struct DoseInstance {
  uint64_t eventId = 0;
  uint8_t profileId = 0;
  uint16_t medId = 0;
  int64_t scheduledUtc = 0;
  int32_t localDay = 0;
  int16_t localMinute = 0;
  DoseState state = DoseState::Pending;
  uint8_t snoozeCount = 0;
  int64_t snoozeUntil = 0;
  uint8_t alarmsFired = 0;      // alarms in the current ladder (initial + repeats)
  int64_t ladderStartUtc = 0;   // start of the current ladder
  int64_t actionUtc = 0;        // effective time of the final action
  int64_t lastActionRecorded = 0;  // recordedUtc of the last undoable user action
  // undo information (one level)
  bool undoable = false;
  DoseState prevState = DoseState::Pending;
  uint8_t prevSnoozeCount = 0;
  int64_t prevSnoozeUntil = 0;
  uint8_t prevAlarmsFired = 0;
  int64_t prevLadderStart = 0;
  int64_t prevLastTaken = 0;
  bool supplyDecremented = false;
};

enum class EngineEventKind : uint8_t { BecameDue = 0, AlarmTick, SnoozeEnded, BecameMissed };

struct EngineEvent {
  int64_t atUtc;
  uint64_t eventId;
  uint16_t medId;
  EngineEventKind kind;
  uint8_t alarmIndex;  // 0 = initial alarm, 1.. = repeats
  bool audible;        // false during quiet hours unless the medication is critical
  bool stale;          // happened more than 60 s before processing (catch-up): do not ring
};

enum class ActionResult : uint8_t {
  Ok = 0,
  NotFound,
  InvalidState,
  TooEarly,
  TakenAtInFuture,
  WouldDoubleDose,
  SnoozeLimit,
  InvalidSnooze,
  SnoozeBeyondGrace,
  UndoExpired,
  NothingToUndo,
  NotPrn,
  PrnTooSoon,
  PrnDailyCap
};
const char* actionResultName(ActionResult r);

struct PrnStatus {
  bool isPrn = false;
  bool allowed = false;
  int takenLast24h = 0;
  int maxPer24h = 0;
  int64_t lastTakenUtc = 0;
  int64_t nextAllowedUtc = 0;  // 0 = now
};

struct EngineSnapshot {
  int64_t generatedUntilUtc = 0;
  int64_t processedUntilUtc = 0;
  std::vector<DoseInstance> instances;
  std::vector<std::pair<uint16_t, int64_t>> lastTaken;
};

class DoseEngine {
 public:
  DoseEngine(const IClock& clock, Database& db, IDoseLogSink* log);

  bool setTimezone(const std::string& posixTz);
  const PosixTz& timezone() const { return tz_; }

  // Starts a fresh engine: nothing before startUtc is generated.
  void begin(int64_t startUtc);
  // Restores persisted state (after reboot or deep sleep).
  void restore(const EngineSnapshot& s);
  EngineSnapshot snapshot() const;

  // Generates occurrences and applies all timed transitions up to the clock's
  // "now", strictly in chronological order. Safe to call after a long sleep:
  // every occurrence is processed exactly once (deterministic ids).
  std::vector<EngineEvent> process();
  std::vector<EngineEvent> processUntil(int64_t nowUtc);

  // Drops future Pending instances after a schedule edit so they are regenerated.
  void regenerateFuture();

  ActionResult take(uint64_t eventId, ActionSource src, int64_t takenAtUtc = kNow);
  ActionResult skip(uint64_t eventId, ActionSource src);
  ActionResult snooze(uint64_t eventId, int minutes, ActionSource src);
  ActionResult undo(uint64_t eventId, ActionSource src);

  PrnStatus prnStatus(uint16_t medId) const;
  ActionResult takePrn(uint16_t medId, ActionSource src, uint64_t* outEventId = nullptr);

  const DoseInstance* find(uint64_t eventId) const;
  const std::vector<DoseInstance>& instances() const { return inst_; }
  // Non-final instances with scheduled time <= now + horizon, sorted by time.
  std::vector<const DoseInstance*> upcoming(int64_t horizonSec) const;
  int64_t lastTaken(uint16_t medId) const;
  int64_t graceEnd(const DoseInstance& d) const;
  // Earliest future time at which process() would change something (for the RTC wake timer).
  int64_t nextWakeUtc() const;
  int64_t generatedUntil() const { return generatedUntil_; }

 private:
  DoseInstance* findMut(uint64_t id);
  void generate(int64_t untilUtc);
  int64_t nextTransition(const DoseInstance& d) const;
  void applyTransition(DoseInstance& d, int64_t at, int64_t now, std::vector<EngineEvent>& ev);
  bool audibleAt(const Medication& m, int64_t utc) const;
  void log(const DoseInstance& d, DoseAction a, int64_t actionUtc, ActionSource src, const char* note);
  void setLastTaken(uint16_t medId, int64_t t);
  void prune(int64_t now);

  const IClock& clock_;
  Database& db_;
  IDoseLogSink* log_;
  PosixTz tz_;
  int64_t generatedUntil_ = 0;
  int64_t processedUntil_ = 0;
  std::vector<DoseInstance> inst_;
  std::vector<std::pair<uint16_t, int64_t>> lastTaken_;
};

}  // namespace core
