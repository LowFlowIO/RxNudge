#include "storage/auto_backup.h"

#include <algorithm>
#include <cstdio>

#include "core/selftest_logic.h"  // crc32
#include "core/civil.h"
#include "storage/maintenance.h"

namespace storage {

const char* autoBackupStatusName(AutoBackupStatus s) {
  switch (s) {
    case AutoBackupStatus::Never: return "never";
    case AutoBackupStatus::Ok: return "ok";
    case AutoBackupStatus::Unchanged: return "unchanged";
    case AutoBackupStatus::NoCard: return "no_card";
    case AutoBackupStatus::Failed: return "failed";
    case AutoBackupStatus::SkippedBattery: return "skipped_battery";
  }
  return "?";
}

int32_t dueSlotDay(const core::PosixTz& tz, int64_t nowUtc, int slotMinute) {
  const core::LocalTime lt = core::utcToLocal(tz, nowUtc);
  return lt.secOfDay >= slotMinute * 60 ? lt.days : lt.days - 1;
}

AutoBackupAction decideAutoBackup(const core::PosixTz& tz, const AutoBackupInputs& in, const AutoBackupState& st) {
  if (!in.enabled || !in.clockValid) return AutoBackupAction::None;
  const int32_t due = dueSlotDay(tz, in.nowUtc);
  const bool failed = st.status == AutoBackupStatus::Failed;
  if (failed && in.nowUtc - st.lastAttemptUtc < kAutoBackupRetrySec) return AutoBackupAction::None;  // hourly retry
  if (st.lastSlotDay >= due && !failed) return AutoBackupAction::None;
  if (in.criticalBattery) return AutoBackupAction::WaitBattery;
  if (!in.sdMounted) return AutoBackupAction::WaitCard;
  return AutoBackupAction::Run;
}

uint32_t backupFingerprint(const std::string& json) {
  return core::crc32(reinterpret_cast<const uint8_t*>(json.data()), json.size());
}

bool backupNeeded(const AutoBackupState& st, uint32_t fingerprint, int64_t nowUtc) {
  if (st.lastBackupUtc == 0) return true;
  if (fingerprint != st.lastFingerprint) return true;
  return nowUtc - st.lastBackupUtc >= kAutoBackupWeekSec;
}

std::string autoBackupFileName(int year, int month, int day) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "auto-%04d%02d%02d.json", year, month, day);
  return buf;
}

bool isAutoBackupName(const std::string& n) {
  if (n.size() != 18 || n.rfind("auto-", 0) != 0 || n.compare(13, 5, ".json") != 0) return false;
  for (size_t i = 5; i < 13; ++i)
    if (n[i] < '0' || n[i] > '9') return false;
  return true;
}

std::vector<std::string> autoBackupsToPrune(const std::vector<std::string>& names, int keep) {
  std::vector<std::string> autos;
  for (const auto& n : names)
    if (isAutoBackupName(n)) autos.push_back(n);
  std::sort(autos.rbegin(), autos.rend());  // newest first (fixed-width date)
  std::vector<std::string> out;
  for (size_t i = static_cast<size_t>(std::max(keep, 0)); i < autos.size(); ++i) out.push_back(autos[i]);
  return out;
}

AutoBackupResult finishAutoBackup(const core::PosixTz& tz, const AutoBackupInputs& in, AutoBackupState& st,
                                  IFileSystem& sd, const std::string& json) {
  AutoBackupResult r;
  const int32_t due = dueSlotDay(tz, in.nowUtc);
  st.lastAttemptUtc = in.nowUtc;
  const uint32_t fp = backupFingerprint(json);
  if (json.empty()) {
    st.status = AutoBackupStatus::Failed;
  } else if (!backupNeeded(st, fp, in.nowUtc)) {
    st.status = AutoBackupStatus::Unchanged;
    st.lastSlotDay = due;
  } else {
    const core::LocalTime lt = core::utcToLocal(tz, in.nowUtc);
    const core::Ymd d = core::civilFromDays(lt.days);
    const std::string name = autoBackupFileName(d.y, d.m, d.d);
    if (writeBackupFile(sd, name, json)) {
      r.wrote = true;
      st.status = AutoBackupStatus::Ok;
      st.lastBackupUtc = in.nowUtc;
      st.lastFingerprint = fp;
      st.lastName = name;
      st.lastSlotDay = due;
      sd.remove(backupDir() + "/" + name + ".tmp");
    } else {
      sd.remove(backupDir() + "/" + name + ".tmp");  // a full card must not keep a half file
      st.status = AutoBackupStatus::Failed;
    }
  }
  if (r.wrote) {
    for (const std::string& n : autoBackupsToPrune(sd.list(backupDir()))) {
      if (sd.remove(backupDir() + "/" + n)) r.pruned++;
      else r.pruneFailed++;
    }
  }
  r.status = st.status;
  return r;
}

}  // namespace storage
