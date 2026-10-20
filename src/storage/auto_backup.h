#pragma once
// Nightly automatic SD backups: when to run, what changed, which old files to prune.
// Pure logic; the device runs it on the storage task (src/app/device_app.cpp) and keeps
// AutoBackupState in NVS ("abk").
#include <cstdint>
#include <string>
#include <vector>

#include "core/tz.h"
#include "storage/fs.h"

namespace storage {

constexpr int kAutoBackupSlotMinute = 3 * 60;   // local 03:00, the nightly refresh wake
constexpr int kAutoBackupKeep = 14;             // auto- files kept
constexpr int64_t kAutoBackupWeekSec = 7 * 86400;
constexpr int64_t kAutoBackupRetrySec = 3600;   // after a failure, retry at most hourly

enum class AutoBackupStatus : uint8_t { Never = 0, Ok, Unchanged, NoCard, Failed, SkippedBattery };
const char* autoBackupStatusName(AutoBackupStatus s);

struct AutoBackupState {
  int64_t lastBackupUtc = 0;    // last successful write
  uint32_t lastFingerprint = 0; // CRC32 of the JSON that was written
  int32_t lastSlotDay = -1;     // local day number whose 03:00 slot was handled
  int64_t lastAttemptUtc = 0;
  AutoBackupStatus status = AutoBackupStatus::Never;
  std::string lastName;
};

struct AutoBackupInputs {
  bool enabled = true;
  int64_t nowUtc = 0;
  bool clockValid = true;       // no backups (and no file names) from an unset clock
  bool sdMounted = false;
  bool criticalBattery = false;
};

enum class AutoBackupAction : uint8_t { None, WaitBattery, WaitCard, Run };

// The slot of local day D is D 03:00. The slot that should have run by now is today's
// (after 03:00) or yesterday's (before 03:00); a slot missed while asleep or off is caught
// up at the next evaluation (wake / boot / 30 s status poll).
int32_t dueSlotDay(const core::PosixTz& tz, int64_t nowUtc, int slotMinute = kAutoBackupSlotMinute);
AutoBackupAction decideAutoBackup(const core::PosixTz& tz, const AutoBackupInputs& in, const AutoBackupState& st);

uint32_t backupFingerprint(const std::string& json);
// Write only if the data changed since the last successful backup or it is a week old.
bool backupNeeded(const AutoBackupState& st, uint32_t fingerprint, int64_t nowUtc);

std::string autoBackupFileName(int year, int month, int day);  // auto-YYYYMMDD.json
bool isAutoBackupName(const std::string& name);
// Auto files beyond the newest `keep`, oldest last. Other names are never returned.
std::vector<std::string> autoBackupsToPrune(const std::vector<std::string>& names, int keep = kAutoBackupKeep);

struct AutoBackupResult {
  AutoBackupStatus status = AutoBackupStatus::Never;
  bool wrote = false;
  int pruned = 0;
  int pruneFailed = 0;
};

// Runs one evaluation. makeJson is called only when the slot is due. Updates `st`.
// The caller holds the SPI lock around SD access (the device passes a locking adapter).
template <typename MakeJson>
AutoBackupResult runAutoBackup(const core::PosixTz& tz, const AutoBackupInputs& in, AutoBackupState& st,
                               IFileSystem* sd, MakeJson&& makeJson);

// Implementation detail of runAutoBackup (non-template part).
AutoBackupResult finishAutoBackup(const core::PosixTz& tz, const AutoBackupInputs& in, AutoBackupState& st,
                                  IFileSystem& sd, const std::string& json);

// True when a banner should report the last automatic backup as failed.
inline bool autoBackupFailed(const AutoBackupState& st, bool enabled) {
  return enabled && st.status == AutoBackupStatus::Failed;
}

template <typename MakeJson>
AutoBackupResult runAutoBackup(const core::PosixTz& tz, const AutoBackupInputs& in, AutoBackupState& st,
                               IFileSystem* sd, MakeJson&& makeJson) {
  AutoBackupResult r;
  switch (decideAutoBackup(tz, in, st)) {
    case AutoBackupAction::None: r.status = st.status; return r;
    case AutoBackupAction::WaitBattery:
      if (st.status != AutoBackupStatus::Failed) st.status = AutoBackupStatus::SkippedBattery;
      r.status = st.status;
      return r;
    case AutoBackupAction::WaitCard:
      st.status = AutoBackupStatus::NoCard;
      r.status = st.status;
      return r;
    case AutoBackupAction::Run: break;
  }
  if (!sd) {
    st.status = AutoBackupStatus::NoCard;
    r.status = st.status;
    return r;
  }
  return finishAutoBackup(tz, in, st, *sd, makeJson());
}

}  // namespace storage
