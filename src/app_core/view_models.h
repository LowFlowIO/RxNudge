#pragma once
// Plain data the screens render. Built by AppModel from the database, the dose
// engine and the recent dose log; contains no references into either.
#include <cstdint>
#include <string>
#include <vector>

#include "core/dose_engine.h"
#include "core/model.h"

namespace app {

struct StatusFlags {
  bool clockUnset = false;
  bool clockUntrusted = false;
  int batteryPct = -1;  // -1 unknown
  bool charging = false;
  bool usbPowered = false;  // external power present (charging or full)
  bool lowBattery = false;
  bool veryLowBattery = false;  // <= 10 %: persistent banner
  bool sdPresent = false;
  bool sdError = false;
  bool storageError = false;
  bool alarmHwFault = false;
  bool wifiOn = false;
  bool autoBackupOn = true;    // SD-missing banner only when backups are wanted
  bool backupFailed = false;   // last automatic backup failed (SD error or full)
};

enum class BannerKind : uint8_t { None, ClockUnset, VeryLowBattery, StorageError, AlarmHw, ClockUntrusted, LowBattery, SdError, BackupFailed, SdMissing };
// Highest-priority banner and how many more are active.
BannerKind topBanner(const StatusFlags& s, int* more = nullptr);

enum class RowState : uint8_t { Pending, Due, Snoozed, Taken, TakenLate, Skipped, Missed, Untracked };
RowState rowStateFrom(core::DoseState s);

struct LaneVM {
  uint8_t profileId = 0;
  std::string title;   // name, or initials in privacy mode
  bool privacy = false;
  std::string nextTime;   // "14:00", "now" or "" (none left)
  std::string nextLabel;  // med name + dose; empty in privacy mode
  int taken = 0, total = 0, overdue = 0;
};

struct HomeVM {
  std::string clock;  // "14:05"
  std::string date;   // "Sunday 18 October"
  StatusFlags status;
  std::vector<LaneVM> lanes;  // only the current page
  int page = 0, pageCount = 1, profileCount = 0;
};

struct TimelineRow {
  uint64_t eventId = 0;
  uint16_t medId = 0;
  int minute = 0;  // local minute of day
  std::string time;
  std::string med;
  std::string dose;
  core::MedForm form = core::MedForm::Pill;
  RowState state = RowState::Pending;
  std::string lastTaken;  // "Last 08:03" / "Last Sat 20:01" / ""
  bool prn = false;
  bool critical = false;
};

struct TodayVM {
  uint8_t profileId = 0;
  std::string person;
  std::vector<TimelineRow> rows;
  int taken = 0, total = 0;
  bool hasPrn = false;
};

struct DoseDetailVM {
  bool found = false;
  uint64_t eventId = 0;
  uint8_t profileId = 0;
  std::string person, med, dose, instructions, notes;
  core::MedForm form = core::MedForm::Pill;
  std::string scheduled;  // "Scheduled 08:00"
  RowState state = RowState::Pending;
  std::string lastTaken;
  std::string supply;     // "12 left", "" if not tracked
  std::string refillBy;   // "Refill by Tue 3 Nov"
  bool lowStock = false, critical = false;
  bool canTake = false, canSkip = false, canBackfill = false, canUndo = false;
  int64_t scheduledUtc = 0;
};

struct AlarmMed {
  uint64_t eventId = 0;
  std::string med, dose, instructions;
  core::MedForm form = core::MedForm::Pill;
  bool critical = false;
};

struct AlarmVM {
  bool active = false;
  uint8_t profileId = 0;
  std::string person;
  std::string time;  // scheduled local time of the earliest due dose
  std::vector<AlarmMed> meds;
  int othersWaiting = 0;
  std::string othersNames;  // "Ben" (initials when private)
  bool canSnooze = false;
  bool critical = false;
  bool silent = false;  // quiet hours and nothing critical: visual only
  int64_t earliestScheduledUtc = 0;
};

struct PrnRow {
  uint16_t medId = 0;
  std::string med, dose;
  core::MedForm form = core::MedForm::Pill;
  bool allowed = false;
  std::string status;  // "Next allowed 18:30" or "1 of 3 in 24 h"
  std::string lastTaken;
};

struct PrnVM {
  uint8_t profileId = 0;
  std::string person;
  std::vector<PrnRow> rows;
};

enum class HistoryMode : uint8_t { Day, Week, Month };
enum class CellState : uint8_t { Blank, Future, NoData, Perfect, Partial, Missed };

struct DayCell {
  int32_t day = 0;
  int dayOfMonth = 0;
  CellState state = CellState::Blank;
  int taken = 0, total = 0;
  bool today = false;
};

struct HistoryRow {  // day mode
  std::string time, med;
  RowState state = RowState::Pending;
};

struct MedAdherence {
  std::string med;
  int percent = 0;
  int taken = 0, total = 0;
};

struct HistoryVM {
  uint8_t profileId = 0;
  std::string person;
  HistoryMode mode = HistoryMode::Week;
  std::string periodTitle;  // "Week of 12 Oct", "October 2026", "Sun 18 Oct"
  int32_t fromDay = 0, toDay = 0;
  std::vector<DayCell> cells;  // week: 7, month: 42 (Monday first, blanks around)
  std::vector<HistoryRow> rows;
  std::vector<MedAdherence> perMed;
  int percent = 0, streak = 0, longest = 0;
  int onTime = 0, late = 0, skipped = 0, missed = 0, prn = 0;
};

}  // namespace app
