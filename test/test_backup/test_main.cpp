// Nightly automatic SD backups: schedule, catch-up, change detection, retention, failure banner.
#include <unity.h>

#include <algorithm>

#include "app_core/view_models.h"
#include "core/civil.h"
#include "core/codec.h"
#include "core/json.h"
#include "core/tz.h"
#include "storage/auto_backup.h"
#include "storage/maintenance.h"
#include "storage/mem_fs.h"

using namespace storage;

namespace {

core::PosixTz berlin() {
  core::PosixTz tz;
  core::parsePosixTz("CET-1CEST,M3.5.0,M10.5.0/3", tz);
  return tz;
}

int64_t localUtc(int y, int m, int d, int hh, int mm) {
  return core::localToUtc(berlin(), core::daysFromCivil(y, m, d), hh * 3600 + mm * 60);
}

AutoBackupInputs inputs(int64_t now, bool sd = true) {
  AutoBackupInputs in;
  in.nowUtc = now;
  in.sdMounted = sd;
  return in;
}

int countPrefix(MemFs& fs, const std::string& prefix) {
  int n = 0;
  for (const auto& f : fs.list(backupDir()))
    if (f.rfind(prefix, 0) == 0) n++;
  return n;
}

AutoBackupResult run(AutoBackupState& st, const AutoBackupInputs& in, MemFs* fs, const std::string& json, int* jsonCalls = nullptr) {
  return runAutoBackup(berlin(), in, st, fs, [&]() {
    if (jsonCalls) (*jsonCalls)++;
    return json;
  });
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_schedule_decision() {
  const core::PosixTz tz = berlin();
  AutoBackupState st;
  st.lastSlotDay = core::daysFromCivil(2026, 10, 17);  // yesterday's slot done
  // 02:59 local: the next slot (today 03:00) is not due yet.
  TEST_ASSERT_EQUAL(static_cast<int>(AutoBackupAction::None),
                    static_cast<int>(decideAutoBackup(tz, inputs(localUtc(2026, 10, 18, 2, 59)), st)));
  // 03:00 local, the nightly refresh wake: due.
  const int64_t slot = localUtc(2026, 10, 18, 3, 0);
  TEST_ASSERT_EQUAL(static_cast<int>(AutoBackupAction::Run), static_cast<int>(decideAutoBackup(tz, inputs(slot), st)));
  TEST_ASSERT_EQUAL(core::daysFromCivil(2026, 10, 18), dueSlotDay(tz, slot));
  // Toggle off, clock unset, critical battery, no card.
  AutoBackupInputs in = inputs(slot);
  in.enabled = false;
  TEST_ASSERT_EQUAL(static_cast<int>(AutoBackupAction::None), static_cast<int>(decideAutoBackup(tz, in, st)));
  in = inputs(slot);
  in.clockValid = false;
  TEST_ASSERT_EQUAL(static_cast<int>(AutoBackupAction::None), static_cast<int>(decideAutoBackup(tz, in, st)));
  in = inputs(slot);
  in.criticalBattery = true;
  TEST_ASSERT_EQUAL(static_cast<int>(AutoBackupAction::WaitBattery), static_cast<int>(decideAutoBackup(tz, in, st)));
  TEST_ASSERT_EQUAL(static_cast<int>(AutoBackupAction::WaitCard), static_cast<int>(decideAutoBackup(tz, inputs(slot, false), st)));

  // Waiting states do not consume the slot: no JSON is built, the slot stays pending.
  MemFs fs;
  int calls = 0;
  in = inputs(slot);
  in.criticalBattery = true;
  TEST_ASSERT_EQUAL(static_cast<int>(AutoBackupStatus::SkippedBattery), static_cast<int>(run(st, in, &fs, "{}", &calls).status));
  TEST_ASSERT_EQUAL(static_cast<int>(AutoBackupStatus::NoCard), static_cast<int>(run(st, inputs(slot, false), &fs, "{}", &calls).status));
  TEST_ASSERT_EQUAL(0, calls);
  TEST_ASSERT_EQUAL(core::daysFromCivil(2026, 10, 17), st.lastSlotDay);
  // Card inserted later that morning: it runs then, once.
  const AutoBackupResult r = run(st, inputs(localUtc(2026, 10, 18, 9, 30)), &fs, "{\"a\":1}", &calls);
  TEST_ASSERT_TRUE(r.wrote);
  TEST_ASSERT_TRUE(fs.exists("/RxNudge/backups/auto-20261018.json"));
  TEST_ASSERT_FALSE(run(st, inputs(localUtc(2026, 10, 18, 23, 0)), &fs, "{\"a\":2}", &calls).wrote);
  TEST_ASSERT_EQUAL(1, calls);
}

void test_missed_slot_catch_up() {
  MemFs fs;
  AutoBackupState st;
  st.lastSlotDay = core::daysFromCivil(2026, 10, 14);  // asleep / flat battery for three nights
  // Next wake is a dose alarm at 14:00 on the 17th: catches up once (not once per missed night).
  AutoBackupResult r = run(st, inputs(localUtc(2026, 10, 17, 14, 0)), &fs, "{\"v\":1}");
  TEST_ASSERT_TRUE(r.wrote);
  TEST_ASSERT_EQUAL(1, countPrefix(fs, "auto-"));
  TEST_ASSERT_EQUAL(core::daysFromCivil(2026, 10, 17), st.lastSlotDay);
  // Boot at 02:00 on the 19th after missing the 18th's slot: runs for the 18th...
  r = run(st, inputs(localUtc(2026, 10, 19, 2, 0)), &fs, "{\"v\":2}");
  TEST_ASSERT_TRUE(r.wrote);
  TEST_ASSERT_TRUE(fs.exists("/RxNudge/backups/auto-20261019.json"));  // named by the local date it ran
  TEST_ASSERT_EQUAL(core::daysFromCivil(2026, 10, 18), st.lastSlotDay);
  // ...and still handles the 19th's own slot at 03:00 (unchanged data: no file).
  r = run(st, inputs(localUtc(2026, 10, 19, 3, 0)), &fs, "{\"v\":2}");
  TEST_ASSERT_FALSE(r.wrote);
  TEST_ASSERT_EQUAL(static_cast<int>(AutoBackupStatus::Unchanged), static_cast<int>(r.status));
  TEST_ASSERT_EQUAL(core::daysFromCivil(2026, 10, 19), st.lastSlotDay);
  // DST end night (25 Oct, 03:00 occurs twice): one run.
  st.lastSlotDay = core::daysFromCivil(2026, 10, 24);
  const int64_t first = localUtc(2026, 10, 25, 3, 0);
  TEST_ASSERT_TRUE(run(st, inputs(first), &fs, "{\"v\":3}").wrote);
  TEST_ASSERT_FALSE(run(st, inputs(first + 3600), &fs, "{\"v\":4}").wrote);
}

void test_change_detection_and_weekly() {
  MemFs fs;
  AutoBackupState st;
  const std::string a = "{\"database\":1}", b = "{\"database\":2}";
  TEST_ASSERT_TRUE(backupNeeded(st, backupFingerprint(a), 1000));  // never backed up
  TEST_ASSERT_TRUE(run(st, inputs(localUtc(2026, 10, 1, 3, 0)), &fs, a).wrote);
  TEST_ASSERT_EQUAL_UINT32(backupFingerprint(a), st.lastFingerprint);
  // Same data on the following nights: nothing written for 6 nights.
  for (int d = 2; d <= 7; ++d) {
    const AutoBackupResult r = run(st, inputs(localUtc(2026, 10, d, 3, 0)), &fs, a);
    TEST_ASSERT_FALSE(r.wrote);
    TEST_ASSERT_EQUAL(static_cast<int>(AutoBackupStatus::Unchanged), static_cast<int>(r.status));
  }
  TEST_ASSERT_EQUAL(1, countPrefix(fs, "auto-"));
  // Day 8: a week since the last backup, so it is written even though unchanged.
  TEST_ASSERT_TRUE(run(st, inputs(localUtc(2026, 10, 8, 3, 0)), &fs, a).wrote);
  // Changed data is written the next night.
  TEST_ASSERT_TRUE(run(st, inputs(localUtc(2026, 10, 9, 3, 0)), &fs, b).wrote);
  TEST_ASSERT_EQUAL(3, countPrefix(fs, "auto-"));
  std::string json;
  TEST_ASSERT_TRUE(readBackupFile(fs, "auto-20261009.json", json));
  TEST_ASSERT_EQUAL_STRING(b.c_str(), json.c_str());
  TEST_ASSERT_EQUAL_STRING("auto-20261009.json", st.lastName.c_str());
}

void test_retention_pruning() {
  // Pure selection: only well-formed auto- names, oldest beyond 14.
  std::vector<std::string> names = {"backup-20260101-1200.json", "auto-notes.json", "auto-2026010.json", "readme.txt"};
  for (int d = 1; d <= 20; ++d) names.push_back(autoBackupFileName(2026, 9, d));
  std::vector<std::string> prune = autoBackupsToPrune(names);
  TEST_ASSERT_EQUAL(6, static_cast<int>(prune.size()));
  for (const auto& n : prune) TEST_ASSERT_TRUE(isAutoBackupName(n));
  TEST_ASSERT_TRUE(std::find(prune.begin(), prune.end(), "auto-20260901.json") != prune.end());
  TEST_ASSERT_TRUE(std::find(prune.begin(), prune.end(), "auto-20260907.json") == prune.end());
  TEST_ASSERT_TRUE(autoBackupsToPrune(names, 30).empty());

  // On the card: manual backups and foreign files survive; 14 auto files remain.
  MemFs fs;
  fs.mkdirs(backupDir());
  for (int d = 1; d <= 20; ++d) fs.writeFile(backupDir() + "/" + autoBackupFileName(2026, 9, d), "{}");
  fs.writeFile(backupDir() + "/backup-20250101-0800.json", "{\"manual\":1}");
  fs.writeFile(backupDir() + "/backup-20260930-2200.json", "{\"manual\":2}");
  fs.writeFile(backupDir() + "/auto-notes.json", "keep");
  AutoBackupState st;
  const AutoBackupResult r = run(st, inputs(localUtc(2026, 10, 18, 3, 0)), &fs, "{\"new\":1}");
  TEST_ASSERT_TRUE(r.wrote);
  TEST_ASSERT_EQUAL(7, r.pruned);  // 21 auto files -> 14
  int autos = 0;
  for (const auto& f : fs.list(backupDir()))
    if (isAutoBackupName(f)) autos++;
  TEST_ASSERT_EQUAL(14, autos);
  TEST_ASSERT_TRUE(fs.exists(backupDir() + "/auto-20261018.json"));
  TEST_ASSERT_FALSE(fs.exists(backupDir() + "/auto-20260907.json"));
  TEST_ASSERT_TRUE(fs.exists(backupDir() + "/auto-20260908.json"));
  TEST_ASSERT_TRUE(fs.exists(backupDir() + "/backup-20250101-0800.json"));
  TEST_ASSERT_TRUE(fs.exists(backupDir() + "/backup-20260930-2200.json"));
  TEST_ASSERT_TRUE(fs.exists(backupDir() + "/auto-notes.json"));
  // Restore list shows both kinds, newest first by date.
  const std::vector<std::string> list = listBackups(fs);
  TEST_ASSERT_EQUAL_STRING("auto-20261018.json", list.front().c_str());
  TEST_ASSERT_EQUAL_STRING("backup-20260930-2200.json", list[1].c_str());
  TEST_ASSERT_EQUAL_STRING("backup-20250101-0800.json", list.back().c_str());
}

void test_failure_banner_and_retry() {
  MemFs fs;
  AutoBackupState st;
  const int64_t slot = localUtc(2026, 10, 18, 3, 0);
  fs.failWriteAfter = 0;  // card full / write error
  AutoBackupResult r = run(st, inputs(slot), &fs, "{\"x\":1}");
  TEST_ASSERT_FALSE(r.wrote);
  TEST_ASSERT_EQUAL(static_cast<int>(AutoBackupStatus::Failed), static_cast<int>(r.status));
  TEST_ASSERT_FALSE(fs.exists(backupDir() + "/auto-20261018.json"));
  TEST_ASSERT_FALSE(fs.exists(backupDir() + "/auto-20261018.json.tmp"));  // no half file left
  TEST_ASSERT_EQUAL(0, static_cast<int>(st.lastBackupUtc));
  TEST_ASSERT_TRUE(autoBackupFailed(st, true));
  TEST_ASSERT_FALSE(autoBackupFailed(st, false));  // toggle off: no banner

  app::StatusFlags s;
  s.sdPresent = true;
  s.backupFailed = autoBackupFailed(st, true);
  TEST_ASSERT_EQUAL(static_cast<int>(app::BannerKind::BackupFailed), static_cast<int>(app::topBanner(s)));
  s.sdError = true;  // a card error is the more specific banner
  TEST_ASSERT_EQUAL(static_cast<int>(app::BannerKind::SdError), static_cast<int>(app::topBanner(s)));

  // Retries at most hourly, then succeeds and the banner clears.
  fs.failWriteAfter = -1;
  TEST_ASSERT_EQUAL(static_cast<int>(AutoBackupAction::None),
                    static_cast<int>(decideAutoBackup(berlin(), inputs(slot + 1800), st)));
  r = run(st, inputs(slot + 3600), &fs, "{\"x\":1}");
  TEST_ASSERT_TRUE(r.wrote);
  TEST_ASSERT_FALSE(autoBackupFailed(st, true));

  // "No SD card - backups paused" only while backups are wanted.
  app::StatusFlags n;
  TEST_ASSERT_EQUAL(static_cast<int>(app::BannerKind::SdMissing), static_cast<int>(app::topBanner(n)));
  n.autoBackupOn = false;
  TEST_ASSERT_EQUAL(static_cast<int>(app::BannerKind::None), static_cast<int>(app::topBanner(n)));
}

void test_settings_toggle_default_on() {
  core::Database db;
  TEST_ASSERT_TRUE(db.settings.autoBackup);
  db.settings.autoBackup = false;
  core::Database back;
  std::string err;
  TEST_ASSERT_TRUE(core::decodeDatabase(core::encodeDatabase(db), back, &err));
  TEST_ASSERT_FALSE(back.settings.autoBackup);
  // Older data without the key: ON.
  core::JsonValue j = core::encodeDatabase(core::Database{});
  core::JsonValue st = j["settings"];
  core::JsonValue trimmed = core::JsonValue::object();
  trimmed.set("tz", st["tz"]);
  j.set("settings", trimmed);
  core::Database old;
  old.settings.autoBackup = false;
  TEST_ASSERT_TRUE(core::decodeDatabase(j, old, &err));
  TEST_ASSERT_TRUE(old.settings.autoBackup);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_schedule_decision);
  RUN_TEST(test_missed_slot_catch_up);
  RUN_TEST(test_change_detection_and_weekly);
  RUN_TEST(test_retention_pruning);
  RUN_TEST(test_failure_banner_and_retry);
  RUN_TEST(test_settings_toggle_default_on);
  return UNITY_END();
}
